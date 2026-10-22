/*
 * DEVELOPMENT TOOL. Runs the RADE V1 pilot correlator (src/rade_correlator.c) and the RADE V2
 * cyclic-prefix correlator (src/rade_v2_correlator.c) side by side over a capture, with the
 * hand-over rule proposed for the combined "RADE auto" reference (docs/diversity-rade-auto.md,
 * Stage 1), and says which waveform holds the weight, second by second.
 *
 *   auto_replay [options] FILE [FILE2]
 *
 *   --a-secs N      take only the first N seconds of FILE, then continue with FILE2 (a splice: two
 *                   stations one after the other on the same tuning, no reset between)
 *   --fade-q Q      V1 counts as faded when its quality is below Q (default 0.05; the module has
 *                   no presence flag of its own, V2 has)
 *   --csv PATH      one row per block
 *   -q              events and the summary only
 *
 * The rule, as tried here:
 *   - both modules see every block, each with its own gate, unchanged
 *   - with nobody holding, the first module to lock holds (V1 if both lock on one block)
 *   - the holder keeps the weight however long it is gone (no timeout)
 *   - a holder that is faded (V1: quality below --fade-q; V2: prefix not present) is replaced
 *     by the other module when that locks - a lock that began while the holder was healthy is
 *     not counted, however long it stays locked: it is a lock on something the holder was
 *     already explaining
 *   - an engine reset or a dropped block clears both modules and the holder
 *
 * Output ends with one SUMMARY line: first lock time per module and for the holder, and the
 * hand-overs. Builds with the rest of devtools (make auto_replay).
 */
#include <gtk/gtk.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "divcap_replay.h"
#include "rade_correlator.h"
#include "rade_tuning.h"
#include "rade_v2_correlator.h"
#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"
#include "radio.h"

/* Stubs, as in replay_rade.c: the engine's globals, quietly. */
static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int div_auto_mode = DIV_MANUAL;
ADC adc[3];
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double auto_div_cos = 1.0, auto_div_sin = 0.0, auto_div_gain = 0.0, auto_div_phase = 0.0;
double div_norm = 1.0;
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];

void t_print(const char *fmt, ...) { (void)fmt; }
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }

enum { NONE = 0, V1 = 1, V2 = 2 };

static const char *const NAME[] = { "none", "V1", "V2" };

struct feed {
  FILE *f;
  struct divcap_header h;
  float *buf;
  double secs;          /* stop after this many seconds, <= 0 for all */
  double used;
};

static int open_feed(struct feed *a, const char *path, double secs) {
  long start;

  memset(a, 0, sizeof *a);
  a->f = divcap_open(path, &a->h, &start);

  if (!a->f) { return 0; }

  a->buf = malloc(a->h.block_bytes);
  a->secs = secs;
  return a->buf != NULL;
}

static int next_block(struct feed *a, struct divcap_block *m) {
  if (a->secs > 0 && a->used >= a->secs) { return 0; }

  if (fread(m, sizeof *m, 1, a->f) != 1 || m->rec_magic != DIVCAP_REC_MAGIC) { return 0; }

  if (fread(a->buf, 1, a->h.block_bytes, a->f) != a->h.block_bytes) { return 0; }

  a->used += (double)a->h.nfft / a->h.sample_rate;
  return 1;
}

int main(int argc, char **argv) {
  const char *path[2] = { NULL, NULL };
  const char *csvpath = NULL;
  double a_secs = 0.0, fade_q = 0.05;
  int quiet = 0, np = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--a-secs") && i + 1 < argc) { a_secs = atof(argv[++i]); }
    else if (!strcmp(argv[i], "--fade-q") && i + 1 < argc) { fade_q = atof(argv[++i]); }
    else if (!strcmp(argv[i], "--csv") && i + 1 < argc) { csvpath = argv[++i]; }
    else if (!strcmp(argv[i], "-q")) { quiet = 1; }
    else if (argv[i][0] != '-' && np < 2) { path[np++] = argv[i]; }
    else { fprintf(stderr, "usage: %s [--a-secs N] [--fade-q Q] [--csv PATH] [-q] FILE [FILE2]\n", argv[0]); return 2; }
  }

  if (np == 0 || (a_secs > 0 && np < 2)) {
    fprintf(stderr, "usage: %s [--a-secs N] [--fade-q Q] [--csv PATH] [-q] FILE [FILE2]\n", argv[0]);
    return 2;
  }

  struct feed fd[2];

  if (!open_feed(&fd[0], path[0], a_secs)) { return 1; }

  if (np == 2) {
    if (!open_feed(&fd[1], path[1], 0.0)) { return 1; }

    if (fd[1].h.sample_rate != fd[0].h.sample_rate || fd[1].h.nfft != fd[0].h.nfft) {
      fprintf(stderr, "the two captures differ in rate or block size, cannot splice\n");
      return 1;
    }
  }

  const unsigned rate = fd[0].h.sample_rate;
  const size_t nfft = fd[0].h.nfft;

  if (!rade_corr_start((int)rate) || !rade_v2c_start((int)rate)) { fprintf(stderr, "rate %u: cannot run\n", rate); return 1; }

  FILE *csv = csvpath ? fopen(csvpath, "w") : NULL;

  if (csv) { fprintf(csv, "t,file,holder,v1_locked,v1_q,v2_locked,v2_present,v2_q,wr,wi\n"); }

  int holder = NONE, cur = 0;
  long nblk = 0, nhand = 0, nreset = 0;
  double t_v1 = -1, t_v2 = -1, t_hold = -1, splice_t = -1;
  double t_first_after_splice[3] = { -1, -1, -1 };
  double next = 1.0, wr = 1.0, wi = 0.0;
  int was_v1 = 0, was_v2 = 0;
  double held[3] = { 0, 0, 0 };    /* seconds after the splice with each holder */
  double t_new = -1;               /* after the splice: first block a different module than before holds */
  int pre_holder = NONE;
  int eligible[3] = { 0, 0, 0 };   /* a challenger whose lock began while the holder was faded or absent */
  int faded_now = 1;               /* the holder's state at the end of the previous block */
  struct divcap_block m;
  int first_of_b = 0;

  for (;;) {
    if (!next_block(&fd[cur], &m)) {
      if (cur == 0 && np == 2) {
        cur = 1;
        first_of_b = 1;
        splice_t = (double)nblk * nfft / rate;
        pre_holder = holder;
        printf("%7.1f s  --- splice: %s ---\n", splice_t, path[1]);
        continue;
      }

      break;
    }

    /* a capture's first block carries its own start-up reset; a splice is not an engine reset */
    if ((m.rec_flags & DIVCAP_FLAG_ENGINE_RESET && !first_of_b) || m.dropped > 0) {
      rade_corr_reset();
      rade_v2c_reset();
      holder = NONE;
      was_v1 = was_v2 = 0;
      eligible[V1] = eligible[V2] = 0;
      nreset++;
    }

    first_of_b = 0;
    nblk++;
    const double t = (double)nblk * nfft / rate;
    const float *a0 = fd[cur].buf, *a1 = fd[cur].buf + 2 * nfft;
    double w1r = 1, w1i = 0, w2r = 1, w2i = 0;
    const int u1 = rade_corr_process(a0, a1, (int)nfft, m.expect_bank, m.frame_off, m.tau, &w1r, &w1i);
    const int v1_locked = rade_corr_locked, v1_mir = rade_corr_mirrored;
    const double v1_q = rade_corr_quality;
    const int u2 = rade_v2c_process(a0, a1, (int)nfft, m.expect_bank, m.frame_off, m.tau, &w2r, &w2i);
    const int v2_locked = rade_v2c_locked, v2_present = rade_v2c_present;
    const double v2_q = rade_v2c_quality;

    if (v1_locked && !was_v1) { eligible[V1] = (holder == NONE || faded_now); }
    if (v2_locked && !was_v2) { eligible[V2] = (holder == NONE || faded_now); }
    if (!v1_locked) { eligible[V1] = 0; }
    if (!v2_locked) { eligible[V2] = 0; }

    if (v1_locked && !was_v1) {
      printf("%7.1f s  V1 pilot locks (%s)\n", t, v1_mir ? "above" : "below");

      if (t_v1 < 0) { t_v1 = t; }

      if (splice_t >= 0 && t_first_after_splice[V1] < 0) { t_first_after_splice[V1] = t - splice_t; }
    }

    if (v2_locked && !was_v2) {
      printf("%7.1f s  V2 prefix locks (%s)\n", t, rade_v2c_mirrored ? "above" : "below");

      if (t_v2 < 0) { t_v2 = t; }

      if (splice_t >= 0 && t_first_after_splice[V2] < 0) { t_first_after_splice[V2] = t - splice_t; }
    }

    if (!v1_locked && was_v1) { printf("%7.1f s  V1 lock cleared\n", t); }
    if (!v2_locked && was_v2) { printf("%7.1f s  V2 lock cleared\n", t); }

    was_v1 = v1_locked;
    was_v2 = v2_locked;

    //
    // The rule.
    //
    const int faded1 = !v1_locked || v1_q < fade_q;
    const int faded2 = !v2_locked || !v2_present;
    int want = holder;

    if (holder == V1 && !v1_locked) { want = NONE; }
    if (holder == V2 && !v2_locked) { want = NONE; }

    if (want == NONE) {
      if (v1_locked) { want = V1; }
      else if (v2_locked) { want = V2; }
    } else if (want == V1 && faded1 && v2_locked && eligible[V2]) {
      want = V2;
    } else if (want == V2 && faded2 && v1_locked && eligible[V1]) {
      want = V1;
    }

    if (want != holder) {
      printf("%7.1f s  holder %s -> %s   (V1 q %.2f, V2 dpk %.2f%s)\n", t, NAME[holder], NAME[want], v1_q, v2_q,
             v2_present ? "" : " not present");

      if (holder != NONE && want != NONE) { nhand++; }

      if (t_hold < 0 && want != NONE) { t_hold = t; }

      holder = want;
      eligible[want] = 0;
    }

    if (splice_t >= 0) {
      held[holder] += (double)nfft / rate;

      if (t_new < 0 && holder != NONE && holder != pre_holder) { t_new = t - splice_t; }
    }

    faded_now = (holder == V1) ? faded1 : (holder == V2) ? faded2 : 1;

    const int upd = (holder == V1) ? u1 : (holder == V2) ? u2 : 0;

    if (upd) {
      wr = (holder == V1) ? w1r : w2r;
      wi = (holder == V1) ? w1i : w2i;
    }

    if (csv) {
      fprintf(csv, "%.3f,%d,%s,%d,%.3f,%d,%d,%.3f,%.4f,%.4f\n", t, cur, NAME[holder], v1_locked, v1_q, v2_locked,
              v2_present, v2_q, wr, wi);
    }

    if (!quiet && t >= next) {
      next += 1.0;
      printf("%7.1f s  file %d  V1 %s q %.2f   V2 %s%s dpk %.2f   holds %s\n", t, cur, v1_locked ? "LOCK  " : "search",
             v1_q, v2_locked ? "LOCK  " : "search", v2_present ? "+" : " ", v2_q, NAME[holder]);
    }
  }

  printf("SUMMARY %s%s%s: first lock V1 %.1f s, V2 %.1f s, holder %.1f s; hand-overs %ld; resets %ld; ends holding %s",
         path[0], np == 2 ? " + " : "", np == 2 ? path[1] : "", t_v1, t_v2, t_hold, nhand, nreset, NAME[holder]);

  if (splice_t >= 0) {
    printf("; after splice (%.1f s, holder then %s) V1 locks %.1f s, V2 locks %.1f s; held V1 %.1f V2 %.1f none %.1f s; "
           "other module holds from %.1f s", splice_t, NAME[pre_holder], t_first_after_splice[V1],
           t_first_after_splice[V2], held[V1], held[V2], held[NONE], t_new);
  }

  printf("\n");
  rade_corr_stop();
  rade_v2c_stop();

  if (csv) { fclose(csv); }

  return 0;
}

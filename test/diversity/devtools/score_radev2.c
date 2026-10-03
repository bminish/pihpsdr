/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see docs/tools/radev2-scoring.md.
 *
 * Scores a .divc capture of a RADE V2 signal by decoding it, the way
 * score_rade does for V1: several rade_c V2 receivers side by side over
 * one capture, each fed a different combination of the two arms.
 *
 *   arm0, arm1   each antenna alone
 *   radio        arm0 + w*arm1 with the weight the radio recorded as
 *                applied (live_cos/live_sin), i.e. what was heard
 *   NAME         --weights NAME=FILE: a weight series from run_ref or
 *                replay_rade (columns wr, wi, ok), one row per block
 *
 * The receivers are fed the 8 kHz stream the RADE V1 correlator's own
 * NCO and decimator produce, exactly as score_rade does, so the frame
 * bookkeeping (CTUN, offset, the buffer's spectral inversion) is the one
 * already proven on air. Combining after the decimator is exact: the
 * weight is one complex scalar per block and the decimator is linear.
 * The V1 correlator's search runs alongside and is ignored.
 *
 *   ./score_radev2 cap.divc
 *   ./score_radev2 cap.divc --weights window=w.csv --weights digital=d.csv
 *   ./score_radev2 cap.divc --flip          # the other sideband sense
 *   ./score_radev2 cap.divc --csv-dir out/  # one row per symbol per stream
 *   ./score_radev2 cap.divc --iq-dir out/   # each stream's 8 kHz I/Q
 *
 * What the numbers mean, and why these ones, is in the doc above.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <gtk/gtk.h>

#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"
#include "rade_correlator.h"
#include "rade_tuning.h"
#include "diversity_capture.h"
#include "divcap_replay.h"
#include "radio.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int div_auto_mode = DIV_MANUAL;   /* TEST keeps these in radio.c */
ADC adc[3];
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double auto_div_cos = 1.0, auto_div_sin = 0.0, auto_div_gain = 0.0, auto_div_phase = 0.0;
double div_norm = 1.0;
int div_indep_att = 0;
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }
struct _vfo vfo[MAX_VFOS];

static int verbose = 0;
void t_print(const char *fmt, ...) {
  if (!verbose) { return; }

  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
}
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }

/*
 * rade_c first, then the correlator. rade_c's rade_dsp.h defines the
 * three acquisition-grid names the correlator also defines, differently;
 * take them back so the correlator searches its own grid (see the same
 * note in score_rade.c).
 */
#include "radev2_probe.h"

#undef RADE_ACQ_FRANGE
#undef RADE_ACQ_FSTEP
#undef RADE_ACQ_NFREQ

#include "rade_correlator_tunable.c"

/* ------------------------------------------------------------------ */

#define MAX_STREAM 10

struct wseq {
  double *wr, *wi;
  int     n;
};

/* Same reader as score_rade: wr/wi by column name, ok = 0 holds. */
static int wseq_load(struct wseq *w, const char *path) {
  FILE *f = fopen(path, "r");

  if (f == NULL) { perror(path); return 0; }

  char line[1024];
  int cwr = -1, cwi = -1, cok = -1, cap = 1024;
  w->n = 0;
  w->wr = malloc(sizeof(double) * cap);
  w->wi = malloc(sizeof(double) * cap);

  if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return 0; }

  {
    int col = 0;
    char *save = NULL;

    for (char *t = strtok_r(line, ",\r\n", &save); t != NULL;
         t = strtok_r(NULL, ",\r\n", &save), col++) {
      if (!strcmp(t, "wr")) { cwr = col; }
      else if (!strcmp(t, "wi")) { cwi = col; }
      else if (!strcmp(t, "ok")) { cok = col; }
    }
  }

  if (cwr < 0 || cwi < 0) {
    fprintf(stderr, "%s: no wr/wi columns in the header row\n", path);
    fclose(f);
    return 0;
  }

  double lr = 0.0, li = 0.0;

  while (fgets(line, sizeof(line), f) != NULL) {
    double vr = lr, vi = li;
    int ok = 1, col = 0;
    char *save = NULL;

    for (char *t = strtok_r(line, ",\r\n", &save); t != NULL;
         t = strtok_r(NULL, ",\r\n", &save), col++) {
      if (col == cwr) { vr = atof(t); }
      else if (col == cwi) { vi = atof(t); }
      else if (col == cok) { ok = atoi(t); }
    }

    if (!ok) { vr = lr; vi = li; }

    if (w->n >= cap) {
      cap *= 2;
      w->wr = realloc(w->wr, sizeof(double) * cap);
      w->wi = realloc(w->wi, sizeof(double) * cap);
    }

    w->wr[w->n] = vr;
    w->wi[w->n] = vi;
    w->n++;
    lr = vr;
    li = vi;
  }

  fclose(f);
  return 1;
}

/* -1 arm 0, -2 arm 1, -3 the recorded weight, >= 0 an index into wseq[] */
struct stream {
  struct v2probe p;
  int   src;
  FILE *iq;
};

/* mirrors DIV_RETUNE_HZ in diversity_auto.c, as score_rade does */
#define SCORE_RETUNE_HZ 20

static int ctx_changed(const struct divcap_block *a, const struct divcap_block *b) {
  return llabs(a->frequency      - b->frequency)      > SCORE_RETUNE_HZ ||
         llabs(a->ctun_frequency - b->ctun_frequency) > SCORE_RETUNE_HZ ||
         llabs(a->offset         - b->offset)         > SCORE_RETUNE_HZ ||
         a->ctx_sample_rate != b->ctx_sample_rate ||
         a->mode           != b->mode;
}

/*
 * Per-bin view of one stream, for the paired comparison: every stream is
 * judged on the same stretches of time.
 */
struct bins {
  int     nb;
  double *snr;     /* mean snr_est over the bin's symbols, dB              */
  double *sig;     /* fraction of the bin's symbols with the detector on   */
  double *fs;      /* mean FrameSyncNet output over the bin's decoded
                      frames; 0 if nothing was decoded in it               */
  double *aux;     /* mean |aux bit| over the bin's decoded frames; 0 if
                      nothing was decoded in it                            */
  int    *n;
};

static void bins_make(struct bins *b, const struct v2probe *p, double width, int nb) {
  int *nv = calloc((size_t)nb, sizeof(int));
  b->nb = nb;
  b->snr = calloc((size_t)nb, sizeof(double));
  b->sig = calloc((size_t)nb, sizeof(double));
  b->fs  = calloc((size_t)nb, sizeof(double));
  b->aux = calloc((size_t)nb, sizeof(double));
  b->n   = calloc((size_t)nb, sizeof(int));

  for (long i = 0; i < p->nsym; i++) {
    const int k = (int)(p->sym[i].t / width);

    if (k < 0 || k >= nb) { continue; }

    b->snr[k] += p->sym[i].snr;
    b->sig[k] += p->sym[i].sig;
    b->n[k]++;

    if (p->sym[i].valid) {
      b->fs[k] += p->sym[i].fsync;
      b->aux[k] += fabsf(p->sym[i].data);
      nv[k]++;
    }
  }

  for (int k = 0; k < nb; k++) {
    if (b->n[k] > 0) {
      b->snr[k] /= b->n[k];
      b->sig[k] /= b->n[k];
    }

    if (nv[k] > 0) {
      b->fs[k] /= nv[k];
      b->aux[k] /= nv[k];
    }
  }

  free(nv);
}

static void bins_free(struct bins *b) {
  free(b->snr);
  free(b->sig);
  free(b->fs);
  free(b->aux);
  free(b->n);
}

/*
 * One paired table: every stream from index 2 on, against the better
 * antenna and against per-bin selection, on the bins where either
 * antenna alone has a V2 signal.
 */
static void paired(const char *what, const char *unit, struct bins *b,
                   struct stream *st, int nst, int nb, int which) {
  int nsig = 0;
  double a0 = 0.0, a1 = 0.0, sel = 0.0;

#define V(i, k) (which == 2 ? b[i].aux[k] : which == 1 ? b[i].fs[k] : b[i].snr[k])

  for (int k = 0; k < nb; k++) {
    if (b[0].n[k] == 0 || b[1].n[k] == 0) { continue; }

    if (b[0].sig[k] < 0.5 && b[1].sig[k] < 0.5) { continue; }

    nsig++;
    a0 += V(0, k);
    a1 += V(1, k);
    sel += (V(0, k) > V(1, k)) ? V(0, k) : V(1, k);
  }

  if (nsig == 0) { return; }

  a0 /= nsig;
  a1 /= nsig;
  sel /= nsig;
  const double best = (a0 > a1) ? a0 : a1;
  printf("\n  %s: arm0 %.3f%s, arm1 %.3f%s, better antenna %s, per-bin selection %.3f%s\n",
         what, a0, unit, a1, unit, (a0 > a1) ? "arm0" : "arm1", sel, unit);
  printf("  %-10s %9s %9s %9s %9s\n", "stream", "mean", "vs best", "vs select", "bins won");

  for (int i = 2; i < nst; i++) {
    double sum = 0.0;
    int won = 0, n = 0;

    for (int k = 0; k < nb; k++) {
      if (b[0].n[k] == 0 || b[1].n[k] == 0 || b[i].n[k] == 0) { continue; }

      if (b[0].sig[k] < 0.5 && b[1].sig[k] < 0.5) { continue; }

      const double s = (V(0, k) > V(1, k)) ? V(0, k) : V(1, k);
      sum += V(i, k);
      won += (V(i, k) > s);
      n++;
    }

    if (n == 0) { continue; }

    printf("  %-10s %9.3f %+9.3f %+9.3f %8.1f%%\n", st[i].p.name, sum / n,
           sum / n - best, sum / n - sel, 100.0 * won / n);
  }

#undef V
}

static void usage(const char *me) {
  fprintf(stderr,
          "usage: %s FILE.divc [--weights NAME=FILE]... [--flip] [--no-agc] [--gain G]\n"
          "          [--noise SIGMA] [--seed N] [--bin SECONDS]\n"
          "          [--csv-dir DIR] [--iq-dir DIR] [-v]\n", me);
}

int main(int argc, char **argv) {
  const char *path = NULL, *csv_dir = NULL, *iq_dir = NULL;
  double noise = 0.0, bin_s = 1.0, gain = 0.0;
  unsigned seed = 0;
  int flip = 0, agc = 1;
  struct wseq wseq[MAX_STREAM];
  const char *wname[MAX_STREAM];
  int nw = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) {
      verbose = 1;
    } else if (!strcmp(argv[i], "--flip")) {
      flip = 1;
    } else if (!strcmp(argv[i], "--no-agc")) {
      agc = 0;
    } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
      gain = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--noise") && i + 1 < argc) {
      noise = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      seed = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--bin") && i + 1 < argc) {
      bin_s = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--csv-dir") && i + 1 < argc) {
      csv_dir = argv[++i];
    } else if (!strcmp(argv[i], "--iq-dir") && i + 1 < argc) {
      iq_dir = argv[++i];
    } else if (!strcmp(argv[i], "--weights") && i + 1 < argc) {
      char *a = argv[++i];
      char *eq = strchr(a, '=');

      if (eq == NULL || nw >= MAX_STREAM - 3) {
        fprintf(stderr, "score_radev2: --weights wants NAME=FILE\n");
        return 2;
      }

      *eq = '\0';
      wname[nw] = a;

      if (!wseq_load(&wseq[nw], eq + 1)) { return 1; }

      nw++;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else {
      path = argv[i];
    }
  }

  if (path == NULL || bin_s <= 0.0) { usage(argv[0]); return 2; }

  struct divcap_header h;
  long data_start = 0;
  FILE *f = divcap_open(path, &h, &data_start);

  if (f == NULL) { return 1; }

  rade_initialize();
  struct stream st[MAX_STREAM];
  const int nst = 3 + nw;
  const char *base[3] = { "arm0", "arm1", "radio" };

  for (int i = 0; i < nst; i++) {
    memset(&st[i], 0, sizeof(st[i]));
    st[i].src = (i < 3) ? -(i + 1) : (i - 3);

    if (!v2probe_open(&st[i].p, (i < 3) ? base[i] : wname[i - 3], verbose, agc)) { return 1; }
  }

  if (csv_dir != NULL || iq_dir != NULL) {
    if (csv_dir != NULL) { mkdir(csv_dir, 0755); }

    if (iq_dir != NULL) { mkdir(iq_dir, 0755); }

    for (int i = 0; i < nst; i++) {
      char fn[1024];

      if (csv_dir != NULL) {
        snprintf(fn, sizeof(fn), "%s/%s.csv", csv_dir, st[i].p.name);

        if ((st[i].p.csv_out = fopen(fn, "w")) == NULL) { perror(fn); return 1; }

        fprintf(st[i].p.csv_out, "t,sync,sig,valid,eoo,ry_max,snr,foff,data,fsync\n");
      }

      if (iq_dir != NULL) {
        snprintf(fn, sizeof(fn), "%s/%s.iq", iq_dir, st[i].p.name);

        if ((st[i].iq = fopen(fn, "wb")) == NULL) { perror(fn); return 1; }
      }
    }
  }

  if (!rade_corr_start((int)h.sample_rate)) {
    fprintf(stderr, "score_radev2: the front end will not run at %u Hz\n", h.sample_rate);
    return 1;
  }

  rade_corr_freq_off = 0.0;
  rade_corr_mirrored = 0;
  const int nfft = (int)h.nfft;
  const size_t half = (size_t)nfft * 2u * sizeof(float);
  float *arm0 = malloc(half), *arm1 = malloc(half);
  struct divcap_block m, prev, first;
  int have_prev = 0, mirror_used = -1, mirror_changes = 0;
  long blocks = 0;

  /*
   * Input level. The decimated stream sits wherever the radio's DDC
   * scaling puts it - about 6e-4 RMS on the first captures, some 40 dB
   * below the ~0.7 rade_c's V2 receiver expects. Its input AGC can make
   * up only 20 dB, and the cyclic-prefix sync is normalised and does not
   * care, so a stream that low syncs perfectly while the decoder is fed
   * near-zero latents: frame-sync confidence pinned at 0.49 and the aux
   * bit at -0.30 whatever the signal. So, unless --gain says otherwise,
   * a pre-pass over the first ten seconds measures the louder arm and one
   * fixed gain puts it at 0.5 RMS. The same gain goes to every stream,
   * so their relative levels are untouched; the AGC does the rest.
   */
  if (gain <= 0.0) {
    const long npre = (long)(10.0 * h.sample_rate / nfft) + 1;
    double p0 = 0.0, p1 = 0.0;
    long np = 0;
    divcap_noise_seed(seed);
    fseek(f, data_start, SEEK_SET);

    for (long k = 0; k < npre; k++) {
      if (fread(&m, sizeof(m), 1, f) != 1 || m.rec_magic != DIVCAP_REC_MAGIC) { break; }

      if (fread(arm0, 1, half, f) != half || fread(arm1, 1, half, f) != half) { break; }

      divcap_add_noise(arm0, arm1, nfft, noise);
      const int64_t before = ringtotal;
      double nwr, nwi;
      (void)rade_corr_process(arm0, arm1, nfft, m.expect_bank,
                              m.frame_off, m.tau, &nwr, &nwi);

      for (int64_t a = before; a < ringtotal; a++) {
        const cplx z0 = ring_get(ring0, a), z1 = ring_get(ring1, a);
        p0 += z0.re * z0.re + z0.im * z0.im;
        p1 += z1.re * z1.re + z1.im * z1.im;
        np++;
      }
    }

    const double rms = np ? sqrt(((p0 > p1) ? p0 : p1) / np) : 0.0;
    gain = (rms > 0.0) ? 0.5 / rms : 1.0;
    rade_corr_stop();

    if (!rade_corr_start((int)h.sample_rate)) { return 1; }

    rade_corr_freq_off = 0.0;
    rade_corr_mirrored = 0;
  }

  divcap_noise_seed(seed);
  fseek(f, data_start, SEEK_SET);

  for (;;) {
    if (fread(&m, sizeof(m), 1, f) != 1 || m.rec_magic != DIVCAP_REC_MAGIC) { break; }

    if (fread(arm0, 1, half, f) != half) { break; }

    if (fread(arm1, 1, half, f) != half) { break; }

    if (!have_prev) { first = m; }

    divcap_add_noise(arm0, arm1, nfft, noise);

    if (have_prev && ctx_changed(&m, &prev)) { rade_corr_reset(); }

    if (m.dropped > 0) { rade_corr_reset(); }

    const int64_t before = ringtotal;
    double nwr, nwi;
    (void)rade_corr_process(arm0, arm1, nfft, m.expect_bank,
                            m.frame_off, m.tau, &nwr, &nwi);
    /*
     * RADE V2 is transmitted upright on USB. Bank 1 is the USB bank and
     * arrives mirrored in the decimated stream, so conjugate it - the same
     * rule score_rade uses for V1. A passband that straddles the carrier
     * says nothing, and is taken as unmirrored. --flip inverts the rule,
     * for a station received on the other sideband from the one it
     * transmitted for.
     */
    int mirror = (m.expect_bank == 1);

    if (flip) { mirror = !mirror; }

    if (mirror_used >= 0 && mirror != mirror_used) { mirror_changes++; }

    mirror_used = mirror;
    const double sgn = mirror ? -1.0 : 1.0;

    for (int64_t a = before; a < ringtotal; a++) {
      const cplx z0 = ring_get(ring0, a);
      const cplx z1 = ring_get(ring1, a);

      for (int i = 0; i < nst; i++) {
        double ar, ai;

        if (st[i].src == -1) {
          ar = z0.re;
          ai = z0.im;
        } else if (st[i].src == -2) {
          ar = z1.re;
          ai = z1.im;
        } else {
          double ur, ui;

          if (st[i].src == -3) {
            ur = m.live_cos;
            ui = m.live_sin;
          } else {
            const struct wseq *q = &wseq[st[i].src];
            const int k = (blocks < q->n) ? (int)blocks : (q->n - 1);
            ur = (k >= 0) ? q->wr[k] : 0.0;
            ui = (k >= 0) ? q->wi[k] : 0.0;
          }

          ar = z0.re + (ur * z1.re - ui * z1.im);
          ai = z0.im + (ur * z1.im + ui * z1.re);
        }

        const float o[2] = { (float)(gain * ar), (float)(gain * sgn * ai) };
        v2probe_push(&st[i].p, o[0], o[1]);

        if (st[i].iq != NULL) { fwrite(o, sizeof(float), 2, st[i].iq); }
      }
    }

    for (int i = 0; i < nst; i++) { v2probe_drain(&st[i].p); }

    prev = m;
    have_prev = 1;
    blocks++;
  }

  rade_corr_stop();
  fclose(f);
  free(arm0);
  free(arm1);

  if (!have_prev) {
    fprintf(stderr, "score_radev2: no blocks in %s\n", path);
    return 1;
  }

  printf("# %s\n# rate %u Hz, nfft %u, %ld blocks, mode %d, filter %d..%d Hz,"
         " dial %.6f MHz\n",
         path, h.sample_rate, h.nfft, blocks, first.mode, first.filter_low,
         first.filter_high, first.frequency * 1e-6);
  printf("# input gain %.1f dB\n", 20.0 * log10(gain));
  printf("# sense: %s%s%s\n", mirror_used ? "conjugated (USB bank)" : "as tapped (LSB bank)",
         flip ? ", --flip" : "", mirror_changes ? ", CHANGED during the capture" : "");
  printf("\n%-10s %7s %6s %6s %7s %4s %4s %8s %8s %7s %7s\n",
         "stream", "seconds", "sync%", "sig%", "frames", "acq", "eoo",
         "snr_sig", "snr_sync", "fsync", "|aux|");
  double aux_med = 0.0;

  for (int i = 0; i < nst; i++) {
    struct v2summary s;
    v2probe_summary(&st[i].p, &s);
    printf("%-10s %7.1f %5.1f%% %5.1f%% %7ld %4d %4ld %8.2f %8.2f %7.3f %7.3f\n",
           st[i].p.name, s.seconds, 100.0 * s.sync_frac, 100.0 * s.sig_frac,
           s.frames, s.acq, s.eoo, s.snr_sig, s.snr_sync, s.fsync, s.data_abs);

    if (i < 2 && isfinite(s.data_med) && s.data_med > aux_med) { aux_med = s.data_med; }
  }

  /*
   * The aux bit is a quality measure only if the station sends aux bits
   * (+/-1 per frame); then a sure decoder puts its soft output near +/-1.
   * A station whose bits are not recognisable leaves |aux| low on every
   * stream; say so rather than let that be read as a quality figure.
   * Check the input gain first: a stream decoded too low reads exactly
   * like this (see the note at the pre-pass).
   */
  printf("# aux: %s (median |aux| %.2f on the better antenna)\n",
         aux_med > 0.7 ? "the station sends aux bits; |aux| is the lead measure" :
         "NO usable aux bits on this capture - lead with frame-sync confidence",
         aux_med);

  /*
   * Paired comparison. Every stream is scored on the same bins: those
   * where either antenna alone has the detector on for at least half of
   * the bin's symbols, which is "a V2 signal is there" judged without
   * reference to any weight. Against the better antenna over those bins,
   * and against picking the better antenna bin by bin (selection
   * diversity with hindsight), which is what combining has to beat to be
   * worth more than a switch.
   *
   * Three measures, in the order the calibration trusts them (see the
   * doc): the decoder's confidence in the aux bit, the frame-sync
   * network's confidence in the decoded latents, and the CP SNR estimate,
   * which ranks a combination against an antenna wrongly on multipath.
   */
  double tmax = 0.0;

  for (int i = 0; i < nst; i++) {
    const struct v2probe *p = &st[i].p;

    if (p->nsym > 0 && p->sym[p->nsym - 1].t > tmax) { tmax = p->sym[p->nsym - 1].t; }
  }

  const int nb = (int)(tmax / bin_s) + 1;
  struct bins b[MAX_STREAM];
  int nsig = 0;

  for (int i = 0; i < nst; i++) { bins_make(&b[i], &st[i].p, bin_s, nb); }

  for (int k = 0; k < nb; k++) {
    if (b[0].n[k] > 0 && b[1].n[k] > 0 && (b[0].sig[k] >= 0.5 || b[1].sig[k] >= 0.5)) { nsig++; }
  }

  printf("\npaired, over %d of %d %.2f s bin(s) with a V2 signal on either antenna:\n",
         nsig, nb, bin_s);

  if (nsig == 0) {
    printf("  none - nothing to compare\n");
  } else {
    paired("decoded |aux| (lead measure; 95% of pairs ranked right in calibration)", "", b, st, nst, nb, 2);
    paired("frame-sync confidence (93%)", "", b, st, nst, nb, 1);
    paired("CP SNR estimate (89%; wrong on multipath, see the doc)", " dB", b, st, nst, nb, 0);
  }

  for (int i = 0; i < nst; i++) {
    if (st[i].p.csv_out != NULL) { fclose(st[i].p.csv_out); }

    if (st[i].iq != NULL) { fclose(st[i].iq); }

    v2probe_close(&st[i].p);

    bins_free(&b[i]);
  }

  rade_finalize();
  return 0;
}

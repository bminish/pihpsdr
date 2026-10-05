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

#define MAX_STREAM 24
#define MAX_BLIND   6
#define MAX_RX2     8

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


/* ------------------------------------------------------------------ */
/*
 * The blind scalar reference, docs/diversity-radeV2-combining.md section 8.
 *
 * Both arms (in the V2 sense: conjugated on the mirrored bank) go through
 * the receiver's own band filter, 975 Hz at 1469 Hz, and the running
 *     p0 = <|x0|^2>,  p1 = <|x1|^2>,  c = <x1 conj(x0)>
 * are kept with a time constant of TAU symbols (160 samples each). With the
 * transmitted symbol cancelling, R = c / p0 is h1/h0 in the band, and the
 * combination is the noise-weighted MRC at arm 0's phase:
 *     out = (x0 + rho conj(R) x1) / sqrt(1 + rho |R|^2),  rho = n0 / n1
 * where n0, n1 are the arms' in-band noise and n0 is taken off p0. Mode k
 * measures them in two guard bands (500-900 and 1950-2350 Hz, the lower
 * density of the two, scaled to the signal band): no use when the operator's
 * passband has no empty part. Mode c takes n = p (1 - rho_cp) from each arm's
 * own V2 receiver's CP correlation. Mode u skips it (rho = 1, nothing taken
 * off): right only for arms with equal noise.
 *
 * It is an estimator on the 8 kHz stream and needs no symbol timing: the
 * pooled covariance over the carriers is the in-band time-domain
 * correlation. The weight changes every sample unless the stream is "hold":
 * then it is latched at the start of each capture block from the end of the
 * one before, which is how the engine can apply a weight at best.
 */
#define BP_NT 101

struct bpf {
  double h[BP_NT], pr, pi, ir, ii;
  double mr[BP_NT], mi[BP_NT];
  int    pos;
};

static void bpf_init(struct bpf *b, double centre, double bw, double fs) {
  memset(b, 0, sizeof(*b));
  const double B = bw / fs;

  for (int i = 0; i < BP_NT; i++) {
    const double n = i - (BP_NT - 1) / 2, x = M_PI * n * B;
    b->h[i] = B * (x == 0.0 ? 1.0 : sin(x) / x);
  }

  b->pr = 1.0;
  b->ir = cos(2.0 * M_PI * centre / fs);
  b->ii = -sin(2.0 * M_PI * centre / fs);
}

/* mix down, low-pass; the mix back up is not needed for powers and for
   products of two arms mixed the same way */
static void bpf_step(struct bpf *b, double xr, double xi, double *yr, double *yi) {
  const double nr = b->pr * b->ir - b->pi * b->ii, ni = b->pr * b->ii + b->pi * b->ir;
  b->pr = nr;
  b->pi = ni;
  b->pos = (b->pos + 1) % BP_NT;
  b->mr[b->pos] = xr * nr - xi * ni;
  b->mi[b->pos] = xr * ni + xi * nr;
  double ar = 0.0, ai = 0.0;

  for (int k = 0; k < BP_NT; k++) {
    const int j = (b->pos - k + BP_NT) % BP_NT;
    ar += b->h[k] * b->mr[j];
    ai += b->h[k] * b->mi[j];
  }

  *yr = ar;
  *yi = ai;
}

struct blindcfg {
  const char *name;
  double tau;      /* symbols */
  int    noisek;   /* 1: noise-aware (mode k), 2: from the arms' own CP correlation (mode c), 0: mode u */
  int    hold;
};

struct blindst {
  struct blindcfg c;
  double a, wsum, p0, p1, cr, ci;
  double wr, wi, nrm;     /* current: w for out = nrm*(x0 + w x1), in the V2 domain */
  double lwr, lwi, lnrm;  /* latched at the start of the block */
};

/* shared front end: the filters every blind stream reads */
struct blindfe {
  struct bpf sig[2], glo[2], ghi[2];
  double psig[2], pglo[2], pghi[2];   /* running powers, 1 s */
  double n[2];                        /* in-band noise estimate per arm */
  double cp[2];                       /* each arm's own CP correlation peak, as of the last block */
  double xr[2], xi[2];                /* the filtered signal band, this sample */
  int    ready;
};

static void blindfe_init(struct blindfe *f) {
  memset(f, 0, sizeof(*f));

  for (int k = 0; k < 2; k++) {
    bpf_init(&f->sig[k], 1468.75, 975.0, 8000.0);
    bpf_init(&f->glo[k], 700.0, 400.0, 8000.0);
    bpf_init(&f->ghi[k], 2150.0, 400.0, 8000.0);
  }
}

static void blindfe_step(struct blindfe *f, const double xr[2], const double xi[2]) {
  const double al = exp(-1.0 / 8000.0);

  for (int k = 0; k < 2; k++) {
    double gr, gi, hr, hi;
    bpf_step(&f->sig[k], xr[k], xi[k], &f->xr[k], &f->xi[k]);
    bpf_step(&f->glo[k], xr[k], xi[k], &gr, &gi);
    bpf_step(&f->ghi[k], xr[k], xi[k], &hr, &hi);
    f->pglo[k] = al * f->pglo[k] + (1.0 - al) * (gr * gr + gi * gi);
    f->pghi[k] = al * f->pghi[k] + (1.0 - al) * (hr * hr + hi * hi);
    /* density per Hz, the lower guard, times the signal band; the lowpass
       passes bw of each, so the bands' own widths are 400 and 975 */
    const double dlo = f->pglo[k] / 400.0, dhi = f->pghi[k] / 400.0;
    f->n[k] = ((dlo < dhi) ? dlo : dhi) * 975.0;
  }

  f->ready = 1;
}

static void blind_init(struct blindst *b, const struct blindcfg *c) {
  memset(b, 0, sizeof(*b));
  b->c = *c;
  b->a = exp(-1.0 / (c->tau * 160.0));
  b->wr = b->lwr = 0.0;
  b->nrm = b->lnrm = 1.0;
}

static void blind_step(struct blindst *b, const struct blindfe *f) {
  const double a = b->a;
  b->wsum = a * b->wsum + (1.0 - a);
  b->p0 = a * b->p0 + (1.0 - a) * (f->xr[0] * f->xr[0] + f->xi[0] * f->xi[0]);
  b->p1 = a * b->p1 + (1.0 - a) * (f->xr[1] * f->xr[1] + f->xi[1] * f->xi[1]);
  /* x1 conj(x0) */
  b->cr = a * b->cr + (1.0 - a) * (f->xr[1] * f->xr[0] + f->xi[1] * f->xi[0]);
  b->ci = a * b->ci + (1.0 - a) * (f->xi[1] * f->xr[0] - f->xr[1] * f->xi[0]);
  const double p0 = b->p0 / b->wsum, cr = b->cr / b->wsum, ci = b->ci / b->wsum;
  double den = p0, rho = 1.0;

  if (b->c.noisek) {
    double n0 = f->n[0], n1 = f->n[1];

    if (b->c.noisek == 2) {   /* from each arm's own receiver: noise share = 1 - CP correlation */
      n0 = p0 * (1.0 - f->cp[0]);
      n1 = (b->p1 / b->wsum) * (1.0 - f->cp[1]);
    }

    den = p0 - n0;

    if (den < 0.1 * p0) { den = 0.1 * p0; }

    rho = (n1 > 0.0) ? n0 / n1 : 1.0;
  }

  if (den <= 0.0) { return; }

  const double Rr = cr / den, Ri = ci / den;                   /* h1/h0 */
  const double R2 = Rr * Rr + Ri * Ri;
  b->wr = rho * Rr;                                            /* rho conj(R) */
  b->wi = -rho * Ri;
  b->nrm = 1.0 / sqrt(1.0 + rho * R2);
}

/* -1 arm 0, -2 arm 1, -3 the recorded weight, 0.. an index into wseq[], 1000.. a blind stream */
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

/* What one pair of 8 kHz samples is fed to: every stream. */
struct feed {
  struct stream   *st;
  int              nst, nbl;
  struct blindst  *bst;
  struct blindfe  *bfe;
  struct rx2_cfg  *xcfg;
  struct wseq     *wseq;
  double           gain, sgn, live_cos, live_sin;
  int              mirror;
  long             blocks;
  int              iqmode;       /* --iq2: stream 2 is the plain sum of the arms, not the radio's weight */
};

static void feed_sample(const struct feed *F, cplx z0, cplx z1) {
  struct stream *st = F->st;
  struct blindst *bst = F->bst;
  struct blindfe *bfe = F->bfe;
  struct rx2_cfg *xcfg = F->xcfg;
  struct wseq *wseq = F->wseq;
  const int nst = F->nst, nbl = F->nbl, mirror = F->mirror;
  const double gain = F->gain, sgn = F->sgn;
  const long blocks = F->blocks;
  if (nbl > 0) {                    /* the V2 domain: conjugated on the mirrored bank */
    const double xr[2] = { z0.re, z1.re }, xi[2] = { sgn * z0.im, sgn * z1.im };
    blindfe_step(bfe, xr, xi);

    for (int j = 0; j < nbl; j++) { blind_step(&bst[j], bfe); }
  }

  for (int i = 0; i < nst; i++) {
    double ar, ai, nrm = 1.0;

    if (st[i].src >= 2000) {        /* the two-input receiver: both arms, no weight */
      /* sync == 2: the third stream is the first --blind stream's output, the engine's own
         time-domain combination, in the V2 domain */
      double cr = 0.0, ci = 0.0;

      if (xcfg[st[i].src - 2000].sync_both == 2 && nbl > 0) {
        const struct blindst *q = &bst[0];
        const double wr = q->c.hold ? q->lwr : q->wr, wi = q->c.hold ? q->lwi : q->wi;
        const double k = q->c.hold ? q->lnrm : q->nrm;
        const double x0r = z0.re, x0i = sgn * z0.im, x1r = z1.re, x1i = sgn * z1.im;
        cr = k * x0r + k * (wr * x1r - wi * x1i);
        ci = k * x0i + k * (wr * x1i + wi * x1r);
      }

      v2probe_push2(&st[i].p, (float)(gain * z0.re), (float)(gain * sgn * z0.im),
                    (float)(gain * z1.re), (float)(gain * sgn * z1.im),
                    (float)(gain * cr), (float)(gain * ci));
      continue;
    }

    if (st[i].src == -1) {
      ar = z0.re;
      ai = z0.im;
    } else if (st[i].src == -2) {
      ar = z1.re;
      ai = z1.im;
    } else {
      double ur, ui;

      if (st[i].src == -3) {
        ur = F->live_cos;
        ui = F->live_sin;
      } else if (st[i].src >= 1000) {
        const struct blindst *q = &bst[st[i].src - 1000];
        const double wr = q->c.hold ? q->lwr : q->wr, wi = q->c.hold ? q->lwi : q->wi;
        const double k = q->c.hold ? q->lnrm : q->nrm;
        /* the weight is in the V2 domain; the sum below is not */
        ur = k * wr;
        ui = k * (mirror ? -wi : wi);
        nrm = k;
      } else {
        const struct wseq *q = &wseq[st[i].src];
        const int k = (blocks < q->n) ? (int)blocks : (q->n - 1);
        ur = (k >= 0) ? q->wr[k] : 0.0;
        ui = (k >= 0) ? q->wi[k] : 0.0;
      }

      ar = nrm * z0.re + (ur * z1.re - ui * z1.im);
      ai = nrm * z0.im + (ur * z1.im + ui * z1.re);
    }

    const float o[2] = { (float)(gain * ar), (float)(gain * sgn * ai) };
    v2probe_push(&st[i].p, o[0], o[1]);

    if (st[i].iq != NULL) { fwrite(o, sizeof(float), 2, st[i].iq); }
  }
}

static void usage(const char *me) {
  fprintf(stderr,
          "usage: %s FILE.divc [--weights NAME=FILE]... [--blind NAME=TAU,k|c|u[,hold]]...\n"
          "          [--rx2 NAME=SYNC,COMB[,TAU[,TAUN[,NB]]]]...\n"
          "          [--flip] [--no-agc] [--gain G]\n"
          "          [--noise SIGMA] [--seed N] [--bin SECONDS]\n"
          "          [--csv-dir DIR] [--iq-dir DIR] [-v]\n", me);
}

int main(int argc, char **argv) {
  const char *path = NULL, *csv_dir = NULL, *iq_dir = NULL, *iq2[2] = { NULL, NULL };
  double noise = 0.0, bin_s = 1.0, gain = 0.0;
  unsigned seed = 0;
  int flip = 0, agc = 1;
  struct wseq wseq[MAX_STREAM];
  const char *wname[MAX_STREAM];
  int nw = 0, nbl = 0, nx2 = 0;
  struct blindcfg bcfg[MAX_BLIND];
  struct rx2_cfg xcfg[MAX_RX2];
  const char *xname[MAX_RX2];

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
    } else if (!strcmp(argv[i], "--iq2") && i + 2 < argc) {
      iq2[0] = argv[++i];
      iq2[1] = argv[++i];
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
    } else if (!strcmp(argv[i], "--blind") && i + 1 < argc) {
      char *a = argv[++i];
      char *eq = strchr(a, '=');
      double tau;
      char mode[8] = "k", hold[8] = "";

      if (eq == NULL || nbl >= MAX_BLIND ||
          sscanf(eq + 1, "%lf,%7[^,],%7s", &tau, mode, hold) < 1) {
        fprintf(stderr, "score_radev2: --blind wants NAME=TAU,k|c|u[,hold]\n");
        return 2;
      }

      *eq = '\0';
      bcfg[nbl].name = a;
      bcfg[nbl].tau = tau;
      bcfg[nbl].noisek = (mode[0] == 'u') ? 0 : (mode[0] == 'c') ? 2 : 1;
      bcfg[nbl].hold = (hold[0] == 'h');
      nbl++;
    } else if (!strcmp(argv[i], "--rx2") && i + 1 < argc) {
      char *a = argv[++i];
      char *eq = strchr(a, '=');
      int sy = 1, co = 2;
      double tau = 6.0, taun = 4.0;
      int nbr = 1;

      if (eq == NULL || nx2 >= MAX_RX2 ||
          sscanf(eq + 1, "%d,%d,%lf,%lf,%d", &sy, &co, &tau, &taun, &nbr) < 2) {
        fprintf(stderr, "score_radev2: --rx2 wants NAME=SYNC,COMB[,TAU[,TAUN[,NB]]]\n");
        return 2;
      }

      *eq = '\0';
      xname[nx2] = a;
      xcfg[nx2].sync_both = sy;
      xcfg[nx2].comb = co;
      xcfg[nx2].tau = (float)tau;
      xcfg[nx2].tau_n = (float)taun;
      xcfg[nx2].nb = nbr;
      nx2++;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else {
      path = argv[i];
    }
  }

  const int iqmode = (iq2[0] != NULL);

  if ((path == NULL && !iqmode) || bin_s <= 0.0) { usage(argv[0]); return 2; }

  struct divcap_header h;
  long data_start = 0;
  FILE *f = NULL;
  memset(&h, 0, sizeof(h));

  if (!iqmode) {
    f = divcap_open(path, &h, &data_start);

    if (f == NULL) { return 1; }
  }

  rade_initialize();
  struct stream st[MAX_STREAM];
  const int nst = 3 + nw + nbl + nx2;
  const char *base[3] = { "arm0", "arm1", iqmode ? "sumlr" : "radio" };
  struct blindst bst[MAX_BLIND];
  struct blindfe bfe;
  blindfe_init(&bfe);

  for (int i = 0; i < nst; i++) {
    memset(&st[i], 0, sizeof(st[i]));
    const char *nm;

    if (i < 3) {
      st[i].src = -(i + 1);
      nm = base[i];
    } else if (i < 3 + nw) {
      st[i].src = i - 3;
      nm = wname[i - 3];
    } else if (i < 3 + nw + nbl) {
      st[i].src = 1000 + (i - 3 - nw);
      nm = bcfg[i - 3 - nw].name;
      blind_init(&bst[i - 3 - nw], &bcfg[i - 3 - nw]);
    } else {
      st[i].src = 2000 + (i - 3 - nw - nbl);
      nm = xname[i - 3 - nw - nbl];
    }

    if (st[i].src >= 2000) {
      if (!v2probe_open2(&st[i].p, nm, &xcfg[st[i].src - 2000], agc)) { return 1; }
    } else if (!v2probe_open(&st[i].p, nm, verbose, agc)) { return 1; }
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

  struct divcap_block first;
  long blocks = 0;
  int mirror_used = 0, mirror_changes = 0, have_prev = 0;
  memset(&first, 0, sizeof(first));
  float *iqd[2] = { NULL, NULL };

  if (iqmode) {
    /*
     * Two 8 kHz complex files (py/wav2iq.py: a binaural WAV's two ears), fed to the same streams as
     * a capture's arms, with no V1 front end and no mirroring: the V2 carriers are in the audio.
     * The level pre-pass is the same: the louder arm's first ten seconds at 0.5 RMS.
     */
    long n[2];

    for (int k = 0; k < 2; k++) {
      FILE *q = fopen(iq2[k], "rb");

      if (q == NULL) { perror(iq2[k]); return 1; }

      fseek(q, 0, SEEK_END);
      n[k] = ftell(q) / (long)(2 * sizeof(float));
      fseek(q, 0, SEEK_SET);
      iqd[k] = malloc((size_t)n[k] * 2 * sizeof(float));

      if (fread(iqd[k], 2 * sizeof(float), (size_t)n[k], q) != (size_t)n[k]) { return 1; }

      fclose(q);
    }

    const long nn = (n[0] < n[1]) ? n[0] : n[1];

    if (gain <= 0.0) {
      const long npre = (nn < 80000) ? nn : 80000;
      double p0 = 0.0, p1 = 0.0;

      for (long i = 0; i < 2 * npre; i++) { p0 += iqd[0][i] * iqd[0][i]; p1 += iqd[1][i] * iqd[1][i]; }

      const double rms = sqrt(((p0 > p1) ? p0 : p1) / (double)npre);
      gain = (rms > 0.0) ? 0.5 / rms : 1.0;
    }

    const long chunk = 683;             /* one capture block's worth, 16384 samples at 192 kHz */
    struct feed F = { st, nst, nbl, bst, &bfe, xcfg, wseq, gain, 1.0, 1.0, 0.0, 0, 0, 1 };

    for (long i0 = 0; i0 < nn; i0 += chunk) {
      for (int k = 0; k < 2; k++) {
        double r = st[k].p.r->rx_v2.Ry_max;
        bfe.cp[k] = (r < 0.0) ? 0.0 : (r > 0.98) ? 0.98 : r;
      }

      for (int j = 0; j < nbl; j++) {
        bst[j].lwr = bst[j].wr;
        bst[j].lwi = bst[j].wi;
        bst[j].lnrm = bst[j].nrm;
      }

      F.blocks = blocks;
      const long i1 = (i0 + chunk < nn) ? i0 + chunk : nn;

      for (long i = i0; i < i1; i++) {
        const cplx z0 = { iqd[0][2 * i], iqd[0][2 * i + 1] }, z1 = { iqd[1][2 * i], iqd[1][2 * i + 1] };
        feed_sample(&F, z0, z1);
      }

      for (int i = 0; i < nst; i++) { v2probe_drain(&st[i].p); }

      blocks++;
    }

    have_prev = 1;
  } else {
    if (!rade_corr_start((int)h.sample_rate)) {
      fprintf(stderr, "score_radev2: the front end will not run at %u Hz\n", h.sample_rate);
      return 1;
    }

    rade_corr_freq_off = 0.0;
    rade_corr_mirrored = 0;
    const int nfft = (int)h.nfft;
    const size_t half = (size_t)nfft * 2u * sizeof(float);
    float *arm0 = malloc(half), *arm1 = malloc(half);
    struct divcap_block m, prev;
    mirror_used = -1;

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

      for (int k = 0; k < 2; k++) {       /* each arm's CP correlation, from the receivers just run */
        double r = st[k].p.r->rx_v2.Ry_max;
        bfe.cp[k] = (r < 0.0) ? 0.0 : (r > 0.98) ? 0.98 : r;
      }

      for (int j = 0; j < nbl; j++) {      /* the hold variants take last block's answer */
        bst[j].lwr = bst[j].wr;
        bst[j].lwi = bst[j].wi;
        bst[j].lnrm = bst[j].nrm;
      }

      struct feed F = { st, nst, nbl, bst, &bfe, xcfg, wseq, gain, sgn, m.live_cos, m.live_sin, mirror, blocks, 0 };

      for (int64_t a = before; a < ringtotal; a++) {
        feed_sample(&F, ring_get(ring0, a), ring_get(ring1, a));
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
  }

  if (!have_prev) {
    fprintf(stderr, "score_radev2: no blocks in %s\n", path);
    return 1;
  }

  if (iqmode) {
    printf("# %s + %s\n# 8 kHz complex, %ld chunks of 683 samples\n", iq2[0], iq2[1], blocks);
  } else {
    printf("# %s\n# rate %u Hz, nfft %u, %ld blocks, mode %d, filter %d..%d Hz,"
           " dial %.6f MHz\n",
           path, h.sample_rate, h.nfft, blocks, first.mode, first.filter_low,
           first.filter_high, first.frequency * 1e-6);
  }

  printf("# input gain %.1f dB\n", 20.0 * log10(gain));

  if (nbl > 0) {                        /* what the blind estimator ended on: is its noise measurement sane? */
    printf("# blind: in-band power arm0 %.3g arm1 %.3g; noise from the guard bands arm0 %.3g arm1 %.3g"
           " (%.1f%% / %.1f%% of in-band); |R| %.2f\n",
           bst[0].p0 / bst[0].wsum, bst[0].p1 / bst[0].wsum, bfe.n[0], bfe.n[1],
           100.0 * bfe.n[0] / (bst[0].p0 / bst[0].wsum), 100.0 * bfe.n[1] / (bst[0].p1 / bst[0].wsum),
           sqrt(bst[0].cr * bst[0].cr + bst[0].ci * bst[0].ci) / (bst[0].p0 + 1e-30) / 1.0);
  }
  printf("# sense: %s%s%s\n", iqmode ? "as given (audio domain)" : mirror_used ? "conjugated (USB bank)" : "as tapped (LSB bank)",
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

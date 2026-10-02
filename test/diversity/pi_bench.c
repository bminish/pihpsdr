/*
 * pi_bench - ballpark costs of the diversity engine's hot spots, for a
 * Raspberry Pi 5 (or any machine), with nothing but a C compiler.
 *
 * Make the single file to copy (the engine's selection code is spliced
 * in, so it is what the radio runs, not a copy that can drift):
 *
 *   make -C test/diversity pi_bench_standalone.c
 *   scp test/diversity/pi_bench_standalone.c pi@<your-pi>:
 *
 * On the Pi:
 *
 *   cc -O3 -o pi_bench pi_bench_standalone.c -lm
 *   ./pi_bench > pi_bench.txt
 *
 * With FFTW's single-precision library installed (libfftw3-dev - the
 * radio needs it anyway) add the transforms, which are the bulk of the
 * engine's per-block cost:
 *
 *   cc -O3 -DWITH_FFTW -o pi_bench pi_bench_standalone.c -lfftw3f -lm
 *
 * -O3 because the radio's own Makefile builds with it. Takes about five
 * minutes (FFTW_PATIENT at 65536 points is the slow part); send back pi_bench.txt. Exit status 1 if the selection ever
 * disagrees with qsort.
 *
 * What it measures, each as the engine does it per analysis block:
 *
 *   1. The noise floor's order statistics (LC-025, LC-033): qsort, as
 *      on TEST, against the selection on test/quickselect. With an
 *      identity check on this machine's libc and compiler.
 *   2. The two qsorts still in the engine, and what a selection would
 *      buy there: the FSK/Digital occupancy median (div_digital_solve)
 *      and the CW off-tone floor (div_cw_floor).
 *   3. The floor's gather: |X|^2 of 1024 strided bins of each arm's
 *      spectrum, which is memory traffic rather than arithmetic.
 *   4. A per-bin cross-spectral accumulation over the analysis window,
 *      the shape of the Window reference's inner loop. Approximate.
 *   5. With -DWITH_FFTW: one complex forward transform per size, planned
 *      FFTW_ESTIMATE as the engine plans it. The engine does two a block.
 *   6. With -DWITH_FFTW: the same transforms planned FFTW_MEASURE and
 *      FFTW_PATIENT, with the time each plan takes, and the time to plan
 *      again from saved wisdom (which is how MEASURE could be used
 *      without stalling the radio when diversity is switched on).
 *   7. The RADE V1 decimator (rade_corr_process(): DDC rate -> 8 kHz, 16
 *      taps a phase, double accumulators over a circular float delay
 *      line), against the same filter written to vectorise: planar float
 *      delay lines, doubled so there is no wrap test, summed in eight
 *      lanes. And that leaner filter decimating to 192 kHz, the cost of
 *      the "decimate before the FFT" idea.
 *
 * Two timings for the costly items:
 *
 *   hot    - called back to back; the core is at full clock and the
 *            caches are warm. The best case.
 *   paced  - one call, then sleep 85 ms, as the radio's worker does at
 *            12 Hz bins. On a desktop under the powersave governor this
 *            cost five times the hot figure for the same work, so it is
 *            the one that predicts the radio. Median of the calls.
 *
 * "% core" converts a per-block cost to a share of one core at 11.7
 * blocks per second (12 Hz bins, every rate up to 768 kHz; at 1536 kHz
 * the engine runs 23.4 blocks a second and the share doubles).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef WITH_FFTW
#include <fftw3.h>
#endif

/* ---- The engine's code, spliced in by make ---------------------------- */
#include "nf_select.inc"
/* ----------------------------------------------------------------------- */

#define DIV_NF_PCT    10
#define DIV_NF_BAND   2
#define BLOCKS_PER_S  11.7
#define PACE_NS       85300000L
#define NMAX          4096

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;
  return (x > y) - (x < y);
}

/* ---- What the engine does, and the alternatives ----------------------- */

//
// The noise floor before LC-033: sort, then the mean of the band.
//
static double floor_qsort(double *a, int n) {
  int ilo = (n * (DIV_NF_PCT - DIV_NF_BAND)) / 100;
  int ihi = (n * (DIV_NF_PCT + DIV_NF_BAND)) / 100;

  if (ihi >= n) { ihi = n - 1; }

  qsort(a, (size_t)n, sizeof(double), cmp);
  double sum = 0.0;

  for (int i = ilo; i <= ihi; i++) { sum += a[i]; }

  return sum / (double)(ihi - ilo + 1);
}

//
// The noise floor since LC-033: the engine's own div_nf_band_mean().
//
static double floor_select(double *a, int n) {
  int ilo = (n * (DIV_NF_PCT - DIV_NF_BAND)) / 100;
  int ihi = (n * (DIV_NF_PCT + DIV_NF_BAND)) / 100;

  if (ihi >= n) { ihi = n - 1; }

  return div_nf_band_mean(a, n, ilo, ihi);
}

//
// div_digital_solve(): the median of the region's bin powers, by qsort.
//
static double median_qsort(double *a, int n) {
  qsort(a, (size_t)n, sizeof(double), cmp);
  return a[n / 2];
}

//
// ...and by selection. Exact: one order statistic, nothing summed.
//
static double median_select(double *a, int n) {
  div_nf_select(a, 0, n - 1, n / 2);
  return a[n / 2];
}

//
// div_cw_floor(): the mean of the lower half, by qsort.
//
static double lowhalf_qsort(double *a, int n) {
  qsort(a, (size_t)n, sizeof(double), cmp);
  double sum = 0.0;

  for (int i = 0; i < n / 2; i++) { sum += a[i]; }

  return sum / (double)(n / 2);
}

//
// ...and by selection, sorting only the lower half so the sum is the
// same bit for bit.
//
static double lowhalf_select(double *a, int n) {
  div_nf_select(a, 0, n - 1, n / 2 - 1);
  qsort(a, (size_t)(n / 2), sizeof(double), cmp);
  double sum = 0.0;

  for (int i = 0; i < n / 2; i++) { sum += a[i]; }

  return sum / (double)(n / 2);
}

/* ---- Data -------------------------------------------------------------- */

static double urand(void) { return (rand() + 1.0) / ((double)RAND_MAX + 2.0); }

//
// Bin powers of complex Gaussian noise are exponential; a fraction occ
// of bins carry a station 10 to 60 dB up.
//
static void fill_band(double *a, int n, double occ) {
  for (int i = 0; i < n; i++) {
    a[i] = -log(urand());

    if (urand() < occ) { a[i] *= pow(10.0, 1.0 + 5.0 * urand()); }
  }
}

/* ---- Timing ------------------------------------------------------------ */

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

static void pace(void) {
  struct timespec t = { 0, PACE_NS };
  nanosleep(&t, NULL);
}

static int dcmp(const void *a, const void *b) { return cmp(a, b); }

typedef double (*statfn)(double *, int);

#define NSRC 64
static double src[NSRC][NMAX], work[NMAX];

static void prepare(int n, double occ) {
  for (int s = 0; s < NSRC; s++) { fill_band(src[s], n, occ); }
}

//
// Hot: best of five runs of reps calls, the copy's cost taken off.
//
static double time_hot(statfn f, int n, int reps) {
  volatile double sink = 0.0;
  double best = 1e9;

  for (int run = 0; run < 5; run++) {
    double t0 = now();

    for (int r = 0; r < reps; r++) { memcpy(work, src[r % NSRC], n * sizeof(double)); sink += work[r % n]; }

    const double tc = now() - t0;
    t0 = now();

    for (int r = 0; r < reps; r++) {
      memcpy(work, src[r % NSRC], n * sizeof(double));
      sink += f(work, n);
    }

    const double t = (now() - t0 - tc) / reps;

    if (t < best) { best = t; }
  }

  (void)sink;
  return 1e6 * best;
}

//
// Paced: one call after each 85 ms sleep, median of calls.
//
static double time_paced(statfn f, int n, int calls) {
  static double t[256];
  volatile double sink = 0.0;

  for (int c = 0; c < calls; c++) {
    memcpy(work, src[c % NSRC], n * sizeof(double));
    pace();
    const double t0 = now();
    sink += f(work, n);
    t[c] = now() - t0;
  }

  (void)sink;
  qsort(t, (size_t)calls, sizeof(double), dcmp);
  return 1e6 * t[calls / 2];
}

static void row(const char *what, int n, double q, double s, int arms) {
  printf("  %-30s %5d  %9.1f %9.1f  %5.1fx   %5.2f%% -> %5.2f%%\n", what, n, q * arms, s * arms,
         q / s, 100.0 * q * arms * 1e-6 * BLOCKS_PER_S, 100.0 * s * arms * 1e-6 * BLOCKS_PER_S);
}

/* ---- Identity ---------------------------------------------------------- */

static int identity(void) {
  static double a[NMAX], b[NMAX];
  long cases = 0, bad = 0;

  for (int kind = 0; kind < 4; kind++) {
    for (int n = 128; n <= 1024; n++) {
      for (int d = 0; d < 2; d++) {
        switch (kind) {
        case 0: fill_band(a, n, 0.0); break;

        case 1: fill_band(a, n, 0.41); break;

        case 2: for (int i = 0; i < n; i++) { a[i] = (double)(rand() % 8); } break;

        case 3: for (int i = 0; i < n; i++) { a[i] = (double)i; } break;
        }

        memcpy(b, a, n * sizeof(double));
        const double x = floor_qsort(a, n), y = floor_select(b, n);
        cases++;

        if (memcmp(&x, &y, sizeof x)) { bad++; }

        fill_band(a, n, 0.2);
        memcpy(b, a, n * sizeof(double));
        const double u = lowhalf_qsort(a, n), v = lowhalf_select(b, n);
        cases++;

        if (memcmp(&u, &v, sizeof u)) { bad++; }

        fill_band(a, n, 0.2);
        memcpy(b, a, n * sizeof(double));
        const double m1 = median_qsort(a, n), m2 = median_select(b, n);
        cases++;

        if (memcmp(&m1, &m2, sizeof m1)) { bad++; }
      }
    }
  }

  printf("Identity, selection against qsort on this machine: %ld cases, %ld differ%s\n\n",
         cases, bad, bad ? "  <-- PLEASE REPORT" : "");
  return bad != 0;
}

/* ---- Host ------------------------------------------------------------- */

static void cat_first_line(const char *label, const char *path) {
  FILE *f = fopen(path, "r");
  char line[256];

  if (f && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    printf("%-10s %s\n", label, line);
  }

  if (f) { fclose(f); }
}

static void host(void) {
  cat_first_line("model:", "/proc/device-tree/model");
  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[256];

  while (f && fgets(line, sizeof line, f)) {
    if (!strncmp(line, "model name", 10) || !strncmp(line, "CPU part", 8)) {
      printf("%-10s %s", "cpu:", strchr(line, ':') + 2);
      break;
    }
  }

  if (f) { fclose(f); }

  cat_first_line("governor:", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  cat_first_line("max kHz:", "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
  printf("%-10s %s", "compiler:", __VERSION__);
#ifdef __OPTIMIZE__
  printf(", optimised");
#else
  printf(", NOT optimised - use -O3");
#endif
#ifdef __aarch64__
  printf(", aarch64");
#elif defined(__arm__)
  printf(", 32-bit ARM");
#elif defined(__x86_64__)
  printf(", x86-64");
#endif
#ifdef WITH_FFTW
  printf(", FFTW %s\n", fftwf_version);
#else
  printf(", no FFTW (add -DWITH_FFTW ... -lfftw3f for the transforms)\n");
#endif
  printf("\n");
}

/* ---- The sections ------------------------------------------------------ */

static void sorts(void) {
  printf("Order statistics, per block (both arms where the engine does both).\n");
  printf("  %-30s %5s  %9s %9s  %6s   %s\n", "", "n", "qsort us", "select us", "ratio",
         "% of a core, qsort -> select");
  struct { const char *what; statfn q, s; int n; double occ; int arms; } t[] = {
    { "noise floor, quiet band",       floor_qsort,   floor_select,   1024, 0.0,  2 },
    { "noise floor, quiet band",       floor_qsort,   floor_select,    850, 0.0,  2 },
    { "noise floor, busy band (41 %)", floor_qsort,   floor_select,   1024, 0.41, 2 },
    { "Digital median, SSB filter",    median_qsort,  median_select,   220, 0.3,  1 },
    { "Digital median, wide window",   median_qsort,  median_select,  1024, 0.3,  1 },
    { "Digital median, maximum",       median_qsort,  median_select,  4096, 0.3,  1 },
    { "CW off-tone floor, 100 Hz",     lowhalf_qsort, lowhalf_select,    8, 0.2,  2 },
    { "CW off-tone floor, 500 Hz",     lowhalf_qsort, lowhalf_select,   40, 0.2,  2 },
  };
  printf("hot (back to back):\n");

  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
    prepare(t[i].n, t[i].occ);
    const int reps = t[i].n > 2000 ? 2000 : 20000;
    row(t[i].what, t[i].n, time_hot(t[i].q, t[i].n, reps), time_hot(t[i].s, t[i].n, reps), t[i].arms);
  }

  printf("paced (one call per 85 ms, as the radio; median of 60):\n");

  for (unsigned i = 0; i < 4; i += 3) {
    prepare(t[i].n, t[i].occ);
    row(t[i].what, t[i].n, time_paced(t[i].q, t[i].n, 60), time_paced(t[i].s, t[i].n, 60), t[i].arms);
  }

  printf("\n");
}

//
// The floor's gather: up to 1024 strided bins of each arm's spectrum,
// |X|^2 in double, as div_noise_floor_update() does.
//
static void gather(void) {
  printf("Noise-floor gather, per block (2 arms x 1024 strided bins of |X|^2):\n");
  const int sizes[] = { 4096, 16384, 65536 };

  for (unsigned s = 0; s < 3; s++) {
    const int nfft = sizes[s];
    float *x0 = malloc(sizeof(float) * 2 * nfft), *x1 = malloc(sizeof(float) * 2 * nfft);
    double *o0 = malloc(sizeof(double) * 1024), *o1 = malloc(sizeof(double) * 1024);

    for (int i = 0; i < 2 * nfft; i++) { x0[i] = (float)urand(); x1[i] = (float)urand(); }

    //
    // Span +-20 kHz at 12 Hz bins is about 3400 bins, so a stride of 4;
    // at 4096 the whole usable span is sampled at a stride of 3.
    //
    const int stride = (nfft == 4096) ? 3 : 4;
    volatile double sink = 0.0;
    double hot = 1e9;

    for (int run = 0; run < 5; run++) {
      const double t0 = now();

      for (int r = 0; r < 20000; r++) {
        for (int j = 0, k = -1700; j < 1024; j++, k += stride) {
          int idx = k % nfft;

          if (idx < 0) { idx += nfft; }

          o0[j] = (double)x0[2 * idx] * x0[2 * idx] + (double)x0[2 * idx + 1] * x0[2 * idx + 1];
          o1[j] = (double)x1[2 * idx] * x1[2 * idx] + (double)x1[2 * idx + 1] * x1[2 * idx + 1];
        }

        sink += o0[r & 1023] + o1[r & 1023];
      }

      const double t = (now() - t0) / 20000;

      if (t < hot) { hot = t; }
    }

    (void)sink;
    printf("  nfft %6d: %7.2f us hot\n", nfft, 1e6 * hot);
    free(x0); free(x1); free(o0); free(o1);
  }

  printf("\n");
}

//
// The shape of the Window reference's per-bin work: smoothed auto- and
// cross-spectra for every bin in the analysis window. The engine does
// more around it (notches, the gate), so this is a floor, not the cost.
//
static void accumulate(void) {
  printf("Per-bin cross-spectral accumulation, per block (approximate):\n");
  const int bins[] = { 256, 1400, 16384 };
  const char *what[] = { "SSB filter, 3 kHz", "AM / wide window, 16 kHz", "whole span at 192 kHz" };

  for (unsigned b = 0; b < 3; b++) {
    const int n = bins[b];
    float *x0 = malloc(sizeof(float) * 2 * n), *x1 = malloc(sizeof(float) * 2 * n);
    double *xx = calloc(n, sizeof(double)), *yy = calloc(n, sizeof(double));
    double *re = calloc(n, sizeof(double)), *im = calloc(n, sizeof(double));

    for (int i = 0; i < 2 * n; i++) { x0[i] = (float)urand(); x1[i] = (float)urand(); }

    const double a = 0.04;
    const int reps = 200000000 / (n * 8) + 1;
    double hot = 1e9;

    for (int run = 0; run < 5; run++) {
      const double t0 = now();

      for (int r = 0; r < reps; r++) {
        for (int k = 0; k < n; k++) {
          const double ar = x0[2 * k], ai = x0[2 * k + 1], br = x1[2 * k], bi = x1[2 * k + 1];
          xx[k] += a * (ar * ar + ai * ai - xx[k]);
          yy[k] += a * (br * br + bi * bi - yy[k]);
          re[k] += a * (ar * br + ai * bi - re[k]);
          im[k] += a * (ai * br - ar * bi - im[k]);
        }
      }

      const double t = (now() - t0) / reps;

      if (t < hot) { hot = t; }
    }

    printf("  %-26s %6d bins: %8.2f us hot  (%5.2f%% of a core)\n", what[b], n, 1e6 * hot,
           100.0 * hot * BLOCKS_PER_S);
    free(x0); free(x1); free(xx); free(yy); free(re); free(im);
  }

  printf("\n");
}

#ifdef WITH_FFTW
static void transforms(void) {
  printf("FFT, one complex single-precision forward transform (the engine does two a block):\n");
  printf("  %6s  %-28s %10s %10s  %s\n", "nfft", "sample rate at 12 Hz bins", "hot us", "paced us",
         "2 per block, % of a core (paced)");
  const int sizes[] = { 4096, 8192, 16384, 32768, 65536 };
  const char *rate[] = { "48 kHz", "96 kHz", "192 kHz", "384 kHz", "768 and 1536 kHz (cap)" };

  for (unsigned s = 0; s < 5; s++) {
    const int n = sizes[s];
    fftwf_complex *in = fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_complex *out = fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_plan p = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

    for (int i = 0; i < n; i++) { in[i][0] = (float)urand(); in[i][1] = (float)urand(); }

    const int reps = 20000000 / n + 1;
    double hot = 1e9;

    for (int run = 0; run < 5; run++) {
      const double t0 = now();

      for (int r = 0; r < reps; r++) { fftwf_execute(p); }

      const double t = (now() - t0) / reps;

      if (t < hot) { hot = t; }
    }

    static double t[40];

    for (int c = 0; c < 40; c++) {
      pace();
      const double t0 = now();
      fftwf_execute(p);
      t[c] = now() - t0;
    }

    qsort(t, 40, sizeof(double), dcmp);
    const double paced = t[20];
    //
    // At 1536 kHz the transform is capped at 65536, so bins are 23.4 Hz
    // and blocks come twice as often.
    //
    const double bps = (n == 65536) ? 23.4 : BLOCKS_PER_S;
    printf("  %6d  %-28s %10.1f %10.1f  %5.2f%%%s\n", n, rate[s], 1e6 * hot, 1e6 * paced,
           100.0 * 2.0 * paced * bps, n == 65536 ? " (at 1536 kHz, 23.4 blocks/s)" : "");
    fftwf_destroy_plan(p);
    fftwf_free(in);
    fftwf_free(out);
  }

  printf("\n");
}
#endif

#ifdef WITH_FFTW
/* ---- 6. Planner: ESTIMATE, MEASURE, PATIENT, and wisdom ---------------- */

static double exec_hot(fftwf_plan p, int n) {
  const int reps = 20000000 / n + 1;
  double best = 1e9;

  for (int run = 0; run < 5; run++) {
    const double t0 = now();

    for (int r = 0; r < reps; r++) { fftwf_execute(p); }

    const double t = (now() - t0) / reps;

    if (t < best) { best = t; }
  }

  return best;
}

static double exec_paced(fftwf_plan p) {
  static double t[40];

  for (int c = 0; c < 40; c++) {
    pace();
    const double t0 = now();
    fftwf_execute(p);
    t[c] = now() - t0;
  }

  qsort(t, 40, sizeof(double), dcmp);
  return t[20];
}

static void planners(void) {
  printf("FFT planner, out of place as the engine (two transforms a block; %% of a core paced):\n");
  printf("  %6s  %-8s %10s %9s %9s %7s  %s\n", "nfft", "flags", "plan ms", "hot us", "paced us",
         "% core", "replan from wisdom ms");
  const int sizes[] = { 4096, 8192, 16384, 32768, 65536 };
  const unsigned flags[] = { FFTW_ESTIMATE, FFTW_MEASURE, FFTW_PATIENT };
  const char *fname[] = { "ESTIMATE", "MEASURE", "PATIENT" };
  //
  // PATIENT can run for minutes at the largest sizes; a minute each is
  // plenty to see where it lands.
  //
  fftwf_set_timelimit(60.0);

  for (unsigned s = 0; s < 5; s++) {
    const int n = sizes[s];
    fftwf_complex *in = fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_complex *out = fftwf_malloc(sizeof(fftwf_complex) * n);
    const double bps = (n == 65536) ? 23.4 : BLOCKS_PER_S;

    for (unsigned f = 0; f < 3; f++) {
      fftwf_forget_wisdom();
      double t0 = now();
      fftwf_plan p = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, flags[f]);
      const double plan = now() - t0;

      // planning with MEASURE/PATIENT scribbles on the arrays
      for (int i = 0; i < n; i++) { in[i][0] = (float)urand(); in[i][1] = (float)urand(); }

      const double h = exec_hot(p, n);
      const double pc = exec_paced(p);
      double replan = -1.0;

      if (flags[f] != FFTW_ESTIMATE) {
        char *w = fftwf_export_wisdom_to_string();
        fftwf_forget_wisdom();
        t0 = now();
        fftwf_import_wisdom_from_string(w);
        fftwf_plan q = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, flags[f] | FFTW_WISDOM_ONLY);
        replan = now() - t0;

        if (q == NULL) { replan = -2.0; }
        else { fftwf_destroy_plan(q); }

        free(w);
      }

      printf("  %6d  %-8s %10.1f %9.1f %9.1f %6.2f%%  ", n, fname[f], 1e3 * plan, 1e6 * h, 1e6 * pc,
             100.0 * 2.0 * pc * bps);

      if (replan == -1.0) { printf("-\n"); }
      else if (replan == -2.0) { printf("wisdom did not replan\n"); }
      else { printf("%.2f\n", 1e3 * replan); }

      printf("TSV\tfft\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.4f\t%.4f\n", n, fname[f], 1e3 * plan, 1e6 * h,
             1e6 * pc, 100.0 * 2.0 * pc * bps, replan > 0 ? 1e3 * replan : replan);
      fflush(stdout);
      fftwf_destroy_plan(p);
    }

    fftwf_free(in);
    fftwf_free(out);
  }

  fftwf_forget_wisdom();
  printf("\n");
}
#endif

/* ---- 7. Decimators ----------------------------------------------------- */

//
// The state of one decimator: as in rade_correlator.c, one delay line per
// arm, interleaved complex.
//
#define DEC_TAPS_PER_PHASE 16

typedef struct {
  int decim, ntaps, dpos, phase;
  float *taps, *d0, *d1, *out0, *out1;
  double c, s;
} DEC;

static void dec_init(DEC *d, int decim, int doubled) {
  memset(d, 0, sizeof(*d));
  d->decim = decim;
  d->ntaps = DEC_TAPS_PER_PHASE * decim;
  d->taps = malloc(sizeof(float) * d->ntaps);
  const int len = 2 * d->ntaps * (doubled ? 2 : 1);
  d->d0 = calloc(len, sizeof(float));
  d->d1 = calloc(len, sizeof(float));
  d->out0 = malloc(sizeof(float) * 2 * 65536);
  d->out1 = malloc(sizeof(float) * 2 * 65536);

  //
  // The same taps for both forms, from a fixed seed, so their outputs
  // can be compared. The lean form runs oldest first, so it holds them
  // reversed.
  //
  unsigned seed = 777;

  for (int k = 0; k < d->ntaps; k++) {
    seed = seed * 1103515245u + 12345u;
    const float t = (float)((seed >> 8) & 0xffff) / 65536.0f - 0.5f;
    d->taps[doubled ? d->ntaps - 1 - k : k] = t;
  }

  d->c = 1.0;
  d->s = 0.0;
}

static void dec_free(DEC *d) {
  free(d->taps); free(d->d0); free(d->d1); free(d->out0); free(d->out1);
}

//
// rade_corr_process()'s front end as it is: the shift rotator, a store
// into a circular delay line, and on every decim-th sample a FIR over all
// ntaps entries with double accumulators and a wrap test per tap.
//
static int dec_as_rade(DEC *d, const float *a0, const float *a1, int n) {
  const double cd = cos(0.01), sd = sin(0.01);
  double c = d->c, s = d->s;
  int dpos = d->dpos, phase = d->phase, nout = 0;
  const int ntaps = d->ntaps;
  float *dl0 = d->d0, *dl1 = d->d1;

  for (int i = 0; i < n; i++) {
    double i0 = a0[2 * i], q0 = a0[2 * i + 1];
    double i1 = a1[2 * i], q1 = a1[2 * i + 1];
    dl0[2 * dpos    ] = (float)(i0 * c - q0 * s);
    dl0[2 * dpos + 1] = (float)(i0 * s + q0 * c);
    dl1[2 * dpos    ] = (float)(i1 * c - q1 * s);
    dl1[2 * dpos + 1] = (float)(i1 * s + q1 * c);
    double ct = c;
    c = ct * cd - s * sd;
    s = ct * sd + s * cd;
    dpos++;

    if (dpos >= ntaps) { dpos = 0; }

    if (++phase < d->decim) { continue; }

    phase = 0;
    double a0r = 0.0, a0i = 0.0, a1r = 0.0, a1i = 0.0;
    int idx = dpos - 1;

    for (int k = 0; k < ntaps; k++) {
      if (idx < 0) { idx += ntaps; }

      double tk = d->taps[k];
      a0r += tk * dl0[2 * idx    ];
      a0i += tk * dl0[2 * idx + 1];
      a1r += tk * dl1[2 * idx    ];
      a1i += tk * dl1[2 * idx + 1];
      idx--;
    }

    d->out0[2 * nout] = (float)a0r; d->out0[2 * nout + 1] = (float)a0i;
    d->out1[2 * nout] = (float)a1r; d->out1[2 * nout + 1] = (float)a1i;
    nout++;
  }

  d->c = c; d->s = s; d->dpos = dpos; d->phase = phase;
  return nout;
}

//
// The same filter, leaner: the delay line split into four planar float
// arrays (I and Q of each arm), each sample written twice, ntaps apart,
// so the newest ntaps always lie contiguous and the FIR is a plain dot
// product with no wrap test. The sum runs in eight independent lanes,
// added at the end, which the compiler vectorises without -ffast-math
// (one running sum is a dependency chain it may not reorder). ntaps is a
// multiple of 16, so the lanes divide it. The taps are held reversed,
// because this runs oldest first. Same rotator.
//
#define LANES 8

static inline float dot8(const float *restrict t, const float *restrict x, int n) {
  float acc[LANES] = { 0 };

  for (int k = 0; k < n; k += LANES) {
    for (int j = 0; j < LANES; j++) { acc[j] += t[k + j] * x[k + j]; }
  }

  float sum = 0.0f;

  for (int j = 0; j < LANES; j++) { sum += acc[j]; }

  return sum;
}

static int dec_lean(DEC *d, const float *a0, const float *a1, int n) {
  const double cd = cos(0.01), sd = sin(0.01);
  double c = d->c, s = d->s;
  int dpos = d->dpos, phase = d->phase, nout = 0;
  const int ntaps = d->ntaps;
  // d0 holds I0 then Q0, d1 holds I1 then Q1, each 2 * ntaps long
  float *i0l = d->d0, *q0l = d->d0 + 2 * ntaps;
  float *i1l = d->d1, *q1l = d->d1 + 2 * ntaps;

  for (int i = 0; i < n; i++) {
    double i0 = a0[2 * i], q0 = a0[2 * i + 1];
    double i1 = a1[2 * i], q1 = a1[2 * i + 1];
    i0l[dpos] = i0l[dpos + ntaps] = (float)(i0 * c - q0 * s);
    q0l[dpos] = q0l[dpos + ntaps] = (float)(i0 * s + q0 * c);
    i1l[dpos] = i1l[dpos + ntaps] = (float)(i1 * c - q1 * s);
    q1l[dpos] = q1l[dpos + ntaps] = (float)(i1 * s + q1 * c);
    double ct = c;
    c = ct * cd - s * sd;
    s = ct * sd + s * cd;
    dpos++;

    if (dpos >= ntaps) { dpos = 0; }

    if (++phase < d->decim) { continue; }

    phase = 0;
    // oldest first: the ntaps entries from dpos on
    d->out0[2 * nout]     = dot8(d->taps, i0l + dpos, ntaps);
    d->out0[2 * nout + 1] = dot8(d->taps, q0l + dpos, ntaps);
    d->out1[2 * nout]     = dot8(d->taps, i1l + dpos, ntaps);
    d->out1[2 * nout + 1] = dot8(d->taps, q1l + dpos, ntaps);
    nout++;
  }

  d->c = c; d->s = s; d->dpos = dpos; d->phase = phase;
  return nout;
}

typedef int (*decfn)(DEC *, const float *, const float *, int);

static void dec_time(decfn f, DEC *d, const float *a0, const float *a1, int n,
                     double *hot, double *paced) {
  volatile int sink = 0;
  *hot = 1e9;

  for (int run = 0; run < 5; run++) {
    const int reps = 4194304 / n;   // about 4 M samples a run
    const double t0 = now();

    for (int r = 0; r < reps; r++) { sink += f(d, a0, a1, n); }

    const double t = (now() - t0) / reps;

    if (t < *hot) { *hot = t; }
  }

  static double t[20];

  for (int c = 0; c < 20; c++) {
    pace();
    const double t0 = now();
    sink += f(d, a0, a1, n);
    t[c] = now() - t0;
  }

  qsort(t, 20, sizeof(double), dcmp);
  *paced = t[10];
  (void)sink;
}

static int lean_ok = 1;

static void decimators(void) {
  printf("Decimators, per analysis block (both arms; %% of a core paced, over the block period):\n");
  printf("  %-24s %6s %6s %6s %6s %10s %10s %7s\n", "filter", "rate", "nfft", "decim", "taps",
         "hot us", "paced us", "% core");
  const int rates[] = { 48000, 96000, 192000, 384000, 768000, 1536000 };
  float *a0 = malloc(sizeof(float) * 2 * 65536);
  float *a1 = malloc(sizeof(float) * 2 * 65536);

  for (int i = 0; i < 2 * 65536; i++) { a0[i] = (float)(urand() - 0.5); a1[i] = (float)(urand() - 0.5); }

  //
  // The lean form must compute the same filter: run both from rest over
  // one block at 192 kHz and compare. Float against double accumulation,
  // so not bit for bit.
  //
  {
    DEC x, y;
    dec_init(&x, 24, 0);
    dec_init(&y, 24, 1);
    const int nx = dec_as_rade(&x, a0, a1, 16384);
    const int ny = dec_lean(&y, a0, a1, 16384);
    double err = 0.0, mag = 0.0;

    for (int i = 0; i < 2 * nx; i++) {
      err = fmax(err, fabs(x.out0[i] - y.out0[i]));
      err = fmax(err, fabs(x.out1[i] - y.out1[i]));
      mag = fmax(mag, fabs(x.out0[i]));
    }

    printf("  lean against as-RADE, 192 kHz, %d outputs: max difference %.2e of a peak %.2e%s\n",
           nx, err, mag, (nx == ny && err < 1e-4 * mag) ? "" : "  ** MISMATCH **");
    lean_ok = (nx == ny && err < 1e-4 * mag);
    dec_free(&x);
    dec_free(&y);
  }

  for (int r = 0; r < 6; r++) {
    int n = 4096;

    while (n < 65536 && (double)rates[r] / n > 12.0) { n <<= 1; }

    const double period = (double)n / rates[r];
    struct { const char *name; decfn f; int to; int doubled; } v[] = {
      { "to 8 kHz, as RADE V1",  dec_as_rade, 8000,   0 },
      { "to 8 kHz, lean",        dec_lean,    8000,   1 },
      { "to 192 kHz, lean",      dec_lean,    192000, 1 },
    };

    for (unsigned k = 0; k < 3; k++) {
      if (rates[r] <= v[k].to) { continue; }

      DEC d;
      dec_init(&d, rates[r] / v[k].to, v[k].doubled);
      double h, pc;
      dec_time(v[k].f, &d, a0, a1, n, &h, &pc);
      printf("  %-24s %5dk %6d %6d %6d %10.1f %10.1f %6.2f%%\n", v[k].name, rates[r] / 1000, n,
             d.decim, d.ntaps, 1e6 * h, 1e6 * pc, 100.0 * pc / period);
      printf("TSV\tdec\t%s\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.4f\n", v[k].name, rates[r], n, d.decim,
             d.ntaps, 1e6 * h, 1e6 * pc, 100.0 * pc / period);
      fflush(stdout);
      dec_free(&d);
    }
  }

  free(a0);
  free(a1);
  printf("\n");
}

int main(void) {
  srand(12345);
  printf("pi_bench - diversity engine hot spots\n\n");
  host();
  const int bad = identity();
  sorts();
  gather();
  accumulate();
#ifdef WITH_FFTW
  transforms();
  planners();
#endif
  decimators();

  if (!lean_ok) { return 1; }

  printf("For the whole engine (every reference, RADE V1 included): the Pi 5 bundle,\n"
         "make -C test/diversity pi5-bench.tar.gz, runs this and bench_cpu together.\n");
  return bad;
}

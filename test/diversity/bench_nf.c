/*
 * The noise floor's order statistics: selection against a full sort.
 *
 * div_noise_floor_update() needs the mean of the 8th to 12th percentile
 * of up to DIV_NF_SAMPLES bin powers per arm per block. It used to qsort
 * the lot; it now fences the band with two selections and sorts only the
 * band (div_nf_band_mean()). This checks the two give the same answer,
 * bit for bit, and times them.
 *
 * The engine's own functions are used, not a copy: the Makefile cuts
 * div_nf_select() and div_nf_band_mean() out of diversity_auto.c into
 * nf_select.inc, so this cannot drift from what the radio runs.
 *
 *   make -C test/diversity bench_nf && ./bench_nf
 *
 * Exit status 1 if any case differs by so much as one bit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "nf_select.inc"

#define DIV_NF_PCT  10
#define DIV_NF_BAND 2
#define NMAX        1024

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;
  return (x > y) - (x < y);
}

//
// The code this replaces, as it stood on TEST at 6dc31b3f.
//
static double band_mean_qsort(double *a, int n, int ilo, int ihi) {
  qsort(a, (size_t)n, sizeof(double), cmp);
  double sum = 0.0;

  for (int i = ilo; i <= ihi; i++) { sum += a[i]; }

  return sum / (double)(ihi - ilo + 1);
}

static void band_of(int n, int *ilo, int *ihi) {
  *ilo = (n * (DIV_NF_PCT - DIV_NF_BAND)) / 100;
  *ihi = (n * (DIV_NF_PCT + DIV_NF_BAND)) / 100;

  if (*ihi >= n) { *ihi = n - 1; }

  if (*ihi < *ilo) { *ihi = *ilo; }
}

static double urand(void) { return (rand() + 1.0) / ((double)RAND_MAX + 2.0); }

//
// Bin power of complex Gaussian noise is exponential. A fraction occ of
// the bins carry a station 10 to 60 dB up, as on a busy band.
//
static void fill_band(double *a, int n, double occ) {
  for (int i = 0; i < n; i++) {
    a[i] = -log(urand());

    if (urand() < occ) { a[i] *= pow(10.0, 1.0 + 5.0 * urand()); }
  }
}

enum { K_NOISE, K_BUSY, K_TIES, K_ZEROS, K_EQUAL, K_UP, K_DOWN, K_PIPE, K_SLOPE, K_KINDS };
static const char *kname[K_KINDS] = {
  "noise", "busy band (41 % occupied)", "heavy ties (8 levels)", "half zeros",
  "all equal", "ascending", "descending", "organ pipe", "sloping spectrum"
};

static void fill(double *a, int n, int kind) {
  switch (kind) {
  case K_NOISE: fill_band(a, n, 0.0); break;

  case K_BUSY:  fill_band(a, n, 0.41); break;

  case K_TIES:  for (int i = 0; i < n; i++) { a[i] = (double)(rand() % 8); } break;

  case K_ZEROS: fill_band(a, n, 0.1); for (int i = 0; i < n; i += 2) { a[i] = 0.0; } break;

  case K_EQUAL: for (int i = 0; i < n; i++) { a[i] = 1.5e-7; } break;

  case K_UP:    for (int i = 0; i < n; i++) { a[i] = i; } break;

  case K_DOWN:  for (int i = 0; i < n; i++) { a[i] = n - i; } break;

  case K_PIPE:  for (int i = 0; i < n; i++) { a[i] = (i < n / 2) ? i : n - i; } break;

  case K_SLOPE: fill_band(a, n, 0.0);

    for (int i = 0; i < n; i++) { a[i] *= pow(10.0, 3.0 * i / n); }

    break;
  }
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

//
// Time per call, in microseconds, best of five runs: each call works on a
// fresh copy of one of 64 prepared arrays, and the copy's own cost is
// measured separately and taken off.
//
static double time_one(int sel, int n, int kind) {
  static double src[64][NMAX], w[NMAX];
  int ilo, ihi;
  band_of(n, &ilo, &ihi);

  for (int s = 0; s < 64; s++) { fill(src[s], n, kind); }

  const int reps = 20000;
  volatile double sink = 0.0;
  double best = 1e9;

  for (int run = 0; run < 5; run++) {
    double t0 = now();

    for (int r = 0; r < reps; r++) { memcpy(w, src[r & 63], n * sizeof(double)); sink += w[r % n]; }

    const double tcopy = now() - t0;
    t0 = now();

    for (int r = 0; r < reps; r++) {
      memcpy(w, src[r & 63], n * sizeof(double));
      sink += sel ? div_nf_band_mean(w, n, ilo, ihi) : band_mean_qsort(w, n, ilo, ihi);
    }

    const double t = (now() - t0 - tcopy) / reps;

    if (t < best) { best = t; }
  }

  (void)sink;
  return 1e6 * best;
}

int main(void) {
  static double a[NMAX], b[NMAX];
  long cases = 0, bad = 0;
  srand(12345);

  //
  // Identity: every kind, every length the engine can pass (it refuses
  // fewer than DIV_NF_MIN_BINS, 128), several draws each.
  //
  for (int kind = 0; kind < K_KINDS; kind++) {
    for (int n = 128; n <= NMAX; n++) {
      for (int d = 0; d < 4; d++) {
        int ilo, ihi;
        band_of(n, &ilo, &ihi);
        fill(a, n, kind);
        memcpy(b, a, n * sizeof(double));
        const double x = band_mean_qsort(a, n, ilo, ihi);
        const double y = div_nf_band_mean(b, n, ilo, ihi);
        cases++;

        if (memcmp(&x, &y, sizeof(double)) != 0) {
          if (bad < 10) {
            printf("DIFFER: %s n=%d draw %d: %.17g vs %.17g\n", kname[kind], n, d, x, y);
          }

          bad++;
        }
      }
    }
  }

  printf("Identity: %ld cases (9 kinds x n = 128..%d x 4 draws), %ld differ\n\n",
         cases, NMAX, bad);
  printf("Time per arm per block, microseconds (best of 5 x 20000 calls):\n\n");
  printf("%-28s %5s %9s %9s %7s\n", "data", "n", "qsort", "select", "ratio");
  const int ns[] = { 1024, 850, 300, 128 };

  for (int kind = 0; kind < K_KINDS; kind++) {
    for (unsigned k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
      if (kind != K_NOISE && kind != K_BUSY && ns[k] != 1024) { continue; }

      const double tq = time_one(0, ns[k], kind);
      const double ts = time_one(1, ns[k], kind);
      printf("%-28s %5d %9.2f %9.2f %6.1fx\n", kname[kind], ns[k], tq, ts, tq / ts);
    }
  }

  return bad ? 1 : 0;
}

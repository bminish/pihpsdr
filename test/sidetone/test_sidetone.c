//
// Stand-alone test of src/sidetone.c: how much do callback timing errors
// disturb the element lengths of the synthesised side tone?
//
//   gcc -O2 -I src -o /tmp/test_sidetone test/sidetone/test_sidetone.c src/sidetone.c -lm
//
// A 64-frame callback is simulated, with the callback entry time
// t0 = n*64/48000 + jitter. A keyer sending dits at 22 wpm queues events at their
// exact times. The envelope of the rendered output is analysed (50 % crossings) and
// compared to the requested times.
//
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "sidetone.h"

#define FS 48000.0
#define NQ 64

static unsigned int rng = 12345u;
static double urand(void) {   // uniform in [0,1)
  rng = rng * 1664525u + 1013904223u;
  return (rng >> 8) / 16777216.0;
}

typedef struct { double mean, sd, max; } STAT;

static void stat(const double *v, int n, STAT *s) {
  double sum = 0, sum2 = 0, mx = 0;
  for (int i = 0; i < n; i++) { sum += v[i]; sum2 += v[i] * v[i]; }
  double mean = sum / n;
  for (int i = 0; i < n; i++) { if (fabs(v[i] - mean) > mx) { mx = fabs(v[i] - mean); } }
  s->mean = mean;
  s->sd = sqrt(sum2 / n - mean * mean);
  s->max = mx;
}

static void run(const char *name, double jit, double spike_ms, int spike_every) {
  const int wpm = 22;
  const int dot = 57600 / wpm;                 // samples
  const int nelem = 400;
  const double total = 0.5 + nelem * (2.0 * dot / FS + 0.1) + 3; // seconds, generous
  const long ncb = (long)(total * FS / NQ);
  double *out = malloc(sizeof(double) * ncb * NQ);
  double *buf = malloc(sizeof(double) * NQ);
  // element start times: one dit every 109 ms (dot+gap) with 300 ms word gaps now and then
  double *start = malloc(sizeof(double) * nelem);
  double t = 0.5;
  for (int i = 0; i < nelem; i++) {
    start[i] = t;
    t += 2.0 * dot / FS + ((i % 5) == 4 ? 0.3 : 0.0) + urand() * 0.01;
  }
  int next = 0;
  sidetone_reset();
  for (long n = 0; n < ncb; n++) {
    double j = (urand() * 2.0 - 1.0) * jit;
    if (spike_every && (n % spike_every) == 0) { j += spike_ms * 1e-3; }
    double t0 = (double) n * NQ / FS + j;
    while (next < nelem && start[next] <= t0) {
      // the keyer queues down, up(dot), up(dot) all at once
      sidetone_queue(1, 0);
      sidetone_queue(0, dot);
      sidetone_queue(0, dot);
      next++;
    }
    sidetone_render(buf, NQ, 12000, 1.0);   // 12 kHz = quarter of fs: 0,1,0,-1
    for (int k = 0; k < NQ; k++) { out[n * NQ + k] = buf[k]; }
  }
  // find 50 % crossings of the envelope, 4-sample window
  long nsamp = ncb * NQ;
  double *rise = malloc(sizeof(double) * nelem), *fall = malloc(sizeof(double) * nelem);
  int nr = 0, nf = 0, state = 0;
  for (long i = 0; i + 4 < nsamp; i += 4) {
    double e = 0;
    for (int k = 0; k < 4; k++) { if (fabs(out[i + k]) > e) { e = fabs(out[i + k]); } }
    if (!state && e >= 0.5) { if (nr < nelem) { rise[nr++] = (double) i; } state = 1; }
    if (state && e < 0.5 && i > 0 && nr > nf) { if (nf < nelem) { fall[nf++] = (double) i; } state = 0; }
  }
  int ne = nr < nf ? nr : nf;
  double *len = malloc(sizeof(double) * ne), *lat = malloc(sizeof(double) * ne);
  for (int i = 0; i < ne; i++) {
    len[i] = (fall[i] - rise[i]) - dot;                   // length error, samples
    lat[i] = (rise[i] / FS - start[i]) * 1e3 - 2.5;       // msec, minus the 2.5 ms ramp half
  }
  STAT sl, st;
  stat(len, ne, &sl);
  stat(lat, ne, &st);
  printf("%-34s elements=%3d  length error: mean %+5.1f sd %5.1f max %5.1f samples (sd %4.0f us, max %4.0f us)\n",
         name, ne, sl.mean, sl.sd, sl.max, sl.sd / FS * 1e6, sl.max / FS * 1e6);
  double mn = lat[0], mx = lat[0];
  for (int i = 0; i < ne; i++) { if (lat[i] < mn) { mn = lat[i]; } if (lat[i] > mx) { mx = lat[i]; } }
  printf("%-34s              onset lag, event to start of output: mean %.2f ms, min %.2f, max %.2f\n", "",
         st.mean, mn, mx);
  free(out); free(buf); free(start); free(rise); free(fall); free(len); free(lat);
}

int main(void) {
  printf("22 wpm dits (%d samples = %.1f ms), callback of %d frames = %.2f ms\n\n", 57600 / 22, 57600 / 22 / 48.0, NQ, NQ / 48.0);
  run("no jitter", 0.0, 0, 0);
  run("jitter +-0.1 ms", 0.0001, 0, 0);
  run("jitter +-0.3 ms", 0.0003, 0, 0);
  run("jitter +-1 ms", 0.001, 0, 0);
  run("jitter +-3 ms", 0.003, 0, 0);
  run("+-0.1 ms and a 5 ms spike /200 cb", 0.0001, 5.0, 200);
  return 0;
}

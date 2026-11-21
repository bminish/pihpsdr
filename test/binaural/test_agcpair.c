/*
 * The binaural paired AGC (wdsp/wcpAGCpair.c) against the WDSP it is built into.
 *
 * Two AGCs as RXA builds them, each run by its own thread through xwcpagc(), as
 * the two ear channels are; arm 2 is arm 1 scaled by 0.25 (a loud second, then
 * -40 dB). Linked with SetRXAAGCLink(), there is one gain, so arm 2's output is
 * exactly 0.25 times arm 1's on every sample, and the two threads meet without
 * the 100 ms rendezvous timeout (which would cost ~40 s over 400 blocks).
 * Unlinked, each AGC levels its own arm and the ratio moves well away from 0.25.
 *
 * Run it after any WDSP update: the pair copies xwcpagc()'s arithmetic and rests
 * on WDSP's platform layer (semaphores, interlocked operations, csDSP).
 *
 *   make -C test/binaural run      (needs wdsp/libwdsp.a and libspecbleach built)
 */
#include "comm.h"
#include <pthread.h>
#include <time.h>
extern void SetRXAAGCLink (int channel_a, int channel_b, int on);
#define N 1024
#define BLOCKS 400
static double in[2][2 * N], out[2][2 * N];
static double rec[2][BLOCKS][2 * N];
static WCPAGC agc[2];
static pthread_barrier_t bar;
static double sig(long n) {                      // 1 kHz tone at 48 kHz, loud for 1 s, then -40 dB
  double amp = (n < 48000) ? 0.5 : 0.005;
  return amp * sin(2 * M_PI * 1000.0 * n / 48000.0);
}
static void *run(void *arg) {
  int k = (int)(long)arg;
  double scale = k ? 0.25 : 1.0;
  for (int b = 0; b < BLOCKS; b++) {
    pthread_barrier_wait(&bar);                   // both blocks offered at about the same time, as the ears are
    for (int i = 0; i < N; i++) { long n = (long)b * N + i; in[k][2*i] = scale * sig(n); in[k][2*i+1] = 0.0; }
    xwcpagc(agc[k]);
    memcpy(rec[k][b], out[k], sizeof out[k]);
  }
  return NULL;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void make(void) {
  for (int k = 0; k < 2; k++) {
    agc[k] = create_wcpagc(1, 3, 1, in[k], out[k], N, 48000, 0.001, 0.250, 4, 10000.0, 1.5, 1000.0, 1.0, 1.0,
                           0.250, 0.005, 5.0, 1, 0.500, 0.250, 0.250, 0.100);
    rxa[k].agc.p = agc[k];
    ch[k].exchange = 1;                           // both channels "running", as under the split
    ch[k].iob.pd = calloc(1, 4096);               // exec_bypass read as 0
    InitializeCriticalSection(&ch[k].csDSP);
  }
}
static int pass(int linked, double *maxdev, double *ratio_end, double *secs) {
  make();
  if (linked) SetRXAAGCLink(0, 1, 1);
  pthread_barrier_init(&bar, NULL, 2);
  pthread_t t[2];
  double t0 = now();
  for (long k = 0; k < 2; k++) pthread_create(&t[k], NULL, run, (void *)k);
  for (int k = 0; k < 2; k++) pthread_join(t[k], NULL);
  *secs = now() - t0;
  if (linked) SetRXAAGCLink(0, 1, 0);
  *maxdev = 0.0;
  double p0 = 0, p1 = 0;
  for (int b = 0; b < BLOCKS; b++)
    for (int i = 0; i < 2 * N; i++) {
      double d = fabs(rec[1][b][i] - 0.25 * rec[0][b][i]);
      if (d > *maxdev) *maxdev = d;
      if (b >= BLOCKS - 20) { p0 += rec[0][b][i] * rec[0][b][i]; p1 += rec[1][b][i] * rec[1][b][i]; }
    }
  *ratio_end = sqrt(p1 / p0);
  for (int k = 0; k < 2; k++) { destroy_wcpagc(agc[k]); free(ch[k].iob.pd); }
  return 0;
}
int main(void) {
  double dev, r, s;
  int fails = 0;
  pass(1, &dev, &r, &s);
  printf("linked:   max |out1 - 0.25*out0| = %.3g, out1/out0 at the end = %.4f, %d blocks in %.2f s\n", dev, r, BLOCKS, s);
  if (dev > 1e-12) { printf("  FAIL linked gains differ\n"); fails++; }
  if (s > BLOCKS * 0.1 * 0.25) { printf("  FAIL rendezvous timing out (%.2f s)\n", s); fails++; }
  pass(0, &dev, &r, &s);
  printf("unlinked: max |out1 - 0.25*out0| = %.3g, out1/out0 at the end = %.4f\n", dev, r);
  if (fabs(r - 0.25) < 0.05) { printf("  FAIL unlinked AGCs did not level independently\n"); fails++; }
  printf(fails ? "FAILED %d\n" : "all passed\n", fails);
  return fails != 0;
}

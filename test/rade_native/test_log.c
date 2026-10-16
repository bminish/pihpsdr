/* Generates parameter logs (src/rade_native_log.c) from a scripted scene, in the current directory (rade-logs/): test_log.py checks them.
 *   run 1: V2 over of 6 s in sync (25 calls a second), end-of-over frame, a call with a negative SNR, then 3 s without a lock
 *          (one row a second, as the engine writes them), then a keyed stretch of 2 s
 *   run 2: V1 over with a call that came with no lock (snr=none)
 * and the cost of a producer call. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rade_native_log.h"

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static double worst = 0, sum = 0; static long calls = 0;

static void rx(int sync, int eoo, float snr, float foff, int v2) {
  rade_log_rx_t r = { .rx_pos = 0, .sync = sync, .eoo = eoo, .nout = 144, .snr_db = snr, .foff_hz = foff,
                      .delta_hat = v2 ? 1.5f : 0, .est_foff = v2 ? foff : 0, .gain = v2 ? 3.0f : 0, .snr_est = v2 ? snr : 0, .in_fill = 100, .out_fill = 200 };
  double t = now_ns();
  rade_log_rx(&r);
  t = now_ns() - t; if (t > worst) worst = t; sum += t; calls++;
}
static void tx(int keyed_state_end, unsigned long under) {
  rade_log_tx_t t = { .tx = 1, .trim_db = 1.5, .agc_db = 3.25, .drive_db = -2, .loud_mom = -21, .loud_avg = NAN, .peak_dbfs = -3,
                      .limiting = 0, .underruns = under, .out_fill = 5000, .pcm_fill = 10, .end_state = keyed_state_end };
  double s = now_ns();
  rade_log_tx(&t);
  s = now_ns() - s; if (s > worst) worst = s; sum += s; calls++;
}

int main(void) {
  rade_log_ctx_t c = { .dial_hz = 28645000, .wave = 2, .invert = 0 };
  snprintf(c.mode, sizeof c.mode, "DIGU");
  rade_log_context(&c);
  if (rade_log_start("waveform: V2\ndial_hz: 28645000") != 0) { puts("start failed"); return 1; }
  rade_log_event("wave 2"); rade_log_event("dial 28645000"); rade_log_event("mode DIGU");
  for (int i = 0; i < 150; i++) { rx(1, 0, 10.0f + (i % 7) * 0.5f - (i == 60 ? 8.0f : 0.0f), 11.2f, 1); usleep(40000); }   // 6 s in sync, a fade at i = 60
  rx(1, 1, 9.0f, 11.2f, 1);                                  // the end-of-over frame
  rade_log_event("call EI6IZ snr=-3.5");
  for (int i = 0; i < 4; i++) { rx(0, 0, 0.0f, 0.0f, 1); sleep(1); }     // no lock: one row a second
  for (int i = 0; i < 20; i++) { tx(i >= 17, i >= 10 ? 2 : 0); usleep(100000); }   // 2 s keyed, 3 rows in the end-of-over hold, 2 underruns
  rade_log_event("drive -2.0");
  sleep(3);
  rade_log_stop();
  char first[200]; snprintf(first, sizeof first, "%s", rade_log_path());
  sleep(1);                                                    // the directory name has a one-second resolution
  c.wave = 1; snprintf(c.mode, sizeof c.mode, "DIGL"); c.invert = 1; c.dial_hz = 7053000;
  rade_log_context(&c);
  if (rade_log_start("waveform: V1") != 0) { puts("second start failed"); return 1; }
  for (int i = 0; i < 40; i++) { rx(1, 0, 5.0f, -2.0f, 0); usleep(120000); }
  rx(1, 1, 5.0f, -2.0f, 0);
  rade_log_event("call G4ABC snr=none");
  sleep(4);
  rade_log_stop();
  printf("first=%s\nsecond=%s\nproducer: mean %.0f ns, worst %.0f ns over %ld calls\n", first, rade_log_path(), sum / calls, worst, calls);
  return 0;
}

/* The parameter log: rows from two threads at the rate the engine makes them, a new file each time, column
 * count right for every row kind, nothing dropped, and how long a producer call takes. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rade_native_log.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static double worst_ns = 0, sum_ns = 0; static long calls = 0;
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

static void *rx_thread(void *a) {      // V2 rate: a call every 40 ms; run 100x faster
  for (long i = 0; i < 1500; i++) {
    rade_log_rx_t r = { .rx_pos = i * 320, .sync = 1, .nout = 144, .snr_db = 12.5f, .foff_hz = -3.2f,
                        .delta_hat = 1, .est_foff = 2, .gain = 3, .snr_est = 4, .in_fill = 100, .out_fill = 200 };
    double t = now_ns();
    rade_log_rx(&r);
    t = now_ns() - t; if (t > worst_ns) { worst_ns = t; } sum_ns += t; calls++;
    usleep(400);
  }
  return NULL;
}
static void *tx_thread(void *a) {
  for (long i = 0; i < 300; i++) {
    rade_log_tx_t t = { .tx = 1, .trim_db = 1.5, .agc_db = 3.25, .drive_db = -2, .loud_mom = -21, .loud_avg = (i % 10) ? NAN : -20.1,
                        .peak_dbfs = -3, .limiting = 0, .out_fill = 5000, .pcm_fill = 10 };
    double s = now_ns();
    rade_log_tx(&t);
    s = now_ns() - s; if (s > worst_ns) { worst_ns = s; } sum_ns += s; calls++;
    usleep(2000);
  }
  return NULL;
}

int main(void) {
  char first[256];
  for (int run = 0; run < 2; run++) {
    CHECK(rade_log_start("waveform: V2\ndial_hz: 14250000") == 0);
    CHECK(rade_log_active());
    snprintf(first, sizeof first, "%s", rade_log_path());
    rade_log_event("wave=2 dial=14250000 mode=DIGU invert=0 tx=0");
    pthread_t a, b;
    pthread_create(&a, NULL, rx_thread, NULL); pthread_create(&b, NULL, tx_thread, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    rade_log_event("has, a \"comma\"");
    rade_log_stop();
    CHECK(!rade_log_active());
    FILE *f = fopen(first, "r");
    CHECK(f != NULL);
    char line[1024]; int nrx = 0, ntx = 0, nev = 0, hdr = 0, cm = 0, bad = 0, drop = -1; double tprev = -1; int order = 1;
    while (f && fgets(line, sizeof line, f)) {
      if (line[0] == '#') { cm++; sscanf(line, "# dropped rows: %d", &drop); continue; }
      if (!hdr) { hdr = 1; continue; }
      int commas = 0; int inq = 0;
      for (char *p = line; *p; p++) { if (*p == '"') inq = !inq; else if (*p == ',' && !inq) commas++; }
      if (commas != 28) { bad++; }
      double t = atof(line); if (t < tprev) { order = 0; } tprev = t;
      if (strstr(line, ",rx,")) nrx++; else if (strstr(line, ",tx,")) ntx++; else if (strstr(line, ",ev,")) nev++;
    }
    if (f) { fclose(f); }
    printf("run %d: %s  rx %d tx %d ev %d, comment lines %d, dropped %d\n", run, first, nrx, ntx, nev, cm, drop);
    CHECK(nrx == 1500 && ntx == 300 && nev == 2);
    printf("  bad %d order %d drop %d comments %d\n", bad, order, drop, cm);
    CHECK(bad == 0 && order && drop == 0 && cm >= 4);
    sleep(1);                                   // file names have a one-second resolution
  }
  printf("producer call: mean %.0f ns, worst %.0f ns over %ld calls\n", sum_ns / calls, worst_ns, calls);
  CHECK(worst_ns < 1e6);                        // under a millisecond even when the scheduler is unkind
  printf(fails ? "FAILED\n" : "all passed\n");
  return fails != 0;
}

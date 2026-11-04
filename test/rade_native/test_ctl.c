/* Test server for the harness: the real socket server and WAV loader (src/rade_native_test.c) with a stand-in
 * for piHPSDR behind it, run on a GLib main loop as in the program. test_ctl.py drives it. */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rade_native_test.h"

static int armed, loaded, keyed, tx, aborts;
static double t_key, dur, t0, t1;
static double now(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static int exec_stub(const char *verb, const char *args, char *r, size_t n) {
  char v[200];
  double frac = 0.0; int inj = 0;
  if (keyed && tx) {
    double el = now() - t_key;
    if (t0 == 0.0) t0 = t_key;
    frac = el / dur; if (frac >= 1.0) { frac = 1.0; if (t1 == 0.0) t1 = t_key + dur; }
    inj = frac >= 1.0 ? 2 : 1;
  } else if (loaded) inj = 1;
  if (!strcmp(verb, "status") || !strcmp(verb, "poll")) {
    snprintf(r, n, "armed=%d tx=%d inj=%d inj_frac=%.3f t0=%.3f t1=%.3f aborts=%d tx_drive_pct=1 dial=28645000 can_key=1 refusal=none", armed, tx, inj, frac, t0, t1, aborts);
    return 0;
  }
  if (!strcmp(verb, "abort") || !strcmp(verb, "unkey")) {
    if (!strcmp(verb, "abort")) { loaded = 0; aborts++; }
    if (keyed) { keyed = 0; tx = 0; }
    snprintf(r, n, "released=1");
    return 0;
  }
  if (!strcmp(verb, "set") && rade_test_arg(args, "armed", v, sizeof v)) { armed = atoi(v); snprintf(r, n, "armed=%d", armed); return 0; }
  if (!strcmp(verb, "arm")) { armed = 1; snprintf(r, n, "armed=1"); return 0; }
  if (!armed) { snprintf(r, n, "not armed"); return -1; }
  if (!strcmp(verb, "set")) { snprintf(r, n, "set=1"); return 0; }
  if (!strcmp(verb, "capture")) { snprintf(r, n, "stem=/nonexistent/stem"); return 0; }
  if (!strcmp(verb, "log")) { snprintf(r, n, "file=/nonexistent/log.csv"); return 0; }
  if (!strcmp(verb, "inject")) {
    char path[200], err[100], g[20];
    float *b; long len;
    if (!rade_test_arg(args, "file", path, sizeof path)) { snprintf(r, n, "file= is needed"); return -1; }
    double gain = rade_test_arg(args, "gain_db", g, sizeof g) ? atof(g) : 0.0;
    if (rade_test_load_wav(path, gain, 90.0, &b, &len, err, sizeof err)) { snprintf(r, n, "%s", err); return -1; }
    double pk = 0; for (long i = 0; i < len; i++) if (fabs(b[i]) > pk) pk = fabs(b[i]);
    free(b);
    loaded = 1; dur = len / 48000.0; t0 = t1 = 0;
    snprintf(r, n, "duration_s=%.3f samples=%ld peak=%.4f", dur, len, pk);
    return 0;
  }
  if (!strcmp(verb, "key")) {
    if (!loaded) { snprintf(r, n, "nothing injected"); return -1; }
    keyed = 1; tx = 1; t_key = now();
    snprintf(r, n, "keyed=1");
    return 0;
  }
  snprintf(r, n, "unknown request %s", verb);
  return -1;
}

int main(void) {
  rade_test_start(exec_stub);
  g_main_loop_run(g_main_loop_new(NULL, FALSE));
  return 0;
}

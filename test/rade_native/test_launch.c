/*
 * Test of src/rade_launch.c, the detached start used for the Reporter button:
 * a program that fails at once is reported (with its output in the log), one
 * that runs on is not, and the real Reporter, started this way, is still
 * running after the parent has gone.
 *
 *   test_launch LOG REPORTER_SCRIPT
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rade_launch.h"

static GMainLoop *loop;
static int early_seen = -1, status_seen = -99, got = 0;

static void done(int early, int status, void *d) {
  early_seen = early; status_seen = status; got = 1;
  g_main_loop_quit(loop);
}

static int timeout_cb(gpointer d) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

static int run(char *const argv[], const char *log, int grace, int wait_s) {
  got = 0; early_seen = -1; status_seen = -99;
  char err[200];
  if (rade_launch(argv, log, grace, done, NULL, err, sizeof err) != 0) { printf("launch failed: %s\n", err); return -1; }
  loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add_seconds(wait_s, timeout_cb, NULL);
  g_main_loop_run(loop);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: test_launch LOG SCRIPT\n"); return 1; }
  int fails = 0;
  char *bad[] = {"python3", "-c", "import sys; print('boom: it did not start'); sys.exit(3)", NULL};
  run(bad, argv[1], 5, 8);
  printf("%s: fails at once -> reported early=%d status=%d\n", got && early_seen == 1 && status_seen == 3 ? "ok  " : "FAIL", early_seen, status_seen);
  fails += !(got && early_seen == 1 && status_seen == 3);
  char *late[] = {"python3", "-c", "import time; time.sleep(2)", NULL};
  run(late, argv[1], 1, 8);
  printf("%s: ends after the grace time -> early=%d\n", got && early_seen == 0 ? "ok  " : "FAIL", early_seen);
  fails += !(got && early_seen == 0);
  char *none[] = {"no-such-program-xyz", NULL};
  char err[200];
  int rc = rade_launch(none, argv[1], 5, NULL, NULL, err, sizeof err);
  printf("%s: missing program -> rc %d (%s)\n", rc == -1 ? "ok  " : "FAIL", rc, err);
  fails += rc != -1;
  FILE *f = fopen(argv[1], "r");
  char buf[4096] = "";
  if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
  printf("%s: the failed start's output is in the log\n", strstr(buf, "boom: it did not start") ? "ok  " : "FAIL");
  fails += !strstr(buf, "boom: it did not start");
  // the real thing, left running when this program ends
  char *rep[] = {"python3", argv[2], NULL};
  run(rep, argv[1], 5, 4);
  printf("%s: the Reporter is still running after 4 s (no early exit reported)\n", !got ? "ok  " : "FAIL");
  fails += got;
  printf(fails ? "FAILED %d\n" : "all passed\n", fails);
  return fails != 0;       // exits with the Reporter still going
}

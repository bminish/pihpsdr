/*
 * Host test of the break-in timing of src/iambic.c (phase 0 of docs/cw_breakin_plan.md).
 *
 * The real keyer_thread() is compiled (a copy of src/iambic.c, see run.sh) against stub headers in shim/.
 * What is simulated:
 *   - the GTK main loop: one thread that runs g_idle_add / g_timeout_add callbacks in order, one at a time.
 *     A timer can be delayed ("timer_extra_ms", a busy main loop);
 *   - radio_set_mox(): as in radio.c, "mox" changes only after rxtx() has returned, and rxtx() takes
 *     rxtx_ms[state] in the main loop thread (about 30 ms on the real machine);
 *   - tx_queue_cw_event(): only logged.
 * Time is real (CLOCK_MONOTONIC), 40 wpm and a hang of 300 ms to keep the scenarios short.
 *
 * These are CHARACTERISATION tests: they state what the code does today, bugs included, so that
 * every later phase changes the expectations on purpose. Lines marked BUG are the open findings of
 * docs/cw_timings.md section 3 and docs/cw_breakin_plan.md section 2.
 *
 * usage: test_breakin SCENARIO     (0 = list)      exit status 0: all expectations met
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include "mode.h"
#include "radio.h"
#include "iambic.h"

//
// Effective hang / set hang. The counter in keyer_thread() is decremented in CHECK and in EXITLOOP,
// and CHECK has no sleep: 2 counts per millisecond. Phase 1 makes this 1.0.
//
#ifndef HANG_RATIO
#define HANG_RATIO 0.5
#endif
// TX off after the hang time expired: the keyer waits for the 50 ms timer. Phase 2 may change this.
#define TX_OFF_TIMER_MS 50
#define SPEED 40
#define HANG  300
#define DOT_MS (1200.0 / SPEED)

// ---- settings and calls used by iambic.c -----------------------------------------------------
volatile int mox = 0;
int cw_keyer_mode = KEYER_MODE_B, cw_breakin = 1, cw_keyer_hang_time = HANG, cw_keyer_speed = SPEED;
int cw_keyer_weight = 50, cw_keys_reversed = 0, cw_keyer_spacing = 0, cw_keyer_internal = 0, CAT_cw_is_active = 0;
double cw_ramp_width = 9.0;
int vfo_get_tx_mode(void) { return modeCWL; }
int fatal_error(gpointer data) { return 0; }

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---- event log ---------------------------------------------------------------------------------
enum { EV_KEY, EV_MOXCALL, EV_MOXSET, EV_TIMER, EV_JOB };
typedef struct { long long t; int kind, a, b, c; } EV;
static EV evs[4096];
static int nev = 0;
static pthread_mutex_t evm = PTHREAD_MUTEX_INITIALIZER;
static long long t0;

static void logev(int kind, int a, int b, int c) {
  pthread_mutex_lock(&evm);
  if (nev < 4096) { evs[nev].t = now_ns(); evs[nev].kind = kind; evs[nev].a = a; evs[nev].b = b; evs[nev].c = c; nev++; }
  pthread_mutex_unlock(&evm);
}

static double ms(long long t) { return (t - t0) / 1.0e6; }

// nth event of a kind with a == a (n from 0); returns time in ms since t0, or -1
static double find(int kind, int a, int n) {
  double r = -1;
  pthread_mutex_lock(&evm);
  for (int i = 0; i < nev; i++) {
    if (evs[i].kind == kind && evs[i].a == a && n-- == 0) { r = ms(evs[i].t); break; }
  }
  pthread_mutex_unlock(&evm);
  return r;
}

static int count(int kind, int a) {
  int c = 0;
  pthread_mutex_lock(&evm);
  for (int i = 0; i < nev; i++) { if (evs[i].kind == kind && evs[i].a == a) { c++; } }
  pthread_mutex_unlock(&evm);
  return c;
}

// mox as seen when the nth key-down was queued
static int mox_at_key(int n) {
  int r = -1;
  pthread_mutex_lock(&evm);
  for (int i = 0; i < nev; i++) {
    if (evs[i].kind == EV_KEY && evs[i].a == 1 && n-- == 0) { r = evs[i].c; break; }
  }
  pthread_mutex_unlock(&evm);
  return r;
}

void tx_queue_cw_event(int down, int wait) { logev(EV_KEY, down, wait, mox); }

// ---- simulated GTK main loop -------------------------------------------------------------------
typedef struct job { long long due; GSourceFunc fn; gpointer data; struct job *next; } JOB;
static JOB *jobs = NULL;
static pthread_mutex_t jm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jc = PTHREAD_COND_INITIALIZER;
static int timer_extra_ms = 0;
static int rxtx_ms[2] = { 30, 30 };   // [0]: TX to RX, [1]: RX to TX

static guint add_job(long long due, GSourceFunc fn, gpointer data) {
  JOB *j = calloc(1, sizeof(JOB));
  j->due = due; j->fn = fn; j->data = data;
  pthread_mutex_lock(&jm);
  JOB **p = &jobs;
  while (*p && (*p)->due <= due) { p = &(*p)->next; }
  j->next = *p; *p = j;
  pthread_cond_signal(&jc);
  pthread_mutex_unlock(&jm);
  return 1;
}

guint g_idle_add(GSourceFunc fn, gpointer data) { return add_job(now_ns(), fn, data); }
guint g_timeout_add(guint t, GSourceFunc fn, gpointer data) {
  logev(EV_TIMER, (int) t, 0, mox);   // the keyer arms the TX-off timer: the hang time has expired
  return add_job(now_ns() + ((long long) t + timer_extra_ms) * 1000000LL, fn, data);
}

static void *gtk_thread(void *arg) {
  for (;;) {
    pthread_mutex_lock(&jm);
    while (jobs == NULL) { pthread_cond_wait(&jc, &jm); }
    JOB *j = jobs;
    long long n = now_ns();
    if (j->due > n) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      long long d = j->due - n;
      ts.tv_sec += d / 1000000000LL; ts.tv_nsec += d % 1000000000LL;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
      pthread_cond_timedwait(&jc, &jm, &ts);
      pthread_mutex_unlock(&jm);
      continue;
    }
    jobs = j->next;
    pthread_mutex_unlock(&jm);
    logev(EV_JOB, 0, 0, 0);
    j->fn(j->data);
    free(j);
  }
  return NULL;
}

static void msleep(double t) {
  struct timespec ts = { (time_t)(t / 1000), (long)((t - 1000 * (long)(t / 1000)) * 1e6) };
  nanosleep(&ts, NULL);
}

// as radio_set_mox() in radio.c: rxtx() first, then "mox" (the TX/RX transition blocks the main loop)
void radio_set_mox(int state) {
  logev(EV_MOXCALL, state, 0, mox);
  if (state != mox) { msleep(rxtx_ms[state]); }
  mox = state;
  logev(EV_MOXSET, state, 0, 0);
}

int ext_radio_set_mox(gpointer data) { radio_set_mox(GPOINTER_TO_INT(data)); return G_SOURCE_REMOVE; }

// ---- test script helpers -----------------------------------------------------------------------
static void at(double t) {
  double d = t - ms(now_ns());
  if (d > 0) { msleep(d); }
}

static void paddle(double t, double hold) {   // left paddle = dot
  at(t); keyer_event(1, 1);
  at(t + hold); keyer_event(1, 0);
}

static void start(void) {
  pthread_t th;
  pthread_create(&th, NULL, gtk_thread, NULL);
  keyer_update();           // starts the keyer thread (cw_keyer_internal == 0)
  msleep(50);               // let it reach sem_wait(), as it always has in real use
  t0 = now_ns();
  pthread_mutex_lock(&evm); nev = 0; pthread_mutex_unlock(&evm);
}

static void dump(void) {
  if (!getenv("TB_DUMP")) { return; }
  pthread_mutex_lock(&evm);
  for (int i = 0; i < nev; i++) {
    printf("    %8.1f ms  %s a=%d b=%d mox=%d\n", ms(evs[i].t), evs[i].kind == EV_KEY ? "key  " : evs[i].kind == EV_MOXCALL ? "moxcall" : evs[i].kind == EV_TIMER ? "timer-armed" : evs[i].kind == EV_JOB ? "job-run" : "moxset ",
           evs[i].a, evs[i].b, evs[i].c);
  }
  pthread_mutex_unlock(&evm);
}

static int failures = 0;
static void expect(int ok, const char *what, double got, const char *want) {
  printf("  %s  %-62s %9.1f   (%s)\n", ok ? "ok  " : "FAIL", what, got, want);
  if (!ok) { failures++; }
}
static int within(double v, double lo, double hi) { return v >= lo && v <= hi; }

// time of the key-down of the first element after a cold start
static double first_key(void) { return find(EV_KEY, 1, 0); }

// wait until the keyer has armed the TX-off timer (the hang time has expired), return that time in ms
static double wait_request(void) {
  double t;
  while ((t = find(EV_TIMER, TX_OFF_TIMER_MS, 0)) < 0) { msleep(0.2); }
  return t;
}

// ---- scenarios ---------------------------------------------------------------------------------
// 1 cold start of a short tap (shorter than the RX to TX transition): the element is not lost
static void s1(void) {
  printf("S1 cold start, tap of 15 ms (RX to TX takes %d ms)\n", rxtx_ms[1]);
  start(); paddle(0, 15); msleep(300);
  expect(first_key() > 0, "an element was sent (LC-065)", first_key(), "> 0");
  expect(within(first_key(), rxtx_ms[1], rxtx_ms[1] + 8), "key-down after the transition, ms after the press", first_key(), "30 to 38");
}

// 2 where the hang time expires and TX is dropped
static void s2(void) {
  printf("S2 hang expiry, one dit, hang %d ms\n", HANG);
  start(); paddle(0, 20); msleep(900);
  double tk = first_key();
  double off = find(EV_MOXCALL, 0, 0);
  double eff = off - TX_OFF_TIMER_MS - tk - 2 * DOT_MS;
  expect(off > 0, "TX off was requested (ms after the press)", off, "> 0");
  printf("  effective hang %.0f ms of %d set: ratio %.2f\n", eff, HANG, eff / HANG);
  expect(within(eff / HANG, HANG_RATIO - 0.05, HANG_RATIO + 0.10), "effective hang / set hang", eff / HANG,
         HANG_RATIO == 0.5 ? "BUG: 0.50, phase 1 makes it 1.00" : "1.00");
}

// 3 a tap after the expiry but before the 50 ms timer has fired: TX stays up
static void s3(void) {
  printf("S3 tap 20 ms after the hang time expired, before the timer fires\n");
  start(); paddle(0, 20);
  double treq = wait_request();
  paddle(treq + 20, 20); msleep(900);
  double k2 = find(EV_KEY, 1, 1);
  expect(k2 > 0, "second element sent", k2, "> 0");
  expect(within(k2 - (treq + 20), 0, 4), "key-down ms after the press (no transition)", k2 - (treq + 20), "0 to 4");
  expect(count(EV_MOXCALL, 0) == 1, "TX off requested once (after the second element)", count(EV_MOXCALL, 0), "1");
  expect(count(EV_MOXSET, 1) == 1, "no second RX to TX transition", count(EV_MOXSET, 1), "1");
}

// 4 a tap while TX is already going down: wait for RX, start again
static void s4(void) {
  printf("S4 tap 60 ms after the hang time expired, TX drop in progress\n");
  start(); paddle(0, 20);
  double treq = wait_request();
  paddle(treq + 60, 20); msleep(900);
  double k2 = find(EV_KEY, 1, 1);
  expect(k2 > 0, "second element sent", k2, "> 0");
  expect(within(k2 - (treq + 60), 25, 80), "key-down ms after the press (drop, then RX to TX)", k2 - (treq + 60), "25 to 80");
  expect(count(EV_MOXSET, 0) == 2 && count(EV_MOXSET, 1) == 2, "TX went down and up again, then down", count(EV_MOXSET, 1), "2 up, 2 down");
}

// 5 BUG: the TX-off timer is late (busy main loop); it fires while a later session is keying
static void s5(void) {
  timer_extra_ms = 350;
  printf("S5 TX-off timer delayed by %d ms (busy main loop)\n", timer_extra_ms);
  start(); paddle(0, 20);
  double treq = wait_request();
  paddle(treq + 300, 20);   // the keyer gave up waiting for the drop (250 polls, about 267 ms): a new session, TX still up
  paddle(treq + 450, 20);   // after the stale timer has fired (at +400) and the drop is done (+430)
  msleep(1500);
  double off = find(EV_MOXCALL, 0, 0);
  expect(within(off - treq, 380, 430), "BUG: stale timer drops TX in the second session, ms after the expiry", off - treq,
         "phase 2: never during a session");
  expect(mox_at_key(2) == 0, "BUG: a key-down queued while TX is off (mox at the key-down)", mox_at_key(2), "phase 2: 1");
}

// 6 BUG: slow TX to RX transition (400 ms): the restart sees mox still 1 and latches TX on
static void s6(void) {
  rxtx_ms[0] = 400;
  printf("S6 TX to RX transition of %d ms, tap during it\n", rxtx_ms[0]);
  start(); paddle(0, 20);
  double treq = wait_request();
  paddle(treq + 60, 20); msleep(2500);
  expect(mox == 1, "BUG: TX is still on 2.5 s after the last element (mox)", mox, "phase 2: 0");
  expect(count(EV_MOXCALL, 0) == 1, "BUG: only one TX off request over two sessions", count(EV_MOXCALL, 0), "phase 2: 2");
}

// 7 BUG: hang time 0 with break-in: TX is never dropped
static void s7(void) {
  cw_keyer_hang_time = 0;
  printf("S7 hang time 0, break-in on\n");
  start(); paddle(0, 20); msleep(1500);
  expect(mox == 1 && count(EV_MOXCALL, 0) == 0, "BUG: TX stays up (mox, TX-off requests 0)", mox, "phase 2: defined");
}

// 8 cold start, element after the transition
static void s8(void) {
  printf("S8 cold start, tap of 100 ms\n");
  start(); paddle(0, 100); msleep(400);
  expect(within(first_key(), rxtx_ms[1], rxtx_ms[1] + 8), "key-down ms after the press", first_key(), "30 to 38");
}

int main(int argc, char **argv) {
  void (*sc[])(void) = { NULL, s1, s2, s3, s4, s5, s6, s7, s8 };
  int n = argc > 1 ? atoi(argv[1]) : 0;
  if (n < 1 || n > 8) { printf("scenarios 1 to 8\n"); return 2; }
  sc[n]();
  dump();
  printf("%s\n", failures ? "FAILED" : "passed");
  return failures ? 1 : 0;
}

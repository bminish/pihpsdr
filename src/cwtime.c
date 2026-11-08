/*
 * Timing probe for the break-in sequence, see cwtime.h. Compiled to nothing without CW_TIMING_LOG.
 */
#ifdef CW_TIMING_LOG

#include <time.h>

#include "cwtime.h"
#include "message.h"

static volatile long long marks[CWT_COUNT];

void cwtime_mark(int id) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  // only the first occurrence in a session counts; the session starts with its first press
  if (marks[id] == 0 && (id == CWT_PRESS || marks[CWT_PRESS] != 0)) {
    marks[id] = (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
  }
}

static double d(int from, int to) {
  if (marks[from] == 0 || marks[to] == 0) { return -1.0; }
  return 1.0E-6 * (double)(marks[to] - marks[from]);
}

void cwtime_reset(void) {
  for (int i = 0; i < CWT_COUNT; i++) { marks[i] = 0; }
}

void cwtime_report(void) {
  t_print("cwtime: idle-queue %.1f, rxtx %.1f, rxtx-to-mox %.1f, mox-to-keyer %.1f, press-to-keydown %.1f | expiry-to-timer %.1f, "
          "timer-to-rxtx %.1f, rxtx %.1f, rxtx-to-mox %.1f, mox-to-keyer %.1f (ms, -1: not seen)\n",
          d(CWT_MOX_REQ, CWT_RXTX_START), d(CWT_RXTX_START, CWT_RXTX_END), d(CWT_RXTX_END, CWT_MOX_SET),
          d(CWT_MOX_SET, CWT_MOX_SEEN), d(CWT_PRESS, CWT_KEYDOWN),
          d(CWT_EXPIRY, CWT_OFF_FIRE), d(CWT_OFF_FIRE, CWT_OFF_RXTX_START), d(CWT_OFF_RXTX_START, CWT_OFF_RXTX_END),
          d(CWT_OFF_RXTX_END, CWT_OFF_MOX_SET), d(CWT_OFF_MOX_SET, CWT_OFF_SEEN));
  cwtime_reset();
}

#endif

/*
 * Timing probe for the break-in sequence (docs/cw_breakin_plan.md, phase 0).
 *
 * Off unless piHPSDR is built with "make CWTIME=1" (touch src/cwtime.c src/iambic.c src/radio.c first: make does not see
 * a changed define). Without it CWT() is empty and nothing changes.
 *
 * With it, CWT(id) stores the time of the first occurrence of an event in a break-in session, and one line
 * per session is printed after the TX-off wait of the keyer thread, when TX is already down. Nothing is
 * printed or locked in the timing path.
 */

#ifndef _CWTIME_H_
#define _CWTIME_H_

enum {
  CWT_PRESS,            // first paddle press of the session (keyer_event)
  CWT_MOX_REQ,          // keyer thread: just before it asks for MOX
  CWT_RXTX_START,       // rxtx(1) entered (GTK thread)
  CWT_RXTX_END,         // rxtx(1) returned
  CWT_MOX_SET,          // radio_set_mox(1) has set mox
  CWT_MOX_SEEN,         // keyer thread sees mox
  CWT_KEYDOWN,          // first key-down queued
  CWT_EXPIRY,           // hang time expired
  CWT_OFF_FIRE,         // the TX-off timer fired and calls radio_set_mox(0)
  CWT_OFF_RXTX_START,   // rxtx(0) entered
  CWT_OFF_RXTX_END,     // rxtx(0) returned
  CWT_OFF_MOX_SET,      // radio_set_mox(0) has set mox
  CWT_OFF_SEEN,         // keyer thread sees mox gone
  CWT_COUNT
};

#ifdef CW_TIMING_LOG
  void cwtime_mark(int id);
  void cwtime_report(void);
  void cwtime_reset(void);
  #define CWT(id) cwtime_mark(id)
  #define CWT_REPORT() cwtime_report()
  #define CWT_RESET() cwtime_reset()
#else
  #define CWT(id) do { } while (0)
  #define CWT_REPORT() do { } while (0)
  #define CWT_RESET() do { } while (0)
#endif

#endif

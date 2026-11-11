/* Copyright (C)
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

//
// Stand-alone test of the idle / start-of-TX behaviour of src/sidetone.c.
// Unlike test_sidetone.c this one has assertions: it exits non-zero on failure.
//
//   gcc -O2 -I src -o /tmp/test_sidetone_idle test/sidetone/test_sidetone_idle.c src/sidetone.c -lm
//   /tmp/test_sidetone_idle
//
// The audio callback calls sidetone_idle() while the radio is not transmitting and sidetone_render()
// while it is. The callbacks are simulated here with 128 frames each. The tests check that
//
//   T1  a key-down queued while not transmitting (an external keyer whose key-down arrives before TX is up)
//       is kept and starts the tone in the first block rendered after TX is up,
//   T2  an event with a large "wait" (key-down after a pause shorter than 0.5 sec, see
//       CW_KEYER_KEYDOWN in actions.c) is executed at once when TX starts, as in the TX engine,
//   T3  the spacing of the following events is still exactly the requested one,
//   T4  events that were queued but never played (TX never came) are discarded after 200 msec,
//       so they do not start a tone much later,
//   T5  the tone is released by sidetone_reset()/sidetone_idle() and nothing is left over.
//
// Compile with -DUSE_RESET to run T1/T2 with sidetone_reset() in place of sidetone_idle(),
// which is what the audio callback did before (it shows what the old behaviour was).
//
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sidetone.h"

#ifdef USE_RESET
#define RX_CALLBACK() sidetone_reset()
#else
#define RX_CALLBACK() sidetone_idle()
#endif

#define NQ    128
#define FREQ  700
#define AMP   1.0

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); } else { printf("  FAIL  "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

static double buf[8192];

//
// Render nblocks callbacks of NQ frames, return the index of the first sample whose magnitude is above
// "thr", or -1. The raised cosine ramp is 240 samples long, so 0.005 is reached after about 10 samples.
//
static int first_above(int nblocks, double thr) {
  for (int b = 0; b < nblocks; b++) {
    sidetone_render(buf, NQ, FREQ, AMP);
    for (int i = 0; i < NQ; i++) {
      if (fabs(buf[i]) > thr) { return b * NQ + i; }
    }
  }
  return -1;
}

static void msleep(int ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

int main(void) {
  //
  // T1: key-down queued while the callback still runs in "not transmitting" mode
  //
  printf("T1 key-down queued before TX is up\n");
  sidetone_reset();
  sidetone_queue(1, 0);
  for (int i = 0; i < 4; i++) { RX_CALLBACK(); }       // ~10 msec of RX callbacks at 128 frames
  int on = first_above(8, 0.005);
  CHECK(on >= 0 && on < 40, "tone starts in the first block after TX is up (first sample above threshold: %d)", on);
  sidetone_reset();

  //
  // T2: key-down with a large wait, first event after TX starts
  //
  printf("T2 key-down with wait = 12000 samples (250 msec) as first event after TX start\n");
  sidetone_reset();
  for (int i = 0; i < 4; i++) { RX_CALLBACK(); }
  sidetone_queue(1, 12000);
  on = first_above(8, 0.005);
  CHECK(on >= 0 && on < 40, "executed at once (first sample above threshold: %d, would be ~12000 if the wait were counted from TX start)", on);
  sidetone_reset();

  //
  // T3: spacing of the following events: key-down (wait 0), key-up after 2400 samples (50 msec)
  //
  printf("T3 element length after a late start\n");
  sidetone_reset();
  for (int i = 0; i < 4; i++) { RX_CALLBACK(); }
  sidetone_queue(1, 12000);
  sidetone_queue(0, 2400);
  // 12 kHz is a quarter of the sample rate: the samples are sin(k*pi/2) = 0, 1, 0, -1, so every window of
  // 8 samples contains a full peak and the peak over the window is the envelope.
  int nblk = 80;
  int up = -1, down = -1;
  double peak = 0.0;
  // envelope: peak of |out| over 8 samples
  long n = 0;
  double env_prev = 0.0;
  for (int b = 0; b < nblk; b++) {
    sidetone_render(buf, NQ, 12000, AMP);
    for (int i = 0; i < NQ; i += 8) {
      double e = 0.0;
      for (int j = 0; j < 8; j++) { double a = fabs(buf[i + j]); if (a > e) { e = a; } }
      if (e > peak) { peak = e; }
      if (down < 0 && e >= 0.5) { down = (int)(n + i); }           // 50% rising crossing
      if (down >= 0 && up < 0 && env_prev >= 0.5 && e < 0.5) { up = (int)(n + i); }
      env_prev = e;
    }
    n += NQ;
  }
  int len = up - down;
  CHECK(down >= 0 && up > down, "rising and falling 50%% crossings found (%d, %d)", down, up);
  CHECK(abs(len - 2400) <= 16, "element length %d samples (requested 2400, tolerance 16 for the 8 sample envelope)", len);
  sidetone_reset();

  //
  // T4: events never played are discarded after a while
  //
  printf("T4 stale events are discarded\n");
  sidetone_reset();
  sidetone_queue(1, 0);
  RX_CALLBACK();
  msleep(350);
  for (int i = 0; i < 4; i++) { RX_CALLBACK(); }
  on = first_above(8, 0.005);
#ifdef USE_RESET
  CHECK(on < 0, "(USE_RESET) nothing left over (first sample above threshold: %d)", on);
#else
  CHECK(on < 0, "queued key-down older than the grace time was dropped (first sample above threshold: %d)", on);
#endif
  sidetone_reset();

  //
  // T5: reset releases the key
  //
  printf("T5 reset releases the tone\n");
  sidetone_reset();
  sidetone_queue(1, 0);
  first_above(4, 2.0);                                  // render 4 blocks, tone is on
  sidetone_idle();
  sidetone_reset();
  int again = first_above(4, 0.001);
  CHECK(again < 0, "silence after reset (first sample above threshold: %d)", again);

  printf("%s\n", failures ? "FAILED" : "ALL PASSED");
  return failures ? 1 : 0;
}

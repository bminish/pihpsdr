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

#include <math.h>
#include <stdatomic.h>
#include <time.h>

#include "sidetone.h"

//
// Ring of CW key events, written by tx_queue_cw_event() (through sidetone_queue()) and read by the audio
// output callback. Single producer in practice (the same assumption the CW ring in transmitter.c makes),
// single consumer.
//
#define ST_RING_SIZE 1024
#define ST_RING_MASK 1023

typedef struct {
  int state;     // 1: key down, 0: key up
  int wait;      // samples to wait, counted from the previous event being executed
} ST_EVENT;

static ST_EVENT st_ring[ST_RING_SIZE];
static volatile atomic_int st_inpt = 0;
static volatile atomic_int st_outpt = 0;
static volatile atomic_llong st_queue_ns = 0;   // CLOCK_MONOTONIC time of the latest sidetone_queue()

//
// Events that are queued while the radio is not yet transmitting (break-in: the key event comes first, the
// RX -> TX switch takes some 20 msec) must survive until TX is up. Events older than this are stale (TX never
// came, wrong mode, duplex, ...) and are dropped by sidetone_idle().
//
#define ST_STALE_NS 200000000LL

static long long st_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

//
// Renderer state (consumer side only)
//
#define RAMP_LEN 240               // 5 msec, same as the TX engine's side tone ramp
static double ramp[RAMP_LEN + 1];
static int    ramp_ready = 0;
static int    ramp_ptr = 0;
static int    key = 0;
static long   key_samples = 0;     // how long the key has been down
static int    delay_samples = 0;   // samples since the previous event was executed
static double phase = 0.0;

void sidetone_queue(int down, int wait) {
  int num = (st_inpt - st_outpt) & ST_RING_MASK;
  //
  // As in the TX engine: if the buffer is nearly full, only queue key-up events
  //
  if (num + 16 > ST_RING_SIZE) { down = 0; }
  int newpt = (st_inpt + 1) & ST_RING_MASK;
  if (newpt != st_outpt) {
    st_ring[st_inpt].state = down;
    st_ring[st_inpt].wait = wait;
    atomic_thread_fence(memory_order_release);
    st_inpt = newpt;
  }
  st_queue_ns = st_now_ns();
}

//
// Put the renderer into the state it has after a long pause: key up, no ramp, and delay_samples at its
// maximum. The TX engine counts cw_delay_time all the time, also while receiving, so the first event after a
// pause is executed at once whatever its "wait" is (a key-down from CW_KEYER_KEYDOWN carries the gap to the
// previous event, up to 0.5 sec). This renderer only runs while transmitting, so the state of a long pause
// has to be set explicitly, otherwise that wait would be counted again from the start of TX.
//
static void st_pause_state(void) {
  key = 0;
  key_samples = 0;
  delay_samples = 999999;
  ramp_ptr = 0;
}

void sidetone_reset(void) {
  st_outpt = st_inpt;
  st_pause_state();
}

void sidetone_idle(void) {
  st_pause_state();
  //
  // Keep the queue unless its newest event is stale
  //
  if (st_outpt != st_inpt && st_now_ns() - st_queue_ns > ST_STALE_NS) {
    st_outpt = st_inpt;
  }
}

void sidetone_render(double *out, int n, int freq, double amp) {
  if (!ramp_ready) {
    //
    // raised cosine, as the TX engine uses for the side tone
    //
    for (int i = 0; i <= RAMP_LEN; i++) {
      ramp[i] = 0.5 * (1.0 - cos((double) i * M_PI / (double) RAMP_LEN));
    }
    ramp_ready = 1;
  }
  const double dphi = 2.0 * M_PI * (double) freq / 48000.0;
  for (int k = 0; k < n; k++) {
    //
    // Execute the next event if it has been queued and its wait time has elapsed.
    // The wait is counted in output samples from the previous event actually being
    // executed (this is what the TX engine does as well), therefore the length of
    // an element does not depend on when the callbacks happen to run.
    //
    if (delay_samples < 999999) { delay_samples++; }
    if (st_outpt != st_inpt && delay_samples >= st_ring[st_outpt].wait) {
      key = st_ring[st_outpt].state;
      delay_samples = 0;
      st_outpt = (st_outpt + 1) & ST_RING_MASK;
    }
    if (key) {
      //
      // hardware protection as in the TX engine: key down for more than 20 seconds
      //
      if (++key_samples > 960000) { key = 0; }
    } else {
      key_samples = 0;
    }
    if (key) {
      if (ramp_ptr < RAMP_LEN) { ramp_ptr++; }
    } else {
      if (ramp_ptr > 0) { ramp_ptr--; }
    }
    out[k] = amp * ramp[ramp_ptr] * sin(phase);
    phase += dphi;
    if (phase > 2.0 * M_PI) { phase -= 2.0 * M_PI; }
  }
}

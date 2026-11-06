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
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#include "sidetone.h"

#ifdef CWDEBUG
#include <glib.h>
#include "message.h"
#endif

//
// Absolute-time event ring. Single producer in practice (the same assumption
// the CW ring in transmitter.c makes), single consumer (the audio callback).
//
#define ST_RING_SIZE 1024
#define ST_RING_MASK 1023

typedef struct {
  int    state;
  int    wait;      // samples to wait, counted from the previous event being executed
  double qtime;     // CLOCK_MONOTONIC when queued, used for statistics only
} ST_EVENT;

static ST_EVENT st_ring[ST_RING_SIZE];
static volatile atomic_int st_inpt = 0;
static volatile atomic_int st_outpt = 0;

//
// Renderer state (consumer side only)
//
#define RAMP_LEN 240               // 5 msec, same as the TX engine
static double ramp[RAMP_LEN + 1];
static int    ramp_ready = 0;
static int    ramp_ptr = 0;
static int    key = 0;
static long   key_samples = 0;     // how long the key has been down
static int    delay_samples = 0;   // samples since the previous event was executed
static double phase = 0.0;

double sidetone_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1.0E-9 * ts.tv_nsec;
}

#ifdef CWDEBUG
//
// Measurement. Nothing is printed from the audio thread; records go into
// a ring and are printed from a GLib timer in the main thread.
//
typedef struct {
  int    kind;        // 0: element, 1: callback interval statistics
  double lag_down;    // msec, event time -> sample position executed
  double lag_up;
  double len_err;     // unused
  double len;         // msec, rendered length of the element
  int    n;           // statistics: number of callbacks
  double rms;         // statistics: rms deviation of callback interval, usec
  double max;         // statistics: largest deviation, usec
} ST_DBG;
#define ST_DBG_N 128
static ST_DBG st_dbg[ST_DBG_N];
static volatile int st_dbg_in = 0;
static volatile int st_dbg_out = 0;
static guint st_dbg_timer = 0;
static long   out_pos = 0;          // samples rendered so far
static long   down_pos = 0;
static double down_lag = 0.0;
static double prev_t0 = 0.0;
static int    cb_n = 0;
static double cb_sum2 = 0.0;
static double cb_max = 0.0;

static int st_dbg_drain(gpointer data) {
  while (st_dbg_out != st_dbg_in) {
    ST_DBG *r = &st_dbg[st_dbg_out];
    if (r->kind == 0) {
      t_print("CWDBG synth: len=%.1f ms lag(queue->output position): down=%.2f ms up=%.2f ms\n",
              r->len, r->lag_down, r->lag_up);
    } else {
      t_print("CWDBG synth: callback interval jitter over %d callbacks: rms=%.0f us max=%.0f us\n",
              r->n, r->rms, r->max);
    }
    st_dbg_out = (st_dbg_out + 1) % ST_DBG_N;
  }
  return TRUE;
}

static void st_dbg_push(const ST_DBG *r) {
  int n = st_dbg_in;
  st_dbg[n] = *r;
  st_dbg_in = (n + 1) % ST_DBG_N;
}
#endif

void sidetone_queue_at(int down, int wait, double now) {
  int num = (st_inpt - st_outpt) & ST_RING_MASK;
  //
  // As in the TX engine: if the buffer is nearly full, only queue key-up events
  //
  if (num + 16 > ST_RING_SIZE) { down = 0; }
  int newpt = (st_inpt + 1) & ST_RING_MASK;
  if (newpt != st_outpt) {
    st_ring[st_inpt].state = down;
    st_ring[st_inpt].wait = wait;
    st_ring[st_inpt].qtime = now;
    atomic_thread_fence(memory_order_release);
    st_inpt = newpt;
  }
#ifdef CWDEBUG
  if (!st_dbg_timer) { st_dbg_timer = g_timeout_add(500, st_dbg_drain, NULL); }
#endif
}

void sidetone_queue(int down, int wait) {
  sidetone_queue_at(down, wait, sidetone_now());
}

void sidetone_reset(void) {
  st_outpt = st_inpt;
  key = 0;
  key_samples = 0;
  delay_samples = 0;
  ramp_ptr = 0;
}

void sidetone_render(double *out, int n, double t0, int freq, double amp) {
  if (!ramp_ready) {
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
      int state = st_ring[st_outpt].state;
#ifdef CWDEBUG
      double lag = (t0 + (double) k / 48000.0 - st_ring[st_outpt].qtime) * 1000.0;
      if (state && !key) {
        down_pos = out_pos + k;
        down_lag = lag;
      } else if (!state && key) {
        ST_DBG r;
        r.kind = 0;
        r.len = (double)((out_pos + k) - down_pos) / 48.0;
        r.len_err = 0.0;
        r.lag_down = down_lag;
        r.lag_up = lag;
        st_dbg_push(&r);
      }
#endif
      key = state;
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
#ifdef CWDEBUG
  //
  // Callback timing: the interval between two callbacks should be exactly
  // n/48000 sec. Its deviation is the jitter that ends up in the element lengths.
  //
  if (prev_t0 != 0.0 && (t0 - prev_t0) < 0.1) {
    double d = ((t0 - prev_t0) - (double) n / 48000.0) * 1.0E6;   // usec
    cb_sum2 += d * d;
    if (fabs(d) > cb_max) { cb_max = fabs(d); }
    if (++cb_n >= 1500) {
      ST_DBG r;
      r.kind = 1;
      r.n = cb_n;
      r.rms = sqrt(cb_sum2 / cb_n);
      r.max = cb_max;
      st_dbg_push(&r);
      cb_n = 0;
      cb_sum2 = 0.0;
      cb_max = 0.0;
    }
  }
  prev_t0 = t0;
  out_pos += n;
#endif
}

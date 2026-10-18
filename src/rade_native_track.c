/* Copyright (C)
*  2026 - Brendan Minish, EI6IZ
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
#include <string.h>

#include "rade_native_track.h"

void rade_track_reset(rade_track_t *tr) {
  memset(tr, 0, sizeof *tr);
}

int rade_track_update(rade_track_t *tr, double now, int sync, double foff_hz, int sign, long long rit_now, long long *rit_new) {
  double dt = tr->last_t > 0.0 ? now - tr->last_t : 0.0;
  if (dt < 0.0 || dt > 2.0) { dt = 0.0; }            // a clock step or a long stall: no catching up
  tr->last_t = now;
  if (!sync) {
    tr->locked = 0;
    if (tr->seen && now - tr->last_sync_t > TRACK_OVER_GAP_S) {
      // the over has ended: back to zero in one go, nothing is being received
      rade_track_t fresh;
      rade_track_reset(&fresh);
      fresh.last_t = now;
      *tr = fresh;
      if (rit_now != 0) { *rit_new = 0; return 1; }
    }
    return 0;
  }
  if (!tr->locked) {                                  // a lock gained, or regained: the average starts again
    tr->locked = 1;
    tr->n = 0;
    tr->mean = 0.0;
    tr->age = 0.0;
    if (!tr->seen) { tr->cmd = (double)rit_now; }
    tr->seen = 1;
  }
  tr->last_sync_t = now;
  tr->age += dt;
  double need = (double)rit_now + (sign >= 0 ? 1.0 : -1.0) * (double)foff_hz;
  if (tr->n < TRACK_WINDOW) { tr->n++; }
  tr->mean += (need - tr->mean) / tr->n;
  double target = tr->mean;
  if (target > TRACK_MAX_HZ) { target = TRACK_MAX_HZ; }
  if (target < -TRACK_MAX_HZ) { target = -TRACK_MAX_HZ; }
  double lim = TRACK_FAST_HZ_S / (1.0 + tr->age / TRACK_DECAY_S);
  if (lim < TRACK_SLOW_HZ_S) { lim = TRACK_SLOW_HZ_S; }
  double step = target - tr->cmd;
  if (step > lim * dt) { step = lim * dt; }
  if (step < -lim * dt) { step = -lim * dt; }
  tr->cmd += step;
  long long r = llround(tr->cmd);
  if (r == rit_now) { return 0; }
  *rit_new = r;
  return 1;
}

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

#ifndef _RADE_NATIVE_TRACK_H_
#define _RADE_NATIVE_TRACK_H_

//
// Follows the station being received by moving the RIT, from the frequency offset librade reports.
// Pure arithmetic, no GTK and no radio: rade_native_menu.c feeds it the status every poll and applies what it returns.
//
//  - The offset is averaged over the lock: the plain mean while the lock is young, a window of TRACK_WINDOW samples at most.
//    What is averaged is the RIT that would have brought the offset to zero (the RIT in force plus the offset, signed for
//    the sideband), so the average does not chase the correction the RIT has already made.
//  - The RIT moves towards that at no more than TRACK_FAST_HZ_S, and the limit falls with the age of the lock to TRACK_SLOW_HZ_S
//    (1 Hz in 5 s), so a settled station is not pulled about.
//  - Lock lost then regained: the average starts again (the RIT stays where it is). No lock for longer than TRACK_OVER_GAP_S
//    (the log's own end of an over): the RIT goes to zero.
//
#define TRACK_FAST_HZ_S   3.0     // limit on the RIT's rate of change as a lock is gained
#define TRACK_SLOW_HZ_S   0.2     // and when it has been held a long time
#define TRACK_DECAY_S     5.0     // the limit is FAST / (1 + age / DECAY)
#define TRACK_WINDOW      240     // samples (60 s at the 250 ms poll) at most in the average
#define TRACK_OVER_GAP_S  1.5
#define TRACK_MAX_HZ      50      // the RIT never goes further than this from zero

typedef struct {
  int    locked;          // sync on the last sample
  int    seen;            // sync has been seen since the RIT was last zeroed
  int    n;               // samples in the average
  double mean;            // the RIT that would zero the offset
  double cmd;             // where the RIT has been taken to (fractional)
  double age;             // seconds since the lock was (re)gained
  double last_t;          // time of the last sample
  double last_sync_t;     // time of the last sample with sync
} rade_track_t;

void rade_track_reset(rade_track_t *tr);

//
// One sample: now in seconds, sync and foff_hz from the status, sign +1 for an upper-sideband passband and -1 for lower,
// rit_now the RIT in force. Returns 1 and sets *rit_new when the RIT should change.
//
int rade_track_update(rade_track_t *tr, double now, int sync, double foff_hz, int sign, long long rit_now, long long *rit_new);

#endif

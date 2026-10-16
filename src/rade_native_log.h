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

#ifndef _RADE_NATIVE_LOG_H_
#define _RADE_NATIVE_LOG_H_

//
// Parameter log for RADE: what librade gives us on receive (sync, SNR, frequency offset, the
// per-call statistics, buffer fill) and what the transmit side is doing (mic loudness, peak,
// leveller gain, drive, buffer fill), as one CSV file per run, for analysis and graphs later.
//
// The producers are the receive worker and the transmit audio thread; they must never wait.
// Each has a single-producer ring buffer: logging a row is a copy and an atomic store. A
// writer thread wakes 5 times a second, formats and writes what is there, and flushes. If the
// writer falls behind the ring overwrites nothing: the row is dropped and counted (the count
// is written at the end of the file). Nothing is done when logging is off but one flag test.
//
// rade_log_event() is for the GTK main thread only (menu changes); it is a third ring.
//
// The file is rade-logs/<time>_<kHz>_RADE-V<n>.csv, a new one each time logging is started. Lines
// starting with # are settings; the header row follows; the columns are the same for every row
// kind (rx, tx, ev) and left empty where they do not apply:
//
//   t_unix,t,kind,rx_pos,sync,snr_db,foff_hz,nout,delta_hat,est_foff,gain,snr_est,in_fill,out_fill,
//   rx_underruns,rx_overruns,tx,trim_db,agc_db,drive_db,loud_mom,loud_avg,peak_dbfs,limiting,
//   tx_underruns,tx_out_fill,tx_pcm_fill,end_state,note
//
// t is seconds since the log was started; rx_pos is the 8 kHz sample count handed to librade
// (the same clock as rx_pos in a capture's _stats.csv).
//
typedef struct {
  long   rx_pos;
  int    sync, nout;
  float  snr_db, foff_hz;                  // as shown in the RADE window
  float  delta_hat, est_foff, gain, snr_est;   // rade_get_stats()
  long   in_fill, out_fill;                // 8 kHz samples queued to librade / 16 kHz speech queued to play
  unsigned long underruns, overruns;
} rade_log_rx_t;

typedef struct {
  int    tx;                               // keyed
  double trim_db, agc_db, drive_db;
  double loud_mom;                         // LUFS, last 400 ms
  double loud_avg;                         // LUFS, gated average; NaN if not computed this row
  double peak_dbfs;                        // peak of the last second
  int    limiting;
  unsigned long underruns;
  long   out_fill, pcm_fill;
  int    end_state;
} rade_log_tx_t;

// Starts a new file (stopping any current one). meta: lines of settings, without the leading #.
// Returns 0, or -1 with the log left off.
int         rade_log_start(const char *meta);
void        rade_log_stop(void);
int         rade_log_active(void);
const char *rade_log_path(void);

void        rade_log_rx(const rade_log_rx_t *r);
void        rade_log_tx(const rade_log_tx_t *r);
void        rade_log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif

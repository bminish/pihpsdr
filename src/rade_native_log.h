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
// Parameter log for RADE, for analysis and graphs: what librade gives us on receive and what the transmit side is doing, reduced
// to what is worth keeping (see docs/rade/logging.md for what was measured and why).
//
// The producers are the receive worker and the transmit audio thread; they never wait: each has a single-producer ring and
// logging a row is a copy and an atomic store. A writer thread wakes 5 times a second and does all the rest: it averages,
// finds the overs (stretches in sync, keyed stretches), puts the dial, mode and call to them, and writes the files below. If it
// falls behind a row is dropped and counted (in meta.txt at the end).
//
// A run is a directory rade-logs/<time>/ (a new one each time logging is started) of plain CSV tables with no empty columns, and
// rade-logs/<time>.tar.gz of it when logging stops. t is seconds since the start (0.01 s); utc_* are ISO 8601 UTC.
//
//   meta.txt     settings at the start; at the end the number of dropped rows
//   rx.csv       only while in sync, 2 rows a second, means over the half second:
//                  t, snr_mean, snr_min, foff_mean, n  (and for V2 the means of librade's delta_hat, gain, snr_est, est_foff)
//   tx.csv       only while keyed, 2 rows a second: t, loud_lufs, peak_dbfs, agc_db, limiting_pct, out_fill_min
//   events.csv   on change: t, kind, value. Kinds: wave dial mode invert tx_enabled drive trim agc (the settings, at the start and
//                when they change), key_down key_up sync_on sync_off (overs), call (value: CALL snr=x), eoo, underrun overrun
//                (counts since the last), tx_underrun
//   overs.csv    one row per heard over (in sync for at least 1 s, gaps under 1.5 s joined), written when it ends:
//                  utc_start, t_start, t_end, dial_hz, mode, wave, invert, snr_mean, snr_min, snr_max, foff_mean, foff_sd,
//                  sync_pct, eoo, call, call_snr
//   tx_overs.csv one row per transmitted over: utc_start, t_start, t_end, dial_hz, wave, drive_db, trim_db, agc_db_end,
//                  loud_mean, peak_max, limiting_pct, underruns, hold_ms
//
// With no lock nothing is written for the receiver (the SNR is not a measurement then); while not keyed nothing for the transmitter.
//

typedef struct {
  long   rx_pos;
  int    sync, nout, eoo;                   // eoo: this call of librade saw an end-of-over frame
  float  snr_db, foff_hz;                  // as shown in the RADE window
  float  delta_hat, est_foff, gain, snr_est;   // rade_get_stats() (librade's V2 statistics; zero for V1)
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

// What the radio was doing, kept up to date by the menu (about 4 times a second); the writer takes it at the start of an over.
typedef struct {
  long long dial_hz;
  char   mode[8];
  int    wave;                             // 0 off, 1 V1, 2 V2
  int    invert;
} rade_log_ctx_t;

// Starts a new run (stopping any current one). meta: lines of settings. Returns 0, or -1 with the log left off.
int         rade_log_start(const char *meta);
void        rade_log_stop(void);
int         rade_log_active(void);
const char *rade_log_path(void);              // the run's directory

void        rade_log_rx(const rade_log_rx_t *r);
void        rade_log_tx(const rade_log_tx_t *r);
void        rade_log_context(const rade_log_ctx_t *c);
// Events: GTK main thread only. "kind value" (see above); the value may contain spaces.
void        rade_log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

//
// Makes dir ignore its own contents for git (a .gitignore of "*" inside it), if it has none. The run logs and captures are
// written to the current directory, which is the source tree when piHPSDR is started from it, and must not be committed;
// doing it from here keeps the repository's own .gitignore free of them.
//
void rade_ignore_dir(const char *dir);

#endif

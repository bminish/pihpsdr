# RADE native: the parameter log - what it holds, what is worth keeping

> **Built (2026-10-16): section 4 is the format now written.** Sections 1-3 describe the old single-file log and why it was reduced; section 5 lists what was done first.
> The questions of section 6 were answered: split tables yes; V2's librade statistics kept (in `rx.csv`, for V2 rows); a `tar.gz` copy made when a run stops.
> Details of the implemented tables: the comment at the top of `src/rade_native_log.h`; test: `test/rade_native/test_log.py`.

Measured on the 77 logs in `rade-logs/` (2.83 MB, 14 118 receive rows, 17 116 transmit rows, 194 events), by `test/rade_native/log_analysis.py`
(repeatable: `python3 test/rade_native/log_analysis.py [files]`). The log is meant to feed an application that graphs SNR, offsets and so on
and lets transmissions be found by frequency, time and the call heard at the end of the over. Nothing below the "Done" heading has been changed.

## 1. What is logged now

One CSV per run, one wide table of 29 columns for three kinds of row (`rx`, `tx`, `ev`), the fields of the other kinds left empty.

| Kind | Rate | What |
|---|---|---|
| `rx` | one per librade call: 8.3/s (V1), about 25/s (V2) | `rx_pos, sync, snr_db, foff_hz, nout, delta_hat, est_foff, gain, snr_est, in_fill, out_fill, rx_underruns, rx_overruns` |
| `tx` | every 100 ms **whenever the engine is enabled, keyed or not** | `tx, trim_db, agc_db, drive_db, loud_mom, loud_avg, peak_dbfs, limiting, tx_underruns, tx_out_fill, tx_pcm_fill, end_state` |
| `ev` | on change, polled every 250 ms | the state line `wave= dial= mode= invert= tx=`; drive changes; log start |

About 102 bytes per `rx` row and 80 per `tx` row. **A fixed 15 bytes of each row is the absolute `t_unix`, and 12-13 of the 29 fields are empty commas.**

## 2. How it is spent (all 77 logs)

- **47 % of the transmit rows were written while not keyed** (8 027 of 17 116): the engine runs the mic meter whenever RADE TX is enabled, and the log copied it. In the one long receive-only log (446 s, 738 KB) the 4 463 transmit rows, 358 KB, **48 % of the file,
  say only that the mic hiss moved**: nothing else in them changes while idle (trim, drive, AGC, buffers are constant).
- **76 % of the receive rows were while not in sync** (10 664 of 14 118). With no lock the SNR, `gain` and `snr_est` still change nearly every row (84 %) but they are not measurements (the SNR the decoder gives with no lock is not an SNR: that is what put 0s in the Reporter);
  the offset changes 15 % of the time.
- **Columns that never change**: `delta_hat`, `est_foff`, `gain`, `snr_est` (0.00 in sync; they are librade's V2 statistics and are zeros for V1 - they only move when V2 is receiving), `nout` (1 %), `rx_underruns`, `rx_overruns`, `tx_underruns`, `trim_db`, `agc_db`, `drive_db`.
  The few times they change are events, not samples.
- **Columns that change every row but are bookkeeping**: `rx_pos` (always +`nin`: it is the position in the capture's `rx48.wav`; the capture has it already), `in_fill` and, with a lock, `out_fill` (buffer levels, 100 % changing, useful only as a health check: min/max per second would do), `tx_out_fill` and `tx_pcm_fill` (99 %, 97 %).
- **Columns that carry the information**, in sync: `snr_db` (changes 99 % of rows; a 2 Hz average keeps it to 0.39 dB rms, worst 3.2 dB; 1 Hz: 0.72 dB, worst 6.8 dB; 4 Hz: 0.20 dB, worst 1.6 dB) and `foff_hz` (57 %).
  While keyed: `loud_mom`, `loud_avg`, `peak_dbfs`, `limiting`, `agc_db` (changes only with the leveller), `end_state`.
- The `ev` state line is the only place the dial, mode and waveform are recorded, which is what is needed to say *which frequency* a stretch of rows was; it is polled at 250 ms and written only on change, so it is cheap.
- In the long receive log, 8 overs (stretches in sync joined across gaps under 1.5 s, at least 1 s) cover 93 % of the rows; they last 1.7-112 s with mean SNRs of 0.6-14.8 dB and offsets that cluster at about -2 Hz and about -16 Hz
  (two different transmitting stations). That is the structure an application wants to show and it is not in the file as such: it has to be rebuilt from the rows.

## 3. What a graphing / exploring application needs

1. **Time on the UTC axis** (one start time, then seconds).
2. **SNR and offset against time while locked**, at a rate that shows fades (2 Hz is enough), with the minimum SNR over each interval so a fade deeper than the mean shows.
3. **Overs as objects**: start, end, duration, **dial frequency, mode, waveform, invert**, SNR mean / min / max, offset mean and spread, **the call decoded from the end-of-over frame** and whether the end-of-over frame was seen, sync fraction. To pick a transmission by frequency, time and call.
4. **Transmit overs the same way**: start, end, frequency, drive, trim, leveller gain, loudness and peak, underruns, end-of-over hold; and a sample series while keyed.
5. **Events** (dial / mode / waveform / invert / drive / trim / AGC changes, underruns and overruns as counts when they happen, sync gained and lost), so the above can be put in context.
6. What it does not need: anything while idle, the unlocked SNR, the buffer fill values sample by sample.

## 4. Proposal (not built)

One directory per run, `rade-logs/<start>/`, plain CSV tables with no empty columns, so pandas / a viewer can read each as it is:

| File | Rows | Columns |
|---|---|---|
| `meta.txt` | once | program, start time (UTC and local), waveform, dial, mode, invert, trim, AGC, drive, TX state |
| `rx.csv` | **only in sync, 2 per second**: mean over the interval | `t` (s, 0.01), `snr_mean`, `snr_min`, `foff_mean` (+ `n` calls averaged) |
| `tx.csv` | **only while keyed, 2 per second** | `t`, `loud_lufs`, `peak_dbfs`, `agc_db`, `limiting` (share of the interval), `out_fill_min` |
| `events.csv` | on change | `t`, `kind`, `value`: `dial`, `mode`, `wave`, `invert`, `drive`, `trim`, `agc`, `key_down`, `key_up`, `sync_on`, `sync_off`, `eoo_seen`, `call` (with the SNR), `underrun` / `overrun` (count), `tx_underrun` |
| `overs.csv` | **one row per heard over**, written when it ends (a few seconds after sync is lost, to take the call) | `t_start`, `t_end`, `dial_hz`, `mode`, `wave`, `invert`, `snr_mean`, `snr_min`, `snr_max`, `foff_mean`, `foff_sd`, `sync_pct`, `eoo` (0/1), `call`, `call_snr` |
| `tx_overs.csv` | one row per transmitted over | `t_key`, `t_end`, `dial_hz`, `wave`, `drive_db`, `trim_db`, `agc_db_end`, `loud_mean`, `peak_max`, `limiting_pct`, `underruns`, `hold_ms` |

- **Not in sync: no rows**, only the `sync_on` / `sync_off` events (and the over rows). Not keyed: no rows at all.
- **Estimated size** for the 77 logs: about **122 KB for 2.83 MB (23 times smaller)**, 74 KB of which is the air-test transmit overs (kept at 2 Hz); the receive log of 446 s becomes about 30 KB. Of the 15 bytes per row for the clock, 10 go; the empty columns go.
- **What is lost**: sample-by-sample SNR and offset (kept to about 0.4 dB rms by the mean, with the minimum); the librade statistics that are zero for V1 (V2's `delta_hat`, `gain`, `snr_est`, `est_foff` can be kept as events or as columns of `rx.csv` for V2 only, if wanted); buffer fill sample by sample (underruns and overruns are events).
  Anything that needs the full-rate series (a spectrum of the SNR's fluctuation, say) is what the capture is for (`rx48.wav`, `stats.csv` per call).
- **Where it is done**: in the log's writer thread (not in the audio threads, which only push raw rows into the lock-free rings as now): the thread averages, detects the over from the sync flags, takes the dial / mode context from the menu's 250 ms state poll and the call from the call event, and writes the files. The cost to the receive and transmit threads does not change.
- **Compatibility**: the single wide CSV is replaced; `log_analysis.py` and `air_test.py` (which read `rade-logs/*.csv` for the mic statistics) would be moved to the new tables.

## 5. Done now (small, safe, same format)

- **Transmit rows are written only while keyed** (47 % of the transmit rows, none of the information).
- **With no lock, receive rows are written at the change of state and then once a second** (76 % of the receive rows were without a lock, and carried nothing a graph can use).
These two alone remove about 60 % of the bytes of a receive-only log; the rest of section 4 is the larger change.

## 6. Questions for the decision (answered: 1 yes, 2 keep for V2, 3 yes)

1. Go ahead with the split tables of section 4 (and the over summaries)? The rates (2 Hz) and the minimum SNR column are the main choices.
2. Keep V2's `delta_hat` / `gain` / `snr_est` / `est_foff` in `rx.csv` for V2 overs? (They are librade's own V2 statistics and show how the decoder sees the channel, but they are constant for V1 and unexplained for graphs.)
3. A compressed copy (gzip on close) as well? About 5-10 times smaller again, but unreadable while the run is going.

## 7. As built

- A run is `rade-logs/<YYYYMMDD-HHMMSS>/` with `meta.txt`, `rx.csv`, `tx.csv`, `events.csv`, `overs.csv`, `tx_overs.csv`, and `rade-logs/<same>.tar.gz` made in the background when the run stops (needs `tar`).
- **rx.csv**: only while in sync, 2 rows a second: `t, snr_mean, snr_min, foff_mean, n` and, for V2 rows, `delta_hat, gain, snr_est, est_foff` (empty for V1). **tx.csv**: only while keyed, 2 a second. **events.csv**: the settings as events (wave, dial, mode, invert, tx_enabled, drive,
  trim, agc: all at the start, then on change), `key_down`/`key_up`, `sync_on`/`sync_off`, `eoo`, `call` (value `CALL snr=x`, `snr=none` if there was no lock behind it), `underrun`/`overrun` (counts), `tx_underruns`. The file is in the order written: sort by `t`.
- **overs.csv** and **tx_overs.csv** as in section 4, with the dial, mode, waveform and invert taken when the over starts (from the menu's 250 ms poll) and the call attached from the call event arriving up to 2.5 s after the over ends.
- Rules: an over is a stretch in sync of at least 1 s, joined across gaps under 1.5 s; a transmitted over ends when no keyed row has come for 0.5 s; `hold_ms` is the time the engine spent in the end-of-over hold (100 ms steps).
- Sizes, scripted scene (`test_log.py`): 2 runs in 2.6 KB; the producers' cost is unchanged (mean about 0.6 us per row, worst about 23 us on a busy machine). The old single CSVs in `rade-logs/*.csv` are left alone; `log_analysis.py` reads both; `air_test.py` reads the new `tx_overs.csv` for the mic columns.
- **Not yet tried on a live run**: only the scripted scene and the unit build. The first real use will show whether the over rules (1.5 s gap, 1 s minimum) suit real fading.

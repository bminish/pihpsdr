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
- **Clean-up of runs that did not end cleanly (2026-10-17).** piHPSDR compresses a run and removes its directory only when the run is stopped (it also removes the directories of earlier *ended* runs
  that have a copy). A run that ends any other way (program killed, crash, power) stays a directory with no `ended_utc`, and nothing finished it. `tools/rade-logview/tidy_logs.py` does: **every run directory but the newest is taken as over**
  (names are start times; only the newest can still be written to), and for each it adds `ended_utc` (the time of the last file written) and `recovered: 1` to `meta.txt` if there is no `ended_utc`, writes `<run>.tar.gz.part`, reads it back whole and compares file sizes with the
  directory, renames it to `<run>.tar.gz` and only then removes the directory. A copy that does not check out leaves the directory alone; so does a directory with a file changed in the last 60 s, or one named in `--keep`. The viewer runs it at start (`rade_logview.py`
  `main`), passing what was named on its command line as `--keep`. By hand: `python3 tools/rade-logview/tidy_logs.py [--dry-run] [rade-logs-dir]`. Test: `test/rade_logview/test_tidy_logs.py`. The old single-file `*_RADE.csv` logs are not touched.
- Sizes, scripted scene (`test_log.py`): 2 runs in 2.6 KB; the producers' cost is unchanged (mean about 0.6 us per row, worst about 23 us on a busy machine). The old single CSVs in `rade-logs/*.csv` are left alone; `log_analysis.py` reads both; `air_test.py` reads the new `tx_overs.csv` for the mic columns.
- **Not yet tried on a live run**: only the scripted scene and the unit build. The first real use will show whether the over rules (1.5 s gap, 1 s minimum) suit real fading.

## 8. Development setting: log always on, and every point kept (2026-10-18) - **not intended to ship**

While the modem and the diversity correlator are being developed:

- **The log starts when RADE is switched on and stops when it is switched off** (`retune()` in `rade_native_menu.c`: Off to V1/V2 starts a run, to Off stops it; V1 to V2 keeps the run). The Log button follows and can still stop or start a run by hand. A release should leave logging to the operator again: remove the two `rade_log_*` calls at the end of `retune()`.
- **`rx_full.csv`** is written with every run: every call of librade, in sync or not (V1 8.3 rows/s, V2 about 50/s): `t, rx_pos, sync, eoo, snr_db, foff_hz, nout, in_fill, out_fill` and, for V2, `delta_hat, est_foff, gain, snr_est` (blank for V1). `rx.csv` is unchanged, so the viewer and `tidy_logs.py` work as before; the viewer does not plot `rx_full.csv` yet.
- **Cost (estimated from `rade-captures/*_stats.csv`, which has the same columns; not measured in the writer)**: about 62-68 bytes a row, 16 compressed. Receiving all the time: V1 1.9 MB/h raw, 0.5 MB/h compressed; V2 12 MB/h raw, 2.9 MB/h compressed (about 70 MB a day). The writer thread formats about 1 us a row, so 50 rows/s is about 0.005 % of a core; the audio threads still only push into the ring (4096 rows, about 80 s of V2).
- piHPSDR exiting with RADE on does not stop the run: the directory is left without `ended_utc`, and `tidy_logs.py` finishes it the next time the viewer starts.

## 9. End of over and callsign in the log and the viewer (2026-10-20)

How a call reaches the receiver differs between the waveforms, and so does what the viewer marks. The marks were worked out in `rade-grabber` (the Kiwi receiver that writes the same run directories) and taken from there.

**RADE V1: the call is in the end-of-over (EOO) frame only.** No callsign is sent in the speech frames.

- *Transmit*: when PTT is released the engine sends `rade_tx_eoo()` with the bits from `rade_eoo_text_encode()` (`rade_native.c`, `rade_eoo_text.c`). The frame is 180 floats, 90 QPSK symbols: 56 information bits (up to 8 characters of the 6-bit set 0-9 A-Z /, 48 bits, and a CRC-8) go through LDPC(112,56) to 112 bits, which are 56 symbols with the stride-37 interleave; the other 34 symbols are a known pseudo-random sequence the receiver uses for noise and channel estimation.
- *Receive*: librade reports `has_eoo` when it detects the frame; the engine then runs the soft symbols through the LDPC decoder, checks the CRC-8 and unpacks the call (`rade_eoo_text_decode()`). Detecting the frame writes the `eoo` event; a decoded call writes the `call` event.
- *So there are two separate facts*: the frame was seen (`eoo`), and its call block decoded (`call`). A frame can be seen and its call lost to noise. An over that ends by fading or dropout has neither.

**RADE V2: the call is in the speech stream, the EOO frame carries none.**

- *Call channel* (`rade_v2_text.c`): every 40 ms modem frame carries one BPSK data symbol (+1/-1) through `rade_tx_set_data_symbol()`. The callsign (up to 32 characters, in blocks of 8, base 38) is a repeating stream of 112-bit LDPC(112,56) codewords with a CRC-8, one bit per frame: 112 x 40 ms = 4.5 s a block, four blocks (18 s) for a 32-character call, repeated all through the over. The receiver decodes from a sliding buffer, so the `call` event is written the first time a block decodes, anywhere in the over, not at its end.
- *EOO*: PTT release still sends a short EOO burst, but with no call in it. As reported by `rade-grabber` (`rade_n_eoo_bits()` is 0 for V2; **not re-checked here**; the engine's `neoo > 0` guards in `rade_native.c` are what rely on it), its use is to tell the receiver the over has ended, so the vocoder and the audio can be flushed and muted without waiting for the signal to go. An over that ends by fading has no EOO.

**What the viewer draws** (*End-of-over / calls* ticked):

| Mark | Meaning |
|---|---|
| light red line (`#d97777`, 70 %) | an EOO frame was detected, at the `eoo` event; an over whose row says `eoo` = 1 and has no event is marked at its end. Overs that ended by fading, dropout or an unrecorded flag (old logs) are **not** marked |
| solid green line, call above the top graph | V1: the call block of the EOO frame decoded, at the `call` event |
| dotted green line, call above the top graph | V2: the first time the call decoded in that over (a repeat of the same call in the over is not marked again) |

The wave of a call is taken from the over, else the last `wave` event before it, else the run's `waveform` in `meta.txt`, else the `v1` / `v2` folder the run is in. The colours replace the earlier purple line (V1), the purple label and end-of-over line (V2) and the orange `eoo` line; `events.csv` and `overs.csv` do not change.

**Folders.** `Folder…` and the command line now find runs at any depth under the folder given (`captures/v1/<run>`, `captures/7190kHz_V1/<run>`, as well as `rade-logs/<run>`); a file named is taken as the run it is in; the choice list shows each run's path below the common folder, with Select all / Clear all and scroll bars; a run in a `v1`, `v2` or `...kHz...` folder carries that folder in its name, so runs of different frequencies that began in the same second are told apart. Test: `test/rade_logview/test_logview.py`.

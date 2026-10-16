# RADE native: GPS-stamped Kiwi recordings, transmit frequency and timing

The Kiwi now has a GPS solution (`gps_good` and `fixes` in its status), so its IQ recordings can carry the GNSS time of every block
and the transmit frequency and the transmit timing can be measured against it. What was built, how it was checked, and what the
columns mean. **No GPS-stamped recording of a real over exists yet**: everything below was validated on the Kiwi's receive-only
signals (MSF, WWV) and on synthetic recordings; the first air run with `--gps auto` is the first real measurement.

## 1. What changed

| | |
|---|---|
| `kiwi_rx.py record` | `--gps auto|on|off` (auto: when the Kiwi reports a GPS solution). The two IQ profiles are written as `kiwirecorder -w` (`--kiwi-wav`) files; the audio profile (`usb`/`lsb`) cannot carry stamps (IQ mode only). `session.json` records `gps` and the launch times. |
| `kiwi_gps_wav.py` | reader for those files (a `kiwi` chunk with GPS seconds of week and nanoseconds before every data block; normal WAV readers see only the first block): fits the true sample rate (11998.937 Hz, not 12000) and the UTC time of sample 0 from the stamps (0.1 us scatter). GPS time is ahead of UTC by 18 s (the week is chosen nearest the recording's wall-clock time). |
| `kiwi_rx.py analyze` | for a GPS file the 8 kHz stream is put on exact 1/8000 s steps of GPS time (the Kiwi's rate is resampled out), so the decoder's times are UTC: `start_unix`, `end_unix`, `eoo_at_unix` per over; and the **start / end of the speech against the speech sent** (`speech_start_clipped_ms`, `speech_end_missing_ms`, losses over the first 250 ms, first and last second and the middle, nothing trimmed). |
| engine (`rade_native.c`) | event times of every over, `rade_native_tx_times()`: first keyed block, first modem sample to WDSP (`ev_out0`, after the two-frame prefill), end of over asked for, end-of-over frame generated and its last sample handed to WDSP (`ev_eoo_gen`, `ev_eoo_out`), hold finished (`ev_done`), first unkeyed block; and the capture now writes `<stem>_txblocks.csv` (when each TX block reached the capture, to put `_tx.wav` on the clock). The harness reports them, with `t_mox_on` / `t_mox_off` (when it called key-up/-down), in `status`, `poll` and the `play` result. |
| `air_timing.py` | RF onset and end from the modem-band power of the GPS recording, the WDSP output times from `_tx.wav`, and the intervals between them, the engine events and the decoder (section 4). Called by `air_test.py`; `air_timing.py SESSION` recomputes a session. |
| `kiwi_time_check.py`, `kiwi_freq_check.py` | the checks of the Kiwi's time axis and frequency (section 2). |

## 2. Validating the Kiwi (measured 2026-10-16)

- **Frequency** (`kiwi_freq_check.py`, WWV carriers, time stations good to 1e-11): 10 MHz -0.22 Hz (-0.022 ppm), 15 MHz +0.38 Hz (+0.025 ppm). The Kiwi's
  ADC clock is GPS-corrected: the GPS-fitted sample rate is 2.9 ppm from the one its configured clock gives, and the carriers are right to +-0.03 ppm
  (**about +-0.9 Hz at 28.645 MHz**, plus the ionosphere's Doppler of a few tenths of a Hz). The 5 MHz try found only a weak signal (19 dB) and was not used.
  So the transmit frequency error read from the decoder's frequency offset (earlier runs: +11.2 Hz at 28645 kHz, +5.4 Hz at 14250 kHz: both +0.39 ppm) is the **radio's**,
  to about +-0.03 ppm; it is the same ppm on both bands.
- **Time axis** (`kiwi_time_check.py`, MSF Anthorn 60 kHz, whose carrier is off for the first 100 ms of every UTC second; ground wave about 280 km, 1 ms): the edges fall
  +3.1 ms and rise +103.3 ms (expected 0 and 100 plus 1 ms): **the Kiwi's stamps are late by about +2.2 ms** (two recordings, 25 s and 90 s: +2.1 and +2.2 ms; the mean of the two edges
  cancels the symmetric parts of the transmitter's edges and the filter). `air_timing.py` subtracts the figure in `out/kiwi_time_offset.json` from every time it takes from the recording.
  Uncertainty about +-2 ms. **A WWV 1 kHz tick test disagreed** (tick start about 11 ms after the UTC second where a skywave from Colorado cannot be earlier than about 25 ms); it was not
  understood and not used (the signal may not have been WWV: 10 MHz is shared), the MSF test is the reference.
- **PC clock** (`chronyc tracking`): synchronised to a LAN server, system time within 0.02 ms of it, RMS offset 7 us; the LAN server's own accuracy against UTC is not known but chrony says the root
  dispersion is 11 us. The engine's event times are on this clock, so they are good to about a millisecond; the engine's block size (1024 samples at 48 kHz = 21 ms) limits *when* an event is noticed, not the clock.
- **Detector** (`test_timing.py`, a synthetic recording in the Kiwi layout with a burst of known RF start/end and 25 ms ramps in noise): the fitted sample rate and the UTC time of sample 0 come back exactly; the onset
  and end at -10 dB come back within 2.5 ms and 1.5 ms (the 5 ms power smoothing); a WDSP-output capture with 2 ms of block-clock jitter is placed within 1 ms. In `make check`.

## 3. Running it

```
cd ~/sdr/bm-pihpsdr
python3 test/rade_native/kiwi_time_check.py                 # now and then: the Kiwi's time axis (needs MSF, any time of day)
python3 test/rade_native/air_test.py --freq 28645 --wave v1,v2 --levels=0,3 --repeat 3 --margin 14 --kiwi-gain 40
```
(`--gps auto` is the default; the harness must be armed as before. The GPS Kiwi connection also gives `iq-fixed.wav` in the stamped layout, which `air_weak.py` reads.) Restart piHPSDR on the new build first: the engine events and `_txblocks.csv` are in it.

## 4. The columns (`results.csv`; times are unix seconds on the UTC axis)

| Column | Meaning and what a good value looks like |
|---|---|
| `rf_onset_unix`, `rf_end_unix` (-10 dB), `rf_onset_m20_unix`, `rf_end_m20_unix` | RF start and end of the over in the Kiwi (modem-band power 10 dB / 20 dB under the steady level; Kiwi offset removed) |
| `rf_rise_ms`, `rf_fall_ms` | 10-90 % amplitude rise at the start and fall at the end: how the onset and the end are slewed (WDSP up-slew; the PA) |
| `engine_to_wdsp_out_ms` | first modem sample handed to WDSP (`ev_out0`) to the first modem energy in WDSP's output (`_tx.wav`): WDSP's own delay, **66 ms predicted offline** at the default 2048 taps (`rade-native-filter-lab.md`) |
| `wdsp_out_to_rf_onset_ms` | WDSP's output to RF on the air: the radio protocol, the radio, the Kiwi's path. The number the engine cannot influence |
| `tx_start_latency_ms` | `ev_out0` to RF onset: the two above |
| `key_to_rf_onset_ms`, `mox_on_to_rf_onset_ms` | the key-down (first keyed block / the harness's `radio_set_mox(1)`) to RF onset: includes the two-frame prefill (240 ms V1, 80 ms V2) |
| `wdsp_out_last_to_rf_end_ms` | the last modem energy out of WDSP to RF end. **Equal to `wdsp_out_to_rf_onset_ms` (within the jitter) = everything WDSP gave the radio went out. Shorter by tens of ms = the radio was released with samples still on their way: a clipped tail.** |
| `tail_after_eoo_ms` | RF end minus (`ev_eoo_out` + the start latency): >= 0 means the end-of-over frame went out whole, and how much further RF there was (the TX filter's ring-down, the hold) |
| `mox_off_after_wdsp_last_ms`, `unkey_after_wdsp_last_ms` | key-up called (harness) / first unkeyed block, after the last modem energy left WDSP: **negative = released before WDSP had finished** |
| `hold_after_eoo_ms` | the engine's hold after the last EOO sample went to WDSP (`ev_done - ev_eoo_out`): 0.4 s of it is `TX_TAIL` |
| `mox_off_to_rf_end_ms`, `unkey_to_rf_end_ms` | key-up to the end of the RF |
| `sync_after_onset_ms` | RF onset to librade's first sync (end of its block): how long the receiver needs to acquire |
| `eoo_decoded_after_rf_end_ms`, `eoo_decoded_vs_ev_eoo_out_ms` | when the Kiwi's decoder reported the end-of-over frame, against the RF end and the engine's hand-off |
| `fx_speech_start_clipped_ms`, `fx_speech_end_missing_ms`, `fx_rx_start_vs_speech_frames` | against the speech sent: how much of the start of the speech the decoded output does not contain (0 = none; frames are 10 ms), how much of the end; `rx_start_vs_speech_frames` >= 0 means the decoder's first frame was at or before the first speech frame |
| `fx_loss_first_250ms`, `fx_loss_first_1s`, `fx_loss_middle`, `fx_loss_last_1s` | feature loss of the aligned decode, untrimmed, by part of the over (compare the first and last with the middle) |
| `tx_freq_error_hz`, `tx_freq_error_ppm` | the decoder's frequency offset averaged over the over (Kiwi tuned to the dial, GPS-corrected: the radio's error, +-0.03 ppm) |
| `kiwi_time_offset_ms`, `kiwi_fs_hz`, `gps_solution_pct`, `gps_stamp_scatter_us`, `wdsp_block_clock_jitter_ms` | the correction applied, the fitted Kiwi sample rate, the share of blocks with a GPS solution (the flag is on in about half of blocks even with a lock, which is normal for the Kiwi), the stamps' scatter, the jitter of the engine's block clock |

## 5. What the first real run should settle (not yet measured)

1. **Is the pipeline cleared at the end of an over?** `wdsp_out_last_to_rf_end_ms` against `wdsp_out_to_rf_onset_ms`, `mox_off_after_wdsp_last_ms`, `tail_after_eoo_ms`, and the end-of-over frame decoding (`iq_eoo_ok`, `iq_call_ok`).
2. **Are starts clipped?** `fx_speech_start_clipped_ms`, `fx_loss_first_250ms` against the middle, `rf_rise_ms`, `sync_after_onset_ms`.
3. **Real TX delays**, split into engine -> WDSP (predicted 66 ms), WDSP -> RF, and the hold; per waveform.
4. **Transmit frequency** against the GPS-corrected Kiwi (+11 Hz = +0.39 ppm so far, from the decoder's estimate on the older, unstamped recordings).

## 6. Limits

- The receive-side turnaround (RX engine, WDSP RX 128 ms, the 200 / 100 ms play-out cushion) is not measured over the air: the Kiwi cannot hear piHPSDR's receiver, and the radio cannot transmit and receive at once. It is known from the offline numbers only.
- Timing accuracy about +-3 ms on the Kiwi side (detector bias and the +-2 ms of the offset check) and about +-5 ms from block-clock jitter on the WDSP-output side: not good enough to resolve single-sample effects, plenty for tens of milliseconds.
- The engine's events are noticed once per audio block (21 ms); `ev_out0` and the others are stamped at the block, so are early by up to a block.
- A transmit frequency measurement from the decoder's offset is only as good as librade's estimator (it moves by about 1 Hz between waveforms on the same transmit chain: V1 11.2 Hz, V2 11-12.6 Hz). A single-carrier test (tune) would be sharper; the harness does not key tune, by design.

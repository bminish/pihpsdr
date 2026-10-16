# RADE native in piHPSDR: findings to date

Branch `feature/rade-native`. This is the one-page-per-topic summary of what has been built and what the tests
say, with where the evidence is. Detail and tables are in the documents named in each section; the signal path
and the change log are in `rade-native-signal-paths.md` (keep that one current). Everything marked *measured* was
measured, on the air or offline, on the dates given; everything marked *not measured* was not.

## 1. What exists

| Part | Where | State |
|---|---|---|
| librade (V1, V2) built into piHPSDR, RX and TX engines on the WDSP I/Q chain | `src/rade_native.[ch]`, `tools/rade/build_rade.sh` (`make RADE_NATIVE=ON`) | works on the air, both directions |
| RADE window (waveform, Invert, TX, mic trim/AGC/drive, capture, parameter log, calls, test harness), assignable to a toolbar button | `src/rade_native_menu.c`, `MENU_RADE` | built; the GTK side of the newer parts is not covered by tests |
| V1 callsign in the end-of-over frame (freedv-gui's LDPC text) | `src/rade_eoo_text.c` | interoperates; V2 callsign text **not implemented** |
| RADE Reporter (FreeDV Reporter client, V1 / V2 / both) | `tools/rade-reporter/` | reports only to the reporter of the active mode, only for a RADE transmission |
| Parameter log (non-blocking, one file per run) | `src/rade_native_log.[ch]` | producer cost 0.3 us mean |
| Test harness (control socket, operator-armed, guarded) + automated air runs + Kiwi analysis | `src/rade_native_test.[ch]`, `test/rade_native/{rade_ctl,air_test,air_weak,kiwi_rx}.py` | run on the air twice (see 6) |
| Filter lab (WDSP TX/RX filter options, offline) | `test/rade_native/filter_lab.py` | see `rade-native-filter-lab.md` |

## 2. Geometry, sideband and the dial (measured offline; behaviour on the radio as used)

- The dial is the suppressed carrier of the sideband in use; the energy centre is `dial + 1500 Hz` (upper) or `dial - 1500`
  (lower) and **does not move** when the waveform, Invert or RADE changes: the dial moves instead, by 3 kHz when the sideband
  changes (`rade_native_plan()`, `test_geometry`, `modes_table`). V1 is lower sideband below 10 MHz, V2 upper. Switching RADE off
  hands back to USB or LSB on the same carrier. The Reporter is told the dial. (`rade-native-filtering.md`)
- V1's carriers are centred on 1475 Hz, V2's on 1500 Hz; WDSP's convention is the conjugate of librade's, handled at the two interfaces.

## 3. Transmit spectrum and the speech stages (measured)

- The SSB profile's leveller and CFC were being re-applied to the RADE signal: the energy centre read +1650 Hz against the modem's
  +1457 Hz with a 5 dB tilt. Guards in `tx_set_compressor/equalizer/phrot/dexp` keep them off while RADE TX is on. Every capture
  since (V1 at 14250 and 28645 kHz, V2 at 28645 kHz) passes `tx_shape.py`: centre within 20 Hz, tilt under 1 dB.
- **V2's own spectrum is not flat**: the modem's carriers fall away towards the edges (about -11 dB at the first carrier and -18 dB at
  the last against the middle), and that is librade's output, not the filter. librade's own V2 TX band-pass is on in the engine and does
  not change the spectrum measurably (0.1 dB).

## 4. Levels and the ALC (measured on the air, 28645 kHz, drive 20 %; `rade-native-air-run-20261016.md`, `rade-native-air-run2-20261016.md`)

- The ALC ceiling at the 192 kHz P2 output is **-0.97 dBFS (0.894)**, reached at the defined level (V1 0.357, V2 0.504 into WDSP; `drive_db` 0).
- Above it every dB of extra level is taken back: +3 dB gives +1.0 dB of received power (V1) and +0.4 dB (V2), +6 dB gives nothing; the crest
  factor falls (V1 4.7 -> 3.4 dB, V2 3.4 -> 2.8 dB).
- **Hitting the ALC costs nothing measurable**: strong signal band SNR never falls (V1 37.5 -> 38.5 dB, V2 41.0 -> 41.4); decode feature loss is
  the same from 0 to +6 dB; on emulated weak signals +1.5 to +6 dB are equal or better than 0 dB (+3 dB best by a small margin). V1's own SNR estimate falls 5 dB
  above the knee (clip distortion at about -26 dB) without a decode effect. Suggested working level +3 dB (**not applied**).
- Far-out regrowth before the PA: -127 dB at -12 dB level, -101 at 0, -80 at +3 and +6 (V1); V2 -130, -106, -89. The PA is not in these numbers.

## 5. Weak-signal thresholds (measured; white noise, one clip, one path)

| | sync holds to | end-of-over frame from | V1 callsign from |
|---|---|---|---|
| V1 | about 0 dB band SNR | about +1.7 dB | about +8.5 dB |
| V2 | about -1 dB | about +6 to +7 dB | (no V2 callsign text) |

Both hold sync at level -24 dB on the air at drive 1 %, and lose it at -30 dB. librade's V2 SNR estimate tops out near 18 dB (V1 near 32), so on strong
signals use the band-power SNR or the feature loss. The weak-signal emulation (`air_weak.py`) agrees with the real weak runs.

## 6. Test method and what the runs were

- Air run 1 (drive 1 %, 18 runs): level sweep -30..+6 dB, V1 and V2. Air run 2 (drive 20 %, 36 runs of the knee x2 repeats, 10 real weak runs) plus
  emulation of weak signals on the strong recordings. Harness, runner and analysis: `rade-native-air-tests.md`.
- The harness arms only when the operator ticks a box; limits are compile-time constants (drive 20 %, 10 m, 90 s). A client that goes away releases the transmitter.
- Faults found by the runs and fixed: capture started twice by the harness (harmless message); the parameter-log path; SIGPIPE on a closed client socket;
  an over-the-air delay figure that was wrong and was removed.

## 7. Timing (measured)

- Hold after the last speech sample before the transmitter is released: **0.65 s V1, 0.43-0.47 s V2** (0.4 s of it is the end-of-over frame and WDSP's TX filter tail).
- First speech sample within about 0.05 s of the key. No transmit underruns in 64 runs.
- WDSP filter delay, offline (`rade-native-filter-lab.md`): TX chain 66 ms and RX chain 128 ms at the default 2048 taps, of which the linear-phase filters are
  21 ms (TX, two of them, at 96 kHz) and 43 ms (RX, two of them, at 48 kHz). The over-the-air delay is **not measured** (the Kiwi recordings carry no time stamps).

- **GPS-stamped Kiwi** (2026-10-16): the Kiwi's frequency is right to +-0.03 ppm (WWV 10 / 15 MHz), its time axis late by about 2.2 ms (MSF 60 kHz). First run with it (`rade-native-air-run3-20261016.md`, 12 overs): engine -> WDSP output 78.8 ms, WDSP -> RF 10.9 ms, key-down to RF 335 ms (V1) / 174 ms (V2) of which 240 / 80 ms is the prefill; RF rise 20 ms; the end-of-over frame and call decoded in every over and nothing of the end is missing; the radio is released about 115 ms after the RF has ended (dead time); **the receiver loses the first 0.52 s (V1) / 0.21 s (V2) of the speech to acquisition** (sync 0.65 s / 0.27 s after the RF onset, at any SNR); the end-of-over frame is 2.4 dB (V1) below the speech's average power at 0 dB drive. The transmit frequency is within +-1 Hz of the GPS-corrected Kiwi: the +0.39 ppm seen earlier was the Kiwi's own, before its GPS (corrected).

## 8. Mic path (measured offline, `test_tx_level.py`)

- Trim, a slow leveller (0.2-0.5 dB/s up, 2 dB/s down for a shout; +20 / -30 dB limits; changes only while transmitting and only on speech above -35 LUFS, held through receive
  and between overs), a soft limiter, and a BS.1770 loudness meter against -20 LUFS. The injected test clip is at -20 LUFS but its peaks reach full scale, so the limiter acts in
  about half of the 100 ms blocks; no decode effect seen.

## 9. Reporter (tested against mock servers only; never published to a live service in tests)

- V1 -> qso.freedv.org, V2 -> reporter-radev2.k6aq.net (the host freedv-gui hard-codes; a view-only connection returned a station list). Show V1 / V2 / both is separate from where
  reports go; reports go only to the reporter of the active mode, and only for a RADE transmission (not tune, two-tone or SSB).

## 10. Open items

1. Filter optimisation (this document's sibling): see `rade-native-filter-lab.md` for the options measured and the recommendation; not applied until agreed.
2. A V2 callsign in the transmission (V2 carries it throughout the over, not in the end-of-over frame).
3. The over-the-air delay needs a time reference in the recording.
4. The PA and the radiated spectrum (Kiwi spectrum of the over versus the TX output) are not characterised; fading is not tested; one clip.
5. The working TX level (+3 dB) is a recommendation, not a change.
6. Pi CPU cost of the engines is unmeasured.

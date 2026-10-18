# RADE native in piHPSDR: findings to date

Branch `feature/rade-native`. This is the one-page-per-topic summary of what has been built and what the tests
say, with where the evidence is. Detail and tables are in the documents named in each section; the signal path
and the change log are in `signal-paths.md` (keep that one current). Everything marked *measured* was
measured, on the air or offline, on the dates given; everything marked *not measured* was not.

## 1. What exists

| Part | Where | State |
|---|---|---|
| librade (V1, V2) built into piHPSDR, RX and TX engines on the WDSP I/Q chain | `src/rade_native.[ch]`, `tools/rade/build_rade.sh` (`make RADE_NATIVE=ON`) | works on the air, both directions |
| RADE window (waveform, Invert, TX, mic trim/AGC/drive, capture, parameter log, calls, test harness), assignable to a toolbar button | `src/rade_native_menu.c`, `MENU_RADE` | built; the GTK side of the newer parts is not covered by tests |
| V1 callsign in the end-of-over frame (freedv-gui's LDPC text) | `src/rade_eoo_text.c` | interoperates; **V2 callsign on the data-symbol channel: `src/rade_v2_text.c`, sent with every V2 transmission, decoded by freedv-gui's receiver offline, and decoded on receive by a port of freedv-gui's receiver (same file); **sending validated on air 2026-10-17** (our transmission on 28645 kHz, recorded by a Kiwi SDR, decoded as `EI6IZ` by freedv-gui built for V2; `air-tests.md`); **receiving validated on air too, thinly: DF6VI decoded in 6 V2 overs on 5354 kHz, 2026-10-17 18:06-18:18 UTC (runs `20261017-190002` and `20261017-191431`; over lengths 57-94 s, mean SNR -2.9 to +3.7 dB, `call_snr` -7.1 to +8.9 dB)** |
| RADE Reporter (FreeDV Reporter client, V1 / V2 / both) | `tools/rade-reporter/` | reports only to the reporter of the active mode, only for a RADE transmission |
| Parameter log (non-blocking, one file per run) | `src/rade_native_log.[ch]` | producer cost 0.3 us mean |
| Test harness (control socket, operator-armed, guarded) + automated air runs + Kiwi analysis | `src/rade_native_test.[ch]`, `test/rade_native/{rade_ctl,air_test,air_weak,kiwi_rx}.py` | run on the air twice (see 6) |
| Filter lab (WDSP TX/RX filter options, offline) | `test/rade_native/filter_lab.py` | see `filter-lab.md` |

## 2. Geometry, sideband and the dial (measured offline; behaviour on the radio as used)

- The dial is the suppressed carrier of the sideband in use; the energy centre is `dial + 1500 Hz` (upper) or `dial - 1500`
  (lower) and **does not move** when the waveform, Invert or RADE changes: the dial moves instead, by 3 kHz when the sideband
  changes (`rade_native_plan()`, `test_geometry`, `modes_table`). V1 is lower sideband below 10 MHz, V2 upper; **on 60 m (5250-5450 kHz) USB is the convention, so both V1 and V2 default to upper sideband and Invert there means below the carrier, for both** (`RADE_NATIVE_60M_*`, `rade_native_band_class()`). Switching RADE off
  hands back to USB or LSB on the same carrier (V2 on 160/80/40 m: LSB, dial +3 kHz, energy kept). The Reporter is told the dial. (`filtering.md`)
- V1's carriers are centred on 1475 Hz, V2's on 1500 Hz; WDSP's convention is the conjugate of librade's, handled at the two interfaces.

## 3. Transmit spectrum and the speech stages (measured)

- The SSB profile's leveller and CFC were being re-applied to the RADE signal: the energy centre read +1650 Hz against the modem's
  +1457 Hz with a 5 dB tilt. Guards in `tx_set_compressor/equalizer/phrot/dexp` keep them off while RADE TX is on. Every capture
  since (V1 at 14250 and 28645 kHz, V2 at 28645 kHz) passes `tx_shape.py`: centre within 20 Hz, tilt under 1 dB.
- **V2's own spectrum is not flat**: the modem's carriers fall away towards the edges (about -11 dB at the first carrier and -18 dB at
  the last against the middle), and that is librade's output, not the filter. librade's own V2 TX band-pass is on in the engine and does
  not change the spectrum measurably (0.1 dB).

## 4. Levels and the ALC (measured on the air, 28645 kHz, drive 20 %; `air-run-1.md`, `air-run-2.md`)

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
| V2 | about -1 dB | about +6 to +7 dB | (V2 callsign rides the data symbols through the over, not the end-of-over frame; receive threshold not measured, decoded in the engine test at 6 dB SNR) |

Both hold sync at level -24 dB on the air at drive 1 %, and lose it at -30 dB. librade's V2 SNR estimate tops out near 18 dB (V1 near 32), so on strong
signals use the band-power SNR or the feature loss. The weak-signal emulation (`air_weak.py`) agrees with the real weak runs.

## 6. Test method and what the runs were

- Air run 1 (drive 1 %, 18 runs): level sweep -30..+6 dB, V1 and V2. Air run 2 (drive 20 %, 36 runs of the knee x2 repeats, 10 real weak runs) plus
  emulation of weak signals on the strong recordings. Harness, runner and analysis: `air-tests.md`.
- The harness arms only when the operator ticks a box; limits are compile-time constants (drive 20 %, 10 m, 90 s). A client that goes away releases the transmitter.
- Faults found by the runs and fixed: capture started twice by the harness (harmless message); the parameter-log path; SIGPIPE on a closed client socket;
  an over-the-air delay figure that was wrong and was removed.

## 7. Timing (measured)

- Hold after the last speech sample before the transmitter is released: **0.65 s V1, 0.43-0.47 s V2** (0.4 s of it is the end-of-over frame and WDSP's TX filter tail).
- First speech sample within about 0.05 s of the key. No transmit underruns in 64 runs.
- WDSP filter delay, offline (`filter-lab.md`): TX chain 66 ms and RX chain 128 ms at the default 2048 taps (RX with `bp1` forced on; the radio runs one RX filter, 106.6 ms), of which the linear-phase filters are
  21 ms (TX, two of them, at 96 kHz) and 21 ms (RX live, one at 48 kHz; 43 ms with two). The over-the-air delay is **not measured** (the Kiwi recordings carry no time stamps).

- **GPS-stamped Kiwi** (2026-10-16): the Kiwi's frequency is right to +-0.03 ppm (WWV 10 / 15 MHz), its time axis late by about 2.2 ms (MSF 60 kHz). First run with it (`air-run-3.md`, 12 overs): engine -> WDSP output 78.8 ms, WDSP -> RF 10.9 ms, key-down to RF 335 ms (V1) / 174 ms (V2) of which 240 / 80 ms is the prefill; RF rise 20 ms; the end-of-over frame and call decoded in every over and nothing of the end is missing; the radio is released about 115 ms after the RF has ended (dead time); **the receiver loses the first 0.52 s (V1) / 0.21 s (V2) of the speech to acquisition** (sync 0.65 s / 0.27 s after the RF onset, at any SNR); the end-of-over frame is 2.4 dB (V1) below the speech's average power at 0 dB drive. The transmit frequency is within +-1 Hz of the GPS-corrected Kiwi: the +0.39 ppm seen earlier was the Kiwi's own, before its GPS (corrected).

## 8. Mic path (measured offline, `test_tx_level.py`)

- Trim, a slow leveller (0.2-0.5 dB/s up, 2 dB/s down for a shout; +20 / -30 dB limits; changes only while transmitting and only on speech above -35 LUFS, held through receive
  and between overs), a soft limiter, and a BS.1770 loudness meter against -20 LUFS. The injected test clip is at -20 LUFS but its peaks reach full scale, so the limiter acts in
  about half of the 100 ms blocks; no decode effect seen.

## 9. Reporter (tested against mock servers only; never published to a live service in tests)

- V1 -> qso.freedv.org, V2 -> reporter-radev2.k6aq.net (the host freedv-gui hard-codes; a view-only connection returned a station list). Show V1 / V2 / both is separate from where
  reports go; reports go only to the reporter of the active mode, and only for a RADE transmission (not tune, two-tone or SSB).

## 10. Open items

1. Filter optimisation (this document's sibling): see `filter-lab.md` for the options measured and the recommendation; not applied until agreed.
2. V2 callsign: **transmit and receive built 2026-10-16** (`rade_v2_text.c`; tests `v2_text_interop` part D = our receiver identical to freedv-gui's symbol by symbol, `test_v2_text.py` = real TX and RX engines); **sending validated on air 2026-10-17**: our transmission, recorded by a Kiwi SDR, decoded by freedv-gui built for V2 (two overs at -12 / -6 dB, `EI6IZ` on all Kiwi profiles and in the freedv-gui GUI); **receive on air**: DF6VI decoded in 6 V2 overs on 5354 kHz, 2026-10-17 18:06-18:18 UTC (runs `20261017-190002` and `20261017-191431`; over lengths 57-94 s, mean SNR -2.9 to +3.7 dB, `call_snr` -7.1 to +8.9 dB) - DF6VI was running freedv-gui; the V2 overs without a call were from stations running an experimental Thetis build (per the operator), which says nothing about our receiver. One freedv-gui station, so no threshold yet.
3. The over-the-air delay needs a time reference in the recording.
4. The PA and the radiated spectrum (Kiwi spectrum of the over versus the TX output) are not characterised; fading is not tested; one clip.
5. The working TX level (+3 dB) is a recommendation, not a change.
6. Pi CPU cost of the engines is unmeasured.
7. **Reporter tuning (2026-10-17), not yet tried against a radio**: `command_poll()` in `rade_native_menu.c` (click a station's MHz, kHz box, per-mode list) is compiled, and the Reporter's side is tested (`test_gui_tune.py`), but a real mode change and dial set through `command.json` has not been seen. To check: click a V1 and a V2 station from the other mode and see the RADE window follow; a request while transmitting is refused; one left in the file before piHPSDR starts is ignored.
8. **TX state at the end of a TUNE (2026-10-17), not yet tried on a radio**: the two-poll confirmation in `state_cb` (a `not_rade` latch in `transmitter.c` was tried and removed: RADE had no RF with it). To check: TUNE for a few seconds during Reporting; the Reporter must send no `tx_report` and no TX pulse, at the start or the end. Then a real RADE over must still be reported (about 0.25 s late).
9. Plain `make` does not compile any of the RADE code (`make RADE_NATIVE=ON` does): a change here has only been built once it has been built with that.

## Frequency offsets in practice (2026-10-18, 268 overs of 10 s or more in `rade-logs/`)

Measured from `rx.csv` / `overs.csv`: **inside an over the offset hardly moves** (median standard deviation 0.17 Hz V1, 0.73 Hz V2; median drift 0.5 Hz/min V1, 1.5 Hz/min V2; the larger values are weak or fading overs). **Between stations it differs by a constant**: median |mean| 1.2 Hz (V1) and 0.6 Hz (V2), but a station keeps its own figure over a whole QSO - F5DB -5.3 Hz, DK1WP -4.3, G4MKT -14 (5368.5 kHz round table, against G6WPJ about 0), VE3OWV +7.5, PE4WJ +30 - so in a round table the offset steps by 5 to 20 Hz from one over to the next. That is what the Track button follows (`rade_native_track.c`: a mean over the lock, the RIT moving at most 3 Hz/s early and 0.2 Hz/s late, zero at the end of an over). Replayed over three runs with the RIT at zero in the logs, the spread of the offset fell from 7.6 to 4.8 Hz, 6.2 to 1.6 Hz and 2.5 to 1.8 Hz; what remains is the slew at the start of each over (a 14 Hz step takes 5 s). **Tried on the air 2026-10-18: it worked** (the sign is right: the offset shrinks with Track on). The RIT stops at +-50 Hz. Test: `test/rade_native/test_track.py [run directory]`.

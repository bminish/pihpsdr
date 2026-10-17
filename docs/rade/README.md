# RADE in piHPSDR: what was built, what it touches, why

Branch `feature/rade-native`. Build with `make RADE_NATIVE=ON` (off by default; with it off nothing here is compiled).
Details live in the documents listed at the end; this page is the map. Last checked against the code 2026-10-17 (Reporter tuning and the TX latch added; log tidy-up).

## What it does

piHPSDR receives and transmits FreeDV **RADE V1 and V2** itself, in I/Q, with David Rowe's `librade`
(`third_party/rade_c`, built by `tools/rade/build_rade.sh`). No freedv-gui, no audio loopback, no second program in the
audio path. The operator chooses **Off / V1 / V2** and **Invert** in the RADE window (also assignable to a toolbar button).

## How it fits in

```
radio I/Q -> WDSP RXA (shift, RADE passband, AGC) -> RX engine -> speech -> normal audio output
mic       -> TX engine -> WDSP TXA (RADE passband, ALC) -> radio I/Q
```

The engines sit *on* the WDSP chain, at 48 kHz complex, using only WDSP's public calls. WDSP is not modified. Inside an
engine: decimate to 8 kHz (the modem rate), `librade`, and FARGAN (receive) or LPCNet (transmit) for the speech at 16 kHz.
Each stage, rate and level: [signal-paths.md](signal-paths.md).

**Why this way.** Taking the audio chain's own I/Q keeps AGC, shift, filters, ALC, the panadapter and the radio's
timing working as they do for any mode, and avoids a sound-card hop that costs delay and a sample-rate mismatch.

## Files

| Where | What | Size |
|---|---|---|
| `src/rade_native.[ch]` | the engines: receive worker, transmit worker, mic level (trim, leveller, limiter, loudness), geometry (sideband, centre, dial), end-of-over hold | core |
| `src/rade_native_menu.[ch]` | the RADE window; state file for the Reporter and the Reporter's command file (mode and dial requests); saved settings | core |
| `src/rade_eoo_text.[ch]`, `rade_eoo_text_hra.h` | V1 callsign in the end-of-over frame (freedv-gui's LDPC text) | core |
| `src/rade_v2_text.[ch]` | V2 callsign on the data-symbol channel, send and receive, ported from freedv-gui | core |
| `src/rade_native_log.[ch]` | non-blocking parameter log, one directory per run | core |
| `src/rade_native_test.[ch]` | test harness (control socket); armed by the operator, limited in power, band and time | core |
| `src/rade_launch.[ch]` | starts the Reporter / log viewer detached | core |
| `rade_native.mk` | the build switch, objects, libraries | core |
| `tools/rade-reporter/` | FreeDV Reporter client (V1 and V2), a separate Python process reading the state file | extra |
| `tools/rade/trim_capture.py` | cuts a stretch (or the best N seconds in sync) out of a capture in `rade-captures/`, to keep it as a sample; the originals are left alone | extra |
| `tools/rade-logview/` | viewer for the parameter logs; `tidy_logs.py` finishes (compresses, removes) runs left unfinished | extra |
| `test/rade_native/`, `test/rade_reporter/`, `test/rade_logview/` | offline tests, over-the-air test runner, analysis | extra |

(`src/rade_correlator.c` is the diversity work's V1 pilot correlator, not part of this; see `docs/archive/diversity/`.)

## What it touches in upstream files

Only guarded by `#ifdef RADE_NATIVE`, and each is a call into `src/rade_native*.c` or a guard; about 170 lines in all.

| File | Change | Why |
|---|---|---|
| `receiver.c` `rx_full_buffer` | WDSP's audio output replaced by the engine's speech; `rx_set_filter` sets the RADE passband; `rx_set_af_binaural` keeps I and Q apart | the engine needs the complex signal, and must be the audio source |
| `transmitter.c` `tx_full_buffer` | mic block replaced by the modem signal; TX capture; a latch (`not_rade`) that holds a key-down as "not RADE" once TUNE, two-tone or noise has been seen in it, until the radio unkeys | the engine is the signal source; TUNE ends by clearing its flag while the radio is still keyed, which looked like a RADE transmission to the Reporter |
| `transmitter.c` `tx_set_filter`, `tx_set_compressor/equalizer/phrot/dexp` | RADE passband; speech processors held off while RADE transmit is on | the SSB profile's leveller and CFC were re-shaping the modem signal (energy centre +1650 Hz against +1457, 5 dB tilt); also re-enabled after TUNE |
| `radio.c` `radio_set_mox` | the end of an over waits for the end-of-over frame | otherwise the callsign frame is cut off |
| `radio.c` load/save | one call each into `rade_native_restore_state()` / `rade_native_save_state()` | callsign, Invert, trim, leveller, drive persist |
| `new_menu.c`, `actions.c` (+ `.h`) | menu button, `MENU_RADE` action | the window and toolbar assignment |
| `Makefile` | three lines: `include rade_native.mk`, `$(RADE_NATIVE_LIBS)`, `$(RADE_NATIVE_OBJS)` | keeps the diff against upstream minimal |

Keeping these small and rebasing onto upstream: [upstreaming.md](upstreaming.md) (`tools/rade/rade-patches.sh check`).

## Design decisions, and the evidence behind them

- **The dial is the carrier of the sideband in use; the modem's energy stays put.** The energy centre is `dial ± 1500 Hz`
  and does not move when waveform or Invert changes; the dial moves instead (by 3 kHz across a sideband change).
  V1 is lower sideband below 10 MHz, V2 upper, both upper on 60 m. The sideband is stored when chosen, not derived from
  the dial. Everything the Reporter and logs say about frequency is this dial. Test: `test_geometry`, `modes_table`.
- **WDSP and librade disagree on the sign of I/Q**, so the engine conjugates at the two interfaces, unless Invert.
- **Transmit level is fixed where the ALC just does not act** (V1 0.357, V2 0.504 into WDSP). Above that the ALC takes
  back every dB; hitting it costs no decode performance but nothing is gained. A +3 dB working level is recommended, not
  applied. Tests: `air_test.py`, `alc_sweep.py`, [air-run-1](air-run-1.md), [air-run-2](air-run-2.md).
- **TX spectrum is checked, not assumed.** `tx_shape.py` on every capture: centre within 20 Hz, tilt under 1 dB.
- **Mic level** is a slow, gated leveller to -20 LUFS with a soft limiter, because the modem wants a steady level and the
  operator wants a reading in dB above/below ideal. Test: `test_tx_level.py`.
- **End of over** holds the radio about 0.65 s (V1) / 0.45 s (V2): the end-of-over frame (callsign) plus WDSP's filter tail.
  Test: [air-run-3](air-run-3.md), GPS-timed Kiwi recordings ([gps-timing.md](gps-timing.md)).
- **Callsigns.** V1: LDPC text in the end-of-over frame, sent at the end of every over. V2: one BPSK data symbol per
  modem frame carries a repeating LDPC-coded call through the whole over, so the receiver can combine repetitions. Both
  follow freedv-gui bit for bit. V2 receive uses **sync as the gate**: symbols are only taken while `librade` is in sync;
  after a loss the new lock is joined to the old reception only if its timing correlates with the call's repetition cycle
  (a dropout), otherwise it starts afresh (a different station). Tests: `v2_text_interop` (our code against freedv-gui's
  unmodified code, symbol by symbol), `test_v2_text.py` (real engines). Checked on air, our transmission -> freedv-gui's receiver (2026-10-17, 28645 kHz, Kiwi recording, also in the freedv-gui GUI); freedv-gui's transmission -> our receiver graded in noise against freedv-gui's receiver, equal ([air-tests.md](air-tests.md)); not yet on air.
- **Only a real RADE signal counts as a transmission.** The Reporter is told "transmitting" only while the engine has had
  the key for speech for 170 ms (`rade_native_tx_live()`), never for TUNE, two-tone or noise. Three guards, because the key, the
  TUNE flag and the engine's count change at different moments: (1) the engine is given "keyed for RADE" only if no TUNE, two-tone or
  noise has been seen since the key went down (`not_rade` in `tx_full_buffer`; **the end of a TUNE clears its flag before the radio
  unkeys**, which is the case this was added for); (2) `rade_is_tx()` also needs the engine's 170 ms; (3) `state_cb` reports it
  only after two 250 ms polls in a row, and counts `tx_count` (the short TX pulse) from that. Test: `test_tx_live` (guards 1 and 2); guard 3 and the latch are **not yet tested**, on the bench or in a test.
- **Reporter** is a separate process (so a network fault cannot touch the audio), driven by `state.json`; V1 reports to
  qso.freedv.org, V2 to the radev2 reporter. It reports only to the reporter of the active mode. Tests use mock servers
  only. [tools/rade-reporter/README.md](../../tools/rade-reporter/README.md).
- **The Reporter can tune piHPSDR** (click a station's MHz, a kHz box, a per-mode frequency list kept in `reporter.json` and edited in Settings > Frequencies).
  It is a second file next to `state.json`, `command.json` (`{"seq","wave","dial_hz"}`), because piHPSDR has no port for it and the
  file keeps the Reporter a separate process. piHPSDR polls it with the 250 ms state timer, acts once per `seq`, ignores a request over 10 s old
  (and whatever was in the file when it started), refuses while transmitting, sets the mode first (`waveform_cb`'s `retune`, which can move the dial 3 kHz for Invert) and
  then the exact dial, and answers in `state.json` (`cmd_seq`, `cmd_err`). Changing mode needs the RADE window open (its buttons are what is driven). Test: `test/rade_reporter/test_gui_tune.py` (the Reporter's side only;
  piHPSDR's side, `command_poll()`, has been **compiled but not yet run against a radio**).
- **Logging never blocks the engines** (0.3 µs per row). One directory per run with `meta.txt`, `rx.csv`, `tx.csv`, `events.csv`, `overs.csv`, `tx_overs.csv`;
  the viewer reads them live. A run stopped cleanly is compressed by piHPSDR; one that ended any other way is finished by `tools/rade-logview/tidy_logs.py`,
  which the viewer runs at start (every run directory but the newest). [logging.md](logging.md), [tools/rade-logview/README.md](../../tools/rade-logview/README.md).
- **Weak signals.** Sync holds to about 0 dB band SNR (V1) / -1 dB (V2); the end-of-over frame needs about +2 / +6 dB; the
  V1 callsign about +8.5 dB. Test: `air_weak.py`. Numbers and conditions: [findings.md](findings.md).

## Not done / not known

- V2 callsign over the air with freedv-gui transmitting and our receiver decoding (done in simulation from freedv-gui's own transmission, not yet on air).
- Over-the-air delay end to end, PA and radiated spectrum, fading, Pi CPU cost of the engines (PC: V2 text receive 0.5-0.9 % of a core).
- Working TX level (+3 dB) and the filter recommendations in [filter-lab.md](filter-lab.md) are proposals.

## Documents

| Document | Use |
|---|---|
| [signal-paths.md](signal-paths.md) | every stage, rate, level, and where each displayed/reported number comes from; **keep current** |
| [findings.md](findings.md) | measured results, thresholds, open items |
| [upstreaming.md](upstreaming.md) | patch layers and the upstream check |
| [logging.md](logging.md) | what the parameter log holds |
| [filtering.md](filtering.md), [filter-lab.md](filter-lab.md) | passband and WDSP filter measurements |
| [wdsp-review.md](wdsp-review.md) | how we use WDSP checked against the WDSP Guide 2.1.1: `dsp_size`, the single RX filter, AGC, TX chain |
| [air-tests.md](air-tests.md), [air-run-1](air-run-1.md), [air-run-2](air-run-2.md), [air-run-3](air-run-3.md), [gps-timing.md](gps-timing.md) | over-the-air test method and the three runs |
| [../archive/rade/](../archive/rade/) | the original plan and the day-by-day progress log (history, not maintained) |

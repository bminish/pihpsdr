# RADE native: progress

Plan: [rade-native.md](rade-native.md). Branch `feature/rade-native`.
Updated with every commit that moves the work. Newest entry first.

## Status

| Phase | State |
|---|---|
| 0 Build/link `librade`, T-0, T-1, T-11 | done: librade builds, T-0, T-1, T-11, `RADE_NATIVE` make option |
| 1 Harness | started: T-11b chain test works; T-2 channels next |
| 2 RX | first working version: engine + menu + receiver hooks, offline-tested; needs an on-air capture |
| 3 Tuning, SNR, logging | not started |
| 4 TX | first working version: engine + menu toggle + TXA hooks + TX capture; validated offline against librade's own tools; needs on-air test |
| 5 Extras | not started |

## Log

- 2026-10-15 (Kiwi receive harness): `test/rade_native/kiwi_rx.py` records
  our transmissions off air from the KiwiSDR at `kiwi.minish.org:8074`
  (connection details as given; no password needed) and checks them.
  **Several connections at once work**: three in parallel were run
  against it (the status page says `users_max=1`, but 3 connections were
  accepted). Profiles, one `kiwiclient/kiwirecorder.py` each: `iq` (12 kHz
  complex, never compressed, Kiwi AGC), `iq-fixed` (IQ with the AGC off,
  `--gain`), `usb` / `lsb` (audio, `--ncomp`, uncompressed). **Kiwi IQ
  convention measured**: tuned 2 kHz below WWV on 5, 10 and 15 MHz, the
  carrier appears at +2000 Hz, i.e. positive frequency is above the tuned
  frequency (I + jQ), the same as librade; so an inverted signal (V1
  below 10 MHz) is conjugated, a normal one is not. Commands:
  `kiwi_rx.py record|analyze|run|selftest` (`--help` for the options:
  `--freq` the dial in kHz, `--wave v1|v2`, `--duration`, `--call`,
  `--ref speech.wav`, `--tx-freq` if the Kiwi is not on the dial,
  `--min-snr`, `--max-loss`). Per over it reports sync %, SNR (librade's
  estimate, dB in 3 kHz) mean/min/max, frequency offset, the EOO frame
  (how many, where, how long after sync ended), the V1 callsign decoded
  from it against `--call`, and with `--ref` the vocoder-feature loss and
  the speech-envelope correlation against the ideal decode. Output:
  `rade-captures/kiwi/<utc>_<kHz>_<wave>/` (`<profile>.wav` as recorded,
  8 kHz baseband, stats, events, decoded speech, `session.json`,
  `analysis.json`, `report.txt`). Decoder: `rade_decode_iq` (librade +
  FARGAN, writes the stats, EOO events, features and speech).
  **Checked without a transmitter** (`kiwi_rx.py selftest`: our TX engine's
  over with call EI6IZ, dressed as a Kiwi recording, noise added, Kiwi
  tuned off the dial): V1 inverted at 7053 and normal at 14236, V2 at
  7053 and 14236, IQ and audio profiles, all PASS at 12-20 dB SNR; a
  Kiwi tuned 0.3, 1.0 and 2.5 kHz away from the dial is handled. **Where
  the V1 callsign gives out: the speech stays in sync down to at least 2 dB
  but the EOO call decodes only from about 10 dB** (librade SNR estimate;
  tests at 8-10 dB injected: EOO flagged, call not decoded; 11 dB and
  above: decoded), because the EOO is one 144 ms frame. Against the
  real Kiwi: recorded 3 profiles in parallel for 10 s at 7053 kHz, no
  transmission on, correctly reported "no over found". **Not yet done:
  an actual transmission through it**; and the audio-quality figures need
  the transmitted speech to be known (`--ref`), which means feeding a known
  wav to the transmitter, which the RADE window cannot do yet (it takes the
  mic): a test-wav source in the TX path is the next piece for that.

- 2026-10-15 (RADE window as a working panel): from on-air use: (1) the
  "Transmit RADE" toggle only switched RADE transmit processing on, it did
  not key the radio. Now RADE transmit follows the waveform (V1/V2 selected
  = transmit is RADE as well; a "RADE TX" check box turns that off), and
  the window has a real **Transmit** button, which keys the radio the way
  the space bar does; the **space bar in the RADE window** does the same
  (also with the callsign box focused: it never contains a space). Both
  refuse unless RADE transmit is on, so they cannot send ordinary speech by
  mistake; the space bar in the main window is unchanged and, with RADE
  transmit on, sends RADE. The waveform, Invert and RADE TX controls are
  greyed while transmitting. (2) The window is no longer one of the
  numbered menus: it is not `sub_menu`, so opening the main menu or any
  other menu does not close it; it stays until the Close button or the
  window's x. It is transient for the main window and kept above it, and
  placed in its lower right corner (a window manager that refuses
  placement, e.g. Wayland, ignores that; drag it). Opening the RADE button
  again raises it. **Not exercised in the running GUI** (it needs a radio
  connected); built clean with no warnings.

- 2026-10-15 (end of over, callsign, call log): 
  **End of over / TX hold.** `radio_set_mox(0)` now asks the TX engine
  (`rade_native_tx_end_request()`); while RADE TX is on it keeps the radio
  keyed, lets the engine finish the speech in flight, pad the V1/V2 modem
  frame with silence, send the EOO frame (with the callsign, V1) and wait
  for WDSP's TX filters to put it out (8000 samples = 167 ms), and then
  releases (`rade_native_tx_end_state() == 2`, polled every 20 ms from the
  GTK loop in `radio.c`). Keying again meanwhile cancels the hold. Hold
  measured offline: 576-619 ms for V1, 384 ms for V2, after the last mic
  sample. Since the EOO was not reaching the receiver reliably in testing,
  the TX output now starts only once two modem frames are queued (a hole
  of zeros in the middle of the signal, from a late worker, made the EOO
  fail to decode intermittently; with the prefill: 0 underruns and 12 of 12
  V1 runs decode the call; before it, 2 in 5 batches failed one clip).
  **Callsign.** The RADE window has a "My call" box (7 characters,
  upper-cased, letters / digits / '/'), saved in the props file
  (`rade_native.callsign`, also `rade_native.invert`). When it is not
  empty it goes into the EOO frame. This is the V1 format freedv-gui uses
  (LDPC(112,56) over 56 QPSK symbols, CRC-8, 6-bit characters, interleaved;
  `src/rade_eoo_text.c`, a C port of freedv-gui's `rade_text.cpp` +
  `ldpc_*.cpp`, BSD terms kept in the file). **Interop was tested against
  freedv-gui's own code** (`make -C test/rade_native text_interop`,
  built unmodified): the two encoders write identical symbols; each decodes
  the other's, 7/7 clean and 139-140 of 140 at noise sigma 0.25 and 0.45.
  **Not done: V2 callsign text.** V2 carries text in a different channel (a
  stream of data symbols, framed by freedv-gui's newer `rade_text` with
  blocks, CRC and LDPC); the box says so when V2 is selected and sends
  nothing. **Receive.** Calls decoded from the EOO of a received V1 over
  appear in the RADE window (last six, newest first) and are appended to
  `rade-captures/rade-calls.csv`: utc, local, callsign, waveform, dial_hz,
  mode, effective_invert, snr_db (smoothed over the over, before it ended),
  foff_hz, rx_filter_low/high, agc, volume_db, adc, attenuation, preamp,
  and the capture stem when a capture was running (the same line goes into
  its `events.log`). Dial and mode are read up to 0.5 s after the decode.

- 2026-10-15 (TX, Phase 4 first cut): `rade_native.c` has the transmit
  engine: mic (I slot of WDSP's TX input buffer, 48 kHz) → 36-tap
  decimator to 16 kHz → `lpcnet_compute_single_frame_features` →
  `rade_tx` on a worker thread (own `struct rade`, own LPCNet state) →
  48-tap polyphase interpolator to 48 kHz → level 0.3 → conj unless
  invert → back into `tx->mic_input_buffer` as I and Q. WDSP side
  (`rade_native_menu.c`, "Transmit RADE" toggle): the existing
  `tx_xmit_captured_data_start()` state (EQ, leveler, CFC, compressor,
  DEXP off, panel gain 1), `SetTXAPanelSelect(3)`, and the TX bandpass
  from `tx_set_filter()` taking the RADE edges. Hooks in `transmitter.c`
  (`#ifdef RADE_NATIVE`): `tx_full_buffer` (substitute before `xdexp`/
  `fexchange0`, capture after), `tx_set_filter`, `tx_set_mic_gain` (the
  slider does not scale the modem). Not allowed with PureSignal. Capture
  now also writes `_tx.wav` (the TXA output block, float32 stereo at the TX
  IQ rate) and `_txmic.wav` (16 kHz speech into the vocoder).
  **Known gaps:** (the EOO gap that was listed here is closed, see above)
  only tested at 48 kHz TX IQ (P1 rate; P2 runs WDSP at 96 kHz and
  resamples to 192 kHz, not exercised); the mic gain slider is ignored
  while RADE TX is on; the level is fixed (0.3 into WDSP, ALC idle,
  drive sets power) and uncalibrated against power; no VOX/PTT-timing
  work; the TX filter edges are the nominal 1.6 / 1.0 kHz.
  Also still open from the capture work: no script yet that plots
  `stats.csv` (SNR / sync / offset) against `speech.wav` energy.

- 2026-10-15 (capture): the RADE menu has a **Capture** toggle. It writes
  to `rade-captures/` (created in the working directory, git-ignored):
  `<date>-<time>_<kHz>kHz_RADE-V<n>[-inv]_` + `rx48.wav` (float32 stereo
  48 kHz, L=I R=Q exactly as WDSP delivered it, includes the RX volume,
  WDSP sign convention), `speech.wav` (decoded speech as played, 48 kHz
  mono), `stats.csv` (one row per `rade_rx` call: sync, SNR, offset,
  ring levels, underruns, V2 per-symbol stats; `rx48_pos` indexes into
  `rx48.wav`), `events.log` (a line whenever dial, ctun, mode, filter,
  RADE settings, AGC, volume, NB/NR/SNB/ANF, ADC att/preamp, or diversity
  state changes), and `meta.txt` (the same, in full, at start and at the
  end, plus build commit and duration). WAV headers are rewritten every
  200 ms so a crashed capture is still readable. About 29 MB a minute.
  Replay a capture offline, through the same engine:
  `test/rade_native/test_rx_engine [--v2] [--invert] [--speed N] \
  <stem>_rx48.wav out.wav` (use `--invert` if meta.txt says
  `rade_effective_invert: 1`). Offline check: the recorded `rx48.wav` is
  bit-identical to the input, no dropped data, replay decodes.

- 2026-10-15 (RX engine, Phase 2 first cut): `rade_native.c` now has the
  receive engine: WDSP output (panel binaural = complex, 48 kHz) → conj
  unless invert → 48-tap decimator to 8 kHz → slow level normaliser →
  ring → worker thread (`rade_rx` → FARGAN, with the 5-frame `fargan_cont`
  warm-up as `rade_rx_wav` does) → 16 kHz speech ring → 3:1 interpolator →
  replaces the block in `rx->audio_output_buffer`, at the RX volume.
  Hooks (all `#ifdef RADE_NATIVE`): `rx_set_filter` (passband from the
  waveform and effective invert), `rx_set_af_binaural` (forced on),
  `rx_full_buffer` (calls the engine before `rx_process_buffer`),
  `new_menu.c` (RADE button in the RX column), `rade_native_menu.c`
  (Off / V1 / V2, Invert, edges, sync, SNR, offset, underruns).
  AGC is left as the operator set it (medium by default); `loss_check.sh`
  passes with medium AGC for V1 and V2. Receiver 0 only, state not saved
  in props, no TX yet.
  **How to try it:** `tools/rade/build_rade.sh` once, then
  `make RADE_NATIVE=ON`, run `./pihpsdr`, set receiver 0 to DIGU, open
  the RADE button, pick V1 or V2 (Invert to flip the default).

- 2026-10-15 (make option): `make RADE_NATIVE=ON` compiles `src/rade_native.c`
  and links `librade` (rpath into `third_party/rade_c-build`); default is
  OFF and the default build is unchanged and still builds. `rade_native.[ch]`
  holds only engine-independent code so far: waveform centres and widths,
  default/effective Invert (V1 below 10 MHz inverted, V2 never, user
  Invert XORed), WDSP passband edges (negative for inverted), the
  conjugation rule, and a probe that opens V1 and V2 contexts. Not called
  from the engine yet. `make -C test/rade_native check` tests it
  (librade 2.2 reported). A gotcha worth keeping: librade's headers include
  a `nnet.h` that clashes with `wdsp/nnet.h`, so only the `rade_native.o`
  object gets librade's include path.

- 2026-10-15 (T-11): `wdsp_probe.c` + `t11_wdsp_probe.py` run tones through
  the vendored WDSP RXA/TXA channels set up as piHPSDR does it. Results
  below. Needed: `WDSPwisdom()` (without wisdom FFT planning takes
  minutes), `dsp_size = 2 * buffer_size`, `RXASetPassband` (not
  `SetRXABandpassFreqs` alone: it leaves `nbp0` at its default and the
  output is ~ -130 dB).

- 2026-10-15 (Phase 0): `tools/rade/build_rade.sh` builds `librade`
  from `freedv/rade_c` pinned at `b3e3748` into `third_party/rade_c-*`
  (untracked; the old `third_party/rade_c` held only a stale build with
  no sources and is left alone). Harness in `test/rade_native/`:
  `t0_reference.py`, `rade_tx_iq.c`, `t1_tx_spectrum.py`. Results below.

- 2026-10-15: branch created from `TEST` (0c871087). Plan written and
  reworked around the WDSP I/Q chain. Diversity docs moved to
  `docs/archive/diversity/`. Phase 0 starting.

## Open questions (from the plan)

- RX panel `copy` value piHPSDR leaves for SSB (T-11).
- Does TXA pass a complex input unchanged (T-11)?
- Is `rade_tx` output one-sided (T-1)?
- ALC / overshoot control / RX AGC on OFDM (T-9, T-10).

## Findings

All from `test/rade_native/`, librade `b3e3748`. Measured unless marked.

**T-0, reference decode of `~/sdr/RadeV2_clips` (V2, `rade_rx_wav`)**

| clip | s | sync % | SNR dB | f-off Hz (mean, sd) |
|---|---|---|---|---|
| FDV_FromRadio ..EI6IZ_V2 | 64.5 | 97.1 | 15.6 | 2.6, 3.5 |
| g3sdr 21:37:53 | 20.5 | 81.3 | 6.5 | 0.6, 0.5 |
| g3sdr 21:39:55 | 27.5 | 84.6 | 4.4 | -1.3, 0.1 |
| g3sdr 21:43:05 | 80.6 | 82.9 | -0.9 | -1.5, 0.3 |
| g3sdr 21:47:20 | 148.8 | 25.9 | 0.4 | -0.7, 1.1 |
| g3sdr 21:51:18 | 11.3 | 97.2 | 3.0 | -1.5, 0.1 |
| g3sdr 22:51:53 | 665.1 | 97.9 | 4.5 | -5.3, 3.3 |

(clips resampled to 8 kHz; SNR and offset averaged over in-sync symbols.)

**Sync and SNR do not tell you the orientation (V2).** The same clips with
the spectrum mirrored about the 1500 Hz centre (`--invert`) still show
95-99 % sync and similar SNR and offset (FDV clip 99.4 % / 15.1 dB), yet
the decoded audio is different (RMS 1166 vs 1967, no useful correlation
with the normal decode). So "sync" cannot be the auto-invert detector for
V2, and a test of orientation has to look at the decoded output.
*Not yet listened to*: whether the mirrored decode is garbage or merely
different. Listen to `out/*.dec.wav` from both runs before relying on it.

**T-1, rade_tx output (`rade_tx_iq`, 10 s of speech)**

| | complex | real part |
|---|---|---|
| V2 (no TX BPF) | 99.9 % positive freq, 1070..1898 Hz, PAPR 3.4 dB | 50/50 split, PAPR 6.4 dB |
| V1 | 98.8 % positive, PAPR 0.8 dB, -20 dB edges -207..3211 Hz | 50/50 split, PAPR 3.9 dB |
| V1 + 1.6 kHz filter at 1475 | 100 % positive, 730..2223 Hz, PAPR 4.9 dB | |
| V2 + 1.0 kHz filter at 1500 | 100 % positive, 1098..1879 Hz, PAPR 3.6 dB | |

- The one-sided I/Q premise holds for both; the complex output has 3 dB
  lower PAPR than its real part.
- V1's modulator applies a time-domain soft limiter (`rade_tanh_limit` in
  `rade_ofdm.c`) and the library's V1 TX bandpass is off by default
  (`bpf_en = 0`, `rade_api.c:103`). About 10 % of V1's power lies outside
  750..2200 Hz. The 1.6 kHz TX filter removes it and puts the PAPR back to
  4.9 dB; the filter is therefore required for V1, and WDSP's peak control
  will see that 4.9 dB, not 0.8 dB.
- V2's library BPF (300..2700 Hz) is on by default; with it off the
  signal is already within 1070..1898 Hz.

**T-11, WDSP RXA/TXA with complex tones** (`test/rade_native/out/t11.txt` after a run)

- RX panel select 3, copy 0: output stays complex and one-sided. With
  copy 1 (I copied to Q) it collapses to a real signal (50/50 split).
- TX panel select 3: a complex input passes through TXA as a complex,
  one-sided output; bp0 gain is 6.0 dB (the 2.0 in `TXASetupBPFilters`).
  The speech stages being on or off, and ALC on or off, made no
  difference at this single-tone level; the level and OFDM behaviour of
  ALC / overshoot control is T-10, not done.
- **Sign convention: WDSP's positive passband selects the tone at
  negative numpy frequency (`I + jQ = exp(+jwt)`), on RX and TX.** So a
  conjugation is needed at both interfaces to and from `librade`, on top
  of Invert. Recorded in plan section 3.
- **piHPSDR's RX panel copy (read from source):** it never calls
  `SetRXAPanelCopy`; it calls `SetRXAPanelBinaural(binaural)` in
  `rx_set_af_binaural()` (`receiver.c:1724`), and WDSP maps that to
  `copy = 1 - binaural`. So normal audio is copy 1 (I to both
  channels) and **binaural on gives copy 0, i.e. complex I/Q out on
  L/R**. The RADE receive mode can use the existing binaural path
  (`rx->binaural = 1` for the tap) and restore it on leaving the mode.
- *Not yet done in T-11*: a `rade_tx` frame through TXA, a RADE signal
  through RXA, group delay, and the AGC level behaviour (with top gain
  80 dB a 0.1 amplitude tone came out +31 dB, i.e. the AGC is working on
  signals at ordinary IQ levels).

**T-11 second pass: a RADE signal through WDSP TX and RX** (`t11b_chain.py`, no noise yet)

Chain: features, `rade_tx`, x6 up, conj, TXA (DIGU, panel select 3,
speech stages off, filter 675..2275 Hz V1 / 1000..2000 Hz V2), RXA (same
edges, panel copy 0, AGC medium/top 80 dB), conj, /6, `rade_rx`. Measure:
decoded-vs-transmitted vocoder features, normalised error (no vocoder
involved), 5 s of speech.

| | sync % | SNR est dB | feature error |
|---|---|---|---|
| V1 direct (no WDSP) | 96.3 | 30.9 | 0.0174 |
| V1 through WDSP | 92.7 | 29.1 | 0.0177 |
| V2 direct | 96.6 | 17.5 | 0.0239 |
| V2 through WDSP | 94.5 | 17.1 | 0.0114 |

- Through WDSP TX+RX with the planned filters the link is transparent in a
  noiseless channel: V1 feature error 0.0174 vs 0.0177. (The V2 figure
  being lower than direct is within what the alignment search and a 5 s
  clip can give; read it as "no worse", not as "better".) Sync
  percentage is a little lower (starts later through the pipeline delay).
- Same result for RX AGC long / medium / fast / fixed 0 dB (V1): the
  decoder was handed the RX output re-levelled to rms 0.9, so this does
  **not** yet say what the AGC does to a fading signal; that is T-9 with a
  channel.
- **Delay through TXA + RXA: 11934 samples = 248.6 ms** at the default
  filter lengths (linear-phase FIR filters). Large against V2's 40 ms
  frame; `TXASetNC` / `RXASetNC` can shorten it. Not tuned yet.
- **TX level and ALC (first look, V1 and V2 behave alike):** the TX chain
  has +6 dB gain; ALC is idle up to an output of about rms 0.53 (V1,
  input rms 0.27) and then clamps the output to rms 0.67 / PAPR 3.5 dB,
  i.e. a peak limit at 1.0. With ALC off, output scales without limit
  (rms 3.5 at input rms 1.8). Link quality with the ALC clamping at input
  rms 0.9 was unchanged on features (0.0177) but the SNR estimate dropped
  from 29 to 25 dB. A sensible operating point is an input where the
  output peak stays below the ALC threshold; calibration to power is T-10
  proper.

**RX engine, offline** (`test/rade_native/test_rx_engine.c`, 50 s of speech
through the WDSP TX+RX chain, played in real time into
`rade_native_rx_block()` in 1024-sample blocks):

| | underruns | speech envelope vs original | latency incl. WDSP 0.25 s |
|---|---|---|---|
| V1 | 0 | corr 0.90 | 0.70 s |
| V2 | 1 (start-up) | corr 0.92 | 0.41 s |

Bugs found and fixed on the way: speech buffer truncated at 8 of 12 frames
per V1 modem frame; play-out cushion shorter than a V1 frame (now 200 ms V1,
100 ms V2). Speech was listened to by correlation only, not by ear.

**TX validation, offline** (`test/rade_native/tx_validate.py`; 12 s clips from
rade_c's `wav/`, reference = `rade_tx_wav` + `rade_rx_wav`, ours = TX
engine → WDSP TXA (the capture, far side of WDSP) → WDSP RXA → RX engine):

| clip | feature error | decode vs ref (envelope) | TXA out rms / PAPR | TXA band |
|---|---|---|---|---|
| brian_g8sez V1 | 0.004 | 0.98 | 0.52 / 4.8 dB | -2227..-738 Hz |
| david_vk5dgr V1 | 0.007 | 0.93 | 0.52 / 4.7 dB | -2227..-727 Hz |
| k0pfx_mel V1 | 0.009 | 0.96 | 0.52 / 4.7 dB | -2227..-727 Hz |
| brian_g8sez V2 | 0.004 | 0.98 | 0.41 / 3.5 dB | -1887..-1102 Hz |
| david_vk5dgr V2 | 0.007 | 0.97 | 0.42 / 3.4 dB | -1887..-1102 Hz |
| k0pfx_mel V2 | 0.009 | 0.98 | 0.42 / 3.3 dB | -1887..-1102 Hz |

All six PASS (feature error under 5 %, envelope correlation 0.90 or more
against the reference decode). The TXA output sits at negative frequency,
which is WDSP's upper sideband, inside the planned filter. Output level
is well under the ALC clamp (rms 0.67). Notes: (1) the waveform
correlation with the reference decode is 0.02-0.06 (V1) and 0.18-0.42 (V2)
even though the features agree to under 1 %: the vocoder output is not
phase-reproducible, so only envelopes are compared; (2) the first version of
the metric compared unaligned ends and wrongly failed clips, now aligned and
trimmed by 0.5 s; (3) end-to-end latency TX engine + WDSP + RX engine is
about 1.9 s on V1 (clip start to first decoded speech, measured on one
clip; not broken down by stage). The WDSP TXA+RXA pair alone was 0.25 s.

**End of over and callsign, offline** (`tx_validate.py`, TX engine with call
EI6IZ → TXA → RXA → RX engine): V1 decodes `EI6IZ` from the EOO on all three
clips, 12 of 12 runs; hold after release 576-619 ms (V1), 384 ms (V2); V2
has no call. Harness note: the TX engine test runs at 3x real time
(`--speed 3`); at 8x the worker is late and the test itself makes holes.
The TX worker's cost on a Pi is not measured.

## Next

1. Capture on air with the RADE button; report what happens.
2. On-air TX test (V1 first): Transmit RADE, Capture on, key up briefly into a dummy load or on a quiet frequency; check `_tx.wav` band and level, then the far end.
3. T-2: the same chain over AWGN and fading channels, with and without WDSP.
4. T-3: orientation matrix (Invert set right and wrong, V1 and V2).
5. Listen to the mirrored V2 decode.

# RADE native: progress

Plan: [rade-native.md](rade-native.md). Branch `feature/rade-native`.
Updated with every commit that moves the work. Newest entry first.

## Status

| Phase | State |
|---|---|
| 0 Build/link `librade`, T-0, T-1, T-11 | in progress: librade builds, T-0, T-1, T-11 (first pass) done; `RADE_NATIVE` make option done |
| 1 Harness | not started |
| 2 RX | not started |
| 3 Tuning, SNR, logging | not started |
| 4 TX | not started |
| 5 Extras | not started |

## Log

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

## Next

1. `RADE_NATIVE` make option linking librade (no engine code yet).
2. T-11 second pass: RADE signals through RXA/TXA (T-2 round trip needs
   this), group delay.
3. Listen to the mirrored V2 decode.

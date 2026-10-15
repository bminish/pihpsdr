# Native RADE V1 / V2 in piHPSDR: strategy and plan

Branch `feature/rade-native`, started 2026-10-15 from `TEST` (0c871087).
This is a planning document: nothing here is built yet. It is a local
branch, not upstream work, so the `TEST` house rules (one `LC-NNN` per
commit, register entries) do not bind it. What we keep is the habit that
matters: every claim in this file is marked **known** (read in source),
**to verify** (inferred, a test settles it) or **not measured**.

## 1. Goal

Receive and transmit FreeDV RADE V1 and V2 inside piHPSDR, in I/Q,
without freedv-gui and an audio loopback in between.

- Work in complex baseband at 8 kHz, which is what `librade` takes.
- Treat "which sideband" as **inversion plus shift**, not LSB/USB.
- Three operator choices: **RADE V1**, **RADE V2**, **Invert**.
- Filter the TX signal: about 1.6 kHz wide for V1, about 1.0 kHz for V2.
- Tuning indication, SNR, sync, frequency offset, logging and tracking.
- Test harness good enough to validate performance numerically, before
  and after every step.

Non-goals for now: upstreaming; diversity changes (the existing V1 pilot
correlator stays as it is, see `diversity-rade.md`); PureSignal with RADE
TX; data/text channel beyond reading what the library exposes.

## 2. What `librade` gives us (known)

`third_party/rade_c` builds `librade.so`, a CMake project named
`radae_nopy`: David Rowe's C port of the RADE modem. It also builds
`libopus.a` containing `lpcnet_compute_single_frame_features*` and
`fargan_synthesize*`, which the speech side needs. (`radae_nopy/build` is
an older static build of the same thing.) API in
`rade_api.h`:

| Need | Call | Notes |
|---|---|---|
| Open V1 / V2 | `rade_open(model, flags)` | `RADE_MODE_V2`, `RADE_USE_C_ENCODER\|DECODER`, `RADE_NO_TX_BPF` (V2 only), verbosity flags. One context holds one Tx and one Rx. |
| RX I/Q in | `rade_nin()`, `rade_rx(r, feat, &eoo, eoo_bits, RADE_COMP in[])` | **Complex** input at 8 kHz, a variable `nin` per call (timing tracking). Returns speech *features*, not audio. |
| TX I/Q out | `rade_tx(r, RADE_COMP out[], feat)`, `rade_tx_eoo()` | **Complex** output at 8 kHz. EOO frame at end of over, V1 and V2. |
| Sync | `rade_sync()` | |
| Freq offset | `rade_freq_offset()` | Hz, valid in sync. |
| SNR | `rade_snrdB_3k_est()` | dB in a 3 kHz reference bandwidth. |
| Per-symbol stats | `rade_get_stats()` | **V2 only**; all zero on V1. Timing, freq, AGC gain, SNR. |
| EOO | `has_eoo_out` from `rade_rx` | V1 also returns 8 soft EOO bits. |
| V2 aux symbol | `rade_tx_set_data_symbol`, `rade_rx_get_data_symbol` | BPSK, one per modem frame. |
| V2 RX AGC | `rade_rx_set_agc()` | Normalises RMS; correction window about ±20 dB. |

Things the library does **not** have (known):

- **Speech.** TX needs LPCNet feature extraction at 16 kHz in; RX needs
  FARGAN synthesis to 16 kHz out. Both are in the bundled opus build
  (`lpcnet_compute_single_frame_features`, `fargan_synthesize`), and
  freedv-gui's `RADETransmitStep.cpp` / `RADEReceiveStep.cpp` are the
  reference for calling them. We can link them from the same build.
- **Callsign text.** `rade_text.{h,cpp}` is in freedv-gui, not in
  `librade`. Wrapping the aux symbol in text is a port (small, C++ in the
  original). Defer.
- **V1 "stats".** For V1 only sync / offset / SNR / EOO are available.

Waveform facts (known, from `rade_dsp.h` and `rade_v2_ofdm.h`):

| | V1 | V2 |
|---|---|---|
| Carriers | 30 at 50 Hz, 750 .. 2200 Hz | 14 at 62.5 Hz, 1062.5 .. 1937.5 Hz |
| Centre | 1475 Hz (aimed at 1500) | 1500 Hz |
| Occupied | about 1.5 kHz | about 0.9 kHz |
| Symbol / CP | 160 + 32 | 128 + 32 |
| Modem frame | 120 ms, one pilot + 4 data | 40 ms, 2 data symbols per frame; sync from CP autocorrelation plus a frame-sync net (pilot structure to verify)  |
| TX BPF in library | none in the API | 300-2700 Hz, on by default, the model was trained with it |

These bandwidths support the stated TX filter targets: **1.6 kHz** is
the V1 occupied width plus skirts; **1.0 kHz** is the V2 width plus
skirts. Whether filters that narrow cost decode quality is a **to
verify** item (section 8, test T-4): the V2 model was trained under a
300-2700 Hz filter, so a much narrower one is a change the model has not
seen.

## 3. The frequency model: centre, shift, inversion

`librade` expects the modem at **positive** baseband frequency: V1 carriers
at +750 .. +2200 Hz, V2 at +1062.5 .. +1937.5 Hz, from a complex stream.
(freedv-gui feeds it real audio from an SSB demodulator, imag = 0, with a
frequency-shift step for the tuning offset; the real-input form needs the
sideband to have been chosen *upstream* of the library.)

In I/Q we can do better. Define, per band/operator setting:

```
  x_lib[n] = conj?( x_if[n] * exp(-j 2 pi * d * n / Fs) )
```

- `x_if` is the 8 kHz complex baseband produced from the radio's I/Q by
  mixing the **dial frequency** to zero and decimating.
- **Shift `d`** (Hz) is how far the modem centre sits from where the
  library expects it, i.e. the tuning error plus any deliberate offset.
  Inner loop: `rade_freq_offset()` tracks the residue (V1 acquisition
  range is +-100 Hz, `RADE_ACQ_FRANGE`; V2 to verify). Outer loop: the
  mixer here, steered by the tuning indication.
- **Inversion** is a conjugation. It turns a lower-sideband signal into
  an upper-sideband one with no change of dial frequency, because
  conjugating baseband mirrors the spectrum about the dial.

So the conventional sidebands become a *default* for `invert`, not a mode:

| Waveform | Convention on air | `invert` default |
|---|---|---|
| V1 below 10 MHz | LSB (signal below the suppressed carrier) | on |
| V1 above 10 MHz | USB | off |
| V2 | USB only | off |

and the **Invert** control is XOR-ed with that default, so a single
button corrects the on-air mistakes: V2 sent on LSB, V1 sent on the
"wrong" side of 10 MHz, or a station running a mirrored chain. The
10 MHz boundary is a default from the user's brief and should be a
named constant, ideally per-band overridable.

TX is the same transform run the other way: library output is at positive
frequency, so conjugate (if the effective invert is on), shift, and hand
the result to the radio as I/Q. A conjugated baseband spectrum appears
below the dial frequency, which is exactly what LSB means. **To verify**:
that `rade_tx` output really is one-sided (carriers only at positive
frequency). If so, I/Q TX is a clean single-sideband signal with no image
at all, which is better than the real-part-then-SSB-modulator path
freedv-gui relies on. A spectrum capture of a `rade_tx` frame settles it
(test T-1).

### Centre frequency as the operator's number

Presenting "LSB / USB" is the thing to avoid. Proposal:

- Keep **VFO = dial frequency** (what freedv-gui users and the existing
  RADE nets use), with the library centre readout beside it: `dial +/-
  1475` (V1) or `dial +/- 1500` (V2), sign from the effective invert.
- Panadapter overlay already exists for V1 (`rx_panadapter.c`,
  `RADE_CORR_OCC_LO/HI`, side from `div_rade_side_get()`); generalise it
  to follow *mode + invert* rather than the filter midpoint, and add V2
  geometry.
- Option for later: a "tune to centre" display where the VFO shows the
  modem centre and the dial is derived. piHPSDR already has `vfo[].offset`
  for this kind of thing, and `diversity_auto.c` uses it as `frame_off`;
  reuse that rather than inventing a second offset concept.

## 4. Where it plugs in

The RX and TX engines (`receiver.c`, `transmitter.c`) have clean seams.

### RX (known from `receiver.c`)

`rx_add_iq_samples()` fills `rx->iq_input_buffer`; `rx_full_buffer()` runs
the noise blanker, `fexchange0()` (WDSP, produces `rx->audio_output_buffer`
at 48 kHz) and `Spectrum0()`; then `rx_process_buffer()` does volume,
mute, local audio, and hands samples to the protocol. The diversity engine
already taps this stream before the noise blanker (`rx_add_div_iq_samples`
→ `diversity_auto_sample`) and decimates to 8 kHz for the V1 correlator,
so a working decimator and rate table (48 kHz .. 1536 kHz, all integer
ratios to 8 kHz) exists in-tree.

**Option R1 (recommended): tap, decode off-thread, replace the audio.**

```
 iq_input_buffer ──► (WDSP as today: spectrum, S-meter, NB) 
        │
        └─► decimate to 8 kHz ─► mix/invert ─► ring ─► RADE worker thread
                                                         rade_rx → FARGAN
        audio_output_buffer ◄── resample 16k→48k ◄──────┘  (16 kHz speech)
```

- WDSP keeps running, so the panadapter, waterfall, S-meter and the
  existing diversity machinery are untouched; only the demodulated audio
  is replaced when RADE is the active mode and has speech to give.
- Worker thread because `rade_rx` is not cheap (the existing bench puts
  V1 correlation, not the decoder, at 13 % of one Pi core;
  the **decoder cost on a Pi is not measured**). RADE is block-timed at
  120 ms (V1) / 40 ms (V2), so the thread adds no latency beyond the
  protocol itself.
- Needs: its own level normaliser ahead of the V1 decoder (V2 has the
  library AGC, `rade_rx_set_agc`), a squelch-to-silence when not in
  sync, and a sample-clock-drift policy for the 16 kHz speech against
  the 48 kHz audio clock (a slow-adjusting resampler or fixed-depth FIFO
  with occasional repeat/drop; freedv-gui's pipeline does something
  similar).
- Pre-decimation selectivity: WDSP's RXA filter does not apply, so the
  decimator must reject adjacent signals itself. A 3 kHz wide channel at
  8 kHz is enough for both waveforms; a strong adjacent signal inside the
  radio's own passband remains a limitation we share with any I/Q
  receiver.

**Option R2: a mode in WDSP.** Adds a RADE stage inside the RXA chain.
It would get filtering, AGC and shift for free but means modifying WDSP
(vendored, upstream-owned) and WDSP stages are real-valued by the time
they leave the demodulator. Rejected: it works against the I/Q goal.

**Option R3: audio loopback to freedv-gui** (status quo). Kept as the
**reference** for A/B comparison in the harness, not as a product option.

### TX (known from `transmitter.c`)

`tx_add_mic_sample()` fills `tx->mic_input_buffer` at 48 kHz;
`tx_full_buffer()` runs `fexchange0()` into `tx->iq_output_buffer` and
sends the samples to the protocol layer (`old_protocol_iq_samples()` /
`new_protocol_iq_samples()`), after applying `drive_scale`.

**Option T1 (recommended): replace the IQ at the seam.**

```
 mic 48k ─► decimate 16k ─► LPCNet features ─► rade_tx (8 kHz, complex)
        ─► TX bandpass 1.6k (V1) / 1.0k (V2), complex, centred on fc
        ─► conj? · shift ─► interpolate to radio TX rate ─► drive/limit ─► protocol
```

- Where WDSP would have produced `iq_output_buffer`, we produce it
  ourselves. Still call `fexchange0()` (and discard) so the WDSP TX
  state machine (slew, PTT sequencing, VOX) keeps working, as the CW
  path already does.
- The TX panadapter reads `iq_output_buffer`, so it shows what is sent.
- V2: pass `RADE_NO_TX_BPF` and filter ourselves (we need a complex,
  invertible, shiftable filter anyway), or leave the library filter on
  and add ours behind it. Which to choose is test T-4.
- **ALC / drive**: RADE is an OFDM waveform, so peak-to-average matters
  and the radio's PA is the limit. We need a defined RMS level out of the
  chain, a hard peak limiter after the filter (or clip → filter → clip,
  as freedv-gui does for RADE; **to verify** against its code), and
  `drive_scale` applied last. Power calibration is a bench job, **not
  measured**.
- **EOO** on unkey: `rade_tx_eoo()` must be sent before the PA drops.
  piHPSDR's PTT-release path needs a short, bounded tail: flush the last
  frame, send EOO, then go. The tail length is 1152 samples at 8 kHz
  (V1, 144 ms) or 960 (V2, 120 ms) plus filter group delay.
- **Not supported with RADE TX**: PureSignal (distorts a waveform the
  predistorter cannot assume), CFC and the TX equaliser/compressor
  (bypassed because we bypass `fexchange0()` output). The menus should
  grey them out rather than silently ignore them.

**Option T2: feed RADE audio into the existing WDSP SSB TX** (i.e. do
what freedv-gui does: real audio into USB/LSB). Easiest, and the A/B
reference for T1, but the real-audio form has an in-band image that WDSP's
SSB filter then removes, and it makes the sideband a WDSP-mode question
again. Kept as a fallback; it would be a day of work, not a design.

## 5. Tuning indication

Three layers, cheapest first:

1. **Library offset**: `rade_freq_offset()` while in sync. Display as a
   signed Hz number and as a marker on the panadapter. Closing the loop
   (steering the mixer `d`) is a policy question: drift or tuned-off
   transmitters will give a steady offset, and the operator wants the
   dial to be right as well as the decode.
2. **Pre-sync pointers**: while searching, show where the existing V1
   pilot correlator thinks the signal is. This is what
   `rade_corr_freq_off` / `rade_corr_quality` already provide for V1.
   V2 has no pilot to correlate: use the CP autocorrelation peak that the
   library computes internally (**not exported today**; would need a
   getter added to `librade`, a small, local patch).
3. **Spectral template**: compare the received 8 kHz spectrum against the
   waveform's known comb (V1: 30 lines at 50 Hz; V2: 14 at 62.5 Hz). Cheap
   to compute and gives a "tune here" metric without sync. This is
   optional and only worth it if the acquisition tests show
   frequent failures to find the signal.

Auto-inversion: a mis-set Invert is the most likely cause of "nothing
happens", so a hint is worth having. For V1 the existing mirrored-pilot
test (`rade_corr_mirrored`, sideband expected vs found) can say "the
pilot is on the other side". For V2 acquisition on both orientations
would need two Rx contexts (one `struct rade` holds one Rx), or an
orientation test ahead of `rade_rx`. **Open**: does the V2 CP-based
timing metric discriminate orientation, and does the sync net's output
differ between orientations? Test T-5.

## 6. SNR, logging, tracking, reporting

Known sources: `rade_snrdB_3k_est()` (both), `rade_sync()`,
`rade_freq_offset()`, and for V2 `rade_get_stats()`.

- **Meter**: show RADE SNR in the S-meter area in place of, or beside,
  the signal level while locked. Convention: dB in 3 kHz, as the library
  defines it. Not directly comparable to the diversity engine's SNR or to
  WDSP's noise-floor estimate; the label should say so.
- **Per-frame record**, at modem-frame rate: time, sync, f_off, SNR,
  AGC gain, level in, invert, dial, mode (V1/V2), plus for V2 timing
  (`delta_hat`). This is the same data the diversity captures
  already carry for the correlator; writing it to a text/CSV log that
  the harness can read back avoids a second capture format.
- **Over tracking**: an "over" starts at sync and ends at EOO or loss for
  more than a threshold; track per over: mean/min/max SNR, duration,
  freq offset drift, decode-gap count, whether EOO was seen. A
  **session table** in the UI is a later nicety; the log is the
  priority.
- **Spots/reporting**: the library has no reporting function (**known**:
  no spot/PSK-Reporter-style calls in `rade_api.h`). What exists is the
  V2 aux symbol and, in freedv-gui, the text layer over it. Two
  reporting directions to keep separate: (a) *station reporting* by the
  FreeDV reporter service, which freedv-gui does and would be a network
  client here (a later phase, needs the callsign text layer first);
  (b) *local reporting*, i.e. the log above. Only (b) is in scope now.

## 7. Phases

Per the working rule: each phase ends in something that runs and is
measured before the next is planned in detail.

| Phase | Deliverable | Gate |
|---|---|---|
| 0 | `librade` + lpcnet/fargan building and linking in the pihpsdr Makefile, behind an option; headless "hello" decode of a WAV | builds on x86 and Pi; decodes a known clip |
| 1 | **Harness first** (section 8): offline I/Q file → pihpsdr RADE RX chain → decoded audio + log | numbers reproduce freedv-gui's on the same clip |
| 2 | RX V1 + V2 live, Invert, shift; audio replaces WDSP output; status line (sync/SNR/offset) | A/B vs freedv-gui loopback on the same recordings |
| 3 | Tuning aids, SNR meter, per-frame log, over tracking | log matches harness numbers |
| 4 | TX: features → `rade_tx` → filter → I/Q; EOO; drive/limit; V1 then V2 | spectrum/mask and decode-loopback tests; on-air only after |
| 5 | Auto-invert hint, per-band default overrides, text/reporting | |

Phases 0 and 1 first. Everything after rests on the harness being right.

## 8. Test harness

Existing assets we can build on (known): `test/diversity/` (a C test
suite with `test_rade`, `test_digital`, `bench_cpu`, `pi_bench`),
`test/diversity/devtools/` (`replay_rade`, `run_ref`, `score_rade`,
`score_radev2`, `radev2_iq`, `py/`), the 183 `captures/*.divc` diversity
captures, and `~/sdr/RadeV2_clips/` (V2 on-air recordings from
2026-09-11, plus a `FDV_FromRadio_..._EI6IZ_V2.wav`). The V2 oracle noted
in project memory is the decoder-only reference; check it still runs
before relying on it.

New pieces, in order of value:

- **T-0 Reference decode.** For each clip, freedv-gui's decode as the
  baseline: sync fraction, mean SNR, decoded audio (and a loss figure for
  V2 where the oracle has one). Run once, store the results next to the
  clip as the numbers a native decoder must match.
- **T-1 TX one-sidedness.** FFT of `rade_tx` output (V1, V2): confirms
  positive-frequency-only, and measures occupied bandwidth and PAPR
  before and after our filter.
- **T-2 Round trip without a radio.** `rade_tx` → our TX chain
  (filter, invert, shift, interpolate) → synthetic channel (AWGN, then
  the CCIR/ITU multipath/Doppler profiles freedv-gui's `ch` tool uses) →
  our RX chain (decimate, invert, shift) → `rade_rx`. Output: decode
  success vs SNR curve, for V1 and V2, each of {normal, inverted} x
  {0, +-20, +-50, +-100 Hz offset} x {with, without our filters}. This
  is the main validation instrument and runs in seconds, no hardware.
- **T-3 Orientation matrix.** The same tests with the signal mirrored
  at the channel and Invert set correctly vs incorrectly; asserts that
  Invert fixes the mirrored case and that the wrong setting fails
  cleanly (no false sync, no crash).
- **T-4 Filter width sweep.** TX filter from 0.8 to 2.7 kHz in steps on V1
  and V2, measured as the SNR needed for a given decode quality (V1/V2
  loss figure or sync-to-bit-error proxy). Gives the
  1.6 kHz / 1.0 kHz numbers evidence instead of a starting guess.
- **T-5 Acquisition and orientation detection.** Time-to-sync and false
  sync rate vs SNR and offset; both orientations, to settle the
  auto-invert question in section 5.
- **T-6 Decimator/mixer.** Unit tests at every radio sample rate
  (48 .. 1536 kHz): passband flatness, stopband, image rejection for
  the mixer, and phase continuity across blocks (the existing
  decimator's tests are the model).
- **T-7 CPU and latency.** Decoder and encoder cost per frame on x86 and
  the Pi 5 (target: the whole chain a small fraction of one core, not
  measured yet); end-to-end audio latency TX and RX.
- **T-8 On-air recordings.** The 2026-09-11 clips plus new recordings
  made with the planned release, scored with the same log format, for
  regressions. Captures are taken from the desktop (project memory): the
  user records, we analyse.

Output convention: every test writes the same per-frame CSV as the live
logger (section 6), so the harness and the product share one format.

## 9. Risks and open questions

- **`librade` is in motion.** V2 is new (the freedv-gui checkout is on a
  `ms-rade-v2` merge). Pin the commit we link, record it here, and
  expect model-file / API changes. `rade_version()` should be checked at
  start-up.
- **Model-file and licence handling.** `rade_open` takes a model path
  but the C encoder/decoder (`RADE_USE_C_*`) have the weights compiled in.
  Confirm distribution terms for the bundled opus/lpcnet pieces before
  anything is shared; this branch is not for upstream and the code stays
  separately licensed from piHPSDR core.
- **TX power and mask.** OFDM at amateur power into a PA that is not
  linear is the main practical hazard. Do not key a PA with RADE until
  T-1 and a dummy-load spectrum check are done.
- **Sample-clock drift** between the radio's RX I/Q clock and the audio
  sink affects the RX speech FIFO.
- **V2 acquisition range and V1 tolerance of the shift** are as stated by
  the library constants (V1 +-100 Hz) but not yet confirmed to hold with
  our decimator in the chain.
- **Remote (client/server) operation** is deferred, as for diversity:
  keep the engine server-side and the state machine free of GTK so that
  it can be wired up later.
- **Diversity interaction.** The V1 pilot correlator shares the 8 kHz
  front end and the Invert/side idea. Decide in phase 1 whether the
  native decoder reuses `rade_correlator.c`'s decimator, or both use one
  new shared module. Prefer a shared module; do not fork the code.

## 10. Immediate next step

Phase 0 and the first harness pieces: link `librade` + lpcnet/fargan
behind a `RADE_NATIVE` make option, a small `test/rade_native/` directory
with T-0 (reference decode of the existing clips) and T-1 (TX spectrum).
Both are offline, touch no engine code, and tell us whether the
one-sided I/Q premise holds before any of the RX/TX design depends on it.

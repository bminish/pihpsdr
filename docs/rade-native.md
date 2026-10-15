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

Constraint: work **with** the existing WDSP chain (its shift, filters,
AGC, slew, ALC), using its public `Set*` calls. No bypass of
`fexchange0()` and no changes to WDSP code. Upstream WDSP code and
docs are at <https://github.com/TAPR/OpenHPSDR-wdsp> and are consulted
as needed.

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

- `x_if` is the 8 kHz complex baseband taken from the WDSP RXA output
  (section 4), which has already mixed the **dial frequency** to zero and
  applied the shift and bandpass. In the WDSP design most of this
  transform is done by WDSP itself: the RXA shift (`SetRXAShiftFreq`)
  and the complex bandpass edges (negative for invert) place the signal;
  only the final conjugation and the 6:1 rate change are ours.
- **Shift `d`** (Hz) is how far the modem centre sits from where the
  library expects it, i.e. the tuning error plus any deliberate offset.
  Inner loop: `rade_freq_offset()` tracks the residue (V1 acquisition
  range is +-100 Hz, `RADE_ACQ_FRANGE`; V2 to verify). Outer loop: the
  RXA shift, steered by the tuning indication.
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
frequency, so conjugate (if the effective invert is on), and feed the
result to the WDSP TXA chain as complex input, where the TX bandpass
(at negative edges for invert) and the rest of the chain take it to the
radio as I/Q. A conjugated baseband spectrum appears
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

## 4. Where it plugs in: through WDSP, not around it

Decision: **use WDSP's I/Q path as it is.** We do not bypass `fexchange0()`
or replace `iq_output_buffer`. WDSP supplies the shift, the complex
bandpass, the notch/NB, the AGC, the PTT slew, ALC/overshoot control and
(in principle) PureSignal; we add only what it does not have: the 8 kHz
rate change, `librade`, the speech coder, and the invert conjugation.

**WDSP references.** The local `wdsp/` is piHPSDR's vendored copy and may
differ from upstream. Upstream code and documentation are at
<https://github.com/TAPR/OpenHPSDR-wdsp> and are the reference for stage
behaviour, parameter ranges and the meaning of the `Set*` calls. When a
section below says "to verify against WDSP", check the local copy first,
then upstream, and record any difference here.

### 4.1 What the local WDSP does, from the source (known)

- **RXA chain** (`wdsp/RXA.c`): `shift → rsmpin → gen0/adcmeter → notchdb →
  nbp0 → bpsnba → … → agc → bp1 → siphon → … → panel → rsmpout`. The
  chain is complex (interleaved I/Q doubles) until the panel.
- **Sideband selection is a complex bandpass**, not a demodulator: in the
  SSB modes the filter passes one side of zero and the panel copies I to
  Q. Positive `filter_low/high` pass the signal above the dial frequency,
  negative the signal below it.
- **RX panel** (`wdsp/patchpanel.c`): `inselect` 3 passes I and Q; `copy`
  0 leaves them separate, 1 copies I to Q, 2 copies Q to I, 3 swaps.
  piHPSDR sets `SetRXAPanelSelect(rx->id, 3)` (`receiver.c:958`). The
  copy setting piHPSDR leaves for SSB audio is **to verify**; we need
  `copy = 0` so the output stays complex.
- **TX chain** (`wdsp/TXA.c`): `rsmpin → gen0 → panel → phrot → micmeter →
  amsq → eqp → preemph → leveler → cfcomp → bp0 → compressor → bp1 →
  osctrl → bp2 → alc → ammod/fmmod → gen1 → uslew → siphon → calcc/iqc →
  cfir → rsmpout`. Also complex.
- **TX panel**: `SetTXAPanelSelect(…, 2)` is "mic I" (what piHPSDR uses,
  `transmitter.c:1274`); **`select = 3` with `copy = 0` passes I and Q**
  (`patchpanel.c:228`). So the TX input can be complex.
- **TX bandpass**: in the digital and SSB modes `bp0` is a complex filter
  between `f_low` and `f_high` (`TXASetupBPFilters`, `TXA.c:824`), set by
  `SetTXABandpassFreqs`. Negative values pass the lower sideband.
- **I/O**: `fexchange0(channel, in, out, &err)` takes interleaved I/Q at
  the channel input rate (48 kHz for the mic and RX audio side).
  `fexchange2` takes separate I and Q buffers; piHPSDR uses `fexchange0`.

### 4.2 RX design

```
 radio I/Q ─► (rx->iq_input_buffer) ─► WDSP RXA  ──────────────────────────┐
                                        shift · NB · notch · bandpass · AGC │
                                        panel: select 3, copy 0             │
                                                                            ▼
                          audio_output_buffer = complex baseband, 48 kHz  (rx_full_buffer)
                                                                            │
         (display, S-meter, volume, protocol audio unchanged)               ▼
                            decimate 6:1 ─► conj if invert ─► ring ─► RADE worker thread
                                                                      rade_rx → FARGAN
        audio_output_buffer (L/R) ◄── interpolate 16 kHz → 48 kHz ◄──────┘  speech
```

- **WDSP sets the geometry.** The RX filter is set around the modem
  centre `fc` (1475 Hz V1, 1500 Hz V2) with a width that comes from the
  waveform (V1 about 1.6 kHz, V2 about 1.0 kHz plus skirts, to be
  tuned in T-4). With effective invert on, the same edges are negative:
  WDSP selects the lower sideband, and we conjugate after the tap.
  So "LSB/USB" never appears in the operator's view: the UI shows
  V1 / V2 / Invert, and the code maps that to the filter edges and one
  conjugation.
- **Tap point** is the end of `rx_full_buffer()`, after `fexchange0()` and
  before `rx_process_buffer()`. The complex samples are consumed there,
  and the same buffer is then overwritten with decoded speech (or
  silence) before the existing volume / mute / local-audio / protocol
  code runs. Display, S-meter and diversity code are untouched.
- **AGC.** The WDSP AGC stays in the chain. A fast AGC on a flat OFDM
  spectrum will modulate the envelope; the aim is slow/fixed AGC for RADE
  so that RADE sees a steady level. V2 also has `rade_rx_set_agc()`. Which
  combination decodes best is test T-9 (WDSP AGC off / slow / fast
  × library AGC on / off).
- **Rate change only.** 48 kHz → 8 kHz is 6:1, a half-band-friendly
  chain; the existing decimator in `rade_correlator.c` is a candidate
  to share (see section 9). Speech back is 16 kHz → 48 kHz, 3:1.
- **Thread.** `rade_rx` and FARGAN run on a worker, fed from a ring.
  Decoder cost on a Pi is **not measured** (T-7).
- **When not in sync** the speech path outputs silence; the operator
  hears nothing rather than the complex baseband as noise.
- **Mode.** RADE V1 / V2 become receive modes in the same sense as DIGU:
  mode change sets filter edges, AGC and panel; leaving the mode restores
  panel `copy` and the normal filter.

### 4.3 TX design

```
 mic 48 kHz ─► decimate to 16 kHz ─► LPCNet features ─► rade_tx (8 kHz, complex)
     ─► interpolate to 48 kHz ─► conj if invert ─► mic_input_buffer (I and Q)
                                                          │
 WDSP TXA:  panel(select 3, copy 0) · [phrot/eq/preemph/leveler/cfc/compressor bypassed]
            bp0 = complex bandpass fc ± (0.8 | 0.5) kHz · osctrl · alc · uslew
            · iqc (PureSignal, if it can be made to work) · cfir · rsmpout
                                                          │
                                           iq_output_buffer ─► protocol (unchanged)
```

- WDSP provides the TX bandpass (the **1.6 kHz / 1.0 kHz** filters are just
  `SetTXABandpassFreqs` values, centred on `fc`, negative for invert),
  the PTT up/down slew, ALC and overshoot control, `drive_scale` handling
  in piHPSDR as today, and the TX panadapter data.
- **Mic-path stages must be neutral.** EQ, leveler, CFC, compressor,
  pre-emphasis and phase rotator are speech processors; they are bypassed
  while a RADE TX mode is active, with the `Set*Run` calls and the saved
  profile values left alone so that leaving the mode restores them. The
  mic gain (panel gain1) becomes the single RADE level control; the
  calibration is test T-10.
- **ALC and overshoot control are the open item.** They were designed
  for speech. On OFDM they may hold the level down on peaks and so raise
  the in-band distortion; whether to leave ALC on, use it as a plain
  peak guard, or set a threshold per waveform is decided by T-1 (peak
  statistics) and T-10 (drive vs. distortion against the decoder).
- **The RADE audio chain** (mic → 16 kHz → features → `rade_tx`) runs on
  the TX thread's mic sample cadence. It produces 8 kHz complex samples
  in 120 ms (V1) / 40 ms (V2) frames; a small FIFO smooths the burst
  per frame into the steady 48 kHz stream WDSP wants.
- **EOO.** On PTT release, `rade_tx_eoo()` is queued behind the last
  frame, then zeros; the TX state machine holds the PTT/slew for
  EOO length plus filter delay (V1 1152 samples = 144 ms, V2 960 =
  120 ms) before ramping down. How piHPSDR sequences the end of an over
  (VOX hang, `txrxcount`, radio relay) is **to verify** in
  `radio_is_transmitting()` and the PTT path before the tail is added.
- **PureSignal** is *possible* in principle, since the TXA siphon/iqc
  stages see a complex signal. It is **not a goal**; treat as a later
  experiment, not part of the first TX release.
- **Chain input is complex.** `tx_add_mic_sample()` writes `Q = 0` today; the
  RADE path writes both I and Q into the same buffer at the same place.
  VOX, mic metering and TX-in-progress logic are untouched, except
  that mic-driven VOX needs to be disabled or replaced by the PTT while
  RADE speech is the source.

### 4.4 Alternatives kept for the harness

- **freedv-gui over an audio loopback.** Still the reference decoder and
  transmitter for A/B tests (T-0, T-8).
- **RADE audio into normal DIGU TX.** Real audio in a normal WDSP SSB
  channel, as freedv-gui does. A quick comparison transmit for the
  occupied-spectrum test, not a design.

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

Each phase ends in something that runs and is measured before the next
is planned in detail.

| Phase | Deliverable | Gate |
|---|---|---|
| 0 | `librade` + lpcnet/fargan building and linking in the Makefile behind an option; headless decode of a WAV; **WDSP I/Q probe** (T-11) | builds on x86 and Pi; decodes a known clip; panel/TXA questions in 4.1 settled |
| 1 | Harness (section 8): I/Q file → WDSP RXA at 48 kHz → decimate → `rade_rx`; and the reverse for TX | numbers match freedv-gui on the same clip |
| 2 | RX V1 + V2 live: filter edges, invert, panel copy 0, tap, worker thread, speech back; status line | A/B vs freedv-gui on the same recordings |
| 3 | Tuning aids, SNR meter, per-frame log, over tracking | log matches harness numbers |
| 4 | TX V1 then V2: features, `rade_tx`, interpolate, TXA with neutral mic stages, EOO | spectrum/mask, decode loopback, dummy load; on-air only after |
| 5 | Auto-invert hint, per-band defaults, text/reporting; PureSignal experiment | |

Phases 0 and 1 first.

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
- **T-6 Rate changers.** Unit tests for the 48 kHz <-> 8 kHz and
  48 kHz <-> 16 kHz converters we add: passband flatness, stopband,
  image rejection, and phase continuity across blocks (the existing
  decimator's tests are the model). The radio-rate to 48 kHz part is
  WDSP's and is not retested here.
- **T-7 CPU and latency.** Decoder and encoder cost per frame on x86 and
  the Pi 5 (target: the whole chain a small fraction of one core, not
  measured yet); end-to-end audio latency TX and RX.
- **T-8 On-air recordings.** The 2026-09-11 clips plus new recordings
  made with the planned release, scored with the same log format, for
  regressions. Captures are taken from the desktop (project memory): the
  user records, we analyse.

- **T-9 WDSP RX settings.** Over RXA with the real filter edges: AGC
  off / slow / fast, filter width and window, notch/NB on and off, with
  and without invert; measure decode quality against the T-2 channel and
  find which settings leave RADE undamaged.
- **T-10 WDSP TX settings.** Over TXA: mic-stage bypass check (output with
  EQ/leveler/CFC/compressor at their stored values must be identical to
  output with them off), ALC and overshoot control on/off, gain sweep;
  measure occupied bandwidth, PAPR, in-band and adjacent-band distortion
  and decode-loopback loss against drive level. This defines the RADE
  level calibration.
- **T-11 WDSP I/Q probe.** Settles the "to verify" items in 4.1 with a
  small offline program linking the vendored `wdsp/`: push a known
  complex tone and a `rade_tx` frame through RXA (panel select 3, copy 0)
  and through TXA (panel select 3, copy 0), at 48 kHz, in each of
  positive/negative filter edges; confirm I and Q survive, which side is
  passed, group delay, and that output at the TX end is single-sideband.
  Any behaviour that differs from upstream
  (<https://github.com/TAPR/OpenHPSDR-wdsp>) is noted in this file.

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
  T-1, T-10 and a dummy-load spectrum check are done.
- **WDSP stages that were built for speech.** AGC (RX), and ALC and
  overshoot control (TX), act on a waveform whose envelope statistics they
  were not designed for. Whether the stock settings are acceptable or
  need RADE-specific values is a test result (T-9, T-10), not an
  assumption. We change settings through the existing `Set*` calls, not
  WDSP code.
- **Vendored vs upstream WDSP.** Local `wdsp/` may differ from
  <https://github.com/TAPR/OpenHPSDR-wdsp>. Behaviour we depend on (panel
  select/copy, bandpass sideband handling, slew) is recorded in 4.1 with
  the local source as the check, and upstream consulted when the local
  copy is unclear.
- **Sample-clock drift** between the radio's RX I/Q clock and the audio
  sink affects the RX speech FIFO.
- **V2 acquisition range and V1 tolerance of the shift** are as stated by
  the library constants (V1 +-100 Hz) but not yet confirmed to hold with
  our decimator in the chain.
- **Remote (client/server) operation** is deferred, as for diversity:
  keep the engine server-side and the state machine free of GTK so that
  it can be wired up later.
- **Diversity interaction.** The V1 pilot correlator works on raw radio
  I/Q ahead of WDSP and has its own 8 kHz front end; the native decoder
  works after WDSP at 48 kHz. They share the Invert/side idea but not a
  signal path. Keep them separate; share only the rate-change helper if
  it fits, and do not fork the code.
- **Mode plumbing.** Adding RADE V1/V2 as modes touches mode tables,
  profiles, filters, props and the remote protocol. Keep the list of
  touch points in this file as phase 2 starts.

## 10. Immediate next step

Phase 0: link `librade` + lpcnet/fargan behind a `RADE_NATIVE` make
option; a small `test/rade_native/` directory with T-0 (reference decode
of the existing clips), T-1 (`rade_tx` spectrum and peak statistics) and
T-11 (WDSP I/Q probe). All offline, none touching engine code. T-1 tells
us whether the one-sided I/Q premise holds; T-11 tells us whether the
WDSP chain carries complex RADE signals as section 4 assumes, before
anything in the RX/TX design depends on either.

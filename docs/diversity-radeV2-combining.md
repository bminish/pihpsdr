# RADE V2 diversity: what can be presented to the codec

> **Branch `test/radeV2-correlator`**, 2026-10-04. A reading of the rade_c V2
> receiver (`third_party/rade_c`, pinned `cc17222`) to work out what
> diversity-combined RF can be handed to it, and what a two-input receiver
> would involve. **Nothing here is measured** unless it says so. The scoring
> and findings (V2-F1 to V2-F3) are in
> [`diversity-radeV2.md`](diversity-radeV2.md); the tools in
> [`tools/radev2-scoring.md`](tools/radev2-scoring.md); V1 in
> [`diversity-rade.md`](diversity-rade.md).

## 1. What the receiver consumes

From `rade_rx_v2.c`, `rade_v2_ofdm.c`, `rade_dec_v2.c`:

- **The latents are raw.** `az_hat` is the DFT of the CP-stripped symbol at
  the 14 carriers, for the last two symbols: 14 x 2 x (re, im) = 56 floats.
  There is no equaliser, no channel estimate and no pilot in front of the
  decoder. `rade_core_decoder_v2` and FrameSyncNet (`rade_frame_sync`) both
  take that vector as it is. Which channels the networks were trained on is
  not in this repo: **unverified**.
- **Whatever is combined becomes "the channel".** The weight is applied
  before the receiver's BPF, AGC and CP sync, so the network sees the
  combined channel, not two antennas. That fits V2-F2 (the decoder reacts to
  the shape and dynamics of the channel, not only its SNR). It is a reading,
  not a proof.
- **The ISI budget is about 16 samples.** `rade_v2_ofdm_demod_frame` is
  called with `time_offset = -16`, so the DFT window starts 16 samples into
  the 32-sample CP. A path later than about 16 samples (2 ms) is outside the
  window. The 2 ms synthetic two-path of the calibration sits at that edge.
  A combiner that lengthens the combined impulse response spends this budget.
- **Slow trackers.** Timing, frequency and the even/odd frame-parity
  accumulators use `BETA = 0.999`, about 1000 symbols (20 s). `Ry_smooth`
  uses 0.95 (about 20 symbols, 0.4 s). The AGC averages over about 0.1 s and
  corrects +/-20 dB toward 0.707 RMS (V2-F1). The decoder is stateful (five
  GRUs), so a step in the weight is carried forward in its state.
- **The receiver filters its input.** `rade_rx_v2_init(rx, 1)` (what
  `rade_open` uses) puts a 101-tap BPF in front, about 975 Hz wide centred at
  1469 Hz. The off-carrier noise reference the V1 correlator measures is gone
  by the time the signal reaches the receiver. Any Rnn has to be measured
  before it, in the engine.
- **Observables the receiver gives for free:** `Ry_max` (the CP SNR estimate;
  biased on multipath, V2-F2), the FrameSyncNet metric, `data_symbol` (the
  aux bit; needs a station that sends aux bits), and the EOO metric.
- **The only known waveform** is the end-of-over frame: six identical
  symbols of Barker pilots at -8 dB (`rade_v2_ofdm_get_eoo`).

## 2. The relative channel is observable without a pilot

For two arms, per carrier: `y_k[c] = h_k[c] X[c] + n_k`, with the transmitted
`X[c]` unknown. Then `y1 y0*` / `|y0|^2` estimates `h1/h0` with `X` cancelling,
so no known symbols are needed. It is the V2 counterpart of the V1
cross-spectrum, and the Window and FSK/Digital references already work that
way across the passband.

A combiner that keeps arm 0's phase and has unit noise, with `R = h1/h0`:

```
az = (y0 + conj(R) y1) / sqrt(1 + |R|^2)
h_eff = sqrt(|h0|^2 + |h1|^2) * exp(j arg h0)
```

A single virtual antenna with the summed power and arm 0's phase. This is MRC.

A caveat that needs a test: arm 0's phase is jumpy through its own nulls
while the amplitude is propped up by arm 1. That is a channel the network
may never have seen. The reference phase is a choice to compare, not decide
here: arm 0, the stronger arm per carrier (phase jumps at crossovers), or a
smoothed reference.

## 3. A two-input `rade_rx_v2`

Everything after `az_hat` already takes one 56-float vector
(`update_frame_sync_decode`, `rade_frame_sync`, `rade_core_decoder_v2`;
`rade_dec_v2_test.c` feeds the decoder latents directly). The change is the
front end and a combiner. The decoder and FrameSyncNet are untouched.

| Stage in `rade_rx_v2_process` | Change for two inputs |
|---|---|
| BPF (`rade_bpf`) | One per arm; its state is per instance. Linear, so a scalar combine commutes with it |
| AGC (`compute_gain`) | One gain from the summed power, applied to both arms, so the arm ratio survives |
| `rx_buf`, `compute_autocorr` | Per arm; sum numerators and denominators across arms (a power-weighted MRC of the CP correlation) before `Ry_smooth`. Or hybrid: sync from the engine's existing scalar-combined stream |
| `detect_signal`, state machine, `delta_hat`, `freq_offset` | Unchanged, on the combined `Ry_smooth`. One offset serves both arms if the ADCs share a clock, which the single complex weight already assumes |
| `extract_symbol` | Same `delta_hat`, same `rx_phase`, applied to both arms (two `rx_i` buffers) |
| `rade_v2_ofdm_demod_frame` | Called per arm: `az0`, `az1` |
| **New: combiner** | Per carrier, `C01 += az1 az0*`, `C00 += abs(az0)^2`, `C11 += abs(az1)^2`, IIR over 5 to 25 symbols; form `R[c]`; output `az` |
| `detect_eoo` | On the combined time-domain symbol, or arm 0. The EOO pilots give a free `h_k[c]` snapshot: log it |
| `update_frame_sync_decode` and the decoder | Unchanged |

- **Noise term.** The demod has only the in-band carriers. Options: noise per
  arm from its own CP correlation (`rho/(1-rho)`, as rade's SNR estimate; it is
  biased on multipath, V2-F2, but only the ratio between arms is needed), or
  an Rnn measured pre-BPF in the engine and handed in. The dominant
  eigenvector of the 2x2 `C[c]` needs no noise estimate but is biased toward
  the noisier arm unless the noise is equal.
- **Differential delay** between the arms shows up as a linear phase across
  carriers and is absorbed by `R[c]`. Combining in the carrier domain adds no
  ISI: each arm keeps only its own channel's spread.
- **Cost.** Two BPFs (101 taps x 8000/s, about 0.8 M complex MAC/s each), twice
  the 14 x 128 DFTs (they already run twice per symbol, because the last two
  symbols are re-demodulated on every call) and the per-carrier accumulators.
  The decoder dominates and does not change. Decoder cost per extra candidate,
  if candidates are ever scored by decode: **not measured**.
- **Where it lives.** `rade_rx_v2` is C in a pinned submodule. Either carry a
  patch in the submodule, or copy the receiver into `test/diversity/devtools`
  for the experiment and leave upstream alone, as `radev2_probe` already reads
  receiver state directly. The copy is the lower-commitment start; upstream or
  fork is a decision for after the bound is known.
- **Limit.** It needs the decode in our process. With freedv-gui as the
  decoder only time-domain options remain (a scalar, or a short filter on
  arm 1), and a filter spends the 16-sample ISI budget.
- **Pre-release.** rade_c says the V2 waveform, weights and API may change
  without notice. The pin stays.

## 4. Options

| # | Presentation | Needs | Notes |
|---|---|---|---|
| 1 | Scalar weight, V2-aware band: the Window reference over 1062.5-1875 Hz, Rnn from the rest of the passband | Engine only | Window today: +0.01 to +0.03 \|aux\| on the captures |
| 2 | A few complex sub-band weights, or delay + phase, as a short filter on arm 1 | Engine, external decoder ok | Coherence bandwidth at 2 ms spread is about 500 Hz, so about 2 to 4 pieces over 812 Hz. Spends the ISI budget |
| 3 | Per-carrier combining in a two-input receiver (section 3) | Decode in our process | Exact MRC, no ISI cost. Per-carrier SNR is 11.5 dB below band SNR, so smooth adjacent carriers |
| 4 | Selection as the floor | Engine only | On T-009 (SSB QRM) selection beat every combiner by 0.04 to 0.10. Try MVDR with Rnn there before concluding selection is simply better |

## 5. Experiments, in order

Steps 1 (partly) and 5 (partly) have been run: see section 7.

Existing harness: `radev2_calib.py`, `radev2_iq.c`, `score_radev2`, `run_ref`.

1. **Oracle ladder** on the synthetic channels: scalar, 3 to 4 sub-bands,
   per-carrier oracle, with the reference phase varied. This is the bound. If
   the per-carrier oracle still loses to the better antenna on decoded loss,
   the decoder dislikes combined multipath and the answer is selection.
2. **Blind estimator against the oracle**: the price of having no pilot,
   averaging 5 to 25 symbols.
3. **EOO pilots as on-air truth** for `h_k[c]` on T-008 and T-009. Few points,
   but the only on-air truth for the relative channel.
4. **Two-input prototype** in devtools, scored on the four captures against
   Window and per-bin selection.
5. **Level and weight-step behaviour** against the AGC and CP sync.

Score on decoded measures (feature loss, \|aux\|); never CP SNR alone (V2-F2).

## 6. Carried over

No hang or timeouts, hold through fades, no sub-1% mechanisms
([`changes/settled-decisions.md`](changes/settled-decisions.md)). ADC1/ADC2 in
prose, indices stay 0-based. The engine never writes menu settings.

## 7. First results (oracle weights, decoder only)

Two tools, both in `test/diversity/devtools/py/`: `radev2_oracle.py` and
`radev2_steps.py`. They take the receiver out of the loop on purpose: a
synthetic transmission (rade_c's `wav/all.wav`) so timing is known, the
receiver's own input BPF, one AGC gain, its DFT (window 16 samples into the
CP, `phase_corr`), and the latents straight into `rade_dec_v2_test`. So this
is what the **decoder** makes of each presentation, with no sync and no
estimator. The gate: a clean transmission through that path gives loss 0.0815
(rade_c publishes 0.080 for the whole receiver). Loss is radae's
`distortion_loss`, lower is better; the channels are those of
`radev2_calib.py` (4 seeds, one speech sample, one two-path model).

### V2-F4: the decoder is hurt by steps in channel phase, not by gain

One antenna in AWGN, +4 dB, phase toggled between 0 and theta, loss against
0.129 unchanged (the +12 dB run agrees to within 0.01):

| theta | every 0.16 s | 0.32 s | 0.64 s | 1.28 s | 2.56 s |
|---|---|---|---|---|---|
| 30 deg | +0.010 | +0.005 | +0.001 | +0.001 | +0.001 |
| 90 deg | +0.670 | +0.204 | +0.066 | +0.024 | +0.020 |
| 180 deg | +4.754 | +3.497 | +1.516 | +0.763 | +0.404 |

A steady phase ramp costs nothing up to 1 Hz of rotation (+0.003), +0.018 at
2 Hz and +0.185 at 4 Hz. Gain steps of 3 dB cost +0.003 and of 6 dB +0.009,
at any rate tried. Steps here land on a frame boundary; **mid-frame steps
were not tried**, and the receiver's own CP tracker was out of the loop.

What it means for a weight: the phase of the combined channel can move
smoothly, up to about a hertz of rotation, or in steps of about 30 degrees,
and the gain can step freely. A 180 degree step, which is what the Invert
button and a Null/Sum swap do, costs the decoder seconds of audio.

### V2-F5: the oracle ladder

Mean loss, mpp (two-path, 2 ms, 1 Hz), selected rows. The full tables, with
awgn and flat, are what `radev2_oracle.py` prints.

| SNR | arm0 | arm1 | sel | scalar | perc | fir16 | eq | eq0 | ph0 |
|---|---|---|---|---|---|---|---|---|---|
| -2 dB | 0.470 | 0.422 | 0.576 | 0.337 | 0.309 | 0.308 | 0.215 | 0.518 | 0.416 |
| +2 dB | 0.269 | 0.271 | 0.589 | 0.248 | 0.224 | 0.213 | 0.149 | 0.344 | 0.259 |
| +6 dB | 0.208 | 0.192 | 0.512 | 0.228 | 0.211 | 0.201 | 0.121 | 0.322 | 0.182 |
| +12 dB | 0.148 | 0.143 | 0.427 | 0.212 | 0.185 | 0.174 | 0.103 | 0.218 | 0.139 |

`sel` is the better antenna by true channel power per 160 ms block; `scalar`
one complex weight for the band; `perc` one weight per carrier with arm 0's
phase kept and unit noise; `fir16` the same weights as a 16-tap filter on each
arm (what the engine could do without touching the decoder; the design is a
ridge fit that charges taps by distance from the centre); `eq` per-carrier MRC
with the channel phase removed (needs the true absolute phase); `eq0`/`ph0`
arm 0 alone with its true phase removed (`eq0` also weighted by |h|). Times a
rung beat the better antenna by more than 0.01, out of 28 scenarios:

| channel | sel | scalar | sub3 | sub4 | perc | fir16 | eq | strong | ph0 |
|---|---|---|---|---|---|---|---|---|---|
| awgn | 0 | 20 | 20 | 20 | 20 | 20 | 20 | 21 | 0 |
| flat | 0 | 16 | 16 | 16 | 16 | 16 | 28 | 0 | 0 |
| mpp | 0 | 11 | 7 | 9 | 12 | 17 | 28 | 0 | 4 |

What these say, sized honestly:

- **Hard selection loses to either antenna, by a lot** (0.41 to 0.60 against
  0.14 to 0.27 on mpp). It is V2-F4: each switch steps the channel phase by a
  random amount. The same for `strong`, which switches its phase reference.
  This is selection as an implemented switch, not the post-hoc per-bin
  selection `score_radev2` calls "sel", which picks among decodes that were
  never switched. The on-air T-009 finding (selection beat every combiner) is
  about the second kind and is not contradicted.
- **Combining keeps arm 0's phase and wins at low SNR, loses at high** (with
  these oracle weights held 160 ms; section 8 revisits it). `perc`
  beats the better antenna by 0.04 to 0.16 up to about +2 dB on mpp, is level
  at +4 to +6 and loses by 0.04 at +12 (0.185 against 0.143). That is V2-F2
  with per-carrier weights: it is not a scalar's limit, and it is not
  repaired by finer weights. Pinning to arm 0's phase puts arm 0's phase
  dynamics into the output even where arm 1 carries the signal.
- **A scalar is already most of it.** On mpp `scalar` against `perc`: 0.248 /
  0.224 at +2 dB, 0.266 / 0.233 at +4, 0.212 / 0.185 at +12. Per-carrier weights
  are worth about 0.03 over a scalar and sub-bands (`sub3`, `sub4`) fall in
  between or worse. On flat, where the channel is the same at every carrier,
  all of them are identical, as they should be.
- **No sign that a time-domain filter loses to combining after the DFT.**
  `fir16` against `perc` on mpp: 0.213 / 0.224 at +2 dB, 0.201 / 0.211 at +6,
  0.174 / 0.185 at +12; `fir8` and `fir32` scatter about the same (0.01 to 0.02,
  at the level the 4 seeds can separate). I expected the filter to spend the
  16-sample ISI budget (section 1) and it does not show, on this one model.
  Taken alone this says an engine-side filter pair, with both arms filtered,
  reaches what a two-input `rade_rx_v2` reaches at oracle weights. It does
  not say it will with estimated weights, nor with a longer delay spread.
  A filter on arm 1 alone with `conj(R)` is catastrophic (loss above 4 on
  mpp): `R` blows up where arm 0 fades. The bounded two-filter form is needed.
- **`eq` is the largest number and is not available.** Per-carrier MRC with
  the channel phase removed halves the loss (0.139 against 0.269 at +4 dB, flat;
  0.132 against 0.222 on mpp) and beats the better antenna in every scenario.
  It needs each arm's absolute phase, which with no pilot is unobservable;
  only `h1/h0` is. And phase removal on one antenna does not help: `ph0` is
  level with arm 0 and `eq0` (also weighted by |h|) is worse. So the gain is
  the combination of a steady phase with the fades filled in by the second
  antenna, not the phase alone. That leaves a question for later, not an answer:
  is there a reference phase, built from `R` and smooth, that gets part of the
  way from `perc` to `eq`?
- **Not measured:** a blind estimate of `R` in place of the true one (so every
  number above is the best case for its rung); the full receiver with its own
  sync; other delay spreads or fading rates; a second speech sample; any
  on-air capture.

### What this changes in the plan

- The engine already steps its weight by block. V2 tolerates gain steps but
  not phase steps over about 30 degrees: a V2 weight needs a **phase slew
  limit** (about 1 Hz of rotation, steps of 30 degrees or less) and Null/Sum
  and Invert are hostile to it. Not yet checked against what the engine does.
- Option 4 (selection as a floor) is not a floor unless the hand-over is
  phase-continuous. A fade between arms has to be done on the aligned
  combination (`conj(R)` on arm 1, gain 0 to 1), not between the arms.
- Option 2 (a filter pair in the engine) is not ruled out by the ISI worry,
  so the case for a two-input receiver rests on what it lets the estimator do
  (the EOO pilots, the decoder's own outputs), not on the combining.
- The blind estimator is section 8. The smooth reference phase (the question
  under `eq`) is still to do.

## 8. The blind estimator

`blind_R` in `radev2_oracle.py`: per carrier, an IIR average (tau symbols of
20 ms) of `y0*conj(y0)`, `y1*conj(y1)` and `y1*conj(y0)` over the latents,
`R = C01 / C00`, causal, no pilot and no knowledge of the channel. Variants:
`u` as it stands; `k` with the noise power taken off `C00` (the noise is
known here, which on the engine would be the pre-BPF Rnn measurement);
`e` the dominant eigenvector of the 2x2, which needs no noise estimate when
both arms' noise is equal, as it is in these tests; `scalar` pools the
carriers, `perc` one weight per carrier, `perc3` each carrier with its two
neighbours. Output is `perc`'s: arm 0's phase, unit noise. Weights update every
symbol unless a suffix says otherwise. Loss, 6 seeds, 7 SNRs from -2 to +12 dB,
one speech sample; "wins" is scenarios (of 42) beating the better antenna by
more than 0.01.

| rung (tau 6 unless shown) | mpp all | mpp <= +2 dB | mpp >= +6 dB | mpp wins | flat all | flat wins |
|---|---|---|---|---|---|---|
| better antenna | 0.252 | 0.346 | 0.166 | - | 0.313 | - |
| oracle scalar (true h, per frame) | 0.217 | 0.271 | 0.165 | 25 | 0.218 | 31 |
| oracle per carrier (true h, per frame) | 0.203 | 0.245 | 0.169 | 27 | 0.218 | 31 |
| blind scalar, noise known (`bscalar_k`) | **0.205** | 0.263 | **0.153** | **36** | **0.190** | **39** |
| blind scalar, noise not removed (`bscalar_u`) | 0.211 | 0.277 | 0.153 | 35 | 0.207 | 40 |
| blind scalar, tau 12 / tau 25 | 0.213 / 0.226 | 0.277 / 0.301 | 0.156 / 0.160 | 35 / 27 | 0.207 / 0.238 | 41 / 36 |
| blind per carrier (`bperc_k`) | 0.230 | 0.293 | 0.174 | 21 | 0.239 | 36 |
| blind per carrier + neighbours (`bperc3_k`) | **0.197** | 0.248 | 0.151 | 34 | 0.199 | 39 |
| blind `bperc3`, eigenvector (`e`, no noise estimate) | 0.208 | 0.258 | 0.164 | 30 | 0.196 | 38 |
| `bperc3_k` as a 16-tap filter pair, updated every frame | 0.198 | 0.250 | 0.152 | 33 | 0.198 | 41 |
| oracle `eq` (phase known; not available) | 0.138 | 0.173 | 0.108 | 42 | 0.145 | 42 |

In AWGN every combining rung, blind or not, is within 0.002 of the oracle
(0.115 to 0.117 against 0.145 for an antenna).

### V2-F6: a smoothed blind estimate beats the true channel

The blind weights are better than the oracle weights taken from the true
channel, on both fading channels and at every SNR from 0 dB up, including +12 dB
on mpp, where the oracle combination loses to the better antenna (0.161 against
0.143) and the blind one does not (0.136 to 0.140). The oracle per frame is
the best a weight can do at tracking the channel, so the oracle is not the
bound, and V2-F4 is the reason to suspect why: the decoder prefers a combined
channel whose phase moves slowly, and the true `h1/h0` moves fast near a fade
of arm 0. A power-weighted average (`C01/C00` is dominated by the moments arm 0
is strong) does not follow it there. That is a hypothesis; I have not isolated
it. What the data do show:

- there is a best smoothing: tau 6 symbols (120 ms) beats 12 (240 ms) and 25
  (500 ms) on both channels, so it is not simply "more smoothing is better";
- holding the oracle weight for longer costs it: flat scalar 0.222 held 1
  frame, 0.227 held 2, 0.269 held 4;
- this does not square with V2-F2's "a smooth per-sample weight does no
  better than one held per block". That was measured on the full receiver
  with a true-channel weight; this is the decoder alone. Not reconciled.

### How the weight is applied matters as much as how it is estimated

`bscalar_k_6`, loss (all SNRs), mpp / flat; better antenna 0.252 / 0.313:

| applied as | mpp | flat |
|---|---|---|
| updated every symbol (20 ms) | 0.205 | 0.190 |
| held 2 frames (80 ms) | 0.210 | 0.199 |
| held 4 frames (160 ms) | 0.220 | 0.230 |
| held 8 frames (320 ms) | 0.253 | 0.283 |
| interpolated between every 4th (needs the next estimate) | 0.205 | 0.187 |
| interpolated between every 8th | 0.211 | 0.190 |
| the same, a block late so it is causal: 2 / 4 / 8 frames | 0.212 / 0.226 / 0.245 | 0.203 / 0.221 / 0.272 |

Interpolation recovers everything a hold loses, but the interpolation that
does it looks at the next estimate; made causal (one block of delay) it is
about as bad as holding. So a weight that is updated slowly and late costs
0.015 to 0.04 against one updated each symbol, and a block of 160 ms or more
leaves little of the gain at 320 ms (mpp 0.253 against 0.252 for an antenna,
7 wins of 42 at 8 frames late). The engine's block is 171 ms and, per
`diversity-rade.md`, about 0.7 correlator updates per block: that is the h4
to c4 rows, not the first row.

### What the blind estimator says about the options

- **A scalar weight is enough.** Blind scalar (0.205 mpp, 0.190 flat) is
  within 0.01 of the best per-carrier variant on mpp and better than it on
  flat. Per-carrier weights need neighbour smoothing to work at all (`bperc_k`
  0.230 against `bperc3_k` 0.197) and then add 0.01 on mpp. The frequency
  resolution is not what pays; the smoothing in time is.
- **A two-input `rade_rx_v2` is not needed for the combining.** The filter
  pair designed from the blind weights equals the carrier-domain combine when
  its coefficients are updated every frame (0.198 against 0.197 on mpp, 0.198
  against 0.199 on flat), and falls behind when they are updated per 160 ms
  block (mpp 0.217 to 0.224, flat 0.233 to 0.242), for the same reason as the
  hold above. Neither beat a scalar by more than 0.01 on mpp. If the
  two-input receiver is built it is for what it can estimate with (EOO pilots,
  decoder outputs), not for the combining.
- **Noise knowledge is worth about 0.01 to 0.02.** `k` against `u`: mpp
  0.205 / 0.211, flat 0.190 / 0.207. The eigenvector form, which needs none,
  is no better than `u` on mpp. The engine measures the noise from the
  off-carrier part of the passband (pre-BPF), so `k` is available there.
- **The phase-reference question stays open.** `eq` (0.138) is still far
  ahead of every blind rung (0.197 to 0.205) and still needs absolute phase.

### What this does not show

Every rung here is at the oracle's own SNR, in a model where both antennas
have the same noise and share a clock, one speech sample, one delay spread,
the decoder without the receiver's sync, and the receiver's own timing and
frequency tracking out of the loop (V2-F4's steps would pass through them on
air). The estimator is not the engine's: it averages the latents' covariance,
where the engine correlates against a reference. No capture has been scored
this way.

### What this changes in the plan

- The engine's V2 reference should be a **scalar weight, estimated by a
  power-weighted average of the arm-1/arm-0 cross-spectrum over about 120 ms
  (6 symbols), with the noise taken off, applied to arm 1 at arm 0's phase,
  updated each symbol or as near as the correlator allows.** That is a
  testable specification, not a result: the next step is to implement it as a
  reference and score it on the four captures against Window.
- The update rate and latency matter more than anything about the weight's
  structure: from a block-held weight to one updated each symbol is worth
  about 0.015 to 0.04 here. Check what the engine's V1 and Window references
  update at, and whether the audio path can slew or has to hold.
- Experiment 2 (the blind estimator against the oracle) is done for a scalar
  and a per-carrier weight in the decoder-only path. Experiments 3 (EOO as
  on-air truth) and 4 (two-input prototype) are not needed to decide the
  combining; 3 remains the only on-air check of the relative channel.

## 9. On the four captures

The scalar reference of section 8, as a stream in `score_radev2`
(`--blind NAME=TAU,MODE[,hold]`; see `docs/tools/radev2-scoring.md`), against
Window as `run_ref` replays it (Flat, Min coherence 0.20, `--pace 20000`, as in
`diversity-radeV2.md`). Same capture, same bins, same decoder, the arms' own
receivers alongside. Decoded |aux| against the better antenna, over the bins
where either antenna has a V2 signal (14 to 60 bins; a difference under about
0.01 is not a ranking):

| capture | window | blind, tau 6, noise from guard bands (`k`) | tau 6, noise from the arms' CP correlation (`c`) | the same held per block, a block late | tau 12 (`c`) | tau 6, no noise (`u`) |
|---|---|---|---|---|---|---|
| T-008 `184835` | +0.019 | +0.013 | +0.018 | +0.023 | +0.022 | +0.006 |
| T-008 `185337` | +0.005 | -0.002 | +0.002 | +0.007 | +0.001 | +0.002 |
| T-008 `185725` | +0.009 | +0.016 | +0.014 | +0.014 | +0.019 | +0.001 |
| T-009 `192558` | +0.030 | +0.043 | **+0.067** | +0.065 | +0.065 | -0.003 |

Against per-bin selection with hindsight, T-009: Window -0.044, `c` -0.007
(`u` -0.076, the radio's weight -0.102). On T-008 `c` is within 0.005 of
selection on all three. Frame-sync confidence against the better antenna,
`c` against Window: +0.011 / +0.014, +0.010 / +0.007, +0.012 / +0.002 and
+0.054 / +0.024. The CP SNR estimate is higher for `c` on all four (+1.98,
+1.75, +1.10, +1.63 dB against Window's +1.76, +1.32, +0.93, +0.79), which
V2-F2 says not to read as quality.

What it says, sized honestly:

- **On T-008 the blind scalar is level with Window**, within 0.01 on every
  measure of every capture: a tie, and an unremarkable one, since a single
  antenna is already at |aux| 0.94 to 0.97 there, where the measure moves about
  0.01 per dB.
- **On T-009, the weak one with SSB QRM, it is clearly better**: +0.067 against
  the better antenna, 0.037 over Window, and 0.007 short of per-bin selection
  where Window is 0.044 short and the radio 0.102. The decoder-only test said
  the gain should appear where the antennas are weak and the channel is bad.
  This is the first on-air capture to show it, and it is one capture.
- **The noise term decides it.** No noise term (`u`) gives nothing (+0.006,
  +0.002, +0.001, -0.003): the two arms' noise differs and weighting as if it
  were equal is wrong. The guard-band measurement (`k`) works on T-008 and half
  works on T-009. These captures were taken with a 1000 to 2000 Hz passband
  (T-009 mirrored), so the guard bands (500-900 and 1950-2350 Hz) lie outside
  it and the estimator reads filter skirts: it read the noise as 2% and 6% of
  the in-band power on T-009 where the arms' own CP correlation says it is
  well over half. That `k` still gains there is the noise *ratio* between the
  arms doing most of the work. `c` takes the noise from each arm's own V2
  receiver (`p (1 - rho_cp)`), which needs no guard band and was the best
  mode on three of four. It needs a CP correlation per arm, which the engine
  does not have for V2; the decoder-only test used the noise as known.
- **Update rate hardly matters on these captures.** Held per block and applied a
  block late (the engine's best case), `c` is within 0.005 of per-symbol
  updates on all four, against 0.015 to 0.04 on the synthetic fading of section
  8. These channels fade slowly (about 1 Hz at most), so the block delay is
  small against them. It would matter on faster fading.
- **tau 3 is worse, 6 and 12 are equal** (T-009: `b3c` +0.033, `b6c` +0.067,
  `b12c` +0.065), as the decoder-only test found the optimum near 6 and
  not much worse at 12.

**Selection effects.** tau 6 came from the synthetic study, not from these
captures; 3 and 12 are shown. The noise mode was not: `k` was tried first, read
badly on T-009, the diagnostic above followed, and `c` was written after. All
four captures were scored with every variant. The T-009 figure especially
should be read as "with a noise term taken from the arms' own receivers, the
scalar reference does what the synthetic test said it should", not as a
measured +0.067. Not tested for significance.

### Status of the V2 reference

A scalar weight on arm 1 at arm 0's phase, estimated from a power-weighted
cross-spectrum over about 6 symbols with a per-arm noise term, ties Window on
T-008 and beats it on T-009. It is a reference in the replay tool, not in the
engine. What stands between it and the engine: the noise term (the arms' CP
correlation, or a guard-band measurement that the operator's passband
may not allow), the engine's update rate (section 8), and the phase-step limit
of V2-F4 against Invert and Null/Sum. Nothing here touches `diversity_menu.c`.

## 10. Noise and interference from the cyclic prefix, and MVDR

Section 3 listed what a combiner inside the receiver could do that one outside
cannot. This tests the one that matters most: measuring the **noise covariance
of the two arms from the data, with no guard band**, and combining against it.
`rade_v2_ofdm.c` and `rade_rx_v2.c` already hold the pieces (`compute_autocorr`
forms `D_cp`, `D_m` and `Ry`); `radev2_oracle.py` has them as `cp_noise`,
`blind_mvdr` and `mvdr_out`. Decoder-only path again (section 7), gate 0.0815.

**The measurement.** The cyclic prefix and the tail it copies carry the same
signal, so `x[n] - x[n+M]` over CP samples 16 to 31 (clear of a 2 ms delay
spread, as the demod's own window) holds only noise and interference. Its 2x2
covariance across the arms, halved, scaled to one DFT bin (x `M`, x `Fs/975`
because the receiver's filter band-limits it), is the in-band `Rnn`, with
the correlation between the arms. Checked on a synthetic stream against the
true value: the diagonal is within about 1% (with interference) to 20%
(thermal only, where a little signal leaks into the difference); the cross
term reads about 15% of the noise when it should be zero.

**The combiner.** `u = (1, R)` is the signal's direction at arm 0's phase and
`out = u^H Rnn^-1 y / sqrt(q)`, `q = u^H Rnn^-1 u`. `R` comes from the pooled
covariance of the latents with the noise covariance taken off: the whole 2x2
(`bcpf`, full) or the diagonal only (`bcpd`, which is mode `c` of section 9).
The cross term is the point: a coherent interferer puts correlation between the
arms that the diagonal reads as signal.

**The interference model.** White Gaussian, through its own flat Rayleigh
channel (0.5 Hz) to each arm, so its direction `g1/g0` differs from the
signal's; the SIR is in the signal band; thermal noise at the SNR shown. Two
paths for the signal as before (flat, mpp). 6 seeds. Loss, +12 dB thermal SNR,
the arms about equal (arm 0 shown):

| signal channel, SIR | arm 0 | scalar, true R, blind to the interferer | blind diagonal (`bcpd`) | **blind full 2x2 (`bcpf`)** | oracle MVDR (true R and Rnn) |
|---|---|---|---|---|---|
| flat, +6 dB | 0.386 | 0.252 | 0.270 | **0.174** | 0.204 |
| flat, 0 dB | 0.777 | 0.432 | 0.612 | **0.273** | 0.227 |
| flat, -6 dB | 1.574 | 0.968 | 1.500 | **0.420** | 0.258 |
| mpp, +6 dB | 0.300 | 0.239 | 0.256 | **0.175** | 0.210 |
| mpp, 0 dB | 0.650 | 0.455 | 0.655 | **0.242** | 0.265 |
| mpp, -6 dB | 1.553 | 1.130 | 1.668 | **0.410** | 0.317 |

(`bcpf` is tau 6 on the signal and 4 on the noise.) Loss above about 0.4 is not
speech. At 0 dB the antennas are 0.65 to 0.78 and a scalar weight from the true
channel 0.43 to 0.46: nothing the combiners of section 8 do reaches 0.25. The
full covariance does. At +4 dB thermal the same ordering holds with every loss
larger (flat, 0 dB: 0.830, 0.438, 0.629, 0.330, 0.273).

Without interference it costs little: thermal only, flat / mpp, +4 and +12 dB,
`bcpd` 0.163 / 0.183 and 0.133 / 0.137, `bcpf` 0.176 / 0.187 and 0.140 / 0.140,
the known-noise combiner of section 8 0.160 / 0.190 and 0.130 / 0.136. So the
CP-derived noise is as good as known noise (the engine could use `bcpd` for the
noise term of section 9 without a guard band), and the cross term costs about
0.01 when there is nothing to null.

**The noise has to be tracked as fast as the interference fades.** Signal tau 6,
noise tau varied, flat 0 dB, +12 dB thermal:

| noise tau (symbols) | 48 | 24 | 12 | 8 | 6 | 4 | 3 | 2 |
|---|---|---|---|---|---|---|---|---|
| loss, SIR 0 dB | 0.619 | 0.496 | 0.361 | 0.280 | 0.270 | 0.273 | 0.267 | 0.252 |
| loss, SIR -6 dB | 1.638 | 1.347 | 0.951 | 0.694 | 0.525 | 0.420 | 0.433 | 0.438 |
| loss, no interference, +4 dB | | | | | 0.164 | 0.176 | 0.183 | 0.211 |

Slow smoothing, the natural choice for a noise floor, is wrong here: the
interferer's direction fades at the same rate as the signal's. Tau 4 is where
the interference result stops improving and the thermal-only cost has not yet
grown.

What it says, sized honestly:

- **The cross term is the whole gain.** The diagonal version does no better
  than the antennas on strong interference and loses to the scalar at -6 dB.
  Oracle MVDR with only the diagonal of the true `Rnn` (`mvdr_od`) is 0.371 /
  0.795 / 0.777 against 0.227 / 0.258 with the full matrix at flat 0 dB, -6 dB
  and +12 dB thermal (flat, 0 dB, -6 dB).
- **Blind full gets most of the way to the oracle**, beats it at +6 dB and on mpp
  at 0 dB (smoothing again, section 8), and is 0.05 to 0.16 short of it at flat
  0 dB and -6 dB. The gap is the estimate of the signal's `R` when the signal
  covariance is a small difference of two large ones.
- **What this model leaves out**, and each is open: the interferer is white
  across the whole band and stationary, where SSB speech occupies part of the
  band and starts and stops; `Rnn` is taken as flat across the 14 carriers, so a
  narrow interferer would need a per-carrier one the CP difference cannot give;
  one interferer, so two arms can null it exactly; the signal's leak into the
  difference is a few percent here; no frequency offset in the synthetic; the
  decoder-only path has no sync, and the CP difference needs symbol timing.
- **Against the engine's approach.** For wideband stationary interference an
  engine that can see an empty part of the passband measures the same `Rnn`
  there with no symbol timing, and does not need a receiver. The CP route earns
  its place when the operator's passband has no empty part, as on the four
  captures (1000 to 2000 Hz), or when the interferer is inside the band only.
  Section 9's guard-band mode failed on exactly those captures.
- **Not tried on a capture.** T-009 has SSB QRM and is where it would show, and
  that needs the CP difference inside a receiver (a copy of `rade_rx_v2` in
  devtools with a second input) because `score_radev2` has the 8 kHz streams but
  not the symbol timing of each arm's receiver. That is the first build that
  needs the two-input receiver.

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

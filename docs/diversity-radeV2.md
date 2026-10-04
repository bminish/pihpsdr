# RADE V2 diversity: scoring, and a correlator to come

> **Branch `test/radeV2-correlator`**, cut from `feature/diversity-binaural-2`
> on 2026-10-03. Work on a RADE V2 reference for the diversity engine, and
> on a better RADE V1 correlator if the chance comes. This file tracks the
> V2 work. The tool details are in
> [`tools/radev2-scoring.md`](tools/radev2-scoring.md). What can be
> presented to the codec, and what a two-input receiver involves, is in
> [`diversity-radeV2-combining.md`](diversity-radeV2-combining.md). For V1
> see [`diversity-rade.md`](diversity-rade.md).

## Status

| Step | State |
|---|---|
| rade_c pulled in (`third_party/rade_c`, submodule, pinned `cc17222`) | done |
| A decode-based score for V2 (`score_radev2`) | done |
| The score calibrated against true feature loss (`radev2_calib.py`) | done, synthetic channels only |
| The four V2 captures (T-008, T-009) scored | done; results below |
| A V2 reference in the engine | not started |
| A better V1 correlator | not started |

## RADE V2, as rade_c has it

From `rade_v2_ofdm.h` and `rade_rx_v2.c` at the pin:

```
Fs   8000 Hz    modem sample rate
Nc   14         OFDM carriers, 62.5 Hz apart, 1062.5 .. 1875 Hz
M    128        samples per symbol
Ncp  32         cyclic prefix (4 ms)
Ns   2          symbols per modem frame: 320 samples, 40 ms
```

What matters for a correlator, set against V1:

- **No pilots.** V1 has a known pilot symbol every 120 ms, which is what
  the V1 correlator correlates against. V2 has none. The receiver gets
  timing, frequency and its SNR estimate from the cyclic prefix alone. A
  neural network (FrameSyncNet) picks the frame parity. The only known
  waveform is the end-of-over frame.
- **The CP frequency estimate is ambiguous by whole carrier spacings.** It
  measures phase over M samples, so it is unambiguous only over ±31 Hz.
- **Upright on USB.** In the tapped buffer the USB bank is mirrored, as
  for V1. The passband rule V1 uses (conjugate the USB bank) is the right
  one for V2 as well: on all four captures it decodes and the other sense
  decodes nothing.
- **The receiver wants its input near 0.7 RMS.** Its AGC corrects only
  ±20 dB. See V2-F1.
- **Pre-release.** rade_c's README says the V2 waveform, weights and API
  may change without notice and without backward compatibility. The model
  weights at the pin have been unchanged since 2026-05-02 and are
  identical to the ones in the local freedv-gui builds.

## The score

`score_radev2` decodes a capture several ways at once with rade_c's V2
receiver: each antenna alone, the weight the radio applied, and any
replayed weight series. It then compares them on the same 1 s bins.
Three measures, in the order the calibration trusts them:

1. **Decoded |aux|.** V2 carries one BPSK aux bit per frame through the
   neural codec, and the decoder's soft output for it sits near ±1 when it
   is sure. Usable only when the station sends aux bits; the tool checks.
2. **Frame-sync confidence.** FrameSyncNet's output on the latents that
   were decoded: the receiver's own learned judgement of whether they look
   like a RADE frame.
3. **CP SNR estimate.** rade's own estimate from the cyclic-prefix
   correlation. Available on every symbol, but see V2-F2.

Each is reported against the better antenna and against picking the
better antenna bin by bin with hindsight (selection). Mechanics, options
and the meaning of every column are in the tools doc.

### Calibration

A known transmission (rade_c's `wav/all.wav`, 50 s) through two synthetic
antennas with independent channels and noise: AWGN, flat Rayleigh (0.5 Hz)
and two-path (2 ms, 1 Hz, MPP-like). Nine SNRs from −4 to +12 dB, three
seeds each. In each case there are six candidate streams: each antenna,
and an ideal per-block MRC weight rotated 0°, 60°, 120° and 180°. The truth
is radae's `distortion_loss()` of every decoded frame against the
transmitted features. Frames never decoded are charged at the loss of
silence. The loss reproduces rade_c's published clean figure (0.0825 at
start 224, against 0.080 at 224).

How often each measure orders a pair of streams the way the loss does
(pairs the loss separates by at least 0.02), and how often it picks the
best of the six:

| Measure | Pairs ordered right | Best picked |
|---|---|---|
| decoded \|aux\| | 95.5 % of 1005 | 79.0 % of 81 |
| frame-sync confidence | 92.9 % | 63.0 % |
| CP SNR estimate (over sync) | 89.3 % | 66.7 % |
| detector-on share | 71.7 % | 45.7 % |
| sync share | 61.9 % | 30.9 % |
| decoded frames | 22.0 % | 11.1 % |

The question a diversity score exists for is narrower: is the combination
better than the better antenna? For the ideal combination (MRC at 0°):

| Channel | Truth: combination better | \|aux\| right | frame-sync right | CP SNR right |
|---|---|---|---|---|
| AWGN | 15 of 15 | 15 | 15 | 15 |
| flat Rayleigh | 19 of 22 | 20 | 20 | 19 |
| two-path | **6 of 24** | **23** | 17 | **8** |

What a difference in |aux| is worth: on the antennas alone, mean |aux|
against channel SNR.

| Channel SNR | AWGN | flat | two-path |
|---|---|---|---|
| −4 dB | 0.861 | 0.764 | 0.793 |
| 0 dB | 0.945 | 0.853 | 0.882 |
| +4 dB | 0.986 | 0.925 | 0.944 |
| +8 dB | 1.004 | 0.964 | 0.979 |
| +12 dB | 1.007 | 0.984 | 0.988 |

Around 0.94 to 0.98, where the captures below sit, 0.01 is very roughly
1 dB of channel SNR. Above about +8 dB it flattens and stops resolving.

**Limits of the calibration.** These are synthetic channels and one
speech sample. The synthetic aux bit is a constant −1, where on air it
carries text. The two-path model is one model. The "ideal" weight is held
per 171 ms block, as the radio applies it, and in two-path it is a
band-wide scalar that cannot equalise the second path. None of this has
been checked against a decode on air that someone listened to.

## Findings

### V2-F1: at the captured level, the decoder sees nothing, but sync is perfect

The 8 kHz stream out of the front end sits at 6e-4 to 1e-3 RMS, about
60 dB below the 0.7 rade_c's V2 receiver wants. Its AGC makes up only
20 dB. The CP correlation is normalised, so the receiver syncs as if all
were well (99 % on T-008) while the decoder is fed near-zero latents. The
signs are frame-sync confidence pinned at 0.49 (the sigmoid's midpoint),
the aux bit pinned at −0.30, and frame parity flipping (2011 frames in
60 s, against 1500 for a clean lock).

The first scores of these captures were taken that way, and they misled
me. The constant aux value looked like stations sending no aux bits, and
then like a different V2 model on air. Neither was true: scaled to
0.5 RMS, the same streams decode on T-008 with frame-sync 0.77 to 0.93 and
|aux| 0.90 to 0.97, and other on-air V2 recordings (EI6IZ; a 1984 kHz websdr
station) decode the same way. `score_radev2` now measures the level over
the first 10 s and applies one fixed gain to every stream (55 to 59 dB on
these four). Any future V2 work fed from the tap needs the same.

### V2-F2: on multipath, CP SNR prefers a combination the decoder does worse on

On the two-path channel the ideal combination raises the CP SNR estimate
by 1 to 2 dB over either antenna and still decodes worse in 18 of 24
cases. At +8 dB, for example, loss is 0.305 against 0.192. CP SNR gets
the comparison right in 8 of 24; |aux| in 23 of 24.

What it is not, as far as tested:

- Not the weight steps. A smooth per-sample weight does no better than
  one held per block.
- Not the output level. Normalised MRC, MRC with arm 0 pinned at unity
  (as the radio combines), and the same with the gain capped at 4 all
  still decode worse than either antenna at +4, +8 and +12 dB (three
  seeds each).

What was seen: the V2 decoder is sensitive to the *dynamics* of the
channel's phase, not only to its SNR. Taking one antenna's own channel
phase out of its signal, with level unchanged, improves its loss in flat
fading (0.343 → 0.283 at +4 dB) and worsens it in two-path (0.246 →
0.355). The likely reading is that a combined two-path channel has time
behaviour the network was not trained on. That is not established.

The consequence for this work: **CP SNR alone must not be used to choose
between combinations of a V2 signal**, and a V2 correlator that maximises
something like it could make the audio worse on exactly the channel
diversity is for. Score with decoded measures.

### V2-F3: the sideband rule needs nothing new

T-009 was logged as "the signal is inverted", and so needing a V2
correlator that un-mirrors it. It does not. In LSB, with the passband
rule as it stands (the LSB bank, as tapped), it decodes; the other sense
decodes nothing. The station was presumably transmitting on LSB, which
LSB reception undoes, as `diversity-rade.md` describes for V1.

## The captures

From [`test-findings.md`](test-findings.md) T-008 and T-009: two V2
stations in QSO on 40 m, on a path with a lot of multipath. On air the
radio ran the Window reference (T-008) and FSK/Digital (T-009), Sum,
0.42 s averaging. `window` and `digital` below are `run_ref` replays of
those references over the same capture (Flat, Min coherence 0.20 and
0.30, `--pace 20000`). `radio` is the weight the radio recorded as
applied.

Paired against the better antenna (the "sel" rows: against per-bin
selection), over the bins where either antenna has a V2 signal:

| Capture | bins | antennas \|aux\| | radio | window | digital |
|---|---|---|---|---|---|
| T-008 `184835` | 60 | 0.936 / 0.897 | +0.016 | **+0.019** | −0.000 |
| sel | | 0.953 | −0.001 | +0.001 | −0.018 |
| T-008 `185337` | 14 | 0.969 / 0.911 | −0.004 | **+0.005** | +0.004 |
| sel | | 0.977 | −0.012 | −0.003 | −0.003 |
| T-008 `185725` | 49 | 0.956 / 0.920 | +0.001 | **+0.009** | +0.007 |
| sel | | 0.968 | −0.011 | −0.003 | −0.005 |
| T-009 `192558` | 37 | 0.743 / 0.745 | −0.029 | **+0.030** | −0.027 |
| sel | | 0.819 | −0.102 | −0.044 | −0.101 |

The CP SNR estimate on the same bins, for comparison (dB against the
better antenna):

| Capture | radio | window | digital |
|---|---|---|---|
| `184835` | +1.57 | +1.76 | +0.53 |
| `185337` | +0.07 | +1.32 | +1.51 |
| `185725` | +0.56 | +0.93 | +0.27 |
| `192558` | +0.01 | +0.79 | +0.00 |

What these say, sized honestly:

- **Single antennas already decode these well.** On T-008, |aux| is 0.94
  to 0.97 on arm 0 alone. That is the region where the measure moves
  about 0.01 per dB, so the gains seen are about a dB at most.
- **Window is the best of the three, or tied, on every capture, by
  small margins.** The largest are +0.019 on `184835` and +0.030 on
  T-009. On T-008 it is level with per-bin selection, not clearly above
  it.
- **On T-009, under SSB QRM, selection beats every combiner clearly**
  (by 0.04 to 0.10). Which antenna has the station, second by second,
  matters more there than how the two are combined.
- **CP SNR overstates the combiners**: by up to 1.8 dB, where the
  decoded measures show a few hundredths. That is V2-F2 seen on air,
  though one cannot tell from a capture how much of it is the mechanism
  found on the synthetic channels.
- **Not tested for significance.** With 14 to 60 bins, a difference
  below about 0.01 should not be read as a ranking. The decoder is
  deterministic, and `run_ref` at `--pace 20000` replays byte-identically
  (tooling notes), so the scatter that matters is the signal's, not the
  tools'.
- On T-008, where the radio ran Window, the replay beats the weight the
  radio recorded on all three, by 0.003 to 0.009. The replay and the
  radio should agree; why they don't has not been followed up. `185337`,
  where the gap is largest, is 13 s long and the radio entered it with a
  held weight, while the replay starts cold.

## Next

1. **A V2 reference for the engine.** With no pilot, the candidates are:
   the cyclic prefix across the two arms (cross-arm CP correlation at the
   receiver's own timing, which gives `h1/h0` from the CP's redundancy
   without any known symbols); the end-of-over pilots, rare but known;
   and decision-directed estimation from the decoded latents. Each must
   be scored on decoded measures (V2-F2), against Window, which is the
   bar on these captures. The analysis of what the receiver can be handed
   (a per-carrier cross-spectrum also gives `h1/h0` without known
   symbols), the options, and the experiment order are in
   [`diversity-radeV2-combining.md`](diversity-radeV2-combining.md). Its
   first experiment, an oracle ladder from scalar to per-carrier weights,
   sets the bound before anything is built.
2. **Learn more about V2-F2** before a correlator optimises against it:
   whether it holds for a weight a real estimator produces, and for other
   multipath models.
3. **More captures**: a weaker V2 station, where single antennas fail and
   there is headroom for combining; and one off a single station without
   QRM.
4. **V1**, if the opportunity arises. `score_rade` scores V1 on sync and
   SNR only. It has not been checked for a level problem like V2-F1;
   V1's pilot-based equalisation should make it immune, but that is not
   measured.

# RADE V2 scoring tools

Local tooling (LT-021, never upstream). The tools that score a diversity
combination of a RADE V2 signal by decoding it. The work they serve, and
what they have found so far, is tracked in
[`../diversity-radeV2.md`](../diversity-radeV2.md).

| Tool | What it does |
|---|---|
| `test/diversity/devtools/score_radev2` | Decodes a `.divc` capture several ways at once (each antenna, the weight the radio applied, any replayed weight series) and scores each |
| `test/diversity/devtools/radev2_iq` | Decodes one 8 kHz I/Q file with the same probe and prints a one-line summary; the calibration's decoder |
| `test/diversity/devtools/py/radev2_calib.py` | Calibrates the score against true feature loss on synthetic two-antenna channels |
| `test/diversity/devtools/py/radev2_oracle.py` | The oracle ladder and the blind estimator: how well the decoder alone (no sync) does on each way of presenting two antennas, with the true channels and with an estimate from the latents, and with coherent interference (`<channel>_q<SIR dB>`) against noise measured from the cyclic prefix. See [`diversity-radeV2-combining.md`](../diversity-radeV2-combining.md) |
| `test/diversity/devtools/py/radev2_steps.py` | What a step or ramp in the channel phase, or a gain step, costs the decoder on one antenna |
| `test/diversity/devtools/radev2_probe.[ch]` | The shared probe: one rade_c V2 receiver that records every symbol |

## Building

The decoder is [rade_c](https://github.com/freedv/rade_c), the FreeDV
team's C port of RADE V1 and V2, as a submodule at `third_party/rade_c`
(pinned at `cc17222`, 2026-09-24). It is built on its own, with its own
CMake, and nothing in piHPSDR links it.

```
git submodule update --init third_party/rade_c
cmake -S third_party/rade_c -B third_party/rade_c/build -DCMAKE_BUILD_TYPE=Release
make -C third_party/rade_c/build -j$(nproc)
make -C test/diversity/devtools radev2        # radev2_iq, score_radev2
make -C test/diversity/devtools run_ref       # for weight series to score
```

The tools link `librade.so` from that build with an rpath, so they run
from anywhere without `LD_LIBRARY_PATH`. `score_rade` (V1) is separate and
still uses the librade from freedv-gui; see the devtools README.

**rade_c's V2 is pre-release.** Its README says the waveform, the model
weights and the API may change without notice, and future versions will
not be backwards compatible. A capture decoded with a different pin may
score differently, so the pin is part of every result. If rade_c moves
and the on-air stations move with it, old captures may stop decoding.

## Scoring a capture

```
score_radev2 captures/X.divc
score_radev2 captures/X.divc --weights window=w_band.csv --weights digital=w_dig.csv
```

Streams, each decoded by its own receiver:

| Stream | What is decoded |
|---|---|
| `arm0`, `arm1` | each antenna alone |
| `radio` | `arm0 + w·arm1` with the weight the radio recorded as applied (`live_cos`, `live_sin`), i.e. what the operator heard |
| `NAME` | from `--blind NAME=TAU,MODE[,hold]`: a stream whose weight is estimated from the two arms as it goes, no `run_ref` series needed. A scalar weight at arm 0's phase from a running cross-spectrum over TAU symbols, in the V2 band; MODE `c` takes each arm's noise from its own V2 receiver's CP correlation, `k` from two guard bands (500-900 and 1950-2350 Hz, useless if the passband has no empty part), `u` none (right only for equal noise); `hold` latches the weight at each block start, a block late. See [`diversity-radeV2-combining.md`](../diversity-radeV2-combining.md) |
| `NAME` | from `--weights NAME=FILE`: a weight series from `run_ref` or `replay_rade` (columns `wr`, `wi`, `ok`), one row per block, the same convention as `score_rade` |

To get weight series for the existing references, replay them with the
settings `TEST` uses (see the devtools README for why they are passed
explicitly):

```
run_ref captures/X.divc --ref band    --mode sum --weighting flat --cohmin 0.20 --pace 20000 --out w_band.csv
run_ref captures/X.divc --ref digital --mode sum --weighting flat --cohmin 0.30 --pace 20000 --out w_dig.csv
```

Other options:

| Option | |
|---|---|
| `--flip` | Use the other sideband sense (see below) |
| `--noise SIGMA` | Add independent noise to each arm, as `score_rade --noise` does, to walk a capture down towards threshold |
| `--bin S` | Width of the paired-comparison bins, default 1 s |
| `--csv-dir DIR` | One CSV per stream, one row per OFDM symbol: `t,sync,sig,valid,eoo,ry_max,snr,foff,data` |
| `--iq-dir DIR` | Each stream's 8 kHz I/Q as complex float32, which rade_c's `radae_rx --v2` and `radev2_iq` both read. Pipe through `radae_rx --v2 \| lpcnet_demo -fargan-synthesis` to listen |
| `--gain G` | Fixed input gain (linear) instead of the automatic one (below) |
| `--no-agc` | Turn off rade_c's V2 input AGC (on by default, as on air) |

### How the stream is made

The same way `score_rade` makes it for V1. The capture's two arms go
through the RADE V1 correlator's own NCO and polyphase decimator (by
`#include` of the generated correlator, so it is the shipping code), at
the recorded `frame_off`, down to 8 kHz complex. The streams are combined
there. Combining after the decimator is exact, because the weight is one
complex scalar per block and the decimator is linear. The V1 search runs
alongside and its answer is ignored.

**Input level.** The decimated stream sits at about 6e-4 to 1e-3 RMS,
about 60 dB below the 0.7 the V2 receiver expects, and its AGC corrects
only ±20 dB. At that level the receiver still syncs perfectly (the CP
correlation is normalised) while the decoder sees near-zero latents.
Frame-sync confidence then sits at 0.49 and the aux bit at −0.30, whatever
the signal. So a pre-pass over the first 10 s measures the louder arm,
and one fixed gain puts it at 0.5 RMS. The same gain goes to every
stream, which keeps their relative levels; the AGC does the rest. The
gain is printed (`# input gain`). If frame-sync reads 0.49 on everything,
suspect the level before anything else. See V2-F1 in the tracking doc.

**Sideband.** RADE V2 is transmitted upright on USB. The decimated
stream is mirrored on the USB bank (bank 1), so that bank is conjugated,
which is the rule `score_rade` uses for V1. The passband's midpoint picks
the bank, as on air. `--flip` inverts the rule. On all four captures so
far, the passband's rule decodes and `--flip` decodes nothing (0 symbols
in sync), so the sense is not in doubt on any of them.

## What it reports

Per stream, over the whole capture:

| Column | Meaning |
|---|---|
| `sync%` | Share of symbols with the receiver in sync. **It includes the receiver's 75-symbol (1.5 s) hangover**, during which it keeps decoding noise after the signal has gone |
| `sig%` | Share of symbols where the detector fired: CP correlation peak above 0.38 and not a sine. Unlike `sync%`, this has no hangover |
| `frames` | Decoder outputs (each four 10 ms feature vectors). **Not "more is better"**: a receiver in sync should emit one every 40 ms (25 a second); more than that means the frame-parity decision is flipping, and each flip emits an extra frame |
| `acq` | Acquisitions |
| `eoo` | End-of-over detections |
| `snr_sig`, `snr_sync` | Mean of rade_c's own SNR estimate (dB in 3 kHz) over `sig` and over `sync` symbols |
| `fsync` | Mean FrameSyncNet output over decoded frames (0..1): the receiver's learned judgement that the latents look like a RADE frame. 0.93 on a clean signal; 0.49 means the decoder is being fed nothing (check the level) |
| `\|aux\|` | Mean magnitude of the decoder's soft aux bit over decoded frames. V2 carries one BPSK bit a frame through the codec; near 1 means the decoder is sure of it |

The `# aux:` line says whether the aux bit is usable: the median |aux|
on the better antenna must exceed 0.7. A station that sends no
recognisable aux bits would fail it. So would a stream decoded at the
wrong level, which is why the input gain comes first.

Then the **paired comparison**, which is the score, for three measures
in the order the calibration trusts them: decoded |aux| (lead),
frame-sync confidence, and the CP SNR estimate. The capture is cut
into bins (1 s by default). A bin counts if either antenna alone has the
detector on for at least half of its symbols, which decides "a V2 signal
is there" without reference to any weight, so every stream and every run
is judged on the same bins. Over those bins:

| Column | Meaning |
|---|---|
| `mean` | Mean over the bins of the per-bin mean (a bin with nothing decoded scores 0 for \|aux\| and frame-sync) |
| `vs best` | Against the better antenna over the same bins |
| `vs select` | Against picking the better antenna bin by bin with hindsight. A combiner has to beat this to be worth more than an antenna switch |
| `bins won` | Share of bins where the stream beats both antennas |

### The three measures

**Decoded |aux| and frame-sync confidence** are read off the decoder's
own output, so they see what the decoder makes of the signal. That
includes things a signal measure cannot see (V2-F2). They are
reference-free: they need no knowledge of the speech, only that the
station sends aux bits (|aux|). Both flatten near their ceiling above
about +8 dB in 3 kHz, where they stop resolving differences.

**The CP SNR estimate.** V2 has no pilots. All the receiver knows about the signal comes from the
cyclic prefix: timing, frequency and SNR. The estimate is
`10·log10(ρ/(1−ρ))`, corrected for the band and fitted by the FreeDV team,
where ρ is the peak of the normalised CP autocorrelation, IIR-smoothed
with a time constant of about 0.4 s. It is available on every symbol,
whether or not the receiver is in sync, which makes a paired comparison
possible. Anything that degrades the received symbol relative to its own
cyclic prefix (noise, interference, delay spread beyond the prefix)
lowers it.

What it is not is a measure of decoded speech quality, and the two can
disagree. They do on multipath, where it ranks a combination against an
antenna wrongly 16 times in 24 (V2-F2). It is kept because it is
available on every symbol, in sync or not.

## Calibration

`py/radev2_calib.py --work DIR` answers one question: if the score
prefers one combination of the antennas to another, would the decoded
speech agree?

1. One known transmission: `lpcnet_demo -features` of rade_c's
   `wav/all.wav`, through `radae_tx --v2` (50 s).
2. Two synthetic antennas: independent channel and independent noise at a
   set SNR per antenna (dB in 3 kHz, as rade's estimator states it),
   with arm 1 at a random path phase. Channels: AWGN; flat Rayleigh at
   0.5 Hz Doppler; and two-path at 2 ms and 1 Hz Doppler (MPP-like).
3. Six streams per case: each arm, and an ideal maximum-ratio weight
   (from the known channel, held per 171 ms block as the radio applies
   it) rotated 0°, 60°, 120° and 180° off its correct phase. These are
   candidate answers that a weight estimator might give, with a known
   order of merit.
4. Each stream decoded with `radev2_iq`. The truth is radae's
   `distortion_loss()` (the one its ctests use) of each decoded frame
   against the transmitted features.

The truth itself was checked first. The clean decode of `wav/all.wav`,
clipped as rade_c's ctest clips it, gives a loss of 0.0825 at start 224,
against the 0.080 at 224 that rade_c's README publishes.

Two details of the truth matter:

- **Alignment by run.** The decoded output is cut into runs of frames
  40 ms apart, and each run is aligned on its own, as loss.py aligns a
  whole file. A stream that drops sync or flips parity is not contiguous.
  Placing each frame by its time stamp alone moves with the receiver's
  timing steps, and read 0.135 against 0.111 on the same clean decode.
- **Missing frames are charged.** `loss_all` charges each frame that was
  never decoded at the loss of silence (all-zero features), so a stream
  cannot look good by decoding less. `cover` is the share decoded.

Run time is about two minutes for the full sweep (486 decodes). `--quick`
is a smaller grid. Output: `DIR/calib.csv` (one row per scenario and
stream), `DIR/calib_bins.csv` (per-second probe SNR against frame loss),
and the summary tables on stdout, which are the ones quoted in the
tracking doc.

Results: see [`../diversity-radeV2.md`](../diversity-radeV2.md).

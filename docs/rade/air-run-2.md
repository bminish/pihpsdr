# RADE native: air run 2 - the ALC, strong and weak signals (28645 kHz, drive 20 %, 2026-10-16)

Sessions (git-ignored): `rade-captures/air/20261016-123010_28645kHz` (36 runs, the knee, two repeats interleaved),
`.../20261016-125913_28645kHz` (10 real weak runs); `weak.csv` / `weak.md` in the first (emulated weak signals; the
losses 26-38 dB x 3 noise seeds are the ones used; an earlier pass at 38-54 dB is in `weak_38-54.*`, mostly below sync).
Levels are `drive_db` around the defined level (V1 0.357, V2 0.504 into WDSP; 0 = the ALC just reached). One clip
(11.5 s), ANT2, drive 20 %, Kiwi fixed-gain profile at 40 dB, leveller off.

## Questions and answers

**Does hitting the ALC limit reduce the maximum SNR seen for a strong signal?** No: the received power rises to the
ceiling and then stops; it does not fall. Band-power SNR (independent of librade): V1 37.5 dB at 0 dB, 38.5 at +1.5 and
the same to +6; V2 41.0 -> 41.4 and flat. Two things do change above the knee: the crest factor drops (V1 4.7 -> 4.2 -> 3.4 dB,
V2 3.4 -> 2.8 dB) and **V1's own SNR estimate falls about 5 dB** (31.4 at 0 dB, 30.4 at +1.5, 26.3 at +3 and 26.4 at +6),
i.e. the ALC's clipping looks like noise at about -26 dB relative to the signal to librade. The decode does not notice:
V1 feature loss is 0.0185 at 0 dB and 0.018-0.019 at +3, +4.5 and +6. (V2's estimate is stuck at 18 dB from the estimator's
own ceiling, so it cannot show this.)

**Does hitting the ALC limit degrade a weak signal?** Not measurably; if anything past-the-knee levels do better, because
they carry the extra ~1 dB (V1) / ~0.4 dB (V2) of power that the ALC still allows, and nothing is lost to the clamp. At
the same noise floor and the same path loss, feature loss is lower or equal from +1.5 dB upward than at 0 dB, and +3 to +6
dB are equal to each other within the scatter (below). Where it matters most, near the decoding threshold, +3/+4.5 dB beat 0 dB:
V1 at 34 dB loss: 0.0458 (0 dB), 0.0298 (+1.5), 0.0251 (+3), 0.0242 (+4.5), 0.0247 (+6); V1 at 38 dB loss (SNR about 0):
0.098, 0.062, 0.046, 0.036, 0.059 (means of 6; medians 0.048, 0.049, 0.037, 0.036, 0.039).

**Where is the sweet spot for a weak signal?** At the ALC's limit and a little past it: **+1.5 to +3 dB** (V1 and V2). The
received power stops rising at +1.5 dB (V1 12.5 dB SNR at 26 dB loss for +1.5, +3, +4.5 and +6 alike; V2 15.2), so nothing is gained
beyond that; the decode figures at +3 to +6 are the same or marginally noisier, and there is no benefit to going further.
Below the knee every dB lost costs a dB of SNR and the decode degrades at the threshold. Suggested working level: **+3 dB**
(V1 0.357 -> 0.504, V2 0.504 -> 0.712 into WDSP), which is on the plateau with margin, or +1.5 dB for the least clipping. **Not
changed in the code**; say if you want it.

## Strong signals (real, run 1; 2 runs per cell, interleaved)

| Wave | level dB | TX peak dBFS | TX rms | crest dB | band SNR (2 runs) | librade SNR est | feature loss | EOO / call (of 2) |
|---|---|---|---|---|---|---|---|---|
| V1 | -6 | -6.6 | 0.28 | 4.7 | 30.2/30.1 | 27.6 | 0.0185 | 2 / 2 |
| V1 | -4.5 | -5.0 | 0.33 | 4.7 | 31.9/32.0 | 29.0 | 0.0228 | 2 / 2 |
| V1 | -3 | -3.5 | 0.39 | 4.7 | 33.8/33.9 | 29.9 | 0.0193 | 2 / 2 |
| V1 | -1.5 | -2.0 | 0.46 | 4.7 | 35.7/35.9 | 30.9 | 0.0192 | 2 / 1 |
| V1 | +0 | -1.0 | 0.55 | 4.2 | 37.3/37.6 | 31.4 | 0.0185 | 2 / 2 |
| V1 | +1.5 | -1.0 | 0.60 | 3.5 | 38.5/38.6 | 30.4 | 0.0242 | 2 / 2 |
| V1 | +3 | -1.0 | 0.60 | 3.4 | 38.5/38.6 | 26.3 | 0.0188 | 2 / 2 |
| V1 | +4.5 | -1.0 | 0.60 | 3.4 | 38.5/38.5 | 26.2 | 0.0182 | 2 / 2 |
| V1 | +6 | -1.0 | 0.60 | 3.4 | 38.4/38.7 | 26.4 | 0.0184 | 2 / 2 |
| V2 | -6 | -6.7 | 0.31 | 3.4 | 33.1/33.6 | 18.0 | 0.0335 | 2 / n/a |
| V2 | -4.5 | -5.2 | 0.37 | 3.4 | 35.2/35.3 | 18.1 | 0.0145 | 2 / n/a |
| V2 | -3 | -3.7 | 0.44 | 3.4 | 37.0/37.2 | 18.1 | 0.0151 | 2 / n/a |
| V2 | -1.5 | -2.2 | 0.52 | 3.4 | 39.0/39.3 | 18.1 | 0.0151 | 2 / n/a |
| V2 | +0 | -1.0 | 0.62 | 3.2 | 41.0/41.0 | 18.1 | 0.0317 | 2 / n/a |
| V2 | +1.5 | -1.0 | 0.65 | 2.8 | 41.2/41.5 | 17.9 | 0.0161 | 2 / n/a |
| V2 | +3 | -1.0 | 0.65 | 2.8 | 41.4/41.5 | 17.8 | 0.0159 | 2 / n/a |
| V2 | +4.5 | -1.0 | 0.65 | 2.8 | 41.2/41.4 | 17.9 | 0.0145 | 2 / n/a |
| V2 | +6 | -1.0 | 0.65 | 2.8 | 41.3/41.5 | 17.3 | 0.0157 | 2 / n/a |

TX peak sits at -1.0 dBFS (the ALC ceiling at the 192 kHz output) from 0 dB up. The call is decoded in all but one V1 run in the
knee (V1 -1.5 dB, one of two).

## Weak signals, emulated (V1; V2 below)

Method and its limits: `air_weak.py` (see `air-tests.md`): the strong fixed-gain recording scaled down by the path loss, white
noise added to the Kiwi's measured noise in the modem band, decoded; 2 runs x 3 noise seeds = 6 per cell. "SNR now" is the
band SNR after the loss.

### V1, path loss 26 dB
| level dB | SNR now | sync % | EOO ok | call ok | feature loss (mean / median) |
|---|---|---|---|---|---|
| -6 | 4.2 | 99 | 6/6 | 0/6 | 0.0369 / 0.0267 |
| -4.5 | 5.9 | 100 | 6/6 | 1/6 | 0.0258 / 0.0249 |
| -3 | 7.8 | 100 | 6/6 | 5/6 | 0.0213 / 0.0215 |
| -1.5 | 9.7 | 100 | 6/6 | 3/6 | 0.0210 / 0.0209 |
| +0 | 11.3 | 100 | 6/6 | 6/6 | 0.0197 / 0.0198 |
| +1.5 | 12.5 | 100 | 6/6 | 6/6 | 0.0238 / 0.0233 |
| +3 | 12.5 | 100 | 6/6 | 6/6 | 0.0198 / 0.0196 |
| +4.5 | 12.5 | 100 | 6/6 | 6/6 | 0.0189 / 0.0190 |
| +6 | 12.4 | 100 | 6/6 | 6/6 | 0.0190 / 0.0190 |

### V1, path loss 34 dB
| level dB | SNR now | sync % | EOO ok | call ok | feature loss (mean / median) |
|---|---|---|---|---|---|
| -6 | -3.8 | 60 | 1/6 | 0/6 | 0.1908 / 0.2041 |
| -4.5 | -2.1 | 95 | 4/6 | 0/6 | 0.1164 / 0.0721 |
| -3 | -0.2 | 99 | 5/6 | 0/6 | 0.0599 / 0.0420 |
| -1.5 | 1.7 | 99 | 6/6 | 0/6 | 0.0422 / 0.0324 |
| +0 | 3.3 | 99 | 6/6 | 0/6 | 0.0458 / 0.0278 |
| +1.5 | 4.5 | 100 | 6/6 | 0/6 | 0.0298 / 0.0275 |
| +3 | 4.5 | 100 | 6/6 | 0/6 | 0.0251 / 0.0251 |
| +4.5 | 4.5 | 100 | 6/6 | 0/6 | 0.0242 / 0.0242 |
| +6 | 4.4 | 100 | 6/6 | 0/6 | 0.0247 / 0.0246 |

### V1, path loss 38 dB
| level dB | SNR now | sync % | EOO ok | call ok | feature loss (mean / median) |
|---|---|---|---|---|---|
| -6 | -7.8 | 0 | 0/6 | 0/6 | - |
| -4.5 | -6.1 | 33 | 0/6 | 0/6 | 0.2111 / 0.2111 |
| -3 | -4.2 | 78 | 1/6 | 0/6 | 0.1744 / 0.1867 |
| -1.5 | -2.3 | 95 | 2/6 | 0/6 | 0.2343 / 0.0819 |
| +0 | -0.7 | 97 | 6/6 | 0/6 | 0.0981 / 0.0479 |
| +1.5 | 0.5 | 99 | 6/6 | 0/6 | 0.0620 / 0.0491 |
| +3 | 0.5 | 99 | 6/6 | 0/6 | 0.0459 / 0.0372 |
| +4.5 | 0.5 | 100 | 6/6 | 0/6 | 0.0360 / 0.0355 |
| +6 | 0.4 | 99 | 5/6 | 0/6 | 0.0592 / 0.0392 |

### V2, path loss 30 dB
| level dB | SNR now | sync % | EOO ok | call ok | feature loss (mean / median) |
|---|---|---|---|---|---|
| -6 | 3.1 | 100 | 0/6 | n/a | 0.0392 / 0.0381 |
| -4.5 | 5.2 | 100 | 2/6 | n/a | 0.0363 / 0.0311 |
| -3 | 7.0 | 100 | 5/6 | n/a | 0.0298 / 0.0295 |
| -1.5 | 9.0 | 100 | 6/6 | n/a | 0.0249 / 0.0210 |
| +0 | 11.0 | 100 | 6/6 | n/a | 0.0201 / 0.0187 |
| +1.5 | 11.2 | 100 | 6/6 | n/a | 0.0206 / 0.0192 |
| +3 | 11.4 | 100 | 6/6 | n/a | 0.0233 / 0.0197 |
| +4.5 | 11.2 | 100 | 6/6 | n/a | 0.0299 / 0.0274 |
| +6 | 11.3 | 100 | 6/6 | n/a | 0.0214 / 0.0200 |

### V2, path loss 34 dB
| level dB | SNR now | sync % | EOO ok | call ok | feature loss (mean / median) |
|---|---|---|---|---|---|
| -6 | -0.9 | 100 | 0/6 | n/a | 0.0575 / 0.0570 |
| -4.5 | 1.2 | 100 | 0/6 | n/a | 0.0485 / 0.0427 |
| -3 | 3.0 | 100 | 0/6 | n/a | 0.0394 / 0.0347 |
| -1.5 | 5.0 | 100 | 2/6 | n/a | 0.0334 / 0.0330 |
| +0 | 7.0 | 100 | 6/6 | n/a | 0.0311 / 0.0285 |
| +1.5 | 7.2 | 100 | 6/6 | n/a | 0.0331 / 0.0333 |
| +3 | 7.4 | 100 | 6/6 | n/a | 0.0275 / 0.0246 |
| +4.5 | 7.2 | 100 | 6/6 | n/a | 0.0266 / 0.0253 |
| +6 | 7.3 | 100 | 6/6 | n/a | 0.0316 / 0.0256 |

(All the other losses and V2 26 / 38 dB are in `weak.md`.)

## Thresholds, from the same tables

| | sync holds to | end-of-over frame reliable from | V1 callsign reliable from |
|---|---|---|---|
| V1 | about 0 dB band SNR (97 % at -0.7, falls apart below -4) | about +1.7 dB (6/6 from 1.7; 2/6 at -2.3) | about +8.5 dB (6/6 at 8.5; 5/6 at 7.8 and 5.7 is 1/6) |
| V2 | about -1 dB (100 % at -1.0, 98 % at -2.8, none at -4.9) | about +6 to +7 dB (6/6 at 7.0; 2/6 at 5.0; 5/6 at 7.0 in another set) | n/a (no V2 callsign text yet) |

## Does the emulation agree with reality? (run 2: real weak runs, 1 per cell)

| Wave | level dB | band SNR | librade est | feature loss | EOO | call |
|---|---|---|---|---|---|---|
| V1 | -48 | - | - | - | no | no |
| V1 | -42 | - | -6.7 | 0.2097 | no | no |
| V1 | -36 | -1.5 | -5.6 | 0.0548 | yes | no |
| V1 | -30 | 4.7 | 0.8 | 0.0259 | yes | no |
| V1 | -24 | 10.9 | 7.6 | 0.0206 | yes | yes |
| V2 | -48 | - | - | - | no | n/a |
| V2 | -42 | - | - | - | no | n/a |
| V2 | -36 | 1.6 | -2.6 | 0.0438 | no | n/a |
| V2 | -30 | 8.2 | 5.6 | 0.0229 | yes | n/a |
| V2 | -24 | 38.5 | 10.6 | 0.149 | yes | n/a |

Compared at the same band SNR: V1 real -24 dB (SNR 10.9, loss 0.0206, EOO yes, call yes) against emulated 9.7-11.3 dB (loss 0.0197-0.0210);
V1 real -30 dB (4.7, 0.0259, call no) against emulated 4.2-5.9 (0.026-0.037, call 0-1/6); V2 real -30 dB (8.2, 0.0229, EOO yes) against
emulated 7.0-7.2 (0.027-0.031, EOO 6/6); V2 real -36 dB (1.6, 0.0438, EOO no) against emulated 1.2 (0.0485, EOO 0/6). They agree; the
scatter at the threshold is large (V1 at SNR about -2: real loss 0.055, emulated 0.08-0.23). **The V2 -24 dB real run is not usable:** its band
SNR reads 38.5 dB where about 11 would be expected, so something else was in the band during that over (interference).

## Open points

- One clip, one band path and one afternoon; the Kiwi's noise and the band change. The thresholds above are for this path (white noise, no fading); fading will
  move them, and the ALC question under fading is not answered.
- Emulation is additive noise only; the feature loss numbers at the threshold scatter (means pulled up by single bad runs: compare the medians).
- The PA and its linearity are not in any of this (the TX output measured is before the PA); the over-the-air splatter was not measured. Before
  the local TX capture the far-out regrowth was -80 dB (V1, +3 and +6 dB; earlier session).
- The mic limiter was acting on the clip's peaks throughout (see the first run's write-up).
- The over-the-air delay is still not measured.

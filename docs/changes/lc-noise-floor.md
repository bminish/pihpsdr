# The noise floor and Best

Each antenna's noise floor measured across frequency (LC-025) and what is built on it: the Sum noise ratio, Best's per-arm SNR and its switching rule, and a cheaper way to compute it. Measurements in [test-noisefloor.md](../test-noisefloor.md); the plan in [noise-floor-refactor.md](../noise-floor-refactor.md).

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-025"></a>

## LC-025 — Each arm's noise floor, measured across frequency

**Why.** Maximum-ratio combining wants N0/N1. The Window and Carrier
references took it from a minimum over time, which needs the band to go
quiet: on a signal with no gaps it publishes nothing (the old "branch
noise ratio" known gap), and on a fading carrier a ratio of two fades is
published as a ratio of noises (Finding 47: +10.5 dB where the truth was
−0.35).

**Change.** `div_noise_floor_update()` takes each arm's floor every block
from the bins outside the RX filter (and outside a hand-placed window
wider than it, plus 1 kHz of skirt), no further than 20 kHz either side
of the dial, within the central 80 % of the DDC span: the 8th-12th
percentile of up to 1024 bins, smoothed over 2 s. The Window/Carrier Sum
weight takes N0/N1 from it; the temporal minimum stays as the fallback
where too few bins remain. `diversity_auto_noise_floor()` reads it for
the harness and, later, the attenuator calibration
(`docs/feature-att-calibration.md`).

**Measured** (`docs/test-noisefloor.md`):

- The estimate: within about 0.5 dB of the guard-band truth on every
  capture checked; `test_rates` reads +10 dB as +9.81 / +10.04 / +10.04
  at 48 / 192 / 1536 kHz through 40 strong carriers.
- Sum over 39 Window/Carrier captures: +0.50 dB against the better
  antenna (+0.27 with the temporal minimum), ahead on 29. Weaker where
  both antennas hear one noise source (`154822`, 0.99 coherent): the
  ratio weight is optimal only for uncorrelated noise.
- On a crowded 40 m band (T-013 to T-016) the models are within 0.5 dB
  on three captures; on the lopsided pair (T-013) it beats the temporal
  minimum by 1.3 dB.
- The ±20 kHz limit is neutral on 45 captures (192 kHz +0.02 dB, 48 kHz
  identical) and keeps 1536 kHz from sampling ±614 kHz.

**Limitation.** Not a covariance: common noise both antennas hear is not
cancelled. Two attempts to do that were measured and dropped.

---

<a id="lc-028"></a>

## LC-028 — Best: each arm's SNR from the floor, and a 2 dB, 1 s switch

**Change.** `div_arm_from_floor()` takes the noise from LC-025's floor,
scaled by the bins actually summed (notches allowed for), the temporal
floor as fallback. Best changes antenna only when the other leads by
more than 2 dB (`DIV_BEST_HYST_DB`, was 1) continuously for 1 s
(`DIV_BEST_DWELL`).

**One change, not two.** The floor gives a readout on nearly every
block, and with 1 dB hysteresis two near-equal antennas changed places
every few blocks (−17.97 dB on `154822`, arm 1 on 56 % of blocks). The
dwell on `TEST`'s temporal floor, measured on its own on 45 captures,
was a coin toss: better on 17, worse on 17, −4.53 dB worst on the guard
score and −17.9 dB on one in-band.

**Measured** together, against LC-025 alone, on the 45 Window/Carrier
captures: guard score +0.58 dB, better on 29, worse on 12 (worst −3.62,
best +4.26); in-band level on average (−0.10 dB), better on 20, worse on
18, worst −10.13. Best is still the mode most likely to pick the worse
antenna.

**Depends on** LC-025.

---

<a id="lc-033"></a>

## LC-033 — The noise floor selects its percentile band instead of sorting

**Why.** `div_noise_floor_update()` (LC-025) ran `qsort` over up to 1024
bin powers per arm per block, only to average order statistics 8 % to
12 %. The sorts were most of the floor's cost. This is step 5 of
`docs/noise-floor-refactor.md`.

**Change.** Two selections (Hoare's FIND with a median-of-three pivot)
fence the band, and only the band (about 40 values) is
insertion-sorted. It is then summed smallest first, as before, so the
result is the same bit for bit. `div_nf_cmp` goes.

**Measured.**
- **Same answer.** Bit-identical on 32 292 synthetic cases (`bench_nf`,
  LT-016: nine data shapes from plain noise to presorted, n = 128 to
  1024). On all 127 captures, replayed in Sum and in Best, the
  `run_ref` output is identical (254 replays). RADE V1 never runs the
  floor and needs `--pace 20000` to replay repeatably under load (see [tooling.md](tooling.md#lt-005)).
- **Cost in place** (a `run_ref` with the call timed on its thread, 61
  captures that run the floor, i7-12700K):
  - At replay pacing on the `powersave` governor (a worker that wakes,
    works and sleeps, as in the radio): median 554 → 121 µs per block
    (4.6×). That is about 0.65 % → 0.14 % of a core at 11.7 blocks per
    second.
  - With the core kept busy: about 100 → 20 µs.
  - Every reference and rate falls between 4.4× and 4.8×.
- **Microbenchmark** (`bench_nf`, per arm): 49 → 9 µs at 1024 values
  (5.4×), 40 → 7 µs at 850. Up to 11× only on presorted input.

Not measured on a Raspberry Pi. The plan's "factor of 5-10" holds at the
low end on real data.

---

<a id="lc-036"></a>

## LC-036 — Best's per-arm SNR from the mean noise, and one arm clear is enough

**Why.** `div_arm_from_floor()` (LC-028) took `nbins × div_nf` as the
noise in the window. `div_nf` is the 8th-12th percentile, 9.77 dB below
the mean noise per bin. So:
- a window of bare noise read 9.3 dB of SNR per arm and passed
  `DIV_ARM_MIN_DB`, and the readout was valid on 98 % of blocks across
  the capture set, noise or not;
- real differences were compressed: 6 dB read as 2.2 dB at 0 dB arm
  SNR.

This was Finding 56's defect, found in the 2026-10-02 evaluation of the
findings' open list.

**Change.**
- `DIV_NF_MEAN_FRAC` (0.1055) scales the floor to the mean noise before
  it is used as an absolute level. The constant is exact for
  exponential bin powers at 1024 samples, and within 0.2 dB at 128-850.
  The Sum ratios divide one floor by the other, so it cancels there and
  they are untouched.
- One arm, not both, must clear `DIV_ARM_MIN_DB`. A buried arm is
  credited `DIV_ARM_BURIED_DB` (−10 dB), so the readout becomes a lower
  bound on the other arm's lead.
- The stored floor is never scaled. `div_nf0`/`div_nf1`, the Sum ratios
  and `diversity_auto_noise_floor()` all keep the raw low percentile,
  which stays the measurement because the stations on a busy band
  cannot reach it. The conversion happens only where the floor is
  subtracted from a summed window power, and is named there
  (`mean_noise0/1`, the fixup). It is a fixed multiplier, so it lets no
  signal in. Real band noise follows the statistics it assumes: on
  `122843`'s outside bins the median sits 8.16 dB over the 10th
  percentile, against 8.17 for pure noise. With the floor corrected, requiring
  both arms silenced Best where one antenna is buried: a weak antenna
  or a dead port (Finding 56), which is the case Best exists for.

**Measured.** In Best, on the 45 Window/Carrier captures, against
`TEST` (`ab.py` guard and in-band, relative to the better antenna):

| Variant | Readout valid | Guard (mean) | Better / worse | In-band (mean) |
|---|---|---|---|---|
| `TEST` | 98.0 % | | | |
| Constant only, both arms must clear | 68.4 % | −0.022 dB | 4 / 11 | −0.107 dB |
| **LC-036: constant, one arm must clear** | 76.7 % | **+0.088 dB** | **3 / 2** | +0.006 dB |

- `122843` (ADC2 15 dB noisier): +6.0 dB in-band, +1.5 dB guard. `TEST`
  sat on the noisier arm 62 % of the minute on noise-biased readings;
  LC-036 sits there 13 %.
- `233616` has no carrier at all. `TEST` read it valid on 46 % of blocks
  and switched on noise; LC-036 never does (+3.0 dB guard). Its −6.8 dB
  in-band figure has no signal behind it.
- `142333` (−0.6 dB guard) never switches in any build. The loss is the
  held weight: while the readout is invalid Best holds the weight it is
  slewing, so the start's 1 + 0j lingers longer. That is item 33 of
  the open list, not a wrong pick.
- `test_rates` (LT-019): bare noise is invalid, a +3.91 dB lead reads
  +4.09, and a buried arm 0 gives a valid +17.9 dB. All three fail on
  `TEST`; the last two also fail with the constant alone.
- Replays of the committed code match the scored variant on 45 of 45
  captures.

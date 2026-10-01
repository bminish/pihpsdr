# The noise-floor work: findings to date

**Status: work with merit, parked for a revisit.** Branch
`test/noise-floor` (from `TEST` at `0dfc870c`); the plain port is also on
`wip/lc-025-noise-floor`. Nothing here is on `TEST`. How to listen to the
branch is in `docs/eval-noise-floor.md`.

Some of it is a clear improvement and could go towards `TEST` on its own
merits. The central question - whether a *correct* noise ratio makes what
the operator hears better - is not settled, because the two ways we have
of scoring it disagree. That is what the revisit has to answer.

## Why we started

Upstream gained a fast noise-floor estimate for the panadapter
(`b77d4237`: a percentile across frequency, driving automatic AGC gain and
panadapter low). That prompted the question of whether diversity had a
better way to estimate noise too. It does, in principle:
`feature/auto-diversity`'s `8a393217` measures the noise across frequency
in the same spirit, but per antenna and on the raw streams.

Two things in the Window and Carrier references need each antenna's noise
level:

- **Sum** turns the noisier antenna down in proportion to its noise
  (maximum-ratio combining needs N0/N1).
- **Best** chooses the antenna with the better signal-to-noise ratio.

`TEST` takes both from the quietest the in-window power has been over the
last 5 to 10 seconds. That is a limitation of the method, not a coding
bug: it assumes there was a moment with no signal. On a signal with no
gaps it gives no answer (Sum then treats the antennas as equally noisy);
on a fading signal the minima land in the fades, so it publishes a ratio
of two fades (the feature branch measured +10.5 dB where the truth was
−0.35, and Best on the wrong antenna 95.6 % of the time). It cannot tell
"signal absent" from "signal faded".

## What was built

| Piece | What it does | Lines (approx.) | State |
|---|---|---|---|
| Across-frequency floor | Each antenna's noise from the bins outside the RX filter, every block: mean of the 8th–12th percentile of up to 1024 bins, smoothed over 2 s. Time minimum kept as fallback | 95 | Keep |
| Sum noise model (eval control) | Old (time minimum) / Ratio (outside-filter floor, default) | 35 (menu, props) | Evaluation only; marked `PORT-TO-TEST: remove` |
| Calmer Best | Changes antenna only after the other has been > 2 dB better (was 1) for 1 s | 12 | Keep |
| Level output (eval tick) | Combined output held at arm 0's passband level, recomputed whenever the weight is written; not in Null or RADE V1 | 60 + 2 multiplies per sample | Keep |
| Covariance (outside filter) | MVDR against the noise covariance from the outside-filter bins | — | Removed: dead end |
| Gap covariance (in band) | In-band noise covariance from flat (signal-free) blocks, Sum solved against the signal covariance | — | Removed: negative result |

## What was measured

All on the 39 Window and Carrier captures, `run_ref` driving the real
engine, each weight applied one block late. Figures are dB against the
better antenna alone unless stated.

### The estimate itself: right

Within about 0.5 dB of the noise ratio measured in the guard band beside
the passband, on every capture checked (seven, including the worst
scorers). `test_digital`'s 20 dB-noisier case: +16.66 → +29.97 dB SINR,
closing the "branch noise ratio" known gap.

### Sum, Old against Ratio: the two scores disagree

| | Guard-band score | In-band score |
|---|---|---|
| Old | +0.27, ahead on 25/39 | −0.38, ahead on 15/35 |
| Ratio | **+0.50**, ahead on 29/39 | −0.83, ahead on 18/35 |
| Ratio − Old | **+0.23**, better 23 / worse 11 | **−0.46**, better 13 / worse 19, worst −7.96 |

- The **guard-band score** (`score_wideband.py`, Finding 18) takes the
  noise from a band beside the passband. Finding 18 adopted it for
  captures *without* dead air.
- The **in-band score** is signal blocks against the noise heard between
  overs, inside the passband - a rough version of Finding 6's
  voice-against-quiet split, which the measurements doc prefers where
  there is dead air. Rough: "noise" blocks are picked by a simple power
  threshold, and the weight differs between signal and noise blocks.
- On `154822` Old's guard-band lead of 4.5 dB shrinks to about 1 dB
  in-band: the noise there is 0.99 shared between the antennas beside the
  passband but only 0.44 inside it. On `122632` and `235906` Old still
  wins clearly in-band.

Where Old wins, it is not doing anything deliberate. Its noise estimate is
wrong, and on those captures the wrong weight lands nearer one that
cancels some noise both antennas share. The ratio weight is optimal only
for unrelated noise, and gives that cancellation up.

### Two attempts at cancelling shared noise: both negative

- **Covariance from the bins outside the filter** (+0.32 on the guard
  score, against Ratio's +0.50). Those bins show about zero correlation
  between the antennas on every capture, even where the band beside the
  passband is 0.64–0.99 correlated, so it could not see what it was meant
  to cancel; and choosing the quieter bins skewed the ratio on a lopsided
  pair (`122119`: −4.2 dB against −8.5). Removed.
- **Gap covariance, in band.** Gaps found by spectral flatness (mean over
  median of per-bin power under 1.6), which separates cleanly on most
  captures but passes `122632`'s flat signal as gap, and finds no gaps at
  all on AM or busy captures (falls back to Ratio). Without a margin it
  steered to noise (`154822` −7.92); with a 6 dB margin it is level with
  Ratio: −0.15 guard, −0.05 in-band, better on 8–9, worse on 13. It
  recovers part of `122632` in-band (−3.26 against −8.21) and loses 7.45
  on `151241`. Removed 2026-10-01 (`4635df1b`); the code is in
  `6da9b6b3` if a revisit wants it back.

### Best: clearly better

`TEST` −0.73 → branch −0.37, better on 23 captures, worse on 10. The plain
port, with a per-arm SNR on every block and 1 dB hysteresis, collapsed to
−17.97 dB on `154822` by switching antennas on 56 % of blocks; the 2 dB,
1 s rule removes that. Worst remaining: `185337` −3.62, `143952` −2.76.

### Level output: clearly better, and no effect on SNR

It multiplies the output by one number, so the signal-to-noise ratio is
untouched; it only removes the level change the combiner causes.

| (median over 39 captures) | Louder than one antenna | Worst 10 % | Blocks jumping > 3 dB |
|---|---|---|---|
| Sum, `TEST` | +2.07 dB | +7.40 | 793 |
| Sum, branch | −0.08 dB | +0.03 | 784 |
| Best, `TEST` | +8.85 dB | +22.11 | 1175 |
| Best, branch | 0.00 dB | 0.00 | 774 |

`test_window`: a Sum that raised the level +4.18 dB comes out at 0.00 dB;
Null is untouched. It closes the "Level output" known gap. Because it
follows within about a second, it brings the background between overs
back to arm 0's level whatever the weight does - so by ear, judge
readability during a transmission, not the background level.

### Cost

`bench_cpu`, Window at 192 kHz, per 85 ms analysis block: `TEST` 0.55 ms,
the branch without the dropped Covariance 0.72–0.77 ms - about +0.2 ms,
0.2 % of one core, mostly two sorts of 1024 values. Carrier similar;
FSK/Digital, CW and RADE V1 unchanged. (Gap covariance, now removed,
cost a further sort per block while selected.) About 180 lines of `src/` for the
pieces worth keeping, excluding the evaluation controls.

## Upstream's panadapter floor as a cross-check

Same principle (a percentile across frequency), different job:

| | Upstream `rx->noise_floor` | Ours |
|---|---|---|
| Looks at | What that receiver displays; with diversity on, RX1 is the *combined* output | Each antenna's raw stream |
| Where | Whole display span, signals included | Only outside the RX filter |
| Statistic | Nudged until 10–30 % of pixels lie above it (≈ 70th–90th percentile) | Mean of the 8th–12th percentile |
| Updates | Each display frame, only while drawn | Every analysis block, display or not |
| Units | Calibrated dBm | Uncalibrated; only the ratio is used |

It cannot replace ours, and captures do not record it. On the air it could
check two things - the noise ratio (diversity off: RX1's floor minus
RX2's) and Level output (RX1's floor should not rise when diversity comes
on) - but upstream shows the value nowhere except through the automatic
AGC line and panadapter low (5 dB steps), too coarse to read. A small
evaluation readout of both floors, our ratio and the Level output gain
would make it usable. Not built.

## Found on the way

- Upstream initialises `auto_div_sin = 1.0` beside `auto_div_cos = 1.0`
  (`radio.c`), so until the loop's first answer the automatic weight is
  1+j (+3 dB, 45°) while the status reads 0 dB / 0°. Probably a typo for
  0.0; a one-line upstream fix.

## For the revisit

1. **Settle the scoring before settling Ratio.** Build a proper
   voice-against-quiet in-band scorer (Finding 6's method, not the rough
   threshold used here) and report both scores for every comparison.
2. **Listen.** Old against Ratio in SSB and AM, quiet antenna on ADC0 and
   noisier on ADC1, Level output on; and once with a local noise source on
   both antennas, where Old may sound better. Capture each.
3. **Shared noise is the real gap.** Neither cancelling attempt worked.
   If it matters on the air, the noise has to be measured close to the
   signal and the solve kept off the signal (Finding 1's guard bins
   inside the passband are a starting point), or Null used for it.
4. **Calmer Best and Level output stand on their own** and could be taken
   towards `TEST` without the Ratio question being settled - Best does
   need the per-arm SNR on every block that the new floor supplies, or a
   check that the dwell rule is harmless with the old one.
5. **The panadapter readout** (above), if an on-air cross-check is wanted.
6. **Drop the evaluation controls** before anything moves to `TEST`.
   Gap covariance is already gone. The Sum noise selector is marked
   `PORT-TO-TEST: remove` at every place it lives (`git grep
   PORT-TO-TEST`): Ratio becomes the only model, with the time minimum
   as its fallback. Level output, the calmer Best and the floor are
   kept.

## Commits on `test/noise-floor`

| Commit | What |
|---|---|
| `05015522` | The across-frequency floor, ported from `8a393217` |
| `16c18851` | Sum noise model control, Best dwell, Level output |
| `85200203` | Tooling: `run_ref --sumnoise`, a `norm` column, `score_level.py` |
| `4a3d8536` | `docs/eval-noise-floor.md` |
| `92808b17` | Covariance removed; comments checked against the code |
| `6da9b6b3` | Gap covariance prototype (negative result) |
| `abaaeef4` | This document |
| `fb61d6f7` | Menu: Level output on the top row, greyed when inactive; Sum noise beside Invert |
| `4635df1b` | Gap covariance removed (reverts `6da9b6b3`) |
| `5792117c` | The Sum noise selector marked `PORT-TO-TEST: remove` |

Hashes as of the rebase onto the new `TEST` on 2026-10-01; they change
whenever the branch is rebased again. The pre-rebase branch is kept as
`history/backup/test-noise-floor-pre-rebase-20261001`.

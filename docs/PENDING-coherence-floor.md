# PENDING: the Min coherence floor is too low, and only the menu applies it

Status: **open on this branch.** Fixed on `TEST` as LC-012 (commit
`5d8d9580`, "Diversity: the coherence gate never goes below its own noise
floor"). See `docs/changes.md` on `TEST` for the full entry. Recorded
here on 2026-09-30 so the finding isn't lost; bring the fix across (or
rebase onto `TEST`) before relying on the slider's floor.

## What is wrong here

`diversity_auto_coh_floor()` (from `01df2313`, "a threshold with a floor")
sets the bottom of the Min coherence slider to the coherence two
uncorrelated noises reach 1 % of the time, `1 - 0.01^(1/(N-1))`. It has
three faults.

1. **It counts every FFT bin as an independent sample.** The analysis
   window is a 4-term Blackman-Harris (`div_make_window()`), whose
   neighbouring bins are strongly correlated for white noise:

   | bins apart | 1 | 2 | 3 | 4 |
   |---|---|---|---|---|
   | correlation ρ | 0.816 | 0.439 | 0.150 | 0.030 |

   The effective number of independent samples over n bins is
   n² / Σ|ρ(j−k)|², which is about **n / 2.76** on a wide window. The
   code uses n, so N is overstated 2.76×, and the floor is too low.

2. **It assumes steady state.** It counts the exponential average as
   (2−α)/α blocks. On the first blocks after a reset, retune, reference
   change or averaging change, the average holds only one or two
   blocks. That's exactly when a noise block is most easily taken for a
   signal. The engine can track the true count as (Σw)²/Σw² as it runs.

3. **Only the menu enforces it.** `diversity_auto_clamp_cohmin()` is
   called from `update_coh_range()` in `diversity_menu.c`. The four
   gates in `diversity_auto.c` compare against `div_auto_coherence_min`
   directly. So with the menu closed, or when the filter width, the
   occupied span or a client's settings change, nothing holds the
   threshold at the floor.

## What it costs

Monte Carlo: two independent white noises through the same window and
exponential average, with the gate set at this branch's floor (1 %
target, rounded up to 0.1 %). Share of blocks that pass:

| Case | Floor here | Passes, overall | Passes, steady state | Target |
|---|---|---|---|---|
| Carrier, 5 bins, 2 s | 2.0 % | 17.1 % | 11.0 % | 1 % |
| Carrier, 5 bins, 0.2 s | 17.9 % | 12.7 % | 12.5 % | 1 % |
| Digital, 200 Hz occupied, 0.2 s | 5.5 % | 16.8 % | 16.6 % | 1 % |
| Window, 2.4 kHz, 2 s | 0.1 % | 7.6 % | 1.5 % | 1 % |
| Window, 2.4 kHz, 0.2 s | 0.5 % | 17.5 % | 17.4 % | 1 % |

"Overall" includes a reset every 500 blocks. So an operator who takes
the slider to its bottom gets **random tracking on roughly one block in
six to eight**, where the control implies one in a hundred. The Window
case at 2 s shows the start-up effect alone: 1.5 % in steady state, but
7.6 % once resets are included.

The shipped defaults (0.20 Window, 0.30 Carrier and Digital) sit well
above this floor on the Window and Digital references, so the defaults
mostly aren't affected. The Carrier reference is: at the 0.30 default it
passes noise on 1.9 % of blocks at 0.2 s averaging, and 0.8 % at 2 s,
almost all of the latter just after resets.

## The fix, as done on TEST (LC-012)

- The floor uses the Blackman-Harris correlation above for the bin count,
  and the running (Σw)²/Σw² for the block count.
- **The engine enforces it at every gate**: it compares against
  max(setting, floor over the bins and blocks actually in that block's
  estimate). RADE V1 is exempt, because it gates on its pilot.
- The target is 0.1 %, which at about twelve blocks a second is one
  noise pass a minute at the floor. With the corrected floor, measured
  pass rates are 0.02–0.27 %.
- The menu's slider steps in 0.5 % and its bottom follows the floor on
  the status tick. The operator's setting is never overwritten; it
  reappears when the floor falls.
- The floor is capped at 0.5. On the Carrier reference at short
  averaging, a weak carrier then needs up to 0.5 coherence before the
  loop acts. Longer averaging lowers it.

## Reproduce

`docs/tools/coh_floor_mc.py` (numpy, a few minutes) recomputes the
correlations and pass rates for the corrected floor. The table above
came from the same simulation with this branch's formula substituted:
`N = nbins * (2-alpha)/alpha`, PFA 0.01, rounded up to 0.1 %.

## Still to measure

The simulation covers noise only. How often the corrected floor holds a
*real* weak signal that the old one would have tracked should be scored
with `run_ref` on the capture set, particularly Carrier captures at
short averaging.

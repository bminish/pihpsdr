# Engine, combiner and threads

Changes to how the two arms are fed, combined, held and reset, outside any one reference.

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-007"></a>

## LC-007 — Hold stays on until the operator releases it

**Why.** A held weight is something an operator may want to keep on
purpose:

- to **hold a null on local interference** (a noisy appliance, a
  neighbour's PLT) while retuning or changing mode; or
- to **hold a peak that favours one direction**, instead of letting the
  loop follow whatever happens to be strongest.

Upstream releases Hold as soon as the Diversity menu is closed, which
makes both impossible.

**Change.** Hold now survives closing the menu, retuning and mode
changes. It is released only by the Hold button, or by switching
diversity off and on. `radio_set_diversity()` releases it only on a
real change of state. The server calls that function for every
diversity command a client sends, including manual gain and phase
moves, and those must not release Hold. The Hold button shows the real
state when the menu opens, and follows it if Hold is released from
outside the dialog.

**Trade-off.** This reverses a deliberate upstream choice. Upstream
released Hold because there is no indicator for it outside the dialog,
so a forgotten Hold stops the loop with nothing on screen to explain it.
If upstream resists, an indicator outside the dialog would answer that
objection.

---

<a id="lc-022"></a>

## LC-022 — Arm 0 follows the ADC the operator set RX1 to

Ported from `4299eb6d` (`feature/auto-diversity`); its capture-format
part is LT-011.

**Problem.** The combiner forms z = z0 + w·z1 with arm 0 at unit gain,
and every way the loop gives up resolves to w = 0: arm 0 alone. Both
protocols force ADC1 to DDC0 and ADC2 to DDC1 while diversity runs, so
arm 0 was always ADC1. An operator on ADC2 with nothing on ADC1 got a
dead arm 0 when they enabled diversity: 8.79 s of a minute at 26.4 dB
below the live antenna on capture `112712` (Finding 56).

**Change.** `div_arm_swapped()` reads `receiver[0]->adc`, which the
forced mapping otherwise leaves inert, and `rx_add_div_iq_samples()`
exchanges the pair on the way in, ahead of the analysis and of both the
manual and the automatic combine. Each protocol's raw feed to RX2 swaps
too, so RX2 still shows the other antenna. Read live; a move is in the
analysis context and restarts the statistics.

**Note.** With RX1 on ADC1 nothing changes. With RX1 on ADC2, the RX
menu's ADC control now has an effect while diversity is on, including on
which antenna the manual weight applies to. It decides which port the
loop fails towards; it does not stop it failing deaf (Finding 56's
guard is not ported).

---

<a id="lc-023"></a>

## LC-023 — The transmit gap and reset requests stop racing the threads

The fault part of `67b211e3` (`feature/auto-diversity`).

**Problem.** `diversity_auto_gap()` runs on the GTK thread, from
`rxtx()`, and zeroed `fillptr`, the protocol receive thread's fill
position, so the store could land mid-block. `reset_requested` was a
test-and-clear flag, which loses a request raised between the worker's
read and its clear.

**Change.** Both are generation counters bumped by the GTK thread. The
sample path restarts its block and counts the gap itself on its first
sample after a change, exactly on the boundary; the worker compares the
reset counter with its own copy. `rxtx()` signals the gap on both edges,
from the top of the function, so samples that arrive between the RX→TX
call and the stream stopping are discarded before post-TX samples join
them.

**Not taken.** The feature commit also rewrote the analysis queue as a
lock-free ring with a semaphore. With the gap on the sample thread the
mutex has no third writer, so that is an optimisation, not a fix.

---

<a id="lc-027"></a>

## LC-027 — An operator reset clears the statistics on the worker

**Problem.** `diversity_auto_reset()` runs on the GTK thread (menu
callbacks, `rxtx()`) and called `div_reset_stats()` directly. That zeroes
more than the transform accumulators the worker only adds to: the
smoothed noise floors, the noise ratio and the key-detection minimum,
with their valid flags, which the worker reads and writes in place.
Zeroed under a worker half way through an update, a floor could be
rebuilt from zero and marked valid, and a wildly wrong noise ratio
smoothed into the Sum weight over seconds.

**Change.** It only bumps `reset_gen`; the worker calls
`div_reset_stats()` with `rade_corr_reset()` before its next block, as
LC-023 arranged for the correlator. With diversity stopped nothing is
lost: `diversity_auto_start()` resets everything itself.

**Checked.** Replays with recorded resets are bit-identical.
`test_rates` (LT-013) fires resets from another thread every 0.7 ms and
finds the floor valid after all 47 blocks; with the old code, after none.

---

<a id="lc-030"></a>

## LC-030 — Level output: the combined output held at one antenna's level

**Why.** `receiver.c` forms z0 + w·z1 with arm 0 at unit gain, so the
output is louder than one antenna by whatever the weight does: a median
+2.1 dB in Sum, and +20 dB the moment Best hands over to arm 1. That rise
is not signal, and an AGC hears it as the band getting louder.

**Change.** `div_norm` scales the output back to arm 0's level over the
passband, from the passband powers and cross-power smoothed at 1 s and
the weight in force, recomputed whenever the weight is written, clamped
to −40..+6 dB. Not in Null or on RADE V1; 1.0 whenever the engine is
stopped. "Level output" on the menu's top row turns it on and off
(default on, saved), greyed whenever it is not acting: diversity off,
Manual, Null, RADE V1, or a remote client.

**Measured.** One multiplier, so SNR is unchanged. Median over 39
captures (`score_level.py`): Sum +2.07 → −0.08 dB over one antenna, Best
+8.85 → 0.00 (ninetieth percentile +22.1 → 0.00); Best's block-to-block
steps over 3 dB 1175 → 774. `test_window`: a Sum that raised the level
+4.2 dB comes out at 0.00, Null untouched.

---

<a id="lc-035"></a>

## LC-035 — Comments name the ADCs ADC1 and ADC2, as the hardware does

**Change.** ADC0 → ADC1 and ADC1 → ADC2 in every diversity comment:
our own files, and the diversity comments of ours in `radio.c`,
`receiver.c` and `client_server.c`. Two header comments that called
arms ADCs (`div_auto_arm_db`, `_pick`) now say arm 0 / arm 1. Comments
only. Upstream's own non-diversity text is left alone (see [open-items.md](open-items.md#flagged-for-a-later-patch)). Its line dependencies are textual only: it rewords
comments other LCs added.

---

<a id="lc-037"></a>

## LC-037 — The weights start at unity, not 1 + 1j

**Problem.** `radio.c` initialised both weight pairs to `cos = 1.0`,
`sin = 1.0`:
```c
double man_div_cos = 1.0;   double man_div_sin = 1.0;
double auto_div_cos = 1.0;  double auto_div_sin = 1.0;
```
That is +3.0 dB at 45° on arm 1, while the gain and phase declared
beside them are 0 dB and 0°.

**How it happened.** `3d3bb23b` (2019-07-25, "gain+phase rather than
I+Q") replaced the rotation arrays `i_rotate[2] = {1.0, 1.0}`,
`q_rotate[2] = {0.0, 0.0}` (unity) with scalars and wrote the Q factor
as 1.0. It was masked for seven years: switching diversity on
recomputes cos and sin from gain and phase (`radio_calc_div_params()`),
and the props file restores a consistent pair. `4865d602` (2026-09-27)
split the weight into manual and automatic halves. The manual half is
still masked. The automatic half is not recomputed or saved, so it
started at 1 + 1j.

**Effect.** A degraded starting weight, nothing more. From program
start until the loop writes its first weight, the combiner applies
1 + 1j instead of 1 + 0j, and the automatic readout says 0 dB / 0°
while it does:
- On Window, Carrier and FSK/Digital the first solve replaces it within
  seconds.
- On RADE V1 it stands until the first lock, but it does not delay the
  lock: the correlator works on the two raw arms, not on the combined
  output.
- In Best it stands while the antenna readout is invalid.

The tracked readout, seeded from `auto_div_gain` by `div_reset_stats()`,
was wrong in the same way.

On a fading HF channel a non-optimal fixed combination of two antennas
usually costs little against the better one alone. It was worth fixing
because it is wrong and costs two characters, not because it was
measured to cost much. It has not been measured.

**Change.** `man_div_sin = 0.0` and `auto_div_sin = 0.0`, matching the
gain and phase beside them.

**Why nothing caught it.** The harness and `run_ref` start their own
weight at 1 + 0j and don't link `radio.c`. Found by reading the code.

**Checks.** Applies to bare `upstream/TEST` and builds; reverts from the
tip; the suite passes. For dl1ycf: see
[menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

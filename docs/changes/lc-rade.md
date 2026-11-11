# RADE V1

The pilot correlator: how a lock is found, held and replaced, and how it is shown. LC-010, LC-011 and LC-014 were built to go together; LC-011 must never be taken without LC-010.

Part of the local-change register: [changes.md](../changes.md).

> **2026-10-15:** every change in this file is in `upstream/TEST`, except any listed as deferred or dropped in the [register](../changes.md#register): LC-001 to LC-047 since `f05546e3` (2026-10-08) and LC-048 to LC-058 since `07ae1f56` (2026-10-15). The text below is the record of what each is and why; "goes upstream", "for a PR" and "Local" in it describe the state before that. See [upstream-reconcile.md](upstream-reconcile.md).

---

<a id="lc-010"></a>

## LC-010 — RADE resyncs on a detection, not on a timeout

Ported from `567edf07` on `feature/auto-diversity` (`src/rade_correlator.c`
only; it applied without changes).

**Problem.** The correlator only searched for a pilot when it was not
tracking, and tracking was cleared only when Hang expired. So Hang did
more than hold the weight: it switched the search off. At a changeover
between two stations the correlator spent the whole of Hang correlating
the old station's timing, and looked nowhere else. That cost about 10 s
of Hang + 2.9 s of search + 0.8 s of confirmation, or **13.7 s**, before
the new station's weight was in force (Finding 44, from six 80 m
captures).

**Change.** While the lock is frozen, the correlator runs the full
acquisition ladder once per modem frame, as a cold search does.

- A candidate at a **different timing alignment** (modulo one modem frame,
  more than 4 of 960 samples away) is a new station. The old lock is
  dropped through `rade_corr_reset()` and the candidate is taken.
- The **same alignment** is the same station coming out of a fade, which
  keeps its lock and its averages.
- Timing decides, not frequency, because the frequency loop's lock
  points are 8.33 Hz apart and two stations can differ by an amount it
  cannot see.
- While frozen, the pilot averages **decay at the Averaging time** instead
  of being held outright, so a new station does not inherit most of an
  average built on one that stopped seconds ago. The decay stops 30 dB
  down.

**Holding is preserved.** The applied weight is only ever written from a
confirmed lock. It stays in force through the freeze, the search and a
new candidate's confirmation, and when nothing new turns up it stays
where it was.

**Validated** (on the feature branch, Findings 44 and 45):

| Test | Result |
|---|---|
| `test_rade` two-station changeover | 3.50 s, against 13.5 s |
| Same, at a 2 s and a 10 s Hang | 2.82 s and 2.90 s: Hang no longer matters |
| Same station, noise floor 31 dB higher for 5.1 s | Lock held; weight back to within 0.033 |
| Six 80 m on-air captures, scored on decode | +53, −29, four unchanged: not measurable |

The on-air set could not show a difference, because in every capture
one antenna alone decoded 97.9–100 % of frames. A capture with a marginal
signal on both antennas is still needed. `test_rade` has since come
across (LT-002), and both synthetic checks pass on `TEST`.

---

<a id="lc-011"></a>

## LC-011 — The Hang slider is removed

Ported from the Hang part of `01df2313` only; that commit's other five
changes are not taken. **Superseded in part by LC-014:** the value this
change pinned no longer does anything.

**What it does.** Removes the Hang slider and its callback.
`div_settings_validate()` pins the value (`DIV_HANG_DEFAULT`, 10 s)
instead of range-checking it, so an older props file or client can't
bring one in. The field stays on the wire and in the props file, so
neither changes shape.

**Why.** Hang only ever existed for RADE V1. The other references work on
the coherence gate and Averaging alone: the averages update every block,
the gate decides whether the result is applied, and otherwise the last
weight is held. With LC-010, RADE takes a new station on detection too.
So nothing is left for an operator to set.

**What this entry used to say, and why it was wrong.** It argued that the
setting "does not matter" and kept 10 s because "it re-acquires least
often". Three things were wrong with that:

- **The evidence was stale.** It came from Findings 33, 35 and 41, which
  were measured before the resync search existed, when Hang still gated
  the search. The first bullet also merged two captures: the 38 % → 94 %
  uptime was `165826`'s, and the "+10, +11, +10, +10" frames were
  `234624`'s.
- **It doesn't hold on `TEST`.** Re-measured with LC-010 in place, a
  short timer is harmful (−51 frames on `165826` at 2 s). See LC-014.
- **It framed a clock as a choice.** A timer that discards a lock nothing
  has contradicted is arbitrary however long it is. See [settled-decisions.md](settled-decisions.md).

**Order.** Must not be taken without LC-010. It also goes after LC-008,
because the slider it removes sits directly under a line LC-008 adds.

---

<a id="lc-014"></a>

## LC-014 — A RADE lock has no timeout: a new lock replaces an old one

**Problem.** After LC-010, the Hang timer's only remaining act was
`rade_corr_reset()` on a lock nothing had contradicted, discarding it
when a clock ran out. The weight was held either way, and a new station
was already taken on detection.

**Measured** on `TEST` through the whole engine (`run_ref`, Sum) and
scored on decode by `score_rade`. Three runs each; synced frames against
the better single antenna:

| Capture | 2 s timer | 10 s timer | No timer (600 s) |
|---|---|---|---|
| `165826` (marginal) | **+6** | +57 | +57 |
| `190516` | **+42** | +72 | +72 |
| `190715` | **+47** | +57 | +57 |
| `190932` | +3 | **−2** | +3 |
| `202743`, `112151`, `190822`, `193105` | identical at all three | | |

A short timer drops locks that would have recovered. A long one does
nothing, or costs a little. The timer never helps.

**Change.**

- The timeout is gone. A lock is held, weight and all, until the resync
  search finds a new one, or a retune or other context change resets
  everything.
- While the pilot is absent the averages age at the Averaging time, as
  LC-010 made them.
- The `hang` argument is removed from `rade_corr_process()` and
  `rade_track()`. The Hang field stays on the wire and in the props file,
  pinned, and nothing reads it.
- CPU: a held lock with no pilot runs the same search a cold one does, so
  it costs what having no lock would.

**Accepted, deliberately.** A new station landing within 4 samples of the
old timing (0.9 % of changeovers) is taken for the old one returning. In
a synthetic test (station B at A's exact timing, 20 Hz away), the lock
was not replaced within a minute. A rule to catch that case was built
and measured: across all 38 RADE captures in the set, a pilot found at
the held timing while frozen happened 6 times, never twice in a row, so
the rule would have been safe. It was still rejected. If the impostor's
pilot is strong enough the tracker follows it and Averaging corrects the
weight; if not, the old weight stays. Neither outcome is worth a
mechanism. See [settled-decisions.md](settled-decisions.md).

**Checked.**

- `test_rade` passes: a changeover costs 3.50 s at any Hang value, and a
  5.1 s fade at 31 dB worse SNR keeps its lock and its weight.
- The whole-engine decode scores match the committed engine at a 600 s
  timer on all eight RADE captures tried.
- The unit suite passes.
- `run_ref` is not byte-deterministic (see LT-005), so these figures come
  from repeated runs.

**Depends on** LC-010 and LC-011.

---

<a id="lc-016"></a>

## LC-016 — RADE V1's Min coherence is retired: the pilot already gates

Ported from `082dba0b` (`feature/auto-diversity`).

**Problem.** In RADE V1 the Min coherence slider doesn't gate a
coherence. It gates `rade_corr_quality`, the pilot's signal fraction,
across the slider's full 0–95 % range, and it can only do harm:

- **The job is already done.** Three pilot gates stand in front of it:
  the acquisition ladder, confirmation and probation, and the per-frame
  freeze. On the five no-signal captures they produced no weight at all,
  over 3,515 blocks.
- **Quality doesn't separate a good lock from a poor one.** `234508`
  (strong, a weight on three blocks in four) reads a median 0.217 with
  31 % of blocks under 0.05. `202743` (re-acquires eight times a minute)
  reads 0.193.
- **No reachable setting was safe.** On `165826`, the marginal capture
  where the combiner beats both antennas, moving it from 0 to 0.15 took
  the loop from a weight on 32.6 % of blocks to 1.7 %.

**Change.** Pinned at 0, its default, so nothing an operator has today
moves. It's pinned at every route in:

- `div_settings_validate()` pins it instead of ranging it;
- `div_settings_load()` ignores the incoming value, and LC-026's switch
  gives 0 as the live threshold on RADE V1;
- the menu's `store_ref_values()` no longer files the live value into
  its slot, and `restore_ref_values()` brings in 0.

(Before the 2026-10-01 rebase these were `div_cohmin_for_ref()`,
`diversity_auto_ref_store()` and LC-004's client path.)

The menu hides the Min coherence row while RADE V1 is selected. The field
stays on the wire and in the props file, and the engine's comparison
stays; the engine's comparison was cut by [LC-049](lc-gate.md#lc-049) (it
could not fire with the threshold pinned at 0).

**Checked.**

- `test_props`: a stored 15 % comes back as 0, and a client sending 30 %
  on RADE V1 leaves both the live gate and the slot at 0. The client
  check fails without this change.
- The unit suite passes.
- RADE decode through the whole engine is unchanged: `190516` +72,
  `165826` +57 synced frames.

**Depends on** LC-008 (the slider pointer) and LC-026 (the switch).

---

<a id="lc-031"></a>

## LC-031 — A RADE V1 correlator that cannot start no longer changes the reference

**Why.** If the pilot correlator could not start at the DDC rate,
`diversity_auto_start()` set `div_auto_ref` to `DIV_REF_DIGITAL_IQ`. The
engine was changing a menu setting (E1 in [ownership.md](ownership.md#review-e1-to-e8)): the menu
showed RADE V1 while the loop ran FSK/Digital, and the next props save
stored the substitute.

**Change.** The substitution is gone. The reference is left as set and
the loop holds. When the correlator isn't running, `rade_corr_process()`
returns no weight, `rade_corr_stop()` returns at once, and
`rade_corr_reset()` only clears plain variables, so the failure has no
blast radius. The menu reads "search". The only trace is
`rade_corr_start()`'s own log line.

**Reach.** It cannot happen today: every rate piHPSDR offers is a
multiple of 8 kHz. So, deliberately, there is no status flag and no menu
change (decided 2026-10-01). The first cut, with a
`div_auto_rade_unavailable` flag and an "n/a" status, is kept on
tag `backup/TEST-lc031-flag-20261001`.

---

<a id="lc-038"></a>

## LC-038 — The RADE V1 overlay covers the outer carriers whole

**Problem.** The green overlay drawn on the RX panadapter for the RADE V1
reference ran from `RADE_CORR_FLO` to `RADE_CORR_FHI`, 750 to 2200 Hz.
Those are the centres of the first and last carriers. radae spaces the
30 carriers 50 Hz apart (Fs/M = 8000/160) and puts carrier 1 in bin 15
(`rade_ofdm.c`: 1500 − 50·30/2 = 750), so each carrier spends 25 Hz
beyond its centre. The overlay cut the outer two carriers in half. Seen
on air as "the overlay leaves out a little of the edge furthest from
the carrier".

**Change.**
- New `RADE_CORR_OCC_LO`/`RADE_CORR_OCC_HI` in `rade_correlator.h`: the
  outer centres widened by half the carrier spacing, written in terms of
  `RADE_CORR_FS`/`RADE_CORR_M` (725 and 2225 Hz). The overlay draws those,
  mirrored for LSB as before.
- `RADE_CORR_FLO`/`FHI` are unchanged. The sideband test in
  `div_rade_side_expected()` wants carrier centres, and the comment now
  says that is what they are.
- Comment: the modem's centre is 1475 Hz, not 1500. radae aims at 1500
  and rounds the first carrier to the 50 Hz grid.

**Not changed (decided 2026-10-02).** The overlay is still drawn at the
nominal position. It isn't shifted by the tracked frequency offset
(`rade_corr_freq_off`), so a station that is off-frequency still shows
its far edge outside the overlay by the offset.

**Checks.** Applies to bare `upstream/TEST` and builds; the suite passes.
The overlay code is upstream's (`df47f59a`), so there's no LC dependency.
On air (2026-10-02): the overlay covers both outer carriers.

**Operating note (local config, not in the commit).** The three
radios' `.props` files set the digi-mode Var filters for RADE V1, both
centred 1500 Hz from the carrier: Var1 1600 Hz wide (DIGU 700 to 2300 Hz,
DIGL −2300 to −700 Hz), which covers the occupied 725 to 2225 Hz with
25 Hz to spare each side, and Var2 1000 Hz wide (1000 to 2000 Hz). The
stock 2.0k preset is left as shipped. A reshaped 2.0k was tried and
dropped because its button would still read "2.0k": the title is
compiled in and is also the props key. The 1.5k preset (750 to 2250 Hz)
cuts the lowest carrier at its centre.

---

<a id="rade-v2"></a>

# RADE V2 (LC-071 to LC-075)

A second RADE reference beside V1, from the cyclic prefix. Built on
`feature/rade-native` (2026-10-21 and 22), not on `TEST`. The method, the
evidence and what is not known: [../diversity-radeV2-cp.md](../diversity-radeV2-cp.md).
Tooling: LT-038.

<a id="lc-071"></a>

## LC-071 — One name for "a RADE reference"

**Problem.** The engine and the menu test `== DIV_REF_RADE_V1` in about a
dozen places to mean "the time-domain RADE reference, with no Min coherence
and no window of its own". A second such reference would need every one of
them found and changed, and the range check (`> DIV_REF_RADE_V1`) with them.

**Change.** `DIV_REF_IS_RADE(ref)` and `DIV_REF_LAST` in `diversity_auto.h`,
used where the engine meant that; the range check ends at `DIV_REF_LAST`. The
correlator start, stop and reset go through `div_rade_start/stop/reset()`,
which call the V1 correlator and nothing else.

**No behaviour change.** The unit tests under `test/diversity/` pass as before
(`test_rade`, `test_props`, `test_modal`, `test_window`, `test_digital`,
`test_cw`, `test_rates`, `test_modes_live`).

<a id="lc-072"></a>

## LC-072 — A RADE V2 correlator from the cyclic prefix

**Problem.** V2 has no pilot, so the V1 correlator has nothing to correlate
against. What V2 does have is its prefix: each 160-sample symbol repeats its
last 32 samples.

**Change.** `src/rade_v2_correlator.[ch]`, in the shape of
`rade_correlator.h`. Both arms are shifted, decimated to 8 kHz and band-limited
to the modem band; at every symbol the lag-128 products over a 32-sample window
are summed at all 160 offsets into two running profiles (0.5 s for detection,
the Averaging time for the weight). A tone's pedestal is the complex mean away
from the bump and is subtracted; the contrast of the matched peak over that
floor ("dpk") is the statistic. Lock is 4 of the last 5 one-second samples
above 0.30 with the bump within 20 samples; there is no timeout, and the
weight is held while the contrast is below 0.20. The weight issued is the unit
direction `conj(h1/h0)/|h1/h0|` (LC-075 replaced this with the noise-weighted weight), its phase from the prefix-gated cross-term
(which a common-mode interferer is not in), its gain ratio from the arms' own
prefix correlations. The maximal-ratio weight, each arm's prefix SNR, the tone
share and each arm's power are published for the log only. Self-contained: no
GLib, no `t_print`, so the devtools build it alone.

**Checks.** Against rade_c's own V2 receiver over the 171 captures: 24 are V2
by its sync and frame-sync; 21 are found (5 to 7 s to lock on strong signals,
17 to 23 s on weak), 3 weak ones are missed, and 2 recordings of an AM
broadcast are locked on falsely (rade_c's own sync accepts them too). V1, SSB,
STANAG, wideband and CW captures do not lock. Weight phase agrees with the
reference to a median of 7 degrees on T-008. The parameter sweep is in the
source comment and in the V2 document.

**Not known.** Whether the weight helps on air; the noise-weighted
(maximal-ratio) weight at all; detection with a strong tone inside a V2
signal; the Pi's CPU (counted, not measured).

<a id="lc-073"></a>

## LC-073 — `DIV_REF_RADE_V2` in the engine

**Change.** Reference 5, appended (`DIV_REFS` is 8 and V1 has no slot, so the
settings command does not change; an older build that meets 5 resets it to
Window in `div_settings_validate()`). The RADE block in `div_process_block()`
serves both: V2 runs `rade_v2c_process()` and `div_rade_v2_publish()` copies
its status into the `rade_corr_*` variables that the menu, the capture record
and the client's copy already read, so none of them changes. Start follows the
reference; stop and reset do both. Objectives as V1: Best uses the unit
co-phase direction (and the arm figure `arm_db` picks the arm), Sum the weight,
Null its negative. Min coherence is pinned at 0. The engine's slew smooths the
weight; the module does not. Log lines for lock, lock cleared, prefix lost,
prefix back, and a tracking line every 5 s while it holds.

**Checks.** `run_ref --ref rade2` drives the shipping `diversity_auto.c` over
T-008: locks at 4.9 s, stays locked, never locks on the SSB capture
(`test_cp_replay.py`). All earlier unit tests pass. The log lines compile and
run, but the test harness stubs `t_print`, so their text has not been seen.

**Open.** Invert and Null are 180 degree steps through the engine's slew; the
V2 decoder does not like phase steps, and the value of a null on V2 is
uncertain. Allowed, with the cost noted in the menu text once LC-074 is in.

<a id="lc-074"></a>

## LC-074 — The menu row and the overlay for RADE V2

**Change.** "RADE V2 prefix (blind)" in the "Measure on" list. Every place in
`diversity_menu.c` that tested for RADE V1 to hide Min coherence, switch to Sum
on selection, grey the level control or blank the coherence field uses
`DIV_REF_IS_RADE()`; the status line reads "RADE V1" or "RADE V2". The
panadapter draws V2's occupied band, 1031 to 1906 Hz (`RADE_V2C_OCC_*`).

**Ownership.** `diversity_menu.c` is dl1ycf's. This commit is separate from
LC-073 so it can be taken or left; the matter is in
[../menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

**Not checked on screen.** Built and linked; the combo row, the hidden slider
and the overlay have not been seen running.

<a id="lc-075"></a>

## LC-075 — RADE V2: Sum applies the noise-weighted gain

**Problem.** LC-072 issues the unit co-phase direction, so Sum adds arm 1 to arm 0 at equal gain. With two antennas that
differ, equal gain adds the poorer arm's noise (and the SSB it hears) at the strength of the better arm's signal. On
five 40 m captures (T-027) the radio's own combine scored 0.12 below one antenna alone in decoded |aux| on the weakest
(sync 43 % against 61 %) and level with it on two others; on two 60 m captures with the other arm the better (T-028), the
unit weight replayed at 0.003 and 0.011 below the better arm.

**Change.** The module issues the maximal-ratio weight it already computed for the log, `conj(h1/h0) * N0/N1`, each arm's
noise `N_k` being its power less its prefix-correlated part, the gain capped at 4 (`V2_GAIN_MAX`) and the noise floored at 2 %
of the arm's power. The unit direction stays in `rade_v2c_arm_cos/sin`. In the engine's RADE block, Sum applies the module's
weight; Null on V2 takes the unit direction turned over (`-rade_corr_arm_cos/-rade_corr_arm_sin`), as it did, since a null
that cancels the signal wants the direction, not the noise-weighted gain; Best and Invert are unchanged (Best already read
the unit direction). **No new setting**: no menu row, no props key, no change to the settings command or the capture
format. The slew (`DIV_SLEW_FRAC` 0.15), the hold threshold (0.20) and the Averaging time are as they were. `div_norm` does
not act on RADE references; the output is 2 to 3 dB louder than with the unit weight and the decoder's AGC absorbs it.

**Checks.**
- Replayed on the five 40 m captures, decoded |aux| rose over the radio's by +0.004, +0.009, +0.024, +0.038 and +0.113
  (0.927, 0.912, 0.902, 0.783, 0.750; the engine path through `run_ref --ref rade2` gives 0.927, 0.910, 0.901, 0.782, 0.754,
  with `113613` locking late under the shipped rule and so not a test there). It is at or above the unit weight on all five,
  and reaches the better antenna except by 0.010 on the weakest.
- On air, two 60 m captures with the weight live: +0.014 and +0.011 above the better arm and 0.017 and 0.022 above the unit
  weight replayed; the decoder's AGC gain did not move (0.77 to 0.79 median against 0.79 to 0.80).
- The parameters were swept on the same captures and are flat: gain cap 4 to 10 (cap 2 loses up to 0.011), noise floor
  0.5 to 5 %, slew 5 to 25 % per block, Averaging 0.8 to 6 s on the mean (V2 note, sections 12.3, 12.10; T-028). The prefix
  hold threshold was tried lower and was worse (0.750 down to 0.654).
- `test_cp_replay.py` checks that the weight's phase is the unit direction's, the gain never exceeds 4, the unit direction
  has unit length, and that the weight issued is the logged maximal-ratio one; `make -C test/diversity run` exits 0 with the
  same FAIL and KNOWN GAP lines as without the change. The engine's recorded state matches the replay to 1 block in 703 on
  the 60 m captures.

**Size, honestly.** The gain is large only where the antennas differ and the signal is weak (+0.113 on one capture, about ten
points of sync time, six seconds of a minute); on a clean signal with one good antenna it is +0.01 to +0.02 above the better
arm and keeps the old weight from going below it. Window and FSK/Digital are level with it on the strong captures. All
numbers come from seven captures of one day, on 40 m and 60 m, with an inverted L and a loop.

**Not known.** Whether the noise estimate holds on multipath (it is biased there, V2-F2) and with a tone in band; the value
of a null on V2 (the unit weight cancels the signal only when the arms carry it at equal gain, and `1/|h1/h0|` would be the
right null gain); the listener's side, which |aux| does not measure. The engine's state before the lock is plain 1 + 0j, which
summed a dead arm into the good one on the one 80 m capture that never locked (T-028); that is the lock rule's matter.

**Commit.** One commit holds the module, the engine's Null branch and, for now, the replay tool and its test; at the cut for
`TEST` the replay-tool files (`cp_replay.c`, `test_cp_replay.py`) go under LT-038 (rule 8).


## LC-070 — RADE and the operator's controls: leave on a mode request, follow the waveform, close on a second call

**Problem.** Three things in operating with RADE on. (1) A request for any mode but DIGU or DIGL (the mode menu, MIDI, CAT)
left RADE running on a mode it does not belong in: the receive chain kept its RADE passband and panel while the mode said
SSB. (2) With a RADE reference chosen in the diversity menu, the engine ran the correlator for the reference, not for the
waveform on air: V1 pilot correlation while RADE V2 was running, or the other way round. (3) The toolbar button and the MIDI
action for the RADE menu, and for the Diversity menu, only opened the window; the Filter, Noise and AGC menus close on the
same request again. They did because `start_*_menu()` records `active_menu` and closes when it is already that menu. The
Diversity menu never recorded it, and the RADE window is deliberately not `sub_menu`, so it could not.

**Change.**
- `vfo_id_mode_changed()`: on RX0, with RADE on, a mode other than DIGU or DIGL first calls `rade_native_leave()`, which does
  what choosing Off in the RADE window does (the window's own Off button when it is open, the same steps when it is not), and
  then the requested mode is set. RADE's own mode changes are DIGU or DIGL, or follow switching it off, so they do not recurse.
- (Superseded by LC-076: the reference itself now follows.) `diversity_auto_effective_ref()`: `div_auto_ref`, except that with a RADE reference chosen and RADE running, the reference
  is the one for the waveform (V1 for V1, V2 for V2). The engine start, the block context and the panadapter overlay use it.
  `div_auto_ref` is never written (the menu owns it); a reference that is not a RADE one is left alone. `retune()` calls
  `diversity_auto_rade_waveform_changed()` when the waveform changes, which restarts the loop if it runs on a RADE reference,
  because the correlator front end is built at start.
- `start_diversity_menu()` takes `DIVERSITY_MENU` in `active_menu` like the Filter menu. `start_rade_menu()` closes the
  window when it is open (`rade_native_menu_is_open()`, `rade_native_menu_close()`); only the window closes, RADE and the mode
  stay as they are, and the window stays open until it is closed or called again, as before.

**Checks.** Builds with `RADE_NATIVE=ON` and `DIVCAP=1`, and `diversity_auto.c` compiles without RADE_NATIVE. Not yet tried on
the radio. Not covered: band and bandstack changes set the mode directly and do not go through `vfo_id_mode_changed()`, so
RADE stays on across a band change into a non-RADE mode; the Diversity menu's status tag still names the reference the menu
holds, so it can say V2 while V1 runs (`diversity_menu.c` is dl1ycf's: a note for him).

## LC-076 — Entering RADE sets the diversity reference to match; RADE averaging nudged to 1.5 s on a first run

**Problem.** LC-070 ran the correlator of the waveform on air but left the stored reference alone, so the Diversity menu
said "RADE V1" while the engine and the panadapter overlay were on V2. The wrong correlator measures nothing, and a menu
that disagrees with it is worse than one that is wrong in a visible way. Also, nothing put an operator new to RADE near
useful settings.

**Change.** An aid to operation, not a mandate: the operator can choose Window, Carrier or any reference at any time, and
may want a wideband window in RADE to null a carrier.
- `diversity_auto_rade_waveform_changed(old_wave, new_wave)` (from `retune()`): **entering** RADE sets the reference to the
  correlator for the waveform. **V1 to V2** (or back) moves the reference to the other correlator, but only while it is a
  RADE one; a reference chosen for something else stays. **Leaving** RADE changes nothing.
- The reference is set through `diversity_menu_nudge()` in the menu, the way the operator's own choice is (the outgoing
  reference's window values filed, the new one's restored, the loop restarted on the right correlator), and the open
  menu's row and slider follow without calling their handlers; a client is sent the settings. `diversity_auto_effective_ref()`
  of LC-070 is removed: one stored reference, so the menu, status tag, overlay and engine agree.
- Averaging: **only** when the props file held no diversity settings at start, the first entry into RADE in that session sets
  Averaging to 1.5 s, to put the operator near good settings. With any settings saved, Averaging is left alone, as it is on
  later entries. Nothing new is saved.
- The engine writing the reference and Averaging is an exception to the menu owning them (decided 2026-11-11); it goes
  through the menu's own function. `diversity_menu.c` is dl1ycf's: a note for him. The objective (Manual, Sum, Null) is not
  touched, whereas choosing a RADE reference in the menu sets Sum.

**Checks.** Builds with `DIVCAP=1`. Not yet tried on the radio.

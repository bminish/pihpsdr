# Local changes against dl1ycf/pihpsdr TEST

This branch (`TEST` on `bminish/pihpsdr`) tracks the upstream `TEST` branch
of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a set of
local changes on top of it. This file is the register of those changes:
what each one does, why, and how it relates to the others.

We re-synchronise with upstream regularly. We are not submitting pull
requests yet, but every change is kept in a shape that can go upstream
later as a series of small PRs that the maintainer can follow one at a
time.

## Working rules

1. **One change, one commit.** A commit does one thing that can be
   explained in a sentence. Anything that bundles a fix with an unrelated
   behaviour change is split before it lands on `TEST`.
2. **Every change has an ID.** It is `LC-NNN`, carried as a
   `Local-Change: LC-NNN` trailer in the commit message and used as the
   heading below. Commit hashes change when a branch is rebased, but the
   ID does not. So this file refers to IDs, never hashes.
3. **Every commit builds on its own**, and can be reverted from the tip
   of `TEST` without conflicts.
4. **Independent where possible.** A change should apply to
   `upstream/TEST` on its own. Where one change genuinely needs another,
   the dependency is listed in the register. Watch for *textual*
   coupling as well: two changes that each add a line in the same spot
   (a static declaration, a reset in `cleanup()`) conflict when taken
   apart. Put each change's lines somewhere of its own.
5. **Bug fixes come before behaviour changes** in the series, so the
   fixes can go upstream even if a behaviour change is not accepted.
6. **Only source goes in the commits.** Captures, props files,
   `src/.divcap-on`, patches and the like are never committed with a
   change. Stage paths explicitly, not with `git add -A`.
7. **This file is local.** `docs/changes.md` is never part of an
   upstream PR. It is updated in the same push as the change it
   describes.
8. **Tooling is not a change.** The capture recorder, the test harness
   and the findings docs are local tooling. They carry
   `Local-Tooling: LT-NNN` instead of `Local-Change:`, are never part of
   an upstream PR, and no LC commit may depend on them. Before trusting a
   change, score it with the tools (see "Local tooling" below).

## Settled decisions (do not reopen without new evidence)

These have been argued through and measured. Changing one needs a
capture that shows the current rule failing, not a new line of
reasoning.

1. **No hang, no timeout, in any reference.**
   - **RADE V1: a new lock replaces an old one.** Nothing else ends a
     lock, short of a retune or other context change. While the pilot is
     absent the lock and the weight are held, and the averages age at
     the Averaging time. The resync search (LC-010) takes a new station
     as soon as it finds one.
   - **Every other reference: the correlation is the arbiter.** The
     averages update every block, the coherence gate decides whether the
     result is applied, and otherwise the last weight is held. Averaging
     decides how fast old data is forgotten. Hang time has no part to
     play.
   - **Why:** measured on `TEST` and scored on decode (LC-014), a short
     timer drops locks that would have recovered: 51 synced frames lost
     on the one marginal capture. A long one does nothing, or costs a
     little. A timer never helped.
2. **Hold good solutions through fades.** When there's nothing new to
   correlate on, keep the weight where it was: it's the best chance of
   being right when the signal returns (LC-012, LC-014).
3. **Don't add mechanisms for sub-1 % cases.** A new RADE station
   landing within 4 samples of the old one's timing (0.9 % of
   changeovers) is taken for the old one returning. If its pilot is
   strong enough the tracker follows it and Averaging corrects the
   weight; otherwise the old weight stays. We accept that. A timeout, or
   a rule to catch it, costs more than the case does.

## Commands

List the local changes and their IDs:

```sh
git fetch upstream
git log --reverse --no-merges upstream/TEST..TEST \
    --format='%(trailers:key=Local-Change,valueonly,separator=) %h %s'
```

The same with `key=Local-Tooling` lists the tooling commits.

See which local changes upstream has already taken, in any form (a `-`
means upstream has an equivalent patch):

```sh
git cherry -v upstream/TEST TEST
```

Re-synchronise with upstream:

```sh
git fetch upstream
git switch TEST
git merge upstream/TEST          # resolve, build, test
git push origin TEST
```

Merging keeps `origin/TEST` fast-forward only, with no force pushes. The
`LC` commits stay intact inside the merged history. If upstream has taken
a change, mark it *Upstream* in the register below.

Build a PR branch for one change (or one group) when the time comes:

```sh
git switch -c pr/lc-001 upstream/TEST
git cherry-pick <commit of LC-001>
```

Before sending it, drop the `Local-Change:` trailer if the maintainer
does not want it.

## Register

Status: **Local** means carried here only. **Proposed** means a PR is
open. **Upstream** means taken upstream (the commit can be dropped at the
next resync). **Dropped** means abandoned.

| ID     | Kind      | Summary                                               | Files                                     | Depends on | Status |
|--------|-----------|-------------------------------------------------------|-------------------------------------------|------------|--------|
| LC-001 | Fix       | Restore the saved auto-diversity settings at start-up | radio.c                                   | —          | Local  |
| LC-002 | Fix       | Treat impossible saved values as missing              | diversity_auto.c                          | —          | Local  |
| LC-003 | Fix       | Client settings block starts from the settings in force | server_thread.c, diversity_menu.c       | —          | Local  |
| LC-004 | Fix       | Keep a client's Min coherence change                  | diversity_auto.c                          | (LC-003)   | Local  |
| LC-005 | Fix       | Invert button swaps Null and Sum again                | diversity_menu.c                          | —          | Local  |
| LC-006 | Behaviour | Retire Coherence weighting; Window threshold 0.20     | diversity_auto.c/.h, diversity_menu.c     | —          | Local  |
| LC-007 | Behaviour | Hold stays on until the operator releases it          | diversity_auto.c, diversity_menu.c, radio.c | —        | Local  |
| LC-008 | Behaviour | Reference change recalls that reference's settings    | diversity_menu.c                          | —          | Local  |
| LC-009 | Behaviour | Unticking Follow RX filter starts on the passband     | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| LC-010 | Behaviour | RADE resyncs on a detection, not on a timeout         | rade_correlator.c                         | —          | Local  |
| LC-011 | Behaviour | Hang slider removed (value unused since LC-014)       | diversity_auto.c, diversity_menu.c, rade_correlator.c | LC-010, [LC-008] | Local |
| LC-012 | Behaviour | Coherence gate never below its own noise floor        | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| LC-013 | Fix       | Bins in the operator's manual notches left out of the estimate | diversity_auto.c                 | —          | Local  |
| LC-014 | Behaviour | No RADE lock timeout: a new lock replaces an old one  | rade_correlator.c/.h, diversity_auto.c/.h, diversity_menu.c | LC-010, LC-011 | Local |
| LC-015 | Fix       | "Measure on" menu runs the reference it shows         | diversity_menu.c                          | [LC-008]   | PR prepared |

"(LC-003)" means the change applies and builds without LC-003, but only
makes full sense with it. "[LC-008]" means a purely textual dependency:
the change touches lines next to LC-008's, so it does not apply without
it, but it does not use anything LC-008 adds.

- LC-009 and LC-012 do not build without LC-008, because they use the
  widget pointers LC-008 keeps.
- LC-011 must never be taken without LC-010. On its own it would fix
  every RADE changeover at a 10 s search blackout.
- LC-015's changed line in `ref_changed_cb()` sits next to the one LC-008
  changes. So its upstream PR is a separate commit, cut from
  `upstream/TEST` (branch `pr/diversity-menu-ref-row`), not a cherry-pick.
- LC-014 needs LC-010 (the resync search is what replaces a lock) and
  LC-011 (it rewrites that change's note). LC-010, LC-011 and LC-014 go
  upstream together.
- Reverting LC-008 means reverting LC-012, LC-011 and LC-009 first, and
  reverting LC-010 or LC-011 means reverting LC-014 first; they conflict
  otherwise. Every other change reverts cleanly from the tip.

**First PR:** LC-015 (see its entry), prepared on
`pr/diversity-menu-ref-row` against dl1ycf's `TEST`, awaiting review.

Suggested PR grouping, when we get there: LC-001 + LC-002 (settings are
restored, and restored sanely), then LC-003 + LC-004 (client/server
settings), then LC-005, then LC-008 + LC-009, then LC-007, then LC-010 +
LC-011 + LC-014 (RADE: resync, Hang slider gone, no timeout) and LC-012. LC-013 (notches) stands
alone and can go at any point. LC-006 goes last because it needs the
measurement data behind it.

---

## Fixes

### LC-001 — Restore the saved auto-diversity settings at start-up

**Problem.** `diversity_auto_restore_state()` exists but upstream never
calls it. The auto-diversity settings are saved on exit and never read
back. The per-mode-group settings blocks stay at their static all-zero
initial value. The first mode change loads one of them into the live
settings, and the next save writes zeros out for every group.

**Change.** One line: call `diversity_auto_restore_state()` in
`radio_restore_state()`, next to where the manual diversity gain and
phase are read.

**Note.** With this fix, settings saved by earlier builds now load.
Those may include the all-zero blocks, which LC-002 repairs.

### LC-002 — Treat impossible saved values as missing, not clamp them

**Problem.** `div_settings_validate()` clamps every value into its legal
range. An all-zero block (see LC-001) clamps to values that are legal
but useless: 0.2 s averaging, 3 Hz bins, and a 20 Hz window with Follow
RX filter off. That is what an operator saw on entering auto diversity
in CW.

**Change.** Values that no control can produce are treated as missing
and given their default:

- a non-positive or NaN averaging time, hang time or resolution;
- a window narrower than 20 Hz. That reference goes back to its default
  window, and the live window also goes back to following the RX filter.

The default window widths become named constants (`DIV_WIDTH_DEFAULT`,
`DIV_DIGITAL_WIDTH_DEFAULT`), so the initialisers and the repair agree.
This also repairs props files already written with an all-zero block.

### LC-003 — Start a client's settings block from the settings in force

**Problem.** The `CMD_DIV_SETTINGS` wire format carries the live
coherence threshold but not the four per-reference ones. Both receive
sites build a `DIV_SETTINGS` on the stack without filling those four
fields: the radio taking a client's change (`server_thread.c`), and the
client taking the radio's settings (`diversity_client_set_settings()`).
`div_settings_load()` then copies the uninitialised stack values into
the per-reference slots and makes one of them the live gate. Moving any
control on a remote client could leave the radio gating on an arbitrary
threshold, which was then saved to the props file.

**Change.** Both sites fill the block from `diversity_auto_get_settings()`
first, and overwrite only the fields the wire carries. The wire format
does not change.

### LC-004 — Keep a client's Min coherence change instead of dropping it

**Problem.** `div_settings_load()` always takes the live threshold from
the selected reference's slot, because after a reference change the
incoming live value may still belong to the previous reference. But the
wire carries only the live value, so a Min coherence slider move on a
client was silently discarded.

**Change.** When the incoming block keeps the reference already
selected, its live threshold is written into that reference's slot
first. It is clamped to the slider's 0–0.95 range, because it has not
been through validation. After a reference change the slot still wins,
as before.

### LC-005 — Make the Invert button swap Null and Sum again

**Problem.** When the menu was rewritten, the body of the Invert button
was left commented out. It pointed at a combo box that no longer
exists, so pressing Invert did nothing.

**Change.** The objective combo is kept in a static, and Invert moves it
between Null and Sum. That goes through `mode_changed_cb()`, so the
button and the combo cannot behave differently. Invert still does
nothing in Manual or Best.

---

## Behaviour changes

### LC-006 — Retire Coherence weighting; Window threshold 0.30 → 0.20

**Why.** We measured Coherence weighting against recorded two-antenna
captures, and it gave no benefit:

- It is not a better estimator. With the gate out of the way, Flat was
  as good or better on 4 of 5 captures (by up to 0.66 dB).
- Its apparent benefit was a bias in the gate. Weighting bins by their
  own coherence raised the reported coherence on every capture,
  including noise-only ones, so the same threshold became a looser test.
  At equal false-alarm rate, Flat keeps more signal blocks and gives
  better output SNR at every operating point.
- On weak SSB, the case it was meant to help, Flat was ahead or level on
  20 of 24 measurements (by up to 0.93 dB). On strong signals the choice
  makes no difference.

Flat at 0.20 gives slightly fewer false alarms than Coherence at 0.30
(5.2 % against 5.7 %) and about 0.3 dB more signal. So the Window
threshold default moves with the weighting.

**Change.** The Weighting control is removed from the menu, and the
setting is pinned to Flat on load, so an older props file or client
cannot bring it back. `DIV_WEIGHT_COHERENCE` stays in the enum, and the
field stays in the wire protocol and the props file, so neither format
changes.

**Open item.** Only a fresh install gets the new 0.20 default. A props
file that already holds `diversity_band_cohmin=0.30` keeps it, which is
a slightly stricter gate than intended under Flat. Decide whether to
migrate it, for example when the saved weighting was Coherence.

### LC-007 — Hold stays on until the operator releases it

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

### LC-008 — A reference change brings in that reference's own settings

**Problem.** Each reference (Window, Carrier, Digital IQ, RADE V1) keeps
its own window and its own Min coherence threshold. But upstream left
`div_window_recall()` under `#if 0`, and commented out its call in
`ref_changed_cb()`. After a reference change, the new reference gated on
the previous reference's threshold and inherited its window, and the
next centre or width move then filed that window under the new
reference.

**Change.** `div_window_recall()` is enabled again, the centre, width
and Min coherence widgets it updates are kept in statics, and
`ref_changed_cb()` calls it.

**Kept as one commit, on purpose.** This re-enables code that upstream
disabled. If there turns out to be a reason for that, reverting this one
commit (`git revert <LC-008>`) restores upstream's behaviour exactly.
LC-009 must be reverted with it or first, because it uses the same
widget pointers.

### LC-009 — Unticking Follow RX filter starts the window on the passband

**Why.** We do not want to start from useless values when none are
saved. Unticking "Window follows RX filter" hands the window to the
operator. If that reference has no window of its own yet, it fell back
to its built-in default: centre 0 and 1000 Hz wide (2600 Hz for Digital
IQ). In SSB that straddles the carrier. A bad props value could also
leave it at the 20 Hz floor. Either way, the first manual window the
operator saw had to be dragged into place before auto diversity did
anything sensible.

**Change.** If the selected reference's window is still at its default
or at the floor, it is placed on the current RX passband, exactly where
the follow window was (CW included). A window the operator has placed
is left alone. Following the RX filter stays the default when nothing
is saved.

**Depends on** LC-008 (the widget pointers).

### LC-010 — RADE resyncs on a detection, not on a timeout

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

### LC-011 — The Hang slider is removed

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
  has contradicted is arbitrary however long it is. See "Settled
  decisions" at the top.

**Order.** Must not be taken without LC-010. It also goes after LC-008,
because the slider it removes sits directly under a line LC-008 adds.

### LC-012 — The coherence gate never goes below its own noise floor

**Why.** Random tracking is not useful. The gate compares Min coherence
against a coherence *estimate*, and over N independent samples two
unrelated noises reach a coherence of about 1/N by chance. Below that, a
threshold passes noise-only blocks and the loop fits a weight to an
accident. When there is nothing real to correlate on, the right answer
is to hold where we were, because that is the best chance of being right
when the signal returns.

**Change.** Each block, the gate compares against the larger of the
operator's setting and the coherence that pure noise reaches 0.1 % of
the time over the bins and blocks actually in that estimate:

- **Bins.** The window, the carrier tracker's five bins, or the occupied
  span on FSK/Digital. Neighbouring 4-term Blackman-Harris bins are
  82 / 44 / 15 % correlated at 1 / 2 / 3 bins apart, so n bins count as
  n² / Σ|ρ(j−k)|² independent samples (about n / 2.76 on a wide window).
- **Blocks.** The engine tracks (Σw)² / Σw² for the exponential average as
  it runs. That's (2−α)/α in steady state, but only one block right after
  a reset, retune or averaging change, which is when noise is most easily
  mistaken for signal.

So the floor follows the reference, window or filter, bin width and
averaging time. RADE V1 gates on its pilot and is unaffected. The Min
coherence slider now steps in half percent, and its bottom follows the
floor on the menu's status tick. The operator's own setting is never
overwritten: when the floor falls again, their value reappears.

**Validated** by Monte Carlo (`docs/tools/coh_floor_mc.py`): two
independent noises through the same window and average, share of blocks
passing the gate:

| Case | Floor | At the floor | At the old default |
|---|---|---|---|
| Carrier, 5 bins, 0.2 s | 50.0 % (capped) | 0.11 % | 1.92 % (0.30) |
| Carrier, 5 bins, 2 s | 6.5 % | 0.27 % | 0.77 % (0.30) |
| Digital, 200 Hz occupied, 0.2 s | 20.2 % | 0.03 % | 0.01 % (0.30) |
| Window, 2.4 kHz, 0.2 s | 1.9 % | 0.05 % | 0.00 % (0.20) |

- The 0.77 % at the Carrier default with 2 s averaging comes almost
  entirely from the first blocks after a reset. Only a floor that counts
  the blocks actually held can catch that.
- The floor from `01df2313`, which assumed every bin independent and
  steady state, lets noise through 4–8 % of the time at the same target.
  It was also enforced only from the menu, so it did nothing while the
  menu was closed or when a client changed settings.

**Trade-off.** On the Carrier reference at short averaging, a weak
carrier now needs a coherence of up to 0.5 before the loop acts on it.
Longer averaging lowers the floor. That's the intended exchange: the
loop waits for evidence instead of tracking noise.

**Scored on recorded captures (2026-09-30).** Seven weak captures from
the findings, covering all three references, run through `TEST`'s engine
with and without LC-012 (`run_ref`, Flat weighting, recorded averaging)
and scored with `test/diversity/devtools/py/score_wideband.py`. The
scorer reproduces Finding 38 on `235906` (12.56 against 12.75 dB at gate
0, 4.28 against 4.24 at 0.30). Split-guard passband SNR, before → after:

| Capture | Ref, averaging | At `TEST`'s default threshold | Threshold 0 (floor only) | Noise-only passes at 0 | Noise coherence p95 |
|---|---|---|---|---|---|
| `235906` 17 m USB | Window, 1.12 s | 4.21 → 4.21 | 12.94 → 6.28 | 99 → 50 % | 0.71 |
| `123333` 17 m USB | Window, 0.32 s | 5.26 → 5.26 | 8.77 → 8.65 | 100 → 90 % | 0.11 |
| `122843` 17 m USB | Window, 0.32 s | 2.74 → 2.74 | 2.79 → 2.61 | 99 → 89 % | 0.37 |
| `011225` 60 m AM | Window, 0.20 s | 29.90 → 29.90 | 29.89 → 29.89 | 100 → 100 % | 0.94 |
| `000412` 13.72 AM | Carrier, 0.20 s | **26.08 → 25.94** | 26.23 → 25.94 | 100 → 67 % | 0.95 |
| `000537` 13.65 AM | Carrier, 2.19 s | 22.10 → 22.10 | 22.12 → 22.14 | 99 → 96 % | 0.79 |
| `003309` FSK | Digital, 0.20 s | 18.96 → 18.96 | 19.15 → 19.12 | 97 → 89 % | 0.70 |

What landed where expected:

- **At the defaults it is inert** on six of seven, identical to the last
  decimal, because the floor sits far below 0.20 or 0.30 there.
- **It acts on Carrier at short averaging**, as predicted: on `000412`,
  noise-only passes fall from 84 % to 67 %, for −0.13 dB.
- **It holds after a reset.** On `235906` at threshold 0, the loop first
  acts at block 55, not block 0: the opening dead air is uncorrelated,
  and it is held.

What did not:

- **Real dead air is mostly correlated noise, not uncorrelated noise.**
  Noise-only blocks reach a coherence of 0.11 to 0.95 (95th percentile),
  far above the floor. That's common-mode or band noise, a real
  correlation, which the floor correctly lets through. So the drop in
  noise-only passes is much smaller than the Monte Carlo suggests.
- **Tracking uncorrelated noise does not cost 3 dB with the flat Sum
  weight.** The premise was a unity-magnitude weight with random phase.
  But Sum's weight is Sxy/Sxx, whose magnitude shrinks with the
  coherence. Before LC-012, the median |w| in dead air is −19.6 dB
  (`235906` with its arms matched by `match_arms.py`) and −14 dB
  (`123333`, matched): effectively arm 0 alone, a stand-down that happens
  by itself. On matched arms, where the penalty should be largest, the
  pre-LC-012 engine at threshold 0 scores the same as at the default
  (`123333`: 2.83 against 2.87 dB).
- **Where the floor does bind, holding costs a little.** It keeps the
  station's weight (about −5.6 dB) through dead air instead of letting it
  shrink: −0.47 dB on matched `235906`, −3.9 dB on the real, lopsided one
  with the cold start excluded, −0.1 to −0.3 dB elsewhere. That only
  happens with the threshold below its default.

**Assessment.** Keep LC-012. It's inert at the defaults, costs 0.13 dB
in the one case it was expected to act on, and it implements the hold
rule. But its rationale is narrower than stated: on this set the floor
prevents no measurable harm with the Sum objective, and "random tracking
costs about 3 dB" is not supported for flat Sum. What the data shows
instead is the stand-down question in a new form: in dead air, a Sum
weight that is allowed to track shrinks on its own, and holding pays for
the station's weight. Not measured: Null and Best, and Carrier or Digital
on matched arms.

### LC-013 — Bins in the operator's manual notches are left out of the estimate

Ported from the notch parts of `d3b73b8a` and from `8117d9c7`
(`feature/diversity-binaural`), ahead of the CW correlator, which will
call it. It doesn't depend on CW, and on binaural it was bundled into the
CW keying commit.

**Problem.** The analysis taps the two raw antenna streams, upstream of
WDSP; the manual notch is applied a long way downstream. A notched
interferer was still in our spectrum at full strength, and every
reference that works from the transform picked it as a peak and fitted
the weight to it.

**Change.**

- The three notches join the analysis context. Enabling, moving or
  resizing one restarts the statistics; a disabled notch isn't compared.
- `div_bin_notched()` drops any bin lying entirely inside an active notch
  from the Window accumulation and the combine over it, the Carrier peak
  search, and all four passes of the FSK/Digital occupancy split.
- There's no CW special case. The notch sits in the same frame as
  `div_frame_off()` (checked against `rx_set_offset()` on `TEST`), so a
  notch centre maps to bin frequency −centre and the sidetone cancels.
- RADE V1 is the exception: the correlator works in the time domain, so
  there are no bins to leave out.
- LC-012's floor counts the bins actually used, so a notch raises it
  correctly with no further change. Binaural's per-bin count correction
  to the per-arm SNR isn't needed on `TEST`, whose per-arm floor is taken
  over the same bins and resets with the notch.

**Checked.**

- With no notch set, fourteen `run_ref` replays (seven captures, three
  references, two thresholds) are byte-identical to the engine before
  it.
- `test_modes_live`'s notch pass (every reference, signal notched out)
  now counts, and passes: none converges on the notched channel.
- It applies to `upstream/TEST` on its own and builds.

**Not yet measured.** The mechanism was confirmed on air on the feature
branch (an operator notched an interferer and watched the combiner stop
following it). Its magnitude has never been measured: a capture can't
record a notch. LT-004 makes that possible by replaying with the notch
set. The validation scenario is still to be planned.

**What the capture set offers so far** (`score_wideband.py --peaks`, four
captures checked):

- `000412` (13.72 MHz AM) has the only truly steady carrier: 100 % of
  blocks, +42 dB over the passband median. But it's the wanted signal's
  own carrier. It served as the plumbing check: a 30 Hz notch over it
  removes 9 passband bins from the score and changes the Carrier
  reference's answer.
- `143433` (20 m CW) has narrow peaks near the zero beat, but they're
  present in only 36–40 % of blocks. A 40 Hz notch over the strongest
  moved the Window weight by less than 0.001.
- `235906` and `122843` (17 m USB) have no steady narrow peak: the
  strongest are present in 4–16 % of blocks.

The case LC-013 is for is a **steady interferer that isn't the wanted
signal, inside the passband of a weaker station**. None of these shows
it. The scenario therefore needs either:

- a `--peaks` scan of the whole capture set for steady peaks in SSB or
  Digital passbands; or
- new captures taken for it with `make DIVCAP=1`: a heterodyne or carrier
  over a weak SSB or Digital signal, recorded in pairs with and without
  the notch, back to back, and said so in `PIHPSDR_DIVCAP_NOTE`. The
  notch doesn't show in the file, so the note is the only record.

What to measure once there is one: split-guard SNR with the notch
applied to both the weight and the score, notched against un-notched,
per reference (Window, Carrier, Digital). Also how often the loop acts,
and whether the weight stops following the interferer.

### LC-014 — A RADE lock has no timeout: a new lock replaces an old one

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
mechanism. See "Settled decisions" at the top.

**Checked.**

- `test_rade` passes: a changeover costs 3.50 s at any Hang value, and a
  5.1 s fade at 31 dB worse SNR keeps its lock and its weight.
- The whole-engine decode scores match the committed engine at a 600 s
  timer on all eight RADE captures tried.
- The unit suite passes.
- `run_ref` is not byte-deterministic (see LT-005), so these figures come
  from repeated runs.

**Depends on** LC-010 and LC-011.

### LC-015 — The "Measure on" menu runs the reference it shows

**Problem.** The combo lists Window, FSK/Digital, Carrier, RADE V1. The
`DIV_REF_*` enum is BAND, CARRIER, RADE_V1, DIGITAL_IQ. And
`ref_changed_cb()` stored the combo row as the reference. So the menu ran
a different reference from the one it showed:

| Row | Menu shows | Engine ran |
|---|---|---|
| 0 | Window | Window |
| 1 | FSK/Digital | Carrier |
| 2 | Carrier | RADE V1 |
| 3 | RADE V1 | FSK/Digital |

Opening the menu had the same fault in reverse.

**How it was found.** Five captures taken on 2026-09-30 with "RADE V1"
selected (`172640`, `172847`, `172907`, `173144`, `173330`). The operator
saw the "correlator" appear to lock and follow junk. Every block of all
five records reference 3 (FSK/Digital), and the RADE correlator's lock
state is 0 throughout: it never ran. What looked like a RADE tracking
regression was the FSK/Digital occupancy solve fitting to whatever was
loudest in the passband. These captures therefore say nothing about RADE
tracking; that still needs RADE captures taken with this fix in.

**Change.** A row table, `div_ref_rows[]`, maps rows to references and
back, keeping the order the menu shows. `div_ref_to_row()` is the name
the populate code under `#if 0` already calls. The objective and
resolution combos were checked and are correct.

**Origin.** The bug is upstream. The feature branch had this mapping as
`ref_rows[]`, and it was lost when the menu was rewritten for upstream.

**Upstream.** Prepared as a one-commit PR against dl1ycf's `TEST`: branch
`pr/diversity-menu-ref-row`, cut from `upstream/TEST` at `883243c0`,
`src/diversity_menu.c` only, +30 −2. It builds. Not pushed; awaiting
review.

---

## Local tooling (never upstream)

The means to score a change against the recorded captures before keeping
it. Usage is in `test/diversity/devtools/README.md`, whose first section
covers this branch. The findings the tools produced so far are in
`docs/diversity-measurements.md` and `docs/diversity-rade.md`, as
recorded on the feature branches.

| ID | Summary | Where |
|---|---|---|
| LT-001 | Capture recorder: `make DIVCAP=1`, Capture button, format-3 writer fix, `captures/` ignored | `src/diversity_capture.[ch]`, `Makefile`, `DIVERSITY_CAPTURE` blocks in `src/diversity_auto.c` and `src/diversity_menu.c`, `.gitignore` |
| LT-002 | Test harness: seven unit tests, `replay_rade`, `run_ref`, `test_capture`, `score_rade`, `known_gaps.h` | `test/diversity/` |
| LT-003 | Wideband scorer and matched-arm generator (Python) | `test/diversity/devtools/py/` |
| LT-004 | Replay and score with manual notches: `run_ref --notch`, `score_wideband.py --notch` and `--peaks` | `test/diversity/devtools/` |
| LT-005 | Follow LC-014: no Hang passed or swept; `--hang` is an error | `test/diversity/devtools/` |

**LT-005.** `rade_corr_process()` no longer takes a hang, so the replay
tools stop passing one. `run_ref --hang` and `replay_rade --hang` stop
with an error pointing at LC-014 instead of silently doing nothing.
Worth knowing when reading any replay: `run_ref` is not
byte-deterministic. Its worker thread can shift a read by a block, and on
`165826` one run in three at a 600 s timer scored +39 against +57. Repeat
a replay before trusting a single difference.

The tooling commits form a stack: each later one edits files an earlier
one created. They revert in reverse order, and only LT-001 applies to
`upstream/TEST` on its own. That's fine, because none of them goes
upstream.

**LT-004.** A capture can't record a notch, but because the notch acts
downstream of the tap, a replay with one set is exactly what the radio
would have done. `run_ref --notch C:W` sets it, with the values the notch
menu stores. `score_wideband.py --notch C:W` leaves the notched bins out
of the passband score, as WDSP leaves them out of the audio, by the
engine's rule. `--peaks N` lists the steadiest strong peaks in a
capture's passband, with the notch centre that covers each, as the
starting point for a notch scenario.

**LT-003** scores `run_ref` weight series on Window, Carrier and Digital
captures: split-guard passband SNR as the findings define it, plus pass
rates, coherence and |w| in signal and noise-only blocks. It's calibrated
against Finding 38. `match_arms.py` writes a copy of a capture with arm 1
scaled to arm 0's noise floor, because every capture in the set has
lopsided arms. Usage is in the devtools README.

**LT-001** brings the recorder the feature branches used. Upstream `TEST`
kept the recorder's hooks in `diversity_auto.c` but not the recorder, the
Makefile switch or the button. Those hooks were also an older writer that
always wrote `rec_flags` as zero, so a capture taken on upstream's code
never marked a context change or an engine reset, and its replay diverged
at the first attenuator step. The round-trip test differed on 60 of 160
blocks before the fix and on none after.

**LT-002** is the harness from `feature/diversity-binaural`, taken from
before `8ea8c8f3` and adapted to `TEST`'s engine in the tools only. It
reproduces Finding 45 exactly on `190516`: the replay gives 4
acquisitions, 0.714 locked, −11.37 dB and 0.101, and the decode score
gives arm 0 332 frames at 97.9 % and arm 1 223 frames at 99.1 %. It also
independently confirms LC-006 (a stored Coherence weighting comes back
as Flat), LC-010 (the resync drops at 2.82 s; a 31 dB, 5.1 s fade keeps
its lock) and LC-011 (a stored Hang of 1 s comes back as 10 s).

### Known gaps

Features the harness was written for that `TEST` does not have, from
`test/diversity/known_gaps.h`. Each check still runs and prints its
figures; a failure is reported but not counted. When one is ported, it
gets an LC number and its line in `known_gaps.h` is deleted, so the check
becomes its regression test.

| Gap | Feature branch commit | What the check shows on `TEST` |
|---|---|---|
| Branch noise ratio | `e6c12c05` | Window Sum does not back off an arm 20 dB noisier (SINR +16.66 dB, against +29.96 dB for Digital) |
| Level output | `4f24f5c3` | The combined output is 4.16 dB louder than one antenna |
| Stand-down | `fc0b3d1e`, `94b4cc6f` | Never stands down on an empty band. **Conflicts with the hold rule; decision needed, see below** |
| Carrier search follows the filter | `41f8700c` | Two follow cases pick the wrong carrier |
| Wire helpers | `42f68714` | Not a behaviour: the conversion is inline on `TEST`, so the round trip cannot be called |
| RADE quality retired | `082dba0b` | A stored 15 % RADE quality gate is kept, not pinned to 0 |
| CW reference | `6027208a`, `d3b73b8a` | Not built: `test_cw` needs `DIV_REF_CW` |

**Stand-down is not simply a gap.** On the feature branches, the combiner
slews the weight to zero on an empty band and puts it back when the band
fills. That's the opposite of the rule on `TEST`: when there is nothing
new to correlate on, hold where we were. The feature branch measured the
cost of holding as 12.18 dB of extra noise between overs on `122843` and
3.5 dB over two thirds of a minute on `235906`, where the held weight had
been fitted with one arm 15 dB hotter. Its tuning also assumed a gate
that "passes about one no-signal block in twenty", which is the floor
LC-012 replaced. Scoring LC-012 on captures has since added evidence
(see "Scored on recorded captures" under LC-012): with the flat Sum
weight, a loop allowed to track dead air lets its weight shrink to about
−14 to −20 dB, which is a stand-down in effect. Holding the station's
weight instead cost 0.5 to 3.9 dB on `235906` when the threshold was
below its default. Stand-down itself is not on `TEST`, so it has not
been scored here.

---

## Not carried

- **6 Hz default bins.** An earlier local commit changed the
  no-saved-settings default resolution from 12 Hz to 6 Hz. We dropped
  it and keep upstream's 12 Hz. Speed of response is usually what we
  want, and the Averaging slider, not Resolution, is the control an
  operator uses most of the time. 6 Hz doubles the FFT length and the
  block duration. The operator can still choose 12, 6 or 3 Hz.

## Pending: to be ported from `feature/auto-diversity`

Features and documentation will be brought in from
`feature/auto-diversity` one at a time. Each one gets the next `LC`
number, a commit (or a short run of commits) that follows the rules
above, and an entry in the register and in the Fixes or Behaviour
section, in the same push.

Noted while porting, not yet decided:

- The validation scenario for LC-013 (notches): no capture checked so far
  has a steady interferer inside a weaker station's passband. See "What
  the capture set offers so far" under LC-013.

- `082dba0b`, which retires the RADE V1 Min quality slider (the pilot
  already gates). Separate decision.
- Stand-down (`fc0b3d1e`, `94b4cc6f`) against the hold rule. LC-012's
  capture scoring bears on it: see "Scored on recorded captures" under
  LC-012.
- `feature/auto-diversity`'s Findings 50 and 51 (the CW reference, and
  the notches) are not in this branch's `docs/diversity-measurements.md`.
  See the note at its top.

## History

- 2026-09-30: LC-015 (the "Measure on" row/reference mismatch) fixed,
  and prepared as the first upstream PR.
- 2026-09-30: LC-014 (no RADE lock timeout) and LT-005. LC-011's
  description corrected, and "Settled decisions" added.
- 2026-09-30: LC-013 (notch carve-out) and LT-004 (notch replay and
  scoring) ported.
- 2026-09-30: LT-001 and LT-002 ported (capture recorder, test harness),
  with `docs/diversity-measurements.md` and `docs/diversity-rade.md`.
- 2026-09-30: LC-010 to LC-012 ported (RADE resync, Hang pinned, gate
  noise floor).
- 2026-09-30: register created. Upstream `TEST` at `883243c0`. The eight
  local commits made on 2026-09-28 were re-cut into LC-001 … LC-009, and
  the 6 Hz default was dropped. The pre-split branch is kept as
  `history/backup/TEST-pre-split-20260930`.

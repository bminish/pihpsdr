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
9. **Corrections to a landed change are fixup commits.** A later fix to
   an LC's own code (a stale comment, dead code it left behind) is a
   separate commit whose subject starts `Diversity: LC-NNN fixup -` and
   which carries that LC's `Local-Change:` trailer. `TEST` is never
   rewritten for them. When the LC's PR branch is cut, its fixups are
   folded into it (see "Cutting a PR branch" below).

## Settled decisions (do not reopen without new evidence)

These have been argued through and measured. Changing one needs a
capture that shows the current rule failing, not a new line of
reasoning.

1. **No hang, no timeout, in any reference.**
   - **RADE V1: a new lock replaces an old one.** Nothing else ends a
     lock, short of a retune or other context change. While the pilot is
     absent the lock and the weight are held, and the averages age at
     the Averaging time. The resync search (LC-010) takes a new station
     as soon as it finds one. RADE V1 has no Min coherence either: the
     pilot gates already do that job (LC-016).
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
4. **Averages age at the Averaging time, whether or not a gate accepts
   the block.** Otherwise, under a gate that opens only some of the
   time, the average's real age grows well past the Averaging time just
   when conditions are hardest, and a new signal is averaged into data
   from one that ended long ago. Window, Carrier and FSK/Digital update
   every block anyway. RADE V1 ages through a freeze (LC-010). CW ages
   every bin in its region every block (LC-017).

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

### Cutting a PR branch

- **Fold in the fixups.** Cherry-pick the LC commit and every
  `LC-NNN fixup` commit after it, and squash them into one.
- **Delete what is kept only for our tools.** `TEST` keeps some engine
  code that nothing in the radio can reach, because `run_ref` still uses
  it to compare old behaviour against new. It stays local, and is deleted
  in the PR:
  - LC-006: the Coherence-weighted accumulation in
    `div_process_block()` (`coherence_weighted` and the branch it guards,
    and the comment about the staleness test under Coherence weighting).
    `DIV_WEIGHT_COHERENCE` stays in the enum for the wire and the props
    file.
  - LC-016: the RADE V1 quality comparison against
    `div_auto_coherence_min` in `div_process_block()`. `rade_cohmin`
    stays on the wire and in the props file.
- **Drop the `Local-Change:` trailer** if the maintainer does not want it.
- **Comments may cite `docs/`.** Our docs don't go upstream, and upstream
  code already cites `docs/diversity-measurements.md`, so a reference to
  a finding or to this register is left as it is.

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
| LC-005 | Fix       | Invert button swaps Null and Sum again                | diversity_menu.c                          | —          | Upstream (`b180b79a`) |
| LC-006 | Behaviour | Retire Coherence weighting; Window threshold 0.20     | diversity_auto.c/.h, diversity_menu.c     | —          | Local  |
| LC-007 | Behaviour | Hold stays on until the operator releases it          | diversity_auto.c, diversity_menu.c, radio.c | —        | Local  |
| LC-008 | Behaviour | Reference change recalls that reference's settings    | diversity_menu.c                          | —          | Local  |
| LC-009 | Behaviour | Unticking Follow RX filter starts on the passband     | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| LC-010 | Behaviour | RADE resyncs on a detection, not on a timeout         | rade_correlator.c                         | —          | Local  |
| LC-011 | Behaviour | Hang slider removed (value unused since LC-014)       | diversity_auto.c, diversity_menu.c, rade_correlator.c | LC-010, [LC-008] | Local |
| LC-012 | Behaviour | Coherence gate never below its own noise floor        | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| LC-013 | Fix       | Bins in the operator's manual notches left out of the estimate | diversity_auto.c                 | —          | Local  |
| LC-014 | Behaviour | No RADE lock timeout: a new lock replaces an old one  | rade_correlator.c/.h, diversity_auto.c/.h, diversity_menu.c | LC-010, LC-011 | Local |
| LC-015 | Fix       | "Measure on" menu runs the reference it shows         | diversity_menu.c                          | [LC-008]   | Proposed ([#150](https://github.com/dl1ycf/pihpsdr/pull/150)) |
| LC-016 | Behaviour | RADE V1's Min coherence retired (pinned at 0, row hidden) | diversity_auto.c, diversity_menu.c    | LC-008, [LC-004] | Local |
| LC-017 | Behaviour | A CW / Morse reference                                | diversity_auto.c/.h, diversity_menu.c, rx_panadapter.c | LC-009, LC-012, LC-013, LC-015 | Local |
| LC-018 | Behaviour | CW tells keying from a steady carrier                 | diversity_auto.c                          | LC-017     | Local  |
| LC-019 | Behaviour | Fresh install: CW modes start on CW at 0.2 s          | diversity_auto.c                          | LC-017     | Local  |
| LC-020 | UI        | Window row hidden while following; "Follow RX Filter" | diversity_menu.c (+ two comments)         | [LC-009]   | Local  |
| LC-021 | Fix       | Window spin buttons set digits as spin buttons        | diversity_menu.c                          | —          | Local (PR prepared) |

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
- LC-016 uses LC-008's slider pointer and edits LC-004's slot writer, so
  it goes after both.
- Reverting from the tip: a change goes with its fixups and everything
  that depends on it, newest first. Checked 2026-09-30: only LC-001,
  LC-003, LC-005, LC-007, LC-014 and LC-019 revert cleanly on their own.
  The fixups and the CW changes edit lines of most of the others. This
  matters less than it did, because PR branches are cut fresh from
  `upstream/TEST` with the fixups folded in (see "Cutting a PR branch").

**First PR:** LC-015, opened 2026-09-30 as
[dl1ycf/pihpsdr#150](https://github.com/dl1ycf/pihpsdr/pull/150) from
`pr/diversity-menu-ref-row`.

Suggested PR grouping, when we get there: LC-001 + LC-002 (settings are
restored, and restored sanely), then LC-003 + LC-004 (client/server
settings), then LC-005, then LC-008 + LC-009, then LC-007, then LC-010 +
LC-011 + LC-014 + LC-016 (RADE: resync, Hang slider gone, no timeout,
no threshold) and LC-012. LC-013 (notches) stands
alone and can go at any point. LC-017 + LC-018 + LC-019 (CW) go after
LC-012, LC-013 and LC-015. The "Measure on" order and the CW row are the
part most likely to interest upstream on their own. LC-020 (menu tidy)
can go with LC-008 + LC-009. LC-006 goes last because it needs the
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

- a non-positive or NaN averaging time or resolution (and hang time,
  until LC-011 pinned it and the repair was removed as dead);
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

**Upstream.** dl1ycf made the same fix independently in `b180b79a` (the
objective combo as `auto_btn`). Taken as upstream wrote it at the merge;
our `mode_combo` is gone and the LC-005 commit is superseded.

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

**Not migrated.** Only a fresh install gets the new 0.20 default. A
props file that already holds `diversity_band_cohmin=0.30` keeps it,
which is a slightly stricter gate than intended under Flat. This is a
test branch, so we don't migrate it; an operator can move the slider.

**Kept local, deleted in the PR:** the Coherence-weighted code path,
which only `run_ref --weighting coherence` can reach. See "Cutting a PR
branch".

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
saved. Unticking "Follow RX Filter" (then "Window follows RX filter") hands the window to the
operator. If that reference has no window of its own yet, it fell back
to its built-in default: centre 0 and 1000 Hz wide (2600 Hz for Digital
IQ). In SSB that straddles the carrier, so the first manual window the
operator saw had to be dragged into place before auto diversity did
anything sensible.

**Change.** If the selected reference's window is still at its default,
it is placed on the current RX passband, exactly where the follow window
was (CW included). A window the operator has placed is left alone,
however narrow: 20 Hz is a width the slider offers, and anything below
it is already put back to the default by LC-002. (It first also seeded a
window at 20 Hz or below; a fixup removed that.) Following the RX filter
stays the default when nothing is saved.

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
  a reset or retune, which is when noise is most easily mistaken for
  signal. An averaging change doesn't reset; the count follows the new α
  over the next few blocks.

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
  the notch, back to back. The notch doesn't show in the file, so which
  run had it, and where, is recorded in `docs/test-findings.md` when the
  captures are ingested.

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
`src/diversity_menu.c` only, +30 −2. It builds. Opened 2026-09-30 as
[dl1ycf/pihpsdr#150](https://github.com/dl1ycf/pihpsdr/pull/150).
Rebased onto `b180b79a` the same day, when upstream's menu layout
changed the combo's attach line next to it; still +30 −2, mergeable. When
it's merged, mark LC-015 *Upstream*; the local commit can then be dropped
at the next resync.

### LC-016 — RADE V1's Min coherence is retired: the pilot already gates

Ported from `082dba0b` (`feature/auto-diversity`), with an extra guard
for the client path that LC-004 opens on `TEST`.

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
- `div_settings_load()` ignores the incoming value;
- `div_cohmin_for_ref()` returns 0 for RADE V1;
- the client path from LC-004 no longer files a value into its slot;
- `diversity_auto_ref_store()` no longer files the live value there.

The menu hides the Min coherence row while RADE V1 is selected. The field
stays on the wire and in the props file, and the engine's comparison
stays, so `run_ref --cohmin` can still sweep the retired path. That
comparison is kept local and deleted in the PR (see "Cutting a PR
branch").

**Checked.**

- `test_props`: a stored 15 % comes back as 0, and a client sending 30 %
  on RADE V1 leaves both the live gate and the slot at 0. The client
  check fails without this change.
- The unit suite passes.
- RADE decode through the whole engine is unchanged: `190516` +72,
  `165826` +57 synced frames.

**Depends on** LC-008 (the slider pointer) and, textually, LC-004.

### LC-017 — A CW / Morse reference

Ported from `ceeca2eb` (`feature/auto-diversity`), with the changes below.

**What it does.** `DIV_REF_CW` searches the RX filter (or a hand-placed
window) for the strongest tone, preferring the centre with a Gaussian
weighting, because in CW the centre of the passband is the note the
operator zero-beat. It accumulates the cross spectrum over that tone and
one bin either side, and nothing else in the region.
- **Sum** carries the branch noise ratio, from the off-tone bins.
- **Null** is not scaled.
- **Best** uses the per-arm SNR from the same floors.
- A keyclick that lifts the whole region (tone per bin under 2× the
  region's mean) is not accepted.

**Changed from the feature branch.**

- **The averages age every block** (see Settled decision 4). The feature
  branch ran CW after the window accumulation, so every key-up block went
  into the tone bins as noise and every accepted block was counted twice.
  Here CW runs first, every bin in the region is scaled by (1 − α) each
  block, and only an accepted block adds its tone bins. `test_cw` checks
  it: after a 5 s gap a new channel is taken within two elements, and
  with the ageing disabled the check fails (−62° against −78°).
- **The noise ratio is steady.** The feature branch took it from one
  block's 10th percentile of the off-tone bins, the fourth smallest of
  the forty-odd a CW filter leaves, which moved by 10 dB from block to
  block. On `test_cw`'s synthetic tone the Sum gain landed 2.5 to 9.6 dB
  off. Now it's the mean of the quieter half of those bins, smoothed at
  the Averaging time: within 0.1 dB. (A fixup commit.)
- **The gate goes through LC-012's floor** over the three tone bins, with
  the block count following the ageing.
- **Hold, not stand-down.** The in-block key-down test is not ported: it
  rejected 0 of 4123 blocks (AD-50). Key detection is LC-018.

**Settings.** Own window (600 Hz default) and threshold slots, in
`DIV_SETTINGS`, the per-group and flat props, store/recall, validation
and the slider floor. A props file from before CW gives it CW's own
threshold, not the live one. `DIV_REF_CW` is appended to the enum, so no
saved reference moves. The wire is unchanged: like the other
references' slots, CW's are not on it.

**UI** (worth upstream's attention even if the engine is not taken):
"Measure on" is now **Window, CW, FSK/Digital, Carrier, RADE V1**, with
the labels in the row table so the order lives in one place. Carrier
keeps its place between FSK/Digital and RADE V1. The status line reads `CW <bins> track
<tone Hz>`, and the panadapter shades the search region and the tone.

**Depends on** LC-012 (the gate floor), LC-013 (`div_bin_notched()`),
LC-015 (the row table) and LC-009 (`diversity_auto_seed_window()`, now
using `div_width_default()`).

### LC-018 — The CW reference tells keying from a steady carrier

Ported from `565c6e40`, with Key detect as a constant instead of a
control.

**Why.** A carrier in a CW passband holds steady through the gaps where
every station stops, and LC-017 can't tell it from keying. The keying
rate can't either: at 10 to 35 WPM the envelope moves at 4 to 15 Hz,
which one block per 43 to 171 ms samples below Nyquist. What survives is
that Morse stops.

**Change.** A block is keyed when the region's peak stands 3 dB above the
quietest that peak has recently been: a minimum that falls at once and
climbs back at 12 dB/s, seeded from the block's own noise. An unkeyed
block is held and the averages age through it. CW's Min coherence default
is 0.10, and a region of fewer than six bins is held.

**Why a constant.** Every setting from 2 to 6 dB rejects the carrier
equally; above that range the gate stops the mode (AD-50). A constant
also means no new setting and no wire change.

**Measured** (`score_cw.py`, Sum, recorded averaging, the eleven usable
CW captures scored as 16 segments between retunes and filter changes,
against the better antenna):

| | Mean | Segments ahead of the better antenna |
|---|---|---|
| Window (`TEST` before CW) | −0.70 dB | 5 / 16 |
| FSK/Digital | −0.14 dB | 8 / 16 |
| CW, LC-017 only | −0.70 dB | 8 / 16 |
| CW, with LC-018 | −0.12 dB | 9 / 16 |
| CW, with the steady noise ratio (as shipped) | −0.14 dB | 9 / 16 |

These replace the first figures, which scored each capture as one
passband and had the tone's neighbours wrong where the passband
straddles 0 Hz (`score_cw.py` fixed, LT-007). Key-up blocks acted on:
31–94 % with LC-017 alone, 2–50 % with LC-018. On three CW captures
taken with the reference in (T-010 to T-012 in `docs/test-findings.md`)
it is the best of the three references on strong contest signals, and
gains 1.6 dB on a weak beacon.

On `143433`, of the blocks the loop acted on, the tracker was within 1.5
bins of the steady carrier on **52.4 %** without key detection and
**6.5 %** with it (AD-50: 38.1 % → 3.4 %, by its own tolerance).

**Limitation.** A strong carrier that *appears* is accepted as keying
until the floor has climbed to it, at 12 dB/s: one 40 dB up for over
3 s. A carrier present all along (the AD-50 case, and `test_cw`'s) is
rejected as soon as the keying stops.

**Depends on** LC-017.

### LC-019 — A fresh install starts CW on the CW reference at 0.2 s

**Change.** When the props file holds no diversity settings at all, the
CW mode group (CWL, CWU) starts on the CW reference, with its own window
and threshold, at 0.2 s averaging. Nothing else changes: a file from
before the per-group blocks still seeds every group from its flat keys
(`test_modal` section 3), and a group's own keys win over both. The
operator can move the averaging like any other setting.

**Why 0.2 s.** Swept over eleven CW captures on the feature branch, the
short end of the slider scored +0.20 dB mean against −0.40 at 1.0 s and
−0.19 at 2.0 s (`565c6e40`). That's a CW result, so it's a seed for this
group rather than a global default. **To re-test:** few of those
captures are marginal. Re-sweep as marginal CW captures come in.

**Depends on** LC-017.

### LC-020 — The window row is hidden while it follows the RX filter

**Change.** The Window centre and Window width controls are hidden while
the follow tick is on, where they had no effect, and shown when it is
cleared. When shown, LC-009 has already placed the window on the
passband. The dialog shrinks to fit. The tick is relabelled from "Window
follows RX filter" to "Follow RX Filter", and the two comments that name
it follow.

**Depends on** LC-009 textually (the comment in
`diversity_auto_seed_window()`, and `follow_cb()`).

### LC-021 — The window spin buttons set their digits as spin buttons

**Problem.** Upstream's `b180b79a` set the Window centre and width spin
buttons to show no decimals with `gtk_scale_set_digits(GTK_SCALE(btn),
0)`. They are `GtkSpinButton`s, not `GtkScale`s: the cast fails GTK's
type check (a critical warning each time the menu opens) and the call
does nothing.

**Change.** `gtk_spin_button_set_digits(GTK_SPIN_BUTTON(btn), 0)`, which
does what was meant.

**Upstream.** A one-commit PR is prepared, not yet pushed or opened:
branch `pr/diversity-spin-digits`, cut from `upstream/TEST` at
`b180b79a`, `src/diversity_menu.c` only, +2 −2. It builds.

---

## Local tooling (never upstream)

The means to score a change against the recorded captures before keeping
it. Usage is in `test/diversity/devtools/README.md`, whose first section
covers this branch. The findings the tools produced so far are in
`docs/diversity-measurements.md` and `docs/diversity-rade.md`, as
recorded on the feature branches.
Findings from captures taken on `TEST` itself are in
`docs/test-findings.md`, numbered T-001 onwards.

| ID | Summary | Where |
|---|---|---|
| LT-001 | Capture recorder: `make DIVCAP=1`, Capture button, format-3 writer fix, `captures/` ignored | `src/diversity_capture.[ch]`, `Makefile`, `DIVERSITY_CAPTURE` blocks in `src/diversity_auto.c` and `src/diversity_menu.c`, `.gitignore` |
| LT-002 | Test harness: seven unit tests, `replay_rade`, `run_ref`, `test_capture`, `score_rade`, `known_gaps.h` | `test/diversity/` |
| LT-003 | Wideband scorer and matched-arm generator (Python) | `test/diversity/devtools/py/` |
| LT-004 | Replay and score with manual notches: `run_ref --notch`, `score_wideband.py --notch` and `--peaks` | `test/diversity/devtools/` |
| LT-005 | Follow LC-014: no Hang passed or swept; `--hang` is an error | `test/diversity/devtools/` |
| LT-006 | Test the RADE V1 threshold pin, props and client paths; close its known gap | `test/diversity/` |
| LT-007 | CW scorer (`score_cw.py`); Findings AD-50 and AD-51 from auto-diversity | `test/diversity/devtools/py/`, `docs/diversity-measurements.md` |
| LT-008 | `run_ref --ref cw`, and a `tone` column (the tracker's readout) | `test/diversity/devtools/run_ref.c` |
| LT-009 | `test_modal` checks the fresh-install CW seed (LC-019) | `test/diversity/test_modal.c` |
| LT-010 | `test_cw` for `TEST`'s CW reference; its known gap closed | `test/diversity/` |

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
- Stand-down (`fc0b3d1e`, `94b4cc6f`) against the hold rule. LC-012's
  capture scoring bears on it: see "Scored on recorded captures" under
  LC-012.
- A RADE V2 correlator. There is no V2 reference on `TEST` yet. Three V2
  captures (two stations, 40 m, heavy multipath, both decoding at about
  0 to 8 dB) are logged as T-008 in `docs/test-findings.md` as the
  starting set. T-009 adds one on the lower sideband (spectrum inverted)
  under strong SSB interference, the case for discriminating against an
  unwanted signal.
- CW, from porting LC-017 to LC-019:
  - **The LC-012 floor binds on CW.** Three tone bins at CW's averaging
    put it at its 0.5 cap, so it, not the 0.10 setting, is the gate.
    Scored with and without it (a scratch build) over the 16 segments of
    the eleven captures: mean −0.14 dB with it, −0.07 without; without
    it is better on 7 segments and worse on 1. Small, and within the
    scorer's reach on these strong captures, but the direction is
    consistent. Revisit with marginal CW captures.
  - **LC-019's 0.2 s** wants re-sweeping on marginal CW captures.
  - **A strong carrier that appears** is accepted until key detection's
    floor climbs to it (LC-018, Limitation).
  - `score_cw.py` follows AD-50's yardstick but not its scripts, so
    AD-50's figures are not reproduced to the decimal. FSK/Digital
    scores better here than there (−0.14 against −0.42).
  - **A beacon's steady tone is rejected as a carrier** by key detection
    (T-012): the weight is held through it. FSK/Digital, which uses it,
    scores 0.3 dB more on that capture. Only a concern for beacons or
    tuning carriers that are themselves the wanted signal.

## History

- 2026-09-30: merged upstream `TEST` at `b180b79a` (the menu's horizontal
  layout). Eight hunks in `diversity_menu.c`: upstream's layout and
  labels taken, our row table, hidden window row and removed rows kept.
  LC-005 is *Upstream* (dl1ycf made the same Invert fix). LC-021 fixes
  the spin-button casts `b180b79a` added; its PR branch is prepared.
  PR #150 rebased onto `b180b79a`.
- 2026-09-30: T-010 to T-012, three CW captures (40 m and 20 m contest,
  30 m beacon) through the CW reference; `score_cw.py` fixed (segments,
  bin order) and LC-018's figures re-scored.
- 2026-09-30: LC-020, the window row hidden while following the RX
  filter, and the tick relabelled "Follow RX Filter".
- 2026-09-30: LC-017 to LC-019 (the CW reference, key detection, the
  fresh-install CW seed) and LT-007 to LT-010 (`score_cw.py` and Findings
  AD-50/AD-51, `run_ref --ref cw` and its tone column, the LC-019 check,
  `test_cw`). Settled decision 4 added: averages age whether or not a
  gate accepts the block.
- 2026-09-30: T-009, a RADE V2 capture on the lower sideband under SSB
  interference, logged for later work (not analysed).
- 2026-09-30: T-008, three RADE V2 captures logged for a future V2
  correlator (not analysed).
- 2026-09-30: code review of everything on `TEST`. Fixups: LC-011 (dead
  Hang repair), LC-013 (a misplaced comment), LC-016 (the threshold
  comment), LC-012 (two comments), LC-009 (a 20 Hz window is no longer
  seeded over). Tooling: the capture removal note, `test_cw` ignored,
  the README's format number. Decided: fixup commits (rule 9);
  harness-only engine paths are kept local and deleted in the PR; the
  0.30 Window threshold is not migrated.
- 2026-09-30: T-002 to T-007 from five more captures (band noise, 40 m
  multipath, an antenna switch). Includes T-003: `score_rade`'s streams
  aren't independent, so one sync period (~8 frames) is within its noise.
- 2026-09-30: T-001, the first RADE capture on `TEST` (two stations on
  20 m), recorded in `docs/test-findings.md`: no regression against the
  feature branch, and the weak station is beyond any fixed combination.
- 2026-09-30: LC-016 (RADE V1's Min coherence retired) and LT-006.
- 2026-09-30: LC-015 (the "Measure on" row/reference mismatch) fixed,
  and opened as the first upstream PR, dl1ycf/pihpsdr#150.
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

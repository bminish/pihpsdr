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

## Commands

List the local changes and their IDs:

```sh
git fetch upstream
git log --reverse --no-merges upstream/TEST..TEST \
    --format='%(trailers:key=Local-Change,valueonly,separator=) %h %s'
```

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

"(LC-003)" means the change applies and builds without LC-003, but only
makes full sense with it. LC-009 does not build without LC-008, because
it updates the window widgets that LC-008 keeps pointers to.

Suggested PR grouping, when we get there: LC-001 + LC-002 (settings are
restored, and restored sanely), then LC-003 + LC-004 (client/server
settings), then LC-005, then LC-008 + LC-009, then LC-007. LC-006 goes
last because it needs the measurement data behind it.

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

## History

- 2026-09-30: register created. Upstream `TEST` at `883243c0`. The eight
  local commits made on 2026-09-28 were re-cut into LC-001 … LC-009, and
  the 6 Hz default was dropped. The pre-split branch is kept as
  `history/backup/TEST-pre-split-20260930`.

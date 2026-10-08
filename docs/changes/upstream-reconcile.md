# Reconcile with upstream, 2026-10-08

Part of the local-change register: [changes.md](../changes.md).

dl1ycf took our series. This is the record of what he took, how that was
checked, what is left on our side, and what is worth a decision. It is
the first re-sync that was not a rebase of our commits over his: his
`TEST` now contains our source.

## What upstream did

`f05546e3` ("Aligned with BM's auto diversity repository", 2026-10-08) is
one commit: 10 files, +2654 −423, taking `diversity_auto.c/.h`,
`diversity_menu.c`, `rade_correlator.c/.h`, `radio.c/.h`, `receiver.c`,
`rx_panadapter.c` and `client_server.c` in a state that matches our `TEST`
at `8b13f572` (LC-047) apart from the lines listed below. The series is squashed: `git cherry`
finds no equivalent patch for any LC, so it cannot be used to tell what
was taken. Two other commits came with it, both TCI audio and neither
ours: `b5be5b73` and `6ce76a30`.

## How "carried" was checked

Not by commit, because the squash defeats that, but by tree. For the ten
files, `git diff upstream/TEST TEST` (before this re-sync, ignoring the
two TCI commits we had not merged) is **seven lines in three files**:

| Where | Upstream has | We had | Resolution |
|---|---|---|---|
| `radio.c`, `radio_restore_state()` | `diversity_auto_restore_state()` last in the `!radio_is_remote` block, after `vfo_restore_state()` | the call beside the manual diversity gain and phase (LC-001) | **take upstream's.** It is equivalent: the function only reads the props file, and no receiver exists yet at either place, so `diversity_auto_mode_changed()` cannot have run. The comment in the function that said the mode was not yet restored was true only of our placement, and is corrected (LC-048). |
| `radio.c`, `rx_panadapter.c` | three blank lines | none | left as upstream has them |

Everything else in those ten files is identical, so every `LC` whose code
lives in them is **in upstream**, including LC-006 to LC-047 as they stood
at `8b13f572` (LC-001 in his placement). Nothing was found missing.

What upstream does *not* have, because it was never meant to go: the
capture module (`src/diversity_capture.[ch]`), `.gitignore` entries,
`test/` and `docs/`. (Two hours later `f9803763` added our `Makefile`
DIVCAP block to his tree, identical to ours, without the module: see
"Follow-up commit" below.)

## Status after the re-sync

| Group | Status |
|---|---|
| LC-001, LC-002, LC-006 to LC-020, LC-023 to LC-047 (all of the series) | **Upstream**, in `f05546e3`. Written up in the topic files, which stay as the record of what each is and why. |
| LC-005, LC-021 | Upstream earlier, unchanged. |
| LC-015, PR [#150](https://github.com/dl1ycf/pihpsdr/pull/150) | Upstream through `f05546e3`; the PR was closed as superseded on 2026-10-08. |
| LC-003, LC-004 | Still **Deferred (client)**; not in his tree. |
| LC-022 | Still **Dropped**. |
| LC-048 | New, **Local**: two stale comments found by this re-sync. |
| LC-049 | New, **Local**: the reference-only code cut. |

## What remains on our side

- **Comments only (LC-048).** `diversity_auto.h` still said z0 comes from
  "ADC0" and z1 from "ADC1": arm 0 is RX1's ADC. And the restore
  comment above. Both are in files upstream now owns, so they are
  changes to his tree like any other.
- **Tooling** (never sent upstream, see below): the capture module and
  its Makefile block, `test/diversity`, the findings and docs. All
  replayed onto his tree; `make`, `make DIVCAP=1` and `make -C
  test/diversity run` pass.
- **To port from `feature/auto-diversity`**, as before and not touched by
  his commit: stand-down, time-based slew, 0.5 s default averaging, the
  Carrier tooltip. (The 24/12/6 Hz Resolution menu was ported afterwards
  from `test/auto-bins` as LC-051, and then replaced by Auto, LC-052 and
  LC-053: see below.)
- **Client/server**: LC-003, LC-004 and the faults in
  [open-items.md](open-items.md) stand. `client_server.c` is identical
  to ours, so nothing there changed.
- **Not carried** by anyone: the 6 Hz default bins (dropped earlier).

## Follow-up commit `f9803763` (2026-10-08)

"Updated copyright messages". Taken the same day. It does two things that
matter here:

- It adds a copyright line for the owner (Brendan Minish, EI6IZ) to
  `diversity_auto.[ch]` and `rade_correlator.[ch]`, and tidies the header
  of `pipewire.c` and `store.h`. The `pipewire.c` header had credited an AI
  coding assistant; it no longer does. **Rule: tools get no copyright and
  no credit in code.** Nothing of ours in `src/`, `test/` or the
  `Makefile` does (checked).
- It adds our `Makefile` DIVCAP block to his tree, byte for byte, but not
  `src/diversity_capture.[ch]`. So `make DIVCAP=1` fails in his tree for a
  missing file, and "upstream has the hooks but not the Makefile block"
  above is no longer true. Open: offer him the module, or ask him to drop
  the block. Ours is unchanged either way.

Our `TEST` was replayed onto it (114 commits, no conflicts; the only
change to the tree is his `src/` lines), so this is a rebase, not a
fast-forward: `origin/TEST` needs a force-push after the earlier one.

## `test/auto-bins` pulled in (2026-10-08)

The auto-bin work (Averaging cap, Resolution 24/12/6 Hz, Auto, the
status-line coherence; 26 commits on the old tip `8b13f572`) was replayed
onto the rebuilt `TEST`. Its LC numbers collided with LC-048 and LC-049
(taken above), so they moved up by two: **LC-048..052 on the branch are
LC-050..054 here** (messages, docs and code comments renumbered on a copy
of the branch before the replay; `test/auto-bins` itself is untouched, and
`backup/auto-bins-pre-pull-20261008` marks it). Register rows wording
edited by later docs commits were de-duplicated by hand. The two built
binaries `radev2_iq` and `score_radev2` that `f546f2c5` had committed were
dropped and gitignored.

## What upstream now holds that we said would stay local

Both were true until `f05546e3`, and the rules and the workflow notes said
so. They are not true now.

1. **The capture hooks.** `diversity_auto.c` (8 `#ifdef DIVERSITY_CAPTURE`
   blocks) and `diversity_menu.c` (5) carry the instrument's hooks,
   including `#include "diversity_capture.h"`, which his tree does not
   contain. Inert in a normal build; but `-DDIVERSITY_CAPTURE` against his
   tree cannot build, and the comments in the hooks ("never sent
   upstream", "delete before submission") are wrong in his file.
2. **The reference-only code** (cut by LC-049). LC-006's Coherence-weighted accumulation
   (`coherence_weighted` in `div_process_block()`) and LC-016's RADE V1
   quality comparison against `div_auto_coherence_min` were to be deleted
   when a PR was cut, because nothing in the radio reaches them and only
   `run_ref` uses them. They are in his tree.
3. **Citations of our register.** Three comments cite it:
   `diversity_auto.c` (LC-029, twice) and `rade_correlator.c` (LC-014, "in
   docs/changes.md"). His tree has no such file.

## Decisions

Decided 2026-10-08 (owner):

1. **DIVCAP stays whole in our `TEST`**: the module, the Makefile block,
   the `.gitignore` entries and the hooks in `diversity_auto.c` and
   `diversity_menu.c`, as they are. **It goes entirely when this work goes
   to `main`.** The removal is clean by construction: every piece is
   either a whole file (`src/diversity_capture.[ch]`), a block between
   `#ifdef DIVERSITY_CAPTURE` and its `#endif`, or inside the Makefile's
   `ifdef DIVCAP` conditional, and
   `test/diversity/devtools/remove.sh` (dry run by default, `--do` to
   act) strips them. *Not re-run on 2026-10-08:* run it dry first when the
   time comes and check that it still finds all 13 hook blocks and the
   Makefile block, since upstream's tree now carries the hooks. Until then
   the hook comments saying "never sent upstream" are wrong about his
   tree but are left, because the code is left intact.
2. **The reference-only code is cut here** (LC-049): see its write-up in
   [lc-gate.md](lc-gate.md#lc-049). He will pull it again.
3. **PR #150 is closed**, superseded by `f05546e3`.
4. **Force-push**: not decided; waits for the owner's OK.
5. **Docs are ours and stay.** The comments in his source that cite our
   register (LC-029 twice, LC-014) are left as they are.

## What was done, mechanically

```sh
git tag backup/TEST-pre-rebase-20261008 TEST        # the old series, 125 commits
git switch -c TEST-rebase-20261008 upstream/TEST
# replay every commit that is not an LC commit (78): the LT tooling and
# the docs; where one conflicted on diversity_auto.c/.h or diversity_menu.c
# the tree already had the final form, so take it; on docs, take the
# commit's own version
```

The 47 LC commits are not replayed: their effect is already in the tree.
Two things rode inside LC commits and were carried by hand: LC-035's
rewording of `diversity_capture.h` (our file) and two history lines that
LC-039 and LC-040 had added. Before the documentation edits that
follow this page, the new tip's `docs/`, `test/`, `Makefile` and capture
module equalled the old tip's.

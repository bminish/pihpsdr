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
capture module (`src/diversity_capture.[ch]`), the `Makefile` DIVCAP
block, `.gitignore` entries, `test/` and `docs/`.

## Status after the re-sync

| Group | Status |
|---|---|
| LC-001, LC-002, LC-006 to LC-020, LC-023 to LC-047 (all of the series) | **Upstream**, in `f05546e3`. Written up in the topic files, which stay as the record of what each is and why. |
| LC-005, LC-021 | Upstream earlier, unchanged. |
| LC-015, PR [#150](https://github.com/dl1ycf/pihpsdr/pull/150) | Upstream through `f05546e3`; the PR is still open and is now moot. Not touched (outward-facing): see the decisions below. |
| LC-003, LC-004 | Still **Deferred (client)**; not in his tree. |
| LC-022 | Still **Dropped**. |
| LC-048 | New, **Local**: two stale comments found by this re-sync. |

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
  24/12/6 Hz Resolution menu, the Carrier tooltip.
- **Client/server**: LC-003, LC-004 and the faults in
  [open-items.md](open-items.md) stand. `client_server.c` is identical
  to ours, so nothing there changed.
- **Not carried** by anyone: the 6 Hz default bins (dropped earlier).

## What upstream now holds that we said would stay local

Both were true until `f05546e3`, and the rules and the workflow notes said
so. They are not true now.

1. **The capture hooks.** `diversity_auto.c` (8 `#ifdef DIVERSITY_CAPTURE`
   blocks) and `diversity_menu.c` (5) carry the instrument's hooks,
   including `#include "diversity_capture.h"`, which his tree does not
   contain. Inert in a normal build; but `-DDIVERSITY_CAPTURE` against his
   tree cannot build, and the comments in the hooks ("never sent
   upstream", "delete before submission") are wrong in his file.
2. **The reference-only code.** LC-006's Coherence-weighted accumulation
   (`coherence_weighted` in `div_process_block()`) and LC-016's RADE V1
   quality comparison against `div_auto_coherence_min` were to be deleted
   when the PR was cut, because nothing in the radio reaches them and only
   `run_ref` uses them. They are in his tree.
3. **Citations of our register.** Three comments cite it:
   `diversity_auto.c` (LC-029, twice) and `rade_correlator.c` (LC-014, "in
   docs/changes.md"). His tree has no such file.

## Decisions

None of these is decided yet; the outcome of each goes in
[history.md](history.md) when it is.

1. Capture hooks and module: offer him the module and the Makefile block
   (so the hooks mean something), or ask him to strip the hooks (we then
   keep a patch), or leave as is and correct our comments. *Open.*
2. Reference-only code: keep it in his tree for `run_ref`'s sake, or cut
   it as the rules said. *Open.*
3. PR #150: close with a note that `f05546e3` supersedes it. *Open.*
4. Force-push of the rebuilt `TEST` to `origin`. *Waiting for the owner's
   OK, as always.*
5. The register: kept as the record of the series. New work on his tree
   carries on from LC-049, and the "proposed PR series" and "cutting a PR
   branch" notes now apply only to such new work.

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

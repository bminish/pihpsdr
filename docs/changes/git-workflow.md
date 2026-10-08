# Git workflow: staying in sync, cutting PRs

Part of the local-change register: [changes.md](../changes.md).

## Commands

List the local changes and their IDs:

```sh
git fetch upstream
git log --reverse --no-merges upstream/TEST..TEST \
    --format='%(trailers:key=Local-Change,valueonly,separator=) %h %s'
```

The same with `key=Local-Tooling` lists the tooling commits.

Every commit's trailer parses (LC-031 to LC-037 and the tooling commits
that had it in a paragraph of its own were reworded at the 2026-10-05
re-sync), so the command above lists the whole series. The two trailer
lines must stay in the same paragraph as `Co-Authored-By:`, or git does
not read them.

See which local changes upstream has already taken, in any form (a `-`
means upstream has an equivalent patch):

```sh
git cherry -v upstream/TEST TEST
```

Re-synchronise with upstream by **rebasing**, so the series stays a
line of small commits on top of `upstream/TEST` that can be cherry-picked
one at a time (merging was the rule until 2026-10-01; see [history.md](history.md)):

```sh
git fetch upstream
git tag backup/TEST-pre-rebase-<date> TEST
git switch -c TEST-rebase-<date> TEST
git rebase -i upstream/TEST      # drop what upstream took, resolve, reword
# every source commit compiles; make -C test/diversity run passes
git switch TEST && git reset --hard TEST-rebase-<date>
git push --force-with-lease origin TEST     # only with the owner's OK
git branch -d TEST-rebase-<date>
```

A rebase rewrites `origin/TEST`, so it is force-pushed, and only after
the owner has looked at the result. The backup tag keeps the old
series (a tag, not a branch, so backups stay out of the branch list). If upstream has taken a change, drop its commit in the rebase
and mark it *Upstream* in the [register](../changes.md#register). If upstream reshapes the
code a change sits in, the change is re-expressed on upstream's shape
(its LC number stays), not used to undo upstream's.

Build a PR branch for one change (or one group) when the time comes:

```sh
git switch -c pr/lc-001 upstream/TEST
git cherry-pick <commit of LC-001>
```

## Cutting a PR branch

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
  - LC-051: the fixed-width branch of `div_target_hz()` (the engine
    honours a `div_auto_resolution` above zero only because `run_ref
    --resolution` and the unit tests set one). With the branch gone,
    `div_auto_resolution` is Auto always and can be dropped from the
    engine; the field stays on the wire and in the props file.
- **Drop the `Local-Change:` trailer** if the maintainer does not want it.
- **Comments may cite `docs/`.** Our docs don't go upstream, and upstream
  code already cites `docs/diversity-measurements.md`, so a reference to
  a finding or to this register is left as it is.

## Dependencies between changes

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
- LC-016 uses LC-008's slider pointer and edits LC-026's switch, so it
  goes after both.
- LC-017 adds CW to LC-026's switch and to the menu's
  `store_ref_values()` / `restore_ref_values()`.
- LC-028 and LC-029 build on LC-025's floor. LC-028 is two pieces that
  are one change because neither is safe alone: Best's SNR from the floor
  without the 2 dB / 1 s rule collapses (−17.97 dB on `154822`), and the
  rule on the temporal floor is a coin toss (see LC-028).
- LC-025, LC-027 to LC-030 were checked on 2026-10-01: LC-027, LC-025
  and LC-030 apply to bare `TEST` alone, LC-028 and LC-029 onto `TEST` +
  LC-025, and each reverts from the tip (LC-025 after LC-029 and LC-028)
  and builds.
- LC-022 is dropped (2026-10-07), and with it the helper
  `div_arm_swapped()`: LC-034 and LC-044 find RX1's ADC from
  `receiver[0]->adc`, so neither needs anything else. Both apply to bare
  `upstream/TEST` (by construction; not cherry-picked there). LC-045
  needs LC-012 (and so LC-008, with its fixups).
- LC-048 to LC-052 (branch `test/auto-bins`; not applied to bare
  `upstream/TEST`): LC-048 stands alone. LC-049 (the 24 / 12 / 6 Hz menu)
  is superseded in part by LC-051, which removes the control; what stays of
  it is `DIV_MIN_NFFT` 2048. LC-050 needs LC-049 and LC-048 (the 5 s tier
  sits under the cap). LC-051 needs LC-050. For a PR, fold LC-049 into
  LC-050 and LC-051. LC-052 stands alone (menu only).
- LC-046 does not apply to bare `upstream/TEST` (checked 2026-10-06): it
  conflicts with LC-002's `DIV_*_WIDTH_DEFAULT` constants, LC-009's
  `diversity_auto_seed_window()` and LC-017's CW width default and
  `div_width_default()` case. The smallest set was not worked out; the
  CW group's prerequisites (LC-012, LC-013, LC-015) are probably not
  needed, and LC-017 itself needs more than it looks.
- Reverting from the tip: a change goes with its fixups and everything
  that depends on it, newest first. Checked 2026-09-30: only LC-001,
  LC-003, LC-005, LC-007, LC-014 and LC-019 revert cleanly on their own.
  The fixups and the CW changes edit lines of most of the others. This
  matters less than it did, because PR branches are cut fresh from
  `upstream/TEST` with the fixups folded in (see [Cutting a PR branch](#cutting-a-pr-branch)).
  Not re-checked since the 2026-10-01 rebase.

**First PR:** LC-015, opened 2026-09-30 as
[dl1ycf/pihpsdr#150](https://github.com/dl1ycf/pihpsdr/pull/150) from
`pr/diversity-menu-ref-row`.

Suggested PR grouping, when we get there: LC-001 + LC-002 (settings are
restored, and restored sanely) with LC-026 (which answers upstream's
"THIS MUST BE CORRECTED"), then LC-008 + LC-009, then LC-007, then LC-010 +
LC-011 + LC-014 + LC-016 (RADE: resync, Hang slider gone, no timeout,
no threshold) and LC-012. LC-013 (notches) stands
alone and can go at any point. LC-017 + LC-018 + LC-019 (CW) go after
LC-012, LC-013 and LC-015. The "Measure on" order and the CW row are the
part most likely to interest upstream on their own. LC-020 (menu tidy)
can go with LC-008 + LC-009. LC-006 goes last because it needs the
measurement data behind it. LC-027 is a fix that stands alone; LC-030
(Level output) stands alone and is the most likely of the noise-floor
group to interest upstream on its own; LC-025, LC-028 and LC-029 go
together, after LC-012.

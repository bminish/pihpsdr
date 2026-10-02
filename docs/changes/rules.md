# House rules

Part of the local-change register: [changes.md](../changes.md).

How a local change is cut, named and kept so that it can go upstream later as a series of small PRs. The git commands are in [git-workflow.md](git-workflow.md).

1. **One change, one commit.** A commit does one thing that can be
   explained in a sentence. Anything that bundles a fix with an unrelated
   behaviour change is split before it lands on `TEST`.
2. **Every change has an ID.** It is `LC-NNN`, carried as a
   `Local-Change: LC-NNN` trailer in the commit message and used in the
   [register](../changes.md#register) and as the heading of its write-up. Commit hashes change when a branch is rebased, but the
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
7. **The register is local.** `docs/changes.md` and `docs/changes/` are
   never part of an upstream PR. It is updated in the same push as the change it
   describes.
8. **Tooling is not a change.** The capture recorder, the test harness
   and the findings docs are local tooling. They carry
   `Local-Tooling: LT-NNN` instead of `Local-Change:`, are never part of
   an upstream PR, and no LC commit may depend on them. Before trusting a
   change, score it with the tools (see [tooling.md](tooling.md)).
9. **Corrections to a landed change are fixup commits.** A later fix to
   an LC's own code (a stale comment, dead code it left behind) is a
   separate commit whose subject starts `Diversity: LC-NNN fixup -` and
   which carries that LC's `Local-Change:` trailer. `TEST` is never
   rewritten for them. When the LC's PR branch is cut, its fixups are
   folded into it (see [git-workflow.md](git-workflow.md#cutting-a-pr-branch)).
10. **ADC naming follows the hardware: ADC1 and ADC2.** Upstream's
    `890ed310` relabelled the diversity attenuators from ADC0/ADC1 to
    ADC1/ADC2, the names the hardware gives the two converter paths.
    Every user-visible string, comment and document of ours does the
    same.
    - Identifiers keep their indices: `adc[0]` is ADC1 and `adc[1]` is
      ADC2. `att0`/`att1` and the capture fields stay as they are.
    - "Arm 0" and "arm 1" are the combiner's inputs, not converters. Arm
      0 is ADC1 unless RX1 is set to ADC2, which exchanges the arms
      (LC-022). Say "arm" when the arm is meant, and name the ADC only
      when the converter is meant.
    - Everything written before 2026-10-02 was converted at the
      re-sync (LC-035, LT-018 and the docs). Findings that quote a
      capture's own note keep the new names too.

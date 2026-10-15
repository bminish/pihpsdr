# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the `TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a few local changes on top of it, mostly to auto diversity. This file is the front page of the register: what is still local, where the finished work is written up, and where the rules, decisions and open work are.

**Where we are (2026-10-15).** dl1ycf has taken our diversity series into `upstream/TEST` twice: `f05546e3` (2026-10-08, LC-001 to LC-047, one squashed commit) and `07ae1f56` (2026-10-15, "updated AUTO DIVERSITY by aligning with Brendan's repo", LC-048 to LC-058). Of the 57 changes in the register, **52 are accepted** and no longer differ from upstream (two of them he made himself), **2 are pending** (local only: LC-059, LC-060, the objective and reference order), 2 are deferred (LC-003, LC-004) and 1 is dropped (LC-022). LC-041 to LC-043 belong to another branch. He changed one thing on the way in: LC-056's panadapter repaint in `diversity_client_set_status()` is gone. `TEST` is `upstream/TEST` (`07ae1f56`, 0 behind) plus our local-only layer: the capture recorder and its `Makefile` block (`make DIVCAP=1`, ours and never going upstream), the test harness and tools, and these docs. The next free number is **LC-061**.

## Register

### Pending: local, not yet in upstream

Status **Local**: carried here only. Both edit files he owns (`radio.h`, `diversity_menu.c`, `diversity_auto.h`). Every change from LC-048 to LC-058 arrived in `07ae1f56` (2026-10-15) and moved to [accepted.md](changes/accepted.md); how that was checked: [upstream-reconcile.md](changes/upstream-reconcile.md#2026-10-15-the-second-reconcile).

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-059](changes/lc-menu.md#lc-059) | UI | The objective list reads Manual, Sum, Null, Best: the `DIV_AUTO_*` enum is reordered to match (`DIV_MANUAL` stays 0), so the combo needs no mapping. Null and Sum swap their stored numbers; no props migration, wire not bumped | radio.h, diversity_menu.c | — | Local |
| [LC-060](changes/lc-menu.md#lc-060) | UI | The "Measure on" list is the `DIV_REF_*` order (Window, CW, FSK/Digital, Carrier, RADE V1): the enum is reordered, `div_ref_rows[]` and its two converters go, the range check ends at RADE V1. Four reference numbers move; same decisions as LC-059 | diversity_auto.h, diversity_auto.c, diversity_menu.c | — | Local |

LC-059 and LC-060 are independent. The capture format is bumped to 5 for both (LT-033) so the existing captures replay as recorded; `reset_diversity_props.py` (LT-034) clears the diversity settings from a props file for the one-time break and for testing as a new user.

**Possible PRs, when he asks or we choose to send:** LC-059 and LC-060 as one pair (they are one request and one break), written up for him in [menu-notes-dl1ycf.md](menu-notes-dl1ycf.md), with the wire-version question that is his to decide.

### Deferred and dropped

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-003](changes/lc-settings.md#lc-003) | Fix | Client settings block starts from the settings in force | server_thread.c, diversity_menu.c | — | Deferred (client) |
| [LC-004](changes/lc-settings.md#lc-004) | Fix | Keep a client's Min coherence change | diversity_auto.c | (LC-003) | Deferred (client) |
| [LC-022](changes/lc-engine.md#lc-022) | Fix | Moving RX1's ADC restarts the statistics (the routing of arm 0 is upstream's, `5db64949`; the restart was not worth a change) | — | — | Dropped (2026-10-07) |

LC-003 and LC-004 are the client half of the settings work and wait on the client/server model (see Open items). LC-022's routing of arm 0 is his (`5db64949`); the restart it added was not worth a change.

### Accepted: in upstream

52 changes, from LC-001 to LC-058 (less the three on another branch and the dropped ones), grouped by topic with links to each write-up: **[changes/accepted.md](changes/accepted.md)**. LC-005 and LC-021 he made himself; LC-001 to LC-047 arrived in `f05546e3` and LC-048 to LC-058 in `07ae1f56`. How that was verified, what it left over, and the decisions that followed: [changes/upstream-reconcile.md](changes/upstream-reconcile.md).

## Where things are

| Document | What it holds |
|---|---|
| [changes/accepted.md](changes/accepted.md) | The 52 accepted changes, by topic, each linking to its write-up |
| [changes/upstream-reconcile.md](changes/upstream-reconcile.md) | The 2026-10-08 and 2026-10-15 reconciles: what upstream took, how it was checked, what is left, decisions |
| [changes/rules.md](changes/rules.md) | House rules: one change per commit, IDs, independence, fixups, what is never committed, ADC naming |
| [changes/git-workflow.md](changes/git-workflow.md) | Listing the series, re-syncing, cutting a PR branch, dependencies between changes |
| [changes/settled-decisions.md](changes/settled-decisions.md) | Design decisions not reopened without new evidence |
| [changes/ownership.md](changes/ownership.md) | Who owns the menu and the engine, and the engine reviewed against that (E1 to E8) |
| [changes/tooling.md](changes/tooling.md) | The local test and capture tooling (LT-NNN) and its known gaps |
| [changes/open-items.md](changes/open-items.md) | Client/server faults, items flagged for later, what is not carried, Pi CPU, work still to port |
| [changes/history.md](changes/history.md) | Dated log of re-syncs, landings and decisions |
| [menu-notes-dl1ycf.md](menu-notes-dl1ycf.md) | What our engine changes need from the menu, written for dl1ycf |
| [test-findings.md](test-findings.md) | Captures taken on `TEST` (T-001 onwards) |
| [diversity-measurements.md](diversity-measurements.md), [diversity-rade.md](diversity-rade.md) | Findings and the RADE design record from the feature branches |

The write-ups, one file per topic: [settings](changes/lc-settings.md), [menu](changes/lc-menu.md), [estimate and gate](changes/lc-gate.md), [RADE V1](changes/lc-rade.md), [CW](changes/lc-cw.md), [noise floor and Best](changes/lc-noise-floor.md), [engine, combiner and threads](changes/lc-engine.md), [averaging and bin width](changes/lc-averaging.md).

## Working rules, in brief

- **House rules.** One change, one commit, with a permanent `LC-NNN` ID (never a hash) in a `Local-Change:` trailer. Each commit builds alone and reverts from the tip. Fixes before behaviour changes. Only source is committed. Corrections are `LC-NNN fixup` commits. Tooling is `LT-NNN` and no LC depends on it. User-visible text says ADC1 and ADC2. Claude gets no copyright or credit in file contents; the commit trailers stay. Full rules: [changes/rules.md](changes/rules.md).
- **Settled decisions** (changed only by a capture that shows the rule failing; evidence in [changes/settled-decisions.md](changes/settled-decisions.md)):
  1. No hang or timeout in any reference: on RADE V1 a new lock replaces an old one; elsewhere the coherence gate decides and Averaging forgets.
  2. Hold a good weight through fades.
  3. No mechanisms for cases under 1 %.
  4. Averages age at the Averaging time whether or not a gate accepts the block.
- **Who owns what.** dl1ycf mostly takes `diversity_menu.c`; we take `diversity_auto.c`. The engine writes settings only at start-up or when a client takes the server's; it never writes a setting the menu can change or substitutes one, and mode-dependent settings are to move into `profiles.c`. Menu halves of our changes are kept minimal and written up for him: [changes/ownership.md](changes/ownership.md).
- **Git workflow.** Re-sync by rebasing onto `upstream/TEST` on a new branch, with the old `TEST` kept as a `backup/` tag, and force-push to the fork only with the owner's OK (the fork is `origin`; nothing is ever pushed to his repo). When he squashes our work, compare trees rather than commits. A PR branch is cut fresh from `upstream/TEST` with the fixups folded in: [changes/git-workflow.md](changes/git-workflow.md).

## Local tooling

The capture recorder (`make DIVCAP=1`), the unit tests under `test/diversity/`, and the replay and scoring tools (`run_ref`, `replay_rade`, `score_*.py`) are how a change is scored on recorded captures before it is kept. They carry `LT-NNN`, and no LC depends on them. **DIVCAP is ours and stays on `TEST` as an option; it is not going upstream.** It stays whole in our tree (the module `src/diversity_capture.[ch]`, the `Makefile` block, `.gitignore`, the `make DIVCAP=1` build) and comes out entirely only if and when `main` is cut. A `DIVCAP=1` build also has the old "Level output" tick, to switch the level normaliser off for comparison (LT-027; plain builds always level). His tree carries the recorder's hooks (the `#ifdef DIVERSITY_CAPTURE` blocks in `diversity_auto.c` and `diversity_menu.c`, which he took with our files) but neither the module nor the `Makefile` block, so `make DIVCAP=1` cannot work there; that is his tree's state, not something we send. A plain build of `TEST` is the same as a build of his.

## Open items

- **Client/server** faults are recorded, not fixed (decided 2026-10-01); upstream is taken there. LC-003 and LC-004 wait on it.
- **DIVCAP in his tree:** the hooks are there and the module and `Makefile` block are not; that stays so (ours, local).
- **Flagged:** upstream text that still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).
- **Stand-down: decided not to port (2026-10-14, T-026, settled decision 5).**
- **To port** from `feature/auto-diversity`: time-based slew, 0.5 s default averaging, the Carrier tooltip. The 24/12/6 Hz Resolution menu was ported as LC-051 and then replaced by Auto (LC-052, LC-053).
- **Pi CPU: rerun done (2026-10-08).** No regression, no dropped blocks, no bottleneck on the CM5; the worst row is RADE V1 on a single sideband at 1536 kHz, 13 % of one core of four ([findings](bench/pi5-rerun-20261008.md)). Still to do: the functional suite on the Pi (needed `libopus-dev`; fixed in the bundle).

All of it: [changes/open-items.md](changes/open-items.md).

## History

Latest: 2026-10-15, upstream took LC-048 to LC-058 (`07ae1f56`), dropping LC-056's repaint; `TEST` was rebuilt on it and force-pushed; then LC-059 and LC-060 (objective and reference order, with the capture format bumped to 5). Before that: 2026-10-14 `TEST` rebased onto `61f9a023`, LC-055 to LC-058 added, the empty-band stand-down decided against (T-026); 2026-10-08 upstream took the series (`f05546e3`); 2026-10-07 LC-022 dropped; 2026-10-06 re-synced onto `c60db7b4`. Full log: [changes/history.md](changes/history.md).

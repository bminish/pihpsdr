# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the `TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a few local changes on top of it, mostly to auto diversity. This file is the front page of the register: what is still local, where the finished work is written up, and where the rules, decisions and open work are.

**Where we are (2026-10-08).** dl1ycf took our whole series into `upstream/TEST` (`f05546e3`, one squashed commit). Of the 52 changes in the register, **41 are accepted** and no longer differ from upstream (two of them he made himself), **8 are pending** (local only), 2 are deferred (LC-003, LC-004) and 1 is dropped (LC-022). LC-041 to LC-043 belong to another branch. `TEST` is 0 behind and 115 ahead of `upstream/TEST` (`f9803763`). The next free number is **LC-056**.

## Register

### Pending: local, not yet in upstream

Status **Local**: carried here only. LC-048, LC-049 and LC-055 edit files he now owns; LC-050 to LC-054 came from the branch `test/auto-bins`.

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-048](changes/lc-engine.md#lc-048) | Comments | Two stale comments: the arms are RX1's ADC and the other (not ADC0/ADC1), and the restore does not depend on call order | diversity_auto.c/.h | — | Local |
| [LC-049](changes/lc-gate.md#lc-049) | Cleanup | The reference-only code cut: Coherence-weighted accumulation, and RADE V1's pinned Min coherence comparison | diversity_auto.c | LC-006, LC-016 (upstream) | Local |
| [LC-050](changes/lc-averaging.md#lc-050) | Behaviour | The Averaging slider stops at 6 s (was 30) | diversity_menu.c, diversity_auto.c | — | Local |
| [LC-051](changes/lc-averaging.md#lc-051) | Behaviour | Resolution offers 24 / 12 / 6 Hz; 3 Hz retired; `DIV_MIN_NFFT` 2048 (the entries go with LC-053) | diversity_menu.c, diversity_auto.c | — | Local |
| [LC-052](changes/lc-averaging.md#lc-052) | Behaviour | Resolution gains Auto: the bin width from Averaging and the reference | diversity_menu.c, diversity_auto.c/.h | LC-050, LC-051 | Local |
| [LC-053](changes/lc-averaging.md#lc-053) | UI | Auto is the only bin width; the Resolution control is removed (pinned like Hang) | diversity_menu.c, diversity_auto.c | LC-052 | Local |
| [LC-054](changes/lc-menu.md#lc-054) | UI | The status line shows the coherence on every reference with a Min coherence (Window, Carrier, CW, FSK/Digital) | diversity_menu.c | — | Local |
| [LC-055](changes/lc-engine.md#lc-055) | Behaviour | Level output always on where it applies; the menu tick and the saved setting removed (when it applies and when not is listed in the write-up) | diversity_auto.c/.h, diversity_menu.c | LC-030 (upstream) | Local |

Dependencies among these: LC-052 needs LC-050 and LC-051, and LC-053 needs LC-052. LC-051's entries go away with LC-053, so LC-051, LC-052 and LC-053 travel together. LC-048, LC-049, LC-050, LC-054 and LC-055 stand alone (LC-055 builds on LC-030, which is upstream). Each applies on top of `upstream/TEST` as it stands; none depends on our tooling.

**Possible PRs, when he asks or we choose to send:**
1. LC-050 (the Averaging cap), then LC-051 + LC-052 + LC-053 folded as one (Auto bin width, no Resolution control). Their evidence is T-019 to T-022 in [test-findings.md](test-findings.md).
2. LC-054 (coherence on the status line): stands alone and is small.
3. LC-048 and LC-049 (two comments, and the reference-only code cut): only as a patch he can take or leave, since they tidy his tree rather than add anything.
4. LC-055 (Level output always on): removes a control from his menu file; written up for him, with the full list of when it applies.

### Deferred and dropped

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-003](changes/lc-settings.md#lc-003) | Fix | Client settings block starts from the settings in force | server_thread.c, diversity_menu.c | — | Deferred (client) |
| [LC-004](changes/lc-settings.md#lc-004) | Fix | Keep a client's Min coherence change | diversity_auto.c | (LC-003) | Deferred (client) |
| [LC-022](changes/lc-engine.md#lc-022) | Fix | Moving RX1's ADC restarts the statistics (the routing of arm 0 is upstream's, `5db64949`; the restart was not worth a change) | — | — | Dropped (2026-10-07) |

LC-003 and LC-004 are the client half of the settings work and wait on the client/server model (see Open items). LC-022's routing of arm 0 is his (`5db64949`); the restart it added was not worth a change.

### Accepted: in upstream

41 changes, from LC-001 to LC-047, grouped by topic with links to each write-up: **[changes/accepted.md](changes/accepted.md)**. LC-005 and LC-021 he made himself; the rest arrived in `f05546e3`. How that was verified, what it left over, and the decisions that followed: [changes/upstream-reconcile.md](changes/upstream-reconcile.md).

## Where things are

| Document | What it holds |
|---|---|
| [changes/accepted.md](changes/accepted.md) | The 41 accepted changes, by topic, each linking to its write-up |
| [changes/upstream-reconcile.md](changes/upstream-reconcile.md) | The 2026-10-08 reconcile: what upstream took, how it was checked, what is left, decisions |
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

The capture recorder (`make DIVCAP=1`), the unit tests under `test/diversity/`, and the replay and scoring tools (`run_ref`, `replay_rade`, `score_*.py`) are how a change is scored on recorded captures before it is kept. They carry `LT-NNN`, and no LC depends on them. DIVCAP stays whole in our tree until `main`, where it comes out entirely. His tree has the recorder's hooks (13 `#ifdef DIVERSITY_CAPTURE` blocks, from `f05546e3`) and our `Makefile` block (`f9803763`) but not the module `src/diversity_capture.[ch]`, so `make DIVCAP=1` fails there; offering him the module, or asking him to drop the block, is open. Five known gaps are reported but not counted. Register and notes: [changes/tooling.md](changes/tooling.md); usage: `test/diversity/devtools/README.md`.

## Open items

- **Client/server** faults are recorded, not fixed (decided 2026-10-01); upstream is taken there. LC-003 and LC-004 wait on it.
- **DIVCAP in his tree:** the module is missing (above).
- **Flagged:** upstream text that still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).
- **To port** from `feature/auto-diversity`: stand-down (which conflicts with the hold rule and needs a decision), time-based slew, 0.5 s default averaging, the Carrier tooltip. The 24/12/6 Hz Resolution menu was ported as LC-051 and then replaced by Auto (LC-052, LC-053).
- **Pi CPU: rerun done (2026-10-08).** No regression, no dropped blocks, no bottleneck on the CM5; the worst row is RADE V1 on a single sideband at 1536 kHz, 13 % of one core of four ([findings](bench/pi5-rerun-20261008.md)). Still to do: the functional suite on the Pi (needed `libopus-dev`; fixed in the bundle).

All of it: [changes/open-items.md](changes/open-items.md).

## History

Latest: 2026-10-08, upstream took the series (`f05546e3`); `TEST` rebuilt on it, LC-048 and LC-049 added, `test/auto-bins` pulled in as LC-050 to LC-054, and replayed onto his copyright commit (`f9803763`). Before that: 2026-10-07 LC-022 dropped; 2026-10-06 re-synced onto `c60db7b4`. Full log: [changes/history.md](changes/history.md).

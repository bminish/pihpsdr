# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the `TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a few local changes on top of it, mostly to auto diversity. This file is the front page of the register: what is still local, where the finished work is written up, and where the rules, decisions and open work are.

**Where we are (2026-11-21).** dl1ycf has taken our diversity series into `upstream/TEST` three times: `f05546e3` (2026-10-08, LC-001 to LC-047, one squashed commit), `07ae1f56` (2026-10-15, LC-048 to LC-058) and `30b327e2` (2026-10-27, LC-059 to LC-063, "alignment with BMs auto-div version"). Of the 63 changes in the register, **57 are accepted** and no longer differ from upstream (two of them he made himself), **none is pending**, 2 are deferred (LC-003, LC-004) and 1 is dropped (LC-022). LC-041 to LC-043 belong to another branch. `TEST` is `upstream/TEST` (`a6894cb5`, 0 behind, resynced 2026-11-21: 16 upstream commits including the consolidated WDSP, see [upstream-reconcile.md](changes/upstream-reconcile.md#2026-11-21-re-sync-onto-a6894cb5-no-lc-taken)) plus our local layer only, as one commit: the capture recorder (`make DIVCAP=1`, ours and never going upstream), the test harness and tools, and these docs. Force-pushed to `origin/TEST` with the owner's OK; the tip before the re-sync is tag `backup/TEST-pre-rebase-20261121`. **Numbering is shared across the branches:** LC-064 to LC-076 are taken on the experimental branches (the CW series LC-064 to LC-066, RADE LC-069 to LC-076) and LC-077 to LC-081 on `feature/diversity-binaural`, so the next free number is **LC-082**.

## Outstanding work

Everything else in this file is done; this is what is left. Detail and the closed list: [changes/open-items.md](changes/open-items.md).

- **Needs the owner's OK or decision:** the on-screen check of the LC-059/LC-060 rows; E3, E4 and E5 with dl1ycf (no code yet).
- **To port from `feature/auto-diversity`** (LC-082 on): time-based slew, 0.5 s default averaging, the Carrier tooltip.
- **To measure or build:** the noise floor's CW items (Best's SNR, `DIV_CW_MIN_BINS`, 48 and 1536 kHz captures, attenuator calibration); CW floor and 0.2 s on marginal captures; a notch validation capture (LC-013); a RADE V2 correlator; the open 24 Hz cases (Carrier, fast fades, other rates); the functional suite on the Pi; LC-061's seeds on captures.
- **Client/server (tracked, not fixed):** LC-003, LC-004, the radio dropping a client's settings, no menu refresh from a settings block.
- **Flagged for dl1ycf:** upstream text that still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).

## Register

### Pending: local, not yet in upstream

None. LC-059 to LC-063 arrived in `30b327e2` (2026-10-27) and moved to [accepted.md](changes/accepted.md); how that was checked: [upstream-reconcile.md](changes/upstream-reconcile.md#2026-10-30-the-third-reconcile). The next change takes **LC-082** (numbering is shared with the experimental branches, see above).

### On this branch (`feature/diversity-binaural`), not on `TEST`

Numbering is shared across the branches (see above). None of these commits carries a `Local-Change:` trailer yet. Hashes as of 2026-11-21. The branch's own account, with the evidence: [binaural/README.md](binaural/README.md).

| ID | Kind | Summary | Commits | Write-up | Status |
|---|---|---|---|---|---|
| [LC-041](changes/lc-engine.md#lc-041) | New | The ear split's two AGCs share one gain, set by the stronger arm (`wdsp/wcpAGCpair.c`, additive) | `f3c5662f`, `c6e5b1e0`, `76a85798` | lc-engine.md | Local (branch) |
| LC-042 | UI | Link AGC beside Level output; the rows below align with the top | `5b1440e7` | [lc-menu.md](changes/lc-menu.md) | Local (branch) |
| [LC-043](changes/lc-noise-floor.md#lc-043) | New | Balance ATT: the hotter arm's attenuator from the noise floor | `c876ed33`, `88e52875` | lc-noise-floor.md | Local (branch) |
| LC-064 | New | The local CW side tone synthesised in the PipeWire output callback, off by default (from `feature/synth-sidetone`, without its quantum-128 commit) | `1e1093d1` to `673e1dde` | [synth-sidetone/README.md](synth-sidetone/README.md) | Local (branch); its own branch is the review copy |
| LC-065 | Fix | A short paddle tap is no longer lost while the keyer waits for TX (from `fix/keyer-lost-first-tap`) | `6892f3a2` | commit message | Local (branch) |
| LC-077 | Fix | CW filter edits (cut, IF shift, IF width) no longer add the side tone offset again on every step; the split's second ear follows those edits and squelch | `2993ad68`, `6e204889` | [binaural/bug-cw-filter-edge-drift.md](binaural/bug-cw-filter-edge-drift.md) | Checked on the radio on `devtest/everything` (2026-11-11), not yet here |
| LC-078 | Fix | The general MUTE is a global flag over every receiver, including one that comes up later (the second ear); TCI's mute the same | `c2861024`, `cc634fa7` | [binaural/README.md](binaural/README.md) | Not checked on the radio |
| LC-079 | Fix | Balance ATT measures in Manual: the analysis thread is started for the measurement (it read a stale floor, or none) | `801939d8` | [binaural/README.md](binaural/README.md) | Not checked on the radio |
| LC-080 | Fix | The split's teardown switches the second ear off while it is still fed (no more 100 ms WDSP timeouts, ~215 ms GTK stall, unflushed channel); diversity off takes the split down first | `03e2d2c2` | [binaural/README.md](binaural/README.md) | Not checked on the radio |
| LC-081 | Fix | Unlinking the AGCs lets a waiting ear go instead of stalling for the rendezvous timeout; both ears submitted before either spectrum is drawn | `5b3925b6` | [binaural/README.md](binaural/README.md) | Unlink measured on the radio (debug build); `test/binaural` |

### Deferred and dropped

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-003](changes/lc-settings.md#lc-003) | Fix | Client settings block starts from the settings in force | server_thread.c, diversity_menu.c | — | Deferred (client) |
| [LC-004](changes/lc-settings.md#lc-004) | Fix | Keep a client's Min coherence change | diversity_auto.c | (LC-003) | Deferred (client) |
| [LC-022](changes/lc-engine.md#lc-022) | Fix | Moving RX1's ADC restarts the statistics (the routing of arm 0 is upstream's, `5db64949`; the restart was not worth a change) | — | — | Dropped (2026-10-07) |

LC-003 and LC-004 are the client half of the settings work and wait on the client/server model (see Open items). LC-022's routing of arm 0 is his (`5db64949`); the restart it added was not worth a change.

### Accepted: in upstream

57 changes, from LC-001 to LC-063 (less the three on another branch and the dropped ones), grouped by topic with links to each write-up: **[changes/accepted.md](changes/accepted.md)**. LC-005 and LC-021 he made himself; LC-001 to LC-047 arrived in `f05546e3`, LC-048 to LC-058 in `07ae1f56` and LC-059 to LC-063 in `30b327e2`. How that was verified, what it left over, and the decisions that followed: [changes/upstream-reconcile.md](changes/upstream-reconcile.md).

## Where things are

| Document | What it holds |
|---|---|
| [changes/accepted.md](changes/accepted.md) | The 57 accepted changes, by topic, each linking to its write-up |
| [changes/upstream-reconcile.md](changes/upstream-reconcile.md) | The 2026-10-08, 2026-10-15 and 2026-10-30 reconciles: what upstream took, how it was checked, what is left, decisions |
| [changes/rules.md](changes/rules.md) | House rules: one change per commit, IDs, independence, fixups, what is never committed, ADC naming |
| [changes/git-workflow.md](changes/git-workflow.md) | Listing the series, re-syncing, cutting a PR branch, dependencies between changes |
| [changes/settled-decisions.md](changes/settled-decisions.md) | Design decisions not reopened without new evidence |
| [changes/ownership.md](changes/ownership.md) | Who owns the menu and the engine, and the engine reviewed against that (E1 to E8) |
| [changes/tooling.md](changes/tooling.md) | The local test and capture tooling (LT-NNN) and its known gaps |
| [changes/open-items.md](changes/open-items.md) | Outstanding work first, then what is closed; client/server faults, flagged items, Pi CPU, work to port |
| [changes/history.md](changes/history.md) | Dated log of re-syncs, landings and decisions |
| [menu-notes-dl1ycf.md](menu-notes-dl1ycf.md) | Pending matters for dl1ycf only (E3 to E5); the earlier notes are in [archive/](archive/) |
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

Listed at the top of this page under **Outstanding work**. The detail, and what is closed (the stand-down, the Resolution menu, the Pi 5 rerun, LC-059 to LC-063), is in [changes/open-items.md](changes/open-items.md).

## History

Latest: 2026-10-30, upstream took LC-059 to LC-063 (`30b327e2`); `TEST` rebuilt on it from the non-LC commits (backup `history/backup/TEST-pre-rebase-20261030`) and force-pushed, no LC pending. Before that: 2026-10-19, `TEST` rebased onto `upstream/TEST` `b0b1d4e9` (his client-discovery fix and `wdsp` header tidy; no conflict, backup `history/backup/TEST-pre-rebase-20261019`), LC-061 added. Before that: 2026-10-15, upstream took LC-048 to LC-058 (`07ae1f56`), dropping LC-056's repaint; `TEST` was rebuilt on it and force-pushed; then LC-059 and LC-060 (objective and reference order, with the capture format bumped to 5). Before that: 2026-10-14 `TEST` rebased onto `61f9a023`, LC-055 to LC-058 added, the empty-band stand-down decided against (T-026); 2026-10-08 upstream took the series (`f05546e3`); 2026-10-07 LC-022 dropped; 2026-10-06 re-synced onto `c60db7b4`. Full log: [changes/history.md](changes/history.md).

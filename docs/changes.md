# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the `TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a few local changes on top of it, mostly to auto diversity. This file is the front page of the register: what is still local, where the finished work is written up, and where the rules, decisions and open work are.

**Where we are (2026-10-30).** dl1ycf has taken our diversity series into `upstream/TEST` three times: `f05546e3` (2026-10-08, LC-001 to LC-047, one squashed commit), `07ae1f56` (2026-10-15, LC-048 to LC-058) and `30b327e2` (2026-10-27, LC-059 to LC-063, "alignment with BMs auto-div version"). Of the 63 changes in the register, **57 are accepted** and no longer differ from upstream (two of them he made himself), **none is pending**, 2 are deferred (LC-003, LC-004) and 1 is dropped (LC-022). LC-041 to LC-043 belong to another branch. `TEST` is `upstream/TEST` (`30b327e2`, 0 behind, resynced 2026-10-30) plus our local layer only: the capture recorder (`make DIVCAP=1`, ours and never going upstream), the test harness and tools, and these docs. Force-pushed to `origin/TEST` on 2026-10-30 with the owner's OK (`c669e307` to `80a75fed`); the old tip is `history/backup/TEST-pre-rebase-20261030`. The next free number is **LC-064**.

## Outstanding work

Everything else in this file is done; this is what is left. Detail and the closed list: [changes/open-items.md](changes/open-items.md).

- **Needs the owner's OK or decision:** the on-screen check of the LC-059/LC-060 rows; E3, E4 and E5 with dl1ycf (no code yet).
- **To port from `feature/auto-diversity`** (LC-064 on): 0.5 s default averaging, the Carrier tooltip. The time-based slew is written as LC-069, to be listened to first.
- **To measure or build:** the noise floor's CW items (Best's SNR, `DIV_CW_MIN_BINS`, 48 and 1536 kHz captures, attenuator calibration); CW floor and 0.2 s on marginal captures; a notch validation capture (LC-013); a RADE V2 correlator; the open 24 Hz cases (Carrier, fast fades, other rates); the functional suite on the Pi; LC-061's seeds on captures.
- **Client/server (tracked, not fixed):** LC-003, LC-004, the radio dropping a client's settings, no menu refresh from a settings block.
- **Flagged for dl1ycf:** upstream text that still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).

## Register

### Pending: local, not yet in upstream

None. LC-059 to LC-063 arrived in `30b327e2` (2026-10-27) and moved to [accepted.md](changes/accepted.md); how that was checked: [upstream-reconcile.md](changes/upstream-reconcile.md#2026-10-30-the-third-reconcile). The next change takes **LC-064**.

### Pending: on `feature/rade-native`, for `TEST` as a patch

RADE V2 as a second RADE reference, side by side with V1 (decided 2026-10-22). Built and pushed on `feature/rade-native`; **not on `TEST`**. Five changes and one tooling item, written up in [lc-rade.md](changes/lc-rade.md#rade-v2) and, with the evidence, in [diversity-radeV2-cp.md](diversity-radeV2-cp.md). The commits do not carry `Local-Change:` trailers yet: they are added when the series is cut for `TEST`, as the IDs are permanent and the hashes are not.

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-064](changes/lc-rade.md#lc-064) | Cleanup | `DIV_REF_IS_RADE()` and `DIV_REF_LAST` in place of the tests for RADE V1 that meant "a RADE reference"; the correlator start, stop and reset go through `div_rade_start/stop/reset()`. No behaviour change | diversity_auto.c, diversity_auto.h | — | Local (branch) |
| [LC-065](changes/lc-rade.md#lc-065) | New | The RADE V2 correlator: detects and locks on the cyclic prefix (lag 128, period 160), issues a weight from the prefix-gated cross-correlation (the unit co-phase direction as built; the noise-weighted weight since LC-068) and publishes per-arm figures for the log. Standalone, nothing calls it | rade_v2_correlator.c, rade_v2_correlator.h, Makefile | — | Local (branch) |
| [LC-066](changes/lc-rade.md#lc-066) | New | `DIV_REF_RADE_V2` (reference 5) in the engine: the RADE block runs either correlator and copies V2's status into the `rade_corr_*` variables; start follows the reference; log lines for lock, loss and tracking | diversity_auto.c, diversity_auto.h, Makefile | LC-064, LC-065 | Local (branch) |
| [LC-067](changes/lc-rade.md#lc-067) | UI | The "Measure on" row, the RADE-wide menu cases through `DIV_REF_IS_RADE()`, the status line, and the panadapter overlay for V2's band. In `diversity_menu.c`, which is dl1ycf's | diversity_menu.c, rx_panadapter.c | LC-066 | Local (branch) |
| [LC-068](changes/lc-rade.md#lc-068) | Behaviour | RADE V2 Sum applies the noise-weighted (maximal-ratio) weight, `conj(h1/h0) N0/N1`, gain capped at 4, in place of the unit co-phase direction; Null keeps the unit direction. No new setting. Decoded aux above the live unit weight on 5 of 5 captures on 40 m (+0.004 to +0.113) and above the unit weight replayed on both 60 m captures (+0.017, +0.022) | rade_v2_correlator.c, rade_v2_correlator.h, diversity_auto.c | LC-065, LC-066 | Local (branch) |
| [LC-070](changes/lc-rade.md#lc-070) | Behaviour | RADE on: a request for a mode other than DIGU or DIGL leaves RADE first; the diversity RADE reference follows the waveform on air (V1 or V2) without changing the menu's setting; a second call of the RADE or Diversity menu closes it (RADE's window stays sticky otherwise) | vfo.c, diversity_auto.c, diversity_auto.h, rx_panadapter.c, new_menu.c, new_menu.h, rade_native_menu.c, rade_native_menu.h | LC-066 | Local (branch) |

Taken in this order. LC-064 can go alone; LC-065 can go alone; LC-066 without LC-067 is a reference the operator cannot select; LC-068 needs LC-065 and LC-066. The settings command does not change: `DIV_REFS` is 8 and V2 has no slot of its own, as V1. Tooling for them is LT-038.

LC-059 and LC-060 are independent. LC-063 follows LC-062. The capture format is bumped to 5 for both (LT-033) so the existing captures replay as recorded; `reset_diversity_props.py` (LT-034) clears the diversity settings from a props file for the one-time break and for testing as a new user.


### Pending: time-based slew, written on `feature/rade-native`, for `TEST`

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-069](changes/lc-slew.md#lc-069) | Behaviour | The Window, CW, FSK/Digital and Carrier references slew the applied weight with a 0.15 s time constant in place of 0.15 of the distance per block (Finding 48); the RADE references keep the per-block fraction. At or above the shipped slew in 14 of 14 cells on seven captures: 0 to 0.2 dB on five, +0.4 to +0.9 dB on one, +1.4 dB on one that opens in dead air. Not listened to | diversity_auto.c | — | Local (working tree, not committed) |

Numbered after LC-068 because LC-064 to LC-068 are taken on this branch; it does not depend on them and goes to `TEST` first if the series is cut that way.

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

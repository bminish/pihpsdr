# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the `TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and carries a few local changes on top of it, mostly to auto diversity. This file is the front page of the register: what is still local, where the finished work is written up, and where the rules, decisions and open work are.

**Where we are (2026-10-30).** dl1ycf has taken our diversity series into `upstream/TEST` three times: `f05546e3` (2026-10-08, LC-001 to LC-047, one squashed commit), `07ae1f56` (2026-10-15, LC-048 to LC-058) and `30b327e2` (2026-10-27, LC-059 to LC-063, "alignment with BMs auto-div version"). Of the 63 changes in the register, **57 are accepted** and no longer differ from upstream (two of them he made himself), **none is pending**, 2 are deferred (LC-003, LC-004) and 1 is dropped (LC-022). LC-041 to LC-043 belong to another branch. `TEST` is `upstream/TEST` (`30b327e2`, 0 behind, resynced 2026-10-30) plus our local layer only: the capture recorder (`make DIVCAP=1`, ours and never going upstream), the test harness and tools, and these docs. Force-pushed to `origin/TEST` on 2026-10-30 with the owner's OK (`c669e307` to `80a75fed`); the old tip is `history/backup/TEST-pre-rebase-20261030`. The next free number is **LC-067** (LC-064 is the synth side tone, LC-065 the keyer's lost-tap fix and LC-066 its straight key counterpart, below).

## Outstanding work

Everything else in this file is done; this is what is left. Detail and the closed list: [changes/open-items.md](changes/open-items.md).

- **LC-064 (synth side tone):** the bench checklist, the 17 sink-only taps, the build matrix (PULSE, PORTAUDIO, ALSA, macOS), client/server treatment, name and default of the option, and whether the tone leading the RF is acceptable ([cw_sidetone.md](cw_sidetone.md#before-this-can-go-upstream)). LC-065 (the keyer's lost first tap) is written up in [cw_keyer_first_tap.md](cw_keyer_first_tap.md); its open items: the hang time counting twice as fast, the cold start's 20 to 55 ms, straight-key taps.
- **CW branch `feature/cw-synth-sidetone` (LC-064 to LC-066), pushed to `origin` 2026-11-08 for review, not to upstream:** still to do before it is called ready: the LC-064 bench checklist (CAT CW, TUNE and SWR tones, volume, duplex, mode and receiver changes, device removal), a steady-state XRUN log at 256, and a regression run of the final keyer code in Iambic mode ([cw_sidetone.md](cw_sidetone.md#what-has-and-has-not-been-verified-2026-11-08)).
- **LC-065 and LC-066 (keyer):** LC-065 is measured against the unchanged build ([cw_keyer_first_tap.md](cw_keyer_first_tap.md)); LC-066 (straight key hole) is measured against the unchanged build ([cw_keyer_straight_key.md](cw_keyer_straight_key.md)); not measured: a real key, other speeds and bands. Open: the hang time's double decrement, the cold start's 20 to 55 ms. Timing review 2026-11-08 ([cw_timings.md](cw_timings.md)): four more items noted, none fixed for now: a stale TX-off timer can drop TX during keying, `moxbefore` can latch on after a slow TX drop, hang 0 with break-in keeps TX up, CAT `SD` has break-in inverted (the last two may become fixes).
- **Needs the owner's OK or decision:** the on-screen check of the LC-059/LC-060 rows; E3, E4 and E5 with dl1ycf (no code yet).
- **To port from `feature/auto-diversity`** (LC-067 on): time-based slew, 0.5 s default averaging, the Carrier tooltip.
- **To measure or build:** the noise floor's CW items (Best's SNR, `DIV_CW_MIN_BINS`, 48 and 1536 kHz captures, attenuator calibration); CW floor and 0.2 s on marginal captures; a notch validation capture (LC-013); a RADE V2 correlator; the open 24 Hz cases (Carrier, fast fades, other rates); the functional suite on the Pi; LC-061's seeds on captures.
- **Client/server (tracked, not fixed):** LC-003, LC-004, the radio dropping a client's settings, no menu refresh from a settings block.
- **Flagged for dl1ycf:** upstream text that still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).

## Register

### Pending: local, not yet in upstream

| ID | Kind | Summary | Files | Depends on | Status |
|---|---|---|---|---|---|
| [LC-064](cw_sidetone.md) | Feature | Optional local CW side tone synthesised in the PipeWire output callback instead of the 8 ms FIFO (CW menu "Synth sidetone", off by default): about 8 ms earlier at the sink at every quantum tried, 19.0 to 10.9 ms at 256; RF unchanged | sidetone.c/.h, pipewire.c, transmitter.c, cw_menu.c, radio.c/.h, audio.h, Makefile | — | Pending (branch `feature/cw-synth-sidetone`; PipeWire only; bench list open) |
| [LC-065](cw_keyer_first_tap.md) | Fix | A short paddle tap is no longer lost while the keyer waits for TX to come up or to go down: the tap is remembered, and the TX drop is cancelled if a tap arrives before its timer fires. Measured 9 of 120 taps lost, 0 with the fix (22 wpm, hang 600, 40 ms taps); paddle modes and the dot paddle in bug mode | iambic.c | — | Pending (branch `feature/cw-synth-sidetone`; measured against the unchanged build, quantum 256, 2026-11-07) |
| [LC-066](cw_keyer_straight_key.md) | Fix | Straight key: a key hold that starts in the RX to TX switch and is over before TX is up is keyed down late for the length of the hold instead of being lost; no debouncing; no element shorter than one RF ramp, so every element is fully shaped | iambic.c | LC-065 | Pending (branch `feature/cw-synth-sidetone`; measured 2026-11-07: of 90 short taps the unchanged build left 41 with no element, LC-066 gives 90 of 90 at full amplitude) |

The CW work is written up in `docs/cw_*`: [cw_sidetone.md](cw_sidetone.md), [design](cw_sidetone-design.md), [findings](cw_sidetone-findings.md), [cw_keyer_first_tap.md](cw_keyer_first_tap.md), [cw_keyer_straight_key.md](cw_keyer_straight_key.md). The tools and unit tests are LT-038. The PipeWire quantum is upstream's decision and is not changed by us. LC-059 to LC-063 arrived in `30b327e2` (2026-10-27) and moved to [accepted.md](changes/accepted.md); how that was checked: [upstream-reconcile.md](changes/upstream-reconcile.md#2026-10-30-the-third-reconcile). The next change takes **LC-064**.

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
| [cw_sidetone.md](cw_sidetone.md) | LC-064: the synthesised CW side tone, before and after, expected timing, rationale; with its [design](cw_sidetone-design.md) and [findings](cw_sidetone-findings.md) |
| [cw_keyer_first_tap.md](cw_keyer_first_tap.md) | LC-065: the keyer's lost first tap, findings, fix, regression runs, open items |
| [cw_timings.md](cw_timings.md) | CW timings: what the code says and where measurements confirm it; the break-in timer step by step; review findings |
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

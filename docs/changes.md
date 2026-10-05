# Local changes against dl1ycf/pihpsdr TEST

`TEST` on [bminish/pihpsdr](https://github.com/bminish/pihpsdr) tracks the
`TEST` branch of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr) and
carries a series of local changes on top of it, mostly to auto diversity.
This file is the register of those changes: what each one is, where it
is written up, and where the rules, decisions and open work around them
are. It is kept accurate so that upstream can refer to it when a change
is taken, or when we are asked to do more on one.

Each change is one commit with a `Local-Change: LC-NNN` trailer, kept in
a shape that can go upstream as a small PR. No PR of ours has been merged
yet; one is open ([#150](https://github.com/dl1ycf/pihpsdr/pull/150),
LC-015). Upstream made two of our fixes independently (LC-005, LC-021), and took the
routing half of a third (LC-022) in `5db64949`.

## Where things are

| Document | What it holds |
|---|---|
| [changes/rules.md](changes/rules.md) | House rules: one change per commit, IDs, independence, fixups, what is never committed, ADC naming |
| [changes/git-workflow.md](changes/git-workflow.md) | Listing the series, re-syncing by rebase, cutting a PR branch, dependencies between changes |
| [changes/settled-decisions.md](changes/settled-decisions.md) | Design decisions that are not reopened without new evidence |
| [changes/ownership.md](changes/ownership.md) | Who owns the menu and the engine, and the engine reviewed against that (E1 to E8) |
| [changes/tooling.md](changes/tooling.md) | The local test and capture tooling (LT-NNN, never upstream) and its known gaps |
| [changes/open-items.md](changes/open-items.md) | Client/server faults tracked but not fixed, items flagged for later, what is not carried, Pi CPU, work still to port |
| [changes/history.md](changes/history.md) | Dated log of re-syncs, landings and decisions |
| [menu-notes-dl1ycf.md](menu-notes-dl1ycf.md) | What our engine changes need from the menu, written for dl1ycf |
| [test-findings.md](test-findings.md) | Captures taken on `TEST` (T-001 onwards) |
| [diversity-measurements.md](diversity-measurements.md), [diversity-rade.md](diversity-rade.md) | Findings and the RADE design record from the feature branches |

The write-ups, one file per topic:
[settings](changes/lc-settings.md), [menu](changes/lc-menu.md),
[estimate and gate](changes/lc-gate.md), [RADE V1](changes/lc-rade.md),
[CW](changes/lc-cw.md), [noise floor and Best](changes/lc-noise-floor.md),
[engine, combiner and threads](changes/lc-engine.md).

## House rules

One change, one commit, with a permanent `LC-NNN` ID (never a hash).
Each commit builds alone, reverts from the tip, and applies to
`upstream/TEST` on its own where it can; real dependencies are listed
below. Fixes go before behaviour changes. Only source is committed: no
captures, props files or patches. Corrections to a landed change are
`LC-NNN fixup` commits, folded in when its PR is cut. Tooling is
`LT-NNN` and no LC depends on it. User-visible text says ADC1 and ADC2,
as the hardware does. Full rules: [changes/rules.md](changes/rules.md).

## Settled decisions

1. No hang or timeout in any reference: on RADE V1 a new lock replaces
   an old one; elsewhere the coherence gate decides and Averaging
   forgets.
2. Hold a good weight through fades.
3. No mechanisms for cases under 1 %.
4. Averages age at the Averaging time whether or not a gate accepts the
   block.

Changing one needs a capture that shows the rule failing. The evidence
behind each: [changes/settled-decisions.md](changes/settled-decisions.md).

## Who owns what

dl1ycf mostly takes `diversity_menu.c`; we take `diversity_auto.c`.
The engine writes settings only at start-up (props file) or when a
client takes the server's; otherwise it never writes a setting the menu
can change, never substitutes one, and mode-dependent settings are to
move into `profiles.c`. Menu halves of our changes are kept minimal and
written up for dl1ycf. Rules and the E1 to E8 review:
[changes/ownership.md](changes/ownership.md).

## Git workflow

The series is re-synced by **rebasing** onto `upstream/TEST` on a new
branch, with the old `TEST` kept as a `backup/` tag, and
force-pushed only with the owner's OK. A PR branch is cut fresh from
`upstream/TEST` with the change's fixups folded in. Commands, PR
cutting and the dependency notes:
[changes/git-workflow.md](changes/git-workflow.md).

## Register

In series order (LC-003, LC-004, LC-005 and LC-021 are out of the
series, deferred or taken upstream, and are listed where they stood).
Status: **Local** carried here only; **Proposed** PR
open; **Upstream** taken upstream (drop at the next re-sync);
**Deferred** out of the series for now, kept on a backup branch;
**Dropped** abandoned.

| ID     | Kind      | Summary                                               | Files                                     | Depends on | Status |
|--------|-----------|-------------------------------------------------------|-------------------------------------------|------------|--------|
| [LC-001](changes/lc-settings.md#lc-001) | Fix       | Restore the saved auto-diversity settings at start-up | radio.c                                   | —          | Local  |
| [LC-002](changes/lc-settings.md#lc-002) | Fix       | Treat impossible saved values as missing              | diversity_auto.c                          | —          | Local  |
| [LC-026](changes/lc-settings.md#lc-026) | Fix       | Live Min coherence from the selected reference's slot | diversity_auto.c                          | —          | Local  |
| [LC-003](changes/lc-settings.md#lc-003) | Fix       | Client settings block starts from the settings in force | server_thread.c, diversity_menu.c       | —          | Deferred (client) |
| [LC-004](changes/lc-settings.md#lc-004) | Fix       | Keep a client's Min coherence change                  | diversity_auto.c                          | (LC-003)   | Deferred (client) |
| [LC-005](changes/lc-menu.md#lc-005) | Fix       | Invert button swaps Null and Sum again                | diversity_menu.c                          | —          | Upstream (`b180b79a`) |
| [LC-006](changes/lc-gate.md#lc-006) | Behaviour | Retire Coherence weighting; Window threshold 0.20     | diversity_auto.c/.h, diversity_menu.c     | —          | Local  |
| [LC-007](changes/lc-engine.md#lc-007) | Behaviour | Hold stays on until the operator releases it          | diversity_auto.c, diversity_menu.c, radio.c | —        | Local  |
| [LC-008](changes/lc-menu.md#lc-008) | Behaviour | Reference change shows that reference's settings      | diversity_menu.c                          | —          | Local  |
| [LC-009](changes/lc-menu.md#lc-009) | Behaviour | Unticking Follow RX filter starts on the passband     | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| [LC-010](changes/lc-rade.md#lc-010) | Behaviour | RADE resyncs on a detection, not on a timeout         | rade_correlator.c                         | —          | Local  |
| [LC-011](changes/lc-rade.md#lc-011) | Behaviour | Hang slider removed (value unused since LC-014)       | diversity_auto.c, diversity_menu.c, rade_correlator.c | LC-010, [LC-008] | Local |
| [LC-012](changes/lc-gate.md#lc-012) | Behaviour | Coherence gate never below its own noise floor        | diversity_auto.c/.h, diversity_menu.c     | LC-008     | Local  |
| [LC-013](changes/lc-gate.md#lc-013) | Fix       | Bins in the operator's manual notches left out of the estimate | diversity_auto.c                 | —          | Local  |
| [LC-014](changes/lc-rade.md#lc-014) | Behaviour | No RADE lock timeout: a new lock replaces an old one  | rade_correlator.c/.h, diversity_auto.c/.h, diversity_menu.c | LC-010, LC-011 | Local |
| [LC-015](changes/lc-menu.md#lc-015) | Fix       | "Measure on" menu runs the reference it shows         | diversity_menu.c                          | [LC-008]   | Proposed ([#150](https://github.com/dl1ycf/pihpsdr/pull/150)) |
| [LC-016](changes/lc-rade.md#lc-016) | Behaviour | RADE V1's Min coherence retired (pinned at 0, row hidden) | diversity_auto.c, diversity_menu.c    | LC-008, LC-026 | Local |
| [LC-017](changes/lc-cw.md#lc-017) | Behaviour | A CW / Morse reference                                | diversity_auto.c/.h, diversity_menu.c, rx_panadapter.c | LC-009, LC-012, LC-013, LC-015 | Local |
| [LC-018](changes/lc-cw.md#lc-018) | Behaviour | CW tells keying from a steady carrier                 | diversity_auto.c                          | LC-017     | Local  |
| [LC-019](changes/lc-cw.md#lc-019) | Behaviour | Fresh install: CW modes start on CW at 0.2 s          | diversity_auto.c                          | LC-017     | Local  |
| [LC-020](changes/lc-menu.md#lc-020) | UI        | The follow tick reads "Follow RX Filter"              | diversity_menu.c (+ two comments)         | [LC-009]   | Local  |
| [LC-021](changes/lc-menu.md#lc-021) | Fix       | Window spin buttons set digits as spin buttons        | diversity_menu.c                          | —          | Upstream (`f5a0ce9c`, differently); #151 closed |
| [LC-022](changes/lc-engine.md#lc-022) | Fix       | Moving RX1's ADC restarts the statistics (the routing of arm 0 is upstream's, `5db64949`) | diversity_auto.c/.h | [LC-013] | Local (reduced) |
| [LC-023](changes/lc-engine.md#lc-023) | Fix       | Transmit gap and reset requests stop racing the threads | diversity_auto.c, radio.c               | —          | Local  |
| [LC-024](changes/lc-cw.md#lc-024) | Fix       | Carrier/CW readout from the zero beat; client overlay repaint | diversity_menu.c                  | [LC-017]   | Local  |
| [LC-027](changes/lc-engine.md#lc-027) | Fix       | An operator reset clears the statistics on the worker | diversity_auto.c                          | —          | Local  |
| [LC-025](changes/lc-noise-floor.md#lc-025) | Behaviour | Each arm's noise floor measured across frequency; Window/Carrier Sum noise ratio from it | diversity_auto.c/.h | — | Local |
| [LC-028](changes/lc-noise-floor.md#lc-028) | Behaviour | Best: per-arm SNR from the floor, 2 dB / 1 s switch   | diversity_auto.c                          | LC-025     | Local  |
| [LC-029](changes/lc-cw.md#lc-029) | Behaviour | CW's Sum noise ratio from the floor outside the filter | diversity_auto.c                         | LC-025     | Local  |
| [LC-030](changes/lc-engine.md#lc-030) | Behaviour | Level output: the combined output at one antenna's level | diversity_auto.c/.h, diversity_menu.c, radio.c/.h, receiver.c | — | Local |
| [LC-031](changes/lc-rade.md#lc-031) | Fix       | A RADE correlator that cannot start no longer changes the reference | diversity_auto.c | — | Local |
| [LC-032](changes/lc-menu.md#lc-032) | Fix       | The seeded window is returned to the menu, not written by the engine | diversity_auto.c/.h, diversity_menu.c | LC-009 | Local |
| [LC-033](changes/lc-noise-floor.md#lc-033) | Behaviour | The noise floor selects its percentile band instead of sorting (same result, about 4.6x cheaper) | diversity_auto.c | LC-025 | Local |
| [LC-034](changes/lc-menu.md#lc-034) | Fix       | The antenna readout names the converter, ADC1 or ADC2 (right when RX1 is on ADC2) | diversity_menu.c | LC-022 | Local |
| [LC-035](changes/lc-engine.md#lc-035) | Comments  | Comments name the ADCs ADC1 and ADC2, as the hardware does | diversity_auto.c/.h, diversity_capture.h, radio.c, receiver.c, client_server.c | [LC-022, LC-023, LC-025, LC-028] | Local |
| [LC-036](changes/lc-noise-floor.md#lc-036) | Fix       | Best's per-arm SNR from the mean noise (percentile floor scaled), and one arm clear is enough | diversity_auto.c | LC-025, LC-028 | Local |
| [LC-037](changes/lc-engine.md#lc-037) | Fix       | The weights start at unity, not 1 + 1j (`radio.c` initialisers) | radio.c | — | Local |
| [LC-038](changes/lc-rade.md#lc-038) | Fix       | The RADE V1 overlay covers the outer carriers whole (725-2225 Hz, not 750-2200) | rade_correlator.h, rx_panadapter.c | — | Local |
| [LC-039](changes/lc-menu.md#lc-039) | UI        | Restart averaging button removed                      | diversity_menu.c                          | —          | Local  |
| [LC-040](changes/lc-menu.md#lc-040) | UI        | Hold and Invert move up into the freed cell           | diversity_menu.c                          | LC-039     | Local  |
| [LC-044](changes/lc-engine.md#lc-044) | Fix       | An attenuator step moves the weight by the right arm (arm = ADC ^ swapped) | diversity_auto.c/.h | LC-022 | Local |
| [LC-045](changes/lc-gate.md#lc-045) | Fix       | The Min coherence slider's bottom is the search region's floor on FSK/Digital, not the moving occupied span | diversity_auto.c | LC-012 | Local |

"(LC-003)": applies and builds without LC-003, but only makes full sense
with it. "[LC-008]": a textual dependency only (adjacent lines). Details,
and which changes must travel together:
[changes/git-workflow.md](changes/git-workflow.md#dependencies-between-changes).

**Proposed PR series**, when we get there:
1. LC-001 + LC-002 + LC-026: settings restored, restored sanely, and from
   the right slot (answers upstream's `THIS MUST BE CORRECTED`).
2. LC-008 + LC-009 + LC-020: the menu shows each reference's own values,
   a new manual window starts on the passband, and the tick's label.
3. LC-007: Hold stays on.
4. LC-010 + LC-011 + LC-014 + LC-016: RADE resync, no Hang, no timeout,
   no threshold. Then LC-012 (gate floor).
5. LC-013 (notches): stands alone, any time.
6. LC-017 + LC-018 + LC-019 (CW): after LC-012, LC-013 and LC-015.
7. LC-025 + LC-028 + LC-029 (+ LC-033, LC-036): the noise floor and Best,
   after LC-012. LC-027 and LC-030 (Level output) stand alone.
8. LC-006 last: it needs the measurement data behind it.

Not yet placed in a group: LC-023, LC-031, LC-037 and LC-038 stand alone;
LC-022 (needs LC-013 textually) goes with LC-034 and LC-044 as the
RX1-on-ADC2 group; LC-045 goes with LC-012;
LC-024 goes with CW, LC-032 with LC-009, and LC-035 after the changes
whose comments it rewords. The "Measure on" order (LC-015, open as #150)
and the CW row are the parts most likely to interest upstream on their
own; Level output (LC-030) the most likely of the noise-floor group.

## Local tooling

The capture recorder (`make DIVCAP=1`), the unit tests under
`test/diversity/`, and the replay and scoring tools (`run_ref`,
`replay_rade`, `score_*.py`) are how a change is scored on recorded
captures before it is kept. They carry `LT-NNN`, are never sent
upstream, and no LC depends on them. Five known gaps (features the harness
checks for that `TEST` lacks, stand-down among them, plus one accepted
limitation) are reported but not counted. Register and notes: [changes/tooling.md](changes/tooling.md);
usage: `test/diversity/devtools/README.md`.

## Open items

- **Client/server** faults are recorded, not fixed (decided 2026-10-01);
  upstream is taken there. LC-003 and LC-004 wait on it.
- **Flagged:** upstream dropped the guard that refused manual weight
  sets while the loop owns the weight (taken); upstream text that
  still says ADC0/ADC1; no menu refresh on a mode change (cured by E5).
- **To port** from `feature/auto-diversity`: stand-down (which conflicts
  with the hold rule and needs a decision), time-based slew, 0.5 s
  default averaging, the 24/12/6 Hz Resolution menu, the Carrier tooltip.
- **Pi CPU: closed.** The Pi 5 copes, with headroom; the one clear win
  left is vectorising RADE V1's decimator (3× on the Pi).

All of it: [changes/open-items.md](changes/open-items.md).

## History

Latest: 2026-10-05, re-synced onto `5db64949` (LC-022 reduced, LC-044
and LC-045 added, upstream's removal of the weight guard taken, the
LC-031 to LC-037 trailers reworded); before that 2026-10-02, LC-038 and
the register split into an index and per-topic files. Full log: [changes/history.md](changes/history.md).

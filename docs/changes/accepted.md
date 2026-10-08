# Accepted changes

Part of the local-change register: [changes.md](../changes.md).

Every change here is in `upstream/TEST`. Most arrived with `f05546e3` (2026-10-08), where dl1ycf took the whole series as one squashed commit; LC-005 and LC-021 he made himself earlier. They are no longer local differences. The write-up of each, with the problem, the change and how it was checked, is in the topic file the ID links to. How the squash was verified, and what it left over: [upstream-reconcile.md](upstream-reconcile.md).

IDs are permanent and numbered in the order they were made, not in the order below. "Depends on" is how the change was built, kept for the record: `(LC-003)` applied without it but made full sense only with it, `[LC-008]` a textual dependency (adjacent lines). Details: [git-workflow.md](git-workflow.md#dependencies-between-changes).


## Settings: saved, restored and validated

[Write-ups: lc-settings.md](lc-settings.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-001](lc-settings.md#lc-001) | Fix | Restore the saved auto-diversity settings at start-up | — | `f05546e3`, with the call placed after `vfo_restore_state()` |
| [LC-002](lc-settings.md#lc-002) | Fix | Treat impossible saved values as missing | — | `f05546e3` |
| [LC-026](lc-settings.md#lc-026) | Fix | Live Min coherence from the selected reference's slot | — | `f05546e3` |

## The Diversity menu

[Write-ups: lc-menu.md](lc-menu.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-005](lc-menu.md#lc-005) | Fix | Invert button swaps Null and Sum again | — | `b180b79a` (his own) |
| [LC-008](lc-menu.md#lc-008) | Behaviour | Reference change shows that reference's settings | — | `f05546e3` |
| [LC-009](lc-menu.md#lc-009) | Behaviour | Unticking Follow RX filter starts on the passband | LC-008 | `f05546e3` |
| [LC-015](lc-menu.md#lc-015) | Fix | "Measure on" menu runs the reference it shows | [LC-008] | `f05546e3`; [#150](https://github.com/dl1ycf/pihpsdr/pull/150) closed as superseded |
| [LC-020](lc-menu.md#lc-020) | UI | The follow tick reads "Follow RX Filter" | [LC-009] | `f05546e3` |
| [LC-021](lc-menu.md#lc-021) | Fix | Window spin buttons set digits as spin buttons | — | `f5a0ce9c` (his own, differently); #151 closed |
| [LC-032](lc-menu.md#lc-032) | Fix | The seeded window is returned to the menu, not written by the engine | LC-009 | `f05546e3` |
| [LC-034](lc-menu.md#lc-034) | Fix | The antenna readout names the converter, ADC1 or ADC2 (right when RX1 is on ADC2) | — | `f05546e3` |
| [LC-039](lc-menu.md#lc-039) | UI | Restart averaging button removed | — | `f05546e3` |
| [LC-040](lc-menu.md#lc-040) | UI | Hold and Invert move up into the freed cell | LC-039 | `f05546e3` |
| [LC-047](lc-menu.md#lc-047) | UI | The second attenuator slider is labelled with its own ADC (ADC1 when RX1 is on ADC2) | — | `f05546e3` |

## The estimate and its gate

[Write-ups: lc-gate.md](lc-gate.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-006](lc-gate.md#lc-006) | Behaviour | Retire Coherence weighting; Window threshold 0.20 | — | `f05546e3` |
| [LC-012](lc-gate.md#lc-012) | Behaviour | Coherence gate never below its own noise floor | LC-008 | `f05546e3` |
| [LC-013](lc-gate.md#lc-013) | Fix | Bins in the operator's manual notches left out of the estimate | — | `f05546e3` |
| [LC-045](lc-gate.md#lc-045) | Fix | The Min coherence slider's bottom is the search region's floor on FSK/Digital, not the moving occupied span | LC-012 | `f05546e3` |
| [LC-046](lc-gate.md#lc-046) | Fix | The Carrier reference follows the RX filter when Follow is ticked: 400 Hz mid-passband, the default width | LC-002, LC-009, LC-017 (textual; not minimised) | `f05546e3` |

## RADE V1

[Write-ups: lc-rade.md](lc-rade.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-010](lc-rade.md#lc-010) | Behaviour | RADE resyncs on a detection, not on a timeout | — | `f05546e3` |
| [LC-011](lc-rade.md#lc-011) | Behaviour | Hang slider removed (value unused since LC-014) | LC-010, [LC-008] | `f05546e3` |
| [LC-014](lc-rade.md#lc-014) | Behaviour | No RADE lock timeout: a new lock replaces an old one | LC-010, LC-011 | `f05546e3` |
| [LC-016](lc-rade.md#lc-016) | Behaviour | RADE V1's Min coherence retired (pinned at 0, row hidden) | LC-008, LC-026 | `f05546e3` |
| [LC-031](lc-rade.md#lc-031) | Fix | A RADE correlator that cannot start no longer changes the reference | — | `f05546e3` |
| [LC-038](lc-rade.md#lc-038) | Fix | The RADE V1 overlay covers the outer carriers whole (725-2225 Hz, not 750-2200) | — | `f05546e3` |

## The CW reference

[Write-ups: lc-cw.md](lc-cw.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-017](lc-cw.md#lc-017) | Behaviour | A CW / Morse reference | LC-009, LC-012, LC-013, LC-015 | `f05546e3` |
| [LC-018](lc-cw.md#lc-018) | Behaviour | CW tells keying from a steady carrier | LC-017 | `f05546e3` |
| [LC-019](lc-cw.md#lc-019) | Behaviour | Fresh install: CW modes start on CW at 0.2 s | LC-017 | `f05546e3` |
| [LC-024](lc-cw.md#lc-024) | Fix | Carrier/CW readout from the zero beat; client overlay repaint | [LC-017] | `f05546e3` |
| [LC-029](lc-cw.md#lc-029) | Behaviour | CW's Sum noise ratio from the floor outside the filter | LC-025 | `f05546e3` |

## The noise floor and Best

[Write-ups: lc-noise-floor.md](lc-noise-floor.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-025](lc-noise-floor.md#lc-025) | Behaviour | Each arm's noise floor measured across frequency; Window/Carrier Sum noise ratio from it | — | `f05546e3` |
| [LC-028](lc-noise-floor.md#lc-028) | Behaviour | Best: per-arm SNR from the floor, 2 dB / 1 s switch | LC-025 | `f05546e3` |
| [LC-033](lc-noise-floor.md#lc-033) | Behaviour | The noise floor selects its percentile band instead of sorting (same result, about 4.6x cheaper) | LC-025 | `f05546e3` |
| [LC-036](lc-noise-floor.md#lc-036) | Fix | Best's per-arm SNR from the mean noise (percentile floor scaled), and one arm clear is enough | LC-025, LC-028 | `f05546e3` |

## Engine, combiner and threads

[Write-ups: lc-engine.md](lc-engine.md)

| ID | Kind | Summary | Depends on | Taken |
|---|---|---|---|---|
| [LC-007](lc-engine.md#lc-007) | Behaviour | Hold stays on until the operator releases it | — | `f05546e3` |
| [LC-023](lc-engine.md#lc-023) | Fix | Transmit gap and reset requests stop racing the threads | — | `f05546e3` |
| [LC-027](lc-engine.md#lc-027) | Fix | An operator reset clears the statistics on the worker | — | `f05546e3` |
| [LC-030](lc-engine.md#lc-030) | Behaviour | Level output: the combined output at one antenna's level (its tick removed by LC-055) | — | `f05546e3` |
| [LC-035](lc-engine.md#lc-035) | Comments | Comments name the ADCs ADC1 and ADC2, as the hardware does | [LC-023, LC-025, LC-028] | `f05546e3` |
| [LC-037](lc-engine.md#lc-037) | Fix | The weights start at unity, not 1 + 1j (`radio.c` initialisers) | — | `f05546e3` |
| [LC-044](lc-engine.md#lc-044) | Fix | An attenuator step moves the weight by the right arm (arm 0 is RX1's ADC) | — | `f05546e3` |

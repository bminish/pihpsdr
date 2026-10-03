# History

Part of the local-change register: [changes.md](../changes.md).

Newest first.

- 2026-10-03: **branches tidied.** Branches that were finished,
  contained elsewhere or idle were removed or moved under `history/`;
  pre-rebase backups became tags. Nothing was lost: each old name below
  maps to where its commits now are. `docs/TCI_PROTOCOL.md` (a
  reference for the TCI server, 2026-07-08) was on the local `master`
  only; it is now here and on `history/docs/tci-protocol`.

  | Old name | Now |
  |---|---|
  | `history/backup/<name>` (7 branches) | tag `backup/<name>` |
  | `wip/lc-025-noise-floor` | `history/diversity/noise-floor-wip` |
  | `fix/p2-unused-adc-bpf-bypass-TEST` | `history/upstream/p2-unused-adc-bpf-bypass-TEST` |
  | `test/noise-floor` | tag `noise-floor-eval-20261001` (same commit) |
  | `test/diversity-arm-swap` | contained in `history/diversity/binaural`; on `TEST` as LC-022 |
  | `test/best-floor-bias`, `test/quickselect`, `port/noise-floor`, `resync/TEST-890ed310`, `TEST-rebase-20261001` | on `TEST` (removed) |
  | `feature/parallel-rx-shutdown` (PR #139, closed 2026-10-03: superseded by upstream `49892a0b`) | `history/upstream/parallel-rx-shutdown` |
  | `master` (local) | reset to `origin/master`; its one commit is `history/docs/tci-protocol` |

- 2026-10-02: the Pi CPU item closed. LT-020 (the Pi 5 bundle) run on a
  CM5; analysis in `docs/bench/pi5-analysis.md`. The Pi copes with
  every reference at every rate. Candidates ranked: RADE V1's decimator
  (3× by vectorising), MEASURE plans from wisdom, and not decimating
  before the FFT.
- 2026-10-03: LC-039, the Restart averaging button removed from the
  Diversity menu (a UI removal in dl1ycf's file; tracked, written up for him).
- 2026-10-02: the register split into a short index (`changes.md`) and
  per-topic files under `docs/changes/`. LC-037's write-up corrected: it
  overstated the effect. The starting weight cannot delay a RADE lock
  (the correlator works on the raw arms), and a non-optimal start costs
  little on a fading HF channel. Found while splitting: LC-031 to LC-037
  carry trailers git cannot parse (flagged in open-items).
- 2026-10-02: LC-038, the RADE V1 overlay covers the outer carriers
  whole; confirmed on air.
- 2026-10-02: LC-037, the weights start at unity (`radio.c`, a 2019
  slip unmasked by the 2026 manual/auto split); written up for dl1ycf.
- 2026-10-02: LC-036 (with its naming fixup) and LT-019 brought into
  `TEST` from `test/best-floor-bias` by fast-forward.
- 2026-10-02: LC-036 and LT-019 on `test/best-floor-bias` (Best's
  percentile-floor bias, and the one-arm-clear rule).
- 2026-10-02: the findings' "What is still open" evaluated against
  `TEST`; two new issues flagged (Best's percentile floor, the 1 + 1j
  start weight); five feature-only changes listed under Pending.
- 2026-10-02: **re-synced onto upstream `890ed310`** ("small updates to
  DIV menu": the attenuators relabelled ADC1/ADC2; switching to Manual
  copies the loop's weight into the manual sliders; a
  `sanitize_man_values()` helper). Rebased on
  `resync/TEST-890ed310`. One conflict, textual: LC-030's
  `div_level_sensitive()` sat where upstream added its helper, so both
  were kept. The pre-rebase `TEST` is kept as
  `history/backup/TEST-pre-890ed310`. Then LC-034, LC-035, LT-018 and
  rule 10 (ADC naming), and the docs renamed (141 lines). PR #150 still
  applies cleanly.
- 2026-10-02: Pi 5 (CM5) `pi_bench` run recorded in `docs/bench/`; LC-033
  is 3.3× cheaper there paced, identical output.
- 2026-10-02: LT-017 (`pi_bench`) for arm64 figures; x86 reference in
  `docs/bench/`.
- 2026-10-02: LC-033 (the noise floor selects instead of sorting) and
  LT-016 (`bench_nf`), on `test/quickselect`.
- 2026-10-01: diversity takes `profiles.c`'s mode grouping (E5).
- 2026-10-01: LC-031 (E1, engine only, silent) and LC-032 (E2); LC-032's
  menu half is written up for dl1ycf in `menu-notes-dl1ycf.md`.
- 2026-10-01: dl1ycf's division of work recorded ("Who owns what: menu
  and engine"), with the engine reviewed against it: E1 to E8.
- 2026-10-01: **the noise-floor work landed** as LC-025, LC-027 to LC-030
  and LT-013 to LT-015, cut on `port/noise-floor` from the net difference
  of `test/noise-floor` (tag `noise-floor-eval-20261001`), not replayed.
  Calmer Best and Best's SNR from the floor merged into LC-028 after the
  former, measured alone, proved a coin toss. Same behaviour as the tag
  (bit-identical replays); each change checked to apply alone and revert
  from the tip. T-013 to T-018 added to `docs/test-findings.md`;
  `docs/test-noisefloor.md`, `docs/noise-floor-refactor.md` and
  `docs/feature-att-calibration.md` come with it.

- 2026-10-01: **rebased onto upstream `f5a0ce9c`** ("continued work on
  auto diversity (unfinished)"), on `TEST-rebase-20261001`; the old
  series is `history/backup/TEST-pre-rebase-20261001`. The resync rule
  changes from merge to rebase. Upstream moved the per-reference slot
  store/recall into the menu and left `// DL1YCF: THIS MUST BE CORRECTED`
  in `div_settings_load()`; LC-026 answers it. Re-expressed on upstream's
  shape: LC-008 (now the widget refresh his comment asks for), LC-009,
  LC-012 (signal blocking instead of the removed flag), LC-016 and LC-017
  (in the menu's functions). LC-020 reduced to the label (upstream greys
  the window row). Dropped: LC-005 and LC-021 (upstream). Deferred:
  LC-003, LC-004 (client; see "Client/server: tracked, not fixed"). Taken
  from upstream and flagged: no menu refresh on a mode change, and no
  scheme-1 migration. LT-012 follows in the tools. Every source commit
  compiles; the unit suite passes; `test_capture` differs on 0 of 160
  blocks; `make DIVCAP=1` builds. Later the same day: PR #150 rebased
  onto `f5a0ce9c` (mergeable), #151 closed.

- 2026-09-30: `8a393217` (branch noise floor across frequency) ported,
  measured on 39 captures, and parked on `wip/lc-025-noise-floor`: the
  estimate is right, but Sum loses on 11 captures and Best collapses on
  one. See Pending.
- 2026-09-30: LC-022 to LC-024, the three faults from
  `feature/auto-diversity`: arm 0 follows RX1's ADC, the transmit-gap and
  reset races, and the Carrier/CW readout from the zero beat with the
  client's overlay repaint. LT-011 records the arm order in captures.
- 2026-09-30: merged upstream `TEST` at `b180b79a` (the menu's horizontal
  layout). Eight hunks in `diversity_menu.c`: upstream's layout and
  labels taken, our row table, hidden window row and removed rows kept.
  LC-005 is *Upstream* (dl1ycf made the same Invert fix). LC-021 fixes
  the spin-button casts `b180b79a` added, proposed as dl1ycf/pihpsdr#151.
  PR #150 rebased onto `b180b79a`.
- 2026-09-30: T-010 to T-012, three CW captures (40 m and 20 m contest,
  30 m beacon) through the CW reference; `score_cw.py` fixed (segments,
  bin order) and LC-018's figures re-scored.
- 2026-09-30: LC-020, the window row hidden while following the RX
  filter, and the tick relabelled "Follow RX Filter".
- 2026-09-30: LC-017 to LC-019 (the CW reference, key detection, the
  fresh-install CW seed) and LT-007 to LT-010 (`score_cw.py` and Findings
  AD-50/AD-51, `run_ref --ref cw` and its tone column, the LC-019 check,
  `test_cw`). Settled decision 4 added: averages age whether or not a
  gate accepts the block.
- 2026-09-30: T-009, a RADE V2 capture on the lower sideband under SSB
  interference, logged for later work (not analysed).
- 2026-09-30: T-008, three RADE V2 captures logged for a future V2
  correlator (not analysed).
- 2026-09-30: code review of everything on `TEST`. Fixups: LC-011 (dead
  Hang repair), LC-013 (a misplaced comment), LC-016 (the threshold
  comment), LC-012 (two comments), LC-009 (a 20 Hz window is no longer
  seeded over). Tooling: the capture removal note, `test_cw` ignored,
  the README's format number. Decided: fixup commits (rule 9);
  harness-only engine paths are kept local and deleted in the PR; the
  0.30 Window threshold is not migrated.
- 2026-09-30: T-002 to T-007 from five more captures (band noise, 40 m
  multipath, an antenna switch). Includes T-003: `score_rade`'s streams
  aren't independent, so one sync period (~8 frames) is within its noise.
- 2026-09-30: T-001, the first RADE capture on `TEST` (two stations on
  20 m), recorded in `docs/test-findings.md`: no regression against the
  feature branch, and the weak station is beyond any fixed combination.
- 2026-09-30: LC-016 (RADE V1's Min coherence retired) and LT-006.
- 2026-09-30: LC-015 (the "Measure on" row/reference mismatch) fixed,
  and opened as the first upstream PR, dl1ycf/pihpsdr#150.
- 2026-09-30: LC-014 (no RADE lock timeout) and LT-005. LC-011's
  description corrected, and "Settled decisions" added.
- 2026-09-30: LC-013 (notch carve-out) and LT-004 (notch replay and
  scoring) ported.
- 2026-09-30: LT-001 and LT-002 ported (capture recorder, test harness),
  with `docs/diversity-measurements.md` and `docs/diversity-rade.md`.
- 2026-09-30: LC-010 to LC-012 ported (RADE resync, Hang pinned, gate
  noise floor).
- 2026-09-30: register created. Upstream `TEST` at `883243c0`. The eight
  local commits made on 2026-09-28 were re-cut into LC-001 … LC-009, and
  the 6 Hz default was dropped. The pre-split branch is kept as
  `history/backup/TEST-pre-split-20260930`.

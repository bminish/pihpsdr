# Local tooling (never upstream)

Part of the local-change register: [changes.md](../changes.md).

The means to score a change against the recorded captures before keeping
it. Usage is in `test/diversity/devtools/README.md`, whose first section
covers this branch. The findings the tools produced so far are in
`docs/diversity-measurements.md` and `docs/diversity-rade.md`, as
recorded on the feature branches.
Findings from captures taken on `TEST` itself are in
`docs/test-findings.md`, numbered T-001 onwards.

| ID | Summary | Where |
|---|---|---|
| LT-001 | Capture recorder: `make DIVCAP=1`, Capture button, format-3 writer fix, `captures/` ignored | `src/diversity_capture.[ch]`, `Makefile`, `DIVERSITY_CAPTURE` blocks in `src/diversity_auto.c` and `src/diversity_menu.c`, `.gitignore` |
| LT-002 | Test harness: seven unit tests, `replay_rade`, `run_ref`, `test_capture`, `score_rade`, `known_gaps.h` | `test/diversity/` |
| LT-003 | Wideband scorer and matched-arm generator (Python) | `test/diversity/devtools/py/` |
| LT-004 | Replay and score with manual notches: `run_ref --notch`, `score_wideband.py --notch` and `--peaks` | `test/diversity/devtools/` |
| LT-005 | Follow LC-014: no Hang passed or swept; `--hang` is an error | `test/diversity/devtools/` |
| LT-006 | Test the RADE V1 threshold pin, props and client paths; close its known gap | `test/diversity/` |
| LT-007 | CW scorer (`score_cw.py`); Findings AD-50 and AD-51 from auto-diversity | `test/diversity/devtools/py/`, `docs/diversity-measurements.md` |
| LT-008 | `run_ref --ref cw`, and a `tone` column (the tracker's readout) | `test/diversity/devtools/run_ref.c` |
| LT-009 | `test_modal` checks the fresh-install CW seed (LC-019) | `test/diversity/test_modal.c` |
| LT-010 | `test_cw` for `TEST`'s CW reference; its known gap closed | `test/diversity/` |
| LT-011 | Captures record which ADC arm 0 came from; `run_ref` and `test_capture` follow it; it only describes the recording, the engine does not act on it | `src/diversity_auto.c` (capture block), `test/diversity/devtools/` |
| LT-012 | Follow `f5a0ce9c`: the tools carry a copy of the menu's slot store/recall; the dropped migration is a known gap | `test/diversity/ref_slots.h`, `test_modal.c`, `test_cw.c`, `test_props.c`, `known_gaps.h`, `devtools/run_ref.c` |
| LT-013 | LC-025's checks: `test_digital`'s Window case counted, `test_rates` (48 / 192 / 1536 kHz, span limit, fallback, reset storm) | `test/diversity/` |
| LT-014 | `test_rates`' CW cases, with LC-029; the 1536 kHz / 100 Hz limitation reported | `test/diversity/test_rates.c`, `known_gaps.h` |
| LT-015 | LC-030's checks: Level output counted; `run_ref`'s `norm` column; `score_level.py` | `test/diversity/` |
| LT-016 | `bench_nf`: LC-033's selection against `qsort`, bit for bit and timed, on the engine's own functions | `test/diversity/` |
| LT-017 | `pi_bench`: one file to copy to a Pi 5 and build with only `cc`; ballpark costs of the engine's hot spots, hot and paced | `test/diversity/pi_bench.c`, `docs/bench/` |
| LT-018 | The tools name the ADCs ADC1 and ADC2 (follows LC-035) | `test/diversity/` |
| LT-019 | `test_rates` checks Best's antenna readout against known answers (follows LC-036) | `test/diversity/test_rates.c` |
| LT-020 | The Pi 5 bundle: `make pi5-bench.tar.gz`, `run_pi5.sh`; `bench_cpu` splits worker from feeder, paces as the radio, adds CW and 768/1536 kHz, and its RADE rows now lock; `pi_bench` adds the FFTW planners with wisdom and the RADE decimator against a vectorised one | `test/diversity/pi5/`, `bench_cpu.c`, `pi_bench.c`, `Makefile`, `docs/bench/` |
| LT-021 | The Carrier-follow check in `test_window` counts (LC-046); the known gap narrows to the scheme-3 migration | `test/diversity/test_window.c`, `known_gaps.h` |
| LT-022 | Follow LC-048 to LC-050: `test_props` checks the 6 s cap, the bin width pinned to Auto, and Auto's table; `test_rates` checks the transform follows Averaging; `run_ref`'s transform floor is 2048 | `test/diversity/` |
| LT-023 | The sweep drivers behind T-019 and T-020: catalogue, averaging and bin-width sweeps, RADE decode sweep, and their scorers | `test/diversity/devtools/py/sweeps/` |
| LT-024 | The dense sweep for Auto's thresholds (T-021): `sweep_dense.py`, `score_dense.py` (Sum and Null, fade rate), `agg_dense.py` (intervals) | `test/diversity/devtools/py/sweeps/` |

**LT-020.** One tarball (`make -C test/diversity pi5-bench.tar.gz`) holds
the engine's own sources (found by the compiler's dependency list),
`bench_cpu`, `pi_bench` and `run_pi5.sh`. On the Pi, `./run_pi5.sh`
checks the packages, records the machine (governor, clocks,
temperature, throttling, load), builds at `-O3`, runs everything, and
leaves one `results-<host>-<time>.tar.gz` to copy back. Every timing
also comes out as a `TSV` line for analysis. Two things it fixed in
`bench_cpu`: the RADE V1 "locked" row never locked, because the raw
buffer's spectrum is the mirror of the dial's and the row looked in the
upper passband; and "% of a core" assumed an 85.3 ms block at every rate
(it is 42.7 ms at 1536 kHz).

**LT-012.** `f5a0ce9c` moved `diversity_auto_ref_store()` and
`diversity_auto_ref_recall()` into the menu, which the tools cannot
link. `test/diversity/ref_slots.h` is a copy of the data half of
`store_ref_values()` / `restore_ref_values()` (`tool_ref_store()`,
`tool_ref_recall()`), to be kept in step with the menu. Between LT-002
and LT-012 in the series, `test_modal`, `test_cw` and `run_ref` do not
link; the radio builds at every commit.

<a id="lt-005"></a>
**LT-005.** `rade_corr_process()` no longer takes a hang, so the replay
tools stop passing one. `run_ref --hang` and `replay_rade --hang` stop
with an error pointing at LC-014 instead of silently doing nothing.
Worth knowing when reading any replay: `run_ref` is not
byte-deterministic. Its worker thread can shift a read by a block, and on
`165826` one run in three at a 600 s timer scored +39 against +57. Repeat
a replay before trusting a single difference. Measured 2026-10-02:
RADE V1 replays at the default 12 ms pace differ run to run when twelve
run in parallel (37 of 37 RADE replays). At `--pace 20000` and four at a
time they were byte-identical between runs and between builds. The other
references were byte-identical even at twelve in parallel.

The tooling commits form a stack: each later one edits files an earlier
one created. They revert in reverse order, and only LT-001 applies to
`upstream/TEST` on its own. That's fine, because none of them goes
upstream.

**LT-004.** A capture can't record a notch, but because the notch acts
downstream of the tap, a replay with one set is exactly what the radio
would have done. `run_ref --notch C:W` sets it, with the values the notch
menu stores. `score_wideband.py --notch C:W` leaves the notched bins out
of the passband score, as WDSP leaves them out of the audio, by the
engine's rule. `--peaks N` lists the steadiest strong peaks in a
capture's passband, with the notch centre that covers each, as the
starting point for a notch scenario.

**LT-003** scores `run_ref` weight series on Window, Carrier and Digital
captures: split-guard passband SNR as the findings define it, plus pass
rates, coherence and |w| in signal and noise-only blocks. It's calibrated
against Finding 38. `match_arms.py` writes a copy of a capture with arm 1
scaled to arm 0's noise floor, because every capture in the set has
lopsided arms. Usage is in the devtools README.

**LT-001** brings the recorder the feature branches used. Upstream `TEST`
kept the recorder's hooks in `diversity_auto.c` but not the recorder, the
Makefile switch or the button. Those hooks were also an older writer that
always wrote `rec_flags` as zero, so a capture taken on upstream's code
never marked a context change or an engine reset, and its replay diverged
at the first attenuator step. The round-trip test differed on 60 of 160
blocks before the fix and on none after.

**LT-002** is the harness from `feature/diversity-binaural`, taken from
before `8ea8c8f3` and adapted to `TEST`'s engine in the tools only. It
reproduces Finding 45 exactly on `190516`: the replay gives 4
acquisitions, 0.714 locked, −11.37 dB and 0.101, and the decode score
gives arm 0 332 frames at 97.9 % and arm 1 223 frames at 99.1 %. It also
independently confirms LC-006 (a stored Coherence weighting comes back
as Flat), LC-010 (the resync drops at 2.82 s; a 31 dB, 5.1 s fade keeps
its lock) and LC-011 (a stored Hang of 1 s comes back as 10 s).

## Known gaps

Features the harness was written for that `TEST` does not have, from
`test/diversity/known_gaps.h`. Each check still runs and prints its
figures; a failure is reported but not counted. When one is ported, it
gets an LC number and its line in `known_gaps.h` is deleted, so the check
becomes its regression test.

| Gap | Feature branch commit | What the check shows on `TEST` |
|---|---|---|
| Stand-down | `fc0b3d1e`, `94b4cc6f` | Never stands down on an empty band. **Conflicts with the hold rule; decision needed, see below** |
| Carrier follow flag, scheme-3 migration | `41f8700c` | The search itself is LC-046 and its `test_window` check counts. What is left is the props migration of the follow flag by scheme, which upstream dropped with the schemes (`f5a0ce9c`) |
| Wire helpers | `42f68714` | Not a behaviour: the conversion is inline on `TEST`, so the round trip cannot be called |
| CW at 1536 kHz, 100 Hz filter | none: an accepted limitation (LC-029) | CW holds; 23.4 Hz bins leave too few for the region |
| Reference scheme migration | upstream `f5a0ce9c` removed it | A scheme-1 props file loads its old reference numbers as they are (2 → RADE V1, 3 → FSK/Digital, 4 → CW). Taken from upstream and tracked, not restored |

**Stand-down is not simply a gap.** On the feature branches, the combiner
slews the weight to zero on an empty band and puts it back when the band
fills. That's the opposite of the rule on `TEST`: when there is nothing
new to correlate on, hold where we were. The feature branch measured the
cost of holding as 12.18 dB of extra noise between overs on `122843` and
3.5 dB over two thirds of a minute on `235906`, where the held weight had
been fitted with one arm 15 dB hotter. Its tuning also assumed a gate
that "passes about one no-signal block in twenty", which is the floor
LC-012 replaced. Scoring LC-012 on captures has since added evidence
(see "Scored on recorded captures" under [LC-012](lc-gate.md#lc-012)): with the flat Sum
weight, a loop allowed to track dead air lets its weight shrink to about
−14 to −20 dB, which is a stand-down in effect. Holding the station's
weight instead cost 0.5 to 3.9 dB on `235906` when the threshold was
below its default. Stand-down itself is not on `TEST`, so it has not
been scored here.

# A combined RADE V1/V2 correlator: plan, and Stage 0

> **Branch `feature/rade-native`**, 2026-10-22. The question: can one RADE reference run the V1
> pilot correlator and the V2 prefix tracker side by side, search both, and lock whichever is
> there, **without losing the strong discrimination against signals that are not RADE**? Stage 0
> is the measurement that says what a union of the two detectors does on the 171 captures.
> V2 itself: [diversity-radeV2-cp.md](diversity-radeV2-cp.md). Decided 2026-10-22: V1 and V2 stay
> separate references; auto-detection waits for the on-air V2 evaluation. This is the plan for
> after it, and the measurement that can be done before.

## 1. The plan

| Stage | What | Engine change |
|---|---|---|
| 0 | Measure which detector locks on what, over every capture (this document) | none |
| 1 | A third reference, "RADE auto": both correlators run, the weight comes from whichever locked | `diversity_auto.c`, menu row, status text |
| 2 | Optional: one shared decimator and band filter for both | refactor of `rade_correlator.c` |
| 3 | Optional: the prefix as a cheap pre-gate for V1's pilot search | see section 4: not supported by Stage 0 |

Rules for Stage 1, set so that discrimination is kept:

1. **Each module keeps its own gate, unchanged**: V1's pilot lock with its probation, V2's prefix
   lock (4 of the last 5 one-second samples above 0.30, bump within 20 samples). A combined gate
   weaker than either is a regression.
2. **A lock by one holds off the other.** The first to lock keeps the weight; the other's new lock
   is ignored while the holder's signal is present. A holder that has faded is replaced only by the
   other module's lock, which is the existing "a new lock replaces an old one" rule across modules.
3. No timeouts and no new thresholds for rare cases (settled decisions 1 and 3).
4. The status says which waveform locked; V1 and V2 remain selectable on their own.

## 2. Stage 0: the measurement

`py/cp_matrix.py` (LT-037), over all 171 captures in `captures/`. Per capture:

- **Truth** from rade_c's own receivers. V1: its V1 receiver syncs on either arm (`score_rade`,
  sync 20% or more). V2: its V2 receiver syncs (`score_radev2`, sync 20% or more and frame-sync 0.6
  or more). Both thresholds are mine. Each tool takes the sideband from the recorded passband.
- **Detectors.** The V1 pilot correlator as shipped (`replay_rade`, bank from the passband); the V1
  cyclic prefix (lag 160, period 192, same statistic and lock rule as V2; `cp_probe.py`, both banks,
  Python only: there is no C version); the V2 prefix tracker as shipped (`cp_replay`, bank from the
  passband, and both banks).

Locks, out of the captures in each class:

| In the capture (by rade_c) | V1 pilot | V1 prefix | V2 prefix (passband bank) | V2 prefix (both banks) |
|---|---|---|---|---|
| V1 only, n = 45 | **44** | 39 | 0 | 0 |
| V2 only, n = 23 | 2 | 0 | **20** | **20** |
| both, n = 1 | 0 | 0 | 1 | 1 |
| neither, n = 102 | 1 | 0 | 2 | 2 |

Time to first lock on the captures where it did, seconds:

| Detector | On | Times |
|---|---|---|
| V1 pilot | V1 | 2.05 to 3.6 for 29 of 44; the rest 6 to 44 |
| V1 prefix | V1 | 9 for 26 of 39; the rest 10 to 51 |
| V2 prefix | V2 | 5 to 7 for 18 of 20; 17 and 23 |

### What the matrix says

- **The prefix detectors do not cross-lock, either way.** The V2 prefix locks on none of the 45 V1
  captures, the V1 prefix on none of the 24 with V2. The lag separates them on real captures, not
  only on the handful I had looked at before.
- **The V1 pilot does lock on two V2 captures**, late: `20261005-230621` at 26.5 s and
  `20261005-231840` at 51.1 s (both 160 m, 750 to 2250 Hz, DIGU). The V2 prefix had locked on both
  at 5 and 7 s. Why the pilot locks on a V2 signal has not been looked at. With the first-lock rule
  the V1 lock would have been ignored on both; without it V1's weight would have replaced V2's
  mid-run.
- **Locks on signals that are neither:** the V1 pilot on one (`20260905-151108`, a wideband
  AM-mode capture, at 39 s), the V2 prefix on two (the two AM-broadcast recordings, at 5 s), the V1
  prefix on none. The union of V1 pilot and V2 prefix is three distinct captures of 102 (2.9%). All
  three are AM or wideband captures, not RADE passbands; whether that matters depends on how often
  an operator leaves a RADE reference on while tuning through such a signal, which these captures do
  not say.
- **The V1 prefix is a worse detector than the V1 pilot** (39 of 45 against 44; 9 s and up against
  2 s) **but has no false locks at all** in the 171 captures. The six it misses were all found by
  the pilot.
- **Misses:** the V1 pilot misses one V1 capture (`20260930-172640`, which the prefix finds at 51 s);
  the V2 prefix misses three weak V2 captures (5 October, `162647`, `162757`, `163038`, which are
  the T-009 kind: LSB under SSB QRM).
- **`20260911-230608`** counts for both waveforms in rade_c's receivers (V2 prefix locks at 42 s, the
  V1 detectors do not). It may hold two stations; I have not looked.

### Limits

- Truth is rade_c's sync at my thresholds, on the sideband the recorded passband implies. A signal
  recorded in the wrong mode is a miss in the truth too.
- Many captures are the same session. The independent stations are far fewer than 171, so these are
  counts of captures, not rates.
- No capture holds a V1 station followed by a V2 station (or the reverse). Nothing here tests
  switching, which is the part of Rule 2 that matters most.
- The V1 prefix numbers are from the Python reference, 60 s per capture; the V1 pilot's are from the
  shipping module through `replay_rade`.

## 3. What this does to the plan

- **Stage 1 looks viable as specified.** The two gates do not cross-lock, with one exception that
  the first-lock rule covers on these captures. The measured exposure on non-RADE signals is the
  union above (3 of 102), not worse than the two taken separately.
- **The weakest part is switching.** Rule 2 is untested on data. Before building the engine side,
  the test harness needs two things: both modules run together over a capture with the arbitration
  in it, and **spliced captures** (a V1 capture followed by a V2 one, and the reverse, on the same
  band and passband) so that a hand-over is exercised, including the case where the holder has faded.
  The 160 m V2 captures and the 7.177 and 5.3685 MHz V1 captures are candidates; the joins would be
  artificial, which is stated whenever a result is quoted.
- **Why V1 locks on those two V2 captures** should be understood first. If it is the V1 pilot
  correlating against V2 energy at some alignment, it says something about the pilot's gate.

## 4. Stage 3 is not supported

A prefix pre-gate for the pilot search would skip the search where the prefix says nothing is
there. The V1 prefix finds 39 of 45 and takes 9 s or more, where the pilot finds 44 and mostly takes
2 to 3.6 s. A gate that waited for the prefix would lose the six it misses, and would delay every V1
lock. It stays a corroborating voter (no false locks in 171) rather than a gate, and only if Stage 1
shows a use for one.

## 5. Next

1. Look at the two V1-on-V2 locks (why, and what the weight did).
2. A harness that runs both modules over a capture with the Stage 1 rules, and the spliced captures.
3. The on-air V2 evaluation, which comes first for V2 itself.

Reproduce: `LD_LIBRARY_PATH=third_party/rade_c-build/src python3 -I test/diversity/devtools/py/cp_matrix.py`
(about seven minutes with six jobs; results cached in `devtools/build/cp_truth/matrix.json`).

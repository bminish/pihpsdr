# Lost first tap in break-in CW (iambic keyer) - findings and fix, 2026-11-07

## Summary
* **Symptom.** A short paddle tap sometimes produces no element at all, most often the first dit or dah of a word.
* **Cause.** `keyer_thread()` (`src/iambic.c`) looks at the live paddle state, not at the paddle memory, and it cannot look
  while it waits for TX to come up (cold start) or to go down (end of the hang time). A tap that is released again in the
  meantime is found gone, and the wake-up is swallowed by `if (!kcwl && !kcwr) continue;`.
* **Measured** (22 wpm, hang 600, 40 ms taps, KiwiSDR on the RF, Iambic B, quantum 128): unchanged keyer 9 of 120 taps lost, with
  the fix 0 of 120. All losses were in two places: the cold start, and a window of about 100 ms after the hang time has expired.
* **Fix.** Branch `fix/keyer-lost-first-tap` (one commit on `master`, `iambic.c` only; also on `test/sidetone`). The paddle that
  was hit is remembered while the thread cannot look, and an element is started for it. The request to leave TX (a 50 ms timer)
  is cancelled if a tap arrives before it fired, so that case also avoids the RX/TX transition.
* **Timing of everything sent before is unchanged.** Warm taps: 7.1 ms to the sink, 22.6 ms to the RF. Cold starts: 27.8 / 43.3 ms.
  A recovered tap is a normal cold start: it starts late by the transition (mean 22 ms, 12..55 ms) and keeps its full length.
* **Not fixed here:** the hang time counts at twice the rate (see below), straight-key mode, and the cost of the cold start itself.

## Where the taps were lost (hang test)
`cwlat --test hang` sends pairs of dit taps, the second D ms after the keyer finished the first (press + dot + gap).
Hang time 600, unchanged keyer:

| D (ms) | 100..300 | 350 | 400 | 450 and later |
|---|---|---|---|---|
| second tap | warm, 7 ms | **lost (3 of 3)** | cold or lost | cold (22..35 ms) |

* The radio is warm up to D = 300..350 ms, which is **about half of the 600 ms set**. `cwvox` is decremented in both `EXITLOOP`
  (after a 1 ms wait) and `CHECK` (no wait), i.e. twice per millisecond. That is a separate bug (patch to follow, see below).
* When `cwvox` reaches 0 the thread schedules the TX drop and then waits up to 250 ms for `mox` to go away. A tap that arrives and
  is released in this window (about 100 ms: the 50 ms timer, then the transition) is lost.
* **Word spaces fall into this window.** The pause before the first element of a word is about 6 dots after the keyer's last
  element and gap, i.e. 7200 / wpm ms: 288 ms at 25 wpm, 327 ms at 22, 360 at 20, 400 at 18. With hang 600 the window is at D of about
  330..420 ms, so the first letter of a word at typical speeds is at risk.
* Cold start: the thread waits for `mox` (the transition takes 20..55 ms) and then looks at the paddle. A 40 ms tap is gone by then.
  With the fix the cold-start losses (6 of 6 in the prototype run) disappeared.

## What the fix does (`src/iambic.c`)
* `keyer_event()` sets `tap_left` / `tap_right` for a press while `tap_recording` is set (idle, cold-start wait, TX-drop wait).
* `CHECK` treats `*kdot || dot_tap` as a dot request (same for the dash); the flags are cleared there. Paddle modes only.
* The TX drop is a cancellable request: `tx_off_request()` starts the 50 ms timer, `tx_off_cancel()` cancels it if it has not fired
  (state under a mutex; a serial number keeps a late timer from acting on a newer request). The thread polls for a tap while it waits
  for `mox` to go away: tap before the timer fired -> stay in TX, back to the hang loop; tap after -> start again from RX.
* About 100 lines. Code style: `src/pihpsdr.astyle` (applied), `README_STYLE.md`, cppcheck on the file reports nothing.

## Regression tests (cwlat, Kiwi, quantum 128, hang 600, synth side tone)
| test | result with the fix |
|---|---|
| hang test, Iambic B | 0 of 120 lost (unchanged keyer: 9); D = 350 ms taps are now warm; no extra elements; warm/cold timing as before |
| Iambic B dit / dah / squeeze (same trial schedule as the unchanged-keyer run of the same day) | the 15 taps that were lost (5 dit, 10 dah) now give one element each; no other trial changed except three on the repeat saddle (below) |
| Iambic A dit / dah / squeeze | 0 lost, dit test 13 extra elements (the legitimate repeats of long holds, same count as Iambic B); **no unchanged-keyer run to compare** |
| straight key | 1 of 60 lost (a 37 ms warm tap); the unchanged keyer lost 0 of 60 (synth) and 1 of 60 (old path) in the runs of 2026-11-06; straight key is not covered by the fix |

* The repeat point (a second element when the paddle is still held after dot + gap = 109 ms) is unchanged. Holds of 110 ms and 140 ms
  gave one element in the unchanged run and two with the fix in three trials; the outcome there depends on the 0..1 ms start-up lag and
  on the length of the cold start (53 ms against 29 ms in the squeeze trial), and warm taps do not touch the new code.
* Analysis artefact: in the fix hang run 18 of 120 elements show a "rise" of 53 ms. Their carrier plateau is 0.75..0.89 of the peak
  (fading at the Kiwi); `find_segments()` takes the 90 % point against the recording's peak, so a weak element never reaches it.
  Length, onset and fall are normal.
* Not tested: real paddles, Iambic A against the unchanged keyer, PTT mode, non-break-in, other speeds.

## Options considered
A prototype with four run-time modes (`~/sdr/patches/keyer-proto/proto-modes-0-3.diff`, not for merging) was measured with the hang test:

| mode | lost taps | RF end of a cold first element (ms after the press) |
|---|---|---|
| 0 unchanged | 9 of 120 | 96.9 ± 4.5 |
| 1 remember the tap | 3 of 120 | 98.7 ± 6.8 |
| 2 + cancel the TX drop (this fix) | 0 | 98.0 ± 6.6 |
| 3 + shorten the late first element so that it ends on time | 0 | 77.0 ± 2.1 (warm: 77.1) |

Mode 3 keeps the end of the first element where the operator put it (no gap is shortened), but the first element of a cold letter is
about 22 ms shorter. Audition files of the four modes (`~/sdr/cw-mode-audition/`, "THIS IS A TEST", PARIS x 8, 22 wpm) were judged and
**mode 3 sounds terrible: ruled out.** Modes 0..2 are the same in timing (the cold letter is late by D as a whole, the gap after it is
shorter by up to 27 ms); mode 2 only stops the loss of the tap.

## Open items
1. **Hang time counts twice as fast** (`CHECK` and `EXITLOOP` both decrement `cwvox`). Separate patch: decrement once per ms. This also
   moves the loss window out to the set hang time. It changes the behaviour of every user's existing hang setting.
2. **Cold-start delay and its spread** (20..55 ms, sd 5..7 ms): not understood. Hypothesis, unmeasured: `g_idle_add(ext_radio_set_mox)`
   runs at idle priority, behind GTK redraws. First step is to time press -> `mox` set -> first RF.
3. Straight-key mode loses an occasional tap (about 1 in 60); the fix does not cover `KEYER_STRAIGHT`.
4. Constant RF latency (hold warm elements to equal the cold start) would remove the cold/warm difference; high risk, not pursued.
5. Run the regression set on the unchanged keyer for Iambic A, and with real paddles.

## Reproduce
    tools/cwlat/build.sh
    ./capture_run.sh --kiwi kiwi.minish.org:8074 --freq 28187 --dir results/<group> --name synth_iambicB_breakin_Q128_hang \
        --label "<text>" -- --confirm-safe --sink <sink node> --dest 32:0 --wpm 22 --test hang --trials 120 --label "<text>"
    ./run_matrix.sh --sink <node> --paths synth --keys iambicB --modes breakin --quanta 128 --kiwi kiwi.minish.org:8074 --kfreq 28187
    python3 -I analysis/cwlat_analyze.py run results/<group>/<name>
Setup: Iambic B/A or straight key, break-in on, hang 600, 22 wpm, 1 % drive into a dummy load on 28187 kHz, GNSS-stamped KiwiSDR.
cwlat uses a fixed random seed, so runs of the same test have the same trial schedule and can be compared trial by trial.
Data: `tools/cwlat/results/` (not in git): `regress-fix-*`, `regress-fix-hang`, `proto-keyer*`, `idlefix-*`.

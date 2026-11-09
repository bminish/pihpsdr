# LC-065: a short paddle tap is no longer lost while the keyer waits for TX

Part of the local-change register: [changes.md](changes.md). Found while measuring [LC-064](cw_sidetone.md), independent of it: the fix is in `src/iambic.c` only and applies to `upstream/TEST` alone. Tool that found it: `tools/cwlat` (LT-038), `--test hang`.

**Status: local, not yet offered to dl1ycf.** Measured on one machine at the shipped quantum of 256 against the unchanged TEST build (see "The A/B run of 2026-11-07" below). The first version of the fix was measured on a build that also had LC-064; this version differs only in the straight key / bug mode handling (the dash paddle is LC-066). Open items at the end.

## Summary

- **Symptom.** A short paddle tap sometimes produces no element at all, most often the first dit or dah of a word.
- **Cause.** `keyer_thread()` looks at the live paddle state, not at the paddle memory, and it cannot look while it waits for TX to come up (cold start, 20 to 55 ms) or to go down (end of the hang time). A tap released in the meantime is found gone, and the wake-up is swallowed by `if (!kcwl && !kcwr) continue;`.
- **Measured** (22 wpm, hang 600, 40 ms taps, KiwiSDR on the RF, Iambic B): unchanged keyer **9 of 120 taps lost**, with the fix **0 of 120**. All losses were in two places: the cold start, and a window of about 100 ms after the hang time has expired.
- **Timing of everything sent before is unchanged.** Warm taps 7.1 ms to the sink and 22.6 ms to the RF; cold starts 27.8 and 43.3 ms. A recovered tap starts late: by the RX to TX transition (mean 22 ms, 12 to 55 ms) when it arrives while the keyer waits for TX to come up, and by more (about 65 to 70 ms to the sink at D = 400 ms in the A/B run, against about 30 to 40 ms for an ordinary cold start) when it arrives while TX is already going down: the keyer then waits for TX to finish dropping and starts again from RX. It keeps its full length in both cases.

## The A/B run of 2026-11-07: unchanged TEST against this branch, quantum 256
Kiwi on 28187 kHz, dummy load, 22 wpm, Iambic B, break-in, hang 600, the same trial schedule on each build; base is `origin/TEST` (`10f40865`), the branch builds had LC-064 ticked or unticked (the keyer losses are the same).

| test | base | this branch, box unticked | box ticked |
|---|---|---|---|
| dit, 60 taps | **5 lost** (holds 20 to 50 ms) | 0 | 0 |
| dah, 60 taps | **10 lost** (holds 20 to 80 ms) | 0 | 0 |
| squeeze, 36 | 0 (not a real squeeze, see below) | 0 | 0 |
| hang test, 120 | **7 lost**, 3 of 3 at D = 350 ms | 0 | 0 |

- Every lost tap is missing at both RF and sink: a keyer loss, not a side tone dropout. A repeat of the base dit run 25 minutes later lost 7 (the same 5 plus 2): the loss is tied to particular taps (the ones released in the RX to TX switch), not random.
- The 5 dit and 10 dah losses are the 15 taps of the earlier run on the same schedule; D = 350 ms was the window it named.
- **The recovered tap is late, and later in the TX-drop window.** At D = 400 ms, where the keyer is waiting for TX to drop, the two second taps came 71 ms (box unticked) and 68 ms (ticked) after the press at the sink, against 36 to 42 ms (box unticked) and 27 to 36 ms (box ticked) for an ordinary cold start at larger D: the keyer waits for TX to finish dropping and then starts again from RX. They keep full length. (n = 2 per D; one build each.)
- Timing of everything that was sent before is unchanged: warm RF 22.5 to 22.7 ms in all three builds (two post builds against base 22.57 to 22.71), edges 5.0 ms, dit and dah length errors -0.16 to -0.18 ms.
- Extra elements (dit run): 11 on the base, 13 on the branch builds; these are the legitimate repeats of long holds at the repeat point and the counts are close; not examined tap by tap.

## Where taps were lost (hang test, earlier prototype run)

`cwlat --test hang` sends pairs of dit taps, the second D ms after the keyer finished the first (press, dot, gap). Hang time 600, unchanged keyer:

| D (ms) | 100 to 300 | 350 | 400 | 450 and later |
|---|---|---|---|---|
| second tap | warm, 7 ms | **lost (3 of 3)** | cold or lost | cold (22 to 35 ms) |

- When `cwvox` reaches 0 the thread schedules the TX drop and waits up to 250 ms for `mox` to go away. A tap that arrives and is released in this window (about 100 ms: the 50 ms timer, then the transition) is lost.
- **Word spaces fall into this window.** The pause before the first element of a word is about 6 dots after the keyer's last element and gap, 7200 / wpm ms: 288 ms at 25 wpm, 327 ms at 22, 360 at 20, 400 at 18. With hang 600 the window is at D of about 330 to 420 ms, so at typical speeds the first letter of a word is at risk. (The window sits at about half the set hang time because of the double decrement, open item 1.)
- Cold start: the thread waits for `mox`, then looks at the paddle. A 40 ms tap is gone by then. With the fix the cold-start losses (6 of 6 in the prototype run) disappeared.

## What the fix does

- `keyer_event()` sets `tap_left` or `tap_right` for a press while `tap_recording` is set (idle, cold-start wait, TX-drop wait).
- `CHECK` treats `*kdot || dot_tap` as a dot request, and the same for the dash; the flags are cleared there. In straight key mode the dot paddle (bug mode: automatic dots) is recorded like a paddle; the dash paddle is the straight key itself and is not recorded here, see [LC-066](cw_keyer_straight_key.md). (The first version of this fix recorded in every mode and used the flags in the paddle branch only; the version measured on 2026-11-07 has this restriction.)
- The TX drop is a cancellable request: `tx_off_request()` starts the 50 ms timer, `tx_off_cancel()` cancels it if it has not fired. The state is under a mutex because the timer runs in the GTK thread, and a serial number keeps a late timer from acting on a newer request. While waiting for `mox` to go away the thread polls for a tap: tap before the timer fired, stay in TX and go back to the hang loop; tap after, start again from RX.
- `radio_set_mox(0)` is called directly from the timer, where upstream called `ext_radio_set_mox` through `g_timeout_add`; `ext_radio_set_mox` is only that call plus `G_SOURCE_REMOVE`.
- About 100 lines in `src/iambic.c`. `astyle` with `src/pihpsdr.astyle` leaves it unchanged, cppcheck reports nothing on the file.

The new flags are plain `volatile int` written from two threads (the key source sets, the keyer thread clears), the same style as `kcwl` and `kcwr`. A press that lands between the keyer thread's last look and its clearing of the flags is not recorded, but the paddle is then still down for the thread's next look in all but a sub-millisecond case.

## Regression tests (cwlat, KiwiSDR, hang 600)

| test | result with the fix |
|---|---|
| hang test, Iambic B | 0 of 120 lost (unchanged keyer: 9); D = 350 ms taps are now warm; no extra elements; warm and cold timing as before |
| Iambic B dit / dah / squeeze (same trial schedule as the unchanged run of the same day) | the 15 taps that were lost (5 dit, 10 dah) now give one element each; no other trial changed except three on the repeat point (below) |
| Iambic A dit / dah / squeeze | dit and dah: 0 lost, dit test 13 extra elements (the legitimate repeats of long holds, as Iambic B); squeeze: see below (the first squeeze run was not a squeeze); **no unchanged-keyer run to compare** |
| straight key | 1 of 60 lost (a 37 ms warm tap); the unchanged keyer lost 0 of 60 (synth side tone) and 1 of 60 (old path) on 2026-11-06; straight key is covered by LC-066, not by this fix |

- **Squeeze results before 2026-11-09 are not squeezes.** cwlat's virtual MIDI port dropped the second of two note-ons in a row (running status, which piHPSDR's parser does not support), so the dash paddle was never pressed: the squeeze rows above (0 lost) are single-paddle results. Repeated with the corrected sender, Iambic B, break-in, hang 600, 22 wpm, quantum 128, 36 trials, LC-065 and LC-066 in the build: holds of 40 to 100 ms give dit, dah (2 elements at the RF in 23 of 24 trials; the exception is trial 35, the last of the run, with one element of 40.9 ms instead of 54.5 and no dah, which may be an artefact of the run ending: not examined); holds of 140 and 200 ms give dit, dah, dit (3 elements, 12 of 12); no tap lost. **Iambic A**, same conditions (36 squeeze trials): holds of 40 to 100 ms give 1 element (the dit: both paddles are released before the dit ends, and mode A then stops; the dash was already down when the dit started, so it is not a dash hit during the dit), holds of 140 and 200 ms give 2 (dit, dah): in every case exactly one element fewer than Iambic B, the extra element mode B adds after release. All 36 seen at the RF. Iambic A dit and dah: RF missing 0 of 120.
- The repeat point (a second element when the paddle is still held after dot and gap, 109 ms) is unchanged. Holds of 110 and 140 ms gave one element in the unchanged run and two with the fix in three trials; the outcome there depends on the 0 to 1 ms start-up lag and on the length of the cold start (53 ms against 29 ms in the squeeze trial), and warm taps do not touch the new code.
- Analysis artefact: in the hang run with the fix, 18 of 120 elements show a "rise" of 53 ms; their carrier plateau is 0.75 to 0.89 of the peak (fading at the Kiwi), and `find_segments()` takes the 90 % point against the recording's peak. Length, onset and fall are normal.
- These runs were at PipeWire quantum 128 with the synth side tone. The fix does not touch audio; the quantum is upstream's decision (256) and is not part of this change.

## Options considered

A prototype with four run-time modes (not for merging) was measured with the hang test:

| mode | lost taps | RF end of a cold first element (ms after the press) |
|---|---|---|
| 0 unchanged | 9 of 120 | 96.9 ± 4.5 |
| 1 remember the tap | 3 of 120 | 98.7 ± 6.8 |
| 2 + cancel the TX drop (this fix) | 0 | 98.0 ± 6.6 |
| 3 + shorten the late first element so that it ends on time | 0 | 77.0 ± 2.1 (warm: 77.1) |

Mode 3 keeps the end of the first element where the operator put it, but the first element of a cold letter is about 22 ms shorter. Listening files of the four modes (22 wpm, "THIS IS A TEST" and PARIS x 8) were judged by the owner: **mode 3 sounds terrible and is ruled out.** Modes 0 to 2 are the same in timing (the cold letter is late by D as a whole, the gap after it shorter by up to 27 ms); mode 2 only stops the loss of the tap.

## Not measured, open items
The list for the whole CW work is in the register ([changes.md](changes.md#outstanding-work)). Specific to this change:
1. **Hang time counts twice as fast** (not part of LC-065): `CHECK` and `EXITLOOP` both decrement `cwvox`, so the radio drops to RX after about 0.56 of the set hang time (warm up to D = 380 ms with 600 set; the expiry at about D = 335). A separate patch (one decrement per millisecond) would also move the loss window out to the set hang time, and would change the behaviour of every user's existing hang setting.
2. **The cold start** (20 to 55 ms, sd 5 to 7 ms) was timed on 2026-11-09 with a temporary probe (not part of this branch): `rxtx()` 18.6 +/- 3.0 ms, steady, plus the wait of the request in GTK's idle queue (mean 5.4 ms, 0 to 44), which is what makes it vary.
3. Constant RF latency (holding warm elements to equal the cold start) would remove the cold and warm difference; high risk, not pursued.
4. Not tested: real paddles, Iambic A against the unchanged keyer, non-break-in, other speeds. (PTT mode: see [findings](cw_sidetone-findings.md#ptt-mode-2026-11-09), no tap lost, no cold-start penalty.) The TEST build of this branch has not been through the hang test.

## Reproduce

    tools/cwlat/build.sh
    tools/cwlat/capture_run.sh --kiwi <kiwi:port> --freq 28187 --dir results/<group> --name <name> \
        --label "<text>" -- --confirm-safe --sink <sink node> --dest <midi port> --wpm 22 --test hang --trials 120 --label "<text>"
    python3 -I tools/cwlat/analysis/cwlat_analyze.py run results/<group>/<name>

Setup: Iambic B or A, or straight key, break-in on, hang 600, 22 wpm, 1 % drive into a dummy load on 28187 kHz, GNSS-stamped KiwiSDR. `cwlat` uses a fixed random seed, so runs of the same test have the same trial schedule and can be compared trial by trial.

# Break-in timer: plan for a refactor (draft)

Part of the CW write-ups: [cw_timings.md](cw_timings.md) (what the code says and where it is measured), [cw_keyer_first_tap.md](cw_keyer_first_tap.md) (LC-065), [cw_keyer_straight_key.md](cw_keyer_straight_key.md) (LC-066). Written 2026-11-08. A plan, not a change: nothing here is implemented. Legend as in `cw_timings.md`: **M** measured, **C** code only, **?** neither.

## 1. What "the break-in timer" is

All of it is in `keyer_thread()` (`src/iambic.c`), with `radio_set_mox()` / `rxtx()` (`src/radio.c`) on the other side:

| Part | Where | What it does |
|---|---|---|
| cold start | `keyer_thread`, after the wake-up | `g_idle_add(ext_radio_set_mox, 1)`, then polls `mox` every 1 ms for up to 200 ms |
| hang counter | `cwvox`, `EXITLOOP` and `CHECK` | set to `cw_keyer_hang_time`, decremented once per visit of `CHECK` and of `EXITLOOP` |
| TX off | `tx_off_request()` / `tx_off_timer()` | a 50 ms GTK timer calls `radio_set_mox(0)`; the thread polls `mox` for up to 250 ms |
| PTT latch | `moxbefore = mox` | if TX was already up at the first key, no TX off afterwards |
| blind-time recovery | `tap_left/right`, `tap_dash_*` (LC-065, LC-066) | remember what happened while the thread was blocked in the waits above |

## 2. What is wrong

1. **The hang counts twice as fast** (**M**). `CHECK` has no sleep, so a pass of `CHECK` + `EXITLOOP` takes 1 ms and two counts: hang 600 is warm up to about D = 300 to 350 ms. The radio's own keyer gets the same number (`cw_keyer_hang_time` goes into the P1/P2 buffers, which carry milliseconds), so one setting probably has two meanings, depending on `cw_keyer_internal` (**C**: the radio's counting was not measured).
2. **The thread is blind while it waits.** The cold-start wait (up to 200 ms) and the TX-off wait (up to 250 ms) are nested `usleep` loops. A paddle event in them is seen only by what `keyer_event()` happens to record. LC-065 and LC-066 are workarounds for this, not a cure: two sets of flags and timestamps, each tied to one blind period.
3. **A stale TX-off timer can drop TX in the middle of keying** (**C**, `cw_timings.md` 3.2). The wait gives up after 250 ms and nothing cancels a request that is still pending; the serial number only protects against a newer request.
4. **`moxbefore` can latch on** (**C**, 3.3): a slow TX-to-RX transition makes the restart see `mox == 1` and treat it as PTT mode.
5. **Hang 0 keeps TX up** (**C**, 3.4): the loop is left without a TX-off request, like PTT mode. The menu allows 0.
6. **The 50 ms is not explained** in the code or by upstream (**C**). A reason that fits the numbers: the last key-up still has to reach the RF (22.6 ms warm, **M**) and its ramp (9 ms) before TX may drop; 50 ms covers that, but it is a guess, and the delay from the last key-up to TX gone has not been measured (**?**).
7. **Every wait for `mox` is a wait for `rxtx()`** (**C**, new in this plan). `radio_set_mox()` sets `mox` only after `rxtx(state)` has returned, and `rxtx()` runs in the GTK main thread: it slews the receivers down (WDSP, "about 30 +/- 10 ms" by its own comment), takes the panadapter widgets out of the container and puts the transmitter's in. So the cold start of 20 to 55 ms (**M**) is probably mostly `rxtx()` itself, and the earlier hypothesis (idle priority behind GTK redraws) is at best a part of it. **?** until it is timed.

The iambic algorithm itself (`PREDOT` to `LETTERSPACE`), the ring in `transmitter.c`, and `sidetone.c` are not the problem and are not touched.

## 3. Principles

* Small patches, each separately measurable, in the order that gives the most for the least risk. `iambic.c` is dl1ycf's file: keep every patch reviewable and offerable on its own.
* No change of behaviour except the intended one, stated in the patch. Whatever a patch changes is measured with `cwlat` before and after (hang test, dit / dah / squeeze, straight, `short`); the trial schedule is fixed, so runs compare trial by trial.
* Pure logic out of the thread: a state machine with an injected clock can be tested on the host (as `test/sidetone/` does for the renderer) for what the radio cannot provoke: expiry to the millisecond, a timer that fires late, a held GTK thread.
* The radio side stays as it is: `radio_set_mox()` and `rxtx()` are shared with PTT, VOX, CAT and the GUI.

## 4. Target design

* **Deadline, not a counter.** `hang_deadline` (CLOCK_MONOTONIC) is set when the thread reaches the point where the counter starts today (after the element and its gap; after the letter space when `cw_keyer_spacing` is on; at the release for the straight key). The wait is until the earlier of the next element time and the deadline, so the hang is exact, independent of how many states the loop passes through.
* **One loop, no nested waits.** The thread waits on the semaphore with a timeout (`sem_timedwait`) instead of `usleep` polling; the transitions are states (`IDLE`, `GOING_TX`, `ACTIVE`, `GOING_RX`) of the same loop, left on an event (`mox` changed, deadline, press). The thread is never blind, so paddle edges can be handled where they happen. LC-065's and LC-066's flags are replaced by one timestamped edge record per paddle that the loop consumes, with the same behaviour (checked against the regression set).
* **A session, an owner, a token.** A session starts at the first key and ends when TX is down or the user took over. `owner` says who asserted TX: the keyer drops TX only if it raised it (replaces `moxbefore`; a MOX or PTT from the user during the session makes the user the owner). The TX-off request carries the session token; a new session, or the thread giving up, invalidates it, so a late timer does nothing.
* **Hang 0** gets a stated meaning (TX off right after the last element and `tx_off_delay`, not PTT mode), and `tx_off_delay` becomes a named constant with its reason (RF latency of the last key-up plus its ramp) once measured.
* **`rxtx()`** is not touched by this refactor; see phase 4.

## 5. Phases

Each is its own patch, measured before the next. Estimates are lines of `iambic.c`.

| Phase | What | Fixes (section 2) | Size | Risk | Measured with |
|---|---|---|---|---|---|
| 0 | Scaffolding, no behaviour change: a host test of the break-in logic with an injected clock (hang expiry, tap before and after the timer, late timer, owner, hang 0); finer D grid (10 ms) in `cwlat --test hang` around the expiry; a timing log of press, `rxtx()` start, `rxtx()` end, `mox` set | measurement | test code + about 20 | none | the test itself, and a baseline hang run **Done 2026-11-08 except the on-air baseline (section 8).** |
| 1 | **Hang in real milliseconds**: deadline instead of the counter, same start point as today | 1 | about 30 | low in code; **changes every user's effective hang** (a stored 600 means 600 ms instead of about 300), so it is its own announced patch | hang test: warm up to D = hang, loss window moves out; unit test of the arithmetic |
| 2 | **Session token and owner**: `tx_off_cancel()` when the thread gives up or a session starts; `owner` replaces `moxbefore`; hang 0 defined | 3, 4, 5 | about 60 | low to medium (PTT, VOX and CAT interplay) | unit tests; a provoked late timer (hold the GTK thread in a debug build); PTT held across keying (needs the cwlat PTT sender fixed first, see its README) |
| 3 | **One loop** with `sem_timedwait` and the edge record, LC-065 and LC-066 reimplemented on it with identical behaviour | 2 | about 250 (rewrite of the loop around the unchanged element states) | **high**: it is the loop every key press goes through | the whole regression set, Iambic A and B, straight, `short`, hang; real paddles before it goes anywhere |
| 4 | **Cold start**: time `rxtx()` first; then, only if it is the cost, look at what can overlap or be shortened (WDSP slew-down for CW, widget juggling) | 7 | unknown | `rxtx()` is shared with all TX: hot-switching and T/R relay timing belong to the owner | cold first element to RF (now 43.3 ms, sd 7) |
| 5 | CAT `SD` break-in logic inverted (`rigctl.c:3440`) | review finding 5 | 1 | none | a CAT test |

Order: 0, 1, 2 (then 5), 4 as a separate track, 3 last and only if phases 1 and 2 leave the code too hard to maintain or to offer upstream. After 1 and 2 the keyer is correct; 3 makes it clean.

## 6. Decisions for the owner

1. Phase 1 changes the meaning of every stored hang time. Accept it as a plain fix, or keep the old meaning behind a one-time migration (halve nothing, but write a note in the log and the CW menu label)?
2. What should hang 0 mean?
3. Keep the 50 ms TX-off delay, or derive it from the RF latency once the time from the last key-up to TX gone is measured?
4. Is phase 3 wanted at all, given that 1 and 2 remove the known faults?
5. Order of offering to dl1ycf: the hang fix (phase 1) is a bug in upstream's own counter and the natural first patch; LC-065 and LC-066 follow.

## 7. Dependencies and risks

* The cwlat **PTT sender fault** blocks tests of the owner logic in phase 2 until it is fixed (`tools/cwlat/README.md`).
* The tests so far are one machine, quantum 128 or 256, 22 wpm, Kiwi at 28187 kHz, no real paddle, no PTT mode: a refactor of the loop must be tested with a real paddle and at other speeds before it is trusted.
* The radio's own keyer (`cw_keyer_internal = 0`) is not involved; phase 1 makes the shared setting mean the same thing in both.

## 8. Phase 0: what exists now (2026-11-08)

**Host test of the real keyer, `test/breakin/run.sh`** (about 12 s, no radio). It compiles a copy of the real `src/iambic.c` against stub headers (`test/breakin/shim/`) and drives it with a simulated GTK main loop: one thread runs idle and timer callbacks in order, `radio_set_mox()` sets `mox` only after a simulated `rxtx()` of 30 ms (the same order as `radio.c`), and a timer can be delayed. Real time, 40 wpm, hang 300 ms. Eight scenarios, one process each, **characterisation tests**: they state what the code does today, bugs included, and are changed on purpose by the later phases (`HANG_RATIO` for phase 1; the lines marked BUG for phase 2). They are repeatable (three runs, same result). Results of the code as it is:

| Scenario | Result |
|---|---|
| S1 cold start, tap of 15 ms (shorter than the 30 ms transition) | element sent (LC-065), key-down 31 ms after the press |
| S2 hang expiry | TX off requested after 160 ms of 300 set: **ratio 0.53** (the 1 ms sleeps take about 1.07 ms; the counter counts twice per pass) |
| S3 tap 20 ms after the expiry, before the 50 ms timer | key-down 0.3 ms after the press, no transition, TX off once, after the second element |
| S4 tap 60 ms after the expiry, drop in progress | key-down 52 ms after the press (drop, then RX to TX), TX goes down, up, down |
| S5 BUG: timer delayed by 350 ms (busy main loop) | the stale timer drops TX 400 ms after the expiry, in the middle of a later session; a key-down is queued with TX off |
| S6 BUG: TX to RX transition of 400 ms, tap during it | TX is still on 2.5 s after the last element, one TX-off request over two sessions (the latch) |
| S7 BUG: hang 0 | TX stays up, no TX-off request |
| S8 cold start, tap of 100 ms | key-down 31 ms after the press |

Two things the test showed that the notes did not say: the 250 polls of the TX-off wait take about 267 ms, not 250 (a tap at 260 ms after the expiry is still inside the wait and cancels the timer, which is the intended behaviour); and S5 and S6 need the tap after the wait has given up, so both need a main loop held for more than a quarter of a second or a TX to RX transition of that order. Neither was provoked on the radio.

**Finer grid in `cwlat --test hang`**: `--hang-from MS --hang-to MS --hang-step MS` (default 100 1400 50, at most 64 values). For the expiry at hang 600, `--hang-from 250 --hang-to 500 --hang-step 10` with `--trials 104` gives two second taps for each of the 26 values in about seven minutes.

**Timing probe, `make CWTIME=1`** (`src/cwtime.[ch]`, marks in `iambic.c` and `radio.c`; `touch src/cwtime.c src/iambic.c src/radio.c` when switching). Without it `CWT()` is empty and the program is unchanged; with it the first occurrence of each event of a break-in session is stored (no print, no lock in the timing path) and one line is printed after the TX-off wait, when TX is down:

    cwtime: idle-queue 0.4, rxtx 30.8, rxtx-to-mox 0.1, mox-to-keyer 0.9, press-to-keydown 33.2 | expiry-to-timer 50.1, timer-to-rxtx 0.6, rxtx 31.0, rxtx-to-mox 0.1, mox-to-keyer 1.0 (ms, -1: not seen)

(An illustration of the format, not a measurement.) "idle-queue" is the time from the keyer's request to the start of `rxtx()`: the hypothesis of an idle callback waiting behind GTK redraws is true if this is large and varies; "rxtx" is the cost of the RX/TX transition itself. A normal session prints one line per first cold start with a TX-off after it.

**Still to do for phase 0**: the on-air baseline at hang 600: the fine-grid hang test (above) and a `CWTIME=1` run of the same, to see where the 20 to 55 ms of the cold start go and where the loss window starts and ends.

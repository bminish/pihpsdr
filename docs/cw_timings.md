# CW timings: what the code says, and where it has been measured

Index and open items: [cw.md](cw.md). Written 2026-11-08 from a read of `src/iambic.c`, `src/transmitter.c`, `src/sidetone.c`, `src/pipewire.c`, `src/radio.c` (`rxtx`, `radio_set_mox`) and `src/rigctl.c` on `feature/cw-synth-sidetone` (1e387c0a); the cold start and the TX drop were measured with the timing probe on 2026-11-09. "Measured" means a number from the write-ups listed at the end: one machine, Kiwi at 28187 kHz, 22 wpm, Iambic B, hang 600, break-in on, quantum 256 (2026-11-07) or 128 (2026-11-09).

Legend: **M** measured, **C** code only (not measured), **?** neither.

## 1. Element timing (keyer to RF)

| Quantity | What the code says | Measured |
|---|---|---|
| Dot | `dot_samples = 57600 / wpm` samples of 1/48000 s (`iambic.c`, `keyer_update`); 2618 at 22 wpm (54.54 ms, truncated by 0.18 sample) | **M** length error against nominal −0.16 to −0.18 ms (the ramp symmetry puts the 50 % points on the key events) |
| Dash | `dash_samples = 3456 * weight / wpm`; weight 50 gives exactly 3 dots. Weight changes the dash only, never the dot or the gap | **M** same error as the dot, at weight 50 only |
| Gap after dot or dash | one `dot_samples` (the second queued key-up carries the wait) | **M** implied by the lengths above; not measured separately |
| Where the length is counted | the **ring**, in samples of the TX engine's mic clock, from the previous event actually executed (`transmitter.c` consumer). The keyer thread's own timer (`loop_delay`, 20833 ns per sample, truncated from 20833.33) only paces when the *next* event is queued | **M** element lengths do not move with load or with the audio path; the ring can run ahead of the thread by ppm only, far below its 1024 events |
| RF ramp | `48 * ratio * cw_ramp_width` samples (default 9 ms), `init_dl1ycf_ramp`; the pointer moves one step per sample while key is down, back while up | **M** 10 to 90 % edges 5.0 ms (4.94 to 5.0) |
| Side tone ramp | 240 samples (5 ms) raised cosine, in the TX engine and in `sidetone.c` alike | **?** edge time of the audio not measured separately; the synth and old tone are the same ramp by construction |
| Key to RF, warm (TX already up) | not stated in code: queue, wait for the next mic sample, the TX engine, the protocol, the radio | **M** 22.6 ms (22.4 to 22.7, three builds) |
| Key to RF, cold (RX to TX first) | the keyer asks for MOX with `g_idle_add`, polls `mox` every 1 ms for up to 200 ms, then queues. `rxtx()` comment: "about 30 ± 10 ms", dominated by WDSP's receiver slew-down | **M** cold first element 43.3 ms to RF, 27.8 ms to the sink; press to key-down 24.5 ms = `rxtx()` 18.6 ± 3.0 ms plus the request waiting in GTK's idle queue 5.4 ms (0 to 44; this is the spread, correlation 0.94); `mox` seen by the keyer 0.5 ms after it is set ([cw_breakin_plan.md](cw_breakin_plan.md) section 9) |
| Key to side tone, old path | the TX engine writes the sample into a FIFO padded to 384 samples (8 ms) | **M** 19.0 ms at the sink (quantum 256), 3.6 ms after the RF |
| Key to side tone, synth path | rendered in `pw_out_cb` from a copy of the event: 0 to one callback | **M** 11.1 ms at 256, 7.0 at 128, 5.0 at 64; 11.5 ms before the RF |
| Straight key element | the key-up is queued with a wait of the hold, but never less than `(cw_ramp_width + 1) * 48` samples | **M** floor 9.9 to 10 ms at the default ramp; edges 4.94 / 4.97 ms |
| Key held more than 20 s | key-up in the TX engine (`cw_delay_time > 960000`) and in the renderer | **?** not exercised |

The events of the first keying after RX carry `wait = 0` or the gap to the previous event; the TX engine's counter `cw_delay_time` runs also while receiving (saturating at 999999), so the first event is executed at once. The renderer is not run in RX, so it is put in the same "long pause" state explicitly (`sidetone_idle`). **M** indirectly: the cold first elements of the A/B runs have normal lengths on both paths.

## 2. The break-in timer, step by step

All times are on the keyer thread's clock. For the RF add the key-event latency of section 1; for the MOX transition the path to the antenna has not been measured.

1. **Press.** `keyer_event` records the tap (`tap_left`/`tap_right`, or the straight key's `tap_dash_t0/t1`) while `tap_recording` is 1 (idle, waiting for TX, waiting for TX to drop) and posts the semaphore.
2. **Cold start.** If break-in is on and the TX mode is CW, `g_idle_add(ext_radio_set_mox, 1)`, then wait for `mox`, at most 200 ms (out of band: it never comes; the keyer continues after 200 ms). `cwvox = cw_keyer_hang_time`. `cw_keyer_ptt_delay` is not used here; it is only sent to the radio for its own keyer.
3. **Counting.** `cwvox` is set to the full hang time in every state except `CHECK` and `EXITLOOP`. In those two it is decremented once per visit, and the `CHECK` visit has **no sleep** when no key is down (`key_state` is set to `EXITLOOP` as default, the 1 ms wait is only added if it is still `CHECK`). So one pass of the pair takes 1 ms and takes **two** counts.
   - Effect: the hang runs out after about **hang / 2** ms (the 1 ms sleeps take about 1.07 ms, so 0.53 to 0.56). **M**: with hang 600 the expiry is at D of about 335 ms (D counted from the end of the element and its gap) and a second tap is warm up to D = 380 ms, which includes the 50 ms timer.
   - It starts when the thread reaches `EXITLOOP` after the element **and its gap** (paddle: one dot after the key-up), or after the letter space when `cw_keyer_spacing` is on (2 dots + 1 ms more, `kdelay > 2 * dot_length`, integer ms). For the straight key it starts at the release. Whether the A/B runs had `cw_keyer_spacing` on is not recorded here: **?**
4. **Expiry.** `cwvox` reaches 0 in `EXITLOOP`. If TX was not already up before the first key (`moxbefore`), `tx_off_request()` starts a 50 ms `g_timeout_add` on the GTK main loop. The purpose of the 50 ms is not stated in the code or in upstream.
5. **Drop.** The timer calls `radio_set_mox(0)` directly: `rxtx(0)` (WDSP `tx_off` slew-down, "about 30 ± 10 ms" per its comment, in the GTK thread) and only then `mox = 0`. The keyer thread meanwhile polls `mox` every 1 ms for up to 250 ms.
6. **A tap during this** (LC-065): before the timer fires, `tx_off_cancel()` wins and the thread returns to the hang loop with the tap recorded (no RX/TX transition); after it fired, the thread waits for `mox == 0` and restarts from RX with the tap in hand.

Predicted timeline from the key-up of the last element, 22 wpm, hang 600: gap 54.5 ms; `cwvox` 0 at about +355 ms; timer fires at about +405 ms; `mox` clears at about +435 ms (using the code comment's 30 ms for `rxtx`, **C**). **M** (probe and 10 ms D grid, 2026-11-09): `cwvox` 0 at about +390 ms (D 335 plus the gap), the timer 50.1 ms later (54 of 71 sessions; 8 more within 62 ms), `rxtx(0)` 21.5 ± 3.8 ms; a tap at D = 390 to 410 is recovered but late.

| Prediction | Measured |
|---|---|
| A tap is at risk from the expiry (D about 300 ms after the end of the gap) until `mox` clears (D about 380 to 400 ms): a window of about 100 ms | **M** unchanged TEST lost 3 of 3 at D = 350, "cold or lost" at 400, cold at 450 and later, warm up to 300. The D grid has 50 ms steps, so the start and end edges are known to one step only |
| With LC-065 no tap is lost in this window | **M** 0 of 120 (unchanged 9 of 120; in the same schedule, dit 0 of 60 against 5 and dah 0 of 60 against 10) |
| A tap before the timer (D about 300 to 350) stays warm; after it, it restarts from RX and is late by the transition | **M** recovered tap at D = 400 ms came 68 to 71 ms after the press at the sink, against 27 to 42 ms for an ordinary cold start (n = 2 per D, one build each) |
| A tap on an expired window sends a full-length element | **M** all recovered taps full length |
| TX drops at about 435 ms after the last key-up (hang 600) | **M** about 460 ms (expiry +390, timer 50, `rxtx(0)` 21; probe). The time from the last key-up to the RF carrier disappearing was not measured as such |

## 3. Review findings

Found in the review of 2026-11-08; no code was changed for them then. [cw_breakin_plan.md](cw_breakin_plan.md) says what a fix of items 1 to 5 would be, in phases (decisions pending). Items 2, 3 and 4 are reproduced on the host by the characterisation tests in `test/breakin/` (section 8 of the plan); none has been provoked on the radio.

1. **Hang time counts twice as fast** (known; open item 1 of [cw_keyer_first_tap.md](cw_keyer_first_tap.md)). Every user's hang setting is about half of what the menu says, so the radio drops sooner than set, and the loss window sits at about half the setting. Fixing it changes the behaviour of every existing setting, so it is a separate, announced patch. **C and M** (section 2, step 3).
2. **A stale TX-off timer can drop TX during keying.** `tx_off_request()` arms the timer and the wait loop gives up after 250 ms, but nothing cancels a request that is still pending when the thread gives up. If the GTK main loop is held up for more than 250 ms (a heavy redraw, a modal dialog), the thread goes back to idle with the request pending; the timer then fires later and calls `radio_set_mox(0)` in the middle of the next keying. The serial number protects against a stale timer *after a newer request*, not against a new keying session. Upstream has the same exposure (it armed `g_timeout_add(50, ext_radio_set_mox)` and never cancelled). Fix: `tx_off_cancel()` when the thread gives up, or at the start of every session. **C**, not provoked.
3. **`moxbefore` can latch on.** If the 250 ms wait ends with `mox` still 1 and a tap is pending, the thread restarts, reads `mox == 1` and treats it as PTT mode: no TX-off after this keying. Needs a TX-to-RX transition slower than 250 ms. **C**, not provoked.
4. **Hang 0 with break-in ticked keeps TX up.** `cwvox` stays 0, the loop is left without a TX-off request, and the keyer behaves as in PTT mode. The menu allows 0 (spin range 0 to 1000). **C**.
5. **CAT `SD` has its break-in logic inverted.** The comment says "x = 0 disables break-in", the code sets `cw_breakin = (b == 0)` (`rigctl.c:3440`): `SD0000` switches break-in on, `SD0300` switches it off. Upstream, since the CAT documentation commit; unrelated to this branch. **C**, not run.
6. **Two clocks, no feedback.** The keyer thread paces on `CLOCK_MONOTONIC`, the ring counts in the radio's mic samples. A difference of a few ppm only matters if the ring fills (1024 events), which a keyer thread pacing itself on wall time does not approach. Not a fault; noted so nobody adds feedback.
7. **No problems found** in: the retrigger of `cwvox` in the element states, the straight key floor (it uses `cw_ramp_width + 1` ms and the RF ramp is rebuilt from the same variable), the lock-free ring order (`MEMORY_BARRIER` before the index in both rings; one source at a time, as documented), the 20 s key-down protection in both consumers, or the `wait` arithmetic in the elements (the second key-up of each element exists to carry the gap).

## 4. Not measured

A real paddle or key; other speeds, bands, hang times and weights; Iambic A against the unchanged keyer; the time from the last key-up to the carrier going away (the TX-off timer and `rxtx(0)` were timed by the probe: 50.1 ms and 21.5 ± 3.8 ms); the PipeWire-less backends; the radio's own keyer (`cw_keyer_internal = 0`: hang time and `cw_keyer_ptt_delay` are then the radio's, sent in the protocol buffers, never read back).

Squeeze results and PTT-mode runs made before 2026-11-09 are invalid (a fault of the test sender: running status, `tools/cwlat/README.md`); the corrected runs are in [cw_keyer_first_tap.md](cw_keyer_first_tap.md) and [cw_sidetone-findings.md](cw_sidetone-findings.md).

## Sources of the measured numbers

[cw_sidetone.md](cw_sidetone.md) (side tone paths, A/B 2026-11-07), [cw_keyer_first_tap.md](cw_keyer_first_tap.md) (hang test, lost taps, recovered taps, squeeze), [cw_keyer_straight_key.md](cw_keyer_straight_key.md) (floor, `short` test), [cw_sidetone-findings.md](cw_sidetone-findings.md) (quantum sweep, PTT mode, external keyer), [cw_breakin_plan.md](cw_breakin_plan.md) (probe baseline). Tool: `tools/cwlat` (LT-039).

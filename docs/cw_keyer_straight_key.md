# LC-066: straight key: the start of a key hold is no longer lost in the RX-to-TX timing hole

Index, numbering and open items: [cw.md](cw.md). Depends on [LC-065](cw_keyer_first_tap.md) (`tap_recording` and the flags it clears). Tool: `cwlat --test short` (LT-039).

**Status: local; measured against the unchanged build, 2026-11-07 (below).** Not measured: other speeds, other bands, a real key, Iambic or bug mode with this change.

## The hole
In straight key mode the keyer thread asks for TX when the key goes down, waits for it (20 to 55 ms), and then looks at the key again. If the key is still down it keys down at once, and the tone ends when the key is released: the element is late by the transition and clipped at the front, which is normal for a cold start. If the key has already been released, nothing is sent, and the radio is left in TX with nothing keyed. The tap is lost.

## Measured (unchanged build, quantum 256, one fixed 60-tap schedule of holds 30 to 160 ms)
| run | taps lost |
|---|---|
| base 1, 2, 3 | 0, 0, 1 (trial 33, hold 32.9 ms) |
| post-off (LC-064 off, LC-065 on) | 2 (trials 31 and 33, holds 37 and 33 ms) |
| post-on | 0 |

Only holds of about 33 to 38 ms were lost, which is the time the switch takes. The post-build losses are not distinguishable from the base's with these numbers, and the straight key code is the same in all three builds; the point is that the loss is real and in that window. Earlier unchanged-keyer runs (2026-11-06) lost 0 and 1 of 60.

How much this matters in use: a straight key element is at least a dot long (55 ms at 22 wpm, about 40 ms at 30 wpm), and the window is the switch time, so it is a high-speed and light-touch effect. Not measured at other speeds.

## What the change does
- `keyer_event()` keeps the time of the press and of the release of the straight key (the dash paddle in straight key mode) while the thread cannot look. The release time is stored before the key state itself changes, with a full fence, so a thread that sees the key up also sees the release time.
- If the thread finds the key up and a press was recorded, it keys down at once and sets `tdown` back by the length of the hold, so the existing `STRAIGHT` state finds the key up one loop later and queues the key-up after that time. GPIO and the hang time are handled by that state as for any element.
- **Length and shape.** The element is as long as the hold was. No straight key element is shorter than one RF ramp (`cw_ramp_width` plus 1 ms): the RF ramp is `cw_ramp_width` ms and the side tone ramp 5 ms, and the pulse reaches its plateau only if the key stays down at least one ramp. The floor is applied in the `STRAIGHT` state where the key-up is queued, so it holds for a replayed tap and for a key that was released only a few ms after TX came up (see the first measurement). With the floor every element has the normal edges (5.0 ms 10 to 90 % rise and fall as measured on every other element) and a plateau.
- **A key that is still held when TX comes up** is handled as before: down late, up on time, clipped at the front by the transition. The two cases are not made equal: a tap that is lost today is the case this fixes.
- **No filtering.** A very short press (contact bounce, 1 to 3 ms) now gives an element of the floor length. Debouncing the key is not the keyer's job, and the keyer already started TX for such a press (the blip test: every probe after a 1, 2 or 3 ms press was warm in all three builds).

## Measured 2026-11-07: the `short` test, unchanged build against LC-066
`cwlat --test short --trials 90`, quantum 256, Kiwi on 28187 kHz, dummy load; 10 taps each of 5, 10, 15, 20, 25, 30, 35, 40 and 50 ms, each after a pause of 1.2 to 1.8 s so that every tap arrives in the RX to TX switch. Same schedule on every build. Classified from the carrier envelope in the 100 ms after each press (`tools/cwlat/analysis/cwlat_short_bursts.py`): **full** = the carrier reaches its plateau (peak at least 0.9 of it), **burst** = something above the noise floor that never reaches the plateau, **none** = nothing above the noise floor.

| build | full | burst | none | taps with nothing, by hold (ms) |
|---|---|---|---|---|
| unchanged TEST (`10f40865`) | 48 | 1 | **41** | 5, 10, 15: all 30; 20: 6; 25: 1; 30: 3; 35: 1; 40 and 50: none (the standard analysis counts 46 taps missing at RF and sink) |
| LC-066, first form | 83 | 6 | 1 | 15: 0 (1 burst); 20: 1 (and 1 burst); 25 and 30: bursts only, 2 each |
| **LC-066, second form** | **90** | 0 | 0 | none |
| LC-066, second form, Synth sidetone ticked | 90 | 0 | 0 | none (RF); all 90 also heard at the side tone sink |

- **The unchanged build loses a lot of this.** 41 of 90 taps with a hold of 15 ms or less, or short of the switch time, gave nothing: that is the hole. It is large for these deliberately short taps; in the earlier 60-tap schedule of holds 30 to 160 ms it cost 0 to 2 taps in 60.
- **First form.** The replay worked (all 5 and 10 ms holds recovered at full amplitude), but a key that was still down when TX came up with only a few ms left gave a key-down and a key-up almost at once. 6 of those left a burst at 61 to 82 % of the plateau, one (trial 75, hold 20 ms) left nothing above the noise floor (0.30; the idle noise in that capture peaked at 0.28). So the first form still lost one tap and left six unshaped. The floor that was on the replayed taps only was moved into the `STRAIGHT` state, and now applies to every straight key element.
- **Second form.** All 90 taps are full-amplitude elements, 10 to 50 ms long: the shortest is 9.9 ms (the floor, one RF ramp plus 1 ms at the default 9 ms ramp), and the 10 to 90 % rise and fall are 4.94 and 4.97 ms, as for every other element. Cold onset is about 45 ms at RF (late by the switch, like every cold start). With the side tone box ticked the synth side tone plays the same late, short elements (shortest 9.8 ms), 11.5 ms ahead of the RF.
- **Straight test, 60 taps (holds 30 to 160 ms), second form:** none missing, warm length error +0.03 ms (sd 0.53), cold first element 24.1 ms short (the unchanged build: 24.2 ms): a key held through the switch behaves as before.
- **Blip test, first and second form:** every 1, 2 and 3 ms press now gives an element (12 of 12 at each length, 36 of 36 in all), at the floor length with normal edges; the probes 150 ms later are warm. In the unchanged build and in the post-LC-065 builds none of them gave an element (the keyer started TX for them and sent nothing). That is the intended change: nothing is debounced.
- **Listening note (operator, 2026-11-07, subjective, not a measurement):** the operator listened to the blip runs on the SDR's own receive audio and, within what can reasonably be expected, every one of the shortest side tones also had a matching sound in the SDR audio, and the timing of the multiple blips matched closely by ear between the side tone and the SDR audio; it all sounded right. The operator's decision: blips much shorter than a dit at 20 wpm are not worth more effort, and the blip test is closed as it stands.
- **Analyser.** The first analysis of a `short` capture found a wrong carrier (5 kHz off): the key-down windows it correlates with are the commanded holds (5 to 50 ms) and the radio's tone starts about 43 ms later, so they miss it. `cwlat_analyze.py` now searches only within 200 Hz of the Kiwi's centre (every run so far was within 10 Hz).

## PTT mode (2026-11-09)
With PTT held by MIDI there is no RX to TX switch, so nothing here applies to the first element; 60 taps of holds 30 to 160 ms: none lost, carrier +0.64 +/- 0.54 ms longer than the hold, tone +0.87 +/- 0.74 ms ([findings](cw_sidetone-findings.md#ptt-mode-2026-11-09)).

## Not measured
- Break-in mode on the current build (quantum 128): the numbers above are at 256 on 2026-11-07.
- A real straight key or bug (the virtual MIDI port gives clean, bounce-free edges), other keyer speeds and bands, drive levels above 1 %.
- The effect on a straight key held through the switch for longer than the floor: unchanged by design, but only the 60-tap schedule covers it.
- The hang time's double decrement and the cold start's 20 to 55 ms: open, see [cw.md](cw.md#open-items-the-one-list-the-other-documents-point-here).

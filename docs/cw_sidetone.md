# LC-064: synthesised local CW side tone (PipeWire)

Index, numbering and open items: [cw.md](cw.md). Design record: [cw_sidetone-design.md](cw_sidetone-design.md). Measurements: [cw_sidetone-findings.md](cw_sidetone-findings.md). Tools and tests: LT-039 (`tools/cwlat/README.md`).

**Status: local, not yet offered to dl1ycf.** Bench-tested on one machine (one USB sound card, one Saturn G2, PipeWire). Measured against the unchanged TEST build at quantum 256 on 2026-11-07 (below); the runs of 2026-11-09 were at quantum 128, which this branch requests (a local setting). The list at the end says what has to be done before it is a PR. Off by default, so a build without the box ticked behaves as before.

## In one paragraph

The local CW side tone now has an option to be generated inside the PipeWire output callback instead of being queued through a FIFO. The FIFO (`st_buffer`) is padded with 6.7 to 8 ms of silence so that late samples from the TX engine cannot cause gaps in a tone; that padding is a pure delay. The side tone is a sine of known frequency, with a known ramp, gated by key events that are known in advance, so it does not need a producer at all. Rendering it in the callback removes the producer, the padding and the underrun risk. Measured at the sink: **about 8 ms earlier at each of the three graph quanta tried, 19.0 to 10.9 ms at upstream's quantum of 256.** The RF is not touched and stays at about 22.5 ms after the key event.

## Before and after

| | Before (old path) | After (synth path) |
|---|---|---|
| Who makes the tone | TX engine, one sample per mic sample, into `st_buffer` through `tx_audio_write()` | `sidetone_render()` in `pw_out_cb`, from a copy of the key events |
| Buffering between producer and sound card | FIFO padded to `CW_LAT_TARGET` (384 samples, 8 ms); fill at the start of a tone measured 6.7 to 8 ms, and down to 4 ms lower during a tone | none; tone is rendered into the buffer PipeWire is about to play |
| Where the key event waits for audio | in the FIFO, behind what is already queued | for the next callback: 0 to 1 callback, mean half a callback |
| Underruns | prevented only by the padding | cannot happen for the side tone (nothing to run dry) |
| Element lengths | counted by the TX engine | counted in output samples from the previous event actually executed (the TX engine's rule), so independent of when the callbacks run |
| Radio's own side tone (P1/P2), RF pulse, keyer | unchanged | unchanged |
| TX monitor and SWR tune tones | through `st_buffer` | still through `st_buffer` |

## Expected timing

Delay from the key event (MIDI press) to the first sample at the PipeWire sink monitor, warm taps (the element follows another within 400 ms):

| graph quantum | old path | synth path | gain |
|---|---|---|---|
| 64 | 13.2 ms | 5.0 ms | 8.2 ms |
| 128 | 15.0 ms | 7.0 ms | 8.0 ms |
| **256 (upstream's choice)** | **19.0 ms** | **10.9 ms** | **8.1 ms** |

- **Why a constant 8 ms.** The old path's delay is the quantum terms plus the FIFO cushion; the synth path has the quantum terms only. The gain is therefore the cushion, and it should not depend on the quantum. It does not, to within 0.2 ms. The design estimated 5.5 to 6.5 ms from the measured FIFO fill; the measurement came out about 2 ms better and the reason has not been traced.
- **What the quantum does.** The quantum is upstream's decision (256, moved from 128 after drop-out reports with HDMI audio) and LC-064 does not change it (this branch requests 128 as a local setting). Reading the table: at the sink the synth path at 256 (10.9 ms) is 4 ms earlier than the old path at 128 (15.0 ms), and an estimate of the delay at the ear (sink time plus one quantum plus the sink's headroom; USB and DAC not included) gives 18.9 ms for the synth path at 256 against 19.0 ms for the old path at 128. The quantum in the code is a request (`node.latency`); on the test machine the USB sink was the graph driver and the graph ran at 1024 frames whatever pihpsdr asked, until `clock.force-quantum` was set. Check what a machine really gets with `pw-top`.
- **RF is unchanged.** MIDI press to RF carrier is 22.5 ms (sd 0.6 to 0.9 ms) with every combination. The synth path adds about 0.1 to 0.2 ms to the mean and 0.25 ms to the spread; the cause is not known.
- **Element lengths unchanged.** Iambic elements are -0.17 ms (dit) and -0.18 ms (dah) against nominal in the measured runs, with no measurable jitter, and the stand-alone test shows the renderer's length error is constant with a spread of at most 19 µs under ±3 ms of simulated callback jitter.

### The A/B run of 2026-11-07: unchanged TEST against this branch, quantum 256
Same machine and sink, quantum forced to 256, Kiwi on 28187 kHz, dummy load, 22 wpm, Iambic B, break-in, hang 600; the same trial schedule in every run. Three builds: **base** (`origin/TEST`, `10f40865`), **box unticked** (this branch, old path) and **box ticked** (this branch, synth path). Warm = within 400 ms of the previous element.

| Iambic B, 276 taps per build (dit 60, dah 60, squeeze 36, hang test 120) | base | box unticked | box ticked |
|---|---|---|---|
| warm side tone at the sink (ms) | 19.0 (18.95 to 19.10) | 19.0 (18.94 to 19.05) | **11.1** (10.78 to 11.36) |
| warm RF onset (ms) | 22.6 (22.57 to 22.71) | 22.5 (22.39 to 22.68) | 22.6 (22.53 to 22.76) |
| side tone after (+) or before (-) the RF (ms) | +3.6 | +3.6 | **-11.5** |
| dit and dah length error against nominal (ms) | -0.17, -0.18 | -0.16, -0.18 | -0.16, -0.17 |
| RF edges, 10 to 90 % rise and fall (ms) | 5.0, 5.0 | 5.0, 5.0 | 5.0, 5.0 |

- **The gain at the shipped quantum is 7.9 ms** at the sink (19.0 to 11.1 ms), 41 % less, and the old path with the box unticked is the old path to within 0.1 ms: unticked, LC-064 changes nothing that was measured. The earlier measurement was 19.0 to 10.9 ms (8.1 ms); the two agree to 0.2 ms. **The quantum 64 and 128 rows are carried over from 2026-11-06, not re-measured.**
- **RF is unchanged** by the synth path: 22.6 ms (sd 0.6 to 0.8) in all three builds, so the earlier note that the synth path adds 0.1 to 0.2 ms and 0.25 ms of spread is not seen here (22.6 against 22.6; iambic runs sd 0.62 to 0.77 against 0.66 to 0.74).
- **Taps absent at the sink but present at RF: none** in all the runs of this session (about 1 100 taps on the three builds, break-in mode). The 17 of the earlier runs were cwlat's detector failing on receiver audio on the sink, see the findings.
- **Listening (operator, subjective):** the side tone and the SDR's receive audio were listened to side by side in the blip runs, and the operator found the side tone consistent with the SDR audio, including the shortest elements, with the timing of multiple blips matching closely by ear. See [cw_keyer_straight_key.md](cw_keyer_straight_key.md).
- **Drift:** a repeat of the base dit run 25 minutes later gave warm RF 22.57 against 22.57 ms and warm sink 18.95 against 18.99 ms.
- The keyer losses that the unchanged build shows in the same schedule, and what LC-065 and LC-066 do about them, are in [cw_keyer_first_tap.md](cw_keyer_first_tap.md) and [cw_keyer_straight_key.md](cw_keyer_straight_key.md); they are the same in the box-ticked and box-unticked builds.

### Side tone against RF: a decision, not a defect

| path | quantum | tone at sink | RF | tone earlier than RF by | at the ear (estimate) |
|---|---|---|---|---|---|
| old | 256 | 19.0 ms | 22.6 | 3.6 | 27.0 ms: **4.5 ms after** RF |
| synth | 256 | 10.9 | 22.7 | 11.8 | 18.9 ms: **4 ms before** RF |

At upstream's quantum the old path sounds the tone slightly after the RF; the synth path sounds it a little before. Hardware transceivers sound the side tone on key-down, before the RF, so a lead is normal, but it is a change of behaviour, and whether it is wanted is for the operator and the reviewer to say. It is a decision recorded as open.

## Rationale

1. The delay of the old path has a part that serves no purpose except to hide a late producer. Making the cushion smaller trades delay for clicks in mid-tone, so it cannot just be turned down.
2. The tone is deterministic: frequency, ramp, amplitude and gating are known when the key event is queued. A deterministic signal does not need a FIFO.
3. The change is additive and switchable, so the old path remains the default and the fall-back, and A/B comparison by ear is possible.

## What the change does

- `tx_queue_cw_event(down, wait)` feeds the TX engine's ring exactly as before. If the synth path is active it also hands `{down, wait}` to `sidetone_queue()`.
- In `pw_out_cb`, for the active receiver only, `sidetone_render()` produces the callback's samples while the radio is transmitting in CWL/CWU with duplex off; the result is added to the RX audio and to what is left of `st_buffer`. At any other time `sidetone_idle()` is called. The gate is the TX engine's: its CW tone is made only while transmitting, so neither path sounds a tone for a key-down in RX (see the open question in [the design record](cw_sidetone-design.md#open-question-2026-11-09-should-the-side-tone-follow-the-key-without-ptt)).
- The ramp (5 ms raised cosine), the amplitude rule (0.00196 × volume, 0.00786 on Saturn, volume 0 becomes 12 for CAT CW) and the 20 s key-down protection are the TX engine's.
- While the synth path is active, the TX engine passes silence to `tx_audio_write()`, so the audio module's RX/TX transition handling is unchanged. The side tone sample it computes still goes to the radio and the RF pulse is unchanged.
- **First event after TX starts.** `sidetone_idle()` puts the renderer into the state of a long pause while not transmitting. Without it, a key-down from the external keyer carries the gap to the previous event (up to 0.5 s) as its wait, which the TX engine has already counted but the renderer would count again from the start of TX. Events queued while not transmitting are kept for 200 ms and then dropped. `test/sidetone/test_sidetone_idle.c` checks this; on air, with the external keyer path, the first key-down after TX start (wait 120 to 480 ms) sounds 4.8 to 8.1 ms after the key ([findings](cw_sidetone-findings.md#the-external-keyer-path-2026-11-09)). The TX engine drops events queued before TX is up (it empties its ring every 2 ms while not transmitting), so such a key-down sounds a tone that is not transmitted; the iambic path queues after `mox` and is not affected.
- **Producers.** `tx_queue_cw_event` is called from the keyer thread, the GPIO or MIDI thread, the CAT thread and the server thread. The synth ring is lock-free with one producer and one consumer and takes the same assumption as upstream's ring: only one key source is active at a time. Decision of 2026-11-07, no lock; the consequences are in [the design record](cw_sidetone-design.md#6-risks-and-open-points).

## Files

| File | Change |
|---|---|
| `src/sidetone.c`, `src/sidetone.h` | new: event ring, ramp, sine; no dependency on the rest of piHPSDR |
| `src/pipewire.c` | `audio_synth_sidetone()`; render in `pw_out_cb` |
| `src/transmitter.c` | `tx_queue_cw_event()` also queues for the renderer; silence to `tx_audio_write()` in synth mode |
| `src/audio.h` | declaration of `audio_synth_sidetone()` (`#ifdef PIPEWIRE`) |
| `src/radio.c`, `src/radio.h` | property `cw_sidetone_synth`, default 0 |
| `src/cw_menu.c` | check box "Synth sidetone" (`#ifdef PIPEWIRE`) |
| `Makefile` | `sidetone.o` with the PipeWire audio objects |

PipeWire only and server only: a remote client keeps its own side tone thread, and the other audio modules are untouched. Not part of LC-064 and kept as tooling (LT-039): `test/sidetone/` and `tools/cwlat/`.

## How it was checked

- `gcc -O2 -I src -o /tmp/ts test/sidetone/test_sidetone.c src/sidetone.c -lm && /tmp/ts`: element length error with simulated callback jitter. `test_sidetone_idle.c` builds the same way and exits non-zero on failure; both pass on this branch.
- `make` with PipeWire on this branch (no warnings in the touched files).
- `tools/cwlat/` (README there): MIDI events injected through a VirMIDI port, the side tone recorded at the sink monitor, the RF recorded on a GNSS-stamped KiwiSDR (28187 kHz, 1 % drive into a dummy load). 42 valid runs, about 2 500 keyed elements, 22 wpm, Iambic B and straight key, break-in and PTT, three quanta, both paths. Results: [cw_sidetone-findings.md](cw_sidetone-findings.md).

## Status (2026-11-09)

**Measured**
* Side tone timing at three quanta, old and synth path (2026-11-06), and against the unchanged TEST build at 256 (2026-11-07); the keyer taps in Iambic A and B (dit, dah, squeeze, hang test), straight key, PTT mode, the external keyer path ([cw_sidetone-findings.md](cw_sidetone-findings.md), [cw_keyer_first_tap.md](cw_keyer_first_tap.md), [cw_keyer_straight_key.md](cw_keyer_straight_key.md)).
* Resolved since 2026-11-08: the 17 taps "keyed at RF but absent at the sink" were cwlat's detector failing with receiver audio on the sink, not side tone dropouts; PTT mode has been run (no tap lost, TX releases); the idle state has been checked on air for the external keyer path.

**Not measured, not established**
* A real paddle (the virtual MIDI port skips the controller's USB-MIDI hop); other speeds, bands, sound cards.
* RF onset jitter was slightly higher on the synth path on 2026-11-06 (sd 0.9 against 0.6 ms); not seen on 2026-11-07 or 2026-11-09.
* Wideband key-click level (the Kiwi's AGC defeated it; the edge *shape* matches the designed ramp to 0.5 % rms). Everything was at 1 % drive; PA splatter is not covered.
* AUDIO=PULSE, PORTAUDIO, ALSA, macOS, client/server: not built or tested (the `#ifdef PIPEWIRE` guards compile on PipeWire only).
* Steady-state XRUN counters at any quantum; a persistent quantum setting (`force-quantum` is a runtime setting).
* The bench checklist below, apart from plain keying: CAT CW text and macros, TUNE and the SWR tones, volume 0 and 127, a side tone frequency change, duplex on, CW to SSB and back with the box ticked, a change of the active receiver during TX, the audio device removed during TX. The A/B exercised plain keying only, so this is the most likely place for a real fault in the side tone code.

## Not changed by this, handled separately

Break-in costs more than the audio path: the first element after a pause starts about 21 ms late (the RX to TX switch), and a straight key then loses that time from the front of the element. That is the keyer (`iambic.c`). Taps lost in the switch are fixed in [LC-065](cw_keyer_first_tap.md) (paddles) and [LC-066](cw_keyer_straight_key.md) (straight key); the late start itself is not, see [cw_breakin_plan.md](cw_breakin_plan.md).

## Before this can go upstream

- [ ] bench checklist completed with the box ticked and unticked: PTT and break-in; Iambic A and B, straight key, bug, keys reversed; first element after a pause; end of tone and hang expiry (no tail, no click); CAT CW text and macros; TUNE and the SWR tones; volume 0 and 127; side tone frequency change; change of the active receiver during TX; duplex on (silent as before); mode change CW and SSB with the box ticked; audio device removed during TX; Saturn / G2 level against the radio's own side tone. Plain keying in PTT and break-in, Iambic A and B and the straight key is done.
- [ ] build matrix: PULSE, PORTAUDIO, ALSA, macOS, `-pedantic`, `astyle`, `cppcheck`
- [ ] client/server: the same treatment or a documented exclusion
- [ ] decide the name and place of the option (CW menu check box or radio menu) and its default
- [ ] decide whether the side tone leading the RF is acceptable, and whether it should follow the key without PTT (zero beating)
- [ ] manual (`release/LatexManual`): CW chapter
- [ ] send the source only: `test/sidetone`, `test/breakin`, `tools/cwlat` and `src/cwtime.[ch]` stay with us

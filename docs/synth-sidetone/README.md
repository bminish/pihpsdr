# Synth side tone - a patch for piHPSDR

**Status: working prototype, bench-tested on one machine, not ready for upstream.** The aim is to get it there; the list at the end says what is missing.

The patch makes piHPSDR generate the *local* CW side tone inside the PipeWire output callback instead of queueing it through a FIFO.
It is off by default (CW menu check box "Synth side tone", property `cw_sidetone_synth`) and changes nothing else:
the radio's own side tone, the RF pulse and the keyer are untouched, and the old path stays as the default and the fall-back.

## Why
The old path hands the side tone from the TX engine to the audio module through `st_buffer`, which is padded with silence to
`CW_LAT_TARGET` (384 samples, 8 ms) so that late samples from the TX engine cannot cause a gap in the middle of a tone. Measured: the fill at the
start of a tone was 6.7-8 ms, and during a tone it fell by up to 4 ms below that. A smaller cushion would give clicks, so the delay cannot simply
be turned down. The side tone is a sine of known frequency with a known ramp and a key state known in advance; it does not need a producer at all.
Rendering it in the callback removes the producer, the cushion and the underrun risk. The measurements are in [findings.md](findings.md).

## What it does
* `tx_queue_cw_event(down, wait)` in `transmitter.c` still feeds the TX engine ring exactly as before. When the synth path is active it also
  hands `{down, wait}` to `sidetone_queue()` (`sidetone.c`).
* In `pw_out_cb` (PipeWire output callback, **active receiver only**) `sidetone_render()` produces `n_frames` samples while the radio is
  transmitting in CWL/CWU with duplex off; the result is added to the RX audio and to what is left of `st_buffer` (TX monitor and SWR tune tones
  still use it). Otherwise the renderer drops its queue and releases the key.
* **Waits are counted in output samples from the previous event actually being executed**, the rule the TX engine uses, so the lengths of
  elements and gaps do not depend on when the callbacks run. (An earlier version used absolute time stamps and shortened every element by 0-1 quantum:
  the key-down waited for the next callback while the key-up landed on its exact time. See design.md 4.1.)
* Ramp (5 ms raised cosine) and amplitude rules are those of the TX engine: 0.00196 x volume (0.00786 on Saturn), volume 0 becomes 12 for CAT CW.
* While the synth path is active the TX engine calls `tx_audio_write()` with silence, so the RX/TX transition handling in the audio module is unchanged.
  The side tone sample it computes still goes to the radio (P1/P2 side tone) and the RF pulse shape is unchanged.
* Delay from a key event to the first output sample: 0 to 1 callback (mean half a callback).

## Files
| File | Change |
|---|---|
| `src/sidetone.c`, `src/sidetone.h` | new: event ring, ramp, sine, no dependency on the rest of piHPSDR |
| `src/pipewire.c` | `audio_synth_sidetone()`; render in `pw_out_cb` |
| `src/transmitter.c` | `tx_queue_cw_event()` also queues for the renderer; silence to `tx_audio_write()` in synth mode |
| `src/audio.h` | declaration of `audio_synth_sidetone()` (`#ifdef PIPEWIRE`) |
| `src/radio.c`, `src/radio.h` | property `cw_sidetone_synth` (default 0) |
| `src/cw_menu.c` | check box "Synth sidetone" (`#ifdef PIPEWIRE`) |
| `Makefile` | `sidetone.o` in `AUDIO_OBJS` (PipeWire only), `sidetone.c` in `CPP_SOURCES`, dependencies |
| `test/sidetone/test_sidetone.c` | stand-alone test of the renderer against simulated callback jitter |
| `tools/cwlat/` | external measurement: MIDI events in, side tone at the sink and RF on a GNSS-stamped KiwiSDR out |

PipeWire only, server only (a remote client keeps its own side tone thread). Other audio modules are untouched.

## Applying it
The patches are generated, not stored (the repository does not keep patch files). From a clone that has the `feature/synth-sidetone` branch:

    tools/synth-sidetone/synth-sidetone-patches.sh export DIR     # writes DIR/000?-*.patch
    tools/synth-sidetone/synth-sidetone-patches.sh check [REF]    # applies the series to REF (default master) in a scratch worktree and compiles the touched files

then on top of upstream: `git am --3way DIR/*.patch`. The series is:

1. the feature itself (`src/`, `Makefile`): applies on its own, nothing else is needed
2. documentation, test and tools (cannot conflict with anything)
3. **optional, local:** PipeWire playback quantum 128 instead of upstream's 256 (see below)

Verified to apply cleanly to upstream `master` (82aa8c7c) and to compile there with `AUDIO=PIPEWIRE`.

## PipeWire quantum
The delay of the side tone, with the synth path, is about 1.5 callbacks plus the sink's own buffering, and both scale with the graph quantum
(measured on a USB sound card, warm taps, MIDI press to sink monitor):

| quantum | old path | synth path | sink latency reported by PipeWire |
|---|---|---|---|
| 64 | 13.2 ms | 5.0 ms | 1 quantum + 48 samples |
| 128 | 15.0 ms | 7.0 ms | 1 quantum + 64 samples |
| 256 | 19.0 ms | 10.9 ms | 1 quantum + 128 samples |

* The quantum in the code is only a *request* (`node.latency`). On the test machine the sink was the graph driver and the graph ran at 1024 frames
  whatever pihpsdr asked for; it only went down after `pw-metadata -n settings 0 clock.force-quantum 128`. That setting is machine-wide and does not
  survive a PipeWire restart. A persistent setting (a `pipewire.conf.d` snippet or a `node.force-quantum` stream property) has **not** been tested.
  Check what you really get with `pw-top` (QUANT column of the sink) and the callback size.
* Upstream moved the playback quantum from 128 to 256 after reports of drop-outs with HDMI audio on some systems. 128 ran without audible
  drop-outs on the test machine, but the XRUN counters (`pw-top` ERR) were not logged during a clean steady-state run at 128; the counters seen
  rose during a session in which the quantum was changed some forty times. Do that run before relying on 128.
* The third patch only changes the constant and its comment.

## Testing
* `gcc -O2 -I src -o /tmp/test_sidetone test/sidetone/test_sidetone.c src/sidetone.c -lm && /tmp/test_sidetone`: element length spread
  <= 19 us with +-3 ms of callback jitter (tests the counting of waits, not PipeWire).
* `tools/cwlat/` (README there): needs a VirMIDI port (`modprobe snd-virmidi`), a KiwiSDR for the RF side, a dummy load. Results: [findings.md](findings.md).
* Bench checklist (everything with the check box ticked and unticked; PA off or a dummy load):
  PTT and break-in; Iambic A / B, straight key, bug, keys reversed; first element after a pause (clipped or missing?); end of tone and hang expiry (no tail,
  no click); CAT CW text and CW macros; TUNE and the SWR tones; volume 0 and 127; sidetone frequency change; change of the active receiver during TX;
  duplex on (silent as before); mode change CW <-> SSB while ticked; audio device removed during TX; Saturn / G2 level against the radio's own side tone.
  The first four items have been done with the cwlat matrix; the rest are open.

## Known limitations and open items
* The side tone **leads** the RF pulse by 12-18 ms (quantum 64-256); RF is 22.5 ms after the key event. The old path at quantum 256 lags it by about 4.5 ms.
  Whether the lead is acceptable (hardware radios sound the side tone on key-down, before RF) is a decision, not a defect.
* Break-in start: the first element after a pause starts about 21 ms late and with a straight key loses that time at its front; short taps
  during the RX->TX switch are lost. Not caused by, and not fixed by, this patch (keyer thread, `iambic.c`).
  **The lost-first-tap fix is a separate branch, `fix/keyer-lost-first-tap`** (one commit on `master`, independent of the side tone):
  `iambic.c` no longer loses a short tap that arrives while the keyer waits for TX. Send it alongside this series; the straight-key
  start-of-hold fix (`devtest/everything`, LC-066) is not on that branch yet.
* 17 taps in the cwlat runs were keyed at RF but absent at the sink (straight key, PTT, both paths): either a real side tone dropout or an
  artefact of cwlat's sink-side detector. Unresolved.
* Synth-path RF onset jitter measured slightly higher (sd 0.9 against 0.6 ms). Not understood.
* Wideband key-click level of the RF edges could not be quantified (Kiwi AGC); the edge *shape* matches the designed DL1YCF ramp (0.5 % rms).
* Not built or tested: AUDIO=PULSE / PORTAUDIO / ALSA (the `#ifdef PIPEWIRE` guards are compile-checked by the `check` script only), macOS, client/server.
* Not measured: Iambic A, other speeds and bands, a real paddle (the virtual MIDI port skips the USB-MIDI hop).
* The PTT-mode runs of cwlat left PTT stuck in piHPSDR. Cause found 2026-11-09: the test sender dropped MIDI running status, which pihpsdr's
  parser does not handle; fixed in the sender (`devtest/everything`, `tools/cwlat`). The PTT-mode results in this branch's findings predate that fix and are invalid.

## Before this can go upstream
- [ ] bench checklist above completed, with the box ticked and unticked
- [ ] quantum 128 (or whichever is chosen) run steady with XRUN counters logged; decide whether piHPSDR should set it (stream property) or document it
- [ ] resolve the 17 sink-only taps
- [ ] build matrix: PULSE, PORTAUDIO, ALSA, macOS, `-pedantic`, `astyle`, `cppcheck`
- [ ] client/server: either the same treatment or a documented exclusion
- [ ] decide the name and place of the option (CW menu check box vs radio menu), and the default
- [ ] manual (`release/LatexManual`): CW chapter, quantum advice
- [ ] split off or drop `tools/cwlat` and the test from what is sent upstream

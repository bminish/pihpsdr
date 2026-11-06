# Design: synthesise the local CW sidetone in the PipeWire callback

Status: implemented as a prototype - see README.md in this directory. This is the original design; 4.1 and 4.5 were revised after testing.
Scope: local sidetone on the PipeWire audio module only (first step).

## 1. Problem

Paddle-to-ear delay for the local sidetone is dominated by two audio-side terms, one of which is fixed by
the design:

1. The PipeWire graph cycle. Measured: the USB sink ran the graph at 1024 frames (21.3 ms) although
   `pihpsdr-rx` asked for 256. Forcing the quantum to 64 made the sidetone "much tighter" (operator report).
2. The sidetone FIFO `st_buffer` in `src/pipewire.c`. It is padded with zeros to `CW_LAT_TARGET`
   (384 samples) at the RX->TX transition and every tone is queued behind whatever is in it.

Measurement with the CWDEBUG logging (about 55-60 tones, callback = 64 frames, 0 underruns):

| Quantity | Observed |
|---|---|
| Fill when a tone starts (= delay behind the FIFO) | 244-437 samples; mostly 320-390 (6.7-8.1 ms) |
| Lowest fill reached during a tone | 193 samples (4.0 ms) |
| Largest drop below onset fill | about 192 samples (4.0 ms) |

The FIFO exists to absorb late samples from the TX engine (the producer). The drops show the producer is
late by up to about 4 ms, so the cushion cannot simply be made smaller. A smaller cushion trades delay for
mid-tone clicks.

## 2. Idea

The sidetone is a sine at a known frequency, shaped by a known 5 ms raised-cosine ramp, gated by a key
state that is known *in advance* (the keyer queues events with their timing). Nothing about it needs to come
through a producer/consumer FIFO. If the PipeWire callback renders the tone itself, there is no producer to
be late, so no cushion is needed, and delay falls to at most one callback.

Expected result: FIFO delay 6.7-8 ms -> at most one callback, 1.3 ms at quantum 64. Gain about 5.5-6.5 ms.

## 3. Current structure (as read from the code)

- `tx_queue_cw_event(down, wait)` (`transmitter.c`) pushes `(state, wait)` into a 1024-entry ring. `wait` is
  in 1/48000 s *since the previous event was executed*.
- The TX engine consumes the ring once per mic sample (`next_cw_sidetone_sample`): an event runs when
  `cw_delay_time >= wait`. It sets `keydown`, and applies the 240-sample audio ramp and the RF ramp.
- The engine returns the sidetone sample. `tx_audio_write()` pushes it into `st_buffer`; the same sample goes
  to the radio for P1/P2 sidetone.
- `tx_audio_write()` also carries the TX monitor and the SWR tune tones, so `st_buffer` cannot go away.
- Producers of `tx_queue_cw_event`: keyer thread (`iambic.c`), MIDI/GPIO path (`actions.c`), CAT (`rigctl.c`),
  client/server (`server_thread.c`).

## 4. Proposed design

### 4.1 Event queue (revised after the stand-alone test)
`tx_queue_cw_event` also pushes `{state, wait}` to a second ring, read by the renderer. The renderer counts
`wait` in **output samples from the previous event actually being executed**, exactly as the TX engine does.

The first draft of this design stamped events with absolute `CLOCK_MONOTONIC` due times. The unit test
(`test/sidetone/test_sidetone.c`) showed that this makes every element 0-1.3 ms short (mean -34 samples,
sd 19), because the key-down is held until the next callback while the key-up lands on its exact time.
Counting waits in output samples removes that: element length error is constant (-2 samples, the edge
detector) with a spread of <= 19 us even with +-3 ms callback jitter or 5 ms spikes.

### 4.2 Renderer
A small module (`sidetone.c/.h`, name open) holding: frequency, volume, the ramp table, phase accumulator, ramp
position, and the event ring. `sidetone_render(double *out, int n, ...)` executes due events per sample, steps the ramp, multiplies by the sine, and returns n samples.
The amplitude rules move here unchanged: 0.00196 * vol (0.00786 on Saturn), minimum vol 12 for CAT CW.

### 4.3 PipeWire callback
`pw_out_cb` adds `sidetone_render()` output to the existing `rx + st_buffer` sum. `st_buffer` stays for the
TX monitor and SWR tones. When the renderer is active the engine writes 0.0 instead of the CW sidetone
sample to `tx_audio_write()`, so the tone is not played twice. P1/P2 radio sidetone is unchanged.

### 4.4 Gating
Render only while transmitting in a CW mode, as the engine does. Reset the absolute-time ring when leaving
TX (mirrors the `keydown = 0; ring reset` branch). Keep the 20 s key-down protection.

### 4.5 Time base
Not needed for timing: element lengths and gaps are counted in output samples. The callback entry time is only
passed in for CWDEBUG statistics. Event onset is quantised to the callback: 0-1 callback (mean 0.7 ms at 64
frames).

### 4.6 Switch
Property `cw_sidetone_synth` (CW menu checkbox "Synth sidetone", PipeWire builds only) selects old or new path, so the operator can A/B by ear. Default off.

## 5. What does not change

- RF pulse timing and shaping (engine clock).
- Radio-generated sidetone on P1/P2.
- Keyer, MIDI, break-in logic.
- Other audio modules (PulseAudio, PortAudio, ALSA).

## 6. Risks and open points

1. **Sidetone vs RF alignment.** The sidetone would lead RF by the engine and radio latency, a few ms. Many
   radios already sound the sidetone from the FPGA, so this is probably acceptable. Reviewer decision.
2. **Multiple producers.** `tx_queue_cw_event` is called from several threads. The existing ring is already
   single-producer in practice; the new ring inherits the same assumption.
3. **Client/server.** `client_sidetone_thread` generates its own sidetone and needs the same treatment or an
   explicit exclusion.
4. **Late events.** An event whose `due` is in the past renders from the start of the next callback. That is
   the intended behaviour, and the old path behaves the same way.
5. **Sink headroom.** The USB sink reports `api.alsa.headroom` 512 (10.7 ms). Not measured, not addressed here.

## 7. Test plan

- Offline: feed the renderer the same event list as the engine and compare envelope and element lengths with
  the engine's output (offset-corrected).
- On the bench with the existing CWDEBUG logging extended to record paddle-press time against first non-zero
  rendered sample.
- Underrun counter must stay at 0 by construction; verify.
- A/B listening with the switch, in PTT and break-in, PA off and drive at zero.

## 8. What this does not fix

- Break-in first element 19-41 ms late (RX->TX switch).
- Tap lost in the RX-recovery window after the hang time (hang counter runs at half rate; 50 ms timer from
  commit `4aa0aeb4`).
- Sink headroom.
- Repeat-point ("saddle point") behaviour of the keyer.

## 9. Size

About 300-400 lines: `transmitter.c` (queue + suppress), `pipewire.c` (callback), new `sidetone.c/.h`,
Makefile, one property.

## 10. Current working-tree state

`src/pipewire.c` is modified and uncommitted: `PIPEWIRE_QUANTUM_PLAYBACK` = 64 and the CWDEBUG measurement
block. Runtime `clock.force-quantum` was set to 64 for testing.

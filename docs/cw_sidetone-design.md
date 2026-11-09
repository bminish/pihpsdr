# Design: synthesise the local CW sidetone in the PipeWire callback

Status: implemented as LC-064 - see [cw_sidetone.md](cw_sidetone.md); index: [cw.md](cw.md). This is the design record; 4.1 and 4.4 were revised after testing.
Scope: local sidetone on the PipeWire audio module only.

## 1. Problem

Paddle-to-ear delay for the local sidetone is dominated by two audio-side terms, one of which is fixed by
the design:

1. The PipeWire graph cycle. Measured: the USB sink ran the graph at 1024 frames (21.3 ms) although
   `pihpsdr-rx` asked for 256. Forcing the quantum made the sidetone "much tighter" (operator report). The quantum is upstream's decision and is not changed by this work; the effect of the graph quantum is only measured (see [cw_sidetone-findings.md](cw_sidetone-findings.md)).
2. The sidetone FIFO `st_buffer` in `src/pipewire.c`. It is padded with zeros to `CW_LAT_TARGET`
   (384 samples) at the RX->TX transition and every tone is queued behind whatever is in it.

Measurement with temporary debug logging (about 55-60 tones, callback = 64 frames, 0 underruns; the logging is removed):

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

Expected result at design time: FIFO delay 6.7-8 ms -> at most one callback, so a gain of about 5.5-6.5 ms at quantum 64. Measured later: about 8 ms at every quantum tried (64, 128, 256); why that exceeds the estimate by about 2 ms was not traced.

## 3. Current structure (as read from the code)

- `tx_queue_cw_event(down, wait)` (`transmitter.c`) pushes `(state, wait)` into a 1024-entry ring. `wait` is
  in 1/48000 s *since the previous event was executed*.
- The TX engine consumes the ring once per mic sample (`next_cw_sidetone_sample`): an event runs when
  `cw_delay_time >= wait`. It sets `keydown`, and applies the 240-sample audio ramp and the RF ramp. Only while
  transmitting: otherwise it empties the ring (every 2 ms).
- The engine returns the sidetone sample. `tx_audio_write()` pushes it into `st_buffer`; the same sample goes
  to the radio for P1/P2 sidetone.
- `tx_audio_write()` also carries the TX monitor and the SWR tune tones, so `st_buffer` cannot go away.
- Producers of `tx_queue_cw_event`: keyer thread (`iambic.c`), MIDI/GPIO path (`actions.c`), CAT (`rigctl.c`),
  client/server (`server_thread.c`).

## 4. Design

### 4.1 Event queue (revised after the stand-alone test)
`tx_queue_cw_event` also pushes `{state, wait}` to a second ring, read by the renderer. The renderer counts
`wait` in **output samples from the previous event actually being executed**, exactly as the TX engine does.

The first draft stamped events with absolute `CLOCK_MONOTONIC` due times. The unit test
(`test/sidetone/test_sidetone.c`) showed that this makes every element 0-1.3 ms short (mean -34 samples,
sd 19), because the key-down is held until the next callback while the key-up lands on its exact time.
Counting waits in output samples removes that: element length error is constant (-2 samples, the edge
detector) with a spread of <= 19 us even with +-3 ms callback jitter or 5 ms spikes.

### 4.2 Renderer
`sidetone.c/.h`: frequency and volume (passed in), the ramp table, phase accumulator, ramp position, and the
event ring. `sidetone_render(double *out, int n, ...)` executes due events per sample, steps the ramp, multiplies by the sine, and returns n samples.
The amplitude rules are the engine's: 0.00196 * vol (0.00786 on Saturn), minimum vol 12 for CAT CW.

### 4.3 PipeWire callback
`pw_out_cb` adds `sidetone_render()` output to the existing `rx + st_buffer` sum. `st_buffer` stays for the
TX monitor and SWR tones. When the renderer is active the engine writes 0.0 instead of the CW sidetone
sample to `tx_audio_write()`, so the tone is not played twice. P1/P2 radio sidetone is unchanged.

### 4.4 Gating and the idle state (revised after testing)
The renderer runs only while transmitting in a CW mode with duplex off, as the engine's tone does. While not
transmitting the callback calls `sidetone_idle()`, which releases the key and sets `delay_samples` to its maximum
("long pause", the state the engine's `cw_delay_time` is always in, because the engine counts it also while
receiving). Consequences:

* the first event after TX starts is executed at once whatever its `wait` is. `CW_KEYER_KEYDOWN` (MIDI/GPIO
  external keyer) sends the gap to the previous event as `wait` (up to 0.5 s); counted again from the start of
  TX that gap delayed the side tone by up to 0.5 s while the RF keyed on time. Checked on air: 4.8 to 8.1 ms
  ([findings](cw_sidetone-findings.md#the-external-keyer-path-2026-11-09));
* events queued while not transmitting are kept for 200 ms and then dropped, so that events of a TX that never
  came cannot start a tone later. The TX engine does not keep them (it empties its ring every 2 ms while not
  transmitting, which is right: key state is queued for transmission only once PTT has started), so an external
  keyer whose key-down arrives before TX is up gets a side tone that is not transmitted. The iambic path queues
  its key-down after `mox` is up and is not affected. (An earlier version of this design assumed that the engine
  keeps early events and that break-in needed the 200 ms; neither is so.)

The 20 s key-down protection is kept. Tested by `test/sidetone/test_sidetone_idle.c` (assertions; the first
cases fail with the previous behaviour: build with `-DUSE_RESET`).

### 4.5 Time base
Not needed for timing: element lengths and gaps are counted in output samples. Event onset is quantised to the
callback: 0-1 callback (mean 0.7 ms at 64 frames).

### 4.6 Switch
Property `cw_sidetone_synth` (CW menu checkbox "Synth sidetone", PipeWire builds only) selects old or new path, so the operator can A/B by ear. Default off.

## 5. What does not change

- RF pulse timing and shaping (engine clock).
- Radio-generated sidetone on P1/P2.
- Keyer, MIDI, break-in logic.
- Other audio modules (PulseAudio, PortAudio, ALSA).

## 6. Risks and open points

1. **Sidetone vs RF alignment.** The sidetone leads RF by the engine and radio latency, 12 to 18 ms at the quantums tried (measured). Hardware transceivers sound the sidetone on key-down, so a lead is normal; reviewer decision.
2. **Multiple producers.** `tx_queue_cw_event` is called from several threads: the keyer thread (internal keyer), the GPIO or MIDI
   thread (external key, `CW_KEYER_KEYDOWN`), the CAT thread and the server thread. The existing ring has no lock and relies on only one
   source being active at a time. **Decision (2026-11-07): the new ring inherits the same assumption and takes no lock**, to stay as
   close to upstream as possible. Consequences: (a) if two sources ever overlap, upstream's ring is already unprotected, and the two rings
   could then disagree, so the side tone and the RF would be out of step (no crash); (b) unlike upstream's TX engine, which resets its ring
   from the consumer side by writing `cw_ring_inpt`, `sidetone_idle()` writes only the consumer's own index. A mutex around both pushes in
   `tx_queue_cw_event` would close the race for both rings; that is a change to upstream's key path and is not part of this series.
3. **Client/server.** `client_sidetone_thread` generates its own sidetone and needs the same treatment or an explicit exclusion.
4. **Late events.** An event whose time is in the past renders from the start of the next callback. That is the intended behaviour, and the old path behaves the same way.
5. **Sink headroom.** The USB sink reports `api.alsa.headroom` 512 (10.7 ms). Not measured, not addressed here.
6. **Large callbacks.** A callback of more than 4096 frames renders nothing while the engine has already stopped feeding the tone to the audio; not seen at the quantums in use.

## 7. How it is tested
`test/sidetone/` (stand-alone, no radio): element length under callback jitter (`test_sidetone.c`), idle and start-of-TX behaviour with assertions (`test_sidetone_idle.c`). On the radio and the KiwiSDR with `tools/cwlat` (README there); results in [cw_sidetone-findings.md](cw_sidetone-findings.md).

## Open question (2026-11-09): should the side tone follow the key without PTT?
The side tone is the key made audible and is never wrong when it sounds with the key down, with or without PTT (zero beating: key down against the receiver audio, nothing transmitted). Today neither path does it: the engine produces the side tone only while transmitting (`xmit` in `tx_add_mic_sample()`), and the synth renders only while `radio_is_transmitting()` (`pw_out_cb()`). If the synth followed the key regardless of PTT, the TX gate and the idle state of `sidetone_idle()` would go: the renderer would run in every callback, `delay_samples` would count all the time (as the engine's `cw_delay_time` does, which is what makes the first event after a pause immediate), and nothing is queued or reset on a TX change. Not implemented; the CW mode, duplex and key-down protection gates stay to be decided.

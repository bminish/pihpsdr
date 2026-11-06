# Callback-synthesised CW sidetone - status, test plan

Branch: `test/sidetone` (from `test/binaural-rade`).
Design rationale: `docs/design-callback-sidetone.md`.

**Status: working prototype, not validated.** First impression from the operator (bench, one machine,
PipeWire, PA off / drive zero): sounds good and feels like real hardware. Nothing below the
"Done" heading has been checked yet, and this must not be taken as finished work.

## 1. Why

Paddle-to-ear delay felt "heavy", with double dits on short taps. Findings, in order:

1. The keyer itself is accurate: dots 53-55 ms against 54.5 ms calculated at 22 wpm, 109 ms to the repeat
   sample, 0-2 ms from element start to the engine passing the event on. No regression in the keyer state machine
   since PipeWire arrived; the one relevant change is `4aa0aeb4` (50 ms timer before leaving TX), which widens
   the window in which a tap after the hang time can be lost.
2. On this machine the PipeWire graph was cycling at **1024 frames (21.3 ms)** because the USB sink
   (Arylic, `api.alsa.period-size` 512, `headroom` 512) was the driver, although `pihpsdr-rx` requested 256.
   `clock.force-quantum` 256, then 64, made the sidetone "much tighter".
3. The sidetone FIFO in `pipewire.c` (`st_buffer`) holds 6.7-8 ms of silence ahead of every tone. Measured
   (`CWDEBUG`, ~55 tones, 64-frame callbacks): fill at onset 244-437 samples (mostly 320-390), fill dropped
   up to ~192 samples (4 ms) below onset during tones, **0 underruns**. The cushion is not oversized: the TX
   engine delivers samples up to ~4 ms late.
4. So the delay cannot be cut safely by shrinking the FIFO. Rendering the tone in the output callback removes
   the producer, hence the cushion and the underrun class.

## 2. Done

### Commit `12fa057a` (already on the branch)
- `src/pipewire.c`: `PIPEWIRE_QUANTUM_PLAYBACK` 256 -> 64 (capture stays 256).
- `src/pipewire.c`: `CWDEBUG`-only measurement of `st_buffer` fill (see section 6).
- `docs/design-callback-sidetone.md`.

### This commit
| File | Change |
|---|---|
| `src/sidetone.c`, `src/sidetone.h` | New. Event ring, 5 ms raised-cosine ramp, sine generator, `CWDEBUG` statistics. No dependency on radio code, so it can be tested stand-alone. |
| `src/pipewire.c` | `audio_synth_sidetone()`; in `pw_out_cb` render the sidetone for the active receiver and add it to the output; reset it when not transmitting CW. |
| `src/transmitter.c` | `tx_queue_cw_event()` also queues the event for the renderer (only when synth is active). In synth mode the TX engine passes silence to `tx_audio_write()` instead of the sidetone sample. |
| `src/audio.h` | Declares `audio_synth_sidetone()` (`#ifdef PIPEWIRE`). |
| `src/radio.c`, `src/radio.h` | New property `cw_sidetone_synth` (default 0), saved and restored. |
| `src/cw_menu.c` | "Synth sidetone" check box in the CW menu (`#ifdef PIPEWIRE`). |
| `Makefile` | `src/sidetone.o` added to `AUDIO_OBJS`, `src/sidetone.c` to `CPP_SOURCES`, header dependency lines. |
| `test/sidetone/test_sidetone.c` | Stand-alone jitter test (section 5). |

## 3. How it works

1. The keyer, MIDI, CAT and GPIO paths all end in `tx_queue_cw_event(down, wait)`. It still feeds the TX engine
   ring exactly as before. If `audio_synth_sidetone()` is true it also pushes `{down, wait}` to the renderer's ring.
2. `pw_out_cb` (PipeWire output callback, active receiver only) renders `n_frames` of sidetone when:
   synth is enabled, the radio is transmitting, duplex is off, and the TX mode is CWL/CWU. Otherwise it drops
   queued events and releases the key (same behaviour as the engine's reset branch).
3. For each output sample the renderer increments a delay counter. If an event is queued and the counter has
   reached its `wait`, the event is executed (key state set, counter cleared). **Waits are counted in output
   samples from the previous event actually executing**, the same rule as the TX engine. Element lengths are
   therefore independent of when callbacks run.
4. The ramp pointer moves one step per sample toward 240 (key down) or 0 (key up); the output is
   `amp * ramp[ptr] * sin(phase)`. Amplitude rules copied from the engine: 0.00196 x volume (0.00786 on Saturn);
   volume 0 becomes 12 for CAT CW.
5. The renderer's output is added to `rx audio + st_buffer` in the same callback. `st_buffer` is untouched and
   still carries the TX monitor and SWR tune tones.
6. The TX engine still computes the sidetone sample: it goes to the radio (P1/P2 sidetone) and the RF pulse
   shape is unchanged. Only the *local* audio write is replaced by silence.
7. Delay from key event to output is quantised to the callback: 0 to 1 callback (mean 0.7 ms, maximum 1.33 ms at
   64 frames). Previously 6.7-8 ms of FIFO plus the same callback quantisation.

## 4. Known limitations / decisions taken

- **PipeWire only.** PulseAudio, PortAudio, ALSA unchanged.
- **Server only.** `audio_synth_sidetone()` returns 0 when `radio_is_remote`; the client's own sidetone thread is
  untouched.
- **Sidetone leads the RF pulse** by the engine and radio latency (a few ms), because it no longer runs on the TX
  engine's sample clock. Accepted at review. Needs to be measured, not assumed.
- Default is off. The old path is still there and is the fallback.
- `CW_LAT_*` and the FIFO still exist and still apply when synth is off.
- Not addressed here: break-in start delay (19-41 ms to first element), the dropped tap shortly after the hang
  time ends, the effective hang being half the configured value (the counter decrements in two states), the
  keyer repeat ("saddle") point.
- The 64-frame playback quantum in code only takes effect if PipeWire honours it; on this machine the
  sink forced 1024 until `clock.force-quantum` was set at runtime. Upstream moved 128 -> 256 because of HDMI
  dropouts on some boxes (`05793824`), so 64 may not be safe elsewhere. Open: keep it, make it a setting, or
  document the runtime setting only.
- `CWDEBUG` code is left in (compiled out by default). Decide whether to keep it before any upstreaming.
- `Makefile` dependency lines were edited by hand.

## 5. Evidence so far

Stand-alone test, `test/sidetone/test_sidetone.c`, 22 wpm dits, 64-frame callbacks, 400 elements per case:

    gcc -O2 -I src -o /tmp/test_sidetone test/sidetone/test_sidetone.c src/sidetone.c -lm && /tmp/test_sidetone

| Callback timing error | Element length spread (sd) |
|---|---|
| none | 0 us |
| +-0.1 ms | 4 us |
| +-0.3 ms | 4 us |
| +-1 ms | 12 us |
| +-3 ms | 19 us |
| +-0.1 ms plus 5 ms spike every 200 callbacks | 0 us |

Constant offset of -2 samples is the 4-sample envelope detector, not the renderer. An earlier version that used
absolute timestamps showed -34 +- 19 samples; that is why waits are now counted in samples.

Caveat: this is a simulation. Real callback timing was **not** measured under load (see test 6.3).

## 6. To do before this can be called done

### 6.1 Correctness / regression (bench, PA off, drive zero, operator present)
| # | Check | How |
|---|---|---|
| 1 | Synth off = old behaviour | Untick the box, compare feel and `CWDBG tone:` lines with the 23:51 baseline log. |
| 2 | PTT and break-in, both | Key dits, dahs, squeezed characters in each mode. |
| 3 | Iambic A, B, straight, bug, keys reversed | Same, with the CW menu settings. |
| 4 | Start of first element after a pause | Listen for a clipped or missing first dit (the RX/TX transition is where the old path pads). |
| 5 | End of tone / hang expiry | Does the tone stop cleanly, no tail or click when TX ends. |
| 6 | CAT CW text, CW macros (CWTxt1-5), TUNE, SWR tune tones | Run each; SWR tones and the TX monitor still go through `st_buffer`. |
| 7 | Volume 0, volume 127, frequency change during a session | Level matches the old path (A/B at same volume); no click on frequency change. |
| 8 | Active receiver change during TX; two receivers | The tone should follow `active_receiver`, never play twice. |
| 9 | Duplex on | Sidetone should be silent as before. |
| 10 | Mode change CW <-> SSB while checked | No stuck tone; reset path works. |
| 11 | Audio device removed/re-added, `audio_close_output` during TX | No crash; static renderer state is not tied to the stream. |
| 12 | Saturn/G2 amplitude | Level against the radio's own sidetone. |

### 6.2 Build matrix (not done)
Only the PipeWire configuration was built (with `-DCWDEBUG`). Need: `AUDIO=PULSE`, `PORTAUDIO`,
`ALSA` builds (the `#ifdef PIPEWIRE` guards in `transmitter.c`, `cw_menu.c`, `audio.h` have not been compiled
without PipeWire), macOS build, `-pedantic`, `astyle`, `cppcheck` (`make cppcheck`; `sidetone.c` added to
`CPP_SOURCES`), `scan-build` if used for upstream.

### 6.3 Measurement
- **Real callback jitter.** `CWDEBUG` build prints `callback interval jitter ... rms / max` every 1500
  callbacks. Run under load (panadapter at full rate, RX2, DX cluster, Chrome playing) and at quantum 64, 128,
  256. Compare with the simulated +-3 ms case.
- **Software latency per element.** `CWDBG synth: ... lag(queue->output position)` lines give the delay from the
  event being queued to the output sample position. Expect <= 1 callback.
- **True end-to-end delay.** Not measured. Needs an external reference: key line (or MIDI interface LED/output)
  and headphone audio into two channels of a second sound card or scope, old path vs new path. This also gives
  the sidetone-to-RF offset (listen on a receiver with the PA off, or use a dummy-load monitor) and shows whether the
  USB sink headroom (512 samples) applies at quantum 64.
- **Element length at the output.** Record the sink monitor (`pw-record --target <sink> ...`) while sending
  a known text at fixed speed through the keyer or CAT CW; measure dits/dahs/gaps against nominal.

### 6.4 Documentation
- Manual (`release/LatexManual`): CW chapter, the new check box and the recommended PipeWire quantum setting.
- `docs/changes.md` / `docs/branches.md` entries for `test/sidetone`.
- Decide what the user-visible name of the option is; "Synth sidetone" is a working label.
- Record the machine's PipeWire notes (sink period/headroom, forced quantum) where users will find them.

### 6.5 Decide before upstreaming
Keep `CWDEBUG` blocks? Keep the 64-frame default? Extend to client/server and other audio modules, or leave
PipeWire-only? Default on or off? Fix the break-in dropped-tap window separately.

## 7. Suggested order
1. 6.1 items 1-5 and 7 (an hour at the bench).
2. 6.3 jitter run under load; if the real rms is well below ~0.5 ms the simulated result stands.
3. 6.2 build matrix.
4. 6.3 external end-to-end measurement, old vs new.
5. 6.4 documentation, then 6.5.

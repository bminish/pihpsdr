# CW Sidetone Latency Regression — commit 32832c5d

**Observed symptom:** MIDI-keyed CW at 22 WPM swallows the first dit of each transmission in
the sidetone. The dit audio is not dropped — it arrives late. Subsequent elements within the
same transmission are unaffected.

Reference: dit at 22 WPM = 54.5 ms / 2618 samples at 48 kHz.

---

## Two compounding issues in `tx_audio_write` (src/audio.c)

### 1. Forced silence pre-fill on every RX→TX transition

The new code stops ALSA with `snd_pcm_prepare()` and then writes `cw_mid_water` (960 samples,
20 ms) of explicit silence before the first CW tone:

```c
snd_pcm_rewind(rx->audio_handle, snd_pcm_rewindable(rx->audio_handle));
snd_pcm_prepare(rx->audio_handle);          // halts playback
snd_pcm_sw_params_set_start_threshold(..., cw_mid_water);
snd_pcm_writei(silence, 960);               // 20 ms of silence written ahead of tone
```

Previously the ALSA device was left **running** and `snd_pcm_rewind()` simply repositioned
the write pointer to leave 20 ms of already-playing RX audio ahead of the CW tone — no stop,
no forced silence, no restart.

The new approach adds a hard 20 ms silence gap at the start of every transmission.

### 2. `snd_pcm_rewindable()` used where `snd_pcm_delay()` is needed

The water-mark control that keeps CW sidetone latency stable was rewritten to use
`snd_pcm_rewindable()` instead of `snd_pcm_delay()`:

```c
// new — updated every 256 samples (~5.3 ms)
rx->queued = snd_pcm_rewindable(rx->audio_handle);
if (rx->queued > cw_high_water) { adjust = 0; }
if (rx->queued < cw_low_water)  { adjust = 2; }
```

`snd_pcm_rewindable()` returns only the frames still in the **software ring buffer** — it
excludes whatever is already committed to the hardware DMA/period pipeline.

With `out_latency = 200000 µs`, `snd_pcm_set_params()` negotiates a buffer of 9600 frames
(200 ms) and a period of approximately 2400 frames (50 ms). Those 2400 frames are invisible
to `snd_pcm_rewindable()`. The control loop therefore stabilises the ring buffer at
`cw_mid_water = 960` frames (20 ms) while an additional ~50 ms of audio sits in the hardware
pipeline, giving a true end-to-end sidetone latency of ~70 ms instead of the intended 20 ms.

The old code called `snd_pcm_delay()` — which includes the full hardware pipeline — every
16 zero-samples (~0.33 ms), so it measured and controlled the true latency directly. The new
update interval of 256 samples (5.3 ms) is also a 16× reduction in control bandwidth, though
the metric mismatch is the primary problem.

---

## Net effect

| Source | Approximate added latency |
|---|---|
| `snd_pcm_prepare` + 960-sample silence pre-fill | +20 ms |
| Hardware period pipeline not counted by `snd_pcm_rewindable()` | +~50 ms |
| **Total** | **~70 ms** |

A ~70 ms audio pipeline means the first dit's sidetone is still draining through hardware
when the keyer has already moved into the inter-element space. The sample data is transmitted
correctly; only the sidetone audio is displaced. This is consistent with the dit appearing
"swallowed" but not entirely absent when monitoring.

---

## Suggested correction

These two points are confined to `tx_audio_write` and should not affect the RX audio
restructuring or the other changes in 32832c5d.

1. **Pre-fill**: avoid stopping ALSA at the RX→TX boundary. Retain `snd_pcm_rewind()` to
   reposition the write pointer, but leave the device in RUNNING state so the CW tone follows
   the existing audio with no forced silence gap — matching the pre-32832c5d behaviour.

2. **Water-mark metric**: replace `snd_pcm_rewindable()` with `snd_pcm_delay()` in the
   rate-control check, and restore the per-16-sample polling cadence. `snd_pcm_rewindable()`
   is correct for the rewind call itself but measures the wrong quantity for latency control.

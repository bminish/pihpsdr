# PipeWire RX/TX Turnaround Engineering Notes

> [!WARNING]
> **DO NOT REPEAT THESE FAILED TURNAROUND BUFFERING ATTEMPTS.**
> Extensive real-time testing has proven that attempting to preserve, freeze, pre-fill, or cross-fade old receive audio buffers across TX/RX transitions introduces objectionable audio artifacts (silence gaps, pops/clicks, stuttering, and buffer starvation).

---

## Tested Approaches That Failed

### 1. Halting and Preserving Pre-TX Buffer Across Turnaround
* **Concept**: Freeze the `rx->audio_buffer` on entry to TX, hold it during TX, and play it out post-TX as a pre-fill cushion.
* **Failure Mode**: The duration of the held-back buffer ($\approx 20\text{--}40\text{ ms}$) is shorter than the combined hardware relay + WDSP turnaround latency ($\approx 60\text{--}70\text{ ms}$). Playout of the held buffer finishes before the new DSP audio arrives, creating a **20--30 ms SILENCE GAP** (`outpt == inpt`) between the old buffer and the new stream.

### 2. Deferring Playout Unpause Until First Post-TX DSP Block
* **Concept**: Keep playout paused during post-TX turnaround until `audio_write()` receives its first block, then unpause.
* **Failure Mode**: Playout unpauses after a 65 ms silence delay. Cross-fading the tail of the halted buffer produced a **silence drop/snip** or amplitude click, while discarding leading zeroes delayed playout resumption excessively.

### 3. Inserting Zero-Silence Pre-fill Cushions
* **Concept**: Pre-fill `rx->audio_buffer` with 50--200 ms of zero silence (`0.0`) on TX release.
* **Failure Mode**: PipeWire played out the zero silence cushion directly into the operator's headphones upon releasing TX, creating an audible **50--200 ms SILENCE DROP**.

---

## Approved Standard Strategy: Clean Muting & Zeroing Buffer on TX

Future PipeWire audio modifications must revert to the simple, clean strategy:
1. **On Entry to TX**: Immediately set `rx->cwaudio = 3`, mute receive playout in `on_playback_process()`, and reset/zero buffer pointers (`rx->audio_buffer_inpt = 0; rx->audio_buffer_outpt = 0;`).
2. **During TX**: Sidetone plays out directly via `h->st_buffer`.
3. **On Exit to TX**: Reset buffer pointers on first post-TX block (`rx->cwaudio = 0`) and feed fresh live DSP samples into the ring buffer cleanly.

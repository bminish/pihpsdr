# Engineering Report: Native Single-Stream Mixed PipeWire Audio Backend Port for pihpsdr

This report provides a detailed analysis of the native PipeWire audio backend implementation in `pihpsdr`, stashed as a clean patch file against `origin/master`.

---

## 1. Patch Overview & Design

The patch implements a native, low-latency, single-stream mixed PipeWire playback backend (`src/pipewire.c`) that replaces the legacy PulseAudio simple API wrapping.

### Key Features
1.  **Unified Playback Stream:** Exposes a single output stream `"pihpsdr-rx"` (internally `h->playback_stream`) to the operating system.
2.  **Internal Software Mixer:** Instead of creating two separate streams, we mix the receiver audio (`rx->audio_buffer`) and the CW sidetone (`h->sidetone_ring`) internally inside the stream's process callback:
    $$\text{samples}[i] = \text{rx\_sample} + \text{sidetone\_sample}$$
3.  **GTK Dropdown Configuration:** A "PipeWire Quantum" combo box is added to the RX properties menu (only when compiled with `PIPEWIRE`), allowing the operator to dynamically select quantums from `16 (0.3ms)` up to `2048 (42.7ms)`. When changed, it saves the value to properties and restarts the stream on the fly.
4.  **Zero-Delay Routing Transitions:** The stream remains active at all times. Transitioning RX/TX takes 0ms because we never pause, resume, or flush the PipeWire stream (we only reset sidetone ring pointers). In simplex mode, the RX path is muted internally during keying.

---

## 2. Latency & Jitter Analysis

*   **Sidetone Latency:** By setting the stream quantum to `64` frames, we request direct scheduling from PipeWire's real-time thread. Sidetone samples are copied directly from the keyer to the ring buffer and played out with a target latency of **1.3 ms**, satisfying the requirements for real-time manual CW keying.
*   **Buffer Decoupling (Jitter Shielding):** 
    *   Receiver (RX) audio is buffered in a deep software queue (`rx->audio_buffer`, 9600 frames / 200 ms).
    *   This shields the DSP thread from the 1.3 ms real-time scheduling constraints of the audio hardware. The DSP thread can process samples in large blocks and sleep, while the PipeWire playback callback smoothly reads 64 frames at a time.
*   **Phase Continuity:** Because there is no rate-control, sample-skipping, or watermark-matching logic on the sidetone path, there are zero phase jumps or timing jitter in the generated CW elements.

---

## 3. Design Evolution: What Worked vs. What Failed

### What Failed

1.  **PulseAudio Stream Recreation:** Freeing and allocating a new stream on every RX $\leftrightarrow$ TX transition to swap latency profiles caused massive delays (20ms - 100ms) due to WirePlumber connection negotiation, cutting off the first parts of CW characters.
2.  **Single Stream with Adaptive Rate Control:** Trying to dynamically change the size of the buffer on a single stream by inserting or deleting samples introduced timing jitter and phase discontinuities, making the keying feel "muddy."
3.  **Dual-Stream Mismatched Quantums:** Running two separate PipeWire streams (one at 2048 quantum and one at 64 quantum) connected to the same sink caused PipeWire's rate-matcher to glitch the RX stream. Because the DSP thread runs in bursts, forcing the RX stream to run at a 64-frame scheduling interval caused constant buffer underruns and choppy, distorted audio.
4.  **Bouncing Transition Machine:** Running the full RX $\rightarrow$ TX $\rightarrow$ RX transition logic (clearing sidetone buffers and pre-filling RX silence) on every block while zero-beating or in duplex mode caused the buffers to wipe continuously. This created a buzzy hum at a frequency aligned with the quantum size (750 Hz buzz at 64 quantum).

### What Worked

1.  **Single Stream running at a constant low quantum** (64 or 128 frames) with internal software mixing.
2.  **Bypassing the transition state machine** (resets and silence injection) when simplex break-in is not active. This keeps both paths continuously running and mixed during duplex or zero-beating.

---

## 4. Bugs Found & Fixed

1.  **Stereo Buffer Overflow:** The internal sidetone ring buffer `buffer` array was sized `RING_BUFFER_SIZE` (65,536 doubles). In stereo, indices of the form `inpt * channels + c` overflowed this limit, corrupting write pointers and crashing the application. Resolved by expanding the buffer array to `RING_BUFFER_SIZE * 2` (131,072 doubles).
2.  **GTK Grid Overlap:** The row index was not incremented after the FM Volume Limiter spin button, causing our new dropdown to overlap with it. Resolved by adding `row++`.
3.  **VFO Mode Bounds checking (Crash):** Loading corrupted properties files containing an out-of-bounds mode (e.g. `mode = 12` which is equal to `MODES`) caused immediately crashing inside `rx_set_filter` due to out-of-bounds dereferencing in the filter table. We stashed the fix as a separate patch [docs/vfo_bounds_check.patch](file:///home/bminish/sdr/bm-pihpsdr/docs/vfo_bounds_check.patch).

---

## 5. Code Footprint

The patch touches the following files:
*   [Makefile](file:///home/bminish/sdr/bm-pihpsdr/Makefile): Adds `-DPIPEWIRE` and PipeWire pkg-config libraries to the build path.
*   [src/receiver.h](file:///home/bminish/sdr/bm-pihpsdr/src/receiver.h): Adds `latency` field and `audio_handle` to `RECEIVER` for PipeWire.
*   [src/receiver.c](file:///home/bminish/sdr/bm-pihpsdr/src/receiver.c): Initializes, saves, and restores the `latency` setting.
*   [src/rx_menu.c](file:///home/bminish/sdr/bm-pihpsdr/src/rx_menu.c): Exposes the combo box dropdown UI to configure quantum and handles the restart callback.
*   [src/transmitter.c](file:///home/bminish/sdr/bm-pihpsdr/src/transmitter.c): Enables sidetone playout when keying even if RF transmission is disabled.
*   [src/pipewire.c](file:///home/bminish/sdr/bm-pihpsdr/src/pipewire.c): The core PipeWire native backend containing the stream loop, single playback stream, internal mixing callback, and duplex/zero-beat transition routing.

---

## 6. Regression Assessment

*   **Impact on ALSA / PulseAudio / PortAudio:** Zero. The PipeWire backend is conditionally compiled under `#ifdef PIPEWIRE`. All structure changes in `receiver.h` are stashed behind `#if !defined(PORTAUDIO) && !defined(PULSEAUDIO) && !defined(ALSA) && defined(PIPEWIRE)`, leaving other backends completely untouched.
*   **Latency-induced CPU Overrun:** If the user sets the quantum size extremely low (e.g. `16` or `32` frames) on low-spec hardware (like old Raspberry Pi models), the rapid callback frequency might cause CPU overruns/crackling. However, this is fully configurable on the fly via the GTK dropdown menu, allowing users to choose the optimal latency profile for their hardware.

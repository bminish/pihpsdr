# Decoupled Sidetone and Delayed RF Keying: Failure Analysis

This document details the goals, implementation attempts, failure modes, and architectural lessons learned during the effort to decouple software sidetone playout from PTT transition states in `pihpsdr`.

---

## 1. Project Goal

The primary objective was to reduce CW keying latency for operators keying via local MIDI controllers:
*   **Immediate Sidetone:** Play software sidetone instantly upon receiving a MIDI event, bypassing the RX $\rightarrow$ TX transition latency.
*   **Delayed RF Carrier:** Delay the start of the transmitted RF carrier by the dynamically measured PTT transition delay ($15\text{--}30\text{ ms}$) plus a $5\text{ ms}$ safety margin. This ensures the radio has completed its physical transition to TX before RF samples are delivered.
*   **Envelope Preservation:** Shift the entire RF envelope in time relative to the sidetone, keeping exact dot/dash durations intact.

---

## 2. Implementation Strategies Attempted

### Phase 1: Sample-Counter Delay Gating
*   **Method:** Separate the shared read pointer `cw_ring_outpt` into `cw_ring_outpt_sidetone` and `cw_ring_outpt_rf`. Gate the RF execution in `next_cw_sidetone_sample()` by adding `rf_delay_samples` to the `wait_time` of the first key-down event.
*   **Failure:** `tx_add_mic_sample` (where the RF carrier is generated) only runs when the radio streams mic packets (during active TX). Since no mic packets arrive during the PTT transition, the sample counter did not increment. The delay-gating condition was bypassed, causing the first dit to be swallowed.

### Phase 2: Gating Flags & Reset Isolation
*   **Method:** Include transition flags (`rf_delay_needed`) in the RF queue gating condition to prevent the queue from being cleared. Stop resetting the shared `cw_ring_inpt` write pointer, and only align the individual read pointers.
*   **Failure:** While this preserved the queue during transition, the sidetone thread and the RF thread remained coupled through the shared shaper index pointers (`cw_ramp_audio_ptr` and `cw_ramp_rf_ptr`) and the shared phase variables (`p1local`/`p2local`). Concurrently running the two threads corrupted the shaper state, leading to skipped key events.

### Phase 3: Monotonic Clock Scheduling & Mutex Separation
*   **Method:** Timestamp every event with `clock_gettime(CLOCK_MONOTONIC)` on queue entry. Delay execution in the RF path by checking if the monotonic wall-clock time exceeded `arrival_time + delay`. Separate the `cw_ramp_mutex` into audio and RF mutexes to stop thread collision.
*   **Failure:** The sidetone and RF paths still conflicted in their local audio writing routines. The concurrent writing of sidetone samples by the sidetone thread and zero-samples by the RF thread into the unified PipeWire playback ring buffer caused sample overwriting and corruption, completely silencing sidetone and RF.

---

## 3. Core Design Errors & Regressions

1.  **High-Frequency Mutex Contention:** `next_cw_sidetone_sample` is called hundreds of times per second. Accessing a shared resource (like the envelope shaper state) using try-locks from two concurrent, asynchronous threads caused massive lock collisions. Skipped locks led to discarded samples, killing sidetone and RF playout, and corrupting the TUNE confidence tone.
2.  **Clock-to-Sample Desynchronization:** Morse code requires sample-accurate timing. Attempting to schedule playout using monotonic system clock timestamps (`now >= target_time`) across two threads running at different rates (48 kHz mic samples vs. DUC IQ samples) broke the sample-level alignment, leading to timing instability.
3.  **Local Audio Buffer Overwrites:** Both threads calling `tx_audio_write` concurrently corrupted the active receiver's audio buffer indices, leading to silent playout.
4.  **Implicit remote-mode assumptions:** `client_sidetone_thread` was designed exclusively for remote-client architectures. Trying to force it to run in local desktop mode introduced resource initialization loops and double-allocation bugs.

---

## 4. Architectural Lessons Learned

*   **Synchronous Playout is Mandatory:** For stable Morse code keying, sidetone and RF carrier generation must remain synchronized within the same DSP processing block. Introducing asynchronous timing loops breaks envelope shape integrity.
*   **Need for a Complete Rewrite:** Decoupling sidetone from PTT in a multi-threaded environment cannot be solved with local changes to queue pointers. It requires a complete redesign of the keyer state machine and shaper variables to fully isolate the audio and RF pipelines.

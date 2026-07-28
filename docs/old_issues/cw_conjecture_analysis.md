# Decoupled CW Keyer Queue and TX Buffer Delay: Conjecture Analysis

This document provides a technical analysis of the conjecture to decouple the software keyer/MIDI input entirely from the transceiver's TX state, queueing events immediately, and playing them out to the RF path only when the transmitter's buffer has reached $1/2$ capacity.

---

## 1. Context & Motivation

In traditional SDR architectures, sidetone playout is synchronized with the actual RF transmission. While this ensures sidetone and RF are perfectly aligned, it introduces a major usability regression: the operator hears the sidetone only *after* the hardware has completed its RX $\rightarrow$ TX transition (typically $15\text{--}35\text{ ms}$).

The proposed conjecture aims to:
1.  **Play sidetone instantly** (< 2 ms) upon receiving a MIDI event.
2.  **Queue the CW events** (key-down, key-up) in a ring buffer.
3.  **Assert PTT immediately** to start the transition.
4.  **Wait for the TX buffer to reach $1/2$ capacity** (~40 ms of samples).
5.  **Start playing out the queued CW events** to the RF path, lagging the sidetone by a constant offset $D$.

---

## 2. The "1/2 Filled TX Buffer" Design Constraint

### Why wait until the buffer is $1/2$ filled?
The HPSDR protocol streams TX IQ samples to the radio FPGA over UDP. To prevent network jitter and host scheduling latency from causing transmission dropouts, both the host software and the FPGA employ FIFO buffers:
*   **FPGA FIFO:** A hardware buffer on the radio. If the FIFO empties, the transmitter underflows, causing audible clicks, clicks in adjacent bands, and carrier dropouts.
*   **Clock Synchronization:** The FPGA pulls samples from the FIFO at a fixed rate (e.g., 192 kHz or 48 kHz). The host must stream samples at exactly the same rate.
*   **The Threshold:** Waiting until the TX buffer is $1/2$ filled (approx. $20\text{--}40\text{ ms}$ of buffer depth) ensures a stable, deep enough safety margin. If the host experiences a temporary scheduling delay (e.g., GUI thread lockup), the FPGA can continue transmitting from the buffer without underflowing.

Thus, the $1/2$-filled wait is a **fundamental physical constraint** of network-streamed SDR transmitters.

---

## 3. Implications of the Decoupled Queue Playout

If we decouple the keyer and start playing out CW elements from the queue only after the TX buffer is $1/2$ full, we introduce a constant temporal shift ($D$) between the sidetone and the RF carrier:
$$\text{RF Playout Time} = \text{Sidetone Playout Time} + D$$

### A. Queue Overrun / Underrun Management
*   **Queue Capacity:** The CW event queue (`cw_ring`) has a size of 1024 slots. At 22 WPM, a single dit/dah sequence takes around $50\text{--}150\text{ ms}$. A delay of $40\text{ ms}$ represents less than 2 events in the queue. Even at extreme speeds (e.g., 60 WPM), the queue occupancy will never exceed 4 or 5 events. Therefore, there is **zero risk of queue overrun**.
*   **Clock Drift:** The sidetone thread processes the queue using the host's real-time clock. The RF thread processes the queue using the TX sample clock (DAC rate). While these clocks drift slightly (parts-per-million), over a typical CW transmission sequence (minutes), the drift is less than a microsecond. Thus, **no underruns due to clock drift** will occur.

### B. PTT / Mox Hang Time (VOX) Management
In a break-in configuration, the transmitter must hold the PTT active until the CW transmission finishes.
*   **The Delay Offset:** Because the RF carrier is delayed by $D$ ms, the RF carrier will continue to play for $D$ ms *after* the operator has finished keying and the sidetone has gone silent.
*   **PTT Release Gating:** The host must **not** release PTT immediately when the user stops keying. PTT release must be gated by:
    $$\text{PTT Release Time} = \text{Sidetone End Time} + D + \text{Break-in Delay}$$
    If PTT is released too early, the final tail of the delayed RF carrier will be cut off (character truncation).

---

## 4. Wider Architectural Implications

### 1. Separation of Thread Contexts
Because the sidetone plays immediately and the RF plays delayed, the two pipelines must be completely isolated:
*   **Read Pointers:** Separate `cw_ring_outpt_sidetone` and `cw_ring_outpt_rf` are mandatory.
*   **State Variables:** Separate `keydown_sidetone` and `keydown_rf` must track key states independently.
*   **Lock Isolation:** The shaper parameters and sine wave phase generators must be protected by separate mutexes (`cw_ramp_mutex` and `cw_ramp_audio_mutex`) to prevent thread contention from causing choppy audio or skipped dits.

### 2. TUNE and Voice Modes
*   **TUNE Mode:** TUNE does not use the keyer queue. It requires immediate, continuous carrier generation. Therefore, TUNE must bypass the event queue and assert PTT/RF immediately.
*   **Voice/Digital Modes:** Decoupled queueing only applies to CW mode. Other modes must continue to stream audio directly into the TX buffer without the CW event delay.

---

## 5. Conclusion

The conjecture is **architecturally sound and highly viable**. By treating the PTT transition and buffer filling as a constant time shift ($D$) applied only to the RF queue reader, we can:
1.  Provide the operator with immediate, zero-latency sidetone feedback.
2.  Maintain the exact, original envelope and timing of the CW characters.
3.  Protect the transmitter from network-induced FIFO underflows.

The implementation requires careful thread separation, separate mutexes, and scaling the break-in hang time (`cwvox`) by the delay offset $D$ to prevent trailing character truncation.

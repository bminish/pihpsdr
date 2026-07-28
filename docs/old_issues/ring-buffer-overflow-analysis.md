# Ring Buffer Overflow Analysis — piHPSDR new_protocol

**Date:** 2026-06-27  
**Branch:** `test-buffers` (diagnostic patches applied)  
**Base branch:** `master` (commit `57175aa9`)  
**File modified:** `src/new_protocol.c`

---

## Background

Intermittent ring buffer overflow messages observed in the log, consistently
appearing around TCI TUNE events and radio sequence resets:

```
iq_thread: DDC(0) sequence error: expected 1089l got 0l
new_protocol_audio_samples: ring buffer overflow          ← repeating every ~88ms
TCI0 TUNE request=1
saturn_post_micaudio: ring buffer overflow.               ← burst during/after TUNE
saturn_post_iq_data: DDC(0) ring buffer overflow.
get_my_buffer: number of buffers increased to 384/448/512/576/640
```

The buffer pool growth to exactly 640 = 512 (IQ ring) + 64 (mic ring) + 64 (high-priority ring)
is significant: it indicates all three ring buffers were simultaneously full, meaning all three
consumer threads were stalled at the same time.

---

## Architecture: The Three Ring Buffer Systems

All data flows through a single UDP receive thread (`new_protocol_thread`) that dispatches
packets into three rings. Each ring has a dedicated consumer thread.

### 1. IQ receive ring — `iq_buffer[ddc][]`

| Property | Value |
|----------|-------|
| Size | `RXIQRINGBUFLEN = 512` slots |
| Producer | `saturn_post_iq_data()` — called from `new_protocol_thread` for every `RX_IQ_TO_HOST_PORT` packet |
| Consumer | `iq_thread` (per DDC) → `process_iq_data()` → `rx_add_iq_samples()` → WDSP RX chain |
| Overflow recovery | Drops packet, skips 128 incoming buffers (`iq_count[ddc] = -128`) |

### 2. Mic audio ring — `mic_line_buffer[]`

| Property | Value |
|----------|-------|
| Size | `MICRINGBUFLEN = 64` slots (~85 ms at 48 kHz, one packet per 1333 µs) |
| Producer | `saturn_post_micaudio()` — called from `new_protocol_thread` for every `MIC_LINE_TO_HOST_PORT` packet |
| Consumer | `mic_line_thread` → `process_mic_data()` → `tx_add_mic_sample()` → WDSP TX chain |
| Overflow recovery | Drops packet, skips 16 incoming buffers (`mic_count = -16`, ~21 ms) |

### 3. RX audio ring — `RXAUDIORINGBUF`

| Property | Value |
|----------|-------|
| Size | `RXAUDIORINGBUFLEN = 16384` bytes = 64 chunks of 256 bytes (~85 ms at 48 kHz) |
| Producer (RX) | `new_protocol_audio_samples()` — called from `receiver.c` RX DSP output loop |
| Producer (TX) | `new_protocol_tx_audio_samples()` — called from `tx_add_mic_sample()` for sidetone/audiomonitor |
| Consumer | `new_protocol_rxaudio_thread` → `sendto()` UDP audio back to radio |
| Overflow recovery | Drops 64 samples, skips 4096 samples (`rxaudio_count = -4096`, ~85 ms) |

### Buffer pool

All network receive buffers come from a pedestrian pool (`get_my_buffer()`). The pool
grows in batches of 64 when exhausted. A growing pool indicates consumers are behind —
buffers are allocated faster than they are freed.

---

## Root Cause Hypothesis

### Primary trigger: radio sequence reset

Both observed sessions begin with:
```
iq_thread: DDC(0) sequence error: expected NNNl got 0l
```
"Got 0" means the radio FPGA restarted and reset its sequence counter. This causes a
packet burst from sequence 0, stressing all consumer threads simultaneously.

### Why `new_protocol_audio_samples` overflows during RX

After the sequence reset, `new_protocol_rxaudio_thread` is sending audio UDP packets to
a radio in mid-restart. The radio likely isn't accepting audio yet. The adaptive pacing
logic (FIFO estimator, `new_protocol.c:1863–1901`) can drive sleep times up to 1000 µs
per chunk. With 64 chunks in the ring, worst-case drain is 64 ms. If the radio restart
takes longer, the ring fills.

The `~88 ms` repeat interval in the log is the recovery skip period: `rxaudio_count = -4096`
skips 4096 samples at 48 kHz = exactly 85 ms. The ring fills again immediately after
recovery, meaning the consumer was making no progress at all during this window.

### Why `saturn_post_micaudio` overflows during/after TCI TUNE

This is the central hypothesis requiring confirmation.

#### The drain-wait mechanism

`new_protocol_tx_audio_samples()` (`new_protocol.c:2750–2764`) contains a blocking
spin-wait that runs **on `mic_line_thread`**:

```c
if (!rxaudio_flag) {
    rxaudio_drain = 1;
    while (rxaudio_inptr != rxaudio_outptr) { usleep(1000); }  // BLOCKS mic_line_thread
    rxaudio_drain = 0;
    rxaudio_flag = 1;
}
```

This fires **once per RX→TX transition**, the first time TX audio is generated. Its
purpose is to flush stale RX audio from the ring before starting sidetone, minimising
CW sidetone latency.

While `mic_line_thread` is blocked here, the radio keeps sending mic audio packets at
750 Hz (every 1333 µs). The mic ring has 64 slots = ~85 ms of capacity. If the drain
takes ≥ 85 ms, the mic ring overflows.

#### When does the drain-wait trigger?

`new_protocol_tx_audio_samples()` is only called when `did_tx_audio = 1` in
`tx_add_mic_sample()` (`transmitter.c:2043–2061`). `did_tx_audio` is set when either:

- `can_tx_audio && transmitter->audiomonitor` — audio monitor is on, OR
- `can_tx_audio && tx->tune && tx->swrtune` — SWR tone is enabled during TUNE

If **neither** is enabled, `new_protocol_tx_audio_samples()` is never called in TUNE
mode, the drain-wait never runs, and the mic ring cannot overflow via this path.

**Note:** In TUNE mode, the mic sample is zeroed at `transmitter.c:1922` regardless:
```c
if (tx->tune || txmode == modeCWL || txmode == modeCWU) {
    mic_sample = 0.0;
}
```
The mic ring IS consumed during TUNE — the audio is just discarded. The overflow is
caused by the consumer being blocked in the drain-wait, not by the consumer being
permanently absent.

#### Why the drain takes longer in this scenario

By the time TCI TUNE fires at `10:58:24`, the `new_protocol_rxaudio_thread` consumer has
already been struggling for 16 seconds (RXAUDIO overflows since `10:58:07`). When the
drain-wait starts on `mic_line_thread`, it depends on this already-degraded consumer to
drain the ring. The result is a long drain, mic ring overflows immediately.

### The deadlock scenario (unconfirmed, more severe)

If `new_protocol_rxaudio_thread` exits (`P2running = 0`, set when `sendto()` to the radio
fails — `new_protocol.c:1904–1906`) while `mic_line_thread` is inside the drain-wait loop,
`mic_line_thread` is **permanently blocked**. Nobody drains `RXAUDIORINGBUF`, the condition
`rxaudio_inptr != rxaudio_outptr` is never satisfied, and the thread never wakes up.

In this state:
- `saturn_post_micaudio()` checks `!P2running` and immediately frees incoming buffers
- No overflow log messages appear
- The mic consumer is silently dead until a full protocol restart

---

## Diagnostic Patches (branch `test-buffers`, commit `8f6128df`)

Three log points added to `src/new_protocol.c`. To revert: `git checkout master`.

### Patch 1 — `saturn_post_micaudio` overflow (`~line 2228`)

**What it logs:**
```
saturn_post_micaudio: ring buffer overflow (tune=1 swrtune=0 audiomonitor=1
    rxaudio_drain=1 rxaudio_fill=48/64 chunks).
```

**How to read it:**
- `tune` — confirms TUNE was active at overflow time
- `swrtune` / `audiomonitor` — identifies which condition triggered the drain-wait; if **both are 0**, the drain path is impossible and something else is stalling the mic consumer
- `rxaudio_drain` — **if 1, mic_line_thread was blocked in the drain loop at this exact moment** — this is the smoking gun
- `rxaudio_fill` — how full the audio ring was; a large value with `drain=1` means the audio consumer is genuinely slow

**Diff:**
```diff
   } else {
-    t_print("%s: ring buffer overflow.\n", __func__);
+    int rxaudio_fill = (rxaudio_inptr - rxaudio_outptr + RXAUDIORINGBUFLEN) % RXAUDIORINGBUFLEN;
+    t_print("%s: ring buffer overflow (tune=%d swrtune=%d audiomonitor=%d rxaudio_drain=%d rxaudio_fill=%d/%d chunks).\n",
+            __func__,
+            transmitter->tune,
+            transmitter->swrtune,
+            transmitter->audiomonitor,
+            rxaudio_drain,
+            rxaudio_fill / 256,
+            RXAUDIORINGBUFLEN / 256);
     mybuf->free = 1;
     mic_count = -16;
   }
```

### Patch 2 — drain-wait timing (`~line 2771`)

**What it logs:**
```
new_protocol_tx_audio_samples: starting rxaudio drain on mic_line_thread (fill=48/64 chunks).
new_protocol_tx_audio_samples: rxaudio drain complete (23 ms).
```

**How to read it:**
- The start message fires on the first TX audio sample after every RX→TX transition
- `fill` at start — how much stale audio was in the ring when drain began
- `drain_ms` — **how long mic_line_thread was blocked**; mic ring capacity is ~85 ms, so values above ~70 ms indicate the overflow is caused by this drain

**Diff:**
```diff
+    struct timespec drain_t0, drain_t1;
+    int drain_fill = (rxaudio_inptr - rxaudio_outptr + RXAUDIORINGBUFLEN) % RXAUDIORINGBUFLEN;
+    clock_gettime(CLOCK_MONOTONIC, &drain_t0);
+    t_print("%s: starting rxaudio drain on mic_line_thread (fill=%d/%d chunks).\n",
+            __func__, drain_fill / 256, RXAUDIORINGBUFLEN / 256);
     rxaudio_drain = 1;
     while (rxaudio_inptr != rxaudio_outptr) { usleep(1000); }
     rxaudio_drain = 0;
     rxaudio_flag = 1;
+    clock_gettime(CLOCK_MONOTONIC, &drain_t1);
+    long drain_ms = (drain_t1.tv_sec - drain_t0.tv_sec) * 1000
+                    + (drain_t1.tv_nsec - drain_t0.tv_nsec) / 1000000;
+    t_print("%s: rxaudio drain complete (%ld ms).\n", __func__, drain_ms);
```

### Patch 3 — `new_protocol_rxaudio_thread` exit (`~line 1915`)

**What it logs:**
```
new_protocol_rxaudio_thread: exiting with 14 unsent chunks in ring -- mic_line_thread drain loop will deadlock.
```
or
```
new_protocol_rxaudio_thread: exiting with ring empty.
```

**How to read it:**
- Fires on both exit paths: external `P2running=0` (protocol shutdown) and `sendto()` failure
- The deadlock warning means any in-progress or future drain-wait on `mic_line_thread` will hang permanently; recovery requires a full protocol restart

**Diff:**
```diff
+  if (rxaudio_inptr != rxaudio_outptr) {
+    t_print("%s: exiting with %d unsent chunks in ring -- mic_line_thread drain loop will deadlock.\n",
+            __func__,
+            (rxaudio_inptr - rxaudio_outptr + RXAUDIORINGBUFLEN) % RXAUDIORINGBUFLEN / 256);
+  } else {
+    t_print("%s: exiting with ring empty.\n", __func__);
+  }
+
   return NULL;
```

---

## How to Revert

```bash
# Return to unmodified master
git checkout master

# Or, to keep the branch but undo changes in working tree
git checkout master src/new_protocol.c
```

The `test-buffers` branch remains intact for reference or further work.

---

## What to Look For in the Next Log Session

### Confirming the drain-wait hypothesis

The log sequence should look like this if the hypothesis is correct:

```
new_protocol_tx_audio_samples: starting rxaudio drain on mic_line_thread (fill=NN/64 chunks).
saturn_post_micaudio: ring buffer overflow (tune=1 swrtune=? audiomonitor=? rxaudio_drain=1 ...)
saturn_post_micaudio: ring buffer overflow (tune=1 ... rxaudio_drain=1 ...)
... [repeating while drain is in progress] ...
new_protocol_tx_audio_samples: rxaudio drain complete (NN ms).
```

The key confirmation signals:
1. `rxaudio_drain=1` in the overflow messages — mic_line_thread was blocked in the drain loop
2. Overflow messages stop shortly after the drain-complete message
3. `drain_ms` ≥ 70 ms (close to the 85 ms mic ring capacity)
4. At least one of `swrtune=1` or `audiomonitor=1` — otherwise this path cannot trigger

### Ruling out the drain-wait hypothesis

If overflows show `rxaudio_drain=0`, the drain-wait is not the cause. Look then at:
- Whether `tune=0` — overflow during RX, not TX (separate issue)
- Whether WDSP processing in `tx_full_buffer()` is taking unusually long

### Detecting the deadlock

If the program goes silent (no more mic overflow messages) after a `sendto` failure, check
whether `new_protocol_rxaudio_thread: exiting with N unsent chunks` appeared in the log
before the silence. If so, `mic_line_thread` is permanently blocked.

---

## Potential Follow-On Fixes (not yet implemented)

These are listed for reference — do not implement until diagnosis is confirmed.

1. **Break the drain dependency** — if neither `swrtune` nor `audiomonitor` is enabled
   during TUNE, skip `new_protocol_tx_audio_samples()` entirely. The mic sample is already
   zeroed at `tx_add_mic_sample:1922`, so no sidetone audio is generated anyway.

2. **Add a drain timeout** — replace the unbounded spin-wait with a timeout:
   ```c
   // bail out after 60 ms rather than blocking forever
   for (int t = 0; t < 60 && rxaudio_inptr != rxaudio_outptr; t++) { usleep(1000); }
   ```
   This bounds the `mic_line_thread` block time below the 85 ms overflow threshold.

3. **Fix the deadlock** — if `new_protocol_rxaudio_thread` exits while the ring is non-empty,
   set `rxaudio_inptr = rxaudio_outptr` so any in-progress drain-wait unblocks immediately.

4. **Fix the `rxaudio_drain` data race** — `rxaudio_drain` is a plain `int` with no memory
   barrier between the write on `mic_line_thread` and the read on `new_protocol_rxaudio_thread`.
   Mark it `volatile` or use `g_atomic_int_set/get`.

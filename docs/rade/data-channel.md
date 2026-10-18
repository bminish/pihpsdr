# RADE V2 data-symbol channel: what is sent, and what else it could carry

Status: **exploration, nothing implemented** (2026-10-18). Written from `src/rade_v2_text.[ch]` and the engine call sites in
`src/rade_native.c`. The freedv-gui V2 receiver source is not on this machine; what is said here about stock freedv-gui is
what our port of it does, which `test/rade_native/v2_text_interop` checks against the real code.

## What goes out today

- One BPSK symbol (+1.0 / -1.0) per modem frame, 25 per second, set with `rade_tx_set_data_symbol()` before each `rade_tx()`.
  The channel is always present while the over lasts.
- The callsign is a repeating stream of LDPC(112,56) codewords, one per block of 8 characters (4.5 s each), bits interleaved
  at stride 37. Payload (56 bits): CRC-8, last-block flag, 2-bit block number, 8 characters in base 38 (42 bits), 3 reserved zeros.
- 1-4 blocks (up to 32 characters, 0-9 A-Z /), then round again. A callsign of up to 8 characters is one block, repeated every 4.5 s.

## Can the channel carry something else without breaking callsign decodes?

Yes, on two conditions.

1. **The receiver does not need a fixed cycle to find the callsign.** After the first 112 symbols (the rotation sweep, which
   only speeds up lock-on for a one-block call) the receiver tries every new symbol as the end of a codeword. A callsign
   block decodes wherever it sits in the stream, so `call, X, call, Y, ...` works. A window that is not a codeword fails the
   56 parity checks (about 2^-56 to pass by chance).
2. **Our own blocks must fail the stock CRC check every time.** The same LDPC code with arbitrary payload would pass the
   stock CRC-8 about 1 time in 256, and the receiver would take it for a text block with a garbage block number / last flag
   (a block 0 wipes the assembled callsign). Fix: put `crc8(fields) ^ 0xA5` in the CRC field. It can never equal the stock
   CRC, so stock receivers reject the block with certainty; our receiver checks for the inverted value. The payload stays 56 bits.

## What it costs

- **Callsign latency.** `[call][X]` brings the callsign round every 9 s instead of 4.5 s. Fine on a clean signal.
- **Weak-signal combining.** The receiver adds repeats in accumulators, one per cycle length of 1-4 blocks, and needs 2 full
  periods before it uses them: 18 s for a 2-block cycle against 9 s now. The cycle must stay at 4 blocks or fewer.
  - A fixed `[call][X]` super-frame combines both blocks.
  - If X changes every cycle, the call slot still combines coherently and the X slot only adds noise.
  - So X is decodable per block, without combining: a **good-SNR channel**. How good is **not measured**; the callsign itself
    has been seen to decode at 6 dB SNR in the engine test, nothing more.
- `call, X, call, Y` with different X and Y has a 4-block period (18 s): each call slot combines only every 18 s. `[call][X]`
  is better when X is the only extra.
- **Only while locked.** Symbols arrive only during lock. After a drop-out the rejoin logic (`rade_v2_text_rx_feed`) correlates
  the first 96 symbols with what was collected, using only positions both have; a changing X slot weakens that evidence a little.
  **Not measured.**

## A route to avoid: data hidden after a null

The stock receiver ends the callsign at the first null character, so `EI6IZ\0...` in block 0 followed by ordinary blocks 1-3
would be ignored by it: about 126 hidden bits per cycle, valid for any receiver. But a stock receiver delivers a callsign only
after every block up to the last-flag block has arrived, so the callsign would take up to 18 s to appear instead of 4.5 s.
The inverted-CRC blocks keep the callsign a one-block message.

## Candidate uses (about 56 bits per extra block, roughly one block per 9 s)

- Maidenhead grid, 6 characters in the same alphabet (the obvious first use; the Reporter could show it).
- The SNR or quality we measure of the station we are receiving (a report).
- Transmit power or profile.
- A sequence number, so receivers can count dropped frames.
- A callsign hash, or a short message.

## If this is built

First step: `rade_v2_text` alternates the call block with an inverted-CRC block; a host test shows the existing interop test
still decodes the callsign from that stream, and a mock stock receiver never accepts an X block over many random payloads.
Open question: what X carries first.

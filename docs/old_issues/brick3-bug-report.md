# Bug report: TX relay/PTT never releases after TX unless a Diversity-style RX command is sent

**Hardware:** Brick3 (ANAN-100D clone)
**Reported firmware/ID string:** `Device Angelia, Protocol2, v12.1`
**Config:** "New PA board" enabled, filter board = ALEX

## Symptom

After any TX cycle (MOX, Tune, or PureSignal-feedback TX — trigger doesn't
matter), the radio's TX relay/PTT output stays asserted indefinitely once
the host returns to RX. The receive front end stays disconnected from the
antenna (relay still routed for TX), so no real signal is ever received,
even though the host (pihpsdr) has correctly cleared MOX and is sending a
valid RX configuration. Only power-cycling the radio recovers it — **or**
enabling Diversity mode, which immediately and reliably releases the relay.

This occurs regardless of: number of active receivers (1 or 2), receiver
sample rate, PureSignal enabled/disabled, and whether TX was triggered via
MOX or Tune. It is NOT fixed by sending any other valid RX reconfiguration
(tested: toggling which ADC a receiver uses, adding/removing a second
receiver) — only Diversity fixes it.

**Caveat (added 2026-06-30):** we have no way to directly observe the
physical relay/PTT line (no scope, meter, or confirmed visible indicator).
Every "relay released / still stuck" judgment in this report, including
the table below, is inferred from RX audio/signal presence. See "Update
2026-06-30" for where this proxy is and isn't trustworthy.

## What we found by packet-capturing the Receive-Specific stream (UDP port 1025, host→radio)

Across three independent test captures, the `receive_specific` packet has
two relevant fields:

- **byte 7** — DDC-enable bitmask
- **byte 1363** — "sync DDC1 to DDC0" flag (set to `0x02` by both the
  PureSignal-feedback and Diversity code paths)

The pattern is identical in every test:

| Phase | byte7 | byte1363 | Relay releases? |
|---|---|---|---|
| Normal RX before TX (DDC2/DDC3, any ADC mapping) | `0x04`/`0x0C` | `0x00` | n/a |
| TX (MOX/Tune, PS feedback active) | `0x01` (DDC0) | `0x02` | n/a (TX) |
| Normal RX after TX — **stuck** | `0x04`/`0x0C` (same as before TX) | `0x00` | **No** |
| ADC toggled on RX1 (different `alex0` value, confirmed via High-Priority packet) | `0x04` | `0x00` | **No** |
| Receiver added/removed | `0x0C`/`0x04` | `0x00` | **No** |
| **Diversity enabled** | `0x01` (DDC0) | **`0x02`** | **Yes, immediately** |

The only configuration that ever releases the relay is a `receive_specific`
packet with **DDC0 enabled and byte 1363 = `0x02`, sent while not
transmitting**. PureSignal also sends this same byte-7/byte-1363 combination
— but only while transmitting, never while in RX, so it never gets a chance
to trigger the release. Plain ADC-selection or receiver-count changes never
touch these two fields at all, and reliably fail to release the relay no
matter how different the rest of the packet is (we confirmed the ALEX
relay-control bits in the High-Priority packet genuinely changed value
during the ADC toggle test, and it still didn't help).

## Appendix: relevant bytes from the actual Receive-Specific packets sent

All three packets below are from the same single-receiver MOX test
(`receive_specific`, UDP port 1025, host → radio). Byte numbering is
0-indexed from the start of the UDP payload. Byte 7 is the DDC-enable
bitmask (bit0=DDC0 ... bit3=DDC3); byte 1363 is the "sync DDC1 to DDC0"
flag.

**1. During TX (PureSignal feedback active) — for reference:**
```
bytes 0-39:      00 00 00 91 02 00 01 01 00 00 00 00 00 00 00 00 00 00 00 c0 00 00 18 02 00 c0 18 00 00 00 00 c0 00 00 18 00 00 00 00 00
bytes 1360-1365: 00 00 00 02 00 00
```
byte7=`0x01` (DDC0 on), byte1363=`0x02` — this is the PureSignal pattern,
sent only while transmitting.

**2. Back in RX after TX ends — STUCK (relay does not release):**
```
bytes 0-39:      00 00 00 94 02 00 01 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 c0 00 00 18 00 00 00 00 00
bytes 1360-1365: 00 00 00 00 00 00
```
byte7=`0x04` (DDC2 on — correct, valid non-diversity RX), byte1363=`0x00`.
This is identical in kind to the packet that was working fine *before* TX
started. The relay stays in the TX position.

**3. Diversity enabled — FIXES IT, relay releases immediately:**
```
bytes 0-39:      00 00 00 c1 02 00 01 01 00 00 00 00 00 00 00 00 00 00 00 c0 00 00 18 01 00 c0 18 00 00 00 00 c0 00 00 18 00 00 00 00 00
bytes 1360-1365: 00 00 00 02 00 00
```
byte7=`0x01` (DDC0 on), byte1363=`0x02` — same byte7/byte1363 combination as
the TX/PureSignal packet above, just sent now while **not** transmitting.
This is the only packet variant, across every test we ran, that has ever
released the relay.

## Update 2026-06-30: host-side instrumentation results — falsifies the byte7/byte1363-alone hypothesis

We built temporary debug instrumentation in pihpsdr to send controlled
`receive_specific`/High-Priority variants on demand from the live UI (F9
cycles variants, F10 fires a one-shot pulse), so we could test pieces of
the "winning" Diversity packet in isolation rather than relying on the
all-or-nothing Diversity toggle. This code is **not currently applied** —
it's parked in a git stash in the pihpsdr repo
(`git stash list` → "brick3 relay debug instrumentation (F9 variant cycle,
F10 pulse)", touching `src/new_protocol.c`, `src/new_protocol.h`,
`src/radio.c`; `git stash pop` to bring it back for further testing).

Variants tested on real hardware:

- **Variant 1** — `receive_specific` byte7 `|= 1` (enable DDC0 *in addition
  to* the normal DDC2/3), byte1363 left at `0x00`.
- **Variant 2** — byte7 left untouched (DDC2/3 normal), byte1363 forced to
  `0x02`.
- **F10 pulse** — one-shot send of the literal Diversity-shaped packet
  (byte7=`0x01`, byte1363=`0x02`), bit-for-bit identical to packet #3 in
  the Appendix above, via `receive_specific` only (the separate
  High-Priority packet was left untouched), then immediately reverted.

**Result: none of these released the stuck relay.** Re-enabling real
Diversity mode (the actual menu toggle) still released it immediately, as
before, in the same session.

This is a stronger falsification than it might first look like: F10 sent
the *exact* packet content that "Update" item #3 in the Appendix shows
fixing things — once — and it did not fix it. So byte7=DDC0-enable and
byte1363=sync-flag in `receive_specific`, alone, are not sufficient
trigger conditions, even though they correlate perfectly with every
capture we'd taken. The original "Superseded hypothesis" section below,
which was built on that correlation, should be treated as ruled out.

Two things differ between our tests and real Diversity that we have **not
yet isolated**:

1. Real Diversity also rewrites the separate High-Priority packet (UDP,
   different port from `receive_specific`) so DDC0 and DDC1 both carry
   receiver-0's NCO frequency (`new_protocol.c`, the
   "Use frequency of first receiver for both DDC0 and DDC1" branch).
   F10's pulse never touched this packet — only `receive_specific`.
2. Real Diversity stays active across many periodic packet cycles
   (High-Priority every ~100ms, `receive_specific` every ~200ms, sent by
   `new_protocol_timer_thread`) until manually disabled. F10's pulse only
   persisted for a single send before reverting on the next cycle.

We designed, but have **not yet run on hardware**, a third variant
("variant 3" in the stashed code) to isolate these two factors: it
sustains the same Diversity-shaped content in *both* `receive_specific`
and High-Priority, without setting the real `diversity_enabled` flag
itself — specifically to avoid pulling in unrelated side effects that flag
also gates elsewhere (ALEX band-pass-filter selection, TX attenuation),
which would otherwise confound the result.

### Why we haven't run variant 3 yet — audio is not a valid proxy for it

Variant 3 sets `receive_specific` byte7=`0x01` (DDC0 only), which disables
DDC2/3, but does **not** update the host's local DDC-routing table
(`update_action_table()` in `new_protocol.c`) to expect DDC0 instead — that
table is still driven by the real `diversity_enabled` flag, which variant
3 deliberately leaves untouched. So even if the relay genuinely released
under variant 3, the host would show silence anyway, because it isn't
listening on the DDC stream the radio is actually sending. Testing variant
3 with audio-presence as the only signal would be uninterpretable either
way.

Before running variant 3 we should either:
- patch `update_action_table()` to also treat variant 3 as the Diversity
  case for routing purposes (so audio becomes a valid proxy again), or
- find a genuine relay-state observation independent of RX audio (e.g.
  listening for an audible relay click at the TX→RX transition, or a
  meter/scope on the relay coil / PTT line if accessible).

Variants 1, 2, and F10 above remain valid despite the audio-proxy caveat,
because none of them touched DDC2/3 enable or the action table — RX audio
presence/absence in those tests still faithfully reflects relay position.

## Superseded hypothesis and ask (originally written, pre-2026-06-30)

**Status: tested and falsified by the "Update 2026-06-30" section above.**
Kept here only so we don't re-derive and re-test the same idea in a future
session — the byte7/byte1363-in-`receive_specific`-alone theory has
already been ruled out by direct experiment.

### Likely root cause (from the host side, without gateware source) — falsified

It looks like the relay/PTT-release logic in the gateware is keyed
specifically to the DDC0/DDC1 pair (the only DDCs that exist in the
original single-ADC Hermes-class design) rather than to "any valid RX
configuration was received." Angelia-class hardware's second front end uses
DDC2/DDC3, and a return to RX on DDC2/DDC3 alone — even though it's a
completely valid, correctly-formed Protocol 2 command — never satisfies
whatever condition the relay-release state machine is actually checking.
Diversity happens to be the only host-side feature that touches DDC0 while
in RX, which is why it "fixes" things — it isn't doing anything special
relay-wise, it's incidentally the only RX mode that pokes the right bits.

### Ask — superseded, do not act on this without re-confirming against the Update above

Please check the relay/PTT-release condition in the TX→RX transition logic:
it should fire on *any* valid RX `receive_specific` command (regardless of
which DDC engines are enabled), not specifically on DDC0/DDC1 activity. A
host-side workaround is possible (deliberately pulsing a DDC0/diversity-like
command on every TX→RX transition) but it adds extra TX/RX transition
latency and is something we'd strongly prefer not to need.

Happy to provide full packet captures (`.pcap`) for any of the three test
scenarios above on request.

# Brick3 relay/PTT-stuck bug: root cause and fix plan

**Companion to:** `brick3-bug-report.md` (same directory)
**Status:** Root cause identified and fix validated on real hardware in
deskHPSDR (2026-06-30). Not yet ported to pihpsdr.

## Summary

After a TX cycle on Brick3 (ANAN-100D clone, Angelia-class Protocol 2
firmware), the TX relay/PTT never releases back to RX unless Diversity
mode is toggled on. `brick3-bug-report.md` shows this is *not* caused by
the `receive_specific` packet's DDC0-enable bit or "sync DDC1 to DDC0"
flag (byte 7 / byte 1363) — replicating those bytes alone, even bit-for-
bit identical to what Diversity sends, was tested on hardware and did not
release the relay.

What actually fixes it, confirmed on real Brick3 hardware via a feature
added independently in deskHPSDR: **continuously refreshing DDC0/DDC1's
NCO frequency word in the High-Priority packet during normal RX**, even
though DDC0/DDC1 stay disabled and DDC2/DDC3 remain the real audio-routed
DDCs. Diversity mode happens to do this as a side effect of its own
purpose, which is why toggling it "fixes" the symptom — but Diversity
itself is not required, only that one side effect is.

## Why this is the cause (root-cause theory)

Angelia's gateware/STM32 firmware — which drives ALEX/PA-board relay and
band-pass-filter selection — appears to track the current band from
DDC0/DDC1's NCO frequency register, regardless of whether those DDCs are
enabled for audio. During TX (PureSignal-feedback packets, or Diversity),
DDC0/DDC1's frequency word gets written to the TX frequency. Plain Angelia
RX using only DDC2/DDC3 never writes DDC0/DDC1's frequency word again
afterward — so it stays stuck at the last TX value indefinitely. The
relay/band-tracking logic apparently never resolves out of that stale
state on its own, which is consistent with only a power-cycle or a fresh
DDC0/DDC1 frequency write (i.e., enabling Diversity) clearing it.

This resolves the open question left in `brick3-bug-report.md`'s
"Update 2026-06-30" section: real Diversity differs from the falsified
test pulses in two ways — (1) it also rewrites the High-Priority packet's
DDC0/DDC1 frequency, and (2) it persists across many packet cycles. The
deskHPSDR fix isolates exactly factor (1), done persistently (factor 2),
**without** touching `receive_specific` at all — and that alone is
sufficient on real hardware.

## The deskHPSDR fix (reference implementation)

Commits `19beb25`, `d5158ac`, `5765511`, `8f6c3d7`, `164b8ad`
(deskHPSDR, 2026-06-30) added an opt-in flag `p2_angelia_ddc0_map`
(default off), exposed as a "Brick3 / ANAN-100D compatibility" checkbox
in the discovery dialog, gated to Angelia-class Protocol 2 devices:

- `src/main.c:90` — `int p2_angelia_ddc0_map = 0;`
- `src/discovery.c:600-608` — checkbox UI, toggled via
  `p2_angelia_ddc0_map_toggled()` (`src/discovery.c:271-273`)
- `src/css.c:1338,1355-1356` — persisted via `SetPropI0`/`GetPropI0`
- `src/new_protocol.c:926-944` — the actual fix, inside
  `new_protocol_high_priority()`'s non-diversity branch, after the normal
  DDC2/DDC3 frequency words are written:

  ```c
  if (p2_angelia_ddc0_map && device == NEW_DEVICE_ANGELIA && !xmit && !diversity_enabled) {
    /*
     * Brick3 / ANAN-100D compatibility:
     * in normal RX, keep the existing Angelia DDC2/DDC3 mapping, but
     * mirror the RX frequency words into DDC0/DDC1 for STM32 band tracking.
     * Do not override the Diversity DDC0/DDC1 case or the PureSignal TX case.
     */
    phase = (unsigned long)(((double) DDCfrequency[0]) * 34.952533333333333333333333333333);
    high_priority_buffer_to_radio[ 9] = (phase >> 24) & 0xFF;
    high_priority_buffer_to_radio[10] = (phase >> 16) & 0xFF;
    high_priority_buffer_to_radio[11] = (phase >>  8) & 0xFF;
    high_priority_buffer_to_radio[12] = (phase) & 0xFF;
    phase = (unsigned long)(((double)(receivers > 1 ? DDCfrequency[1] : DDCfrequency[0]))
                            * 34.952533333333333333333333333333);
    high_priority_buffer_to_radio[13] = (phase >> 24) & 0xFF;
    high_priority_buffer_to_radio[14] = (phase >> 16) & 0xFF;
    high_priority_buffer_to_radio[15] = (phase >>  8) & 0xFF;
    high_priority_buffer_to_radio[16] = (phase) & 0xFF;
  }
  ```

This block runs on every High-Priority send (~100ms cadence), continuously,
whenever the device is Angelia, not transmitting, and not in Diversity. It
never touches `receive_specific` (byte 7/1363 are unaffected — DDC2/DDC3
stay enabled, DDC0/DDC1 stay disabled for audio), never touches
`update_action_table()`'s RX-stream routing, and never touches the
`diversity_enabled`-gated ALEX band-pass-filter or attenuation logic
elsewhere in the file.

User confirmation on real Brick3 hardware: with this checkbox enabled,
the relay releases reliably after TX without ever needing Diversity mode.

## Fix plan for pihpsdr

Port the same flag/gating pattern pihpsdr already uses for
`diversity_enabled`, kept minimal, isolated, and off by default:

1. **New global flag** `p2_angelia_ddc0_map` (int, default 0), declared
   alongside `diversity_enabled`:
   - `src/radio.h:246` area — `extern int p2_angelia_ddc0_map;`
   - `src/radio.c:283` area — `int p2_angelia_ddc0_map = 0;`

2. **Persistence**, following the existing `diversity_enabled` pattern:
   - Load near `src/radio.c:3496` — `GetPropI0("p2_angelia_ddc0_map", p2_angelia_ddc0_map);`
   - Save near `src/radio.c:3728` (inside the `if (!radio_is_remote)`
     block) — `SetPropI0("p2_angelia_ddc0_map", p2_angelia_ddc0_map);`

3. **Discovery dialog checkbox**, in `src/discovery.c`'s `discovery()`
   function (starts line 572), gated on Angelia + Protocol 2, following
   the existing per-device conditional pattern (e.g. the STEMLAB block at
   `src/discovery.c:818`) and existing checkbox style (e.g. "Enable TCP"
   at `src/discovery.c:973-977`, callback pattern at
   `src/discovery.c:272-275`). Label: "Brick3 / ANAN-100D compatibility",
   with a tooltip noting it should be enabled if the TX relay fails to
   release after TX.

4. **Core fix**, in `src/new_protocol.c`'s `new_protocol_high_priority()`,
   inside the existing non-diversity `else` branch (current lines
   804-828 — the path that already computes the DDC offset for
   Angelia/Orion/Orion2/Saturn and writes the DDC frequency words). After
   that existing logic, add the new mirror block shown above, gated
   identically to deskHPSDR: `p2_angelia_ddc0_map && device ==
   NEW_DEVICE_ANGELIA && !xmit && !diversity_enabled`.

5. **Explicitly leave untouched:**
   - `new_protocol_receive_specific()` (byte 7 / byte 1363) — already
     falsified as insufficient by itself; this fix does not depend on it.
   - `update_action_table()` (`src/new_protocol.c:362-462`) — its routing
     `flag` is keyed on `newdev`/`xmit`/`puresignal`/`diversity_enabled`
     only; the new flag must not feed into it, since DDC2/3 remain the
     real audio-routed DDCs throughout.
   - The `diversity_enabled`-gated ALEX band-pass-filter selection and
     ADC attenuation logic elsewhere in `new_protocol.c` — these stay
     conditioned on real Diversity only, matching deskHPSDR.

6. **Stale debug instrumentation**: the git stash ("brick3 relay debug
   instrumentation...") tested different, now-superseded hypotheses
   (receive_specific-only pulses). It's not needed for this fix and can
   be left in the stash or dropped.

## Guidance for other Protocol 2 host applications

The same root cause applies to any host application driving Angelia-class
Protocol 2 hardware (Thetis, SparkSDR, Quisk, etc.): whenever only
DDC2/DDC3 are active for RX, the host should keep refreshing DDC0/DDC1's
NCO frequency word in the High-Priority-equivalent packet (e.g. with RX1's
frequency) on every send cycle, instead of leaving it stale from the last
TX. Recommend gating this behind an opt-in flag rather than defaulting it
on, since the underlying firmware trigger condition is only correlated
from host-side observation, not confirmed against gateware source, and
other Angelia-class radios/revisions may not need it.

## Verification (once implemented)

- Build pihpsdr — confirm no compile errors/warnings.
- On real Brick3 hardware: connect as Angelia/Protocol 2, leave Diversity
  off, enable the new checkbox, run a TX cycle (MOX and/or Tune), confirm
  RX audio resumes without touching Diversity.
- Regression: confirm default-off behavior is unchanged for existing
  users (checkbox unchecked → new code path never executes).
- Confirm Diversity mode and PureSignal TX feedback are unaffected (both
  explicitly excluded by the new block's gating conditions).
- If packet-capture tooling is available, confirm High-Priority bytes
  9-16 now continuously track RX1's frequency after a TX cycle with the
  checkbox on, instead of staying stuck at the last TX frequency.

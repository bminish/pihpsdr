# Brick3/ANAN-100D TX relay stuck — PR write-up

**Companion to:** `brick3-bug-report.md`, `brick3-fix-plan.md` (same
directory). The implementation is the commit attached as
`brick3-fix.patch` in this directory, on branch
`fix/brick3-angelia-ddc0-mirror` (pushed to
`git@github.com:bminish/pihpsdr.git`).

## How to submit upstream

The patch is already committed and pushed to a fork
(`bminish/pihpsdr`, branch `fix/brick3-angelia-ddc0-mirror`), tracking
the real upstream remote `origin` (`dl1ycf/pihpsdr`). To open the PR
against upstream:

```
gh pr create --repo dl1ycf/pihpsdr \
  --head bminish:fix/brick3-angelia-ddc0-mirror \
  --base master \
  --title "<title below>" \
  --body-file docs/brick3-fix-pr.md
```

(swap `--body-file` for whichever section you want as the actual PR
description — the "Suggested PR description" section below is meant to
be pasted as-is). Or open
https://github.com/dl1ycf/pihpsdr/compare/master...bminish:pihpsdr:fix/brick3-angelia-ddc0-mirror
in a browser and use "Create pull request".

Alternatively, `brick3-fix.patch` in this directory is a
`git format-patch` output of the same commit, suitable for attaching to
a GitHub issue or applying with `git am brick3-fix.patch` if a PR isn't
the preferred path.

## Suggested PR title

```
Fix Brick3/ANAN-100D TX relay stuck after TX (opt-in DDC0/DDC1 mirror)
```

## Suggested PR description

```markdown
## Problem

On Brick3 (an ANAN-100D clone, Angelia-class Protocol 2 firmware), the
TX relay/PTT never releases back to RX after a TX cycle — unless
Diversity mode happens to be toggled on, which "fixes" it as a side
effect. This is reproducible reliably and was diagnosed and the fix
validated against real hardware.

## Root cause

Angelia's gateware/STM32 firmware, which drives the ALEX/PA-board relay
and band-pass-filter selection, tracks the current band from DDC0/DDC1's
NCO frequency register — regardless of whether those DDCs are actually
enabled for audio. During TX (PureSignal feedback, or Diversity),
DDC0/DDC1's frequency word gets written to the TX frequency. Plain
Angelia RX uses only DDC2/DDC3 and never writes DDC0/DDC1's frequency
again afterward, so it stays stuck at the last TX value, and the
relay/band-tracking logic never resolves out of that stale state on its
own. Only a power-cycle or a fresh DDC0/DDC1 frequency write (e.g.
enabling Diversity) clears it — which is why toggling Diversity "fixes"
the symptom, even though Diversity itself plays no functional role in
the fix.

I want to credit the deskHPSDR project, where this same root cause was
independently diagnosed and fixed on real hardware (commits `19beb25`,
`d5158ac`, `5765511`, `8f6c3d7`, `164b8ad`); this PR ports the same
approach to piHPSDR.

## Fix

Adds an opt-in flag, `p2_angelia_ddc0_map` (default **off**), gated to
Angelia + Protocol 2 devices. When enabled, it continuously mirrors the
RX1 (and RX2, if active) frequency words into the otherwise-unused
DDC0/DDC1 slots of the High-Priority packet during normal RX — i.e.
whenever not transmitting and not already in Diversity mode — keeping
the STM32's band-tracking state fresh. It does not touch DDC routing,
`receive_specific`, or any Diversity-only logic.

### Changed files

- `src/radio.h`, `src/radio.c` — declare/define `p2_angelia_ddc0_map`
  (default 0), persisted via `Get/SetPropI0`, following the existing
  `diversity_enabled` flag's pattern exactly.
- `src/radio_menu.c` — adds a "Brick3 / ANAN-100D compatibility"
  checkbox to the radio menu, gated to `device == NEW_DEVICE_ANGELIA &&
  protocol == NEW_PROTOCOL`, reusing the existing generic `toggle_cb`
  handler (same pattern as the neighboring "New PA board" checkbox).
  It's in the radio menu rather than the discovery dialog since this is
  a property of the configured radio's hardware revision, not of the
  discovery/connect step.
- `src/new_protocol.c` — in `new_protocol_high_priority()`'s
  non-diversity branch, after the existing DDC2/DDC3 frequency writes,
  adds the mirror block gated by `p2_angelia_ddc0_map && device ==
  NEW_DEVICE_ANGELIA && !xmit && !diversity_enabled`.

## Explicitly untouched (and why)

- `new_protocol_receive_specific()` (DDC0/DDC1-enable bit, "sync DDC1 to
  DDC0" flag) — replicating those bytes alone was tested on hardware
  first and did **not** release the relay, so this fix does not depend
  on them.
- `update_action_table()`'s DDC routing `flag` — DDC2/DDC3 remain the
  real audio-routed DDCs throughout; the new flag only adds extra bytes
  to the High-Priority packet, it never changes routing.
- The `diversity_enabled`-gated ALEX band-pass-filter selection and ADC
  attenuation logic elsewhere in `new_protocol.c` — stays conditioned on
  real Diversity only.

## Testing

- Builds cleanly (`make`), no new warnings.
- Default-off: with the checkbox unchecked, the new code path never
  executes, so behavior for all other radios/users is unchanged.
- On real Brick3 hardware: with Diversity off and the new checkbox
  enabled, the TX relay reliably releases back to RX after a TX cycle
  (MOX and Tune), without ever needing to touch Diversity mode.
- Diversity mode and PureSignal TX feedback are unaffected (both
  excluded by the new block's `!xmit && !diversity_enabled` gating).

## Note for other Protocol 2 frontends

The same root cause likely applies to any host application driving
Angelia-class Protocol 2 hardware (Thetis, SparkSDR, Quisk, etc.) — see
`brick3-fix-plan.md` in this PR's companion notes for a portable
description of the workaround, if useful to maintainers of those
projects too.
```

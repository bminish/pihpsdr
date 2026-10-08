# The CW reference

A reference for Morse: the zero-beat tone, keying told apart from a carrier, its own start-up settings, readout and noise ratio.

Part of the local-change register: [changes.md](../changes.md).

> **2026-10-08:** every change in this file that is listed in [accepted.md](accepted.md) is in `upstream/TEST` (most since `f05546e3`); the pending ones are in the [register](../changes.md#register). The text below is the record of what each is and why; "goes upstream", "for a PR" and "Local" in it describe the state before that. See [upstream-reconcile.md](upstream-reconcile.md).

---

<a id="lc-017"></a>

## LC-017 — A CW / Morse reference

Ported from `ceeca2eb` (`feature/auto-diversity`), with the changes below.

**What it does.** `DIV_REF_CW` searches the RX filter (or a hand-placed
window) for the strongest tone, preferring the centre with a Gaussian
weighting, because in CW the centre of the passband is the note the
operator zero-beat. It accumulates the cross spectrum over that tone and
one bin either side, and nothing else in the region.
- **Sum** carries the branch noise ratio, from the off-tone bins.
- **Null** is not scaled.
- **Best** uses the per-arm SNR from the same floors.
- A keyclick that lifts the whole region (tone per bin under 2× the
  region's mean) is not accepted.

**Changed from the feature branch.**

- **The averages age every block** (see [settled decision 4](settled-decisions.md)). The feature
  branch ran CW after the window accumulation, so every key-up block went
  into the tone bins as noise and every accepted block was counted twice.
  Here CW runs first, every bin in the region is scaled by (1 − α) each
  block, and only an accepted block adds its tone bins. `test_cw` checks
  it: after a 5 s gap a new channel is taken within two elements, and
  with the ageing disabled the check fails (−62° against −78°).
- **The noise ratio is steady.** The feature branch took it from one
  block's 10th percentile of the off-tone bins, the fourth smallest of
  the forty-odd a CW filter leaves, which moved by 10 dB from block to
  block. On `test_cw`'s synthetic tone the Sum gain landed 2.5 to 9.6 dB
  off. Now it's the mean of the quieter half of those bins, smoothed at
  the Averaging time: within 0.1 dB. (A fixup commit.)
- **The gate goes through LC-012's floor** over the three tone bins, with
  the block count following the ageing.
- **Hold, not stand-down.** The in-block key-down test is not ported: it
  rejected 0 of 4123 blocks (AD-50). Key detection is LC-018.

**Settings.** Own window (600 Hz default) and threshold slots, in
`DIV_SETTINGS`, the per-group and flat props, the menu's
`store_ref_values()` / `restore_ref_values()`, LC-026's switch,
validation and the slider floor. A props file from before CW gives it CW's own
threshold, not the live one. `DIV_REF_CW` is appended to the enum, so no
saved reference moves. The wire is unchanged: like the other
references' slots, CW's are not on it.

**UI** (worth upstream's attention even if the engine is not taken):
"Measure on" is now **Window, CW, FSK/Digital, Carrier, RADE V1**, with
the labels in the row table so the order lives in one place. Carrier
keeps its place between FSK/Digital and RADE V1. The status line reads `CW <bins> track
<tone Hz>`, and the panadapter shades the search region and the tone.

**Depends on** LC-012 (the gate floor), LC-013 (`div_bin_notched()`),
LC-015 (the row table) and LC-009 (`diversity_auto_seed_window()`, now
using `div_width_default()`).

---

<a id="lc-018"></a>

## LC-018 — The CW reference tells keying from a steady carrier

Ported from `565c6e40`, with Key detect as a constant instead of a
control.

**Why.** A carrier in a CW passband holds steady through the gaps where
every station stops, and LC-017 can't tell it from keying. The keying
rate can't either: at 10 to 35 WPM the envelope moves at 4 to 15 Hz,
which one block per 43 to 171 ms samples below Nyquist. What survives is
that Morse stops.

**Change.** A block is keyed when the region's peak stands 3 dB above the
quietest that peak has recently been: a minimum that falls at once and
climbs back at 12 dB/s, seeded from the block's own noise. An unkeyed
block is held and the averages age through it. CW's Min coherence default
is 0.10, and a region of fewer than six bins is held.

**Why a constant.** Every setting from 2 to 6 dB rejects the carrier
equally; above that range the gate stops the mode (AD-50). A constant
also means no new setting and no wire change.

**Measured** (`score_cw.py`, Sum, recorded averaging, the eleven usable
CW captures scored as 16 segments between retunes and filter changes,
against the better antenna):

| | Mean | Segments ahead of the better antenna |
|---|---|---|
| Window (`TEST` before CW) | −0.70 dB | 5 / 16 |
| FSK/Digital | −0.14 dB | 8 / 16 |
| CW, LC-017 only | −0.70 dB | 8 / 16 |
| CW, with LC-018 | −0.12 dB | 9 / 16 |
| CW, with the steady noise ratio (as shipped) | −0.14 dB | 9 / 16 |

These replace the first figures, which scored each capture as one
passband and had the tone's neighbours wrong where the passband
straddles 0 Hz (`score_cw.py` fixed, LT-007). Key-up blocks acted on:
31–94 % with LC-017 alone, 2–50 % with LC-018. On three CW captures
taken with the reference in (T-010 to T-012 in `docs/test-findings.md`)
it is the best of the three references on strong contest signals, and
gains 1.6 dB on a weak beacon.

On `143433`, of the blocks the loop acted on, the tracker was within 1.5
bins of the steady carrier on **52.4 %** without key detection and
**6.5 %** with it (AD-50: 38.1 % → 3.4 %, by its own tolerance).

**Limitation.** A strong carrier that *appears* is accepted as keying
until the floor has climbed to it, at 12 dB/s: one 40 dB up for over
3 s. A carrier present all along (the AD-50 case, and `test_cw`'s) is
rejected as soon as the keying stops.

**Depends on** LC-017.

---

<a id="lc-019"></a>

## LC-019 — A fresh install starts CW on the CW reference at 0.2 s

**Change.** When the props file holds no diversity settings at all, the
CW mode group (CWL, CWU) starts on the CW reference, with its own window
and threshold, at 0.2 s averaging. Nothing else changes: a file from
before the per-group blocks still seeds every group from its flat keys
(`test_modal` section 3), and a group's own keys win over both. The
operator can move the averaging like any other setting.

**Why 0.2 s.** Swept over eleven CW captures on the feature branch, the
short end of the slider scored +0.20 dB mean against −0.40 at 1.0 s and
−0.19 at 2.0 s (`565c6e40`). That's a CW result, so it's a seed for this
group rather than a global default. **To re-test:** few of those
captures are marginal. Re-sweep as marginal CW captures come in.

**Depends on** LC-017.

---

<a id="lc-024"></a>

## LC-024 — Carrier and CW readouts from the zero beat; the client's stale overlay

Ported from `14ab067c` (`feature/auto-diversity`), extended to the CW
reference.

**Problem.** The Carrier readout (and LC-017's CW tone) is in the shifted
frame. In CW that frame's zero is one sidetone away from the zero beat,
so a correctly tuned signal read about +800 Hz. And a client only
repaints its panadapter on a spectrum packet, so switching diversity off
at the radio could leave the last overlay on screen.

**Change.** The status line takes the sidetone back out through
`div_window_zero()`, as the panadapter's carrier line and the
hand-placed window already do; one helper, `div_tone_detail()`, serves
both references. The client repaints once when the status it adopts
turns the overlay off.

**Depends on** LC-017 textually (the CW status case).

---

<a id="lc-029"></a>

## LC-029 — The CW reference's Sum noise ratio from the floor outside the filter

**Problem.** CW took its Sum noise ratio from the off-tone bins of its
own region. Those carry the station's keying sidebands and clicks (+10
to +20 dB over the real noise 50-200 Hz from the tone on T-018), which
scale with each antenna's signal, so the floor read the signal ratio as
much as the noise ratio. It failed at a 50 Hz filter (the arms read
10-15 dB apart when level; the weight ran to +20 dB) and in a 15 dB fade
on one arm.

**Change.** LC-025's floor is updated before `div_cw_solve()`, and the
Sum weight takes N0/N1 from it, the off-tone floor as fallback. Key
detection keeps the off-tone floor: it compares the peak with its own
recent minimum, not one antenna with the other, and seeded from a low
percentile elsewhere it would let a carrier through after every reset.
Best's CW SNR is unchanged.

**Measured** against the better antenna (noise taken 300-900 Hz from
the tone): T-018 +1.23 → +2.08 dB, T-017 +1.39 → +1.99; `score_cw.py`
agrees where it can score. A minimum CW region width, tried instead, only
helped the narrowest filters (T-018).

**Limitation.** At 1536 kHz (23.4 Hz bins) a CW filter of about 100 Hz
or less is under `DIV_CW_MIN_BINS` and CW holds. Accepted: narrow the span
or widen the filter. See "Limitations" in `docs/test-noisefloor.md`.

**Depends on** LC-025.

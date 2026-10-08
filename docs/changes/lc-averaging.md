# Averaging and bin width

The Averaging slider's range, the bin width, and Auto, which sets the bin width from the Averaging time and the reference, with no control of its own. Evidence: T-019 and T-020 in [test-findings.md](../test-findings.md); the older findings are 18, 21, 40, 42 and 43 in [diversity-measurements.md](../diversity-measurements.md). All three are on the branch `test/auto-bins` for testing, not yet in `TEST`.

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-048"></a>

## LC-048 — The Averaging slider stops at 6 s

**Why.** The slider ran 0.2 to 30 s. Over 70 wideband captures through the
engine as it is (Sum, flat; T-019), the best fixed value up to 10 s is
worth +0.04 dB on average over capping at 6 s, and only three captures
gain more than 0.3 dB (two of them the 17 m captures where Finding 42 says
Sum should not run). Window and Carrier prefer 0.2 to 2 s by 0.2 to 0.45
dB; FSK/Digital wants about 2 s or more. The RADE decode scores on 13
captures show no trend. A long average also holds the gate shut for
longer after the signal changes, and the RADE weight takes about 3.5 times
the setting to follow a path change (`diversity-rade.md`).

**Change.** `DIV_TAU_MAX` is 6 s (still geometric, so the short end keeps
the travel) and `div_settings_validate()` brings a saved or received tau
above 6 s down to 6 s. Menu half: one constant and its comment, written
up for dl1ycf in [menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

**Not measured.** That 6 s beats 10 s: the data only say nothing gains
from more. Null and CW were not swept (Findings 18 and 21 find shorter
better for Null).

---

<a id="lc-049"></a>

## LC-049 — The Resolution menu offers 24, 12 and 6 Hz; 3 Hz is retired

**Superseded in part by LC-051:** the control is gone, so the menu entries
and the 6 to 24 Hz range this change set no longer exist. What stays is
`DIV_MIN_NFFT` 2048, which Auto needs for 24 Hz at 48 kHz, and the
evidence below.

**Why.** 3 Hz was labelled for weak signals and loses: it trailed 12 Hz on
Sum by 0.27 to 1.22 dB and on Null by 1.7 to 5.5 dB on five of six
captures (Finding 42), and on 54 captures through the current engine it
is behind 12 Hz at every averaging time (T-020, −0.11 to −0.21 dB). 24 Hz
nulled 0.35 to 0.92 dB deeper than 12 Hz on three of four captures
(Finding 43). Above 192 kHz 3 Hz was also not a distinct setting.

**Change.** The combo is 24 / 12 / 6 Hz with the block period beside the
bin width; the default stays 12 Hz. `DIV_MIN_NFFT` drops to 2048, so that
a 24 Hz request at 48 kHz is granted as 23.44 Hz rather than silently as
11.72 Hz (Finding 43); nothing at 12 Hz or finer moves at any rate. A
saved or received 3 Hz becomes 6 Hz in `div_settings_validate()`: a
shorter block than it asked for.

This is the "24 / 12 / 6 Hz Resolution menu" that open-items listed to
port from `feature/auto-diversity`, done on `TEST`'s menu.

---

<a id="lc-050"></a>

## LC-050 — Resolution gains Auto, which sets the bin width from Averaging

**Why.** The block period is 1 / bin width. On a short average a long
block is not honoured (at 0.2 s a 6 Hz block of 171 ms is most of one
averaging time, alpha 0.57, and a 3 Hz block is longer than it), so the
operator should not have to pick. Against fixed 12 Hz, over 72 captures
(T-021), 24 Hz is +0.1 dB on Sum up to 1 s and +0.2 dB on Null at every
averaging time, and a 6 Hz tier above 5 s (the first version of this
policy) loses on Null and on Window Sum, with single captures losing 2 to
4.6 dB. **The 1 s edge was chosen after looking at that data, so it still
needs captures it was not chosen on** (T-021 lists them).

**Change.** "Auto (from Averaging)" is added to the Resolution combo as
its first entry, beside 24, 12 and 6 Hz (LC-051 then removes the combo, so
Auto is the only way the bin width is chosen).
`diversity_auto_bin_policy(ref, tau)`:

| | up to 1 s | above 1 s |
|---|---|---|
| Window, Carrier, FSK/Digital | 24 Hz | 12 Hz |
| CW, RADE V1 | 12 Hz | 12 Hz |

There is no 6 Hz tier. CW stays at 12 Hz: it tracks a tone in a filter
that can be 100 Hz wide, and its key detection (LC-018) compares block
peaks, which a longer block smears; nobody runs CW at a long average, so it
was not swept. RADE V1 works in the time domain, so the bin width only
sets the chunk. The policy runs through the engine as the radio runs it
(`run_ref --resolution auto`) and reproduces the fixed-width cell it names
exactly, on all 72 captures and all 14 averaging times (T-021).

`div_auto_resolution` holds `DIV_RES_AUTO` (−1) for Auto, and the engine
never writes it: the width in use is the engine's own derived value
(`div_target_hz()`), and the achieved width shows in the status line as
before. The transform is rebuilt only when the length Auto wants changes:
`diversity_auto_retarget()` is called from the Averaging slider; a
reference change already restarts; a settings block or a mode-group load
compares the width before and after. `div_settings_validate()` keeps a
negative as Auto and zero or NaN as missing (12 Hz); LC-051 pins every
value to Auto.

**Depends on** LC-049 (the 24 Hz entry, `DIV_MIN_NFFT`).

**Open.** The held-out captures in T-021, and Null measured on a real
interferer (every Null figure so far is the depth on the wanted signal).

---

<a id="lc-051"></a>

## LC-051 — Auto is the only bin width; the Resolution control is removed

**Why.** With Auto in place the choice of 24, 12 or 6 Hz is not one an
operator can improve on without knowing the block period and the
averaging time, which is what Auto reads. T-020 shows the fixed choices
within a few tenths of a dB of one another, and 3 Hz behind at every
averaging time. One less control, and nothing to set wrong.

**Change.** The Resolution label, combo and `res_changed_cb()` come out of
`diversity_menu.c` (the grid row stays empty and takes no space, so Hold,
Invert and the rows below keep their places). `div_auto_resolution` starts
at `DIV_RES_AUTO`, and `div_settings_validate()` and `div_settings_load()`
pin it there, like Hang (LC-011): a 12, 6 or 3 Hz left in a props file by
an older build, or sent by an older client, becomes Auto. The field stays
on the wire and in the file so neither changes shape. The achieved width
still shows in the status line.

**Kept for the tools.** `div_target_hz()` still honours a fixed width,
because `run_ref --resolution` and the unit tests set one directly (a
sweep needs it). It is unreachable from the radio; it is deleted when a PR
branch is cut (see [git-workflow.md](git-workflow.md#cutting-a-pr-branch)).

**Depends on** LC-050.

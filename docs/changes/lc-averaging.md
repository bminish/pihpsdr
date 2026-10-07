# Averaging and bin width

The Averaging slider's range, the Resolution control, and Auto, which sets the bin width from the Averaging time. Evidence: T-019 and T-020 in [test-findings.md](../test-findings.md); the older findings are 18, 21, 40, 42 and 43 in [diversity-measurements.md](../diversity-measurements.md). All three are on the branch `test/auto-bins` for testing, not yet in `TEST`.

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
block is not honoured (at 0.2 s a 6 Hz block of 171 ms is most of one averaging
time, alpha 0.57, and a 3 Hz block is longer than it), and on a long one finer bins cost
nothing, so the operator should not have to pick. The sweep (T-020)
shows no measurable SNR from the mapping itself, +0.08 dB at 0.5 s and
+0.02 dB at 6 s, so Auto is justified by timing and by resolution for the
references that track a narrow feature, not by the numbers. **The
thresholds are a hypothesis to test.**

**Change.** "Auto (from Averaging)" is the first entry; 24, 12 and 6 Hz
stay. `diversity_auto_bin_policy(ref, tau)`:

| | up to 0.5 s | up to 5 s | above 5 s |
|---|---|---|---|
| Window | 24 Hz | 12 Hz | 6 Hz |
| Carrier, CW, FSK/Digital | 12 Hz | 12 Hz | 6 Hz |
| RADE V1 | 12 Hz | 12 Hz | 12 Hz |

Carrier, CW and FSK/Digital track a narrow feature (five bins, the tone's
three, the occupancy edges) and 24 Hz is not measured to help them. RADE
V1 works in the time domain, so the bin width only sets the chunk.

`div_auto_resolution` holds `DIV_RES_AUTO` (−1) for Auto, and the engine
never writes it: the width in use is the engine's own derived value
(`div_target_hz()`), and the achieved width shows in the status line as
before. The transform is rebuilt only when the length Auto wants changes:
`diversity_auto_retarget()` is called from the Averaging slider; a
reference change already restarts; a settings block or a mode-group load
compares the width before and after. `div_settings_validate()` keeps a
negative as Auto and zero or NaN as missing (12 Hz). The default for a
fresh install is still 12 Hz.

**Depends on** LC-049 (the 24 Hz entry, `DIV_MIN_NFFT`) and LC-048 (the
5 s tier sits under the 6 s cap).

**Open.** CW keying: a 171 ms block spans several dits, which LC-018's key
detection might smear. Auto never gives CW more than 6 Hz, and only above
5 s, but it has not been tried on air.

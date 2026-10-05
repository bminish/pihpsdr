# Feature to test later: attenuator calibration from the per-arm noise floor

**Status: designed, not built.** (A one-press subset, which balances the hotter arm from the floor without a sweep and without the converter floor, is built on the binaural branch as LC-043.) Written 2026-10-01 on
`test/noise-floor`, and on `TEST` with the noise floor it builds on
(LC-025). Only the read-only accessor it needs,
`diversity_auto_noise_floor()`, exists in the code.

## What it is for

Suggest each ADC's step attenuator setting from what the diversity engine
already measures, so each antenna is band-noise limited without wasting
dynamic range.

Each step of attenuation lowers the wanted signal and the band noise
together. As long as the band noise stays well above the converter's own
noise, that costs nothing in SNR and buys headroom against overload. Once
the band noise reaches the converter floor, every further dB costs SNR.

| Band noise over the converter floor | SNR given up to the converter |
|---|---|
| 20 dB | 0.04 dB |
| 10 dB | 0.4 dB |
| 6 dB | 1.0 dB |
| 3 dB | 1.8 dB |
| 0 dB | 3.0 dB |

The suggestion for each arm is the most attenuation that keeps a chosen
margin (10 dB as a default; 0.4 dB of SNR), unless the ADC overload flag
says to add more.

**Matching the two arms is not a goal.** The Sum weight is
maximum-ratio combining: it corrects for a level difference between the
arms, so SNR does not depend on the two levels being equal. Two things
still make very lopsided arms undesirable: the weight's clamp
(`DIV_MAX_WEIGHT`, +20 dB, which the CW captures T-017/T-018 reached), and
Manual mode, where the operator sets the balance by hand. Getting each arm
noise-limited matters more than equalising them.

## What we already have

- **Each arm's band-noise floor, every block.** `div_noise_floor_update()`
  (LC-025, from `8a393217`): the 8th-12th percentile of up
  to 1024 bins outside the RX filter, smoothed over 2 s (`div_nf0`,
  `div_nf1`). It is in FFT power units relative to ADC full scale, so it
  is an absolute level per arm, not only a ratio.
- **The attenuator setting in force.** `adc[a].attenuation`, set through
  `radio_set_adc_attenuation()`. The engine is told of every change
  (`diversity_auto_att_changed()` rescales the weight) and an attenuator
  change is a context change (`div_context_changed()` compares `att0` and
  `att1`), so the statistics, the floor included, restart from the new
  setting. The captures record both settings per block.
- **The overload flag.** `adc[a].overload`, set by the protocol threads
  from the hardware (`new_protocol.c` byte 5 bits 0/1, `old_protocol.c`
  `mercury_overload`), and cleared every two seconds by the panadapter
  (`rx_panadapter.c`). It is the only overload evidence that counts: the
  diversity samples are after the DDC and never see the broadcast-band or
  out-of-band signals that actually overload the converter.

## What we do not have: the converter floor

The floor tells us how loud the band noise is. It does not tell us how far
that is above the converter's own noise, which is what the margin needs.
That has to be measured once per arm, and once per DDC sample rate,
because the converter noise in a DDC bin scales with the decimation.

### How to measure it: the slope

Step one arm's attenuator and watch its floor. Model, in dB:

    floor(att) = 10 log10( 10^((B - att)/10) + 10^(C/10) )

with B the band noise at 0 dB attenuation and C the converter floor. Where
the band noise dominates, the floor falls 1 dB per dB of attenuation; as
it approaches C the slope flattens towards zero. Fitting B and C to the
steps gives the margin at any setting: B - att - C.

### What the existing captures show

Five captures from 2026-09-02/03 stepped an attenuator. Fitted with the
model above, using the outside-filter floor:

| Capture | Arm stepped | Range | Slope | Reading |
|---|---|---|---|---|
| `20260903-122632` | arm 1 | 0 to ~11 dB | −0.96 dB/dB, fit error 0.24 dB | band-noise limited throughout |
| `20260902-142333` | arm 0 / arm 1 | 0-4 dB | −0.98 / −0.72 | arm 0 limited by band noise; arm 1 already flattening |
| `20260902-142026` | arm 1 | 21-23 dB | −0.55 | converter floor showing: too much attenuation |
| `20260902-234624` | arm 0 | 0-14 dB | −0.96 | band-noise limited, but steps of 1-3 blocks |
| `20260902-234731` | arm 0 | 0-14 dB | −0.93 | as above |

The method separates the two regimes. None of these gives a reliable C:
most never stepped far enough to flatten, so the fit only bounds C from
above, and the quick steps mixed in the band noise changing over time (on
`234624` arm 1's floor wanders 8 dB with its own attenuator never moving).

### What a good calibration run needs

- **One arm at a time**, the other left alone, so a change in band noise
  shows up on the untouched arm as a reference and can be divided out.
- **Slow steps:** 0 to 31 dB in 3 dB steps, about 2 s per step (the floor
  restarts on each change and its first value is taken from the first
  block, then smoothed at 2 s).
- **Far enough to flatten.** If the slope is still −1 dB/dB at 31 dB, C is
  below what this front end can reveal at that band noise; record it as a
  bound and say so.
- **A quiet moment if possible.** The floor is a percentile and copes with
  a busy band, but a band-noise level that moves by several dB during the
  run spoils the fit; the reference arm catches that.

## Proposed feature

1. **"Att calibrate" (per arm), in the Diversity menu.** Steps that arm's
   attenuator 0-31 dB in 3 dB steps, about 2 s each, with diversity
   running, then restores the setting it found. Fits B and C, corrected
   by the untouched arm's floor. Stores C per arm, per sample rate, in the
   props file. About 25 s per arm.
2. **Live advice on the status line:** each arm's margin over its
   converter floor and what that implies, for example
   `ADC1 +18 dB: can take 8 dB more  ADC2 +7 dB: hold`. If the overload
   flag is set, the advice is to add attenuation regardless.
3. **Advise, do not drive.** Every attenuator change resets the diversity
   statistics, so moving it automatically mid-QSO costs re-convergence. At
   most a one-press "Apply suggestion".

### Threading: what must not happen

- The calibration runs on the GTK thread (a `g_timeout_add` stepping
  through the sequence), never by blocking or sleeping there.
- It reads the floor the worker publishes; it never takes `mbox_mutex` or
  waits for the worker. A per-step snapshot (copy `div_nf0`/`div_nf1` and
  a block counter, published by the worker with atomics or under a small
  lock of its own that nothing else holds) is enough.
- It must not run while transmitting: `rxtx()` already signals the
  transmit gap, and the calibration should abort on it rather than fit
  across it.
- Attenuator changes go through `radio_set_adc_attenuation()` so the
  engine's weight compensation and context change happen as they do for
  the operator.

### Depends on

- The across-frequency noise floor (LC-025), now on `TEST`.
- Hardware with step attenuators on both ADCs (`have_rx_att`).
- For the advice to be trustworthy across bands: C measured per sample
  rate; whether it varies by band (preselector, preamp) is to be checked
  by calibrating on two bands.

## How to test it when it is built

1. Calibrate each arm on a quiet band, twice, and check C repeats to
   within 1 dB.
2. Compare C with an independent measurement: the floor with the antenna
   input terminated, at 0 dB attenuation.
3. On a band with high band noise (40 m at night) and one with low (10 m),
   check the advice differs as expected: room to attenuate on 40 m, little
   or none on 10 m.
4. Set the suggested attenuation and confirm by capture that the combined
   SNR does not drop (scored as in `docs/test-noisefloor.md`), and that
   the overload indicator stays clear where it was tripping before.
5. Confirm the calibration aborts cleanly on transmit and on closing the
   menu, and leaves the attenuators as it found them.

# Diversity menu: what the engine now expects from it

Notes for dl1ycf, who looks after `diversity_menu.c`. Each item is a
change on our side in `diversity_auto.c` that follows the agreed rule:

> The engine writes a setting only at start-up (props file) or when a
> client takes the server's settings. Any other setting the menu can
> change, the menu owns.

Each item says what the engine does now, what the menu has to do, and
what our `TEST` branch already does in the menu. That way a rewrite of
the menu can carry it over, or do it differently.

Register: [changes.md](changes.md); the rules are in
[changes/ownership.md](changes/ownership.md).

---

## E1: RADE V1 that cannot start (LC-031)

**Before.** If `rade_corr_start()` failed, `diversity_auto_start()` set
`div_auto_ref = DIV_REF_DIGITAL_IQ` silently.

**Now.** The reference is left alone and the loop holds; no weight is
produced. **There is nothing for the menu to do**: the RADE V1 status
reads "search". It can't happen at any rate piHPSDR offers, since all of
them are multiples of 8 kHz, so it gets no flag or status of its own.

---

## E2: seeding the window when Follow RX Filter is unticked (LC-032)

**Before.** On untick, the menu called `diversity_auto_seed_window()`,
and that function wrote `div_auto_centre` and `div_auto_width` itself.

**Now.** The engine computes the window and returns it; the menu stores
it:

```c
// diversity_auto.h
extern int diversity_auto_seed_window(double *centre, double *width);
```

- It returns 1 with the window in `*centre` / `*width` when the selected
  reference's window is still at its built-in default (centre 0, default
  width) and the RX filter is at least 20 Hz wide. The window is the
  passband being followed, in the same coordinates as a hand-placed
  window (`div_window_zero()` + centre ± width/2), CW included.
- It returns 0 for "leave the window as it is": RADE V1, a window the
  operator has already placed, or a filter under 20 Hz.

**What the menu needs to do.** In the Follow RX Filter callback, when the
tick goes off:

```c
if (!div_auto_follow_filter) {
  double centre, width;

  if (diversity_auto_seed_window(&centre, &width)) {
    div_auto_centre = centre;
    div_auto_width  = width;
  }

  store_ref_values(div_auto_ref);   // file it under the reference
  div_ref_widgets_show();           // and show it
}
```

Behaviour is unchanged: the same values land in the same globals,
written by the menu instead of the engine.

---

## The antenna readout: ADC1 / ADC2 (LC-034)

Your `890ed310` relabelled the attenuators ADC1 and ADC2. The antenna
line above them still printed the arm index ("ADC0 better by ...",
"using ADC1"). It was also wrong with RX1 set to the second ADC, because
arm 0 is then the ADC RX1 is set to, which is ADC2. Our
`div_arm_status_set()` now does:

```c
const int rxadc = receiver[0]->adc;
...
snprintf(sel, sizeof(sel), "  using ADC%d", (div_auto_arm_pick ^ rxadc) + 1);
...
snprintf(text, sizeof(text), "Antennas  ADC%d better by %4.1f dB%s",
         (((div_auto_arm_db > 0.0) ? 1 : 0) ^ rxadc) + 1, d, sel);
```

Elsewhere, outside the menu, upstream text still says ADC0/ADC1: the
panadapter's overload warnings ("ADC0 overload", "ADC1 overload",
"ADC0+1 overload"), `receiver.c`'s "hard-wired to ADC0", and some
protocol and simulator comments.

---

## Outside the menu: the weights start at 1 + 1j (LC-037)

Not a menu item, but it's your file (`radio.c`), so it goes here. Both
weight pairs are initialised to `cos = 1.0, sin = 1.0`, which is +3 dB
at 45°, while the gain and phase beside them say 0 dB and 0°:

```c
double man_div_sin = 1.0;   // should be 0.0
double auto_div_sin = 1.0;  // should be 0.0
```

**How it happened:**
- **`3d3bb23b` (2019-07-25)** replaced
  `i_rotate[2] = {1.0, 1.0}` / `q_rotate[2] = {0.0, 0.0}` (unity) with
  `div_cos = 1.0` / `div_sin = 1.0`.
- **For seven years it was harmless.** `radio_set_diversity()` calls
  `radio_calc_div_params()`, which recomputes the pair from gain and
  phase, and the props file restores a consistent pair.
- **`4865d602` (2026-09-27)** copied the pair into `man_div_*` and
  `auto_div_*`. The manual pair is still masked. The automatic pair is
  not: nothing recomputes it and it isn't saved. So from start-up until
  the loop's first solve, the combiner applies 1 + 1j while the
  automatic readout shows 0 dB / 0°.
- **Effect: a degraded starting weight, no more.** The first solve
  replaces it within seconds on Window, Carrier and FSK/Digital. On
  RADE V1 it stands until the first lock but can't delay it, since the
  correlator works on the raw arms. It's small in practice; worth fixing
  because it's plainly wrong and the fix is two characters.

**Fix:** `sin = 0.0` on both lines. That's our LC-037, which applies to
your `TEST` as it stands.

---

## Still to discuss (no code yet)

These are in the review table in [changes/ownership.md](changes/ownership.md) (E3 to E5) and need a
decision from both sides before anyone writes code:

- **E3: live copy versus per-reference slots.** The window and Min
  coherence are each kept twice: once as the live value, and once in a
  slot per reference. The menu stores and recalls them by copying, and
  the engine also sets the live Min coherence from the slot (LC-026). We
  propose that the engine reads the selected reference's slot directly,
  and the live copies go away.
- **E4: Hold.** `diversity_auto_set_hold()` writes `div_auto_hold`. We
  propose that the caller writes it and calls an engine function that
  only does the weight handover.
- **E5: per-mode settings into `profiles.c`.** This is your rule 4.
  Decided: diversity takes the profile grouping from
  `profiles_copy_rxtxprofile()`. DSB moves to the SSB group, and AM, SAM
  and FMN each get settings of their own. Old per-group props keys
  migrate once.

---

## The Restart averaging button is gone (LC-039)

We removed the "Restart averaging" button and its `reset_cb()` from
`diversity_menu.c`. Averaging forgets by itself, and a reference or
follow change already restarts the estimate, so a hand reset is not
needed. Hold and Invert then move up one row each into the freed cell
(LC-040, our own layout, not for upstream), so Hold is at row 3,
column 8. The engine is untouched: `diversity_auto_reset()` and
`DIV_ACTION_RESET` remain.


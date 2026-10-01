# Diversity menu: what the engine now expects from it

Notes for dl1ycf, who looks after `diversity_menu.c`. Each item is a
change on our side in `diversity_auto.c` that follows the agreed rule:

> The engine writes a setting only at start-up (props file) or when a
> client takes the server's settings. Any other setting the menu can
> change, the menu owns.

Each item says what the engine does now, what the menu has to do, and
what our `TEST` branch already does in the menu. That way a rewrite of
the menu can carry it over, or do it differently.

Register: [changes.md](changes.md), section "Who owns what: menu and
engine".

---

## E1: RADE V1 that cannot start (LC-031)

**Before.** If `rade_corr_start()` failed, `diversity_auto_start()` set
`div_auto_ref = DIV_REF_DIGITAL_IQ` silently. The menu went on showing
RADE V1, and the next props save stored FSK/Digital.

**Now.** `div_auto_ref` is left alone. The engine sets a new status
variable and holds (no weight is produced):

```c
extern int div_auto_rade_unavailable;   // diversity_auto.h
```

- It is 1 when RADE V1 is selected and the correlator could not start
  at the sample rate in use, and 0 otherwise.
- It is recomputed on every `diversity_auto_start()`, so on every
  restart: a reference change into or out of RADE V1, a resolution
  change, a sample-rate change.
- It is engine output, like `div_auto_holding`. The menu only reads it.

**What the menu needs to do.** Show it. In our `status_update_cb()`, the
`DIV_REF_RADE_V1` case checks it before the lock state:

```c
if (div_auto_rade_unavailable) {
  state = "n/a";
  snprintf(detail, sizeof(detail), "rate");
} else if (rade_corr_locked) {
  ...
```

Any wording will do; the point is that the operator can see that RADE V1
isn't running.

**How likely.** Not reachable today. The correlator needs a DDC rate that
is a multiple of 8 kHz, and every rate piHPSDR offers (48 to 1536 kHz)
is one. The check guards against a future rate.

**Client/server.** The flag is not on the wire (`DIV_STATUS` is
unchanged), so a client never sees it set. If it should, it's one more
field in `DIV_STATUS`. That's left for the client/server work.

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

## Still to discuss (no code yet)

These are in the review table in `changes.md` (E3 to E5) and need a
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
- **E5: per-mode settings into `profiles.c`.** This is your rule 4. One
  question first: `profiles_copy_rxtxprofile()` groups DSB with
  LSB/USB, while diversity groups DSB with AM/SAM, and FM/AM/SAM aren't
  grouped in `profiles.c` at all. Should diversity take the profile
  grouping?

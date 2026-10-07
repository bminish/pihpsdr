# The Diversity menu

Changes in `diversity_menu.c` (dl1ycf's file) or that the menu must act on. Each menu half is kept minimal so his rewrite of the menu can redo it; what each needs from the menu is in [menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-005"></a>

## LC-005 — Make the Invert button swap Null and Sum again

**Problem.** When the menu was rewritten, the body of the Invert button
was left commented out. It pointed at a combo box that no longer
exists, so pressing Invert did nothing.

**Change.** The objective combo is kept in a static, and Invert moves it
between Null and Sum. That goes through `mode_changed_cb()`, so the
button and the combo cannot behave differently. Invert still does
nothing in Manual or Best.

**Upstream.** dl1ycf made the same fix independently in `b180b79a` (the
objective combo as `auto_btn`). Taken as upstream wrote it at the merge;
our `mode_combo` is gone and the LC-005 commit is superseded.

---

<a id="lc-008"></a>

## LC-008 — A reference change shows that reference's own settings

**Problem.** Each reference (Window, Carrier, FSK/Digital, CW, RADE V1)
keeps its own window and its own Min coherence threshold. Before
`f5a0ce9c`, upstream left the recall under `#if 0`, so the new reference
ran on the previous one's threshold and window. `f5a0ce9c` now recalls
them in `ref_changed_cb()` through `restore_ref_values()`, whose comment
says the widgets have to follow with their handlers blocked. They did
not: the centre, width and Min coherence controls kept showing the
previous reference's values, and the next move of any of them filed
those under the new reference.

**Change.** `restore_ref_values()` ends by calling
`div_ref_widgets_show()`, which sets the two spin buttons and the slider
with `centre_cb()`, `width_cb()` and `coh_cb()` blocked
(`g_signal_handlers_block_by_func`, as his comment asks). The slider is
kept in a static (`coh_scale`), and the three pointers are cleared when
the dialog closes. `coh_cb()`'s stale comment about `div_window_recall()`
is corrected.

**Reshaped 2026-10-01.** This used to re-enable `div_window_recall()`
and guard the callbacks with an `updating_from_auto` flag. Upstream
removed both, so the change is now only the widget half, on upstream's
functions. LC-009 and LC-012 use `div_ref_widgets_show()` and the same
blocking.

---

<a id="lc-009"></a>

## LC-009 — Unticking Follow RX filter starts the window on the passband

**Why.** We do not want to start from useless values when none are
saved. Unticking "Follow RX Filter" (then "Window follows RX filter") hands the window to the
operator. If that reference has no window of its own yet, it fell back
to its built-in default: centre 0 and 1000 Hz wide (2600 Hz for Digital
IQ). In SSB that straddles the carrier, so the first manual window the
operator saw had to be dragged into place before auto diversity did
anything sensible.

**Change.** If the selected reference's window is still at its default,
it is placed on the current RX passband, exactly where the follow window
was (CW included). A window the operator has placed is left alone,
however narrow: 20 Hz is a width the slider offers, and anything below
it is already put back to the default by LC-002. (It first also seeded a
window at 20 Hz or below; a fixup removed that.) Following the RX filter
stays the default when nothing is saved.

**Depends on** LC-008 (the widget pointers).

**Since 2026-10-01.** `diversity_auto_seed_window()` only computes the
window; the menu's `follow_cb()` files it with `store_ref_values()` and
shows it with `div_ref_widgets_show()` (LC-008). The engine-side store it
used went with `f5a0ce9c`.

---

<a id="lc-015"></a>

## LC-015 — The "Measure on" menu runs the reference it shows

**Problem.** The combo lists Window, FSK/Digital, Carrier, RADE V1. The
`DIV_REF_*` enum is BAND, CARRIER, RADE_V1, DIGITAL_IQ. And
`ref_changed_cb()` stored the combo row as the reference. So the menu ran
a different reference from the one it showed:

| Row | Menu shows | Engine ran |
|---|---|---|
| 0 | Window | Window |
| 1 | FSK/Digital | Carrier |
| 2 | Carrier | RADE V1 |
| 3 | RADE V1 | FSK/Digital |

Opening the menu had the same fault in reverse.

**How it was found.** Five captures taken on 2026-09-30 with "RADE V1"
selected (`172640`, `172847`, `172907`, `173144`, `173330`). The operator
saw the "correlator" appear to lock and follow junk. Every block of all
five records reference 3 (FSK/Digital), and the RADE correlator's lock
state is 0 throughout: it never ran. What looked like a RADE tracking
regression was the FSK/Digital occupancy solve fitting to whatever was
loudest in the passband. These captures therefore say nothing about RADE
tracking; that still needs RADE captures taken with this fix in.

**Change.** A row table, `div_ref_rows[]`, maps rows to references and
back, keeping the order the menu shows. `div_ref_to_row()` is the name
the populate code under `#if 0` already calls. The objective and
resolution combos were checked and are correct.

**Origin.** The bug is upstream. The feature branch had this mapping as
`ref_rows[]`, and it was lost when the menu was rewritten for upstream.

**Upstream.** Prepared as a one-commit PR against dl1ycf's `TEST`: branch
`pr/diversity-menu-ref-row`, cut from `upstream/TEST` at `883243c0`,
`src/diversity_menu.c` only, +30 −2. It builds. Opened 2026-09-30 as
[dl1ycf/pihpsdr#150](https://github.com/dl1ycf/pihpsdr/pull/150).
Rebased onto `b180b79a` the same day, when upstream's menu layout
changed the combo's attach line next to it; still +30 −2, mergeable. When
it's merged, mark LC-015 *Upstream*; the local commit can then be dropped
at the next resync.

**After `f5a0ce9c` (2026-10-01).** #150 conflicts again and needs
rebasing. The bug now does more harm upstream: `ref_changed_cb()` also
recalls the slot of the row number, and its RADE V1 test is against the
row, so choosing "Carrier" (row 2 = `DIV_REF_RADE_V1`) forces Sum and
starts the RADE correlator. On `TEST` the line is
`div_auto_ref = div_row_to_ref(...)` ahead of `restore_ref_values()`.
Rebased onto `f5a0ce9c` the same day: still one commit, +30 −2, the row
mapped before `store_ref_values()` / `restore_ref_values()` see it.
Mergeable again, with a comment on the PR saying what changed.

---

<a id="lc-020"></a>

## LC-020 — The follow tick reads "Follow RX Filter"

**Change.** The tick is relabelled from "Window follows RX filter" to
"Follow RX Filter", and the two comments that name it follow.

**Reshaped 2026-10-01.** This used to hide the Window centre and width
while the tick is on, and shrink the dialog. `f5a0ce9c` greys them out
instead, which does the same job, so we took upstream's and only the
label is left.

**Depends on** LC-009 textually (the comment in
`diversity_auto_seed_window()`).

---

<a id="lc-021"></a>

## LC-021 — The window spin buttons set their digits as spin buttons

**Problem.** Upstream's `b180b79a` set the Window centre and width spin
buttons to show no decimals with `gtk_scale_set_digits(GTK_SCALE(btn),
0)`. They are `GtkSpinButton`s, not `GtkScale`s: the cast fails GTK's
type check (a critical warning each time the menu opens) and the call
does nothing.

**Change.** `gtk_spin_button_set_digits(GTK_SPIN_BUTTON(btn), 0)`, which
does what was meant.

**Upstream.** A one-commit PR: branch `pr/diversity-spin-digits`, cut
from `upstream/TEST` at `b180b79a`, `src/diversity_menu.c` only, +2 −2.
It builds. Opened 2026-09-30 as
[dl1ycf/pihpsdr#151](https://github.com/dl1ycf/pihpsdr/pull/151). When
it's merged, mark LC-021 *Upstream*.

**Upstream, 2026-10-01.** `f5a0ce9c` deleted the two bad calls (a spin
button with a step of 10 shows no decimals anyway). Our commit was
dropped at the rebase. #151 closed 2026-10-01 with a note saying why,
and its branch `pr/diversity-spin-digits` deleted.

---

<a id="lc-032"></a>

## LC-032 — The seeded window is returned to the menu, not written by the engine

**Why.** LC-009's `diversity_auto_seed_window()` wrote `div_auto_centre`
and `div_auto_width`, which are menu settings (E2 in [ownership.md](ownership.md#review-e1-to-e8)).

**Change.** The function now returns 1, with the window in `*centre` and
`*width`, or 0 to leave the window alone. `follow_cb()` stores the
result. The same values land in the same globals, so there is no change
in behaviour. Needs LC-009, which adds the function. The menu half is
written up for dl1ycf in
[menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md#e2-seeding-the-window-when-follow-rx-filter-is-unticked-lc-032).

---

<a id="lc-034"></a>

## LC-034 — The antenna readout names the converter, ADC1 or ADC2

**Why.** The menu's antenna line printed the arm index as the ADC
("ADC0 better by ...", "using ADC1"). After `890ed310` the attenuator
row beside it says ADC1 and ADC2, so the two disagreed. The line was
also wrong with RX1 set to the second ADC: arm 0 is then the ADC RX1 is
set to (`5db64949`), which is ADC2.

**Change.** The line shows `(arm ^ receiver[0]->adc) + 1`, at the same
width, finding RX1's ADC as the attenuator sliders above it do. This is in dl1ycf's file, so it is written up for him in
[menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

<a id="lc-039"></a>

## LC-039 — The Restart averaging button is removed

**Why.** Averaging forgets by itself (settled decision 4: averages age
at the Averaging time whether or not a gate accepts the block), and a
reference or follow change already restarts the estimate. A control
that throws the statistics away by hand is not needed, and the Menu is
better with one button fewer. The menu is dl1ycf's file; this is a UI
removal that upstream may or may not want, tracked here so it is not
mistaken for an accident at a re-sync.

**Change.** `diversity_menu.c` loses the button and its `reset_cb()`.
The engine is untouched: `diversity_auto_reset()` and
`DIV_ACTION_RESET` stay, as the client/server path and LC-027 still use
them, and the grid cell beside Hold is left empty. Written up for him in
[menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).


<a id="lc-040"></a>

## LC-040 — Hold and Invert move up into the freed cell

**Why.** With the Restart averaging button gone (LC-039) the column
beside the sliders had a hole at the top: an empty cell over Hold and
Invert. Moving the two up one row each closes it, and leaves the row
under them free.

**Change.** `diversity_menu.c`: Hold goes from row 4 to row 3, Invert
from row 5 to row 4, both column 8. No behaviour change. This is our own
layout choice, not something agreed with upstream, and is not meant for
a PR: if the menu is taken from upstream at a re-sync, LC-039 and this
go together or not at all. Depends on LC-039.


<a id="lc-047"></a>

## LC-047 — The second attenuator slider is labelled with its own ADC

**Why.** The attenuator row at the top of the menu has two sliders: the
left, "RX1 ATT:", is the ADC RX1 is set to; the right is the other one.
The right was labelled "ADC2:" whatever RX1 was set to. With RX1 on ADC2
the right slider is ADC1's (the attenuator it moves already follows RX1's
ADC, as it should), so the label named the wrong converter. Seen in use.

**Change.** The label is `ADC%d:` with `otheradc + 1`, the same variable
the slider is wired with. The labels keep the hardware names (ADC1, ADC2),
not "Other"; "other" stays internal. The left label is unchanged. The
menu is built when it opens, so a change of RX1's ADC with the menu open
shows on the next open, like the sliders' own values. No dependencies.
Written up for dl1ycf in [menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md).

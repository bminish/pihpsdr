# Proposal: A Diversity Menu That Stays Open

**Status:** Proposal. Nothing here is built.  
**Target Subsystem:** Menu plumbing (`src/new_menu.c`, `src/new_menu.h`), Diversity menu (`src/diversity_menu.c`), receiver-count changes (`src/radio.c`)  
**Companion Documents:** [`docs/diversity.md`](diversity.md), [`docs/diversity-guide.md`](diversity-guide.md)

---

## 1. The problem

piHPSDR allows one menu open at a time. With **RX** open, opening
**Diversity** closes RX, and the other way round. For diversity this gets
in the way. Setting it up means going back and forth between the Diversity
dialog and the RX, Radio or Display menus, or simply keeping the status
lines (`status_label`, `arm_label`) on screen while operating. Each round
trip closes the Diversity dialog. That throws away its layout, and it also
releases **Hold** (see §4.3).

The goal is for the Diversity dialog to be the one exception: it stays open
while other menus come and go, and opening it does not close whatever else
is open.

Making *every* menu independent is out of scope (§7).

---

## 2. How the one-menu rule is enforced now

There is a single slot, `GtkWidget *sub_menu` (`new_menu.c:77`, exported in
`new_menu.h:25`), plus a single enum `active_menu` that the
`start_*_menu()` toggles use. Every sub-menu stores its dialog in the slot
and clears it on close:

| Where | What it does |
|---|---|
| `diversity_menu.c:2152` | `sub_menu = dialog;`, which puts Diversity into the shared slot |
| `diversity_menu.c` `cleanup()` | `sub_menu = NULL; active_menu = NO_MENU;` |
| `new_menu.c:89` `cleanup()` | destroys `main_menu` **and** `sub_menu`, then resets `active_menu`. Almost every main-menu button calls it before opening its own menu |
| `new_menu.c:524` `new_menu()` | destroys `sub_menu` as soon as the main **Menu** opens |
| `new_menu.c:80` `menu_active_receiver_changed()` | destroys `sub_menu` when the active receiver changes (queued from `receiver.c:108`) |
| `new_menu.c` `new_menu()` | connects the main menu's `delete_event` **and** `destroy` to `close_cb` → `cleanup()`, so closing the main menu in any way also destroys `sub_menu` |
| `new_menu.c:412` `start_diversity_menu()` | `cleanup(); diversity_menu(top_window);` with no toggle, unlike `start_band_menu()` and the others |

Two routes open the dialog: the **Diversity** button in the main menu
(`diversity_cb`), and the `MENU_DIVERSITY` action (`actions.c:962`) bound
to a MIDI, GPIO or toolbar key, which is gated on
`RECEIVERS == 2 && n_adc > 1`.

---

## 3. Why Diversity is a good candidate

The main difficulty with menus that coexist is menus that capture state
when they open. `rx_menu.c`, for example, stores `myrx`/`myadc`/`myid` at
open time and only stays correct because a receiver switch destroys it.
**The Diversity dialog does not do this:**

- It does not bind to `active_receiver`. It reads globals: `adc[0..1]`,
  `receiver[0]`, and the `div_*` settings.
- It already polls. `status_update_cb`, a 250 ms `g_timeout_add` started at
  `diversity_menu.c:2206`, calls `update_enable_control()`,
  `update_att_controls()`, `update_manual_sensitivity()` and
  `update_sliders_from_weight()`. So an attenuator changed from the RX
  menu, the sliders or a client shows up in the Diversity dialog within a
  quarter second, with no new code.
- It already has refresh entry points for changes made elsewhere:
  `diversity_menu_refresh()` (the server, when a client moves a control),
  `diversity_menu_settings_changed()` (a mode change swapping modal
  settings), and `diversity_client_set_settings()` (the client, when the
  radio sends settings).
- Its `cleanup()` already clears every widget pointer, so late callbacks
  and timers become no-ops after the dialog closes.

So what's left is mostly plumbing, not a state-synchronisation redesign.

---

## 4. What needs to change

### 4.1 Take Diversity out of the shared slot (`diversity_menu.c`)

1. Delete `sub_menu = dialog;` (line 2152).
2. In `cleanup()`, delete `sub_menu = NULL;` and `active_menu = NO_MENU;`.
   Once Diversity is independent, leaving these in would wipe the record of
   *another* menu that happens to be open. That breaks that menu's toggle,
   and the next `cleanup()` in `new_menu.c` would no longer find and close
   it.
3. Guard against a second copy. At the top of `diversity_menu()`:

   ```c
   if (dialog != NULL) {
     gtk_window_present(GTK_WINDOW(dialog));
     return;
   }
   ```

   Today a second open can't happen, because `cleanup()` always runs
   first. Once that stops, a second press would build a second dialog over
   the same static widget pointers and start a second `status_timer`.

4. Export a close function, `void diversity_menu_close(void)`, which just
   calls the static `cleanup()`, for the teardown paths in §4.4.

This step on its own gives **half the behaviour**. Opening RX, Radio or
any other menu no longer closes Diversity, because none of the
`sub_menu` destroy sites can reach it. `menu_active_receiver_changed()`
stops closing it as well, which is correct since it never depended on the
active receiver.

### 4.2 Stop Diversity closing the other menu (`new_menu.c`)

The other half: opening Diversity while RX is open should leave RX open.
Changing `start_diversity_menu()` alone is not enough, because by the time
the operator has clicked **Menu → Diversity**, `new_menu()` has already
destroyed RX at line 524. Three changes are needed:

1. **Split `cleanup()` in `new_menu.c`** into `close_main_menu()` (main
   menu only) and `close_sub_menu()` (the slot plus `active_menu`), and
   keep `cleanup()` as both. The other buttons carry on calling
   `cleanup()`, so choosing any other menu still replaces what was in the
   slot, exactly as now.
2. **Remove the `sub_menu` destroy from `new_menu()`** (line 524). Opening
   the main Menu then no longer closes the open sub-menu. Picking a normal
   entry still does, through `cleanup()`, so the rest of the UI is
   unchanged.
3. **Main-menu close handlers call `close_main_menu()`**, not `close_cb`.
   At the moment `delete_event` and `destroy` both run `cleanup()`, so
   dismissing the main menu, *including destroying it from code*, also
   destroys `sub_menu`. `close_main_menu()` has to set `main_menu = NULL`
   *before* calling `gtk_widget_destroy()`, using the `tmp` pattern the
   sub-menus already use, so the re-entrant `destroy` signal does nothing.

`start_diversity_menu()` then becomes:

```c
void start_diversity_menu(void) {
  close_main_menu();
  diversity_menu(top_window);
}
```

**Side effect to accept or avoid:** with change 2, pressing **Menu**
while RX is open and then dismissing the main menu leaves RX open. Today
RX would be gone. That is arguably better, but it is a visible change to
every menu. If it is unwanted, keep line 524 and accept that RX → Diversity
only keeps RX when Diversity is opened from a `MENU_DIVERSITY` key, not
from the main menu.

### 4.3 What a second press does

Today a second press on a `MENU_DIVERSITY` key closes whatever is open and
reopens Diversity. With the dialog independent, a second press needs a
defined meaning:

- **Toggle** (close if open), which matches `start_band_menu()`,
  `start_mode_menu()` and the other key-driven menus. This is the natural
  choice for a MIDI or GPIO key.
- **Present** (raise it if open), which is the natural choice for the
  main-menu button, since nobody opens the main menu to close something.

Suggested: toggle from the `MENU_DIVERSITY` action and present from the
main menu. That means passing a flag through `start_diversity_menu()`.

**Hold:** Diversity's `cleanup()` releases Hold (`diversity_auto_set_hold(0)`)
and sends `div_send_settings(DIV_ACTION_NONE)` so a remote radio lets go
too. This should not change. At the moment, opening any other menu
silently releases Hold because it closes the dialog. After this change it
won't, which is the point, but it does mean Hold can stay on for as long as
the dialog is open. That is the intended behaviour: the dialog, and with it
the Hold button, stays on screen for the whole time.

### 4.4 Teardown the slot used to handle (`radio.c`, callers)

Anything that used to destroy `sub_menu` took Diversity with it. Once it
is independent, these paths have to close it explicitly:

- **Receiver count drops to 1.** `radio_change_receivers()`
  (`radio.c:1802`) is called from the Radio menu (`radio_menu.c:335`),
  Andromeda (`andromeda.c:171`), the server (`server_thread.c:2031`) and
  props restore (`radio.c:1019`). Before this change, the Radio menu and
  Diversity could never be open together. Afterwards they can, and
  switching to one receiver would leave a Diversity dialog controlling a
  feature that can't run. Call `diversity_menu_close()` in the `case 1:`
  branch. It is idempotent: a no-op when the dialog is shut.
- **Protocol restart / radio stop.** `radio_protocol_restart()`
  (`radio.c:4220`), `radio_stop_radio()` (`radio.c:563`) and
  `radio_stop_program()` (`radio.c:1026`). A dialog open across a restart
  is probably harmless because it reads globals, but closing it is simpler
  and safe.
- **Iconify** (`minimize_cb` → `radio_iconify()`). The dialog is transient
  for `top_window`, so window managers normally minimise it along with the
  main window. Check this on the Pi's WM, and close it explicitly if not.
- **Remote client disconnect.** When the client's connection to the radio
  goes away, the dialog on the client has nothing behind it and should
  close.

### 4.5 Screen space

piHPSDR targets small screens (800×480 Pi panels) as well as desktops. On
a small screen, two dialogs open together will overlap each other and most
of the panadapter. Options, cheapest first:

1. Do nothing. The operator can move windows.
2. Remember the Diversity dialog's position (`gtk_window_get_position()`
   in `cleanup()`, `gtk_window_move()` on open, saved in props), so it
   reopens wherever the operator last put it, out of the way.
3. Make the behaviour optional: a "Keep Diversity menu open" checkbox in
   the dialog, with the current one-menu behaviour when it is off. That
   means keeping both code paths in §4.1 and §4.2, switched on a flag,
   which is more to test.

Suggested: option 2 only if testing on a 7" panel shows the default
placement is poor.

---

## 5. What does not need to change

- **Attenuator and ADC state shared with the RX menu.** The 250 ms poll
  (§3) already brings changes made in RX, sliders or a client into the
  Diversity dialog. The reverse direction, Diversity changing attenuation
  while RX is open, leaves the RX menu's own attenuator widget stale
  until it is reopened. That is a limitation of `rx_menu.c`, and it
  already exists today for the sliders and remote changes. Nothing new is
  introduced.
- **Arm 0 follows RX1's ADC** (commit `04183a32`). Changing the ADC in the
  RX menu calls `rx_change_adc()`, which is what the combiner already
  follows. The Diversity dialog shows no ADC selector of its own, so it
  has nothing to go stale.
- **Remote operation.** `diversity_client_set_settings()` and
  `diversity_menu_refresh()` update whatever dialog is open. They do not
  involve the `sub_menu` slot.
- **`radio_save_state()` on close.** It runs as before, just less often.

---

## 6. Test checklist

- RX open → open Diversity from the main menu: both open.
- RX open → open Diversity from a `MENU_DIVERSITY` key: both open.
- Diversity open → open RX, Radio, Display or Band: Diversity stays open,
  and the other menu replaces anything else in the slot as before.
- Diversity open → press the `MENU_DIVERSITY` key again: it closes
  (toggle). Main-menu button again: it is raised (present). Never two
  copies. Check `status_timer` is not started twice (only one set of
  status updates per 250 ms).
- Diversity open → switch the active receiver: Diversity stays open, and
  RX (if open) closes as now.
- Diversity open → Radio menu → set receivers to 1: Diversity closes.
- Diversity open with Hold on → open and close other menus: Hold stays on.
  Close Diversity: Hold released, on a client as well as on the radio.
- Diversity open → change attenuation from the RX menu or the sliders: the
  Diversity attenuator spins follow within 250 ms.
- Diversity open → protocol restart, iconify, quit: no dangling dialog and
  no GTK criticals on stderr.
- Main menu open → dismiss it with its close button, with RX open
  underneath (only if §4.2 change 2 is taken): RX stays open.
- A client with Diversity open on both ends: controls stay in step in both
  directions.

---

## 7. Why not every menu

Generalising this to all ~39 menus means replacing `sub_menu`/`active_menu`
with per-menu state across every `*_menu.c`. The larger problem is that
most menus capture `active_receiver` when they open and would each need to
rebuild themselves, or close, when it changes. They would also need a
broadcast "state changed" path so that two menus editing the same setting
don't show stale values. Diversity avoids all of this for the reasons in
§3, which is why it is worth doing as a one-off. If a second menu wants
the same treatment later, the §4.1 recipe applies unchanged as long as
that menu does not bind to `active_receiver` when it opens.

# Settings: saved, restored and validated

How the auto-diversity settings get from the props file (or a client) into the engine, and what happens to values that make no sense. LC-001, LC-002 and LC-026 were the natural first group: settings are restored, restored sanely, and the threshold comes from the right slot (LC-026 answers upstream's `THIS MUST BE CORRECTED`). LC-003 and LC-004 are client-side and deferred.

Part of the local-change register: [changes.md](../changes.md).

> **2026-10-15:** every change in this file is in `upstream/TEST`, except any listed as deferred or dropped in the [register](../changes.md#register): LC-001 to LC-047 since `f05546e3` (2026-10-08) and LC-048 to LC-058 since `07ae1f56` (2026-10-15). The text below is the record of what each is and why; "goes upstream", "for a PR" and "Local" in it describe the state before that. See [upstream-reconcile.md](upstream-reconcile.md).

---

<a id="lc-001"></a>

## LC-001 — Restore the saved auto-diversity settings at start-up

**Problem.** `diversity_auto_restore_state()` exists but upstream never
calls it. The auto-diversity settings are saved on exit and never read
back. The per-mode-group settings blocks stay at their static all-zero
initial value. The first mode change loads one of them into the live
settings, and the next save writes zeros out for every group.

**Change.** One line: call `diversity_auto_restore_state()` in
`radio_restore_state()`, next to where the manual diversity gain and
phase are read.

**Upstream (`f05546e3`).** Taken, with the call placed last in the
`!radio_is_remote` block, after `vfo_restore_state()`, instead of beside
the manual diversity gain and phase. Equivalent (the function only reads
the props file, and no receiver exists at either place); his placement
is kept.

**Note.** With this fix, settings saved by earlier builds now load.
Those may include the all-zero blocks, which LC-002 repairs.

---

<a id="lc-002"></a>

## LC-002 — Treat impossible saved values as missing, not clamp them

**Problem.** `div_settings_validate()` clamps every value into its legal
range. An all-zero block (see LC-001) clamps to values that are legal
but useless: 0.2 s averaging (and, then, 3 Hz bins), and a 20 Hz window with Follow
RX filter off. That is what an operator saw on entering auto diversity
in CW.

**Change.** Values that no control can produce are treated as missing
and given their default:

- a non-positive or NaN averaging time or resolution (and hang time,
  until LC-011 pinned it and the repair was removed as dead);
- a window narrower than 20 Hz. That reference goes back to its default
  window, and the live window also goes back to following the RX filter.

The default window widths become named constants (`DIV_WIDTH_DEFAULT`,
`DIV_DIGITAL_WIDTH_DEFAULT`), so the initialisers and the repair agree.
This also repairs props files already written with an all-zero block.

---

<a id="lc-026"></a>

## LC-026 — The live Min coherence comes from the selected reference's slot

**Problem.** Upstream's `f5a0ce9c` moved the per-reference store and
recall out of the engine and into the menu (`store_ref_values()`,
`restore_ref_values()`), which took `div_cohmin_for_ref()` with it. In
`div_settings_load()` the live threshold fell back to the block's live
value, marked `// DL1YCF: THIS MUST BE CORRECTED`. That value need not
belong to the reference the block selects (a props file written before
the per-reference slots, or under another reference), so the new
reference gated on the old one's threshold. The comment above the line
already says why the slot has to win on every path into here: a mode
group's block and a properties restore (LC-001), not only the menu.

**Change.** A switch on `s->ref` takes the threshold from the block's
own slot. The block carries every slot, so this needs nothing from the
menu and does not bring back the engine function upstream removed.
LC-016 makes RADE V1's case 0, and LC-017 adds CW's.

**Upstream.** This is the answer to his comment, and a natural small PR.

**Note.** This replaces what LC-004 did on this line. LC-004's extra
rule (on an unchanged reference, file a client's live value into the
slot first) is a client fix and is deferred with it.

---

<a id="lc-003"></a>

## LC-003 — Start a client's settings block from the settings in force

**Deferred (client), 2026-10-01.** Taken out of the series at the
rebase onto `f5a0ce9c`, kept as tag `backup/TEST-pre-rebase-20261001`.
Upstream put the radio's `CMD_DIV_SETTINGS` handler under `#if 0`, so
the server half now edits dead code. We are not working on client/server
for now; see [open-items.md](open-items.md#clientserver-tracked-not-fixed).

**Problem.** The `CMD_DIV_SETTINGS` wire format carries the live
coherence threshold but not the four per-reference ones. Both receive
sites build a `DIV_SETTINGS` on the stack without filling those four
fields: the radio taking a client's change (`server_thread.c`), and the
client taking the radio's settings (`diversity_client_set_settings()`).
`div_settings_load()` then copies the uninitialised stack values into
the per-reference slots and makes one of them the live gate. Moving any
control on a remote client could leave the radio gating on an arbitrary
threshold, which was then saved to the props file.

**Change.** Both sites fill the block from `diversity_auto_get_settings()`
first, and overwrite only the fields the wire carries. The wire format
does not change.

---

<a id="lc-004"></a>

## LC-004 — Keep a client's Min coherence change instead of dropping it

**Deferred (client), 2026-10-01.** As LC-003. Its slot-wins rule is
carried by LC-026; only the client exception described below is out.

**Problem.** `div_settings_load()` always takes the live threshold from
the selected reference's slot, because after a reference change the
incoming live value may still belong to the previous reference. But the
wire carries only the live value, so a Min coherence slider move on a
client was silently discarded.

**Change.** When the incoming block keeps the reference already
selected, its live threshold is written into that reference's slot
first. It is clamped to the slider's 0–0.95 range, because it has not
been through validation. After a reference change the slot still wins,
as before.

---

<a id="lc-056"></a>

## LC-056 — The client's settings and status adopters move to the engine file

**Why.** dl1ycf asked (2026-10-14) for `diversity_client_set_settings()`
and `diversity_client_set_status()` to move out of `diversity_menu.c`. They
were put there when the idea was that the radio sends settings while the
menu is open and the menu has to adjust. Neither touches a widget: the
first converts the wire block and calls `diversity_auto_apply_settings()`,
the second calls `diversity_auto_apply_status()` and, as written here,
repainted the panadapter once if the overlay had just gone. Both were already declared in
`diversity_auto.h`.

**Change.**
- The two bodies move to `diversity_auto.c`, after
  `diversity_auto_apply_status()`, unchanged. The commented-out
  `//div_populate_from_settings();` (a function upstream removed) is
  dropped, and "and show them" is dropped from the first comment, since
  nothing shows them: an open menu is not repainted.
- `diversity_auto.c` includes `client_server.h` (the wire structs and
  `from_double()`) and `rx_panadapter.h` (`rx_panadapter_update()`);
  `diversity_menu.c` loses its `rx_panadapter.h` include, which nothing
  else there used.
- The 13 test and devtool programs that link `diversity_auto.c` got a stub
  for `rx_panadapter_update()` (LT-030, the commit before this one).

**Taken 2026-10-15 (`07ae1f56`) without the repaint.** dl1ycf took the move
but removed the `rx_panadapter_update()` call, the `overlay_was`/`overlay_now`
test around it and the `rx_panadapter.h` include from
`diversity_client_set_status()`. His reason: the panadapter must not be
redrawn until the new pixel samples are complete (still being computed on the
server, or being received on the client); the new window waits for the next
pixels. `rx_panadapter_update()` draws from `rx->pixel_samples`, and the radio
calls it only under `display_mutex` once `pixels_available` is set. We adopted
his form. What it costs: a client that receives no further spectrum packet
after diversity goes off keeps the last frame, overlay included, until one
arrives. Not measured; display only. The stubs (LT-030) are therefore gone
too (LT-032).

**Checked.** Plain and `make DIVCAP=1` builds, DIVCAP by symbol; the
test and devtool programs build; the suite gives the same result as
before (four known-gap lines); `nm` shows both functions in
`diversity_auto.o` and none in `diversity_menu.o`. The moved text differs
from the removed text only in the lines named above.

**Left to dl1ycf.** Two points about the premise, in
[../menu-notes-dl1ycf.md](../archive/menu-notes-dl1ycf-to-LC-058.md): the radio's own menu
still pushes settings to a watching client (`div_send_settings()`), and the
radio drops the settings a client sends.

---

<a id="lc-061"></a>

## LC-061 — A fresh install seeds AM on Carrier and the digital modes on FSK/Digital

**Status.** Local (2026-10-15). Not sent to dl1ycf.

**Why.** A props file with no diversity settings gave every mode group the Window reference except CW, which had a seed from a sweep (LC-019). Nothing was recorded as a reason for leaving AM and the digital modes on Window; they were simply not seeded. A new user in AM, SAM or DSB has a carrier to lock to, and a new user in DIGU or DIGL has the occupied-bin reference built for them, so Window was the wrong first choice in both.

**Change.** `div_group_seed()` in `diversity_auto.c` is a switch: AM, SAM and DSB take the Carrier reference with its own threshold and hand-placed window (400 Hz); DIGU and DIGL take FSK/Digital with its own threshold and window (2600 Hz). Follow RX Filter stays ticked in every group, so the windows apply only once an operator unticks it; the threshold is live at once. Averaging stays at 2.0 s. SSB, FM and Other still start on Window. The seed applies only when the props file holds no diversity settings; a file with settings of its own keeps them in every mode, as before.

**Not measured.** These two seeds are chosen by what the modes carry. No capture sweep supports them the way CW's 0.2 s does, and neither is a claim about which reference scores best.

**Test.** `test_modal.c` section 6 checks the reference, window and threshold for AM and DIGU on a fresh install.


<a id="lc-062"></a>

## LC-062 — Weighting, Hang, the bin-width setting and RADE V1's threshold are removed

**Status.** Local (2026-10-19). Not sent to dl1ycf.

**Why.** Each of the four had no control and was pinned to a constant by `div_settings_validate()` on the way in, yet still travelled in the settings block, in the props file and on the wire, "so neither changes shape". With the wire layout about to be fixed once (LC-063) they would have been fixed into it for good. The owner's decision: none is coming back.

**Change.** `weighting`, `hang`, `resolution` and `rade_cohmin` leave `DIV_SETTINGS`; `div_auto_weighting`, `div_auto_hang` and `div_rade_cohmin` leave the engine, with their props keys (`diversity_auto_*` and the per-group ones) and their pins in `div_settings_validate()`; `weighting` leaves the context comparison. `div_auto_resolution` stays as a test override only (the test tools fix a 12 Hz width), with no setting, props key or wire field. `DIV_WEIGHT_*` and `DIV_HANG_DEFAULT` are gone. An old props file still has the keys; they are no longer read, so they have no effect. Behaviour is unchanged: flat window, Auto bin width, no hang, RADE V1 ungated. The capture record keeps its shape and writes constants (weighting 0, hang 10). The wire still sends constants for the three fields it had (flat, 10 s, Auto) so this commit changes no layout; LC-063 removes them.

**Checked.** `make`, `make DIVCAP=1` and `make -C test/diversity run` (tooling LT-035 follows the removal). `test_rade`'s two-hang roundtable became one run (the test had already shown hang does not decide the drop).

<a id="lc-063"></a>

## LC-063 — The settings command's final layout, and one version bump

**Status.** Local (2026-10-19). Not sent to dl1ycf; the layout is his to approve, and he asked for it.

**Why.** He reported compiler warnings in `diversity_client_set_settings()`: `band/carrier/digital/cw_cohmin` and `cw_centre/cw_width` are not in `DIV_SETTINGS_COMMAND`, so those parts of the `DIV_SETTINGS` built on the client were uninitialised when `diversity_auto_apply_settings()` read them. He wants the layout fixed now so `CLIENT_SERVER_VERSION` moves once and old clients are not stranded again. The client/server model itself stays deferred (LC-003, LC-004 and the other faults in [open-items.md](open-items.md)).

**Change.**
- `DIV_SETTINGS_COMMAND` in the file's own style (64-bit first, parallel tables): `tau, centre, width, coherence_min`, then `ref_cohmin[8]`, `ref_centre[8]`, `ref_width[8]` indexed by `DIV_REF_*` (`DIV_REFS` = 8; five used), then `mode, ref, follow_filter, hold` and `spare[4]`. 244 bytes, pinned by a `_Static_assert`. The live `centre`, `width` and `coherence_min` are kept beside their slot (see "Alternative" below).
- `diversity_settings_slot()` says which fields of a `DIV_SETTINGS` belong to a reference; the sender and the receiver both use it, so they cannot disagree. RADE V1 has no slot.
- The sender zeroes the whole command first. The receiver starts from `diversity_auto_get_settings()` (the settings in force, not stack), unpacks, and runs `div_settings_validate()`, the check a props file gets.
- `CLIENT_SERVER_VERSION` `0x01310001` to `0x01310002`. This is the one bump: it carries LC-059 and LC-060's renumbering, LC-062's removals and this layout, and an old radio or client now refuses the connection instead of swapping Null and Sum silently.

**Rules for what comes next** (also in the header comment). A new correlator takes the next reference slot and needs no layout change unless it has a threshold or window of its own, which fits a free slot. A flag or enum takes a `spare` byte, defined so that zero means off or default. Anything with a value (the auto attenuator's margin, the ear split's balance) gets a dedicated command, as `client_server.h` already does for each feature; the step attenuator values themselves already travel in the ADC data and, read-only, in `DIV_STATUS_DATA` (`att0`, `att1`). `spare[]` is a small departure from the file's habit (no reserved fields anywhere else); if he prefers strict style it can go.

**Alternative, not taken.** Drop the live `centre`, `width` and `coherence_min` from the wire and derive them from the selected reference's slot on both ends (E3 in [ownership.md](ownership.md)). `div_settings_load()` already takes the live threshold from the slot. It saves 24 bytes and removes a way for the two to disagree, but it changes the menu's store and recall, which are his.

**Not done.** `DIV_STATUS_DATA` is reviewed and unchanged. `div_auto_mode` is still sent twice (`DIVERSITY_COMMAND` and `DIV_SETTINGS.mode`); one source of truth is flagged for him. The radio side of `CMD_DIV_SETTINGS` stays as it is, and no client has run this layout against a radio.

**Checked.** Both builds with no warnings in `diversity_auto.c`; `test_props` gains `test_slots` (LT-036: each reference with a slot gets three distinct fields inside the block, RADE V1 and the unused slots none).

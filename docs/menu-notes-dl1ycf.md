# Diversity menu: pending matters for dl1ycf

Notes for dl1ycf, who looks after `diversity_menu.c`. **Only what is still
open between us.** Everything he has already taken into `upstream/TEST`
(LC-001 to LC-058, with the E1, E2 and other menu write-ups) is kept in
[archive/menu-notes-dl1ycf-to-LC-058.md](archive/menu-notes-dl1ycf-to-LC-058.md)
and in the write-ups under [changes/](changes/).

The agreed rule behind all of it:

> The engine writes a setting only at start-up (props file) or when a
> client takes the server's settings. Any other setting the menu can
> change, the menu owns.

Register: [changes.md](changes.md); the rules are in
[changes/ownership.md](changes/ownership.md). Four items below, in the order
we would like a decision (the new one, RADE V2, is last and is on a branch, not
yet a request).

---

## The diversity settings command: layout fixed, one version bump (LC-063)

Your warning is right: `band/carrier/digital/cw_cohmin` and `cw_centre/cw_width` were
read by `diversity_auto_apply_settings()` but not in `DIV_SETTINGS_COMMAND`, so
the client's `DIV_SETTINGS` held stack garbage there. We took the chance to fix
the whole layout once. **Nothing here implements client/server**; it is the
struct, its pack and unpack, and the version.

What changed (LC-062 first, LC-063 on it):

- Weighting, Hang, the bin-width setting and RADE V1's threshold are gone from
  the settings block, the engine, the props file and the wire. None had a
  control and each was pinned already (LC-062).
- The command is now (64-bit first, as the header comment asks): `tau, centre,
  width, coherence_min`; three parallel tables `ref_cohmin[8]`, `ref_centre[8]`,
  `ref_width[8]` indexed by `DIV_REF_*`; then `mode, ref, follow_filter, hold`
  and `spare[4]`. 244 bytes, with a `_Static_assert`. A new correlator takes the
  next reference slot; RADE V1 has none.
- `diversity_settings_slot()` is the one place that says which fields are a
  reference's; the sender and the receiver both use it.
- The sender zeroes the command; the receiver starts from the settings in
  force and runs `div_settings_validate()` like a props file.
- `CLIENT_SERVER_VERSION` is `0x01310002`: one bump for this layout, LC-059's
  and LC-060's renumbering and LC-062's removals. Choose the number you like.

Two points for you. **`spare[4]`**: your file has no reserved fields; features
with values (an auto attenuator, an ear split) would get their own command, and
only flags or enums would use a spare byte. Drop `spare` if you prefer. **The
alternative we did not take:** the live `centre`, `width` and `coherence_min`
are redundant with the slot of the selected reference (our E3 above), so they
could leave the wire (24 bytes) and be derived on both ends; that changes the
menu's store and recall, so it is your call. Also flagged: `div_auto_mode` is
sent in both `DIVERSITY_COMMAND` and `DIV_SETTINGS.mode`.

---

## The objective and reference lists are in enum order (LC-059, LC-060)

Two small changes, one request (the objectives read Manual, Sum, Null, Best),
for you to take or not. They touch `radio.h`, `diversity_auto.h`,
`diversity_auto.c` (one line) and `diversity_menu.c`.

- **LC-059.** `DIV_AUTO_*` in `radio.h` is Manual (0), Sum, Null, Best. The
  four `append_text` lines in the objective combo are in the same order. The
  combo already used the enum value as its row, so nothing is mapped.
- **LC-060.** `DIV_REF_*` in `diversity_auto.h` is Window (0), CW, FSK/Digital,
  Carrier, RADE V1, which is the order the "Measure on" list already showed.
  That removes `div_ref_rows[]`, `div_ref_to_row()` and `div_row_to_ref()`: a
  label array indexed by the enum fills the combo and a row is a reference.
  `ref_changed_cb()` keeps a guard for "no row selected".
- **The one line in the engine:** `div_settings_validate()` checked
  `s->ref > DIV_REF_CW`, written when CW was the last value. It is
  `DIV_REF_RADE_V1` now. Anyone reordering the enum again has to move that
  check, which is why the comment in the header says the last enumerator is the
  one it names.

**What changes for stored data.** Sum and Null swap numbers (1 and 2), and
four reference numbers move (Carrier 1 to 3, RADE V1 2 to 4, FSK/Digital 3 to
2, CW 4 to 1). We did not migrate: a props file written before this loads
with those read the other way until it is reset, and we accept that on TEST.
You may want the same, or a marker key; it is your file format.

**The question that is yours.** Without a version bump, a radio and a client
built on either side of this change swap Null and Sum and move four references
without either noticing. LC-063 (below) bumps it, with the layout settled at the same time.

Not checked on screen: each row of both lists against the objective or
reference it starts, and the Invert button's Null/Sum swap.

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

## RADE V2 on the reference list (LC-064, LC-066, LC-067): for later

Not a request yet. We are adding a RADE V2 reference (cyclic prefix, no pilot)
beside RADE V1, and it will reach you as a patch once it has been on air. What
it does to your files, so that nothing is a surprise:

- **`diversity_auto.h`.** `DIV_REF_RADE_V2` is appended as reference 5, and two
  macros: `DIV_REF_IS_RADE(ref)` (V1 or V2) and `DIV_REF_LAST` (the range
  check's end). The settings command does not change: `DIV_REFS` is 8, V2 has no
  slot of its own (as V1 has none), and an older build that meets 5 resets it
  to Window in `div_settings_validate()`.
- **`diversity_menu.c`.** One label (`"RADE V2 prefix (blind)"`) and the places
  that test `div_auto_ref == DIV_REF_RADE_V1` to mean "a RADE reference" now
  call `DIV_REF_IS_RADE()`: the level control, the blank coherence field, the
  Sum default on selection, the hidden Min coherence row (twice), and the
  per-reference store and recall. The status line says "RADE V1" or "RADE V2".
  That is the whole menu diff; it is its own commit (LC-067) so you can take it
  or leave it.
- **`rx_panadapter.c`.** A case for V2's occupied band (1031 to 1906 Hz),
  beside V1's.

Question for you, when it comes: the helper macro is the part that touches
existing V1 lines. If you would rather not have it, V2 can be a second
`== DIV_REF_RADE_V2` test at each place instead, at the cost of a longer diff.


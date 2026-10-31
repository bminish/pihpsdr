# Diversity menu: notes for dl1ycf, LC-059 to LC-063 (archived)

Archived 2026-10-30: he took all of this in `30b327e2` (2026-10-27), unchanged.
Earlier notes: [menu-notes-dl1ycf-to-LC-058.md](menu-notes-dl1ycf-to-LC-058.md).
Register: [../changes.md](../changes.md).

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


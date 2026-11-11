# Diversity menu: pending matters for dl1ycf

Notes for dl1ycf, who looks after `diversity_menu.c`. **Only what is still
open between us.** Everything he has already taken into `upstream/TEST`
(LC-001 to LC-063) is kept in
[archive/menu-notes-dl1ycf-to-LC-058.md](archive/menu-notes-dl1ycf-to-LC-058.md),
[archive/menu-notes-dl1ycf-to-LC-063.md](archive/menu-notes-dl1ycf-to-LC-063.md)
and in the write-ups under [changes/](changes/).

The agreed rule behind all of it:

> The engine writes a setting only at start-up (props file) or when a
> client takes the server's settings. Any other setting the menu can
> change, the menu owns.

Register: [changes.md](changes.md); the rules are in
[changes/ownership.md](changes/ownership.md). Three items below (E3 to E5), plus one small flag, in the order
we would like a decision; RADE V2 is last and is on a branch, not yet a request.

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
- **One flag from LC-063:** `div_auto_mode` is sent in both
  `DIVERSITY_COMMAND` and `DIV_SETTINGS.mode`. One source of truth would be
  tidier; it is in your files.

---

## RADE V2 on the reference list (LC-071, LC-073, LC-074): for later

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
  That is the whole menu diff; it is its own commit (LC-074) so you can take it
  or leave it.
- **`rx_panadapter.c`.** A case for V2's occupied band (1031 to 1906 Hz),
  beside V1's.

Question for you, when it comes: the helper macro is the part that touches
existing V1 lines. If you would rather not have it, V2 can be a second
`== DIV_REF_RADE_V2` test at each place instead, at the cost of a longer diff.


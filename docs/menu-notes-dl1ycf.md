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
we would like a decision.

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

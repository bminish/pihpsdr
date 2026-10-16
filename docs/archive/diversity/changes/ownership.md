# Who owns what: menu and engine (dl1ycf, 2026-10-01)

Part of the local-change register: [changes.md](../changes.md).

dl1ycf proposed a division of work, and we follow it. He mostly takes
`diversity_menu.c`; we take `diversity_auto.c`. The rules that make that
workable are coding rules for every change from now on:

1. **Settings come from two places only.** A setting is read from the
   props file at start-up, or taken from the server when a client starts.
   Both can be done in `diversity_auto.c`.
2. **Apart from that, the engine never writes a setting the menu can
   change.** If the algorithm needs a value of its own, it keeps it in a
   separate variable, so "what the operator said" and "what the algorithm
   is doing" stay distinct. The model is upstream's `man_div_gain` (the
   operator's) versus `auto_div_gain` (the loop's).
3. **No substitution.** If the engine can't do what the operator asked,
   it doesn't change the setting to something it can do. dl1ycf's
   example: a failed `rade_corr_start()` must not set `div_auto_ref` to
   `DIV_REF_DIGITAL_IQ`. The failure must have no blast radius. It may
   be silent if it can't happen in practice: no status flags or UI for
   impossible cases (decided 2026-10-01).
4. **Mode-dependent settings go into the per-mode settings** handled by
   `profiles.c` (`RXTXprofile[]`; dl1ycf's mail calls it
   `mode_settings[]` / `profile.c`). This replaces calling
   `diversity_auto_mode_changed()` from `rx_mode_changed()`. It's a
   long-term goal.
5. **Local first, client/server later.** Once that works, a client
   sends only the parameters that changed, and the server does not reply.
   This matches [open-items.md](open-items.md#clientserver-tracked-not-fixed).

In practice:
- A change of ours that needs the menu to do something is written up for
  dl1ycf rather than committed into `diversity_menu.c` behind his back.
- Where an engine function computes a value for the menu (a seeded
  window, say), it returns the value and the menu stores it.
- Engine-to-menu calls (`g_idle_add` of menu functions) are not added.
- What each engine change needs from the menu is written up for dl1ycf
  in [menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md). We still make the
  menu half in our own LC, so `TEST` works, but we keep it minimal so
  his rewrite can redo it.

## Review: E1 to E8

Against these rules, 2026-10-01, `TEST` at `4f79c0be`. Status updated as items are done.

**What already complies:**
- The menu writes the settings globals itself. It then calls the engine
  to act on them: `diversity_auto_restart()`, `_reset()` and
  `_invert()`.
- The worker thread only reads settings.
- The weight is split (`man_div_gain` / `auto_div_gain`).
  `diversity_auto_att_changed()` and `_invert()` touch only `auto_div_*`.
- Status (`div_auto_coherence`, `_clamped`, `_arm_*`, `div_norm`, ...)
  is engine-owned output, not settings.
- The engine includes `diversity_menu.h` but no longer calls into the
  menu.

**Where the engine writes operator settings:**

| # | Where | What it writes | Origin | Proposed handling |
|---|---|---|---|---|
| E1 | `diversity_auto_start()`, RADE start failure | `div_auto_ref` → `DIGITAL_IQ` | upstream (our original code) | **Done: LC-031.** Don't substitute: keep the reference and hold, with no weight. The failure is silent apart from `rade_corr_start()`'s own log line. It can't fire today (every DDC rate is a multiple of 8 kHz), so a status flag isn't worth having (decided 2026-10-01); what matters is that it has no blast radius. |
| E2 | `diversity_auto_seed_window()` | `div_auto_centre`, `div_auto_width` | LC-009 | **Done: LC-032.** Make it pure: `int diversity_auto_seed_window(double *centre, double *width)` returns 1 and fills the values, and the menu stores them. This needs a two-line change in `follow_cb` (dl1ycf's side). |
| E3 | `div_settings_load()` | the live `div_auto_coherence_min`, from the reference's slot | LC-026 | The root problem is duplicated state: a live copy *and* per-reference slots for the window and threshold. The menu stores and recalls them by copying (LC-008). Proposal: the engine reads the selected reference's slot directly, and the live copies go away. That's a joint change, to discuss with dl1ycf. Until then, LC-026 stays as the bridge. |
| E4 | `diversity_auto_set_hold()` | `div_auto_hold` | upstream; LC-007 | The menu and `radio_set_diversity()` call it, so the engine never decides on its own. Cleaner: the caller writes `div_auto_hold`, and the engine gets `diversity_auto_hold_changed()` for the weight handover (`div_jump`). Low priority. |
| E5 | `diversity_auto_mode_changed()` + `div_group_*` | every setting, on a mode-group change | upstream (ours originally); LC-019 seed | Rule 4. Move the per-group blocks into `RXTXprofile[].rx` and save and restore them in `profiles.c`. Then the engine only reacts (restart and reset) when told the settings changed. This is the big one. See the notes below. |
| E6 | `diversity_auto_apply_settings()` | every setting, from a block | upstream; client path | Client/server, deferred. Under rule 5 it is replaced by per-parameter commands. The radio-side "draw the consequences" logic stays in the engine. |
| E7 | `diversity_auto_apply_status()` (client) | `diversity_enabled`, `adc[].attenuation` | upstream | Client/server, deferred. |
| E8 | `diversity_auto_restore_state()` / `div_settings_validate()` | every setting, validated and pinned | LC-002, LC-006, LC-011, LC-016, LC-019 | **Allowed** (rule 1: start-up from the props file). The pinning in `div_settings_load()` (RADE cohmin 0, the live threshold) also runs on every mode change. That goes away with E5. |

**Notes on E5 (mode settings into `profiles.c`):**
- **Grouping: diversity takes `profiles.c`'s (decided 2026-10-01).**
  `profiles_copy_rxtxprofile()` keeps LSB/USB/DSB, CWL/CWU and DIGL/DIGU
  together, and every other mode on its own. Two things change from our
  `div_group_of_mode()`:
  - DSB moves from the AM group to the SSB group.
  - AM, SAM, FMN and SPEC each get their own settings instead of
    sharing.

  Migration from the old per-group keys:
  - LSB, USB and DSB are seeded from the old SSB group;
  - AM and SAM from the old AM group;
  - FMN from the FM group;
  - every other mode from the "other" group.
- **The block** (`DIV_SETTINGS`, about 24 fields) would become a
  `struct _rxprofile` member. The props keys change from
  `diversity_group[%d].*` to `modeset.%d.*`. A one-time migration from
  the old keys keeps operators' settings.
- **The CW seed (LC-019)** becomes a default in `RXTXprofile[modeCWU/L]`.
- **Profile loading** (`profiles_load_rx_profile()`) then writes the
  settings, and the engine is told to re-read them (restart if the
  objective, resolution or RADE use changed; reset otherwise). That is
  the same three-condition rule it already applies, but triggered by
  the profile load instead of by `rx_mode_changed()`.
- It also fixes "a mode change with the menu open doesn't refresh it"
  (see [open-items.md](open-items.md#flagged-for-a-later-patch)), if the profile load refreshes open
  menus as it does for other per-mode settings.

**Menu-side LCs.** These touch `diversity_menu.c`, so they now overlap
with dl1ycf's work: LC-006, LC-007, LC-008, LC-009, LC-011, LC-012,
LC-014, LC-015, LC-016, LC-017, LC-020, LC-024, LC-030, LC-032, LC-034,
LC-039 and LC-040, plus the capture tooling. At the next re-sync, expect his rewrite of the menu to
replace their menu halves. Each LC's engine half should keep working
against whatever globals the menu sets.

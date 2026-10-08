# Settings: saved, restored and validated

How the auto-diversity settings get from the props file (or a client) into the engine, and what happens to values that make no sense. LC-001, LC-002 and LC-026 were the natural first group: settings are restored, restored sanely, and the threshold comes from the right slot (LC-026 answers upstream's `THIS MUST BE CORRECTED`). LC-003 and LC-004 are client-side and deferred.

Part of the local-change register: [changes.md](../changes.md).

> **2026-10-08:** every change in this file that is listed in [accepted.md](accepted.md) is in `upstream/TEST` (most since `f05546e3`); the pending ones are in the [register](../changes.md#register). The text below is the record of what each is and why; "goes upstream", "for a PR" and "Local" in it describe the state before that. See [upstream-reconcile.md](upstream-reconcile.md).

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

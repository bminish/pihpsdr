# Bug: CW filter edges drift by one sidetone offset per encoder click

Found 2026-11-10 while testing the Soomfon CN003 MIDI controller on the multifunction encoder. Written up here because it was found during the binaural diversity work and the ear split's filter follow (below) depends on it.

## Summary

In CWU and CWL, every incremental filter edit (Filter Cut Low, Filter Cut High, IF Shift, IF Width) moves the passband by one extra sidetone frequency, always in the same direction, whichever way the encoder is turned. A few clicks put the passband far from the signal. SSB, AM and the other modes are not affected.

## Symptom

The encoders for filter cut, IF shift and IF width "all head in one direction, away from the passband". AF gain and RF gain on the same encoder path behaved correctly, which ruled out the MIDI side (CC 65/63 in offset mode, matching the stored ranges lft 0-63 and rgt 65-127).

## Cause

Two conventions disagree in `src/filter.c`.

- `filter_edges_changed(id, low, high)` takes edges **relative to the nominal CW position** and adds the sidetone (CWU) or subtracts it (CWL) itself before storing them in `rx->filter_low` / `rx->filter_high`.
- `rx->filter_low` / `rx->filter_high` therefore hold **absolute** edges, sidetone included. `receiver.c` sets them the same way (`+ cw_keyer_sidetone_frequency` for CWU, `-` for CWL).
- `filter_high_changed`, `filter_low_changed`, `filter_width_changed` and `filter_shift_changed` read the absolute edges, adjust them by the increment, and pass them straight back to `filter_edges_changed()`, which adds the sidetone a second time.

Each call therefore adds `+sidetone` to both edges in CWU, or `-sidetone` in CWL, plus the intended step. The offset is independent of the sign of the increment, so turning the knob either way pushes the passband the same way. `filter_cut_default()` and the TCI `rx_filter_band` command pass relative edges and were already correct.

Present since `ee7a2fa3` (Big update to Version 3, 2026-06-20); `a9031cdb` only reformatted it.

## Fix

A static helper `filter_abs_edges_changed()` in `src/filter.c` subtracts the sidetone (CWU) or adds it (CWL) before calling `filter_edges_changed()`. The four incremental handlers call it. Other modes pass through unchanged. The convention of `filter_edges_changed()` itself is untouched, so its other callers are unaffected.

## Status

Checked on the radio 2026-11-11: the filter cut, IF shift and IF width fix and the ear split following the edits and squelch both pass, for now. The check was: in CWU and CWL, turn Filter Cut Low/High, IF Shift and IF Width both ways and confirm the edges follow the direction of the turn; one SSB mode to confirm nothing changed there.

## Related: the ear split did not follow these edits

Found in the same pass. With the binaural ear split active (diversity on, split not off, one RX on screen), RX1 is a hidden second ear. The incremental filter edits and squelch did not reach it, so RX1 kept its old passband and squelch while RX0 changed. Fixed alongside:

- `rx_split_follow_edges()` (`src/receiver.c`), called from `filter_edges_changed()`: copies the edges to RX1, applies the bandpass and, in CW, the peak filter. Not a full clone, because it runs on every click.
- A hook at the end of `rx_set_squelch_for()` for RX0 that copies squelch and its enable flag to RX1, in the same style as the existing noise, notch and AGC hooks.

Already carried to RX1 before this: AF gain, AGC gain and mode, noise reduction and blanker, notches, mode, offset and RIT. Not checked: RF gain and attenuation, which are per ADC rather than per receiver.

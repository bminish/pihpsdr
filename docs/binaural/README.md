# Binaural diversity on TEST (`feature/diversity-binaural-2`)

This branch is `TEST` plus the diversity ear split (binaural presentation of
the two diversity arms), ported from `history/diversity/binaural` (was `feature/diversity-binaural`). It is
rebased onto `TEST` as a short series on top, the way `TEST` itself follows
`upstream/TEST`. Work here is meant to reach `TEST` later, in stages.

Two rules differ from `TEST` (see `docs/changes/rules.md` and
`docs/changes/ownership.md` for the rest, which still apply):

- **Client/server must work here.** On `TEST` it is tracked but not fixed
  (`docs/changes/open-items.md`). Here it is the next piece of work, after
  the local ear split works.
- **`diversity_menu.c` may be changed freely.** On `TEST` it is left to
  dl1ycf.

`history/diversity/binaural` (was `feature/diversity-binaural`) is kept unchanged as the reference.

## The series so far

| Commit | What |
|---|---|
| `2ad53789` | The ear split, ported from binaural `2a6bafb6..b0e57819` onto TEST's shapes (manual/auto weights, LC-022 arm swap, LC-030 Level output, parallel RX slew-down, one menu row) |
| `da0956b1` | The second ear follows RX1's CTUN, RIT and CW BFO shift (binaural carried this inside its CW commit `6027208a`; the port missed it) |
| `8d27b0cc` | The split's teardown waits for the second ear to stop (TEST's `rx_off()` honours its wait argument; binaural's did not) |
| `0fd92651` | PipeWire output declared as FL/FR, not MONO/MONO+1 (upstream bug since `184f780b`; affects every stereo use of PipeWire output) |
| `854cc90d` | The ears' input blocks are realigned on the receive thread, not the GTK thread |
| `8fe6222c`, `4ba041c1` | Tooling: the ear recorder and `ears.py` (below) |
| `92dd3bf5`, `2dd0e077` | One AGC gain for both ears under the split, and a menu toggle for it (the paired AGC) |
| `e798f1d1` | The paired AGC moves into its own file `wdsp/wcpAGCpair.c` (additive: `wcpAGC.c` keeps two one-line hooks); the **stronger** arm sets the gain, not the average; link and unlink ordered so the feeding mode and the WDSP link never disagree (leaving binaural had hung); the AGC slider sends only the gain and applies at most every 50 ms |
| `e9630a2a` | "Balance ATT" button: one press, once, from the noise floor |

## Debugging and test suite for the ear split

This is the start of the test suite for the audio path of the split. Use it
before reasoning about what was heard: impressions of "phase", "delay" and
"missing" have each turned out to have a different cause from the one
first guessed (2026-10-02/03).

**What exists:**

0. **The WAV button** (`make DIVCAP=1` only, like Capture: it is a test
   feature and travels with it in the code; Diversity menu, "Record:"
   row): RX1's audio output as passed to `audio_write()` but **before the
   AF gain** (inverse of the panel gain, times upstream capture's 0.6),
   both channels, **16-bit PCM**, 48 kHz, streamed to
   `wav/<YYYYMMDD-HHMMSS>_<kHz>_<mode>_<presentation>.wav` with the
   per-block CSV beside it (presentation: `rx1`,
   `div-manual|null|sum|best`, `div-per-ear`, `div-sum-diff`, as at the
   start). Stops on a second press or when the dialog closes. No length
   limit. Clipped samples are counted in the log. `src/ear_record.c`.
   16-bit because the first recording (CW, AGC on) spanned about 30 dB,
   -46 to -15 dBFS, against 16-bit's -95 dBFS floor; float's headroom
   was never used. Tested standalone: 30 s through the 11 s ring came
   back within half an LSB, clipping counted.
1. **The ear recorder beside a capture** (`make DIVCAP=1` only; the same
   `src/ear_record.c`, hooks in `rx_process_buffer()` in `src/receiver.c`).
   Armed and stopped with the Capture button (now on the "Record:" row, so
   reachable in Manual; it switches diversity on in the last objective if
   it is off, and in Manual starts the analysis thread for the capture
   alone, recording the manual weight as the live one), beside the I/Q capture of the
   same stamp:
   - `captures/ears-<stamp>.wav`: as the WAV button's file (pre-AF,
     16-bit, 48 kHz). Covers summed and both split
     modes.
   - `captures/ears-<stamp>.csv`: one row per `rx_process_buffer()` pass:
     `who` (receiver), `mode` (0 summed, 1 antenna per ear, 2 sum/diff),
     `paired` (on RX1's split pass, whether RX0's half was there; the right
     ear is dropped for the block if not), `rx1_cnt` (on RX0's pass, RX2's
     sample counter when RX0's buffer filled: **1023 = input blocks
     aligned**, anything else is the offset), `nsamp`, `bal_l`, `bal_r`,
     `af_db` (the AF setting taken out).
   - No I/O on the receive thread: a writer thread streams the files.
     Only one recording at a time: Capture's ears or WAV, whichever began.
2. **`test/diversity/devtools/py/ears.py`**: per mode run and per segment,
   each ear's level, the L/R lag maximising |cross-correlation| and the
   signed correlation there (negative = inversion), plus the counter
   values and dropped blocks. Reads any stereo WAV, so it also takes a
   sink-monitor recording (no CSV: one run). Checked on a synthetic file
   with a known 37-sample delay and inversion: both recovered exactly.
3. **What reaches the device**, recorded in parallel from the sink's
   monitor (command not yet tried here):
   `pw-record -P '{ stream.capture.sink=true }' --target <sink node.name> --format f32 captures/monitor.wav`.
   Our recording right and the monitor wrong puts the fault after
   piHPSDR; our recording wrong puts it in the split.

**Not yet covered:** the I/Q capture and the ear recording are not
time-aligned to the sample (only by stamp); nothing replays a capture
through the split offline; TCI's pair is not recorded.

## The AGC link and Balance ATT: state

Built and in use, not registered: no LC numbers yet, by decision (this
may be a dead end; run it for a while first).

- **Paired AGC.** The state machine runs once per sample on the larger of
  the two arms' peaks and back-averages, and the one gain goes to both
  ears. `wcpAGCpair.c` holds a *copy* of `xwcpagc()`'s per-sample
  arithmetic: if upstream changes it, the copy follows by hand. A linked
  channel whose partner is not running (off, slewing down, bypassed) runs
  alone at once; otherwise it would wait out the 100 ms timeout on every
  block and overflow the input. The ears must be fed in pairs while the
  AGCs are linked, which is what `rx_link_agc()` orders.
- **Balance ATT** (Diversity menu, under the ATT sliders). Asks the
  engine for the noise floor for 4 s, reads it, and puts the difference
  on the hotter arm's ADC (arm to ADC through `div_arm_swapped()`),
  capped at 31 dB, then re-reads after 5 s and reports the residual.
  Floor only: the overload flag is not used, since impulse noise trips it
  with no effect on reception. The floor exists in the Window, Carrier,
  CW and FSK/Digital references; **RADE V1 never transforms**, so for it
  the engine windows and transforms the blocks for the length of the
  request (`diversity_auto_noise_floor_demand()`), and digital's missing
  floor update was added.
- **Checked, and not:** `diversity_auto_att_changed()` chooses the sign of
  its weight rescale from the ADC index and does not read the arm swap.
  Capture T-022 (`docs/test-findings.md`) shows it right with RX1 on
  ADC1: arm 0 attenuated 14 dB, the held weight moved by exactly -14 dB.
  With RX1 on ADC2 it looks wrong and no capture covers it.
- **Test captures:** RADE V2 on LSB, four captures and a binaural WAV,
  recorded in `docs/test-findings.md` T-019 to T-023, for the diversity
  RADE V2 receive chain.

## Observations and open items

- **2026-10-03, operator by ear:** in the tracking modes (Null, Sum, Best)
  the **Antenna per ear** presentation inherits the correlator's work, heard
  as an instability; in Manual it does not. May be logical behaviour.
  Debugging deliberately left for now. Not traced: in the code, Antenna
  per ear feeds RX1 the raw arm 0 and RX2 the raw arm 1 with no weight
  applied, so the path by which the loop reaches the ears is not yet known.
  The recorder above is the tool to start from.
- Whether `0fd92651` (PipeWire FL/FR) changed what is heard: the operator
  reported it "may have changed it", not resolved it. Not measured.
- "Sum L / Difference R" is misnamed in Null, where the weight cancels, so
  the "sum" ear carries the null. Rename pending.
- Props from binaural carry its live auto weight in `diversity_gain` /
  `diversity_phase`, which TEST reads as the **manual** weight (seen:
  -27 dB, -185 degrees).
- TCI: upstream now allows a one-channel stream, which takes the left
  channel only; under the split a mono TCI client hears the left ear only.
- Client/server: the split is local to the radio (greyed on a client).
  Next stage of work.

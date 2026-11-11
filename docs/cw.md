# CW keying and side tone: what this branch has, where it is written down

Written 2026-11-09, checked against the code 2026-11-11 (on `devtest/everything`; the branch it came from, `feature/cw-synth-sidetone`, is retired, tag `backup/cw-synth-sidetone-20261111`). One machine (USB sound card, Saturn G2, PipeWire), one operator's PC: none of the figures in these documents is a general result.

## The changes

| Number | What | Document |
|---|---|---|
| LC-064 | Local CW side tone synthesised in the PipeWire output callback (option, off by default) | [cw_sidetone.md](cw_sidetone.md) |
| LC-065 | A short paddle tap is no longer lost while the keyer waits for TX | [cw_keyer_first_tap.md](cw_keyer_first_tap.md) |
| LC-066 | Straight key: the start of a key hold is no longer lost in the RX to TX timing hole | [cw_keyer_straight_key.md](cw_keyer_straight_key.md) |
| (fix) | A CW filter edge drift: every filter cut / IF shift / IF width step in CWU or CWL moved the passband by one sidetone offset. Fixed in `filter.c`; checked on the radio 2026-11-11, passes | [binaural/bug-cw-filter-edge-drift.md](binaural/bug-cw-filter-edge-drift.md) |
| LT-039 | Tools and tests: `tools/cwlat` (measurement, incl. `midisend`), `test/sidetone`, `test/breakin`, the timing probe `src/cwtime.[ch]` | [tools/cwlat/README.md](../tools/cwlat/README.md) |

**Numbering.** LC-064 to LC-066 are this series in the register ([changes.md](changes.md)); the RADE V2 work that once clashed with them was renumbered LC-071 to LC-075 on 2026-11-11 ([changes/lc-rade.md](changes/lc-rade.md)). LT-039 was LT-038 before 2026-11-11, which the RADE V2 tooling holds in the register. The side tone alone, as a patch series on `master`, is the branch `feature/synth-sidetone`; the lost-first-tap fix alone is `fix/keyer-lost-first-tap` ([branches.md](branches.md)).

## The other documents

| Document | What it is |
|---|---|
| [cw_sidetone-design.md](cw_sidetone-design.md) | Design record of the side tone: problem, idea, the points that were revised, risks |
| [cw_sidetone-findings.md](cw_sidetone-findings.md) | All measurements of the side tone, RF shape, PTT mode and the external keyer path |
| [cw_timings.md](cw_timings.md) | What the code says about every CW timing, and where each has been measured; review findings |
| [cw_breakin_plan.md](cw_breakin_plan.md) | The break-in timer: what is wrong, the plan, phase 0 (done) and its baseline |

## Numbers at a glance (22 wpm, 1 % drive into a dummy load, MIDI press to output)

| | Value | Where |
|---|---|---|
| Side tone at the sink, warm, synth path | 5.0 ms at quantum 64, 7.0 at 128, 10.9 at 256 (old path: 8 ms more at each) | cw_sidetone.md |
| RF carrier, warm | 22.5 to 22.8 ms, independent of audio path and quantum | cw_sidetone-findings.md |
| First element after a pause (break-in) | RF 43.3 ms, side tone 27.8 ms; PTT mode: no difference from warm | cw_sidetone-findings.md |
| Cold start, composition | `rxtx()` 18.6 +/- 3.0 ms plus the idle queue 5.4 ms (0 to 44, the source of the spread) | cw_breakin_plan.md section 9 |
| Hang time | the radio drops after about 0.56 of the set value (counter runs at twice the rate) | cw_timings.md |
| Element length error, RF edges | -0.17 ms; 5.0 ms rise and fall | cw_sidetone-findings.md |
| Taps lost | unchanged keyer: 9 of 120 in the hang test; with LC-065: 0 of 120 (straight key: LC-066) | cw_keyer_first_tap.md |

## Conditions to know when reading a number
* **Quantum.** `TEST` requests 256. This branch requests 128 as a local setting (`pipewire.c`), and the graph quantum is whatever the driver node ends up with: on the test machine it had to be forced (`pw-metadata -n settings 0 clock.force-quantum N`), a runtime setting that is lost when PipeWire restarts. Numbers say which quantum they were taken at.
* **The sink must be silent** for cwlat's own sink figures (receiver audio on the sink hides the tone from its detector): `tools/cwlat/README.md`. RF figures (KiwiSDR, GNSS-stamped) are not affected.
* **Results before 2026-11-09** for the squeeze test and for PTT mode were invalid (a fault of the test sender, `tools/cwlat/README.md`); the documents carry the corrected runs.

## Open items (the one list; the other documents point here)
1. **The hang counter runs at twice the rate.** `CHECK` and `EXITLOOP` both decrement `cwvox`. A separate patch (decision pending: it changes every user's stored hang time) is phase 1 of [cw_breakin_plan.md](cw_breakin_plan.md).
2. **The break-in timer**: stale TX-off timer, `moxbefore` latch, hang 0, the cold start (phases 2 to 5 of the plan; none of it implemented beyond the phase 0 tools).
3. **Should the side tone follow the key without PTT** (zero beating)? Today neither path plays a tone in RX ([cw_sidetone-design.md](cw_sidetone-design.md#open-question-2026-11-09-should-the-side-tone-follow-the-key-without-ptt)). Decision pending.
4. **Side tone before upstream** ([cw_sidetone.md](cw_sidetone.md)): the bench checklist (CAT CW, TUNE and SWR tones, volume, duplex, mode change, active receiver change, device removed), steady-state XRUN counts, the other audio backends and client/server, a persistent quantum setting.
5. **Not measured anywhere:** a real paddle or key, other speeds and bands, key-click level (the Kiwi's AGC defeated it), levels above 1 % drive, the ear-level delay (USB and DAC).
6. **Straight key in break-in mode** has not been re-run on this branch since LC-066 (PTT mode has).

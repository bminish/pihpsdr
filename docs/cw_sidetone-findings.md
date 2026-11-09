# CW keying latency, jitter and shape - findings, 2026-11-06 to 2026-11-09

Index and open items: [cw.md](cw.md). The change itself: [cw_sidetone.md](cw_sidetone.md) (LC-064). **One machine, one USB sound card, one radio, one operator's PC:** none of the figures below is a general result. The quantum sweep and the keying shape are from 2026-11-06; the sink check, PTT mode and the external keyer path are from 2026-11-09 (quantum 128).

## Summary
* **Where the delay was.** On the test machine the PipeWire graph ran at 1024 frames although pihpsdr asked for 256 (the USB sink was the
  driver); the sidetone then waited 21 ms for a cycle plus the sink's own 512-sample headroom, plus the 6.7-8 ms side tone FIFO in `pipewire.c`.
  Forcing the quantum (`clock.force-quantum`) made the sidetone "much tighter".
* **What the patch does.** The local side tone is generated inside the PipeWire output callback instead of being queued through the FIFO
  (`cw_sidetone.md`). Key-to-sink delay falls by about **8 ms at every quantum** (warm taps, measured at the sink monitor):
  13.2 -> 5.0 ms at 64, 15.0 -> 7.0 at 128, 19.0 -> 10.9 at 256. Element lengths are unchanged (counted in output samples).
* **RF is not affected by the audio path or quantum:** MIDI press to RF carrier is 22.5 ms (sd 0.6-0.9) in every combination.
  The sidetone therefore leads the RF by 12-18 ms with the patch, but lags it by 4.5 ms on the old path at quantum 256 (estimated at the ear).
* **Break-in costs more than the audio path.** The first element after a pause starts about 21 ms late (RX->TX), and a straight key then loses
  that time from the front of the element (**20 ± 8 ms shorter, worst 45 ms**). Taps shorter than the switch-over are lost altogether.
* **Keying shape is clean:** the RF edge is the designed DL1YCF 9 ms ramp to within 0.5 % rms; shortened elements are normal elements that
  start late, not hard-cut; no chirp, ripple 0.3 %. The wideband (key-click) level could not be quantified with these captures (see below).
* **Quantum.** The playback quantum is upstream's decision (256) and is not changed by LC-064; the quantum rows below are there to show what it does to the timing. Read from the table: the synth path at 256 (10.9 ms at the sink) is as early at the ear (estimated 18.9 ms) as the old path at 128 (19.0 ms).
* **PTT mode** (PTT held by MIDI): no cold-start penalty, no tap lost, TX releases (2026-11-09). **External keyer path** (`CW_KEYER_KEYDOWN`): the first key-down after TX start sounds 4.8 to 8.1 ms after the key.

Tools: `tools/cwlat/` (test driver, KiwiSDR capture, analysis; LT-038).
Setup: pihpsdr (the synth side tone as first prototyped, same code as LC-064) -> radio, 1 % drive into a dummy load on 28187 kHz, PA enabled; paddle events injected as
MIDI notes through a VirMIDI port (CWL 21 / CWR 20 / PTT 22); the RF is recorded by a GPS-disciplined KiwiSDR
(IQ, GNSS time-stamps) and the sidetone at the PipeWire sink monitor. 22 wpm, Iambic B and straight key,
break-in and PTT-held, PipeWire quantum 64 / 128 / 256, old sidetone path and callback-synthesised ("synth") path.
42 valid runs, about 2 500 keyed elements, 60 KiwiSDR recordings in total (18 earlier + 42; 6 more quarantined). The 2026-11-09 runs add Iambic A, PTT mode and the external keyer path at quantum 128 (notes 23 and 24 mapped to `CW Key (Keyer)` and `PTT (CW Keyer)`).

## How the numbers are obtained
* Kiwi samples are placed on UTC from the GNSS stamps (each stamp = time of the first sample of the following data chunk;
  GPS-UTC taken as 18 s). The carrier is found by correlating each frequency bin's power with the times we keyed
  (it sits within 5 Hz of the Kiwi centre frequency), demodulated and low-passed (400 Hz, zero phase).
* Latency = carrier 50 % point minus the MIDI send time (PC clock, NTP-synchronised and trusted).
* Element length = 50 %-to-50 % of the carrier; compared with the commanded hold (straight key) or the nominal dit/dah.
* Sidetone numbers come from cwlat's own capture of the sink monitor on the same PC clock.

## Results (warm = element within 400 ms of a previous one, cold = first after a pause)
### End-to-end RF latency does not depend on the audio path or quantum
| path | Q | warm taps | mean | sd | p5..p95 | max |
|---|---|---|---|---|---|---|
| old | 64 | 278 | 22.53 ms | 0.68 | 21.5..23.8 | 24.7 |
| old | 128 | 279 | 22.50 | 0.57 | 21.6..23.4 | 24.5 |
| old | 256 | 282 | 22.60 | 0.64 | 21.6..23.6 | 24.6 |
| synth | 64 | 276 | 22.62 | 0.91 | 21.0..23.9 | 24.4 |
| synth | 128 | 281 | 22.73 | 0.87 | 21.2..24.1 | 24.6 |
| synth | 256 | 281 | 22.75 | 0.87 | 21.3..24.1 | 24.7 |

MIDI press to RF carrier is about 22.5 ms. The synth path adds roughly 0.1-0.2 ms to the mean and about 0.25 ms to the sd;
the cause is not known.

### Sidetone against RF
| path | Q | sidetone at sink | RF | sidetone earlier than RF by | estimated at the ear* |
|---|---|---|---|---|---|
| old | 64 | 13.2 ms | 22.5 | 9.3 | 15.5 (7 ms before RF) |
| old | 128 | 15.0 | 22.5 | 7.5 | 19.0 (3.5 before) |
| old | 256 | 19.0 | 22.6 | 3.6 | 27.0 (**4.5 after** RF) |
| synth | 64 | 5.0 | 22.6 | 17.7 | 7.3 (15 before) |
| synth | 128 | 7.0 | 22.7 | 15.8 | 11.0 (12 before) |
| synth | 256 | 10.9 | 22.7 | 11.8 | 18.9 (4 before) |

*sink time plus one quantum plus the sink headroom (48 / 64 / 128 samples) reported by PipeWire; USB transfer and DAC not included.
With synth the sidetone leads the RF at every quantum; with the old path at Q256 it lags the RF.

### Break-in cold starts: delayed first element, and clipped when keyed with a straight key
* RF onset of the first element after a pause: 43.4 ± 10.6 ms (iambic B, old) / 43.6 ± 9.9 (synth); straight key 42.5 ± 7.0 / 43.6 ± 8.4.
  Extra delay against warm taps: about 20.8 ms on average, 5th-95th percentile 23-62 ms absolute. Same on both paths.
* With PTT held there is no such delay (cold = warm, 23.3 ms).
* Straight key, break-in: the RF element of a cold tap is **20.4 ± 7.9 ms shorter than the commanded hold** (worst 45 ms);
  warm taps are exact (+0.04 ± 0.54 ms). Length change per ms of extra onset delay: -1.01, i.e. the carrier ends on time and only
  its beginning is lost. This is the shortened first dit / dah. The sidetone is clipped the same way.
* Iambic: cold elements are late but not shortened (the keyer sends a full-length element after TX is up).

### Element timing and RF shape
* Iambic elements: dit -0.17 ± 0.03 ms, dah -0.18 ± 0.03 ms against the nominal 54.5 / 163.6 ms (n = 701 / 705): no measurable jitter.
  Dit-to-dit period on repeat 109.43 ± 0.44 ms (nominal 109.09).
* RF edges: 10-90 % rise 5.00 ± 0.06 ms, fall 5.02 ± 0.05 ms, overshoot 0.07 dB (max 0.57 dB), n = 2 536 elements: clean and uniform, no clicks seen.
* Straight key, warm: tone length against hold +0.04 ± 0.54 ms; release-to-RF-off 22.4 ms, same as onset (no stretching).

### Keyer repeat point (iambic B, dit taps, RF element count)
Taps held up to 100 ms give one dit (one 98 %, rest 100 %); 110 ms gives a second dit in 87 %, 120 ms in 90 %, 140 ms in 100 %.
50 % crossing at 106 ms (nominal dot + gap 109 ms). Independent of path and quantum.

### Taps that produce no RF element (dropped)
* PTT held (runs of 2026-11-06): the first tap after PTT was lost in every PTT run (24 of 24). **Invalid:** a fault of the test sender (running status, see below); PTT mode was re-measured on 2026-11-09.
* Break-in iambic: mostly 20 ms taps (7 of 12 20 ms dit taps in the break-in runs), a few 30-90 ms. Straight key break-in: 33-38 ms
  holds mostly, plus 70 and 86 ms. These are taps released while the radio is still switching to TX: lost at RF as well as in the sidetone.
* Every tap missing at the sink is also missing at RF, except the cases below.

## Are the (shortened) elements cleanly shaped? Key clicks and wideband artefacts
Checked from the KiwiSDR IQ (passband +-6 kHz) with `tools/cwlat/analysis/cwlat_spectrum.py`.

**Established**
* The RF keying edges are the designed DL1YCF 9 ms ramp: the mean measured onset follows the ideal ramp (through the same 400 Hz filter)
  to **0.46 % rms of the plateau, 1.3 % worst**; 10-90 % rise 4.92 ms measured against 4.92 ms ideal (2 745 onsets).
  The class that is clipped (straight key, first element after a pause in break-in, 93 onsets) fits equally well: 0.48 % rms, 5.00 ms rise.
* So a shortened element is a normally ramped element that **starts late**; it is not cut off hard. The 12 isolated elements shorter than 30 ms
  (5-22 ms) reach 0.97-1.01 of full amplitude, i.e. smooth, with the same ramps.
* Plateau of long elements (1 129 elements >= 80 ms): ripple 0.33 ± 0.09 % (worst 1.54 %), deepest dip -0.74 dB, none deeper than 1 dB.
* Carrier frequency in the first 2-12 ms after onset: mean -0.01 Hz (sd 0.08), median excursion 0.97 Hz, largest 8.1 Hz (400 Hz measurement bandwidth): no chirp.
* Spurs inside long elements: nothing more than 4 dB above the local median beyond ±60 Hz except one weak line at -1437 Hz, which is in the region of the
  external keyed station seen at -1.1 to -1.55 kHz.

**Not established: the level of wideband splatter**
* Compared with what a 1 ms linear edge would give (-37 dBc in the 1-2 kHz band, found by injecting synthetic edges into real key-up windows)
  the measured edges are certainly not that abrupt, but a quantitative key-click level cannot be given: the energy in the 300-5000 Hz bands is
  11-13 dB **lower** under the carrier than in key-up windows, so subtracting a noise floor gives meaningless negative numbers. Likely causes: the Kiwi's AGC
  (default in IQ mode unless `-g` is given), an external keyed station, or the transmitter's own noise. Not resolved.
* The ARRL clean-signal figures the DL1YCF ramp is designed for (-60 dBc beyond 338 Hz, -80 beyond 600 Hz, -120 beyond 1.2 kHz) are below what these captures can show.
* Everything was taken at 1 % drive into a dummy load. Splatter from the PA and ALC at real drive levels is not covered at all.

**To close this properly:** re-capture a subset with the Kiwi AGC off and a fixed gain (`kiwirecorder -g`), on a clear frequency or with the interferer
avoided, and ideally at a realistic drive through an attenuator.

## The sink check (2026-11-09)
* **The sink-only losses were the detector, not the side tone.** With receiver audio on the sink (the AF gain minimum is not a mute: about 4e-3 rms against a tone peak of 1e-2) cwlat reported 9 to 48 taps as having no tone at the sink in runs where the radio keyed every tap (RF missing 0). The raw sink recording (`--sink-wav`) showed the tone 22 to 38 ms after the press in every one of them. The envelope of the noise reaches 0.3 to 0.7 of the tone level, trips the 50 % threshold, and the scan then skipped the real tone. Fixed in part (resume 3 ms after a tone, not at 10 %); a silent sink is still needed (`tools/cwlat/README.md`). The 17 "sink only" taps of 2026-11-06 are probably the same artefact.

## The external keyer path (2026-11-09)
`cwlat --test ext`: PTT note 24 (`PTT (CW Keyer)`), key note 23 (`CW Key (Keyer)`, `CW_KEYER_KEYDOWN`), quantum 128, 36 trials.
* **The idle fix is verified on air for the external keyer path:** the first key-down after TX start, which carries a wait of 120 to 480 ms (gap to the previous key-up plus the lead), plays at **4.8 to 8.1 ms (mean about 6.5)** at the sink and 22.2 to 23.7 ms at the RF, with tone and carrier lengths equal to the hold (within 0.2 ms), for leads of 30 and 100 ms after PTT. Counted again from the start of TX that wait would have been 120 to 480 ms late.
* **A key-down before TX is up** (lead 5 ms: the key-down comes 5 ms after the PTT note; TX comes up 18 to 60 ms after it): the side tone sounds in all 12 trials (17 to 41 ms after the key-down, when TX comes up) and the RF carrier is missing in all 12. This is expected and not a fault: the side tone follows the key whether or not PTT is asserted (zero beating: key down against the receiver audio without transmitting), and the TX engine does not queue key state ahead of PTT (it empties its ring every 2 ms while not transmitting); key state matters for transmission only once PTT has started. (An earlier version of this note called it a mismatch and proposed dropping the synth's queued events; that was wrong.) What the code does today is a separate question: the synth renders only while `radio_is_transmitting()`, like the engine, so a key-down without PTT gives no side tone in either path; see the open question in `cw_sidetone-design.md`.
* A key-down 30 ms after PTT is sometimes before TX is up too (TX comes up 18 to 60 ms after the PTT; one of 12 trials lost at both RF and sink). An external keyer therefore needs a PTT lead of at least about 65 ms (measured TX-up time at most 60 ms; not tested above 100 ms).

## PTT mode (2026-11-09)
* **PTT mode, measured correctly for the first time** (PTT held by MIDI note 22, the `PTT` action, break-in off, Iambic B, hang 600, 22 wpm, quantum 128; dit 60, dah 60, squeeze 36 trials; the sink is silent in TX, so the sink side is clean): no tap lost at the RF or at the sink (0 of 156), and TX released afterwards. **No cold-start penalty:** the first element after a pause is as fast as any other (RF onset 22.6 ms against 22.3 for the rest; sink 6.5 against 6.4 ms). Dit: sink 6.4 +/- 0.8 ms, RF 22.3 +/- 0.5 ms, RF leads the sink by 15.9 ms; dah: sink 6.4, RF 21.4; squeeze: sink 6.3, RF 22.0; edges 5.0 ms; length error -0.16 to -0.19 ms. The repeat point is between 100 and 110 ms (1 element up to 100 ms, 2 from 110 ms: 15 of 15 as expected). Squeeze: 2 elements for holds of 40 to 100 ms, 3 for 140 and 200 ms, 36 of 36. The only odd trial (the last of the dit run, three 5 ms blips on the sink after a normal dit) is at the end of the run, when the receiver audio returns; the RF shows one element.
* **PTT mode, straight key** (PTT by note 22, straight key as note 20, holds of 30 to 160 ms, same conditions; 60 trials): no tap lost at the RF or at the sink, TX released afterwards. RF onset 22.8 +/- 0.5 ms, sink 6.7 +/- 0.9 ms (RF 16.1 ms later); key-up to carrier off 23.4 +/- 0.5 ms, to tone off 7.5 +/- 0.8 ms; edges 5.0 ms. The element is slightly longer than the hold: carrier +0.64 +/- 0.54 ms, tone +0.87 +/- 0.74 ms (max 2.75): the keyer looks at the key once a millisecond (`STRAIGHT` state) and the key-up is timed from that look.

## Running status (2026-11-09)
The virtual MIDI port sends running status, which piHPSDR's parser drops; this was the cause of the cwlat PTT fault ("PTT stuck", "first tap after PTT lost") and of the squeeze test never squeezing. Fixed in the sender; squeeze results are in [cw_keyer_first_tap.md](cw_keyer_first_tap.md), PTT mode above. Details: `tools/cwlat/README.md`.

## Hermes Lite 2, Protocol 1 (2026-11-09)

Same PC, sink, Kiwi and settings as the G2 runs (22 wpm, Iambic B or straight key as stated, break-in, hang 600, synth side tone, quantum 128, 1 % drive into a dummy load), but on 28183 kHz and a Hermes Lite 2 (firmware 742, Protocol 1, 192.168.0.103) in place of the Saturn G2 (Protocol 2). Keyer: external (`cw_keyer_internal=0`), so the MIDI keys go through `iambic.c`. Captures: `tools/cwlat/results/hl2-p1/` (not committed). One run each, 60 trials; no repeats yet.

| | G2 (P2) | HL2 (P1) |
|---|---|---|
| Side tone at the sink, warm / cold | 7.0 / 27.8 ms | 7.1 / 27.7 to 34.1 ms |
| RF carrier, warm | 22.5 to 22.8 ms | 62 to 66 ms |
| RF carrier, cold | 43.3 ms | 86 to 89 ms |
| RF minus side tone | about 15 ms | 55 to 59 ms |
| Element length error (RF) / rise / fall | -0.17 ms / 5.0 / 5.0 ms | -0.06 ms / 5.06 / 4.98 ms |

* **The keyer and the side tone behave as on the G2.** 0 of 60 taps lost and no extra RF elements in the straight run; the repeat point of the dits is where it was (1 of 5 at 70 ms, 3, 4 and 5 of 5 at 110, 120 and 140 ms); element length and edges unchanged. Straight key, cold: the element is 20 ms short of the hold at the sink (-20.1 ms) and in RF (-20.4 ms), which is the RX to TX switch taken out of the hold ([cw_keyer_straight_key.md](cw_keyer_straight_key.md)).
* **The RF is about 40 ms later than on the G2**, warm and cold, so the side tone leads the carrier by 55 to 59 ms. Cause not examined: the HL2's TX IQ buffering and piHPSDR's P1 TX path are the obvious candidates. It is the radio and protocol, not the keyer or the side tone.
* **Jitter.** Straight run, warm: RF onset 66.4 +/- 1.5 ms (62.1 to 68.5), steady. Dit run, warm: 64 to 70 ms for 33 of 42 trials, but **8 trials at 48.6 to 49.4 ms and one at 32.9 ms**, in runs of consecutive trials (15 to 18, 26 to 29) and about 16.5 ms apart: a stepped state of the radio, not noise. Not the side tone path (sink latency normal, correlation with RF latency -0.04), not the idle time before the tap (the same range in both groups), not the hold, not the element (RF length the same, 54.4 to 54.5 ms). Cold: dit sd 11.0 ms, straight sd 4.0 ms, following the sink latency (correlation 0.79 and 0.76): the TX switch.
* **One outlier in the straight run:** warm trial 59 (the last), hold 127 ms, tone 168 ms, tone off 47 ms after the release: the last trial of a run, see below.
* **"Key correlation" in the analysis report** (0.32 dit, 0.44 straight) is how well the carrier's on-off pattern follows the commanded key windows, used only to pick the carrier frequency within +-200 Hz of the Kiwi's centre. It is low because the RF comes on after the commanded window has ended (a dit is 55 ms, the RF lags 60 ms); shifting the windows by 40 and 62 ms gives 0.60 / 0.65 (dit) and 0.80 / 0.82 (straight). The carrier found is +7 to +8 Hz from the centre as before, SNR 37 to 38 dB, all 60 RF elements matched.
* **`short` (LC-066, straight key, 90 holds of 5 to 50 ms, every one cold): 90 of 90 taps gave a full-amplitude RF element** (the G2 run of 2026-11-07 also gave 90 of 90). RF onset 86.2 +/- 6.6 ms, sink 28.1 +/- 5.5 ms, RF minus sink 58.1 +/- 3.8 ms, rise 4.9 ms, fall 4.8 ms. The sink detector missed 1 tap that the RF has (a detector miss, not a lost tap). The analysis first locked onto a carrier at +153.5 Hz (key correlation 0.03), because a 5 to 50 ms command window has ended long before the RF comes on; with the windows shifted by the RF delay it finds +9.4 Hz (0.48) and gives the same timing (the 400 Hz envelope filter covers both).
* **Hang test (LC-065, Iambic B, hang 600, 120 trials, grid 100 to 1400 ms in steps of 50, two taps per D from 400 on): 0 of 120 taps lost** (the unchanged keyer lost 9 of 120 on the G2). The radio is still in TX at D = 350 ms (all 3 warm, 5.4 to 8.4 ms at the sink) and has dropped out at D = 400 ms (1 of 2 warm, 1 cold at 64.5 ms), 450 ms and later all cold: the effective hang is about 0.6 of the set value, as on the G2 (expiry at D about 335 to 380, [cw_timings.md](cw_timings.md)); the grid is too coarse to say more. Warm RF onset 65.4 +/- 3.1 ms (n=18), cold 87.4 +/- 10.5 ms (n=102), RF minus sink 58.5 / 57.0 ms. As in `short`, the carrier search first locked on -90.1 Hz (correlation 0.05) and finds +5.1 Hz (0.62) once the windows are shifted by the RF delay; the figures are the same either way.
* **Iambic A squeeze (36 trials, both paddles pressed 6 ms apart, holds 40 to 200 ms):** RF elements per hold: 1 for 40, 60, 80 and 100 ms (all 6 trials each); 2 for 140 ms in 5 of 6 trials and 1 in the sixth; 2 for 200 ms (6 of 6). The G2's corrected Iambic A run gives the same except 6 of 6 at 140 ms (the one trial that differs is at the repeat point, where a hold of 140 ms is about 30 ms from the second element's decision). One element fewer than Iambic B (G2: 2, 2, 2, 2, 3, 3). No tap lost (0 of 36), RF onset 87.3 +/- 6.3 ms, sink 29.7 +/- 5.3 ms, RF minus sink 57.6 ms. The sink counts of the G2 runs are lower than the RF counts in 15 of 36 (Iambic A) and 4 of 36 (Iambic B) trials, which is the sink detector with receiver audio on the sink; this run, with a silent sink, has none of that.
* **Iambic B squeeze (36 trials):** RF elements per hold 2, 2 (5 of 6), 2, 2, 3, 3 for 40, 60, 80, 100, 140, 200 ms, as on the G2 (the sixth trial at 60 ms is the last trial of the run, see the next point). 0 of 36 lost. RF onset 87.0 +/- 5.6 ms, sink 28.3 +/- 5.0 ms, RF minus sink 58.7 +/- 2.1 ms.
* **The last trial of every HL2 run was cut by the end of the run (confirmed).** In 6 of 6 runs the sink showed two tones and the RF one for the last trial (squeeze A trial 35: 54.9 then 44.2 ms; squeeze B trial 35: 54.5 then 34.1 ms, where a 164 ms dah was expected; dit trial 59, hang trial 119, short trial 89 and straight trial 59 likewise; the straight run's first RF element was 171 ms for a 127 ms hold). The G2 captures show it in 4 of 64 runs. After the last event `cwlat` called `release_all()` at once (note-offs for the paddles, the key, the external PTT (note 24) and PTT (note 22)), which ended TX while the last element was still in flight; with the HL2's 58 ms RF lag the RF lost what the side tone had already played. `cwlat` now waits 1 s before `release_all()`; the Iambic B squeeze re-run with the wait (`results/hl2-p1-wait/`): **sink and RF agree in all 36 trials**, the last trial has its dah (RF 163.6 ms) and RF elements per hold are 2, 2, 2, 2, 3 (5 of 6; 2 in the sixth, at the repeat point as in the Iambic A runs), 3 for holds of 40 to 200 ms. RF onset 89.7 +/- 9.2 ms, sink 31.3 +/- 8.6 ms, RF minus sink 58.4 +/- 3.9 ms. **The last trial of the runs before 2026-11-09 22:20 (HL2 dit, straight, short, hang, both squeezes) is not a keyer result and is left out of any judgement; so are the straight run's trial 59 and the squeeze trial 35 above.** The G2 captures may carry the same effect on their last trials.
* **PTT mode (PTT held by `cwlat`, break-in off, Iambic B; dit 60, dah 60, squeeze 36 trials; 1 s wait before `release_all()`):** 0 taps lost, sink and RF agree in every trial, last trials normal. **The RF onset is a constant 65.2 to 66.7 ms (sd 0.18 to 0.20 ms)** in all three runs, warm and cold alike: dit 65.5 +/- 0.2 ms, dah 66.3 +/- 0.2 ms, squeeze 66.2 +/- 0.2 ms; side tone at the sink 6.6 ms; RF minus sink 59 to 60 ms; rise 5.06, fall 4.96 ms; element length error -0.04 ms. Repeat points: dit 1 element up to 100 ms, 2 from 110 ms; dah 1 element at every hold; squeeze 2, 2, 2, 2, 3, 3 elements (40 to 200 ms), all as in Iambic B with break-in. **So the HL2's RF latency is a steady 65.5 ms when TX is up, and the jitter seen earlier (sd 1.5 to 8 ms warm, the 49 and 33 ms steps in the break-in dit run) comes only with break-in** (the same 1 ms keyer thread, the TX-off timer and the RX to TX switch), not from the radio or the P1 stream. Not explained which part; in PTT mode the 64 to 70 ms population of the break-in dit run is the normal one. The G2's PTT-mode RF latency is 22.5 ms.
* **Straight key in PTT mode (60 holds, PTT held, break-in off):** 0 lost, sink and RF agree in all 60, last trial normal. RF onset **64.9 +/- 0.2 ms** (64.6 to 65.4), side tone 6.4 ms, RF minus sink 58.5 ms, **RF off 65.5 +/- 0.3 ms after the release** (the tone ends as late as it starts, so the element length is kept: RF length minus hold +0.62 ms, 0.36 sd, -0.06 to +1.41 ms), rise 5.06 ms, fall 4.99 ms. The same constant 65 ms as the paddle runs in PTT mode. With break-in the warm straight run had 66.4 +/- 1.5 ms and its cold start lost 20 ms of the hold.
* **External keyer path (`--test ext`, PTT note 24, key note 23, 36 trials, 1 s wait before `release_all()`):** the side tone sounds in all 36 trials. **Lead 5 ms** (key-down before TX is up): RF missing in 12 of 12, as on the G2 (expected, see the external keyer path above). **Lead 30 ms:** RF missing in 2 of 12 (trials 3 and 4, the first of the run; the sink latency there is 17 ms, when TX comes up; the G2 lost 1 of 12); an external keyer needs a PTT lead above 30 ms and about 65 ms to be safe, as before. **Lead 100 ms:** RF in 12 of 12. The first key-down after TX start plays at 5.3 to 7.7 ms at the sink (6.3 mean), as on the G2, with tone and carrier lengths equal to the hold (within 0.2 ms, one 0.7 and one 3.1 ms outlier). **The RF onset of the 22 trials that have RF is 65 to 67 ms in 15, 59 ms in 1, 43 ms in 2 (42.6, 43.4) and 30 to 31 ms in 3 (30.0, 30.7, 31.2):** the stepped early values seen in the break-in dit run (49 and 33 ms), here as 43 and 30; they follow neither the lead (30 or 100 ms) nor the hold (40, 60 or 80 ms). They do not occur in PTT mode with PTT held for the whole run (steady 65.5 +/- 0.2 ms in 216 trials), and the side tone is not involved (sink latency 5.3 to 7.7 ms in these trials). Cause not found; the HL2's TX buffering after each TX start is the first suspect, untested.
* **Not yet done on the HL2:** the dit repeat (do the early steps, 49 and 33 ms in break-in, 43 and 30 ms here, move between runs?), and what causes them (a P1 capture of the TX IQ stream against the keying, or a G2 run with a long TX-up time).

## Open items
The list for the whole CW work is in [cw.md](cw.md). Specific to the measurements:
1. **The 22.5 ms RF latency is not decomposed.** It contains the MIDI thread, the keyer's 1 ms poll, the TX engine, the radio's TX buffering and the Kiwi's own filter delay, plus any PC clock error against GPS. The Kiwi delay and the PC clock offset are not corrected.
2. Not measured: other keyer speeds, other bands, a real paddle (the virtual MIDI port skips the controller's USB-MIDI hop), levels above 1 % drive.

## Reproducing
    tools/cwlat/run_matrix.sh --sink <sink> --kiwi kiwi.minish.org:8074 --kfreq 28187 ...    # captures
    python3 -I tools/cwlat/analysis/cwlat_analyze.py all <results dirs> --out results/analysis  # per-run analysis, summary.md
    python3 -I tools/cwlat/analysis/cwlat_report.py results/analysis                           # cross-run report and plots

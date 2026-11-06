# CW keying latency, jitter and clipping - findings of 2026-11-06

## Summary
* **Where the delay was.** On the test machine the PipeWire graph ran at 1024 frames although pihpsdr asked for 256 (the USB sink was the
  driver); the sidetone then waited 21 ms for a cycle plus the sink's own 512-sample headroom, plus the 6.7-8 ms side tone FIFO in `pipewire.c`.
  Forcing the quantum (`clock.force-quantum`) made the sidetone "much tighter".
* **What the patch does.** The local side tone is generated inside the PipeWire output callback instead of being queued through the FIFO
  (`docs/synth-sidetone/README.md`). Key-to-sink delay falls by about **8 ms at every quantum** (warm taps, measured at the sink monitor):
  13.2 -> 5.0 ms at 64, 15.0 -> 7.0 at 128, 19.0 -> 10.9 at 256. Element lengths are unchanged (counted in output samples).
* **RF is not affected by the audio path or quantum:** MIDI press to RF carrier is 22.5 ms (sd 0.6-0.9) in every combination.
  The sidetone therefore leads the RF by 12-18 ms with the patch, but lags it by 4.5 ms on the old path at quantum 256 (estimated at the ear).
* **Break-in costs more than the audio path.** The first element after a pause starts about 21 ms late (RX->TX), and a straight key then loses
  that time from the front of the element (**20 ± 8 ms shorter, worst 45 ms**). Taps shorter than the switch-over are lost altogether.
* **Keying shape is clean:** the RF edge is the designed DL1YCF 9 ms ramp to within 0.5 % rms; shortened elements are normal elements that
  start late, not hard-cut; no chirp, ripple 0.3 %. The wideband (key-click) level could not be quantified with these captures (see below).
* **Recommendation.** Use the synth side tone at quantum 128 (about 6 ms earlier than the old path at 256 and 4 ms earlier than the old path at 128
  at the sink; no drop-outs on the test machine). Fix the break-in start (clipped first element, lost short taps) in the keyer; that is a
  larger effect than anything in the audio path.

Branch `test/sidetone`, patch branch `feature/synth-sidetone`. Tools: `tools/cwlat/` (test driver, KiwiSDR capture, analysis).
Setup: pihpsdr (this branch) -> radio, 1 % drive into a dummy load on 28187 kHz, PA enabled; paddle events injected as
MIDI notes through a VirMIDI port (CWL 21 / CWR 20 / PTT 22); the RF is recorded by a GPS-disciplined KiwiSDR
(IQ, GNSS time-stamps) and the sidetone at the PipeWire sink monitor. 22 wpm, Iambic B and straight key,
break-in and PTT-held, PipeWire quantum 64 / 128 / 256, old sidetone path and callback-synthesised ("synth") path.
42 valid runs, about 2 500 keyed elements, 60 KiwiSDR recordings in total (18 earlier + 42; 6 more quarantined).

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
* PTT held: trial 0, the first tap after PTT is asserted, is lost in **every** PTT run (24 of 24) on both paths, and nothing else
  is lost in the iambic PTT runs. See "Open items".
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

## Open items
1. **First tap after PTT always lost; PTT left stuck after PTT-mode runs.** Both only appear when the test sender holds PTT. The operator
   uses MIDI PTT with CW daily without either. Treated as a fault in the test sender (MIDI stream / timing assumptions) to be fixed before
   further PTT-mode tests - see `tools/cwlat/README.md`. Whether the lost first tap is the same fault is not known.
2. **17 taps keyed at RF but absent at the sink** (old / straight / PTT 7, synth / straight / PTT 10, synth / iambic B / break-in 1).
   The same trial numbers recur across paths and quanta (37, 45 ...), their preceding gaps are 52-90 ms but other taps with equally short
   gaps were fine. Could be a genuine sidetone dropout or an artefact of cwlat's sink-side detector. Needs cwlat to save the sink recording.
3. **The 22.5 ms RF latency is not decomposed.** It contains the MIDI thread, the keyer's 1 ms poll, the TX engine, the radio's TX buffering
   and the Kiwi's own filter delay, plus any PC clock error against GPS. The Kiwi delay and the PC clock offset are not corrected.
4. Synth path RF onset jitter is slightly higher (sd 0.9 against 0.6 ms). Not understood.
5. Not measured: Iambic A, other keyer speeds, other bands, a real paddle (the virtual MIDI port skips the controller's USB-MIDI hop).

## Reproducing
    tools/cwlat/run_matrix.sh --sink <sink> --kiwi kiwi.minish.org:8074 --kfreq 28187 ...    # captures
    python3 -I tools/cwlat/analysis/cwlat_analyze.py all <results dirs> --out results/analysis  # per-run analysis, summary.md
    python3 -I tools/cwlat/analysis/cwlat_report.py results/analysis                           # cross-run report and plots

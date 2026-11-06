# CW keying latency, jitter and clipping - measurements of 2026-11-06

Branch `test/sidetone`. Tools: `tools/cwlat/` (test driver, KiwiSDR capture, analysis).
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

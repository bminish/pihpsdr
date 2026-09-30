# Findings on TEST

Captures taken and measured on `TEST` itself (bminish/pihpsdr), with the
engine and tools as they are here. The feature branches' record is in
`docs/diversity-measurements.md` and keeps its own numbering; findings
here are numbered T-001 onwards so the two never collide.

## T-001: two RADE V1 stations on 20 m, the weak one out of any combiner's reach

**Capture.** `divcap-20260930-175445.divc`: Saturn/G2, 14.236 MHz DIGU,
filter 750–2250 Hz, 192 kHz, 32768-point blocks (171 ms), 59.9 s. Both
attenuators at 0 dB. RADE V1 reference, Sum, 2 s averaging, no context
changes. The recording started before diversity was entered. Taken with
`TEST` at `35cff352`, the first RADE capture after LC-015 fixed the menu.

Operator's description: two RADE V1 stations in QSO. One is strong and
heard for a few seconds; the other is very weak and below decode on
FreeDV. Band noise, little or no analog interference. The capture's note
field is empty; this is the only record of that.

**The replay reproduces the recorded run exactly:** 351 blocks checked,
0 differ.

### What happened, second by second

From the passband power on each arm (against a guard band), the
correlator's replayed state, and the weight the whole engine applied:

| Time | On the air | Correlator | Weight applied |
|---|---|---|---|
| 0–5 s | nothing visible above band noise | searching | 1 (cold start) |
| 5–6 s | weak station's pilot caught | candidate at +10 Hz, locks with pilot/floor **2.76** (2.5 needed) | +2.1 dB −25°, from about a second of data |
| 6–16 s | weak station, within ±1.5 dB of band noise | locked but frozen (quality 0.05) | held |
| 16.4–20 s | **strong station**, 7–17 dB above noise, arm 1 5–6 dB hotter | resync: pilot found 375 samples and −11.9 Hz away, new lock at 0 Hz, pilot/floor **7.62** | slews from the weak station's weight, settles at +5.7 dB −65° around 21 s |
| 20–38 s | weak station again, at noise level | frozen on the strong station's lock | held; it moved about 1 s past the end of the over while the freeze gate caught up |
| 38–60 s | weak station | resync finds a pilot 247 samples and +15.6 Hz away; it **fails confirmation twice** (pilot/floor 1.80, 2.02) | held |

### Decode, and what any combiner could have done

Scored by `score_rade`; frames in sync:

| Stream | In sync |
|---|---|
| Arm 0 alone | 39 of 40 |
| Arm 1 alone | 38 of 40 |
| Engine (the weight the radio applied) | 31 (**−8** against the better arm) |
| Correlator's raw weight | 24 |
| Best of 40 fixed weights (\|w\| −6 to +6 dB, every 45°) | 38 |

- **All ~40 decodable frames are the strong station's 4 s over.** Both
  arms decode it at 95–97 % on their own, so there's nothing for a
  combiner to add.
- **The −8 is one sync period, not a quality loss.** Frame counts move in
  steps of about 7 (22 / 24 / 31 / 38). On a 4 s over, what decides the
  score is which ~0.8 s period the decoder syncs in. The engine started
  the over on the weak station's held weight and needed about 3 s at 2 s
  averaging to settle on the new one, so the decoder synced one period
  later.
- **The weak station is out of reach of any fixed weight.** Across the
  whole grid, none syncs it: the best is 38, below arm 0 alone. A
  two-antenna combiner adds at most 3 dB, and the grid is within about
  0.7 dB of any static optimum. So the weak station is further below the
  decode threshold than combining can recover, at least as a static
  channel.
- **The correlator can see it, intermittently.** It locked once at
  pilot/floor 2.76, and later candidates reached 1.8–2.0 against the 2.5
  needed to confirm. So its pilot sits right at the detection edge,
  which is consistent with a signal the decoder can't use.

### Checked against the feature branch

`feature/auto-diversity`'s engine gives exactly the same result on this
capture: 31 in sync, −8, in two runs of each. On this evidence there's
no RADE tracking regression on `TEST`. The earlier impression of one came
from the "Measure on" menu bug (LC-015), which had been running
FSK/Digital with RADE V1 selected.

### What would make a capture useful for the weak-signal case

- **A weak station close to threshold,** decoding intermittently on at
  least one antenna, rather than one that neither antenna nor any fixed
  combination can sync. That's the only case where a 0–3 dB combining
  gain changes the decode.
- **Longer overs.** With 4 s of signal, the score is dominated by the
  decoder's sync time, in ~0.8 s steps.
- **A note.** `PIHPSDR_DIVCAP_NOTE` with the antennas, the attenuators
  and what's on frequency.

### Open, from this capture

- **The held weight at a changeover.** The strong station's over started
  on the weak station's weight. With both arms decoding anyway, it cost
  only a sync period here. On a marginal station it could matter more.
- **The freeze gate's lag.** It let about 1 s of post-over noise into the
  strong station's weight before freezing. This is the "gate engaging"
  kick `test_rade` already reports.

Neither is acted on: both would need a capture where they visibly cost
decodes.

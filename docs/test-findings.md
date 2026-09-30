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
- **The −8 is within the scorer's own noise** (corrected by T-003).
  Frame counts move in steps of about 7–8 (22 / 24 / 31 / 38), one
  decoder sync period, and `score_rade` itself varies by one period
  depending on stream layout. The engine did start the over on the weak
  station's held weight and needed about 3 s to settle, but this capture
  can't show that it cost anything.
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

## T-002: band noise produces no lock

**Capture.** `divcap-20260930-175944.divc`: 14.236 MHz DIGU, 0/0 dB,
RADE V1, Sum, 0.22 s averaging, 60 s. Band noise, per the operator.

The replay reproduces the run exactly. Neither antenna decodes anything,
and the correlator never locks. It raised two candidates on noise, and
both failed confirmation. So the acquisition search does produce false
candidates on dead air, and probation rejects them: no false lock, and no
weight fitted to noise.

## T-003: `score_rade`'s decoder streams are not independent

Found while scoring T-004 and T-005. `score_rade` runs all its streams
through librade in one process: each antenna, the correlator, and every
`--weights` file. Those streams affect each other:

- On `180822`, arm 0 alone scores **98** frames in sync with one extra
  weight stream and **106** with two.
- A weight of exactly 0 *is* arm 0 alone. Given twice in one run, the
  two copies score 98 and 106.
- It's repeatable run to run for a given layout, so this is shared state
  inside the decoder, not randomness.

The difference is one decoder sync period (about 8 frames). So:

- **A difference of one sync period or less is noise.** Only compare
  streams within a single invocation with the same set of streams.
- Earlier results that rely on a difference that small are not
  established. That includes T-001's −8, and `190932` in LC-014's table
  (+3 against −2). The LC-014 conclusion rests on larger differences
  (−51, −30 frames) and stands.
- **To fix:** give each stream its own process (or its own librade
  instance). That's a tooling change for later.

## T-004: multipath moves the pilot by tens of samples, and resyncing on it is right

**Captures.** 40 m RADE V1, DIGL, 0/0 dB, each a single station, weak,
with significant multipath (operator): `180235` (7.177 MHz, averaging
changed from 0.22 to 0.42 s at block 137), `180822` (7.177 MHz, 0.42 s)
and `180949` (7.190 MHz, 0.42 s). The replays reproduce the radio's lock
state exactly on `180235` and `180822`. On `180949` they differ on 26
blocks up to 26 s: the radio locked about 1.7 s earlier, because the
capture started mid-search. They agree after that.

**Every resync on these single-station captures was the same station.**
The pilot was found 8, 18, 22, 22 and 24 samples (of 960) from the held
lock, within 6.5 Hz. The resync treats anything more than 4 samples away
(`RADE_RESYNC_DA`) as a new station, so each time the lock was dropped,
the averages cleared and the new candidate went through confirmation. By
comparison, T-001's genuine changeovers moved 247 and 375 samples.

**Widening the window to 48 samples** (experiment, not committed)
removes every one of those resyncs, and the two-station capture still
switches. But it doesn't help: whole-engine decode is unchanged on
`180235` and `180822` and worse on `180949` (−63 against −45, same
invocation). On a multipath channel, a pilot that has moved tens of
samples really is a changed channel, and restarting the average helps.
**`RADE_RESYNC_DA` stays at 4.**

## T-005: when one antenna is much better, neither Sum nor Best reliably wins

Same three captures, plus T-001. Whole engine, decode-scored, frames in
sync. Arms and objectives in one invocation each (T-003):

| Capture | Arm 0 | Arm 1 | Sum | Best | Best's choice (blocks on arm 0 / arm 1) |
|---|---|---|---|---|---|
| `175445` (T-001) | 39 | 38 | −8 | −1 | 0 / 183 |
| `180235` | 131 | 103 | +0 | **−25** | 100 / 7 |
| `180822` | 106 | 45 | −16 | +0 | 66 / 0 |
| `180949` | 269 | 356 | **−45** | −30 | 56 / 184 |

(Sum and Best: synced frames against the better single antenna.)

- **`180949`** (arm 1 far better, fast multipath): every combination
  loses several sync periods. Averaging doesn't rescue Sum: −81, −45,
  −54, −64 and −36 at 0.2, 0.42, 1, 2 and 5 s, with no trend. The Sum
  weight wanders from −4.8 to +17 dB and all the way round in phase
  between 30 and 53 s. Best does better, but still loses 30, because it
  moves to the worse arm for about a quarter of the blocks.
- **`180822`** (arm 0 far better): Best keeps arm 0 throughout and loses
  nothing. Sum loses 16 (8 against the one-stream baseline).
- **`180235`** (arm 0 somewhat better): Sum is level, and Best *loses*
  25.
- **None of these captures has the two antennas close enough together
  for combining to add anything.** So what they measure is what the
  engine costs when it shouldn't combine, not what it gains when it
  should.

Open: a rule for when not to combine. Whether Best's choice could be
stickier, or Sum's weight tied more closely to the arm SNRs the
correlator already measures, needs captures where the arms are close
and captures where they aren't, scored in the same way.

## T-006: the antenna switch on ADC1 isn't visible as a clean step

**Capture.** `180949` (T-004, T-005). The operator switched ADC1's (arm
1's) antenna during the capture.

- **Nothing in the capture marks it.** The engine doesn't watch antenna
  selection, so there's no context change or reset.
- **The samples show a broadband dip on both arms, not a step on arm 1.**
  Both antennas' passbands fall 8–17 dB below their guard bands between
  about 9 and 13 s, and arm 1 comes up to +7 dB at 15 s.
- **The correlator's first lock was at 16.4 s,** after that. So the
  diversity result in T-005 is post-switch, and the capture can't show
  what the switch did to a held weight.

To see that, the switch needs to happen while the correlator is locked
on a steady station, with the time written in `PIHPSDR_DIVCAP_NOTE`.

## T-007: a weak station neither antenna decodes

**Capture.** `divcap-20260930-180340.divc`: 7.122757 MHz DIGL, 0/0 dB,
0.42 s averaging, 40 s. A weak RADE V1 station on its own (operator).

Neither antenna decodes a frame, and the correlator never locks (one
candidate, which failed confirmation). The replay's lock state matches
the radio's. Like T-001's weak station, it's below where combining two
antennas could help.

## Capture practice, from T-001 to T-007

- **Write a note** (`PIHPSDR_DIVCAP_NOTE`): which antenna is on each ADC,
  the attenuators, what's on frequency, and the time of any switch or
  retune. All seven captures have an empty note, and the operator's
  description exists only here.
- **Capture signals that are marginal but decodable on at least one
  antenna.** Only there can combining change the decode. T-001's weak
  station and T-007 are below it; T-005's strong signals are above it,
  on one arm.
- **Start before entering diversity** (as T-001 did), so the replay
  starts cold with the radio and reproduces it exactly.


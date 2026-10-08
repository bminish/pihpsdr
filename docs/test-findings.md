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
FreeDV. Band noise, little or no analog interference.

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

## T-006: the antenna switch on ADC2 isn't visible as a clean step

**Capture.** `180949` (T-004, T-005). The operator switched ADC2's (arm
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
on a steady station, and roughly when it happened needs to be said.

## T-007: a weak station neither antenna decodes

**Capture.** `divcap-20260930-180340.divc`: 7.122757 MHz DIGL, 0/0 dB,
0.42 s averaging, 40 s. A weak RADE V1 station on its own (operator).

Neither antenna decodes a frame, and the correlator never locks (one
candidate, which failed confirmation). The replay's lock state matches
the radio's. Like T-001's weak station, it's below where combining two
antennas could help.

## T-008: three RADE V2 captures, kept for a future V2 correlator

**Logged, not analysed.** `TEST` has no RADE V2 reference, so there is
nothing to replay them against yet. They're here so that work on a V2
correlation algorithm can start from them.

**Captures.** All three at 7.166 MHz DIGU, 192 kHz, 32768-point blocks
(171 ms), and every block the same settings: Window reference, RX filter
1000–2000 Hz (followed), Sum, 0.42 s averaging. No context changes.

| Capture | Length |
|---|---|
| `divcap-20260930-184835.divc` | 59.9 s (351 blocks) |
| `divcap-20260930-185337.divc` | 13.3 s (78 blocks) |
| `divcap-20260930-185725.divc` | 59.9 s (351 blocks) |

Operator's description: two RADE V2 stations in QSO on a crowded 40 m
band, on a path with a lot of multipath. The wideband (Window) tracker
was in use, mostly with a 1 kHz RX bandwidth. Both stations decoded in
the RADE V2 decoder, with low but positive SNRs of about 0 to 8 dB.

**What they offer.** Two stations taking turns, both decodable, on a
multipath channel in a crowded band: changeovers, fades and adjacent
interference in one set. Scoring will need a V2 decoder in the loop, as
`score_rade` does for V1 with librade.

## T-009: RADE V2 on the lower sideband, under strong SSB interference

**Logged, not analysed.** As with T-008, `TEST` has no V2 reference yet.

**Capture.** `divcap-20260930-192558.divc`: 7.190 MHz, recorded mode DIGL,
192 kHz, 16384-point blocks (12 Hz bins, 85 ms), 60.0 s. Every block the
same settings: FSK/Digital reference, RX filter −2000 to −1000 Hz, Sum,
0.42 s averaging. No context changes.

Operator's description: two RADE V2 stations in QSO, at times hammered by
strong SSB interference.

- **The signal is inverted.** RADE V2 is only valid on the upper
  sideband, so received on the lower sideband its spectrum is mirrored.
  A V2 correlator has to un-mirror it (or search both banks, as V1's
  does) before this capture can be used.
- **Otherwise a good test of discrimination.** A wanted digital signal
  with a strong, intermittent unwanted SSB signal in or beside the
  passband. That's the case for a tracker that locks to the wanted
  waveform rather than to whatever is loudest, and a baseline for how
  FSK/Digital's occupancy split copes with it.

## T-010 to T-012: three CW captures through the CW reference

**Captures.** All CWL, 192 kHz, 6 Hz bins (171 ms blocks), CW reference,
Sum, 0.22 s averaging, taken with LC-017 to LC-019 in.

| | Capture | Band | What (operator) |
|---|---|---|---|
| T-010 | `divcap-20260930-205652.divc` | 40 m, 7.0327 MHz | rapid-fire contest QSOs; filter widened from 800 Hz to 1 kHz at block 247 |
| T-011 | `divcap-20260930-205923.divc` | 20 m, 14.02–14.03 MHz | contest; the operator tunes throughout (68 frequency changes, 47 engine resets), the wanted signal mostly near the passband centre |
| T-012 | `divcap-20260930-210410.divc` | 30 m, 10.1295 MHz | a beacon: steady tone, then CW, then tone; noisy band, a reasonably weak signal. To the operator's ear it benefits from diversity |

The operator's impression: the correlator is doing its job.

**The replay is exact.** `run_ref --ref cw` with the recorded settings
matches the radio's recorded loop state on 350 of 350 blocks, once the
record's one-block lag is allowed for: a block's recorded state is the
one in force before that block was processed.

**Scores** (`score_cw.py`, against the better single antenna; CW is what
the radio ran, Window and FSK/Digital are replays of the same audio):

| | Segment | Arms (dB) | CW | Window | FSK/Digital | CW acts key-down / key-up |
|---|---|---|---|---|---|---|
| T-010 | blocks 0–246, 800 Hz | 47.6 / 48.0 | **+2.02** | +1.80 | +1.64 | 93 % / 15 % |
| T-010 | blocks 247–350, 1 kHz | 44.9 / 47.7 | **+1.08** | +0.47 | +0.22 | 82 % / 42 % |
| T-011 | blocks 0–102 | 34.9 / 35.6 | **−0.43** | −1.02 | −0.57 | 91 % / 50 % |
| T-011 | blocks 168–242 | 30.8 / 33.2 | +0.33 | **+1.34** | +0.88 | 70 % / 10 % |
| T-012 | whole capture | 19.2 / 17.0 | +1.63 | +1.14 | **+1.96** | 55 % / 3 % |

- **T-010: CW is the best of the three on both segments,** +1 to +2 dB
  over the better antenna. Strong signals (about 45 dB tone-to-noise),
  so this is combining gain on a good path, not rescue of a weak one.
  The tracker sat within 50 Hz of the passband centre on 88 % of the
  blocks it acted on.
- **T-011: tuning doesn't upset it.** After each of the 47 resets from
  retuning, CW had a weight again on the first or second block (median
  0.00 s, 90th percentile 0.17 s). Only two stretches between retunes
  are long enough to score, and they split: CW ahead on one, Window on
  the other, where CW acted on only 70 % of key-down blocks. Across the
  capture, the tracker was within 100 Hz of the passband centre on 69 %
  of the blocks it acted on (median offset +53 Hz), consistent with the
  operator tuning the wanted station to the centre and the tracker
  following it there.
- **T-012: the beacon gains 1.6 dB,** which fits what the operator heard.
  CW acts on only 55 % of the blocks with signal, because key detection
  (LC-018) treats the beacon's steady tone as a carrier: the weight is
  held through the tone and updated through the keying. FSK/Digital,
  which acts on 98 %, scores 0.3 dB more here. The steady tone is the
  one case where a carrier *is* the wanted signal.

**What it adds to AD-50's picture.** These are the first CW captures
taken with the CW reference as the radio's own. On strong contest
signals it's the best reference or level with the best; on a weak
beacon it gains, a little less than FSK/Digital, for a reason we know.
None of the three is marginal enough to settle the LC-012 floor or
LC-019's 0.2 s.

## T-013 to T-016: crowded 40 m LSB, the three Sum noise models

Four captures, 2026-10-01 16:22-16:28, taken on `test/noise-floor`: 40 m
LSB (7.121-7.141 MHz), Window reference following the RX filter, Sum.
A crowded band with overlapping signals; in each, the wanted station is
correctly tuned and is analog voice. T-016 (`162729`) had a notch in use
for periods.

| T | Capture | Averaging | What the radio ran (from its recorded weight) |
|---|---|---|---|
| T-013 | `162255` | 0.55 s | Weight frozen for the first third (Hold, or not applying); then **Old** |
| T-014 | `162444` | 0.27-0.55 s, moved | Ratio |
| T-015 | `162615` | 0.27-0.60 s, moved | Ratio |
| T-016 | `162729` | 0.60 s | Ratio; a notch from about 15 s on |

The capture records neither the Sum noise model nor the notch. The model
was found by replaying each one and comparing it with the weight the radio
applied, on the blocks where that weight moved: Ratio matches to 0.01-0.20
(relative error) where it ran, Old to 0.03 on T-013's last third. The
notch was found the same way: a steady heterodyne at 1676 Hz audio
(+18 dB over the passband median, in 68 % of blocks); with a notch at
-1676:100 the replay tracks the radio at 0.17 where it was 0.83 without
one. The 45 engine resets at 9.5-14.7 s are the notch being placed and
dragged (LC-013 restarts the statistics on every notch change).

**Scores** (`run_ref` with each model, recorded averaging taken from the
first block; dB against the better antenna; deterministic, three runs
identical). Guard = `score_wideband.py`; in-band = speech blocks against
the quietest 20 % of blocks in the passband.

| | Old guard / in-band | Ratio | Gap covariance | Radio as recorded |
|---|---|---|---|---|
| T-013 | −2.58 / −0.01 | −1.30 / **+0.40** | **−1.07** / +0.26 | −4.05 / −1.01 |
| T-014 | **−0.27** / **+0.93** | −0.41 / +0.70 | −0.41 / +0.89 | −0.40 / +0.34 |
| T-015 | **+1.74** / −0.31 | +1.41 / −0.15 | +0.83 / **+0.04** | +1.41 / −0.12 |
| T-016, no notch | +0.65 / +0.25 | +0.76 / +0.39 | **+1.16** / **+0.86** | +0.42 / +0.02 |
| T-016, notch -1676:100 | +1.00 | +0.71 | **+1.25** | |
| Mean (no notch) | −0.12 / +0.22 | +0.12 / +0.34 | +0.13 / +0.51 | |

- **The three are within about 0.5 dB on three of the four.** As on the
  39-capture set, the two scores disagree on which is ahead (T-015).
- **T-013 is where the noise measurement matters.** The arms are
  lopsided: arm 1's noise is 9.5 dB (quiet passband) to 12.7 dB (guard)
  below arm 0's. Ratio, from the bins outside the filter, sees that and
  weights arm 1 up (|w| +2.5 dB); Old's time minimum, on a band that is
  never quiet, does not (|w| −9.7 dB) and loses 1.3 dB on the guard
  score. That is the case the across-frequency floor was ported for.
- **Gap covariance is slightly ahead on the mean** here (+0.17 dB in-band
  over Ratio), all of it from T-016. Four captures; on the 39-capture set
  it was level with Ratio or worse. Not enough to bring it back; worth
  re-checking if more crowded-band captures come in.
- **The guard band is not noise here.** Passband over guard is only +1 to
  +11 dB on either antenna, because the band beside the passband holds
  other stations, 0.43-0.61 coherent between the antennas on T-014 and
  T-015. On this band the guard score measures adjacent-channel rejection
  as much as noise.
- **T-016 is the first on-air case for LC-013**: a steady interferer
  inside the wanted station's passband. With the notch, the loop acts on
  13.8 % of noise-only blocks instead of 49.7 %, and the noise-only
  coherence (95th percentile) falls from 0.67 to 0.35: between overs the
  loop had been fitting the weight to the heterodyne. Every model scores
  within 0.4 dB of its un-notched figure. The notch's exact width and
  on/off times are inferred, not known.

## T-017 and T-018: 20 m CW, a pileup and a filter sweep with keyclicks

Two captures, 2026-10-01 16:39-16:42, taken on `test/noise-floor`: 20 m
CWL, CW reference, Sum, 0.22 s, Sum noise set to Ratio.

- **T-017** (`163953`): a pileup of many stations, retuned 19 times
  (14.020125 to 14.020016 MHz); 1000 Hz filter.
- **T-018** (`164121`): one station with significant keyclicks, filter
  stepped 100, 50, 26, 50, 100 ... 1000 Hz and back down to 50 Hz.

**The Sum noise model does nothing on the CW reference.** CW takes its
noise ratio from its own off-tone bins (`div_cw_floor()`, LC-017), and the
outside-filter floor is updated only after the CW branch has returned.
Old, Ratio and Gap replays are bit-identical, and the replay reproduces
the radio's weight exactly (0.000 on 489 and 364 moving blocks).

**Yardstick.** `score_cw.py` needs off-tone bins inside the passband, so
it cannot score T-018 below about 400 Hz. Added here (scratch script): the
same tone-to-noise of the key-down spectrum, with the noise from bins
300-900 Hz from the tone. Those pass through the same weight and sit
clear of most of the click energy, which reads +10 to +20 dB over that
noise at 50-200 Hz from the tone during key-down. dB against the better
antenna, segment-length weighted:

| | CW as shipped (the radio) | Window, Old | Window, Ratio | Window, Gap | FSK/Digital |
|---|---|---|---|---|---|
| T-017 pileup | +1.39 | **+2.30** | +2.29 | +1.97 | +1.80 |
| T-018 sweep | +1.23 | +1.94 | **+2.05** | +1.93 | +1.22 |

**CW trails Window by about 0.8 dB here, and the cause is its noise
floor.** On T-018 the losses are concentrated:

- **50 Hz filter:** +0.60 and +0.04 dB, against +2.16 and +2.85 for
  Window. Rounded out to bins, the region is 6 bins, just enough for
  `DIV_CW_MIN_BINS`, so CW acts; but its off-tone floor (bins 4 or more
  from the tone) then rests on one or two edge bins full of keyclick
  sideband. The per-arm SNR reads +10 to +15 dB with the arms level, and
  the Sum weight runs to +17 to +20 dB. At 26 Hz CW holds as designed,
  but on the bad weight it inherited.
- **600 Hz, arm 0 in a 15 dB fade:** −0.53 against +1.85.

**Experiment (scratch build, not committed): CW's noise ratio from the
outside-filter floor** that Ratio already measures for Window:

| | CW as shipped | CW, outside-filter floor | CW, own floor needing ≥ 8 bins | Window, Ratio |
|---|---|---|---|---|
| T-017 | +1.39 | **+1.99** | +1.38 | +2.29 |
| T-018 | +1.23 | **+2.08** | +1.79 | +2.05 |
| T-018, the two 50 Hz segments | +0.60 / +0.04 | **+2.27 / +2.87** | +2.30 / +2.81 | +2.16 / +2.85 |
| T-018, 600 Hz fade | −0.53 | **+2.00** | −0.53 | +1.85 |

`score_cw.py`, where it can score, agrees: T-017's main segment +0.55 →
+0.98; T-018's wide segments within ±0.25. The outside-filter floor is the
one fix that covers both faults; requiring more bins only covers the
narrow filter. It is the across-frequency floor's clearest win so far,
and it only exists on this branch.

**A minimum width for CW's own noise region instead** (scratch builds; the
tone search stays inside the filter, only the off-tone floor is widened
to at least 300 or 600 Hz about the filter centre):

| | CW as shipped | min 300 Hz | min 600 Hz | own floor, ≥ 8 bins | outside-filter floor |
|---|---|---|---|---|---|
| T-018 | +1.23 | +1.55 | +1.69 | +1.79 | **+2.08** |
| T-018, the two 50 Hz segments | +0.60 / +0.04 | +0.69 / +2.71 | +1.29 / +2.81 | +2.30 / +2.81 | **+2.27 / +2.87** |
| T-018, 600 Hz fade | −0.53 | −0.53 | −0.59 | −0.53 | **+2.00** |
| T-017 | +1.39 | +1.38 | +1.38 | +1.38 | **+1.99** |

It helps only the narrowest filters, and not fully. The fade, the wide
segments and the pileup (a 1000 Hz filter, already wider than the
minimum) are unchanged. Bins near the tone carry the station's own keying
sidebands and clicks, which scale with each antenna's signal, so a floor
taken there measures the signal ratio as much as the noise ratio; it
needs to be taken well away from the tone, which the outside-filter floor
is (the central 80 % of the DDC span less the filter and 1 kHz either
side, about 150 kHz at 192 kHz).

## Capture practice, from T-001 to T-012

Captures are started from the Capture button, so the note field in the
file stays empty. The operator's description of each capture is recorded
here when it's ingested. Beyond that, the measurements are what count.

- **Capture signals that are marginal but decodable on at least one
  antenna.** Only there can combining change the decode. T-001's weak
  station and T-007 are below it; T-005's strong signals are above it,
  on one arm.
- **Start before entering diversity** (as T-001 did), so the replay
  starts cold with the radio and reproduces it exactly.


## T-019: the Averaging time, swept over the capture set

**What.** Every wideband capture of 10 s or more (Window, Carrier and
FSK/Digital by the reference it was recorded on: 75, of which 70 scored)
through `run_ref` on `TEST` at `8b13f572`, at 0.2, 0.5, 1, 2, 4, 6, 8 and
10 s: Sum, flat weighting, Min coherence 0.20 (Window) or 0.30 (Carrier,
Digital), Follow RX Filter, the capture's own transform size, 600 runs.
Scored by `score_wideband.py`, the split-guard passband SNR against the
better single antenna ("vs arm"), over the whole capture, cold start
included (it is the same for every row of a capture). 13 RADE V1 captures
through the same engine at 0.2 to 10 s (`--pace 20000`, four at a time),
scored on decode by `score_rade`: synced frames against the better arm.
Drivers: `test/diversity/devtools/py/sweeps/` (LT-023).

**Why.** The slider ran to 30 s, and the question was whether anything
above about 6 s is worth having.

### Wideband, Sum: change against 6 s, in dB

| Averaging | Mean | Better by > 0.1 | Worse by > 0.1 |
|---|---|---|---|
| 0.2 s | +0.14 | 42 | 17 |
| 0.5 s | +0.12 | 36 | 17 |
| 1 s | +0.10 | 35 | 14 |
| 2 s | +0.12 | 33 | 13 |
| 4 s | +0.06 | 13 | 9 |
| **6 s** | 0 | | |
| 8 s | −0.04 | 7 | 12 |
| 10 s | −0.05 | 13 | 23 |

By reference (shorter than 6 s against 6 s; 10 s against 6 s):

- **Window, 37 captures:** +0.30 / +0.23 / +0.21 / +0.18 dB at 0.2 / 0.5 /
  1 / 2 s; 10 s −0.02 (better on 4, worse on 14).
- **Carrier, 6 captures:** +0.45 / +0.42 / +0.41 / +0.29 dB; 10 s −0.12.
- **FSK/Digital, 27 captures** is the exception: 0.2 s averages −0.14 dB
  with a worst case of −3.49 dB (13 better, 10 worse); 2 s is +0.02 (13
  better, 7 worse); 10 s −0.08. It wants about 2 s or more.

**What a cap at 6 s costs.** Taking each capture's best value up to 10 s
instead of up to 6 s gains +0.04 dB on average. Three captures gain more
than 0.3 dB: `122843` (+1.16, 10 s), `231724` (+0.64, FSK/Digital) and
`122632` (+0.56, 10 s). `122843` and `122632` are the two 17 m captures
where Finding 42 says Sum should not be running at all.

### RADE V1, decode: synced frames against the better arm

Mean change against 6 s over 13 captures: +1.9, +1.2, +0.3, +2.0, +0.5 and
+0.1 frames at 0.2, 0.5, 1, 2, 4 and 10 s. No trend: the per-capture
figures move by a few frames either way (`165826` +4 at 0.2 s and −8 from
4 s; `234624` +13 to +22; `180949` loses at every setting, least at 10 s).
10 s beats 6 s by more than 5 frames on 2 captures and is worse on 1; the
best value up to 10 s over the best up to 6 s is worth 1.0 frame.

**What it supports.** The slider's top at 6 s (LC-050). It does not say
that 6 s beats 10 s, only that nothing measured gains from more.

**Not covered.** Null (Findings 18 and 21 find shorter better), CW, Best.
One run per cell; RADE replays repeat to a few frames. The sweep is on
`TEST`'s engine, which has the gate floor (LC-012), flat weighting and the
noise-floor Sum weight that the feature-branch sweeps lacked.

## T-020: the bin width against the Averaging time

**What.** The 54 scored of the 60 wideband captures of 20 s or more at
192 kHz, each through `run_ref --resolution` at 24, 12, 6 and 3 Hz and
0.5, 2 and 6 s (720 runs), same settings as T-019. `run_ref` writes one
weight per engine block, so the weights are resampled onto the capture's
block grid before `score_wideband.py` (the latest engine row finished by
the end of capture block k becomes row k; the scorer applies it one block
late as usual). That alignment is new in this sweep and was not checked
against a native-resolution run. Sum only.

**Result: change against 12 Hz at the same averaging, mean dB (captures
better / worse by more than 0.1 dB).**

| Averaging | 24 Hz | 6 Hz | 3 Hz |
|---|---|---|---|
| 0.5 s | +0.07 (17 / 10) | 0.00 (13 / 17) | −0.12 (13 / 28) |
| 2 s | +0.04 (18 / 9) | +0.03 (15 / 16) | −0.11 (13 / 30) |
| 6 s | −0.03 (16 / 7) | +0.02 (15 / 12) | −0.21 (11 / 26) |

- **3 Hz is behind 12 Hz at every averaging time**, including 6 s, and on
  Window by 0.24 to 0.35 dB (worse on about 20 of 30). More averaging does
  not make narrower bins pay on Window or Carrier.
- **24 Hz** is +0.08 dB on Window at 0.5 s (9 / 4) and level or slightly
  behind later. Carrier (4 captures) is +0.39 dB at 2 s, too few to say.
- **FSK/Digital** (20 captures) is the one reference where finer bins
  sometimes help: 3 Hz +0.18 dB at 2 s, 6 Hz +0.22 dB at 6 s (9 better,
  3 worse). That fits its occupancy split needing resolution.
- **Mapped, "24 Hz at 0.5 s, 12 Hz in between, 6 Hz at 6 s"** against a
  fixed 12 Hz: +0.08 dB at 0.5 s and +0.02 dB at 6 s. Nothing measurable
  in SNR; what the mapping buys is the block period matching the average
  (a 171 ms block is longer than a 0.2 s average) and resolution where a
  reference tracks a narrow feature, neither of which this scores.

**What it supports.** Retiring 3 Hz and the 24 Hz bins (LC-051, now
reached only through Auto). Auto (LC-052, LC-053) is a hypothesis built on
the timing argument, not on these numbers. **Not covered:** Null (Findings 42 and 43: coarse bins win by
0.4 to 6 dB), CW keying (a long block smears the elements; LC-018's key
detection compares block peaks), other sample rates.

## T-021: Auto's thresholds, tested against fixed 12 Hz

**What.** The dense sweep behind LC-052's thresholds: 72 wideband captures
(Window 40, Carrier 6, FSK/Digital 26; 20 s or more, at 192 kHz or below)
through `run_ref` on `test/auto-bins` at 24, 12 and 6 Hz, Sum at 0.2, 0.3,
0.5, 0.7, 1, 3, 5, 5.5 and 6 s and Null at 0.2, 0.5, 1, 3 and 6 s (3024
runs), flat weighting, Follow RX Filter. Scored with
`sweeps/score_dense.py`: Sum as the split-guard SNR against the better
antenna, Null as the passband power against arm 0 alone (more negative is
deeper), each engine's weights resampled onto the capture's block grid. The
captures are split by fade rate (the correlation of the inter-arm ratio at
1 s: fast under 0.66, mid under 0.95, slow above). Intervals are bootstrap
95 % over captures. `sweeps/agg_dense.py` prints it all. **These are the
captures the thresholds were chosen on, not a held-out set.**

**Criterion, set before the run:** Auto no worse than fixed 12 Hz by more
than 0.25 dB (the mean over captures).

### Sum: Auto passes, and the 24 Hz tier is barely there

Auto against 12 Hz, all 72 captures: +0.06 / +0.05 / +0.01 dB at 0.2 / 0.3
/ 0.5 s, exactly 0 from 0.7 to 5 s (it is 12 Hz), and 0.00 [−0.11, +0.11]
at 5.5 and 6 s. No setting is below −0.25 on the mean. The edges:

- **24 Hz on Window up to 0.5 s:** +0.12 / +0.08 / +0.02 dB at 0.2 / 0.3 /
  0.5 s, every interval including zero. Mid-fade captures gain most (24 Hz
  +0.20 to +0.25 dB from 0.2 to 1 s, intervals above zero from 0.5 s to
  1 s); fast-fade captures gain nothing (−0.05 at 0.5 s).
- **6 Hz above 5 s is the weak tier.** Window loses 0.14 to 0.16 dB there
  (interval to −0.30), mid-fade captures −0.12, and the worst single
  captures lose 2.05 dB (`000209`) and 0.9 dB (`001054`). Only FSK/Digital
  gains (+0.16 to +0.18 dB, interval above zero) and Carrier (+0.29, six
  captures, interval across zero).

### Null: 24 Hz wins everywhere; the 6 Hz tier loses

Against 12 Hz, all 72: 24 Hz **+0.19 to +0.25 dB at every averaging time**
with intervals clear of zero (+0.06 at the lowest); 6 Hz **−0.23 to
−0.32 dB**, intervals clear of zero. Auto gives 24 Hz only on Window up to
0.5 s (+0.19 / +0.14 dB overall, +0.35 / +0.25 on Window), so it forgoes
about 0.2 dB from 1 s up, and at 6 s it gives 6 Hz: −0.24 dB overall (just
inside the criterion), **−0.39 dB on FSK/Digital (fails it)**, and the
worst captures lose 4.65 dB (`112151`, FSK/Digital) and 2.94 dB (`001054`).

### What it says about the thresholds

- **Pass:** Auto is within criterion on Sum everywhere and on Null up to
  5 s. Nothing at 5 s or under is worse than −0.1 dB on average.
- **The edge is the 5 s tier.** Above it Null loses 0.24 dB (0.39 on
  Digital) and Sum on Window 0.16 dB, with single captures losing 2 to
  4.6 dB; the gains are Digital and Carrier Sum only.
- **The other edge is 24 Hz's reach.** It helps Null everywhere and Sum up
  to about 1 s (+0.10 dB overall at 1 s, interval above zero), but Auto
  stops at 0.5 s and only on Window.
- **A candidate policy,** worth trying and not yet adopted: 24 Hz up to 1 s
  on Window, Carrier and FSK/Digital (CW and RADE V1 stay at 12 Hz), 12 Hz
  above, and no 6 Hz tier. On this data that is Sum +0.09 / +0.05 / +0.06 /
  +0.06 / +0.10 dB at 0.2 to 1 s and exactly 0 from 3 s; Null +0.25 / +0.19
  / +0.20 dB at 0.2 / 0.5 / 1 s and 0 from 3 s; no tier below zero on the
  mean. Its worst single captures at 24 Hz are −0.9 dB Sum (`111852`) and
  −1.25 dB Null (`122632`, a 17 m capture where Finding 42 says the loop
  should not be running). It was picked after looking at this data, so it
  needs captures it was not chosen on.

### Sample rates: where the edges are

Block period depends on the bin width alone, but which widths exist depends
on the rate (`DIV_MIN_NFFT` 2048, `DIV_MAX_NFFT` 65536):

| Rate | 24 Hz asks for | 12 Hz | 6 Hz |
|---|---|---|---|
| 48 kHz | 23.44 (n 2048) | 11.72 | 5.86 |
| 96, 192, 384 kHz | 23.44 | 11.72 | 5.86 |
| 768 kHz | 23.44 | 11.72 | **11.72** (6 Hz unreachable) |
| 1536 kHz | 23.44 | **23.44** | **23.44** (only one width exists) |

Auto therefore needs nothing special up to 384 kHz. At 768 kHz the 6 Hz tier
is the same as 12 Hz, and at 1536 kHz every tier is 23.44 Hz, as it was
before (a 12 Hz request there was also granted as 23.44 Hz). A 6 Hz tier,
if kept, is dead above 384 kHz. At 1536 kHz a 100 Hz CW filter holds too few
bins either way (LC-029). Only one capture here is above 192 kHz, and none
at 96, 384 or 768 kHz.

**Not covered.** CW (not swept: nobody runs CW at a 6 s average), RADE V1
(Auto leaves it at 12 Hz), Best, and a held-out set.

### T-021, adopted: the policy as now in LC-052

The candidate above was adopted as `diversity_auto_bin_policy()` on
`test/auto-bins` (24 Hz up to 1 s on Window, Carrier and FSK/Digital; 12 Hz
otherwise; no 6 Hz tier). **Revised after the held-out captures below:
FSK/Digital is out of the 24 Hz tier.** The table here is the first version. The same 72 captures through the engine as the radio runs it
(`run_ref --resolution auto`, 1008 runs) give the numbers predicted from
the fixed-width cells, and **exactly** them: the largest difference between
the engine's Auto and the cell the policy names is 0.00 dB. Against fixed
12 Hz, mean gain with the 95 % interval, and the worst capture:

| Averaging | Sum | worst | Null | worst |
|---|---|---|---|---|
| 0.2 s | +0.09 [−0.03, +0.21] | −0.93 | +0.25 [+0.14, +0.38] | −0.99 |
| 0.5 s | +0.06 [−0.03, +0.16] | −0.91 | +0.19 [+0.11, +0.28] | −1.02 |
| 1 s | +0.10 [+0.01, +0.21] | −0.89 | +0.20 [+0.10, +0.30] | −1.25 |
| 3 s and up | 0.00 (it is 12 Hz) | 0 | 0.00 | 0 |

The criterion (Auto no worse than 0.25 dB below fixed 12 Hz) holds with
room: the lowest lower bound is −0.06 dB. Individually, 6 to 11 captures of
72 are worse than 0.25 dB at the 24 Hz averaging times (3 to 5 on Null).
This is not a held-out result: the policy was chosen on these captures.

### T-021, what was still needed (before the held-out captures below)

Captures taken for the purpose, 192 kHz, two antennas, with the operator's
description of the conditions in the findings (the recorder writes the
settings):

1. **Mid-fade SSB, Window, 0.2 to 1 s: 10 captures** (40 m and 20 m evening
   QSOs through multipath; inter-arm ratio correlation at 1 s of 0.66 to
   0.95). This is where 24 Hz gained most (+0.2 to +0.25 dB Sum, +0.3 Null).
   Expect Sum +0.1 to +0.25 dB and Null +0.2 to +0.35 dB over 12 Hz. It
   closes the main claim.
2. **Fast-fade SSB, Window: 8 captures** (20 m near the MUF, 17 m and 15 m;
   correlation under 0.66). 24 Hz gave nothing on Sum here (−0.05 dB) and
   +0.34 dB on Null at 0.2 s only. Expect Sum −0.1 to +0.1 dB and Null 0 to
   +0.3 dB, never worse than −0.25 on the mean. It closes the risk that the
   gain is only on the captures that happened to be mid-fade.
3. **AM broadcast, Carrier: 12 more captures** (49, 31 and 25 m in the
   evening, strong and weak, with selective fading). There are 6; 24 Hz was
   +0.18 dB on Sum at 0.7 to 1 s, about 0 on Null, intervals across zero.
   Expect +0.1 to +0.2 dB Sum, 0 on Null. Also record the carrier readout
   against the known station frequency, because five bins at 24 Hz is
   120 Hz and SNR does not show a tracking error.
4. **Narrow digital modes, FSK/Digital: 8 captures** (PSK31, RTTY 45 baud,
   FT8 and FT4 on a busy band, Olivia). This is the highest risk: PSK31 is
   31 Hz wide, 1.3 bins at 24 Hz, and the occupancy split may miss it. The
   existing 26 cover wider signals. Expect 0 to +0.15 dB on Sum and a
   no-worse-than −0.25 dB result; a loss on PSK31 or RTTY would move
   FSK/Digital back to 12 Hz.
5. **Null against a real interferer: 4 captures.** Every Null figure so far
   is the depth on the wanted signal, because the set has no known
   interferer, and Findings 43's one local-interference capture is
   incoherent between the arms. Take a station and a second rig on the
   same band into both antennas (a low-power carrier or SSB tone), once
   with the interferer coherent between the arms. Expect Null 24 Hz
   +0.2 dB deeper than 12 Hz or more; this is the use Null exists for.
6. **Sample rates: one capture each at 96, 384 and 768 kHz** (Window, mid
   fade). Finding 42 says the block period and not the rate sets the result;
   expect within 0.1 dB of the same signal at 192 kHz at equal bin width. At
   768 kHz 24 Hz exists at 23.44 Hz and 12 Hz at 11.72 Hz, as expected, with
   no 6 Hz; it also tells the Pi 5 what a 32768-point block costs at 43 ms.

Offline, on the existing captures: the same sweep at 0.8, 0.9, 1.1, 1.2 and
1.5 s, to see whether 1 s is an edge or the middle of a slope.

### T-021, held-out: twelve captures taken after the policy was fixed

**Captures** (192 kHz, 60 s, 8192-point blocks, recorded 2026-10-08; the
capture notes were empty and the descriptions below are the operator's, the
labels inferred from their order and the filter): 3 SSB on 15 m near the
MUF with fading ("fairly typical for upper HF, not fast"; inter-arm ratio
correlation at 1 s of 0.82 to 0.93), 4 SSB on 20 m (0.90 to 0.97), 1 FSK on
20 m (mark and space about 1.5 kHz, filter 150 to 2550 Hz, correlation
1.00), 2 PSK31 (several signals at times in the 1.15 kHz filter), 1 multitone
mode, and 1 digital capture on about 14092 kHz. A 0.6 s stub is not used.
Run exactly as the others, with the engine's own Auto
(`run_ref --resolution auto`) as well as the fixed widths.

**Window, 7 SSB captures: the 24 Hz tier is confirmed.** Auto against
fixed 12 Hz, mean (95 % interval), worst capture:

| Averaging | Sum | worst | Null | worst |
|---|---|---|---|---|
| 0.2 s | +0.07 [−0.02, +0.16] | −0.11 | +0.74 [+0.40, +1.15] | +0.14 |
| 0.5 s | +0.04 [−0.03, +0.10] | −0.15 | +0.60 [+0.24, +1.07] | −0.05 |
| 1 s | +0.26 [+0.02, +0.58] | −0.12 | +1.07 [+0.21, +2.30] | +0.02 |

No capture is worse than −0.15 dB. One 15 m capture (`134008`, correlation
0.82) gains +1.18 dB on Sum and +4.5 dB on Null at 1 s. These are
single-recording, one-run-per-cell figures. The 15 m set is mid-fade,
not fast; the fast-fade question stays open.

**FSK on 20 m, one capture:** 24 Hz +0.18 / +0.14 / +0.07 dB on Sum at 0.2 /
0.5 / 1 s. A wide signal, as the old FSK/Digital set was.

**Narrow digital modes, 4 captures: 24 Hz fails, as feared.** Against 12 Hz
on Sum, the 24 Hz tier gave:

| Capture | 0.2 s | 0.5 s | 1 s | 3 s |
|---|---|---|---|---|
| PSK31 a | −2.74 | −3.09 | −3.03 | −3.53 |
| PSK31 b | −1.41 | −1.84 | −1.94 | −1.98 |
| multitone | −2.17 | −2.36 | −4.36 | −1.08 |
| 14092 digital | −1.78 | +1.53 | −0.02 | −0.29 |

A mean of −1.4 to −2.3 dB, with every PSK31 and multitone cell worse than
−1 dB, against the criterion of −0.25. The occupancy split needs the
resolution a 31 Hz signal does not have at 24 Hz. **FSK/Digital is taken out
of the 24 Hz tier** (policy now: Window and Carrier 24 Hz up to 1 s, 12 Hz
otherwise); the old 26-capture FSK/Digital set had missed it because its
signals are wide. At 6 Hz the same four are within ±0.5 dB of 12 Hz on
average (−0.05 / +0.50 / −0.20 / +0.56 / +0.11 dB from 0.2 to 6 s), and
carried by the 14092 capture, which gains +2.5 dB at 0.5 s and +2.3 dB from
3 s at 6 Hz (a narrow-tone mode, presumably FT8, whose tones are 6.25 Hz
apart). One capture is not a policy: finer bins for narrow-tone digital
modes are a candidate for more captures, not a change.

**Null.** Every Null figure is positive for 24 Hz on these captures too
(+0.6 to +2.5 dB, FSK and the digital capture the most), but they
are depth on the wanted signal and the captures have no separate
interferer, so they are not evidence for Null's actual job.

**What is closed.** Window at 24 Hz up to 1 s on mid-fade to slow-fade SSB
(item 1 of the list above, with 7 of the 10 asked for). The failure of 24 Hz
on narrow digital modes (item 4, with 4 of the 8), and its removal. The
engine reproduces the revised policy exactly on all 84 captures; over them
24 Hz on Window and Carrier gives Sum +0.06 / +0.05 / +0.02 / +0.02 / +0.05
dB and Null +0.23 / +0.17 / +0.22 dB at 0.2 / 0.3 / 0.5 / 0.7 / 1 s (Null at
0.2, 0.5 and 1 s), intervals above zero on Null and spanning zero on Sum,
lowest lower bound −0.03.

**What is still open.** Carrier (no new AM captures, six in the set), fast
fades, Null against a real interferer, 96, 384 and 768 kHz, and finer bins for
narrow-tone digital modes (FT8, Olivia, PSK at several signals).

## T-022: interferers - local noise on the MW band, an EMC source, and an AM station beside another

**Captures** (2026-10-08, 14:21 to 14:44; the capture notes were empty and
these are read from the recorded settings and the operator's description):

| Time | Signal | Recorded as |
|---|---|---|
| 142805, 142917 | local noise on the MW band, 698.6 kHz, USB, close AM broadcast | Null, 150 to 3050 Hz, Follow ticked (digital and window references) |
| 143059 | 693 kHz, SAM, wide filter | Sum, Carrier |
| 143304, 143345, 143402, 143436 | the same, EMC source nulled with a window offset (centre +6500 Hz, 3 kHz wide) | Null, FSK/Digital reference, Follow off; the slider moved through 0.2 to 6 s, and **crossing 1 s ends the capture** (143402 starts above it, nfft 16384) |
| 143613, 143739 | the same at 96 kHz and 768 kHz, 1.19 s | Null |
| 144115, 144142 | AM 6130 kHz with local interference on 6135 | Sum, Carrier |
| 144318 | AM 6130 kHz, the 6135 interferer nulled by a window | Null, window centre +6500 Hz, 3.3 kHz wide |
| 144446 | AM 6140 kHz, the 6135 interferer nulled | Null, window centre −5200 Hz, 3.3 kHz wide |
| 142118 | USB, 1.15 kHz filter, FSK/Digital | Sum (a digital capture, not a Null one) |

**The capture stopping at 1 s was Auto.** A capture file has one transform
size, and `diversity_auto_stop()` closes it, so Averaging crossing 1 s (24 Hz
on Window and Carrier, 12 Hz above) restarted the engine and ended the file.
(The same would have happened to a change of the old Resolution control.)
Fixed on the branch: while a capture is recording `diversity_auto_retarget()`
leaves the transform alone, and the menu's status tick catches up afterwards.
The files themselves are fine; they are what a crossing produces.

**Method.** Each capture through `run_ref` with the operator's own window and
Follow setting and objective (not forced as in T-019 to T-021), at 24, 12, 6
Hz and the engine's Auto, at 0.2, 0.5, 1, 1.2, 3 and 6 s (336 runs). Scored
(`sweeps/score_interf.py`) on the **second half of each capture**, where the
loop has settled: the depth in the operator's window (the interferer: the
output's power against arm 0 alone, in dB, more negative is deeper), the
change in the rest of the passband (the wanted signal), and, for the Sum
captures, the Sum SNR against the better antenna. **The first half is thrown
away because `run_ref` starts at w = 1 and the radio had already settled;**
on an interferer 25 dB hotter on one arm the cold start alone dominates the
power and made every cell read +3 to +9 dB until it was excluded.

**The replay reproduces the radio.** The weight the radio actually applied
(recorded in each block) and the engine's own settle to the same magnitude
and phase, and the depth over the second half agrees to 0.06 dB or better
on the six cleanest captures (698.6 kHz −5.52 against −5.52, 6130 kHz −9.65
against −9.65, 6140 kHz −8.58 against −8.58).

### Null depth on a stationary interferer does not depend on the bins or on Averaging

| Capture | depth, 12 Hz | 24 and 6 Hz and Auto minus 12 Hz, 0.2 to 6 s | wanted signal |
|---|---|---|---|
| 698.6 kHz local noise (digital ref) | −5.52 dB | within ±0.01 | 0.00 |
| 698.6 kHz local noise (window ref) | −5.01 | within ±0.01 | 0.00 |
| 693 kHz EMC, four captures | −1.56 to −3.44 | within ±0.01 | +0.5 to +0.6 |
| 693 kHz EMC at 96 kHz | −2.75 | within 0.01 | +0.46 |
| 693 kHz EMC at 768 kHz | −4.30 | within 0.01 | +0.47 |
| 6130 kHz AM, window on 6135 | −9.65 | within ±0.15 | +0.52 |
| 6140 kHz AM, window on 6135 | −8.58 | within ±0.05 | +1.53 |

For every one of these the loop converges to the same weight whatever the
bin width and averaging time, so **Auto costs nothing here and a fixed width
would have gained nothing**: no capture is more than 0.15 dB from the best.
The +0.2 dB that 24 Hz gave Null in T-021 is tracking of a moving wanted
signal, not interferer suppression. At 768 kHz a 6 Hz request is 11.72 Hz,
the same as 12 Hz, as T-021's table said, and nothing else changes at 96 or
768 kHz at 1.19 s: this closes the sample-rate item for this signal.

**The depth is the physics, not the bins.** On the MW captures arm 1 is 22 to
26 dB hotter in the window, and the best single complex weight (computed
directly from the data) is |w| = −27 dB and gives the same depth as the radio
did: −5.5, −5.0, −1.7 and −4.3 dB, not a deeper null that a better setting
would find. The noise is only partly correlated between the arms. The HF AM
captures null the 6135 interferer by −9 dB with |w| = 0.98, at +0.5 to
+1.5 dB on the wanted signal.

### Sum on the three Carrier captures

| Capture | 12 Hz vs better arm | 24 Hz, 6 Hz, Auto minus 12 Hz at 0.2 / 1.2 / 6 s |
|---|---|---|
| 693 kHz SAM, carrier | −3.6 dB | 24 Hz **+0.74 / +0.76 / +0.76**; 6 Hz −0.36 / −0.35 / −0.35; Auto +0.74 / 0 / 0 |
| 6130 kHz AM (a) | +1.3 / +1.1 / +0.9 | 24 Hz 0.00 / +0.04 / +0.04 |
| 6130 kHz AM (b) | +1.4 / +1.5 / +1.5 | 24 Hz −0.03 / −0.01 / −0.01 |

**This says Carrier wants 24 Hz at every averaging time, not only up to 1 s.**
It is also T-021's Carrier result (+0.36 to +0.38 dB at 3 to 6 s over six
captures, interval above zero): with these three, nine captures, a mean of
about +0.3 dB at 3 to 6 s, and none of the three new ones worse than
−0.03 dB. Auto stops
at 1 s on Carrier and gives that up (the 693 kHz capture would gain +0.76 dB
at 1.2 s and at 6 s). Candidate, not adopted: Carrier at 24 Hz at any
averaging time.

**Digital, the 142118 capture:** 24 Hz +0.14 / +0.37 / +1.26 dB and 6 Hz
+0.73 / −0.21 / −0.61 dB at 0.2 / 1.2 / 6 s, with no pattern; it keeps
FSK/Digital at 12 Hz, T-021's finding on narrow modes standing.

**Not covered.** The operator's perception: "nulls very well" is the audio,
and the depth above is the window power, which for the MW noise is −5 dB
against arm 0 alone. The wanted signal in the MW noise captures is not
separated from the noise by anything this measures.

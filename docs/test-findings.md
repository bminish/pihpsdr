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

## T-019 to T-023: RADE V2 on 7.087 MHz LSB, four captures and a binaural WAV

**Logged, not analysed.** Taken 2026-10-05 on `test/binaural-agc-link`,
for the work on a diversity-capable RADE V2 receive chain. `TEST` still
has no V2 reference, so nothing here is replayed yet. The files are in
`captures/` and `wav/`, which git ignores; this entry is the record.

**Captures.** All four at 7.087 MHz, recorded mode DIGL, 192 kHz,
16384-point blocks (85 ms), RX filter −2000 to −1000 Hz. Settings read
from the first block of each file; the later blocks were not checked for
changes. Each has the ear recorder's pair beside it
(`ears-<stamp>.wav` / `.csv`, pre-AF, 16-bit, 48 kHz).

| | Capture | Length | Reference recorded |
|---|---|---|---|
| T-019 | `divcap-20261005-162647.divc` | 60.0 s (703 blocks) | FSK/Digital |
| T-020 | `divcap-20261005-162757.divc` | 60.0 s (703 blocks) | FSK/Digital |
| T-021 | `divcap-20261005-163038.divc` | 33.3 s (390 blocks) | FSK/Digital |
| T-022 | `divcap-20261005-163117.divc` | 60.0 s (703 blocks) | Window |

Operator's description: RADE V2 signals received on the **lower
sideband**. The operator did not describe the stations or the
conditions beyond that; nothing was measured here about decode or SNR.
Like T-009, a V2 correlator has to un-mirror the signal (or search both
banks) before these can be used.

**T-022 holds an attenuator step, the first record of Balance ATT.**
At block 274 (t = 23.4 s, 39 % in) ADC1's attenuator went from 0 to
14 dB in one step; ADC2 stayed at 0 dB throughout. From the file:

| | before (20 blocks) | after (from 4 blocks on) |
|---|---|---|
| arm 0 block power | −44.7 dB | −58.9 dB (−14.2) |
| arm 1 block power | −61.0 dB | −60.8 dB |
| arm 0 − arm 1, block power | 16.3 dB | 1.9 dB |
| arm 0 − arm 1, outside-filter floor\* | 14.4 dB | 0.1 dB |

\* Not the engine's own value, which the file does not record: rebuilt
here as the 10th percentile of the Hann-windowed power in the bins
between 3.5 and 70 kHz from the dial, per arm. It reproduces what
`div_noise_floor_update()` is meant to give closely enough to compare
before and after, not to quote to a tenth of a dB.

- The floor difference before the step (about 14.1–14.4 dB over the 20
  blocks) is what a floor-only Balance ATT reads, and 14 dB is what was
  applied; the residual after is 0.1 dB. Arm 0 fell by the amount of the
  step, so it was band-noise limited at 14 dB. That the whole-block
  powers are 16.3 dB apart against a floor 14.4 dB apart says arm 0 also
  carried more signal.
- The file does not say who made the step (button or hand); the
  operator described it as auto attenuation.
- The engine's weight: in all 703 blocks the loop was **holding** (the
  recorded holding flag; coherence median 0.02, maximum 0.78) and the
  weight takes only two values, **+0.46 dB / 76°** before the step and
  **−13.54 dB / 76°** after. So the loop never moved the weight in this
  capture, and the 14.00 dB change at block 274 is exactly the rescale
  in `diversity_auto_att_changed()`. Arm 0 was ADC1 (no swap): arm 0's
  power fell, so the rescale's sign (weight down when arm 0 is
  attenuated) agrees with the data for that case. The swapped case
  (RX1 on ADC2) is still not covered by any capture.
- Block 274 itself has arm 0 only about 1 dB down: the first block
  recorded with the new setting is mostly pre-step, as the step lands
  mid-block. The statistics were reset by the change (coherence 0.00 at
  274), as `div_context_changed()` is meant to do.

`captures/ears-20261005-161154.{wav,csv}` is also there from earlier the
same day, with no capture beside it and no description.

**T-023: the binaural WAV.**
`wav/20261005-162914_7087.000kHz_DIGL_div-per-ear.wav` (and its `.csv`):
the WAV button's recording, RX1's output as handed to `audio_write()`
before the AF gain, 16-bit, 48 kHz, 72.2 s, presentation `div-per-ear`
(Antenna per ear: arm 0 to the left ear, arm 1 to the right). No `.divc`
was recorded beside it, so it cannot be tied to a capture block for
block.

Operator's description: binaural RADE V2 on LSB. **After demodulation the
audio is no longer inverted**, unlike the I/Q (T-009). This is the
operator's statement; `ears.py` does not test it, and why it is so has not
been traced.

Measured with `ears.py` on the first 40 s of the printed segments: the
two ears are aligned (`rx1_cnt` 1024 throughout, 0 of 13539 blocks
dropped); the left ear sits at about −17.7 dBFS RMS and the right at about
−33 dBFS, 15 dB apart; the L/R cross-correlation is weak (mostly
|r| < 0.2) with a scattered lag and both signs, so the two arms' audio does
not line up in a single segment of this recording. That is a measurement
of this file, not a finding about the antennas.

**What they offer.** A V2 signal on the sideband that mirrors it, from
two antennas at once, with a ready-made per-ear audio reference for a
receiver chain that works after demodulation as well as on the I/Q.

## T-024 to T-027: RADE V2 on 7.0879 MHz USB, two captures and two binaural WAVs

**Logged, not analysed.** Taken 2026-10-05 (17:04-17:26) on
`test/binaural-agc-link`; like T-019 to T-023, for the diversity RADE V2
receive chain, and in `captures/` and `wav/` (git ignores both).
Unlike those, **upper sideband**: recorded mode DIGU, so the I/Q is not
mirrored.

| | File | Length | Recorded |
|---|---|---|---|
| T-024 | `divcap-20261005-170413.divc` | 60.0 s (703 blocks) | Window, filter +1000 to +2000 Hz |
| T-025 | `divcap-20261005-171800.divc` | 60.0 s (703 blocks) | Window, filter +1000 to +2000 Hz |
| T-026 | `wav/20261005-171023_7087.900kHz_DIGU_div-per-ear.wav` | 235.0 s | per-ear |
| T-027 | `wav/20261005-172359_7087.900kHz_DIGU_div-per-ear.wav` | 127.4 s | per-ear |

The captures: 192 kHz, 16384-point blocks, settings from the first block
only. **ADC1's attenuator reads 15 dB and ADC2's 0 dB at the start of both**,
i.e. the arms were already attenuated when recording began. No operator
description beyond RADE V2.

The WAVs are the WAV button's recording (pre-AF, 16-bit stereo, 48 kHz,
presentation `div-per-ear`, CSV mode 1 on both receivers throughout).
Level over 0.5 s segments:

| | L mean | R mean | L − R mean (sd) | range of L − R |
|---|---|---|---|---|
| T-026 | −15.5 dBFS | −16.9 | +1.3 (3.2) | −5.1 to +9.7 |
| T-027 | −14.8 dBFS | −18.3 | +3.5 (3.0) | −2.5 to +10.0 |
| T-023 (for comparison) | −17.7 | −32.8 | +15.1 (0.8) | +13.8 to +17.8 |

**Is the AGC linked in them? Consistent with it, not shown.** The file
does not record the link. In all three per-ear WAVs the louder ear holds
near one level (spread 0.6-2 dB over 0.5 s segments) while the quieter ear
moves more (1-3 dB), which is what a shared gain driven by the stronger arm
gives and not what two independent AGCs give. In T-026 and T-027 the two
ears also swap which is louder (L > R in 58 % and 87 % of the segments).
Two independent AGCs could produce something like it if one arm sat below
its threshold, so this is evidence from the level pattern only. A WAV
beside a capture would settle it: the ear level ratio would follow the
arms' power ratio with a shared gain and sit near 0 dB without.

**Discarded.** Three files taken in the same session were not binaural and
were deleted on 2026-10-05, with their CSVs: `wav/20261005-170548_7087.900kHz_DIGU_div-sum.wav`
(presentation `div-sum`: summed, left identical to right) and
`captures/ears-20261005-170413.wav` and `ears-20261005-171800.wav` (the ear
recorder's files beside T-024 and T-025: CSV mode 0, summed, left
identical to right). The two `.divc` captures were kept.

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


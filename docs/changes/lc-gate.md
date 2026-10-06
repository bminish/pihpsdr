# The estimate and its gate

What the Window, Carrier and FSK/Digital references accumulate, and when the result is allowed to become the weight.

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-006"></a>

## LC-006 — Retire Coherence weighting; Window threshold 0.30 → 0.20

**Why.** We measured Coherence weighting against recorded two-antenna
captures, and it gave no benefit:

- It is not a better estimator. With the gate out of the way, Flat was
  as good or better on 4 of 5 captures (by up to 0.66 dB).
- Its apparent benefit was a bias in the gate. Weighting bins by their
  own coherence raised the reported coherence on every capture,
  including noise-only ones, so the same threshold became a looser test.
  At equal false-alarm rate, Flat keeps more signal blocks and gives
  better output SNR at every operating point.
- On weak SSB, the case it was meant to help, Flat was ahead or level on
  20 of 24 measurements (by up to 0.93 dB). On strong signals the choice
  makes no difference.

Flat at 0.20 gives slightly fewer false alarms than Coherence at 0.30
(5.2 % against 5.7 %) and about 0.3 dB more signal. So the Window
threshold default moves with the weighting.

**Change.** The Weighting control is removed from the menu, and the
setting is pinned to Flat on load, so an older props file or client
cannot bring it back. `DIV_WEIGHT_COHERENCE` stays in the enum, and the
field stays in the wire protocol and the props file, so neither format
changes.

**Not migrated.** Only a fresh install gets the new 0.20 default. A
props file that already holds `diversity_band_cohmin=0.30` keeps it,
which is a slightly stricter gate than intended under Flat. This is a
test branch, so we don't migrate it; an operator can move the slider.

**Kept local, deleted in the PR:** the Coherence-weighted code path,
which only `run_ref --weighting coherence` can reach. See [git-workflow.md](git-workflow.md#cutting-a-pr-branch).

---

<a id="lc-012"></a>

## LC-012 — The coherence gate never goes below its own noise floor

**Why.** Random tracking is not useful. The gate compares Min coherence
against a coherence *estimate*, and over N independent samples two
unrelated noises reach a coherence of about 1/N by chance. Below that, a
threshold passes noise-only blocks and the loop fits a weight to an
accident. When there is nothing real to correlate on, the right answer
is to hold where we were, because that is the best chance of being right
when the signal returns.

**Change.** Each block, the gate compares against the larger of the
operator's setting and the coherence that pure noise reaches 0.1 % of
the time over the bins and blocks actually in that estimate:

- **Bins.** The window, the carrier tracker's five bins, or the occupied
  span on FSK/Digital. Neighbouring 4-term Blackman-Harris bins are
  82 / 44 / 15 % correlated at 1 / 2 / 3 bins apart, so n bins count as
  n² / Σ|ρ(j−k)|² independent samples (about n / 2.76 on a wide window).
- **Blocks.** The engine tracks (Σw)² / Σw² for the exponential average as
  it runs. That's (2−α)/α in steady state, but only one block right after
  a reset or retune, which is when noise is most easily mistaken for
  signal. An averaging change doesn't reset; the count follows the new α
  over the next few blocks.

So the floor follows the reference, window or filter, bin width and
averaging time. RADE V1 gates on its pilot and is unaffected. The Min
coherence slider now steps in half percent, and its bottom follows the
floor on the menu's status tick (on FSK/Digital, from the search region
rather than the occupied span, since LC-045). The operator's own setting is never
overwritten: when the floor falls again, their value reappears.

**Validated** by Monte Carlo (`docs/tools/coh_floor_mc.py`): two
independent noises through the same window and average, share of blocks
passing the gate:

| Case | Floor | At the floor | At the old default |
|---|---|---|---|
| Carrier, 5 bins, 0.2 s | 50.0 % (capped) | 0.11 % | 1.92 % (0.30) |
| Carrier, 5 bins, 2 s | 6.5 % | 0.27 % | 0.77 % (0.30) |
| Digital, 200 Hz occupied, 0.2 s | 20.2 % | 0.03 % | 0.01 % (0.30) |
| Window, 2.4 kHz, 0.2 s | 1.9 % | 0.05 % | 0.00 % (0.20) |

- The 0.77 % at the Carrier default with 2 s averaging comes almost
  entirely from the first blocks after a reset. Only a floor that counts
  the blocks actually held can catch that.
- The floor from `01df2313`, which assumed every bin independent and
  steady state, lets noise through 4–8 % of the time at the same target.
  It was also enforced only from the menu, so it did nothing while the
  menu was closed or when a client changed settings.

**Trade-off.** On the Carrier reference at short averaging, a weak
carrier now needs a coherence of up to 0.5 before the loop acts on it.
Longer averaging lowers the floor. That's the intended exchange: the
loop waits for evidence instead of tracking noise.

**Scored on recorded captures (2026-09-30).** Seven weak captures from
the findings, covering all three references, run through `TEST`'s engine
with and without LC-012 (`run_ref`, Flat weighting, recorded averaging)
and scored with `test/diversity/devtools/py/score_wideband.py`. The
scorer reproduces Finding 38 on `235906` (12.56 against 12.75 dB at gate
0, 4.28 against 4.24 at 0.30). Split-guard passband SNR, before → after:

| Capture | Ref, averaging | At `TEST`'s default threshold | Threshold 0 (floor only) | Noise-only passes at 0 | Noise coherence p95 |
|---|---|---|---|---|---|
| `235906` 17 m USB | Window, 1.12 s | 4.21 → 4.21 | 12.94 → 6.28 | 99 → 50 % | 0.71 |
| `123333` 17 m USB | Window, 0.32 s | 5.26 → 5.26 | 8.77 → 8.65 | 100 → 90 % | 0.11 |
| `122843` 17 m USB | Window, 0.32 s | 2.74 → 2.74 | 2.79 → 2.61 | 99 → 89 % | 0.37 |
| `011225` 60 m AM | Window, 0.20 s | 29.90 → 29.90 | 29.89 → 29.89 | 100 → 100 % | 0.94 |
| `000412` 13.72 AM | Carrier, 0.20 s | **26.08 → 25.94** | 26.23 → 25.94 | 100 → 67 % | 0.95 |
| `000537` 13.65 AM | Carrier, 2.19 s | 22.10 → 22.10 | 22.12 → 22.14 | 99 → 96 % | 0.79 |
| `003309` FSK | Digital, 0.20 s | 18.96 → 18.96 | 19.15 → 19.12 | 97 → 89 % | 0.70 |

What landed where expected:

- **At the defaults it is inert** on six of seven, identical to the last
  decimal, because the floor sits far below 0.20 or 0.30 there.
- **It acts on Carrier at short averaging**, as predicted: on `000412`,
  noise-only passes fall from 84 % to 67 %, for −0.13 dB.
- **It holds after a reset.** On `235906` at threshold 0, the loop first
  acts at block 55, not block 0: the opening dead air is uncorrelated,
  and it is held.

What did not:

- **Real dead air is mostly correlated noise, not uncorrelated noise.**
  Noise-only blocks reach a coherence of 0.11 to 0.95 (95th percentile),
  far above the floor. That's common-mode or band noise, a real
  correlation, which the floor correctly lets through. So the drop in
  noise-only passes is much smaller than the Monte Carlo suggests.
- **Tracking uncorrelated noise does not cost 3 dB with the flat Sum
  weight.** The premise was a unity-magnitude weight with random phase.
  But Sum's weight is Sxy/Sxx, whose magnitude shrinks with the
  coherence. Before LC-012, the median |w| in dead air is −19.6 dB
  (`235906` with its arms matched by `match_arms.py`) and −14 dB
  (`123333`, matched): effectively arm 0 alone, a stand-down that happens
  by itself. On matched arms, where the penalty should be largest, the
  pre-LC-012 engine at threshold 0 scores the same as at the default
  (`123333`: 2.83 against 2.87 dB).
- **Where the floor does bind, holding costs a little.** It keeps the
  station's weight (about −5.6 dB) through dead air instead of letting it
  shrink: −0.47 dB on matched `235906`, −3.9 dB on the real, lopsided one
  with the cold start excluded, −0.1 to −0.3 dB elsewhere. That only
  happens with the threshold below its default.

**Assessment.** Keep LC-012. It's inert at the defaults, costs 0.13 dB
in the one case it was expected to act on, and it implements the hold
rule. But its rationale is narrower than stated: on this set the floor
prevents no measurable harm with the Sum objective, and "random tracking
costs about 3 dB" is not supported for flat Sum. What the data shows
instead is the stand-down question in a new form: in dead air, a Sum
weight that is allowed to track shrinks on its own, and holding pays for
the station's weight. Not measured: Null and Best, and Carrier or Digital
on matched arms.

**Since 2026-10-01.** `div_coh_range_update()` moves the slider with
`coh_cb()` blocked rather than under the `updating_from_auto` flag, which
upstream removed. Behaviour is unchanged: the floor is never filed as
the operator's setting.

---

<a id="lc-013"></a>

## LC-013 — Bins in the operator's manual notches are left out of the estimate

Ported from the notch parts of `d3b73b8a` and from `8117d9c7`
(`feature/diversity-binaural`), ahead of the CW correlator, which will
call it. It doesn't depend on CW, and on binaural it was bundled into the
CW keying commit.

**Problem.** The analysis taps the two raw antenna streams, upstream of
WDSP; the manual notch is applied a long way downstream. A notched
interferer was still in our spectrum at full strength, and every
reference that works from the transform picked it as a peak and fitted
the weight to it.

**Change.**

- The three notches join the analysis context. Enabling, moving or
  resizing one restarts the statistics; a disabled notch isn't compared.
- `div_bin_notched()` drops any bin lying entirely inside an active notch
  from the Window accumulation and the combine over it, the Carrier peak
  search, and all four passes of the FSK/Digital occupancy split.
- There's no CW special case. The notch sits in the same frame as
  `div_frame_off()` (checked against `rx_set_offset()` on `TEST`), so a
  notch centre maps to bin frequency −centre and the sidetone cancels.
- RADE V1 is the exception: the correlator works in the time domain, so
  there are no bins to leave out.
- LC-012's floor counts the bins actually used, so a notch raises it
  correctly with no further change. Binaural's per-bin count correction
  to the per-arm SNR isn't needed on `TEST`, whose per-arm floor is taken
  over the same bins and resets with the notch.

**Checked.**

- With no notch set, fourteen `run_ref` replays (seven captures, three
  references, two thresholds) are byte-identical to the engine before
  it.
- `test_modes_live`'s notch pass (every reference, signal notched out)
  now counts, and passes: none converges on the notched channel.
- It applies to `upstream/TEST` on its own and builds.

**Not yet measured.** The mechanism was confirmed on air on the feature
branch (an operator notched an interferer and watched the combiner stop
following it). Its magnitude has never been measured: a capture can't
record a notch. LT-004 makes that possible by replaying with the notch
set. The validation scenario is still to be planned.

**What the capture set offers so far** (`score_wideband.py --peaks`, four
captures checked):

- `000412` (13.72 MHz AM) has the only truly steady carrier: 100 % of
  blocks, +42 dB over the passband median. But it's the wanted signal's
  own carrier. It served as the plumbing check: a 30 Hz notch over it
  removes 9 passband bins from the score and changes the Carrier
  reference's answer.
- `143433` (20 m CW) has narrow peaks near the zero beat, but they're
  present in only 36–40 % of blocks. A 40 Hz notch over the strongest
  moved the Window weight by less than 0.001.
- `235906` and `122843` (17 m USB) have no steady narrow peak: the
  strongest are present in 4–16 % of blocks.

The case LC-013 is for is a **steady interferer that isn't the wanted
signal, inside the passband of a weaker station**. None of these shows
it. The scenario therefore needs either:

- a `--peaks` scan of the whole capture set for steady peaks in SSB or
  Digital passbands; or
- new captures taken for it with `make DIVCAP=1`: a heterodyne or carrier
  over a weak SSB or Digital signal, recorded in pairs with and without
  the notch, back to back. The notch doesn't show in the file, so which
  run had it, and where, is recorded in `docs/test-findings.md` when the
  captures are ingested.

What to measure once there is one: split-guard SNR with the notch
applied to both the weight and the score, notched against un-notched,
per reference (Window, Carrier, Digital). Also how often the loop acts,
and whether the weight stops following the interferer.

---

<a id="lc-045"></a>

## LC-045 — The slider's bottom does not follow the occupied span

**Why.** LC-012 puts the bottom of the Min coherence slider on the
gate's noise floor, from `diversity_auto_coh_floor()`, on every status
tick. On FSK/Digital that floor came from the occupied span, which the
engine re-estimates each block and which comes and goes with the signal.
The floor is steep in the bin count (about 20 % for a 200 Hz span at
0.2 s, a few percent for the whole region), so the range and the slider
with it jumped about, and again when the span went invalid and the floor
fell back to the search width. Reported on FSK/Digital; Carrier, Window
and CW use a fixed bin count and never showed it. LC-012's scoring used a
fixed span, which is why nothing caught it. Found from the code, not
measured on air.

**Change.** On FSK/Digital the function gives the floor for the search
region (the filter when following it, otherwise the window), as for
Window. That is the lower floor, so the slider never claims more than the
gate holds to. The gate is untouched: `div_gate_threshold()` still uses
each block's own bin count, so on a narrow signal it is stricter than the
slider shows. The operator's setting is not written.

**Confirmed in use, 2026-10-05:** on FSK/Digital the slider stays put.

**Checks.** Needs LC-012. Builds; the suite passes (known gaps
unchanged). Nothing in the suite moves the span under the menu.

---

<a id="lc-046"></a>

## LC-046 — The Carrier reference follows the RX filter

**Why.** The carrier tracker (`diversity_auto.c`, the peak search before
the bin range is re-aimed) always searched the hand-placed window,
centre and width, and never read the Follow RX Filter flag; the
panadapter drew the same window for Carrier. With Follow ticked the menu
greys the centre and width out, yet they still decided where the carrier
was looked for. Reported on AM/SAM. Not a regression of a recent change:
the search has used the hand window since the original auto-diversity
code, while Window, Digital and CW follow the filter. Found from the
code, not measured on air.

**Change.**
- With Follow ticked the search region is 400 Hz centred in the passband
  (the whole passband if narrower), `div_carrier_follow_window()`. Not the
  whole filter, so that a sideband peak cannot win over the carrier.
- The Carrier reference's own default width is 400 Hz (was 1000), also
  where an invalid saved width is reset.
- Unticking Follow seeds the hand window with that window rather than the
  whole filter (`diversity_auto_seed_window()`).
- The panadapter shading uses the same helper, so what is drawn is what
  is searched.
- Window, Digital, CW and RADE V1 are unchanged. A saved Carrier width of
  exactly 1000 Hz stays, since it cannot be told from a deliberate one.

**Confirmed in use, 2026-10-06:** Carrier follows the filter with Follow
ticked.

**Checks.** Builds. Not in the suite: the harness does not move the
Follow flag for Carrier.

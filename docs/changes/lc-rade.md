# RADE V1

The pilot correlator: how a lock is found, held and replaced, and how it is shown. LC-010, LC-011 and LC-014 go upstream together; LC-011 must never be taken without LC-010.

Part of the local-change register: [changes.md](../changes.md).

---

<a id="lc-010"></a>

## LC-010 — RADE resyncs on a detection, not on a timeout

Ported from `567edf07` on `feature/auto-diversity` (`src/rade_correlator.c`
only; it applied without changes).

**Problem.** The correlator only searched for a pilot when it was not
tracking, and tracking was cleared only when Hang expired. So Hang did
more than hold the weight: it switched the search off. At a changeover
between two stations the correlator spent the whole of Hang correlating
the old station's timing, and looked nowhere else. That cost about 10 s
of Hang + 2.9 s of search + 0.8 s of confirmation, or **13.7 s**, before
the new station's weight was in force (Finding 44, from six 80 m
captures).

**Change.** While the lock is frozen, the correlator runs the full
acquisition ladder once per modem frame, as a cold search does.

- A candidate at a **different timing alignment** (modulo one modem frame,
  more than 4 of 960 samples away) is a new station. The old lock is
  dropped through `rade_corr_reset()` and the candidate is taken.
- The **same alignment** is the same station coming out of a fade, which
  keeps its lock and its averages.
- Timing decides, not frequency, because the frequency loop's lock
  points are 8.33 Hz apart and two stations can differ by an amount it
  cannot see.
- While frozen, the pilot averages **decay at the Averaging time** instead
  of being held outright, so a new station does not inherit most of an
  average built on one that stopped seconds ago. The decay stops 30 dB
  down.

**Holding is preserved.** The applied weight is only ever written from a
confirmed lock. It stays in force through the freeze, the search and a
new candidate's confirmation, and when nothing new turns up it stays
where it was.

**Validated** (on the feature branch, Findings 44 and 45):

| Test | Result |
|---|---|
| `test_rade` two-station changeover | 3.50 s, against 13.5 s |
| Same, at a 2 s and a 10 s Hang | 2.82 s and 2.90 s: Hang no longer matters |
| Same station, noise floor 31 dB higher for 5.1 s | Lock held; weight back to within 0.033 |
| Six 80 m on-air captures, scored on decode | +53, −29, four unchanged: not measurable |

The on-air set could not show a difference, because in every capture
one antenna alone decoded 97.9–100 % of frames. A capture with a marginal
signal on both antennas is still needed. `test_rade` has since come
across (LT-002), and both synthetic checks pass on `TEST`.

---

<a id="lc-011"></a>

## LC-011 — The Hang slider is removed

Ported from the Hang part of `01df2313` only; that commit's other five
changes are not taken. **Superseded in part by LC-014:** the value this
change pinned no longer does anything.

**What it does.** Removes the Hang slider and its callback.
`div_settings_validate()` pins the value (`DIV_HANG_DEFAULT`, 10 s)
instead of range-checking it, so an older props file or client can't
bring one in. The field stays on the wire and in the props file, so
neither changes shape.

**Why.** Hang only ever existed for RADE V1. The other references work on
the coherence gate and Averaging alone: the averages update every block,
the gate decides whether the result is applied, and otherwise the last
weight is held. With LC-010, RADE takes a new station on detection too.
So nothing is left for an operator to set.

**What this entry used to say, and why it was wrong.** It argued that the
setting "does not matter" and kept 10 s because "it re-acquires least
often". Three things were wrong with that:

- **The evidence was stale.** It came from Findings 33, 35 and 41, which
  were measured before the resync search existed, when Hang still gated
  the search. The first bullet also merged two captures: the 38 % → 94 %
  uptime was `165826`'s, and the "+10, +11, +10, +10" frames were
  `234624`'s.
- **It doesn't hold on `TEST`.** Re-measured with LC-010 in place, a
  short timer is harmful (−51 frames on `165826` at 2 s). See LC-014.
- **It framed a clock as a choice.** A timer that discards a lock nothing
  has contradicted is arbitrary however long it is. See [settled-decisions.md](settled-decisions.md).

**Order.** Must not be taken without LC-010. It also goes after LC-008,
because the slider it removes sits directly under a line LC-008 adds.

---

<a id="lc-014"></a>

## LC-014 — A RADE lock has no timeout: a new lock replaces an old one

**Problem.** After LC-010, the Hang timer's only remaining act was
`rade_corr_reset()` on a lock nothing had contradicted, discarding it
when a clock ran out. The weight was held either way, and a new station
was already taken on detection.

**Measured** on `TEST` through the whole engine (`run_ref`, Sum) and
scored on decode by `score_rade`. Three runs each; synced frames against
the better single antenna:

| Capture | 2 s timer | 10 s timer | No timer (600 s) |
|---|---|---|---|
| `165826` (marginal) | **+6** | +57 | +57 |
| `190516` | **+42** | +72 | +72 |
| `190715` | **+47** | +57 | +57 |
| `190932` | +3 | **−2** | +3 |
| `202743`, `112151`, `190822`, `193105` | identical at all three | | |

A short timer drops locks that would have recovered. A long one does
nothing, or costs a little. The timer never helps.

**Change.**

- The timeout is gone. A lock is held, weight and all, until the resync
  search finds a new one, or a retune or other context change resets
  everything.
- While the pilot is absent the averages age at the Averaging time, as
  LC-010 made them.
- The `hang` argument is removed from `rade_corr_process()` and
  `rade_track()`. The Hang field stays on the wire and in the props file,
  pinned, and nothing reads it.
- CPU: a held lock with no pilot runs the same search a cold one does, so
  it costs what having no lock would.

**Accepted, deliberately.** A new station landing within 4 samples of the
old timing (0.9 % of changeovers) is taken for the old one returning. In
a synthetic test (station B at A's exact timing, 20 Hz away), the lock
was not replaced within a minute. A rule to catch that case was built
and measured: across all 38 RADE captures in the set, a pilot found at
the held timing while frozen happened 6 times, never twice in a row, so
the rule would have been safe. It was still rejected. If the impostor's
pilot is strong enough the tracker follows it and Averaging corrects the
weight; if not, the old weight stays. Neither outcome is worth a
mechanism. See [settled-decisions.md](settled-decisions.md).

**Checked.**

- `test_rade` passes: a changeover costs 3.50 s at any Hang value, and a
  5.1 s fade at 31 dB worse SNR keeps its lock and its weight.
- The whole-engine decode scores match the committed engine at a 600 s
  timer on all eight RADE captures tried.
- The unit suite passes.
- `run_ref` is not byte-deterministic (see LT-005), so these figures come
  from repeated runs.

**Depends on** LC-010 and LC-011.

---

<a id="lc-016"></a>

## LC-016 — RADE V1's Min coherence is retired: the pilot already gates

Ported from `082dba0b` (`feature/auto-diversity`).

**Problem.** In RADE V1 the Min coherence slider doesn't gate a
coherence. It gates `rade_corr_quality`, the pilot's signal fraction,
across the slider's full 0–95 % range, and it can only do harm:

- **The job is already done.** Three pilot gates stand in front of it:
  the acquisition ladder, confirmation and probation, and the per-frame
  freeze. On the five no-signal captures they produced no weight at all,
  over 3,515 blocks.
- **Quality doesn't separate a good lock from a poor one.** `234508`
  (strong, a weight on three blocks in four) reads a median 0.217 with
  31 % of blocks under 0.05. `202743` (re-acquires eight times a minute)
  reads 0.193.
- **No reachable setting was safe.** On `165826`, the marginal capture
  where the combiner beats both antennas, moving it from 0 to 0.15 took
  the loop from a weight on 32.6 % of blocks to 1.7 %.

**Change.** Pinned at 0, its default, so nothing an operator has today
moves. It's pinned at every route in:

- `div_settings_validate()` pins it instead of ranging it;
- `div_settings_load()` ignores the incoming value, and LC-026's switch
  gives 0 as the live threshold on RADE V1;
- the menu's `store_ref_values()` no longer files the live value into
  its slot, and `restore_ref_values()` brings in 0.

(Before the 2026-10-01 rebase these were `div_cohmin_for_ref()`,
`diversity_auto_ref_store()` and LC-004's client path.)

The menu hides the Min coherence row while RADE V1 is selected. The field
stays on the wire and in the props file, and the engine's comparison
stays, so `run_ref --cohmin` can still sweep the retired path. That
comparison is kept local and deleted in the PR (see [git-workflow.md](git-workflow.md#cutting-a-pr-branch)).

**Checked.**

- `test_props`: a stored 15 % comes back as 0, and a client sending 30 %
  on RADE V1 leaves both the live gate and the slot at 0. The client
  check fails without this change.
- The unit suite passes.
- RADE decode through the whole engine is unchanged: `190516` +72,
  `165826` +57 synced frames.

**Depends on** LC-008 (the slider pointer) and LC-026 (the switch).

---

<a id="lc-031"></a>

## LC-031 — A RADE V1 correlator that cannot start no longer changes the reference

**Why.** If the pilot correlator could not start at the DDC rate,
`diversity_auto_start()` set `div_auto_ref` to `DIV_REF_DIGITAL_IQ`. The
engine was changing a menu setting (E1 in [ownership.md](ownership.md#review-e1-to-e8)): the menu
showed RADE V1 while the loop ran FSK/Digital, and the next props save
stored the substitute.

**Change.** The substitution is gone. The reference is left as set and
the loop holds. When the correlator isn't running, `rade_corr_process()`
returns no weight, `rade_corr_stop()` returns at once, and
`rade_corr_reset()` only clears plain variables, so the failure has no
blast radius. The menu reads "search". The only trace is
`rade_corr_start()`'s own log line.

**Reach.** It cannot happen today: every rate piHPSDR offers is a
multiple of 8 kHz. So, deliberately, there is no status flag and no menu
change (decided 2026-10-01). The first cut, with a
`div_auto_rade_unavailable` flag and an "n/a" status, is kept on
`history/backup/TEST-lc031-flag-20261001`.

---

<a id="lc-038"></a>

## LC-038 — The RADE V1 overlay covers the outer carriers whole

**Problem.** The green overlay drawn on the RX panadapter for the RADE V1
reference ran from `RADE_CORR_FLO` to `RADE_CORR_FHI`, 750 to 2200 Hz.
Those are the centres of the first and last carriers. radae spaces the
30 carriers 50 Hz apart (Fs/M = 8000/160) and puts carrier 1 in bin 15
(`rade_ofdm.c`: 1500 − 50·30/2 = 750), so each carrier spends 25 Hz
beyond its centre. The overlay cut the outer two carriers in half. Seen
on air as "the overlay leaves out a little of the edge furthest from
the carrier".

**Change.**
- New `RADE_CORR_OCC_LO`/`RADE_CORR_OCC_HI` in `rade_correlator.h`: the
  outer centres widened by half the carrier spacing, written in terms of
  `RADE_CORR_FS`/`RADE_CORR_M` (725 and 2225 Hz). The overlay draws those,
  mirrored for LSB as before.
- `RADE_CORR_FLO`/`FHI` are unchanged. The sideband test in
  `div_rade_side_expected()` wants carrier centres, and the comment now
  says that is what they are.
- Comment: the modem's centre is 1475 Hz, not 1500. radae aims at 1500
  and rounds the first carrier to the 50 Hz grid.

**Not changed (decided 2026-10-02).** The overlay is still drawn at the
nominal position. It isn't shifted by the tracked frequency offset
(`rade_corr_freq_off`), so a station that is off-frequency still shows
its far edge outside the overlay by the offset.

**Checks.** Applies to bare `upstream/TEST` and builds; the suite passes.
The overlay code is upstream's (`df47f59a`), so there's no LC dependency.
On air (2026-10-02): the overlay covers both outer carriers.

**Operating note (local config, not in the commit).** The three
radios' `.props` files set the digi-mode Var filters for RADE V1, both
centred 1500 Hz from the carrier: Var1 1600 Hz wide (DIGU 700 to 2300 Hz,
DIGL −2300 to −700 Hz), which covers the occupied 725 to 2225 Hz with
25 Hz to spare each side, and Var2 1000 Hz wide (1000 to 2000 Hz). The
stock 2.0k preset is left as shipped. A reshaped 2.0k was tried and
dropped because its button would still read "2.0k": the title is
compiled in and is also the props key. The 1.5k preset (750 to 2250 Hz)
cuts the lowest carrier at its centre.

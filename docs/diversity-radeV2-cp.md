# RADE V2 diversity: a cyclic-prefix tracker

> **Branch `feature/rade-native`**, 2026-10-21 and 22. A V2 counterpart of the V1 pilot
> correlator, built on the one structure V2 has: the cyclic prefix. This file records
> what was checked, what was built, what the evidence is and is not, and how the
> engine hook is to be done so it can go to `TEST` as a patch. Background on V2 and the
> decode-based scoring that came before: `docs/diversity-radeV2.md` (on
> `test/radeV2-correlator`; last version at commit `2b628fe7`) and
> `docs/diversity-radeV2-combining.md` (`8e0098b4`).

## 1. Summary

- **V1 is tracked correctly.** `src/rade_correlator.c` reproduces rade_c's pilot, carrier
  grid, frame timing and acquisition range, and the sideband rule in its header agrees
  with an independent check from the cyclic prefix (section 2).
- **V2 can be tracked from its cyclic prefix.** V2 has no pilots, but every 160-sample
  symbol begins with a copy of its own last 32 samples. The lag-128 autocorrelation over
  a 32-sample window, folded over the 160-sample symbol period, is a triangle where the
  window sits on the prefix and flat elsewhere. A tone or a 62.5 Hz comb gives a flat
  pedestal (removed); speech, SSB, noise, STANAG data and V1 give no triangle.
- **The same prefix gives the inter-arm phase with the interference left out.**
  `y1[n] * conj(y0[n+128])` over the prefix is `h1 * conj(h0) * S`; a common-mode
  interferer is uncorrelated 16 ms later and drops out. The lag-0 cross-spectrum follows
  whichever source is strongest instead.
- **Built:** `src/rade_v2_correlator.[ch]` (C, standalone, same shape as the V1 API), the
  Python reference it was ported from, a capture replay tool, the engine reference
  `DIV_REF_RADE_V2` beside V1, its menu row and overlay, and the checks (LC-064 to LC-067,
  LT-037; section 8).
- **Not done:** no decode scoring of the weight, nothing on air, the menu row and log lines
  not seen running. The next step is the operator's on-air evaluation (section 9), on
  purpose before any decode score.

## 2. V1: what was checked

Compared `src/rade_correlator.c` with `third_party/rade_c-src/src` (pin `cc17222`):

| Item | rade_c | `rade_correlator.c` |
|---|---|---|
| Pilot | Barker-13 on every carrier, scaled by sqrt(2), IDFT | same (`rade_corr_make_pilot`) |
| Carriers | 30, index 15 to 44, 750 to 2200 Hz at 50 Hz | `RADE_CARRIER_K0/K1` 15 and 44 |
| Frame | pilot first, then 4 data symbols; 960 samples, 120 ms | `RADE_FRAME_SECS` |
| Acquisition range | +/-50 Hz | `RADE_ACQ_FRANGE` 100 Hz total |
| Frame-rate frequency alias | n/a (our own loop) | resolved by the half-pilot lag, `RADE_ALIAS_LAG` |

Independent check of the **sideband rule**: the same cyclic-prefix machinery with V1
parameters (lag 160, period 192) finds V1 on the as-tapped bank on the 7.177 MHz LSB
captures (prefix contrast 0.68) and on the mirrored bank on the 5.3685 MHz USB captures
(0.67); the wrong bank is at noise (0.06 to 0.12). That is the rule in the header of
`rade_correlator.c` (bank 0 is the LSB bank, the tapped buffer being inverted with respect
to RF).

**Limits of this review:** parameters, geometry and the sideband rule were compared. The
1800 lines were not audited for bugs and the correlator was not run for this.

Not a fault, but noted: V1 correlates one symbol in six (the pilot). The prefix of every
symbol is available as well, and its lag (160, against 128 for V2) separates the two
waveforms, so a future "RADE" reference could tell V1 from V2 by itself.

## 3. V2 as rade_c has it

From `rade_v2_ofdm.h` and `rade_rx_v2.c`:

```
Fs   8000 Hz    modem sample rate
Nc   14         carriers, 62.5 Hz apart, 1062.5 .. 1875 Hz
M    128        samples per symbol (useful part)
Ncp  32         cyclic prefix
Ns   2          symbols per modem frame: 320 samples, 40 ms
```

No pilots. rade_c's own receiver finds timing, frequency and its SNR estimate from the
same prefix: `compute_autocorr` over the 160 offsets, IIR-smoothed (0.95), peak above
0.38 and not flat (`TSIG`, `TSIN`). The only known waveform is the end-of-over frame.
Because the 62.5 Hz carrier spacing is exactly Fs/M, every carrier is periodic over 128
samples, which is why the prefix repeats cleanly at lag 128.

## 4. The method

Everything is done on the raw DDC tap, as for V1: shift by `frame_off`, decimate to
8 kHz, then per bank (0 = as tapped, the LSB bank; 1 = its mirror, the USB bank):

1. conjugate for bank 1, mix the 1470 Hz band centre to zero, 129-tap low-pass, 880 Hz
   wide;
2. every symbol (160 samples), at **all 160 offsets**, the sums over a sliding 32-sample
   window of `y[n]*conj(y[n+128])` for each arm and each arm pair, and of the energy,
   into exponentially weighted folded profiles: one at 0.5 s for detection, one at the
   operator's averaging time for the weight.

### 4.1 Detection statistic ("dpk")

On the two arms' power-summed profile `A(g)`, energy `B(g)`:

1. `rho0 = 2|A|/B`; the bump position `g0` is the peak of `rho0` smoothed with a triangle
   of half-width 32 (the shape the prefix makes).
2. The tone pedestal is the complex mean of `A` over the offsets more than 48 from `g0`.
   `rho = 2|A - pedestal|/B`.
3. `dpk = rho[g] - mean(rho off the bump)` at the matched peak `g`.

It is an absolute contrast, not a z-score. A z-score was tried (peak over the spread of
the offsets away from the bump) and was worse: a strong tone, or a strongly modulated
carrier, shrinks the spread and inflates it.

On the 14-capture panel (SSB, STANAG data, wideband, V1, CW retuned into the band; about
1960 one-second windows): non-V2 median 0.09, 5% above 0.23. A decodable V2 signal reads
0.5 to 0.6 (T-008).

### 4.2 Lock and hold

A single window is not a lock. A lock needs `V2_LOCK_K` of the last `V2_LOCK_N`
one-second samples above `V2_T_LOCK`, with the bump position within `V2_LOCK_TOL` samples.
The tolerance is wide because on multipath the bump wanders by 10 to 20 samples.

Once locked there is **no timeout** (same decision as V1): the weight is held, however
long the prefix is gone. New weights are issued only while the prefix measures present
(`dpk > V2_T_HOLD`, 0.20).

### 4.3 The weight

At the bump, with the pedestal taken out of each of the four sums (arm 0, arm 1, and the
two cross terms):

- `theta = arg(ry0)` is the carrier offset's phase over the lag, from arm 0's own
  correlation (channel-independent);
- `X = x10*e^(-j theta) + conj(x01*e^(+j theta))` is twice `h1*conj(h0)*S*W`; `r = X /
  (2|ry0|)` is `h1/h0`;
- each arm's noise is its power less its prefix-correlated part: `N_k = b_k/(2W) -
  |ry_k|/W`, floored at 2% of the arm's power;
- **issued weight: the unit direction `conj(r)/|r|`** (decided 2026-10-22), ready to apply as
  `z0 + w*z1`: arm 1 added in phase with arm 0 at equal gain. The phase is what the objective
  (maximise the RADE SNR, by putting the phase where the interference is not) rests on. The
  engine's slew smooths it; the module does not (V2-F4: the decoder follows a ramp, not
  steps; see section 6);
- **published for the log, not used:** the maximal-ratio weight `conj(r) N0/N1` (gain capped
  at 4), each arm's prefix SNR, each arm's band power, and the share of power that is a tone.
  Unit weights cost something when one arm is mostly noise; the log says how often and how
  much, so the noise-weighted weight can be a second experiment on evidence.

`arm_db`, `arm_cos`, `arm_sin` are published as for V1 (`|r|^2 N0/N1` in dB, and the unit
direction `conj(r)/|r|`). The gain ratio `|r|` is taken from the arms' own prefix
correlations (`sqrt(|ry1|/|ry0|)`), not from the cross term: averaged over a window in which
the inter-arm phase moves, `|X|` shrinks, and an `arm_db` read from it disagreed with the two
arm SNRs by 2.8 dB on T-008 until this was changed.

This is the "blind scalar weight with a per-arm CP noise term" the combining notes
specified (their section 8), put in a form that runs live. It is not the per-carrier
version.

## 5. Evidence

### 5.1 Detection against everything but V2 (Python reference, 14 captures)

| Capture type | Result |
|---|---|
| V2, T-008 (7.166 MHz, USB) | dpk median 0.61, locked 92% of windows (first rule: 3 of 4, tol 25) |
| V1 (LSB 7.177, USB 5.3685), read at the V2 lag | noise level (0.09 median) |
| SSB (three captures), STANAG (two), wideband (two) | noise level |
| CW retuned into the band (two) | needed the pedestal subtraction; without it 5 to 20% of single windows false-alarmed |

Weak spots seen: one wide USB capture (`20260902-154822`) with a strong in-band carrier
raised single windows to ~0.3 (the persistence rule stops these); a strong in-band tone
dilutes `rho`, so detection would be lost rather than falsely made. **Detection with a tone
injected into a V2 signal was not measured.**

### 5.2 Inter-arm phase with a common-mode interferer (T-008 `184835`, bank 1)

Interferer present on both arms with the same waveform and a deliberately different
inter-arm phase (110 degrees), level relative to signal plus noise. Error against the
clean CP estimate of the same windows (the clean estimate is itself a CP estimate, not
on-air truth), median / 90th percentile, degrees:

| Interferer | -3 dB | +3 dB | +10 dB | Lag-0 cross-spectrum (all levels) |
|---|---|---|---|---|
| noise | 12 / 35 | 24 / 49 | 62 / 124 | 95 / 153 at -3 dB, 122 / 160 at +3 dB, 128 / 162 at +10 dB |
| tone | 5 / 27 | 9 / 35 | 19 / 96 | same, ~95 to 128 |
| 62.5 Hz comb | 13 / 51 | 30 / 84 | 69 / 157 | same |
| impulses | 9 / 24 | 28 / 69 | 55 / 132 | same |

Reading: the lag-0 estimate goes to the interferer's phase at every level, as expected
(it measures the strongest source). The CP-gated estimate holds to about +3 dB and fails
at +10 dB for noise-like interferers; for a tone it is better because the pedestal is
exactly removable. The comb on the modem's own grid is the worst case. **The injection is
synthetic, fully coherent between the arms, on one capture.** Real EMC is not that
tidy; this shows the mechanism, it does not size the effect on air.

### 5.3 The C port against the Python reference (one capture)

`py/test_cp_replay.py`: T-008 locks at 5 s on bank 1; the SSB and V1 captures never lock;
once locked, the weight's phase agrees with the reference's inter-arm phase to a median of
7 degrees over 35 seconds. The carrier-offset readout is within +/-2 Hz of the carrier
grid on T-008. (The readout was off by 30 Hz at first, because the 1470 Hz mix is not on the
62.5 Hz grid; fixed.)

### 5.4 Against rade_c's own V2 receiver, all 171 captures

The reference for "this capture holds V2" is `score_radev2` (rade_c's receiver): sync 20%
or more and frame-sync 0.6 or more, on either arm. 24 captures meet it. The threshold
is mine; the receiver is rade_c's. Many of the 24 are consecutive captures of the same
band session (the 1.984 MHz series of 11 September, 160 m), so there are far fewer
independent stations than captures.

Lock parameters were swept (`py/cp_sweep.py`, `cp_replay` built with `-D...`):

| T_LOCK | K of N | tol | found | time to lock | locked on non-V2 |
|---|---|---|---|---|---|
| 0.25 | 3 of 4 | 25 | 24/24 | 4 to 5 s; weak 12 to 31 s | 13 |
| 0.30 | 3 of 4 | 20 | 22/24 | | 4 |
| **0.30** | **4 of 5** | **20** | **21/24** | **5 to 7 s; weak 17 to 23 s** | **2** |
| 0.35 | 4 of 5 | 15 | 20/24 | | 2 |
| 0.30 | 5 of 6 | 15 | 20/24 | 6 to 8 s; weak 34 to 42 s | 0 |

Shipped: the bold row. The three it misses are the weak ones from 5 October
(`20261005-162647`, `-162757`, `-163038`). The two false locks are recordings of an AM
broadcast on 698.6 kHz (`20261008-142805`, `-142917`), which rade_c's own prefix sync also
accepts (95% sync; its frame-sync net sits at 0.49 and the aux bit at 0.47, so nothing
decodes). The strictest row clears them and loses the weakest real signals, which are the
ones diversity is for. On those two recordings a false lock does issue weights, since the
prefix measures present.

One note on the first run of this comparison: `score_radev2` picks the sideband sense from
the recorded mode, so a signal recorded in the wrong mode scores 0%. Of the 13 non-V2 locks
of the loosest row, 11 were re-scored with `--flip` (not the two AM recordings, nor
`20260902-154822`); all stayed at 0% sync. Three of the 13 (`20260830-110923`, `-111051`,
`-111734`) are not clear-cut in the default sense: rade_c syncs 6 to 13% with frame-sync
0.57 to 0.82, so they may be weak V2 rather than false locks. They are counted as non-V2
here because they fall under the threshold, and they are the captures the 3-of-4 row
finds and the shipped row does not.

## 6. What is not known

- **Whether the weight helps on air.** It has not been decode-scored and not been on air.
  Section 9 says why it goes straight to the operator.
- **The per-arm noise term.** `N_k` is power less prefix-correlated power. It is the term
  the combining notes found the weight needs and it is untested against the Window
  reference on air. On multipath the prefix-correlated part is biased (V2-F2).
- **Detection with a strong tone in band** (section 5.1), and with real EMC (5.2).
- **CPU on the Pi.** Counted, not measured: about 2 x 129 real-tap multiply-adds per
  8 kHz sample per arm per bank for the band filter (some 4M multiply-adds per second per
  bank for both arms), plus 160 offsets x 6 sums per symbol; searching both banks doubles it.
- **The weak-signal end.** Time to lock is 17 to 23 s on the weak captures, and three weak
  ones are missed. The rule was chosen by the sweep above on a small number of independent
  stations.
- **Phase steps.** The module no longer limits its phase; the engine's slew does
  (`div_apply_weight()` moves the weight 15% of the way to the target per block). Worked out,
  not measured: a 90 degree change starts at about 10 degrees in the first block, i.e. 0.33
  Hz of ramp at 85 ms blocks (16384 samples at 192 kHz) and less at 171 ms. A 180 degree
  step (Invert, Null against Sum) goes straight through zero gain and flips there: not a ramp.
  V2-F4's tolerance was found on synthetic channels. Invert and Null are allowed, with the
  cost noted in the menu text; their value on V2 is uncertain.
- **Sidetone.** The replay tool takes the recorded `vfo.offset` only; CW sidetone folding as
  in `div_frame_off()` is the engine's and is irrelevant to the digital modes.

## 7. Files

| File | Purpose |
|---|---|
| `src/rade_v2_correlator.[ch]` | the tracker. No GLib, `t_print` or `radio.h`; diagnostics are the extern variables in the header |
| `test/diversity/devtools/py/cp_probe.py` | the reference (`scan`: detection and lock; `cross`: the interferer-injection test) |
| `test/diversity/devtools/cp_replay.c` | replays a `.divc` capture through the C module with what the engine was handed (bank, tau, frame offset, resets), one line a second, a per-block CSV, and a self-check against the engine's recorded state; `make cp_replay` |
| `test/diversity/devtools/py/test_cp_replay.py` | holds the port to the reference (lock, rejections, phase agreement), the engine path through `run_ref`, and the self-check path |
| `test/diversity/devtools/run_ref.c`, `py/divc.py` | `--ref rade2` drives the whole engine on a recording; `divc.py` names reference 5 |
| `test/diversity/devtools/py/cp_sweep.py` | the section 5.4 comparison against rade_c; `--cp-replay` takes a build with other `-D` lock parameters |

Public interface (`rade_v2_correlator.h`): `rade_v2c_start(ddc_rate)`, `_stop`, `_reset`,
`rade_v2c_process(arm0, arm1, n, expect_bank, frame_off, tau, &wr, &wi)`, and
`rade_v2c_locked`, `_present`, `_quality`, `_snr`, `_mirrored`, `_gamma`, `_freq_off`,
`_arm_db`, `_arm_valid`, `_arm_cos`, `_arm_sin`; for the log `_mrc_re/_im`, `_snr0/_snr1`,
`_pedestal`, `_p0/_p1`. The arguments mean what they mean for `rade_corr_process()`.

## 8. The engine hook

Built on 2026-10-22 as four changes and tooling, in the order they apply (IDs from the
register, [changes.md](changes.md); write-ups in [changes/lc-rade.md](changes/lc-rade.md#rade-v2)):

| ID | Commit subject (branch hashes will change at the cut) | Applies alone? |
|---|---|---|
| LC-064 | `Diversity: DIV_REF_IS_RADE() and one set of RADE correlator calls, no behaviour change` | yes |
| LC-065 | the module: `Diversity: RADE V2 cyclic-prefix tracker ...` and the two commits that refine it | yes; nothing calls it |
| LC-066 | `Diversity: DIV_REF_RADE_V2, the RADE V2 cyclic-prefix reference in the engine` and `... says what it is doing in the log` | needs LC-064, LC-065 |
| LC-067 | `Diversity menu and overlay: RADE V2 prefix (blind)` | needs LC-066; dl1ycf's file |
| LT-037 | the devtools files | never upstream |

None of them carries a `Local-Change:` trailer yet; they are added when the series is cut for
`TEST`, since the IDs are permanent and the hashes are not. LC-065 is several commits that
become one at the cut (rule 9 of [changes/rules.md](changes/rules.md)).

**Decided 2026-10-22, with the owner:**

- V1 and V2 are **separate references side by side**; both waveforms are in use. Auto-detection
  is deferred until the V2 tracker has been evaluated on air. The lags separate the waveforms
  cleanly (read at the V2 lag, V1 is at noise: dpk 0.09 median; at the V1 lag, V2 is at noise:
  0.04 to 0.08, V1 reads 0.6 to 0.7), but its cost against the V1 pilot search is not measured,
  and the engine would be choosing a waveform the operator did not pick.
- A helper (`DIV_REF_IS_RADE()`) rather than a second `== DIV_REF_RADE_V2` at each place; its
  own change, no behaviour change.
- The engine's slew does the smoothing; the module issues the unit co-phase weight, and the
  noise-weighted weight is logged and compared offline (section 9).
- Invert and Null are allowed on V2; the value of a null is uncertain.
- Decode scoring waits; the operator's on-air evaluation comes first.

**How it is wired.** `DIV_REFS` is 8 and V1 has no slot, so the settings command does not change
and V2 needs none; an older build that meets reference 5 resets it to Window in
`div_settings_validate()`. The RADE block in `div_process_block()` serves both correlators. V2
copies its status into the `rade_corr_*` variables that the menu, the capture record and the
client's copy already read (`div_rade_v2_publish()`), so none of them changes. Start follows the
reference; stop and reset do both (the one that is not running costs nothing). The objectives are
V1's: Best uses the co-phase direction and `arm_db` picks the arm, Sum the weight, Null its
negative. The panadapter overlay draws 1031 to 1906 Hz (`RADE_V2C_OCC_*`).

**Smaller points.**

- The client/server state is unchanged and stays deferred.
- `div_rade_side` (the overlay's side) follows the passband as for V1.
- The devtools `tunable.manifest` generator is for `rade_correlator.c` only; the V2 module is
  replayed by `cp_replay`.
- The log lines (lock, loss, return, a tracking line every 5 s) compile and run, but the test
  harness stubs `t_print`; their text has not been seen.

## 9. Logging and on-air evaluation

The next step is the operator running it, and decode scoring is deferred on purpose. The numbers
a V2 score leans on (CP SNR, frame-sync confidence, `|aux|`) are themselves estimates: coarse (0.01
of `|aux|` is about 1 dB near 0.95 and stops resolving above about +8 dB), measured on few captures
and synthetic channels, and CP SNR is known to prefer combinations the decoder does worse on
(V2-F2). Tuning to them would fit the tool to the tool, and the RADE engine's own reported SNR is
no better a judge. The decision that matters is the operator's, listening to a real station.

What follows is what to record so that a run can be judged later without a number standing in for
the listening.

### 9.1 What is recorded already

- **The capture** (`make DIVCAP=1`, `.divc`). Both arms ahead of any combining, at the DDC rate,
  one record per analysis block (85 or 171 ms), 184 MB a minute at 192 kHz. Because the tracker is
  deterministic, this is the primary log: any number below can be regenerated from it, with any
  later version of the algorithm. Each block carries the reference (5 for V2), the mode, filter,
  bank, tau and frame offset the engine was handed, the attenuators, the arm swap, flags for a
  context reset and for dropped blocks, and the engine's state entering the block.
- **The engine's state for a V2 run**, in the block: locked, mirrored, quality (the prefix
  contrast), frequency offset, snr (the prefix SNR in dB), holding, the loop's answer before
  slewing, and the weight actually applied to the samples (`live_cos`, `live_sin`).
- **A switch of reference mid-capture is recorded** (the `ref` field changes block by block), so
  one capture can hold an A/B on the same station.
- **The terminal log**, in any build: `RADE V2 prefix LOCK`, `lock cleared`, `prefix lost, holding
  last weight`, `prefix back`, and every 5 s while tracking the contrast, bump position, frequency,
  each arm's prefix SNR, the tone share and the applied weight.
- **The RADE engine's own log** (`docs/rade/logging.md`): sync, overs, the callsign when it
  decodes. Those are the receiver's own verdicts. Align it with the capture by wall time
  (`t_start` in the capture header is unix seconds).

### 9.2 What is regenerated from the capture

`cp_replay FILE [bank [tau [CSV]]]` runs the module over the capture with what the engine was
handed (bank, tau, frame offset), resets where the engine did and after a dropped block, and
writes a row per block:

| Column | Meaning |
|---|---|
| `t`, `locked`, `present`, `quality`, `snr_db` | lock state, whether the prefix is measurable now, contrast, the CP SNR estimate |
| `gamma`, `bank`, `freq_off_hz` | where the prefix sits in the 160-sample period, which sideband, carrier offset from the grid |
| `arm_db` | arm 1's advantage in SNR (`|r|^2 N0/N1`) |
| `snr0_db`, `snr1_db`, `p0`, `p1` | each arm's prefix SNR and band power |
| `pedestal` | share of the power that is coherent at lag 128 away from the prefix: a tone or comb |
| `w_re`, `w_im`, `mrc_re`, `mrc_im`, `updated` | the weight issued (unit), the maximal-ratio weight not used, whether a new one was made this block |
| `snr_arm0_db`, `snr_arm1_db`, `snr_egc_db`, `snr_mrc_db` | the prefix SNR each way of combining would have given: arm 0, arm 1, equal gain with the issued phase (what ran), maximal ratio |

On a capture taken on the V2 reference, `cp_replay` also compares its state entering each block
with the engine's and says how many differ. That check passes on a copy of T-008 patched to look
like such a capture (0 of 351 differ); it has not been run on a real one, because none exists yet.
The first V2 capture is also the first real test of it.

### 9.3 The objective, and how the log speaks to it

The objective is to maximise the RADE SNR, and by doing so to put the phase where the
interference is not. Three things in the CSV bear on that:

1. **The prediction.** `snr_egc_db` against the better of `snr_arm0_db` and `snr_arm1_db` is what
   the issued weight should gain over a single arm; `snr_mrc_db` against `snr_egc_db` is what the
   noise-weighted weight would add. If the second is rarely more than a fraction of a dB, unit
   weights lose nothing; if arm 1 is often mostly noise, it says so. These are the tracker's own
   estimates in about 880 Hz of band, not decode results, and coarse; they explain a
   difference, they do not score one.
2. **The interference phase.** The CP-gated phase and the lag-0 cross-spectrum phase disagree when
   a common-mode interferer dominates (section 5.2). `cp_probe.py cross` computes both from a
   capture. Nothing logs the lag-0 phase live; it is a replay-time quantity.
3. **The pedestal.** A tone or comb in the band shows up in `pedestal` and costs detection (it
   dilutes `rho`); a run where it is high and the lock fails is the case section 5.1 says was not
   measured.

### 9.4 A run

1. Pick the RADE V2 reference, Sum, the default Averaging; capture 60 s, or longer if the path is
   slow. Note the station, band, sideband and what was heard, for `docs/test-findings.md`.
2. Without moving anything else, switch to Window (and FSK/Digital) on the same station; capture
   again, or switch inside one capture.
3. Keep the RADE engine's log from the same period.
4. Afterwards: `cp_replay` the capture with the CSV, and read the log lines.

Watch for: time to lock (5 to 7 s on strong, 17 to 23 s on weak, was measured on captures); a lock
on something that is not V2; what the weight does through a fade (it holds, by design); any audible
artefact at Invert, at Null or at Sum; and whether the lock rule's misses (three weak captures)
matter on a station the operator wants.

### 9.5 What the logging does not cover

- The capture is a development build (`DIVCAP=1`) and is deleted before the work goes upstream; a
  release build has the terminal log only.
- The weight is recorded once per block; inside a block the module makes one every 0.1 s.
- A capture that starts after the tracker locked replays from cold, so its first seconds differ
  from the live run.
- The lag-0 inter-arm phase is not logged live (above).
- Nothing here measures what a listener hears. That is the operator's.

## 10. Reproducing

```
make -C test/diversity/devtools cp_replay
python3 -I test/diversity/devtools/py/test_cp_replay.py        # needs captures/
python3 -I test/diversity/devtools/py/cp_sweep.py              # minutes the first time (score_radev2), then cached
python3 -I test/diversity/devtools/py/cp_probe.py scan captures/divcap-20260930-184835.divc
python3 -I test/diversity/devtools/py/cp_probe.py cross captures/divcap-20260930-184835.divc
test/diversity/devtools/cp_replay captures/divcap-20260930-184835.divc 1 0.5
```

`score_radev2` needs librade built (`tools/rade/build_rade.sh`).

# RADE V2 diversity: a cyclic-prefix tracker

> **Branch `feature/rade-native`**, 2026-10-21. A V2 counterpart of the V1 pilot
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
- **Built:** `src/rade_v2_correlator.[ch]` (C, standalone, same shape as the V1 API),
  the Python reference it was ported from, a capture replay tool, and two checks.
- **Not done:** no engine hook, no decode scoring of the weight, nothing on air. The
  next step is the hook (section 8), then the operator's on-air evaluation (section 9).

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
- weight `w = conj(r) * N0/N1`: maximal-ratio with arm 0's phase, ready to apply as
  `z0 + w*z1`. Magnitude capped at 4; **phase slew limited to 1 Hz** (V2-F4: the decoder
  follows a ramp, not steps).

`arm_db`, `arm_cos`, `arm_sin` are published as for V1 (`|r|^2 N0/N1` in dB, and the unit
direction `conj(r)/|r|`).

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
- **Phase steps from the engine.** The module limits its own phase to a 1 Hz ramp. Invert
  and Null/Sum are 180-degree steps applied by the engine and are not covered
  (V2-F4, open item 1 of the combining notes).
- **Sidetone.** The replay tool takes the recorded `vfo.offset` only; CW sidetone folding as
  in `div_frame_off()` is the engine's and is irrelevant to the digital modes.

## 7. Files

| File | Purpose |
|---|---|
| `src/rade_v2_correlator.[ch]` | the tracker. No GLib, `t_print` or `radio.h`; diagnostics are the extern variables in the header |
| `test/diversity/devtools/py/cp_probe.py` | the reference (`scan`: detection and lock; `cross`: the interferer-injection test) |
| `test/diversity/devtools/cp_replay.c` | replays a `.divc` capture through the C module, one line a second; `make cp_replay` |
| `test/diversity/devtools/py/test_cp_replay.py` | holds the port to the reference (lock, rejections, phase agreement) |
| `test/diversity/devtools/py/cp_sweep.py` | the section 5.4 comparison against rade_c; `--cp-replay` takes a build with other `-D` lock parameters |

Public interface (`rade_v2_correlator.h`): `rade_v2c_start(ddc_rate)`, `_stop`, `_reset`,
`rade_v2c_process(arm0, arm1, n, expect_bank, frame_off, tau, &wr, &wi)`, and
`rade_v2c_locked`, `_present`, `_quality`, `_snr`, `_mirrored`, `_gamma`, `_freq_off`,
`_arm_db`, `_arm_valid`, `_arm_cos`, `_arm_sin`. The arguments mean what they mean for
`rade_corr_process()`.

## 8. The engine hook, done so that it can become a TEST patch

`rade_correlator.c` is the V1 module and already lives in `TEST`. The new module sits beside
it and touches nothing existing, so the work divides into layers that apply on their own:

1. **Module and tools.** `src/rade_v2_correlator.[ch]`, the devtools files, the
   `.gitignore` and `Makefile` lines (devtools only). Applies to `TEST` with no engine
   change. The top-level `Makefile` needs the new object (and `cp_replay` builds without it).
2. **Engine.** The only file is `src/diversity_auto.[ch]`, which we own. The hook is a
   second branch beside `if (ctx.ref == DIV_REF_RADE_V1)` in the block loop, the same
   start/stop/reset points (`diversity_auto_restart()`, the settings-change path that
   builds or tears down the front end), and the same use of the result:
   `div_apply_best(rade_v2c_arm_cos, rade_v2c_arm_sin)` in Best, and
   `div_apply_weight(sign * wr, sign * wi)` in Sum and Null. Nothing else changes.
3. **Menu.** `diversity_menu.c` is dl1ycf's and the engine does not write what the menu
   can change. A reference label, the visibility cases that `DIV_REF_RADE_V1` has there
   (about eight places), and the settings slot go in a **note for dl1ycf**, not a commit.

**Open decision, first on the list:** how V2 is selected.

- *(a) A new reference, `DIV_REF_RADE_V2`, appended last.* Explicit, matches the rule of no
  silent substitution, and mirrors V1. Costs: it grows the settings command (one slot per
  reference, `div_settings_validate()`), the menu and the panadapter overlay
  (`rx_panadapter.c` case), so layers 2 and 3 widen.
- *(b) One "RADE" reference that runs the right correlator.* The lag separates V1 from V2,
  so the engine could run both searches until one locks (double the search cost) or be told
  by the RADE window which waveform is selected. No enum change, so a smaller patch, but the
  engine decides something the operator did not pick, and `rade_native` (not in `TEST`)
  would be the only thing that knows.

The recommendation is (a), because it keeps the patch layered and the behaviour visible.
The choice changes layers 2 and 3 only; layer 1 stands either way.

Smaller points for the hook:

- Status shared with the menu: `rade_v2c_locked`, `_quality` and `_mirrored` map onto what
  the V1 path publishes (`live_locked`, `live_quality`, `live_mirrored`), and `st->rade_*`
  in the client/server state follows the same pattern. Client/server stays deferred.
- `div_rade_side` (the overlay's side) follows the passband as for V1.
- The overlay draws `RADE_CORR_FLO/FHI/OCC_*` for V1's 750 to 2200 Hz; V2's band is 1062.5
  to 1875 Hz (about 1030 to 1910 Hz as filtered here), so it needs its own constants.
- The devtools `tunable.manifest` generator is for `rade_correlator.c` only; the V2 module is
  replayed by `cp_replay` and needs nothing from it.
- Capture format and `divcap_replay` need no change: the module takes the same two blocks.

## 9. On-air evaluation, and why not decode scoring first

The next step after the hook is the operator running it. Decode scoring was the earlier
route (`score_radev2`, `|aux|`), and it stays available, but it is deferred on purpose:

- the numbers a V2 score can lean on (CP SNR, frame-sync confidence, `|aux|`) are
  themselves estimates, coarse (0.01 of `|aux|` is about 1 dB near 0.95 and stops resolving
  above about +8 dB) and measured on a few captures and synthetic channels. Tuning to them
  would fit the tool to the tool;
- the decision that matters is the operator's, listening to a real station.

What to take from an evaluation run, so that it can be judged later without a number
standing in for the listening:

- **Captures** (`.divc`) of every run, as now, with the reference and settings recorded in
  the header block, so a run can be replayed through `cp_replay` and the other references.
- **The RADE engine's parameter log** (`docs/rade/logging.md`) from the same run: sync,
  overs, the callsign when it decodes. Sync and a decoded callsign are the receiver's own
  verdicts, not the tracker's.
- **A/B in one session:** the same station and band, V2 reference against Window and against
  FSK/Digital, switched by the operator, noting what was heard.
- **Things to watch:** time to lock; any lock on something that is not V2; what the weight
  does through a fade (it holds, by design); phase wander and any audible artefact at
  Invert or at Sum/Null; whether the 17 to 23 s on weak signals is tolerable.

Nothing in the tracker's outputs should be read as a score. `rade_v2c_snr` is rade_c's CP
estimate, with the bias on multipath that V2-F2 recorded.

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

# RADE native: how we use WDSP, checked against the WDSP Guide 2.1.1

2026-10-17, branch `feature/rade-native` after merging TEST (vendored WDSP 2.1.1). Reference: [../WDSP_Guide_Rev_2_1_1.md](../WDSP_Guide_Rev_2_1_1.md).
Method: every WDSP call the RADE path makes (`receiver.c`, `transmitter.c`, `rade_native.c`) read against the guide and the
vendored source, plus small offline experiments with the vendored `libwdsp.a` (tools in `test/rade_native/`, named below).
Each finding says whether it was **measured**, **read from code**, or **not measured**. No piHPSDR code was changed. Decisions (2026-10-17): `dsp_size` stays 2048; `signal-paths.md`, `filter-lab.md` and `findings.md` corrected for the single RX filter; an RX guard is not built now (section 3, future).

## Summary

The set-up matches the guide where the guide is firm (rates, ALC always on, CFIR on P2, `RXASetPassband` for the RX
passband, linear-phase filters, 2048 taps). Four things are not optimal or not as documented:

| # | Finding | Evidence | Worth |
|---|---|---|---|
| 1 | (kept as is) `dsp_size` is 2048, the guide says 64 to 1024. It is the "unidentified" 85 ms (RX) and 45 ms (TX) in `signal-paths.md` section 7 | measured | RX -43 ms at 1024, -53 ms at 512; TX -11 / -16 ms; CPU unchanged |
| 2 | In DIGU / DIGL the second RX filter `bp1` is **off**; the live RX has one filter (`nbp0`, 4-term), not two. `signal-paths.md` and `filter-lab.md` describe two | measured + read from code | live RX delay 106.6 ms, not 128 ms; docs wrong; lab conclusions unaffected |
| 3 | The RX profile (EQ, NR, ANF, SNB) is not held off during RADE receive, as the TX speech stages are | read from code | risk of distorting the modem signal; not measured; **future possibility, not built** |
| 4 | AGC: medium AGC stays. Turning it off was not better in fading | measured (synthetic, lab) | no change recommended |

Also resolved: the "not explained" 1 dB in `signal-paths.md` section 7a (live TX peak -0.97 dBFS against -0.02 offline).

## 1. `dsp_size` 2048 costs latency for nothing

`receiver.c:769` and `transmitter.c:920` set `dsp_size = fft_size = 2048`. The guide (5.1, 6.1) gives 64 to 1024 and says the
size sets the latency (64 = 1.33 ms at 48 kHz). WDSP collects a whole `dsp_size` block before it processes one, and again on the
way out, so the buffering is `2 * dsp_size / rate`: RX `2 * 2048 / 48000` = 85.3 ms, TX `2 * 2048 / 96000` = 42.7 ms.
That is the 85 ms (RX) and 45 ms (TX) that `signal-paths.md` section 7 lists as "WDSP's other stages, not identified".

Measured with `wdsp_latency.c` (a 1500 Hz Hann burst through one channel opened as piHPSDR opens it, filters 2048 taps,
linear phase; peak of the envelope):

| `dsp_size` | RX delay ms | TX delay ms (P2: 96 k dsp, 192 k out, CFIR, ALC) |
|---|---|---|
| 2048 (now) | 106.6 | 72.2 |
| 1024 | 64.0 | 61.5 |
| 512 | 53.3 | 56.2 |
| 256 | 48.0 | 53.5 |

CPU for the channel (noise in, 51 s of signal): RX 0.1 to 0.2 % of a core at every size, TX about 3 % at every size.
The filters stay 2048 taps (the constraint found in `filter-lab.md`, taps >= `dsp_size`, is met), so the response is the
same; overlap-save does not depend on the block size. **That equivalence was not tested on the decode**, and the guide
says `SetDSPBuffsize` can be called while data flows but piHPSDR does not do this. The saving at 512 is about 70 ms of a
1.9 s clip-to-speech time, and `TX_TAIL` (107 ms) could follow the 16 ms the TX delay drops. Modest. A RADE-only value
would leave the other modes as they are.

## 2. The live RX has one filter, not two

`RXAbp1Set()` (`wdsp/RXA.c:988`) runs `bp1` only when AM/SAM, SNB, NR2, NR4, NNR, ANF or NR is on, and `SetRXAMode()` calls it on
every mode change. `receiver.c:959` does `SetRXABandpassRun(rx->id, 1)` at open, but the later change to DIGU (7) or DIGL (9)
turns it off again. `wdsp_bp1_state.c` runs piHPSDR's call order: `bp1.run` is 1 after open, **0 after `SetRXAMode(DIGU)`**.
`wdsp_probe` (and so `filter_lab.py`) forces `bp1` on, which is where the "two in series" came from. The probe's delay
(128.0 ms) is reproduced with `bp1` on in `wdsp_latency` and is 106.6 ms with it off.

Consequences: `signal-paths.md` (RX diagram, section 8 "nbp0 + bp1 in series", the 43 ms) and `filter-lab.md` sections 1, 3 and 4
(RX rows) are for a configuration the radio does not run unless NR or ANF is on. With NR / ANF on, `bp1` comes back (gain 2.0,
+21 ms). The lab's RX conclusions (no decode gain from length or window; minimum phase worse) should hold for one filter;
the live edge carriers are about the 4-term single-filter figure. Not re-run.

## 3. RX processing is not held off during RADE receive (future possibility, not built)

`transmitter.c` keeps the leveler, CFC, compressor, phase rotator and DEXP off while RADE transmit is on (`73c3925c`), after the
profile's CFC tilted the spectrum by 8 dB. Nothing equivalent exists on receive: `rx_set_equalizer()`, `rx_set_noise()` and the ANF / SNB
calls apply whatever the DIGU / DIGL profile holds, and the RADE window only prints `nr`, `anf`, `snb` in its log. EQ changes the
spectrum the modem sees; NR2, NNR, NR and the ANF are built for speech and tones and would act on an OFDM signal (the guide
describes NNR as trained on voice and CW). **Not measured.** Decision 2026-10-17: no guard for now; a possible later change is the same shape as the TX one (hold the RX EQ, NR, ANF and SNB off while a RADE waveform is on, and show it in the RADE window). Until then the operator should keep them off in the DIGU / DIGL profiles. DIG modes have no squelch
(`rx_set_squelch()`), so that one is already safe. The wideband blanker (`rx->nb`, before WDSP) is not WDSP's channel and was not looked at.

## 4. AGC

The receive path has the operator's AGC (medium, attack 2 ms, decay 250 ms, top 80 dB) ahead of the engine, which then levels to rms 0.9 with its
own 0.5 s EMA. Is the WDSP AGC helping or hurting? `agc_ab.py`: 40 s of V1 modem (real speech) through synthetic flat Rayleigh
fading (1 Hz and 5 Hz Doppler spread, same fades for every AGC setting) plus noise at 3, 6, 9 dB in 3 kHz, WDSP as piHPSDR runs it
(`bp1` off, edges 675..2275), the engine's front end (conjugate, 48-tap 6:1, EMA), `librade`. 10 fades per condition. Feature error against what was sent
(0 = perfect); it is bimodal, 0.03 to 0.07 in sync, 0.2 to 0.6 when a fade loses sync, so it is the number of lost fades that moves the mean.

| fading | SNR dB | off | long | slow | medium | fast |
|---|---|---|---|---|---|---|
| 1 Hz | 3 | 0.207 | 0.235 | 0.206 | 0.283 | 0.283 |
| 1 Hz | 6 | 0.144 | 0.224 | 0.152 | 0.160 | 0.160 |
| 1 Hz | 9 | 0.104 | 0.134 | 0.059 | 0.046 | 0.046 |
| 5 Hz | 3 | 0.115 | 0.160 | 0.140 | 0.139 | 0.139 |
| 5 Hz | 6 | 0.141 | 0.139 | 0.199 | 0.059 | 0.059 |
| 5 Hz | 9 | 0.134 | 0.130 | 0.159 | 0.056 | 0.056 |

Medium against off, paired over all 60 runs: -0.017 +- 0.018 (se); better in 16, worse in 6. Long +0.030, slow +0.012, fast -0.017
(all +- 0.02). So **no sign that switching the AGC off helps, a weak one that medium or fast helps at 6 dB and up, and none that it
matters at 3 dB**. The engine's slow EMA alone follows a fade worse than the AGC's 2 ms attack. Medium and fast gave identical numbers
in all 60 conditions although their decays differ (250 and 50 ms, hang 0 in both); I did not find out why. Limits: synthetic fading, V1 only, flat
fading (an earlier 4-fade run with a two-path profile showed no AGC effect at all), and a real receiver also has the wideband NB and AGC-hang settings the operator chose.
A decisive test needs the **pre-WDSP I/Q** of real overs; the captures hold only the tap after the AGC. Adding a pre-WDSP tap
(`rx->iq_input_buffer`) would let any WDSP setting be replayed through `wdsp_probe` and `librade`.

## 5. Transmit, against the guide

| Guide | Ours | Verdict |
|---|---|---|
| 6.1: `dsp_rate` 96 kHz for all TX | 96 kHz on P2; 48 kHz on P1 (`tx->dsp_rate`) | P1 deviates; 2048 taps at 48 kHz match P2's 4096 (`filter-lab.md` section 3). P2 is the radio used |
| 6.3.15: ALC must never be disabled | on, 1 / 10 ms, max gain 0 dB | correct; idle at our level |
| 6.3.21: CFIR for the openHPSDR new protocol | on for P2 | correct |
| 6.3.12 / 6.3.22: `SetTXABandpassFreqs`, `TXASetMP` | bp0 edges from `rade_native_wdsp_edges()`, linear phase | correct |
| 6.3.3: panel select 1 or 2 | 3 (I and Q) | works (`wdsp/patchpanel.c` takes 3; the guide lists 1 and 2 for TXA only) |
| 11.2.4: minimum phase for digital modes | linear | right for RADE: the guide's argument is that demodulators tolerate dispersion; a modem with a 4 ms cyclic prefix does not (`filter-lab.md`: +1 dB with minimum phase) |

**The 1 dB in `signal-paths.md` section 7a is explained.** `alc_p2_chain.py` runs the V1 modem at 0.357 through TXA with the ALC on:
48 kHz out, as `alc_sweep.py` does: peak -0.02 dBFS. 96 kHz out: -0.02. 96 to 192 kHz, no CFIR: -0.20. 96 to 192 kHz with CFIR (what P2 runs):
**-0.97 dBFS, rms 0.555** — the live capture's -0.97 and 0.55. The CFIR takes 0.77 dB, the resampler 0.18 dB. The ALC ceiling is at 1.0 where the
ALC is, the PA sees 0.895 at the 192 kHz output. The offline sweep over-reads the output peak by 1 dB for P2; its conclusion (above the ceiling
the ALC takes back every dB) is unchanged.

## 6. Not used, and why that is fine, or an option

- **Impulse cache** (guide 8.10, `save_impulse_cache`): not used anywhere in piHPSDR. A change of waveform or Invert re-designs the 2048-tap filters; not timed.
- **WDSP's mic chain** (EQ, leveler, CFC, noise gate, DEXP) is bypassed by design: the vocoder wants a steady natural level, which the engine's leveller gives. A mic EQ or gate before LPCNet would be a new feature, unmeasured.
- **WDSP's RX audio stages** (EQ, NNR, panel) never see the decoded speech: it enters after WDSP. An NNR pass on FARGAN output is possible in principle; not tried.
- **Engine FIRs** (6:1 and 1:6, 36 to 48 taps) are not a quality concern: `nbp0` / `bp0` already put 98 dB or more between the modem band and anything those FIRs would have to reject (reasoned from the lab tables, not separately measured).
- **2.1.1 itself**: the new NNR CW models, the `SetChannelState` change and the portability fixes do not touch this path. The guide's rule that sample flow must continue during a down-slew is met by the end-of-over hold.

## Order I would do things in

Done: `signal-paths.md`, `filter-lab.md`, `findings.md` corrected for the single RX filter. Decided: `dsp_size` stays 2048 (512 would save about 70 ms; revisit with a decode check and a `TX_TAIL` re-measure). Future: the RX guard (section 3). Open:

1. Add a pre-WDSP I/Q tap so AGC and filter choices can be graded on real overs, not synthetic fades.

## Tools added (all in `test/rade_native/`, offline)

| Tool | What |
|---|---|
| `wdsp_latency.c` | delay and CPU of one channel against `dsp_size`, `bp1` on / off |
| `wdsp_bp1_state.c` | whether `bp1` runs in DIGU, in piHPSDR's call order |
| `agc_ab.py` | AGC modes under synthetic fading, through WDSP, the engine front end and `librade` |
| `alc_p2_chain.py` | TX peak and rms through the P2 chain, stage by stage |

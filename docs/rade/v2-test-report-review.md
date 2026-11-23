# RADE V2 test report: what it means for piHPSDR

Written 2026-11-23 on `devtest/everything` (librade 23c3746). This reads David Rowe's
*FreeDV-060 RADE V2 Test Report* (21 Nov 2026, radae `2b0902e`, branch `dr-v2-report-phase-d`) against what we have
measured here. A Markdown copy of the report with its figures is kept outside the repo, at
`~/sdr/Rade2_Tests.md` (with `~/sdr/Rade2_Tests_figures/`). The PDF next to it is the authority.

It is an assessment, not a change. Nothing in piHPSDR was changed for it. The items in section 3 are proposals that need
the owner's decision first.

## 1. Conclusion

There is no sensitivity to recover. Our V2 receive and transmit chain performs as the reference does wherever the two
can be compared. `loss_check.sh`, re-run on 2026-11-22 with librade 23c3746, shows WDSP's TX and RX chains add no
measurable loss.

The report's one on-air fault was a false sync on a +44 Hz tuning error. librade 23c3746 fixes it (rade_c PR #31, the
wrong-k detector, `4d93b7e`), and that version is already in this branch.

Latency is where we fall short of what V2 is meant to offer. Smaller points are the transmit level, the PA, and a
formal verification.

## 2. Where we already match the report

| Report | Ours | Source | Verdict |
|---|---|---|---|
| V2 acquires at -5.44 dB SNR3k (AWGN, Table 5) | Lab chain with WDSP's TX and RX filters: sync above 90 % to -6.76 dB, feature loss < 0.05 to -5.0 dB. Air emulation: sync holds to about -1 dB band SNR (1000 Hz), which is about -5.8 dB SNR3k | [filter-lab.md](filter-lab.md), [air-run-2.md](air-run-2.md) | same |
| The integration should add no loss (the verification procedure) | `loss_check.sh` 2026-11-22: V2 feature error 0.0125 direct and 0.0125 through WDSP TXA+RXA, delta 0.0000 (V1 0.0001), PASS | `test/rade_native/loss_check.sh` | adds nothing |
| PAPR 3.48 dB (100 % CCDF, simulation, Table 4) | 3.4 dB crest factor at our output before the PA at the defined level; 3.6 dB at WDSP's TX output in `loss_check` | [findings.md](findings.md), [air-run-2.md](air-run-2.md) | same |
| Transmit filtering has little effect | No TX filter variant moves V2 sensitivity by more than 0.05 dB. Minimum-phase filters would cost about 1 dB; we use linear phase | [filter-lab.md](filter-lab.md) | already best |
| Acquisition 200-320 ms on a good channel (Table 5) | 240-292 ms on air; the GPS run shows sync 0.27 s after the RF onset | [air-run-3.md](air-run-3.md) | same |
| EOO detected with P = 0.40 at 2.3 dB SNR3k, AWGN (Table 9) | 6 of 6 at about +2.2 dB SNR3k, one clip only | [air-run-2.md](air-run-2.md) | at least as good; thin evidence |
| +44 Hz tuning error: false sync, garbled speech (Phase D issue 5) | librade 23c3746 rejects offsets beyond about +-31 Hz; our TUNE UP / TUNE DOWN indicator (`8ed758b3`, untested) shows them | rade_c `4d93b7e` | fixed in this build |
| RX input wanted at 0.707 RMS (range 0.1 < A < 10, section 2.7) | We level to 0.9 RMS, well inside librade's range. The report shows the level does not change loss | [signal-paths.md](signal-paths.md) | no change |
| SNR estimate: 1.5 dB RMS error, 2.8 dB worst, from -5 to +5 dB, worse in multipath (Figure 5) | Our V2 estimate tops out near 18 dB. Diversity and air comparisons already use band SNR and text BER instead | [findings.md](findings.md) | consistent; keep using band SNR |

The `loss_check.sh` metric is relative (direct against through WDSP). It is not the report's absolute `loss.py` figure;
see item 5 below.

## 3. What to revisit, most valuable first

### 3.1 Latency

V2's selling point is a 40 ms frame and a fast PTT turnaround. Phase D tested exactly that, and W7YC reported turnarounds
faster than V1. We add a lot on top of the 40 ms.

| Stage | Now | Source |
|---|---|---|
| TX: key-down to RF | 174 ms, of which: 80 ms two-frame prefill, 79 ms WDSP, 11 ms radio | [air-run-3.md](air-run-3.md) |
| TX: WDSP `dsp_size` 2048 buffering | part of the 79 ms (72 ms TXA measured offline) | [wdsp-review.md](wdsp-review.md) |
| RX: WDSP RXA | 106.6 ms, of which 85 ms is `dsp_size` 2048 buffering | [wdsp-review.md](wdsp-review.md) |
| RX: play-out cushion | 100 ms (`PREBUFFER_V2`) | [signal-paths.md](signal-paths.md) |
| RX: end to end, air to speaker | **not measured** | |

The options are:

| Option | Saves | Risk |
|---|---|---|
| `dsp_size` 2048 → 512 | RX 53 ms, TX 16 ms (measured offline; CPU unchanged) | Kept at 2048 on 2026-10-17 until a decode check and a `TX_TAIL` re-measure are done |
| V2 prefill 2 frames → 1 | 40 ms | The TX output could run dry; needs an underrun count on the Pi |
| Smaller V2 play-out cushion | up to about 60 ms | RX underruns / clicks; the report notes short buffers stress sound drivers |

Measure the RX side end to end first, then try the changes one at a time.

### 3.2 Transmit level

The report says to drive until the ALC just responds, because overdriving a little costs little. Its decoded loss was
unchanged up to 6 dB into the ALC. Our [air-run-2.md](air-run-2.md) found the same (best at +1.5 to +3 dB above the
defined level, V1 and V2). We never applied it.

The V2 defined level is 0.504 into WDSP (0 dB = the ALC just reached). Moving it to about 0.60 (+1.5 dB) gives up to
about 0.4 dB more average power at no measured cost. It is a one-line change, or the operator's drive setting.

### 3.3 The PA

All our PAPR figures stop at the DAC. The report measured two Icom radios (IC-7200, IC-7300) at the PA output:

- At low and medium power, gain expansion raised the PAPR by about 1 dB.
- Near full drive, PA compression cancelled it.
- The report lists SDR-based radios with other PA and ALC designs as further work.

Our radio is an SDR-based radio. A measurement at real power, with and without PureSignal, would be a contribution.
PureSignal could linearise the expansion, which an Icom cannot do.

PA heating also applies to us. At a PAPR of about 3 dB the RMS amplitude is about 0.7 of the peak, close to a class B
PA's worst-case dissipation. The report saw 178-190 W dissipated at 43-59 W average, against 134-141 W for a carrier.
Expect 45-55 % of peak as average power, and reduce power for long overs.

### 3.4 V2 through WDSP's AGC on fading channels

Our AGC A/B ([wdsp-review.md](wdsp-review.md)) was V1 only. V2 has no pilot symbols and leaves the channel to the
network, so it may react differently to an AGC acting within a fade. `agc_ab.py` can be re-run offline for V2 at
little cost. A tap before WDSP, as an alternative input, is still open.

### 3.5 Formal verification

Phase D only counts results from *verified* integrations, and no third-party application was verified (Thetis was used
but is not verified). The rade_c README's procedure needs radae's `loss.py` through our TX and RX path, with a loss
within +-10 % of 0.080. radae is not installed on this machine. Passing it would let piHPSDR be listed as verified, and
its on-air samples would then count.

### 3.6 Small items

- Log `rade_clock_offset_ppm()` (new in 23c3746) in the parameter log. It shows the other station's sample-clock error
  and costs nothing.
- Do not narrow the RX filter. 1030-1970 Hz gains 0.3 dB of sync in the lab ([filter-lab.md](filter-lab.md)), but it
  cuts the edge carriers of a signal that is legitimately up to 31 Hz off.
- Keep treating librade's V2 SNR as a rough indication (section 2 above).
- Nothing for us in the "underwater" artefact, EOO detection at low SNR, or acquisition time. These are librade or
  model work; we pick them up by updating librade.

## 4. Not covered

- The report's MPD/MPP channel results (Tables 2, 3 and 6) have no counterpart here; we have not run fading channels
  through WDSP for V2 (item 3.4 is the cheap part of that).
- Phase A to C listening tests and speech-quality judgements: nothing of ours to compare.

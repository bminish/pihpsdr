# RADE native: the WDSP TX and RX filters, measured offline

Question: is the V2 transmit filter as good as it can be, and how do the DSP options (filter size, window, linear
phase against minimum phase / low latency) change what the transmitted and received RADE signal sees? Answer, in
short: **the default V2 TX filter costs the edge carriers 1.7 dB but this has no measurable effect on the decode;
nothing about the TX filter's length, window or edges moves the V2 sensitivity by more than 0.05 dB; the only option
that matters is minimum phase, and it makes things worse (about 1 dB at the threshold) because its delay spread
across the band is longer than the cyclic prefix.** The real gains available are small and are listed in section 6.
Everything here is offline (WDSP through `wdsp_probe`, librade through `rade_rx_iq`), white noise only, one speech set,
at the P2 rates (see 1). **Nothing was changed in piHPSDR; no on-air A/B of the filters has been done.**

## 1. What piHPSDR does today (code, not measurement)

| | TX | RX |
|---|---|---|
| Filter that shapes the RADE band | `bp0` (TXA band-pass), edges from `rade_native_wdsp_edges()`: V2 1000..2000 Hz, V1 675..2275 Hz | **two in series**: `nbp0` (notch/band-pass, window 4-term, WDSP's default) and `bp1` (7-term), same edges |
| Length (taps) | `tx->fft_size` = 2048 (`TXASetNC`) at the **WDSP DSP rate: 96 kHz on P2**, 48 kHz on P1 | `rx->fft_size` = 2048 (`RXASetNC`), at 48 kHz |
| Window | 7-term Blackman-Harris, hard-coded (`SetTXABandpassWindow(.., 1)`, `transmitter.c`) | `nbp0` 4-term (default, never changed), `bp1` 7-term (`create_bandpass`) |
| Phase | linear, hard-coded (`TXASetMP(.., 0)`) | `rx->low_latency` = 0 (`RXASetMP`), a saved option |
| Also in the path | CFIR (P2 only, run when the output rate is 192 kHz), a 96 -> 192 kHz resampler | the AGC and others (not the filters) |

Constraints found: WDSP crashes (segfault in `fexchange0`) if the filter has fewer taps than the DSP buffer, 2048 in piHPSDR:
**512 and 1024 taps cannot be used** without changing `dsp_size`. 4096, 8192 and 16384 work (delay grows with them). The band-pass edges are the
**-6 dB points**; the carrier span of V2 is 1062.5..1937.5 Hz, so the edge carriers sit 62.5 Hz inside the cut-off.

## 2. The lab (how to test the filters without a radio)

`test/rade_native/filter_lab.py`, on `wdsp_probe` (new options `--nc`, `--mp`, `--win`, `--nbpwin`, `--nbp`; defaults are piHPSDR's) and
librade. A config is `nc:win:mp:low:high` (win 0 four-term / 1 seven-term, mp 0 linear / 1 minimum phase).

| Test | Method | What it measures |
|---|---|---|
| `response` | a comb of 681 tones (2.5 Hz apart, random phases) at WDSP's sign through one channel (TX with ALC off, neutral, P2 rates and CFIR; RX with fixed gain), read by a 2 s DFT after settling | gain against frequency to 0.1 dB (no estimation noise); the group delay (phase slope) across the carriers; the channel delay (cross-correlation with white noise) |
| `decode` | 40 s of real speech (four `rade_c` clips) through `rade_tx` (V2 with its own TX band-pass on, as the engine runs it) -> TX channel -> calibrated white noise -> RX channel -> `rade_rx_iq` | feature error against what was sent, and sync; the SNR (in 3 kHz, signal power while sending against noise power in 3 kHz) at which the feature error falls through 0.05 and at which sync rises through 90 %, interpolated between SNRs 0.5 dB apart, 4 noise seeds |

```
cd test/rade_native && make wdsp_probe rade_rx_iq rade_tx_iq
python3 filter_lab.py response --wave v2 --kinds tx --nc 2048,4096 --win 0,1 --mp 0,1
python3 filter_lab.py response --wave v2 --kinds rx --nc 2048,4096 --win 0,1 --mp 0,1 [--nbpwin 1]
python3 filter_lab.py response --wave v2 --kinds tx --txedges 960,2040
python3 filter_lab.py decode   --wave v2 --snrs=-9,-8.5,-8,-7.5,-7,-6.5,-6,-5.5,-5 --seeds 4 --seconds 40 \
     --configs "tx:2048:1:0:1000:2000,rx:2048:1:0:1000:2000;tx:4096:0:0:1000:2000,rx:2048:1:0:1000:2000"
```
Output is in `out/lab/`. The scatter of the decode thresholds, from repeated baselines and variants that should not matter, is about **+-0.05 dB**.

## 3. Response and delay (V2, P2 rates)

Gain in dB relative to the middle of the band at the frequencies given (Hz in the modem's frame; the carriers are at 1062.5 ... 1937.5); the two
extreme carriers are the 1062.5 and 1937.5 columns.

### TX (default edges 1000..2000)

| TX filter | delay ms | group-delay spread across the carriers ms | 912.5 | 962.5 | 1012.5 | 1062.5 | 1112.5 | 1937.5 | 1987.5 | 2037.5 | 2087.5 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| nc2048/4t/lin 1000..2000 | 66.2 | 0.0 | -27.7 | -12.6 | -4.5 | -1.0 | -0.1 | -1.0 | -4.5 | -12.6 | -27.6 |
| nc2048/4t/min 1000..2000 | 58.5 | 2.9 | -27.7 | -12.6 | -4.5 | -1.0 | -0.1 | -1.0 | -4.5 | -12.6 | -27.6 |
| nc2048/7t/lin 1000..2000 | 66.2 | 0.0 | -20.0 | -10.7 | -4.9 | -1.7 | -0.4 | -1.7 | -4.8 | -10.7 | -20.0 |
| nc2048/7t/min 1000..2000 | 59.4 | 2.6 | -20.0 | -10.7 | -4.9 | -1.7 | -0.4 | -1.7 | -4.9 | -10.7 | -20.0 |
| nc4096/4t/lin 1000..2000 | 87.5 | 0.0 | -96.7 | -23.1 | -3.3 | -0.0 | -0.0 | -0.0 | -3.3 | -23.1 | -96.7 |
| nc4096/4t/min 1000..2000 | 69.6 | 4.7 | -96.7 | -23.1 | -3.3 | -0.0 | -0.0 | -0.0 | -3.3 | -23.1 | -96.7 |
| nc4096/7t/lin 1000..2000 | 87.5 | 0.0 | -47.8 | -17.3 | -3.9 | -0.3 | -0.0 | -0.3 | -3.9 | -17.3 | -47.8 |
| nc4096/7t/min 1000..2000 | 70.7 | 5.1 | -47.8 | -17.3 | -3.9 | -0.3 | -0.0 | -0.3 | -3.9 | -17.3 | -47.8 |

### RX (default edges 1000..2000; `nbp0` window 0, `bp1` window as listed)

| RX filter | delay ms | group-delay spread across the carriers ms | 912.5 | 962.5 | 1012.5 | 1062.5 | 1112.5 | 1937.5 | 1987.5 | 2037.5 | 2087.5 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| nc2048/4t/lin 1000..2000 | 128.0 | 0.0 | -169.0 | -46.1 | -6.6 | -0.1 | 0.0 | -0.1 | -6.6 | -46.1 | -170.1 |
| nc2048/4t/min 1000..2000 | 91.8 | 9.4 | -171.8 | -46.1 | -6.6 | -0.1 | -0.0 | -0.1 | -6.6 | -46.1 | -169.6 |
| nc2048/7t/lin 1000..2000 | 128.0 | 0.0 | -144.6 | -40.4 | -7.2 | -0.3 | -0.0 | -0.3 | -7.2 | -40.4 | -143.7 |
| nc2048/7t/min 1000..2000 | 92.9 | 9.8 | -144.5 | -40.4 | -7.2 | -0.3 | -0.0 | -0.3 | -7.2 | -40.4 | -145.2 |
| nc4096/4t/lin 1000..2000 | 170.6 | 0.0 | -173.0 | -131.7 | -3.2 | 0.0 | 0.0 | 0.0 | -3.2 | -131.1 | -172.8 |
| nc4096/4t/min 1000..2000 | 92.4 | 12.6 | -169.3 | -131.7 | -3.3 | -0.0 | -0.0 | -0.0 | -3.3 | -131.7 | -171.2 |
| nc4096/7t/lin 1000..2000 | 170.6 | 0.0 | -170.3 | -103.9 | -4.0 | -0.0 | 0.0 | -0.0 | -4.0 | -103.9 | -171.7 |
| nc4096/7t/min 1000..2000 | 93.7 | 13.9 | -174.9 | -103.9 | -4.0 | -0.0 | -0.0 | -0.0 | -4.0 | -103.9 | -173.0 |

### TX with edges moved in 40 Hz each side (960..2040), for comparison

| TX filter | delay ms | group-delay spread across the carriers ms | 912.5 | 962.5 | 1012.5 | 1062.5 | 1112.5 | 1937.5 | 1987.5 | 2037.5 | 2087.5 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| nc2048/4t/lin 960..2040 | 66.2 | 0.0 | -15.0 | -5.7 | -1.5 | -0.2 | -0.0 | -0.2 | -1.5 | -5.7 | -14.9 |
| nc2048/7t/lin 960..2040 | 66.2 | 0.0 | -12.2 | -5.8 | -2.2 | -0.6 | -0.1 | -0.6 | -2.2 | -5.8 | -12.2 |
| nc4096/4t/lin 960..2040 | 87.5 | 0.0 | -30.8 | -5.4 | -0.2 | -0.0 | -0.0 | 0.0 | -0.1 | -5.4 | -30.8 |
| nc4096/7t/lin 960..2040 | 87.5 | 0.0 | -21.8 | -5.5 | -0.6 | -0.0 | -0.0 | -0.0 | -0.5 | -5.5 | -21.8 |

Reading: the **4-term window gives a narrower transition than the 7-term at the same length** (edge carriers -1.0 dB against -1.7; 27.7 dB down at 912 Hz against 20), at the same delay,
with far side-lobes (about -90 dB or better) that nothing in the chain cares about; the 7-term keeps decaying but is wider. **Doubling the length to 4096 halves the transition** (edge carriers 0.0 dB, 97 dB down at 912 Hz with the 4-term window) and adds 21 ms (two filters in the TX
path, each 4096/2/96000). Minimum phase takes 7 ms (2048) or 18 ms (4096) out of the TX and 36-80 ms out of the RX but leaves a group-delay spread across the carriers of 2.6-5 ms (TX) and **9.4-14 ms (RX)**, against a cyclic prefix of 4 ms.
V1 is the same in kind: its edge carriers (750 and 2200 Hz) lose 1.3 dB (7-term) or 0.6 dB (4-term) at 2048 taps (`out/lab/resp_v1.json`).

**On P1 (WDSP's TX at 48 kHz, no CFIR) the same 2048 taps give the P2 4096 result** (edge carriers -0.3 dB 7-term / 0.0 dB 4-term; 912 Hz down 47.8 / 96.6 dB), because a tap is twice as long in Hz; the P1 delay measured 106.6 ms. The tables are for P2, which is the radio used for every capture here.

Where the delay goes (2048 taps): the linear-phase filters are 21 ms in the TX (two of them at 96 kHz, 10.7 ms each) of a 66 ms channel delay, and 43 ms in the RX (two at 48 kHz, 21.3 ms each) of 128 ms; the rest (45 ms TX, 85 ms RX) is WDSP's other stages (what they are was not identified here), not touched by these options.

## 4. What the decode says (V2, AWGN, SNR in 3 kHz; "change" is against the first row, negative is better)

| TX filter | RX filter | feature loss < 0.05 at (dB) | change vs default | sync > 90 % at (dB) | change vs default |
|---|---|---|---|---|---|
| nc2048/7t/lin 1000..2000 | nc2048/7t/lin 1000..2000 | -5.00 | +0.00 | -6.76 | +0.00 |
| nc4096/4t/lin 1000..2000 | nc2048/7t/lin 1000..2000 | -4.98 | +0.03 | -6.79 | -0.03 |
| nc2048/4t/lin 1000..2000 | nc2048/7t/lin 1000..2000 | -5.00 | -0.00 | -6.80 | -0.03 |
| nc2048/7t/lin 950..2050 | nc2048/7t/lin 1000..2000 | -5.00 | -0.00 | -6.77 | -0.00 |
| nc2048/7t/lin 1000..2000 | nc2048/4t/lin 1000..2000 | -5.00 | +0.00 | -6.75 | +0.02 |
| nc2048/7t/lin 1000..2000 | nc2048/7t/lin 1030..1970 | -5.00 | -0.00 | -7.08 | -0.32 |
| nc2048/7t/lin 1000..2000 | nc2048/7t/lin 1060..1940 | -5.04 | -0.04 | -7.13 | -0.37 |
| nc2048/7t/lin 1000..2000 | nc4096/4t/lin 1000..2000 | -4.92 | +0.08 | -6.68 | +0.08 |
| nc4096/4t/lin 1000..2000 | nc2048/4t/lin 1000..2000 | -5.05 | -0.05 | -6.79 | -0.03 |
| nc2048/7t/lin 1000..2000 | nc2048/7t/lin 970..2030 | -5.00 | -0.00 | -6.71 | +0.06 |
| nc2048/4t/min 1000..2000 | nc2048/7t/lin 1000..2000 | -4.05 | +0.96 | -6.32 | +0.44 |
| nc4096/4t/min 1000..2000 | nc2048/7t/lin 1000..2000 | -5.00 | -0.00 | -6.61 | +0.15 |
| nc2048/7t/lin 1000..2000 | nc2048/7t/min 1000..2000 | -4.02 | +0.98 | -6.20 | +0.56 |
| nc2048/7t/lin 1000..2000 | nc2048/4t/min 1000..2000 | -4.08 | +0.92 | -6.31 | +0.46 |
| nc2048/4t/min 1000..2000 | nc2048/4t/min 1000..2000 | n/a | n/a | -5.80 | +0.96 |
| nc2048/7t/lin 1000..2000 | nc2048/7t/lin 1045..1955 | -5.06 | -0.06 | -7.07 | -0.30 |
| nc2048/7t/lin 1000..2000 | nc2048/4t/lin 1030..1970 | -5.01 | -0.01 | -6.97 | -0.20 |

Reading:
- **Nothing about the TX filter moves the sensitivity** within the +-0.05 dB scatter: not the length (2048 or 4096), the window, nor the edges moved out to 950..2050. The edge carriers are 11 and 20 dB below the middle in the modem's own spectrum (section 5), so their further 1.7 dB loss does not matter to the decode.
- **Minimum phase costs sensitivity: TX 2048 +0.96 dB (loss) and +0.44 dB (sync), RX 2048 about +1.0 dB and +0.5 dB, both together +1.0 dB at the sync threshold and the loss never reaching 0.05 in the range tried.** TX 4096/min phase costs +0.15 dB (sync) and nothing at the loss threshold, but saves only 18 ms against linear 4096 and is 3 ms slower than the default linear 2048. The cause is the group-delay spread (section 3) against the 4 ms cyclic prefix.
- **A narrower RX improves sync by about 0.3 dB** (1030..1970 gives -0.32 dB, 1045..1955 -0.30; the noise bandwidth falls 0.3 dB) and leaves the loss threshold unchanged, because the same filter takes 2.6 dB (1030..1970) or 5.7 dB (1045..1955) off the two edge carriers; the two effects cancel in the decode. A wider RX (970..2030) is 0.06 dB worse. RX 4096 taps is 0.08 dB worse, not better.
- V1 (coarse probe, two seeds): narrowing the RX from 675..2275 to 725..2225 changes nothing measurable (sync threshold -5.20 -> -5.14 dB); V1's carriers fill the band.

## 5. The recordings, and what the V2 TX spectrum actually is

- The V2 modem's own spectrum is bell-shaped: relative to the middle of the band, the carriers at 1062.5 and 1937.5 Hz are about **-11 dB and -18 dB** (librade output, with its own TX band-pass on or off: the same to 0.1 dB, so that band-pass is not the cause and does nothing visible). That is the "edge carriers -14 / -18.5 dB" that `tx_shape.py` has reported for every V2 capture; it is the modem, not the filter. (This also means the modem puts little power into the edge carriers; losing 1.7 dB of it costs the decode nothing, section 4.)
- Local TX captures (V2, no ALC, 192 kHz, `rade-captures/20261016-120252_*`) agree with the lab's TX filter response within about 1.5 dB at the sampled frequencies (962 Hz: -10.6 dB measured, -10.7 dB lab; 912 Hz: -17.6 against -20.0; 1012 Hz: -3.6 against -4.9; at the edge carrier 1062.5 Hz the capture reads -0.1 dB against the lab's -1.7 dB): the lab is the reliable figure (deterministic), the capture comparison is limited by the speech the spectrum is taken from.
- The Kiwi recordings of the same overs (on air, 8 runs averaged) show the received shape, which includes the radiated signal, the path and the Kiwi's own response: up to about 11 dB different from the modem's at some frequencies (a loop antenna and a receiver with its own passband), so **they cannot be used to measure the TX filter** and were not. They do show that the received signal is not lopsided: the upper and lower edge carriers are within 3 dB of what the modem and local TX capture predict.
- `rade_native.c` runs librade's V2 TX band-pass (on by default, `RADE_NO_TX_BPF` not set) and its V2 RX band-pass; the TX one has no visible effect on the spectrum (0.1 dB) or on the decode (feature loss 0.016 at 12 dB SNR with and without it, measured); **the RX one was not tested**. Each adds 6 ms of delay (101 taps at 8 kHz). Left alone.

## 6. Recommendations (evidence strength in brackets; none applied)

1. **Leave the TX filter as it is** for V2 unless the radiated bandwidth is the worry: no length, window or edge setting improves the decode (measured, +-0.05 dB) and the default is already linear phase, which is what the decoder wants.
2. **If a cleaner transmitted spectrum is wanted for free, change TX `bp0` to the 4-term window** (`SetTXABandpassWindow(tx->id, 0)` for RADE only): edge carriers -1.0 dB instead of -1.7, 27.7 dB down at 912 Hz instead of 20, **the same delay, the same decode** (measured). Optional; the benefit is out-of-band, not sensitivity.
3. **If maximum out-of-band rejection matters more than 21 ms: TX 4096 taps, 4-term**: edge carriers 0.0 dB, 97 dB down at 912 Hz, decode unchanged (measured); not needed for sensitivity.
4. **Do not use minimum phase (low latency) for RADE, TX or RX**: about 1 dB at the threshold in the lab (measured), and the group-delay spread across the carriers (9-14 ms RX, 3-5 ms TX) is longer than the 4 ms cyclic prefix. The saving it buys is 7 ms (TX) and 36 ms (RX); on the radio the real cost would be larger on a multipath path (not measured).
5. **Optional RX narrowing for V2 (1030..1970): +0.3 dB sync sensitivity, no change in the loss threshold, 2.6 dB off the edge carriers** (measured, white noise); worth an on-air A/B because the two effects cancel in the lab and may not on a real path. RX length: 2048 is as good as any (4096 is 0.08 dB worse and 43 ms slower).
6. **To save latency without a sensitivity penalty there is nothing in these options**: 2048 taps is the smallest WDSP will take with piHPSDR's `dsp_size`, and the two filters in each direction are the filter's own 21 ms (TX) and 43 ms (RX) of 66 and 128 ms. The remaining 45 ms (TX) and 85 ms (RX) are in WDSP's other stages (not identified here) and would have to be found there.

## 7. Limits

White Gaussian noise only; no fading, no multipath (where the delay spread of a filter matters more, and a sharper filter's ringing could too), no ALC or PA in the loop, one 40 s speech set, P2 rates for the response tests (P1 differs, section 3). The decode thresholds are for an offline decoder
fed the filtered signal at its own 8 kHz rate; the AGC is fixed-gain here (the real RX has the medium AGC). V1's decode was probed coarsely only. The group-delay spread of the min-phase filters is from the phase slope of the comb, to about +-2 ms.

## 8. Next, if wanted

- Add `tx_win`, `tx_nc`, `tx_mp` and `rx_*` to the test harness (`set`) so the same runs can be repeated over the air with `air_test.py` and the Kiwi (a real-channel check of 2, 3 and 5).
- A fading channel in the lab (Watterson) for the minimum-phase and narrow-RX questions.

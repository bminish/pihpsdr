# RADE native: first automated over-the-air run, 28645 kHz, 2026-10-16

Session `rade-captures/air/20261016-115633_28645kHz` (git-ignored; 18 runs, `results.csv` / `results.json`), run with
`air_test.py --freq 28645 --wave v1,v2 --levels -30,-24,-18,-12,-6,-3,0,3,6`. ANT2, drive setting 1 %, Kiwi
kiwi.minish.org (LZ1AQ loop), one clip (`david_vk5dgr.wav`, 11.5 s), leveller off, trim 0, each run recorded
by the Kiwi (`iq`, `iq-fixed`, `usb`) and by piHPSDR (capture + parameter log). Levels are `drive_db`: dB around the
defined level (0 = V1 0.357, V2 0.504 into WDSP).

## Results

SNR is librade's, on the Kiwi `iq` recording (dB in 3 kHz); "EOO/call" is the end-of-over frame seen and, for V1, the
callsign decoded as sent. TX columns are from the WDSP TX output block (192 kHz, before the PA).

| Wave | level dB | TX peak dBFS | TX rms | TX crest dB | SNR mean | SNR min | SNR (fixed-gain) | feature loss | EOO/call | sync % |
|---|---|---|---|---|---|---|---|---|---|---|
| V1 | -30 | -30.56 | 0.0174 | 4.63 |  |  |  |  | no/n/a | 0.0 |
| V1 | -24 | -24.54 | 0.0347 | 4.66 | -5.8 | -9.9 | -5.5 | 0.0622 | no/no | 100.0 |
| V1 | -18 | -18.53 | 0.0691 | 4.68 | 0.0 | -7.0 | 0.0 | 0.0271 | no/no | 100.0 |
| V1 | -12 | -12.55 | 0.1379 | 4.65 | 7.2 | 3.9 | 7.4 | 0.0215 | yes/yes | 100.0 |
| V1 | -6 | -6.54 | 0.2752 | 4.67 | 14.0 | 8.2 | 14.3 | 0.02 | yes/yes | 100.0 |
| V1 | -3 | -3.52 | 0.3886 | 4.69 | 16.0 | 0.0 | 15.8 | 0.0209 | no/no | 100.0 |
| V1 | +0 | -0.97 | 0.5489 | 4.24 | 20.7 | 11.3 | 21.1 | 0.0186 | yes/yes | 100.0 |
| V1 | +3 | -0.97 | 0.6036 | 3.42 | 21.5 | 14.3 | 21.3 | 0.0181 | yes/yes | 100.0 |
| V1 | +6 | -0.96 | 0.6035 | 3.42 | 21.0 | 11.9 | 17.2 | 0.0189 | yes/yes | 100.0 |
| V2 | -30 | -30.73 | 0.0196 | 3.42 |  |  |  |  | no/n/a | 0.0 |
| V2 | -24 | -24.73 | 0.0391 | 3.43 | -3.0 | -14.7 | -2.9 | 0.0412 | no/n/a | 100.0 |
| V2 | -18 | -18.73 | 0.078 | 3.42 | 5.6 | 4.0 | 5.6 | 0.041 | yes/n/a | 100.0 |
| V2 | -12 | -12.73 | 0.1557 | 3.43 | 11.5 | 8.0 | 11.8 | 0.0166 | yes/n/a | 100.0 |
| V2 | -6 | -6.73 | 0.3107 | 3.42 | 15.7 | 11.4 | 15.9 | 0.0163 | yes/n/a | 100.0 |
| V2 | -3 | -3.73 | 0.4387 | 3.42 | 16.8 | 11.9 | 17.0 | 0.0301 | yes/n/a | 100.0 |
| V2 | +0 | -0.97 | 0.6194 | 3.19 | 17.5 | 12.1 | 17.6 | 0.0149 | yes/n/a | 100.0 |
| V2 | +3 | -0.97 | 0.6465 | 2.82 | 17.4 | 12.2 | 17.5 | 0.016 | yes/n/a | 100.0 |
| V2 | +6 | -0.97 | 0.6467 | 2.82 | 17.6 | 12.2 | 17.6 | 0.0168 | yes/n/a | 100.0 |

## What it shows

1. **The levels do what they should below the ALC.** TX peak and rms follow `drive_db` dB for dB from -30 to -3 (peak
   -30.6 ... -3.5 dBFS V1, -30.7 ... -3.7 V2). The energy centre and tilt pass `tx_shape.py` in every run
   (centre +1475 Hz V1 against +1457 modem; the speech-stage fix holds on 10 m).
2. **The ALC ceiling is -0.97 dBFS (0.894) at the P2 output, not 1.0**, and it is reached at the 0 dB level for both
   waveforms. At 0 dB the peak is on the ceiling and the power still follows (V1 rms 0.389 at -3 dB, 0.549 at 0 dB:
   +3.0 dB). Above it every dB is taken back: V1 rms 0.549 -> 0.604 (+0.8 dB) at +3 dB and 0.604 at +6 dB; V2
   0.619 -> 0.647 -> 0.647. So **+3 dB buys 0.8 dB (V1) / 0.3 dB (V2) of power, +6 dB buys nothing**, and the crest factor
   falls (V1 4.65 -> 4.24 -> 3.42 dB, V2 3.4 -> 3.2 -> 2.8 dB).
3. **V1 SNR on the Kiwi follows the power**, about 1 dB per dB from -24 dB (-5.8) to 0 dB (20.7), then +0.8 dB at +3 dB
   (21.5) and nothing at +6 dB (21.0), matching the power. Feature loss is flat from -12 dB up (0.0215 -> 0.0186 -> 0.0181
   -> 0.0189): **the ALC compression, up to +6 dB, did not measurably harm the decode.**
4. **V2 SNR flattens from about -6 dB** (15.7, 16.8, 17.5, 17.4, 17.6) although the TX power keeps rising to 0 dB. This is
   consistent with librade's V2 SNR estimate topping out near 18 dB (offline, a clean V2 channel reads 18.2 dB; V1 reads
   32 dB), not with the ALC. Feature loss is the better V2 figure: flat 0.015-0.017 from -12 dB (one outlier, -3 dB, 0.0301).
5. **Recovery at the noise floor.** Both waveforms hold sync down to -24 dB (V1 SNR -5.8, V2 -3.0) and lose it at -30 dB
   (nothing found, both). The end-of-over frame and V1 callsign are less tolerant: V1 EOO/call fails at -24 and -18 dB
   (SNR mean -5.8 and 0.0) and is decoded from -12 dB (SNR 7.2); V2's EOO is seen from -18 dB (5.6), not at -24 dB (-3.0).
   Feature loss rises from about 0.02 to 0.027 (V1 -18 dB) and 0.062 (V1 -24 dB); V2 0.041 at -24 and -18 dB.
6. **Far-out spectral regrowth from the ALC (before the PA, local capture):** beyond 1 kHz out the TX output is -127 dB
   (V1) at -12 dB, -101 dB at 0 dB, -80 dB at +3 and +6 dB; V2 -130, -106, -89 dB. Close skirts (100-500 Hz) do not move
   (-31 / -32 dB V1, -47 dB V2). The PA's own behaviour is not in these numbers.
7. **Timing (harness, same clock):** the first speech sample goes in within about 0.05 s of the key; a 11.47 s clip is
   keyed for about 12.1 s; the end-of-over hold after the last sample is **0.65 s V1, 0.43-0.47 s V2**; no transmit
   underruns in any run, the leveller stayed at 0 dB (off).
8. **The mic path was working as designed but the clip is hot at the peaks:** the engine measured -20.1 LUFS (the target)
   on every run, but the clip's peaks reach full scale, so the mic soft limiter had acted within the last second in
   47-48 % of the keyed 100 ms blocks. Not harmful to the decode here (loss unchanged) but it is part of what is being
   sent, and another clip, or one at lower peak level, would show whether it matters.

## Faults and open points

- **"capture_start: could not start capture" in the log, on every run: harness bug, now fixed.** `capture on` started
  the capture and then ticked the Capture button, whose handler started it a second time and printed that message. The
  captures themselves were made correctly (all 18, with `_tx.wav`, `_txmic.wav`, `_rx48.wav`, `_stats.csv`). Fixed in
  `rade_native_menu.c` (the harness now shows the button state without running its handler); it needs a rebuild and
  restart of piHPSDR to take effect.
- **The first results had no local-side columns:** the capture path came back without its directory, and `air_test.py`
  looked for it relative to its own working directory. Fixed (paths are resolved against the repo) and
  `air_test.py --reanalyze SESSION` recomputed this session; the table above includes them.
- **The over-the-air delay was not measured.** The Kiwi recordings carry no time stamps; the `delay_keyed_to_sync_s`
  column that was first written compared against the recorder's start time and read 0.1-0.2 s, which cannot be right
  (the engine prefill and WDSP alone are more). That column is removed. A real measure needs a time reference in the
  recording (a GPS-stamped Kiwi format, or a marker burst).
- **V1 -3 dB run: SNR mean 16.0 but the EOO and call missed** (SNR min 0.0, the sync ends at the end of the over with a dip).
  One run, neighbours at -6 and 0 dB are fine; it looks like a fade at the end of the over, not the level, but that is not
  shown. Repeat (`--repeat`) before reading anything into it.
- **The Kiwi's fixed-gain profile read 17.2 dB at V1 +6 dB** against 21.0 on the AGC profile (the two agree within 0.4 dB
  in every other run): unexplained, a single run.
- **Not covered:** one clip and one path/day (the band and the Kiwi's noise change); the effect of the leveller on
  air; the PA and its linearity; weak-signal behaviour between -24 and -30 dB in finer steps; a second callsign length
  for V2 (V2 has no callsign text yet).

## Suggested next runs

1. Rebuild and restart piHPSDR (capture message fix), then repeat the sweep with `--repeat 3` at -30, -27, -24, -21, -18, -15, -12
   for the noise-floor threshold, and 0, +1.5, +3 for where the ALC starts to cost.
2. A second clip with a lower crest factor, to see whether the mic limiter matters.
3. The same with `--agc` to see the leveller on air.

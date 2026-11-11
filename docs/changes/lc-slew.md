# Slew (LC-069)

Part of the local-change register: [../changes.md](../changes.md).

<a id="lc-069"></a>

## LC-069 — The wideband references slew with a time constant

**Problem.** The applied weight follows the loop's answer by a fixed fraction of the distance left per analysis block
(`DIV_SLEW_FRAC`, 0.15). A block lasts as long as the reciprocal of the bin width, so the lag moves with the bin width
(Finding 48: 0.26 s at 24 Hz bins, about 0.5 s at 12 Hz, 1 s at 6 Hz) and not with anything the operator set. At short
Averaging it is longer than the Averaging time, so the slider's low end cannot reach the output. This is the "time-based
slew (Finding 48)" item that was waiting to be ported from `feature/auto-diversity`; Finding 48 measured it there, on a different engine.

**Change.** In `div_apply_weight()` the Window, CW, FSK/Digital and Carrier references move the weight by
`1 - exp(-blocktime / DIV_SLEW_TAU)` of the distance left, `DIV_SLEW_TAU` being 0.15 s. The RADE references keep the fixed
0.15 per block, as does any call before `blocktime` has been set. The `div_jump` step on an objective or reference change is as it
was. **No new setting**: no menu row, no props key, no change to the settings command, the capture format or
`CLIENT_SERVER_VERSION`. Only `src/diversity_auto.c`.

**Why RADE is left alone.** The decoder dislikes a quick change of phase, and the RADE weight comes from a correlator with its
own averaging. V2's per-block slew was swept from 5 to 25 % on the 40 m and 60 m captures and is flat (LC-075). Nothing here
argues for changing it.

**Checks.** `run_ref --ref band --mode sum --weighting flat --cohmin 0.20` over seven captures (`235906`, `143952`, `144505`,
`152310`, `232845`, `211946`, `135316`; chosen to disagree, not a random draw), at Averaging 0.5 s and 2.0 s, scored with
`score_wideband.py` (guard-band SNR, dB), the shipped slew against time constants of 0.5, 0.15 and 0.05 s:

| capture | shipped | 0.5 s | 0.15 s | 0.05 s |
|---|---|---|---|---|
| 235906 | 3.31 / 3.20 | 4.22 / 4.10 | 4.69 / 4.64 | 4.76 / 4.70 |
| 143952 | 18.80 / 18.74 | 18.80 / 18.74 | 18.91 / 18.86 | 18.94 / 18.90 |
| 144505 | 19.19 / 19.16 | 19.19 / 19.16 | 19.18 / 19.20 | 19.18 / 19.21 |
| 152310 | -15.39 / -15.44 | -15.38 / -15.43 | -15.16 / -15.42 | -14.98 / -15.43 |
| 232845 | 14.35 / 14.37 | 14.35 / 14.37 | 14.36 / 14.38 | 14.36 / 14.38 |
| 211946 | 2.46 / 2.05 | 2.76 / 2.68 | 2.82 / 2.97 | 2.84 / 3.02 |
| 135316 | 17.70 / 17.56 | 17.45 / 17.34 | 17.79 / 17.63 | 17.84 / 17.67 |

(each cell: Averaging 0.5 s / 2.0 s.) At 0.15 s the SNR is at or above the shipped slew in all 14 cells; 0.05 s adds 0 to
0.2 dB more, which is not worth a faster step in the mix. The change as committed was run on `211946` and `235906` and
reproduces the 0.15 s columns exactly (2.82 and 4.69). `make -C test/diversity run` exits 0 with the same four FAIL lines and
the same KNOWN GAP lines as without it.

**Size, honestly.** On five of the seven captures the gain is 0 to 0.2 dB. `211946` gains 0.4 to 0.9 dB and `235906` 1.4 dB,
and `235906` opens in dead air, so part of that is the cold start of Finding 38; its from-the-first-action column still gains
about 2.7 dB. The gain is at short Averaging and on fading paths, and is small elsewhere. Seven captures of one operator.

**Not known.** Whether a 0.15 s step is audible: the original reason for slewing was that a fast change in the mix is heard as
a step, and SNR does not measure that. The change has not been listened to; `211946` is the capture to try first. At coarse
bins (6 Hz, blocks near 170 ms) the fraction per block rises to about 0.6, so the slew is nearly gone there, which is the
intent but a change of behaviour. CW and Carrier were not in the seven captures.

**Commit.** One commit, `src/diversity_auto.c` and these docs. Independent of LC-071 to LC-075, and applies to `TEST` alone
(the reference test uses `DIV_REF_IS_RADE()`, from LC-071; on `TEST` that reads `== DIV_REF_RADE_V1`).

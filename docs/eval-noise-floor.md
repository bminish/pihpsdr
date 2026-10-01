# Evaluating the noise-floor branch

The findings so far, and what a revisit needs, are in
`docs/test-noisefloor.md`.

Branch `test/noise-floor`, built from `TEST` at `0dfc870c`. Everything
here is for listening to and measuring, not yet for `TEST`. It must both
*measure right* on the captures and *sound right* on the air before any of
it goes back.

Build it as usual (`make DIVCAP=1` keeps the Capture button).

## What is on the branch

1. **An across-frequency noise floor** (`8a393217` from
   `feature/auto-diversity`). Each antenna's noise level is read every
   block from the bins outside the RX filter, instead of from the quietest
   moment in the recent past. It is accurate: within about 0.5 dB of the
   true ratio on every capture checked.
2. **Sum backs off the noisier antenna by the measured noise ratio**, on
   the Window, Carrier and CW references, from the floor outside the
   filter, no further than 20 kHz either side of the dial. Where too few
   bins are left outside the filter it falls back to what `TEST` does: the
   temporal minimum, or on CW the bins beside the tone. Until `6b2cdf2c`
   this was a menu choice ("Sum noise": Old or Ratio); the measurements
   below are from then. On CW it matters most with a narrow filter, a
   station with keyclicks, or a fading antenna (T-017, T-018 in
   `docs/test-findings.md`). Two attempts at cancelling noise both
   antennas share (a covariance solve, Gap covariance) measured as no
   better and were removed; see `docs/test-noisefloor.md`.
3. **Calmer Best.** It changes antenna only when the other one has been
   more than 2 dB better (was 1 dB) for a full second.
4. **Level output** (menu tick, on by default). The combined output is
   held at the level of one antenna, instead of getting louder by whatever
   the combining does. It is recalculated whenever the weight changes, so
   when Best hands over to the second antenna there is no jump. It is not
   applied in Null (making the output quieter is Null's job) or on RADE V1.

Level output works on the radio only; on a remote client it is greyed
out.

## What was measured (39 Window and Carrier captures)

Score against the better antenna alone (`score_wideband.py`):

| | Mean | Ahead of the better antenna |
|---|---|---|
| Sum, Old | +0.27 dB | 25 |
| Sum, Ratio | **+0.50 dB** | 29 |
| Best, `TEST` | −0.73 dB | 18 |
| Best, branch | −0.37 dB | 22 |

What the output level does (`score_level.py`), median over captures:

| | Louder than one antenna | Worst 10 % | Blocks jumping > 3 dB |
|---|---|---|---|
| Sum, `TEST` | +2.07 dB | +7.40 dB | 793 |
| Sum, branch | −0.08 dB | +0.03 dB | 784 |
| Best, `TEST` | +8.85 dB | +22.11 dB | 1175 |
| Best, branch | 0.00 dB | 0.00 dB | 774 |

Where it is known to be worse:

- **Sum, Ratio, where both antennas hear the same noise** (a local noise
  source): `154822` −4.45 dB, `122632` −2.84, `235906` −1.67 against Old.
  Old was not cancelling that noise on purpose; its wrong noise estimate
  happened to help.
- **Best**: `185337` −3.62 dB, `143952` −2.76, `000209` −2.34 against
  `TEST`.

Tried and dropped: a *covariance* solve, meant to cancel noise common to
both antennas, from the same outside-filter bins. It scored +0.32 dB
against Ratio's +0.50. Those bins show no correlation between the antennas
even where the band beside the passband is 0.64–0.99 correlated, so it
could not see the noise it was meant to cancel, and picking the quieter
bins skewed the noise ratio on a lopsided pair. Cancelling common noise
would need it measured beside the signal, not far from it.

## What to listen for, mode by mode

| Mode (reference) | Try | Listen for |
|---|---|---|
| SSB (Window) | Sum, against `TEST` | With one antenna noisier, it should sound quieter between words without losing the voice. With a local noise source both antennas hear (a switching supply, a neighbour's device), `TEST` may sound better: see "Where it is known to be worse". |
| AM / SAM (Carrier) | Sum, against `TEST` | As SSB. A fading broadcast is where `TEST`'s temporal minimum went most wrong in the feature branch's measurements. |
| CW | Sum, against `TEST` | Narrow filters (50 Hz and under), a station with keyclicks, one antenna fading. |
| SSB / AM | Best | Fewer changes of antenna; no loud jump when it changes. It will still sometimes pick the worse one. |
| Any Sum or Best | Level output off against on | Off, switching diversity on makes the band louder, and Best's changes are 20 dB jumps. On, the level stays put and only the noise should drop. How your AGC setting reacts matters here. |
| FSK/Digital, CW | Level output | Applies here too. |
| RADE V1, Null | — | Unchanged. |

A capture of anything that sounds wrong is the most useful thing to bring
back. Say which setting it was taken with; for an A/B, take one each way,
back to back.

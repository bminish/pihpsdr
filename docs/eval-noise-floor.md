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
2. **A choice of Sum noise model** for the Window, Carrier and CW references
   (menu, "Sum noise"):
   - *Old (time minimum)*: what `TEST` does.
   - *Ratio (outside filter)*, the default: the noisier antenna is backed
     off by the measured noise ratio.

   It applies to the CW reference too (since `a5145aa6`): with Ratio, CW's
   Sum weight takes the noise ratio from outside the filter; with Old it
   uses the bins beside the tone, as `TEST` does. That is the one to listen
   to with a narrow filter, a station with keyclicks, or a fading antenna
   (T-017, T-018 in `docs/test-findings.md`).

   A third option, *Gap covariance*, tried to cancel noise both antennas
   share. It measured as no better than Ratio and has been removed; see
   `docs/test-noisefloor.md`. The selector itself is an evaluation
   control and goes when this moves to `TEST`.
3. **Calmer Best.** It changes antenna only when the other one has been
   more than 2 dB better (was 1 dB) for a full second.
4. **Level output** (menu tick, on by default). The combined output is
   held at the level of one antenna, instead of getting louder by whatever
   the combining does. It is recalculated whenever the weight changes, so
   when Best hands over to the second antenna there is no jump. It is not
   applied in Null (making the output quieter is Null's job) or on RADE V1.

The two menu controls work on the radio only; on a remote client they are
greyed out.

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
| SSB (Window) | Sum: Old against Ratio | With one antenna noisier, Ratio should sound quieter between words without losing the voice. With a local noise source both antennas hear (a switching supply, a neighbour's device), Old may sound better. |
| AM / SAM (Carrier) | Sum: Old against Ratio | As SSB. A fading broadcast is where Old went most wrong in the feature branch's measurements. |
| SSB / AM | Best | Fewer changes of antenna; no loud jump when it changes. It will still sometimes pick the worse one. |
| Any Sum or Best | Level output off against on | Off, switching diversity on makes the band louder, and Best's changes are 20 dB jumps. On, the level stays put and only the noise should drop. How your AGC setting reacts matters here. |
| FSK/Digital, CW | Level output | The Sum noise choice does not apply (these measure their own noise); Level output does. |
| RADE V1, Null | — | Unchanged. |

A capture of anything that sounds wrong is the most useful thing to bring
back. Say which setting it was taken with; for an A/B, take one each way,
back to back.

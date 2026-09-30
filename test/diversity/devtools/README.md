# Diversity capture and replay — development tools

Instrumentation for tuning the RADE V1 pilot correlator against recorded
real-world signals.

**None of this is part of the diversity feature.** It exists to produce
measurements; the measurements go into `docs/diversity-rade.md` and into
the constants in `src/rade_correlator.c`, and then the instrument is
deleted. See "Removal" at the bottom — that procedure is the point, and
it was rehearsed before any of this was written.

## On our TEST branch

This harness was ported to `TEST` (bminish/pihpsdr) on 2026-09-30 from
`feature/diversity-binaural`, as the harness stood just before `8ea8c8f3`
(the per-subcarrier delay measurement, which reads correlator state
`TEST` does not have). The recorder and this README come from
`feature/auto-diversity`, which writes to `captures/`.

**It is local tooling and never goes to dl1ycf.** The LC-numbered
changes in `docs/changes.md` are what may go upstream. Nothing here is
part of them, and a PR branch built by cherry-picking LC commits onto
`upstream/TEST` carries none of it.

What differs from the feature branches:

- **The engine is `TEST`'s**, so every tool scores `TEST`'s behaviour,
  which is the point. Upstream moved the mode enum, `div_auto_mode` and
  the applied weight (`auto_div_cos/sin/gain/phase`) into `radio.h` /
  `radio.c`, so the tools include `radio.h` and define those themselves.
- **`test/diversity/known_gaps.h`** lists the features the tests were
  written for that `TEST` does not have. Their checks still run and print
  their figures, but a failure is reported as `KNOWN GAP` and not
  counted. Delete a gap's line when the feature is ported, and the check
  counts again. `test_cw` is not built at all: `TEST` has no CW reference.
- **`tunable.manifest`** drops the four per-subcarrier constants.
- **`score_rade`** puts librade's include paths first: upstream `wdsp/`
  has its own `nnet.h`, which otherwise shadows the Opus one librade
  needs.
- **The recorder writes format 3** (`rec_flags`). The hooks upstream kept
  in `src/diversity_auto.c` were an older writer that always wrote zero
  there, so the context-change and engine-reset bits were never set.

Checked on `TEST` at the port:

| Check | Result |
|---|---|
| `make -C test/diversity run` | all six pass; gaps reported (branch noise ratio, Level output, stand-down, carrier follow, wire helpers) |
| `test_capture` round trip | writer → reader → replay, 160 blocks, 0 differ |
| `replay_rade captures/divcap-20260903-190516.divc` | 4 acquisitions, 0.714 locked, −11.37 dB, 0.101: Finding 45's "new" row exactly |
| `score_rade` on the same capture | arm 0 332 frames, 97.9 %; arm 1 223 frames, 99.1 %: Finding 45 exactly |
| `test_props` | stored Coherence weighting → Flat (LC-006), stored Hang 1.0 s → 10 s (LC-011) |
| `test_rade` | the resync checks pass: drop at 2.82 s, not the timeout; a 31 dB, 5.1 s fade keeps its lock (LC-010) |

**There is no Hang to sweep.** Since LC-014 a RADE lock has no timeout:
it is held through any fade and replaced only when the resync search
finds a new one. `run_ref --hang` and `replay_rade --hang` now stop with
an error that says so. `run_ref` is also not byte-deterministic: its
worker thread can shift a read by a block, so repeat a replay before
reading anything into a single difference (on `165826`, one run in
three at a 600 s timer scored +39 against +57).

The quick commands, from the repository root:

```
make -C test/diversity run                      # unit tests
make -C test/diversity/devtools                 # replay_rade, run_ref, test_capture
make -C test/diversity/devtools run             # round-trip check
D=$HOME/sdr/freedv-gui/build_linux/_deps/freedv_backend-build
make -C test/diversity/devtools score RADE_DIR=$D/rade_src \
  RADE_INC="$D/rade_build/build_opus-prefix/src/build_opus/dnn $D/rade_build/build_opus-prefix/src/build_opus/include" \
  RADE_LIB=$D/rade_build/src/librade.so
LD_LIBRARY_PATH=$D/rade_build/src test/diversity/devtools/score_rade captures/<file>.divc
make DIVCAP=1                                   # radio with the Capture button; plain make strips it
```

### Scoring a change on Window, Carrier and Digital captures

`score_rade` scores RADE on decode. The other references have no decoder,
so `py/score_wideband.py` scores `run_ref` weight series the way the
findings do: the weight applied one block late (out = arm0 + w·arm1, as
`receiver.c` combines), and the SNR is signal in the RX passband against
noise in a guard band the loop never fitted on (Finding 18). It is
calibrated against Finding 38 on `235906`: 12.56 against 12.75 dB at gate
0, and 4.28 against 4.24 at 0.30.

To compare a change, build `run_ref` once with it and once without (a
scratch worktree with the change reverted), run both over the same
capture with the same settings, and score them together:

```
test/diversity/devtools/run_ref captures/X.divc --ref band --mode sum \
    --weighting flat --cohmin 0.20 --out after.csv
/path/to/run_ref_without_it captures/X.divc --ref band --mode sum \
    --weighting flat --cohmin 0.20 --out before.csv
cd test/diversity/devtools/py && python3 score_wideband.py ../../../../captures/X.divc before.csv after.csv
```

Pass the settings explicitly. `run_ref` otherwise takes the weighting the
capture recorded, and a Min coherence of 0.20 for every reference, which
isn't what `TEST` uses (Window 0.20, Carrier and Digital 0.30, Flat).

Beside the SNR, the scorer reports how often the loop acted in signal
and in noise-only blocks, the coherence of the noise-only blocks, and the
median |w| in each. The "from N" column scores from the first block
after both runs have acted, which takes `run_ref`'s cold start (w = 1
until the loop first acts) out of the comparison. Finding 38 shows that
artifact alone can be worth 8 dB on a capture that opens in dead air.

`py/match_arms.py IN.divc OUT.divc` writes a copy with arm 1 scaled to
arm 0's guard-band noise. Every capture in the set has lopsided arms
(arm 1 usually 14–15 dB hotter), so this is the only way to see what a
weight does on a matched pair. Channel, signal and noise correlation are
untouched.

LC-012 was assessed this way; the results are in `docs/changes.md`.

### Manual notches

A capture cannot record a notch: WDSP applies it downstream of the tap,
and the block record has no notch fields. But for the same reason, a
replay with a notch set is exactly what the radio would have done with
it. So:

```
cd test/diversity/devtools/py
python3 score_wideband.py ../../../../captures/X.divc --peaks 6
```

lists the strongest narrowband peaks in the passband, with the notch
centre that covers each and how steady it is. A carrier or heterodyne
reads near 100 % of blocks; speech and keying read far less. Then give
the same notch to both tools:

```
run_ref captures/X.divc --ref band ... --notch C:W --out notch.csv
run_ref captures/X.divc --ref band ...             --out plain.csv
python3 score_wideband.py captures/X.divc plain.csv notch.csv --notch C:W
```

`C:W` is centre and width in Hz, as the radio's notch menu stores them,
up to three. With `--notch`, the scorer leaves the notched bins out of
the passband, as WDSP leaves them out of the audio, using the engine's
rule (a bin entirely inside the notch). So both runs are scored on the
audio the operator would hear with the notch in.

## Why

Every number in `src/rade_correlator.c` was set against a synthetic signal
generated by `test/diversity/test_rade.c`. `docs/diversity-rade.md`
records a string of faults that only ever appeared on air — the pilot
pointer that failed to advance, the ratcheting hold reference, the
accumulators being fed pure noise during a freeze. Each was diagnosed from
console logs and a fresh guess, because there was no way to run the same
signal past the correlator twice.

Now there is.

## Taking a capture

Build the radio with the instrument in:

```
make DIVCAP=1
```

and back out again with a plain `make`. No `make clean` is needed either
way: `-D` changes are invisible to make, so the `DIVCAP` block keeps a
stamp in `src/.divcap-on` and drops the two affected objects when it
flips. That matters in both directions — `make DIVCAP=1` after a plain
make would otherwise link a menu calling an arming function the engine
was not compiled with, and a plain `make` after `DIVCAP=1` would leave the
instrument silently compiled into a binary meant to be clean.

**If there is no Capture button in the Diversity menu, the binary is a
default build.** `nm pihpsdr | grep div_capture_active` says which.

Configuration is environment only — deliberately, so that nothing about
this survives in an operator's `.props` file once it is removed:

```
PIHPSDR_DIVCAP_DIR       where the files go            default "captures"
PIHPSDR_DIVCAP_SECONDS   stops itself after this long  default 60
PIHPSDR_DIVCAP_NOTE      free text stored in the file  default ""
```

Then tune the station, open the Diversity menu and press **Capture**. The
button counts blocks as they are written and comes back out by itself when
the budget is up.

**Where they land.** `captures/` under the working directory, created on
the first capture. The whole directory is in `.gitignore` by name, so a
minute of 192 kHz I/Q cannot be committed by accident whatever the file is
called - the set runs to tens of gigabytes. Nothing else knows the path:
every tool here takes the file as an argument, so

```
./run_ref captures/divcap-20260903-123333.divc --ref band --out w.csv
```

is the shape of every command below with a real capture in it.

**Write a note.** `docs/diversity-guide.md` records that the engine does
not watch antenna or attenuator changes, and that on pre-Orion2 boards the
two chains are not symmetric. A capture that does not say which antenna
was on which ADC is not reusable six months later:

```
PIHPSDR_DIVCAP_NOTE="ADC0=80m dipole ADC1=beverage NE, 20dB att both, S3 QSB" \
PIHPSDR_DIVCAP_SECONDS=120 ./pihpsdr
```

Volume is 768 kB/s at 48 kHz and 3.1 MB/s at 384 kHz — 46 MB and 184 MB
for a minute.

What lands in the file is the analysis block exactly as the correlator was
given it: both arms, at the DDC rate, block aligned, ahead of any
combining, with the context that produced it and the state the correlator
was in. The tap is in `div_process_block()`, which is the only place all
of that exists together.

The context includes **both step attenuators**, from format version 2.
`div_context_changed()` compares them, so moving either resets the
statistics, and a capture that could not show them could not be replayed
through that reset - which is what happened on the capture where the
operator stepped ADC1 twice while recording, and the two settings had to
be inferred afterwards from arm 1's own noise floor. `att0` and `att1` sit
in what was `pad0` plus the padding already there, so the block record is
the same 208 bytes and a **v1 file still replays**; the tools say once
that its attenuators are unknown rather than letting two zeros be read as
two settings. `run_ref` follows them block by block on a v2 file.

**Format version 3 writes `rec_flags`.** It has two bits and they answer
different questions:

| bit | meaning |
|---|---|
| `DIVCAP_FLAG_CTX_CHANGED` | this block's context differs from the previous block's, compared **exactly** |
| `DIVCAP_FLAG_ENGINE_RESET` | the engine ran `div_reset_stats()` and `rade_corr_reset()` before this block |

They differ because `div_context_changed()` tolerates `DIV_RETUNE_HZ`, so
a slow dial walk sets the first bit on every step and the second on none
of them. Bit 0 is for a person reading a file back - it is how you find
the block where an attenuator or a filter moved. Bit 1 is what
`divcap_replay()` follows, so a recording containing a retune or an
attenuator step replays through the restart instead of diverging from it.

Before v3 the writer assigned `rec_flags` a literal zero and neither bit
was ever set. **Every capture taken before that fix is v2 or v1 and reads
zero on both bits whatever the operator did**, so on an older file the
only way to find a context change is to compare the recorded fields block
by block. `divcap_replay()` falls back to its own copy of
`div_context_changed()` for those, and on a v3 file it compares its copy
with the recorded bit and complains if they disagree - which is the alarm
for that copy drifting, as it had: it was missing both attenuators.

**Take a capture of nothing, too.** Same antennas, same band, no signal.
That is the companion run for any threshold sweep: it is what says how far
a threshold can come down before it starts finding pilots that are not
there.

### Walking a capture down to threshold

`--noise RMS` adds independent AWGN to each arm, per component - the one
part of the noise a two-branch array cannot null - so a capture that
decodes at 99 % can be pushed to where the modem is actually failing and
the correlator's constants have something to bite on. `replay_rade`,
`run_ref` and `score_rade` all take it, with `--seed`.

**The `--noise` and `--seed` must match wherever a weight series crosses
between tools.** All three seed the same generator and call
`divcap_add_noise()` once per block in file order, so the realisation is
identical; mismatch them and `run_ref` fits a weight to one signal while
`score_rade` decodes another, silently. The check that it is right: a
`run_ref` weight series at the same noise scores within 0.1 dB of
`score_rade`'s own `correlator` stream.

The threshold is sharp and the right level is per capture - the noise is
white across the DDC span and only 8 kHz of it reaches the modem, so a
48 kHz capture needs about a quarter the amplitude of a 192 kHz one.
`docs/diversity-measurements.md` Finding 41 has calibrated levels for two
captures and the method for finding more.

## Replaying

```
make -C test/diversity/devtools
./replay_rade cap.divc --verify
```

`--verify` first, always. It checks the replayed correlator state against
what the radio recorded, block by block. Until that passes, a sweep is
measuring this harness rather than the correlator.

**Except after the correlator itself changes.** A recording holds the
state the build that made it reached, so once `rade_correlator.c` is
altered `--verify` against an older capture reports differences and is
*supposed* to: it is comparing two different correlators, not finding a
harness fault. The check that still means something then is `make run`
below, which records and replays with the same build. Captures taken since
the change verify normally.

Then:

```
./replay_rade cap.divc --set use_ratio=2.0 --set probation=4
./replay_rade cap.divc --sweep use_ratio=1.5:4.0:0.25 --csv out.csv
./replay_rade cap.divc --sweep probation=4:16:4 --sweep use_ratio=2:3:0.5 --csv out.csv
```

Metrics per point: blocks, seconds, acquisitions, locked fraction, time to
first lock, mean pilot SNR, mean quality, weight jitter.

Replay is at the block level, calling `rade_corr_process()` directly with
the bank, frame offset and averaging time the live run used. There
is no queue, no worker thread and no `g_usleep()` pacing — which is why
the capture is taken at block granularity rather than per sample.

### What is sweepable

`replay_rade` with no arguments lists the names. They are the thresholds
and time constants, not the array dimensions:

```
acq_at0..2  acq_sigma0..2  probation  mag_alpha  use_alpha  use_ratio
floor_df  floor_guard  freq_alpha  freq_limit
```

`RADE_ACQ_FRANGE`, `RADE_ACQ_FSTEP`, `RADE_ACQ_TSTEP`, `RADE_DEC_CUTOFF`
and `RADE_DEC_TAPS_PER_PHASE` size static arrays and the pre-rotated pilot
tables, so they are not runtime-settable. Rebuild with `-D` to move one.

### How that works without touching the correlator

`src/rade_correlator.c` is the one file the diversity patch must not leave
marks on, so it has none. `mktunable.awk` generates a copy —
`build/rade_correlator_tunable.c` — with the `#define` lines named in
`tunable.manifest` removed and `rade_tuning.h` prepended in their place.
Every *use* site is untouched. The live build links the original; only
this harness links the generated one.

The generator exits non-zero if a manifest name is not found, so a
constant renamed in the correlator stops the build rather than silently
dropping out of the sweep. Worth checking occasionally:

```
sed 's/^#define RADE_USE_RATIO/#define RADE_USE_RATIO_X/' \
    ../../../src/rade_correlator.c > /tmp/renamed.c
awk -v manifest=tunable.manifest -f mktunable.awk /tmp/renamed.c   # must fail
```

## Scoring on decode, not on lock

`replay_rade` measures the detector. Whether RADE actually decodes is a
different question, and the one the combiner exists to answer.

```
D=$HOME/sdr/freedv-gui/build_linux/_deps/freedv_backend-build
make score \
  RADE_DIR=$D/rade_src \
  RADE_INC="$D/rade_build/build_opus-prefix/src/build_opus/dnn \
            $D/rade_build/build_opus-prefix/src/build_opus/include" \
  RADE_LIB=$D/rade_build/src/librade.so
LD_LIBRARY_PATH=$D/rade_build/src ./score_rade cap.divc
```

Three librade receivers run side by side over one capture — arm 0 alone,
arm 1 alone, and the two combined with the weight the correlator produces
as it goes:

```
stream      rx frames    in sync   sync %  mean SNR
arm0               73         64    87.7%     28.8
arm1               73         64    87.7%     27.6
combined           73         64    87.7%     30.3

combined - best arm: +0 synced frame(s)
```

The number to maximise is the last line. On a clean signal every stream
decodes and only the SNR column separates them, as above; the frame count
starts to matter on the captures worth having, which are the marginal
ones.

This target is optional and off by default. librade pulls in ONNX Runtime
and Opus, and nothing else here may depend on that being present —
`make` alone builds `replay_rade` and `test_capture` with only GTK and
fftw3f.

Two things to know about it. It `#include`s the generated correlator
rather than linking it, because the combining has to happen on the 8 kHz
stream that correlator's own NCO and decimator produce and those are
static; combining after the decimator rather than before is exact, since
the weight is one complex scalar over the block and the decimator is
linear. And radae's own `rade_dsp.h` defines `RADE_ACQ_FRANGE`,
`RADE_ACQ_FSTEP` and `RADE_ACQ_NFREQ` with different values from ours, so
`score_rade.c` includes librade first and then takes those three names
back — without that the correlator would be searching a different grid
here from the one it searches on air.

## The round-trip check

```
make -C test/diversity/devtools run
```

`test_capture` generates a synthetic two-arm RADE signal, runs it through
the real engine with the capture armed, then replays the file and checks
the correlator ends up in the same state block for block. It is what stops
the writer, the record layout and the replay drifting apart.

It steps ADC1's attenuator once part-way through, which is the only thing
an operator does that the samples cannot show. That gives the run a
context change to carry, and the check asserts three things about it: the
recorded `att1` follows the step, `rec_flags` bit 0 is set on exactly the
blocks whose recorded context differs from the one before, and bit 1 is
set once. The verify pass then has to reproduce the restart as well as the
tracking, which is what says a capture with a retune in it can be replayed
at all.

With `PIHPSDR_DIVCAP_DIR` set it keeps the file, which is how to get a
sample `.divc` to try the tools on without a radio:

```
PIHPSDR_DIVCAP_DIR=/tmp/caps make run
```

## Running whole references, not just the correlator

`replay_rade` calls `rade_corr_process()` directly, which is right for
sweeping the correlator's own constants. `run_ref` instead feeds samples
through `diversity_auto_sample()` at 12 ms pacing, so the worker thread,
`div_process_block()` and every reference run exactly as they do on air —
which is the only way to reach the Digital I/Q solve, since it is static
and lives on the analysis thread.

```
./run_ref cap.divc --ref band|carrier|rade|digital --out W.csv
./run_ref cap.divc --ref digital --weighting flat --out W.csv
./run_ref cap.divc --ref band --resolution 12 --out W.csv
```

It follows the operator's context **block by block** from the recording,
not just at block 0. That matters more than it sounds: with the context
pinned to the first block the recorded retuning is invisible to the
engine, so anything to do with `div_context_changed()` measures as having
no effect whatever. It cost one wrong conclusion before it was noticed.

**The transform size comes from the capture too**, and it did not always.
`div_auto_resolution` is not part of `div_get_context()`, so nothing used
to set it and the engine ran at its compiled default of 12 Hz bins -
nfft 16384 at 192 kHz - however the radio had been configured. Two things
followed and both were silent. The analysis window was two or four times
shorter than the recorded one on any capture taken at a finer setting.
And a recorded block then decomposed into two or four engine blocks
pushed back to back, which overran the four-deep queue: on an nfft 65536
capture that is one analysis block lost in four, the drop path calls
`rade_corr_reset()`, and **`--ref rade` never acquired at all** while
reporting a clean run. `replay_rade` was never affected - it hands
`rade_corr_process()` the recorded block directly.

`run_ref` now derives the target bin width from the header, prints the
transform size it settled on, and warns if that is not the capture's own.
The pacing is per engine block rather than per recorded block, so
`--resolution` can be used to sweep the Resolution control deliberately
without reintroducing the drops. Sweeping it is the one thing on that
menu that has never been measured on a recording.

## One thing this turned up

`rade_corr_reset()` used to clear every exported status word except
`rade_corr_freq_off` and `rade_corr_mirrored`, which are only written when
a lock is taken and so survived a reset — and a `rade_corr_stop()` /
`rade_corr_start()` — with the previous lock's values still in them. On
air that meant the menu showed a stale sideband and frequency for as long
as it took to re-acquire.

Fixed in `src/rade_correlator.c` as part of the diversity work proper.
`divcap_replay.c` still clears the two itself, which is now belt and
braces rather than a workaround.

## Removal

When the tuning is done, the measurements are in `docs/diversity-rade.md`
and the constants are settled:

```
sh test/diversity/devtools/remove.sh          # say what would go
sh test/diversity/devtools/remove.sh --do     # do it
```

which deletes the new files and every guarded block, all findable by one
grep. The dry run prints the count, which is the number to trust - it has
grown as the instrument has:

| File | What to delete |
|---|---|
| `src/diversity_auto.c` | the `#ifdef DIVERSITY_CAPTURE` blocks: the include, the drop stash, the previous-context memory and `divcap_ctx_differs()`, the reset flag in `div_process_block()`, the tap, the stash in the worker, `diversity_auto_capture_start()`, and the stop hook |
| `src/diversity_menu.c` | the `#ifdef DIVERSITY_CAPTURE` blocks: the include, the button callback, the button, the label refresh |
| `Makefile` | the `ifdef DIVCAP` block after the `OBJS` list |
| `.gitignore` | the `captures/`, `*.divc` and `src/.divcap-*` lines |
| the tree | `captures/` itself, once nothing left needs the recordings |

and check:

```
grep -rn 'DIVERSITY_CAPTURE\|DIVCAP\|diversity_capture' .    # must be empty
```

The commits are all prefixed `devtool:` so the set can be dropped with an
interactive rebase instead.

The guarantee that makes this cheap: with `DIVCAP` unset, every object
file is byte-identical to what it was before any of this existed. That was
checked when it went in, and it is worth re-checking before it comes out:

```
md5sum src/*.o > /tmp/before.md5
# ... apply or remove ...
make && md5sum -c /tmp/before.md5
```

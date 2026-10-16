# Diversity engine on a Pi 5: what it costs, and what is worth optimising

> **2026-10-08: this is the baseline for a rerun, and the rerun is done:
> [pi5-rerun-20261008.md](pi5-rerun-20261008.md) (no regression, no
> bottleneck; the functional suite did not run on the Pi).** The text
> below is the 2026-10-02 analysis, not the current figures. The engine has changed since: Auto bin width
> (LC-050 to LC-053: 24 Hz bins on Window and Carrier at an Averaging time
> of 1 s or less, so a smaller transform at twice the block rate), Level
> output always on (LC-055), the reference-only code cut (LC-049). The
> harness is updated (LT-026): see [Rerun](#rerun-2026-10-08-pending). One
> row below is withdrawn: **RADE V1 searching on an AM passband** is a real
> outlier (the search follows the passband: one pilot bank unless the
> passband straddles the carrier, as AM does), so it is not scored, and its figures (5.7-8.8 % up to 15.1 %) and
> the "worst case" sentence that rests on them should not be quoted.
> Read the SSB rows instead: at most 5.6 % up to 192 kHz, 11.9 % at
> 1536 kHz.

Measured 2026-10-02 with the LT-020 bundle (`make -C test/diversity
pi5-bench.tar.gz`, bundle `887d4016`) on a Compute Module 5 (Cortex-A76,
4 cores, `ondemand` 1.5-2.4 GHz, Debian trixie, gcc 14.2, FFTW 3.3.10).
Raw output: [pi5-bundle-cm5/](pi5-bundle-cm5/). The same bundle on the
i7-12700K: [pi5-bundle-i7-12700K/](pi5-bundle-i7-12700K/).

All figures are a share of **one** Pi core, paced as the radio runs (one
analysis block per block period), unless marked hot. The Pi has four.
The run was clean: no dropped blocks, every RADE V1 row locked or
searched as intended, the selection and decimator checks passed, and
the CPU stayed at 38-47 °C. `throttled=0x50000` was already set before
the run: under-voltage and throttling had happened at some point since
boot, though neither was active during it. It's worth checking that
module's supply.

Limits: the signals are synthetic, and only the diversity engine is
measured, not WDSP or the rest of piHPSDR. RADE V1's search cost varies
from run to run by the number of acquisition passes, so its rows are
good to about ±1 percentage point. `perf` was not installed, so there is
no function-level profile from the Pi.

## What it costs

| Reference | ≤ 192 kHz | 384 kHz | 768 kHz | 1536 kHz |
|---|---|---|---|---|
| Window, Carrier, FSK/Digital, CW | 0.15-0.75 % | 1.2-1.3 % | 3.0-4.1 % | 5.9-6.1 % |
| RADE V1, locked | 0.5-1.7 % | 3.2 % | 6.4 % | 10.7 % |
| RADE V1, searching (SSB passband) | 3.2-5.6 % | 4.8 % | 10.2 % | 11.9 % |
| RADE V1, searching (AM passband) | 5.7-8.8 % | 11.3 % | 9.9 % | 15.1 % |
| Sample path, receive thread (every reference) | ≤ 0.3 % | 0.4-0.6 % | 0.8-1.1 % | 1.5-1.8 % |

At the rates most people run (192 kHz and below) the engine costs under
1 % of one core for every reference except RADE V1. RADE V1 costs up to
about 6 % while searching on an SSB passband, and up to about 9 % on an
AM passband, which searches both sidebands. Even the worst case, RADE V1
searching on an AM passband at 1536 kHz, is 15 % of one core of four.
**Nothing here needs optimising for the Pi 5 to cope.** What follows is
about headroom, ranked by what it buys.

The Pi's paced figures are mostly *lower* than the i7's. The i7 clocks
down hard between blocks, and its scheduler can put the worker on an
efficiency core. The Pi's `ondemand` floor is 1.5 GHz. So the i7 is no
upper bound for the Pi; measure on the Pi.

## Where the time goes

**RADE V1's decimator is most of RADE V1's rate-dependent cost.**
`rade_corr_process()` decimates the DDC stream to 8 kHz with 16 taps a
phase. That is 64 multiply-adds per input sample at any rate, in double
precision over a circular delay line with a wrap test on every tap. On
its own (pi_bench, section 7) it costs 1.05 % at 192 kHz, 2.2 % at 384,
4.2 % at 768 and 8.4 % at 1536 kHz. That is 64-79 % of the whole locked
RADE V1 cost at 192 kHz and above.

**The FFTs are the rest of the rate-dependent cost** for every reference:
0.35-0.48 % at 192 kHz, 0.84-1.06 % at 384, and 4.5-5.1 % at 1536 kHz
(65536 points, 23.4 blocks a second).

**RADE V1's search** (the pilot acquisition) costs about 2.5-4 ms a block
at any rate: 3-5 % of a core while there is no lock. Without `perf` on
the Pi it can't be split further. On the i7, `rade_acquire()` is the
largest single function in the analysis thread.

## Optimisation candidates, ranked

1. **Vectorise the RADE V1 decimator: recommended.** The same filter in
   float, over planar delay lines doubled to remove the wrap test, summed
   in eight lanes, runs **3.0× faster on the Pi** (NEON) and matches the
   current output to about 1 part in 10⁷ of the output's peak. Saved, per
   rate: 0.7 % of a core at 192 kHz, 1.6 % at 384, 2.8 % at 768 and 5.5 %
   at 1536 kHz. Locked RADE V1 at 1536 kHz would go from about 10.7 % to
   about 5 %. The change is contained in `rade_correlator.c`, behind an
   unchanged interface. It is a candidate LC; the replays would score it
   bit-for-bit close to the current engine.
2. **FFTW_MEASURE plans, from wisdom: worth having, not urgent.** MEASURE
   transforms are 35-48 % faster than ESTIMATE's from 8192 points up
   (no faster at 4096): 4.5 → 2.9 % at
   1536 kHz, 1.06 → 0.65 % at 384, 0.48 → 0.27 % at 192. PATIENT gains
   nothing more. But planning MEASURE on this Pi took **17 to 51 seconds
   a size** (under a second on the i7). Our guess, unconfirmed: Debian's
   aarch64 FFTW has no cycle counter, so it times each candidate plan
   with a slow clock. That rules out planning when diversity is switched
   on. From saved wisdom a plan takes 0.3-0.5 ms. The way to have it
   would be the one piHPSDR already uses for WDSP (`src/main.c`, built in
   a thread at first start): build single-precision wisdom for the five
   sizes once (about 2.5 minutes on the Pi), and plan with
   `FFTW_WISDOM_ONLY`, falling back to ESTIMATE. That touches start-up
   code that isn't ours, so it's one for dl1ycf if at all.
3. **Decimate before the FFT: no, for now.** Decimating to 192 kHz with
   the vectorised filter costs 1.3 % at 384 kHz, 1.5 % at 768 and 2.8 %
   at 1536. It saves the difference between a 65536-point FFT and a
   16384-point one. That leaves a net loss at 384 kHz, about 0.3 % gained
   at 768 kHz, and about 1.2 % gained at 1536 kHz. Its one real benefit
   would be 12 Hz bins at 1536 kHz, which lifts the CW limitation there
   (LC-029). Revisit only if that limitation matters to someone.
4. **The noise floor, the sorts, the gather, the accumulation:
   negligible** (pi_bench sections 1-4, all under 0.1 %). Nothing to do.
5. **RADE V1's search:** 3-5 % while searching. The next step would be
   `perf` on the Pi (`sudo apt install linux-perf` and re-run the bundle),
   but at this cost it isn't pressing.

## Rerun, 2026-10-08 (pending)

**Why.** To verify that nothing regressed and nothing became a bottleneck
on the Pi 5 after the changes since 2026-10-02 (above), and to cover the
parts of the diversity code the first run did not time.

**What the bundle does now** (`make -C test/diversity pi5-bench.tar.gz`,
then `./run_pi5.sh` on the Pi, about 25 minutes, piHPSDR closed):

| Step | What | New since 2026-10-02 |
|---|---|---|
| `pi_bench` | kernels, FFT planners (ESTIMATE, MEASURE, PATIENT), the RADE V1 decimator and a vectorised one | the per-sample combine on the receive thread (`rx_add_div_iq_samples`, with the level scale), 2.2 ns a sample on the i7 |
| `bench_cpu`, paced and hot | every reference at 48 to 1536 kHz, as a share of one core, worker and feeder, with drops counted | transform size from Auto's own policy (checked against the engine, `NFFT-MISMATCH` otherwise); the 24 Hz tier on Window and Carrier; Best and Null (Level output rides on Sum and Best, so Sum against Null is its cost); three notches; CW with Best; the AM-passband RADE V1 row removed |
| Transitions | the engine's start, stop and restart in wall-clock ms at every transform size, in both tiers: what the UI thread waits for when diversity is switched on or off, the reference changes, or Averaging crosses 1 s | all |
| Functional suite | `make -C test/diversity run` on the Pi, for behaviour on the Pi's compiler and NEON | all |
| Comparison | `compare_pi5.py` lines every row up against `baseline/` (the 2026-10-02 CM5 run) | all |

**Verdicts** (`compare.txt`, last line):
- *FAIL*: a row dropped blocks, RADE V1 did not lock where it should (or locked where it should not), or the bench's transform size disagreed with the engine's.
- *REGRESSION*: a row in both runs costs more than 25 % and more than 0.3 points of a core above the baseline (1.0 point for RADE V1's search, which varies by about that between runs).
- *BOTTLENECK*: a row over 25 % of one core (worker plus feeder), or a start, stop or restart holding the caller longer than 50 ms.
- *NEW* rows have no baseline; they are checked against the two absolute limits only.

**Not covered, and why.** The menu's timers and the panadapter's
diversity overlay are GTK drawing and need the real application; WDSP and
the rest of piHPSDR are not measured either, as before. Compare like with
like: the baseline is the CM5 under `ondemand`, so rerun on that module,
in that state (the older note about `throttled=0x50000` still stands: check
the supply).

**Results:** [pi5-rerun-20261008.md](pi5-rerun-20261008.md). Run on the
CM5 the same evening: no regression, no dropped blocks, no bottleneck; the
worst row is RADE V1 on a single sideband at 1536 kHz, 13 % of one core.
The functional suite did not run there (missing `libopus-dev`; fixed in the
bundle). The comparison script's tolerances were corrected from this run.

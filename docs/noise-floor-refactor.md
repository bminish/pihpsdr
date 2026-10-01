# Noise floor: refactor plan before it goes to TEST

Written 2026-10-01 on `test/noise-floor`. Decisions taken by the owner:

1. Ratio (the floor outside the filter) is always the noise routine.
2. The CW tracker is improved on the back of it.
3. The "Sum noise" box goes.
4. It has to hold up at 48 kHz and at 1536 kHz.

The time-minimum ("Old") code stays, but only as the internal fallback for
a block where the outside floor cannot be measured. It is no longer a
choice.

## What the floor costs, and what it touches

`div_noise_floor_update()` runs on the analysis worker thread, once per
block, inside `div_process_block()`:

- it strides through the bins of the central 80 % of the DDC span, less the
  filter and 1 kHz either side, taking up to 1024 per arm
  (`DIV_NF_SAMPLES`);
- sorts each arm's sample (`qsort`, 2 × 1024 doubles), averages the 8th to
  12th percentile, and smooths at `DIV_NF_TAU` (2 s).

Measured on the branch at 192 kHz: about +0.2 ms per block, 0.2 % of one
core, mostly the two sorts (`docs/test-noisefloor.md`). Since `a5145aa6`
the CW reference runs it too. The block rate is set by Resolution, not by
the sample rate (block time = 1 / bin width), so the cost per second is
about the same at every rate up to the `DIV_MAX_NFFT` cap (below).

### Locking and deadlock

- **No new lock, so no new deadlock.** The floor holds no mutex. The
  worker takes `mbox_mutex` only around the queue, and runs
  `div_process_block()` outside it. The GTK thread never waits on the
  worker.
- **A race that matters more now:** `diversity_auto_reset()` runs on the
  GTK thread (menu callbacks, `rxtx()`) and calls `div_reset_stats()`
  directly. That zeroes `div_nf0`/`div_nf1`, `div_nf_valid`, `cw_nf*` and
  the rest while the worker may be half way through updating them. The
  worst case is a floor rebuilt from a zeroed value and marked valid: a
  noise ratio wildly off, smoothed in over 2 s, and fed into the Sum
  weight. Its comment calls this harmless because it was written for the
  accumulators, which the worker only adds to; it is not harmless for the
  smoothed floors and ratios. **Fix:** `diversity_auto_reset()` only bumps
  `reset_gen`, and the worker calls `div_reset_stats()` when it sees the
  change, as it already does for `rade_corr_reset()` (LC-023's pattern).
  This exists on `TEST` too and should go there as its own fix.
- **Status-line reads** of doubles the worker writes (`div_auto_arm_db`
  and the like) can tear on 32-bit ARM. Cosmetic only; leave it.
- **The attenuator calibration** (`docs/feature-att-calibration.md`) must
  read a snapshot the worker publishes, never wait for it.

### Limit the span: ±20 kHz

Today the floor samples ±0.4 × the sample rate: ±19.2 kHz at 48 kHz,
±76.8 kHz at 192 kHz, **±614 kHz at 1536 kHz**. At 1536 kHz on 40 m that
runs from 6.5 to 7.7 MHz, across the 41 m broadcast band and the
antennas' changing response, and the noise ratio between two antennas is
not the same across that much spectrum: on the 40 m captures the
broadcast part's ratio already differed from the amateur part's by
1.5-2 dB within ±77 kHz.

Measured on nine captures (40 m LSB, 20 m CW, and three from 2026-09-30),
the noise ratio from the outside floor at different half-spans:

| Capture | ±5 kHz | ±10 kHz | ±20 kHz | ±40 kHz | ±77 kHz |
|---|---|---|---|---|---|
| `162255` | −9.73 | −9.47 | −9.29 | −9.32 | −9.14 |
| `162444` | −9.11 | −9.05 | −9.16 | −9.27 | −9.01 |
| `162615` | −9.26 | −9.45 | −9.26 | −9.25 | −9.16 |
| `162729` | −9.48 | −9.15 | −9.21 | −9.43 | −9.19 |
| `163953` | +1.55 | +1.54 | +1.51 | +1.62 | +1.61 |
| `164121` | +1.32 | +1.42 | +1.46 | +1.43 | +1.54 |
| `212104` | −6.22 | −6.36 | −6.26 | −6.33 | −6.32 |
| `205652` | −15.58 | −15.46 | −15.69 | −15.47 | −15.18 |
| `185725` | −14.16 | −14.16 | −14.54 | −14.42 | −14.17 |
| Block-to-block scatter | 1.0-1.35 dB | 0.8-1.2 | 0.7-1.2 | 0.7-1.2 | 0.6-1.2 |

From ±10 to ±77 kHz the estimate moves by 0.5 dB at most; only the
scatter grows as the span narrows, and the 2 s smoothing (20-40 blocks)
takes that out. **Proposal: `DIV_NF_HALF_SPAN_HZ` = 20000, the span being
the smaller of that and 0.4 × the sample rate.** At 48 kHz that is the
whole usable span, as now; at 192 kHz and above every rate then measures
the same neighbourhood, nearer the passband. The bin count stays
comfortable: about 3400 at 12 Hz bins, less the filter, strided to 1024.

### Make it cheaper: quickselect instead of qsort

The two sorts are the floor's whole cost. Only the 8th-12th percentile is
needed, so a selection (quickselect to the 8th percentile, then to the
12th within the upper part) is O(n) instead of O(n log n), roughly a
factor of 5-10 at 1024 samples. Worth doing for the Raspberry Pi, where
the 0.2 ms is several times larger; not needed for correctness. The
result must be bit-identical to the sorted version (same elements
averaged); `run_ref` replays confirm it.

## The steps, as commits

Each one builds, passes `make -C test/diversity run`, and is replayed on
the captures before the next.

1. **Fix: resets happen on the worker.** As above. Small, and a `TEST`
   candidate in its own right (it touches LC-023's mechanism).
2. **The span limit.** `DIV_NF_HALF_SPAN_HZ` 20000. Replay the 39-capture
   set and T-013 to T-018: Sum and Best scores should move by no more than
   the table above suggests.
3. **Ratio always, engine side.** `div_wideband_sum_scale()` and
   `div_cw_solve()` take the outside floor whenever it is valid and fall
   back to the time minimum (Window/Carrier) or the off-tone floor (CW)
   otherwise. `div_eval_sum_noise`, `DIV_SUMNOISE_*` and the props key go,
   with every `PORT-TO-TEST` marker. `run_ref --sumnoise` goes.
4. **The Sum noise box goes.** Menu row and `sumnoise_cb()`. The Capture
   button takes the row back beside Invert, or the row closes up.
5. **Quickselect.** Optional; bit-identical replays as the check.
6. **CW, on the outside floor** (below), one commit per change, each
   scored on T-017 and T-018 with `cwwide.py`-style scoring (noise taken
   300-900 Hz from the tone) and `score_cw.py`.
7. **Sample-rate tests** in the harness (below).

## Improving the CW tracker

What T-017/T-018 showed: CW's own floor, the off-tone bins inside its
region, carries the station's keying sidebands and clicks (+10 to +20 dB
over the real noise 50-200 Hz from the tone). Those scale with each
antenna's signal, so a floor taken there reads the signal ratio as much as
the noise ratio. A minimum region width did not fix it (+1.23 → +1.69 dB
at 600 Hz minimum, the fade case untouched); the outside floor did
(+1.23 → +2.08, +1.39 → +1.99). `cw_nf0`/`cw_nf1` are used in three more
places, which should follow:

| Use | Today | Change | Expectation |
|---|---|---|---|
| Sum noise ratio | outside floor when Ratio is selected (`a5145aa6`) | always, `cw_nf` as fallback | measured: +0.6 to +0.85 dB |
| Best's per-arm SNR (`div_arm_publish()`) | tone power over `cw_nf` | tone power over the outside floor, scaled to the tone bins as `div_arm_from_floor()` does for Window | Best switches on the right evidence in a fade and with keyclicks; to be measured |
| Key detection seed (`cw_act_lo`) | `n0 + n1`, this block's off-tone floor | the outside floor, per bin | narrow filters stop depending on one or two edge bins; to be measured |
| `DIV_CW_MIN_BINS` (6) | needed for the off-tone floor and key detection | with both of those moved, the region only needs the tone and a bin either side: try 3 | a 26 Hz filter acts instead of holding a stale weight; at 1536 kHz (23 Hz bins) filters under about 120 Hz stop being held; to be measured, especially for picking the wrong peak |

The tone search stays inside the filter: only the noise measurement looks
wide. Widening the search would let it lock onto a station the operator
narrowed the filter to exclude.

## At 48 kHz and at 1536 kHz

| | 48 kHz | 192 kHz | 1536 kHz |
|---|---|---|---|
| FFT size at 12 / 6 / 3 Hz requested | 4096 / 8192 / 16384 | 16384 / 32768 / 65536 | 65536 at all three (`DIV_MAX_NFFT`) |
| Bin width achieved | 11.7 / 5.9 / 2.9 Hz | 11.7 / 5.9 / 2.9 Hz | 23.4 Hz |
| Blocks per second | 11.7 / 5.9 / 2.9 | 11.7 / 5.9 / 2.9 | 23.4 |
| Floor span today | ±19.2 kHz | ±76.8 kHz | ±614 kHz |
| Floor span with the limit | ±19.2 kHz | ±20 kHz | ±20 kHz |
| Bins to choose from (SSB, 12 Hz) | ~2900 | ~3000 | ~1300 |

**48 kHz holds up.** The span is ±19.2 kHz, the same as the proposed
limit, and the table above shows ±20 kHz gives the same estimate as ±77 on
every capture. It needs at least about 12 % of those bins to be band noise.
On a crowded band that is fine on the captures we have (41 % of a 192 kHz
span was occupied and the percentile still found the noise). The case to
watch is a wide filter: FM, or a hand-placed window wider than the filter,
excludes more of a narrow span. If fewer than `DIV_NF_MIN_BINS` (128)
remain, the floor is invalid and the time minimum takes over, which is why
it stays as the fallback.

**1536 kHz needs the span limit, and is where CW changes most.**

- Without the limit the floor spans 1.2 MHz, across band edges and
  broadcast bands. With it, ±20 kHz as at every other rate.
- `DIV_MAX_NFFT` (65536) caps the transform, so bins are 23.4 Hz whatever
  Resolution says (true from 384 kHz up, where 3 Hz is out of reach and
  12/6 Hz need not be). The status line already shows the bin width
  achieved. Twice the block rate of 192 kHz at 12 Hz, so twice the
  per-second cost of everything per-block, the floor included; the two
  65536-point FFTs per block, which are there today, dwarf it.
- CW at 23.4 Hz bins: tone ±1 bin is 70 Hz, and the 6-bin minimum is
  140 Hz, so every CW filter under about 120 Hz is held. Lowering the
  minimum (CW table above) matters most here.

**Not covered by any capture so far:** 48 kHz and 1536 kHz. Every capture
in the set is 192 kHz.

### Tests to add to the harness

- **Noise ratio at three rates:** two synthetic noise arms with a known
  ratio (say −10 dB) at 48, 192 and 1536 kHz, with a few strong carriers
  across the span; the floor's ratio within 0.5 dB at each rate.
- **Fallback:** at 48 kHz, an FM-width filter that leaves fewer than 128
  bins; the floor reports invalid and the Sum weight uses the time
  minimum without a step.
- **Span limit:** at 1536 kHz, a noise ratio that differs between the
  inner ±20 kHz and the rest of the span; the floor reports the inner one.
- **CW at 23.4 Hz bins:** `test_cw`'s keyed tone at 1536 kHz with a 100 Hz
  filter acts (after the `DIV_CW_MIN_BINS` change) and takes the keyed
  signal's channel.
- **Reset race:** a stress test calling `diversity_auto_reset()` from a
  second thread while blocks run; the floor never reports valid with a
  zero arm.

And captures: one at 48 kHz and one at 1536 kHz on a busy band, and a CW
one at 1536 kHz with filters of 100 Hz and under.

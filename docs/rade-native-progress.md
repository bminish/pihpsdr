# RADE native: progress

Plan: [rade-native.md](rade-native.md). Branch `feature/rade-native`.
Updated with every commit that moves the work. Newest entry first.

## Status

| Phase | State |
|---|---|
| 0 Build/link `librade`, T-0, T-1, T-11 | in progress |
| 1 Harness | not started |
| 2 RX | not started |
| 3 Tuning, SNR, logging | not started |
| 4 TX | not started |
| 5 Extras | not started |

## Log

- 2026-10-15: branch created from `TEST` (0c871087). Plan written and
  reworked around the WDSP I/Q chain. Diversity docs moved to
  `docs/archive/diversity/`. Phase 0 starting.

## Open questions (from the plan)

- RX panel `copy` value piHPSDR leaves for SSB (T-11).
- Does TXA pass a complex input unchanged (T-11)?
- Is `rade_tx` output one-sided (T-1)?
- ALC / overshoot control / RX AGC on OFDM (T-9, T-10).

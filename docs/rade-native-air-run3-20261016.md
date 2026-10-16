# RADE native: air run 3 - GPS-stamped timing, start and end of the over (28645 kHz, drive 20 %, 2026-10-16)

Session `rade-captures/air/20261016-143040_28645kHz` (git-ignored; `results.csv`): V1 and V2, levels 0 and +3 dB, three repeats each, 12 runs, the
clip `david_vk5dgr.wav` (11.5 s), Kiwi GPS-stamped (`iq`, `iq-fixed`; time axis corrected by -2.2 ms, `kiwi_time_check.py`), the engine's event times and
WDSP's output capture on the PC's clock (chrony). Method, accuracy (about +-3 ms Kiwi side, +-5 ms WDSP side) and the meaning of the columns:
`rade-native-gps-timing.md`.

## Summary

| Question | Answer (measured) |
|---|---|
| Is the end of the over clipped? | **No.** The end-of-over frame decoded in 12 of 12 overs and the callsign in 6 of 6 (V1); nothing of the end of the speech is missing (`speech_end_missing` 0 in all). The RF stops 5.5-7.7 ms after the last modem energy leaves WDSP, about 3-5 ms sooner than the start latency predicts: at the limit of what the method resolves (+-3 ms), so **at most a few ms of the very end of the ring-down could be missing, not any of the frame** |
| Is the pipeline emptied before the radio is released? | **Yes, with a lot to spare**: the engine's first unkeyed block comes 113-137 ms *after* the last modem energy left WDSP, and the RF has ended about 7 ms after it. About **115 ms of the key-down is dead time with no RF** |
| Are starts clipped? | **The first part of the speech is not decoded by the receiver: V1 loses 0.36-0.60 s (mean 0.52 s), V2 0.20-0.24 s (mean 0.21 s).** It is receiver acquisition: librade syncs 0.48-0.74 s (V1) / 0.24-0.29 s (V2) after the RF onset, at an SNR of 18-33 dB, so it is not a weak-signal effect. The transmitter's start is clean: 14-21 ms slew, the first modem sample leaves WDSP on time |
| Transmit frequency | **+0.1 Hz V1 (0.003 ppm), -0.6 ... +1.0 Hz V2** against the GPS-corrected Kiwi: the radio is on frequency to about the measurement's own limit (see "A correction") |

## Delays (means; ranges in the CSV)

| | V1 | V2 |
|---|---|---|
| key-down to first modem sample into WDSP (the two-frame prefill) | 240 ms | 80 ms |
| engine -> WDSP output (first modem energy) | **78.8 ms** (78.3-79.3) | **78.7 ms** (78.1-79.6) |
| WDSP output -> RF on the air (protocol, radio, Kiwi path) | **10.9 ms** (10.0-11.6) | **10.2 ms** (9.0-11.1) |
| engine -> RF (the two above) | 89.7 ms | 88.9 ms |
| key-down to RF onset | **335 ms** | **174 ms** |
| RF rise (10-90 % of the amplitude) | 20 ms | 18 ms (14-20) |
| RF onset to librade sync | 645 ms (476-743) | 268 ms (240-292) |
| last EOO sample into WDSP to last energy out of WDSP | 78.2 ms | 76.5 ms |
| last energy out of WDSP to RF end | 7.2 ms | 6.3 ms |
| engine's hold after the last EOO sample (`TX_TAIL` etc.) | 159 ms | 160.5 ms |
| last energy out of WDSP to the engine's first unkeyed block | **124 ms** | **121 ms** |

- The WDSP delay is 78-79 ms, not the 66 ms the offline probe predicted (the probe had the speech stages off and no radio protocol; about 12 ms more is in the real chain). It is the same for V1 and V2
  and for both levels. The radio side adds about 10 ms.
- **The key-to-RF delay is mostly the prefill** (240 ms V1, 80 ms V2): two modem frames are buffered before the first sample goes out so the output never runs dry.

## The start of the over

- **Sync takes 0.5-0.7 s in V1 and about 0.27 s in V2, at any SNR here**, and the speech sent in that time is lost: V1 0.52 s, V2 0.21 s on average. The first decoded frames are also poorer for a while: feature loss in the first 250 ms is 0.05-0.29
  (V1) and 0.05-1.37 (V2, one run) against 0.02 in the middle; the first second is 0.027-0.086 (V1) and 0.026-0.60 (V2).
- It is not the transmitter's delay: the modem frames are on the air from the RF onset; the receiver needs several frames to find them. A transmitter-side remedy exists: **send a lead-in of modem frames with no speech
  before the speech** (for V1 about 5-6 frames, 0.6-0.7 s; V2 about 6 frames, 0.24 s) so the receiver is in sync when the speech starts. The first word is then kept, at the cost of delaying the speech by the lead-in (the mic would be buffered); the receiver
  hears the same silence it hears now instead of a missing word. **Not implemented**; it is a design choice (the operator pays the delay, or a short word is lost). It would need a check that the lead-in frames sync as quickly as speech frames (they carry the same pilots).
- Level makes little difference: V1 0 dB 560 ms clipped, +3 dB 480 ms; V2 227 / 200 ms (three runs each).

## The end of the over

- The end-of-over frame is transmitted 2.4 dB (V1) / 0.6 dB (V2) **below the average power of the speech frames at the 0 dB level**, and 1.2 dB / 0.1 dB below at +3 dB (power over its last 100-150 ms against the steady level); the last 50-100 ms are lower still
  (the slew and the TX filter's ring-down). The callsign decoded every time here (SNR 18-33 dB), but the end-of-over frame needs about 8.5 dB for V1 and its level is part of that threshold (`rade-native-air-run2-20261016.md`): raising the end-of-over frame by the
  difference (+2.4 dB at 0 dB drive, still within the ALC ceiling since its crest is lower) would be worth about that much at the margin. **Not done; not tested.**
- **The hold is longer than it needs to be.** From the last EOO sample into WDSP, the last energy leaves WDSP 78 ms later and the RF is over 7 ms after that: 85 ms. The engine holds the radio for `TX_TAIL` = 8000 samples (167 ms, measured 159 ms after the last sample), so the key is down for
  about 115 ms with nothing on the air. A tail of about 5000 samples (104 ms) would leave a 20 ms margin and shorten every transmit-to-receive turnaround by about 60 ms. **Not changed.**
- The RF fall time reads 116-147 ms at the 0 dB level and 5-18 ms at +3 dB: the fall includes the end-of-over frame, which is a few dB under the rest at 0 dB, so the "90 %" point is reached well before the RF ends; it is not a slow slew.

## A correction

Earlier in the day this work said the +11.2 Hz (28645 kHz) and +5.4 Hz (14250 kHz) frequency offsets seen on the first runs, both +0.39 ppm, were the radio's. **They were the Kiwi's**: those runs were before the GPS was fitted (their `session.json` has `gps_good` 0 and `fixes` 0), so its ADC clock was
uncorrected. With the GPS locked the same transmitter reads +0.1 Hz (V1) and -0.6 ... +1.0 Hz (V2). The GPS-timing document and the earlier statement in this session are corrected; the earlier SNR, ALC and filter results do not depend on the frequency offset
(the decoder tracks it, and every run decoded).

## Limits

- Three runs per cell, one clip, one path and one afternoon. The timing figures are +-3 ms (Kiwi side) and +-5 ms (WDSP-output side) accurate; the differences quoted of 3-5 ms are inside that.
- The Kiwi's loop antenna and the 12 kHz IQ path set the detection of the RF edges; the RF edge times are the modem-band power at -10 dB of the steady level, smoothed over 5 ms.
- The harness's `mox_off_after_wdsp_last_ms` column (about -500 ms) is the key-up *request*, which precedes the end-of-over hold: it is not a measure of early release (corrected in the column definitions); `unkey_after_wdsp_last_ms` is.
  V2 runs 7, 8 and 12 have no `unkey` time (the engine's first unkeyed block was not seen before the poll).
- The receive-side turnaround was not measured (the Kiwi cannot hear our receiver).

## Candidates (none applied; each needs a decision)

1. A **lead-in** of modem frames before the speech (recovers 0.5 s of V1 speech start and 0.2 s of V2): the biggest finding of the run.
2. **Raise the end-of-over frame** to the speech's average power (up to +2.4 dB V1 at 0 dB drive).
3. **Shorten `TX_TAIL`** to about 5000 samples: about 60 ms off every turnaround. **Done (5120 samples, 107 ms) on request, same day; not yet measured on the air - re-run the GPS timing and check that `unkey_after_wdsp_last_ms` is about 64 ms, `tail_after_eoo_ms` is no worse and the end-of-over frame still decodes in every over.**
4. Working drive +3 dB (from the ALC work): also reduces the first two items' gaps (EOO 1.2 dB below instead of 2.4).

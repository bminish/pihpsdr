# Open items

Part of the local-change register: [changes.md](../changes.md).

What is known and not yet done: faults tracked but deliberately left, things flagged for a later patch, work still to port, and where the CPU goes on a Pi.

## Client/server: tracked, not fixed

Decided 2026-10-01: at this stage we do not work on the client/server
model. Faults in it are recorded here and fixed later; where upstream
changes it, we take upstream.

- **The radio ignores a client's auto-diversity settings.** `f5a0ce9c`
  put the radio-side `CMD_DIV_SETTINGS` case in `server_command()` under
  `#if 0`, with a comment that the server only sends such data on
  connect. But the client's menu still sends it (`div_send_settings()`
  when `radio_is_remote`), and the radio now logs "forgotten case" and
  drops it. Radio-to-client is handled in `client_thread.c` and is not
  affected. Worth telling dl1ycf.
- **The client fills its settings block from the wire only.**
  `diversity_client_set_settings()` leaves the per-reference slots
  uninitialised, so a reference change on the client recalls garbage.
  That was LC-003's client half (deferred).
- **A client's Min coherence move would be dropped** if the handler came
  back: the wire carries only the live value, and LC-026 takes the slot.
  That was LC-004 (deferred).
- `diversity_menu_refresh()` and `div_populate_from_settings()` are gone
  upstream; nothing updates an open menu from a settings block.

## Flagged for a later patch

- **Upstream text still says ADC0/ADC1** (for dl1ycf, not changed
  here):
  - `rx_panadapter.c`'s overload warnings ("ADC0 overload", "ADC1
    overload", "ADC0+1 overload");
  - `receiver.c`'s "hard-wired to ADC0" comments;
  - the protocol and simulator comments (`old_protocol.c`,
    `new_protocol.c`, `newhpsdrsim.c`, `hpsdrsim.c`), which already mix
    both conventions;
  - the `ADC0`/`ADC1` overload globals in `radio.c`.

  The receive menu already says ADC1/ADC2.

- **A mode change with the Diversity menu open no longer refreshes it.**
  `f5a0ce9c` removed `g_idle_add(diversity_menu_settings_changed, ...)`
  from `diversity_auto_mode_changed()` and the function itself. The
  engine swaps in the new mode group's settings, but the open menu keeps
  showing the old group's controls, and a moved control then writes the
  displayed (old) value. Rare, since the mode seldom changes with the
  menu open. Taken from upstream for now (decided 2026-10-01). The
  intended cure is E5 in [ownership.md](ownership.md#review-e1-to-e8): the per-group settings move
  into `profiles.c`.
- Upstream leftovers from `f5a0ce9c`, harmless: `diversity_auto.h` still
  declares `diversity_auto_ref_store()` / `_recall()` and mentions
  `DIV_REF_SCHEME`, neither of which exists any more.

## Not carried

- **6 Hz default bins.** An earlier local commit changed the
  no-saved-settings default resolution from 12 Hz to 6 Hz. We dropped
  it and keep upstream's 12 Hz. Speed of response is usually what we
  want, and the Averaging slider, not Resolution, is the control an
  operator uses most of the time. 6 Hz doubles the FFT length and the
  block duration. The operator can still choose 12, 6 or 3 Hz.

## Efficiency on the Pi: closed (measured 2026-10-02)

Measured on a CM5 with the LT-020 bundle. The analysis is in
[../bench/pi5-analysis.md](../bench/pi5-analysis.md). **The Pi 5 copes:**
at 192 kHz and below every reference costs under 1 % of one core,
except RADE V1 (up to about 6 % searching on an SSB passband, 9 % on an
AM passband). The worst case, RADE V1 searching on an AM passband at
1536 kHz, is 15 % of one core of four. What is left is headroom:

- **Candidate LC:** vectorise RADE V1's decimator. 3.0× faster on the Pi,
  same output; saves 0.7 % of a core at 192 kHz and 5.5 % at 1536 kHz.
- **For dl1ycf, if at all:** FFTW_MEASURE plans from wisdom built at
  first start (about 2.5 minutes on the Pi, as WDSP's), 35-48 % off the
  FFTs from 8192 points up. Planning MEASURE live takes 17-51 s a size on the Pi, so it can
  only come from wisdom.
- **Not now:** decimating before the FFT (a gain only at 1536 kHz), the
  sorts (negligible), and RADE V1's search (3-5 %; needs `perf` on the
  Pi to go further).
- Done earlier: the noise floor's selection (LC-033).

## Pending: to be ported from `feature/auto-diversity`

From the 2026-10-02 evaluation of the findings' open list
(`diversity-measurements.md`, "Status on `TEST`"), these are described
as done in the findings but are on `feature/auto-diversity` only:
- the empty-band stand-down (`div_window_quiet()`, `DIV_QUIET_DWELL`);
- the time-based slew (Finding 48);
- the 0.5 s default averaging;
- the 24 / 12 / 6 Hz Resolution menu (`DIV_MIN_NFFT` 2048);
- the Carrier tooltip.

Features and documentation will be brought in from
`feature/auto-diversity` one at a time. Each one gets the next `LC`
number, a commit (or a short run of commits) that follows the
[house rules](rules.md), a row in the register and a write-up in the
matching topic file, all in the same push.

Noted while porting, not yet decided:

- The validation scenario for LC-013 (notches): no capture checked so far
  has a steady interferer inside a weaker station's passband. See "What the capture set offers so far" under [LC-013](lc-gate.md#lc-013).
- Stand-down (`fc0b3d1e`, `94b4cc6f`) against the hold rule. LC-012's
  capture scoring bears on it: see "Scored on recorded captures" under [LC-012](lc-gate.md#lc-012).
- A RADE V2 correlator. There is no V2 reference on `TEST` yet. Three V2
  captures (two stations, 40 m, heavy multipath, both decoding at about
  0 to 8 dB) are logged as T-008 in `docs/test-findings.md` as the
  starting set. T-009 adds one on the lower sideband (spectrum inverted)
  under strong SSB interference, the case for discriminating against an
  unwanted signal.
- **The noise floor (LC-025 and after): still open** (from
  [noise-floor-refactor.md](../noise-floor-refactor.md)): Best's CW SNR
  from the floor; `DIV_CW_MIN_BINS`; captures at 48 and 1536 kHz; and the
  attenuator calibration
  ([feature-att-calibration.md](../feature-att-calibration.md)).
- CW, from porting LC-017 to LC-019:
  - **The LC-012 floor binds on CW.** Three tone bins at CW's averaging
    put it at its 0.5 cap, so it, not the 0.10 setting, is the gate.
    Scored with and without it (a scratch build) over the 16 segments of
    the eleven captures: mean −0.14 dB with it, −0.07 without; without
    it is better on 7 segments and worse on 1. Small, and within the
    scorer's reach on these strong captures, but the direction is
    consistent. Revisit with marginal CW captures.
  - **LC-019's 0.2 s** wants re-sweeping on marginal CW captures.
  - **A strong carrier that appears** is accepted until key detection's
    floor climbs to it (LC-018, Limitation).
  - `score_cw.py` follows AD-50's yardstick but not its scripts, so
    AD-50's figures are not reproduced to the decimal. FSK/Digital
    scores better here than there (−0.14 against −0.42).
  - **A beacon's steady tone is rejected as a carrier** by key detection
    (T-012): the weight is held through it. FSK/Digital, which uses it,
    scores 0.3 dB more on that capture. Only a concern for beacons or
    tuning carriers that are themselves the wanted signal.

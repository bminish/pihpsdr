# Open items

Part of the local-change register: [changes.md](../changes.md).

Outstanding work comes first (as of 2026-10-19), then what is closed. Nothing here is a local change waiting to be written: LC-059 to LC-061 are committed and pushed; what is left is decisions that are the owner's, work to port or measure, and the client/server faults we are not touching yet.

## Outstanding

**Waiting on the owner (Brendan or dl1ycf)**

- [ ] Force-push of `TEST` rebased onto `upstream/TEST` `b0b1d4e9` (2026-10-19): done locally, `origin/TEST` is still `2f6e1f01`. Only on OK, to `origin`, never upstream.
- [ ] LC-059/LC-060, his to decide: `CLIENT_SERVER_VERSION` is not bumped, so a radio and a client built either side of them silently swap Null and Sum and move four reference numbers (rebuild both ends together, or bump so a mismatch refuses). Details below, "Objective and reference order".
- [ ] LC-059/LC-060 not checked on screen: each menu row against the running objective and reference.
- [ ] To discuss with dl1ycf, no code yet: E3 (live copy versus per-reference slots), E4 (Hold), E5 (per-mode settings into `profiles.c`). [menu-notes-dl1ycf.md](../menu-notes-dl1ycf.md#still-to-discuss-no-code-yet), [ownership.md](ownership.md#review-e1-to-e8).

**To port from `feature/auto-diversity`** (each gets the next LC, from LC-062)

- [ ] The time-based slew (Finding 48).
- [ ] The 0.5 s default averaging.
- [ ] The Carrier tooltip.

**To measure or build**

- [ ] The noise floor (LC-025 on): Best's CW SNR from the floor; `DIV_CW_MIN_BINS`; captures at 48 and 1536 kHz; the attenuator calibration ([noise-floor-refactor.md](../noise-floor-refactor.md), [feature-att-calibration.md](../feature-att-calibration.md)).
- [ ] CW, on marginal captures: whether the LC-012 floor binds (it does on strong ones, by 0.07 dB); re-sweep LC-019's 0.2 s.
- [ ] LC-013 (notches): a capture with a steady interferer inside a weaker station's passband. None so far; the mechanism is confirmed on air only.
- [ ] A RADE V2 correlator: no V2 reference on `TEST` yet; starting set T-008 and T-009 in [test-findings.md](../test-findings.md).
- [ ] Open from the 24 Hz tier (T-019/T-020): Carrier (six AM captures only), fast fades, Null against a real interferer, 96, 384 and 768 kHz, finer bins for narrow-tone digital modes.
- [ ] Pi: run the functional suite on the CM5 (needs `libopus-dev`, now in the bundle's check).
- [ ] Pi, only if wanted: vectorise RADE V1's decimator (3.0x faster, 0.7 % of a core at 192 kHz); FFTW wisdom for dl1ycf.
- [ ] LC-061's two seeds are chosen by what the modes carry, not by a sweep; a capture set per mode would back or change them.

**Client/server: tracked, not fixed (decided 2026-10-01; take upstream there)**

- [ ] The radio ignores a client's auto-diversity settings; the client fills its settings block from the wire only (LC-003, deferred); a client's Min coherence move would be dropped (LC-004, deferred); no menu refresh from a settings block. Detail below.

**Flagged for dl1ycf, not changed here**

- [ ] Upstream text that still says ADC0/ADC1. List below.
- [ ] A mode change with the Diversity menu open does not refresh it (cured by E5).

## Closed

- [x] LC-059 and LC-060, the objective and reference order (2026-10-15); LC-061, the fresh-install seeds (2026-10-15). Decisions: no props migration, capture format 5, wire not bumped.
- [x] LC-048 to LC-058 accepted into `upstream/TEST` (`07ae1f56`); the leftovers in `diversity_auto.h` were LC-058.
- [x] The empty-band stand-down: decided against (2026-10-14, T-026, settled decision 5).
- [x] The 24/12/6 Hz Resolution menu: ported as LC-051, replaced by Auto (LC-052, LC-053).
- [x] Pi 5 rerun (2026-10-08): no regression, no dropped blocks, no bottleneck; worst row RADE V1 on one sideband at 1536 kHz, 13 % of one core of four ([findings](../bench/pi5-rerun-20261008.md)). No code change.
- [x] The noise floor's selection (LC-033).
- [x] E1 (LC-031) and E2 (LC-032) of the ownership review.
- [x] Not carried, by decision: the 6 Hz default bins. We keep upstream's 12 Hz; the Averaging slider is the control an operator uses most, and 6 Hz doubles the FFT length and block duration.
- [x] `TEST` resynced onto `upstream/TEST` `b0b1d4e9` (2026-10-19): three of his commits (client discovery host entry, `wdsp` header tidy), no conflict.

## Detail on the outstanding items

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
  `diversity_client_set_settings()` (in `diversity_auto.c` since LC-056) leaves the per-reference slots
  uninitialised, so a reference change on the client recalls garbage.
  That was LC-003's client half (deferred).
- **A client's Min coherence move would be dropped** if the handler came
  back: the wire carries only the live value, and LC-026 takes the slot.
  That was LC-004 (deferred).
- `diversity_menu_refresh()` and `div_populate_from_settings()` are gone
  upstream; nothing updates an open menu from a settings block.

## Objective and reference order: what is still open after LC-059 and LC-060

Both enums were reordered to the menu order (done 2026-10-15). Still open, and his:

- `CLIENT_SERVER_VERSION` (`client_server.h`) is unchanged, so a radio and a
  client built on either side silently swap Null and Sum and move four
  reference numbers. A bump would make a mismatch refuse instead.
- A props file written before the change loads with Null and Sum, and four
  references, read the other way until reset (`reset_diversity_props.py`).
- Not checked on screen: each row against the running objective and
  reference.

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
- Done: the `f5a0ce9c` leftovers in `diversity_auto.h` (LC-058, in his tree).

## Notes on the work to port and measure

The list is at the top of this file; the evidence behind it:

- The validation scenario for LC-013 (notches): no capture checked so far
  has a steady interferer inside a weaker station's passband. See "What the capture set offers so far" under [LC-013](lc-gate.md#lc-013).
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

## Efficiency on the Pi: closed (rerun done 2026-10-08)

**Rerun result: no regression, no dropped blocks, no bottleneck** (worst
row: RADE V1 on a single sideband at 1536 kHz, 13 % of one core of four).
Write-up: [../bench/pi5-rerun-20261008.md](../bench/pi5-rerun-20261008.md).
Open from it: the functional suite has still to be run on the Pi (it
needs `libopus-dev`, now in the bundle's check). RADE V1 on an AM passband
is an outlier and stays as it is. What follows is the 2026-10-02 text.

Measured on a CM5 with the LT-020 bundle. The analysis is in
[../bench/pi5-analysis.md](../bench/pi5-analysis.md). **The Pi 5 coped:**
at 192 kHz and below every reference cost under 1 % of one core, except
RADE V1 (up to about 6 % searching on an SSB passband). That analysis also
quoted RADE V1 on an AM passband (9 %, 15 % at 1536 kHz); that is a real
outlier (the search follows the passband, one bank unless it straddles the
carrier), so it is withdrawn and not scored. No code change (decided
2026-10-08). The engine changed afterwards (Auto bin width, always-on
Level output); the 2026-10-08 rerun covers that and found no regression.
What is left is headroom, all optional:

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

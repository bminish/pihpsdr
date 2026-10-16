# RADE native: progress

Plan: [rade-native.md](rade-native.md). Branch `feature/rade-native`.
Updated with every commit that moves the work. Newest entry first.

## Status

| Phase | State |
|---|---|
| 0 Build/link `librade`, T-0, T-1, T-11 | done: librade builds, T-0, T-1, T-11, `RADE_NATIVE` make option |
| 1 Harness | started: T-11b chain test works; T-2 channels next |
| 2 RX | first working version: engine + menu + receiver hooks, offline-tested; needs an on-air capture |
| 3 Tuning, SNR, logging | not started |
| 4 TX | first working version: engine + menu toggle + TXA hooks + TX capture; validated offline against librade's own tools; needs on-air test |
| 5 Extras | not started |

## Log

- 2026-10-16 (first automated air run, 28645 kHz, 18 runs V1/V2 -30..+6 dB): written up in
  `docs/rade-native-air-run-20261016.md`. ALC ceiling at the P2 output is -0.97 dBFS and is reached at the
  0 dB level; +3 dB buys 0.8 dB (V1) of power, +6 dB nothing, with no measurable decode cost;
  sync holds to -24 dB, EOO/call need about SNR 7 dB (V1) / 5 dB (V2); V2's SNR estimate tops out near
  18 dB. Harness bug found and fixed (capture started twice: harmless message). Air delay not measured.

- 2026-10-16 (what counts as a RADE transmission): state.json `tx` and `tx_count`, which
  drive the Reporter's `tx_report`, now follow `rade_is_tx()`: keyed, a RADE waveform on,
  the RADE transmit engine in use, and not TUNE or two-tone. Before, any keying in RADE
  mode (tune, an SSB over) was reported as a RADE transmission. Built; not on a radio.

- 2026-10-16 (leveller revised on request): the gain is **held** through
  receive and from one over to the next (no reset at key-down; the loudness
  average does start empty each over). It changes only while transmitting and
  only on speech: 400 ms loudness before the gain at least -35 LUFS (raised from -50 on request), within 15 LU
  of the loudest speech of the over (forgets 0.03 dB/s), within 10 LU of the speech
  average. Pauses, hiss or room noise, even beyond the 10 s average, never raise it
  (`test_tx_level.py`: speech then 50 s of noise and silence, gain unchanged to
  0.05 dB; a room above the floor but 20 LU under the speech, unchanged). A very
  mic whose loudest syllables stay under -35 LUFS before the gain is not levelled at all, by design.

- 2026-10-16 (Reporter V1/V2/All, toolbar, drive, leveller, log): (1) Reporter:
  V2's host is `reporter-radev2.k6aq.net:443` TLS (from `../rade3_reporting.md`,
  freedv-gui's hard-coded value; a view-only connection returned a station
  list); the "v3" placeholder is gone. **Show** V1 / V2 / All chooses the
  list; one connection per reporter; reports go only to the reporter of the
  mode piHPSDR has selected (`core.plan`); tests in `test_reporter.py` and the
  new `test_gui_view.py`. (2) The RADE window is an action, `MENU_RADE`
  ("RADE Menu"), assignable to toolbar / g2panel / GPIO buttons like Mode.
  (3) Drive slider -6..+6 dB around the defined level (0 = the ALC-ceiling
  level), stored in the settings. (4) Leveller: acts only while transmitting,
  0 dB at every key-down and the loudness history cleared, 0.2-0.5 dB/s up,
  2 dB/s down for a shout, at most +20 dB, down to -30 dB; the meter still reads
  unkeyed (trim only). (5) Parameter log: `Log` button in the RADE window,
  `rade-logs/<time>_RADE.csv`, a new file each time; rx rows (sync, SNR,
  offset, librade statistics, buffer fills) per `rade_rx` call, tx rows (keyed,
  trim, leveller, drive, loudness, peak, buffers) per 100 ms, event rows
  (waveform, dial, mode, invert, tx, drive); lock-free rings and a writer thread;
  producer call mean 0.3 us (`test_log`). Built and tested offline; **not run
  on a radio**.

- 2026-10-16 (TX level and ALC, first on-air captures at 14250 kHz): TX levels
  into WDSP set per waveform (V1 0.357, V2 0.504; were 0.3) so the ALC peaks
  at about 0 dB, 0.5 dB backed off on request. Capture 20261016-005144 (V1,
  39 s): `tx_shape.py` PASS (centre +1467 Hz, tilt -0.2 dB), confirming the
  speech-stage fix; Kiwi shows 100 % sync, SNR 25.3 dB, EOO and call EI6IZ
  decoded. `alc_sweep.py` (offline): at the current levels the ALC is already
  at its ceiling, more level only lowers the TXA gain, decode not harmed; the
  live peak (-0.97 dBFS) is about 1 dB below the offline one, unexplained.
  Write-up in `docs/rade-native-signal-paths.md` section 7a.

- 2026-10-16 (signal paths): `docs/rade-native-signal-paths.md` - RX and TX diagrams, stage tables, what each display/report item is measured from, taps, parameter register, and a change log of the signal path (add a line with every change to a stage).

- 2026-10-16 (mode changes keep the energy still): built. `rade_native_plan()`
  (`src/rade_native.c`, unit-tested in `test_geometry`) gives the sideband
  and the dial for any change of waveform / Invert / RADE on-off such that
  `Fe = dial + sign * 1500` does not move; the RADE window's `retune()` applies
  it (dial via `vfo_id_set_frequency`, VFO mode DIGU/DIGL while RADE is on,
  USB/LSB on the same carrier when it is switched off). The sideband is now
  stored (`rade_native_get_invert()`), not recomputed from the dial; the
  receiver, transmitter and menu read it. The dial used everywhere (state
  file for the Reporter, logs, captures) is the CTUN frequency when CTUN is
  on. Reporter frequency: the dial is the carrier of the sideband in use, so
  V1 centred on 1985.5 is reported as 1987.0 and V2 as 1984.0 with no further
  change. Tuning across 10 MHz by over 100 kHz with V1 on re-applies the
  band default. Built and unit-tested; **not run on a radio**.

- 2026-10-16 (filtering and mode changes, from the captures): see
  `docs/rade-native-filtering.md`. In short: (1) the three TX captures show
  the SSB profile's CFC post-equaliser and leveler active during RADE
  transmit (energy centre +1650 Hz instead of +1457, bottom third of the
  carriers about 5 dB down); reproduced offline; guarded in
  `tx_set_compressor/equalizer/phrot/dexp` so the profile cannot switch them
  back on while RADE transmit is on (compiled, not run on a radio);
  `tx_shape.py` is the check for the next capture; effect on decode is
  small (feature error 0.041 vs 0.038 at 0 dB). (2) 1.6 kHz TX and RX
  filters are the right size (TX leaves the end carriers at 0 dB and is 98 dB
  down at 300/2700 Hz; RX decode is the same from 1.4 to 2.0 kHz). (3) V1
  pull-in is about +/-60 Hz whatever the filter. (4) at a fixed dial, Invert
  and V1 -> V2 below 10 MHz move the energy 3 kHz; the fix (dial and VFO
  mode follow the sideband) is not built, two questions are in the document.

- 2026-10-15 (Reporter: status, last TX, every call): checked against
  freedv-gui. It sends `tx_report` {mode, transmitting} when PTT changes
  and at start-up (`ongui.cpp`, `main.cpp`), and the server keeps the
  last-TX time and tells everyone (the `last_tx` the Reporter shows in its
  Last TX column, for us too); it sends `rx_report` for a call each time one
  is decoded. The Reporter already sent `tx_report` on each change and
  showed Status and Last TX; **what was missing was robustness**:
  (1) a transmission that began and ended between two looks at piHPSDR's
  state (the Reporter looks twice a second) would never have been reported,
  so the server's last-TX would be stale: piHPSDR now counts transmissions
  (`tx_count`, `tx_last_t` in the state file) and the Reporter, finding the
  count up with nothing transmitting, sends `tx_report` on then off;
  (2) of several calls decoded between two looks only the latest was
  reported: piHPSDR now keeps the last five (`heard` list) and the Reporter
  reports every one, oldest first, the same station again included;
  (3) piHPSDR's engine suppressed a call repeated within 5 s, now only the
  same frame flagged twice within a second. Tests: 75 checks
  (`test_reporter.py`) and `test_gui_sticky.py` (two calls arriving
  together, the same station twice: both sent; a short transmission: on,
  then off). Not known: whether the server's own last-TX needs anything
  more than `tx_report`.

- 2026-10-15 (Reporter: regular SNR, corrected to freedv-gui): the
  previous version needed a callsign to send the SNR and invented a rule to
  guess one (last call, within 3 min, within 5 Hz). **That was wrong**:
  freedv-gui does not need one. Its timer (freedv-gui3 `src/main.cpp`,
  "Special case for RADE--report '--' for callsign so we can at least report
  that we're receiving *something*") sends, while the modem is in sync and no
  new callsign has just been decoded, an `rx_report` with the callsign last
  reported in this reception - **blank if none yet** - the mode and
  `(int)(snr + 0.5)`, once every 10 timer ticks (about a second), and forgets
  the callsign when sync has been lost for the stats reset time (10 s).
  The Reporter now does exactly that: `PeriodicSnr` has no attribution
  rule; the interval is 1-30 s (default 5); rx_report SNR is `int(snr +
  0.5)` (so -3.6 goes as -3, as freedv-gui sends it; the end-of-over report
  uses the same). The window's line says what it is doing ("sending the SNR
  every 5 s (no call decoded yet, sent blank)" / "(call OH2BCD)").
  **One thing to check**: the freedv-gui comment says '--' but its code
  passes the combo box's text, which is empty until a call is decoded, so
  that is what is sent; whether the server shows or accepts an empty
  callsign is freedv-gui's behaviour, not tested here. Tests: 70 checks
  (`test_reporter.py`) and `test_gui_sticky.py` (the SNR goes out with a blank
  call before any is decoded, the end-of-over call goes out with the over's
  SNR, the live SNR then carries that call, nothing without sync or
  while transmitting). **Also**: the GUI tests ran with the window on the
  desktop and I think one was closed by hand part-way through, which looked
  like a failure; the window can now be built unmapped (`App(show=False)`)
  and the tests use that.

- 2026-10-15 (Reporter: regular SNR, sticky settings): **Regular SNR
  reports.** While a signal is in sync and reporting is on, the Reporter
  sends `rx_report` (callsign, mode, live SNR to the whole dB) every N
  seconds (3-30, default 5), and the window says so on its own line: a green
  dot and "sending for OH2BCD every 5 s   last: OH2BCD +9.0 dB 2s ago", or
  why not ("waiting for a signal", "paused while transmitting", "signal is
  not where OH2BCD was (+2 Hz, now +40 Hz)", "waiting: not reporting").
  **The attribution is an assumption and is stated in the tooltip**:
  `rx_report` needs a callsign and a V1 station only sends its call in the
  end-of-over frame, so during an over the station is taken to be the last
  one heard, provided that was under 3 minutes ago and the signal's
  frequency offset is within 5 Hz of where that station's over was
  (piHPSDR now also writes `heard_foff`). A different station arriving
  within 5 Hz and 3 minutes would be reported under the wrong call until
  its own end-of-over report corrects things; reports already sent cannot be
  withdrawn. The end-of-over report (the call actually decoded, the over's
  SNR) is separate and always sent. **Sticky**: the message is kept in the
  config and sent on every connection (also with the first, and again after a
  reconnect or a change of server) until Clear; Track (on/off, band or
  frequency), the Band filter, the Idle filter and the SNR settings are also
  kept and restored. Reporting itself is still never kept. Tests: 70 checks
  (`test_reporter.py`, mock server: the interval, the foff gate, staleness,
  pause while transmitting, the message on each of four connections), and
  `test_gui_sticky.py` drives the window (restore, first connection sends
  the saved message, the call then repeated SNR reports at the live value,
  a 40 Hz offset signal not attributed, nothing while transmitting, changes
  saved, a new start restores them and reporting is off). Not tested
  against a real reporter; the server's reaction to repeated `rx_report`s
  for one station is not known.

- 2026-10-15 (Reporter: CPU and slow close): **three causes, all mine**.
  (1) The first update was scheduled with `GLib.idle_add` and the update
  returns True (to keep its 0.5 s timer), so as an idle callback it ran
  flat out, forever: reading the state file, deciding, and rebuilding the
  station list each time. Measured with 60 stations: **99 % of a core**.
  (2) That rebuild cleared and refilled the list, which re-sorted and
  re-filtered the whole list for every row added. It now changes only the
  cells that differ, refilters only when rows, frequencies or filters
  changed, refreshes the list when something changed or every 3 s, and sorts
  newest-first on the update *time* rather than an age that changed every
  second. (3) **Slow close**: `Link` held its lock while it waited for the
  connection's thread to end, and that thread needed the same lock to say
  it was stopping, so every close waited out the 1-3 s join timeout. The
  connection is now stopped with the lock released; the socket is also shut
  down so a read in progress ends at once; and the window is hidden first.
  After: the Reporter alone idles at **2 % of a core** (no server; GTK's own
  cost is most of that), SIGTERM to exit **0.00 s**, closing the window in
  the test **0.00 s** (was 3.00 s, then 1.00 s). `test_gui_idle.py` guards
  this: the update runs about twice a second (9 times in 4 s) and the
  window closes in under 1.5 s. Not measured: the cost with the live
  server's traffic over a long run.

- 2026-10-15 (Reporter: SNR and heard calls): piHPSDR's state file now also
  carries the live receive state (`rx_sync`, `rx_snr`, `rx_foff`, rewritten
  at least every 2 s) and the last call decoded from an end-of-over frame
  (`heard_seq`, `heard_call`, `heard_snr`, `heard_t`; a call forces an
  immediate write). The Reporter shows "Receiving: signal, SNR x dB, offset
  y Hz" / "no signal" / "transmitting" and the last call heard, and when
  reporting is on it sends each newly heard call as freedv-gui's `rx_report`
  (`callsign`, `mode` RADEV1/RADEV2, `snr` rounded to a whole dB) on the
  server of the active mode. Not sent: your own call, a call older than 60 s,
  calls heard before reporting was turned on, anything while only watching.
  **What it cannot do**: report "the current SNR" on its own every few
  seconds, because `rx_report` is *about a callsign* and a V1 station's call
  only arrives with the end-of-over frame; so the live SNR is displayed
  (updating every couple of seconds) and the SNR sent to the server is the
  over's, taken with the call. V2 callsigns are not decoded yet, so V2 sends
  no `rx_report`. Tested against the mock server, not a real one (14
  more checks, 54 in all; and the window driven through the whole sequence:
  nothing sent while not reporting, the earlier call not replayed when
  reporting went on, a new call sent as `rx_report`, our own not).

- 2026-10-15 (Reporter would not launch): **cause**: `rade_reporter.py` died
  on import, `gi.repository.Gdk` was imported without `require_version`, so
  PyGObject loaded Gdk 4.0 against Gtk 3.0 (`Requiring namespace 'Gdk'
  version '3.0', but '4.0' is already loaded`). My tests had imported Gtk
  first and so never saw it. piHPSDR's button did start the program, but with
  its output sent to /dev/null and "started" printed as soon as the spawn
  succeeded, so nothing showed; the one-line status was also overwritten
  by the capture line every 250 ms. **Fixed**: Gdk 3.0 is now required
  explicitly; the start goes through `src/rade_launch.c` (detached in its own
  session, output appended to `~/.local/state/pihpsdr-rade/reporter.log`,
  and if the program ends within 5 s the window says so with its status and
  the log's name); messages have their own line at the bottom of the RADE
  window; and the program uses `GLibUnix.signal_add` where PyGObject has it
  (the old call is deprecated). `test_launch` checks: a program that fails
  at once is reported with its output in the log, one that runs on is not, a
  missing program fails cleanly, and the real Reporter started this way was
  still running after the parent had exited. **The piHPSDR that was running
  (started 22:39) is the older binary**: its button now works because the
  fix is in the script, but it will not show the new messages until it is
  restarted.

- 2026-10-15 (mic level): RADE TX now has a mic **trim** (-20..+20 dB), an
  **AGC** (leveller, on by default) and a **meter**, all in the RADE window
  (row under Transmit); trim and AGC are saved in the props file
  (`rade_native.mic_trim_db`, `rade_native.mic_agc`). The meter works when
  the radio is not keyed, so the level can be set before transmitting. It
  measures ITU-R BS.1770 loudness (K-weighted, mono, 48 kHz) of what goes
  into the vocoder, gated as the standard says and averaged over the last
  10 s so pauses do not count, and shows it against the **ideal of -20
  LUFS**: a bar from 20 dB under to 10 dB over (ideal two thirds along,
  green within 3 dB), and a line "Mic -22.4 LUFS (ideal -20): 2.4 dB ...
  about right / x dB TOO LOW / x dB TOO HIGH, peak, CLIPPING, AGC +x dB".
  **Where -20 comes from**: the integrated loudness of the speech clips
  that ship with rade_c (`wav/`): -24.7 to -17.1 LUFS, median -19.9. It is
  a reference level, not a tuned optimum. **What librade does with level**
  (David and Mel clips, nominal -18.5 / -21.1 LUFS, scaled from -40 to +10
  dB): the output follows the input 1.5 to 2.7 dB lower; the speech
  envelope stays within 0.95 (David) / 0.99 (Mel) of the nominal decode
  from 10 dB under to 3 dB over, 0.88-0.92 at 30-40 dB under (envelope only:
  how noisy quiet speech sounds was not judged), and from +3 dB over the
  clip starts to clip (0.4 % of samples at +10 dB on Mel, 2 % on David),
  so the penalty is on the loud side. **The leveller**: acts on the gated
  average, 0.5 to 2 dB/s up (faster the further under), 1 dB/s down, 6
  dB/s for a shout more than 8 dB over, range -20..+30 dB, holds when the
  mic is quiet (nothing under -55 LUFS is lifted) so it does not pump
  noise. It follows the meter, so it settles slowly from a very quiet mic:
  in the test a mic 24 dB too quiet reached within 3 dB in well under the
  97 s run. A gentle limiter (0.9 to 1.0 of full scale) stops the audio
  clipping; the meter reports the peak before it and CLIPPING when it
  acted. The Mic Gain slider still has no effect on RADE; the trim
  replaces it. Tests (`test_tx_level.py`, 15 checks): the meter agrees with
  an independent BS.1770 calculation to within 0.4 dB (it reads 0.4 dB
  high at every level), the trim moves it by exactly the dB set, the
  limiter reports, and the leveller brings mics at -24, -10 and +8 dB to
  within 1 dB of the ideal. `tx_validate.py` (V1 and V2) still passes with
  the leveller off, which the test engine now does by default; with it on,
  the TX features no longer equal the unprocessed reference's, as they
  should not. **Not done / not known**: the meter has never seen a real
  microphone or the radio's mic input; the 3 dB "about right" band and
  the -20 target are judgement; the meter shows nothing while RADE TX is
  off.

- 2026-10-15 (RADE Reporter): `tools/rade-reporter/` is a standalone
  FreeDV-Reporter-style window (GTK 3 via PyGObject, our own small
  Socket.IO client over `websocket-client`, so nothing new to install),
  modelled on freedv-gui's reporter: station list with Callsign, Locator,
  km, Hdg, Version, MHz, Mode, Status, Msg, Last TX, RX call, SNR, Last
  update; band filter, Track (band / frequency), idle filter, message
  Send / Clear, Request QSY, Website, QRZ / HamQTH lookup. The protocol is
  read from freedv-gui's `FreeDVReporter.cpp` (auth dictionary with
  `role` view / report, events `freq_change`, `tx_report`, `message_update`,
  `qsy_request`, `bulk_update` ...). **Reporting is off every time it
  starts** and the tick box is never saved. **Presence only**: `freq_change`
  and `tx_report` (mode `RADEV1` / `RADEV2`, transmitting) and the message;
  never `rx_report`. **Only the active mode**: V1 to qso.freedv.org, V2 to
  the v3 reporter; RADE off or piHPSDR gone withdraws it at once (it then
  just watches). **The v3 address is not known**: nothing in the freedv-gui
  or freedv-gui3 sources names a separate v3 server (both default to
  qso.freedv.org), so it is blank in Settings and V2 is not reported until
  it is entered. piHPSDR side (`rade_native_menu.c`): writes
  `~/.local/state/pihpsdr-rade/state.json` atomically (mode, dial, transmit,
  call; on change and every 2 s; `running:0` at exit), started from
  `radio_restore_state`; the RADE window's **Reporter** button starts the
  program detached (own session, no inherited descriptors), so it outlives
  piHPSDR (checked with the same `g_spawn_async` call and a stand-in
  child: the parent exited, the child kept running in its own session).
  Tests (`test/rade_reporter/test_reporter.py`, 40 checks): places and
  bands, the report/don't-report decisions, and the whole link against a
  mock server (watching sends no identity; reporting sends identity then
  `freq_change` + `tx_report`; PTT and frequency changes are sent once;
  V1 -> V2 leaves one server and joins the other; RADE off or piHPSDR gone
  leaves). With `--live`, a read-only look at qso.freedv.org listed 51
  stations. **A mistake to own up to**: while testing the window I ran it
  twice against the live qso.freedv.org with reporting switched on, so
  EI6IZ (grid IO53hu, RADEV1, 7.184 MHz, not transmitting) was briefly
  listed, about a second each time; the tests now use the mock server only.
  Not done: chat, the "Show" columns menu, miles, sound/notifications; and
  reporting has never been exercised against a real server on purpose.

- 2026-10-15 (Kiwi receive harness): `test/rade_native/kiwi_rx.py` records
  our transmissions off air from the KiwiSDR at `kiwi.minish.org:8074`
  (connection details as given; no password needed) and checks them.
  **Several connections at once work**: three in parallel were run
  against it (the status page says `users_max=1`, but 3 connections were
  accepted). Profiles, one `kiwiclient/kiwirecorder.py` each: `iq` (12 kHz
  complex, never compressed, Kiwi AGC), `iq-fixed` (IQ with the AGC off,
  `--gain`), `usb` / `lsb` (audio, `--ncomp`, uncompressed). **Kiwi IQ
  convention measured**: tuned 2 kHz below WWV on 5, 10 and 15 MHz, the
  carrier appears at +2000 Hz, i.e. positive frequency is above the tuned
  frequency (I + jQ), the same as librade; so an inverted signal (V1
  below 10 MHz) is conjugated, a normal one is not. Commands:
  `kiwi_rx.py record|analyze|run|selftest` (`--help` for the options:
  `--freq` the dial in kHz, `--wave v1|v2`, `--duration`, `--call`,
  `--ref speech.wav`, `--tx-freq` if the Kiwi is not on the dial,
  `--min-snr`, `--max-loss`). Per over it reports sync %, SNR (librade's
  estimate, dB in 3 kHz) mean/min/max, frequency offset, the EOO frame
  (how many, where, how long after sync ended), the V1 callsign decoded
  from it against `--call`, and with `--ref` the vocoder-feature loss and
  the speech-envelope correlation against the ideal decode. Output:
  `rade-captures/kiwi/<utc>_<kHz>_<wave>/` (`<profile>.wav` as recorded,
  8 kHz baseband, stats, events, decoded speech, `session.json`,
  `analysis.json`, `report.txt`). Decoder: `rade_decode_iq` (librade +
  FARGAN, writes the stats, EOO events, features and speech).
  **Checked without a transmitter** (`kiwi_rx.py selftest`: our TX engine's
  over with call EI6IZ, dressed as a Kiwi recording, noise added, Kiwi
  tuned off the dial): V1 inverted at 7053 and normal at 14236, V2 at
  7053 and 14236, IQ and audio profiles, all PASS at 12-20 dB SNR; a
  Kiwi tuned 0.3, 1.0 and 2.5 kHz away from the dial is handled. **Where
  the V1 callsign gives out: the speech stays in sync down to at least 2 dB
  but the EOO call decodes only from about 10 dB** (librade SNR estimate;
  tests at 8-10 dB injected: EOO flagged, call not decoded; 11 dB and
  above: decoded), because the EOO is one 144 ms frame. Against the
  real Kiwi: recorded 3 profiles in parallel for 10 s at 7053 kHz, no
  transmission on, correctly reported "no over found". **Not yet done:
  an actual transmission through it**; and the audio-quality figures need
  the transmitted speech to be known (`--ref`), which means feeding a known
  wav to the transmitter, which the RADE window cannot do yet (it takes the
  mic): a test-wav source in the TX path is the next piece for that.

- 2026-10-15 (RADE window as a working panel): from on-air use: (1) the
  "Transmit RADE" toggle only switched RADE transmit processing on, it did
  not key the radio. Now RADE transmit follows the waveform (V1/V2 selected
  = transmit is RADE as well; a "RADE TX" check box turns that off), and
  the window has a real **Transmit** button, which keys the radio the way
  the space bar does; the **space bar in the RADE window** does the same
  (also with the callsign box focused: it never contains a space). Both
  refuse unless RADE transmit is on, so they cannot send ordinary speech by
  mistake; the space bar in the main window is unchanged and, with RADE
  transmit on, sends RADE. The waveform, Invert and RADE TX controls are
  greyed while transmitting. (2) The window is no longer one of the
  numbered menus: it is not `sub_menu`, so opening the main menu or any
  other menu does not close it; it stays until the Close button or the
  window's x. It is transient for the main window and kept above it, and
  placed in its lower right corner (a window manager that refuses
  placement, e.g. Wayland, ignores that; drag it). Opening the RADE button
  again raises it. **Not exercised in the running GUI** (it needs a radio
  connected); built clean with no warnings.

- 2026-10-15 (end of over, callsign, call log): 
  **End of over / TX hold.** `radio_set_mox(0)` now asks the TX engine
  (`rade_native_tx_end_request()`); while RADE TX is on it keeps the radio
  keyed, lets the engine finish the speech in flight, pad the V1/V2 modem
  frame with silence, send the EOO frame (with the callsign, V1) and wait
  for WDSP's TX filters to put it out (8000 samples = 167 ms), and then
  releases (`rade_native_tx_end_state() == 2`, polled every 20 ms from the
  GTK loop in `radio.c`). Keying again meanwhile cancels the hold. Hold
  measured offline: 576-619 ms for V1, 384 ms for V2, after the last mic
  sample. Since the EOO was not reaching the receiver reliably in testing,
  the TX output now starts only once two modem frames are queued (a hole
  of zeros in the middle of the signal, from a late worker, made the EOO
  fail to decode intermittently; with the prefill: 0 underruns and 12 of 12
  V1 runs decode the call; before it, 2 in 5 batches failed one clip).
  **Callsign.** The RADE window has a "My call" box (7 characters,
  upper-cased, letters / digits / '/'), saved in the props file
  (`rade_native.callsign`, also `rade_native.invert`). When it is not
  empty it goes into the EOO frame. This is the V1 format freedv-gui uses
  (LDPC(112,56) over 56 QPSK symbols, CRC-8, 6-bit characters, interleaved;
  `src/rade_eoo_text.c`, a C port of freedv-gui's `rade_text.cpp` +
  `ldpc_*.cpp`, BSD terms kept in the file). **Interop was tested against
  freedv-gui's own code** (`make -C test/rade_native text_interop`,
  built unmodified): the two encoders write identical symbols; each decodes
  the other's, 7/7 clean and 139-140 of 140 at noise sigma 0.25 and 0.45.
  **Not done: V2 callsign text.** V2 carries text in a different channel (a
  stream of data symbols, framed by freedv-gui's newer `rade_text` with
  blocks, CRC and LDPC); the box says so when V2 is selected and sends
  nothing. **Receive.** Calls decoded from the EOO of a received V1 over
  appear in the RADE window (last six, newest first) and are appended to
  `rade-captures/rade-calls.csv`: utc, local, callsign, waveform, dial_hz,
  mode, effective_invert, snr_db (smoothed over the over, before it ended),
  foff_hz, rx_filter_low/high, agc, volume_db, adc, attenuation, preamp,
  and the capture stem when a capture was running (the same line goes into
  its `events.log`). Dial and mode are read up to 0.5 s after the decode.

- 2026-10-15 (TX, Phase 4 first cut): `rade_native.c` has the transmit
  engine: mic (I slot of WDSP's TX input buffer, 48 kHz) → 36-tap
  decimator to 16 kHz → `lpcnet_compute_single_frame_features` →
  `rade_tx` on a worker thread (own `struct rade`, own LPCNet state) →
  48-tap polyphase interpolator to 48 kHz → level 0.3 → conj unless
  invert → back into `tx->mic_input_buffer` as I and Q. WDSP side
  (`rade_native_menu.c`, "Transmit RADE" toggle): the existing
  `tx_xmit_captured_data_start()` state (EQ, leveler, CFC, compressor,
  DEXP off, panel gain 1), `SetTXAPanelSelect(3)`, and the TX bandpass
  from `tx_set_filter()` taking the RADE edges. Hooks in `transmitter.c`
  (`#ifdef RADE_NATIVE`): `tx_full_buffer` (substitute before `xdexp`/
  `fexchange0`, capture after), `tx_set_filter`, `tx_set_mic_gain` (the
  slider does not scale the modem). Not allowed with PureSignal. Capture
  now also writes `_tx.wav` (the TXA output block, float32 stereo at the TX
  IQ rate) and `_txmic.wav` (16 kHz speech into the vocoder).
  **Known gaps:** (the EOO gap that was listed here is closed, see above)
  only tested at 48 kHz TX IQ (P1 rate; P2 runs WDSP at 96 kHz and
  resamples to 192 kHz, not exercised); the mic gain slider is ignored
  while RADE TX is on; the level is fixed (0.3 into WDSP, ALC idle,
  drive sets power) and uncalibrated against power; no VOX/PTT-timing
  work; the TX filter edges are the nominal 1.6 / 1.0 kHz.
  Also still open from the capture work: no script yet that plots
  `stats.csv` (SNR / sync / offset) against `speech.wav` energy.

- 2026-10-15 (capture): the RADE menu has a **Capture** toggle. It writes
  to `rade-captures/` (created in the working directory, git-ignored):
  `<date>-<time>_<kHz>kHz_RADE-V<n>[-inv]_` + `rx48.wav` (float32 stereo
  48 kHz, L=I R=Q exactly as WDSP delivered it, includes the RX volume,
  WDSP sign convention), `speech.wav` (decoded speech as played, 48 kHz
  mono), `stats.csv` (one row per `rade_rx` call: sync, SNR, offset,
  ring levels, underruns, V2 per-symbol stats; `rx48_pos` indexes into
  `rx48.wav`), `events.log` (a line whenever dial, ctun, mode, filter,
  RADE settings, AGC, volume, NB/NR/SNB/ANF, ADC att/preamp, or diversity
  state changes), and `meta.txt` (the same, in full, at start and at the
  end, plus build commit and duration). WAV headers are rewritten every
  200 ms so a crashed capture is still readable. About 29 MB a minute.
  Replay a capture offline, through the same engine:
  `test/rade_native/test_rx_engine [--v2] [--invert] [--speed N] \
  <stem>_rx48.wav out.wav` (use `--invert` if meta.txt says
  `rade_effective_invert: 1`). Offline check: the recorded `rx48.wav` is
  bit-identical to the input, no dropped data, replay decodes.

- 2026-10-15 (RX engine, Phase 2 first cut): `rade_native.c` now has the
  receive engine: WDSP output (panel binaural = complex, 48 kHz) → conj
  unless invert → 48-tap decimator to 8 kHz → slow level normaliser →
  ring → worker thread (`rade_rx` → FARGAN, with the 5-frame `fargan_cont`
  warm-up as `rade_rx_wav` does) → 16 kHz speech ring → 3:1 interpolator →
  replaces the block in `rx->audio_output_buffer`, at the RX volume.
  Hooks (all `#ifdef RADE_NATIVE`): `rx_set_filter` (passband from the
  waveform and effective invert), `rx_set_af_binaural` (forced on),
  `rx_full_buffer` (calls the engine before `rx_process_buffer`),
  `new_menu.c` (RADE button in the RX column), `rade_native_menu.c`
  (Off / V1 / V2, Invert, edges, sync, SNR, offset, underruns).
  AGC is left as the operator set it (medium by default); `loss_check.sh`
  passes with medium AGC for V1 and V2. Receiver 0 only, state not saved
  in props, no TX yet.
  **How to try it:** `tools/rade/build_rade.sh` once, then
  `make RADE_NATIVE=ON`, run `./pihpsdr`, set receiver 0 to DIGU, open
  the RADE button, pick V1 or V2 (Invert to flip the default).

- 2026-10-15 (make option): `make RADE_NATIVE=ON` compiles `src/rade_native.c`
  and links `librade` (rpath into `third_party/rade_c-build`); default is
  OFF and the default build is unchanged and still builds. `rade_native.[ch]`
  holds only engine-independent code so far: waveform centres and widths,
  default/effective Invert (V1 below 10 MHz inverted, V2 never, user
  Invert XORed), WDSP passband edges (negative for inverted), the
  conjugation rule, and a probe that opens V1 and V2 contexts. Not called
  from the engine yet. `make -C test/rade_native check` tests it
  (librade 2.2 reported). A gotcha worth keeping: librade's headers include
  a `nnet.h` that clashes with `wdsp/nnet.h`, so only the `rade_native.o`
  object gets librade's include path.

- 2026-10-15 (T-11): `wdsp_probe.c` + `t11_wdsp_probe.py` run tones through
  the vendored WDSP RXA/TXA channels set up as piHPSDR does it. Results
  below. Needed: `WDSPwisdom()` (without wisdom FFT planning takes
  minutes), `dsp_size = 2 * buffer_size`, `RXASetPassband` (not
  `SetRXABandpassFreqs` alone: it leaves `nbp0` at its default and the
  output is ~ -130 dB).

- 2026-10-15 (Phase 0): `tools/rade/build_rade.sh` builds `librade`
  from `freedv/rade_c` pinned at `b3e3748` into `third_party/rade_c-*`
  (untracked; the old `third_party/rade_c` held only a stale build with
  no sources and is left alone). Harness in `test/rade_native/`:
  `t0_reference.py`, `rade_tx_iq.c`, `t1_tx_spectrum.py`. Results below.

- 2026-10-15: branch created from `TEST` (0c871087). Plan written and
  reworked around the WDSP I/Q chain. Diversity docs moved to
  `docs/archive/diversity/`. Phase 0 starting.

## Open questions (from the plan)

- RX panel `copy` value piHPSDR leaves for SSB (T-11).
- Does TXA pass a complex input unchanged (T-11)?
- Is `rade_tx` output one-sided (T-1)?
- ALC / overshoot control / RX AGC on OFDM (T-9, T-10).

## Findings

All from `test/rade_native/`, librade `b3e3748`. Measured unless marked.

**T-0, reference decode of `~/sdr/RadeV2_clips` (V2, `rade_rx_wav`)**

| clip | s | sync % | SNR dB | f-off Hz (mean, sd) |
|---|---|---|---|---|
| FDV_FromRadio ..EI6IZ_V2 | 64.5 | 97.1 | 15.6 | 2.6, 3.5 |
| g3sdr 21:37:53 | 20.5 | 81.3 | 6.5 | 0.6, 0.5 |
| g3sdr 21:39:55 | 27.5 | 84.6 | 4.4 | -1.3, 0.1 |
| g3sdr 21:43:05 | 80.6 | 82.9 | -0.9 | -1.5, 0.3 |
| g3sdr 21:47:20 | 148.8 | 25.9 | 0.4 | -0.7, 1.1 |
| g3sdr 21:51:18 | 11.3 | 97.2 | 3.0 | -1.5, 0.1 |
| g3sdr 22:51:53 | 665.1 | 97.9 | 4.5 | -5.3, 3.3 |

(clips resampled to 8 kHz; SNR and offset averaged over in-sync symbols.)

**Sync and SNR do not tell you the orientation (V2).** The same clips with
the spectrum mirrored about the 1500 Hz centre (`--invert`) still show
95-99 % sync and similar SNR and offset (FDV clip 99.4 % / 15.1 dB), yet
the decoded audio is different (RMS 1166 vs 1967, no useful correlation
with the normal decode). So "sync" cannot be the auto-invert detector for
V2, and a test of orientation has to look at the decoded output.
*Not yet listened to*: whether the mirrored decode is garbage or merely
different. Listen to `out/*.dec.wav` from both runs before relying on it.

**T-1, rade_tx output (`rade_tx_iq`, 10 s of speech)**

| | complex | real part |
|---|---|---|
| V2 (no TX BPF) | 99.9 % positive freq, 1070..1898 Hz, PAPR 3.4 dB | 50/50 split, PAPR 6.4 dB |
| V1 | 98.8 % positive, PAPR 0.8 dB, -20 dB edges -207..3211 Hz | 50/50 split, PAPR 3.9 dB |
| V1 + 1.6 kHz filter at 1475 | 100 % positive, 730..2223 Hz, PAPR 4.9 dB | |
| V2 + 1.0 kHz filter at 1500 | 100 % positive, 1098..1879 Hz, PAPR 3.6 dB | |

- The one-sided I/Q premise holds for both; the complex output has 3 dB
  lower PAPR than its real part.
- V1's modulator applies a time-domain soft limiter (`rade_tanh_limit` in
  `rade_ofdm.c`) and the library's V1 TX bandpass is off by default
  (`bpf_en = 0`, `rade_api.c:103`). About 10 % of V1's power lies outside
  750..2200 Hz. The 1.6 kHz TX filter removes it and puts the PAPR back to
  4.9 dB; the filter is therefore required for V1, and WDSP's peak control
  will see that 4.9 dB, not 0.8 dB.
- V2's library BPF (300..2700 Hz) is on by default; with it off the
  signal is already within 1070..1898 Hz.

**T-11, WDSP RXA/TXA with complex tones** (`test/rade_native/out/t11.txt` after a run)

- RX panel select 3, copy 0: output stays complex and one-sided. With
  copy 1 (I copied to Q) it collapses to a real signal (50/50 split).
- TX panel select 3: a complex input passes through TXA as a complex,
  one-sided output; bp0 gain is 6.0 dB (the 2.0 in `TXASetupBPFilters`).
  The speech stages being on or off, and ALC on or off, made no
  difference at this single-tone level; the level and OFDM behaviour of
  ALC / overshoot control is T-10, not done.
- **Sign convention: WDSP's positive passband selects the tone at
  negative numpy frequency (`I + jQ = exp(+jwt)`), on RX and TX.** So a
  conjugation is needed at both interfaces to and from `librade`, on top
  of Invert. Recorded in plan section 3.
- **piHPSDR's RX panel copy (read from source):** it never calls
  `SetRXAPanelCopy`; it calls `SetRXAPanelBinaural(binaural)` in
  `rx_set_af_binaural()` (`receiver.c:1724`), and WDSP maps that to
  `copy = 1 - binaural`. So normal audio is copy 1 (I to both
  channels) and **binaural on gives copy 0, i.e. complex I/Q out on
  L/R**. The RADE receive mode can use the existing binaural path
  (`rx->binaural = 1` for the tap) and restore it on leaving the mode.
- *Not yet done in T-11*: a `rade_tx` frame through TXA, a RADE signal
  through RXA, group delay, and the AGC level behaviour (with top gain
  80 dB a 0.1 amplitude tone came out +31 dB, i.e. the AGC is working on
  signals at ordinary IQ levels).

**T-11 second pass: a RADE signal through WDSP TX and RX** (`t11b_chain.py`, no noise yet)

Chain: features, `rade_tx`, x6 up, conj, TXA (DIGU, panel select 3,
speech stages off, filter 675..2275 Hz V1 / 1000..2000 Hz V2), RXA (same
edges, panel copy 0, AGC medium/top 80 dB), conj, /6, `rade_rx`. Measure:
decoded-vs-transmitted vocoder features, normalised error (no vocoder
involved), 5 s of speech.

| | sync % | SNR est dB | feature error |
|---|---|---|---|
| V1 direct (no WDSP) | 96.3 | 30.9 | 0.0174 |
| V1 through WDSP | 92.7 | 29.1 | 0.0177 |
| V2 direct | 96.6 | 17.5 | 0.0239 |
| V2 through WDSP | 94.5 | 17.1 | 0.0114 |

- Through WDSP TX+RX with the planned filters the link is transparent in a
  noiseless channel: V1 feature error 0.0174 vs 0.0177. (The V2 figure
  being lower than direct is within what the alignment search and a 5 s
  clip can give; read it as "no worse", not as "better".) Sync
  percentage is a little lower (starts later through the pipeline delay).
- Same result for RX AGC long / medium / fast / fixed 0 dB (V1): the
  decoder was handed the RX output re-levelled to rms 0.9, so this does
  **not** yet say what the AGC does to a fading signal; that is T-9 with a
  channel.
- **Delay through TXA + RXA: 11934 samples = 248.6 ms** at the default
  filter lengths (linear-phase FIR filters). Large against V2's 40 ms
  frame; `TXASetNC` / `RXASetNC` can shorten it. Not tuned yet.
- **TX level and ALC (first look, V1 and V2 behave alike):** the TX chain
  has +6 dB gain; ALC is idle up to an output of about rms 0.53 (V1,
  input rms 0.27) and then clamps the output to rms 0.67 / PAPR 3.5 dB,
  i.e. a peak limit at 1.0. With ALC off, output scales without limit
  (rms 3.5 at input rms 1.8). Link quality with the ALC clamping at input
  rms 0.9 was unchanged on features (0.0177) but the SNR estimate dropped
  from 29 to 25 dB. A sensible operating point is an input where the
  output peak stays below the ALC threshold; calibration to power is T-10
  proper.

**RX engine, offline** (`test/rade_native/test_rx_engine.c`, 50 s of speech
through the WDSP TX+RX chain, played in real time into
`rade_native_rx_block()` in 1024-sample blocks):

| | underruns | speech envelope vs original | latency incl. WDSP 0.25 s |
|---|---|---|---|
| V1 | 0 | corr 0.90 | 0.70 s |
| V2 | 1 (start-up) | corr 0.92 | 0.41 s |

Bugs found and fixed on the way: speech buffer truncated at 8 of 12 frames
per V1 modem frame; play-out cushion shorter than a V1 frame (now 200 ms V1,
100 ms V2). Speech was listened to by correlation only, not by ear.

**TX validation, offline** (`test/rade_native/tx_validate.py`; 12 s clips from
rade_c's `wav/`, reference = `rade_tx_wav` + `rade_rx_wav`, ours = TX
engine → WDSP TXA (the capture, far side of WDSP) → WDSP RXA → RX engine):

| clip | feature error | decode vs ref (envelope) | TXA out rms / PAPR | TXA band |
|---|---|---|---|---|
| brian_g8sez V1 | 0.004 | 0.98 | 0.52 / 4.8 dB | -2227..-738 Hz |
| david_vk5dgr V1 | 0.007 | 0.93 | 0.52 / 4.7 dB | -2227..-727 Hz |
| k0pfx_mel V1 | 0.009 | 0.96 | 0.52 / 4.7 dB | -2227..-727 Hz |
| brian_g8sez V2 | 0.004 | 0.98 | 0.41 / 3.5 dB | -1887..-1102 Hz |
| david_vk5dgr V2 | 0.007 | 0.97 | 0.42 / 3.4 dB | -1887..-1102 Hz |
| k0pfx_mel V2 | 0.009 | 0.98 | 0.42 / 3.3 dB | -1887..-1102 Hz |

All six PASS (feature error under 5 %, envelope correlation 0.90 or more
against the reference decode). The TXA output sits at negative frequency,
which is WDSP's upper sideband, inside the planned filter. Output level
is well under the ALC clamp (rms 0.67). Notes: (1) the waveform
correlation with the reference decode is 0.02-0.06 (V1) and 0.18-0.42 (V2)
even though the features agree to under 1 %: the vocoder output is not
phase-reproducible, so only envelopes are compared; (2) the first version of
the metric compared unaligned ends and wrongly failed clips, now aligned and
trimmed by 0.5 s; (3) end-to-end latency TX engine + WDSP + RX engine is
about 1.9 s on V1 (clip start to first decoded speech, measured on one
clip; not broken down by stage). The WDSP TXA+RXA pair alone was 0.25 s.

**End of over and callsign, offline** (`tx_validate.py`, TX engine with call
EI6IZ → TXA → RXA → RX engine): V1 decodes `EI6IZ` from the EOO on all three
clips, 12 of 12 runs; hold after release 576-619 ms (V1), 384 ms (V2); V2
has no call. Harness note: the TX engine test runs at 3x real time
(`--speed 3`); at 8x the worker is late and the test itself makes holes.
The TX worker's cost on a Pi is not measured.

## Next

1. Capture on air with the RADE button; report what happens.
2. On-air TX test (V1 first): Transmit RADE, Capture on, key up briefly into a dummy load or on a quiet frequency; check `_tx.wav` band and level, then the far end.
3. T-2: the same chain over AWGN and fading channels, with and without WDSP.
4. T-3: orientation matrix (Invert set right and wrong, V1 and V2).
5. Listen to the mirrored V2 decode.

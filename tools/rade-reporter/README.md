# RADE Reporter

A standalone window that shows the FreeDV Reporter (like freedv-gui's) and,
only when you turn it on, reports that you are present in the RADE mode
piHPSDR has selected. Started from the **Reporter** button in piHPSDR's RADE
window; it is a separate program and keeps running when piHPSDR is closed.

    python3 tools/rade-reporter/rade_reporter.py      # needs PyGObject (GTK 3) and websocket-client

- **Starts with reporting off, every time.** The tick box is never saved.
- **What is reported**, with freedv-gui's own events: your presence (callsign, grid, dial
  frequency, mode `RADEV1` / `RADEV2`, transmitting: `freq_change`, `tx_report`,
  `message_update`) and each callsign piHPSDR decodes from a received end-of-over frame
  (`rx_report`: the call, the mode, the SNR to the whole dB). Only while reporting is on,
  only in the active mode, never your own call, and not calls heard before you went on.
  The window also shows the live receive state (SNR and offset, from piHPSDR every
  couple of seconds) and the last call heard. **Regular SNR**, as freedv-gui does it: while a signal is in sync it offers the live SNR every second (no tick box: whenever you are
  Reporting and there is an SNR) and `Link._push` decides what goes: a changed SNR up to once a second, an unchanged one every 20 s (see "Traffic to the servers" below). No callsign is needed: until one has
  been decoded the report carries a blank one; after that it carries the last call decoded, until sync has been
  lost for 10 s. The window shows when it is doing this, and why when it is not. A V1 call arrives only at the end of an
  over, so there is no callsign to report with while the over is still going.
- **Only in the active mode**: V1 goes to qso.freedv.org as `RADEV1`; V2 goes to
  reporter-radev2.k6aq.net (the host freedv-gui's ms-rade-v2 branch uses) as
  `RADEV2`. Both can be changed in *Settings*. RADE off in piHPSDR, or piHPSDR
  gone, takes you off the list at once. Nothing is ever sent to the other reporter.
- **Show**: V1 only, V2 only, or All. That is only what the list shows (each
  shown reporter is watched; the same station on both appears twice). If you are
  present, the reporter for your mode is joined as a reporter even if it is not
  the one shown. The choice is remembered.
- piHPSDR tells it what it is doing through
  `~/.local/state/pihpsdr-rade/state.json`; settings (grid, servers)
  are in `~/.config/pihpsdr-rade/reporter.json`.
- Needs a callsign and a Maidenhead grid square (4, 6 or 8 characters). Changes to
  either are announced only after 1.5 s without typing.
- Kept between runs: the message (sent on every connection until Clear), Track, band and idle
  filters, grid, servers. Not kept: reporting itself, or the sort: every launch starts with the nearest
  station first (stations with no locator last), and a click on a heading changes it for that run.
- The Msg column starts 32 characters wide, longer text cut with an ellipsis (the tooltip and the
  right-click copy still have all of it); drag the heading edge to widen it.
- One window: starting it again raises the one that is open.

Files: `sio.py` (Socket.IO client), `core.py` (stations, places, the link, what
to report), `rade_reporter.py` (the window). Tests: `test/rade_reporter/`,
against a mock server; `--live` only watches the real one.

## The list: columns and filters

- **Columns**: drag a heading to move the column; right click any heading for the list of columns and tick/untick to show or hide them
  (at least one stays; Callsign cannot be hidden). Order and hidden columns are remembered.
- **Distance**: right click **km** for *Distance filter...*: show only stations between two distances from you (from your Grid; 0 = no limit on
  that side). Stations with no locator are hidden while it is on. The heading shows `km *` while it is on; the menu clears it.
- **Frequency range**: right click **MHz** for *Frequency range filter...*: only stations in a range of frequencies (stations that have sent no
  frequency are hidden while it is on). The heading shows `MHz *`.
- The filters are remembered, and apply together with Band, Track and Idle.

## SNR is only reported with a lock

No lock, no SNR: when piHPSDR's receiver is not in sync it tells the Reporter `rx_snr: null` (librade's SNR value then means nothing), and
nothing is sent. A lock at an SNR of exactly 0 dB, or a negative one, is a real measurement and is reported. A call decoded in an end-of-over frame is
reported with the SNR averaged over the reception's in-sync frames (the average starts afresh after 1.5 s without sync); if there were none, the call is
not reported. The RADE window and the received-calls log show "-" rather than 0 in the same cases.

- **Callsign**: not entered here; it is always the one set in piHPSDR's RADE window (shown as "Call ..."). **Reporting** is the former "Report my presence".
- **Show**: *Follow radio mode* (the default; shows the reporter of the RADE mode piHPSDR has selected, both while RADE is off) , V1 only, V2 only, All. The choice is remembered; Follow was made the default once, when this version first ran.
- **Colours**, as freedv-gui: orange while a station transmits; blue only while its status is Receiving and an rx_report with an SNR arrived in the last 20 s (5 s if the report had no call). Your own row is no longer marked.
- **Traffic to the servers is kept to what is new** (`Link._push`): while the dial is being tuned nothing is sent at all (no frequency, transmit state, message or reception report); the frequency goes out once the dial has stood still for 5 s, or at once when a signal is in sync with an SNR. After that, at most one event of each kind (frequency, transmit, message) per 5 s; reception reports are limited by their own rule: a changed SNR up to once a second, an unchanged one repeated every 20 s only while genuinely in sync with an SNR (a keep-alive, so the station stays shown as receiving), otherwise not at all; what is waiting is merged so the newest goes out when its turn comes (a decoded call goes before a live SNR), and anything identical to what the server already has is not sent.  A transmit on/off still goes out, delayed to the next 5 s slot if it follows another within 5 s. Tests: `pacing()` in `test/rade_reporter/test_reporter.py`.

## Tuning piHPSDR from the Reporter

- **Double-click a station's MHz** (a single click only selects the row): piHPSDR selects that station's RADE mode (V1 or V2) and tunes its dial to the
  station's frequency. A station on any other mode, or with no frequency, is not acted on, and the line under the buttons says why.
- **Tune box**: a frequency in kHz to 0.1 (up to 999999.9), then Enter: the dial only, the mode is left alone.
- **Frequency list** (drop-down next to it): the usual frequencies for the RADE mode piHPSDR is in (with RADE off, the mode *Show* is set to; V1 when both).
  Picking one tunes the dial. The lists start as freedv-gui's default Reporting frequencies (identical in the V1 and V2 trees today), are kept
  per mode in `reporter.json` (`freqs.v1`, `freqs.v2`, Hz) and are always shown lowest first.
- **Settings > Frequencies**: a basic editor for the two lists (Add, Delete, double click a frequency to change it); applied on OK.
- How: the Reporter writes `~/.local/state/pihpsdr-rade/command.json` (`{"seq", "wave", "dial_hz"}`, atomic); piHPSDR polls it with its state
  timer (250 ms), acts once on each `seq` and only if the request is under 10 s old (one left from before piHPSDR started is ignored),
  never while transmitting, and answers in `state.json` (`cmd_seq`, `cmd_err`), which the Reporter shows. The mode is set first (it can move the
  dial 3 kHz for Invert), then the dial to exactly the frequency asked. Changing the mode needs piHPSDR's RADE window open. Test: `test/rade_reporter/test_gui_tune.py`.

## Request QSY (checked against freedv-gui, 2026-10-17)

Same protocol as freedv-gui's `FreeDVReporter::requestQSY` / `onFreeDVReporterQsyRequest_` (identical in both its trees):
- **Sent** as `qsy_request {dest_sid, message, frequency}`: the selected station's sid, an empty message (freedv-gui's is also empty, "custom message TBD"), and our dial in Hz as an unsigned integer.
  The button is enabled only as in freedv-gui's `refreshQSYButtonState`: a station is selected, we are Reporting on its reporter (call and grid set), it is not our own callsign, we have a frequency, RADE is on, and it is not already on our frequency.
  Sent, the line under the buttons says "QSY request sent to CALL" (freedv-gui pops up a box).
- **Received** as `qsy_request {callsign, frequency, message}`; anything where callsign or message is not a string, or frequency is not an unsigned integer, is dropped (as freedv-gui does). It opens a box: "CALL has requested that you QSY to X MHz." (freedv-gui's default of MHz to 4 places; it shows kHz to 1 place if its "frequency as kHz" option is on), with the message and the reporter it came on.
- **Change Frequency**, as freedv-gui offers when it can change frequency: the box has *Cancel* and *Change Frequency*; the latter selects the RADE mode of the reporter the request came on (V1 or V2) and tunes to the frequency asked. Not offered (the box only tells) while piHPSDR is transmitting or is not running. Not checked: what the live servers do with it (tests use the mock only): `test/rade_reporter/test_gui_qsy.py`.

## Buttons

Left to right: **Request QSY**, **Website** (the reporter's site for the mode shown; both sites when both are shown, or RADE is off and Show is Follow), **Filters** (the columns / distance / frequency menu), **Settings**.

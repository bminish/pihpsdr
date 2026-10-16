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
  couple of seconds) and the last call heard. **Regular SNR**, as freedv-gui does it: while a signal is in sync it sends the live SNR every few
  seconds (fixed at 5 s, no tick box: it is sent whenever you are Reporting and there is an SNR; freedv-gui sends about once a second). No callsign is needed: until one has
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
  filters, the sort column and direction, grid, servers. Not kept: reporting itself.
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

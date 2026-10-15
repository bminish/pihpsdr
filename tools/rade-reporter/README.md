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
  seconds (1-30 s, your choice; freedv-gui sends about once a second). No callsign is needed: until one has
  been decoded the report carries a blank one; after that it carries the last call decoded, until sync has been
  lost for 10 s. The window shows when it is doing this, and why when it is not. A V1 call arrives only at the end of an
  over, so there is no callsign to report with while the over is still going.
- **Only in the active mode**: V1 goes to qso.freedv.org as `RADEV1`; V2 goes to
  the v3 reporter as `RADEV2`. RADE off in piHPSDR, or piHPSDR gone, takes you
  off the list at once. The address of the v3 reporter is set in *Settings*;
  until it is, nothing is sent for V2.
- piHPSDR tells it what it is doing through
  `~/.local/state/pihpsdr-rade/state.json`; settings (callsign, grid, servers)
  are in `~/.config/pihpsdr-rade/reporter.json`.
- Needs a callsign and a Maidenhead grid square (4, 6 or 8 characters). Changes to
  either are announced only after 1.5 s without typing.
- Kept between runs: the message (sent on every connection until Clear), Track, band and idle
  filters, the SNR setting, callsign, grid, servers. Not kept: reporting itself.
- One window: starting it again raises the one that is open.

Files: `sio.py` (Socket.IO client), `core.py` (stations, places, the link, what
to report), `rade_reporter.py` (the window). Tests: `test/rade_reporter/`,
against a mock server; `--live` only watches the real one.

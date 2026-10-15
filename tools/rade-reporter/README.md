# RADE Reporter

A standalone window that shows the FreeDV Reporter (like freedv-gui's) and,
only when you turn it on, reports that you are present in the RADE mode
piHPSDR has selected. Started from the **Reporter** button in piHPSDR's RADE
window; it is a separate program and keeps running when piHPSDR is closed.

    python3 tools/rade-reporter/rade_reporter.py      # needs PyGObject (GTK 3) and websocket-client

- **Starts with reporting off, every time.** The tick box is never saved.
- **Presence only**: callsign, grid, dial frequency, mode (`RADEV1` / `RADEV2`)
  and whether you are transmitting, sent with freedv-gui's own events
  (`freq_change`, `tx_report`, `message_update`). No receive reports.
- **Only in the active mode**: V1 goes to qso.freedv.org as `RADEV1`; V2 goes to
  the v3 reporter as `RADEV2`. RADE off in piHPSDR, or piHPSDR gone, takes you
  off the list at once. The address of the v3 reporter is set in *Settings*;
  until it is, nothing is sent for V2.
- piHPSDR tells it what it is doing through
  `~/.local/state/pihpsdr-rade/state.json`; settings (callsign, grid, servers)
  are in `~/.config/pihpsdr-rade/reporter.json`.
- Needs a callsign and a Maidenhead grid square (4, 6 or 8 characters). Changes to
  either are announced only after 1.5 s without typing.
- One window: starting it again raises the one that is open.

Files: `sio.py` (Socket.IO client), `core.py` (stations, places, the link, what
to report), `rade_reporter.py` (the window). Tests: `test/rade_reporter/`,
against a mock server; `--live` only watches the real one.

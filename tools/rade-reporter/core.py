"""The parts of the RADE Reporter that are not the window: places, stations, and
the link to a FreeDV Reporter server.

The server side is the one freedv-gui talks to (src/reporting/FreeDVReporter.cpp
in the freedv-backend): Socket.IO, an auth dictionary at connect
({role: "view"} to watch, {role: "report", callsign, grid_square, version,
rx_only, os} to be listed), then events:

  we send:     freq_change {freq}, tx_report {mode, transmitting},
               message_update {message}, qsy_request {dest_sid, message, frequency}
  we receive:  new_connection, remove_connection, freq_change, tx_report,
               rx_report, message_update, qsy_request, connection_successful,
               bulk_update (a list of [event, data])

What this program reports is presence: that the station is on a frequency in
the RADE mode piHPSDR has selected (RADEV1 or RADEV2), and whether it is
transmitting; and the receive reports (heard calls, SNR). All of it goes only to
the reporter for the mode piHPSDR has selected: V1 to qso.freedv.org, V2 to
reporter-radev2.k6aq.net. What is *shown* is chosen separately: V1, V2 or both.
"""
import json
import math
import os
import platform
import re
import threading
import time

from sio import SioClient

# --------------------------------------------------------------------- places

_GRID = re.compile(r"^[A-R]{2}([0-9]{2}([A-X]{2}([0-9]{2})?)?)?$")


def grid_valid(g):
    g = (g or "").strip().upper()
    return len(g) in (4, 6, 8) and bool(_GRID.match(g))


def grid_to_latlon(g):
    """Centre of a Maidenhead square (4, 6 or 8 characters)."""
    g = g.strip().upper()
    lon = (ord(g[0]) - 65) * 20 - 180
    lat = (ord(g[1]) - 65) * 10 - 90
    lon += int(g[2]) * 2
    lat += int(g[3])
    w, h = 2.0, 1.0
    if len(g) >= 6:
        w, h = 2.0 / 24, 1.0 / 24
        lon += (ord(g[4]) - 65) * w
        lat += (ord(g[5]) - 65) * h
    if len(g) >= 8:
        w, h = w / 10, h / 10
        lon += int(g[6]) * w
        lat += int(g[7]) * h
    return lat + h / 2, lon + w / 2


def distance_bearing(g1, g2):
    """(km, bearing in degrees) from square g1 to g2, or None if either is not a square."""
    if not (grid_valid(g1) and grid_valid(g2)):
        return None
    la1, lo1 = map(math.radians, grid_to_latlon(g1))
    la2, lo2 = map(math.radians, grid_to_latlon(g2))
    dl = lo2 - lo1
    a = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin(dl / 2) ** 2
    km = 6371.0 * 2 * math.asin(min(1.0, math.sqrt(a)))
    y = math.sin(dl) * math.cos(la2)
    x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(dl)
    return km, (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


BANDS = [("160 m", 1800e3, 2000e3), ("80 m", 3500e3, 4000e3), ("60 m", 5250e3, 5450e3),
         ("40 m", 7000e3, 7300e3), ("30 m", 10100e3, 10150e3), ("20 m", 14000e3, 14350e3),
         ("17 m", 18068e3, 18168e3), ("15 m", 21000e3, 21450e3), ("12 m", 24890e3, 24990e3),
         ("10 m", 28000e3, 29700e3), (">= 6 m", 50000e3, 1e12)]


def band_of(freq_hz):
    for name, lo, hi in BANDS:
        if lo <= freq_hz <= hi:
            return name
    return "Other"


# ------------------------------------------------------------------- stations

class Station:
    __slots__ = ("sid", "callsign", "grid", "version", "rx_only", "connect_time", "freq", "mode",
                 "transmitting", "last_tx", "message", "rx_call", "rx_snr", "rx_mode", "last_update",
                 "seen")

    def __init__(self, sid):
        self.sid = sid
        self.callsign = self.grid = self.version = self.mode = self.message = ""
        self.rx_only = False
        self.connect_time = self.last_tx = self.last_update = ""
        self.freq = 0
        self.transmitting = False
        self.rx_call = self.rx_mode = ""
        self.rx_snr = None
        self.seen = time.time()

    @property
    def status(self):
        if self.transmitting:
            return "Transmitting"
        if self.rx_only:
            return "Receive Only"
        return "Receiving"


class Roster:
    """The stations the server tells us about. Thread safe; events come from the link thread."""

    def __init__(self):
        self.lock = threading.Lock()
        self.stations = {}
        self.chat = []

    def clear(self):
        with self.lock:
            self.stations.clear()

    def snapshot(self):
        with self.lock:
            return list(self.stations.values())

    def event(self, name, d):
        """Apply one server event. Returns the name of what changed, or None."""
        if name == "bulk_update":
            changed = None
            for item in d if isinstance(d, list) else []:
                if isinstance(item, list) and item and isinstance(item[0], str):
                    changed = self.event(item[0], item[1] if len(item) > 1 else None) or changed
            return changed
        if not isinstance(d, dict):
            return None
        with self.lock:
            sid = d.get("sid")
            if name == "new_connection" and sid:
                s = self.stations.setdefault(sid, Station(sid))
                s.callsign = d.get("callsign", "")
                s.grid = d.get("grid_square", "")
                s.version = d.get("version", "")
                s.rx_only = bool(d.get("rx_only", False))
                s.connect_time = d.get("connect_time", "")
                s.last_update = d.get("last_update", "")
                return name
            if name == "remove_connection" and sid:
                self.stations.pop(sid, None)
                return name
            if sid and sid in self.stations:
                s = self.stations[sid]
                s.last_update = d.get("last_update", s.last_update)
                s.seen = time.time()
                if name == "freq_change" and isinstance(d.get("freq"), int):
                    s.freq = d["freq"]
                    return name
                if name == "tx_report":
                    s.mode = d.get("mode", s.mode)
                    s.transmitting = bool(d.get("transmitting", False))
                    if d.get("last_tx"):
                        s.last_tx = d["last_tx"]
                    return name
                if name == "message_update":
                    s.message = d.get("message", "")
                    return name
                if name == "rx_report":
                    s.rx_call = d.get("callsign", "")
                    s.rx_mode = d.get("mode", "")
                    snr = d.get("snr")
                    s.rx_snr = snr if isinstance(snr, (int, float)) and not isinstance(snr, bool) else None
                    if not s.rx_call and not s.rx_mode:
                        # The server sends this (with an snr of 0) to everyone when a station changes frequency or connects: it
                        # means "nothing heard now", not an SNR of 0. freedv-gui blanks the call, mode and SNR for it
                        # (FreeDVReporterDataModel::onReceiveUpdateFn_); so do we.
                        s.rx_snr = None
                    return name
        return None


# ---------------------------------------------------------------------- servers

DEFAULT_SERVERS = {
    # key: the RADE mode piHPSDR has selected -> where it is reported
    "v1": {"label": "RADE V1", "mode": "RADEV1", "host": "qso.freedv.org", "port": 443, "secure": True,
           "web": "https://qso.freedv.org/"},
    # RADE V2 has its own reporter: freedv-gui's ms-rade-v2 branch hard-codes this host (GUI/main.cpp),
    # TLS on 443 like the other. Settings can change it; empty means nothing is sent for V2.
    "v2": {"label": "RADE V2", "mode": "RADEV2", "host": "reporter-radev2.k6aq.net", "port": 443, "secure": True,
           "web": "https://reporter-radev2.k6aq.net/"},
}
WAVE_KEY = {1: "v1", 2: "v2"}
VIEWS = ("v1", "v2", "all")          # what the list shows: V1 only, V2 only, both


def parse_server(text):
    """'host', 'host:port', 'https://host[:port]/', 'http://host:port' -> (host, port, secure)."""
    t = (text or "").strip()
    secure = True
    m = re.match(r"^(https?|wss?)://", t, re.I)
    if m:
        secure = m.group(1).lower() in ("https", "wss")
        t = t[m.end():]
    t = t.split("/")[0]
    host, _, port = t.partition(":")
    try:
        p = int(port) if port else (443 if secure else 80)
    except ValueError:
        p = 443 if secure else 80
    return host, p, secure


# ------------------------------------------------------------------------- link

class Link:
    """One connection to one server, either watching or reporting; reconnects by itself.

    set_target() says what is wanted; the connection is changed only when it
    has to be (another server, or watching <-> reporting), otherwise the
    frequency / mode / transmitting / message updates are sent as they change,
    as freedv-gui does.
    """

    def __init__(self, roster, on_event, on_state, software, client_factory=SioClient):
        self.roster, self.on_event, self.on_state = roster, on_event, on_state
        self.software = software
        self.factory = client_factory
        self.client = None
        self.key = None            # (server key, role, callsign, grid)
        self.fully = False         # connection_successful seen
        self.sent = {}             # what the server has been told
        self.want = {}
        self.state = "idle"
        self.lock = threading.RLock()

    # ---- public
    def set_target(self, server, role, callsign="", grid="", freq=0, mode="", tx=False, message=""):
        """server: a DEFAULT_SERVERS-style dict with host/port/secure, or None to be disconnected."""
        old, reopen = None, False
        with self.lock:
            if server is None or not server.get("host"):
                old = self._detach()
            else:
                key = (server["host"], server["port"], server["secure"], role,
                       callsign if role == "report" else "", grid if role == "report" else "")
                self.want = {"freq": int(freq), "mode": mode, "tx": bool(tx), "message": message}
                if key != self.key:
                    old = self._detach()
                    reopen = True
                else:
                    self._push()
        # The old connection is stopped without our lock held: its thread reports back through it, and
        # waiting for that thread while holding the lock made every close wait out the join timeout.
        self._finish(old)
        if reopen:
            with self.lock:
                self._open(server, role, callsign, grid)
                self.key = key

    def emit(self, event, data):
        with self.lock:
            if self.client is not None and self.fully:
                self.client.emit(event, data)
                return True
        return False

    def report_rx(self, callsign, mode, snr):
        """freedv-gui's rx_report: callsign (may be empty: something is being received but nobody is
        identified yet), mode, SNR as it rounds it, int(snr + 0.5). Only while reporting."""
        if not isinstance(snr, (int, float)) or isinstance(snr, bool):
            return False                   # no lock, no SNR: nothing is sent (0 is a real SNR; "none" is not 0)
        with self.lock:
            if (self.client is not None and self.fully and getattr(self, "role", "view") == "report" and mode):
                self.client.emit("rx_report", {"callsign": callsign or "", "mode": mode, "snr": int(snr + 0.5)})
                return True
        return False

    def pulse_tx(self, mode):
        """A transmission that began and ended between two looks: tell the server both, in order, so that its
        'last TX' is right (freedv-gui reports every PTT change)."""
        with self.lock:
            if self.client is not None and self.fully and getattr(self, "role", "view") == "report" and mode:
                self.client.emit("tx_report", {"mode": mode, "transmitting": True})
                self.client.emit("tx_report", {"mode": mode, "transmitting": False})
                self.sent["mode"], self.sent["tx"] = mode, False
                return True
        return False

    def close(self):
        with self.lock:
            old = self._detach()
        self._finish(old)

    # ---- internal
    def _open(self, server, role, callsign, grid):
        auth = {"protocol_version": 2}
        if role == "report":
            auth.update({"role": "report", "callsign": callsign, "grid_square": grid,
                         "version": self.software, "rx_only": False,
                         "os": platform.system().lower() or "linux"})
        else:
            auth["role"] = "view"
        self.roster.clear()
        self.fully = False
        self.sent = {}
        self.role = role
        self.client = self.factory(server["host"], server["port"], server["secure"], auth,
                                   self._event, self._state)
        self.client.start()

    def _detach(self):
        """Forget the connection (lock held); the caller stops it with _finish(), lock released."""
        c, self.client = self.client, None
        self.key = None
        self.fully = False
        self.sent = {}
        return c

    def _finish(self, c):
        if c is not None:
            c.stop()
            self.roster.clear()
            self.on_state("idle", "")

    def _state(self, st, msg):
        with self.lock:
            if st in ("disconnected", "error", "connecting"):
                self.fully = False
                self.sent = {}
            self.state = st
        self.on_state(st, msg)

    def _event(self, name, data):
        if name == "connection_successful":
            with self.lock:
                self.fully = True
                self.sent = {}
                self._push()
        self.roster.event(name, data)
        self.on_event(name, data)

    def _push(self):
        """Tell the server what changed. Only when reporting, only when it is ready for it."""
        if self.client is None or not self.fully or getattr(self, "role", "view") != "report":
            return
        w, s = self.want, self.sent
        if w["freq"] and w["freq"] != s.get("freq"):
            self.client.emit("freq_change", {"freq": w["freq"]})
            s["freq"] = w["freq"]
        if w["mode"] and (w["mode"], w["tx"]) != (s.get("mode"), s.get("tx")):
            self.client.emit("tx_report", {"mode": w["mode"], "transmitting": w["tx"]})
            s["mode"], s["tx"] = w["mode"], w["tx"]
        if w["message"] != s.get("message", ""):
            self.client.emit("message_update", {"message": w["message"]})
            s["message"] = w["message"]


# ------------------------------------------------------------------ pihpsdr link

def state_path():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "pihpsdr-rade", "state.json")


def config_path():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "pihpsdr-rade", "reporter.json")


_last_good = None


def read_pihpsdr_state(path=None, now=None):
    """What piHPSDR last wrote, or None. 'linked' is true while it is alive.

    piHPSDR rewrites the file at least every two seconds, and once more with
    running=0 when it exits; a file older than 8 s is a piHPSDR that has gone.
    """
    global _last_good
    try:
        with open(path or state_path()) as f:
            d = json.load(f)
        _last_good = d
    except OSError:
        return None
    except ValueError:
        # caught mid-write: use the last good copy; its own time stamp still decides whether it is stale
        if _last_good is None:
            return None
        d = dict(_last_good)
    now = time.time() if now is None else now
    d["linked"] = bool(d.get("running")) and (now - float(d.get("updated", 0))) < 8.0
    return d


class PeriodicSnr:
    """The RADE SNR, sent every few seconds while a signal is in sync - what freedv-gui does.

    In freedv-gui's timer (main.cpp, "Special case for RADE"), while the modem is in sync and no new
    callsign has just been decoded, it sends an rx_report with the callsign last reported in this
    reception, or an empty one if none has been decoded yet, the mode, and the SNR - so that the
    reporter shows that something is being received even before anyone is identified. The callsign is
    forgotten when sync has been lost for 10 s (its "Stats / ResetTime"). No callsign is needed.
    """

    def __init__(self, interval=5.0, reset_s=10.0):
        self.interval, self.reset_s = interval, reset_s
        self.enabled = True
        self.call = ""               # last call decoded in this reception
        self.lost_since = None
        self.last_sent = 0.0
        self.last = None             # (call, snr, time) of the last report sent

    def heard(self, call, now):
        """A call was decoded from an end-of-over frame (and reported separately)."""
        self.call = call
        self.last_sent = now         # that report has just gone: the next one a full interval later

    def reset(self):
        self.call = ""
        self.lost_since = None
        self.last = None

    def step(self, p, now):
        """-> (send, call, snr, status). send is True when a report is due."""
        if not self.enabled:
            return False, "", 0.0, "off"
        if not p or not p.get("linked"):
            return False, "", 0.0, "waiting: piHPSDR is not running"
        if p.get("rx_sync"):
            if self.lost_since is not None and now - self.lost_since >= self.reset_s:
                self.call = ""       # back in sync after a long gap: a new reception, as freedv-gui sees it
            self.lost_since = None
        elif self.lost_since is None:
            self.lost_since = now
        if p.get("tx"):
            return False, "", 0.0, "paused while transmitting"
        if not p.get("rx_sync"):
            return False, "", 0.0, "waiting for a signal"
        if not isinstance(p.get("rx_snr"), (int, float)):
            return False, "", 0.0, "in sync but no SNR yet: nothing sent"
        what = "call %s" % self.call if self.call else "no call decoded yet, sent blank"
        status = "sending the SNR every %g s (%s)" % (self.interval, what)
        if now - self.last_sent < self.interval:
            return False, "", 0.0, status
        self.last_sent = now
        snr = float(p["rx_snr"])
        self.last = (self.call, snr, now)
        return True, self.call, snr, status


def rx_reports_due(p, last_seq, mycall, now=None, max_age=60.0):
    """Every call piHPSDR has decoded from an end-of-over frame since last_seq, oldest first, as
    [(call, snr, seq)]. Each decode is reported, even a call that was just reported (freedv-gui reports
    a call every time one is decoded). last_seq is the count of calls already dealt with (None before the
    first look: whatever is there is old news). Stale, empty and our own calls are left out."""
    if not p or not p.get("linked") or last_seq is None:
        return []
    now = time.time() if now is None else now
    entries = p.get("heard")
    if not isinstance(entries, list):          # an older piHPSDR: just the latest
        entries = [{"seq": p.get("heard_seq", 0), "call": p.get("heard_call"), "snr": p.get("heard_snr"),
                    "t": p.get("heard_t", 0)}]
    out = []
    for e in sorted(entries, key=lambda x: int(x.get("seq", 0))):
        seq = int(e.get("seq", 0))
        call = (e.get("call") or "").strip().upper()
        if seq <= last_seq or not call or call == (mycall or "").strip().upper():
            continue
        if now - float(e.get("t", 0)) > max_age:
            continue
        if not isinstance(e.get("snr"), (int, float)):
            continue                       # decoded with no lock behind it: there is no SNR to report, so no report
        out.append((call, float(e["snr"]), seq))
    return out


def load_config(path=None):
    try:
        with open(path or config_path()) as f:
            c = json.load(f)
    except (OSError, ValueError):
        c = {}
    servers = json.loads(json.dumps(DEFAULT_SERVERS))
    for k, v in (c.get("servers") or {}).items():
        if k in servers and isinstance(v, dict):
            for f in ("host", "port", "secure"):
                if f in v:
                    servers[k][f] = v[f]
    c["servers"] = servers
    v = c.get("view")
    c["view"] = "v2" if v == "v3" else (v if v in VIEWS else "all")     # "v3" was V2's name before it had an address
    # reporting is never remembered: every start is "off"
    c.pop("reporting", None)
    return c


def save_config(c, path=None):
    path = path or config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out = {k: v for k, v in c.items() if k != "reporting"}
    out["servers"] = {k: {f: v[f] for f in ("host", "port", "secure")} for k, v in c["servers"].items()}
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(out, f, indent=1)
    os.replace(tmp, path)


def plan(cfg, pihpsdr, reporting, callsign, grid, view):
    """Which servers to be connected to, and in what role.

    view is "v1", "v2" or "all": what the list shows, so those servers are watched. If we are present
    (decide() says so) the server of the RADE mode piHPSDR has selected is joined as a reporter - added
    if it is not being watched - and it is the only one that is ever sent anything.

    Returns (targets, why, report_key, view_keys); targets is {key: (server, role, mode, freq_hz, tx)}.
    """
    srv, role, mode, freq, tx, why = decide(cfg, pihpsdr, reporting, callsign, grid, "v1")
    rkey = next((k for k, v in cfg["servers"].items() if v is srv), None) if role == "report" else None
    vkeys = ["v1", "v2"] if view == "all" else [view if view in cfg["servers"] else "v1"]
    targets = {}
    for k in vkeys:
        if cfg["servers"][k].get("host"):
            targets[k] = (cfg["servers"][k], "view", "", 0, False)
    if rkey:
        targets[rkey] = (srv, "report", mode, freq, tx)
    return targets, why, rkey, vkeys


def decide(cfg, pihpsdr, reporting, callsign, grid, view_key):
    """What the link should be doing, given everything.

    Returns (server dict or None, role, mode, freq_hz, tx, why). Presence is only
    ever reported for the RADE mode piHPSDR has selected, while piHPSDR is
    running; otherwise the reporter just watches (view_key's server).
    """
    linked = bool(pihpsdr and pihpsdr.get("linked"))
    wave = int(pihpsdr.get("waveform", 0)) if linked else 0
    key = WAVE_KEY.get(wave)
    if reporting:
        if not linked:
            why = "not reporting: piHPSDR is not running"
        elif key is None:
            why = "not reporting: RADE is off in piHPSDR"
        elif not cfg["servers"][key]["host"]:
            why = "not reporting: no server set for %s (Settings)" % cfg["servers"][key]["label"]
        elif not callsign:
            why = "not reporting: no callsign"
        elif not grid_valid(grid):
            why = "not reporting: grid square missing or not valid"
        else:
            srv = cfg["servers"][key]
            return (srv, "report", srv["mode"], int(pihpsdr.get("dial_hz", 0)),
                    bool(pihpsdr.get("tx")), "reporting %s" % srv["mode"])
    else:
        why = "reporting is off"
    # watching: follow the active mode if there is one, else the chosen server
    vk = key or view_key
    srv = cfg["servers"].get(vk)
    return (srv if srv and srv.get("host") else None, "view", "", 0, False, why)

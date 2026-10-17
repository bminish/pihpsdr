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
                 "seen", "rx_time")

    def __init__(self, sid):
        self.sid = sid
        self.callsign = self.grid = self.version = self.mode = self.message = ""
        self.rx_only = False
        self.connect_time = self.last_tx = self.last_update = ""
        self.freq = 0
        self.transmitting = False
        self.rx_call = self.rx_mode = ""
        self.rx_snr = None
        self.rx_time = 0.0           # when the last rx_report with something in it arrived (our clock)
        self.seen = time.time()

    @property
    def status(self):
        if self.transmitting:
            return "Transmitting"
        if self.rx_only:
            return "Receive Only"
        return "Receiving"

    def receiving_now(self, now):
        """Shown as receiving, as freedv-gui does: not transmitting, not receive-only, and an rx_report with an SNR
        arrived recently (20 s if it named a call, 5 s if not)."""
        return (self.status == "Receiving" and self.rx_snr is not None and self.rx_time > 0 and
                now - self.rx_time <= (RX_COLOUR_WITH_CALL_S if self.rx_call else RX_COLOUR_NO_CALL_S))


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
                    s.rx_time = time.time()
                    if not s.rx_call and not s.rx_mode:
                        s.rx_time = 0.0
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
VIEWS = ("follow", "v1", "v2", "all")   # what the list shows: the radio's RADE mode, V1 only, V2 only, both
RX_COLOUR_WITH_CALL_S = 20.0           # freedv-gui: a station shows as receiving for 20 s after an rx_report with a call ...
RX_COLOUR_NO_CALL_S = 5.0              # ... and for 5 s after one without


def resolve_view(view, pihpsdr):
    """"follow" shows the reporter of the RADE mode piHPSDR has selected (both while RADE is off or piHPSDR is not running)."""
    if view != "follow":
        return view
    if pihpsdr and pihpsdr.get("linked"):
        return {1: "v1", 2: "v2"}.get(int(pihpsdr.get("waveform", 0) or 0), "all")
    return "all"


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

    MIN_GAP_S = 5.0            # never two events of one kind within this long: remote servers are not to be burdened
    RX_GAP_S = 1.0             # a reception report whose SNR has changed: up to once a second
    RX_KEEPALIVE_S = 20.0      # an unchanged SNR, while genuinely in sync: repeated this often, so the station stays shown as receiving
    STABLE_S = 5.0             # the dial must stand still this long (or a signal be in sync) before the frequency is reported

    def __init__(self, roster, on_event, on_state, software, client_factory=SioClient, clock=time.time):
        self.clock = clock
        self.rx_calls = []         # calls decoded, waiting for their turn (oldest first)
        self.rx_blank = None       # the latest live SNR report, (callsign, mode, snr), waiting for its turn
        self.pulse = None          # mode of a transmission that began and ended between two looks
        self.freq_seen = None
        self.freq_since = 0.0
        self.live = False
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
    def set_target(self, server, role, callsign="", grid="", freq=0, mode="", tx=False, message="", live=False):
        """server: a DEFAULT_SERVERS-style dict with host/port/secure, or None to be disconnected.
        live: a signal is in sync with an SNR - new information that settles the frequency at once."""
        old, reopen = None, False
        with self.lock:
            if int(freq) != self.freq_seen:           # tuning: the clock for "stable" starts again
                self.freq_seen, self.freq_since = int(freq), self.clock()
            self.live = bool(live)
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
                item = (callsign or "", mode, int(snr + 0.5))
                if item[0]:
                    if item not in self.rx_calls:
                        self.rx_calls = (self.rx_calls + [item])[-4:]
                else:
                    self.rx_blank = item        # only the newest live report is worth sending
                self._push()
                return True
        return False

    def pulse_tx(self, mode):
        """A transmission that began and ended between two looks: tell the server both, in order, so that its
        'last TX' is right (freedv-gui reports every PTT change)."""
        with self.lock:
            if self.client is not None and self.fully and getattr(self, "role", "view") == "report" and mode:
                self.pulse = mode               # sent as on, then off, when its turn comes
                self._push()
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
        self.rx_calls, self.rx_blank, self.pulse = [], None, None
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
        self.rx_calls, self.rx_blank, self.pulse = [], None, None
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
        """Tell the server what is new - and nothing else. Only when reporting and the server is ready. At most one event of each
        kind per MIN_GAP_S (what is waiting is merged, so the latest state goes out when the time comes), except reception reports:
        a changed SNR may go every RX_GAP_S, an unchanged one only as a keep-alive every RX_KEEPALIVE_S while in sync; nothing that repeats what
        the server already has; while the dial is being tuned nothing at all, until it has stood still for STABLE_S or a signal is
        in sync with an SNR."""
        if self.client is None or not self.fully or getattr(self, "role", "view") != "report":
            return
        now = self.clock()
        w, s = self.want, self.sent
        t = s.setdefault("_t", {})
        def due(kind):
            return now - t.get(kind, -1e9) >= self.MIN_GAP_S
        def sent(kind):
            t[kind] = now
        settled = self.live or now - self.freq_since >= self.STABLE_S
        if w["freq"] and w["freq"] != s.get("freq") and settled and due("freq"):
            self.client.emit("freq_change", {"freq": w["freq"]})
            s["freq"] = w["freq"]
            sent("freq")
        if not settled:
            return                       # tuning: no transmit, receive or message traffic either
        if self.pulse and due("tx"):
            self.client.emit("tx_report", {"mode": self.pulse, "transmitting": True})
            self.client.emit("tx_report", {"mode": self.pulse, "transmitting": False})
            s["mode"], s["tx"] = self.pulse, False
            self.pulse = None
            sent("tx")
        elif w["mode"] and (w["mode"], w["tx"]) != (s.get("mode"), s.get("tx")) and due("tx"):
            self.client.emit("tx_report", {"mode": w["mode"], "transmitting": w["tx"]})
            s["mode"], s["tx"] = w["mode"], w["tx"]
            sent("tx")
            self.pulse = None
        if w["message"] != s.get("message", "") and due("message"):
            self.client.emit("message_update", {"message": w["message"]})
            s["message"] = w["message"]
            sent("message")
        if w["freq"] and s.get("freq") != w["freq"]:
            return                       # the server does not have our frequency yet: its reception reports would mean nothing
        item = self.rx_calls[0] if self.rx_calls else self.rx_blank
        if item is not None:
            payload = {"callsign": item[0], "mode": item[1], "snr": item[2]}
            since = now - t.get("rx", -1e9)
            if payload == s.get("rx"):               # nothing new; only a keep-alive while really in sync with an SNR
                if item[0]:
                    self.rx_calls.pop(0)
                else:
                    self.rx_blank = None
                if self.live and since >= self.RX_KEEPALIVE_S:
                    self.client.emit("rx_report", payload)
                    sent("rx")
            elif since >= self.RX_GAP_S:             # changed: up to once a second
                if item[0]:
                    self.rx_calls.pop(0)
                else:
                    self.rx_blank = None
                self.client.emit("rx_report", payload)
                s["rx"] = payload
                sent("rx")


# ------------------------------------------------------------------ pihpsdr link

def state_path():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "pihpsdr-rade", "state.json")


def command_path():
    return os.path.join(os.path.dirname(state_path()), "command.json")


def send_command(wave=None, dial_hz=None, path=None):
    """Asks piHPSDR to select RADE V1/V2 (wave 1 or 2) and/or tune its dial (Hz). The file is written atomically; its 'seq' is
    the time in ms so that every request is new, and piHPSDR acts on a request once, only if it is newer than the last one it
    saw and less than 10 s old. Returns the seq (piHPSDR answers in state.json with cmd_seq and cmd_err)."""
    path = path or command_path()
    seq = int(time.time() * 1000)
    cmd = {"seq": seq}
    if wave in (1, 2):
        cmd["wave"] = wave
    if dial_hz is not None:
        cmd["dial_hz"] = int(dial_hz)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cmd, f)
    os.replace(tmp, path)
    return seq


# The usual RADE frequencies, from freedv-gui's default Reporting frequency list (the same in the V1 and the V2 trees today).
# Kept per mode in the config (cfg["freqs"]["v1"] / ["v2"], Hz) so that they can differ.
DEFAULT_FREQS_HZ = [1870000, 3625000, 3643000, 3693000, 3697000, 3803000, 5368500, 5403500, 7177000, 7197000,
                    14236000, 14240000, 18118000, 21313000, 24933000, 28330000, 28720000, 10489640000]


def clean_freqs(lst):
    """Hz values, whole multiples of 100 Hz (the 0.1 kHz the box allows), each once, lowest first."""
    out = set()
    for v in lst or []:
        if isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and v > 0:
            out.add(int(round(v / 100.0)) * 100)
    return sorted(out)


def parse_khz(text):
    """'14236.0' -> 14236000 (Hz), or None. One decimal place at most, 0 < f <= 999999.9 kHz."""
    t = (text or "").strip().replace(",", ".")
    if not re.fullmatch(r"\d{1,6}(\.\d?)?", t):
        return None
    hz = int(round(float(t) * 10)) * 100
    return hz if hz > 0 else None


def fmt_khz(hz):
    return "%.1f" % (hz / 1000.0)


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
        if p.get("waveform") == 2 and isinstance(p.get("rx_call"), str):
            # V2: piHPSDR knows which reception a lock belongs to (a drop-out of the same sender keeps the call, another
            # sender starts afresh), so its call is used and the 10 s rule below is not
            self.call = p["rx_call"]
            self.lost_since = None
        elif p.get("rx_sync"):
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
        status = "sending the SNR when it changes, at most every %g s (%s)" % (self.interval, what)
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
    c["view"] = "v2" if v == "v3" else (v if v in VIEWS else "follow")  # "v3" was V2's name before it had an address
    if c.get("view_v") != 2:        # Follow became the default: take it up once, then whatever is chosen is remembered
        c["view"], c["view_v"] = "follow", 2
    # reporting is never remembered: every start is "off"
    c.pop("reporting", None)
    f = c.get("freqs") if isinstance(c.get("freqs"), dict) else {}
    seeded = not all(k in f for k in ("v1", "v2"))
    c["freqs"] = {k: clean_freqs(f[k]) if k in f else list(DEFAULT_FREQS_HZ) for k in ("v1", "v2")}
    if seeded:
        try:
            save_config(c, path)          # the lists are put in the config file the first time
        except OSError:
            pass
    return c


def save_config(c, path=None):
    path = path or config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out = {k: v for k, v in c.items() if k != "reporting"}
    out["freqs"] = {k: clean_freqs(c.get("freqs", {}).get(k)) for k in ("v1", "v2")}
    out["servers"] = {k: {f: v[f] for f in ("host", "port", "secure")} for k, v in c["servers"].items()}
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(out, f, indent=1)
    os.replace(tmp, path)


FREQ_MIN_HZ, FREQ_MAX_HZ = 100_000, 450_000_000      # what can be a dial frequency worth reporting


def valid_freq(hz):
    """A frequency that may be reported: a real number of Hz inside the radio's range (never 0, negative, NaN or a string)."""
    return (isinstance(hz, (int, float)) and not isinstance(hz, bool) and hz == hz and FREQ_MIN_HZ <= hz <= FREQ_MAX_HZ)


def report_gate(pihpsdr):
    """May anything be reported at all? -> (True, "") or (False, why). Needs piHPSDR running and telling us now (its state file is
    fresh), and a valid dial frequency. When this is False reporting is switched off, not just paused: a station must never be listed
    with a bad frequency or a state that is not known to be true."""
    if not pihpsdr or not pihpsdr.get("linked"):
        return False, "piHPSDR is not running (no current state from it)"
    if not valid_freq(pihpsdr.get("dial_hz")):
        return False, "no valid frequency from piHPSDR"
    return True, ""


def plan(cfg, pihpsdr, reporting, callsign, grid, view):
    """Which servers to be connected to, and in what role.

    view is "v1", "v2" or "all": what the list shows, so those servers are watched. If we are present
    (decide() says so) the server of the RADE mode piHPSDR has selected is joined as a reporter - added
    if it is not being watched - and it is the only one that is ever sent anything.

    Returns (targets, why, report_key, view_keys); targets is {key: (server, role, mode, freq_hz, tx)}.
    """
    view = resolve_view(view, pihpsdr)
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
    ok_gate, why_gate = report_gate(pihpsdr)
    if reporting:
        if not ok_gate:
            why = "not reporting: " + why_gate
        elif not linked:
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

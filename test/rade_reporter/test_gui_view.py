#!/usr/bin/env python3
"""The Show choice in the Reporter window: V1 only, V2 only, or both - and reports go only to the reporter for
the RADE mode piHPSDR has selected, whatever is shown. Mock servers and a scratch config; no window is shown."""
import json, os, sys, tempfile, time

if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
    print("no display: skipped"); sys.exit(0)
scratch = tempfile.mkdtemp()
os.environ["XDG_STATE_HOME"] = scratch + "/state"
os.environ["XDG_CONFIG_HOME"] = scratch + "/config"
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "rade-reporter")); sys.path.insert(0, HERE)
import gi
gi.require_version("Gtk", "3.0"); gi.require_version("Gdk", "3.0")
from gi.repository import Gtk, GLib
import core
from mock_server import MockServer

fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

def pump(n=0.3):
    t = time.time()
    while time.time() - t < n:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        time.sleep(0.01)

def wait(f, to=5.0):
    t = time.time()
    while time.time() - t < to:
        pump(0.05)
        if f():
            return True
    return False

s1, s2 = MockServer().start(), MockServer().start()
cfg = core.load_config()
cfg["servers"]["v1"].update(host="127.0.0.1", port=s1.port, secure=False)
cfg["servers"]["v2"].update(host="127.0.0.1", port=s2.port, secure=False)
cfg["callsign"], cfg["grid"] = "EI6IZ", "IO53hu"
core.save_config(cfg)
os.makedirs(os.path.dirname(core.state_path()), exist_ok=True)
def state(wave):
    json.dump({"running": 1, "updated": time.time(), "waveform": wave, "dial_hz": 14250000, "tx": 0, "call": "EI6IZ",
               "version": "t"}, open(core.state_path(), "w"))
state(2)
import rade_reporter as rr
app = rr.App(show=False)
app._ident_since = 0

def feed(key, sid, call):
    app.rosters[key].event("new_connection", {"sid": sid, "last_update": "2026-10-15T20:00:00+00:00",
                                              "callsign": call, "grid_square": "FN31", "version": "x",
                                              "rx_only": False, "connect_time": "x"})
def rows():
    return sorted(r[rr.C_CALL] + "@" + r[rr.C_SID] for r in app.store)

def show(v):
    app.view.set_active_id(v)
    state(2); app._tick(); app.dirty = True; app._fill(); pump(0.2)

print("which reporters are joined, by Show")
show("v1")
check(wait(lambda: s1.clients and s1.clients[0]["auth"] is not None), "V1 only: joined V1")
check(not s2.clients, "V1 only: not connected to V2")
check(s1.clients[0]["auth"].get("role") == "view", "V1 only, not reporting: as a viewer")
show("all")
check(wait(lambda: s2.clients and s2.clients[0]["auth"] is not None), "all: V2 joined as well")
check(s2.clients[0]["auth"].get("role") == "view", "all, not reporting: both as viewers")
show("v2")
check(wait(lambda: s1.clients[0]["closed"] or s1.clients[0]["left"]), "V2 only: left V1")

print("the list")
for lk in app.links.values():
    lk.close()
app.rosters["v1"].clear(); app.rosters["v2"].clear()
feed("v1", "same", "K1ONE"); feed("v2", "same", "K2TWO"); feed("v2", "only2", "K3TWO")
# the same sid on both reporters must not collide; links closed above, so set the view without reconnecting
app.view_keys = ["v1"]; app._fill()
check(rows() == ["K1ONE@v1:same"], "V1 only: %s" % rows())
app.view_keys = ["v2"]; app._fill()
check(rows() == ["K2TWO@v2:same", "K3TWO@v2:only2"], "V2 only: %s" % rows())
app.view_keys = ["v1", "v2"]; app._fill()
check(len(rows()) == 3 and "K1ONE@v1:same" in rows() and "K2TWO@v2:same" in rows(), "all: both, the same sid twice: %s" % rows())

print("reports go to the mode's reporter only")
app.rosters["v1"].clear(); app.rosters["v2"].clear()
app.rep_chk.set_active(True)
show("all"); app._ident_since = 0
for _ in range(10):
    state(1); app._tick(); pump(0.1)
check(wait(lambda: s1.clients and s1.clients[-1]["auth"] and s1.clients[-1]["auth"].get("role") == "report"), "radio on V1: reporting on V1")
check(s2.clients and s2.clients[-1]["auth"].get("role") == "view", "radio on V1: V2 stays a viewer")
state(2); app._tick(); pump(0.2); state(2); app._tick()
check(wait(lambda: s2.clients[-1]["auth"].get("role") == "report"), "radio on V2: reporting on V2")
check(wait(lambda: s1.clients[-1]["auth"].get("role") == "view" or s1.clients[-1]["closed"] or s1.clients[-1]["left"]),
      "radio on V2: no longer reported on V1")
show("v1"); state(2); app._tick(); pump(0.3)
check(app.report_key == "v2" and wait(lambda: s2.clients[-1]["auth"].get("role") == "report"),
      "showing V1 only with the radio on V2: reports still go to V2")
app._quit = lambda *a: None
for lk in app.links.values():
    lk.close()
s1.stop(); s2.stop()
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

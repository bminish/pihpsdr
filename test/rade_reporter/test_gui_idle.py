#!/usr/bin/env python3
"""The Reporter window must be quiet when nothing is happening: its update runs about twice a
second (it once ran flat out, as an idle callback that returned True, and rebuilt the whole list each
time), and it must close promptly. Mock server and a scratch config only; needs a display."""
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

mock = MockServer().start()
cfg = core.load_config()
cfg["servers"]["v1"].update(host="127.0.0.1", port=mock.port, secure=False)
cfg["servers"]["v3"].update(host="127.0.0.1", port=mock.port, secure=False)
core.save_config(cfg)
assert core.config_path().startswith(scratch)
os.makedirs(os.path.dirname(core.state_path()), exist_ok=True)
json.dump({"running": 1, "updated": time.time(), "waveform": 1, "dial_hz": 7184000, "tx": 0, "call": "EI6IZ"},
          open(core.state_path(), "w"))
import rade_reporter as rr
ticks = [0]
_orig_tick = rr.App._tick
def _counted(self):
    ticks[0] += 1
    return _orig_tick(self)
rr.App._tick = _counted
app = rr.App()
for i in range(60):
    app.roster.event("new_connection", {"sid": "s%d" % i, "last_update": "2026-10-15T20:00:00+00:00",
                                        "callsign": "K%dABC" % i, "grid_square": "FN31", "version": "x",
                                        "rx_only": False, "connect_time": "x"})
GLib.timeout_add(1000, lambda: (json.dump({"running": 1, "updated": time.time(), "waveform": 1, "dial_hz": 7184000,
                                           "tx": 0, "call": "EI6IZ"}, open(core.state_path(), "w")), True)[1])
GLib.timeout_add(4000, lambda: (Gtk.main_quit(), False)[1])
Gtk.main()
ticks_ok = 4 <= ticks[0] <= 16
print("  %-4s update ran %d times in 4 s (about 8 expected)" % ("ok" if ticks_ok else "FAIL", ticks[0]))
t = time.time()
app._quit()
closed = time.time() - t
print("  %-4s closes in %.2f s" % ("ok" if closed < 1.5 else "FAIL", closed))
mock.stop()
sys.exit(0 if ticks_ok and closed < 1.5 else 1)

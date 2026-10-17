#!/usr/bin/env python3
"""Request QSY, both ways, against freedv-gui's behaviour (FreeDVReporter::requestQSY / onFreeDVReporterQsyRequest_, the same in the
ms-rade-v2 branch): sent as the event "qsy_request" {dest_sid: str, message: str, frequency: unsigned int}, only when reporting (callsign and
grid set), to a station that is not ourselves and is not already on our frequency; received as {callsign: str, frequency: unsigned int,
message: str}, and anything else is dropped. Mock server, scratch config, no window shown."""
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
from gi.repository import Gtk
import core
from mock_server import MockServer

fails = 0
def check(c, what, info=""):
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
cfg["grid"] = "IO53hu"
core.save_config(cfg)
os.makedirs(os.path.dirname(core.state_path()), exist_ok=True)
def state(wave=1, dial=14236000, call="EI6IZ", tx=0):
    json.dump({"running": 1, "updated": time.time(), "waveform": wave, "dial_hz": dial, "tx": tx, "call": call, "version": "t"},
              open(core.state_path(), "w"))
state()
import rade_reporter as rr
app = rr.App(show=False)
app._ident_since = 0
shown = []
real_qsy = app._qsy_request
app._qsy_request = lambda d, key=None: shown.append((d, key))        # the dialog itself is not opened

def tick():
    state(); app._tick(); app.dirty = True; app._fill(); pump(0.2)
def select_row(sid_suffix):
    for r in app.sort:
        if r[rr.C_SID].endswith(":" + sid_suffix):
            app.tree.get_selection().select_path(r.path)
            return True
    return False

app.view.set_active_id("v1")
tick()
check(wait(lambda: s1.clients and s1.clients[0]["auth"] is not None), "connected as a viewer")
tick()
check(select_row("s1"), "the station VK5DGR (7.053 MHz) is in the list and selected")

print("sending")
app._on_qsy()
pump(0.3)
check(not [e for e in s1.clients[0]["events"] if e[0] == "qsy_request"], "not reporting: nothing is sent (freedv-gui: not valid for reporting)")
app.rep_chk.set_active(True); tick()
check(wait(lambda: s1.clients[-1]["auth"] and s1.clients[-1]["auth"].get("role") == "report"), "reporting: joined as a reporter")
tick(); select_row("s1")
cl = s1.clients[-1]
app._on_qsy()
check(wait(lambda: [e for e in cl["events"] if e[0] == "qsy_request"]), "reporting: qsy_request sent")
q = [e for e in cl["events"] if e[0] == "qsy_request"][0][1]
check(sorted(q) == ["dest_sid", "frequency", "message"], "with exactly the fields freedv-gui sends")
check(q["dest_sid"] == "s1" and q["message"] == "" and q["frequency"] == 14236000 and isinstance(q["frequency"], int) and not isinstance(q["frequency"], bool),
      "dest_sid of the station, empty message, our dial as an unsigned integer")

print("when freedv-gui would not offer it")
n0 = len([e for e in cl["events"] if e[0] == "qsy_request"])
app.rosters["v1"].event("new_connection", {"sid": "me", "last_update": "2026-10-15T20:00:00+00:00", "callsign": "EI6IZ", "grid_square": "IO53",
                                          "version": "x", "rx_only": False, "connect_time": "x"})
app.dirty = True; app._fill(); pump(0.1)
select_row("me"); app._on_qsy(); pump(0.4)
check(len([e for e in cl["events"] if e[0] == "qsy_request"]) == n0, "to our own callsign: not sent")
app.rosters["v1"].event("freq_change", {"sid": "s1", "last_update": "2026-10-15T20:10:00+00:00", "freq": 14236000})
app.dirty = True; app._fill(); pump(0.1)
select_row("s1"); app._on_qsy(); pump(0.4)
check(len([e for e in cl["events"] if e[0] == "qsy_request"]) == n0, "to a station already on our frequency: not sent")

print("receiving")
for payload, label, expect in (
        ({"callsign": "VK5DGR", "frequency": 7053000, "message": "hello"}, "as the server sends it", True),
        ({"callsign": "VK5DGR", "frequency": "7053000", "message": ""}, "frequency as a string (freedv-gui drops it)", False),
        ({"callsign": 5, "frequency": 7053000, "message": ""}, "callsign not a string (dropped)", False),
        ({"callsign": "VK5DGR", "frequency": 7053000}, "no message (dropped)", False),
        ({"callsign": "VK5DGR", "frequency": -5, "message": ""}, "negative frequency (not an unsigned integer: dropped)", False),
        ({"callsign": "VK5DGR", "frequency": 7053000.5, "message": ""}, "fractional frequency (dropped)", False)):
    shown.clear()
    s1.push(cl, "qsy_request", payload)
    pump(0.6)
    check(bool(shown) == expect, "%s: %s" % (label, "shown" if expect else "ignored"))
print("the dialog: Change Frequency")
dialogs = []
Gtk.MessageDialog.show = lambda self: dialogs.append(self)
def ask(key="v1", **kw):
    dialogs.clear()
    state(**kw); app._tick()
    real_qsy({"callsign": "VK5DGR", "frequency": 7053000, "message": "to 40m?"}, key)
    return dialogs[0] if dialogs else None
def press(dlg, resp):
    dlg.emit("response", resp)
def cmd():
    try:
        return json.load(open(core.command_path()))
    except OSError:
        return {}
d = ask()
check(d and d.get_property("text") == "VK5DGR has requested that you QSY to 7.0530 MHz." and "to 40m?" in d.get_property("secondary-text")
      and "RADE V1" in d.get_property("secondary-text") and "change to that frequency now" in d.get_property("secondary-text"),
      "names the caller and the frequency (MHz, 4 places), the message and the reporter, and offers the change")
labels = sorted(b.get_label() for b in d.get_action_area().get_children())
check(labels == ["Cancel", "Change Frequency"], "buttons: Cancel and Change Frequency (freedv-gui's)", str(labels))
before = cmd().get("seq")
press(d, Gtk.ResponseType.CANCEL)
check(cmd().get("seq") == before, "Cancel: nothing asked of piHPSDR")
d = ask("v2", wave=2)
press(d, Gtk.ResponseType.YES)
c = cmd()
check(c.get("wave") == 2 and c.get("dial_hz") == 7053000, "Change Frequency on a V2 request: RADE V2 and the frequency", str(c))
d = ask("v1", wave=2)
press(d, Gtk.ResponseType.YES)
c = cmd()
check(c.get("wave") == 1 and c.get("dial_hz") == 7053000, "the mode is the reporter the request came on (V1), whatever piHPSDR was in", str(c))
seq = c["seq"]
state(1, tx=1); app._tick()
d = ask("v1", tx=1)
check(d and [b.get_label() for b in d.get_action_area().get_children()] == ["OK"] and "change to that frequency" not in d.get_property("secondary-text"),
      "while transmitting: only tells, no offer")
press(d, Gtk.ResponseType.OK)
check(cmd()["seq"] == seq, "and asks nothing of piHPSDR")
try:
    os.remove(core.state_path())
except OSError:
    pass
app._tick()
dialogs.clear(); real_qsy({"callsign": "VK5DGR", "frequency": 7053000, "message": ""}, "v1")
check(dialogs and [b.get_label() for b in dialogs[0].get_action_area().get_children()] == ["OK"], "piHPSDR not running: only tells")
for dlg in dialogs:
    dlg.destroy()
print("all passed" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)

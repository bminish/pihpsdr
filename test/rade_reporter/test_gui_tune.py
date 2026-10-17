#!/usr/bin/env python3
"""Tuning piHPSDR from the Reporter: click a station's MHz, type a frequency in kHz, pick from the per-mode list, and the editor
for those lists. Nothing is sent to a server (no host is set); piHPSDR is played by this test through state.json / command.json."""
import json, os, sys, tempfile, time

if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
    print("no display: skipped"); sys.exit(0)
scratch = tempfile.mkdtemp()
os.environ["XDG_STATE_HOME"] = scratch + "/state"
os.environ["XDG_CONFIG_HOME"] = scratch + "/config"
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "rade-reporter"))
import gi
gi.require_version("Gtk", "3.0"); gi.require_version("Gdk", "3.0")
from gi.repository import Gtk, Gdk
import core

fails = 0
def check(c, what, info=""):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

print("the helpers")
check(core.parse_khz("14236.0") == 14236000 and core.parse_khz("7177") == 7177000, "kHz to Hz")
check(core.parse_khz("999999.9") == 999999900, "the largest, 999999.9 kHz")
check(core.parse_khz("1000000") is None and core.parse_khz("7.12") is None and core.parse_khz("") is None and core.parse_khz("0") is None,
      "too big, two decimals, empty, zero: refused")
check(core.clean_freqs([7177000, 1870000, 7177000, 14236049]) == [1870000, 7177000, 14236000], "sorted, no repeats, to 0.1 kHz")
c = core.load_config()
check(c["freqs"]["v1"] == c["freqs"]["v2"] == sorted(core.DEFAULT_FREQS_HZ), "both lists start as freedv-gui's, lowest first")
check(json.load(open(core.config_path()))["freqs"]["v2"][0] == 1870000, "and are put in the config file")

print("the command file")
os.makedirs(os.path.dirname(core.state_path()), exist_ok=True)
def state(wave, **kw):
    d = {"running": 1, "updated": time.time(), "waveform": wave, "dial_hz": 14250000, "tx": 0, "call": "EI6IZ", "version": "t"}
    d.update(kw)
    json.dump(d, open(core.state_path(), "w"))
state(1)
import rade_reporter as rr
app = rr.App(show=False)
app._tick()
def cmd():
    return json.load(open(core.command_path()))

app.tune_e.set_text("14236.0"); app._on_tune_go()
d = cmd()
check(d.get("dial_hz") == 14236000 and "wave" not in d, "typed kHz: the dial only")
check(app._cmd_seq == d["seq"], "the request is remembered, to match the answer")
def type_(text):                                    # one key at a time, at the end, as an operator does
    for ch in text:
        app.tune_e.insert_text(ch, len(app.tune_e.get_text()))
app.tune_e.set_text("")
type_("1.255")
check(app.tune_e.get_text() == "1.2", "a second decimal is not taken")
type_("x")
check(app.tune_e.get_text() == "1.2", "letters are not taken")
app.tune_e.set_text(""); type_("1234567")
check(app.tune_e.get_text() == "123456", "a seventh digit is not taken")
type_(".9")
check(app.tune_e.get_text() == "123456.9", "999999.9 style: six digits and one decimal")
app.tune_e.set_text(""); app._on_tune_go()
check("kHz" in app.tune_l.get_text() and cmd()["seq"] == d["seq"], "nonsense sends nothing")

state(1, cmd_seq=app._cmd_seq, cmd_err="not while transmitting"); app._tick()
check("not while transmitting" in app.tune_l.get_label(), "piHPSDR's refusal is shown")
app._tune(hz=7177000, what="x"); state(1, cmd_seq=app._cmd_seq, cmd_err=""); app._tick()
check(app.tune_l.get_text() == "Tuned", "and its yes")

print("the list for the mode in use")
check(app._flist_wave == 1 and app.flist.get_model().iter_n_children(None) == 18, "V1: 18 frequencies")
app.cfg["freqs"]["v2"] = [14236000, 3625000]
state(2); app._tick()
check(app._flist_wave == 2 and app._flist_hz == [3625000, 14236000], "V2: its own list, lowest first")
app.flist.set_active(1)
d = cmd()
check(d.get("dial_hz") == 14236000 and "wave" not in d, "a pick tunes the dial")

print("clicking a station's MHz")
class Ev:                                   # what the click handler reads
    button = 1; x = 0; y = 0; type = Gdk.EventType._2BUTTON_PRESS
class Ev1(Ev):
    type = Gdk.EventType.BUTTON_PRESS
from types import SimpleNamespace
app.tree.get_path_at_pos = lambda x, y: (Gtk.TreePath(0), app.cols["MHz"], 0, 0)
state(2); app._tick(); app._fill()
app.store.append(["K1ONE", "FN31", "", "", "v", "14.2360", "RADEV1", "", "", "", "", "", "", "a:1", None, -1.0, -1.0, 14.236, -99.0, 0.0])
app.store.append(["K2TWO", "FN31", "", "", "v", "7.1970", "FreeDV 700D", "", "", "", "", "", "", "a:2", None, -1.0, -1.0, 7.197, -99.0, 0.0])
app._refilter(True)
app.sort.set_sort_column_id(rr.C_CALL, Gtk.SortType.ASCENDING)
seq0 = cmd()["seq"]
app._on_tree_click(app.tree, Ev1)
check(cmd()["seq"] == seq0, "a single click on a station's MHz does not tune")
app._on_tree_click(app.tree, Ev)
d = cmd()
check(d.get("wave") == 1 and d.get("dial_hz") == 14236000, "a double click on a RADEV1 station's MHz: RADE V1 and its frequency")
app.tree.get_path_at_pos = lambda x, y: (Gtk.TreePath(1), app.cols["MHz"], 0, 0)
seq = d["seq"]; app._on_tree_click(app.tree, Ev)
check(cmd()["seq"] == seq and "700D" in app.tune_l.get_text(), "a station on another mode: nothing is sent, and it says why")
app.tree.get_path_at_pos = lambda x, y: (Gtk.TreePath(0), app.cols["Callsign"], 0, 0)
app._on_tree_click(app.tree, Ev)
check(cmd()["seq"] == seq, "other columns do not tune")

print("the editor")
big = app._freq_editor("t", list(core.DEFAULT_FREQS_HZ))
check(big.get_children()[1].get_child().get_model().iter_n_children(None) == 18, "the default list (with 10 GHz) opens in the editor")
hzs = [14236000, 3625000]
box = app._freq_editor("t", hzs)
tree = box.get_children()[1].get_child()
store = tree.get_model()
check([r[0] for r in store] == ["3625.0", "14236.0"], "shown lowest first")
entry = [w for w in box.get_children()[2].get_children() if isinstance(w, Gtk.Entry)][0]
entry.set_text("7177"); entry.emit("activate")
check(hzs == [3625000, 7177000, 14236000] and [r[0] for r in store] == ["3625.0", "7177.0", "14236.0"], "Add puts it in its place")
cell = tree.get_column(0).get_cells()[0]
cell.emit("edited", "2", "100.5")
check(hzs == [100500, 3625000, 7177000], "edit: changed")
check([r[0] for r in store] == ["100.5", "3625.0", "7177.0"], "edit: and sorted again")
cell.emit("edited", "0", "junk")
check(hzs == [100500, 3625000, 7177000], "edit: junk refused")
tree.get_selection().select_path(Gtk.TreePath(1))
box.get_children()[2].get_children()[2].emit("clicked")
check(hzs == [100500, 7177000], "Delete (the selected one)")

print("the window")
def buttons(w, out):
    if isinstance(w, Gtk.Button):
        out.append(w.get_label())
    if isinstance(w, Gtk.Container):
        for c in w.get_children():
            buttons(c, out)
    return out
labels = buttons(app, [])
check("Go" not in labels and "Columns / filters" not in labels, "no Go button; Columns / filters is now Filters", str(labels))
rows_ = [b for b in app.get_children()[0].get_children() if isinstance(b, Gtk.Box) and any(isinstance(c, Gtk.Button) and c.get_label() == "Settings" for c in b.get_children())]
order = [c.get_label() for c in rows_[0].get_children() if isinstance(c, Gtk.Button)]
order = [o for o in order if o in ("Request QSY", "Website", "Filters", "Settings")]
check(order == ["Request QSY", "Website", "Filters", "Settings"], "left to right: Request QSY, Website, Filters, Settings", str(order))
def tune_row():
    for b in app.get_children()[0].get_children():
        if isinstance(b, Gtk.Box) and any(c is app.tune_e for c in b.get_children()):
            return b
texts = [c.get_text() for c in tune_row().get_children() if isinstance(c, Gtk.Label)]
check("kHz" not in texts and app.tune_e.get_placeholder_text() == "kHz", "the only kHz is the one in the empty box", str(texts))
import subprocess
opened = []
real_popen = subprocess.Popen
subprocess.Popen = lambda a, **k: opened.append(a[1])
for view, wave, want in (("v1", 1, 1), ("v2", 2, 1), ("all", 1, 2), ("follow", 0, 2), ("follow", 2, 1)):
    app.cfg["view"] = view; state(wave); app._tick(); opened.clear(); app._on_web()
    check(len(opened) == want, "Website with Show %s, RADE %s: %d site(s)" % (view, wave or "off", want), str(opened))
subprocess.Popen = real_popen
print("all passed" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)

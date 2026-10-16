#!/usr/bin/env python3
"""The Reporter list: columns can be hidden and reordered (and that is remembered), right click on the km heading gives a
distance filter and on MHz a frequency-range filter. Scratch config, no windows shown."""
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

fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

cfg = core.load_config()
for k in cfg["servers"]:
    cfg["servers"][k]["host"] = ""            # nothing to connect to: the rosters are filled by hand
cfg["callsign"], cfg["grid"] = "EI6IZ", "IO53hu"
core.save_config(cfg)
import rade_reporter as rr

def make():
    app = rr.App(show=False)
    app.view_keys = ["v1"]
    return app

def feed(app, sid, call, grid, mhz):
    app.rosters["v1"].event("new_connection", {"sid": sid, "last_update": "2026-10-15T20:00:00+00:00", "callsign": call,
                                               "grid_square": grid, "version": "x", "rx_only": False, "connect_time": "x"})
    if mhz:
        app.rosters["v1"].event("freq_change", {"sid": sid, "freq": int(mhz * 1e6), "last_update": "2026-10-15T20:00:00+00:00"})

def shown(app):
    app._fill()
    return sorted(r[rr.C_CALL] for r in app.filt)

app = make()
# distances from IO53hu (Castlebar): EI7 IO63 about 100 km, G IO91 about 600 km, K1 FN42 about 4700 km, VK QF56 about 17000 km
feed(app, "a", "EI7AAA", "IO63", 7.1); feed(app, "b", "G4BBB", "IO91", 14.236); feed(app, "c", "K1CCC", "FN42", 14.236)
feed(app, "d", "VK5DDD", "QF56", 14.236); feed(app, "e", "NOGRID", "", 14.236); feed(app, "f", "NOFREQ", "IO91", 0)
check(len(shown(app)) == 6, "no filter: all six: %s" % shown(app))

print("headings")
for n, c in app.cols.items():
    b = app.labels[n].get_ancestor(Gtk.Button)
    check(b is not None, "heading %s has a button of its own to take the right click" % n) if n in ("km", "MHz", "Callsign") else None
mv = app._header_menu(None)
labs = [i.get_label() for i in mv.get_children() if i.get_label()]
check(labs[:2] == ["Distance filter...", "Frequency range filter..."] and "Callsign" in labs, "the Columns / filters button's menu has both filters and the columns")
print("distance filter (right click km)")
m_km = app._header_menu("km")          # keep the menu alive: its items go with it
check(m_km.get_children()[0].get_label() == "Distance filter...", "the km heading menu starts with the distance filter")
m_cs = app._header_menu("Callsign")
check(not any("filter" in (i.get_label() or "") for i in m_cs.get_children() if isinstance(i, Gtk.MenuItem) and not isinstance(i, Gtk.CheckMenuItem)),
      "another heading's menu has no filter item")
app._set_dist(0, 1000)
check(shown(app) == ["EI7AAA", "G4BBB", "NOFREQ"], "within 1000 km: %s (the station with no locator is hidden)" % shown(app))
check(app.labels["km"].get_text() == "km *", "the heading carries a star while it is on")
app._set_dist(500, 5000)
check(shown(app) == ["G4BBB", "K1CCC", "NOFREQ"], "between 500 and 5000 km: %s" % shown(app))
app._set_dist(10000, 0)
check(shown(app) == ["VK5DDD"], "beyond 10000 km: %s" % shown(app))
app._set_dist(0, 0)
check(len(shown(app)) == 6 and app.labels["km"].get_text() == "km", "cleared: everything again, no star")

print("frequency range filter (right click MHz)")
m_f = app._header_menu("MHz")
check(m_f.get_children()[0].get_label() == "Frequency range filter...", "the MHz heading menu starts with the range filter")
app._set_freq(14.0, 14.5)
check(shown(app) == ["G4BBB", "K1CCC", "NOGRID", "VK5DDD"], "14.0 to 14.5 MHz: %s (no frequency: hidden)" % shown(app))
app._set_freq(7.0, 7.2)
check(shown(app) == ["EI7AAA"], "7.0 to 7.2 MHz: %s" % shown(app))
app._set_freq(14.2, 0)
check(shown(app) == ["G4BBB", "K1CCC", "NOGRID", "VK5DDD"], "from 14.2 MHz up: %s" % shown(app))
app._set_dist(0, 1000)
check(shown(app) == ["G4BBB"], "both filters together: %s" % shown(app))
app._set_dist(0, 0); app._set_freq(0, 0)

print("columns: hide, reorder, remembered")
names = app._col_names()
check(names == rr.HEAD, "to begin with in the usual order")
app._set_column_visible("Msg", False); app._set_column_visible("Version", False)
check(not app.cols["Msg"].get_visible() and not app.cols["Version"].get_visible(), "two columns hidden")
app.tree.move_column_after(app.cols["SNR"], None)
app.tree.move_column_after(app.cols["Status"], app.cols["Callsign"])
check(app._col_names()[:3] == ["SNR", "Callsign", "Status"], "reordered: %s" % app._col_names()[:4])
menu = app._header_menu("Locator")
checks = [i for i in menu.get_children() if isinstance(i, Gtk.CheckMenuItem)]
check(len(checks) == len(rr.HEAD) and [i.get_active() for i in checks if i.get_label() in ("Msg", "Version")] == [False, False],
      "the heading menu lists every column with its state")
for n in rr.HEAD[1:]:
    app._set_column_visible(n, False)
check(sum(1 for c in app.cols.values() if c.get_visible()) == 1, "all but one hidden ...")
app._set_column_visible("Callsign", False)
check(app.cols["Callsign"].get_visible() or sum(1 for c in app.cols.values() if c.get_visible()) >= 1, "... and the last one cannot be hidden")
for n in rr.HEAD:
    app._set_column_visible(n, n not in ("Msg", "Version"))
app._set_dist(0, 2500); app._set_freq(14.0, 14.5)
d = json.load(open(core.config_path()))
check(d["columns"]["hidden"] == ["Version", "Msg"] or sorted(d["columns"]["hidden"]) == ["Msg", "Version"], "written to the config: hidden %s" % d["columns"]["hidden"])
check(d["columns"]["order"][:3] == ["SNR", "Callsign", "Status"], "written to the config: order %s ..." % d["columns"]["order"][:4])
check((d["dist_min"], d["dist_max"], d["f_min"], d["f_max"]) == (0, 2500, 14.0, 14.5), "the filters too")
for lk in app.links.values():
    lk.close()

print("a new start")
app2 = make()
check(app2._col_names()[:3] == ["SNR", "Callsign", "Status"], "the order is back: %s" % app2._col_names()[:4])
check(not app2.cols["Msg"].get_visible() and not app2.cols["Version"].get_visible() and app2.cols["Callsign"].get_visible(), "the hidden columns are back")
check(app2._dist == (0, 2500) and app2._frng == (14.0, 14.5) and app2.labels["km"].get_text() == "km *" and app2.labels["MHz"].get_text() == "MHz *",
      "the filters are back, with their stars")
for lk in app2.links.values():
    lk.close()
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

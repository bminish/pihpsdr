#!/usr/bin/env python3
"""The Reporter window keeps its message, Track, band and idle filter, and the SNR settings between runs,
sends the message on every connection, and reports the RADE SNR every few seconds. Mock server and a
scratch config only; needs a display."""
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

mock = MockServer().start()
cfg = core.load_config()
cfg["servers"]["v1"].update(host="127.0.0.1", port=mock.port, secure=False)
cfg["servers"]["v3"].update(host="127.0.0.1", port=mock.port, secure=False)
cfg.update(callsign="EI6IZ", grid="IO53hu", message="QRV on 40", band="40 m", track=True, track_mode="freq",
           idle="15 min", snr_enabled=True, snr_interval=3.0)
core.save_config(cfg)
assert core.config_path().startswith(scratch) and core.load_config()["servers"]["v1"]["host"] == "127.0.0.1"
os.makedirs(os.path.dirname(core.state_path()), exist_ok=True)

def put(**kw):
    p = core.state_path()
    try: d = json.load(open(p))
    except Exception: d = {}
    d.update(kw); d["updated"] = time.time()
    json.dump(d, open(p + ".t", "w")); os.replace(p + ".t", p)

put(running=1, waveform=1, dial_hz=7053000, tx=0, call="EI6IZ", rx_sync=0, rx_snr=0, rx_foff=0, heard_seq=0,
    heard_call="", heard_snr=0, heard_foff=0, heard_t=0)
import rade_reporter as rr
print("restored from the last run")
app = rr.App()
check(app.msg_e.get_text() == "QRV on 40" and app.message == "QRV on 40", "message is back")
check(app.band.get_active_text() == "40 m", "band filter is back")
check(app.track.get_active() and app.t_freq.get_active() and not app.t_band.get_active(), "Track on, by frequency")
check(app.idle.get_active_text() == "15 min", "idle filter is back")
check(app.snr_chk.get_active() and app.snr_spin.get_value() == 3.0, "SNR reporting on, every 3 s")
GLib.timeout_add(1000, lambda: (put(), True)[1])
rx = lambda: [e for c in mock.clients for e in c["events"] if e[0] == "rx_report"]
msgs = lambda: [e for c in mock.clients for e in c["events"] if e[0] == "message_update"]
def s1():
    app.rep_chk.set_active(True)       # callsign and grid came from the saved config
def s2():
    check(any(e[1]["message"] == "QRV on 40" for e in msgs()), "the saved message is sent when it connects")
    put(rx_sync=1, rx_snr=6.0, rx_foff=2.0, heard_seq=1, heard_call="OH2BCD", heard_snr=7.0, heard_foff=2.5, heard_t=time.time())
def s3():
    r = rx(); print("  rx_reports so far:", [x[1] for x in r])
    check(len(r) >= 1 and r[0][1] == {"callsign": "OH2BCD", "mode": "RADEV1", "snr": 7}, "the end-of-over call goes out first, with the over's SNR")
    put(rx_snr=9.0)
def s4():
    r = [x[1] for x in rx()]
    check(sum(1 for x in r if x["callsign"] == "OH2BCD") >= 3, "then the SNR goes out every few seconds (%d reports)" % len(r))
    check(any(x["snr"] == 9 for x in r), "with the SNR as it is now")
    print("  indicator:", app.snr_ind.get_text())
    check("sending for OH2BCD" in app.snr_ind.get_text(), "and the window says so")
    n = len(rx()); put(rx_foff=40.0)
    GLib.timeout_add(4500, lambda: (check(len(rx()) == n, "a signal 40 Hz away is not attributed to that station"), False)[1])
def s5():
    print("  indicator:", app.snr_ind.get_text())
    put(rx_foff=2.0, tx=1)
def s6():
    n = len(rx())
    GLib.timeout_add(4000, lambda: (check(len(rx()) == n, "nothing while transmitting"), False)[1])
    # change the sticky settings
    app.msg_e.set_text("QSY 40.5"); app._on_send()
    app.band.set_active(0); app.track.set_active(False); app.t_band.set_active(True)
    app.snr_spin.set_value(10)
def s7():
    app.rep_chk.set_active(False)
    c = json.load(open(core.config_path()))
    check(c["message"] == "QSY 40.5" and c["band"] == "All" and c["track"] is False and c["track_mode"] == "band"
          and c["snr_interval"] == 10.0 and "reporting" not in c, "changes are written to the config")
    app._quit.__func__  # (the window is closed below)
    Gtk.main_quit()
for t, f in ((2000, s1), (6000, s2), (12000, s3), (18000, s4), (24000, s5), (25000, s6), (31000, s7)):
    GLib.timeout_add(t, lambda f=f: (f(), False)[1])
Gtk.main()
app.link.close()
print("a new start")
app2 = rr.App()
check(app2.msg_e.get_text() == "QSY 40.5" and app2.band.get_active_text() == "All" and not app2.track.get_active()
      and app2.snr_spin.get_value() == 10 and app2.reporting is False and not app2.rep_chk.get_active(),
      "the changes are there next time, and reporting starts off")
app2.link.close(); mock.stop()
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

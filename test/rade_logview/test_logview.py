#!/usr/bin/env python3
"""Tests of the RADE log viewer on a synthetic run (scratch directory only). The window test needs a
display; the window is withdrawn so nothing appears on the desktop."""
import os, sys, tarfile, tempfile, math, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "rade-logview"))
import numpy as np
import logdata as ld

fails = []
def check(name, ok, info=""):
    print("  %-4s %s %s" % ("ok" if ok else "FAIL", name, info))
    if not ok: fails.append(name)

d = tempfile.mkdtemp()
run_dir = os.path.join(d, "20261016-120000")
os.makedirs(run_dir)
T0 = 1792152000.0
open(run_dir + "/meta.txt", "w").write("started_unix: %.3f\nwaveform: V1\ndial_hz: 7177000\nmode: DIGL\ninvert: 1\nprogram: test\n" % T0)
open(run_dir + "/rx.csv", "w").write("t,snr_mean,snr_min,foff_mean,n,delta_hat,gain,snr_est,est_foff\n" +
    "".join("%.2f,%.1f,%.1f,-2.0,25,,,,\n" % (0.5 * i, 8 + i % 3, 6 + i % 3) for i in range(10)) +
    "".join("%.2f,%.1f,%.1f,-2.5,25,,,,\n" % (20 + 0.5 * i, 3, 1) for i in range(10)))
open(run_dir + "/tx.csv", "w").write("t,loud_lufs,peak_dbfs,agc_db,limiting_pct,out_fill_min\n30.25,-20.0,-3.0,1.0,0,5\n30.75,-21.0,-2.0,1.5,10,5\n")
open(run_dir + "/events.csv", "w").write("t,kind,value\n0.1,dial,7177000\n5.2,eoo,\n5.4,call,EI6IZ snr=9.1\n30.0,key_down,\n31.0,key_up,\n")
open(run_dir + "/overs.csv", "w").write("utc_start,t_start,t_end,dial_hz,mode,wave,invert,snr_mean,snr_min,snr_max,foff_mean,foff_sd,sync_pct,eoo,call,call_snr\n"
    "x,0.00,5.00,7177000,DIGL,V1,1,9.0,6.0,10.0,-2.0,0.1,100,1,EI6IZ,9.1\nx,20.00,25.00,14236000,USB,V2,0,3.0,1.0,5.0,-2.5,0.2,95,0,,\n")
open(run_dir + "/tx_overs.csv", "w").write("utc_start,t_start,t_end,dial_hz,wave,drive_db,trim_db,agc_db_end,loud_mean,peak_max,limiting_pct,underruns,hold_ms\nx,30.0,31.0,7177000,V1,0,0,1.5,-20.5,-2,5,0,500\n")
tgz = run_dir + ".tar.gz"
with tarfile.open(tgz, "w:gz") as tf:
    tf.add(run_dir, arcname=os.path.basename(run_dir))

run = ld.load_run(run_dir)
check("loads rx rows", len(run.rx["t"]) == 20 and abs(run.rx["t"][0] - (T0 + 0)) < 1e-6)
check("empty V2 fields are NaN", np.isnan(run.rx["gain"]).all())
check("events and overs", len(run.events) == 5 and len(run.overs) == 2 and len(run.tx_overs) == 1)
check("tar.gz gives the same run", len(ld.load_run(tgz).rx["t"]) == 20 and ld.load_run(tgz).name == "20261016-120000")
check("file inside the run resolves", ld.load_run(run_dir + "/rx.csv").path == run_dir)
check("energy centre inverted / normal", ld.energy_centre_hz(7177000, 1) == 7175500 and ld.energy_centre_hz(14236000, 0) == 14237500)
t, y = ld.with_gaps(run.rx["t"], run.rx["snr_mean"])
check("gap becomes a break", len(t) == 21 and np.isnan(t[10]) and np.isnan(y[10]))
st = ld.window_stats(run.rx["t"], run.rx["snr_mean"], T0 + 20, T0 + 25)
check("window statistics", st and st["n"] == 10 and st["mean"] == 3.0)
check("no data in window -> None", ld.window_stats(run.rx["t"], run.rx["snr_mean"], T0 + 12, T0 + 15) is None)
ot = ld.over_text(run, run.overs[0])
check("over text has call, centre, dial", "EI6IZ" in ot and "7175.500 kHz nominal" in ot and "7177.000 kHz" in ot, "")
check("run text lists dials and calls", "14236.000" in ld.run_text(run) and "EI6IZ" in ld.run_text(run))
check("tx over text", "500 ms" in ld.tx_over_text(run, run.tx_overs[0]))
try:
    ld.load_run(os.path.join(d, "nothing.csv")); check("bad path raises", False)
except ValueError:
    check("bad path raises", True)

if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
    import tkinter as tk
    import rade_logview as rv
    root = tk.Tk(); root.withdraw()
    v = rv.Viewer(root, [run_dir, tgz.replace(".tar.gz", "") + ".tar.gz"])
    root.update()
    check("window: one run (same one not added twice)", len(v.runs) == 1)
    check("window: overs listed", len(v.tree.get_children()) == 3)
    v.pvars["TX loudness (LUFS)"].set(True); v.evars["Key down/up"].set(True); v.evars["Dial / mode / settings"].set(True)
    v.replot(); root.update()
    check("window: three panels", len(v.axes) == 3)
    v.tree.selection_set(v.tree.get_children()[1]); root.update()
    x0, x1 = v.axes[0].get_xlim()
    check("window: choosing an over zooms to it", 15 < v.from_x(x0) - T0 + 0 + 5 < 40 and (v.from_x(x1) - v.from_x(x0)) < 10)
    v.mvar.set(True)
    class E: pass
    for xd in (v.x(T0 + 21), v.x(T0 + 24)):
        e = E(); e.inaxes = v.axes[0]; e.xdata = float(xd); e.ydata = 3.0; v.on_click(e)
    root.update()
    check("window: measure dt = 3 s", "dt = 3.000" in v.readout.cget("text"), v.readout.cget("text"))
    v.utc.set(False); v.replot(); root.update(); v.fit_all()
    check("window: seconds axis", len(v.axes) == 3)
    v.remove_run if False else None
    root.destroy()
else:
    print("  no display: window tests skipped")
sys.exit(1 if fails else 0)

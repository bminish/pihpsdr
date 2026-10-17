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
hc = ld.heard_calls(run)
check("V1 call pairs with its over", len(hc) == 1 and hc[0]["call"] == "EI6IZ" and not hc[0]["v2"] and abs(hc[0]["t_end"] - (T0 + 5.0)) < 1e-6)
run.events += [(T0 + 40.0, "sync_on", ""), (T0 + 40.0, "wave", "2"), (T0 + 43.0, "call", "G4XYZ snr=3.0"), (T0 + 44.0, "call", "G4XYZ snr=4.0")]
run.events.sort(key=lambda e: e[0])
hc = ld.heard_calls(run)
check("V2 call known mid-over, no end yet, listed once", len(hc) == 2 and hc[1]["v2"] and hc[1]["t_end"] is None and abs(hc[1]["t_start"] - (T0 + 40.0)) < 1e-6)
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


# several runs in a folder; live runs
folder = os.path.join(d, "folder"); os.makedirs(folder)
import shutil
for n in ("20261016-120000", "20261016-130000"):
    shutil.copytree(run_dir, os.path.join(folder, n))
shutil.copy(tgz, os.path.join(folder, "20261016-120000.tar.gz"))      # the .tar.gz of a run that is also there as a directory
shutil.copy(tgz, os.path.join(folder, "20261015-090000.tar.gz"))
fr = ld.find_runs(folder)
check("find_runs: directories, and a .tar.gz only when its directory is not there",
      [os.path.basename(x) for x in fr] == ["20261016-120000", "20261016-130000", "20261015-090000.tar.gz"], str([os.path.basename(x) for x in fr]))
check("find_runs: a run is itself", ld.find_runs(run_dir) == [run_dir])
check("a directory run without ended_utc is live, a .tar.gz is not", ld.load_run(run_dir).live and not ld.load_run(tgz).live)
sig0 = ld.signature(run_dir)
assert ld.signature(run_dir) == sig0
with open(run_dir + "/rx.csv", "a") as f:
    f.write("%.2f,%.1f,%.1f,-2.0,25,,,,\n" % (40.0, 7, 5))
    f.write("40.5,7.0,5")                                        # half a line, as when read while the log is writing
check("a write changes the signature", ld.signature(run_dir) != sig0)
r2 = ld.load_run(run_dir)
check("the half-written last line is left out, the whole one is read", len(r2.rx["t"]) == 21 and abs(r2.rx["t"][-1] - (T0 + 40.0)) < 1e-6, str(len(r2.rx["t"])))
with open(run_dir + "/rx.csv", "a") as f:
    f.write(",25,,,,\n")                                         # the rest arrives
check("and the line read once it is complete", len(ld.load_run(run_dir).rx["t"]) == 22)

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
    check("window: no legend boxes over the data; the lines are named in the axis label", all(a.get_legend() is None for a in v.axes) and "solid mean" in v.axes[0].get_ylabel(), v.axes[0].get_ylabel())
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
    # several at once, live runs, update
    root2 = tk.Tk(); root2.withdraw()
    v2 = rv.Viewer(root2, [folder, tgz])
    v2.quiet = True
    v2.logroot = folder
    root2.update()
    check("window: a folder of runs opens all of them (the .tar.gz of an open directory is the same run)", len(v2.runs) == 3, str([r.name for r in v2.runs]))
    check("window: live runs are marked", v2.runlist.get(0).startswith("● ") and not v2.runlist.get(2).startswith("●"), v2.runlist.get(0))
    check("window: nothing changed, nothing reloaded", v2.update_live() == 0)
    n0 = len(v2.runs[0].rx["t"])
    with open(os.path.join(folder, "20261016-120000", "rx.csv"), "a") as f:
        for i in range(8):
            f.write("%.2f,%.1f,%.1f,-2.0,25,,,,\n" % (60 + 0.5 * i, 6, 4))
    v2.axes and v2.set_xlim(T0 + 0, T0 + 30)
    check("window: update picks up the new rows", v2.update_live() == 1 and len(v2.runs[0].rx["t"]) == n0 + 8)
    x0, x1 = v2.axes[0].get_xlim()
    check("window: following puts the right edge on the newest data, same width",
          abs((x1 - x0) * 86400 - 30) < 0.5 and abs(v2.from_x(x1) - (T0 + 63.5 + 0.9)) < 1.5, str((v2.from_x(x1) - T0, (x1 - x0) * 86400)))
    open(os.path.join(folder, "20261016-120000", "meta.txt"), "a").write("ended_utc: x\ndropped_rows: 0\n")
    v2.update_live()
    check("window: a run that has ended is no longer live", not v2.runs[0].live and not v2.runlist.get(0).startswith("●"))
    check("window: Update with nothing live changed does nothing", v2.update_live() == 0)
    # frequency panel centred on 0, y zoom held on update, remove all, F5 opens the newest run
    v2.pvars["Frequency offset (Hz)"].set(True); v2.replot(); root2.update()
    fa = [a for a in v2.axes if a.get_ylabel().startswith("Frequency")][0]
    lo, hi = fa.get_ylim()
    check("window: frequency panel is centred on 0 Hz", abs(lo + hi) < 1e-9 and hi >= 2.0, str((lo, hi)))
    fa.set_ylim(-0.5, 0.5); v2.replot(); root2.update()
    fa = [a for a in v2.axes if a.get_ylabel().startswith("Frequency")][0]
    check("window: the operator's frequency zoom is kept when the plot is redrawn", fa.get_ylim() == (-0.5, 0.5), str(fa.get_ylim()))
    v2.remove_all(); root2.update()
    check("window: remove all", not v2.runs and v2.runlist.size() == 0)
    v2.update_live()
    check("window: F5 with nothing open opens the newest run", len(v2.runs) == 1 and v2.runs[0].name == "20261016-130000", str([r.name for r in v2.runs]))
    check("window: and not twice", v2.open_newest() == 0 and len(v2.runs) == 1)
    v2.auto.set(True); v2.auto_changed(); root2.update(); v2.auto.set(False); v2.auto_changed()
    root2.destroy()

else:
    print("  no display: window tests skipped")
sys.exit(1 if fails else 0)

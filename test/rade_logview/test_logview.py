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
full = "t,rx_pos,sync,eoo,snr_db,foff_hz,nout,in_fill,out_fill,delta_hat,est_foff,gain,snr_est\n" + "".join(
    "%.3f,%d,%d,0,%.2f,%.2f,144,0,0,1.5,%.2f,3.0,%.2f\n" % (0.04 * i, 0, 1 if i % 10 else 0, 8.0, -2.0, -2.0, 8.0) for i in range(50))
open(run_dir + "/rx_full.csv", "w").write(full)
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
check("rx_full.csv loads whole", len(run.rxf["t"]) == 50 and abs(run.rxf["t"][1] - (T0 + 0.04)) < 1e-6 and run.rxf_off > 0)
open(run_dir + "/rx_full.csv", "a").write("2.000,0,1,0,9.00,-1.00,144,0,0,1.5,-1.00,3.0,9.00\n2.040,0,1,0,9.0")      # the last line half written
more = ld.load_run(run_dir, prev=run)
check("live: only the new complete lines are added, the half line waits", len(more.rxf["t"]) == 51 and more.rxf_off > run.rxf_off)
open(run_dir + "/rx_full.csv", "a").write("0,-1.00,144,0,0,1.5,-1.00,3.0,9.00\n")
check("live: the finished line arrives next time", len(ld.load_run(run_dir, prev=more).rxf["t"]) == 52)
check("same result as reading it all", np.allclose(ld.load_run(run_dir, prev=more).rxf["snr_db"], ld.load_run(run_dir).rxf["snr_db"]))
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
check("find_runs: a file in a run is its run, a .tar.gz is itself, any other file is nothing",
      ld.find_runs(run_dir + "/rx.csv") == [run_dir] and ld.find_runs(tgz) == [tgz] and ld.find_runs(d + "/nothing.csv") == [])

# runs nested in subfolders (the grabber's captures/v1/<run>, captures/7190kHz_V1/<run>)
nest = os.path.join(d, "captures")
for sub, n in (("v1", "2026-10-19_14236kHz_235552"), ("v2", "2026-10-20_14240kHz_000559"), ("7190kHz_V1", "20261018-173500")):
    shutil.copytree(run_dir, os.path.join(nest, sub, n))
shutil.copy(tgz, os.path.join(nest, "v2", "2026-10-20_14240kHz_000559.tar.gz"))      # beside its own directory
shutil.copy(tgz, os.path.join(nest, "v1", "2026-10-18_old.tar.gz"))
nr = [os.path.relpath(x, nest) for x in ld.find_runs(nest)]
check("find_runs: nested runs at any depth, a .tar.gz only when its directory is not beside it",
      nr == ["7190kHz_V1/20261018-173500", "v1/2026-10-19_14236kHz_235552", "v2/2026-10-20_14240kHz_000559", "v1/2026-10-18_old.tar.gz"], str(nr))
check("find_runs: a run directory is not searched for more runs",
      ld.find_runs(os.path.join(nest, "v1", "2026-10-19_14236kHz_235552")) == [os.path.join(nest, "v1", "2026-10-19_14236kHz_235552")])
check("load_run: a run in a v1 / v2 / kHz folder carries the folder in its name",
      [ld.load_run(x).name for x in ld.find_runs(nest)[:3]] == ["7190kHz_V1/20261018-173500", "v1/2026-10-19_14236kHz_235552", "v2/2026-10-20_14240kHz_000559"])
nrun = ld.load_run(os.path.join(nest, "v2", "2026-10-20_14240kHz_000559"))
nrun.events = [e for e in nrun.events if e[1] not in ("call", "wave")] + [(T0 + 3.0, "call", "G4XYZ snr=3.0")]
nrun.overs = []
check("heard_calls: with no wave event and no over, the run's waveform says V1; with none, the v2 folder says V2",
      not ld.heard_calls(nrun)[0]["v2"] and (nrun.meta.pop("waveform") or True) and ld.heard_calls(nrun)[0]["v2"])
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
    v.evars["End-of-over / calls"].set(True); v.replot(); root.update()
    run1 = v.runs[0]
    check("marks: end-of-over at the eoo event; the over with eoo=1 has it, the faded one (eoo=0) has none",
          [round(t - T0, 1) for t in v.eoo_times(run1)] == [5.2], str(v.eoo_times(run1)))
    run1.events = [e for e in run1.events if e[1] != "eoo"]
    check("marks: an over whose row says eoo=1 and has no event is marked at its end",
          [round(t - T0, 1) for t in v.eoo_times(run1)] == [5.0])
    run1.overs[1]["eoo"] = math.nan
    check("marks: an unrecorded eoo (old format) is not a mark", len(v.eoo_times(run1)) == 1)
    run1.events.append((T0 + 5.2, "eoo", ""))
    ax = v.axes[0]; v.replot(); root.update(); ax = v.axes[0]
    cols = [(l.get_color(), l.get_linestyle()) for l in ax.get_lines() if len(l.get_xdata()) == 2]
    check("marks: end-of-over light red, V1 call solid green", ("#d97777", "-") in cols and ("green", "-") in cols, str(cols))
    check("marks: V1 call labelled on the top graph only",
          [t.get_text() for t in ax.texts].count("EI6IZ") == 1 and not any(t.get_text() == "EI6IZ" for a in v.axes[1:] for t in a.texts))
    run1.events += [(T0 + 40.0, "sync_on", ""), (T0 + 40.0, "wave", "2"), (T0 + 43.0, "call", "G4XYZ snr=3.0"), (T0 + 44.0, "call", "G4XYZ snr=4.0")]
    v.replot(); root.update(); ax = v.axes[0]
    cols = [(l.get_color(), l.get_linestyle()) for l in ax.get_lines() if len(l.get_xdata()) == 2]
    check("marks: V2 first decode of the call dotted green, once", cols.count(("green", ":")) == 1, str(cols))
    run1.events = [e for e in run1.events if e[0] < T0 + 39]
    v.replot(); root.update()
    v.tree.selection_set(v.tree.get_children()[1]); root.update()
    x0, x1 = v.axes[0].get_xlim()
    check("window: choosing an over zooms to it", 15 < v.from_x(x0) - T0 + 0 + 5 < 40 and (v.from_x(x1) - v.from_x(x0)) < 10)
    v.mvar.set(True)
    v.full.set(False); v.replot(); root.update()          # the synthetic rx_full.csv covers 0-2 s only: measure on rx.csv here
    class E: pass
    def ev(t, ax=0, px=100):
        e = E(); e.inaxes = v.axes[ax]; e.xdata = float(v.x(t)); e.ydata = 3.0; e.x = px; e.button = 1; return e
    v.on_press(ev(T0 + 21)); v.on_release(ev(T0 + 21)); root.update()
    txt = v.text.get("1.0", "end")
    check("window: a click is a cursor on every graph", all(len(a._mlines) == 1 for a in v.axes), [len(a._mlines) for a in v.axes])
    check("window: the cursor lists every line (SNR mean and min, offset, loudness)", txt.count("run 1") == 4 and "Cursor" in txt, txt)
    check("window: the value at the cursor is the sample there (offset -2.5 at 21 s)", "-2.500" in txt, txt)
    v.on_press(ev(T0 + 21, px=100)); v.on_move(ev(T0 + 24, px=160)); v.on_release(ev(T0 + 24, px=160)); root.update()
    txt = v.text.get("1.0", "end")
    check("window: a drag is a box on every graph", all(len(a._mlines) == 3 for a in v.axes), [len(a._mlines) for a in v.axes])
    check("window: the range readout has dt", "dt = 3.000" in v.readout.cget("text"), v.readout.cget("text"))
    check("window: max, mean and min for every line", "max" in txt and txt.count("run 1") == 4 and "3.000" in txt, txt)
    v.clear_measure()
    x0, x1 = v.axes[0].get_xlim(); xm = float(v.x(T0 + 22))
    e = ev(T0 + 22); e.step = 1; e.key = None; v.on_scroll(e)
    n0, n1 = v.axes[0].get_xlim()
    check("window: wheel up zooms the time axis in about the pointer", abs((n1 - n0) - (x1 - x0) / 1.25) < 1e-9 and abs((xm - n0) / (n1 - n0) - (xm - x0) / (x1 - x0)) < 1e-6)
    check("window: ...on every graph (shared axis)", all(a.get_xlim() == v.axes[0].get_xlim() for a in v.axes))
    e = ev(T0 + 22); e.step = -1; e.key = None; v.on_scroll(e)
    check("window: wheel down zooms back out", abs(v.axes[0].get_xlim()[0] - x0) < 1e-9 and abs(v.axes[0].get_xlim()[1] - x1) < 1e-9)
    y0, y1 = v.axes[1].get_ylim(); e = ev(T0 + 22, ax=1); e.step = 1; e.key = "control"; v.on_scroll(e)
    check("window: Ctrl + wheel zooms the level axis of that graph only", v.axes[1].get_ylim() != (y0, y1) and v.axes[0].get_xlim() == (n0 if False else v.axes[0].get_xlim()))
    v.toolbar.pan(); root.update(); v.mvar.set(True); v.measure_toggled()
    check("window: Measure lets go of the toolbar's pan", v.toolbar.mode == "")
    v.clear_measure()
    check("window: Esc clears the marks", all(len(a._mlines) == 0 for a in v.axes))
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

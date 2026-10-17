#!/usr/bin/env python3
"""RADE log viewer: zoomable graphs of the parameter logs written by piHPSDR (rade-logs/<run>/).

    rade_logview.py [run-directory | run.tar.gz | folder-of-runs ...]

Pick runs (Open), tick the parameters to plot, zoom/pan with the toolbar, tick Measure and click
twice on the graph for the time and level difference and the statistics between the two points.
Choosing an over in the list zooms to it and shows what is known about it in the text area.
"""
import os, sys, math, datetime
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import matplotlib
if os.environ.get("RADE_LOGVIEW_AGG"):
    matplotlib.use("Agg")
else:
    matplotlib.use("TkAgg")
import matplotlib.dates as mdates
from matplotlib.figure import Figure
import logdata as ld
import tidy_logs

# (label, table, [(column, legend, style)])
PARAMS = [
    ("SNR (dB)", "rx", [("snr_mean", "mean", "-"), ("snr_min", "min", ":")]),
    ("Frequency offset (Hz)", "rx", [("foff_mean", "offset", "-")]),
    ("V2 SNR estimate (dB)", "rx", [("snr_est", "snr_est", "-")]),
    ("V2 gain", "rx", [("gain", "gain", "-")]),
    ("V2 delta_hat", "rx", [("delta_hat", "delta_hat", "-")]),
    ("V2 est_foff (Hz)", "rx", [("est_foff", "est_foff", "-")]),
    ("TX loudness (LUFS)", "tx", [("loud_lufs", "loudness", "-")]),
    ("TX peak (dBFS)", "tx", [("peak_dbfs", "peak", "-")]),
    ("TX leveller gain (dB)", "tx", [("agc_db", "gain", "-")]),
    ("TX limiting (%)", "tx", [("limiting_pct", "limiting", "-")]),
    ("TX output fill (min)", "tx", [("out_fill_min", "fill", "-")]),
]
SNR_LIMITS = (-12.0, 30.0)          # RADE is never better than +30 dB or worse than -12 dB: the SNR panels cannot be zoomed outside this
DEFAULT_ON = {"SNR (dB)", "Frequency offset (Hz)"}
EVENT_GROUPS = [
    ("Overs shaded", None),
    ("Key down/up", ("key_down", "key_up")),
    ("End-of-over / calls", ("eoo", "call")),
    ("Dial / mode / settings", ("dial", "mode", "wave", "invert", "drive", "trim", "agc")),
]
COLOURS = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf", "#8c564b", "#e377c2"]


def to_num(t_abs):
    return np.asarray(t_abs, dtype=float) / 86400.0      # matplotlib date numbers count days from 1970


class Viewer:
    def __init__(self, root, paths=()):
        self.root = root
        self.runs = []
        self.xlim = None
        self.logroot = None                # where the runs are (default: rade-logs/ of the piHPSDR directory)
        self.yuser = {}                    # y ranges set by the operator, by axis label: kept when the plot is redrawn
        self.measure = []                  # clicked points: (time_num, axes_index, y)
        root.title("RADE log viewer")
        root.geometry("1250x800")
        pw = ttk.PanedWindow(root, orient="horizontal")
        pw.pack(fill="both", expand=True)
        left = ttk.Frame(pw, width=330)
        right = ttk.Frame(pw)
        pw.add(left, weight=0)
        pw.add(right, weight=1)

        bar = ttk.Frame(left); bar.pack(fill="x", padx=4, pady=4)
        ttk.Button(bar, text="Files…", command=self.open_dialog).pack(side="left")
        ttk.Button(bar, text="Folder…", command=self.open_folder).pack(side="left", padx=4)
        ttk.Button(bar, text="Remove", command=self.remove_run).pack(side="left")
        ttk.Button(bar, text="Remove all", command=self.remove_all).pack(side="left", padx=4)
        ttk.Button(bar, text="Fit all", command=self.fit_all).pack(side="left", padx=4)
        self.runlist = tk.Listbox(left, height=6, exportselection=False)
        self.runlist.pack(fill="x", padx=4)
        lf = ttk.Frame(left); lf.pack(fill="x", padx=4, pady=(4, 0))
        ttk.Button(lf, text="Update live (F5)", command=self.update_live).pack(side="left")
        self.auto = tk.BooleanVar(value=False)
        ttk.Checkbutton(lf, text="every", variable=self.auto, command=self.auto_changed).pack(side="left", padx=(6, 0))
        self.auto_s = tk.IntVar(value=2)
        ttk.Spinbox(lf, from_=1, to=60, width=3, textvariable=self.auto_s).pack(side="left")
        ttk.Label(lf, text="s").pack(side="left")
        self.follow = tk.BooleanVar(value=True)
        ttk.Checkbutton(left, text="Follow the newest data when updating", variable=self.follow).pack(anchor="w", padx=4)
        self.live_status = ttk.Label(left, text="no live run", anchor="w", font=("monospace", 8))
        self.live_status.pack(fill="x", padx=4)
        self._auto_id = None
        self.runlist.bind("<<ListboxSelect>>", lambda e: self.show_run_text())

        pf = ttk.LabelFrame(left, text="Parameters (one panel each)"); pf.pack(fill="x", padx=4, pady=4)
        self.pvars = {}
        for label, _t, _c in PARAMS:
            v = tk.BooleanVar(value=label in DEFAULT_ON)
            self.pvars[label] = v
            ttk.Checkbutton(pf, text=label, variable=v, command=self.replot).pack(anchor="w")
        ef = ttk.LabelFrame(left, text="Marks"); ef.pack(fill="x", padx=4)
        self.evars = {}
        for label, _k in EVENT_GROUPS:
            v = tk.BooleanVar(value=label in ("Overs shaded", "End-of-over / calls"))
            self.evars[label] = v
            ttk.Checkbutton(ef, text=label, variable=v, command=self.replot).pack(anchor="w")
        self.utc = tk.BooleanVar(value=True)
        ttk.Checkbutton(left, text="UTC clock on the axis (off: seconds from first run start)", variable=self.utc,
                        command=self.replot).pack(anchor="w", padx=4)

        of = ttk.LabelFrame(left, text="Overs (heard: H, transmitted: T) - choose one to zoom")
        of.pack(fill="both", expand=True, padx=4, pady=4)
        cols = ("run", "start", "dur", "kHz", "wave", "snr", "call")
        self.tree = ttk.Treeview(of, columns=cols, show="headings", height=10, selectmode="browse")
        for c, w in zip(cols, (50, 62, 40, 70, 40, 40, 70)):
            self.tree.heading(c, text=c); self.tree.column(c, width=w, anchor="w", stretch=c in ("call",))
        sb = ttk.Scrollbar(of, command=self.tree.yview); self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True); sb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self.on_over)

        self.fig = Figure(figsize=(8, 6), dpi=100)
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
        top = ttk.Frame(right); top.pack(fill="both", expand=True)
        self.canvas = FigureCanvasTkAgg(self.fig, master=top)
        tb = ttk.Frame(top); tb.pack(side="top", fill="x")
        self.toolbar = NavigationToolbar2Tk(self.canvas, tb)
        self.toolbar.update()
        self.mvar = tk.BooleanVar(value=False)
        ttk.Checkbutton(tb, text="Measure (click A, click B)", variable=self.mvar,
                        command=self.clear_measure).pack(side="left", padx=8)
        self.canvas.get_tk_widget().pack(side="top", fill="both", expand=True)
        self.readout = ttk.Label(right, text="", anchor="w", font=("monospace", 9))
        self.readout.pack(fill="x")
        self.text = tk.Text(right, height=13, wrap="none", font=("monospace", 9))
        self.text.pack(fill="x")
        self.text.configure(state="disabled")
        self.canvas.mpl_connect("motion_notify_event", self.on_move)
        self.canvas.mpl_connect("button_press_event", self.on_click)
        root.bind("<Key-Escape>", lambda e: self.clear_measure())
        root.bind("<F5>", lambda e: self.update_live())
        for p in paths:
            self.add_any(p)
        self.replot()

    # ---------------------------------------------------------------- runs
    def open_dialog(self):
        ps = filedialog.askopenfilenames(title="Open runs: choose files in run directories and/or .tar.gz (several at once)",
                                         initialdir=self.initialdir(),
                                         filetypes=[("RADE logs", "*.csv *.txt *.tar.gz"), ("all", "*")])
        for p in ps:
            self.add_path(p)
        if ps:
            self.replot()

    def open_folder(self):
        p = filedialog.askdirectory(title="Open a run directory (live runs can be updated), or a folder of runs",
                                    initialdir=self.initialdir(), mustexist=True)
        if p:
            self.add_any(p, choose=True)
            self.replot()

    def pick_runs(self, found):
        """Several runs in a folder: a list to choose from (shift / ctrl click), all selected to begin with."""
        top = tk.Toplevel(self.root)
        top.title("Choose runs")
        top.transient(self.root)
        lb = tk.Listbox(top, selectmode="extended", width=60, height=min(20, len(found)), exportselection=False)
        for q in found:
            lb.insert("end", os.path.basename(q))
        lb.selection_set(0, "end")
        lb.pack(fill="both", expand=True, padx=6, pady=6)
        chosen = []

        def ok():
            chosen.extend(found[i] for i in lb.curselection())
            top.destroy()
        row = ttk.Frame(top); row.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Button(row, text="Open", command=ok).pack(side="right")
        ttk.Button(row, text="Cancel", command=top.destroy).pack(side="right", padx=4)
        top.grab_set()
        self.root.wait_window(top)
        return chosen

    def add_any(self, p, choose=False):
        """A run (directory, file in one, .tar.gz) or a folder of runs: all of them are opened (choose: pick from the list)."""
        if os.path.isdir(p):
            found = ld.find_runs(p)
            if not found:
                self.error("Cannot open", "%s\n\nno run (a directory with meta.txt) or .tar.gz in it" % p)
            if choose and len(found) > 1 and not getattr(self, "quiet", False):
                found = self.pick_runs(found)
            for q in found:
                self.add_path(q)
        else:
            self.add_path(p)

    def error(self, title, msg):
        if getattr(self, "quiet", False):
            print(title, msg)
        else:
            messagebox.showerror(title, msg)

    def initialdir(self):
        if self.logroot:
            return self.logroot
        d = os.path.join(HERE, "..", "..", "rade-logs")
        return os.path.abspath(d) if os.path.isdir(d) else os.getcwd()

    def add_path(self, p):
        try:
            run = ld.load_run(p)
        except Exception as e:                               # a bad file is reported, never fatal
            self.error("Cannot open", "%s\n\n%s" % (p, e))
            return None
        if any(r.name == run.name and r.t0 == run.t0 for r in self.runs):
            return None
        self.runs.append(run)
        self.runlist.insert("end", self.run_label(run))
        self.colour_runs()
        self.refresh_overs()
        self.live_note()
        self.set_text(ld.run_text(run))
        return run

    def colour_runs(self):
        for i in range(self.runlist.size()):                  # the list is the key to the colours in the graphs
            self.runlist.itemconfig(i, foreground=COLOURS[i % len(COLOURS)])

    def run_label(self, run):
        return ("● " if run.live else "") + run.name

    def live_note(self, text=None):
        n = sum(1 for r in self.runs if r.live)
        if text is None:
            text = "%d live run%s (● in the list)" % (n, "" if n == 1 else "s") if n else "no live run"
        self.live_status.configure(text=text)

    def auto_changed(self):
        if self._auto_id is not None:
            self.root.after_cancel(self._auto_id)
            self._auto_id = None
        if self.auto.get():
            self._auto_tick()

    def _auto_tick(self):
        self.update_live()
        try:
            ms = max(1, int(self.auto_s.get())) * 1000
        except (tk.TclError, ValueError):
            ms = 2000
        self._auto_id = self.root.after(ms, self._auto_tick) if self.auto.get() else None

    def newest(self):
        """The latest time in any live run (absolute seconds)."""
        t = []
        for r in self.runs:
            if not r.live:
                continue
            for d in (r.rx, r.tx):
                if "t" in d and len(d["t"]):
                    t.append(float(np.nanmax(d["t"])))
            if r.events:
                t.append(r.events[-1][0])
        return max(t) if t else None

    def open_newest(self):
        """The most recent run directory in rade-logs/, opened if it is not open already. 1 if it was opened."""
        runs = [q for q in ld.find_runs(self.initialdir()) if os.path.isdir(q)]
        if not runs:
            return 0
        q = max(runs, key=os.path.basename)                 # the names are start times
        if any(os.path.abspath(r.source) == os.path.abspath(q) for r in self.runs):
            return 0
        return 1 if self.add_path(q) is not None else 0

    def update_live(self):
        """Reload every live run whose files have changed; keep the view, or follow the newest data."""
        changed = self.open_newest()
        for i, r in enumerate(self.runs):
            if not r.live or ld.signature(r.source) == r.sig:
                continue
            try:
                new = ld.load_run(r.source)
            except Exception as e:                           # caught mid-write, or gone: keep what is shown
                self.live_note("update of %s failed: %s" % (r.name, e))
                continue
            new.name = r.name
            self.runs[i] = new
            self.runlist.delete(i)
            self.runlist.insert(i, self.run_label(new))
            self.colour_runs()
            changed += 1
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        if not changed:
            self.live_note("%s: no change (%d live)" % (stamp, sum(1 for r in self.runs if r.live)))
            return 0
        old = self.axes_xlim() if getattr(self, "axes", None) else None
        self.refresh_overs()
        self.replot()
        if self.follow.get() and old is not None and self.axes:
            tn = self.newest()
            if tn is not None:
                width = (old[1] - old[0]) * (86400.0 if self.utc.get() else 1.0)      # seconds
                self.set_xlim(tn - width * 0.97, tn + width * 0.03)    # the right edge goes to the newest data, same width
        sel = self.runlist.curselection()
        if sel:
            self.show_run_text()
        self.live_note("%s: updated %d run%s" % (stamp, changed, "" if changed == 1 else "s"))
        return changed

    def remove_run(self):
        sel = self.runlist.curselection()
        if not sel:
            return
        del self.runs[sel[0]]
        self.runlist.delete(sel[0])
        self.colour_runs()
        self.refresh_overs()
        self.replot()

    def remove_all(self):
        self.runs = []
        self.runlist.delete(0, "end")
        self.yuser = {}
        self.refresh_overs()
        self.live_note()
        self.set_text("")
        self.replot()

    def show_run_text(self):
        sel = self.runlist.curselection()
        if sel:
            self.set_text(ld.run_text(self.runs[sel[0]]))

    def set_text(self, s):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.insert("1.0", s)
        self.text.configure(state="disabled")

    # ---------------------------------------------------------------- overs
    def refresh_overs(self):
        self.tree.delete(*self.tree.get_children())
        self.over_by_iid = {}
        rows = []
        for ri, r in enumerate(self.runs):
            for o in r.overs:
                rows.append((o["t_start"], ri, "H", o))
            for o in r.tx_overs:
                rows.append((o["t_start"], ri, "T", o))
        rows.sort(key=lambda x: x[0])
        for t, ri, kind, o in rows:
            utc = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%H:%M:%S")
            snr = "%.1f" % o["snr_mean"] if kind == "H" and not math.isnan(o["snr_mean"]) else ""
            iid = self.tree.insert("", "end", values=("%s%d" % (kind, ri + 1), utc, "%.0f" % (o["t_end"] - o["t_start"]),
                                                      "%.1f" % (o["dial_hz"] / 1e3), o.get("wave", ""), snr,
                                                      o.get("call", "") if kind == "H" else ""))
            self.over_by_iid[iid] = (ri, kind, o)

    def on_over(self, _e=None):
        sel = self.tree.selection()
        if not sel:
            return
        ri, kind, o = self.over_by_iid[sel[0]]
        run = self.runs[ri]
        self.set_text(ld.over_text(run, o) if kind == "H" else ld.tx_over_text(run, o))
        pad = max(2.0, (o["t_end"] - o["t_start"]) * 0.1)
        self.set_xlim(o["t_start"] - pad, o["t_end"] + pad)

    # ---------------------------------------------------------------- plotting
    def x(self, t_abs):
        if self.utc.get():
            return to_num(t_abs)
        base = min((r.t0 for r in self.runs), default=0.0)
        return (np.asarray(t_abs, dtype=float) - base)

    def selected_params(self):
        return [p for p in PARAMS if self.pvars[p[0]].get()]

    def replot(self):
        old = self.axes_xlim() if getattr(self, "axes", None) else None
        was_utc = getattr(self, "_was_utc", self.utc.get())
        for ax, key, auto in getattr(self, "ykeys", []):         # a y range the operator set (zoom, pan) is kept
            if tuple(np.round(ax.get_ylim(), 6)) != tuple(np.round(auto, 6)):
                self.yuser[key] = ax.get_ylim()
        self.fig.clear()
        self.measure = []
        self.ykeys = []
        self.axes = []
        params = self.selected_params()
        if not self.runs or not params:
            self.fig.text(0.5, 0.5, "Open a run (Open…) and tick a parameter" if not self.runs else "Tick a parameter",
                          ha="center", va="center", color="gray")
            self.canvas.draw_idle()
            return
        axs = self.fig.subplots(len(params), 1, sharex=True, squeeze=False)[:, 0]
        for ax, (label, table, series) in zip(axs, params):
            for ri, run in enumerate(self.runs):
                data = run.rx if table == "rx" else run.tx
                if "t" not in data:
                    continue
                for col, leg, style in series:
                    if col not in data:
                        continue
                    t, y = ld.with_gaps(data["t"], data[col])
                    if np.all(np.isnan(y)):
                        continue
                    ax.plot(self.x(t), y, style, color=COLOURS[ri % len(COLOURS)], lw=1.1)
            if len(series) > 1:                              # what the lines are, in the axis label: no box over the data
                label += "\n" + " / ".join("%s %s" % ({"-": "solid", ":": "dotted"}.get(st, st), leg) for _c, leg, st in series)
            ax.set_ylabel(label, fontsize=8)
            if "(Hz)" in label:                              # frequency: centred on 0, a few Hz wide unless the data is wider
                ax.set_ylim(*self.yuser.get(label, self.freq_range(ax)))
            ax.grid(True, alpha=0.3)
            self.ykeys.append((ax, label, ax.get_ylim()))
            if label in self.yuser and "(Hz)" not in label:
                ax.set_ylim(*self.yuser[label])
            if "SNR" in label:
                self.limit_y(ax, *SNR_LIMITS)
            ax.tick_params(labelsize=8)
            self.draw_marks(ax, first=(ax is axs[0]))
        self.axes = list(axs)
        if self.utc.get():
            loc = mdates.AutoDateLocator()
            axs[-1].xaxis.set_major_locator(loc)
            axs[-1].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc, formats=["%Y", "%b", "%d", "%H:%M", "%H:%M:%S", "%S.%f"]))
            axs[-1].set_xlabel("UTC", fontsize=8)
        else:
            axs[-1].set_xlabel("seconds from the first run's start", fontsize=8)
        if old is not None and was_utc == self.utc.get():
            axs[0].set_xlim(*old)
        self._was_utc = self.utc.get()
        self.fig.subplots_adjust(left=0.09, right=0.99, top=0.98, bottom=0.08, hspace=0.12)
        self.canvas.draw_idle()

    @staticmethod
    def limit_y(ax, lo, hi):
        """Keep ax's y range inside lo..hi, now and after any zoom or pan."""
        def clamp(a):
            y0, y1 = a.get_ylim()
            n0, n1 = max(min(y0, y1), lo), min(max(y0, y1), hi)
            if n0 >= n1:
                n0, n1 = lo, hi
            if (n0, n1) != (y0, y1):
                a.set_ylim(n0, n1)              # the callback runs once more and finds nothing to change
        clamp(ax)
        ax.callbacks.connect("ylim_changed", clamp)

    def freq_range(self, ax):
        """Symmetric about 0 Hz: the largest value drawn, at least +-2 Hz."""
        m = 0.0
        for ln in ax.get_lines():
            y = np.asarray(ln.get_ydata(), float)
            y = y[np.isfinite(y)]
            if len(y):
                m = max(m, float(np.max(np.abs(y))))
        h = max(2.0, m * 1.1)
        return (-h, h)

    def axes_xlim(self):
        return self.axes[0].get_xlim() if self.axes else None

    def draw_marks(self, ax, first):
        on = {k: v.get() for k, v in self.evars.items()}
        for ri, run in enumerate(self.runs):
            col = COLOURS[ri % len(COLOURS)]
            if on["Overs shaded"]:
                for o in run.overs:
                    ax.axvspan(self.x(o["t_start"]), self.x(o["t_end"]), color=col, alpha=0.07, lw=0)
                for o in run.tx_overs:
                    ax.axvspan(self.x(o["t_start"]), self.x(o["t_end"]), color="red", alpha=0.07, lw=0)
            if on["End-of-over / calls"]:
                self.draw_calls(ax, run, first)
            for t, kind, val in run.events:
                if kind in ("key_down", "key_up") and on["Key down/up"]:
                    ax.axvline(self.x(t), color="green" if kind == "key_down" else "firebrick", lw=0.7, ls="--")
                elif kind == "eoo" and on["End-of-over / calls"]:
                    ax.axvline(self.x(t), color="darkorange", lw=0.9)
                elif kind in ld_settings() and on["Dial / mode / settings"]:
                    ax.axvline(self.x(t), color="gray", lw=0.4, ls=":")
                    if first and kind == "dial":
                        ax.annotate("%.1f" % (float(val) / 1e3) if val else "", (self.x(t), 0.0), xycoords=("data", "axes fraction"),
                                    rotation=90, fontsize=6, color="gray", va="bottom", ha="left")

    def draw_calls(self, ax, run, first):
        """V1: a line where the call arrived, labelled on the top graph.  V2: the call small at the upper left of
        the over on every graph, from the moment it is decoded, and a line at the end of the over once that is known."""
        for c in ld.heard_calls(run):
            if not c["v2"]:
                ax.axvline(self.x(c["t_call"]), color="purple", lw=0.9)
                if first:
                    ax.annotate(c["call"], (self.x(c["t_call"]), 1.0), xycoords=("data", "axes fraction"),
                                rotation=90, fontsize=7, color="purple", va="top", ha="right")
                continue
            ax.annotate(c["call"], (self.x(c["t_start"]), 1.0), xycoords=("data", "axes fraction"), xytext=(2, -2),
                        textcoords="offset points", fontsize=6, color="purple", va="top", ha="left", clip_on=True)
            if c["t_end"] is not None:
                ax.axvline(self.x(c["t_end"]), color="purple", lw=0.9)
                if first:
                    ax.annotate(c["call"], (self.x(c["t_end"]), 1.0), xycoords=("data", "axes fraction"),
                                rotation=90, fontsize=7, color="purple", va="top", ha="right")

    # ---------------------------------------------------------------- zoom
    def set_xlim(self, a, b):
        if not self.axes:
            return
        if self.utc.get():
            self.axes[0].set_xlim(to_num(a), to_num(b))
        else:
            base = min(r.t0 for r in self.runs)
            self.axes[0].set_xlim(a - base, b - base)
        self.canvas.draw_idle()

    @staticmethod
    def run_end(r):
        """The last time the run has anything for: its tables, events and overs; never the current time."""
        ends = [r.t0] + [x[0] for x in r.events] + [o["t_end"] for o in r.overs + r.tx_overs]
        for data in (r.rx, r.tx):
            if "t" in data and len(data["t"]):
                tt = np.asarray(data["t"], dtype=float)
                tt = tt[np.isfinite(tt)]
                if len(tt):
                    ends.append(float(tt.max()))
        return max(ends)

    def fit_all(self):
        if not self.axes:
            return
        t = [r.t0 for r in self.runs]
        e = [self.run_end(r) for r in self.runs]
        self.set_xlim(min(t), max(e) + 1)
        self.yuser = {}
        self.replot()

    # ---------------------------------------------------------------- pointer and measuring
    def from_x(self, xv):
        if self.utc.get():
            return float(xv) * 86400.0
        return float(xv) + min(r.t0 for r in self.runs)

    def on_move(self, ev):
        if ev.inaxes is None or ev.xdata is None:
            return
        t = self.from_x(ev.xdata)
        self.readout.configure(text="%s   y = %.3f" % (ld.utc_str(t), ev.ydata) if not self.measure or len(self.measure) >= 2
                               else "A set - click B   (Esc clears)   %s" % ld.utc_str(t))

    def clear_measure(self):
        for ax in self.axes:
            for l in getattr(ax, "_mlines", []):
                try:
                    l.remove()
                except ValueError:
                    pass
            ax._mlines = []
        self.measure = []
        self.readout.configure(text="Measure: click A, then B" if self.mvar.get() else "")
        self.canvas.draw_idle()

    def on_click(self, ev):
        if not self.mvar.get() or ev.inaxes is None or ev.xdata is None or self.toolbar.mode != "":
            return
        if len(self.measure) >= 2:
            self.clear_measure()
        idx = self.axes.index(ev.inaxes)
        self.measure.append((ev.xdata, idx, ev.ydata))
        for ax in self.axes:
            ax._mlines = getattr(ax, "_mlines", []) + [ax.axvline(ev.xdata, color="black", lw=1.0)]
        if len(self.measure) == 2:
            self.readout.configure(text=self.measure_text())
        self.canvas.draw_idle()

    def measure_text(self):
        (xa, ia, ya), (xb, ib, yb) = self.measure
        ta, tb = self.from_x(xa), self.from_x(xb)
        parts = ["dt = %.3f s (A %s  B %s)" % (tb - ta, ld.utc_str(ta)[11:], ld.utc_str(tb)[11:])]
        if ia == ib:
            parts.append("dy = %+.3f (%s)" % (yb - ya, self.selected_params()[ia][0]))
        out = ["   ".join(parts)]
        for label, table, series in self.selected_params():
            for ri, run in enumerate(self.runs):
                data = run.rx if table == "rx" else run.tx
                if "t" not in data:
                    continue
                for col, leg, _s in series:
                    st = ld.window_stats(data["t"], data.get(col, np.array([])), ta, tb) if col in data else None
                    if st:
                        out.append("  run %d %s %s: n=%d mean %.2f min %.2f max %.2f" % (ri + 1, label, leg, st["n"], st["mean"], st["min"], st["max"]))
        text = "\n".join(out)
        self.set_text(text)
        return parts[0] + ("   " + parts[1] if len(parts) > 1 else "")


def ld_settings():
    return ("dial", "mode", "wave", "invert", "drive", "trim", "agc")


def main(argv):
    # Runs that ended without piHPSDR stopping them cleanly (killed, crashed) are left as directories: finish every one but the newest
    # (compress, check, remove the directory) before anything is opened. What was named on the command line is left alone.
    try:
        tidy_logs.tidy(tidy_logs.default_root(), keep=[a for a in argv[1:]])
    except Exception as e:                                   # never stops the viewer from starting
        print("tidy_logs: %s" % e)
    root = tk.Tk()
    Viewer(root, argv[1:])
    root.mainloop()


if __name__ == "__main__":
    main(sys.argv)

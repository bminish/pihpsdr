#!/usr/bin/env python3
"""RADE log viewer: zoomable graphs of the parameter logs written by piHPSDR (rade-logs/<run>/).

    rade_logview.py [run-directory | run.tar.gz | folder-of-runs ...]

Pick runs (Open), tick the parameters to plot, zoom/pan with the toolbar, tick Measure: a click puts a cursor on every graph and
lists the value of every line there, a drag draws a box over the same times on every graph and lists max, mean and min of every line in it.
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
DRAG_PX = 5                         # a press and release less than this far apart is a click, not a drag
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
        self.measure = []
        self.drag = None                   # a press waiting for its release: (pixel x, time_num)
        self.marks_on = False              # a cursor or range is on the graphs
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
        self.full = tk.BooleanVar(value=True)
        ttk.Checkbutton(left, text="Every call of librade (rx_full.csv), where a run has it", variable=self.full,
                        command=self.replot).pack(anchor="w", padx=4)
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
        ttk.Checkbutton(tb, text="Measure (click: values, drag: range)", variable=self.mvar,
                        command=self.measure_toggled).pack(side="left", padx=8)
        self.canvas.get_tk_widget().pack(side="top", fill="both", expand=True)
        self.readout = ttk.Label(right, text="", anchor="w", font=("monospace", 9))
        self.readout.pack(fill="x")
        self.text = tk.Text(right, height=13, wrap="none", font=("monospace", 9))
        self.text.pack(fill="x")
        self.text.configure(state="disabled")
        self.canvas.mpl_connect("motion_notify_event", self.on_move)
        self.canvas.mpl_connect("button_press_event", self.on_press)
        self.canvas.mpl_connect("button_release_event", self.on_release)
        self.canvas.mpl_connect("scroll_event", self.on_scroll)
        root.bind("<Key-Escape>", lambda e: self.clear_measure())
        root.bind("<F5>", lambda e: self.update_live())
        for p in paths:
            self.add_any(p)
        self.replot()

    # ---------------------------------------------------------------- runs
    def open_dialog(self):
        ps = filedialog.askopenfilenames(title="Open runs: choose files in run directories and/or .tar.gz (several at once)",
                                         initialdir=self.initialdir(),
                                         filetypes=[("RADE logs", "*.csv *.txt *.tar.gz *.json *.wav"), ("all", "*")])
        for p in ps:
            self.add_any(p)
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
        frame = ttk.Frame(top, padding=6)
        frame.pack(fill="both", expand=True)
        box = ttk.Frame(frame)
        box.pack(fill="both", expand=True, pady=(0, 6))
        lb = tk.Listbox(box, selectmode="extended", width=70, height=min(20, max(5, len(found))), exportselection=False)
        sb_y = ttk.Scrollbar(box, orient="vertical", command=lb.yview)
        sb_x = ttk.Scrollbar(box, orient="horizontal", command=lb.xview)
        lb.configure(yscrollcommand=sb_y.set, xscrollcommand=sb_x.set)
        lb.grid(row=0, column=0, sticky="nsew")
        sb_y.grid(row=0, column=1, sticky="ns")
        sb_x.grid(row=1, column=0, sticky="ew")
        box.grid_rowconfigure(0, weight=1)
        box.grid_columnconfigure(0, weight=1)
        common = os.path.commonpath([os.path.abspath(q) for q in found]) if len(found) > 1 else ""
        for q in found:                                       # runs nested in subfolders are told apart by their path
            rel = os.path.relpath(os.path.abspath(q), common) if common else os.path.basename(q)
            lb.insert("end", rel if rel != "." else os.path.basename(q))
        lb.selection_set(0, "end")
        chosen = []

        def ok():
            chosen.extend(found[i] for i in lb.curselection())
            top.destroy()
        sel = ttk.Frame(frame); sel.pack(fill="x", pady=(0, 6))
        ttk.Button(sel, text="Select all", command=lambda: lb.selection_set(0, "end")).pack(side="left")
        ttk.Button(sel, text="Clear all", command=lambda: lb.selection_clear(0, "end")).pack(side="left", padx=4)
        row = ttk.Frame(frame); row.pack(fill="x")
        ttk.Button(row, text="Open", command=ok).pack(side="right")
        ttk.Button(row, text="Cancel", command=top.destroy).pack(side="right", padx=4)
        top.bind("<Return>", lambda e: ok())
        top.bind("<Escape>", lambda e: top.destroy())
        top.grab_set()
        self.root.wait_window(top)
        return chosen

    def add_any(self, p, choose=False):
        """A run (directory, file in one, .tar.gz) or a folder of runs: all of them are opened (choose: pick from the list)."""
        found = ld.find_runs(p)
        if not found:
            if os.path.isdir(p):
                self.error("Cannot open", "%s\n\nno run (a directory with meta.txt) or .tar.gz in it, at any depth" % p)
            else:
                self.add_path(p)                              # not a run file: load_run says why
            return
        if choose and len(found) > 1 and not getattr(self, "quiet", False):
            found = self.pick_runs(found)
        for q in found:
            self.add_path(q)

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
                new = ld.load_run(r.source, prev=r)
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
                data = self.rx_data(run) if table == "rx" else run.tx
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

    def rx_data(self, run):
        """The receive columns to plot: rx.csv (2 Hz, in sync), or with the box ticked and the run holding it, rx_full.csv: every call of
        librade, the values blanked where there was no lock. snr_min has no full-rate column (each point is its own minimum)."""
        f = run.rxf
        if not self.full.get() or "t" not in f or "sync" not in f:
            return run.rx
        locked = f["sync"] == 1
        out = {"t": f["t"]}
        for col, src in (("snr_mean", "snr_db"), ("foff_mean", "foff_hz"), ("delta_hat", "delta_hat"), ("est_foff", "est_foff"),
                         ("gain", "gain"), ("snr_est", "snr_est")):
            if src in f:
                out[col] = np.where(locked, f[src], np.nan)
        return out

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
                for t in self.eoo_times(run):
                    ax.axvline(self.x(t), color="#d97777", alpha=0.7, lw=0.85)
            for t, kind, val in run.events:
                if kind in ("key_down", "key_up") and on["Key down/up"]:
                    ax.axvline(self.x(t), color="green" if kind == "key_down" else "firebrick", lw=0.7, ls="--")
                elif kind in ld_settings() and on["Dial / mode / settings"]:
                    ax.axvline(self.x(t), color="gray", lw=0.4, ls=":")
                    if first and kind == "dial":
                        ax.annotate("%.1f" % (float(val) / 1e3) if val else "", (self.x(t), 0.0), xycoords=("data", "axes fraction"),
                                    rotation=90, fontsize=6, color="gray", va="bottom", ha="left")

    @staticmethod
    def eoo_times(run):
        """Where an end-of-over burst was detected: the eoo events, and the end of an over whose row says it was seen
        when it has no event.  An over that ended by fading or dropout has neither, so it is not marked."""
        times = [t for t, kind, _v in run.events if kind == "eoo"]
        for o in run.overs:
            if o.get("eoo") == 1 and not any(o["t_start"] <= t <= o["t_end"] + ld.CALL_WAIT_S for t in times):
                times.append(o["t_end"])
        return sorted(times)

    def draw_calls(self, ax, run, first):
        """V1: the call block of the end-of-over frame, decoded: a solid green line where it arrived.  V2: the first
        time the call is decoded in an over: a dotted green line.  Both are labelled on the top graph."""
        for c in ld.heard_calls(run):
            ax.axvline(self.x(c["t_call"]), color="green", lw=1.2 if c["v2"] else 1.0, ls=":" if c["v2"] else "-")
            if first:
                ax.annotate(c["call"], (self.x(c["t_call"]), 1.0), xycoords=("data", "axes fraction"),
                            rotation=90, fontsize=7, color="green", va="top", ha="right")

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
        if self.drag is not None and ev.xdata is not None:
            self.drag_last = ev.xdata
            if abs(ev.x - self.drag[0]) >= DRAG_PX:                  # the box follows the pointer
                self.mark_range(self.drag[1], ev.xdata, final=False)
            return
        if ev.inaxes is None or ev.xdata is None:
            return
        t = self.from_x(ev.xdata)
        self.readout.configure(text="%s   y = %.3f" % (ld.utc_str(t), ev.ydata) if not self.mvar.get() or not self.marks_on else
                               self.readout.cget("text"))

    def clear_measure(self):
        for ax in self.axes:
            for l in getattr(ax, "_mlines", []):
                try:
                    l.remove()
                except ValueError:
                    pass
            ax._mlines = []
        self.measure = []
        self.marks_on = False
        self.drag = None
        self.readout.configure(text="Measure: click for the values at a time, drag for max / mean / min over a range" if self.mvar.get() else "")
        self.canvas.draw_idle()

    def measure_toggled(self):
        """Measure on: the toolbar's pan and zoom tools are let go, so the pointer is the cursor."""
        if self.mvar.get():
            if self.toolbar.mode == "pan/zoom":
                self.toolbar.pan()
            elif self.toolbar.mode == "zoom rect":
                self.toolbar.zoom()
        self.clear_measure()

    def on_scroll(self, ev):
        """The wheel zooms the time axis about the pointer (all graphs share it); with Ctrl, the level axis of the graph under the pointer."""
        if ev.inaxes is None or ev.xdata is None or ev.ydata is None or not ev.step:
            return
        f = 1.25 ** (-ev.step)                     # wheel up: in
        if ev.key == "control":
            lo, hi = ev.inaxes.get_ylim()
            ev.inaxes.set_ylim(ev.ydata - (ev.ydata - lo) * f, ev.ydata + (hi - ev.ydata) * f)
        else:
            lo, hi = self.axes[0].get_xlim()
            self.axes[0].set_xlim(ev.xdata - (ev.xdata - lo) * f, ev.xdata + (hi - ev.xdata) * f)
        self.canvas.draw_idle()

    def on_press(self, ev):
        if not self.mvar.get() or ev.button != 1 or ev.inaxes is None or ev.xdata is None or self.toolbar.mode != "":
            return
        self.drag = (ev.x, ev.xdata)
        self.drag_last = ev.xdata

    def on_release(self, ev):
        if self.drag is None:
            return
        (px, x0), self.drag = self.drag, None
        x1 = ev.xdata if ev.xdata is not None else self.drag_last
        if abs(ev.x - px) < DRAG_PX or x1 is None:
            self.mark_cursor(x0)
        else:
            self.mark_range(x0, x1, final=True)

    def _clear_marks(self):
        for ax in self.axes:
            for l in getattr(ax, "_mlines", []):
                try:
                    l.remove()
                except ValueError:
                    pass
            ax._mlines = []

    def mark_cursor(self, xv):
        """One cursor on every graph; the value of every plotted series there goes to the text area."""
        self._clear_marks()
        for ax in self.axes:
            ax._mlines = [ax.axvline(xv, color="black", lw=1.0)]
        self.marks_on = True
        tc = self.from_x(xv)
        out = ["Cursor  %s" % ld.utc_str(tc)]
        for ri, label, leg, t, y in self.series_data():
            i = int(np.searchsorted(t, tc))
            j = min(range(max(i - 1, 0), min(i + 1, len(t))), key=lambda k: abs(t[k] - tc), default=None) if len(t) else None
            ok = j is not None and abs(t[j] - tc) <= ld.GAP_S and not np.isnan(y[j])
            out.append("  run %d  %-26s %-8s %s" % (ri + 1, label, leg, "%10.3f   (sample %+.2f s)" % (y[j], t[j] - tc) if ok else "      no data"))
        self.set_text("\n".join(out))
        self.readout.configure(text="Cursor %s" % ld.utc_str(tc)[11:])
        self.canvas.draw_idle()

    def mark_range(self, xa, xb, final):
        """A box over the same times on every graph; max, mean and min of every plotted series in it (when the drag is finished)."""
        self._clear_marks()
        lo, hi = min(xa, xb), max(xa, xb)
        for ax in self.axes:
            ax._mlines = [ax.axvspan(lo, hi, color="black", alpha=0.12, lw=0), ax.axvline(lo, color="black", lw=0.8), ax.axvline(hi, color="black", lw=0.8)]
        self.marks_on = True
        ta, tb = sorted((self.from_x(xa), self.from_x(xb)))
        self.readout.configure(text="Range dt = %.3f s  (%s to %s)" % (tb - ta, ld.utc_str(ta)[11:], ld.utc_str(tb)[11:]))
        if final:
            out = ["Range  %s  to  %s   dt = %.3f s" % (ld.utc_str(ta), ld.utc_str(tb)[11:], tb - ta),
                   "  %-5s %-26s %-8s %7s %10s %10s %10s" % ("run", "graph", "line", "n", "max", "mean", "min")]
            for ri, label, leg, t, y in self.series_data():
                st = ld.window_stats(t, y, ta, tb)
                out.append("  run %d %-26s %-8s %7s %10s %10s %10s" % (ri + 1, label, leg, *(
                    (st["n"], "%.3f" % st["max"], "%.3f" % st["mean"], "%.3f" % st["min"]) if st else ("0", "-", "-", "-"))))
            self.set_text("\n".join(out))
        self.canvas.draw_idle()

    def series_data(self):
        """(run index, graph label, line name, t, y) for every line on the graphs, as plotted."""
        for label, table, series in self.selected_params():
            for ri, run in enumerate(self.runs):
                data = self.rx_data(run) if table == "rx" else run.tx
                if "t" not in data:
                    continue
                for col, leg, _s in series:
                    if col in data:
                        yield ri, label, leg, data["t"], data[col]


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

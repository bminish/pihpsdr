#!/usr/bin/env python3
"""cross-run report from the per-trial csv files written by `cwlat_analyze.py all`:
repeat-point curve, cold-start delay, clipping, dropped taps, sidetone lead; writes report.md and plots."""
import csv, glob, json, os, sys
from collections import defaultdict
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src = sys.argv[1] if len(sys.argv) > 1 else "results/analysis"
runs = {}
for f in sorted(glob.glob(os.path.join(src, "*.trials.csv"))):
    name = os.path.basename(f)[:-len(".trials.csv")]
    path, key, mode, q, test = name.split("_")
    rows = []
    for r in csv.DictReader(open(f)):
        rows.append({k: (float(v) if v not in ("", None) else np.nan) for k, v in r.items()})
    runs[name] = dict(path=path, key=key, mode=mode, q=int(q[1:]), test=test, rows=rows)

def col(rows, k):
    return np.array([r.get(k, np.nan) for r in rows], float)

out = ["# cwlat cross-run report", ""]

# ---- 1. repeat point (keyer repeats a dit if the paddle is still down at the end of dot+gap)
out += ["## Keyer repeat point (dit taps, all runs, RF element count)", "",
        "| hold ms | taps | 1 element | 2+ elements | none (dropped) |", "|---|---|---|---|---|"]
byhold = defaultdict(lambda: [0, 0, 0, 0])
for n, R in runs.items():
    if R["test"] != "dit":
        continue
    for r in R["rows"]:
        h = int(round(r["hold_ms"]))
        b = byhold[h]
        b[0] += 1
        nel = r.get("rf_nel", np.nan)
        if nel == 0: b[3] += 1
        elif nel == 1: b[1] += 1
        elif nel >= 2: b[2] += 1
curve = []
for h in sorted(byhold):
    t, e1, e2, dr = byhold[h]
    out.append(f"| {h} | {t} | {e1} ({100*e1/t:.0f} %) | {e2} ({100*e2/t:.0f} %) | {dr} |")
    if t - dr > 0:
        curve.append((h, e2 / (t - dr)))
out.append("")
# 50 % crossing by linear interpolation
thr = None
for (h0, p0), (h1, p1) in zip(curve, curve[1:]):
    if p0 < 0.5 <= p1:
        thr = h0 + (0.5 - p0) / (p1 - p0) * (h1 - h0)
out.append(f"Hold at which half of the taps give a second dit: {thr:.0f} ms (nominal dot+gap = 109 ms at 22 wpm)." if thr else "no 50 % crossing")
out.append("")
fig, ax = plt.subplots(figsize=(6, 3.5))
ax.plot([c[0] for c in curve], [100 * c[1] for c in curve], "o-")
ax.axvline(1200 / 22 * 2, color="gray", ls="--", label="2 x dot (109 ms)")
ax.set_xlabel("paddle hold (ms)"); ax.set_ylabel("% of taps with a second dit"); ax.legend(); ax.grid(alpha=0.3)
fig.tight_layout(); os.makedirs(os.path.join(src, "plots"), exist_ok=True)
fig.savefig(os.path.join(src, "plots", "repeat_curve.png"), dpi=110); plt.close(fig)

# ---- 2. RF latency overall, by path / Q (warm)
out += ["## End-to-end latency, MIDI press to RF carrier (50 % point), warm taps", "",
        "| path | Q | n | mean | sd | p5 | p50 | p95 | max |", "|---|---|---|---|---|---|---|---|---|"]
agg = defaultdict(list)
for n, R in runs.items():
    for r in R["rows"]:
        if r.get("cold") == 0 and R["test"] in ("dit", "dah", "straight"):
            agg[(R["path"], R["q"])].append(r["rf_lat50_ms"])
for (p, q), v in sorted(agg.items()):
    v = np.array(v)
    out.append(f"| {p} | {q} | {len(v)} | {v.mean():.2f} | {v.std():.2f} | {np.percentile(v,5):.2f} | {np.percentile(v,50):.2f} | {np.percentile(v,95):.2f} | {v.max():.2f} |")
out.append("")

# ---- 3. sidetone lead over RF
out += ["## Sidetone (at the sink) relative to RF, warm taps", "",
        "Positive = sidetone earlier than RF.", "",
        "| path | Q | n | RF − sink (ms) mean±sd | sink onset | RF onset |", "|---|---|---|---|---|---|"]
lead = defaultdict(lambda: ([], [], []))
for n, R in runs.items():
    for r in R["rows"]:
        if r.get("cold") == 0 and np.isfinite(r.get("rf_minus_sink_lat_ms", np.nan)):
            a = lead[(R["path"], R["q"])]
            a[0].append(r["rf_minus_sink_lat_ms"]); a[1].append(r["sink_lat50_ms"]); a[2].append(r["rf_lat50_ms"])
for (p, q), (d, s, rf) in sorted(lead.items()):
    out.append(f"| {p} | {q} | {len(d)} | {np.mean(d):.1f}±{np.std(d):.1f} | {np.mean(s):.1f} | {np.mean(rf):.1f} |")
out.append("")
fig, ax = plt.subplots(figsize=(6, 3.5))
for p, c in (("old", "tab:orange"), ("synth", "tab:blue")):
    qs = sorted(q for (pp, q) in lead if pp == p)
    ax.plot(qs, [np.mean(lead[(p, q)][1]) for q in qs], "o-", color=c, label=f"{p}: sidetone at sink")
ax.axhline(np.mean([np.mean(v[2]) for v in lead.values()]), color="k", ls="--", label="RF carrier")
ax.set_xscale("log", base=2); ax.set_xlabel("PipeWire quantum (frames)"); ax.set_ylabel("ms after MIDI press")
ax.legend(); ax.grid(alpha=0.3); fig.tight_layout(); fig.savefig(os.path.join(src, "plots", "sidetone_vs_rf.png"), dpi=110); plt.close(fig)

# ---- 4. cold start (break-in RX->TX)
out += ["## Break-in cold starts (first element after a pause > 400 ms)", "",
        "| key | path | n cold | RF onset cold mean±sd | warm mean | extra delay | p5..p95 cold |", "|---|---|---|---|---|---|---|"]
cs = defaultdict(lambda: ([], []))
for n, R in runs.items():
    if R["mode"] != "breakin": continue
    for r in R["rows"]:
        if r.get("cold") == 1: cs[(R["key"], R["path"])][0].append(r["rf_lat50_ms"])
        if r.get("cold") == 0: cs[(R["key"], R["path"])][1].append(r["rf_lat50_ms"])
allcold = []
for (k, p), (c, w) in sorted(cs.items()):
    c, w = np.array(c), np.array(w)
    allcold += list(c)
    out.append(f"| {k} | {p} | {len(c)} | {c.mean():.1f}±{c.std():.1f} | {w.mean():.1f} | {c.mean()-w.mean():.1f} | {np.percentile(c,5):.0f}..{np.percentile(c,95):.0f} |")
out.append("")
fig, ax = plt.subplots(figsize=(6, 3.5))
ax.hist(allcold, bins=40, color="tab:red", alpha=0.8)
ax.set_xlabel("RF onset latency of first element after a pause (ms)"); ax.set_ylabel("taps"); ax.grid(alpha=0.3)
fig.tight_layout(); fig.savefig(os.path.join(src, "plots", "cold_start_hist.png"), dpi=110); plt.close(fig)

# ---- 5. clipping of straight-key elements
out += ["## Element clipping, straight key, break-in (RF element minus commanded hold)", "",
        "| class | n | mean | sd | min |", "|---|---|---|---|---|"]
warm, cold = [], []
xs, ys = [], []
for n, R in runs.items():
    if R["key"] == "straight" and R["mode"] == "breakin":
        wm = np.nanmean([r["rf_lat50_ms"] for r in R["rows"] if r.get("cold") == 0])
        for r in R["rows"]:
            v = r.get("rf_len0_minus_ref_ms", np.nan)
            if not np.isfinite(v): continue
            (cold if r.get("cold") == 1 else warm).append(v)
            if r.get("cold") == 1: xs.append(r["rf_lat50_ms"] - wm); ys.append(v)
for nm, v in (("warm", warm), ("cold", cold)):
    v = np.array(v); out.append(f"| {nm} | {len(v)} | {v.mean():+.2f} | {v.std():.2f} | {v.min():+.1f} |")
slope = np.polyfit(xs, ys, 1)[0]
out += ["", f"Length change per ms of extra onset delay on cold taps: {slope:+.2f} (−1.00 = the tone ends on time, only its start is late).", ""]
fig, ax = plt.subplots(figsize=(6, 3.5))
ax.scatter(xs, ys, s=10)
ax.set_xlabel("extra onset delay vs warm taps (ms)"); ax.set_ylabel("RF element minus commanded hold (ms)"); ax.grid(alpha=0.3)
fig.tight_layout(); fig.savefig(os.path.join(src, "plots", "clipping_scatter.png"), dpi=110); plt.close(fig)

# ---- 6. iambic element accuracy
out += ["## Iambic element accuracy (RF element minus nominal 22 wpm dit/dah)", "",
        "| test | n | mean | sd | min | max |", "|---|---|---|---|---|---|"]
for t in ("dit", "dah"):
    v = np.array([r["rf_len0_minus_ref_ms"] for n, R in runs.items() if R["test"] == t for r in R["rows"]
                  if np.isfinite(r.get("rf_len0_minus_ref_ms", np.nan))])
    out.append(f"| {t} | {len(v)} | {v.mean():+.2f} | {v.std():.3f} | {v.min():+.2f} | {v.max():+.2f} |")
out.append("")
p01 = np.array([r["rf_period01_ms"] for n, R in runs.items() if R["test"] == "dit" for r in R["rows"] if np.isfinite(r.get("rf_period01_ms", np.nan))])
if len(p01):
    out += [f"Dit-to-dit period when the keyer repeats (nominal 109.1 ms): {p01.mean():.2f}±{p01.std():.2f} ms, n={len(p01)}.", ""]

# ---- 7. edges
rise = np.array([r["rf_rise_ms"] for R in runs.values() for r in R["rows"] if np.isfinite(r.get("rf_rise_ms", np.nan))])
fall = np.array([r["rf_fall_ms"] for R in runs.values() for r in R["rows"] if np.isfinite(r.get("rf_fall_ms", np.nan))])
ov = np.array([r["rf_overshoot_db"] for R in runs.values() for r in R["rows"] if np.isfinite(r.get("rf_overshoot_db", np.nan))])
out += ["## RF keying edges", "", f"10–90 % rise {rise.mean():.2f}±{rise.std():.2f} ms, fall {fall.mean():.2f}±{fall.std():.2f} ms, "
        f"overshoot {ov.mean():.2f} dB (max {ov.max():.2f}), n={len(rise)} elements.", ""]

# ---- 8. dropped taps
out += ["## Taps with no RF element (dropped), by test and position", "",
        "| run group | runs | taps | dropped | trial 0 dropped in | other dropped holds (ms) |", "|---|---|---|---|---|---|"]
grp = defaultdict(lambda: [0, 0, 0, 0, []])
for n, R in runs.items():
    g = grp[(R["key"], R["mode"], R["test"])]
    g[0] += 1
    for r in R["rows"]:
        g[1] += 1
        if r.get("rf_nel", 1) == 0:
            g[2] += 1
            if int(r["trial"]) == 0: g[3] += 1
            else: g[4].append(int(round(r["hold_ms"])))
for k, g in sorted(grp.items()):
    out.append(f"| {'/'.join(k)} | {g[0]} | {g[1]} | {g[2]} | {g[3]} of {g[0]} | {sorted(g[4])} |")
out.append("")
so = defaultdict(int)
for n, R in runs.items():
    for r in R["rows"]:
        if r.get("rf_nel", 0) > 0 and r.get("sink_dropped", 0) == 1: so[(R["path"], R["key"], R["mode"])] += 1
out += ["Taps with RF keyed but NO sidetone at the sink:", ""]
out += [f"- {k[0]} / {k[1]} / {k[2]}: {v}" for k, v in sorted(so.items())] or ["- none"]
out.append("")
open(os.path.join(src, "report.md"), "w").write("\n".join(out))
print("\n".join(out))

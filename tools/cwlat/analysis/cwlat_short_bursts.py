#!/usr/bin/env python3
"""cwlat --test short: was each tap keyed, and did it reach full amplitude?

For each trial the carrier envelope (from cwlat_analyze.py's Kiwi analysis) is taken in the window 20 to 120 ms after the
press, relative to the 99.5th percentile of the whole recording. A tap is "full" if that peak is at least 0.9, a "burst" if it
is above the noise floor but below 0.9 (a pulse that never reached its plateau), and "none" if it is not above the noise floor.
The noise floor is measured in 100 ms windows that lie 600 to 700 ms before each press (key up, hang time long over); a burst
has to exceed 1.2 times the largest of those, and at least 0.4.

usage: python3 -I cwlat_short_bursts.py LABEL=CAPTURE_DIR [LABEL=CAPTURE_DIR ...]
"""
import importlib.util
import os
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cwlat_analyze", os.path.join(here, "cwlat_analyze.py"))
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


def classify(d):
    name = os.path.basename(os.path.normpath(d))
    info, out, quality, segs, x = a.analyse_run(d, keep=True)
    env = np.asarray(x["env"])
    tu = x["kt"].utc(np.arange(len(env)))
    ev = a.load_events(os.path.join(d, name + ".events.csv"))
    tp = {}
    for e in ev:
        if e["on"] and e["t"] > 0 and e["trial"] >= 0:
            tp.setdefault(e["trial"], e["t"])
    plateau = np.percentile(env, 99.5)
    peaks, noise = {}, []
    for row in out:
        t0 = tp[row["trial"]]
        m = (tu >= t0 + 0.020) & (tu <= t0 + 0.120)
        peaks[row["trial"]] = (row["hold_ms"], env[m].max() / plateau)
        m2 = (tu >= t0 - 0.700) & (tu <= t0 - 0.600)
        noise.append(env[m2].max() / plateau)
    thr = max(1.2 * max(noise), 0.4)
    return peaks, thr, max(noise), quality


for arg in sys.argv[1:]:
    label, d = arg.split("=", 1)
    peaks, thr, nmax, q = classify(d)
    P = [p for _, p in peaks.values()]
    print(f"== {label}: carrier {q['carrier_offset_hz']:+.1f} Hz, noise peak max {nmax:.2f}, burst threshold {thr:.2f}")
    print(f"   taps {len(P)}: full {sum(p >= 0.9 for p in P)}, burst {sum(thr <= p < 0.9 for p in P)}, none {sum(p < thr for p in P)}")
    by = {}
    for h, p in peaks.values():
        by.setdefault(h, []).append(p)
    print("   hold ms: full/burst/none  " + "  ".join(
        f"{h:.0f}: {sum(p >= 0.9 for p in v)}/{sum(thr <= p < 0.9 for p in v)}/{sum(p < thr for p in v)}" for h, v in sorted(by.items())))

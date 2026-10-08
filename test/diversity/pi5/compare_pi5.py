#!/usr/bin/env python3
"""
Compare a Pi 5 benchmark run with a baseline, and say whether anything
regressed or became a bottleneck.

    compare_pi5.py BASELINE NEW

BASELINE and NEW are each a results directory (the unpacked
results-<host>-<time>.tar.gz, or docs/bench/pi5-bundle-cm5), or a single
bench_cpu / pi_bench output file. Only the "TSV" lines are read.

What it checks, and what each verdict means:

  FAIL        the run is not valid or not sustainable: a row dropped blocks
              (the worker needed more than a block period), RADE V1 did not
              lock where it should or locked where it should not, or the
              bench's transform size disagreed with the engine's (NFFT-MISMATCH).
  REGRESSION  a bench_cpu row that exists in both runs costs clearly more now
              in BOTH the paced and the hot run: more than 25 % over the
              baseline AND more than 0.3 percentage points of a core (1.0 for
              RADE V1's search, whose cost varies from run to run by about a
              point). One pacing alone is shown as "noise?" and not counted:
              on the Pi 5 (ondemand governor) a single run moves a row by up
              to about +-35 % with no change to the code (2026-10-08: CW 768k
              was +28 % paced and -23 % hot in one run). The micro-benchmarks
              (pi_bench) must be over 60 % to count, for the same reason:
              unchanged decimator code read x1.41 and x1.48.
  BOTTLENECK  a row, new or old, costs more than BUDGET_PCT of one core
              (worker plus feeder), or a start / stop / restart holds the
              caller longer than SLOW_MS.
  NEW         a row with no baseline: shown with its figure, and checked only
              against the absolute budgets.
  gone        a baseline row that this run no longer has (the RADE V1
              AM-passband rows were dropped on 2026-10-08 on purpose).

Exit status 1 if anything is FAIL, REGRESSION or BOTTLENECK, else 0.
Python 3, standard library only.
"""

import os
import sys

BUDGET_PCT = 25.0       # of one core, worker + feeder, for one row
SLOW_MS = 50.0          # a start / stop / restart the UI thread waits for
REL = 1.25              # regression: ratio over the baseline ...
ABS_PTS = 0.3           # ... and points of a core over it
ABS_PTS_SEARCH = 1.0    # RADE V1 searching varies by about a point
REL_PIB = 1.6           # pi_bench micro-benchmarks: identical code read x1.48 once


def read_tsv(path):
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("TSV\t"):
                rows.append(line.rstrip("\n").split("\t")[1:])
    return rows


def collect(where):
    """Return {kind: [rows]} for a directory or a file."""
    files = []
    if os.path.isdir(where):
        # the unpacked results tarball has one more directory level
        for root, _dirs, names in os.walk(where):
            for n in names:
                if n.startswith(("bench_cpu", "pi_bench")) and n.endswith(".txt"):
                    files.append(os.path.join(root, n))
    else:
        files.append(where)
    out = {"cpu": [], "pib": []}
    for p in sorted(files):
        for r in read_tsv(p):
            if r and r[0] in ("paced", "hot"):
                out["cpu"].append(r)
            else:
                out["pib"].append(r)
    return out


def cpu_rows(rows):
    """bench_cpu rows: pacing, reference, rate, nfft, period, worker_ms,
    worker_pct, feeder_ms, feeder_pct, drops, state"""
    d = {}
    for r in rows:
        if r[0] == "pacing":
            continue
        if len(r) < 11:
            continue
        key = (r[0], r[1], int(r[2]))
        d[key] = dict(nfft=int(r[3]), worker=float(r[6]), feeder=float(r[8]),
                      drops=int(r[9]), state=r[10])
    return d


def pib_rows(rows):
    d = {}
    for r in rows:
        try:
            if r[0] == "fft" and len(r) >= 7:
                d[("fft", int(r[1]), r[2])] = dict(us=float(r[5]), pct=float(r[6]))
            elif r[0] == "dec" and len(r) >= 9:
                d[("dec", r[1], int(r[2]))] = dict(us=float(r[7]), pct=float(r[8]))
            elif r[0] == "combine" and len(r) >= 4:
                d[("combine", int(r[1]))] = dict(us=float(r[2]) / 1000.0, pct=float(r[3]))
            elif r[0] == "transition" and len(r) >= 8 and r[1] != "when":
                d[("transition", r[1], int(r[2]), float(r[3]))] = dict(
                    nfft=int(r[4]), start=float(r[5]), stop=float(r[6]), restart=float(r[7]))
        except ValueError:
            continue
    return d


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    base, new = collect(argv[1]), collect(argv[2])
    bc, nc = cpu_rows(base["cpu"]), cpu_rows(new["cpu"])
    bp, np_ = pib_rows(base["pib"]), pib_rows(new["pib"])
    bad = 0
    lines = []
    cand = {}       # (reference, rate) -> {pacing: text}, for the both-pacings rule

    def note(verdict, text):
        nonlocal bad
        if verdict in ("FAIL", "REGRESSION", "BOTTLENECK"):
            bad += 1
        lines.append("%-10s %s" % (verdict, text))

    # ---- bench_cpu ----------------------------------------------------
    print("bench_cpu: share of one core (worker + feeder), baseline -> new\n")
    print("%-6s %-22s %6s %6s %9s %9s %8s  %s" %
          ("pacing", "reference", "rate", "nfft", "base %", "new %", "delta", "verdict"))
    for key in sorted(nc):
        n = nc[key]
        tot = n["worker"] + n["feeder"]
        b = bc.get(key)
        tag = ""
        why = []
        if n["drops"] > 0:
            note("FAIL", "%s %s %dk: %d blocks dropped (worker needs more than a block period)" %
                 (key[0], key[1], key[2] // 1000, n["drops"]))
            tag = "FAIL"
        if n["state"] in ("NFFT-MISMATCH", "DID-NOT-LOCK", "UNEXPECTED-LOCK"):
            note("FAIL", "%s %s %dk: %s" % (key[0], key[1], key[2] // 1000, n["state"]))
            tag = "FAIL"
        if tot > BUDGET_PCT:
            note("BOTTLENECK", "%s %s %dk: %.1f %% of a core (budget %.0f %%)" %
                 (key[0], key[1], key[2] // 1000, tot, BUDGET_PCT))
            tag = tag or "BOTTLENECK"
        if b is None:
            tag = tag or "NEW"
            print("%-6s %-22s %5dk %6d %9s %9.2f %8s  %s" %
                  (key[0], key[1], key[2] // 1000, n["nfft"], "-", tot, "-", tag))
            continue
        bt = b["worker"] + b["feeder"]
        d = tot - bt
        tol = ABS_PTS_SEARCH if "search" in key[1] else ABS_PTS
        if tot > bt * REL and d > tol:
            cand.setdefault((key[1], key[2]), {})[key[0]] = \
                "%.2f %% -> %.2f %% (+%.2f points, x%.2f)" % (bt, tot, d, tot / bt if bt else 0)
            tag = tag or "over?"
        if b["nfft"] != n["nfft"]:
            lines.append("%-10s %s %s %dk: transform size %d -> %d" %
                         ("note", key[0], key[1], key[2] // 1000, b["nfft"], n["nfft"]))
        print("%-6s %-22s %5dk %6d %9.2f %9.2f %+8.2f  %s" %
              (key[0], key[1], key[2] // 1000, n["nfft"], bt, tot, d, tag or "ok"))
    for (ref, rate), per in sorted(cand.items()):
        both = all(((p, ref, rate) in nc and (p, ref, rate) in bc) for p in ("paced", "hot"))
        if both and len(per) == 2:
            note("REGRESSION", "%s %dk, paced and hot: paced %s; hot %s" %
                 (ref, rate // 1000, per["paced"], per["hot"]))
        else:
            for p, t in per.items():
                lines.append("%-10s %s %s %dk only in the %s run: %s (the other pacing does not agree)" %
                             ("noise?", p, ref, rate // 1000, p, t))
    present = set((k[0], k[1]) for k in nc)
    for ref in sorted(set((k[0], k[1]) for k in bc) - present):
        lines.append("%-10s %s %s (every rate; in the baseline only)" % ("gone", ref[0], ref[1]))

    # ---- pi_bench and transitions --------------------------------------
    print("\npi_bench kernels, transforms, decimators and the combine; start / stop / restart\n")
    print("%-34s %12s %12s  %s" % ("item", "base", "new", "verdict"))
    for key in sorted(np_, key=str):
        n = np_[key]
        b = bp.get(key)
        if key[0] == "transition":
            worst = max(n["start"], n["stop"], n["restart"])
            label = "%s %dk tau %.1f nfft %d" % (key[1], key[2] // 1000, key[3], n["nfft"])
            tag = "NEW"
            if worst > SLOW_MS:
                note("BOTTLENECK", "transition %s: start %.1f / stop %.1f / restart %.1f ms (UI waits)" %
                     (label, n["start"], n["stop"], n["restart"]))
                tag = "BOTTLENECK"
            print("%-34s %12s %12s  %s" % (label, "-", "%.1f/%.1f/%.1f" %
                                             (n["start"], n["stop"], n["restart"]), tag))
            continue
        label = " ".join(str(x) for x in key)
        if b is None:
            print("%-34s %12s %12.4f  NEW" % (label[:34], "-", n["pct"]))
            continue
        tag = "ok"
        if n["pct"] > b["pct"] * REL_PIB and n["pct"] - b["pct"] > ABS_PTS:
            note("REGRESSION", "%s: %.3f %% -> %.3f %% of a core" % (label, b["pct"], n["pct"]))
            tag = "REGRESSION"
        print("%-34s %12.4f %12.4f  %s" % (label[:34], b["pct"], n["pct"], tag))

    print("\n--- findings ---")
    if lines:
        print("\n".join(lines))
    else:
        print("none")
    print("\n%s" % ("NOT CLEAN: %d finding(s)" % bad if bad else "CLEAN: no regression, drop or bottleneck"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

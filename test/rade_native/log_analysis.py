#!/usr/bin/env python3
"""What the parameter logs (rade-logs/*.csv) hold, how often each column changes, and what a reduced log would cost.

  log_analysis.py [rade-logs/file.csv ... | rade-logs/<run>/ ...]   (default: every run directory, then every old single CSV)

For a run directory of the current format it lists the table sizes and the overs heard and sent. The rest is for the old single-file logs.

Prints, for the files together: rows and bytes by kind; how many transmit rows were while not keyed and receive rows while not in
sync; for each column the share of rows that differ from the previous row (in sync / not, keyed / not); how well a 1, 2 or 4 Hz
average of the SNR keeps the SNR; and the size of the reduced form proposed in docs/rade/logging.md (a receive sample table
at 2 Hz in sync and nothing out of sync but its transitions, transmit samples only while keyed at 2 Hz, events, and one row per
heard over with the frequency, time, SNR and offset statistics and the call), with the overs it would list.
"""
import csv, glob, os, sys, collections
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def load(f):
    meta, rows = {}, []
    for l in open(f):
        if l.startswith("#"):
            if ": " in l:
                k, v = l[2:].split(": ", 1); meta[k.strip()] = v.strip()
            continue
        rows.append(l)
    rd = csv.DictReader(rows)
    return meta, list(rd), sum(len(l) for l in rows)


def overs(rows, gap=1.5, min_s=1.0):
    """Runs of rx rows in sync, joined across gaps shorter than `gap` s: [(t0, t1, snrs, foffs)]"""
    out, cur, last = [], None, None
    for r in rows:
        if r["kind"] != "rx" or r["sync"] != "1":
            continue
        t = float(r["t"])
        if cur is None or t - last > gap:
            if cur and cur[1] - cur[0] >= min_s:
                out.append(cur)
            cur = [t, t, [], []]
        cur[1] = t; last = t
        cur[2].append(float(r["snr_db"])); cur[3].append(float(r["foff_hz"]))
    if cur and cur[1] - cur[0] >= min_s:
        out.append(cur)
    return out


def summarize_dir(d):
    """A run of the current format (a directory of tables): sizes, rows, and the overs it lists."""
    names = ["rx.csv", "tx.csv", "events.csv", "overs.csv", "tx_overs.csv", "meta.txt"]
    sizes = {n: os.path.getsize(os.path.join(d, n)) for n in names if os.path.exists(os.path.join(d, n))}
    rows = {}
    for n in names[:5]:
        p = os.path.join(d, n)
        if os.path.exists(p):
            rows[n] = max(0, sum(1 for _ in open(p)) - 1)
    print("%s: %d bytes in %s; rows %s" % (os.path.basename(d.rstrip("/")), sum(sizes.values()), len(sizes), rows))
    p = os.path.join(d, "overs.csv")
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            print("   heard %s  %7.1f s  %8s Hz %-5s V%s  SNR mean %5s min %5s  offset %6s Hz  call %s" % (
                r["utc_start"][:19], float(r["t_end"]) - float(r["t_start"]), r["dial_hz"], r["mode"], r["wave"][1:], r["snr_mean"], r["snr_min"],
                r["foff_mean"], r["call"] or "-"))
    p = os.path.join(d, "tx_overs.csv")
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            print("   sent  %s  %7.1f s  %8s Hz V%s  drive %s dB  loudness %s  peak %s  underruns %s" % (
                r["utc_start"][:19], float(r["t_end"]) - float(r["t_start"]), r["dial_hz"], r["wave"][1:], r["drive_db"], r["loud_mean"], r["peak_max"], r["underruns"]))


def main():
    dirs = [a for a in sys.argv[1:] if os.path.isdir(a)]
    if dirs or (not sys.argv[1:] and glob.glob(os.path.join(ROOT, "rade-logs", "*", "meta.txt"))):
        for d in dirs or sorted(os.path.dirname(x) for x in glob.glob(os.path.join(ROOT, "rade-logs", "*", "meta.txt"))):
            summarize_dir(d)
        if dirs:
            return
        print()
    files = [a for a in sys.argv[1:] if not os.path.isdir(a)] or sorted(glob.glob(os.path.join(ROOT, "rade-logs", "*.csv")))
    kinds, bytes_by, rows_all, n_files, size = collections.Counter(), collections.Counter(), [], 0, 0
    for f in files:
        meta, rows, b = load(f)
        n_files += 1; size += os.path.getsize(f)
        for r in rows:
            kinds[r["kind"]] += 1
        rows_all.append((f, meta, rows))
    print("%d files, %.2f MB; rows %s" % (n_files, size / 1e6, dict(kinds)))
    tx = [r for _, _, rows in rows_all for r in rows if r["kind"] == "tx"]
    rx = [r for _, _, rows in rows_all for r in rows if r["kind"] == "rx"]
    print("transmit rows while not keyed: %d of %d (%.0f%%); receive rows while not in sync: %d of %d (%.0f%%)" % (
        sum(r["tx"] != "1" for r in tx), len(tx), 100 * np.mean([r["tx"] != "1" for r in tx]) if tx else 0,
        sum(r["sync"] != "1" for r in rx), len(rx), 100 * np.mean([r["sync"] != "1" for r in rx]) if rx else 0))
    cols = ["snr_db", "foff_hz", "nout", "delta_hat", "est_foff", "gain", "snr_est", "in_fill", "out_fill", "rx_underruns", "rx_overruns", "rx_pos"]
    print("receive columns: share of rows that differ from the previous one (in sync | not in sync)")
    for c in cols:
        res = []
        for sel in (True, False):
            n = d = 0
            for _, _, rows in rows_all:
                g = [r for r in rows if r["kind"] == "rx" and (r["sync"] == "1") == sel]
                n += max(0, len(g) - 1); d += sum(1 for a, b in zip(g, g[1:]) if a[c] != b[c])
            res.append("%.2f" % (d / n) if n else "  - ")
        print("  %-13s %s | %s" % (c, res[0], res[1]))
    print("transmit columns while keyed:")
    for c in ["trim_db", "agc_db", "drive_db", "loud_mom", "loud_avg", "peak_dbfs", "limiting", "tx_underruns", "tx_out_fill", "tx_pcm_fill", "end_state"]:
        n = d = 0
        for _, _, rows in rows_all:
            g = [r for r in rows if r["kind"] == "tx" and r["tx"] == "1"]
            n += max(0, len(g) - 1); d += sum(1 for a, b in zip(g, g[1:]) if a[c] != b[c])
        print("  %-13s %s" % (c, "%.2f" % (d / n) if n else "  -"))
    # decimation of the SNR
    t = np.array([float(r["t"]) for _, _, rows in rows_all[-1:] for r in rows if r["kind"] == "rx" and r["sync"] == "1"])
    snr = np.array([float(r["snr_db"]) for _, _, rows in rows_all[-1:] for r in rows if r["kind"] == "rx" and r["sync"] == "1"])
    if len(snr) > 50:
        print("SNR kept by an average per bin (the last file, %d in-sync rows):" % len(snr))
        for hz in (1, 2, 4):
            b = np.floor(t * hz).astype(int)
            dev = np.concatenate([snr[b == u] - snr[b == u].mean() for u in np.unique(b)])
            print("  %d Hz: %d rows, within-bin sd %.2f dB, worst %.1f dB" % (hz, len(np.unique(b)), dev.std(), np.abs(dev).max()))
    # the reduced form
    new_rx = new_tx = 0
    listing = []
    for f, meta, rows in rows_all:
        o = overs(rows)
        for a in o:
            n = int((a[1] - a[0]) * 2) + 1
            new_rx += n * 36
            listing.append((os.path.basename(f), a[0], a[1] - a[0], np.mean(a[2]), np.min(a[2]), np.mean(a[3])))
        k = [r for r in rows if r["kind"] == "tx" and r["tx"] == "1"]
        if k:
            new_tx += (int((float(k[-1]["t"]) - float(k[0]["t"])) * 2) + 1) * 40
    ev = kinds["ev"]
    print("reduced form: receive %.0f KB + transmit %.0f KB + events %d x 90 B + one row per over (%d, ~120 B) = %.0f KB, against %.0f KB now"
          % (new_rx / 1e3, new_tx / 1e3, ev, len(listing), (new_rx + new_tx + ev * 90 + len(listing) * 120) / 1e3, size / 1e3))
    print("the overs it would list (file, t_start s, duration s, SNR mean, SNR min, offset mean Hz):")
    for x in listing[:12]:
        print("  %-28s %7.1f %6.1f %6.1f %6.1f %7.1f" % x)


if __name__ == "__main__":
    main()

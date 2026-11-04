#!/usr/bin/env python3
"""Convert the old single-file parameter logs (rade-logs/<time>_RADE.csv) to the run-directory format
(rade-logs/<time>/ with meta.txt, rx.csv, tx.csv, events.csv, overs.csv, tx_overs.csv, and <time>.tar.gz).

    convert_old_logs.py [rade-logs-dir | old.csv ...]     (default: ../../rade-logs)

The rules are those of src/rade_native_log.c (2 Hz means, in sync / keyed only, overs of at least 1 s with gaps under
1.5 s joined, transmitted overs ended by 0.5 s without a keyed row). What the old format never recorded stays empty:
the end-of-over flag and the call (so `eoo` and `call` are blank in overs.csv and there are no eoo / call events).
An existing run directory of the same name is never overwritten. The old files are left where they are.
"""
import csv, datetime, glob, math, os, sys, tarfile

GAP, MIN_S, TX_GAP = 1.5, 1.0, 0.5
M64 = 1 << 64


def fnum(s, d=math.nan):
    try:
        return float(s)
    except (TypeError, ValueError):
        return d


def utc(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def read_old(path):
    meta, lines = {}, []
    for l in open(path, newline=""):
        if l.startswith("#"):
            if ": " in l:
                k, v = l[2:].rstrip("\n").split(": ", 1)
                meta[k.strip()] = v.strip()
        else:
            lines.append(l)
    return meta, list(csv.DictReader(lines))


def convert(path, outdir):
    meta, rows = read_old(path)
    stem = os.path.basename(path)[:-len(".csv")].replace("_RADE", "")
    dest = os.path.join(outdir, stem)
    if os.path.exists(dest):
        return "exists, skipped: " + stem
    if not rows:
        return "no rows: " + path
    t0 = fnum(rows[0]["t_unix"]) - fnum(rows[0]["t"])
    ev = []                                           # (t, kind, value)
    def event(t, k, v=""):
        ev.append((t, k, v))
    ctx = {"wave": {"V1": 1, "V2": 2}.get(meta.get("waveform"), 0), "dial": int(fnum(meta.get("dial_hz"), 0)),
           "mode": meta.get("mode", ""), "invert": int(fnum(meta.get("invert"), 0))}
    ctx_at = [(-1.0, dict(ctx))]                      # settings in force, by time
    last = {}
    def setting(t, k, v):
        if last.get(k) != v:
            last[k] = v
            event(t, k, str(v))
    # settings events of the old format (kind ev: "wave=1 dial=... mode=... invert=... tx=...")
    for r in rows:
        if r["kind"] != "ev":
            continue
        t = fnum(r["t"])
        kv = dict(p.split("=", 1) for p in r["note"].split() if "=" in p)
        c = dict(ctx_at[-1][1])
        if "wave" in kv: c["wave"] = int(kv["wave"])
        if "dial" in kv: c["dial"] = int(kv["dial"])
        if "mode" in kv: c["mode"] = kv["mode"]
        if "invert" in kv: c["invert"] = int(kv["invert"])
        for k in ("wave", "dial", "mode", "invert"):
            if k in kv:
                setting(t, k, c[k])
        if "tx" in kv:
            setting(t, "tx_enabled", int(kv["tx"]))
        ctx_at.append((t, c))
    def ctx_for(t):
        c = ctx_at[0][1]
        for tt, cc in ctx_at:
            if tt <= t: c = cc
        return c
    # receive
    rx_out, overs = [], []
    ov = None
    binn, acc = None, None
    last_under = last_overrun = 0
    def flush_bin():
        nonlocal acc, binn
        if acc and acc["n"]:
            n = acc["n"]
            v2 = acc["v2"]
            rx_out.append("%.2f,%.1f,%.1f,%.1f,%d,%s" % (binn * 0.5 + 0.25, acc["snr"] / n, acc["min"], acc["foff"] / n, n,
                          "%.4f,%.4f,%.2f,%.3f" % tuple(acc[k] / n for k in ("dh", "g", "se", "ef")) if v2 else ",,,"))
        acc = None
    def close_over():
        nonlocal ov
        if ov is None:
            return
        dur = ov["t1"] - ov["t0"]
        if ov["n"] and dur >= MIN_S:
            n = ov["n"]; fm = ov["foff"] / n; fsd = math.sqrt(max(0.0, ov["foff2"] / n - fm * fm))
            c = ov["c"]
            overs.append("%s,%.2f,%.2f,%d,%s,V%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,,," % (
                utc(t0 + ov["t0"]), ov["t0"], ov["t1"], c["dial"], c["mode"], c["wave"], c["invert"], ov["snr"] / n, ov["min"], ov["max"],
                fm, fsd, min(100.0, 100.0 * ov["sync_s"] / dur)))
            event(ov["t1"], "sync_off")
        ov = None
    for r in rows:
        if r["kind"] != "rx":
            continue
        t = fnum(r["t"]); sync = r["sync"] == "1"
        u, o = int(fnum(r["rx_underruns"], 0)), int(fnum(r["rx_overruns"], 0))
        if u != last_under: event(t, "underrun", str((u - last_under) % M64)); last_under = u
        if o != last_overrun: event(t, "overrun", str((o - last_overrun) % M64)); last_overrun = o
        if ov and t - ov["last"] > GAP:
            close_over()
        if not sync:
            continue
        snr, fo = fnum(r["snr_db"]), fnum(r["foff_hz"])
        if ov is None:
            ov = {"t0": t, "t1": t, "last": t, "n": 0, "sync_s": 0.0, "snr": 0.0, "min": 1e9, "max": -1e9, "foff": 0.0, "foff2": 0.0,
                  "c": dict(ctx_for(t))}
            event(t, "sync_on")
        dt = t - ov["last"]
        if 0 < dt < 0.5: ov["sync_s"] += dt
        ov["last"] = ov["t1"] = t; ov["n"] += 1
        ov["snr"] += snr; ov["foff"] += fo; ov["foff2"] += fo * fo
        ov["min"] = min(ov["min"], snr); ov["max"] = max(ov["max"], snr)
        b = int(math.floor(t * 2.0))
        if b != binn:
            flush_bin(); binn = b
            acc = {"n": 0, "snr": 0.0, "min": 1e9, "foff": 0.0, "dh": 0.0, "g": 0.0, "se": 0.0, "ef": 0.0, "v2": False}
        acc["n"] += 1; acc["snr"] += snr; acc["min"] = min(acc["min"], snr); acc["foff"] += fo
        for k, col in (("dh", "delta_hat"), ("g", "gain"), ("se", "snr_est"), ("ef", "est_foff")):
            acc[k] += fnum(r[col], 0.0)
        acc["v2"] = ctx_for(t)["wave"] == 2
    flush_bin(); close_over()
    # transmit
    tx_out, txov = [], []
    tov = None; tb = None; ta = None
    def flush_tx():
        nonlocal ta, tb
        if ta and ta["n"]:
            tx_out.append("%.2f,%.1f,%.1f,%.2f,%.0f,%d" % (tb * 0.5 + 0.25, ta["loud"] / ta["n"], ta["peak"], ta["agc"], 100.0 * ta["lim"] / ta["n"], ta["fill"]))
        ta = None
    def close_tov():
        nonlocal tov
        if tov:
            c = tov["c"]
            txov.append("%s,%.2f,%.2f,%d,V%d,%.1f,%.1f,%.2f,%.1f,%.1f,%.0f,%d,%.0f" % (
                utc(t0 + tov["t0"]), tov["t0"], tov["t1"], c["dial"], c["wave"], tov["drive"], tov["trim"], tov["agc"],
                tov["loud"] / tov["nl"] if tov["nl"] else -99.0, tov["peak"], 100.0 * tov["lim"] / tov["rows"] if tov["rows"] else 0.0,
                (tov["ul"] - tov["u0"]) % M64, tov["hold"] * 100.0))
            event(tov["t1"], "key_up")
        tov = None
    prev_drive = prev_trim = None
    for r in rows:
        if r["kind"] != "tx" or r["tx"] != "1":
            continue
        t = fnum(r["t"])
        loud, peak = fnum(r["loud_mom"], -99.0), fnum(r["peak_dbfs"], -200.0)
        und = int(fnum(r["tx_underruns"], 0))
        if tov and t - tov["t1"] > TX_GAP:
            close_tov()
        if tov is None:
            tov = {"t0": t, "t1": t, "n": 0, "rows": 0, "loud": 0.0, "nl": 0, "peak": -200.0, "drive": 0.0, "trim": 0.0, "agc": 0.0,
                   "lim": 0, "hold": 0, "u0": und, "ul": 0, "c": dict(ctx_for(t))}
            event(t, "key_down")
        tov["t1"] = t; tov["rows"] += 1
        if loud > -60.0: tov["loud"] += loud; tov["nl"] += 1
        tov["peak"] = max(tov["peak"], peak)
        drive, trim, agc = fnum(r["drive_db"], 0.0), fnum(r["trim_db"], 0.0), fnum(r["agc_db"], 0.0)
        tov["drive"], tov["trim"], tov["agc"] = drive, trim, agc
        if r["limiting"] == "1": tov["lim"] += 1
        if int(fnum(r["end_state"], 0)) > 0: tov["hold"] += 1
        if und != tov["ul"]: event(t, "tx_underruns", str((und - tov["ul"]) % M64))
        tov["ul"] = und
        if drive != prev_drive: event(t, "drive", "%.1f" % drive); prev_drive = drive
        if trim != prev_trim: event(t, "trim", "%.1f" % trim); prev_trim = trim
        b = int(math.floor(t * 2.0))
        if b != tb:
            flush_tx(); tb = b
            ta = {"n": 0, "lim": 0, "loud": 0.0, "peak": -200.0, "agc": 0.0, "fill": 1 << 40}
        ta["loud"] += max(loud, -90.0); ta["peak"] = max(ta["peak"], peak); ta["agc"] = agc
        if r["limiting"] == "1": ta["lim"] += 1
        ta["fill"] = min(ta["fill"], int(fnum(r["tx_out_fill"], 0)))
        ta["n"] += 1
    flush_tx(); close_tov()
    # write
    os.makedirs(dest)
    t_last = fnum(rows[-1]["t_unix"])
    with open(os.path.join(dest, "meta.txt"), "w") as f:
        f.write("started_utc: %s\nstarted_unix: %.3f\nstarted_local: %s\n" % (utc(t0), t0, stem))
        for k in ("program", "waveform", "dial_hz", "mode", "invert", "trim_db", "agc", "drive_db", "rade_tx"):
            if k in meta: f.write("%s: %s\n" % (k, meta[k]))
        f.write("ended_utc: %s\nconverted_from: %s (old single-file format; the end-of-over flag and calls were not recorded then)\n" % (
            utc(t_last), os.path.basename(path)))
    def table(name, header, lines):
        with open(os.path.join(dest, name), "w") as f:
            f.write(header + "\n")
            f.writelines(l + "\n" for l in lines)
    table("rx.csv", "t,snr_mean,snr_min,foff_mean,n,delta_hat,gain,snr_est,est_foff", rx_out)
    table("tx.csv", "t,loud_lufs,peak_dbfs,agc_db,limiting_pct,out_fill_min", tx_out)
    ev.sort(key=lambda e: e[0])
    table("events.csv", "t,kind,value", ["%.2f,%s,%s" % e for e in ev])
    table("overs.csv", "utc_start,t_start,t_end,dial_hz,mode,wave,invert,snr_mean,snr_min,snr_max,foff_mean,foff_sd,sync_pct,eoo,call,call_snr", overs)
    table("tx_overs.csv", "utc_start,t_start,t_end,dial_hz,wave,drive_db,trim_db,agc_db_end,loud_mean,peak_max,limiting_pct,underruns,hold_ms", txov)
    with tarfile.open(dest + ".tar.gz", "w:gz") as tf:
        tf.add(dest, arcname=stem)
    return "%s: %d rx rows, %d tx rows, %d heard overs, %d sent overs, %d events" % (stem, len(rx_out), len(tx_out), len(overs), len(txov), len(ev))


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    args = argv[1:] or [os.path.join(here, "..", "..", "rade-logs")]
    for a in args:
        files = sorted(glob.glob(os.path.join(a, "*_RADE.csv"))) if os.path.isdir(a) else [a]
        outdir = a if os.path.isdir(a) else os.path.dirname(os.path.abspath(a))
        for f in files:
            print(convert(f, outdir))


if __name__ == "__main__":
    main(sys.argv)

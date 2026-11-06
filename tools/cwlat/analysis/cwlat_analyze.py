#!/usr/bin/env python3
"""
cwlat_analyze - analyse cwlat test captures: KiwiSDR GNSS-stamped IQ + cwlat event log.

For every capture directory written by capture_run.sh it
  1. reads the KiwiSDR IQ wav and puts every sample on the UTC time scale (GNSS stamps),
  2. finds the keyed RF carrier and its on/off edges (10 / 50 / 90 % points),
  3. matches RF elements to the MIDI events of each trial (events.csv),
  4. computes end-to-end latency, jitter, element lengths (truncation / clipping against the
     commanded hold or the nominal dit/dah), extra / missing elements, rise and fall times,
     the RF-vs-sidetone offset (cwlat trials.csv), and quality figures for the recording.

usage:
  cwlat_analyze.py run  CAPTURE_DIR [--plot]          one capture, prints a report
  cwlat_analyze.py all  RESULTS_DIR... [--out DIR]    every capture below the directories,
                                                      writes summary.csv / summary.md / per-trial csv

Caveats (also printed in the report):
  * latency is measured against the PC clock (NTP-synchronised, trusted) versus GPS time (leap seconds
    taken as 18 s). Any PC clock error is a constant offset in every latency figure.
  * the Kiwi's own filter delay (a fraction of a millisecond) is not corrected.
  * the RF edge shape includes the radio's CW ramp, so the 50 % point is the reference.
"""
import argparse
import csv
import glob
import json
import os
import struct
import sys
from collections import defaultdict

import numpy as np
from scipy import signal

GPS_EPOCH = 315964800.0     # 1980-01-06 00:00:00 UTC as a unix time
LEAP = 18.0                 # GPS - UTC, seconds (valid since 2017-01-01)
WEEK = 604800.0


# ------------------------------------------------------------------ Kiwi wav reading
def read_kiwi_wav(path):
    """returns (iq complex64 array, chunk_start_index array, gps_sow array, gps_flag array)"""
    f = open(path, "rb").read()
    if f[:4] != b"RIFF" or f[8:12] != b"WAVE":
        raise ValueError("not a wav file")
    i = 12
    fmt = None
    iq_parts, starts, sows, flags = [], [], [], []
    pos = 0
    pend = None
    while i + 8 <= len(f):
        cid = f[i:i + 4]
        sz = struct.unpack("<I", f[i + 4:i + 8])[0]
        body = f[i + 8:i + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"kiwi":
            if len(body) == 10:
                flag, _, sec, nsec = struct.unpack("<BBII", body)
                pend = (flag, sec + 1e-9 * nsec)
        elif cid == b"data":
            n = len(body) // 4
            if n and len(body) == sz:
                x = np.frombuffer(body[:n * 4], dtype="<i2").astype(np.float32)
                iq_parts.append(x[0::2] + 1j * x[1::2])
                starts.append(pos)
                if pend is None:
                    sows.append(0.0)
                    flags.append(255)
                else:
                    flags.append(pend[0])
                    sows.append(pend[1])
                pos += n
            pend = None
        i += 8 + sz + (sz & 1)
    if fmt is None or not iq_parts:
        raise ValueError("no IQ data")
    return (np.concatenate(iq_parts).astype(np.complex64), np.array(starts, float),
            np.array(sows, float), np.array(flags, int), fmt[2])


def gps_to_utc_epoch(sow, ref_utc):
    """GPS seconds-of-week -> unix/UTC epoch seconds, week chosen to be nearest to ref_utc"""
    ref_gps = ref_utc + LEAP - GPS_EPOCH
    week = np.round((ref_gps - sow) / WEEK)
    return GPS_EPOCH + week * WEEK + sow - LEAP


class KiwiTime:
    """sample index -> UTC epoch seconds from the GNSS stamps"""
    def __init__(self, starts, sows, flags, ref_utc):
        ok = (flags < 254) & (sows > 0)
        s, t = starts[ok], sows[ok]
        # unwrap the week roll-over
        t = np.unwrap(t, period=WEEK)
        self.n_chunks = len(starts)
        self.n_valid = int(ok.sum())
        if self.n_valid < 10:
            raise ValueError("fewer than 10 GNSS stamps")
        p = np.polyfit(s, t, 1)
        res = t - np.polyval(p, s)
        # drop wild stamps, refit
        keep = np.abs(res) < max(5 * np.std(res), 2e-4)
        p = np.polyfit(s[keep], t[keep], 1)
        res = t[keep] - np.polyval(p, s[keep])
        self.p = p
        self.rate = 1.0 / p[0]
        self.res_rms_us = float(np.sqrt(np.mean(res ** 2)) * 1e6)
        self.res_max_us = float(np.max(np.abs(res)) * 1e6)
        self.ref_utc = ref_utc
        self.sow0 = np.polyval(p, 0.0)
        self.utc0 = float(gps_to_utc_epoch(np.array([self.sow0]), ref_utc)[0])
        self.n_dropped_stamps = int((~keep).sum())

    def utc(self, idx):
        return self.utc0 + np.asarray(idx, float) * self.p[0]


# ---------------------------------------------------------------- envelope / edges
def find_carrier(z, fs, kt, wins):
    """locate OUR carrier: the frequency whose power follows the times we keyed (wins = list of (t0, t1) UTC
    intervals of key-down commands). Robust against other stations / steady interferers."""
    nper, hop = 512, 128
    f, t, S = signal.stft(z, fs=fs, nperseg=nper, noverlap=nper - hop, return_onesided=False, boundary=None,
                          padded=False)
    S = np.abs(S)
    f = np.fft.fftshift(f)
    S = np.fft.fftshift(S, axes=0)
    tu = kt.utc(t * fs)
    ind = np.zeros(len(tu))
    for t0, t1 in wins:
        ind[(tu >= t0) & (tu <= t1)] = 1.0
    if ind.sum() < 3 or ind.sum() > len(ind) - 3:
        raise ValueError("cannot correlate: key indicator is constant")
    L = 20 * np.log10(S + 1e-3)
    L = (L - L.mean(axis=1, keepdims=True)) / (L.std(axis=1, keepdims=True) + 1e-9)
    c = (L * ((ind - ind.mean()) / ind.std())[None, :]).mean(axis=1)
    k = int(np.argmax(c))
    f_coarse = float(f[k])
    # refine on the samples that were inside key-down windows
    mask = np.zeros(len(z), bool)
    tz = kt.utc(np.arange(len(z)))
    for t0, t1 in wins:
        mask[(tz >= t0 + 0.02) & (tz <= t1)] = True
    if mask.sum() < 2048:
        return f_coarse, float(c[k])
    seg = z[mask]
    nfft = 1 << 18
    sp = np.abs(np.fft.fft(seg * np.hanning(len(seg)), nfft)) ** 2
    fr = np.fft.fftfreq(nfft, 1.0 / fs)
    sel = np.abs(fr - f_coarse) < 40.0
    kk = np.flatnonzero(sel)[int(np.argmax(sp[sel]))]
    return float(fr[kk]), float(c[k])


def carrier_envelope(z, fs, f0, bw=400.0):
    t = np.arange(len(z)) / fs
    x = z * np.exp(-2j * np.pi * f0 * t)
    sos = signal.butter(4, bw / (fs / 2), output="sos")
    xl = signal.sosfiltfilt(sos, x)
    return np.abs(xl).astype(np.float32)


def find_segments(env, fs, ktime):
    n = len(env)
    lo = float(np.percentile(env, 10))
    hi = float(np.percentile(env, 99.5))
    snr = 20 * np.log10(hi / max(lo, 1e-12))
    P = hi
    t10, t50, t90 = 0.1 * P, 0.5 * P, 0.9 * P
    above = env >= t50
    idx = np.flatnonzero(np.diff(above.astype(np.int8)))
    segs = []
    i = 0
    # pair rising / falling crossings
    rising = [j for j in idx if not above[j] and above[j + 1]]
    falling = [j for j in idx if above[j] and not above[j + 1]]
    def cross(j, thr):
        a, b = env[j], env[j + 1]
        return j + (thr - a) / (b - a) if b != a else j
    fi = 0
    for r in rising:
        while fi < len(falling) and falling[fi] <= r:
            fi += 1
        if fi >= len(falling):
            break
        f = falling[fi]
        on50, off50 = cross(r, t50), cross(f, t50)
        if (off50 - on50) / fs < 0.004:
            continue
        j = r
        while j > 0 and env[j] >= t10:
            j -= 1
        on10 = cross(j, t10) if env[j + 1] != env[j] else j
        j = r + 1
        while j < n - 1 and env[j] < t90 and j < f:
            j += 1
        on90 = cross(j - 1, t90) if env[j] != env[j - 1] else j
        j = f
        while j > r and env[j] >= t90:
            j -= 1
        off90 = cross(j, t90) if env[j + 1] != env[j] else j
        j = f + 1
        while j < n - 1 and env[j] >= t10:
            j += 1
        off10 = cross(j - 1, t10) if env[j] != env[j - 1] else j
        mid = env[int(on50 + (off50 - on50) * 0.25):int(on50 + (off50 - on50) * 0.75) + 1]
        plateau = float(np.median(mid)) if len(mid) else P
        peak = float(env[int(on50):int(off50) + 1].max())
        segs.append(dict(on10=on10, on50=on50, on90=on90, off90=off90, off50=off50, off10=off10,
                         plateau=plateau, peak=peak))
    for s in segs:
        for k in ("on10", "on50", "on90", "off90", "off50", "off10"):
            s[k + "_t"] = float(ktime.utc(s[k]))
    return segs, dict(P=P, floor=lo, snr_db=snr)


# ----------------------------------------------------------------------- run loader
def load_events(path):
    rows = []
    meta = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            rows.append(line)
    rd = csv.DictReader(rows)
    ev = [dict(idx=int(r["idx"]), trial=int(r["trial"]), note=int(r["note"]), on=int(r["on"]),
               hold=float(r["hold_ms"]), t=float(r["t_send_utc_s"])) for r in rd]
    return ev


def load_trials(path):
    out = []
    with open(path) as f:
        for r in csv.DictReader(f):
            out.append(r)
    return out


def analyse_run(d, plot=False, cold_ms=400.0, verbose=False):
    name = os.path.basename(d.rstrip("/"))
    meta = json.load(open(os.path.join(d, name + ".meta.json")))
    ev = load_events(os.path.join(d, name + ".events.csv"))
    trials = load_trials(os.path.join(d, name + ".trials.csv"))
    z, starts, sows, flags, wavrate = read_kiwi_wav(os.path.join(d, meta["kiwi_file"]))
    ref_utc = np.mean([e["t"] for e in ev if e["t"] > 0])
    kt = KiwiTime(starts, sows, flags, ref_utc)
    fs = kt.rate
    wins = []
    for i in range(len(trials)):
        e_on = [e["t"] for e in ev if e["trial"] == i and e["on"] and e["t"] > 0]
        e_off = [e["t"] for e in ev if e["trial"] == i and not e["on"] and e["t"] > 0]
        if e_on and e_off:
            wins.append((min(e_on), max(e_off) + 0.03))
    f0, corr = find_carrier(z, fs, kt, wins)
    env = carrier_envelope(z, fs, f0)
    segs, lv = find_segments(env, fs, kt)
    parts = name.split("_")
    path_, key, mode, q, test = parts[0], parts[1], parts[2], parts[3], parts[4]
    wpm = 22
    args = meta.get("cwlat_args", "").split()
    if "--wpm" in args:
        wpm = int(args[args.index("--wpm") + 1])
    dot = 1200.0 / wpm

    ntr = len(trials)
    tp = np.zeros(ntr)
    tr_rel = np.zeros(ntr)
    for i in range(ntr):
        e_on = [e["t"] for e in ev if e["trial"] == i and e["on"] and e["t"] > 0]
        e_off = [e["t"] for e in ev if e["trial"] == i and not e["on"] and e["t"] > 0]
        tp[i] = min(e_on) if e_on else 0.0
        tr_rel[i] = min(e_off) if e_off else 0.0

    out = []
    s0 = 0
    prev_off = None
    for i in range(ntr):
        T = trials[i]
        r = dict(trial=i, kind=int(T["kind"]), hold_ms=float(T["hold_ms"]))
        r["sink_lat50_ms"] = float(T["lat50_ms"]) if T["dropped"] == "0" else np.nan
        r["sink_nel"] = int(T["nel"])
        r["sink_len0_ms"] = float(T["len0_ms"]) if T["dropped"] == "0" else np.nan
        r["sink_dropped"] = int(T["dropped"])
        if tp[i] == 0.0:
            r["rf_nel"] = -1
            out.append(r)
            continue
        t_end = (tp[i + 1] if i + 1 < ntr and tp[i + 1] > 0 else tp[i] + 3.0) - 0.003
        mine = [s for s in segs if tp[i] - 0.003 <= s["on50_t"] < t_end]
        r["rf_nel"] = len(mine)
        r["rf_dropped"] = int(len(mine) == 0)
        prev = [s for s in segs if s["off50_t"] < tp[i]]
        idle = (mine[0]["on50_t"] - prev[-1]["off50_t"]) * 1000 if mine and prev else (1e9 if mine else np.nan)
        r["rf_idle_ms"] = idle
        r["cold"] = int(idle > cold_ms) if mine else -1
        if mine:
            m = mine[0]
            r["rf_lat10_ms"] = (m["on10_t"] - tp[i]) * 1000
            r["rf_lat50_ms"] = (m["on50_t"] - tp[i]) * 1000
            r["rf_lat90_ms"] = (m["on90_t"] - tp[i]) * 1000
            r["rf_rise_ms"] = (m["on90_t"] - m["on10_t"]) * 1000
            r["rf_fall_ms"] = (m["off10_t"] - m["off90_t"]) * 1000
            r["rf_off_lat_ms"] = (m["off50_t"] - tr_rel[i]) * 1000 if (tr_rel[i] > 0 and r["kind"] == 0) else np.nan
            for k, s in enumerate(mine[:4]):
                r[f"rf_len{k}_ms"] = (s["off50_t"] - s["on50_t"]) * 1000
            r["rf_overshoot_db"] = 20 * np.log10(m["peak"] / max(m["plateau"], 1e-12))
            if len(mine) >= 2:
                r["rf_period01_ms"] = (mine[1]["on50_t"] - mine[0]["on50_t"]) * 1000
            if r["kind"] == 0:
                ref = r["hold_ms"]
            elif r["kind"] == 2:
                ref = 3 * dot
            elif r["kind"] == 1:
                ref = dot
            else:
                ref = np.nan
            r["rf_len0_minus_ref_ms"] = r["rf_len0_ms"] - ref if ref == ref else np.nan
            r["ref_ms"] = ref
            if r["sink_lat50_ms"] == r["sink_lat50_ms"]:
                r["rf_minus_sink_lat_ms"] = r["rf_lat50_ms"] - r["sink_lat50_ms"]
        out.append(r)

    quality = dict(
        kiwi_rate_hz=fs, kiwi_stamp_res_rms_us=kt.res_rms_us, kiwi_stamp_res_max_us=kt.res_max_us,
        kiwi_stamps_valid=kt.n_valid, kiwi_chunks=kt.n_chunks, kiwi_stamps_dropped=kt.n_dropped_stamps,
        carrier_offset_hz=f0, carrier_corr=corr, carrier_snr_db=lv["snr_db"], rf_segments=len(segs),
        rec_start_utc=float(kt.utc(0)), rec_end_utc=float(kt.utc(len(z))),
        first_event_utc=float(min(tp[tp > 0])), last_event_utc=float(max(tp)),
        pc_clock_offset_drift_ms=None,
    )
    try:
        with open(os.path.join(d, name + ".events.csv")) as f:
            hdr = [l for l in f if l.startswith("# utc_minus_mono")][0]
        a = float(hdr.split("start=")[1].split()[0])
        b = float(hdr.split("end=")[1].split()[0])
        quality["pc_clock_offset_drift_ms"] = (b - a) * 1000
    except Exception:
        pass
    info = dict(name=name, path=path_, key=key, mode=mode, q=q, test=test, wpm=wpm, dot_ms=dot)
    if plot:
        make_plot(d, name, env, kt, fs, segs, tp, tr_rel, out)
    return info, out, quality, segs


def make_plot(d, name, env, kt, fs, segs, tp, tr_rel, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    t0 = tp[tp > 0].min() - 0.5
    t1 = t0 + 6.0
    idx0 = int((t0 - kt.utc0) * fs)
    idx1 = int((t1 - kt.utc0) * fs)
    tt = kt.utc(np.arange(idx0, idx1)) - t0
    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(tt, 20 * np.log10(env[idx0:idx1] + 1e-3), lw=0.8, color="tab:blue", label="RF envelope (Kiwi, dB)")
    for i in range(len(tp)):
        if tp[i] and t0 <= tp[i] <= t1:
            ax.axvline(tp[i] - t0, color="g", lw=0.8)
            if tr_rel[i]:
                ax.axvline(tr_rel[i] - t0, color="r", lw=0.8, ls="--")
    ax.set_xlabel("seconds (green = MIDI press, red dashed = release)")
    ax.set_title(name)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(os.path.join(d, name + ".overview.png"), dpi=110)
    plt.close(fig)


# ------------------------------------------------------------------------ reporting
def fin(v):
    return [x for x in v if x is not None and x == x]


def stat(v):
    v = np.array(fin(v), float)
    if len(v) == 0:
        return dict(n=0)
    return dict(n=len(v), mean=float(v.mean()), sd=float(v.std()), min=float(v.min()),
                p5=float(np.percentile(v, 5)), p50=float(np.percentile(v, 50)),
                p95=float(np.percentile(v, 95)), max=float(v.max()),
                spread90=float(np.percentile(v, 95) - np.percentile(v, 5)), range=float(v.max() - v.min()))


def fmt(s, nd=2):
    if s["n"] == 0:
        return "-"
    return f"{s['mean']:.{nd}f}±{s['sd']:.{nd}f} (n={s['n']})"


def summarise(info, rows, quality):
    ok = [r for r in rows if r.get("rf_nel", 0) > 0]
    n = len(rows)
    warm = [r for r in ok if r["cold"] == 0]
    cold = [r for r in ok if r["cold"] == 1]
    sm = dict(info)
    sm.update(trials=n, rf_dropped=sum(1 for r in rows if r.get("rf_nel", 0) == 0),
              sink_dropped=sum(r["sink_dropped"] for r in rows),
              both_missing=sum(1 for r in rows if r.get("rf_nel", 0) == 0 and r["sink_dropped"]),
              rf_only_missing=sum(1 for r in rows if r.get("rf_nel", 0) == 0 and not r["sink_dropped"]),
              sink_only_missing=sum(1 for r in rows if r.get("rf_nel", 0) > 0 and r["sink_dropped"]))
    k = lambda rs, key: [r.get(key) for r in rs]
    for tag, grp in (("all", ok), ("warm", warm), ("cold", cold)):
        for key in ("rf_lat50_ms", "rf_off_lat_ms", "rf_len0_minus_ref_ms", "rf_minus_sink_lat_ms", "sink_lat50_ms"):
            sm[f"{tag}_{key}"] = stat(k(grp, key))
    sm["rf_rise_ms"] = stat(k(ok, "rf_rise_ms"))
    sm["rf_fall_ms"] = stat(k(ok, "rf_fall_ms"))
    sm["rf_overshoot_db"] = stat(k(ok, "rf_overshoot_db"))
    sm["rf_period01_ms"] = stat(k(ok, "rf_period01_ms"))
    sm["extra_elements"] = sum(1 for r in ok if r["rf_nel"] > (2 if r["kind"] == 3 else 1) and r["kind"] != 3)
    sm["quality"] = quality
    return sm


def write_trial_csv(path, rows):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if (isinstance(v, float) and v != v) else v) for k, v in r.items()})


def report(sm):
    q = sm["quality"]
    print(f"== {sm['name']}")
    print(f"   Kiwi: rate {q['kiwi_rate_hz']:.2f} Hz, GNSS stamps {q['kiwi_stamps_valid']}/{q['kiwi_chunks']} "
          f"(fit residual rms {q['kiwi_stamp_res_rms_us']:.0f} us, max {q['kiwi_stamp_res_max_us']:.0f} us), "
          f"carrier {q['carrier_offset_hz']:+.1f} Hz from centre (key correlation {q['carrier_corr']:.2f}), SNR {q['carrier_snr_db']:.1f} dB, "
          f"{q['rf_segments']} RF elements")
    print(f"   trials {sm['trials']}: RF missing {sm['rf_dropped']} (sink missing {sm['sink_dropped']}; both {sm['both_missing']}, "
          f"RF only {sm['rf_only_missing']}, sink only {sm['sink_only_missing']}); extra elements {sm['extra_elements']}")
    for tag in ("warm", "cold"):
        print(f"   {tag:4}: RF onset lat {fmt(sm[tag + '_rf_lat50_ms'])} ms | sink onset {fmt(sm[tag + '_sink_lat50_ms'])} | "
              f"RF-sink {fmt(sm[tag + '_rf_minus_sink_lat_ms'])} | RF len-ref {fmt(sm[tag + '_rf_len0_minus_ref_ms'])} | "
              f"RF off lat {fmt(sm[tag + '_rf_off_lat_ms'])}")
    print(f"   rise {fmt(sm['rf_rise_ms'])} ms  fall {fmt(sm['rf_fall_ms'])} ms  overshoot {fmt(sm['rf_overshoot_db'])} dB")


def write_markdown(sums, path):
    """compact tables grouped by key mode / test, one row per path x quantum"""
    def f(st, nd=1):
        return "-" if not st or st.get("n", 0) == 0 else f"{st['mean']:.{nd}f}±{st['sd']:.{nd}f}"
    groups = defaultdict(list)
    for sm in sums:
        groups[(sm["key"], sm["mode"], sm["test"])].append(sm)
    lines = ["# cwlat analysis summary", "",
             "RF = KiwiSDR carrier (GNSS-stamped), measured from the MIDI press event to the 50 % point of the carrier edge. "
             "sink = sidetone at the PipeWire sink monitor (cwlat). Times in ms, mean±sd. warm = first element within 400 ms of a "
             "previous one, cold = first element after a pause (break-in RX→TX). 'len-ref' = RF element length minus the commanded hold "
             "(straight) or the nominal dit/dah.", ""]
    for (key, mode, test), g in sorted(groups.items()):
        lines += [f"## {key} / {mode} / {test}", "",
                  "| path | Q | trials | RF missing | RF warm onset | RF cold onset | sink warm onset | RF−sink (warm) | "
                  "RF len-ref warm | RF len-ref cold | RF off lat (warm) | rise | fall | extra elem |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for sm in sorted(g, key=lambda x: (x["path"], int(x["q"][1:]))):
            lines.append(f"| {sm['path']} | {sm['q'][1:]} | {sm['trials']} | {sm['rf_dropped']} | "
                         f"{f(sm.get('warm_rf_lat50_ms'))} | {f(sm.get('cold_rf_lat50_ms'))} | {f(sm.get('warm_sink_lat50_ms'))} | "
                         f"{f(sm.get('warm_rf_minus_sink_lat_ms'))} | {f(sm.get('warm_rf_len0_minus_ref_ms'))} | "
                         f"{f(sm.get('cold_rf_len0_minus_ref_ms'))} | {f(sm.get('warm_rf_off_lat_ms'))} | "
                         f"{f(sm.get('rf_rise_ms'))} | {f(sm.get('rf_fall_ms'))} | {sm.get('extra_elements', 0)} |")
        lines.append("")
    open(path, "w").write("\n".join(lines))


def find_runs(dirs):
    out = []
    for d in dirs:
        for m in sorted(glob.glob(os.path.join(d, "**", "*.meta.json"), recursive=True)):
            if "_invalid" in m:
                continue
            out.append(os.path.dirname(m))
    return out


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("run")
    a.add_argument("dir")
    a.add_argument("--plot", action="store_true")
    b = sub.add_parser("all")
    b.add_argument("dirs", nargs="+")
    b.add_argument("--out", default="analysis_out")
    b.add_argument("--plot", action="store_true")
    args = ap.parse_args()
    if args.cmd == "run":
        info, rows, quality, segs = analyse_run(args.dir, plot=args.plot)
        sm = summarise(info, rows, quality)
        report(sm)
        write_trial_csv(os.path.join(args.dir, info["name"] + ".analysis.csv"), rows)
        return
    os.makedirs(args.out, exist_ok=True)
    sums = []
    for d in find_runs(args.dirs):
        try:
            info, rows, quality, segs = analyse_run(d, plot=args.plot)
        except Exception as e:
            print(f"!! {d}: {e}", file=sys.stderr)
            continue
        sm = summarise(info, rows, quality)
        report(sm)
        write_trial_csv(os.path.join(args.out, info["name"] + ".trials.csv"), rows)
        sums.append(sm)
    json.dump(sums, open(os.path.join(args.out, "summary.json"), "w"), indent=1, default=float)
    write_markdown(sums, os.path.join(args.out, "summary.md"))
    print(f"\n{len(sums)} runs analysed, results in {args.out}/")


if __name__ == "__main__":
    main()

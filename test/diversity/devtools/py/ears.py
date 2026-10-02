#!/usr/bin/env python3
"""Analyse an ear recording: captures/ears-<stamp>.wav + .csv.

The WAV is what piHPSDR handed to audio_write() for RX1's output (left,
right). The CSV has one row per rx_process_buffer() pass. For each run of
one split mode this prints, per segment of --seg seconds:

  rms_l/rms_r   level of each ear, dBFS
  lag           the L/R lag that maximises |cross-correlation|, in samples
                at 48 kHz (positive: right lags left)
  corr          normalised cross-correlation at that lag (sign kept: a
                negative value is a polarity inversion)
  corr0         the same at lag 0
  rx1_cnt       on RX0's passes, RX2's sample counter when RX0's buffer
                filled (1023 = input blocks aligned); the distinct values
  dropped       RX1 split passes that found no RX0 half to pair with

Usage: ears.py captures/ears-YYYYMMDD-HHMMSS.wav [--seg 2] [--maxlag 2048]
"""
import argparse
import csv
import struct
import sys

import numpy as np

MODES = {0: "summed", 1: "per-ear", 2: "sum/diff"}


def read_wav(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV")
    fmt, ch, rate = struct.unpack("<HHI", b[20:28])
    if fmt != 3 or ch != 2:
        sys.exit(f"{path}: expected float stereo, got format {fmt}, {ch} ch")
    n = struct.unpack("<I", b[40:44])[0]
    x = np.frombuffer(b[44:44 + n], dtype="<f4").reshape(-1, 2)
    return x, rate


def read_csv(path):
    with open(path) as f:
        return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]


def db(x):
    return 20 * np.log10(max(np.sqrt(np.mean(x * x)), 1e-12))


def xcorr(l, r, maxlag):
    n = len(l)
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    c = np.fft.irfft(np.fft.rfft(r, nfft) * np.conj(np.fft.rfft(l, nfft)), nfft)
    c = np.concatenate([c[-maxlag:], c[:maxlag + 1]])
    norm = np.sqrt(np.sum(l * l) * np.sum(r * r)) or 1.0
    c /= norm
    k = int(np.argmax(np.abs(c)))
    return k - maxlag, c[k], c[maxlag]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--seg", type=float, default=2.0)
    ap.add_argument("--maxlag", type=int, default=2048)
    a = ap.parse_args()
    x, rate = read_wav(a.wav)
    ev = read_csv(a.wav[:-4] + ".csv")
    print(f"{a.wav}: {len(x)} frames, {len(x) / rate:.1f} s, {len(ev)} blocks")

    # Mode runs, by the frame each block started at.
    runs = []
    for e in ev:
        m = int(e["mode"])
        if not runs or runs[-1][0] != m:
            runs.append([m, int(e["frame"]), []])
        runs[-1][2].append(e)
    for i, r in enumerate(runs):
        r.append(runs[i + 1][1] if i + 1 < len(runs) else len(x))

    seg = int(a.seg * rate)
    for m, f0, evs, f1 in runs:
        cnt = sorted({int(e["rx1_cnt"]) for e in evs if e["who"] == 0})
        rx1 = [e for e in evs if e["who"] == 1 and e["paired"] >= 0]
        drop = sum(1 for e in rx1 if e["paired"] == 0)
        bal = {(round(e["bal_l"], 3), round(e["bal_r"], 3)) for e in evs}
        print(f"\n{MODES.get(m, m)}: {f0 / rate:.1f}-{f1 / rate:.1f} s"
              f"  rx1_cnt={cnt[:8]}{'...' if len(cnt) > 8 else ''}"
              f"  dropped={drop}/{len(rx1)}  bal(l,r)={sorted(bal)[:4]}")
        print("   t(s)   rms_l   rms_r     lag     corr    corr0")
        for s in range(f0, f1 - seg + 1, seg):
            l, r = x[s:s + seg, 0].astype(float), x[s:s + seg, 1].astype(float)
            lag, c, c0 = xcorr(l, r, a.maxlag)
            print(f"{s / rate:7.1f} {db(l):7.1f} {db(r):7.1f} {lag:7d} {c:8.3f} {c0:8.3f}")


if __name__ == "__main__":
    main()

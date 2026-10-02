#!/usr/bin/env python3
"""Analyse an ear recording: captures/ears-<stamp>.wav + .csv.

The WAV is RX1's output pair as handed to audio_write(), taken before the
AF gain (x 0.6 headroom), 16-bit since 13539f8d's successor; older files
are float.  The CSV has one row per rx_process_buffer() pass. For each run of
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

MODES = {0: "summed", 1: "per-ear", 2: "sum/diff", -1: "no CSV (whole file)"}


def read_wav(path):
    """Float or 16/32-bit integer stereo, any chunk layout (pw-record too)."""
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE:                      # WAVE_FORMAT_EXTENSIBLE
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if ch != 2:
        sys.exit(f"{path}: {ch} channels, expected 2")
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, dtype="<f4")
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, dtype="<i2") / 32768.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, dtype="<i4") / 2147483648.0
    else:
        sys.exit(f"{path}: format {tag}/{bits} bits not handled")
    return x[: len(x) // 2 * 2].reshape(-1, 2), rate


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
    try:
        ev = read_csv(a.wav[:-4] + ".csv")
    except FileNotFoundError:
        # A recording made outside piHPSDR (a sink monitor): one run.
        ev = [{"frame": 0, "who": 2, "mode": -1, "paired": -1, "rx1_cnt": -1,
               "bal_l": 0, "bal_r": 0}]
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

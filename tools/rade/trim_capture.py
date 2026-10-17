#!/usr/bin/env python3
"""Cuts a stretch out of a piHPSDR RADE capture (rade-captures/<stem>_*), so a long capture with a short signal in it can be kept as a sample.

    trim_capture.py STEM --start S --end E  [--out DIR] [--tx-seconds N]
    trim_capture.py STEM --best N           [--out DIR] [--pad 5]     (the N-second window with the most time in sync, padded)

STEM is the capture's time stamp (20261017-191431) or the full stem; the files are looked for in rade-captures/ (RADE_CAPTURES=dir to change).
S and E are seconds from the start of the capture (the t_sec of _stats.csv). The cut is written to DIR (default: ./trimmed/) under the same
file names, so the analysis scripts read it as they read the original (copy it into rade-captures/ in place of the original, or point
RADE_CAPTURES at DIR). Nothing in the capture directory is changed or removed.

What each file becomes:
  _rx48.wav, _speech.wav   the samples of the window. rx48 is cut by the sample position in _stats.csv (rx48_pos); the speech file is the
                           same length and is cut at the same sample numbers (checked, and refused if it is not the same length)
  _stats.csv               the rows in the window, with t_sec and rx48_pos taken back to the start of the cut
  _events.log              the lines in the window, times shifted; the last settings line before the window is kept, at 0
  _meta.txt                as it was, with duration_s and rx48_samples of the cut, and an "excerpt" section saying what it was cut from
  _tx.wav                  the transmit capture has its own time base (only while transmitting): with --tx-seconds N, the first N seconds
                           of it that are transmitting; otherwise it is left out
  _txmic.wav, others       copied whole when they are under 20 MB, otherwise left out
Standard library and numpy only.
"""
import argparse
import csv
import glob
import os
import re
import struct
import sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CAP = os.environ.get("RADE_CAPTURES", os.path.join(ROOT, "rade-captures"))
SMALL = 20e6


def wav_info(fn):
    """(rate, channels, bytes per sample, data offset, data bytes, fmt chunk) of a RIFF wav (walks the chunks)."""
    with open(fn, "rb") as f:
        h = f.read(12)
        if h[:4] != b"RIFF" or h[8:12] != b"WAVE":
            raise ValueError("%s is not a wav file" % fn)
        fmt = None
        while True:
            c = f.read(8)
            if len(c) < 8:
                raise ValueError("%s has no data chunk" % fn)
            tag, size = c[:4], struct.unpack("<I", c[4:])[0]
            if tag == b"fmt ":
                fmt = f.read(size)
            elif tag == b"data":
                total = os.path.getsize(fn) - f.tell()
                return (struct.unpack("<I", fmt[4:8])[0], struct.unpack("<H", fmt[2:4])[0],
                        struct.unpack("<H", fmt[14:16])[0] // 8, f.tell(), min(size, total), fmt)
            else:
                f.seek(size + (size & 1), 1)


def write_wav(src, dst, first, count, frames_per_chunk=1 << 20):
    """Frames first..first+count-1 of src (any PCM/float wav) as a wav with the same format and a 44-byte header."""
    rate, ch, bps, off, nbytes, fmt = wav_info(src)
    fb = ch * bps
    first = max(0, min(first, nbytes // fb))
    count = max(0, min(count, nbytes // fb - first))
    with open(src, "rb") as f, open(dst, "wb") as g:
        g.write(b"RIFF" + struct.pack("<I", 36 + count * fb) + b"WAVEfmt " + struct.pack("<I", 16) + fmt[:16]
                + b"data" + struct.pack("<I", count * fb))
        f.seek(off + first * fb)
        left = count * fb
        while left > 0:
            b = f.read(min(left, frames_per_chunk * fb))
            if not b:
                break
            g.write(b)
            left -= len(b)
    return count


def find(stem):
    """The full stem(s) of the capture: '<time>_<kHz>kHz_RADE-V..'."""
    metas = sorted(glob.glob(os.path.join(CAP, stem + "*_meta.txt")))
    if len(metas) != 1:
        sys.exit("trim_capture: %d captures match %r in %s" % (len(metas), stem, CAP))
    return os.path.basename(metas[0])[:-len("_meta.txt")]


def read_stats(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def best_window(rows, n):
    """Start (s) of the n-second window holding the most in-sync stats rows."""
    t = np.array([float(r["t_sec"]) for r in rows])
    s = np.array([int(r["sync"]) for r in rows])
    cs = np.concatenate([[0], np.cumsum(s)])
    best, at = -1, t[0]
    for i in range(len(t)):
        j = np.searchsorted(t, t[i] + n)
        if cs[j] - cs[i] > best:
            best, at = cs[j] - cs[i], t[i]
    return at


def tx_window(path, seconds):
    """(first frame, frames) of the first `seconds` of transmitting in a _tx.wav (power over 2 % of the peak block power)."""
    rate, ch, bps, off, nbytes, _ = wav_info(path)
    blk = rate // 10
    pw = []
    with open(path, "rb") as f:
        f.seek(off)
        while True:
            a = np.fromfile(f, dtype=np.float32, count=blk * ch)
            if a.size < blk * ch:
                break
            pw.append(float(np.mean(a * a)))
    pw = np.array(pw)
    on = np.nonzero(pw > 0.02 * pw.max())[0]
    if not len(on):
        return 0, 0
    return int(on[0]) * blk, min(int(seconds * rate), nbytes // (ch * bps) - int(on[0]) * blk)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stem")
    ap.add_argument("--start", type=float)
    ap.add_argument("--end", type=float)
    ap.add_argument("--best", type=float, help="the window of this many seconds with the most time in sync")
    ap.add_argument("--pad", type=float, default=5.0, help="seconds added each side of --best (default 5)")
    ap.add_argument("--out", default="trimmed")
    ap.add_argument("--tx-seconds", type=float, default=0.0)
    a = ap.parse_args(argv)
    stem = find(a.stem)
    p = os.path.join(CAP, stem)
    rows = read_stats(p + "_stats.csv")
    last = float(rows[-1]["t_sec"])
    if a.best:
        s = best_window(rows, a.best)
        a.start, a.end = max(0.0, s - a.pad), min(last, s + a.best + a.pad)
    if a.start is None or a.end is None or a.end <= a.start:
        sys.exit("trim_capture: give --start and --end (seconds, start < end), or --best N")
    win = [r for r in rows if a.start <= float(r["t_sec"]) <= a.end]
    if not win:
        sys.exit("trim_capture: no stats rows between %.1f and %.1f s (the capture runs to %.1f s)" % (a.start, a.end, last))
    pos0, pos1 = int(win[0]["rx48_pos"]), int(win[-1]["rx48_pos"])
    t0 = float(win[0]["t_sec"])
    os.makedirs(a.out, exist_ok=True)
    o = os.path.join(a.out, stem)

    n_rx = write_wav(p + "_rx48.wav", o + "_rx48.wav", pos0, pos1 - pos0)
    if os.path.exists(p + "_speech.wav"):
        r1, c1, b1, _, n1, _ = wav_info(p + "_rx48.wav")
        r2, c2, b2, _, n2, _ = wav_info(p + "_speech.wav")
        if n1 // (c1 * b1) != n2 // (c2 * b2) or r1 != r2:
            sys.exit("trim_capture: _speech.wav is not the same length as _rx48.wav, so it cannot be cut at the same samples")
        write_wav(p + "_speech.wav", o + "_speech.wav", pos0, pos1 - pos0)
    with open(o + "_stats.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        for r in win:
            r = dict(r)
            r["t_sec"] = "%.4f" % (float(r["t_sec"]) - t0)
            r["rx48_pos"] = str(int(r["rx48_pos"]) - pos0)
            w.writerow(r)
    if os.path.exists(p + "_events.log"):
        keep, state = [], None
        for line in open(p + "_events.log"):
            m = re.match(r"(\d+\.\d+) (.*)", line)
            if not m:
                continue
            t = float(m.group(1))
            if t < t0:
                if " call " not in " " + m.group(2):
                    state = m.group(2)                      # the settings in force when the cut starts
            elif t <= a.end:
                keep.append("%.3f %s\n" % (t - t0, m.group(2)))
        with open(o + "_events.log", "w") as f:
            if state is not None:
                f.write("0.000 %s\n" % state)
            f.writelines(keep)
    meta = open(p + "_meta.txt").read()
    dur = (pos1 - pos0) / 48000.0
    orig_dur = re.search(r"^duration_s: ([\d.]+)", meta, re.M)
    meta = re.sub(r"^duration_s: .*$", "duration_s: %.1f" % dur, meta, flags=re.M)
    meta = re.sub(r"^rx48_samples: .*$", "rx48_samples: %d" % n_rx, meta, flags=re.M)
    meta += ("--- excerpt ---\ncut from: %s\nfrom_s: %.2f\nto_s: %.2f\noriginal_duration_s: %s\n"
             "note: _stats.csv and _events.log times, and rx48_pos, are from the start of the cut\n"
             % (stem, t0, float(win[-1]["t_sec"]), orig_dur.group(1) if orig_dur else "unknown"))
    open(o + "_meta.txt", "w").write(meta)

    txs = ""
    if os.path.exists(p + "_tx.wav") and a.tx_seconds > 0:
        first, count = tx_window(p + "_tx.wav", a.tx_seconds)
        n = write_wav(p + "_tx.wav", o + "_tx.wav", first, count)
        txs = ", _tx.wav %.1f s of transmitting" % (n / wav_info(p + "_tx.wav")[0])
    for f in sorted(glob.glob(glob.escape(p) + "_*")):
        suffix = f[len(p):]
        if suffix in ("_rx48.wav", "_speech.wav", "_stats.csv", "_events.log", "_meta.txt", "_tx.wav"):
            continue
        if os.path.getsize(f) < SMALL:
            with open(f, "rb") as src, open(o + suffix, "wb") as dst:
                dst.write(src.read())
    print("%s: %.1f..%.1f s of %s -> %.1f s, %d stats rows%s; in %s" % (
        stem, t0, float(win[-1]["t_sec"]), orig_dur.group(1) if orig_dur else "?", dur, len(win), txs, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

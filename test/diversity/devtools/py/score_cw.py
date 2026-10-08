#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Score run_ref weight series on a CW capture, with the
yardstick Finding AD-50 used (docs/diversity-measurements.md).

There is no CW decoder in the harness, so the score is tone-to-noise of
the key-down-averaged spectrum:

- the tone is the strongest bin of the passband in arm0 + arm1 power,
  averaged over the whole capture (or --tone HZ, the bin frequency as
  --peaks in score_wideband.py prints it);
- a block is key-down when the three bins at the tone, in arm0 + arm1,
  stand KEY_DB above the median of the passband's off-tone bins in that
  same block. The set is chosen from the arms and never from a candidate
  stream, so every weight is scored on the same blocks;
- each weight is applied one block late, as run_ref records it and as the
  radio applies it (out = arm0 + w * arm1). Scoring a block with the
  weight derived from itself flatters an adaptive weight by up to 9 dB;
- the combined output's power spectrum is averaged over the key-down
  blocks, and the score is (mean of the three tone bins - median of the
  off-tone passband bins) / that median, in dB. A ratio inside one
  spectrum, so a constant gain cancels.

Off-tone means four or more bins from the tone, as div_cw_solve() takes
its floor.

It is after AD-50 rather than calibrated to it: that finding's scripts are
not in the tree, so its figures are not reproduced to the decimal.

A capture the operator retuned, or changed filter in, is scored in
segments: a new one starts wherever the frame offset or the filter
changes, or the dial frequency, and runs shorter than 40 blocks are not
scored. Each segment
finds its own tone.

usage: score_cw.py CAPTURE.divc [RUN.csv ...] [--tone HZ] [--key-db DB]
"""
import csv
import struct
import sys

import numpy as np

from divc import BLK, REC_MAGIC, open_divc
from score_wideband import bh4, load_blocks

KEY_DB = 10.0


def parse_args(argv):
    cap, runs, tone, key_db = None, [], None, KEY_DB
    i = 1

    while i < len(argv):
        a = argv[i]

        if a == '--tone':
            tone = float(argv[i + 1])
            i += 2
        elif a == '--key-db':
            key_db = float(argv[i + 1])
            i += 2
        elif cap is None:
            cap = a
            i += 1
        else:
            runs.append(a)
            i += 1

    return cap, runs, tone, key_db


def passband(rate, n, fo, flo, fhi):
    """The RX passband in bin frequency: the engine maps s to -(s + frame_off)."""
    fr = np.fft.fftfreq(n, 1.0 / rate)
    lo, hi = sorted((-(fhi + fo), -(flo + fo)))
    return (fr >= lo) & (fr <= hi)


def frequencies(path):
    """The dial frequency of every block (struct divcap_block.frequency)."""
    f, h = open_divc(path)
    out = []

    while True:
        m = f.read(BLK)

        if len(m) < BLK or struct.unpack_from('<I', m, 0)[0] != REC_MAGIC:
            break

        out.append(struct.unpack_from('<q', m, 16)[0])
        f.seek(16 * h['nfft'], 1)

    f.close()
    return out


def segments(blks, freqs, least):
    """Runs of blocks with the same dial frequency, frame offset and filter:
    a retune or a filter change starts a new one. Runs shorter than least
    are dropped."""
    out, start = [], 0

    def ctx(b):
        return (freqs[b],) + tuple(blks[b][:3])

    for b in range(1, len(blks) + 1):
        if b == len(blks) or ctx(b) != ctx(start):
            if b - start >= least:
                out.append((start, b))

            start = b

    return out


def score_segment(cap, h, blks, lo, hi, runs, tone_hz, key_db, label):
    n, rate = h['nfft'], h['rate']
    win = bh4(n).astype(np.float32)
    fr = np.fft.fftfreq(n, 1.0 / rate)
    fo, flo, fhi = blks[lo][0], blks[lo][1], blks[lo][2]
    pb = passband(rate, n, fo, flo, fhi)
    #
    # In frequency order: FFT order puts the positive frequencies first, so
    # a passband straddling 0 Hz would otherwise have its neighbours apart.
    #
    idx = np.where(pb)[0]
    idx = idx[np.argsort(fr[idx])]
    F0 = np.array([np.fft.fft(blks[b][3] * win)[idx] for b in range(lo, hi)])
    F1 = np.array([np.fft.fft(blks[b][4] * win)[idx] for b in range(lo, hi)])
    nb = hi - lo
    P = np.abs(F0) ** 2 + np.abs(F1) ** 2

    if tone_hz is None:
        kt = int(np.argmax(P.mean(axis=0)))
    else:
        kt = int(np.argmin(np.abs(fr[idx] - tone_hz)))

    j = np.arange(len(idx))
    ton = (j >= kt - 1) & (j <= kt + 1)
    off = np.abs(j - kt) >= 4

    if np.sum(off) < 4:
        print(f"# {label}: passband too narrow to score ({len(idx)} bins)")
        return

    key = P[:, ton].mean(axis=1) > np.median(P[:, off], axis=1) * 10 ** (key_db / 10)

    def score(wl):
        y = F0[key] + wl[key][:, None] * F1[key]
        sp = np.mean(np.abs(y) ** 2, axis=0)
        med = np.median(sp[off])
        return 10 * np.log10(max(sp[ton].mean() - med, 1e-30) / med)

    print(f"# {label}: {nb} blocks, key-down {100 * np.mean(key):.0f} % ({int(np.sum(key))}), "
          f"tone {fr[idx][kt]:+.1f} Hz, {len(idx)} passband bins", end='')

    if not key.any():
        print()
        return

    arm0 = score(np.zeros(nb, complex))
    y1 = np.mean(np.abs(F1[key]) ** 2, axis=0)
    arm1 = 10 * np.log10(max(y1[ton].mean() - np.median(y1[off]), 1e-30) / np.median(y1[off]))
    best = max(arm0, arm1)
    print(f"; arm0 {arm0:+.2f} dB, arm1 {arm1:+.2f} dB")

    if not runs:
        return

    print(f"{'run':40s} {'score':>7s} {'vs best arm':>11s} {'acts key-down':>13s} {'acts key-up':>11s}")

    for r in runs:
        rows = list(csv.DictReader(open(r)))
        ok = np.array([int(x['ok']) for x in rows])
        w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in rows])
        wl = np.concatenate(([1.0 + 0j], w[:-1]))   # one block late; run_ref starts at w = 1

        if len(wl) < hi:
            wl = np.concatenate((wl, np.full(hi - len(wl), wl[-1])))
            ok = np.concatenate((ok, np.zeros(hi - len(ok), int)))

        wl, ok = wl[lo:hi], ok[lo:hi]
        sc = score(wl)
        print(f"{r.split('/')[-1][:40]:40s} {sc:+7.2f} {sc - best:+11.2f} "
              f"{100 * np.mean(ok[key]):12.1f}% {100 * np.mean(ok[~key]) if (~key).any() else 0:10.1f}%")


def main():
    cap, runs, tone_hz, key_db = parse_args(sys.argv)

    if cap is None:
        print(__doc__)
        return

    h, blks = load_blocks(cap)
    segs = segments(blks, frequencies(cap), 40)
    name = cap.split('/')[-1]

    if len(segs) == 1 and segs[0] == (0, len(blks)):
        score_segment(cap, h, blks, 0, len(blks), runs, tone_hz, key_db, name)
        return

    for lo, hi in segs:
        score_segment(cap, h, blks, lo, hi, runs, tone_hz, key_db, f"{name} blocks {lo}-{hi - 1}")


if __name__ == '__main__':
    main()

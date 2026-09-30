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

usage: score_cw.py CAPTURE.divc [RUN.csv ...] [--tone HZ] [--key-db DB]
"""
import csv
import sys

import numpy as np

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


def main():
    cap, runs, tone_hz, key_db = parse_args(sys.argv)

    if cap is None:
        print(__doc__)
        return

    h, blks = load_blocks(cap)
    n, rate = h['nfft'], h['rate']
    win = bh4(n).astype(np.float32)
    fr = np.fft.fftfreq(n, 1.0 / rate)
    fo, flo, fhi = blks[0][0], blks[0][1], blks[0][2]
    pb = passband(rate, n, fo, flo, fhi)
    idx = np.where(pb)[0]
    F0 = np.array([np.fft.fft(a0 * win)[idx] for _, _, _, a0, _ in blks])
    F1 = np.array([np.fft.fft(a1 * win)[idx] for _, _, _, _, a1 in blks])
    nb = len(blks)
    P = np.abs(F0) ** 2 + np.abs(F1) ** 2

    if tone_hz is None:
        kt = int(np.argmax(P.mean(axis=0)))
    else:
        kt = int(np.argmin(np.abs(fr[idx] - tone_hz)))

    j = np.arange(len(idx))
    ton = (j >= kt - 1) & (j <= kt + 1)
    off = np.abs(j - kt) >= 4

    if np.sum(off) < 4:
        print(f"# {cap.split('/')[-1]}: passband too narrow to score ({len(idx)} bins)")
        return

    key = P[:, ton].mean(axis=1) > np.median(P[:, off], axis=1) * 10 ** (key_db / 10)

    def score(wl):
        y = F0[key] + wl[key][:, None] * F1[key]
        s = np.mean(np.abs(y) ** 2, axis=0)
        med = np.median(s[off])
        return 10 * np.log10(max(s[ton].mean() - med, 1e-30) / med)

    arm0 = score(np.zeros(nb, complex))
    y1 = np.mean(np.abs(F1[key]) ** 2, axis=0)
    arm1 = 10 * np.log10(max(y1[ton].mean() - np.median(y1[off]), 1e-30) / np.median(y1[off]))
    best = max(arm0, arm1)

    print(f"# {cap.split('/')[-1]}: {nb} blocks, key-down {100 * np.mean(key):.0f} % "
          f"({int(np.sum(key))}), tone {fr[idx][kt]:+.1f} Hz, {len(idx)} passband bins; "
          f"arm0 {arm0:+.2f} dB, arm1 {arm1:+.2f} dB")

    if not key.any():
        return

    print(f"{'run':40s} {'score':>7s} {'vs best arm':>11s} {'acts key-down':>13s} {'acts key-up':>11s}")

    for r in runs:
        rows = list(csv.DictReader(open(r)))[:nb]
        ok = np.array([int(x['ok']) for x in rows])
        w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in rows])
        wl = np.concatenate(([1.0 + 0j], w[:-1]))   # one block late; run_ref starts at w = 1

        if len(wl) < nb:
            wl = np.concatenate((wl, np.full(nb - len(wl), wl[-1])))

        k = key[:len(ok)]
        s = score(wl)
        print(f"{r.split('/')[-1][:40]:40s} {s:+7.2f} {s - best:+11.2f} "
              f"{100 * np.mean(ok[k]):12.1f}% {100 * np.mean(ok[~k]) if (~k).any() else 0:10.1f}%")


if __name__ == '__main__':
    main()

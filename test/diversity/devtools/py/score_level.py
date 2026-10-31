#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. How the combined output's *level* behaves, for run_ref
weight series: what an operator hears as the band getting louder, or as
pumping, rather than what an SNR score measures.

For every block, the passband level of the output as the radio forms it -
out = (arm0 + w * arm1) * norm, with w and norm applied one block late as
run_ref records them - against the level of arm 0 alone in the same block.
Arm 0's own fading cancels out of that difference, so what is left is what
the combiner does to the level.

- rise: the median of that difference, dB. 0 means the output is as loud
  as arm 0 alone; Sum without the normaliser typically reads +3 to +8.
- step p90: the 90th percentile of its block-to-block change, dB. How much
  the level jumps between one block and the next - a Best switch without
  the normaliser is a 20 dB step.
- steps > 3 dB: how many blocks jump by more than 3 dB, which is roughly
  where a level change is heard as a change rather than as fading.

A run without a norm column (an older run_ref, or TEST's) is taken as
norm = 1 throughout.

usage: score_level.py CAPTURE.divc RUN.csv [RUN.csv ...]
"""
import csv
import sys

import numpy as np

from score_wideband import bh4, load_blocks, regions


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return

    cap, runs = sys.argv[1], sys.argv[2:]
    h, blks = load_blocks(cap)
    n, rate = h['nfft'], h['rate']
    win = bh4(n).astype(np.float32)
    F0, F1, PB = [], [], []

    for fo, flo, fhi, a0, a1 in blks:
        pb, _ = regions(rate, n, fo, flo, fhi)
        F0.append(np.fft.fft(a0 * win)[pb])
        F1.append(np.fft.fft(a1 * win)[pb])

    nb = len(blks)
    l0 = np.array([10 * np.log10(np.mean(np.abs(F0[b]) ** 2)) for b in range(nb)])
    print(f"# {cap.split('/')[-1]}: {nb} blocks")
    print(f"{'run':40s} {'rise dB':>8s} {'step p90':>9s} {'steps>3dB':>10s}")

    for r in runs:
        rows = list(csv.DictReader(open(r)))[:nb]
        w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in rows])
        g = np.array([float(x['norm']) if x.get('norm') not in (None, '') else 1.0 for x in rows])
        wl = np.concatenate(([1.0 + 0j], w[:-1]))    # one block late, as applied
        gl = np.concatenate(([1.0], g[:-1]))
        m = min(nb, len(wl))
        lo = np.array([10 * np.log10(np.mean(np.abs(F0[b] + wl[b] * F1[b]) ** 2) * gl[b] ** 2)
                       for b in range(m)])
        d = lo - l0[:m]
        step = np.abs(np.diff(d))
        print(f"{r.split('/')[-1][:40]:40s} {np.median(d):+8.2f} {np.percentile(step, 90):9.2f} "
              f"{int(np.sum(step > 3.0)):10d}")


if __name__ == '__main__':
    main()

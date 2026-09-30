#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Write a copy of a .divc with arm 1 scaled so that its
guard-band noise matches arm 0's.

Every capture in the set has lopsided arms (arm 1 typically 14-15 dB
hotter), so nothing in it can show what a weight does on a *matched* pair,
where a weight fitted to noise is supposed to cost the most. Scaling arm 1
by a constant changes nothing about the channel, the signal or the noise
correlation - only the level the loop sees - so the matched copy is the
same recording with the imbalance taken out, in the same spirit as
replay_rade --noise. The block records are copied unchanged.

usage: match_arms.py IN.divc OUT.divc
"""
import struct
import sys

import numpy as np

from divc import BLK, HDR, REC_MAGIC
from score_wideband import bh4, load_blocks, regions


def main():
    src, dst = sys.argv[1], sys.argv[2]
    h, blks = load_blocks(src)
    n, rate = h['nfft'], h['rate']
    win = bh4(n).astype(np.float32)
    g0 = g1 = 0.0

    for fo, flo, fhi, a0, a1 in blks:
        _, g = regions(rate, n, fo, flo, fhi)
        g0 += np.mean(np.abs(np.fft.fft(a0 * win)[g]) ** 2)
        g1 += np.mean(np.abs(np.fft.fft(a1 * win)[g]) ** 2)

    ratio_db = 10 * np.log10(g1 / g0)
    k = np.float32(10 ** (-ratio_db / 20))

    with open(src, 'rb') as fi, open(dst, 'wb') as fo:
        fo.write(fi.read(HDR))

        while True:
            m = fi.read(BLK)

            if len(m) < BLK or struct.unpack_from('<I', m, 0)[0] != REC_MAGIC:
                fo.write(m)
                fo.write(fi.read())   # the trailer, as it was
                break

            a0 = fi.read(8 * n)
            a1 = np.frombuffer(fi.read(8 * n), dtype=np.float32) * k
            fo.write(m)
            fo.write(a0)
            fo.write(a1.astype(np.float32).tobytes())

    print(f"{src}: arm 1 guard noise {ratio_db:+.2f} dB against arm 0, "
          f"scaled by {-ratio_db:+.2f} dB -> {dst}")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/diversity-radeV2-combining.md section 14.

What is a drop in aux bit errors worth in dB? Arm 0's aux bit error rate varies with its own signal
quality as the channel fades; fit that (per block of seconds: logistic in arm 0's CP SNR estimate),
then ask what shift in arm 0's SNR would bring its error rate down to the combiner's. The shift is
the combiner's gain, in the units of the receiver's own SNR estimate, on the recordings where the
aux message repeats. Uses the files score_radev2 --lat-dir --csv-dir wrote.

    dbgain.py DIR NAME [DIR NAME ...] [--block 10]
"""
import argparse
import csv
import os
import sys
import tempfile

import numpy as np
from scipy.optimize import brentq, minimize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddscore as d  # noqa: E402
import radev2_oracle as o  # noqa: E402


def frames(dirn, name):
    raw = np.fromfile(os.path.join(dirn, name + '.lat'), np.float32).reshape(-1, 3, 56)
    rows = [r for r in csv.DictReader(open(os.path.join(dirn, name + '.csv'))) if r['valid'] == '1']
    t = np.array([float(r['t']) for r in rows])
    n = min(len(raw), len(t))
    raw, t = raw[:n], t[:n]
    tmp = os.path.join(tempfile.mkdtemp(), 'x')
    z0, zc = d.to_complex(raw[:, 0]), d.to_complex(raw[:, 2])
    s0, sc, kk, tt = [], [], [], []

    for s, e in d.runs(t):
        if e - s < 12:
            continue

        s0.append(d.aux(o.decode_raw(o.latents(z0[s:e]), tmp)))
        sc.append(d.aux(o.decode_raw(o.latents(zc[s:e]), tmp)))
        kk.append(np.arange(s, e))
        tt.append(t[s:e])

    return np.concatenate(s0), np.concatenate(sc), np.concatenate(kk), np.concatenate(tt)


def arm0_snr(dirn):
    a = np.genfromtxt(os.path.join(dirn, 'arm0.csv'), delimiter=',', names=True)
    return a['t'], np.where(a['sig'] > 0, a['snr'], np.nan)


def blocks(dirn, name, block):
    a0, ac, k, t = frames(dirn, name)
    cons, ok = d.consensus(ac, k)            # the combined stream's copies
    cons0, ok0 = d.consensus(a0, k)          # arm 0's, the conservative one
    ts, snr = arm0_snr(dirn)
    out = []

    for b in range(int(t.max() // block)):
        m = (t >= b * block) & (t < (b + 1) * block)
        mm = m & ok & ok0
        sm = (ts >= b * block) & (ts < (b + 1) * block)

        if mm.sum() < 0.6 * block * 25 or np.isnan(snr[sm]).all():
            continue

        out.append((np.nanmean(snr[sm]), np.mean(np.sign(a0[mm]) != cons[mm]), np.mean(np.sign(ac[mm]) != cons[mm]),
                    np.mean(np.sign(a0[mm]) != cons0[mm]), np.mean(np.sign(ac[mm]) != cons0[mm]), mm.sum()))

    return np.array(out)


def fit(snr, ber, n):
    """logistic: ber = 1 / (1 + exp(a + b snr)), weighted by frames"""
    def nll(p):
        pr = np.clip(1 / (1 + np.exp(p[0] + p[1] * snr)), 1e-4, 1 - 1e-4)
        return -np.sum(n * (ber * np.log(pr) + (1 - ber) * np.log(1 - pr)))

    return minimize(nll, [0.0, 0.3], method='Nelder-Mead').x


def shift(p, snr, n, target):
    """the dB by which arm 0's SNRs would have to rise for its fitted mean error rate to fall to target"""
    def f(sh):
        return np.sum(n / (1 + np.exp(p[0] + p[1] * (snr + sh)))) / np.sum(n) - target

    try:
        return brentq(f, -10, 30)
    except ValueError:
        return float('nan')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pairs', nargs='+')
    ap.add_argument('--block', type=float, default=10.0)
    a = ap.parse_args()
    allb = []

    for i in range(0, len(a.pairs), 2):
        b = blocks(a.pairs[i], a.pairs[i + 1], a.block)
        allb.append(b)
        print('%s: %d blocks of %g s' % (a.pairs[i + 1], len(b), a.block))

    B = np.concatenate(allb)
    snr, n = B[:, 0], B[:, 5]

    for col, label in ((1, 'consensus from the combined stream'), (3, 'consensus from arm 0')):
        p = fit(snr, B[:, col], n)
        mean0, meanc = np.average(B[:, col], weights=n), np.average(B[:, col + 1], weights=n)
        print('\n%s: arm 0 errors %.2f%% -> combined %.2f%% over %d blocks' % (label, 100 * mean0, 100 * meanc, len(B)))
        print('  arm 0: error rate falls by a factor e every %.2f dB of its CP SNR (logistic slope %.2f/dB); '
              '50%% at %.1f dB' % (1 / p[1], p[1], -p[0] / p[1]))
        sh = shift(p, snr, n, meanc)
        rng = np.random.default_rng(1)
        bs = []

        for _ in range(300):
            idx = rng.integers(0, len(B), len(B))
            q = fit(snr[idx], B[idx, col], n[idx])
            bs.append(shift(q, snr[idx], n[idx], np.average(B[idx, col + 1], weights=n[idx])))

        print('  equivalent gain: %.1f dB  (bootstrap over blocks, 90%%: %.1f to %.1f)'
              % (sh, np.nanpercentile(bs, 5), np.nanpercentile(bs, 95)))


if __name__ == '__main__':
    main()

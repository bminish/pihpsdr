#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Cyclic-prefix probe for RADE V2 (and V1) on two-arm .divc captures.

The reference the C module src/rade_v2_correlator.c was ported from. Same maths, whole-capture
numpy instead of a running tracker, so a change to the C side can be checked against it.

RADE V2 has no pilots, but every 160-sample symbol repeats its last 32 samples at the front. The
lag-128 autocorrelation over a 32-sample window, folded over the 160-sample symbol period, is a
triangle about 64 samples wide where that window sits on the prefix, and flat elsewhere. A tone, a
comb on the 62.5 Hz grid, speech and noise do not make the triangle; a tone makes a flat pedestal,
which is subtracted as the complex mean of the offsets away from the bump.

  scan  FILE...     per-capture detection statistic ("dpk") and the persistence lock, both banks
  cross FILE        inter-arm phase from the CP-gated lag-128 cross-correlation against the
                    lag-0 cross-spectrum, with synthetic common-mode interferers injected

Bank 0 is the tapped buffer as it is (LSB), bank 1 its mirror (USB), as in rade_correlator.c.
Nothing here is imported by anything else. Run with `python3 -I`.
"""
import argparse
import os
import struct
import sys

import numpy as np
from scipy.signal import firwin, lfilter, resample_poly

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import divc   # noqa: E402

FS = 8000
V2 = dict(L=128, W=32, P=160, lo=1030.0, hi=1910.0)
V1 = dict(L=160, W=32, P=192, lo=725.0, hi=2225.0)
GUARD = 48          # offsets further than this from the bump are "off the prefix"
T_LOCK = 0.30       # dpk a one-second window needs to count towards a lock (as rade_v2_correlator.c)
LOCK_K, LOCK_N, LOCK_TOL = 4, 5, 20     # 4 of the last 5 windows, bump within +/-20 samples


def load(path, max_s=None):
    """Both arms at the DDC rate, the rate, and the first block's frame offset."""
    f, h = divc.open_divc(path)
    rate, nfft, bb = h['rate'], h['nfft'], h['block_bytes']
    a0, a1, offs, n = [], [], [], 0
    while True:
        m = f.read(divc.BLK)
        if len(m) < divc.BLK or struct.unpack_from('<I', m, 0)[0] != divc.REC_MAGIC:
            break
        rec = f.read(bb)
        if len(rec) < bb:
            break
        x = np.frombuffer(rec, dtype='<f4', count=4 * nfft).reshape(2, nfft, 2)
        a0.append(x[0, :, 0] + 1j * x[0, :, 1])
        a1.append(x[1, :, 0] + 1j * x[1, :, 1])
        offs.append(divc.block_meta(m)['offset'])
        n += nfft
        if max_s and n >= max_s * rate:
            break
    return np.concatenate(a0), np.concatenate(a1), rate, offs[0]


def to_8k(x, rate, shift_hz=0.0):
    """Shift the tuned carrier to zero and decimate to 8 kHz."""
    x = x * np.exp(-2j * np.pi * shift_hz * np.arange(len(x)) / rate)
    return resample_poly(x, 1, rate // FS, window=('kaiser', 8.0))


def band(x, lo, hi):
    """Complex band-pass at 8 kHz, band centre kept where it is."""
    c, n = 0.5 * (lo + hi), np.arange(len(x))
    y = lfilter(firwin(129, 0.5 * (hi - lo), fs=FS), 1.0, x * np.exp(-2j * np.pi * c * n / FS))
    return y * np.exp(2j * np.pi * c * n / FS)


def arms(path, mirror, max_s=40, wave=V2):
    a0, a1, rate, off = load(path, max_s)
    out = []
    for a in (a0, a1):
        x = to_8k(a, rate)
        out.append(band(np.conj(x) if mirror else x, wave['lo'], wave['hi']))
    return out[0], out[1], off


def fold(ya, yb, L, W, P):
    """R[s, g] = sum over the W samples from s*P+g of ya[n] conj(yb[n+L])."""
    n = len(ya) - L
    cs = np.concatenate([[0], np.cumsum(ya[:n] * np.conj(yb[L:L + n]))])
    R = cs[W:] - cs[:-W]
    ns = len(R) // P
    return R[:ns * P].reshape(ns, P)


def fold_energy(ya, L, W, P):
    n = len(ya) - L
    cs = np.concatenate([[0], np.cumsum(np.abs(ya[:n]) ** 2 + np.abs(ya[L:L + n]) ** 2)])
    R = cs[W:] - cs[:-W]
    ns = len(R) // P
    return R[:ns * P].reshape(ns, P)


_TRI = {}


def tri_smooth(rho, P, W):
    k = (P, W)
    if k not in _TRI:
        t = np.zeros(P)
        for d in range(-W, W + 1):
            t[d % P] = 1 - abs(d) / W
        _TRI[k] = np.fft.fft(t / t.sum())
    return np.real(np.fft.ifft(np.fft.fft(rho) * _TRI[k]))


def circ(P, g):
    return np.minimum((np.arange(P) - g) % P, (g - np.arange(P)) % P)


def detect(a, b, P, W):
    """The statistic. a: complex folded sum, b: its energy. Returns gamma, dpk, rho at gamma.
    Pass 1 finds the bump on the triangle-smoothed |a|; pass 2 takes the tone pedestal as the
    complex mean of the offsets away from it and scores rho at the matched peak against the mean
    of those offsets."""
    rho0 = 2 * np.abs(a) / (b + 1e-30)
    g0 = int(np.argmax(tri_smooth(rho0, P, W)))
    ped = a[circ(P, g0) > GUARD].mean()
    rho = 2 * np.abs(a - ped) / (b + 1e-30)
    g = int(np.argmax(tri_smooth(rho, P, W)))
    off = circ(P, g) > GUARD
    return g, rho[g] - rho[off].mean(), rho[g]


def windows(y0, y1, wave, win_s=1.0):
    """Per window (both arms power-summed, as the C module does): gamma, dpk."""
    L, W, P = wave['L'], wave['W'], wave['P']
    A = fold(y0, y0, L, W, P) + fold(y1, y1, L, W, P)
    B = fold_energy(y0, L, W, P) + fold_energy(y1, L, W, P)
    ws = int(win_s * FS / P)
    return np.array([detect(A[i:i + ws].sum(0), B[i:i + ws].sum(0), P, W)
                     for i in range(0, A.shape[0] - ws + 1, ws)])


def lock(res, t1=T_LOCK, P=160):
    """Locked from the first window of a run of LOCK_K in LOCK_N above t1 with the bump in place.
    P is the fold period (160 for V2, 192 for V1), for the wrap-around of the bump position."""
    h = P // 2
    out = np.zeros(len(res), bool)
    for i in range(LOCK_N - 1, len(res)):
        w = res[i - LOCK_N + 1:i + 1]
        ok = w[w[:, 1] > t1]
        if len(ok) >= LOCK_K:
            g = ok[:, 0]
            ref = np.median(((g - g[0] + h) % P) - h) + g[0]
            if (np.abs(((g - ref + h) % P) - h) <= LOCK_TOL).sum() >= LOCK_K:
                out[i] = True
    return out


def cmd_scan(args):
    for path in args.files:
        for mirror in (0, 1):
            y0, y1, off = arms(path, mirror, args.max_s)
            r = windows(y0, y1, V2)
            lk = lock(r)
            print(f'{os.path.basename(path)} bank {mirror} off={off}: dpk median {np.median(r[:, 1]):.2f} '
                  f'p90 {np.percentile(r[:, 1], 90):.2f}  windows>{T_LOCK}: {(r[:, 1] > T_LOCK).mean() * 100:3.0f}%  '
                  f'locked: {lk.mean() * 100:3.0f}%')


def wrap(x):
    return (x + np.pi) % (2 * np.pi) - np.pi


def cross_est(y0, y1, gates, win=50, wave=V2):
    """Per window: inter-arm phase by CP-gated lag-128 (pedestal out), and by lag-0 cross-spectrum."""
    L, W, P = wave['L'], wave['W'], wave['P']
    R00, R10, R01 = fold(y0, y0, L, W, P), fold(y1, y0, L, W, P), fold(y0, y1, L, W, P)
    out = []
    for k, i in enumerate(range(0, R00.shape[0] - win + 1, win)):
        g = gates[k]
        off = circ(P, g) > GUARD

        def pk(R):
            s = R[i:i + win].sum(0)
            return s[g] - s[off].mean()
        th = np.angle(pk(R00))
        x = pk(R10) * np.exp(-1j * th) + np.conj(pk(R01) * np.exp(1j * th))
        sl = slice(i * P, (i + win) * P)
        out.append((np.angle(x), np.angle(np.sum(y1[sl] * np.conj(y0[sl])))))
    return np.array(out)


def inject(kind, n, rng):
    if kind == 'noise':
        z = band(rng.standard_normal(n) + 1j * rng.standard_normal(n), V2['lo'], V2['hi'])
    elif kind == 'tone':
        z = np.exp(2j * np.pi * 1311.3 * np.arange(n) / FS)
    elif kind == 'comb':      # lines on the modem's own 62.5 Hz grid: period 128, worst case at lag 128
        k = np.arange(n)
        z = sum(np.exp(2j * np.pi * (1062.5 + 62.5 * m) * k / FS + 1j * rng.uniform(0, 6.28)) for m in range(14))
    else:
        z = np.zeros(n, complex)
        idx = rng.integers(0, n, n // 400)
        z[idx] = (rng.standard_normal(len(idx)) + 1j * rng.standard_normal(len(idx))) * 30
        z = band(z, V2['lo'], V2['hi'])
    return z / np.sqrt(np.mean(np.abs(z) ** 2))


def cmd_cross(args):
    rng = np.random.default_rng(1)
    y0, y1, _ = arms(args.file, args.bank, args.max_s)
    P, W, L = V2['P'], V2['W'], V2['L']

    def gates(a0):
        A = fold(a0, a0, L, W, P)
        B = fold_energy(a0, L, W, P)
        return [detect(A[i:i + 50].sum(0), B[i:i + 50].sum(0), P, W)[0] for i in range(0, A.shape[0] - 49, 50)]
    g0 = gates(y0)
    base = cross_est(y0, y1, g0)
    pw = np.mean(np.abs(y0) ** 2)
    print(f'{os.path.basename(args.file)}: {len(g0)} windows; phase error against the clean CP estimate, '
          'median/90th pct (deg)')
    for kind in ('noise', 'tone', 'comb', 'impulse'):
        for rel in (-3, 3, 10):
            z = inject(kind, len(y0), rng)
            amp = np.sqrt(pw) * 10 ** (rel / 20)
            Y0, Y1 = y0 + amp * z, y1 + amp * np.exp(1j * np.deg2rad(110.0)) * z
            e = cross_est(Y0, Y1, g0)
            f = lambda x: '%5.1f/%5.1f' % tuple(np.percentile(np.degrees(np.abs(wrap(x - base[:, 0]))), (50, 90)))
            print(f'  {kind:8s} {rel:+3d} dB   lag128 CP {f(e[:, 0])}   lag0 {f(e[:, 1])}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('files', nargs='+')
    s.add_argument('--max-s', type=float, default=40)
    s.set_defaults(fn=cmd_scan)
    c = sub.add_parser('cross')
    c.add_argument('file')
    c.add_argument('--bank', type=int, default=1)
    c.add_argument('--max-s', type=float, default=40)
    c.set_defaults(fn=cmd_cross)
    a = ap.parse_args()
    a.fn(a)

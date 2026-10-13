#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Burst-gated weight estimation for interferers that switch
on and off on a repeating schedule (T-023, T-024): inverter and switch-mode
noise, which is a burst train at 100 Hz.

scan  [CAPTURE.divc ...]   which captures have a repeating on/off pattern
                           and how coherent it is between the arms.
score CAPTURE.divc ...     for every 3 kHz band: a weight from the first
                           half, applied to the second, three ways.

The envelope is the power of the burst-dominated bins of a short-time
transform (about 750 Hz bins, 1.3 ms frames at 192 kHz), so a wanted signal
in the passband does not shape the gate: it is derived from bins at least
6 kHz away from the band being scored. The gate is on when the envelope is
above the geometric mean of its 20th and 95th percentiles.

Three weights, w = -Cab/Cbb with the covariances taken over
  all   every frame (the plain least squares the engine uses now)
  on    burst-on frames only
  diff  burst-on minus burst-off (a stationary wanted signal cancels)

Scores, in dB, on the held-out half, relative to arm 0 alone in the band:
  total  power of the whole output (what is heard)
  on     power in burst-on frames (the interferer, and what is under it)
  off    power in burst-off frames (the wanted signal and the background:
         a rise here is what the weight costs it)

usage: burst_gate.py scan [CAPTURE.divc ...]    (default: captures/*.divc)
       burst_gate.py score CAPTURE.divc ...
"""
import glob
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from score_wideband import load_blocks

CAPDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '..', 'captures')


def frames(path):
    """STFT frames of both arms: A, B (frames x N), frequencies in Hz, rate."""
    h, bl = load_blocks(path)
    rate = h['rate']
    n = max(32, int(2 ** round(np.log2(rate / 750.0))))
    a = np.concatenate([b[3] for b in bl])
    b = np.concatenate([b[4] for b in bl])
    k = len(a) // n
    w = np.hanning(n)
    fa = np.fft.fft(a[:k * n].reshape(k, n) * w, axis=1)
    fb = np.fft.fft(b[:k * n].reshape(k, n) * w, axis=1)
    return fa, fb, np.fft.fftfreq(n, 1 / rate), rate, len(bl) * h['nfft'] / rate


def gate(p, sel):
    e = p[:, sel].sum(1)
    lo, hi = np.percentile(e, 20), np.percentile(e, 95)
    return e > (lo * hi) ** 0.5, e


def bursty(p, good):
    ratio = p.mean(0) / np.maximum(np.median(p, 0), 1e-30)
    sel = good & (ratio > 4)

    if sel.sum() < max(3, 0.03 * good.sum()):
        sel = good & (ratio > 2.5)

    return sel


def cov(a, b, m):
    if not m.any():
        return 0j, 0.0, 0.0

    return (a[m] * b[m].conj()).sum() / m.sum(), (abs(a[m]) ** 2).sum() / m.sum(), (abs(b[m]) ** 2).sum() / m.sum()


def line(e, rate_frames):
    """The strongest line of the envelope above 40 Hz: (Hz, dB over the median, 2nd harmonic dB)."""
    k = len(e)
    e = e / e.mean() - 1
    s = abs(np.fft.rfft(e * np.hanning(k))) ** 2
    f = np.fft.rfftfreq(k, 1 / rate_frames)
    m = (f > 20) & (f < rate_frames / 2 * 0.95)
    base = np.median(s[m])
    m2 = (f > 40) & (f < rate_frames / 2 * 0.95)
    i = np.argmax(s[m2])
    f2, s2 = f[m2], s[m2]
    j = np.argmin(abs(f2 - 2 * f2[i]))
    h2 = 10 * np.log10(s2[max(j - 1, 0):j + 2].max() / base) if 2 * f2[i] < f2[-1] else float('nan')
    return f2[i], 10 * np.log10(s2[i] / base), h2


def scan_one(path):
    try:
        fa, fb, f, rate, dur = frames(path)
        name = os.path.basename(path)[7:-5]

        if dur < 8:
            return dict(f=name, err='short')

        n = len(f)
        good = abs(f) < 0.45 * rate
        best = None

        for arm in (fa, fb):
            p = abs(arm) ** 2
            sel = bursty(p, good)

            if sel.sum() < 2:
                continue

            r = line(p[:, sel].sum(1), rate / n)

            if best is None or r[1] > best[1]:
                best = r

        if best is None:
            return dict(f=name, err='no bursty bins')

        p = abs(fa) ** 2 + abs(fb) ** 2
        sel = bursty(p, good)
        on, _ = gate(p, sel)
        idx = np.where(sel)[0]
        ceil = []

        for g in range(0, len(idx) - 3, 4):
            ii = idx[g:g + 4]
            c1, a1, b1 = cov(fa[:, ii], fb[:, ii], on)
            c0, a0, b0 = cov(fa[:, ii], fb[:, ii], ~on)
            da, db = a1 - a0, b1 - b0

            if da > 0 and db > 0:
                g2 = min(abs(c1 - c0) ** 2 / (da * db), 0.9999)
                ceil.append(-10 * np.log10(1 - g2))

        return dict(f=name, rate=rate, f0=best[0], snr=best[1], snr2=best[2],
                    onfrac=on.mean(), ceil=np.median(ceil) if ceil else float('nan'))
    except Exception as ex:
        return dict(f=os.path.basename(path), err=str(ex)[:60])


def score_one(path):
    fa, fb, f, rate, dur = frames(path)
    k, n = fa.shape
    good = abs(f) < 0.45 * rate
    p = abs(fa) ** 2 + abs(fb) ** 2
    half = k // 2
    width = 4                       # bins: 3 kHz at 192 kHz
    rows = []

    for lo in range(1, n // 2 - width, width):
        for sgn in (1, -1):
            ii = np.arange(lo, lo + width) * sgn

            if abs(f[ii]).max() > 0.45 * rate:
                continue

            near = np.zeros(n, bool)
            near[ii] = True
            near |= abs(f[:, None] - f[ii][None, :]).min(1) < 6000
            sel = bursty(p, good & ~near)

            if sel.sum() < 2:
                continue

            on, _ = gate(p, sel)
            a, b = fa[:, ii], fb[:, ii]
            tr, te = np.arange(k) < half, np.arange(k) >= half

            c1, a1, b1 = cov(a, b, on & tr)
            c0, a0, b0 = cov(a, b, ~on & tr)
            ca, aa, ba = cov(a, b, tr)
            da, db = a1 - a0, b1 - b0

            # only bands where the bursts are really there, and coherent: the
            # contrast in arm 0 of 6 dB or more and a gated coherence of 0.7
            if da <= 0 or db <= 0 or b1 <= 0 or ba <= 0 or a0 <= 0:
                continue

            if 10 * np.log10(a1 / a0) < 6 or abs(c1 - c0) ** 2 / (da * db) < 0.49:
                continue

            ws = dict(all=-ca / ba, on=-c1 / b1, diff=-(c1 - c0) / db)
            out = dict(band=float(f[ii].mean()))

            for nm, w in ws.items():
                y = a + w * b

                for lab, m in (('total', te), ('on', on & te), ('off', ~on & te)):
                    out[nm + '_' + lab] = 10 * np.log10((abs(y[m]) ** 2).sum() / (abs(a[m]) ** 2).sum())

            out['wmag'] = abs(ws['diff'])
            rows.append(out)

    return rows


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('scan', 'score'):
        print(__doc__)
        return

    cmd, paths = sys.argv[1], sys.argv[2:]

    if cmd == 'scan':
        paths = paths or sorted(glob.glob(os.path.join(CAPDIR, '*.divc')))

        with Pool(6) as pl:
            res = pl.map(scan_one, paths, chunksize=1)

        print(f"{'capture':16s} {'rate':>6s} {'f0 Hz':>7s} {'line dB':>8s} {'2f0 dB':>7s} {'on %':>5s} {'ceiling dB':>10s}")

        for r in sorted((r for r in res if 'err' not in r), key=lambda r: -r['snr']):
            print(f"{r['f']:16s} {r['rate']:6d} {r['f0']:7.2f} {r['snr']:8.1f} {r['snr2']:7.1f} "
                  f"{100 * r['onfrac']:5.0f} {r['ceil']:10.1f}")

        for r in res:
            if 'err' in r:
                print(f"{r['f']}: skipped ({r['err']})")
        return

    print(f"{'capture':16s} {'bands':>5s} | {'total dB: all':>13s} {'on':>6s} {'diff':>6s} | "
          f"{'on-frames dB: all':>17s} {'on':>6s} {'diff':>6s} | {'off-frames dB: all':>18s} {'on':>6s} {'diff':>6s}")
    print("(bands with 6 dB burst contrast and gated coherence >= 0.7 only; negative is a reduction vs arm 0 alone)")

    with Pool(6) as pl:
        res = pl.map(score_one, paths, chunksize=1)

    for pth, rows in zip(paths, res):
        if not rows:
            print(f"{os.path.basename(pth)[7:-5]:16s} no bands")
            continue

        def m(k):
            return np.median([r[k] for r in rows])

        better = sum(r['diff_on'] < r['all_on'] - 1 for r in rows)
        worse = sum(r['diff_on'] > r['all_on'] + 1 for r in rows)
        print(f"{os.path.basename(pth)[7:-5]:16s} {len(rows):5d} | {m('all_total'):13.1f} {m('on_total'):6.1f} {m('diff_total'):6.1f} | "
              f"{m('all_on'):17.1f} {m('on_on'):6.1f} {m('diff_on'):6.1f} | {m('all_off'):18.1f} {m('on_off'):6.1f} {m('diff_off'):6.1f} | diff better/worse than all by 1 dB on-frames: {better}/{worse}")


if __name__ == '__main__':
    main()

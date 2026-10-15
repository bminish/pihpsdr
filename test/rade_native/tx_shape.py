#!/usr/bin/env python3
"""Is the transmitted RADE spectrum what the modem makes, or has something in the chain reshaped it?

Reads a capture's _tx.wav (the WDSP TX output, taken when the Capture button was on and the radio was
transmitting) and its _meta.txt, and reports

  centre   the power-weighted centre frequency of the transmitted energy, on the side the modem is on,
           against what librade's own output has for speech: V1 +1457 Hz, V2 +1505 Hz (the carriers span
           750..2200 and 1062..1938). A speech equaliser, compressor or CFC left on moves it.
  tilt     the low third of the carriers against the top third, dB. The modem makes these equal.
  edges    the carrier at each end against the middle, dB (what the TX filter does to them).
  skirts   power 100..500 Hz outside the carriers, and beyond 1 kHz out, against the carriers.

PASS: |centre - ideal| <= 75 Hz and |tilt| <= 3 dB.

  tx_shape.py capture_stem [...]          (stem as in rade-captures/, e.g. 20261015-232319;
                                           RADE_CAPTURES=dir to look elsewhere)
"""
import glob, os, re, sys
import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CAP = os.environ.get("RADE_CAPTURES", os.path.join(ROOT, "rade-captures"))
IDEAL = {1: (1457.0, 750.0, 2200.0), 2: (1505.0, 1062.5, 1937.5)}      # centre, first and last carrier
N = 16384


def spectrum(fn):
    with open(fn, "rb") as f:
        h = f.read(44)
    rate = int(np.frombuffer(h[24:28], dtype="<u4")[0])
    n = (os.path.getsize(fn) - 44) // 8
    win = np.hanning(N)
    acc = np.zeros(N)
    cnt = 0
    with open(fn, "rb") as f:
        f.seek(44)
        while True:
            a = np.fromfile(f, dtype=np.float32, count=2 * N * 64)
            if a.size < 2 * N:
                break
            z = (a[0::2] + 1j * a[1::2]).astype(np.complex64)
            seg = z[: (len(z) // N) * N].reshape(-1, N)
            p = (np.abs(seg) ** 2).mean(1)
            keep = (p > 0.02 * p.max()) & (p > 1e-5)           # transmitting
            if keep.any():
                acc += (np.abs(np.fft.fft(seg[keep] * win, axis=1)) ** 2).sum(0)
                cnt += int(keep.sum())
    f = np.fft.fftshift(np.fft.fftfreq(N, 1.0 / rate))
    return f, np.fft.fftshift(acc / max(cnt, 1)), cnt * N / rate


def main():
    ok_all = True
    for stem in sys.argv[1:]:
        tx = glob.glob(os.path.join(CAP, stem + "*_tx.wav"))
        meta = glob.glob(os.path.join(CAP, stem + "*_meta.txt"))
        if not tx or not meta:
            print(stem, ": no _tx.wav / _meta.txt"); ok_all = False; continue
        m = open(meta[0]).read()
        wave = int(re.search(r"rade_waveform: V(\d)", m).group(1))
        ideal, c0, c1 = IDEAL[wave]
        f, P, secs = spectrum(tx[0])
        sgn = 1 if (f * P).sum() / P.sum() > 0 else -1          # which side of the dial the modem is on (WDSP's convention: + is below the dial)
        x = sgn * f
        cen = (x * P).sum() / P.sum()
        def lev(a, b):
            sel = (x >= a) & (x < b)
            return 10 * np.log10(P[sel].mean() + 1e-30)
        span = c1 - c0
        low, high, mid = lev(c0, c0 + span / 3), lev(c1 - span / 3, c1), lev(c0 + span / 3, c1 - span / 3)
        tilt = high - low
        ok = abs(cen - ideal) <= 75 and abs(tilt) <= 3.0
        ok_all &= ok
        print("%s  V%d, %.0f s transmitting, modem on the %s side of the dial" % (stem, wave, secs, "lower (LSB-like)" if sgn > 0 else "upper (USB-like)"))   # WDSP: + is below the dial
        print("   centre %+.0f Hz (modem's own: %+.0f)   tilt top third - bottom third %+.1f dB" % (cen, ideal, tilt))
        print("   edge carriers vs middle: first %+.1f dB, last %+.1f dB" % (lev(c0 - 10, c0 + 40) - mid, lev(c1 - 40, c1 + 10) - mid))
        print("   skirts vs carriers: 100..500 Hz outside %+.1f / %+.1f dB, beyond 1 kHz out %+.1f / %+.1f dB" %
              (lev(c0 - 500, c0 - 100) - mid, lev(c1 + 100, c1 + 500) - mid, lev(c0 - 3000, c0 - 1000) - mid, lev(c1 + 1000, c1 + 3000) - mid))
        print("   -> %s" % ("PASS" if ok else "FAIL: the spectrum has been reshaped (a speech equaliser / CFC / compressor?)"))
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()

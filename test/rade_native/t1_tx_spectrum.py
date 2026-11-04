#!/usr/bin/env python3
"""T-1: what does rade_tx() actually put out?

Reads interleaved float32 I/Q at 8 kHz (from rade_tx_iq). Reports, for the
complex signal and for its real part (what a real-audio chain sends):
  - power in the positive vs negative frequency halves (one-sidedness)
  - 99 % occupied band and -20 dB edges
  - PAPR and the 99.9 % crest factor
Optionally a bandpass (--bw, centred on --fc) is applied first to show what
the planned TX filter does to the peaks.

  t1_tx_spectrum.py [--fc 1475] [--bw 1600] file.iq.f32 ...
"""
import argparse, os
import numpy as np
from scipy.signal import welch, firwin, lfilter

FS = 8000.0


def stats(x, label):
    f, p = welch(x, FS, nperseg=2048, return_onesided=False, detrend=False,
                 scaling="density")
    o = np.argsort(f); f, p = f[o], p[o]
    tot = p.sum()
    pos = p[f > 0].sum() / tot
    neg = p[f < 0].sum() / tot
    c = np.cumsum(p) / tot
    lo, hi = f[np.searchsorted(c, 0.005)], f[np.searchsorted(c, 0.995)]
    pk = p.max()
    above = f[p > pk * 0.01]
    pw = np.abs(x) ** 2
    papr = 10 * np.log10(pw.max() / pw.mean())
    cf = 10 * np.log10(np.percentile(pw, 99.9) / pw.mean())
    print("  %-14s pos %5.1f%% neg %5.1f%%  99%% band %6.0f..%6.0f Hz"
          "  -20dB edges %6.0f..%6.0f  PAPR %4.1f dB  99.9%% %4.1f dB" %
          (label, 100 * pos, 100 * neg, lo, hi, above.min(), above.max(), papr, cf))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fc", type=float, default=None)
    ap.add_argument("--bw", type=float, default=None)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    for fn in a.files:
        d = np.fromfile(fn, dtype=np.float32)
        x = (d[0::2] + 1j * d[1::2]).astype(np.complex128)
        print(os.path.basename(fn), "%.1f s, rms %.3f" % (len(x) / FS, np.sqrt(np.mean(np.abs(x) ** 2))))
        stats(x, "complex")
        stats(x.real.astype(np.complex128), "real part")
        if a.fc and a.bw:
            h = firwin(401, a.bw / 2, fs=FS) * np.exp(2j * np.pi * a.fc * np.arange(401) / FS
                                                   - 2j * np.pi * a.fc * 200 / FS)
            y = lfilter(h, 1.0, x)[400:]
            stats(y, "filt %d@%d" % (a.bw, a.fc))


if __name__ == "__main__":
    main()

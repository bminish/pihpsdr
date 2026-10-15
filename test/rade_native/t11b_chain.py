#!/usr/bin/env python3
"""T-11 second pass: a RADE signal through the WDSP TX and RX chains.

  features -> rade_tx -> [x6 up, conj] -> WDSP TXA -> WDSP RXA -> [conj, /6]
           -> rade_rx -> decoded features

compared with the same without WDSP. The measure of the link is the
feature error: mean square error of the decoded against the transmitted
vocoder features (first 20 of 36 per frame), as a fraction of the feature
variance, after aligning in time. No vocoder is involved. Also reported:
sync fraction, SNR estimate, and the delay through the two WDSP chains.

  t11b_chain.py [--v2] [--scale S] [--rx-agc N] [--rx-fixed DB] [--no-alc]
"""
import argparse, os, subprocess, tempfile
import numpy as np
from scipy.signal import resample_poly, correlate

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "out")
PROBE = os.path.join(HERE, "wdsp_probe")
RXIQ = os.path.join(HERE, "rade_rx_iq")


def probe(kind, x, args):
    with tempfile.TemporaryDirectory() as d:
        fi, fo = os.path.join(d, "i"), os.path.join(d, "o")
        np.stack([x.real, x.imag], 1).astype(np.float32).tofile(fi)
        subprocess.run([PROBE, kind, "--wisdom", ROOT + "/"] + args + [fi, fo],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        y = np.fromfile(fo, dtype=np.float32).reshape(-1, 2)
    return y[:, 0] + 1j * y[:, 1]


def decode(x8, v2, tag):
    iq = os.path.join(OUT, tag + ".iq.f32")
    np.stack([x8.real, x8.imag], 1).astype(np.float32).tofile(iq)
    feat, st = os.path.join(OUT, tag + ".dec.feat"), os.path.join(OUT, tag + ".stats.csv")
    subprocess.run([RXIQ] + (["--v2"] if v2 else []) + [iq, feat, st], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = np.genfromtxt(st, delimiter=",", names=True)
    ins = s["sync"] == 1
    return (np.fromfile(feat, dtype=np.float32).reshape(-1, 36),
            100.0 * ins.mean(), s["snr_db"][ins].mean() if ins.any() else float("nan"))


def feature_error(tx, dec, clip=5):
    """Normalised MSE over the 20 used features, best frame alignment."""
    tx, dec = tx[:, :20], dec[:, :20]
    best = None
    for lag in range(-120, 121):
        a = tx[max(0, lag):]; b = dec[max(0, -lag):]
        n = min(len(a), len(b)) - 10
        if n < 50:
            continue
        a, b = a[clip:n], b[clip:n]
        e = np.mean((a - b) ** 2) / np.mean(a ** 2)
        if best is None or e < best[0]:
            best = (e, lag, n)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--v2", action="store_true")
    ap.add_argument("--scale", type=float, default=0.3)
    ap.add_argument("--rx-agc", type=int, default=3)
    ap.add_argument("--rx-fixed", type=float, default=None)
    ap.add_argument("--no-alc", action="store_true")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--prefix", default="brian", help="out/PREFIX.vN.{iq.f32,feat}")
    ap.add_argument("--clip-start", type=int, default=25,
                    help="feature frames ignored at the start (as loss.py --clip_start)")
    ap.add_argument("--tol", type=float, default=None,
                    help="exit 1 if |error(WDSP) - error(direct)| exceeds this")
    a = ap.parse_args()
    v = "v2" if a.v2 else "v1"
    fc, bw = (1500.0, 1000.0) if a.v2 else (1475.0, 1600.0)
    lo, hi = fc - bw / 2, fc + bw / 2
    d = np.fromfile(os.path.join(OUT, "%s.%s.iq.f32" % (a.prefix, v)), dtype=np.float32)
    x8 = d[0::2] + 1j * d[1::2]
    txf = np.fromfile(os.path.join(OUT, "%s.%s.feat" % (a.prefix, v)), dtype=np.float32).reshape(-1, 36)
    tag = a.tag or "t11b_" + v

    d0, s0, n0 = decode(x8, a.v2, tag + "_direct")
    e0 = feature_error(txf, d0, a.clip_start)
    print("direct (no WDSP):      sync %5.1f%%  snr %5.1f  feature error %.4f (lag %d)" %
          (s0, n0, e0[0], e0[1]))

    # WDSP convention: conj on the way in (normal = not inverted)
    x48 = np.conj(resample_poly(x8, 6, 1)) * a.scale
    txargs = ["--low", str(lo), "--high", str(hi), "--neutral"] + (["--alc", "0"] if a.no_alc else [])
    y = probe("tx", x48.astype(np.complex64), txargs)
    rxargs = ["--low", str(lo), "--high", str(hi), "--copy", "0"]
    rxargs += ["--agc", "0", "--fixed", str(a.rx_fixed)] if a.rx_fixed is not None else ["--agc", str(a.rx_agc)]
    z = probe("rx", y.astype(np.complex64), rxargs)

    # delay through TXA+RXA: lag of z against x48 (both in the WDSP domain)
    n = min(len(x48), len(z)) // 2
    c = correlate(z[:n], x48[:n], mode="full", method="fft")
    lag = np.argmax(np.abs(c)) - (n - 1)
    pw = np.abs(y[len(y)//4:]) ** 2
    print("WDSP TX out: rms %.3f  PAPR %.1f dB   (input rms %.3f)" %
          (np.sqrt(pw.mean()), 10*np.log10(pw.max()/pw.mean()), np.sqrt(np.mean(np.abs(x48)**2))))
    print("delay TXA+RXA: %d samples at 48 kHz = %.1f ms" % (lag, 1000.0 * lag / 48000))

    x8b = np.conj(resample_poly(z, 1, 6))
    rms = np.sqrt(np.mean(np.abs(x8b[len(x8b)//2:]) ** 2)) + 1e-30
    print("RX out rms %.4f" % rms)
    x8b = x8b / rms * 0.9               # level for the decoder, as an AGC would
    d1, s1, n1 = decode(x8b.astype(np.complex64), a.v2, tag + "_wdsp")
    e1 = feature_error(txf, d1, a.clip_start)
    print("through WDSP TX+RX:    sync %5.1f%%  snr %5.1f  feature error %.4f (lag %d)" %
          (s1, n1, e1[0], e1[1]))
    delta = abs(e1[0] - e0[0])
    print("|delta|: %.4f" % delta + ("  (tolerance %.4f)" % a.tol if a.tol is not None else ""))
    if a.tol is not None:
        print("PASS" if delta <= a.tol else "FAIL: error through WDSP differs from direct")
        raise SystemExit(0 if delta <= a.tol else 1)


if __name__ == "__main__":
    main()

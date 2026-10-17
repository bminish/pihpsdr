#!/usr/bin/env python3
"""AGC A/B for the RADE receive path under synthetic fading (lab; not on air).
  agc_ab.py KINDS SNRS SEEDS MODES TAG    e.g.  agc_ab.py flat1,flat5 3,6,9 10 off,long,slow,medium,fast m2
  KINDS awgn, flat1 (1 Hz Doppler spread), flat5 (5 Hz), poor (2 paths, 2 ms, 1 Hz); SNR in 3 kHz; results in out/agc/
modem (real speech through rade_tx, V1) -> fading + AWGN at 48 kHz (WDSP sign) -> wdsp_probe rx (live piHPSDR
filter set-up: nbp0 only, 2048 taps, edges 675..2275) with a chosen AGC -> engine front end (conj, 48-tap 6:1, 0.5 s level EMA)
-> librade -> feature error against what was sent, sync, SNR estimate."""
import os, sys, json, itertools
import numpy as np
from scipy.signal import resample_poly, lfilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t11b_chain as T
import filter_lab as L
HERE = os.path.dirname(os.path.abspath(__file__))
S = os.path.join(HERE, "out", "agc"); os.makedirs(S, exist_ok=True)
T.OUT = S
FS = 48000
IN_RMS = 3e-4          # mean signal level into WDSP (about -70 dBm re 1.0 = 0 dBm); inside the AGC's working range

def fade(n, spread_hz, rng):
    """unit-power complex Gaussian fading, Gaussian Doppler spectrum with 2*sigma = spread_hz, at 48 kHz."""
    fr = 400.0; m = int(n / FS * fr) + 64
    w = rng.standard_normal(m) + 1j * rng.standard_normal(m)
    f = np.fft.fftfreq(m, 1 / fr)
    h = np.fft.ifft(np.fft.fft(w) * np.exp(-f ** 2 / (2 * (spread_hz / 2) ** 2)))
    h /= np.sqrt(np.mean(np.abs(h) ** 2))
    t = np.arange(n) / FS * fr
    return np.interp(t, np.arange(m), h.real) + 1j * np.interp(t, np.arange(m), h.imag)

def channel(x, kind, rng):
    n = len(x)
    if kind == "awgn":
        y = x
    elif kind == "flat1":
        y = x * fade(n, 1.0, rng)
    elif kind == "flat5":
        y = x * fade(n, 5.0, rng)
    elif kind == "poor":                       # two equal paths, 2 ms apart, 1 Hz spread each (CCIR poor)
        d = int(0.002 * FS)
        y = (x * fade(n, 1.0, rng) + np.concatenate([np.zeros(d), x[:-d]]) * fade(n, 1.0, rng)) / np.sqrt(2)
    return y

dec_h = None
def front_end(z):
    """what rade_native_rx_block does to the WDSP output."""
    global dec_h
    if dec_h is None:
        k = np.arange(48); m = k - 23.5; fc = 3200.0 / 48000
        sinc = np.where(np.abs(m) < 1e-9, 2 * fc, np.sin(2 * np.pi * fc * m) / (np.pi * np.where(m == 0, 1, m)))
        dec_h = sinc * (0.42 - 0.5 * np.cos(2 * np.pi * k / 47) + 0.08 * np.cos(4 * np.pi * k / 47))
    z = np.conj(z)                                        # normal (not inverted) sideband
    y = lfilter(dec_h, 1, z)[::6]
    p = np.abs(y) ** 2
    pw = lfilter([1 / 4000.0], [1, -(1 - 1 / 4000.0)], p)  # EMA, tau 4000 samples at 8 kHz
    return y * (0.9 / np.sqrt(pw + 1e-18))

AGC = {"off": ["--agc", "0", "--fixed", "50"], "long": ["--agc", "1"], "slow": ["--agc", "2"],
       "medium": ["--agc", "3"], "fast": ["--agc", "4"]}

def run(kind, snr, seed, modes, x8, txf):
    rng = np.random.default_rng(1000 * seed + 17)
    x48 = np.conj(resample_poly(x8, 6, 1))
    x48 = x48 / np.sqrt(np.mean(np.abs(x48[np.abs(x48) > 1e-6 * np.abs(x48).max()]) ** 2)) * IN_RMS
    y = channel(x48, kind, rng)
    sigp = np.mean(np.abs(y) ** 2)
    sigma = np.sqrt(sigp / (10 ** (snr / 10) * 3000.0) * FS)       # noise power in 3 kHz = signal power / snr
    y = y + sigma * (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y))) / np.sqrt(2)
    out = {}
    for name in modes:
        a = ["--low", "675", "--high", "2275", "--copy", "0", "--nobp"] + AGC[name]
        z = T.probe("rx", y.astype(np.complex64), a)
        x8b = front_end(z.astype(np.complex128))
        d, s, n = T.decode(x8b.astype(np.complex64), False, "agc_%s_%d" % (name, os.getpid()))
        e = T.feature_error(txf, d, 25)
        out[name] = (e[0] if e else 1.0, s, n)
    return out

if __name__ == "__main__":
    kinds = sys.argv[1].split(","); snrs = [float(v) for v in sys.argv[2].split(",")]; seeds = int(sys.argv[3])
    modes = sys.argv[4].split(",")
    x8, txf = L.modem("v1", 40)
    res = {}
    for kind in kinds:
        for snr in snrs:
            acc = {m: [] for m in modes}
            for sd in range(seeds):
                r = run(kind, snr, sd, modes, x8, txf)
                for m in modes: acc[m].append(r[m])
            line = "%-6s snr %5.1f | " % (kind, snr) + "  ".join(
                "%s %.3f/%3.0f%%" % (m, np.mean([v[0] for v in acc[m]]), np.mean([v[1] for v in acc[m]])) for m in modes)
            print(line, flush=True)
            res["%s_%g" % (kind, snr)] = {m: acc[m] for m in modes}
    json.dump(res, open(os.path.join(S, "agc_ab_%s.json" % sys.argv[5]), "w"), default=float)

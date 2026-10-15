#!/usr/bin/env python3
"""How close to the ALC ceiling can the RADE level go before it hurts?

Speech (a capture's _txmic.wav, 16 kHz) -> rade_tx -> modem I/Q (8 kHz) -> x level -> WDSP TXA (neutral, ALC on, as
the engine drives it) -> optional noise -> WDSP RXA (medium AGC) -> librade RX. For each level (dB relative to the
engine's own: V1 0.357, V2 0.504) it reports

  peak       the largest |I+jQ| leaving TXA, dBFS (the ALC ceiling is 0 dBFS, 1.0)
  gain       TXA output rms against input rms, dB: falls when the ALC is pulling the signal down
  ferr       mean square error of the decoded against the transmitted vocoder features, fraction of variance
  sync, snr  librade's, in sync

Noise is white, added after TXA, at a level fixed in absolute terms (not following the signal), so that more level
means more SNR, as on the air; --snr says what SNR the *reference* level has before the receiver's filters.
Clean runs (no noise) show distortion alone.

  alc_sweep.py capture_stem [--snr 6] [--levels -3,-1.5,0,1.5,3,4.5,6,9]
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import t11b_chain as T

ROOT = T.ROOT
B = os.path.join(ROOT, "third_party", "rade_c-build", "src")
CUR = {1: 0.357, 2: 0.504}


def modem(wav, v2, tag):
    o = T.OUT
    feat = os.path.join(o, tag + ".feat")
    subprocess.run([os.path.join(B, "rade_tx_wav"), "-v", "0"] + (["--v2"] if v2 else []) + ["-f", feat, wav,
                   os.path.join(o, tag + ".real.wav")], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    iq = os.path.join(o, tag + ".iq.f32")
    subprocess.run([os.path.join(HERE, "rade_tx_iq")] + (["--v2", "--no_bpf"] if v2 else []) + [feat, iq],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = np.fromfile(iq, dtype=np.float32)
    return d[0::2] + 1j * d[1::2], np.fromfile(feat, dtype=np.float32).reshape(-1, 36)


def run(x8, txf, v2, level, noise_rms, tag, rng):
    fc, bw = (1500.0, 1000.0) if v2 else (1475.0, 1600.0)
    lo, hi = fc - bw / 2, fc + bw / 2
    x48 = np.conj(resample_poly(x8, 6, 1)) * level
    y = T.probe("tx", x48.astype(np.complex64), ["--low", str(lo), "--high", str(hi), "--neutral"])
    act = np.abs(y) > 1e-3
    yy = y[len(y) // 4:]
    peak = np.abs(yy).max()
    gain = 20 * np.log10(np.sqrt(np.mean(np.abs(yy) ** 2)) / np.sqrt(np.mean(np.abs(x48[len(x48)//4:]) ** 2)))
    if noise_rms > 0:
        y = y + noise_rms / np.sqrt(2) * (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y)))
    z = T.probe("rx", y.astype(np.complex64), ["--low", str(lo), "--high", str(hi), "--copy", "0", "--agc", "3"])
    x8b = np.conj(resample_poly(z, 1, 6))
    x8b = x8b / (np.sqrt(np.mean(np.abs(x8b[len(x8b)//2:]) ** 2)) + 1e-30) * 0.9
    d, s, n = T.decode(x8b.astype(np.complex64), v2, tag)
    e = T.feature_error(txf, d, 25)
    return peak, gain, e[0], s, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stem")
    ap.add_argument("--snr", type=float, default=6.0)
    ap.add_argument("--levels", default="-3,-1.5,0,1.5,3,4.5,6,9")
    ap.add_argument("--seconds", type=float, default=30.0)
    a = ap.parse_args()
    wav = os.path.join(ROOT, "rade-captures", a.stem + "_14250kHz_RADE-V1_txmic.wav")
    if a.seconds > 0:       # trim to the first N seconds of speech
        from scipy.io import wavfile
        r, x = wavfile.read(wav)
        d = tempfile.mkdtemp()
        wav = os.path.join(d, "mic.wav")
        wavfile.write(wav, r, x[: int(a.seconds * r)])
    levels = [float(v) for v in a.levels.split(",")]
    rng = np.random.default_rng(1)
    for v2 in (False, True):
        w = "V2" if v2 else "V1"
        x8, txf = modem(wav, v2, "alcs_" + w.lower())
        cur = CUR[2 if v2 else 1]
        # reference noise: the TXA output rms at the current level, divided by the SNR
        yref = T.probe("tx", np.conj(resample_poly(x8, 6, 1) * cur).astype(np.complex64),
                       ["--low", str(1500 - 500 if v2 else 1475 - 800), "--high", str(1500 + 500 if v2 else 1475 + 800), "--neutral"])
        yref = yref[len(yref) // 4:]
        nrms = np.sqrt(np.mean(np.abs(yref) ** 2)) / 10 ** (a.snr / 20)
        print("\n%s  (current level %.3f; reference noise = %.1f dB below the signal at the current level, white, 48 kHz)" % (w, cur, a.snr))
        print("  level  scale   peak dBFS  TXA gain   | clean: ferr  sync  snr | noisy: ferr  sync  snr")
        for db in levels:
            lv = cur * 10 ** (db / 20)
            p, g, e0, s0, n0 = run(x8, txf, v2, lv, 0.0, "alcs_c", rng)
            _, _, e1, s1, n1 = run(x8, txf, v2, lv, nrms, "alcs_n", rng)
            print("  %+5.1f  %.3f   %+6.2f    %+6.2f     |  %.4f %5.1f %5.1f |  %.4f %5.1f %5.1f" %
                  (db, lv, 20 * np.log10(p), g, e0, s0, n0, e1, s1, n1))


if __name__ == "__main__":
    main()

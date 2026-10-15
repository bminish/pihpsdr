#!/usr/bin/env python3
"""T-0: reference decode of the V2 clips with librade's own rade_rx_wav.

For each input WAV: resample to 8 kHz mono, run rade_rx_wav --v2 with the
per-symbol dumps, and report sync fraction, SNR and frequency offset while
in sync. These are the numbers a native decoder has to match.

  t0_reference.py [--invert] [--out DIR] clip.wav ...

--invert mirrors the spectrum about the 1500 Hz modem centre (f -> 3000 - f),
which is what a wrongly-inverted station looks like once the dial has been
moved to bring it into the passband. A real audio file cannot be conjugated
about zero, so this is done through the analytic signal.
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, hilbert
from math import gcd

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "third_party/rade_c-build/src/rade_rx_wav")


def to_8k(path, invert):
    fs, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x[:, 0]
    g = gcd(8000, fs)
    x = resample_poly(x, 8000 // g, fs // g)
    if invert:
        # f -> 3000 - f : Re{ conj(analytic) * exp(j 2 pi 3000 n / 8000) }
        a = hilbert(x)
        n = np.arange(len(x))
        x = np.real(np.conj(a) * np.exp(2j * np.pi * 3000.0 * n / 8000.0))
    pk = np.max(np.abs(x)) or 1.0
    return (x / pk * 16000).astype(np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--v1", action="store_true")
    ap.add_argument("--out", default=os.path.join(ROOT, "test/rade_native/out"))
    ap.add_argument("clips", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    print("%-44s %7s %7s %7s %8s %8s" %
          ("clip", "secs", "sync%", "snr_dB", "foff_Hz", "foff_sd"))
    for c in a.clips:
        base = os.path.splitext(os.path.basename(c))[0]
        w8 = os.path.join(a.out, base + ".8k.wav")
        wavfile.write(w8, 8000, to_8k(c, a.invert))
        d = lambda s: os.path.join(a.out, base + "." + s)
        cmd = [TOOL, "-v", "0", w8, d("dec.wav")]
        if not a.v1:
            cmd += ["--v2", "--write_state", d("state.i16"),
                    "--write_snr_est", d("snr.f32"),
                    "--write_freq_offset", d("foff.f32")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(base, "FAILED", r.stderr.strip()[:200]); continue
        secs = os.path.getsize(w8) / 2 / 8000.0
        if a.v1:
            print("%-44s %7.1f   (V1: no per-symbol dumps)" % (base[:44], secs))
            continue
        st = np.fromfile(d("state.i16"), dtype=np.int16)
        snr = np.fromfile(d("snr.f32"), dtype=np.float32)
        fo = np.fromfile(d("foff.f32"), dtype=np.float32)
        n = min(len(st), len(snr), len(fo))
        s = st[:n] == 1
        if s.any():
            print("%-44s %7.1f %7.1f %7.1f %8.1f %8.1f" %
                  (base[:44], secs, 100.0 * s.mean(), snr[:n][s].mean(),
                   fo[:n][s].mean(), fo[:n][s].std()))
        else:
            print("%-44s %7.1f %7.1f" % (base[:44], secs, 0.0))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""How much of a mistuned or narrow-filtered RADE V1 signal still decodes, using our own captures.

A capture (_rx48.wav) is what WDSP delivered with the RADE passband on, so the signal sits inside it.
To ask "what if the station were Delta Hz off" the capture is shifted by Delta and passed through a
brickwall bandpass of the width being tested, centred where the passband is centred; the result goes to
librade (rade_decode_iq). Compared with the unshifted, current-width decode of the same stretch:
sync, SNR, calls decoded, and the vocoder features of the frames both decodes produced.

  filter_study.py capture_stem [--start S] [--dur D] [--widths 1200,1400,...] [--deltas -200,...]
"""
import argparse, json, os, subprocess, sys, glob, re
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CAP = os.path.join(ROOT, "rade-captures")
OUT = os.path.join(HERE, "out", "fs")
DEC = os.path.join(HERE, "rade_decode_iq")
FC = 1475.0
PREFIX = ""


def load(stem, start, dur):
    meta = open(glob.glob(os.path.join(CAP, stem + "*_meta.txt"))[0]).read()
    inv = int(re.search(r"rade_effective_invert: (\d)", meta).group(1))
    f = glob.glob(os.path.join(CAP, stem + "*_rx48.wav"))[0]
    fs, d = wavfile.read(f)
    i0, i1 = int(start * fs), int((start + dur) * fs)
    z = d[i0:i1, 0].astype(np.float64) + 1j * d[i0:i1, 1].astype(np.float64)
    if not inv:
        z = np.conj(z)                         # the engine's rule: WDSP convention -> librade's
    z = resample_poly(z, 1, 6)                 # 48 kHz -> 8 kHz
    rms = np.sqrt(np.percentile(np.abs(z[: len(z) // 8 * 8].reshape(-1, 8000)) ** 2, 75, axis=1).mean())
    return z / (rms + 1e-12) * 0.9, inv


def shaped(z, delta, width, edge=25.0):
    """Shift by delta Hz, then a bandpass of `width` centred on FC, raised-cosine edges `edge` Hz wide."""
    n = np.arange(len(z))
    y = z * np.exp(2j * np.pi * delta * n / 8000.0)
    F = np.fft.fft(y)
    f = np.fft.fftfreq(len(y), 1 / 8000.0)
    lo, hi = FC - width / 2, FC + width / 2
    m = np.clip((f - (lo - edge / 2)) / edge, 0, 1) * np.clip(((hi + edge / 2) - f) / edge, 0, 1)
    m = 0.5 - 0.5 * np.cos(np.pi * m)
    return np.fft.ifft(F * m)


def decode(z, tag):
    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, PREFIX + tag)
    np.stack([z.real, z.imag], 1).astype(np.float32).tofile(base + ".iq")
    subprocess.run([DEC, base + ".iq", base + ".csv", base + ".ev", base + ".feat", base + ".wav"],
                   check=True, capture_output=True)
    st = np.genfromtxt(base + ".csv", delimiter=",", names=True)
    feat = np.fromfile(base + ".feat", dtype=np.float32).reshape(-1, 36)
    ev = [l.split() for l in open(base + ".ev")]
    # time of every feature frame: the call time minus its position in the call
    times = []
    for t, n in zip(st["t_sec"], (st["nout"] / 36).astype(int)):
        times += [t - (n - 1 - k) * 0.01 for k in range(n)]
    os.remove(base + ".iq")
    return dict(sync=100 * float(np.mean(st["sync"] > 0.5)), snr=float(np.median(st["snr_db"][st["sync"] > 0.5])) if (st["sync"] > 0.5).any() else float("nan"),
                feat=feat, times=np.array(times), calls=[e[2] for e in ev if len(e) >= 3 and e[2] != "-"], eoo=len(ev))


def agreement(a, b):
    """Normalised error of b's features against a's, over frames both produced (matched in time)."""
    if len(a["times"]) < 100 or len(b["times"]) < 10:
        return None, 0
    j = np.searchsorted(a["times"], b["times"])
    j = np.clip(j, 1, len(a["times"]) - 1)
    near = np.where(np.abs(a["times"][j - 1] - b["times"]) < np.abs(a["times"][j] - b["times"]), j - 1, j)
    ok = np.abs(a["times"][near] - b["times"]) < 0.006
    if ok.sum() < 20:
        return None, int(ok.sum())
    A, B = a["feat"][near[ok]][:, :20], b["feat"][ok][:, :20]
    return float(np.mean((A - B) ** 2) / np.mean(A ** 2)), int(ok.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stem")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--dur", type=float, default=90.0)
    ap.add_argument("--widths", default="1200,1400,1600,1800,2000,2400")
    ap.add_argument("--deltas", default="-300,-200,-150,-100,-50,0,50,100,150,200,300")
    a = ap.parse_args()
    global PREFIX
    PREFIX = a.stem + "_"          # runs on different captures can go side by side
    z, inv = load(a.stem, a.start, a.dur)
    widths = [float(x) for x in a.widths.split(",")]
    deltas = [float(x) for x in a.deltas.split(",")]
    base = decode(shaped(z, 0, 1600.0), "base")
    print("%s  %.0f s from %.0f s, inverted=%d.  Reference (no shift, 1600 Hz): sync %.0f%%, SNR %.1f dB, %d frames, calls %s"
          % (a.stem, a.dur, a.start, inv, base["sync"], base["snr"], len(base["times"]), base["calls"]))
    print("rows: shift of the station from where the passband expects it; columns: filter width (Hz).")
    print("cells: sync % / SNR dB / feature error vs the reference decode (blank = too few frames)\n")
    print("%8s " % "shift" + "".join("%22d" % w for w in widths))
    res = {}
    for d in deltas:
        row = "%+6.0f Hz " % d
        for w in widths:
            r = decode(shaped(z, d, w), "t")
            e, n = agreement(base, r)
            res[(d, w)] = (r["sync"], r["snr"], e)
            row += "%22s" % ("%3.0f / %5.1f / %s" % (r["sync"], r["snr"], "%.3f" % e if e is not None else "  -  "))
        print(row)
    json.dump({"%g,%g" % k: v for k, v in res.items()}, open(os.path.join(OUT, a.stem + ".json"), "w"))


if __name__ == "__main__":
    main()

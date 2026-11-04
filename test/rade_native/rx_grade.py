#!/usr/bin/env python3
"""Grade V2 callsign reception of a freedv-gui transmission: piHPSDR's receive engine against freedv-gui's text receiver.

  rx_grade.py FDV_TX.wav --call EI6IZ [--snr 12,9,6,3] [--cycles 1,5] [--seeds 20] [--out DIR]

FDV_TX.wav is what freedv-gui sends (48 kHz mono, test/rade_native/README: freedv -ut tx -utmode RADEV2 recorded off its output).
It is made complex (analytic signal, modem at +1500 Hz, level of the transmit engine), cut to a window of N callsign cycles
(one cycle = 112 modem frames = 4.48 s for a call of up to 8 characters; window = 0.5 s lead + 0.7 s for sync + N cycles + 0.5 s
tail), flat complex noise is added so that the SNR is as given in 3 kHz (the convention of test_v2_text.py), and the same noisy
signal goes to

  fdv   librade V2 + freedv-gui's rade_text receiver, unmodified (v2_text_interop/interop --iq), at 8 kHz baseband
  ours  test_rx_engine --v2: the receive engine of src/rade_native.c (WDSP sign convention, sync as the gate)

A trial succeeds when the receiver delivers the callsign. Per receiver, window and SNR the success rate over --seeds noise
realisations (the same for both receivers) is printed, with the SNR at which it first reaches 50 % and 90 %.
Needs `make test_rx_engine v2_text_interop/interop`. Scratch files in out/rx_grade/ (git-ignored).
"""
import argparse, os, subprocess, sys, json
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import hilbert, resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "rx_grade")
CYCLE_S = 112 * 0.04
LEVEL = 0.27          # rms of the transmit engine's complex output


def load_tx(path):
    fs, x = wavfile.read(path)
    assert fs == 48000, "48 kHz expected"
    x = x.astype(np.float64) / 32768.0
    if x.ndim > 1:
        x = x[:, 0]
    z = hilbert(x)
    act = z[np.abs(z) > 0.01]
    return z * (LEVEL / np.sqrt(np.mean(np.abs(act) ** 2)))


def onset(z):
    b = 480
    e = np.array([np.mean(np.abs(z[i:i + b]) ** 2) for i in range(0, len(z) - b, b)])
    return int(np.argmax(e > 0.25 * np.median(e[e > 1e-6]))) * b


def window(z, t0, n_cycles):
    s = max(0, t0 - int(0.5 * 48000))
    n = int((0.5 + 0.7 + n_cycles * CYCLE_S + 0.5) * 48000)
    w = z[s:s + n]
    return np.concatenate([w, np.zeros(n - len(w), dtype=w.dtype)]) if len(w) < n else w


def noisy(w, snr_db, seed):
    act = w[np.abs(w) > 0.01]
    p = np.mean(np.abs(act) ** 2)
    sig = np.sqrt(p * (48000 / 3000) / 10 ** (snr_db / 10) / 2)
    rng = np.random.default_rng(seed)
    return w + sig * (rng.standard_normal(len(w)) + 1j * rng.standard_normal(len(w)))


def rx_fdv(x, call, tag):
    f = os.path.join(OUT, tag + ".iq8.f32")
    y8 = resample_poly(x, 1, 6)
    np.stack([y8.real, y8.imag], 1).astype(np.float32).tofile(f)
    r = subprocess.run([os.path.join(HERE, "v2_text_interop", "interop"), "--iq", f, call], capture_output=True, text=True)
    os.remove(f)
    return r.returncode == 0


def rx_ours(x, call, tag):
    f = os.path.join(OUT, tag + ".rxin.f32")
    xe = np.conj(x)                      # WDSP's convention is the conjugate of librade's
    np.stack([xe.real, xe.imag], 1).astype(np.float32).tofile(f)
    r = subprocess.run([os.path.join(HERE, "test_rx_engine"), "--v2", "--speed", "20", f, f + ".wav"], capture_output=True, text=True)
    os.remove(f); os.remove(f + ".wav")
    calls = [l.split()[1] for l in r.stdout.splitlines() if l.startswith("call: ")]
    return call in calls


def threshold(snrs, rates, level):
    """lowest SNR from which the rate stays at or above level (None if never)"""
    ok = None
    for s, r in sorted(zip(snrs, rates), reverse=True):
        if r >= level:
            ok = s
        else:
            break
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--call", default="EI6IZ")
    ap.add_argument("--snr", default="clean,12,9,6,3")
    ap.add_argument("--cycles", default="1,5")
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    for tool in ("test_rx_engine", "v2_text_interop/interop"):
        if not os.path.exists(os.path.join(HERE, tool)):
            sys.exit("build first: make test_rx_engine v2_text_interop/interop")
    os.makedirs(OUT, exist_ok=True)
    z = load_tx(a.wav)
    t0 = onset(z)
    snrs = [None if s == "clean" else float(s) for s in a.snr.split(",")]
    res = {}
    for n in [int(c) for c in a.cycles.split(",")]:
        w = window(z, t0, n)
        jobs = []
        for si, s in enumerate(snrs):
            for seed in range(1 if s is None else a.seeds):
                jobs.append((si, seed, w if s is None else noisy(w, s, 1000 * n + seed)))

        def run(j):
            si, seed, x = j
            tag = "n%d_s%d_%d" % (n, si, seed)
            return si, rx_fdv(x, a.call, tag), rx_ours(x, a.call, tag)

        with ThreadPoolExecutor(a.jobs) as ex:
            out = list(ex.map(run, jobs))
        tot = {}
        for si, f, o in out:
            t = tot.setdefault(si, [0, 0, 0]); t[0] += 1; t[1] += f; t[2] += o
        res[n] = {("clean" if snrs[si] is None else snrs[si]): {"trials": t[0], "fdv": t[1] / t[0], "ours": t[2] / t[0]} for si, t in tot.items()}
        print("\n%d cycle%s (window %.1f s), %d seeds per SNR" % (n, "" if n == 1 else "s", len(w) / 48000, a.seeds))
        print("  SNR dB   freedv-gui   piHPSDR")
        for si in sorted(tot):
            t = tot[si]
            print("  %-7s  %8.0f %%  %8.0f %%" % ("clean" if snrs[si] is None else "%g" % snrs[si], 100.0 * t[1] / t[0], 100.0 * t[2] / t[0]))
        ns = [(s, res[n][s]) for s in res[n] if s != "clean"]
        if ns:
            for lv in (0.5, 0.9):
                print("  SNR for >= %d %%:  freedv-gui %s   piHPSDR %s" % (100 * lv,
                      threshold([s for s, _ in ns], [r["fdv"] for _, r in ns], lv),
                      threshold([s for s, _ in ns], [r["ours"] for _, r in ns], lv)))
    json.dump(res, open(os.path.join(OUT, os.path.basename(a.wav) + ".json"), "w"), indent=1)


if __name__ == "__main__":
    main()

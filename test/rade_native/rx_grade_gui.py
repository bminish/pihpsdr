#!/usr/bin/env python3
"""Spot checks of rx_grade.py against the real freedv-gui GUI (the freedv-gui3 build, `-ut rx -utmode RADEV2 -rxfile`).

  rx_grade_gui.py FDV_TX.wav --call EI6IZ --cycles 1 --snr 3 --seeds 12 --conf fdv_rep.conf --freedv .../build_linux/src/freedv

For each seed the same noisy window rx_grade.py builds is reduced to its positive-frequency side as real audio (what an SSB receiver delivers), written as a 48 kHz wav,
played through the GUI's receive pipeline, and the GUI log is searched for `received CALL`. The GUI runs under bwrap with
no network (its V2 reporter host is hard-coded to the live reporter and its callsign decoding needs reporting on). The null
sinks the config names (fdv_tx, fdv_spk, fdv_mic) must exist and the config must have reporting enabled. Each trial takes
the window length plus about 10 s. Per seed, the GUI result is printed beside what rx_grade.py's two receivers said."""
import argparse, os, subprocess, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
import rx_grade as G


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav"); ap.add_argument("--call", default="EI6IZ")
    ap.add_argument("--cycles", type=int, default=1); ap.add_argument("--snr", type=float, required=True)
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--conf", required=True); ap.add_argument("--freedv", required=True)
    a = ap.parse_args()
    os.makedirs(G.OUT, exist_ok=True)
    z = G.load_tx(a.wav)
    w = G.window(z, G.onset(z), a.cycles)
    n_gui = n_fdv = n_our = 0
    for seed in range(a.seeds):
        x = G.noisy(w, a.snr, 1000 * a.cycles + seed)
        f, o = G.rx_fdv(x, a.call, "g%d" % seed), G.rx_ours(x, a.call, "g%d" % seed)
        # audio as an SSB receiver delivers it: only the positive-frequency side (100-3500 Hz) of the noisy baseband, as a real
        # signal. Taking the real part of x as it is would fold the noise of both sides into the audio: 3 dB worse than intended.
        X = np.fft.fft(x)
        fr = np.fft.fftfreq(len(x), 1 / 48000)
        X[(fr < 100) | (fr > 3500)] = 0
        y = np.fft.ifft(X).real
        y = y * (0.15 / np.sqrt(np.mean(y ** 2)))
        p = os.path.join(G.OUT, "gui_%d.wav" % seed)
        wavfile.write(p, 48000, (np.clip(y, -1, 1) * 32767).astype(np.int16))
        r = subprocess.run(["bwrap", "--dev-bind", "/", "/", "--unshare-net", "timeout", "120", a.freedv, "-f", a.conf, "-ut", "rx",
                            "-utmode", "RADEV2", "-rxfile", p], capture_output=True, text=True, cwd=os.path.dirname(a.freedv))
        g = ("received " + a.call) in r.stdout + r.stderr
        os.remove(p)
        print("seed %2d  GUI %-4s  freedv-gui text rx (offline) %-4s  piHPSDR %-4s" % (seed, g, f, o), flush=True)
        n_gui += g; n_fdv += f; n_our += o
    print("%d cycle(s), SNR %g dB, %d seeds:  GUI %d   freedv-gui offline %d   piHPSDR %d" % (a.cycles, a.snr, a.seeds, n_gui, n_fdv, n_our))


main()

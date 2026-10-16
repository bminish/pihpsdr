#!/usr/bin/env python3
"""How good is the Kiwi's GNSS time axis? Check it against MSF Anthorn (60 kHz), whose carrier is switched off for the first
100 ms of every UTC second: a ground-wave signal from about 280 km (about 1 ms), so the edges show any fixed delay between a
sample's time stamp and the moment the signal arrived (the Kiwi's decimation filters, mainly).

  kiwi_time_check.py [--seconds 60] [--host kiwi.minish.org] [--port 8074] [--use FILE.wav]

Records the IQ at 60 kHz with time stamps, folds the carrier envelope on the UTC second, and finds the half-amplitude fall and
rise of the 100 ms gap. Prints the offset of each edge (measured - expected) and their mean; writes it to
out/kiwi_time_offset.json, which air_timing.py subtracts from every event it takes from the recording (positive = the Kiwi's
axis is late). Needs an MSF signal; the carrier-to-gap ratio is shown. Accuracy: about +-2 ms (the transmitter's own edges and the 600 Hz
envelope filter included; they are symmetric, the mean of fall and rise cancels most of it).
"""
import argparse, json, os, subprocess, sys, time
import numpy as np
from scipy.signal import firwin

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kiwi_gps_wav as K

OUT = os.path.join(HERE, "out", "kiwi_time_offset.json")


def measure(path, ref_unix):
    r = K.read_kiwi_gps_wav(path, ref_unix)
    z = r.z.astype(np.complex128)
    S = np.abs(np.fft.fftshift(np.fft.fft(z[:2 ** 16] * np.hanning(2 ** 16)))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(2 ** 16, 1 / r.fs))
    f0 = float(f[np.argmax(S)])
    y = z * np.exp(-2j * np.pi * f0 * np.arange(len(z)) / r.fs)
    a = np.abs(np.convolve(y, firwin(301, 600, fs=r.fs), mode="same"))
    t = r.time(np.arange(len(a)))
    ph = ((t + 0.05) % 1.0) - 0.05
    lo, hi, nb = -0.05, 0.25, 300
    sel = (ph >= lo) & (ph < hi)
    bins = np.floor((ph[sel] - lo) / (hi - lo) * nb).astype(int)
    prof = np.bincount(bins, weights=a[sel], minlength=nb) / np.maximum(np.bincount(bins, minlength=nb), 1)
    tt = lo + (np.arange(nb) + 0.5) * (hi - lo) / nb
    on = float(np.median(np.concatenate([prof[tt < -0.01], prof[tt > 0.15]])))
    off = float(np.median(prof[(tt > 0.03) & (tt < 0.09)]))
    thr = 0.5 * (on + off)
    def cross(down):
        """First half-amplitude crossing in the window where the edge should be (fall: -40..60 ms, rise: 60..200 ms)."""
        w0, w1 = (-0.04, 0.06) if down else (0.06, 0.2)
        for i in range(len(prof) - 1):
            if not (w0 < tt[i] < w1):
                continue
            if (down and prof[i] > thr >= prof[i + 1]) or (not down and prof[i] < thr <= prof[i + 1]):
                return tt[i] + (thr - prof[i]) / (prof[i + 1] - prof[i]) * (tt[i + 1] - tt[i])
        return None
    fall, rise = cross(True), cross(False)
    return dict(carrier_hz=f0, gap_db=20 * np.log10(on / max(off, 1e-12)), fall_ms=1000 * fall if fall is not None else None,
                rise_ms=1000 * rise if rise is not None else None, seconds=len(z) / r.fs, fs=r.fs, solution=r.solution)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--host", default="kiwi.minish.org")
    ap.add_argument("--port", type=int, default=8074)
    ap.add_argument("--kiwiclient", default="~/sdr/kiwiclient")
    ap.add_argument("--use", help="measure an existing GPS-stamped IQ recording at 60 kHz instead of recording")
    a = ap.parse_args()
    path, ref = a.use, time.time() - 60
    if not path:
        d = os.path.join(HERE, "out", "timecheck"); os.makedirs(d, exist_ok=True)
        ref = time.time()
        subprocess.run([sys.executable, os.path.join(os.path.expanduser(a.kiwiclient), "kiwirecorder.py"), "-s", a.host, "-p", str(a.port),
                        "-f", "60", "-m", "iq", "--tlimit", str(a.seconds), "-d", d, "--fn", "msf60", "-u", "timecheck", "-q",
                        "--log-level", "warn", "-w", "--ncomp"], check=True, cwd=os.path.expanduser(a.kiwiclient))
        path = os.path.join(d, "msf60.wav")
    m = measure(path, ref)
    print("carrier at %.1f Hz, gap %.1f dB below the carrier, %.0f s, fs %.4f, GPS solution in %.0f%% of blocks" % (
        m["carrier_hz"], m["gap_db"], m["seconds"], m["fs"], 100 * m["solution"]))
    if m["fall_ms"] is None or m["rise_ms"] is None or m["gap_db"] < 6:
        print("no usable MSF second markers (gap under 6 dB, or an edge not found): no offset written")
        return 1
    off = 0.5 * (m["fall_ms"] + (m["rise_ms"] - 100.0))
    print("falling edge at %+.1f ms after the UTC second, rising edge at %+.1f ms (expected 0 and 100, plus about 1 ms of ground wave)" % (m["fall_ms"], m["rise_ms"]))
    print("Kiwi time axis late by %+.1f ms (mean of the two edges, less 1 ms of ground-wave delay: %+.1f ms)" % (off, off - 1.0))
    json.dump({"host": a.host, "offset_ms": round(off - 1.0, 2), "fall_ms": m["fall_ms"], "rise_ms": m["rise_ms"], "gap_db": m["gap_db"],
               "measured_unix": time.time(), "source": "MSF 60 kHz"}, open(OUT, "w"), indent=1)
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())

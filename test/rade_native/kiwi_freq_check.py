#!/usr/bin/env python3
"""Is the Kiwi on frequency? Tune it to a time-signal carrier (WWV/WWVH 10 MHz by default; 5, 15, 20, 25 MHz also) and measure how far
the carrier is from where it should be, in Hz and ppm. The Kiwi corrects its ADC clock against GPS, so a good GPS lock should give
a few hundredths of a ppm (0.02 ppm measured 2026-10-16: -0.22 Hz at 10 MHz). The same ppm applies at every frequency, so this
validates a transmit-frequency measurement at 28.6 MHz to about 0.5 Hz (the time stations' own carriers are good to 1e-11; the
ionosphere's Doppler adds up to a few tenths of a Hz, so a short recording in a quiet moment is the best test).

  kiwi_freq_check.py [--freq 10000] [--seconds 30] [--host H] [--port P] [--use FILE.wav]
"""
import argparse, os, subprocess, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kiwi_gps_wav as K


def measure(path, ref):
    r = K.read_kiwi_gps_wav(path, ref)
    z = r.z.astype(np.complex128)[2000:]
    N = len(z)
    S = np.abs(np.fft.fftshift(np.fft.fft(z * np.hanning(N), 8 * N))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(8 * N, 1 / r.fs))
    m = np.abs(f) < 200
    j = np.where(m)[0][np.argmax(S[m])]
    a, b, c = np.log(S[j - 1]), np.log(S[j]), np.log(S[j + 1])
    d = 0.5 * (a - c) / (a - 2 * b + c)
    return float(f[j] + d * (f[1] - f[0])), 10 * np.log10(S[j] / np.median(S[m])), r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--freq", type=float, default=10000.0, help="kHz")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--host", default="kiwi.minish.org")
    ap.add_argument("--port", type=int, default=8074)
    ap.add_argument("--kiwiclient", default="~/sdr/kiwiclient")
    ap.add_argument("--use")
    a = ap.parse_args()
    path, ref = a.use, time.time() - 60
    if not path:
        d = os.path.join(HERE, "out", "timecheck"); os.makedirs(d, exist_ok=True)
        ref = time.time()
        subprocess.run([sys.executable, os.path.join(os.path.expanduser(a.kiwiclient), "kiwirecorder.py"), "-s", a.host, "-p", str(a.port),
                        "-f", "%g" % a.freq, "-m", "iq", "--tlimit", str(a.seconds), "-d", d, "--fn", "freqcheck", "-u", "freqcheck", "-q",
                        "--log-level", "warn", "-w", "--ncomp"], check=True, cwd=os.path.expanduser(a.kiwiclient))
        path = os.path.join(d, "freqcheck.wav")
    off, snr, r = measure(path, ref)
    print("carrier %.1f dB over the noise; offset from %.0f kHz: %+.3f Hz = %+.3f ppm   (Kiwi fs %.4f Hz fitted from GPS, nominal %s; GPS solution in %.0f%% of blocks)" % (
        snr, a.freq, off, off / (a.freq * 1000) * 1e6, r.fs, r.nominal_fs, 100 * r.solution))
    if snr < 20:
        print("the carrier is weak (under 20 dB): the figure is not reliable")


if __name__ == "__main__":
    main()

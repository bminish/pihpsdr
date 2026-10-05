#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/tools/radev2-scoring.md.

A binaural WAV (the WAV button's or the ear recorder's file: left ear arm 0,
right ear arm 1, 48 kHz, 16-bit, the receiver's audio before the AF gain) as
two 8 kHz complex streams, which is what rade_c's V2 receiver takes:
decimate by 6, then the analytic signal, as rade_c's real2iq does. The V2
carriers are in the audio at 1062.5-1875 Hz, so nothing is mixed.

    wav2iq.py FILE.wav OUTDIR [--seconds N] [--conj]

writes OUTDIR/arm0.iq and arm1.iq (complex64, interleaved float32 I/Q).
--conj conjugates both (the other sideband sense).
"""
import argparse
import os
import sys

import numpy as np
from scipy.signal import hilbert, resample_poly

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ears import read_wav  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('out')
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--conj', action='store_true')
    a = ap.parse_args()
    x, rate = read_wav(a.wav)

    if rate % 8000:
        sys.exit('%s: %d Hz is not a multiple of 8000' % (a.wav, rate))

    if a.seconds:
        x = x[:int(a.seconds * rate)]

    os.makedirs(a.out, exist_ok=True)

    for k in range(2):
        y = resample_poly(x[:, k].astype(np.float64), 1, rate // 8000)
        z = hilbert(y)

        if a.conj:
            z = np.conj(z)

        z.astype(np.complex64).tofile(os.path.join(a.out, 'arm%d.iq' % k))

    print('%s: %.1f s, %d samples at 8 kHz per arm' % (a.wav, len(x) / rate, len(y)))


if __name__ == '__main__':
    main()

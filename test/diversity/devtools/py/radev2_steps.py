"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/diversity-radeV2-combining.md.

What a change in the combining weight costs the RADE V2 decoder.

radev2_oracle.py found that switching between antennas (and anything else
that steps the channel phase the decoder sees) decodes far worse than staying
on either one. V2 has no pilots and its decoder is stateful, so it must infer
the channel phase itself and a step in it has to be re-learned. This measures
that on one antenna in AWGN, where nothing else is going on, by applying to the
latents (per carrier DFT outputs) exactly what a weight change applies:

    phase steps    toggle between 0 and +theta every T seconds
    phase ramp     rotate at f Hz (a smooth, slewed weight)
    gain steps     toggle between 0 and +g dB every T seconds

Same front end as radev2_oracle.py (receiver BPF, one AGC gain, the receiver's
DFT, rade_dec_v2_test alone), so timing is known and this is the decoder only.

    python3 radev2_steps.py --work DIR [--snr 4,12]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import radev2_oracle as o  # noqa: E402
from radev2_calib import FS, load_features, noise  # noqa: E402

FRAME_S = 2 * o.SYM / FS           # 40 ms per decoder step


def apply(az, kind, a, b):
    t = np.arange(az.shape[0]) * FRAME_S

    if kind == 'phase_step':       # a: seconds between steps, b: degrees
        m = np.floor(t / a).astype(int) % 2
        g = np.exp(1j * np.radians(b) * m)
    elif kind == 'phase_ramp':     # a: Hz
        g = np.exp(2j * np.pi * a * t)
    elif kind == 'gain_step':      # a: seconds, b: dB
        m = np.floor(t / a).astype(int) % 2
        g = 10 ** (b * m / 20)
    else:
        raise ValueError(kind)

    return az * g[:, None, None]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--snr', default='4,12')
    ap.add_argument('--seeds', type=int, default=3)
    a = ap.parse_args()
    feat_in = load_features(os.path.join(a.work, 'feat_in.f32'))
    tx = np.fromfile(os.path.join(a.work, 'tx2.iq'), np.complex64).astype(complex)
    nfr, (l0, s0, shift) = o.calibrate(a.work, tx, feat_in)
    print('gate: clean loss %.4f\n' % l0)

    if l0 > 0.12:
        sys.exit('gate failed')

    S = np.mean(np.abs(tx[np.abs(tx) > 1e-6]) ** 2)
    tests = [('none', 0, 0)]
    tests += [('phase_step', T, th) for th in (30, 90, 180) for T in (0.16, 0.32, 0.64, 1.28, 2.56)]
    tests += [('phase_ramp', f, 0) for f in (0.25, 0.5, 1, 2, 4)]
    tests += [('gain_step', T, 3) for T in (0.16, 0.64, 2.56)]
    tests += [('gain_step', T, 6) for T in (0.16, 0.64, 2.56)]

    for snr in [int(x) for x in a.snr.split(',')]:
        print('### AWGN %+d dB, one antenna, mean loss over %d seeds\n' % (snr, a.seeds))
        print('| change | period / rate | size | loss | vs none |')
        print('|---|---|---|---|---|')
        base = None
        azs = []

        for sd in range(a.seeds):
            rng = np.random.default_rng(77 + sd)
            r = o.rx_bpf(tx + noise(len(tx), S * FS / (3000.0 * 10 ** (snr / 10)), rng))
            r = r * np.sqrt(o.AGC_TARGET2 / np.mean(np.abs(r[s0:s0 + nfr * 2 * o.SYM]) ** 2))
            azs.append(o.demod(r, s0, nfr))

        for kind, x, y in tests:
            ls = []

            for sd, az in enumerate(azs):
                z = az if kind == 'none' else apply(az, kind, x, y)
                ls.append(o.loss(feat_in, o.decode(o.latents(z), os.path.join(a.work, 'st%d' % sd)), shift))

            m = float(np.mean(ls))
            base = m if kind == 'none' else base
            desc = {'none': ('-', '-'),
                    'phase_step': ('every %.2f s' % x, '%d deg' % y),
                    'phase_ramp': ('%.2g Hz' % x, '-'),
                    'gain_step': ('every %.2f s' % x, '%d dB' % y)}[kind]
            print('| %s | %s | %s | %.3f | %+.3f |' % (kind, desc[0], desc[1], m, m - base))

        print()


if __name__ == '__main__':
    main()

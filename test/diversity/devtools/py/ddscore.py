#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/diversity-radeV2-combining.md section 13.

The decision-directed phase reference, on a recording. score_radev2 --lat-dir writes, for each
two-input receiver stream, every frame it decoded: arm 0's latents, arm 1's, and the combined ones the
decoder was given (NAME.lat, 3 x 56 floats a frame), and --csv-dir the per-symbol CSV whose valid rows
are those frames in order. This takes a stream's combined latents and, per run of consecutive frames,

    comb      decodes them as they are (what the receiver did)
    arm0      decodes arm 0's latents of the same frames
    ddcT      derotates them per carrier by the phase against the symbols the decoder returned,
              re-encoded and turned as the demodulator turns a carrier, an IIR over T symbols,
              centred, three passes   (radev2_oracle.derotate)
    ddmM_wH   the same with a phase model over 2H+1 symbols (radev2_oracle.derotate_model)

and scores each by the decoded |aux| (the lead measure) on the same frames. There is no truth on a
recording, so no loss; a frame-sync figure needs the receiver and is not computed. Decoder state is
restarted at each run, so the comb row can differ a little from the receiver's own figure, which is
printed beside it as a check.

    ddscore.py DIR NAME [--passes 3] [--model 2] [--h 3] [--tau 3]
"""
import argparse
import csv
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import radev2_oracle as o  # noqa: E402

NC = o.NC
ROT = np.exp(1j * np.radians(175.5 - 22.5 * np.arange(NC)))[None, None, :]    # what the demodulator does to a carrier


def to_complex(z):
    z = z.reshape(-1, 2, NC, 2)
    return z[..., 0] + 1j * z[..., 1]


def aux(f):
    return np.mean(f[:, :, 20], 1)                   # per frame, signed: the aux bit's soft decision


PERIOD = 112                                         # frames in the stations' repeating aux message (14 bytes)


def consensus(sig, k, half=4):
    """
    The aux bits repeat every PERIOD frames, so a bit has other copies: the same place in the
    periods around it. The majority of those, left-one-out, is a reference that does not depend on
    how sure the decoder was of the frame it is judging. Returns (consensus sign, usable) per frame.
    k: the frame's place in the receiver's decoded sequence (counting frames, not time: a decode that
    drops or repeats a frame moves the message's place as well as the count).
    """
    n = len(sig)
    q, p = k // PERIOD, k % PERIOD
    tab = {}

    for i in range(n):
        tab[(q[i], p[i])] = np.sign(sig[i]) or 1.0

    cons = np.zeros(n)
    ok = np.zeros(n, bool)

    for i in range(n):
        v = [tab[(qq, p[i])] for qq in range(q[i] - half, q[i] + half + 1) if qq != q[i] and (qq, p[i]) in tab]

        if len(v) >= half:
            cons[i] = np.sign(np.sum(v)) or 1.0
            ok[i] = np.sum(v) != 0

    return cons, ok


def runs(t):
    gap = np.abs(np.diff(t) - 0.04) > 0.008
    st = np.concatenate([[0], np.nonzero(gap)[0] + 1])
    return list(zip(st, np.concatenate([st[1:], [len(t)]])))


def dd(y, passes, derot, tmp):
    ycur = y

    for _ in range(passes):
        xh = o.encode_raw(o.decode_raw(o.latents(ycur), tmp + 'a'), tmp + 'b')
        n = min(len(xh), len(y))
        x = np.ones_like(y)
        x[:n] = xh[:n] * ROT
        ycur = derot(y, x)

    return ycur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('name')
    ap.add_argument('--passes', type=int, default=3)
    ap.add_argument('--model', type=int, default=2)
    ap.add_argument('--h', type=int, default=3)
    ap.add_argument('--tau', type=float, default=3.0)
    ap.add_argument('--ref', default='comb', help='the stream whose neighbouring copies make the consensus')
    a = ap.parse_args()
    raw = np.fromfile(os.path.join(a.dir, a.name + '.lat'), np.float32).reshape(-1, 3, 56)
    rows = [r for r in csv.DictReader(open(os.path.join(a.dir, a.name + '.csv'))) if r['valid'] == '1']
    t = np.array([float(r['t']) for r in rows])
    rx_aux = np.mean([abs(float(r['data'])) for r in rows])
    n = min(len(raw), len(t))
    raw, t = raw[:n], t[:n]
    z0, z1, zc = to_complex(raw[:, 0]), to_complex(raw[:, 1]), to_complex(raw[:, 2])
    z1 = z1 * np.sqrt(np.mean(np.abs(z0) ** 2) / (np.mean(np.abs(z1) ** 2) + 1e-30))      # arm 1 at arm 0's level
    streams = {'arm0': [], 'arm1': [], 'comb': [], 'ddc%g' % a.tau: [], 'ddm%d_w%d' % (a.model, a.h): []}
    kept = 0
    tmp = os.path.join(tempfile.mkdtemp(), 'dd')
    ks = []

    for s, e in runs(t):
        if e - s < 12:
            continue

        c = zc[s:e]
        kept += e - s
        ks.append(np.arange(s, e))                  # the frame's place in the decoded sequence
        streams['arm0'].append(aux(o.decode_raw(o.latents(z0[s:e]), tmp + 'x')))
        streams['arm1'].append(aux(o.decode_raw(o.latents(z1[s:e]), tmp + 'x')))
        streams['comb'].append(aux(o.decode_raw(o.latents(c), tmp + 'x')))
        streams['ddc%g' % a.tau].append(aux(o.decode_raw(o.latents(
            dd(c, a.passes, lambda y, x: o.derotate(y, x, a.tau, True), tmp)), tmp + 'x')))
        streams['ddm%d_w%d' % (a.model, a.h)].append(aux(o.decode_raw(o.latents(
            dd(c, a.passes, lambda y, x: o.derotate_model(y, x, a.model, a.h), tmp)), tmp + 'x')))

    print('%s/%s: %d frames in %d runs of %d or more (of %d decoded), %.1f s; receiver\'s own mean |aux| %.3f'
          % (a.dir, a.name, kept, len(streams['comb']), 12, n, kept * 0.04, rx_aux))
    base = np.concatenate(streams['comb'])
    k = np.concatenate(ks)
    cons, ok = consensus(np.concatenate(streams[a.ref]), k)
    print('  aux bit errors against the consensus of %s\'s copies %d frames either side (%d of %d frames usable)'
          % (a.ref, 4 * PERIOD, ok.sum(), len(k)))

    from scipy.stats import binomtest
    wrong = {}

    for name, v in streams.items():
        v = np.concatenate(v)
        wrong[name] = np.sign(v[ok]) != cons[ok]
        extra = ''

        if name.startswith('dd') or name == 'comb':    # paired against comb (or, for comb, arm 0): frames fixed against broken
            ref = 'arm0' if name == 'comb' else 'comb'
            b, c = int(np.sum(wrong[ref] & ~wrong[name])), int(np.sum(~wrong[ref] & wrong[name]))
            extra = '   fixed %d broke %d (p %.2g)' % (b, c, binomtest(b, b + c, 0.5).pvalue if b + c else 1.0)

        print('  %-10s mean |aux| %.3f (%+.3f)   aux bit errors %.3f%% of frames%s'
              % (name, np.abs(v).mean(), np.abs(v).mean() - np.abs(base).mean(), 100 * wrong[name].mean(), extra))


if __name__ == '__main__':
    main()

"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/diversity-radeV2-combining.md.

The oracle ladder: how good can a combination of two antennas be, as the
RADE V2 decoder judges it, before any estimator or receiver is built?

radev2_calib.py asks whether the probe's scores rank streams like the true
loss. This asks the question one step earlier: given PERFECT knowledge of the
channels, which way of presenting the two antennas does the decoder like best?
The rungs, each from the true per-carrier channel h0[c], h1[c] (held per
block, as the radio holds its weight):

    arm0, arm1   a single antenna
    sel          the better antenna per block (selection, with hindsight)
    scalar       one complex weight for the band (what the engine applies)
    sub3, sub4   one weight per sub-band
    perc         one weight per carrier, arm 0's phase kept, unit noise
    perc_raw     the same without the noise normalisation
    eq           per carrier MRC, channel phase removed
    strong       per carrier MRC, the stronger arm's phase kept
    eq0, ph0     arm 0 alone with its channel phase removed (ph0: phase only;
                 eq0: also weighted by |h|): what knowing the phase is worth
    fir8/16/32   perc's weights as time-domain filters (one per arm), L taps:
                 what the engine could do without touching the decoder

All but the fir rungs combine AFTER each arm's own DFT, which is what a
two-input rade_rx_v2 would do. The fir rungs combine in the time domain, so
the combined channel's delay spread is the sum of both arms' and the filter's.
fir against perc is the in-receiver advantage and nothing else.

The receiver is taken out of the loop on purpose: timing is known (it is a
synthetic transmission), the DFT is the receiver's own (window 16 samples into
the cyclic prefix, phase_corr, the receiver's input BPF and one common AGC
gain), and the latents go straight to rade_dec_v2_test, rade_c's decoder
alone. So this measures what the DECODER makes of each presentation, not the
sync. Frame sync and the blind estimator are later rungs.

    python3 radev2_oracle.py --work DIR [--quick] [--seeds N] [--hold FRAMES]

Needs third_party/rade_c built. Writes DIR/oracle.csv and prints the tables.
"""
import argparse
import os
import subprocess
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radev2_calib import (BLD, FS, RADE, NUSED, distortion_loss,  # noqa: E402
                          doppler_fade, load_features, noise)

DEC = os.path.join(BLD, 'rade_dec_v2_test')

NC, M, NCP, SYM = 14, 128, 32, 160
RIDGE = 0.05
CARRIER0 = 17                                   # 1062.5 Hz / 62.5 Hz
W = 2 * np.pi * (CARRIER0 + np.arange(NC)) / M  # rad/sample
WFWD = np.exp(-1j * np.outer(W, np.arange(M)))  # [c][n]
PCORR = np.exp(1j * 8 * W)
TIME_OFFSET = -16
BPF_NTAP = 101
AGC_TARGET2 = 0.5                               # 0.707 ** 2
RUNGS = ['arm0', 'arm1', 'sel', 'scalar', 'sub3', 'sub4', 'perc', 'perc_raw',
         'eq', 'strong', 'eq0', 'ph0', 'fir8', 'fir16', 'fir32']
ANTENNAS = ('arm0', 'arm1')


# ------------------------------------------------------------ receiver bits

def rx_bpf(x):
    """rade_bpf as rade_rx_v2_init configures it (101 taps, 975 Hz at 1469 Hz)."""
    bw = 1.2 * (W[-1] - W[0]) * FS / (2 * np.pi)
    centre = (W[-1] + W[0]) * FS / (2 * np.pi) / 2
    B = bw / FS
    n = np.arange(BPF_NTAP) - (BPF_NTAP - 1) // 2
    h = B * np.sinc(n * B)
    alpha = 2 * np.pi * centre / FS
    ph = np.exp(-1j * alpha * (np.arange(len(x)) + 1))
    y = np.convolve(x * ph, h)[:len(x)]
    return y * np.conj(ph)


def demod(x, s0, nfr):
    """Latents of nfr frames, symbol 0 of frame 0 starting at sample s0 (CP start)."""
    az = np.empty((nfr, 2, NC), complex)

    for f in range(nfr):
        for s in range(2):
            a = s0 + (2 * f + s) * SYM + NCP + TIME_OFFSET
            az[f, s] = (WFWD @ x[a:a + M]) * PCORR

    return az


def latents(az):
    """(nfr, 2, NC) complex -> (nfr, 56) float32, interleaved re/im."""
    z = np.empty((az.shape[0], 2, NC, 2), np.float32)
    z[..., 0], z[..., 1] = az.real, az.imag
    return z.reshape(az.shape[0], 56)


def decode(z, path):
    z.astype(np.float32).tofile(path + '.z')

    with open(path + '.z', 'rb') as fi, open(path + '.f', 'wb') as fo:
        r = subprocess.run([DEC], stdin=fi, stdout=fo, stderr=subprocess.DEVNULL)

    if r.returncode != 0:
        sys.exit('rade_dec_v2_test failed')

    out = np.fromfile(path + '.f', np.float32).reshape(-1, 4, 21)[:, :, :NUSED]
    os.remove(path + '.z')
    os.remove(path + '.f')
    return out.reshape(-1, NUSED)


def loss(feat_in, out, shift):
    """Mean distortion loss with output vector j against truth vector j+shift."""
    j0 = max(0, -shift)
    n = min(out.shape[0] - j0, feat_in.shape[0] - (j0 + shift))

    if n < 100:
        return np.nan

    return float(np.mean(distortion_loss(feat_in[j0 + shift:j0 + shift + n],
                                         out[j0:j0 + n])))


# ----------------------------------------------------------------- channel

def channel2(tx, kind, rng):
    """One antenna: its output and the two path gains + delay, for the oracle."""
    n = len(tx)
    z = np.zeros(n, complex)

    if kind == 'awgn':
        return tx.copy(), np.ones(n, complex), z, 0

    if kind == 'flat':
        a = doppler_fade(n, 0.5, rng)
        return a * tx, a, z, 0

    if kind == 'mpp':                           # as radev2_calib: 2 paths, 2 ms, 1 Hz
        d = int(0.002 * FS)
        a, b = doppler_fade(n, 1.0, rng), doppler_fade(n, 1.0, rng)
        y = a * tx
        y[d:] += b[d:] * tx[:-d]
        return y / np.sqrt(2), a / np.sqrt(2), b / np.sqrt(2), d

    raise ValueError(kind)


def block_h(a, b, d, t0, nfr, hold):
    """True H[c] per block of `hold` frames; timeline t0 = tx sample of frame 0."""
    nb = (nfr + hold - 1) // hold
    H = np.empty((nb, NC), complex)
    ph = np.exp(-1j * W * d)

    for k in range(nb):
        s = t0 + k * hold * 2 * SYM
        e = min(s + hold * 2 * SYM, len(a))
        H[k] = np.mean(a[s:e]) + np.mean(b[s:e]) * ph

    return H


# ------------------------------------------------------------- combinations

def groups(S):
    return np.array_split(np.arange(NC), S)


def weights_R(H0, H1, S):
    """R = h1/h0 per carrier, from S sub-band least-squares fits (S=NC: per carrier)."""
    R = np.empty_like(H0)

    for g in groups(S):
        R[:, g] = (np.sum(H1[:, g] * np.conj(H0[:, g]), axis=1)
                   / (np.sum(np.abs(H0[:, g]) ** 2, axis=1) + 1e-12))[:, None]

    return R


def per_block(az, hold):
    """Expand a (nb, NC) array to (nfr, 1, NC) so it multiplies az[f, s, c]."""
    return np.repeat(az, hold, axis=0)[:az.shape[0] * hold, None, :]


def combine(name, az0, az1, H0, H1, hold):
    nfr = az0.shape[0]

    def bx(x):
        return per_block(x, hold)[:nfr]

    if name == 'arm0':
        return az0

    if name == 'arm1':
        return az1

    if name == 'sel':
        use1 = bx((np.sum(np.abs(H1) ** 2, 1) > np.sum(np.abs(H0) ** 2, 1))[:, None]
                  * np.ones((1, NC)))
        return np.where(use1 > 0.5, az1, az0)

    if name in ('scalar', 'sub3', 'sub4', 'perc', 'perc_raw'):
        S = {'scalar': 1, 'sub3': 3, 'sub4': 4, 'perc': NC, 'perc_raw': NC}[name]
        R = bx(weights_R(H0, H1, S))
        y = az0 + np.conj(R) * az1
        return y if name == 'perc_raw' else y / np.sqrt(1 + np.abs(R) ** 2)

    if name in ('eq0', 'ph0'):                  # one antenna, channel known: the pilot-less tax
        h0 = bx(H0)
        return az0 * np.conj(h0) / (np.abs(h0) + 1e-12) * (np.abs(h0) if name == 'eq0' else 1)

    if name in ('eq', 'strong'):
        h0, h1 = bx(H0), bx(H1)
        y = (np.conj(h0) * az0 + np.conj(h1) * az1) / np.sqrt(np.abs(h0) ** 2 + np.abs(h1) ** 2 + 1e-12)

        if name == 'strong':
            ref = np.where(np.abs(h0) >= np.abs(h1), h0, h1)
            y = y * ref / (np.abs(ref) + 1e-12)

        return y

    raise ValueError(name)


def fir_combine(r0, r1, H0, H1, L, hold, s0, nfr):
    """
    perc's weights as a pair of time-domain filters, L taps each: A on arm 0
    and B on arm 1, with A = 1/sqrt(1+|R|^2) and B = conj(R)/sqrt(1+|R|^2) so
    both are bounded. (One filter on arm 1 alone, conj(R), is not: R blows up
    where arm 0 fades, and the taps with it.) Returns (stream, new s0).
    """
    D = L // 2
    E = np.exp(-1j * np.outer(W, np.arange(L)))
    # taps are charged by distance from the centre, so a long filter stays
    # compact where the target does not need it long (minimum-norm does not:
    # it fits the carriers exactly and is wild between them)
    P = np.diag(1e-3 + ((np.arange(L) - D) / max(D, 1)) ** 2)
    R = weights_R(H0, H1, NC)
    A = 1 / np.sqrt(1 + np.abs(R) ** 2)
    B = np.conj(R) * A
    y = np.zeros(len(r0), complex)
    blk = hold * 2 * SYM
    pad = [np.concatenate([np.zeros(L - 1, complex), r]) for r in (r0, r1)]

    for b in range(R.shape[0]):
        t0 = s0 + b * blk if b else 0                   # the first block also covers the lead-in
        t1 = len(r1) if b == R.shape[0] - 1 else s0 + (b + 1) * blk

        for resp, seg in ((A[b], pad[0]), (B[b], pad[1])):
            h = np.linalg.solve(E.conj().T @ E + RIDGE * P, E.conj().T @ (resp * np.exp(-1j * W * D)))
            y[t0:t1] += np.convolve(seg[t0:t1 + L - 1], h, mode='valid')

    return y, s0 + D


# ----------------------------------------------------------------- scenario

def energy(az):
    return float(np.mean(np.abs(az) ** 2))


def run_scenario(args):
    work, kind, snr, seed, hold, rungs, t0, nfr, s0, shift, tx, feat_in, clean = args
    rng = np.random.default_rng(1000 * seed + snr + 50)
    S = np.mean(np.abs(tx[np.abs(tx) > 1e-6]) ** 2)
    sigma2 = S * FS / (3000.0 * 10 ** (snr / 10))
    chans = []

    for _ in range(2):
        y, a, b, d = channel2(tx, kind, rng)
        rot = 1 if os.environ.get('NOROT') else np.exp(1j * rng.uniform(0, 2 * np.pi))  # a second antenna's path phase
        chans.append((y * rot, a * rot, b * rot, d))

    r = [rx_bpf(c[0] + noise(len(tx), sigma2, rng)) for c in chans]
    BPF_DELAY = (BPF_NTAP - 1) // 2
    a0 = s0 - BPF_DELAY                                  # tx sample of frame 0 (the tx BPF delay is in tx)
    H = [block_h(c[1], c[2], c[3], a0, nfr, hold) for c in chans]
    g = np.sqrt(AGC_TARGET2 / np.mean(np.abs(r[0][s0:s0 + nfr * 2 * SYM]) ** 2))
    r = [g * x for x in r]
    az0, az1 = demod(r[0], s0, nfr), demod(r[1], s0, nfr)
    ref = energy(az0)
    res = {}
    tag = os.path.join(work, '%s_%d_%d' % (kind, snr, seed))

    for name in rungs:
        if name.startswith('fir'):
            y, s0f = fir_combine(r[0], r[1], H[0], H[1], int(name[3:]), hold, s0, nfr)
            az = demod(y, s0f, nfr)
        else:
            az = combine(name, az0, az1, H[0], H[1], hold)

        az = az * np.sqrt(ref / energy(az))              # the AGC: same total power as arm 0
        res[name] = loss(feat_in, decode(latents(az), tag + '_' + name), shift)

    return kind, snr, seed, res


# --------------------------------------------------------------------- main

def calibrate(work, tx, feat_in):
    """Frame-0 start and truth alignment, from the clean transmission."""
    nfr = feat_in.shape[0] // 4
    nfr = min(nfr, (len(tx) - 400) // (2 * SYM))
    x = g_clean = rx_bpf(tx)
    best = None

    # the transmitter's own BPF and the receiver's each delay by 50 samples; the
    # cyclic prefix finds the symbol phase, the clean decode picks the frame parity
    m = np.zeros(SYM)

    for g in range(SYM):
        for s in range(100):
            p = g + SYM * (s + 5)
            m[g] += abs(np.sum(x[p:p + NCP] * np.conj(x[p + M:p + SYM])))

    for s0 in (int(np.argmax(m)), int(np.argmax(m)) + SYM):
        z = g_clean[s0:s0 + nfr * 2 * SYM]
        g = np.sqrt(AGC_TARGET2 / np.mean(np.abs(z) ** 2))
        out = decode(latents(demod(g * x, s0, nfr)), os.path.join(work, 'clean'))

        for shift in range(-20, 61):
            l = loss(feat_in, out, shift)

            if np.isfinite(l) and (best is None or l < best[0]):
                best = (l, s0, shift)

    return nfr, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--seeds', type=int, default=3)
    ap.add_argument('--hold', type=int, default=4, help='frames per weight block (4 = 160 ms)')
    ap.add_argument('--rungs', default=','.join(RUNGS))
    ap.add_argument('--kinds', default=None)
    ap.add_argument('--snrs', default=None)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    feat_f, tx_f = os.path.join(a.work, 'feat_in.f32'), os.path.join(a.work, 'tx2.iq')

    if not os.path.exists(tx_f):
        subprocess.run([os.path.join(BLD, 'lpcnet_demo'), '-features',
                        os.path.join(RADE, 'wav', 'all.wav'), feat_f],
                       check=True, capture_output=True)

        with open(feat_f, 'rb') as fi, open(tx_f, 'wb') as fo:
            subprocess.run([os.path.join(BLD, 'radae_tx'), '--v2'], stdin=fi, stdout=fo,
                           check=True, stderr=subprocess.DEVNULL)

    feat_in = load_features(feat_f)
    tx = np.fromfile(tx_f, np.complex64).astype(complex)
    nfr, (l0, s0, shift) = calibrate(a.work, tx, feat_in)
    print('gate: clean latents through rade_dec_v2_test: loss %.4f (rade_c publishes 0.080 '
          'for the whole receiver), frame 0 at sample %d, truth shift %d vectors' % (l0, s0, shift))

    if l0 > 0.12:
        sys.exit('gate failed: the offline demod does not reproduce a clean decode; not going on')

    kinds = (a.kinds.split(',') if a.kinds else (['flat', 'mpp'] if a.quick else ['awgn', 'flat', 'mpp']))
    snrs = ([int(s) for s in a.snrs.split(',')] if a.snrs else ([0, 4, 8] if a.quick else [-2, 0, 2, 4, 6, 8, 12]))
    rungs = a.rungs.split(',')
    jobs = [(a.work, k, s, sd, a.hold, rungs, 0, nfr, s0, shift, tx, feat_in, None)
            for k in kinds for s in snrs for sd in range(a.seeds)]

    with Pool(min(len(jobs), max(1, (os.cpu_count() or 2) - 2))) as p:
        rows = p.map(run_scenario, jobs)

    with open(os.path.join(a.work, 'oracle.csv'), 'w') as f:
        f.write('kind,snr,seed,' + ','.join(rungs) + '\n')

        for k, s, sd, res in rows:
            f.write('%s,%d,%d,%s\n' % (k, s, sd, ','.join('%.4f' % res[n] for n in rungs)))

    report(rows, kinds, snrs, rungs, l0)


def report(rows, kinds, snrs, rungs, clean):
    print('\nMean feature loss over seeds (lower is better; clean decode %.3f).\n' % clean)

    for kind in kinds:
        print('### %s\n' % kind)
        print('| SNR | ' + ' | '.join(rungs) + ' |')
        print('|---' * (len(rungs) + 1) + '|')

        for s in snrs:
            sel = [r[3] for r in rows if r[0] == kind and r[1] == s]
            print('| %+d dB | %s |' % (s, ' | '.join('%.3f' % np.nanmean([x[n] for x in sel]) for n in rungs)))

        print()

    print('### Scenarios in which a rung beats the better antenna by more than 0.01\n')
    print('| channel | ' + ' | '.join(n for n in rungs if n not in ANTENNAS) + ' |')
    print('|---' * (len(rungs) - 1) + '|')

    for kind in kinds:
        sel = [r[3] for r in rows if r[0] == kind]
        cells = []

        for n in rungs:
            if n in ANTENNAS:
                continue

            win = sum(1 for x in sel if x[n] < min(x['arm0'], x['arm1']) - 0.01)
            cells.append('%d of %d' % (win, len(sel)))

        print('| %s | %s |' % (kind, ' | '.join(cells)))


if __name__ == '__main__':
    main()

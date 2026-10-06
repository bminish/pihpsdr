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
    ref<p>       eq's output in the phase of a reference channel built from both arms,
                 sum_k |h_k|^(p-1) h_k (p = 1 equal weights, 2 amplitude, 4 sharper): a
                 reference that moves smoothly between the arms; bref<p>_<S>_<mode>_<tau>
                 is the same from the blind R
    pil<tau>, pilc<tau>
                 perc's output with the channel phase taken off by an estimate from KNOWN
                 symbols (the pilot bound: V2 has none), per carrier and its two neighbours,
                 IIR over tau symbols; pil causal, pilc centred (forward and backward)
    dd<tau>, ddc<tau>
                 the same with the symbols the decoder returned, put back through the
                 encoder (rade_enc_v2_test), turned as the demodulator turns a carrier
                 (dd_rotation): decision-directed, no pilot. dd delays the estimate by the
                 frame it waits for; ddb/pilb start from the blind R. dd[<mode><q>_]...[x<n>]:
                 n passes; pil/dd m<model>w<h> in place of c<tau>: a phase model over 2h+1
                 symbols (0 one phase for all carriers, 1 and a slope across them, 2 and a
                 rate in time) fitted by grid search (derotate_model); a mode (g true frame loss, a |aux|, i idempotence) keeps the best
                 q % of frames in the estimate
    eq           per carrier MRC, channel phase removed
    strong       per carrier MRC, the stronger arm's phase kept
    eq0, ph0     arm 0 alone with its channel phase removed (ph0: phase only;
                 eq0: also weighted by |h|): what knowing the phase is worth
    fir8/16/32   perc's weights as time-domain filters (one per arm), L taps:
                 what the engine could do without touching the decoder

The b* rungs replace the true channel with a blind estimate from the received
latents alone (blind_R): b<S>_<mode>_<tau>[_fir<L> | _h<N> | _i<N> | _c<N>]. S is
scalar, perc or perc3 (a carrier and its neighbours); mode u (C01/C00), k (noise
power taken off C00) or e (eigenvector); tau the IIR time constant in symbols; the
suffix applies the weight as a filter pair, held N frames, interpolated, or
interpolated and a block late (the causal version).

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
ENC = os.path.join(BLD, 'rade_enc_v2_test')

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
DD_ROT = None                                   # set in main(): the receiver's fixed rotation of a latent, see dd_rotation()


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


def decode_raw(z, path):
    """All 21 features of every vector (the last is the aux bit), (nfr, 4, 21)."""
    z.astype(np.float32).tofile(path + '.z')

    with open(path + '.z', 'rb') as fi, open(path + '.f', 'wb') as fo:
        subprocess.run([DEC], stdin=fi, stdout=fo, stderr=subprocess.DEVNULL, check=True)

    f = np.fromfile(path + '.f', np.float32).reshape(-1, 4, 21)
    os.remove(path + '.z')
    os.remove(path + '.f')
    return f


def encode_raw(f, path):
    """What the transmitter would have sent for features f (nfr, 4, 21): (nfr, 2, NC) complex."""
    f.astype(np.float32).tofile(path + '.f')

    with open(path + '.f', 'rb') as fi, open(path + '.e', 'wb') as fo:
        subprocess.run([ENC], stdin=fi, stdout=fo, stderr=subprocess.DEVNULL, check=True)

    e = np.fromfile(path + '.e', np.float32)
    os.remove(path + '.f')
    os.remove(path + '.e')
    nfr = len(e) // 56
    e = e[:nfr * 56].reshape(nfr, 2, NC, 2)
    return e[..., 0] + 1j * e[..., 1]


def reencode(z, path):
    """
    The decoder's own output as symbols: decode the latents z, put all 21 features of each vector
    back through the encoder, and return what the transmitter would have sent for them, (nfr, 2, NC)
    complex. Decision-directed: a reference that needs no pilot, as good as the decode is.
    """
    return encode_raw(decode_raw(z, path), path)


def dd_rotation(work, az):
    """
    The demodulator hands the decoder each carrier turned by a fixed phase (its 16-sample timing
    offset less the 8 the receiver corrects, so a ramp of about 22 degrees a carrier, and a constant):
    what the decoder was trained on, and not what the encoder puts out. A re-encoded symbol has to
    be turned the same way before it is a reference for the received one. Measured once, on the clean
    transmission, through the same decode and encode as the reference.
    """
    a = az * np.sqrt(0.5 * M / np.mean(np.abs(az) ** 2))
    xh = reencode(latents(a), os.path.join(work, 'rot'))
    n = min(a.shape[0], xh.shape[0])
    z = np.sum(a[:n] * np.conj(xh[:n]), axis=0)               # (2, NC)
    return z / np.abs(z)


def vec_loss(feat_in, out, shift):
    """distortion_loss of every output vector against the truth (nan where there is none)."""
    j0 = max(0, -shift)
    n = min(out.shape[0] - j0, feat_in.shape[0] - (j0 + shift))
    l = np.full(out.shape[0], np.nan)

    if n > 0:
        l[j0:j0 + n] = distortion_loss(feat_in[j0 + shift:j0 + shift + n], out[j0:j0 + n])

    return l


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

    if name.startswith('ref'):                  # ref<p>: eq's output, in the phase of the reference channel
        h0, h1 = bx(H0), bx(H1)                 # sum_k |h_k|^(p-1) h_k: one weight per arm, no switch
        pw = float(name[3:]) - 1
        href = np.abs(h0) ** pw * h0 + np.abs(h1) ** pw * h1
        y = (np.conj(h0) * az0 + np.conj(h1) * az1) / np.sqrt(np.abs(h0) ** 2 + np.abs(h1) ** 2 + 1e-12)
        return y * href / (np.abs(href) + 1e-12)

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
    return fir_from_R(r0, r1, weights_R(H0, H1, NC), L, hold, s0)


def fir_from_R(r0, r1, R, L, hold, s0):
    """fir_combine with R (one row per block) given, true or estimated."""
    D = L // 2
    E = np.exp(-1j * np.outer(W, np.arange(L)))
    # taps are charged by distance from the centre, so a long filter stays
    # compact where the target does not need it long (minimum-norm does not:
    # it fits the carriers exactly and is wild between them)
    P = np.diag(1e-3 + ((np.arange(L) - D) / max(D, 1)) ** 2)
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


# ---------------------------------------------------------- blind estimator

def blind_R(az0, az1, S, mode, tau, nvar):
    """
    R = h1/h0 per carrier from the received latents alone, causally: after
    each symbol the per-carrier covariance of (y0, y1) is updated (IIR, time
    constant tau symbols) and R read from it. The transmitted symbol cancels.
      S     'scalar' (pool all carriers), 'perc' (each), 'perc3' (it and its neighbours)
      mode  'u' C01/C00 as it stands; 'k' with the noise power (nvar per carrier)
            taken off C00; 'e' dominant eigenvector of the 2x2 (no noise needed
            if both arms' noise is equal, which it is here)
    """
    y0, y1 = az0.reshape(-1, NC), az1.reshape(-1, NC)
    a = np.exp(-1.0 / tau)
    C00 = np.zeros(NC)
    C11 = np.zeros(NC)
    C01 = np.zeros(NC, complex)
    wsum = 0.0
    R = np.empty((y0.shape[0], NC), complex)
    k3 = np.ones(3)

    for i in range(y0.shape[0]):
        wsum = a * wsum + (1 - a)
        C00 = a * C00 + (1 - a) * np.abs(y0[i]) ** 2
        C11 = a * C11 + (1 - a) * np.abs(y1[i]) ** 2
        C01 = a * C01 + (1 - a) * y1[i] * np.conj(y0[i])
        c00, c11, c01 = C00 / wsum, C11 / wsum, C01 / wsum

        if S == 'scalar':
            n = NC
            c00, c11, c01 = (np.full(NC, x.sum()) for x in (c00, c11, c01))
        elif S == 'perc3':
            n = np.convolve(np.ones(NC), k3, 'same')
            c00, c11, c01 = (np.convolve(x, k3, 'same') for x in (c00, c11, c01))
        else:
            n = 1

        if mode == 'e':
            lam = (c00 + c11) / 2 + np.sqrt(((c00 - c11) / 2) ** 2 + np.abs(c01) ** 2)
            R[i] = (lam - c00) / (np.conj(c01) + 1e-12)
        else:
            den = c00 if mode == 'u' else np.maximum(c00 - n * nvar, 0.1 * c00)
            R[i] = c01 / (den + 1e-12)

    return R.reshape(-1, 2, NC)


def derotate(y, x, tau, centred, nb=1, by_frame=False, w=None):
    """
    y (nfr, 2, NC): a combined stream. x: the symbols it should hold (known, or decoded and re-encoded).
    The channel phase per carrier is the phase of the IIR mean of y conj(x), over tau symbols and the
    carrier and nb neighbours; y goes out with that phase taken off. causal: from earlier symbols only;
    centred: forward and backward, the symbol itself counted once (what a decoder-aided estimate with
    a delay could do). The pilot bound on how far a smooth, estimated reference can get towards eq.
    """
    nfr = y.shape[0]
    c = (y * np.conj(x)).reshape(nfr * 2, NC)

    if w is not None:                           # confidence per symbol: 0 leaves a symbol out of the estimate
        c = c * np.repeat(w, 2)[:c.shape[0], None]

    a = np.exp(-1.0 / tau)
    n = c.shape[0]
    f, b = np.zeros_like(c), np.zeros_like(c)
    acc = np.zeros(NC, complex)

    for i in range(n):
        f[i] = acc
        acc = a * acc + (1 - a) * c[i]

    acc = np.zeros(NC, complex)

    for i in range(n - 1, -1, -1):
        b[i] = acc
        acc = a * acc + (1 - a) * c[i]

    if by_frame and not centred:                # a decoded frame is known only when it has gone by
        f = f[np.arange(n) - (np.arange(n) & 1)]

    est = f + (b + (1 - a) * c if centred else 0)

    if nb:
        k = np.ones(2 * nb + 1)
        est = np.stack([np.convolve(est[i], k, mode='same') for i in range(n)])

    ph = np.where(np.abs(est) > 1e-9, est / (np.abs(est) + 1e-30), 1.0)    # nothing to go on: leave the phase
    return (y.reshape(nfr * 2, NC) * np.conj(ph)).reshape(nfr, 2, NC)


K_GRID = np.arange(-0.8, 0.81, 0.1)           # rad per carrier: a delay of up to about 2 ms
R_GRID = np.arange(-0.5, 0.51, 0.125)          # rad per symbol: a Doppler of up to about 3 Hz


def derotate_model(y, x, model, h):
    """
    The channel phase as a model fitted over a window of 2h+1 symbols, centred, instead of an average:
    phase(t, c) = phi + k c + r (t - t0), and by model
        0   phi                 one phase for all carriers (flat fading)
        1   phi, k              and a slope across the carriers (a delay: two paths)
        2   phi, k, r           and a rate of change in time
    Found by grid search for the k and r that maximise |sum y conj(x) e^{-j(k c + r t)}| over the
    window, phi its angle; y goes out turned by phi + k c at the window's centre. The carriers share
    the fit, so each phase estimate has 14 times the data an IIR over one carrier and its neighbours has.
    """
    nfr = y.shape[0]
    c = (y * np.conj(x)).reshape(nfr * 2, NC)
    n = c.shape[0]
    kk = K_GRID if model >= 1 else np.zeros(1)
    rr = R_GRID if model >= 2 else np.zeros(1)
    t = np.arange(-h, h + 1)
    E = np.exp(-1j * (kk[:, None, None, None] * np.arange(NC)[None, None, None, :]
                      + rr[None, :, None, None] * t[None, None, :, None]))        # (nk, nr, 2h+1, NC)
    E = E.reshape(len(kk) * len(rr), -1)
    pad = np.concatenate([np.zeros((h, NC), complex), c, np.zeros((h, NC), complex)])
    win = np.stack([pad[i:i + 2 * h + 1].reshape(-1) for i in range(n)])           # (n, (2h+1) NC)
    S = win @ E.T                                                                    # (n, nk nr)
    best = np.argmax(np.abs(S), axis=1)
    sb = S[np.arange(n), best]
    kb = kk[best // len(rr)]
    ph = (sb / (np.abs(sb) + 1e-30))[:, None] * np.exp(1j * kb[:, None] * np.arange(NC)[None, :])
    ph = np.where(np.abs(sb)[:, None] > 1e-9, ph, 1.0)
    return (y.reshape(n, NC) * np.conj(ph)).reshape(nfr, 2, NC)


def combine_R(az0, az1, R):
    return (az0 + np.conj(R) * az1) / np.sqrt(1 + np.abs(R) ** 2)



# ------------------------------------------- noise and interference from the CP

def cp_noise(r0, r1, s0, nsym):
    """
    The 2x2 noise covariance of the two arms, per symbol, from the cyclic prefix
    alone: the CP and the tail it copies carry the same signal, so
    x[n] - x[n+M] over the part of the CP clear of the delay spread (samples 16 to
    31, as the demod's own window) holds only noise and interference, and its
    cross-covariance across the arms holds their correlation. Halved (two noisy
    copies) and scaled to the variance per DFT bin: x M, and x Fs/975 because the
    difference is band-limited by the receiver's filter and the noise is taken as
    flat in the band. Per-symbol estimates are 16 samples: they are for smoothing.
    """
    out = np.empty((nsym, 2, 2), complex)
    scale = M * FS / 975.0 / 2

    for i in range(nsym):
        st = s0 + SYM * i
        d = np.stack([r[st + 16:st + NCP] - r[st + 16 + M:st + NCP + M] for r in (r0, r1)])
        out[i] = scale * (d @ d.conj().T) / d.shape[1]

    return out


def mvdr_out(v, R, Rnn):
    """
    u = (1, R) is the signal's direction at arm 0's phase; out = u^H Rnn^-1 v / sqrt(q),
    q = u^H Rnn^-1 u: the matched filter for that direction against that noise, arm 0's
    phase kept, noise power Rnn[0,0]. A diagonal Rnn is the combiner of mode c. v is (2, ...).
    """
    Rinv = np.linalg.inv(Rnn + 1e-3 * np.trace(Rnn).real / 2 * np.eye(2))
    u = np.array([1.0, R])
    f = u.conj() @ Rinv
    q = (f @ u).real
    return np.tensordot(f, v, axes=1) / np.sqrt(q) * np.sqrt(Rnn[0, 0].real)


def blind_mvdr(az0, az1, Ncp, tau, tau_n, full):
    """
    Blind scalar weight with the noise measured from the CP. The signal's R comes
    from the pooled covariance of the latents with the noise covariance taken off
    (full: the whole 2x2, the cross term too, which is what a coherent interferer
    puts into the cross-covariance and would otherwise be read as signal; else the
    diagonal only); the combiner is mvdr_out against the same Rnn.
    """
    y = np.stack([az0.reshape(-1, NC), az1.reshape(-1, NC)], 1)      # (nsym, 2, NC)
    a, an = np.exp(-1.0 / tau), np.exp(-1.0 / tau_n)
    C = np.zeros((2, 2), complex)
    Nn = np.zeros((2, 2), complex)
    w = wn = 0.0
    out = np.empty(y.shape[:1] + (NC,), complex)

    for i in range(y.shape[0]):
        C = a * C + (1 - a) * (y[i] @ y[i].conj().T)
        Nn = an * Nn + (1 - an) * Ncp[i]
        w, wn = a * w + (1 - a), an * wn + (1 - an)
        c, n = C / w, Nn / wn
        n = n if full else np.diag(np.diag(n).real).astype(complex)
        c00 = max((c[0, 0] - NC * n[0, 0]).real, 0.1 * c[0, 0].real)
        R = (c[1, 0] - NC * n[1, 0]) / c00
        out[i] = mvdr_out(y[i], R, n)

    return out.reshape(-1, 2, NC)


def block_mean(x, t0, nfr, hold):
    nb = (nfr + hold - 1) // hold
    return np.array([np.mean(x[t0 + k * hold * 2 * SYM: min(t0 + (k + 1) * hold * 2 * SYM, len(x))])
                     for k in range(nb)])


# ----------------------------------------------------------------- scenario

def energy(az):
    return float(np.mean(np.abs(az) ** 2))


def run_scenario(args):
    work, kind, snr, seed, hold, rungs, t0, nfr, s0, shift, tx, feat_in, clean, dd_rot = args
    rng = np.random.default_rng(1000 * seed + snr + 50)
    S = np.mean(np.abs(tx[np.abs(tx) > 1e-6]) ** 2)
    sigma2 = S * FS / (3000.0 * 10 ** (snr / 10))
    chans = []
    base, _, sir = kind.partition('_q')              # <channel>_q<SIR dB>: a coherent interferer
    gi = [None, None]

    for _ in range(2):
        y, a, b, d = channel2(tx, base, rng)
        rot = 1 if os.environ.get('NOROT') else np.exp(1j * rng.uniform(0, 2 * np.pi))  # a second antenna's path phase
        chans.append((y * rot, a * rot, b * rot, d))

    sq2 = 0.0
    q = np.zeros(len(tx), complex)

    if sir:
        # white Gaussian interference through its own fading channel to each arm; the SIR
        # is in the signal band (975 of the 8000 Hz the white noise covers)
        sq2 = S * 10 ** (-float(sir) / 10) / (975.0 / FS)
        q = noise(len(tx), sq2, rng)
        gi = [doppler_fade(len(tx), 0.5, rng) * np.exp(1j * rng.uniform(0, 2 * np.pi)) for _ in range(2)]

    r = [rx_bpf(c[0] + (gi[k] * q if sir else 0) + noise(len(tx), sigma2, rng))
         for k, c in enumerate(chans)]
    BPF_DELAY = (BPF_NTAP - 1) // 2
    a0 = s0 - BPF_DELAY                                  # tx sample of frame 0 (the tx BPF delay is in tx)
    H = [block_h(c[1], c[2], c[3], a0, nfr, hold) for c in chans]
    g = np.sqrt(AGC_TARGET2 / np.mean(np.abs(r[0][s0:s0 + nfr * 2 * SYM]) ** 2))
    r = [g * x for x in r]
    az0, az1 = demod(r[0], s0, nfr), demod(r[1], s0, nfr)
    ref = energy(az0)
    nb_ = H[0].shape[0]
    Rtrue = weights_R(H[0], H[1], 1)[:, 0]                                  # the signal's R, per block
    Gb = np.stack([block_mean(gi[k], a0, nfr, hold) if sir else np.zeros(nb_) for k in range(2)], 1)
    Rnn_true = g * g * M * (sigma2 * np.eye(2)[None] + sq2 * Gb[:, :, None] * np.conj(Gb)[:, None, :])
    res = {}
    tag = os.path.join(work, '%s_%d_%d' % (kind, snr, seed))
    az_tx = demod(g * rx_bpf(tx), s0, nfr)           # what the clean transmission demodulates to

    for name in rungs:
        if name in ('mvdr_o', 'mvdr_od'):               # true R, true noise + interference covariance
            y = np.stack([az0, az1], 0)
            az = np.empty_like(az0)

            for f in range(nfr):
                k = f // hold
                n = Rnn_true[k] if name == 'mvdr_o' else np.diag(np.diag(Rnn_true[k]).real).astype(complex)
                az[f] = mvdr_out(y[:, f], Rtrue[k], n)
        elif name.startswith('bcp'):                    # bcp<d|f>_<tau>: the blind weight, noise from the CP
            Ncp = cp_noise(r[0], r[1], s0, 2 * nfr)
            f_ = name.split('_')                         # bcp<d|f>_<tau>[_<tau of the noise>]
            tau = float(f_[1])
            az = blind_mvdr(az0, az1, Ncp, tau, float(f_[2]) if len(f_) > 2 else max(2 * tau, 12.0), name[3] == 'f')
        elif name.startswith('pil'):                      # pil[b][c]<tau>: perc's output, channel phase from known symbols
            blind = name.startswith('pilb')
            f_ = name[4 if blind else 3:]
            pm = f_.startswith('m')                       # m<model>w<h>: a phase model over 2h+1 symbols
            centred = f_.startswith('c')
            tau = 0.0 if pm else float(f_[1:] if centred else f_)

            if blind:
                Rb = blind_R(az0, az1, 'perc3', 'k', 6.0, g * g * M * sigma2)
                y = combine_R(az0, az1, Rb)
            else:
                y = combine('perc', az0, az1, H[0], H[1], hold)

            if pm:
                az = derotate_model(y, az_tx, int(f_[1]), int(f_.split('w')[1]))
            else:
                az = derotate(y, az_tx, tau, centred)
        elif name.startswith('dd'):                       # dd[b][<mode><q>_][c]<tau>: pil, with the symbols the decoder
            blind = name.startswith('ddb')                # returned; a mode (g genie, a aux, i idempotence) keeps only
            f_ = name[3 if blind else 2:]                 # the best q % of frames in the estimate
            mode, q = '', 100

            if '_' in f_:
                head, f_ = f_.split('_', 1)
                mode, q = head[0], int(head[1:])

            f_, _, iters = f_.partition('x')              # ...x<n>: derotate and decode n times, each from the last decode
            iters = int(iters) if iters else 1
            pm = f_.startswith('m')
            centred = f_.startswith('c')
            tau = 0.0 if pm else float(f_[1:] if centred else f_)

            if blind:
                Rb = blind_R(az0, az1, 'perc3', 'k', 6.0, g * g * M * sigma2)
                y = combine_R(az0, az1, Rb)
            else:
                y = combine('perc', az0, az1, H[0], H[1], hold)

            y = y * np.sqrt(ref / energy(y))
            ycur = y

            for it in range(iters):
                f1 = decode_raw(latents(ycur), tag + '_' + name + '_p1')
                xh = encode_raw(f1, tag + '_' + name + '_e1')
                n_ = min(xh.shape[0], y.shape[0])
                x2 = np.ones_like(y)
                x2[:n_] = xh[:n_] * dd_rot
                w = None

                if mode:
                    if mode == 'g':                          # the truth: what no receiver knows
                        v = vec_loss(feat_in, f1[:, :, :NUSED].reshape(-1, NUSED), shift).reshape(-1, 4)
                        metric = np.nanmean(np.where(np.isnan(v), 9.0, v), 1)
                    elif mode == 'a':                        # the decoder's own certainty of the aux bit
                        metric = -np.mean(np.abs(f1[:, :, 20]), 1)
                    else:                                    # i: decode what it was re-encoded to; a frame
                        f2 = decode_raw(latents(x2), tag + '_' + name + '_p2')       # that comes back the same is one
                        m2 = min(f2.shape[0], f1.shape[0])                          # the decoder is sure of
                        metric = np.full(f1.shape[0], 9.0)
                        metric[:m2] = np.mean((f1[:m2, :, :NUSED] - f2[:m2, :, :NUSED]) ** 2, (1, 2))

                    thr = np.percentile(metric, q)
                    w = (metric <= thr).astype(float)[:y.shape[0]]

                if pm:
                    ycur = derotate_model(y, x2, int(f_[1]), int(f_.split('w')[1]))
                else:
                    ycur = derotate(y, x2, tau, centred, by_frame=True, w=w)

            az = ycur
        elif name.startswith('bref'):                     # bref<p>_<S>_<mode>_<tau>: the blind R, in the reference phase
            p = name[4:].split('_')
            Rb = blind_R(az0, az1, p[1], p[2], float(p[3]), g * g * M * sigma2)
            pw = float(p[0]) - 1
            rot = 1 + np.abs(Rb) ** pw * Rb                # href / h0, from R alone
            az = combine_R(az0, az1, Rb) * rot / (np.abs(rot) + 1e-12)
        elif name.startswith('b'):                        # b<S>_<mode>_<tau>[_fir<L>]
            p = name[1:].split('_')
            Rb = blind_R(az0, az1, p[0], p[1], float(p[2]), g * g * M * sigma2)

            if len(p) > 3 and p[3][0] in 'hic':         # applied as the engine does: h<N> held for N frames,
                n = int(p[3][1:])                       # i<N> linearly interpolated between every Nth (needs the
                                                        # next estimate), c<N> the same a block late (causal)
                k = np.arange(nfr)
                idx = k // n if p[3][0] == 'h' else None
                late = p[3][0] == 'c'

                if idx is not None:
                    Rb = Rb[idx * n]
                else:
                    kn = np.arange(0, nfr, n)
                    Rb = np.stack([np.stack([np.interp(k, kn, Rb[kn, ss, c].real)
                                             + 1j * np.interp(k, kn, Rb[kn, ss, c].imag)
                                             for c in range(NC)], -1) for ss in range(2)], 1)

                if late:
                    Rb = Rb[np.maximum(k - n, 0)]

                az = combine_R(az0, az1, Rb)
            elif len(p) > 3:                            # the same weights as a filter pair
                L = int(p[3][3:])
                y, s0f = fir_from_R(r[0], r[1], Rb[::hold, 0], L, hold, s0)
                az = demod(y, s0f, nfr)
            else:
                az = combine_R(az0, az1, Rb)
        elif name.startswith('fir'):
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
    global DD_ROT
    DD_ROT = dd_rotation(a.work, demod(rx_bpf(tx), s0, nfr))
    print('decision-directed: the demodulator turns each carrier by %.1f deg at carrier 0 and %.1f deg a carrier'
          % (np.degrees(np.angle(DD_ROT[0, 0])), np.degrees(np.mean(np.angle(DD_ROT[0, 1:] * np.conj(DD_ROT[0, :-1]))))))
    print('gate: clean latents through rade_dec_v2_test: loss %.4f (rade_c publishes 0.080 '
          'for the whole receiver), frame 0 at sample %d, truth shift %d vectors' % (l0, s0, shift))

    if l0 > 0.12:
        sys.exit('gate failed: the offline demod does not reproduce a clean decode; not going on')

    kinds = (a.kinds.split(',') if a.kinds else (['flat', 'mpp'] if a.quick else ['awgn', 'flat', 'mpp']))
    snrs = ([int(s) for s in a.snrs.split(',')] if a.snrs else ([0, 4, 8] if a.quick else [-2, 0, 2, 4, 6, 8, 12]))
    rungs = a.rungs.split(',')
    jobs = [(a.work, k, s, sd, a.hold, rungs, 0, nfr, s0, shift, tx, feat_in, None, DD_ROT)
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

    if not all(n in rungs for n in ANTENNAS):          # nothing to beat without both antennas in the run
        return

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

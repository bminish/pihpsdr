"""
DEVELOPMENT TOOL. Not part of piHPSDR - see docs/tools/radev2-scoring.md.

Calibrates the RADE V2 score against the truth.

score_radev2 scores on-air captures with what the V2 receiver itself
measures - the cyclic-prefix SNR estimate, sync, detector - because on air
there is no reference to compare the decoded speech with. This script
asks whether those numbers can be trusted to rank two combinations of the
antennas the way the decoded speech would.

It takes one known transmission (rade_c's radae_tx --v2 of wav/all.wav),
puts it through synthetic two-antenna channels, decodes each candidate
stream with radev2_iq, and compares the probe's numbers with the feature
distortion loss against the transmitted features - radae's own
distortion_loss(), the yardstick its ctests use.

    python3 radev2_calib.py --work DIR            # everything
    python3 radev2_calib.py --work DIR --quick    # fewer points

Needs third_party/rade_c built and test/diversity/devtools/radev2_iq.
Writes DIR/calib.csv, one row per (scenario, stream), and prints the
summary tables the doc quotes.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
RADE = os.path.join(TOP, 'third_party', 'rade_c')
BLD = os.path.join(RADE, 'build', 'src')
PROBE = os.path.join(TOP, 'test', 'diversity', 'devtools', 'radev2_iq')

FS = 8000
NB_TOTAL = 36           # floats per feature vector in the .f32 files
NUSED = 20
TSTEP = 0.01            # one feature vector per 10 ms
FRAME_VECS = 4          # a decoder output is 4 vectors, 40 ms


# ---------------------------------------------------------------- truth

def distortion_loss(y_true, y_pred):
    """radae/radae_base.py distortion_loss() for 20 features, per vector."""
    ceps = y_pred[..., :18] - y_true[..., :18]
    pitch = 2 * (y_pred[..., 18:19] - y_true[..., 18:19])
    corr = y_pred[..., 19:20] - y_true[..., 19:20]
    pw = np.maximum(y_true[..., 19:20] + 0.5, 0.0) ** 2
    per = np.mean(ceps ** 2 + 3. * (10 / 18) * np.abs(pitch) * pw
                  + (1 / 18) * corr ** 2, axis=-1)
    return per


def load_features(path):
    x = np.fromfile(path, dtype=np.float32)
    return x.reshape(-1, NB_TOTAL)[:, :NUSED]


def frame_losses(feat_in, feat_out, sym):
    """
    Loss of each decoded 40 ms frame against the transmitted features.

    loss.py aligns one contiguous output against the input with a single
    offset. A stream that loses sync and reacquires is not contiguous, and
    one that flips frame parity emits an extra frame, so the output is cut
    into runs of frames 40 ms apart (by the time the probe stamped each
    with) and each run is aligned on its own, as loss.py aligns a whole
    file. The stamps alone are not enough: they move with the receiver's
    timing adjustments, and placing each frame by its stamp read 0.135
    against 0.111 on the same clean decode.

    Checked against rade_c's README: the clean decode of wav/all.wav,
    clipped as its ctest clips it, gives 0.0825 at start 224 against the
    published 0.080 at 224.
    """
    t_valid = sym['t'][sym['valid'] == 1]
    nfr = min(len(t_valid), feat_out.shape[0] // FRAME_VECS)

    if nfr == 0:
        return None, None, None

    out = feat_out[:nfr * FRAME_VECS].reshape(nfr, FRAME_VECS, NUSED)
    t_valid = t_valid[:nfr]
    nin = feat_in.shape[0]
    per = np.full(nfr, np.nan)
    # runs: consecutive outputs 40 ms (+/- a timing step) apart
    gap = np.abs(np.diff(t_valid) - FRAME_VECS * TSTEP) > 0.006
    starts = np.concatenate([[0], np.nonzero(gap)[0] + 1])
    ends = np.concatenate([starts[1:], [nfr]])
    lats = []

    for a, b in zip(starts, ends):
        if b - a < 5:                       # too short to align on its own
            continue

        seg = out[a:b].reshape(-1, NUSED)
        best = None

        # the decoder's latency is 0.2-0.3 s; search it for each run
        for lat in np.arange(0.0, 0.50, TSTEP):
            i0 = int(round((t_valid[a] - lat) / TSTEP))

            if i0 < 0:
                continue

            n = min(seg.shape[0], nin - i0) // FRAME_VECS * FRAME_VECS

            if n < FRAME_VECS * (b - a) // 2:
                continue

            lf = distortion_loss(feat_in[i0:i0 + n], seg[:n]).reshape(-1, FRAME_VECS).mean(axis=1)

            if best is None or lf.mean() < best[0]:
                best = (lf.mean(), lat, lf)

        if best is not None:
            per[a:a + len(best[2])] = best[2]
            lats.append(best[1])

    if not lats:
        return None, None, None

    return per, t_valid, float(np.median(lats))


# -------------------------------------------------------------- channel

def doppler_fade(n, fd, rng, fs=FS):
    """Unit-power complex Rayleigh process with ~fd Hz Doppler spread."""
    step = 100                              # generate at 80 Hz, interpolate
    m = n // step + 64
    g = (rng.standard_normal(m) + 1j * rng.standard_normal(m)) / np.sqrt(2)
    # Gaussian Doppler filter, sigma ~ fd at the coarse rate
    fcoarse = fs / step
    k = np.arange(-32, 33)
    h = np.exp(-0.5 * (k * 2 * np.pi * fd / fcoarse) ** 2)
    g = np.convolve(g, h / np.sqrt(np.sum(h ** 2)), mode='same')
    t = np.arange(n) / step
    re = np.interp(t, np.arange(m), g.real)
    im = np.interp(t, np.arange(m), g.imag)
    z = re + 1j * im
    return z / np.sqrt(np.mean(np.abs(z) ** 2))


def channel(tx, kind, rng):
    """One antenna's view of tx: the channel only, before noise."""
    n = len(tx)

    if kind == 'awgn':
        return tx.copy(), np.ones(n, complex)

    if kind == 'mpp':                       # ITU-ish poor: 2 paths, 2 ms, 1 Hz
        d = int(0.002 * FS)
        a = doppler_fade(n, 1.0, rng)
        b = doppler_fade(n, 1.0, rng)
        y = a * tx
        y[d:] += b[d:] * tx[:-d]
        return y / np.sqrt(2), a / np.sqrt(2)  # a: the first path, for weights

    if kind == 'flat':                      # flat Rayleigh, 0.5 Hz
        a = doppler_fade(n, 0.5, rng)
        return a * tx, a

    raise ValueError(kind)


def noise(n, sigma2, rng):
    return np.sqrt(sigma2 / 2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))


# ---------------------------------------------------------------- probe

def run_probe(iq, work, tag):
    path = os.path.join(work, tag + '.iq')
    feat = os.path.join(work, tag + '.f32')
    csv = os.path.join(work, tag + '.csv')
    iq.astype(np.complex64).tofile(path)
    r = subprocess.run([PROBE, path, '--features', feat, '--csv', csv],
                       capture_output=True, text=True)

    if r.returncode != 0:
        sys.exit(r.stderr)

    summ = dict(kv.split('=') for kv in r.stdout.split())
    sym = np.genfromtxt(csv, delimiter=',', names=True)
    os.remove(path)
    return {k: float(v) for k, v in summ.items()}, sym, load_features(feat)


def score(feat_in, tag, iq, work, nominal_s):
    s, sym, fo = run_probe(iq, work, tag)
    per, tv, lat = frame_losses(feat_in, fo, sym)
    nominal_frames = nominal_s / (FRAME_VECS * TSTEP)
    good = per[np.isfinite(per)] if per is not None else np.array([])
    s['loss'] = float(np.mean(good)) if len(good) else np.nan
    # Coverage: decoded frames that land on the transmission, as a share of
    # the frames there were. Missing speech is the other half of quality.
    s['cover'] = min(1.0, len(good) / nominal_frames)
    # Loss with a dropped frame charged at the loss of silence (all-zero
    # features), so a stream cannot look good by decoding less.
    zl = float(np.mean(distortion_loss(feat_in, np.zeros_like(feat_in))))
    s['loss_all'] = (s['loss'] * s['cover'] + zl * (1 - s['cover'])
                     if len(good) else zl)
    return s, sym, per, tv


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--seeds', type=int, default=3)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    feat_in_f = os.path.join(a.work, 'feat_in.f32')
    tx_f = os.path.join(a.work, 'tx2.iq')

    if not os.path.exists(tx_f):
        subprocess.run([os.path.join(BLD, 'lpcnet_demo'), '-features',
                        os.path.join(RADE, 'wav', 'all.wav'), feat_in_f],
                       check=True, capture_output=True)

        with open(feat_in_f, 'rb') as fi, open(tx_f, 'wb') as fo:
            subprocess.run([os.path.join(BLD, 'radae_tx'), '--v2'], stdin=fi,
                           stdout=fo, check=True, stderr=subprocess.DEVNULL)

    feat_in = load_features(feat_in_f)
    tx = np.fromfile(tx_f, dtype=np.complex64).astype(complex)
    active = np.abs(tx) > 1e-6
    S = np.mean(np.abs(tx[active]) ** 2)
    nominal_s = active.sum() / FS

    snrs = [-3, 0, 3, 6, 9] if a.quick else [-4, -2, 0, 2, 4, 6, 8, 10, 12]
    kinds = ['awgn', 'mpp'] if a.quick else ['awgn', 'flat', 'mpp']
    phases = [0, 60, 120, 180]
    rows = []
    bins = []

    for kind in kinds:
        for snr in snrs:
            for seed in range(a.seeds):
                rng = np.random.default_rng(1000 * seed + snr + 50)
                # SNR in 3 kHz, per antenna, as rade's estimator states it
                sigma2 = S * FS / (3000.0 * 10 ** (snr / 10))
                y0, h0 = channel(tx, kind, rng)
                y1, h1 = channel(tx, kind, rng)
                # arm 1 on a different path phase, as a second antenna is
                rot = np.exp(1j * rng.uniform(0, 2 * np.pi))
                y1, h1 = y1 * rot, h1 * rot
                r0 = y0 + noise(len(tx), sigma2, rng)
                r1 = y1 + noise(len(tx), sigma2, rng)
                # Weights per 0.17 s block, as the radio applies them:
                # ideal MRC from the known channel, then rotated off it.
                blk = int(0.1707 * FS)
                nbk = (len(tx) + blk - 1) // blk
                w = np.empty(len(tx), complex)

                for k in range(nbk):
                    sl = slice(k * blk, (k + 1) * blk)
                    hh0 = np.mean(h0[sl])
                    hh1 = np.mean(h1[sl])
                    w[sl] = np.conj(hh1) * hh0 / (abs(hh0) ** 2 + 1e-12)

                streams = {'arm0': r0, 'arm1': r1}

                for ph in phases:
                    streams['mrc%+d' % ph] = r0 + w * np.exp(1j * np.radians(ph)) * r1

                for name, iq in streams.items():
                    tag = '%s_%d_%d_%s' % (kind, snr, seed, name)
                    s, sym, per, tv = score(feat_in, tag, iq, a.work, nominal_s)
                    s.update(kind=kind, snr=snr, seed=seed, stream=name)
                    rows.append(s)

                    # per-1 s bins: mean probe snr vs mean frame loss
                    if per is not None:
                        for b in range(int(nominal_s)):
                            ms = (sym['t'] >= b) & (sym['t'] < b + 1)
                            mf = (tv >= b) & (tv < b + 1) & np.isfinite(per)

                            if ms.sum() and mf.sum() >= 5:
                                bins.append((kind, snr, name,
                                             float(np.mean(sym['snr'][ms])),
                                             float(np.mean(per[mf]))))

                for f in os.listdir(a.work):
                    if f.startswith('%s_%d_%d_' % (kind, snr, seed)):
                        os.remove(os.path.join(a.work, f))

    keys = ['kind', 'snr', 'seed', 'stream', 'seconds', 'sync', 'sig', 'frames',
            'acq', 'eoo', 'snr_sig', 'snr_sync', 'data_abs', 'data_conf',
            'data_med', 'fsync', 'loss', 'cover', 'loss_all']

    with open(os.path.join(a.work, 'calib.csv'), 'w') as f:
        f.write(','.join(keys) + '\n')

        for r in rows:
            f.write(','.join(str(r[k]) for k in keys) + '\n')

    with open(os.path.join(a.work, 'calib_bins.csv'), 'w') as f:
        f.write('kind,snr,stream,probe_snr,loss\n')

        for b in bins:
            f.write('%s,%d,%s,%.3f,%.4f\n' % b)

    report(rows, bins, kinds, snrs, phases)


def report(rows, bins, kinds, snrs, phases):
    def mean(sel, k):
        v = [r[k] for r in sel if np.isfinite(r[k])]
        return np.mean(v) if v else np.nan

    names = ['arm0', 'arm1'] + ['mrc%+d' % p for p in phases]
    print('\n## Mean over seeds: probe SNR (snr_sync, dB) / true loss_all / cover\n')

    for kind in kinds:
        print('### %s\n' % kind)
        print('| channel SNR | ' + ' | '.join(names) + ' |')
        print('|---' * (len(names) + 1) + '|')

        for snr in snrs:
            cells = []

            for n in names:
                sel = [r for r in rows if r['kind'] == kind and r['snr'] == snr and r['stream'] == n]
                cells.append('%.1f / %.3f / %.2f' % (mean(sel, 'snr_sync'),
                                                     mean(sel, 'loss_all'),
                                                     mean(sel, 'cover')))

            print('| %+d dB | %s |' % (snr, ' | '.join(cells)))

        print()

    # Ranking agreement: within each (kind, snr, seed) the six streams are
    # candidate answers to "which combination is best". How often does the
    # probe pick the same one as the truth, and how often does it order a
    # pair the same way?
    print('## Does the probe rank streams the way the loss does?\n')
    print('| metric | pairs ordered right | best pick right | pick costs (loss) |')
    print('|---|---|---|---|')

    for metric, sign in [('snr_sync', 1), ('snr_sig', 1), ('sync', 1),
                         ('sig', 1), ('data_abs', 1), ('fsync', 1), ('frames', -1)]:
        agree = total = pick_ok = groups = 0
        cost = []

        for kind in kinds:
            for snr in snrs:
                for seed in sorted({r['seed'] for r in rows}):
                    g = [r for r in rows if r['kind'] == kind and r['snr'] == snr and r['seed'] == seed]
                    g = [r for r in g if np.isfinite(r['loss_all'])]

                    if len(g) < 2:
                        continue

                    v = [sign * (r[metric] if np.isfinite(r[metric]) else -1e9) for r in g]
                    t = [-r['loss_all'] for r in g]

                    for i in range(len(g)):
                        for j in range(i + 1, len(g)):
                            if abs(t[i] - t[j]) < 0.02:   # truth can't tell them apart
                                continue

                            total += 1
                            agree += (v[i] - v[j]) * (t[i] - t[j]) > 0

                    groups += 1
                    bi, bt = int(np.argmax(v)), int(np.argmax(t))
                    pick_ok += (bi == bt)
                    cost.append(g[bi]['loss_all'] - g[bt]['loss_all'])

        print('| %s | %.1f%% of %d | %.1f%% of %d | %.3f mean, %.3f worst |' % (
            metric, 100.0 * agree / max(total, 1), total,
            100.0 * pick_ok / max(groups, 1), groups,
            float(np.mean(cost)) if cost else np.nan,
            float(np.max(cost)) if cost else np.nan))

    # The question a diversity score exists to answer: is the combination
    # better than the better antenna alone? Per channel, how often does
    # each metric give the same answer as the loss?
    print('\n## Ideal combination (mrc+0) against the better antenna, per channel\n')
    mets = ['snr_sync', 'sig', 'data_abs', 'fsync']
    print('| channel | truth: combination better | ' + ' | '.join(mets) + ' |')
    print('|---' * (len(mets) + 2) + '|')

    for kind in kinds:
        right = {m: 0 for m in mets}
        better = total = 0

        for snr in snrs:
            for seed in sorted({r['seed'] for r in rows}):
                g = {r['stream']: r for r in rows
                     if r['kind'] == kind and r['snr'] == snr and r['seed'] == seed}

                if 'mrc+0' not in g:
                    continue

                arm = min(g['arm0'], g['arm1'], key=lambda r: r['loss_all'])
                cmb = g['mrc+0']

                if abs(cmb['loss_all'] - arm['loss_all']) < 0.02:
                    continue

                total += 1
                truth = arm['loss_all'] - cmb['loss_all']
                better += truth > 0

                for m in mets:
                    right[m] += (cmb[m] - arm[m]) * truth > 0

        print('| %s | %d of %d | %s |' % (kind, better, total,
              ' | '.join('%d of %d' % (right[m], total) for m in mets)))

    if bins:
        b = np.array([(x[3], x[4]) for x in bins])
        rho = np.corrcoef(b[:, 0], b[:, 1])[0, 1]
        print('\n## Per-second bins: probe SNR against frame loss\n')
        print('%d bins, Pearson r = %.3f' % (len(b), rho))
        edges = [-20, -2, 0, 2, 4, 6, 8, 10, 30]
        print('\n| probe SNR | bins | mean loss | 90th pct loss |')
        print('|---|---|---|---|')

        for lo, hi in zip(edges[:-1], edges[1:]):
            m = (b[:, 0] >= lo) & (b[:, 0] < hi)

            if m.sum():
                print('| %+d..%+d dB | %d | %.3f | %.3f |' % (
                    lo, hi, m.sum(), b[m, 1].mean(), np.percentile(b[m, 1], 90)))


if __name__ == '__main__':
    main()

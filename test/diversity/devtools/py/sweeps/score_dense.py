# Score sweep_dense.py's runs (T-021): Sum SNR against the better arm, and Null depth, for every
# capture, bin width and averaging time, plus a fade-rate figure per capture. Each engine's weights are
# resampled onto the capture's block grid (the latest engine block finished before capture block k is
# applied to k, as score_wideband.py applies one block late). Usage: score_dense.py SCRATCH_DIR
import csv, json, os, sys, concurrent.futures as cf
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from score_wideband import load_blocks, regions, bh4

SP = sys.argv[1]
SUM_TAUS = [0.2, 0.3, 0.5, 0.7, 1, 3, 5, 5.5, 6]
NULL_TAUS = [0.2, 0.5, 1, 3, 6]
RES = [24, 12, 6]
rows = [l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
caps = [(r[0], r[5]) for r in rows if r[5] in ('band', 'carrier', 'digital') and float(r[4]) >= 20 and int(r[1]) <= 192000]

def weights(path, Pc, nb):
    R = list(csv.DictReader(open(path)))
    if len(R) < 2: return None
    t = np.array([float(x['t']) for x in R]); Pe = t[1] - t[0]
    w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in R])
    k = np.arange(nb)
    j = np.searchsorted(t + Pe, k * Pc + 1e-9, side='right') - 1   # latest row finished before block k
    out = np.where(j >= 0, w[np.maximum(j, 0)], 1.0 + 0j)
    return out

def score(c):
    name, ref = c
    h, blks = load_blocks(f'captures/{name}')
    n, rate = h['nfft'], h['rate']; Pc = n / rate; nb = len(blks)
    win = bh4(n).astype(np.float32)
    P0, P1, G0, G1 = [], [], [], []
    for fo, flo, fhi, a0, a1 in blks:
        pb, g = regions(rate, n, fo, flo, fhi)
        f0 = np.fft.fft(a0 * win); f1 = np.fft.fft(a1 * win)
        P0.append(f0[pb]); P1.append(f1[pb]); G0.append(f0[g]); G1.append(f1[g])
    ppow = np.array([np.sum(np.abs(P0[b]) ** 2 + np.abs(P1[b]) ** 2) for b in range(nb)])
    sig = ppow > np.percentile(ppow, 20) * 10 ** 0.6
    # fade rate: correlation of the inter-arm ratio over about a second, on signal blocks
    cb = np.array([np.sum(P1[b] * np.conj(P0[b])) / max(np.sum(np.abs(P0[b]) ** 2), 1e-30) for b in range(nb)])
    L = max(1, int(round(1.0 / Pc)))
    idx = [b for b in range(nb - L) if sig[b] and sig[b + L]]
    rho = (abs(np.sum(cb[idx] * np.conj(cb[[b + L for b in idx]]))) /
           max(np.sqrt(np.sum(abs(cb[idx]) ** 2) * np.sum(abs(cb[[b + L for b in idx]]) ** 2)), 1e-30)) if idx else float('nan')
    arm0 = 10 * np.log10(sum(np.mean(np.abs(P0[b]) ** 2) for b in range(nb)) / sum(np.mean(np.abs(G0[b]) ** 2) for b in range(nb)))
    arm1 = 10 * np.log10(sum(np.mean(np.abs(P1[b]) ** 2) for b in range(nb)) / sum(np.mean(np.abs(G1[b]) ** 2) for b in range(nb)))
    better = max(arm0, arm1)
    p0tot = sum(np.mean(np.abs(P0[b]) ** 2) for b in range(nb))
    res = dict(ref=ref, rate=rate, nfft=n, blocks=nb, rho1s=float(rho), sum={}, null={})
    for mode, taus in (('sum', SUM_TAUS), ('null', NULL_TAUS)):
        for tau in taus:
            for r in RES + ['auto']:
                p = f'{SP}/runs4/{name}.{ref}.{mode}.r{r}.t{tau}.csv'
                if not os.path.exists(p): continue
                w = weights(p, Pc, nb)
                if w is None: continue
                s = sum(np.mean(np.abs(P0[b] + w[b] * P1[b]) ** 2) for b in range(nb))
                if mode == 'sum':
                    g = sum(np.mean(np.abs(G0[b] + w[b] * G1[b]) ** 2) for b in range(nb))
                    val = 10 * np.log10(s / g) - better
                else:
                    val = 10 * np.log10(s / p0tot)
                res[mode].setdefault(str(tau), {})[str(r)] = float(val)
    return name, res

out = {}
with cf.ThreadPoolExecutor(10) as ex:
    for name, res in ex.map(score, caps): out[name] = res
json.dump(out, open(f'{SP}/score4.json', 'w'))
print(len(out), 'captures scored')

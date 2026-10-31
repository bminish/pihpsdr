# T-022 scorer. For each capture and run: depth in the operator's window (the interferer: output power
# against arm 0 alone, dB, more negative is deeper), the change in the rest of the passband (the wanted
# signal: should stay near 0), and the Sum SNR against the better antenna. Weights are resampled onto the
# capture's block grid as in score_dense.py. Usage: score_interf.py SCRATCH_DIR LIST.tsv
import csv, json, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from score_wideband import load_blocks, regions, bh4
from divc import open_divc, BLK, HDR
SP, LIST = sys.argv[1], sys.argv[2]
TAUS = [0.2, 0.5, 1.0, 1.2, 3, 6]; RES = ['24', '12', '6', 'auto']

def weights(path, Pc, nb):
    R = list(csv.DictReader(open(path)))
    if len(R) < 2: return None
    t = np.array([float(x['t']) for x in R]); Pe = t[1] - t[0]
    w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in R])
    j = np.searchsorted(t + Pe, np.arange(nb) * Pc + 1e-9, side='right') - 1
    return np.where(j >= 0, w[np.maximum(j, 0)], 1.0 + 0j)

out = {}
for l in open(LIST):
    name, ref, obj = l.rstrip('\n').split('\t')
    path = f'captures/{name}'
    f, h = open_divc(path); f.seek(HDR)
    cw = []
    for b in range(10 ** 6):
        m = f.read(BLK)
        if len(m) < BLK: break
        cw.append((struct.unpack_from('<d', m, 80)[0], struct.unpack_from('<d', m, 88)[0], struct.unpack_from('<i', m, 64)[0],
                   complex(struct.unpack_from('<d', m, 192)[0], struct.unpack_from('<d', m, 200)[0]),
                   struct.unpack_from('<d', m, 168)[0]))
        f.seek(h['block_bytes'], 1)
    f.close()
    h, blks = load_blocks(path)
    n, rate = h['nfft'], h['rate']; Pc = n / rate; nb = len(blks)
    win = bh4(n).astype(np.float32); fr = np.fft.fftfreq(n, 1.0 / rate)
    W0, W1, R0, R1, P0, P1, G0, G1 = ([] for _ in range(8))
    wrec = np.array([c[3] for c in cw]); cohrec = np.array([c[4] for c in cw])
    for (fo, flo, fhi, a0, a1), (c, wd, fol, _, _) in zip(blks, cw):
        pb, g = regions(rate, n, fo, flo, fhi)
        lo, hi = sorted((-(c + 0.5 * wd + fo), -(c - 0.5 * wd + fo)))
        # Follow RX Filter: the engine's window is the passband, not the hand-placed one
        wm = pb.copy() if fol else ((fr >= lo) & (fr <= hi))
        f0 = np.fft.fft(a0 * win); f1 = np.fft.fft(a1 * win)
        rest = pb & ~wm
        W0.append(f0[wm]); W1.append(f1[wm]); R0.append(f0[rest]); R1.append(f1[rest])
        P0.append(f0[pb]); P1.append(f1[pb]); G0.append(f0[g]); G1.append(f1[g])
    # Steady state: the second half of the capture. run_ref starts at w = 1 and the radio did not, and on an
    # interferer 27 dB hotter on one arm the cold start alone dominates the power (T-022).
    S = nb // 2
    B = range(S, nb)
    def tot(A): return sum(float(np.sum(np.abs(A[b]) ** 2)) for b in B)
    sw0 = tot(W0); sr0 = tot(R0)
    arm0 = 10 * np.log10(sum(np.mean(np.abs(P0[b]) ** 2) for b in B) / max(sum(np.mean(np.abs(G0[b]) ** 2) for b in B), 1e-30))
    arm1 = 10 * np.log10(sum(np.mean(np.abs(P1[b]) ** 2) for b in B) / max(sum(np.mean(np.abs(G1[b]) ** 2) for b in B), 1e-30))
    res = dict(ref=ref, obj=obj, rate=rate, nfft=n, blocks=nb, win_bins=int(np.mean([len(a) for a in W0])),
               follow=int(cw[0][2]), arm0_win_share=sw0 / max(sw0 + sr0, 1e-30), coh_median=float(np.median(cohrec)),
               w_median=float(np.median(np.abs(wrec))), cells={})
    # the weight the radio actually applied while recording (the operator's own run)
    if True:
        sw = sum(float(np.sum(np.abs(W0[b] + wrec[b] * W1[b]) ** 2)) for b in B)
        sr = sum(float(np.sum(np.abs(R0[b] + wrec[b] * R1[b]) ** 2)) for b in B)
        res['recorded'] = dict(win=10 * np.log10(max(sw, 1e-30) / max(sw0, 1e-30)),
                               rest=10 * np.log10(max(sr, 1e-30) / max(sr0, 1e-30)) if sr0 > 0 else 0.0)
    for tau in TAUS:
        for r in RES:
            p = f'{SP}/runs5/{name}.{obj}.r{r}.t{tau}.csv'
            if not os.path.exists(p): continue
            w = weights(p, Pc, nb)
            if w is None: continue
            sw = sum(float(np.sum(np.abs(W0[b] + w[b] * W1[b]) ** 2)) for b in B)
            sr = sum(float(np.sum(np.abs(R0[b] + w[b] * R1[b]) ** 2)) for b in B)
            sp = sum(np.mean(np.abs(P0[b] + w[b] * P1[b]) ** 2) for b in B)
            gg = sum(np.mean(np.abs(G0[b] + w[b] * G1[b]) ** 2) for b in B)
            res['cells'].setdefault(str(tau), {})[r] = dict(
                win=10 * np.log10(max(sw, 1e-30) / max(sw0, 1e-30)),
                rest=10 * np.log10(max(sr, 1e-30) / max(sr0, 1e-30)) if sr0 > 0 else 0.0,
                sum=10 * np.log10(sp / max(gg, 1e-30)) - max(arm0, arm1))
    out[name] = res
json.dump(out, open(f'{SP}/score5.json', 'w'))
print(len(out), 'captures scored')

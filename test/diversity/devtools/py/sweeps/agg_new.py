# Held-out check for T-021: the captures taken after the policy was fixed, one table per capture.
# Usage: agg_new.py SCRATCH_DIR NAME=LABEL ...   (NAME is the capture's HHMMSS-less stem, e.g. 20261008-133410)
import json, sys
import numpy as np
SP = sys.argv[1]
d = json.load(open(f'{SP}/score4.json'))
labels = dict(a.split('=', 1) for a in sys.argv[2:])
def policy(ref, tau):
    return 24 if (tau <= 1.0 and ref in ('band', 'carrier')) else 12
rows = []
for stem, lab in labels.items():
    k = f'divcap-{stem}.divc'
    if k in d: rows.append((stem, lab, d[k]))
for mode, taus in (('sum', [0.2, 0.5, 1, 3, 6]), ('null', [0.2, 0.5, 1, 3, 6])):
    print(f'\n{mode}: gain over fixed 12 Hz, dB (positive is better).  A = the engine\'s Auto; columns 24 and 6 are the fixed widths')
    print('capture         label                       fade  ' + '  '.join(f'{t:>4}s: A    24     6 ' for t in taus))
    for stem, lab, v in rows:
        line = f'{stem:15s} {lab:26s} {v["rho1s"]:4.2f}  '
        for t in taus:
            m = v[mode].get(str(t), {})
            def gg(r):
                if r not in m or '12' not in m: return float('nan')
                return (m[r] - m['12']) if mode == 'sum' else (m['12'] - m[r])
            line += f'{gg("auto"):+5.2f} {gg("24"):+5.2f} {gg("6"):+5.2f}   '
        print(line)
    for grp in sorted({r[1].split(' ')[0] for r in rows}):
        sel = [v for _, lab, v in rows if lab.startswith(grp)]
        line = f'  mean {grp:>8s} (n={len(sel)})            '
        for t in taus:
            g = [((v[mode][str(t)]['auto'] - v[mode][str(t)]['12']) if mode == 'sum' else (v[mode][str(t)]['12'] - v[mode][str(t)]['auto']))
                 for v in sel if str(t) in v[mode] and 'auto' in v[mode][str(t)]]
            line += f'{np.mean(g):+5.2f}              ' if g else '   -                '
        print(line)

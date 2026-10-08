# Aggregate score_dense.py (T-021): Auto against fixed 12 Hz, and each bin width against 12 Hz, per
# averaging time, with bootstrap 95 % intervals; Sum and Null; by reference and by fade rate.
# Criterion (set before the run): Auto no worse than fixed 12 Hz by more than 0.25 dB on average
# (lower bound of the interval above -0.25), and the count of captures worse by more than 0.5 dB.
import json, sys
import numpy as np
SP = sys.argv[1]
d = json.load(open(f'{SP}/score4.json'))
rng = np.random.default_rng(1)

def policy(ref, tau):          # diversity_auto_bin_policy()
    return 24 if (tau <= 1.0 and ref in ('band', 'carrier')) else 12

def ci(x):
    x = np.array(x, float)
    if len(x) < 3: return (float('nan'),) * 2
    m = [np.mean(rng.choice(x, len(x))) for _ in range(4000)]
    return np.percentile(m, 2.5), np.percentile(m, 97.5)

def gain(mode, v12, vx):       # positive = better than 12 Hz
    return vx - v12 if mode == 'sum' else v12 - vx

rhos = [v['rho1s'] for v in d.values() if v['rho1s'] == v['rho1s']]
t1, t2 = np.percentile(rhos, [33, 67])
def stratum(v):
    r = v['rho1s']
    return 'fast' if r < t1 else ('mid' if r < t2 else 'slow')

def table(mode, taus, sel, title):
    L = [v for v in d.values() if sel(v) and mode in v and v[mode]]
    print(f'\n== {title}: {len(L)} captures, {mode}: gain over fixed 12 Hz, dB (95 % interval)')
    print(' tau    24 Hz                    6 Hz                     Auto (policy)               Auto worse >0.5 dB')
    for tau in taus:
        t = str(tau); cols = []
        auto = []
        c24 = []; c6 = []
        for v in L:
            m = v[mode].get(t)
            if not m or '12' not in m: continue
            if '24' in m: c24.append(gain(mode, m['12'], m['24']))
            if '6' in m: c6.append(gain(mode, m['12'], m['6']))
            a = policy(v['ref'], tau)
            if str(a) in m: auto.append(gain(mode, m['12'], m[str(a)]))
        def f(x):
            if not x: return '          -               '
            lo, hi = ci(x); return f'{np.mean(x):+5.2f} [{lo:+5.2f},{hi:+5.2f}] n={len(x):2d}'
        worse = sum(a < -0.5 for a in auto)
        print(f'{tau:>4}  {f(c24)}  {f(c6)}  {f(auto)}   {worse}')
        if auto:
            lo, hi = ci(auto)
            if lo < -0.25: print(f'      ** CRITERION FAILED at tau {tau}: lower bound {lo:+.2f} < -0.25')

SUM_T = [0.2, 0.3, 0.5, 0.7, 1, 3, 5, 5.5, 6]
NULL_T = [0.2, 0.5, 1, 3, 6]
for mode, taus in (('sum', SUM_T), ('null', NULL_T)):
    table(mode, taus, lambda v: True, 'all')
    for ref in ('band', 'carrier'):
        table(mode, taus, lambda v, r=ref: v['ref'] == r, ref)
    for s in ('fast', 'mid', 'slow'):
        table(mode, taus, lambda v, s=s: stratum(v) == s, f'fade {s} (rho at 1 s: <{t1:.2f} / <{t2:.2f} / more)')

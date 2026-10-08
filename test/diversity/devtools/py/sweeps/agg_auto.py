# Validate the engine's own Auto (run_ref --resolution auto) against fixed 12 Hz, and against the fixed-width
# cell its policy should equal. Usage: agg_auto.py SCRATCH_DIR  (after score_dense.py has seen the 'auto' runs)
import json, sys
import numpy as np
SP = sys.argv[1]
d = json.load(open(f'{SP}/score4.json'))
rng = np.random.default_rng(3)
def policy(ref, tau):          # diversity_auto_bin_policy()
    return 24 if (tau <= 1.0 and ref in ('band', 'carrier')) else 12
def ci(x):
    x = np.array(x); m = [rng.choice(x, len(x)).mean() for _ in range(4000)]; return np.percentile(m, [2.5, 97.5])
for mode, taus in (('sum', [0.2, 0.3, 0.5, 0.7, 1, 3, 5, 5.5, 6]), ('null', [0.2, 0.5, 1, 3, 6])):
    print(f'\n{mode}: the engine\'s Auto against fixed 12 Hz (gain, dB; positive is better), and against the cell the policy names')
    print(' tau   gain over 12 Hz [95 % interval]    worst capture   worse >0.25   |Auto - policy cell|: mean / max')
    for t in taus:
        g, w, dev = [], [], []
        for v in d.values():
            m = v[mode].get(str(t))
            if not m or 'auto' not in m or '12' not in m: continue
            sign = 1 if mode == 'sum' else -1
            g.append(sign * (m['auto'] - m['12']))
            dev.append(abs(m['auto'] - m[str(policy(v['ref'], t))]))
        g = np.array(g); lo, hi = ci(g)
        print(f'{t:>4}   {g.mean():+5.2f} [{lo:+5.2f},{hi:+5.2f}] n={len(g)}   {g.min():+6.2f}        {int((g < -0.25).sum()):3d}'
              f'          {np.mean(dev):.3f} / {np.max(dev):.2f}')

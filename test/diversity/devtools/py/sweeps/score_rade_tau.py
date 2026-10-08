# Score sweep_rade_tau.py's weight series on decode with score_rade (at most 5 --weights per run).
import subprocess, re, os, json, sys, statistics as st, concurrent.futures as cf
SP = sys.argv[1]
D = os.path.expanduser('~/sdr/freedv-gui/build_linux/_deps/freedv_backend-build')
TAUS = [0.2, 0.5, 1, 2, 4, 6, 10]; GROUPS = [[0.2, 0.5, 1, 2, 4], [6, 10]]
caps = ['20260902-165826', '20260903-190516', '20260903-190715', '20260903-190932', '20260830-202743',
        '20260903-190822', '20260903-193105', '20260902-234508', '20260902-234624', '20260930-180235',
        '20260930-180949', '20260930-180822', '20260930-175445']
def score(c):
    res = {}
    for g in GROUPS:
        args = ['test/diversity/devtools/score_rade', f'captures/divcap-{c}.divc']
        for t in g: args += ['--weights', f't{t}={SP}/runs3/{c}.{t}.csv']
        p = subprocess.run(args, capture_output=True, text=True,
                           env=dict(os.environ, LD_LIBRARY_PATH=D + '/rade_build/src'))
        for m in re.finditer(r'^\s+t([\d.]+)\s+([+-]\d+) synced', p.stdout, re.M): res[float(m.group(1))] = int(m.group(2))
    return c, res
out = {}
with cf.ThreadPoolExecutor(6) as ex:
    for c, r in ex.map(score, caps): out[c] = r
json.dump(out, open(SP + '/score3.json', 'w'))
print('capture            ' + '  '.join(f'{t:>5}' for t in TAUS))
for c, r in out.items(): print(f'{c:18} ' + '  '.join(f'{r.get(t, "-"):>5}' for t in TAUS))
ok = [r for r in out.values() if all(t in r for t in TAUS)]
print('mean frames vs 6 s: ' + '  '.join(f'{st.mean(r[t] - r[6] for r in ok):+5.1f}' for t in TAUS))

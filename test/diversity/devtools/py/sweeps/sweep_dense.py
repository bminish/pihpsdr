# Dense sweep for Auto's thresholds (T-021): bin width x averaging, Sum (dense taus) and Null (sparse taus),
# on every wideband capture of 20 s or more at 192 kHz or below. Usage: sweep_dense.py SCRATCH_DIR [auto]
import subprocess, sys, os, concurrent.futures as cf
SP = sys.argv[1]
SUM_TAUS = [0.2, 0.3, 0.5, 0.7, 1, 3, 5, 5.5, 6]
NULL_TAUS = [0.2, 0.5, 1, 3, 6]
RES = [24, 12, 6]
if len(sys.argv) > 2 and sys.argv[2] == 'auto': RES = ['auto']   # the engine's own policy, as the radio runs it
COH = {'band': 0.20, 'carrier': 0.30, 'digital': 0.30}
rows = [l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
os.makedirs(f'{SP}/runs4', exist_ok=True)
jobs = []
for r in rows:
    name, rate, nfft, nb, dur, ref = r[:6]
    if ref not in COH or float(dur) < 20 or int(rate) > 192000: continue
    for mode, taus in (('sum', SUM_TAUS), ('null', NULL_TAUS)):
        for res in RES:
            for tau in taus: jobs.append((name, ref, mode, res, tau))
def run(j):
    name, ref, mode, res, tau = j
    out = f'{SP}/runs4/{name}.{ref}.{mode}.r{res}.t{tau}.csv'
    if not os.path.exists(out):
        subprocess.run(['test/diversity/devtools/run_ref', f'captures/{name}', '--ref', ref, '--mode', mode,
                        '--weighting', 'flat', '--cohmin', str(COH[ref]), '--follow', '1', '--tau', str(tau),
                        '--resolution', str(res), '--out', out],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    return j
with cf.ThreadPoolExecutor(18) as ex:
    for i, j in enumerate(ex.map(run, jobs)):
        if i % 100 == 0: print(i, len(jobs), flush=True)
print('done', len(jobs))

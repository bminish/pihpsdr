# T-022: captures with an interferer, run with the operator's own window and Follow setting (not forced),
# in the objective the operator used. Fixed bin widths and the engine's Auto. Usage: sweep_interf.py SCRATCH_DIR LIST.tsv
# LIST.tsv: capture<TAB>ref<TAB>objective(null|sum)
import subprocess, sys, os, concurrent.futures as cf
SP, LIST = sys.argv[1], sys.argv[2]
TAUS = [0.2, 0.5, 1.0, 1.2, 3, 6]
RES = ['24', '12', '6', 'auto']
COH = {'band': 0.20, 'carrier': 0.30, 'digital': 0.30}
os.makedirs(f'{SP}/runs5', exist_ok=True)
jobs = []
for l in open(LIST):
    name, ref, obj = l.rstrip('\n').split('\t')
    for res in RES:
        for tau in TAUS: jobs.append((name, ref, obj, res, tau))
def run(j):
    name, ref, obj, res, tau = j
    out = f'{SP}/runs5/{name}.{obj}.r{res}.t{tau}.csv'
    if not os.path.exists(out):
        subprocess.run(['test/diversity/devtools/run_ref', f'captures/{name}', '--ref', ref, '--mode', obj,
                        '--weighting', 'flat', '--cohmin', str(COH[ref]), '--tau', str(tau),
                        '--resolution', res, '--out', out],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1200)
with cf.ThreadPoolExecutor(14) as ex: list(ex.map(run, jobs))
print('done', len(jobs))

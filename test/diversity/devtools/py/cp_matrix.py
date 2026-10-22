#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Stage 0 of a combined RADE V1/V2 correlator: which detector locks on what.

For every capture it records
  truth   V1: rade_c's V1 receiver syncs (score_rade, sync >= 20% on either arm)
          V2: rade_c's V2 receiver syncs (score_radev2: sync >= 20% and frame-sync >= 0.6)
  locks   V1 pilot   the shipping rade_correlator.c (replay_rade), bank from the passband
          V1 prefix  the cyclic prefix at the V1 lag (160, period 192), cp_probe.py, both banks
          V2 prefix  rade_v2_correlator.c (cp_replay), bank from the passband and both banks
and prints the matrix of truth class against detector: the number that matters for a combined
correlator is the off-diagonal (a lock on the other waveform) and the locks on neither.

  cp_matrix.py [--cache DIR] [--jobs N]

score_rade needs librade on LD_LIBRARY_PATH (third_party/rade_c-build/src); the per-capture
results are cached as JSON and reused. Run with `python3 -I`.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cp_probe as cp   # noqa: E402

DEV = os.path.normpath(os.path.join(HERE, '..'))
TOP = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
CAP = os.path.join(TOP, 'captures')
LIB = os.path.join(TOP, 'third_party', 'rade_c-build', 'src')


def run(cmd, env=None):
    e = dict(os.environ)
    e['LD_LIBRARY_PATH'] = LIB
    return subprocess.run(cmd, capture_output=True, text=True, env=e).stdout


def sync_of(out, names=('arm0', 'arm1')):
    best = 0.0
    for l in out.splitlines():
        c = l.split()
        if c and c[0] in names:
            try:
                best = max(best, float(c[3 if len(c) > 5 and c[3].endswith('%') else 2].rstrip('%')))
            except (ValueError, IndexError):
                pass
    return best


def v2_truth(path, cache):
    f = os.path.join(cache, os.path.basename(path) + '.txt')
    if not os.path.exists(f):
        open(f, 'w').write(''.join(l for l in run([os.path.join(DEV, 'score_radev2'), path]).splitlines(True)
                                   if l.startswith('arm')))
    best = (0.0, 0.0)
    for l in open(f):
        c = l.split()
        try:
            sync, fs = float(c[2].rstrip('%')), float(c[9])
        except (IndexError, ValueError):
            continue
        if sync * fs > best[0] * best[1]:
            best = (sync, fs)
    return best[0] >= 20 and best[1] >= 0.6


def first_lock_cp(exe, path, bank):
    args = [exe, path] + ([str(bank)] if bank is not None else [])
    for l in run(args).splitlines():
        if 'LOCKED' in l:
            return int(l.split()[0])
    return None


def v1_prefix(path):
    a0, a1, rate, _ = cp.load(path, 60)
    x = [cp.to_8k(a, rate) for a in (a0, a1)]
    for bank in (0, 1):
        y = [cp.band(np.conj(v) if bank else v, cp.V1['lo'], cp.V1['hi']) for v in x]
        r = cp.windows(y[0], y[1], cp.V1)
        lk = cp.lock(r, P=cp.V1['P'])
        if lk.any():
            return int(np.argmax(lk)) + cp.LOCK_N     # about the second of the first lock
    return None


def one(args):
    path, cache = args
    n = os.path.basename(path)[7:-5]
    d = {'name': n}
    d['v1_truth'] = sync_of(run([os.path.join(DEV, 'score_rade'), path])) >= 20.0
    d['v2_truth'] = v2_truth(path, cache)
    out = run([os.path.join(DEV, 'replay_rade'), path, '--csv', '/dev/stdout']).splitlines()
    row = [l for l in out if re.match(r'^\d+,[\d.]+,\d+,', l)]
    fl = None
    if row:
        c = row[-1].split(',')
        try:
            fl = float(c[4]) if float(c[3]) > 0 else None
        except ValueError:
            fl = None
    d['v1_pilot'] = fl
    d['v1_prefix'] = v1_prefix(path)
    d['v2_rec'] = first_lock_cp(os.path.join(DEV, 'cp_replay'), path, None)
    d['v2_both'] = first_lock_cp(os.path.join(DEV, 'cp_replay'), path, -1)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', default=os.path.join(DEV, 'build', 'cp_truth'))
    ap.add_argument('--jobs', type=int, default=6)
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    res = os.path.join(a.cache, 'matrix.json')
    if os.path.exists(res):
        rows = json.load(open(res))
    else:
        caps = sorted(glob.glob(os.path.join(CAP, 'divcap-*.divc')))
        with ProcessPoolExecutor(a.jobs) as ex:
            rows = list(ex.map(one, [(p, a.cache) for p in caps]))
        json.dump(rows, open(res, 'w'), indent=0)
    cls = {'V1 (rade_c V1 syncs)': [r for r in rows if r['v1_truth'] and not r['v2_truth']],
           'V2 (rade_c V2 syncs)': [r for r in rows if r['v2_truth'] and not r['v1_truth']],
           'both': [r for r in rows if r['v1_truth'] and r['v2_truth']],
           'neither': [r for r in rows if not r['v1_truth'] and not r['v2_truth']]}
    dets = [('v1_pilot', 'V1 pilot'), ('v1_prefix', 'V1 prefix'), ('v2_rec', 'V2 prefix (passband bank)'),
            ('v2_both', 'V2 prefix (both banks)')]
    print(f'{len(rows)} captures. Rows: what is in the capture by rade_c; columns: how many the detector locks on.\n')
    print(f'{"":24s}' + ''.join(f'{t:>27s}' for _, t in dets))
    for k, v in cls.items():
        print(f'{k + f" n={len(v)}":24s}' + ''.join(f'{sum(r[d] is not None for r in v):>27d}' for d, _ in dets))
    for k, v in cls.items():
        for d, t in dets:
            hit = sorted(r['name'] for r in v if r[d] is not None)
            miss = sorted(r['name'] for r in v if r[d] is None)
            if k.startswith('neither') and hit:
                print(f'\nlocked on neither ({t}): {hit}')
            if k.startswith('V1') and d.startswith('v2') and hit:
                print(f'\nV2 detector locked on V1 captures ({t}): {hit}')
            if k.startswith('V2') and d.startswith('v1') and hit:
                print(f'\nV1 detector locked on V2 captures ({t}): {hit}')
            if (k.startswith('V1') and d.startswith('v1') or k.startswith('V2') and d.startswith('v2')) and miss:
                print(f'\nmissed ({t}) in {k}: {miss}')
    for d, t in dets:
        for k in ('V1 (rade_c V1 syncs)', 'V2 (rade_c V2 syncs)'):
            ts = sorted(r[d] for r in cls[k] if r[d] is not None)
            if ts:
                print(f'\nfirst lock, {t} on {k}: {ts}')


if __name__ == '__main__':
    main()

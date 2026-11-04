#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Scores the C cyclic-prefix tracker (../cp_replay) against rade_c's own V2 receiver
over every capture: which captures are V2 (score_radev2: sync >= 20% and frame-sync >= 0.6), and
whether and when cp_replay locks on each.

  cp_sweep.py [--cache DIR] [--cp-replay EXE] [--tau SECONDS]

score_radev2 takes minutes over a full set, so its verdicts are kept in --cache and reused; delete
the directory to redo them. To sweep the lock parameters build cp_replay with -DV2_T_LOCK=...
-DV2_LOCK_K=... -DV2_LOCK_N=... -DV2_LOCK_TOL=... and pass it as --cp-replay. Run with `python3 -I`.
"""
import argparse
import glob
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.normpath(os.path.join(HERE, '..'))
CAP = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..', 'captures'))


def verdict(path, cache):
    f = os.path.join(cache, os.path.basename(path) + '.txt')
    if not os.path.exists(f):
        out = subprocess.run([os.path.join(DEV, 'score_radev2'), path], capture_output=True, text=True).stdout
        open(f, 'w').write(''.join(l for l in out.splitlines(True) if l.startswith('arm')))
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


def first_lock(exe, path, tau):
    for l in subprocess.run([exe, path, '-1', str(tau)], capture_output=True, text=True).stdout.splitlines():
        if 'LOCKED' in l:
            return int(l.split()[0])
    return None


ap = argparse.ArgumentParser()
ap.add_argument('--cache', default=os.path.join(DEV, 'build', 'cp_truth'))
ap.add_argument('--cp-replay', default=os.path.join(DEV, 'cp_replay'))
ap.add_argument('--tau', type=float, default=1.0)
a = ap.parse_args()
os.makedirs(a.cache, exist_ok=True)
caps = sorted(glob.glob(os.path.join(CAP, 'divcap-*.divc')))
with ThreadPoolExecutor(8) as ex:
    truth = list(ex.map(lambda p: verdict(p, a.cache), caps))
    lock = list(ex.map(lambda p: first_lock(a.cp_replay, p, a.tau), caps))
name = [os.path.basename(p)[7:-5] for p in caps]
found = [(n, t) for n, t, v, k in zip(name, lock, truth, lock) if v and t is not None]
nv = sum(truth)
print(f'{len(caps)} captures, {nv} V2 by rade_c; found {len(found)}; first lock (s): {sorted(t for _, t in found)}')
print('missed:       ', [n for n, v, k in zip(name, truth, lock) if v and k is None])
print('locked on non-V2:', [n for n, v, k in zip(name, truth, lock) if not v and k is not None])

#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Holds the C port (src/rade_v2_correlator.c, via ../cp_replay) to its reference
(cp_probe.py) on recorded captures:

  - locks on a V2 capture within 6 s, on the right bank, and never on SSB or a V1 capture
  - the same through the whole engine (run_ref --ref rade2, the shipping diversity_auto.c): locks
    on T-008 by 6 s and stays locked, and never locks on SSB
  - the capture's self-check: a copy of T-008 made to look as if taken on the V2 reference, with
    the replay's own state written into each block, replays with no block differing from it
  - once locked, the weight's phase agrees with the reference's inter-arm phase to within 30
    degrees at the median. The reference works on the conjugated arms for bank 1, where the true
    weight conj(h1/h0) is the reference's own h1/h0 phase; on bank 0 it would be the negative

Needs captures/ in the tree root and `make cp_replay` done. Run with `python3 -I`.
"""
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cp_probe as cp   # noqa: E402

TOP = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
REPLAY = os.path.join(HERE, '..', 'cp_replay')
LINE = re.compile(r'\s*(\d+) s\s+dpk ([\d.]+)\s+(LOCKED|search)(\+?)\s+gamma\s+(\d+)\s+bank (\d).*?'
                  r'w ([\d.]+) /\s*([+-]\d+) deg')


def replay(name, bank=-1, tau=0.5):
    out = subprocess.run([REPLAY, os.path.join(TOP, 'captures', name), str(bank), str(tau)],
                         capture_output=True, text=True, check=True).stdout
    return [m.groups() for m in map(LINE.match, out.splitlines()) if m]


def first_lock(rows):
    for r in rows:
        if r[2] == 'LOCKED':
            return int(r[0]), int(r[5])
    return None, None


fails = 0


def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what)
    fails += not ok


t, b = first_lock(replay('divcap-20260930-184835.divc'))
check(t is not None and t <= 6 and b == 1, f'T-008 locks by 6 s on bank 1 (t={t}, bank={b})')
t, b = first_lock(replay('divcap-20261008-133410.divc'))
check(t is None, 'SSB capture never locks')
t, b = first_lock(replay('divcap-20260914-153751.divc'))
check(t is None, 'V1 capture never locks')
t, b = first_lock(replay('divcap-20260930-192558.divc', bank=0))
print(f'     T-009 (weak, under SSB QRM) locks at {t} s, bank {b}   [informational]')

RUN_REF = os.path.join(HERE, '..', 'run_ref')


def engine(name):
    """locked flag per block from the whole engine, and the block length in seconds."""
    out = os.path.join(HERE, '..', 'build', 'test_cp_engine.csv')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([RUN_REF, os.path.join(TOP, 'captures', name), '--ref', 'rade2', '--out', out],
                   capture_output=True, check=True)
    rows = [l.split(',') for l in open(out).read().splitlines()[1:]]
    return [float(r[1]) for r in rows], [int(r[2]) for r in rows]


t, lk = engine('divcap-20260930-184835.divc')
first = t[lk.index(1)] if 1 in lk else None
check(first is not None and first <= 6.5 and sum(lk[lk.index(1):]) > 0.9 * len(lk[lk.index(1):]),
      f'engine: T-008 locks by 6.5 s and stays locked (first lock {first} s)')
t, lk = engine('divcap-20261008-133410.divc')
check(sum(lk) == 0, 'engine: SSB capture never locks')

def self_check():
    """Write the replay's state entering each block into a copy of T-008 (ref 5), replay that."""
    import struct
    src = os.path.join(TOP, 'captures', 'divcap-20260930-184835.divc')
    tmp = os.path.join(HERE, '..', 'build', 'test_cp_selfcheck.divc')
    csvf = os.path.join(HERE, '..', 'build', 'test_cp_selfcheck.csv')
    subprocess.run([REPLAY, src, '1', '1.0', csvf], capture_output=True, check=True)
    state = [(0, 0, 0.0)] + [(int(l.split(',')[1]), int(l.split(',')[6]), float(l.split(',')[3]))
                              for l in open(csvf).read().splitlines()[1:]]
    data = bytearray(open(src, 'rb').read())
    nfft, bb = struct.unpack_from('<II', data, 16)[0], struct.unpack_from('<I', data, 20)[0]
    struct.pack_into('<I', data, 8, 5)            # the file format version that numbers reference 5
    pos, i = 280, 0
    while pos + 208 <= len(data) and struct.unpack_from('<I', data, pos)[0] == 0x214B4C42:
        lk, mi, q = state[min(i, len(state) - 1)]
        if struct.unpack_from('<I', data, pos + 12)[0] & 2 or struct.unpack_from('<I', data, pos + 8)[0]:
            lk, mi, q = 0, 0, 0.0         # the engine records its state after the reset it did before the block
        struct.pack_into('<i', data, pos + 60, 5)
        struct.pack_into('<i', data, pos + 96, 1)
        struct.pack_into('<d', data, pos + 112, 1.0)
        struct.pack_into('<i', data, pos + 128, lk)
        struct.pack_into('<i', data, pos + 136, mi)
        struct.pack_into('<d', data, pos + 144, q)
        pos += 208 + bb
        i += 1
    open(tmp, 'wb').write(data)
    return subprocess.run([REPLAY, tmp], capture_output=True, text=True).stderr


m = re.search(r'(\d+) of (\d+) blocks differ', self_check())
check(m is not None and int(m.group(1)) == 0 and int(m.group(2)) > 300,
      f'self-check against recorded state: {m.group(1) if m else "?"} of {m.group(2) if m else "?"} blocks differ')

rows = replay('divcap-20260930-184835.divc', bank=1, tau=0.5)
y0, y1, _ = cp.arms(os.path.join(TOP, 'captures', 'divcap-20260930-184835.divc'), 1, 40)
L, W, P = cp.V2['L'], cp.V2['W'], cp.V2['P']
A = cp.fold(y0, y0, L, W, P)
B = cp.fold_energy(y0, L, W, P)
gates = [cp.detect(A[i:i + 50].sum(0), B[i:i + 50].sum(0), P, W)[0] for i in range(0, A.shape[0] - 49, 50)]
ref = cp.cross_est(y0, y1, gates)[:, 0]
d = [abs(cp.wrap(np.radians(float(r[7])) - ref[int(r[0]) - 1])) for r in rows
     if r[2] == 'LOCKED' and r[3] == '+' and 0 < int(r[0]) <= len(ref)]
med = np.degrees(np.median(d))
check(len(d) > 20 and med < 30, f'weight phase against reference: median {med:.0f} deg over {len(d)} seconds')
sys.exit(1 if fails else 0)

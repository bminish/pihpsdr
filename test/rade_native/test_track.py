#!/usr/bin/env python3
"""Tests of the RIT tracker (src/rade_native_track.c) in a closed loop: the station is at a fixed offset, the modem reports what the RIT leaves.
Run with a run directory of rade-logs/ as argument to replay its overs' offsets through the tracker as well."""
import ctypes, os, subprocess, sys, tempfile, csv
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src")
d = tempfile.mkdtemp()
so = os.path.join(d, "track.so")
subprocess.check_call(["gcc", "-shared", "-fPIC", "-O1", "-I", SRC, "-o", so, os.path.join(SRC, "rade_native_track.c"), "-lm"])
lib = ctypes.CDLL(so)

class Track(ctypes.Structure):
    _fields_ = [("locked", ctypes.c_int), ("seen", ctypes.c_int), ("n", ctypes.c_int), ("mean", ctypes.c_double), ("cmd", ctypes.c_double),
                ("age", ctypes.c_double), ("last_t", ctypes.c_double), ("last_sync_t", ctypes.c_double)]
lib.rade_track_update.argtypes = [ctypes.POINTER(Track), ctypes.c_double, ctypes.c_int, ctypes.c_double, ctypes.c_int, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong)]
lib.rade_track_reset.argtypes = [ctypes.POINTER(Track)]

fails = []
def check(name, ok, info=""):
    print("  %-4s %s %s" % ("ok" if ok else "FAIL", name, info))
    if not ok: fails.append(name)

class Loop:
    """sign +1: the offset the modem reports is station - rit; sign -1: station + rit."""
    def __init__(self, sign):
        self.t = Track(); lib.rade_track_reset(ctypes.byref(self.t))
        self.rit = 0; self.now = 100.0; self.sign = sign; self.hist = []
    def run(self, secs, station, sync=True, noise=None):
        for _ in range(int(secs * 4)):
            self.now += 0.25
            # the modem's offset: with sign +1 the RIT up lowers it, with sign -1 the RIT up raises it
            foff = station - self.sign * self.rit
            if noise: foff += noise()
            new = ctypes.c_longlong()
            if lib.rade_track_update(ctypes.byref(self.t), self.now, 1 if sync else 0, foff if sync else 0.0, self.sign, self.rit, ctypes.byref(new)):
                self.rit = new.value
            self.hist.append((self.now, self.rit))

import random
random.seed(1)
for sign in (1, -1):
    L = Loop(sign)
    L.run(60, 8.0, noise=lambda: random.gauss(0, 1.5))
    # the RIT ends where it cancels the offset, with the sideband's sign
    check("sign %+d: settles on the station (rit %+d)" % (sign, sign * 8), abs(L.rit - sign * 8) <= 1, "rit %d" % L.rit)
    steps = [abs(b[1] - a[1]) for a, b in zip(L.hist, L.hist[1:])]
    check("sign %+d: never more than 1 Hz per poll (3 Hz/s)" % sign, max(steps) <= 1)
    first = [h for h in L.hist if h[0] - 100 <= 5]
    check("sign %+d: moves at most 3 Hz/s early (%d Hz in 5 s)" % (sign, abs(first[-1][1])), abs(first[-1][1]) <= 15 + 1)
    late = [h[1] for h in L.hist if h[0] >= 140]
    check("sign %+d: late on, steadier than 1 Hz per 5 s" % sign, max(late) - min(late) <= 3, "spread %d" % (max(late) - min(late)))

L = Loop(1)
L.run(30, 10.0)
rit_hold = L.rit
L.run(1.0, 10.0, sync=False)
check("short loss: RIT held", L.rit == rit_hold)
L.run(10, 10.0)
check("regained lock: average restarted", L.t.n <= 41)
L.run(2.0, 10.0, sync=False)
check("end of over: RIT back to zero", L.rit == 0 and L.t.seen == 0, "rit %d" % L.rit)

L = Loop(1)
L.run(300, 500.0)
check("limited to +-50 Hz", abs(L.rit) <= 50, "rit %d" % L.rit)

if len(sys.argv) > 1:
    run = sys.argv[1]
    sign = 1
    rows = list(csv.DictReader(open(os.path.join(run, "rx.csv"))))
    # rx.csv holds only in-sync rows, 2 per second; a gap between rows is no lock
    L = Loop(sign); prev = None; t0 = float(rows[0]["t"]); out = []
    L.now = t0
    for r in rows:
        t = float(r["t"])
        if prev is not None and t - prev > 1.5:
            L.now = prev + 0.25
            for _ in range(8):
                L.now += 0.25
                new = ctypes.c_longlong()
                if lib.rade_track_update(ctypes.byref(L.t), L.now, 0, 0.0, 1, L.rit, ctypes.byref(new)): L.rit = new.value
        L.now = t
        new = ctypes.c_longlong()
        f = float(r["foff_mean"])
        # the log was made with the RIT at zero: the offset seen is the station's; the replay applies the tracker's correction
        if lib.rade_track_update(ctypes.byref(L.t), t, 1, f - L.rit * sign, 1, L.rit, ctypes.byref(new)):
            L.rit = new.value
        out.append((t, f, L.rit)); prev = t
    import statistics
    resid = [f - rit for _t, f, rit in out]
    print("replay of %s: %d rows, offset sd %.2f Hz without tracking, %.2f Hz with; RIT range %d..%d" % (
        run, len(out), statistics.pstdev([f for _t, f, _r in out]), statistics.pstdev(resid), min(r for *_x, r in out), max(r for *_x, r in out)))
print("\nFAILED: %s" % fails if fails else "\nall passed")
sys.exit(1 if fails else 0)

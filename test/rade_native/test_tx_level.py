#!/usr/bin/env python3
"""Checks the mic level meter, trim, leveller and limiter (test_tx_level) against an independent
BS.1770 loudness calculation in Python, using speech clips from rade_c."""
import os, re, subprocess, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter, resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "out")
TOOL = os.path.join(HERE, "test_tx_level")
b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]; a1 = [1, -1.69065929318241, 0.73248077421585]
b2 = [1, -2, 1]; a2 = [1, -1.99004745483398, 0.99007225036621]
fails = 0


def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what))
    fails += (not c)


def lufs(x48):
    y = lfilter(b2, a2, lfilter(b1, a1, x48))
    ms = np.array([np.mean(y[i:i + 19200] ** 2) for i in range(0, len(y) - 19200 + 1, 4800)])
    L = -0.691 + 10 * np.log10(ms + 1e-20)
    g = ms[L > -70]
    rel = -0.691 + 10 * np.log10(g.mean()) - 10
    k = ms[(L > -70) & (L > rel)]
    return -0.691 + 10 * np.log10(k.mean())


def run(path, trim=0.0, agc=0, repeat=1, extra=(), raw=False):
    r = subprocess.run([TOOL, "--trim", str(trim), "--agc", str(agc), "--repeat", str(repeat)] + list(extra) + [path],
                       capture_output=True, text=True)
    if raw:
        return r.stdout
    rows = []
    for l in r.stdout.splitlines():
        m = re.match(r"\s*(\d+) s: valid (\d) avg\s+(\S+) mom\s+(\S+) delta\s+(\S+) peak\s+(\S+) dBFS limiting (\d) agc\s+(\S+) dB", l)
        if m:
            rows.append(dict(t=int(m[1]), valid=int(m[2]), avg=float(m[3]), delta=float(m[5]), peak=float(m[6]),
                             limiting=int(m[7]), agc=float(m[8])))
    return rows


fs, x = wavfile.read(os.path.join(ROOT, "third_party/rade_c-src/wav/david_vk5dgr.wav"))
x = x.astype(float) / 32768
mic = resample_poly(x, 3, 1)[: 48000 * 11]
print("meter against BS.1770 (11 s of speech, radio not keyed)")
for gdb in (-30, -18, -9, 0):
    m = mic * 10 ** (gdb / 20)
    p = os.path.join(OUT, "lvl_t.f32")
    np.stack([m, 0 * m], 1).astype(np.float32).tofile(p)
    rows = run(p)
    ref = lufs(np.clip(m, -1, 1)) if gdb < 0 else lufs(np.clip(m, -1, 1))
    got = rows[-1]["avg"]
    check(rows[-1]["valid"] == 1 and abs(got - ref) < 1.0, "input %+d dB: meter %.1f LUFS, independent calculation %.1f" % (gdb, got, ref))
    check(abs(rows[-1]["delta"] - (got + 20.0)) < 0.05, "  delta is loudness minus the -20 LUFS ideal (%+.1f dB)" % rows[-1]["delta"])
m = mic * 10 ** (-12 / 20)
np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
a = run(os.path.join(OUT, "lvl_t.f32"))[-1]["avg"]
b = run(os.path.join(OUT, "lvl_t.f32"), trim=10.0)[-1]["avg"]
check(abs((b - a) - 10.0) < 0.5, "trim +10 dB raises the meter by %.1f dB" % (b - a))
c = run(os.path.join(OUT, "lvl_t.f32"), trim=-6.0)[-1]["avg"]
check(abs((a - c) - 6.0) < 0.5, "trim -6 dB lowers it by %.1f dB" % (a - c))
print("limiter")
m = mic * 10 ** (12 / 20)
np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
r = run(os.path.join(OUT, "lvl_t.f32"))[-1]
check(r["limiting"] == 1 and r["peak"] > 3.0, "+12 dB over: limiter acting, peak %.1f dBFS reported before it" % r["peak"])
print("leveller (works only while transmitting)")
for gdb in (-24, -10, +8):
    m = mic * 10 ** (gdb / 20)
    np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
    rows = run(os.path.join(OUT, "lvl_t.f32"), agc=1, repeat=12, extra=["--keyed"])
    r = rows[-1]
    check(abs(r["delta"]) <= 3.0, "mic %+d dB: after %d s the leveller has it at %+.1f dB of ideal (adding %+.1f dB)" %
          (gdb, r["t"], r["delta"], r["agc"]))
m = mic * 10 ** (-24 / 20)
np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
rows = run(os.path.join(OUT, "lvl_t.f32"), agc=1, repeat=9)
check(all(abs(r["agc"]) < 1e-9 for r in rows), "not transmitting: the leveller adds nothing (0.0 dB on every reading, mic -24 dB)")
m = mic * 10 ** (-45 / 20)
np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
rows = run(os.path.join(OUT, "lvl_t.f32"), agc=1, repeat=40, extra=["--keyed"])
check(max(r["agc"] for r in rows) <= 20.0 + 1e-9, "a very quiet mic (-45 dB): the leveller stops at %+.1f dB (limit +20)" % max(r["agc"] for r in rows))
out = run(os.path.join(OUT, "lvl_t.f32"), agc=1, repeat=12, extra=["--rekey"], raw=True)
mm = re.search(r"rekeyed: agc\s+(\S+)", out); um = re.search(r"unkeyed: agc\s+(\S+)", out)
check(um and abs(float(um[1])) < 1e-9 and mm and abs(float(mm[1])) < 1e-9, "after the over: 0.0 dB unkeyed, and 0.0 dB at the next key-down (no value kept from the last over)")
m = mic * 10 ** (-24 / 20)
np.stack([m, 0 * m], 1).astype(np.float32).tofile(os.path.join(OUT, "lvl_t.f32"))
r1 = run(os.path.join(OUT, "lvl_t.f32"), agc=1, repeat=1, extra=["--keyed"])
check(max(abs(r["agc"]) for r in r1) <= 0.5 * 11, "11 s into an over (mic 24 dB low) the leveller has moved %+.1f dB: at most 0.5 dB a second" % r1[-1]["agc"])
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

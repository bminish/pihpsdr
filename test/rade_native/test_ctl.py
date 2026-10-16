#!/usr/bin/env python3
"""The harness socket server (src/rade_native_test.c) with a stand-in for piHPSDR behind it (test_ctl):
refusals while not armed, WAV loading, the play sequence and its timings, and that a client that goes away
mid-over leaves nothing keyed."""
import os, struct, subprocess, sys, tempfile, threading, time, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rade_ctl

fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

scratch = tempfile.mkdtemp()
os.environ["XDG_STATE_HOME"] = scratch
srv = subprocess.Popen([os.path.join(HERE, "test_ctl")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
path = os.path.join(scratch, "pihpsdr-rade", "control.sock")
for _ in range(100):
    if os.path.exists(path):
        break
    time.sleep(0.05)
try:
    check(oct(os.stat(path).st_mode & 0o777) == "0o600", "the socket is mode 0600")

    def wav(name, secs, rate=48000, kind="i16", ch=1):
        n = int(secs * rate)
        x = 0.3 * np.sin(2 * np.pi * 440 * np.arange(n) / rate)
        p = os.path.join(scratch, name)
        if kind == "i16":
            with wave.open(p, "wb") as w:
                w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
                w.writeframes((np.repeat(x, ch) * 32767).astype("<i2").tobytes())
        else:                                   # float32 WAV, written by hand
            d = x.astype("<f4").tobytes()
            h = b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, rate, rate * 4, 4, 32)
            open(p, "wb").write(h + b"data" + struct.pack("<I", len(d)) + d)
        return p

    c = rade_ctl.Ctl(path)
    print("not armed")
    check(c.status()["armed"] == "0", "status works")
    ok, d = c.request("play file=%s" % wav("a.wav", 1.0))
    check(not ok and "not armed" in d["error"], "play refused")
    ok, d = c.request("set wave=1")
    check(not ok, "set refused")
    ok, d = c.request("frobnicate")
    check(not ok, "an unknown request is an error")
    print("armed")
    c.request("set armed=1")
    ok, d = c.request("inject file=%s" % wav("w48.wav", 1.0, rate=44100))
    check(not ok and "48 kHz" in d["error"], "a 44.1 kHz file is refused: %s" % d.get("error", ""))
    ok, d = c.request("inject file=%s" % wav("st.wav", 1.0, ch=2))
    check(not ok, "a stereo file is refused")
    ok, d = c.request("inject file=/nonexistent.wav")
    check(not ok, "a missing file is refused")
    ok, d = c.request("inject file=%s gain_db=-6" % wav("g.wav", 1.0))
    check(ok and abs(float(d["peak"]) - 0.3 * 10 ** (-6 / 20)) < 0.01, "gain_db scales the samples (peak %s)" % d.get("peak"))
    for kind in ("i16", "f32"):
        p = wav("p_%s.wav" % kind, 2.0, kind=kind)
        t = time.time()
        r = c.play(p)
        el = time.time() - t
        check(r["done"] == "complete" and abs(r["duration_s"] - 2.0) < 0.01, "%s file: played to the end" % kind)
        check(r["t_keyed"] >= r["t_call"] and abs((r["t_inject_end"] - r["t_inject_start"]) - 2.0) < 0.15
              and r["t_unkey"] >= r["t_inject_end"] - 0.1 and r["t_released"] >= r["t_unkey"] and el < 4.5,
              "%s file: times in order (key %.2f s after the call, over %.2f s, released %.2f s after the last sample)" % (
                  kind, r["t_keyed"] - r["t_call"], r["t_inject_end"] - r["t_inject_start"], r["t_released"] - r["t_inject_end"]))
        check(c.status()["tx"] == "0", "%s file: not keyed afterwards" % kind)
    ok, d = c.request("play file=%s max_s=2" % wav("long.wav", 6.0), timeout=30)
    check(not ok and "timeout" in d["error"] and c.status()["tx"] == "0", "an over that outlasts max_s is cut off and released")
    print("a client that goes away mid-over")
    c2 = rade_ctl.Ctl(path)
    th = threading.Thread(target=lambda: c2.request("play file=%s" % wav("m.wav", 10.0), timeout=30), daemon=True)
    th.start()
    time.sleep(1.0)
    check(c.status()["tx"] == "1", "keyed while it plays")
    import socket as _s; c2.s.shutdown(_s.SHUT_RDWR); c2.s.close()
    time.sleep(1.5)
    st = c.status()
    check(st["tx"] == "0" and int(st["aborts"]) >= 1, "client gone: released and aborted")
    c.close()
finally:
    srv.terminate()
    srv.wait()
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

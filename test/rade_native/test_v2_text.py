#!/usr/bin/env python3
"""The V2 callsign as the real transmit engine sends it, decoded by freedv-gui's own V2 text receiver.

  test_v2_text.py [--call EI6IZ] [--seconds 24]

Speech (a clip of rade_c's wav/) goes through test_tx_engine --v2 --call CALL (src/rade_native.c: the engine that runs in
piHPSDR, offline), its 48 kHz modem output is brought to 8 kHz, with and without noise, and v2_text_interop/interop --iq
decodes it with librade V2 and freedv-gui's rade_text receiver, which must deliver CALL. A run with another callsign is
checked to deliver that one (the stream follows the callsign), and one with no callsign must deliver nothing.
Needs `make test_tx_engine v2_text_interop/interop`. Scratch files in out/ only."""
import argparse, os, subprocess, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "out")
CLIP = os.path.join(ROOT, "third_party/rade_c-src/wav/brian_g8sez.wav")
fails = []


def check(ok, what):
    print("  %-4s %s" % ("ok" if ok else "FAIL", what))
    if not ok:
        fails.append(what)


def engine(call, secs, tag):
    fs, x = wavfile.read(CLIP)
    assert fs == 16000
    xf = np.tile(x.astype(float) / 32768.0, 3)[: int(secs * fs)]
    mic = resample_poly(xf, 3, 1)
    m48 = os.path.join(OUT, tag + ".mic48.f32")
    np.stack([mic, np.zeros_like(mic)], 1).astype(np.float32).tofile(m48)
    iq = os.path.join(OUT, tag + ".txeng.f32")
    cmd = [os.path.join(HERE, "test_tx_engine"), "--v2", "--speed", "8"] + (["--call", call] if call is not None else []) + [m48, iq]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("test_tx_engine failed: " + r.stderr[-300:])
    y = np.fromfile(iq, dtype=np.float32).reshape(-1, 2)
    return y[:, 0] + 1j * y[:, 1]


def decode(y48, conj, snr_db, tag, want, seed=1):
    """y48 -> 8 kHz baseband (WDSP's convention is the conjugate of librade's: the engine's output is conjugated for a normal signal)"""
    y = np.conj(y48) if conj else y48
    y8 = resample_poly(y, 1, 6)
    act = y8[np.abs(y8) > 1e-4]
    if snr_db is not None:
        p = np.mean(np.abs(act) ** 2)
        sig = np.sqrt(p * (8000 / 3000) / 10 ** (snr_db / 10) / 2)
        rng = np.random.default_rng(seed)
        y8 = y8 + sig * (rng.standard_normal(len(y8)) + 1j * rng.standard_normal(len(y8)))
    f = os.path.join(OUT, tag + ".iq8.f32")
    np.stack([y8.real, y8.imag], 1).astype(np.float32).tofile(f)
    r = subprocess.run([os.path.join(HERE, "v2_text_interop", "interop"), "--iq", f, want], capture_output=True, text=True)
    line = [l for l in r.stdout.splitlines() if "delivered" in l]
    return r.returncode == 0, (line[0] if line else r.stdout[-200:] + r.stderr[-200:])


def rx_engine(y48, snr_db, tag):
    """the receive engine of piHPSDR (src/rade_native.c) on the 48 kHz signal as WDSP delivers it: the calls it reports"""
    x = y48.copy()
    if snr_db is not None:
        act = x[np.abs(x) > 1e-4]
        p = np.mean(np.abs(act) ** 2)
        sig = np.sqrt(p * (48000 / 3000) / 10 ** (snr_db / 10) / 2)
        rng = np.random.default_rng(3)
        x = x + sig * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
    f = os.path.join(OUT, tag + ".rxin.f32")
    np.stack([x.real, x.imag], 1).astype(np.float32).tofile(f)
    r = subprocess.run([os.path.join(HERE, "test_rx_engine"), "--v2", "--speed", "8", f, os.path.join(OUT, tag + ".rx.wav")],
                       capture_output=True, text=True)
    return [l.split()[1] for l in r.stdout.splitlines() if l.startswith("call: ")], r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--call", default="EI6IZ")
    ap.add_argument("--seconds", type=float, default=24.0)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for tool in ("test_tx_engine", "v2_text_interop/interop"):
        if not os.path.exists(os.path.join(HERE, tool)):
            sys.exit("build first: make test_tx_engine v2_text_interop/interop")
    y = engine(a.call, a.seconds, "v2txt")
    # which way round the engine's output is for librade: whichever decodes (the normal case is conjugated); then it must stay so
    ok_c, info_c = decode(y, True, None, "v2txt_c", a.call)
    ok_n, info_n = decode(y, False, None, "v2txt_n", a.call)
    check(ok_c != ok_n or ok_c, "engine output decodes one way round (conjugated: %s, not: %s)" % (ok_c, ok_n))
    conj = ok_c or not ok_n
    check(ok_c or ok_n, "clean engine output: '%s' delivered by freedv-gui's receiver (%s)" % (a.call, (info_c if ok_c else info_n)[:100]))
    for snr in (9.0, 6.0):
        ok, info = decode(y, conj, snr, "v2txt_%d" % snr, a.call)
        check(ok, "SNR %.0f dB (3 kHz): '%s' delivered (%s)" % (snr, a.call, info[:90]))
    y2 = engine("W1AW/P", a.seconds, "v2txt2")
    ok, info = decode(y2, conj, None, "v2txt2", "W1AW/P")
    check(ok, "another callsign: the stream follows it (%s)" % info[:90])
    y3 = engine("", a.seconds, "v2txt3")
    ok, info = decode(y3, conj, None, "v2txt3", "")
    check("delivered ''" in info and "(0 times)" in info, "no callsign: nothing is delivered (%s)" % info[:90])
    if not os.path.exists(os.path.join(HERE, "test_rx_engine")):
        sys.exit("build first: make test_rx_engine")
    # the receive engine: the callsign comes out of the engine, once per reception
    for snr in (None, 9.0, 6.0):
        calls, out = rx_engine(y, snr, "v2rx%s" % snr)
        check(calls == [a.call], "receive engine%s: calls reported %s" % ("" if snr is None else " at %.0f dB" % snr, calls))
    calls, out = rx_engine(y2, None, "v2rx2")
    check(calls == ["W1AW/P"], "receive engine, another callsign: %s" % calls)
    calls, out = rx_engine(y3, None, "v2rx3")
    check(calls == [], "receive engine, no callsign sent: nothing reported (%s)" % calls)
    print("FAILED %d" % len(fails) if fails else "all passed")
    return 1 if fails else 0


sys.exit(main())

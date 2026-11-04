#!/usr/bin/env python3
"""Validate the TX path against librade's own reference.

For each speech clip (16 kHz, e.g. rade_c's wav/*.wav) and waveform:

  reference:  rade_tx_wav -> rade_rx_wav                      (librade tools)
  ours:       mic 48 kHz -> rade_native TX engine -> WDSP TXA -> [capture]
              -> WDSP RXA -> rade_native RX engine -> speech

The capture is taken at the far side of WDSP (TXA output, 48 kHz complex),
saved, and replayed through RXA and the receive engine, which is what an
on-air signal goes through at the other end.

Reported per clip:
  feat   normalised error of our TX vocoder features against the reference's
  vs ref  correlation of our decoded speech with the reference decode
          (waveform, and 10 ms energy envelope)
  vs in  envelope correlation of each decode with the input speech
Envelope correlations are taken after aligning in time, over the overlap,
ignoring 0.5 s at each end. PASS when the end-of-over callsign is decoded (V1), the hold after release is
under a second, and our TX features are within 5 % of the
reference's (normalised error) and the envelope of our decoded speech
correlates 0.90 or better with the reference decode. The correlation with
the input is shown for information (the alignment against the raw input is
less reliable than against the reference decode). The waveform correlation is
reported but does not gate: the vocoder's output phase is not reproducible
from nearly-equal features (measured: 0.02-0.06 with a feature error under
1 %).

  tx_validate.py [--v2] [--seconds 12] [clip.wav ...]
"""
import argparse, os, subprocess, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, correlate, welch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "out")
B = os.path.join(ROOT, "third_party/rade_c-build/src")
PROBE = os.path.join(HERE, "wdsp_probe")
CALL = "EI6IZ"


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("failed: %s\n%s" % (" ".join(cmd), r.stderr[-400:]))


def env(x, n=160):
    k = len(x) // n
    return np.sqrt((x[:k * n].reshape(k, n) ** 2).mean(1))


def envcorr(a, b, trim=50):
    """Correlation of the 10 ms energy envelopes after aligning them in time,
    over the overlap only, ignoring `trim` frames (0.5 s) at each end where
    start-up delay and flushing differ between the paths."""
    a, b = env(a), env(b)
    a0, b0 = a - a.mean(), b - b.mean()
    c = correlate(b0, a0, mode="full")
    lag = np.argmax(c) - (len(a0) - 1)          # b is a delayed by lag frames
    a1 = a[max(0, -lag):]; b1 = b[max(0, lag):]
    n = min(len(a1), len(b1)) - 2 * trim
    if n < 50:
        return 0.0
    a1, b1 = a1[trim:trim + n], b1[trim:trim + n]
    return float(np.corrcoef(a1, b1)[0, 1])


def wavcorr(a, b, maxlag=16000):
    """Peak normalised cross-correlation of b against a within +-maxlag."""
    n = min(len(a), len(b))
    a = a[:n] - a[:n].mean(); b = b[:n] - b[:n].mean()
    c = correlate(b, a, mode="full", method="fft")
    mid = n - 1
    c = c[mid - maxlag:mid + maxlag + 1]
    return np.abs(c).max() / np.sqrt((a ** 2).sum() * (b ** 2).sum() + 1e-30)


def feat_error(ref, ours):
    ref, ours = ref.reshape(-1, 36)[:, :20], ours.reshape(-1, 36)[:, :20]
    best = None
    for lag in range(-40, 41):
        a = ref[max(0, lag):]; b = ours[max(0, -lag):]
        n = min(len(a), len(b)) - 30
        if n < 100:
            continue
        a, b = a[25:n], b[25:n]
        e = np.mean((a - b) ** 2) / np.mean(a ** 2)
        if best is None or e < best[0]:
            best = (e, lag)
    return best


def one(clip, v2, secs):
    tag = os.path.splitext(os.path.basename(clip))[0] + (".v2" if v2 else ".v1")
    opt = ["--v2"] if v2 else []
    fs, x = wavfile.read(clip)
    assert fs == 16000, "clip must be 16 kHz"
    x = x[:int(secs * fs)]
    src = os.path.join(OUT, tag + ".src.wav")
    wavfile.write(src, 16000, x)
    xf = x.astype(float) / 32768.0

    # reference
    rtx, rf = os.path.join(OUT, tag + ".ref.wav"), os.path.join(OUT, tag + ".ref.feat")
    rdec = os.path.join(OUT, tag + ".refdec.wav")
    run([B + "/rade_tx_wav", "-v", "0"] + opt + ["-f", rf, src, rtx])
    run([B + "/rade_rx_wav", "-v", "0"] + opt + [rtx, rdec])
    ref_feat = np.fromfile(rf, dtype=np.float32)
    _, rd = wavfile.read(rdec)

    # ours: mic at 48 kHz in the I slot
    mic = resample_poly(xf, 3, 1)
    m48 = os.path.join(OUT, tag + ".mic48.f32")
    np.stack([mic, np.zeros_like(mic)], 1).astype(np.float32).tofile(m48)
    iq, feat = os.path.join(OUT, tag + ".txeng.f32"), os.path.join(OUT, tag + ".our.feat")
    r = subprocess.run([os.path.join(HERE, "test_tx_engine")] + opt + ["--speed", "3", "--features", feat,
                                                                  "--call", CALL, m48, iq], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("test_tx_engine failed: " + r.stderr[-300:])
    hold = [l for l in r.stdout.splitlines() if l.startswith("held")]
    hold_ms = float(hold[0].split()[1]) if hold and "NOT DONE" not in hold[0] else -1
    our_feat = np.fromfile(feat, dtype=np.float32)

    fc, bw = (1500.0, 1000.0) if v2 else (1475.0, 1600.0)
    lo, hi = fc - bw / 2, fc + bw / 2
    cap = os.path.join(OUT, tag + ".txcap48.f32")          # the capture, far side of TXA
    run([PROBE, "tx", "--wisdom", ROOT + "/", "--low", str(lo), "--high", str(hi), "--neutral", iq, cap])
    rxo = os.path.join(OUT, tag + ".rxout48.f32")
    run([PROBE, "rx", "--wisdom", ROOT + "/", "--low", str(lo), "--high", str(hi), "--copy", "0", "--agc", "3", cap, rxo])
    dec = os.path.join(OUT, tag + ".decoded.wav")
    r = subprocess.run([os.path.join(HERE, "test_rx_engine")] + opt + ["--speed", "8", rxo, dec],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("test_rx_engine failed")
    calls = [l.split()[1] for l in r.stdout.splitlines() if l.startswith("call:")]
    got_call = calls[0] if calls else "-"
    _, d48 = wavfile.read(dec)
    od = resample_poly(d48.astype(float), 1, 3)

    # numbers
    y = np.fromfile(cap, dtype=np.float32).reshape(-1, 2)
    y = y[:, 0] + 1j * y[:, 1]
    act = y[np.abs(y) > 0.02 * np.abs(y).max()]
    pw = np.abs(act) ** 2
    f, p = welch(act, 48000, nperseg=4096, return_onesided=False)
    o = np.argsort(f); f, p = f[o], p[o]
    c = np.cumsum(p) / p.sum()
    b0, b1 = f[np.searchsorted(c, 0.005)], f[np.searchsorted(c, 0.995)]
    fe = feat_error(ref_feat, our_feat)
    ref_in = envcorr(xf, rd.astype(float))
    our_in = envcorr(xf, od)
    wr = wavcorr(rd.astype(float), od)
    er = envcorr(rd.astype(float), od)
    # the callsign rides in the V1 end-of-over frame; V2 has no text yet
    call_ok = v2 or got_call == CALL
    ok = fe[0] < 0.05 and er >= 0.90 and call_ok and 0 < hold_ms < 1000
    print("%-26s feat %.3f | vs ref: wave %.2f env %.2f | vs in: ours %.2f ref %.2f | TXA out rms %.2f PAPR %.1f dB "
          "band %.0f..%.0f Hz | call %s hold %.0f ms | %s" %
          (tag[:26], fe[0], wr, er, our_in, ref_in, np.sqrt(pw.mean()),
           10 * np.log10(pw.max() / pw.mean()), b0, b1, got_call, hold_ms, "PASS" if ok else "FAIL"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--v2", action="store_true")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("clips", nargs="*")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    clips = a.clips or sorted(os.path.join(ROOT, "third_party/rade_c-src/wav", f)
                              for f in ("brian_g8sez.wav", "david_vk5dgr.wav", "k0pfx_mel.wav"))
    ok = True
    for c in clips:
        ok &= one(c, a.v2, a.seconds)
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()

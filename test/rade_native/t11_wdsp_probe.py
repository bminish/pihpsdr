#!/usr/bin/env python3
"""T-11: does the vendored WDSP carry complex baseband the way the plan assumes?

Runs complex tones through one RXA and one TXA channel (wdsp_probe) with
the filter edges positive and negative, and reports for each case the
output level and how the output power splits between positive and negative
frequency (a complex, single-sideband output is all on one side; a real,
I-copied-to-Q output is split 50/50).

Tones are numpy I + jQ = exp(+j w t). Result recorded in
docs/rade-native-progress.md: WDSP treats a POSITIVE filter as the signal
at NEGATIVE numpy frequency, so librade's positive-frequency convention
needs a conjugation at both WDSP interfaces.
"""
import os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "wdsp_probe")
FS = 48000.0
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def tone(f, secs=3.0, amp=0.1):
    n = np.arange(int(secs * FS))
    return (amp * np.exp(2j * np.pi * f * n / FS)).astype(np.complex64)


def run(kind, x, args):
    with tempfile.TemporaryDirectory() as d:
        fi, fo = os.path.join(d, "i"), os.path.join(d, "o")
        np.stack([x.real, x.imag], 1).astype(np.float32).tofile(fi)
        subprocess.run([PROBE, kind, "--wisdom", ROOT + "/"] + args + [fi, fo], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        y = np.fromfile(fo, dtype=np.float32).reshape(-1, 2)
    return y[:, 0] + 1j * y[:, 1]


def report(label, x, y):
    y = y[len(y) // 2:]                    # settled half
    xi = x[len(x) // 2:]
    pin = np.mean(np.abs(xi) ** 2) + 1e-30
    pout = np.mean(np.abs(y) ** 2) + 1e-30
    sp = np.abs(np.fft.fft(y * np.hanning(len(y)))) ** 2
    f = np.fft.fftfreq(len(y), 1 / FS)
    pos, neg = sp[f > 0].sum(), sp[f < 0].sum()
    frac = pos / (pos + neg + 1e-30)
    iq = np.corrcoef(y.real, y.imag)[0, 1] if pout > 1e-20 else 0
    g = 10 * np.log10(pout / pin)
    print("  %-46s %-6s gain %7.1f dB   output at +f %5.1f%%  |corr(I,Q)| %.2f" %
          (label, "passes" if g > -60 else "stops", g, 100 * frac, abs(iq)))


def main():
    print("RX (RXA, DIGU, panel select 3)")
    for ft, lo, hi, cp in [(1000, 500, 1500, 0), (-1000, 500, 1500, 0),
                           (-1000, -1500, -500, 0), (1000, -1500, -500, 0),
                           (1000, 500, 1500, 1)]:
        x = tone(ft)
        y = run("rx", x, ["--low", str(lo), "--high", str(hi), "--copy", str(cp)])
        report("tone %+5d  filter %+5d..%+5d  copy %d" % (ft, lo, hi, cp), x, y)
    print("TX (TXA, DIGU, panel select 3)")
    for ft, lo, hi, extra in [(1000, 500, 1500, []), (-1000, 500, 1500, []),
                              (-1000, -1500, -500, []), (1000, -1500, -500, []),
                              (1000, 500, 1500, ["--neutral"]),
                              (1000, 500, 1500, ["--neutral", "--alc", "0"])]:
        x = tone(ft, amp=0.3)
        y = run("tx", x, ["--low", str(lo), "--high", str(hi)] + extra)
        report("tone %+5d  filter %+5d..%+5d %s" % (ft, lo, hi, " ".join(extra)), x, y)


if __name__ == "__main__":
    main()

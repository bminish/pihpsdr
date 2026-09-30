#!/usr/bin/env python3
"""
DEVELOPMENT TOOL. Score run_ref weight series on a wideband capture
(Window, Carrier, FSK/Digital), the way docs/diversity-measurements.md
does.

Each weight is applied one block late, as run_ref records it, and the way
receiver.c combines: out = arm0 + w * arm1. The score is a split-guard
passband SNR (Finding 18, "Two guard regions, not one"): signal power in
the RX passband against noise power in a guard band beyond it that the
loop never fitted on, over the whole capture.

Per run it also reports:

- how often the loop acted (run_ref's ok column) in signal blocks and in
  noise-only blocks. A block is "signal" when its passband power, both
  arms, stands 6 dB above the capture's own 20th percentile;
- the 95th percentile coherence in noise-only blocks. High means the
  "noise" is correlated between the antennas (common-mode or band noise),
  which a coherence gate cannot and should not reject;
- the median |w| applied in signal and in noise-only blocks;
- the SNR again from the first block after both of the first two runs
  have acted, which takes run_ref's cold start (w = 1 until the loop
  first acts) out of a comparison between them - see Finding 38.

Calibrated against Finding 38 on 235906 (Window / Sum, as recorded):
gate 0.00 scores 12.56 dB against the published 12.75, gate 0.30 4.28
against 4.24.

usage: score_wideband.py CAPTURE.divc RUN1.csv [RUN2.csv ...]
"""
import csv
import struct
import sys

import numpy as np

from divc import BLK, REC_MAGIC, block_meta, open_divc

MODE_CWL, MODE_CWU = 3, 4


def bh4(n):
    """The engine's 4-term Blackman-Harris window (div_make_window)."""
    x = 2 * np.pi * np.arange(n) / n
    return 0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2 * x) - 0.01168 * np.cos(3 * x)


def load_blocks(path):
    """[(frame_off, filter_low, filter_high, arm0, arm1)] for every block."""
    f, h = open_divc(path)
    n = h['nfft']
    out = []

    while True:
        m = f.read(BLK)

        if len(m) < BLK or struct.unpack_from('<I', m, 0)[0] != REC_MAGIC:
            break

        b = block_meta(m)
        a0 = np.frombuffer(f.read(8 * n), dtype=np.float32).view(np.complex64).copy()
        a1 = np.frombuffer(f.read(8 * n), dtype=np.float32).view(np.complex64).copy()
        # div_frame_off(): CW moves the frame by the sidetone
        fo = b['offset']

        if b['mode'] == MODE_CWU:
            fo -= b['sidetone']
        elif b['mode'] == MODE_CWL:
            fo += b['sidetone']

        out.append((fo, b['filter_low'], b['filter_high'], a0, a1))

    f.close()
    return h, out


def regions(rate, n, fo, flo, fhi):
    """
    Passband and guard masks over the capture's FFT bins. The engine maps
    a shifted-frame frequency s to bin frequency -(s + frame_off)
    (div_shift_to_bin), so the spectrum is inverted.
    """
    fr = np.fft.fftfreq(n, 1.0 / rate)
    lo, hi = sorted((-(fhi + fo), -(flo + fo)))
    pb = (fr >= lo) & (fr <= hi)
    width = min(hi - lo, 2500.0)
    zero = -fo

    if flo < 0 < fhi:
        # symmetric filter (AM, SAM): a guard either side
        g = (((fr > hi + 500) & (fr < hi + 500 + width))
             | ((fr < lo - 500) & (fr > lo - 500 - width)))
    elif hi <= zero:
        g = (fr < lo - 450) & (fr > lo - 450 - width)
    else:
        g = (fr > hi + 450) & (fr < hi + 450 + width)

    return pb, g


def main():
    cap, runs = sys.argv[1], sys.argv[2:]
    h, blks = load_blocks(cap)
    n, rate = h['nfft'], h['rate']
    win = bh4(n).astype(np.float32)
    F0, F1, PB, G = [], [], [], []

    for fo, flo, fhi, a0, a1 in blks:
        pb, g = regions(rate, n, fo, flo, fhi)
        F0.append(np.fft.fft(a0 * win))
        F1.append(np.fft.fft(a1 * win))
        PB.append(pb)
        G.append(g)

    nb = len(blks)
    ppow = np.array([np.sum(np.abs(F0[b][PB[b]]) ** 2 + np.abs(F1[b][PB[b]]) ** 2)
                     for b in range(nb)])
    sig = ppow > np.percentile(ppow, 20) * 10 ** 0.6

    def snr(wl, start=0):
        s = g = 0.0

        for b in range(start, nb):
            y = F0[b] + wl[b] * F1[b]
            s += np.mean(np.abs(y[PB[b]]) ** 2)
            g += np.mean(np.abs(y[G[b]]) ** 2)

        return 10 * np.log10(s / g)

    arm0 = snr(np.zeros(nb, complex))
    s1 = sum(np.mean(np.abs(F1[b][PB[b]]) ** 2) for b in range(nb))
    g1 = sum(np.mean(np.abs(F1[b][G[b]]) ** 2) for b in range(nb))
    arm1 = 10 * np.log10(s1 / g1)

    loaded = []

    for r in runs:
        rows = list(csv.DictReader(open(r)))[:nb]
        ok = np.array([int(x['ok']) for x in rows])
        coh = np.array([float(x['quality']) for x in rows])
        w = np.array([float(x['wr']) + 1j * float(x['wi']) for x in rows])
        wl = np.concatenate(([1.0 + 0j], w[:-1]))   # one block late; run_ref starts at w = 1
        loaded.append((r, ok, coh, wl))

    firsts = [int(np.argmax(ok)) if ok.any() else nb for _, ok, _, _ in loaded[:2]]
    common = max(firsts) + 2

    print(f"# {cap.split('/')[-1]}: {nb} blocks, {100 * np.mean(sig):.0f} % signal; "
          f"arm0 {arm0:+.2f} dB, arm1 {arm1:+.2f} dB")
    print(f"{'run':34s} {'SNR':>7s} {'vs arm':>7s} {'from ' + str(common):>9s} "
          f"{'act sig':>8s} {'act noise':>9s} {'coh95 noise':>11s} {'|w| sig':>8s} {'|w| noise':>9s}")

    for r, ok, coh, wl in loaded:
        noise = ~sig[:len(ok)]
        wdb = 20 * np.log10(np.maximum(np.abs(wl), 1e-6))
        print(f"{r.split('/')[-1][:34]:34s} {snr(wl):+7.2f} {snr(wl) - max(arm0, arm1):+7.2f} "
              f"{snr(wl, common):+9.2f} "
              f"{100 * np.mean(ok[sig[:len(ok)]]):7.1f}% {100 * np.mean(ok[noise]):8.1f}% "
              f"{np.percentile(coh[noise], 95):11.3f} "
              f"{np.median(wdb[sig[:len(ok)]]):+8.1f} {np.median(wdb[noise]):+9.1f}")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
cwlat_spectrum - are the keyed CW elements (including the shortened ones) cleanly shaped?
Looks for key clicks and other wideband / transient artefacts in the KiwiSDR IQ (+-6 kHz passband).

 1. edge spectra: energy of windows around element onsets / offsets and around whole short elements, in bands
    300 Hz..5 kHz from the carrier, with the matching noise-only windows subtracted and compared with synthetic
    references run through the same processing (pihpsdr's DL1YCF 9 ms ramp, a raised cosine, hard edges)
 2. shortened (clipped) elements against normal ones: duration, peak amplitude, spectrum
 3. envelope integrity inside long elements (ripple, dips)
 4. carrier frequency at onset (chirp / settling)
 5. spurs inside long elements
usage: cwlat_spectrum.py RESULTS_DIR... [--out results/analysis]
"""
import argparse, os, sys
import numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cwlat_analyze as A
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

N = 512
W = np.hanning(N)
BANDS = [(300, 600), (600, 1000), (1000, 2000), (2000, 3000), (3000, 5000)]


def dl1ycf_ramp(width):
    y = np.arange(width + 1) / width
    y2 = 2 * np.pi * y
    return (y - 0.12182865361171612 * np.sin(y2) - 0.018557469249199286 * np.sin(2 * y2)
            - 0.0009378783245428506 * np.sin(3 * y2) + 0.0008567571519403228 * np.sin(4 * y2)
            + 0.00018706912431472442 * np.sin(5 * y2))


def envelope_synth(kind, fs_hi=192000, dur_ms=120.0, ramp_ms=9.0):
    """keyed envelope sampled at fs_hi: rises at 40 ms, falls at 40+dur_ms"""
    n = int(fs_hi * (dur_ms + 80) / 1000)
    env = np.zeros(n)
    i0 = int(fs_hi * 0.040)
    i1 = i0 + int(fs_hi * dur_ms / 1000)
    L = int(fs_hi * ramp_ms / 1000)
    if kind == "dl1ycf":
        r = dl1ycf_ramp(L)
    elif kind == "raised-cosine":
        r = 0.5 * (1 - np.cos(np.pi * np.arange(L + 1) / L))
    elif kind.startswith("linear"):
        L = int(fs_hi * float(kind.split("-")[1]) / 1000)
        r = np.arange(L + 1) / L
    env[i0:i0 + len(r)] = r
    env[i0 + len(r):i1] = 1.0
    env[i1:i1 + len(r)] = r[::-1]
    return env


def to_kiwi_rate(env_hi, fs_hi=192000, fs=12000.0):
    # ideal band limit to +-5.5 kHz (the Kiwi passband), then decimate
    sos = signal.butter(8, 5500.0 / (fs_hi / 2), output="sos")
    lo = signal.sosfiltfilt(sos, env_hi.astype(float))
    return lo[::int(fs_hi / fs)]


def band_powers(x, centre, ref, fs):
    """windowed (Hann, N) energy of the baseband signal in the offset bands, relative to the carrier reference power"""
    a = int(round(centre)) - N // 2
    if a < 0 or a + N > len(x):
        return None
    X = np.fft.fft(x[a:a + N] * W)
    P = np.abs(X) ** 2
    f = np.fft.fftfreq(N, 1.0 / fs)
    out = []
    for lo, hi in BANDS:
        m = (np.abs(f) >= lo) & (np.abs(f) < hi)
        out.append(P[m].sum())
    return np.array(out) / ref, P / ref


def collect(dirs):
    runs = []
    for d in A.find_runs(dirs):
        try:
            info, rows, q, segs, ex = A.analyse_run(d, keep=True)
        except Exception as e:
            print("!!", d, e, file=sys.stderr)
            continue
        runs.append((info, rows, q, segs, ex))
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--out", default="results/analysis")
    args = ap.parse_args()
    os.makedirs(os.path.join(args.out, "plots"), exist_ok=True)
    runs = collect(args.dirs)
    fs = float(np.mean([r[4]["fs"] for r in runs]))
    lines = ["# cwlat spectral / shape check", "",
             f"{len(runs)} runs; window {N} samples ({N/fs*1000:.0f} ms, Hann); bands are offsets from the carrier; "
             "levels are energy in the band relative to the carrier energy in the same window.",
             "**Caveat: the noise reference is unreliable.** In-band energy under the carrier is 11-13 dB LOWER than in key-up windows "
             "(cause not established: Kiwi AGC, an external keyed station in the band, or the transmitter's own noise), so the edge-excess "
             "table below comes out negative and must not be read as a measurement of key-click level. Re-capture with the Kiwi AGC off "
             "(kiwirecorder -g) to make this check valid.", ""]

    # accumulators
    cls = {"onset, all elements": [], "offset, all elements": [],
           "onset, straight key break-in, after a pause (clipped class)": [],
           "offset, straight key break-in, after a pause (clipped class)": [],
           "onset, other elements": [], "offset, other elements": [],
           "whole element 5-30 ms (shortened)": []}
    psd = {"onset": [], "offset": [], "noise": []}
    noise_b = []
    noise_x, noise_A = [], []
    on_b, off_b = [], []
    env_on = []
    ripple, dips, chirp, spur_psd = [], [], [], []
    short_info = []
    amp_ref = []
    for info, rows, q, segs, ex in runs:
        z, f0, kt, env = ex["z"], ex["f0"], ex["kt"], ex["env"]
        t = np.arange(len(z)) / fs
        x = (z * np.exp(-2j * np.pi * f0 * t)).astype(np.complex128)
        sos = signal.butter(4, 400.0 / (fs / 2), output="sos")
        xl = signal.sosfiltfilt(sos, x)
        ph = np.unwrap(np.angle(xl))
        finst = np.gradient(ph) * fs / (2 * np.pi)
        P_all = np.array([s["plateau"] for s in segs])
        A0 = float(np.median(P_all)) if len(P_all) else 1.0
        amp_ref.append(A0)
        edges = [(s["on10"], s["off10"]) for s in segs]
        for i, s in enumerate(segs):
            dur_ms = (s["off50"] - s["on50"]) / fs * 1000
            prev_off = segs[i - 1]["off10"] if i > 0 else -1e9
            next_on = segs[i + 1]["on10"] if i + 1 < len(segs) else 1e9
            A_el = s["plateau"] if dur_ms > 12 else A0
            ref = (A_el * 0.5 * W.sum()) ** 2
            idle = (s["on50"] - prev_off) / fs * 1000
            clipped_class = (info["key"] == "straight" and info["mode"] == "breakin" and idle > 400)
            if dur_ms > 30:
                if s["on50"] - prev_off > 28:
                    r = band_powers(x, s["on50"], ref, fs)
                    if r:
                        cls["onset, all elements"].append(r[0]); psd["onset"].append(r[1])
                        cls["onset, straight key break-in, after a pause (clipped class)" if clipped_class else "onset, other elements"].append(r[0])
                if next_on - s["off50"] > 28:
                    r = band_powers(x, s["off50"], ref, fs)
                    if r:
                        cls["offset, all elements"].append(r[0]); psd["offset"].append(r[1])
                        cls["offset, straight key break-in, after a pause (clipped class)" if clipped_class else "offset, other elements"].append(r[0])
            centre = 0.5 * (s["on50"] + s["off50"])
            half = 0.5 * dur_ms * fs / 1000
            if 5 <= dur_ms <= 30 and centre - prev_off > 28 + half and next_on - centre > 28 + half:
                i0 = N // 2 - int(round(half)); i1 = N // 2 + int(round(half))
                refs = (A0 * W[i0:i1].sum()) ** 2
                r = band_powers(x, centre, refs, fs)
                if r:
                    cls["whole element 5-30 ms (shortened)"].append(r[0])
                    short_info.append((dur_ms, s["peak"] / A0))
            # long elements: envelope integrity, frequency at onset, spur PSD
            if dur_ms >= 80:
                a, b = int(s["on50"] + 8e-3 * fs), int(s["off50"] - 8e-3 * fs)
                seg_env = env[a:b]
                if len(seg_env) > 40:
                    pl = np.median(seg_env)
                    ripple.append(np.std(seg_env) / pl)
                    dips.append(20 * np.log10(seg_env.min() / pl))
                fa, fb = int(s["on50"] + 2e-3 * fs), int(s["on50"] + 12e-3 * fs)
                fplat = np.median(finst[int(s["on50"] + 20e-3 * fs):int(s["off50"] - 10e-3 * fs)])
                chirp.append((np.mean(finst[fa:fb]) - fplat, np.max(np.abs(finst[fa:fb] - fplat))))
                if dur_ms >= 110:
                    seg = x[int(s["on50"] + 12e-3 * fs):int(s["off50"] - 12e-3 * fs)]
                    if len(seg) >= 768:
                        f, Pw = signal.welch(seg, fs=fs, nperseg=768, noverlap=384, return_onesided=False, detrend=False)
                        spur_psd.append(np.fft.fftshift(Pw) / s["plateau"] ** 2)
                        spur_f = np.fft.fftshift(f)
        # aligned onset envelopes (normal elements)
        for s in segs[:40]:
            a = int(s["on50"]) - int(0.015 * fs)
            if (s["off50"] - s["on50"]) / fs > 0.04 and a > 0 and a + int(0.03 * fs) < len(env):
                env_on.append(env[a:a + int(0.03 * fs)] / s["plateau"])
        # noise-only windows: at least 60 ms from any edge
        marks = np.sort(np.array([e for ed in edges for e in ed]))
        floor = float(np.percentile(env, 10))
        # carrier-on windows: entirely inside the plateau of a long element
        for sg in segs:
            if (sg["off90"] - sg["on90"]) > N + 40:
                for c in range(int(sg["on90"]) + N // 2 + 10, int(sg["off90"]) - N // 2 - 10, 300):
                    ref = (A0 * 0.5 * W.sum()) ** 2
                    r = band_powers(x, c, ref, fs)
                    if r: on_b.append(r[0])
        # key-up windows adjacent to elements (same run, between 70 and 400 ms from an edge, env quiet over the window)
        for c in range(N, len(x) - N, 400):
            lo = np.searchsorted(marks, c - 0.070 * fs); hi = np.searchsorted(marks, c + 0.070 * fs)
            if hi > lo: continue
            lo2 = np.searchsorted(marks, c - 0.400 * fs); hi2 = np.searchsorted(marks, c + 0.400 * fs)
            if hi2 == lo2: continue
            if env[c - N // 2:c + N // 2].max() > 3.0 * floor: continue
            r = band_powers(x, c, (A0 * 0.5 * W.sum()) ** 2, fs)
            if r: off_b.append(r[0])
        for c in range(N, len(x) - N, 700):
            lo = np.searchsorted(marks, c - 0.060 * fs)
            if lo < len(marks) and marks[lo] < c + 0.060 * fs:
                continue
            if env[c - N // 2:c + N // 2].max() > 3.0 * floor:      # key-up (quiet) windows only
                continue
            noise_x.append(x[c - N // 2:c + N // 2].copy()); noise_A.append(A0)
            ref = (A0 * 0.5 * W.sum()) ** 2
            r = band_powers(x, c, ref, fs)
            if r:
                noise_b.append(r[0]); psd["noise"].append(r[1])
    n_on = np.mean(on_b, axis=0); n_off = np.mean(off_b, axis=0); n_quiet = np.mean(noise_b, axis=0)
    noise = 0.5 * (n_on + n_off)           # an edge window is half carrier-on, half key-up
    nn = len(noise_b)
    lines += ["## Noise references (energy in band, dBc of the carrier energy of the same window geometry)", "",
              "| reference | windows | " + " | ".join(f"{lo}-{hi} Hz" for lo, hi in BANDS) + " |", "|---|---|" + "---|" * len(BANDS),
              f"| carrier-on plateau windows | {len(on_b)} | " + " | ".join(f"{10*np.log10(v):.1f}" for v in n_on) + " |",
              f"| key-up windows next to elements | {len(off_b)} | " + " | ".join(f"{10*np.log10(v):.1f}" for v in n_off) + " |",
              f"| key-up windows anywhere (incl. before/after the run) | {nn} | " + " | ".join(f"{10*np.log10(v):.1f}" for v in n_quiet) + " |",
              "", "Edge windows are compared with the average of the first two rows.", ""]
    nse = 0.5 * np.sqrt((np.std(on_b, axis=0, ddof=1) ** 2) / len(on_b) + (np.std(off_b, axis=0, ddof=1) ** 2) / len(off_b))

    # references through the same chain (noise free)
    refs = {}
    for kind in ("dl1ycf", "raised-cosine", "linear-1", "linear-0.2"):
        e = to_kiwi_rate(envelope_synth(kind))
        xr = e.astype(complex)
        c_on = 0.040 * fs + 0.5 * (9e-3 * fs if not kind.startswith("linear") else 0)
        ref = (1.0 * 0.5 * W.sum()) ** 2
        rb = band_powers(xr, 0.040 * fs + 0.0045 * fs, ref, fs)
        refs[kind] = rb[0]

    # sensitivity check: add synthetic edges (carrier amplitude as measured) into real quiet windows
    inj = {}
    rng = np.random.default_rng(1)
    for kind in ("dl1ycf", "raised-cosine", "linear-1", "linear-0.2"):
        e = to_kiwi_rate(envelope_synth(kind))
        a0 = int(0.040 * fs + 0.0045 * fs) - N // 2
        edge = e[a0:a0 + N]
        vals = []
        for xw, A0w in zip(noise_x, noise_A):
            ref = (A0w * 0.5 * W.sum()) ** 2
            X = np.fft.fft((xw + A0w * edge) * W)
            P = np.abs(X) ** 2
            f_ = np.fft.fftfreq(N, 1.0 / fs)
            vals.append([P[(np.abs(f_) >= lo) & (np.abs(f_) < hi)].sum() / ref for lo, hi in BANDS])
        inj[kind] = np.array(vals)
    lines += ["## Edge energy minus the noise reference (ppm of the carrier energy; NEGATIVE values mean the reference is too high - see the caveat)", "",
              "| class | n | " + " | ".join(f"{lo}-{hi} Hz" for lo, hi in BANDS) + " |", "|---|---|" + "---|" * len(BANDS)]
    def excess_row(name, arr):
        a = np.array(arr)
        m = a.mean(axis=0)
        se = np.sqrt((a.std(axis=0, ddof=1) / np.sqrt(len(a))) ** 2 + nse ** 2)     # noise-floor uncertainty included
        cells = []
        for k in range(len(BANDS)):
            ex_ = m[k] - noise[k]
            cells.append(f"{ex_*1e6:+.1f}±{se[k]*1e6:.1f}")
        lines.append(f"| {name} | {len(a)} | " + " | ".join(cells) + " |")
    lines.append("(table: measured excess over the quiet-window noise floor, in parts per million of the carrier energy ± 1 sigma; "
                 "-60 dBc = 1.0, -50 dBc = 10, -40 dBc = 100)")
    for k, v in cls.items():
        if len(v) > 5:
            excess_row(k, v)
    lines += ["", "Sensitivity check - synthetic edges added to real key-up windows, excess over the same windows without the edge (dBc): shows what this method could see in a valid capture", "",
              "| injected edge | " + " | ".join(f"{lo}-{hi} Hz" for lo, hi in BANDS) + " |", "|---|" + "---|" * len(BANDS)]
    base = np.array(noise_b)
    for kind, v in inj.items():
        cells = []
        for k in range(len(BANDS)):
            ex_ = v[:, k].mean() - base[:, k].mean() if False else v[:, k].mean() - np.mean([ (np.abs(np.fft.fft(xw*W))**2)[(np.abs(np.fft.fftfreq(N,1.0/fs))>=BANDS[k][0])&(np.abs(np.fft.fftfreq(N,1.0/fs))<BANDS[k][1])].sum()/((A0w*0.5*W.sum())**2) for xw, A0w in zip(noise_x, noise_A)])
            cells.append(f"{10*np.log10(max(ex_, 1e-30)):.1f}")
        lines.append(f"| {kind} | " + " | ".join(cells) + " |")
    lines += ["", "References, same processing, no noise (dBc):", "",
              "| edge shape | " + " | ".join(f"{lo}-{hi} Hz" for lo, hi in BANDS) + " |", "|---|" + "---|" * len(BANDS)]
    for kind, v in refs.items():
        lines.append(f"| {kind} | " + " | ".join(f"{10*np.log10(max(x_, 1e-30)):.1f} dBc = {x_*1e6:.2g} ppm" for x_ in v) + " |")
    lines += ["", "(the ARRL clean-signal limits the DL1YCF ramp is designed for: -60 dBc beyond 338 Hz, -80 beyond 600 Hz, -100 beyond 900 Hz, "
              "-120 beyond 1200 Hz; the Kiwi's noise floor limits what can be shown to the 2-sigma figures above)", ""]

    # shortened elements
    si = np.array(short_info)
    if len(si):
        lines += ["## Shortened elements (5-60 ms long)", "", f"{len(si)} isolated elements; durations {si[:,0].min():.1f}-{si[:,0].max():.1f} ms; "
                  f"peak amplitude relative to a full element: {np.percentile(si[:,1],5):.2f} .. {np.percentile(si[:,1],95):.2f} "
                  "(< 1 means the element ends before the ramp finishes, a smooth tent, not a hard cut).", ""]
        for lo, hi in ((5, 10), (10, 20), (20, 30), (30, 60)):
            m = (si[:, 0] >= lo) & (si[:, 0] < hi)
            if m.sum():
                lines.append(f"- {lo}-{hi} ms: n={int(m.sum())}, peak amplitude {si[m,1].mean():.2f}±{si[m,1].std():.2f}")
        lines.append("")
    rp = np.array(ripple); dp = np.array(dips)
    lines += ["## Envelope integrity inside long elements (>= 80 ms, edges excluded)", "",
              f"{len(rp)} elements: ripple (sd / level) {100*rp.mean():.2f} % ± {100*rp.std():.2f} %, worst {100*rp.max():.2f} %; "
              f"deepest dip {dp.min():.2f} dB (mean {dp.mean():.2f} dB); elements with a dip deeper than 1 dB: {int((dp < -1).sum())}.", ""]
    ch = np.array(chirp)
    lines += ["## Carrier frequency in the first 2-12 ms after onset (against the plateau)", "",
              f"{len(ch)} elements: mean offset {ch[:,0].mean():+.2f} Hz (sd {ch[:,0].std():.2f}), largest excursion {ch[:,1].max():.1f} Hz, "
              f"median excursion {np.median(ch[:,1]):.2f} Hz (measurement bandwidth 400 Hz).", ""]
    sp = np.mean(spur_psd, axis=0)
    spdb = 10 * np.log10(sp + 1e-30)
    f = spur_f
    med = signal.medfilt(spdb, 15)
    cand = [(f[i], spdb[i] - med[i]) for i in range(len(f)) if abs(f[i]) > 60 and spdb[i] - med[i] > 4.0]
    lines += ["## Spurs inside long elements (average of {} elements, {:.0f} Hz resolution)".format(len(spur_psd), fs / 768), ""]
    lines += [f"- {fq:+.0f} Hz: {d:.1f} dB above the local median" for fq, d in sorted(cand, key=lambda c: -c[1])[:8]] or ["- no line more than 4 dB above the local median beyond ±60 Hz"]
    lines.append("")
    open(os.path.join(args.out, "spectrum_report.md"), "w").write("\n".join(lines))
    print("\n".join(lines))

    # ---- plots
    fa = np.fft.fftshift(np.fft.fftfreq(N, 1.0 / fs))
    fig, ax = plt.subplots(figsize=(8, 4.2))
    def sm(v):
        return signal.savgol_filter(10 * np.log10(np.mean(v, axis=0) + 1e-30), 9, 2)
    ref_pw = (np.mean(amp_ref) * 0.5 * W.sum()) ** 2
    for k, c in (("onset", "tab:blue"), ("offset", "tab:green"), ("noise", "gray")):
        arr = np.array(psd[k])
        P = np.fft.fftshift(arr.mean(axis=0), axes=-1)
        ax.plot(fa, 10 * np.log10(P / ref_pw + 1e-30), color=c, lw=0.9, label=f"measured {k} (n={len(arr)})")
    for kind, c in (("dl1ycf", "k"), ("linear-1", "tab:red")):
        e = to_kiwi_rate(envelope_synth(kind)); xr = e.astype(complex)
        ref = (0.5 * W.sum()) ** 2
        a = int(0.040 * fs + 0.0045 * fs) - N // 2
        Pw = np.abs(np.fft.fft(xr[a:a + N] * W)) ** 2 / ref
        ax.plot(fa, 10 * np.log10(np.fft.fftshift(Pw) + 1e-30), color=c, ls="--", lw=0.8, label=f"ideal {kind} edge (no noise)")
    ax.set_xlim(-5500, 5500); ax.set_ylim(-110, 5)
    ax.set_xlabel("offset from carrier (Hz)"); ax.set_ylabel("dBc (window energy / carrier energy)")
    ax.grid(alpha=0.3); ax.legend(fontsize=7, loc="upper right"); ax.set_title("Spectrum around keying edges")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "plots", "edge_spectra.png"), dpi=110); plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 4))
    tt = (np.arange(int(0.03 * fs)) - int(0.015 * fs)) / fs * 1000
    for e in env_on[::6]:
        ax.plot(tt, e, color="tab:blue", alpha=0.15, lw=0.6)
    e = to_kiwi_rate(envelope_synth("dl1ycf"))
    sos = signal.butter(4, 400.0 / (fs / 2), output="sos")
    el = signal.sosfiltfilt(sos, e)
    i50 = np.argmax(el > 0.5)
    ax.plot((np.arange(len(el)) - i50) / fs * 1000, el, "k--", lw=1.2, label="ideal DL1YCF 9 ms ramp (through the same 400 Hz filter)")
    ax.set_xlim(-15, 15); ax.set_xlabel("ms from the 50 % point"); ax.set_ylabel("envelope / plateau")
    ax.legend(); ax.grid(alpha=0.3); ax.set_title(f"{len(env_on[::6])} measured onsets")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "plots", "onset_shape.png"), dpi=110); plt.close(fig)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Weak-signal behaviour from strong recordings: add noise, decode again.

Question: does driving the ALC cost anything when the signal is weak, and where is the best transmit level?
On the air the received power cannot be lowered without also changing the ALC state (the digital level is the
only knob), so it is done afterwards, on the Kiwi's fixed-gain recording of each run. A path loss of A dB is
emulated by

      y = x / 10^(A/20)  +  n',      n' white complex Gaussian, variance chosen so that the noise in the modem band
                                     is what the Kiwi measured when nothing was being sent (N0 from the quiet parts
                                     of the same recording),

so every level is judged against the *same* noise floor, with its own received power: the signal at level L
arrives with band SNR(L) - A dB. It is then decoded exactly as the real recordings are (kiwi_rx.analyze_one),
with several noise realisations (--seeds). To check the method, compare a real run at a low level (no ALC, its
own band SNR) with an emulated one from a higher unclipped level attenuated to about the same SNR: the
rows should agree (a few real weak runs are part of the plan for this reason).

  air_weak.py SESSION --loss 0,6,12,18,24,30 [--seeds 3] [--waves v1,v2]
Writes SESSION/weak.csv and weak.md (a table per wave and loss: the decode figures against the transmit level).
"""
import argparse, csv, glob, json, os, re, sys, types
import numpy as np
from scipy.io import wavfile
from scipy.signal import spectrogram

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kiwi_rx

FS = kiwi_rx.FS_KIWI
BW = {"v1": 1600.0, "v2": 1000.0}


def load_iq(path):
    fs, x = wavfile.read(path)
    x = x.astype(np.float64) / 32768.0
    return x[:, 0] + 1j * x[:, 1], fs


def band_levels(z, over, wave, inv, fs=FS):
    """Band power (full scale = 1) in the modem band while the over is on, and in the quiet parts: -> dict."""
    fc, bw = kiwi_rx.FC[wave], BW[wave]
    lo, hi = (-(fc + bw / 2), -(fc - bw / 2)) if inv else (fc - bw / 2, fc + bw / 2)
    f, t, S = spectrogram(z, fs=fs, nperseg=512, noverlap=0, return_onesided=False, scaling="density", mode="psd")
    f = np.fft.fftshift(f); S = np.fft.fftshift(S, axes=0)
    m = (f >= lo) & (f <= hi)
    df = f[1] - f[0]
    p = S[m].sum(axis=0) * df                       # band power per time slice
    s, e = over["start_s"], over["end_s"]
    on = p[(t > s + 1.0) & (t < e - 1.0)]
    off = p[(t < s - 2.0) | (t > e + 3.0)]
    if len(on) < 4 or len(off) < 4:
        return None
    pon, poff = float(np.mean(on)), float(np.mean(off))
    sig = max(pon - poff, 1e-18)
    return {"sig_dbfs": 10 * np.log10(sig), "noise_dbfs": 10 * np.log10(poff), "band_snr_db": 10 * np.log10(sig / poff),
            "n0": poff / bw}                         # noise PSD in the band, per Hz


def over_of(sess, profile):
    a = json.load(open(os.path.join(sess, "analysis.json")))
    for r in a:
        if r["profile"] == profile and r["overs"]:
            return max(r["overs"], key=lambda o: o["duration_s"])
    return None


def emulate(z, n0, loss_db, rng, fs=FS):
    g = 10 ** (-loss_db / 20.0)
    var = n0 * fs * max(1.0 - g * g, 0.0)
    n = np.sqrt(var / 2) * (rng.standard_normal(len(z)) + 1j * rng.standard_normal(len(z)))
    return z * g + n


def write_wav(path, z):
    k = 0.3 / (np.max(np.abs(np.concatenate([z.real, z.imag]))) + 1e-12)
    x = np.stack([z.real, z.imag], 1) * k
    wavfile.write(path, int(round(FS)) - 1, (x * 32767).astype("<i2"))       # the Kiwi's own header says 11999


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--loss", default="0,6,12,18,24,30", help="emulated path loss, dB")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--waves", default="v1,v2")
    ap.add_argument("--call", default="EI6IZ")
    a = ap.parse_args()
    rows = json.load(open(os.path.join(a.session, "results.json")))
    losses = [float(v) for v in a.loss.split(",")]
    waves = [w.strip() for w in a.waves.split(",")]
    out = []
    ns = types.SimpleNamespace(try_other=False, call=a.call, ref=None)
    refs_cache = {}
    for r in rows:
        w = r["wave"]
        if w not in waves or not r.get("kiwi_session"):
            continue
        sess = r["kiwi_session"]
        fixed = os.path.join(sess, "iq-fixed.wav")
        over = over_of(sess, "iq-fixed")
        if not (os.path.exists(fixed) and over):
            print("%s: no iq-fixed over, skipped" % r["run"])
            continue
        meta = json.load(open(os.path.join(sess, "session.json")))
        inv = bool(meta["expected_invert"])
        z, fs = load_iq(fixed)
        lv = band_levels(z, over, w, inv, fs)
        if lv is None:
            print("%s: not enough quiet recording to measure the noise, skipped" % r["run"])
            continue
        if w not in refs_cache:
            ref16 = os.path.join(os.path.dirname(os.path.dirname(sess)), "speech16k.wav")
            refs_cache[w] = kiwi_rx.reference(ref16, w == "v2", os.path.join(os.path.dirname(os.path.dirname(sess)), "ref_" + w))
        emu = os.path.join(sess, "emu")
        os.makedirs(emu, exist_ok=True)
        print("%s  band SNR %.1f dB (signal %.1f, noise %.1f dBFS)" % (r["run"], lv["band_snr_db"], lv["sig_dbfs"], lv["noise_dbfs"]))
        for A in losses:
            for sd in range(a.seeds):
                y = emulate(z, lv["n0"], A, np.random.default_rng(1000 * sd + int(A)), fs)
                p = os.path.join(emu, "loss%g_s%d.wav" % (A, sd))
                write_wav(p, y)
                try:
                    res = kiwi_rx.analyze_one(p, "iq-fixed", meta, ns, refs_cache[w])
                except SystemExit as e:
                    res = {"overs": []}
                o = max(res["overs"], key=lambda x: x["duration_s"]) if res["overs"] else None
                out.append({"run": r["run"], "wave": w, "level_db": r["level_db"], "loss_db": A, "seed": sd,
                            "band_snr_db": round(lv["band_snr_db"], 1), "emulated_snr_db": round(lv["band_snr_db"] - A, 1),
                            "sig_dbfs": round(lv["sig_dbfs"], 1), "noise_dbfs": round(lv["noise_dbfs"], 1),
                            "sync_pct": o["sync_pct"] if o else 0.0, "snr_est": o["snr_mean_db"] if o else None,
                            "eoo_ok": o["eoo_ok"] if o else False, "call_ok": o["call_ok"] if o else False,
                            "feature_loss": o.get("feature_loss") if o else None})
                for f in glob.glob(os.path.join(emu, "loss%g_s%d.*" % (A, sd))):
                    if not f.endswith(".wav") or f.endswith(".speech.wav"):
                        os.remove(f)
    cols = list(out[0].keys()) if out else []
    with open(os.path.join(a.session, "weak.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=cols)
        wr.writeheader(); wr.writerows(out)
    # a table per wave and loss
    md = ["# Emulated weak signal, by transmit level\n"]
    for w in waves:
        for A in losses:
            sel = [x for x in out if x["wave"] == w and x["loss_db"] == A]
            if not sel:
                continue
            md.append("\n## %s, path loss %g dB\n\n| level dB | band SNR now | sync %% | call ok | EOO ok | feature loss |\n|---|---|---|---|---|---|" % (w.upper(), A))
            for lvl in sorted({x["level_db"] for x in sel}, key=float):
                g = [x for x in sel if x["level_db"] == lvl]
                fl = [x["feature_loss"] for x in g if x["feature_loss"] is not None]
                md.append("| %+g | %.1f | %.0f | %d/%d | %d/%d | %s |" % (
                    float(lvl), g[0]["emulated_snr_db"], np.mean([x["sync_pct"] for x in g]),
                    sum(1 for x in g if x["call_ok"]), len(g), sum(1 for x in g if x["eoo_ok"]), len(g),
                    "%.4f" % np.mean(fl) if fl else "-"))
    open(os.path.join(a.session, "weak.md"), "w").write("\n".join(md) + "\n")
    print("wrote", os.path.join(a.session, "weak.csv"), "and weak.md")


if __name__ == "__main__":
    main()

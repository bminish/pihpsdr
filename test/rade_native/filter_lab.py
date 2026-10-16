#!/usr/bin/env python3
"""Filter lab: how WDSP's TX and RX filters treat a RADE signal, with every DSP option, no radio needed.

The options (all in WDSP, set per channel the way piHPSDR does):
  nc   filter length in taps (piHPSDR: 2048, TX at the 96 kHz DSP rate on P2, RX at 48 kHz)
  win  band-pass window: 0 four-term, 1 seven-term Blackman-Harris (piHPSDR: 1, always)
  mp   0 linear phase (piHPSDR: always), 1 minimum phase = low latency
and the band edges (low, high), which piHPSDR takes from rade_native_wdsp_edges().

Tests (each channel on its own, then the pair with noise):
  response   white noise through the channel: gain against frequency (at the V1/V2 carrier edges and the skirts),
             group delay across the band, and the pure delay of the channel
  decode     the modem signal (real speech through rade_tx, as the engine makes it) through TX, a calibrated noise,
             RX, and librade: feature error against what was sent, sync, and the SNR at which the feature error
             crosses a fixed value (the sensitivity of the pair)

  filter_lab.py response --wave v2 [--txedges 1000,2000] ...
  filter_lab.py decode   --wave v2 --configs "tx:2048:1:0:1000:2000,rx:2048:1:0:1000:2000;..."

A config is "nc:win:mp:low:high". Files go to out/lab/. Uses wdsp_probe (--nc --mp --win) and rade_rx_iq.
"""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np
from scipy.signal import resample_poly, welch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import t11b_chain as T

ROOT = T.ROOT
OUT = os.path.join(HERE, "out", "lab")
os.makedirs(OUT, exist_ok=True)
B = os.path.join(ROOT, "third_party", "rade_c-build", "src")
CLIPS = ["david_vk5dgr", "brian_g8sez", "k0pfx_mel", "jh0pcf_kanda"]       # clean speech from rade_c, joined
WAVDIR = os.path.join(ROOT, "third_party", "rade_c-src", "wav")
CARR = {"v1": (750.0, 2200.0, 1475.0), "v2": (1062.5, 1937.5, 1500.0)}     # first and last carrier, centre
DEFAULT_EDGES = {"v1": (675.0, 2275.0), "v2": (1000.0, 2000.0)}
P2 = ["--dsp", "96000", "--out", "192000", "--cfir"]


class Cfg:
    def __init__(self, nc=2048, win=1, mp=0, lo=None, hi=None, nbpwin=0):
        self.nc, self.win, self.mp, self.lo, self.hi, self.nbpwin = nc, win, mp, lo, hi, nbpwin   # nbpwin: RX main filter (WDSP's default 0)

    def args(self):
        return ["--nc", str(self.nc), "--win", str(self.win), "--mp", str(self.mp)]

    def __str__(self):
        return "nc%d/%s/%s" % (self.nc, ["4t", "7t"][self.win], ["lin", "min"][self.mp]) + ("" if self.nbpwin == 0 else "/nbp7t") + (
            " %g..%g" % (self.lo, self.hi) if self.lo is not None else "")


def parse_cfg(s, wave):
    p = s.split(":")
    lo, hi = DEFAULT_EDGES[wave]
    return Cfg(int(p[0]), int(p[1]), int(p[2]), float(p[3]) if len(p) > 3 else lo, float(p[4]) if len(p) > 4 else hi)


def tx_probe(x48, c, p2=True):
    """x48: complex at 48 kHz in WDSP's sign convention -> TX output decimated back to 48 kHz."""
    a = ["--low", str(c.lo), "--high", str(c.hi), "--neutral", "--alc", "0"] + (P2 if p2 else []) + c.args()
    y = T.probe("tx", x48.astype(np.complex64), a)
    return resample_poly(y, 1, 4) if p2 else y


def rx_probe(x48, c, nbpwin=None):
    a = ["--low", str(c.lo), "--high", str(c.hi), "--copy", "0", "--agc", "0", "--fixed", "0"] + c.args() + [
        "--nbpwin", str(c.nbpwin if nbpwin is None else nbpwin)]
    return T.probe("rx", x48.astype(np.complex64), a)


def noise(n, rng):
    return (rng.standard_normal(n) + 1j * rng.standard_normal(n)) / np.sqrt(2)


# ---------------------------------------------------------------- response

COMB = np.arange(700.0, 2401.0, 2.5)        # Hz in the modem frame, 2.5 Hz apart: group delay unambiguous to 200 ms


def comb_response(fn, seed=1):
    """A comb of tones (random phases) through fn at WDSP's sign (negative frequency), read back by DFT after the filter
    has settled: gain (dB) and phase per tone. No estimation noise; the delay comes from the phase slope."""
    rng = np.random.default_rng(seed)
    n = 48000 * 5
    t = np.arange(n) / 48000.0
    ph = rng.uniform(0, 2 * np.pi, len(COMB))
    x = np.zeros(n, dtype=complex)
    for f, p in zip(COMB, ph):
        x += np.exp(1j * (-2 * np.pi * f * t + p))
    x *= 0.002
    y = fn(x)[:n]
    seg = slice(n - 96000, n)                  # the last two seconds: bins are 0.5 Hz, every tone (2.5 Hz apart) on one
    X = np.fft.fft(x[seg]); Y = np.fft.fft(y[seg])
    idx = np.round(-COMB * 2).astype(int) % 96000
    h = Y[idx] / X[idx]
    return 20 * np.log10(np.abs(h) + 1e-12), np.unwrap(np.angle(h))


def xcorr_delay_ms(fn, seed=2):
    """The channel's delay: where the output best matches the input (white noise), in ms."""
    rng = np.random.default_rng(seed)
    n = 48000 * 3
    x = noise(n, rng) * 0.05
    y = fn(x)[:n]
    c = np.fft.ifft(np.fft.fft(y, 2 * n) * np.conj(np.fft.fft(x, 2 * n)))
    lag = int(np.argmax(np.abs(c[:48000])))
    return lag / 48.0


def gd_ms(phase, lo, hi):
    sel = (COMB >= lo) & (COMB <= hi)
    w = -2 * np.pi * COMB[sel]                 # the tones are at negative frequency
    return float(-np.polyfit(w, phase[sel], 1)[0] * 1000.0)


def at(mag, pts, half=2.5):
    return [float(np.mean(mag[(COMB >= p - half) & (COMB <= p + half)])) for p in pts]


def do_response(a):
    c0, c1, ctr = CARR[a.wave]
    pts = [c0 - 250, c0 - 150, c0 - 100, c0 - 50, c0, c0 + 50, c0 + 150, ctr, c1 - 150, c1 - 50, c1, c1 + 50, c1 + 100, c1 + 150, c1 + 250]
    rows = []
    for kind in a.kinds:
        for nc in a.nc:
            for win in a.win:
                for mp in a.mp:
                    lo, hi = DEFAULT_EDGES[a.wave]
                    if kind == "tx" and a.txedges:
                        lo, hi = a.txedges
                    if kind == "rx" and a.rxedges:
                        lo, hi = a.rxedges
                    cfg = Cfg(nc, win, mp, lo, hi, nbpwin=(a.nbpwin if kind == 'rx' else 0))
                    if kind == "tx":
                        fn = lambda x, c=cfg: tx_probe(x, c)
                    else:
                        fn = lambda x, c=cfg: rx_probe(x, c)
                    try:
                        mag, ph = comb_response(fn)
                    except subprocess.CalledProcessError:
                        print('%s %-30s WDSP crashed with this setting (skipped)' % (kind, cfg), flush=True)
                        continue
                    ref = float(np.mean(at(mag, [ctr - 50, ctr, ctr + 50])))
                    g = [v - ref for v in at(mag, pts)]
                    r = dict(kind=kind, cfg=str(cfg), pts=[round(v, 1) for v in g], ref_db=round(ref, 1),
                             delay_ms=round(xcorr_delay_ms(fn), 1),
                             gd_spread_ms=round(float(np.ptp([gd_ms(ph, p - 20, p + 20) for p in (c0, ctr, c1)])), 2))
                    rows.append(r)
                    print("%s %-30s delay %6.1f ms (group delay spread across the carriers %.2f ms)  %s" % (
                        kind, cfg, r["delay_ms"], r["gd_spread_ms"], " ".join("%6.1f" % v for v in r["pts"])), flush=True)
    print("points, Hz in the modem's frame:        %s" % " ".join("%6.0f" % p for p in pts))
    json.dump({"pts": pts, "rows": rows}, open(os.path.join(OUT, "response_%s.json" % a.wave), "w"), indent=1)


# ------------------------------------------------------------------ decode

def modem(wave, seconds):
    """Real speech through rade_tx as the engine runs it (V2 with its own TX BPF on, as the engine has it)."""
    v = wave
    wav, feat, iq = [os.path.join(OUT, "m_%s.%s" % (v, e)) for e in ("wav", "feat", "iq.f32")]
    if not os.path.exists(iq):
        from scipy.io import wavfile
        parts = []
        for c in CLIPS:
            fs, x = wavfile.read(os.path.join(WAVDIR, c + ".wav"))
            x = x.astype(np.float64) / (32768.0 if x.dtype == np.int16 else 1.0)
            if x.ndim > 1:
                x = x[:, 0]
            parts.append(resample_poly(x, 16000, fs) if fs != 16000 else x)
        x = np.concatenate(parts)[: int(seconds * 16000)]
        wavfile.write(wav, 16000, (np.clip(x, -1, 1) * 32767).astype(np.int16))
        subprocess.run([os.path.join(B, "rade_tx_wav"), "-v", "0"] + (["--v2"] if v == "v2" else []) + ["-f", feat, wav,
                        os.path.join(OUT, "m_%s.real.wav" % v)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([os.path.join(HERE, "rade_tx_iq")] + (["--v2"] if v == "v2" else []) + [feat, iq], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = np.fromfile(iq, dtype=np.float32)
    return d[0::2] + 1j * d[1::2], np.fromfile(feat, dtype=np.float32).reshape(-1, 36)


def band_power(y, f0, f1):
    f, p = welch(y, fs=48000, nperseg=4096, return_onesided=False)
    f = np.fft.fftshift(f); p = np.fft.fftshift(p)
    m = (f >= f0) & (f <= f1)
    return float(np.sum(p[m]) * (f[1] - f[0]))


def decode_pair(wave, txc, rxc, snrs, seeds, seconds, x8, txf, p2=True):
    x48 = np.conj(resample_poly(x8, 6, 1)) * 0.3
    y = tx_probe(x48, txc, p2)
    act = np.abs(y) > 1e-4
    sig = float(np.mean(np.abs(y[act]) ** 2))                    # signal power while sending
    out = {}
    for snr in snrs:
        n0 = sig / (10 ** (snr / 10) * 3000.0) * 48000.0 / 48000.0   # per-Hz density: noise power in 3 kHz = sig / snr
        errs, syncs, snrs_est = [], [], []
        for sd in range(seeds):
            rng = np.random.default_rng(100 * sd + int(snr * 10) + 1007)
            yn = y + np.sqrt(n0 * 48000.0) * noise(len(y), rng)
            z = rx_probe(yn, rxc)
            x8b = np.conj(resample_poly(z, 1, 6))
            x8b = x8b / (np.sqrt(np.mean(np.abs(x8b[len(x8b) // 2:]) ** 2)) + 1e-30) * 0.9
            d, s, n = T.decode(x8b.astype(np.complex64), wave == "v2", "lab_dec")
            e = T.feature_error(txf, d, 25)
            errs.append(e[0] if e else 1.0); syncs.append(s); snrs_est.append(n)
        out[snr] = (float(np.mean(errs)), float(np.mean(syncs)), float(np.nanmean(snrs_est)))
    return out


def threshold(res, target=0.05, idx=0):
    """SNR at which the feature error (idx 0) falls through `target`, or sync (idx 1, in %) rises through it
    (interpolated between the SNRs tried), or None."""
    s = sorted(res)
    for a_, b_ in zip(s, s[1:]):
        va, vb = res[a_][idx], res[b_][idx]
        if idx == 0 and va >= target >= vb or idx == 1 and va <= target <= vb:
            return a_ + (b_ - a_) * (va - target) / (va - vb if va != vb else 1e-9)
    ok = res[s[0]][idx] < target if idx == 0 else res[s[0]][idx] > target
    return s[0] if ok else None


def do_decode(a):
    x8, txf = modem(a.wave, a.seconds)
    snrs = [float(v) for v in a.snrs.split(",")]
    rows = []
    for spec in a.configs.split(";"):
        tx_s, rx_s = spec.split(",")
        txc, rxc = parse_cfg(tx_s.split("tx:")[1], a.wave), parse_cfg(rx_s.split("rx:")[1], a.wave)
        res = decode_pair(a.wave, txc, rxc, snrs, a.seeds, a.seconds, x8, txf)
        th, ts = threshold(res), threshold(res, 90.0, 1)
        rows.append(dict(tx=str(txc), rx=str(rxc), res={str(k): v for k, v in res.items()}, thr_loss=th, thr_sync90=ts))
        f = lambda v: ("%.2f" % v) if v is not None else "n/a"
        print("TX %-26s RX %-26s  loss<0.05 at %s dB, sync>90%% at %s dB   %s" % (txc, rxc, f(th), f(ts),
              "  ".join("%g:%.3f/%.0f%%" % (k, v[0], v[1]) for k, v in sorted(res.items()))), flush=True)
    json.dump(rows, open(os.path.join(OUT, a.out or ("decode_%s.json" % a.wave)), "w"), indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("response")
    r.add_argument("--wave", default="v2")
    r.add_argument("--kinds", type=lambda s: s.split(","), default=["tx", "rx"])
    r.add_argument("--nc", type=lambda s: [int(v) for v in s.split(",")], default=[2048])
    r.add_argument("--win", type=lambda s: [int(v) for v in s.split(",")], default=[1])
    r.add_argument("--mp", type=lambda s: [int(v) for v in s.split(",")], default=[0])
    r.add_argument("--txedges", type=lambda s: [float(v) for v in s.split(",")])
    r.add_argument("--nbpwin", type=int, default=0, help="RX: window of the main (nbp0) filter")
    r.add_argument("--rxedges", type=lambda s: [float(v) for v in s.split(",")])
    d = sub.add_parser("decode")
    d.add_argument("--wave", default="v2")
    d.add_argument("--configs", required=True)
    d.add_argument("--snrs", default="2,5,8,11,14")
    d.add_argument("--seeds", type=int, default=2)
    d.add_argument("--seconds", type=float, default=30.0)
    d.add_argument("--out")
    a = ap.parse_args()
    {"response": do_response, "decode": do_decode}[a.cmd](a)


if __name__ == "__main__":
    main()

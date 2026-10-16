#!/usr/bin/env python3
"""kiwi_rx.py - record our RADE transmissions off air from a KiwiSDR and check them.

The receive side of the test harness. The transmit side is the RADE window in
piHPSDR (V1 or V2, with a callsign set); this script listens, in several ways
at once, and says whether the over arrived properly.

  kiwi_rx.py record  --freq 7053.0 --wave v1 --duration 40
  kiwi_rx.py analyze rade-captures/kiwi/<session>   [--ref speech.wav] [--call EI6IZ]
  kiwi_rx.py run     --freq 7053.0 --wave v1 --duration 40 --ref speech.wav --call EI6IZ
  kiwi_rx.py selftest                               (no Kiwi: a synthetic over, to check the analysis)

record starts one kiwirecorder.py (from the kiwiclient checkout) per profile,
all at the same moment, each on its own connection to the Kiwi:

  iq        IQ mode, 12 kHz complex (never compressed), Kiwi's own AGC
  iq-fixed  IQ mode with the AGC off and a fixed gain (--gain)
  usb, lsb  audio mode, uncompressed (--ncomp), 12 kHz mono

IQ is the primary path: it is the same complex baseband the transmitter and
the native receiver work in. The audio profiles are there to see what the
usual way of listening does to the signal. V1 below 10 MHz is sent inverted
(lower sideband, see docs/rade-native.md), so the default set is iq, iq-fixed
and lsb there, and iq, iq-fixed and usb elsewhere; --profiles overrides it.

analyze converts each recording to 8 kHz complex baseband, mixes out any
difference between the Kiwi's tuning and the transmitter's dial (--tx-freq),
undoes the inversion, and runs librade over it (test/rade_native/
rade_decode_iq). Per over it reports

  - how much of the over was in sync, and the SNR (librade's, dB in 3 kHz) and
    frequency offset while it was
  - the end of over: was an EOO frame flagged where the over ended, and for V1
    does the callsign in it decode and match --call
  - with --ref (the speech that was sent, 16 kHz wav): the vocoder feature
    loss against what rade_tx would have sent for that speech, and how well
    the decoded speech follows the reference decode of the same speech

PASS means: an over was found, at least 80 % of it in sync, one EOO at its
end, and the call matched if one was given. SNR and loss are reported;
--min-snr and --max-loss make them part of PASS.

Everything goes in rade-captures/kiwi/<utc>_<kHz>_<wave>/ (git-ignored):
<profile>.wav (as recorded), .8k.f32, .stats.csv, .events.txt, .feat.f32,
.speech.wav (decoded), session.json, analysis.json and report.txt.
"""
import argparse, datetime, json, os, shutil, subprocess, sys, time, urllib.request
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, hilbert, correlate

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DECODER = os.path.join(HERE, "rade_decode_iq")
RADE_BIN = os.path.join(ROOT, "third_party/rade_c-build/src")
OUTDIR = os.path.join(ROOT, "rade-captures", "kiwi")
import kiwi_gps_wav as KG
FS_KIWI = 12000.0          # nominal; the wav header says 11999
FC = {"v1": 1475.0, "v2": 1500.0}


# ------------------------------------------------------------------ geometry

def expected_invert(wave, freq_khz):
    """V1 is sent as lower sideband below 10 MHz, V2 never, and on 60 m (5250-5450 kHz) neither: USB (see rade_native.h)."""
    return wave == "v1" and freq_khz < 10000.0 and not (5250.0 <= freq_khz <= 5450.0)


def default_profiles(wave, freq_khz):
    return ["iq", "iq-fixed", "lsb" if expected_invert(wave, freq_khz) else "usb"]


def profile_args(name, gain, gps=False):
    """gps: the IQ profiles are written as --kiwi-wav files, every block stamped with GNSS time (IQ mode only)."""
    if name == "iq":
        return ["-m", "iq"] + (["-w"] if gps else [])
    if name == "iq-fixed":
        return ["-m", "iq", "-g", str(gain)] + (["-w"] if gps else [])
    if name in ("usb", "lsb"):
        return ["-m", name, "--ncomp"]
    raise SystemExit("unknown profile " + name)


# ------------------------------------------------------------------- record

def kiwi_status(host, port):
    try:
        with urllib.request.urlopen("http://%s:%d/status" % (host, port), timeout=8) as r:
            txt = r.read().decode("utf-8", "replace")
        return dict(l.split("=", 1) for l in txt.splitlines() if "=" in l)
    except Exception as e:
        return {"error": str(e)}


def record(a):
    wave = a.wave.lower()
    profiles = a.profiles.split(",") if a.profiles else default_profiles(wave, a.freq)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    sess = os.path.join(a.out, "%s_%skHz_%s" % (stamp, ("%g" % a.freq).replace(".", "p"), wave.upper()))
    os.makedirs(sess, exist_ok=True)
    rec = os.path.join(os.path.expanduser(a.kiwiclient), "kiwirecorder.py")
    if not os.path.exists(rec):
        raise SystemExit("kiwirecorder.py not found in " + a.kiwiclient)
    status = kiwi_status(a.host, a.port)
    print("Kiwi %s:%d  %s  (users %s/%s)" % (a.host, a.port, status.get("name", "?"),
                                              status.get("users", "?"), status.get("users_max", "?")))
    gps = a.gps == "on" or (a.gps == "auto" and gps_ok(status))
    if a.gps == "on" and not gps_ok(status):
        print("warning: --gps on, but the Kiwi reports no GPS solution (gps_good=%s fixes=%s); the stamps may be invalid" % (
            status.get("gps_good"), status.get("fixes")))
    print("GPS time stamps: %s (gps_good=%s, fixes=%s)" % ("on" if gps else "off", status.get("gps_good"), status.get("fixes")))
    procs = {}
    launched = {}
    t0 = datetime.datetime.now(datetime.timezone.utc)
    for p in profiles:
        cmd = [sys.executable, rec, "-s", a.host, "-p", str(a.port), "-f", "%g" % a.freq,
               "--tlimit", str(a.duration), "-d", sess, "--fn", p, "-u", "radetest-" + p,
               "-q", "--log-level", "warn", "--connect-retries", "3", "--busy-retries", "3",
               "--busy-timeout", "5"] + profile_args(p, a.gain, gps)
        if a.password:
            cmd += ["--pw", a.password]
        launched[p] = time.time()
        procs[p] = subprocess.Popen(cmd, cwd=os.path.dirname(rec), stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
        time.sleep(a.launch_delay)
    print("recording %d s on %s kHz: %s" % (a.duration, "%g" % a.freq, ", ".join(profiles)))
    results = {}
    for p, pr in procs.items():
        try:
            out, _ = pr.communicate(timeout=a.duration + 90)
        except subprocess.TimeoutExpired:
            pr.kill()
            out = "(timed out)"
        f = os.path.join(sess, p + ".wav")
        results[p] = {"ok": os.path.exists(f) and os.path.getsize(f) > 1000, "rc": pr.returncode,
                      "log": (out or "").strip()[-400:]}
        print("  %-9s %s" % (p, "recorded %.1f MB" % (os.path.getsize(f) / 1e6) if results[p]["ok"]
                             else "FAILED: " + results[p]["log"][-120:]))
    junk = os.path.join(sess, "kiwi_noname.txt")
    if os.path.exists(junk):
        os.remove(junk)
    meta = {"host": a.host, "port": a.port, "freq_khz": a.freq,
            "tx_freq_khz": a.tx_freq if a.tx_freq is not None else a.freq,
            "wave": wave, "expected_invert": expected_invert(wave, a.tx_freq or a.freq),
            "profiles": profiles, "gain": a.gain, "duration_s": a.duration,
            "start_utc": t0.strftime("%Y-%m-%dT%H:%M:%SZ"), "kiwi_status": status,
            "results": results, "call": a.call, "gps": bool(gps), "start_unix": t0.timestamp(), "launched_unix": launched}
    json.dump(meta, open(os.path.join(sess, "session.json"), "w"), indent=1)
    return sess


# ------------------------------------------------------------------ analyse

def to_baseband(path, profile, shift_khz_tx_minus_kiwi, fc, ref_unix=None):
    """Recording -> 8 kHz complex baseband with the modem at +fc. Returns (z, t0_unix): t0_unix is the UTC time of z[0]
    for a GPS-stamped recording (the 8 kHz grid is then exactly on GPS time, resampled from the fitted rate), else None."""
    rec = None
    if ref_unix is not None and profile not in ("usb", "lsb"):
        try:
            rec = KG.read_kiwi_gps_wav(path, ref_unix)
        except ValueError:
            rec = None
    if rec is not None:
        z = rec.z.astype(np.complex128)
        fs = rec.fs
        delta = shift_khz_tx_minus_kiwi * 1000.0
    else:
        fs, x = wavfile.read(path)
        x = x.astype(np.float64) / 32768.0
        delta = shift_khz_tx_minus_kiwi * 1000.0
        if x.ndim == 2:
            z = x[:, 0] + 1j * x[:, 1]              # Kiwi IQ: positive = above the tuned frequency
        else:
            z = hilbert(x)                          # audio: the sideband is already chosen by the profile
            if profile == "lsb":
                delta = -delta                      # LSB audio runs the other way
        fs = FS_KIWI
    n = np.arange(len(z))
    if abs(delta) > 1e-6:
        # bring the transmitter's dial to the Kiwi's, at 12 kHz so the modem is not clipped by
        # the 8 kHz filter on its way
        z = z * np.exp(-2j * np.pi * delta * n / fs)
    z8 = resample_poly(z, 2, 3)                 # 12 kHz -> 8 kHz (at the nominal ratio)
    if rec is None:
        return z8, None
    # the Kiwi's true sample rate is fs (11998.94 or so), not 12000: put the 8 kHz stream on exact 1/8000 s steps of GPS time
    from scipy.interpolate import CubicSpline
    step = 1.5 / fs                               # true seconds between z8 samples
    m = int((len(z8) - 2) * step * 8000.0)
    k = np.arange(m) / 8000.0 / step              # fractional index into z8
    zr = CubicSpline(np.arange(len(z8)), z8.real)(k)
    zi = CubicSpline(np.arange(len(z8)), z8.imag)(k)
    return zr + 1j * zi, float(rec.t0_unix)


def level(z):
    """Scale so the loud part has rms 0.9, as the receive engine does."""
    blk = 8000
    nb = len(z) // blk
    if nb < 2:
        return z
    p = np.array([np.mean(np.abs(z[i * blk:(i + 1) * blk]) ** 2) for i in range(nb)])
    ref = np.percentile(p, 75)
    return z * (0.9 / np.sqrt(ref + 1e-18))


def run_decoder(z, v2, base):
    iq = base + ".8k.f32"
    np.stack([z.real, z.imag], 1).astype(np.float32).tofile(iq)
    cmd = [DECODER] + (["--v2"] if v2 else []) + [iq, base + ".stats.csv", base + ".events.txt",
                                                  base + ".feat.f32", base + ".speech.wav"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("rade_decode_iq failed: " + r.stderr[-300:])
    st = np.genfromtxt(base + ".stats.csv", delimiter=",", names=True)
    ev = []
    for l in open(base + ".events.txt"):
        p = l.split()
        if len(p) >= 3 and p[0] == "eoo":
            ev.append((float(p[1]), p[2]))
    return st, ev


def segments(st, merge_s=1.5, min_s=1.0):
    """Overs: runs of sync, gaps shorter than merge_s joined, runs shorter than min_s dropped."""
    t, s = st["t_sec"], st["sync"] > 0.5
    segs, i, n = [], 0, len(t)
    while i < n:
        if s[i]:
            j = i
            while j + 1 < n and s[j + 1]:
                j += 1
            segs.append([i, j])
            i = j + 1
        else:
            i += 1
    merged = []
    for a, b in segs:
        if merged and t[a] - t[merged[-1][1]] < merge_s:
            merged[-1][1] = b
        else:
            merged.append([a, b])
    return [(a, b) for a, b in merged if t[b] - t[a] >= min_s]


def feature_loss(ref, rx, trim=25):
    """Normalised error over the 20 used features, best alignment (as radae's loss.py in spirit)."""
    ref, rx = ref.reshape(-1, 36)[:, :20], rx.reshape(-1, 36)[:, :20]
    if len(rx) < 2 * trim + 50 or len(ref) < 50:
        return None
    a0, b0 = ref - ref.mean(0), rx - rx.mean(0)
    n = len(a0) + len(b0)
    score = sum(correlate(b0[:, c], a0[:, c], mode="full", method="fft") for c in range(20))
    lag = int(np.argmax(score)) - (len(a0) - 1)       # rx is ref delayed by lag frames
    r1 = ref[max(0, -lag):]
    x1 = rx[max(0, lag):]
    m = min(len(r1), len(x1)) - 2 * trim
    if m < 50:
        return None
    r1, x1 = r1[trim:trim + m], x1[trim:trim + m]
    return float(np.mean((r1 - x1) ** 2) / np.mean(r1 ** 2)), lag


def feature_profile(ref, rx):
    """How the decoded features line up with what was sent, with nothing trimmed, so the start and the end of the over show.

    rx[k] corresponds to ref[k - lag]. lag >= 0: the decoder's first frame is at or before the first speech frame (nothing of
    the start of the speech is lost); lag < 0: the first -lag frames (10 ms each) of the speech were not decoded. The same for the
    end: frames of the speech after the decoder's last frame. Normalised errors (re the mean square of ref) over the first
    second, the middle and the last second of the aligned stretch.
    """
    ref, rx = ref.reshape(-1, 36)[:, :20], rx.reshape(-1, 36)[:, :20]
    if len(rx) < 300 or len(ref) < 300:
        return None
    a0, b0 = ref - ref.mean(0), rx - rx.mean(0)
    score = sum(correlate(b0[:, c], a0[:, c], mode="full", method="fft") for c in range(20))
    lag = int(np.argmax(score)) - (len(a0) - 1)
    r1, x1 = ref[max(0, -lag):], rx[max(0, lag):]
    m = min(len(r1), len(x1))
    r1, x1 = r1[:m], x1[:m]
    p = np.mean(r1 ** 2)
    e = np.mean((r1 - x1) ** 2, axis=1) / p
    first_clipped = max(0, -lag)
    ref_frames_after = len(ref) - (max(0, -lag) + m)         # speech frames after the last decoded frame
    return {"rx_start_vs_speech_frames": lag,
            "speech_start_clipped_ms": 10 * first_clipped,
            "speech_end_missing_ms": 10 * max(0, ref_frames_after),
            "loss_first_1s": round(float(np.mean(e[:100])), 4), "loss_last_1s": round(float(np.mean(e[-100:])), 4),
            "loss_middle": round(float(np.mean(e[100:-100])), 4) if m > 300 else None,
            "loss_first_250ms": round(float(np.mean(e[:25])), 4)}


def env(x, n=160):
    k = len(x) // n
    return np.sqrt((x[:k * n].reshape(k, n) ** 2).mean(1))


def env_corr(a, b, trim=50):
    a, b = env(a), env(b)
    a0, b0 = a - a.mean(), b - b.mean()
    c = correlate(b0, a0, mode="full")
    lag = int(np.argmax(c)) - (len(a0) - 1)
    a1 = a[max(0, -lag):]
    b1 = b[max(0, lag):]
    m = min(len(a1), len(b1)) - 2 * trim
    if m < 50:
        return None
    return float(np.corrcoef(a1[trim:trim + m], b1[trim:trim + m])[0, 1])


def reference(ref_wav, v2, out):
    """What the transmitter would have sent for ref_wav: features, and the ideal decode of them."""
    opt = ["--v2"] if v2 else []
    feat, tx, dec = out + ".ref.feat", out + ".ref.wav", out + ".ref.dec.wav"
    subprocess.run([RADE_BIN + "/rade_tx_wav", "-v", "0"] + opt + ["-f", feat, ref_wav, tx],
                   check=True, capture_output=True)
    subprocess.run([RADE_BIN + "/rade_rx_wav", "-v", "0"] + opt + [tx, dec], check=True, capture_output=True)
    return np.fromfile(feat, dtype=np.float32), wavfile.read(dec)[1].astype(float)


def analyze_one(path, profile, meta, a, refs):
    wave = meta["wave"]
    v2 = wave == "v2"
    inv = meta["expected_invert"]
    base = os.path.splitext(path)[0]
    z, t0_unix = to_baseband(path, profile, meta["tx_freq_khz"] - meta["freq_khz"], FC[wave],
                             meta.get("start_unix") if meta.get("gps") else None)
    is_iq = not profile in ("usb", "lsb")
    # IQ: an inverted signal sits below the dial, so conjugate it. Audio profiles
    # already put the modem at positive frequency when the sideband is right.
    if is_iq and inv:
        z = np.conj(z)
    z = level(z)
    st, ev = run_decoder(z, v2, base)
    out = {"profile": profile, "file": os.path.basename(path), "seconds": len(z) / 8000.0, "overs": []}
    if a.try_other and not v2:
        zo = level(np.conj(z) if is_iq else z)
        so, _ = run_decoder(zo, v2, base + ".other")
        out["sync_pct_other_orientation"] = round(100.0 * float(np.mean(so["sync"] > 0.5)), 1)
    segs = segments(st)
    nframes_cum = np.concatenate([[0], np.cumsum((st["nout"] / 36).astype(int))])
    for k, (i, j) in enumerate(segs):
        t0, t1 = st["t_sec"][i], st["t_sec"][j]
        sel = slice(i, j + 1)
        sync = st["sync"][sel] > 0.5
        snr = st["snr_db"][sel][sync & (st["t_sec"][sel] > t0 + 1.0)]   # the estimate starts at 0
        fo = st["foff_hz"][sel][sync]
        # the sync flag falls when the EOO has been received, a frame or so after it: look from just
        # inside the over to 2 s past its end
        eoos = [(t, c) for t, c in ev if t0 + 1.0 <= t <= t1 + 2.0]
        o = {"n": k + 1, "start_s": round(float(t0), 2), "end_s": round(float(t1), 2),
             "duration_s": round(float(t1 - t0), 2), "sync_pct": round(100.0 * float(sync.mean()), 1),
             "snr_mean_db": round(float(snr.mean()), 1) if len(snr) else None,
             "snr_min_db": round(float(snr.min()), 1) if len(snr) else None,
             "snr_max_db": round(float(snr.max()), 1) if len(snr) else None,
             "foff_mean_hz": round(float(fo.mean()), 1) if len(fo) else None,
             "foff_sd_hz": round(float(fo.std()), 1) if len(fo) else None,
             "eoo_count": len(eoos), "eoo_at_s": round(eoos[0][0], 2) if eoos else None,
             "eoo_after_end_s": round(eoos[0][0] - float(t1), 2) if eoos else None,
             "call": eoos[0][1] if eoos else None}
        if t0_unix is not None:                        # GPS-stamped recording: the decoder's times as UTC (end of the block it was in)
            o["start_unix"] = round(t0_unix + float(t0), 4)
            o["end_unix"] = round(t0_unix + float(t1), 4)
            o["eoo_at_unix"] = round(t0_unix + eoos[0][0], 4) if eoos else None
        o["eoo_ok"] = len(eoos) == 1
        want = a.call or meta.get("call")
        o["call_ok"] = None if (v2 or not want) else (len(eoos) >= 1 and eoos[0][1] == want.upper())
        # quality against what was sent
        if refs is not None:
            rf, rdec = refs
            f0, f1 = int(nframes_cum[i]), int(nframes_cum[j + 1])
            rx_feat = np.fromfile(base + ".feat.f32", dtype=np.float32).reshape(-1, 36)[f0:f1]
            fl = feature_loss(rf, rx_feat)
            o["feature_loss"] = round(fl[0], 4) if fl else None
            pr = feature_profile(rf, rx_feat)
            if pr:
                o.update(pr)
            sp = wavfile.read(base + ".speech.wav")[1].astype(float)
            # speech starts after the 5 frame warm-up of the first over only
            s0 = max(0, (f0 - 5) * 160) if k else 0
            s1 = max(s0, (f1 - 5) * 160)
            ec = env_corr(rdec, sp[s0:s1]) if s1 - s0 > 8000 else None
            o["speech_env_corr_vs_ideal"] = round(ec, 3) if ec is not None else None
        out["overs"].append(o)
    out["sync_pct_total"] = round(100.0 * float(np.mean(st["sync"] > 0.5)), 1)
    out["eoo_events"] = [(round(t, 2), c) for t, c in ev]
    return out


def verdict(res, a):
    ok = bool(res["overs"])
    why = [] if ok else ["no over found"]
    for o in res["overs"]:
        if o["sync_pct"] < 80:
            ok = False; why.append("over %d only %.0f%% in sync" % (o["n"], o["sync_pct"]))
        if not o["eoo_ok"]:
            ok = False; why.append("over %d: %d EOO frames at its end" % (o["n"], o["eoo_count"]))
        if o["call_ok"] is False:
            ok = False; why.append("over %d: call %s" % (o["n"], o["call"]))
        if a.min_snr is not None and (o["snr_mean_db"] is None or o["snr_mean_db"] < a.min_snr):
            ok = False; why.append("over %d: SNR %s < %s" % (o["n"], o["snr_mean_db"], a.min_snr))
        if a.max_loss is not None and (o.get("feature_loss") is None or o["feature_loss"] > a.max_loss):
            ok = False; why.append("over %d: feature loss %s > %s" % (o["n"], o.get("feature_loss"), a.max_loss))
    return ok, why


def gps_ok(status):
    """The Kiwi has a GPS solution now: it reports satellites in the solution (gps_good) and fixes."""
    try:
        return int(status.get("gps_good", "0")) > 0 and int(status.get("fixes", "0")) > 0
    except ValueError:
        return False


def report(sess, results, a):
    lines = []
    for r in results:
        lines.append("== %s (%s, %.0f s, sync %.0f%% of the file)" % (r["profile"], r["file"], r["seconds"], r["sync_pct_total"]))
        if "sync_pct_other_orientation" in r:
            lines.append("   other orientation: %.0f%% in sync (V1 only: sync tells which way round it is)" % r["sync_pct_other_orientation"])
        for o in r["overs"]:
            lines.append("   over %d  %.1f-%.1f s (%.1f s)  sync %.0f%%  SNR %s dB (min %s, max %s)  foff %s Hz (sd %s)" %
                         (o["n"], o["start_s"], o["end_s"], o["duration_s"], o["sync_pct"], o["snr_mean_db"],
                          o["snr_min_db"], o["snr_max_db"], o["foff_mean_hz"], o["foff_sd_hz"]))
            lines.append("           end of over: %d EOO, at %s s (%s s after sync ended), call %s%s" %
                         (o["eoo_count"], o["eoo_at_s"], o["eoo_after_end_s"], o["call"],
                          "" if o["call_ok"] is None else ("  [call OK]" if o["call_ok"] else "  [CALL WRONG]")))
            if "feature_loss" in o:
                lines.append("           quality: feature loss %s   speech envelope vs ideal decode %s" %
                             (o["feature_loss"], o["speech_env_corr_vs_ideal"]))
        ok, why = verdict(r, a)
        r["pass"], r["why"] = ok, why
        lines.append("   -> %s%s" % ("PASS" if ok else "FAIL", "" if ok else ": " + "; ".join(why)))
    txt = "\n".join(lines)
    open(os.path.join(sess, "report.txt"), "w").write(txt + "\n")
    json.dump(results, open(os.path.join(sess, "analysis.json"), "w"), indent=1)
    print(txt)


def analyze(a, sess=None):
    sess = sess or a.session
    meta = json.load(open(os.path.join(sess, "session.json")))
    if a.tx_freq is not None:
        meta["tx_freq_khz"] = a.tx_freq
    if a.invert is not None:
        meta["expected_invert"] = a.invert
    refs = None
    if a.ref:
        refs = reference(a.ref, meta["wave"] == "v2", os.path.join(sess, "ref"))
    results = []
    for p in meta["profiles"]:
        f = os.path.join(sess, p + ".wav")
        if os.path.exists(f):
            results.append(analyze_one(f, p, meta, a, refs))
    report(sess, results, a)
    return all(r["pass"] for r in results if r["profile"].startswith("iq")) if results else False


# ----------------------------------------------------------------- selftest

def selftest(a):
    """A synthetic over (our TX engine, with a callsign) dressed as a Kiwi recording, then analysed."""
    wave = a.wave.lower()
    v2 = wave == "v2"
    out = os.path.join(HERE, "out")
    os.makedirs(out, exist_ok=True)
    clip = os.path.join(ROOT, "third_party/rade_c-src/wav/brian_g8sez.wav")
    fs, x = wavfile.read(clip)
    x = x[:fs * 8].astype(float) / 32768
    mic = resample_poly(x, 3, 1)
    m48 = os.path.join(out, "kiwiself.mic48.f32")
    np.stack([mic, 0 * mic], 1).astype(np.float32).tofile(m48)
    txo = os.path.join(out, "kiwiself.tx.f32")
    subprocess.run([os.path.join(HERE, "test_tx_engine"), "--speed", "3", "--call", a.call or "EI6IZ"] +
                   (["--v2"] if v2 else []) + [m48, txo], check=True, capture_output=True)
    y = np.fromfile(txo, dtype=np.float32).reshape(-1, 2)
    z = np.conj(y[:, 0] + 1j * y[:, 1])              # back to librade's convention: modem at +fc
    z = resample_poly(z, 1, 4)                      # 48 kHz -> 12 kHz
    z = z[np.argmax(np.abs(z) > 1e-3):]             # drop the lead-in
    z = z[:np.nonzero(np.abs(z) > 1e-3)[0][-1] + 1]
    p = np.mean(np.abs(z[len(z) // 4:len(z) // 2]) ** 2)
    stamp = "selftest_%s_snr%g" % (wave.upper(), a.snr)
    sess = os.path.join(a.out, stamp)
    shutil.rmtree(sess, ignore_errors=True)
    os.makedirs(sess)
    rng = np.random.default_rng(1)
    pad = int(3 * FS_KIWI)
    zz = np.concatenate([np.zeros(pad), z, np.zeros(pad)])
    # noise: SNR is the signal power to the noise power in 3 kHz; the noise fills the 12 kHz
    sig2 = 4.0 * p / (10 ** (a.snr / 10.0))
    noise = np.sqrt(sig2 / 2) * (rng.standard_normal(len(zz)) + 1j * rng.standard_normal(len(zz)))
    inv = expected_invert(wave, a.freq)
    off = a.kiwi_below_khz * 1000.0                 # the Kiwi is tuned this much below the transmitter
    tt = np.arange(len(zz)) / FS_KIWI
    iq = np.conj(zz) if inv else zz                 # an inverted signal sits below the dial
    iq = iq * np.exp(2j * np.pi * off * tt)
    iq = (iq + noise) * 0.1 / np.sqrt(p)
    prof = default_profiles(wave, a.freq)
    for name in prof:
        if name.startswith("iq"):
            d = np.stack([iq.real, iq.imag], 1)
            wavfile.write(os.path.join(sess, name + ".wav"), 11999, (d * 32768).clip(-32768, 32767).astype(np.int16))
        else:
            # audio mode: the demodulated sideband, modem at positive audio frequencies
            au = (zz * np.exp(2j * np.pi * (-off if name == "lsb" else off) * tt) + noise) * 0.1 / np.sqrt(p)
            wavfile.write(os.path.join(sess, name + ".wav"), 11999, (au.real * 32768).clip(-32768, 32767).astype(np.int16))
    meta = {"host": "selftest", "port": 0, "freq_khz": a.freq - a.kiwi_below_khz, "tx_freq_khz": a.freq, "wave": wave,
            "expected_invert": inv, "profiles": prof, "call": a.call or "EI6IZ", "duration_s": len(zz) / FS_KIWI}
    json.dump(meta, open(os.path.join(sess, "session.json"), "w"), indent=1)
    a.ref = clip
    a.session = sess
    print("selftest: %s at %g kHz, SNR %g dB in 3 kHz, %s" % (wave.upper(), a.freq, a.snr, "inverted" if inv else "normal"))
    return analyze(a, sess)


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, rec=True):
        p.add_argument("--out", default=OUTDIR, help="where sessions go (default rade-captures/kiwi)")
        p.add_argument("--call", help="callsign the transmitter sends (V1); checked in the EOO")
        p.add_argument("--ref", help="the speech that was sent (16 kHz wav), for the quality figures")
        p.add_argument("--tx-freq", type=float, help="transmitter dial, kHz, if not the Kiwi's tuning")
        p.add_argument("--invert", type=lambda s: s.lower() in ("1", "true", "yes"),
                       help="override the expected inversion (default: V1 below 10 MHz)")
        p.add_argument("--min-snr", type=float, help="make SNR part of PASS")
        p.add_argument("--max-loss", type=float, help="make feature loss part of PASS (needs --ref)")
        p.add_argument("--try-other", action="store_true", help="also decode the other way round (V1 diagnostics)")

    def rec(p):
        p.add_argument("--host", default="kiwi.minish.org")
        p.add_argument("--port", type=int, default=8074)
        p.add_argument("--password", help="Kiwi password, if one is needed")
        p.add_argument("--freq", type=float, required=True, help="Kiwi tuning, kHz (the transmitter's dial)")
        p.add_argument("--wave", choices=["v1", "v2", "V1", "V2"], required=True)
        p.add_argument("--duration", type=int, default=40, help="seconds to record")
        p.add_argument("--profiles", help="comma list from iq,iq-fixed,usb,lsb")
        p.add_argument("--gain", type=int, default=50, help="manual gain (dB) for iq-fixed")
        p.add_argument("--launch-delay", type=float, default=1.0)
        p.add_argument("--kiwiclient", default="~/sdr/kiwiclient")
        p.add_argument("--gps", choices=["auto", "on", "off"], default="auto",
                       help="GNSS-stamped IQ recordings (kiwirecorder -w): auto = when the Kiwi reports a GPS solution")

    p = sub.add_parser("record"); rec(p); common(p)
    p = sub.add_parser("analyze"); p.add_argument("session"); common(p)
    p = sub.add_parser("run"); rec(p); common(p)
    p = sub.add_parser("selftest")
    common(p)
    p.add_argument("--wave", default="v1")
    p.add_argument("--freq", type=float, default=7053.0)
    p.add_argument("--snr", type=float, default=15.0)
    p.add_argument("--kiwi-below-khz", type=float, default=0.0, help="selftest: Kiwi tuned this far below the transmitter")
    a = ap.parse_args()
    if a.cmd == "record":
        record(a)
    elif a.cmd == "analyze":
        sys.exit(0 if analyze(a) else 1)
    elif a.cmd == "run":
        sess = record(a)
        sys.exit(0 if analyze(a, sess) else 1)
    elif a.cmd == "selftest":
        sys.exit(0 if selftest(a) else 1)


if __name__ == "__main__":
    main()

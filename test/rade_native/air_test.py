#!/usr/bin/env python3
"""Automated over-the-air runs of RADE transmit, recorded on both sides.

piHPSDR is driven through its test harness socket (rade_ctl.py; the operator has to tick "Test harness" in the
RADE window, and the harness will not key above its drive / band / length limits). For each run:

  set waveform, dial, level (drive_db), trim, leveller   ->   piHPSDR capture + parameter log on
  KiwiSDR recording on the same frequency (kiwi_rx.py: iq, iq-fixed, usb, in parallel)
  play the speech clip as the mic (key, send, unkey, end-of-over)   ->   captures off
  analysis of the Kiwi recordings (sync, SNR, offset, end-of-over, callsign, feature loss against the speech sent)
  and of the local side (the TX output: peak, rms, crest, how much of the over was against the ALC ceiling)

Results go to rade-captures/air/<session>/results.csv and results.json, with each run's piHPSDR capture stem,
parameter log and Kiwi session named. Timing: the harness gives key / first-sample / last-sample / release times; the Kiwi
recordings carry no time stamps, so the air delay is NOT measured (an earlier attempt to infer it
from the recorder's start time was wrong by the connect time and was removed).

  air_test.py --freq 28645 --wave v1,v2 --levels -24,-18,-12,-6,-3,0,3,6 [--clip FILE] [--seconds 20] [--call EI6IZ]
  air_test.py --reanalyze SESSION_DIR   recompute the local-side columns of a finished session
  air_test.py --dry-run ...        everything except the harness: no piHPSDR, no transmission (checks the Kiwi side)
"""
import argparse, csv, datetime, glob, json, os, subprocess, sys, tempfile, threading, time, types
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import rade_ctl, kiwi_rx, air_weak, air_timing

DEFAULT_CLIP = os.path.join(ROOT, "third_party", "rade_c-src", "wav", "david_vk5dgr.wav")


def prepare(clip, seconds, out_dir):
    """The speech: a 16 kHz reference (what the vocoder is given) and the 48 kHz mono file the harness plays."""
    fs, x = wavfile.read(clip)
    x = x.astype(np.float64) / (32768.0 if x.dtype == np.int16 else 1.0)
    if x.ndim > 1:
        x = x[:, 0]
    x16 = resample_poly(x, 16000, fs) if fs != 16000 else x
    x16 = x16[: int(seconds * 16000)]
    x48 = resample_poly(x16, 3, 1)
    p16, p48 = os.path.join(out_dir, "speech16k.wav"), os.path.join(out_dir, "speech48k.wav")
    wavfile.write(p16, 16000, (np.clip(x16, -1, 1) * 32767).astype(np.int16))
    wavfile.write(p48, 48000, (np.clip(x48, -1, 1) * 32767).astype(np.int16))
    return p16, p48, len(x16) / 16000.0


def resolve(path, folder):
    """Paths come back as piHPSDR sees them: relative to its working directory (the repo), or just a stem."""
    for p in (path, os.path.join(ROOT, path), os.path.join(ROOT, folder, os.path.basename(path))):
        if p and (os.path.exists(p) or os.path.exists(p + "_tx.wav")):
            return p
    return path


def log_side(log):
    """The parameter log of a run: the mic as the engine saw it, the buffers, whether the transmitter underran."""
    out = {}
    if not (log and os.path.exists(log)):
        return out
    rows = [r for r in csv.DictReader(l for l in open(log) if not l.startswith("#")) if r["kind"] == "tx" and r["tx"] == "1"]
    if not rows:
        return out
    f = lambda k: [float(r[k]) for r in rows if r[k] not in ("", None)]
    avg = f("loud_avg")
    out.update(mic_lufs=round(avg[-1], 1) if avg else None, mic_peak_dbfs=round(max(f("peak_dbfs")), 2),
               mic_limiting_pct=round(100.0 * sum(1 for r in rows if r["limiting"] == "1") / len(rows), 1),
               tx_underruns=int(max(f("tx_underruns"))), tx_out_fill_min=int(min(f("tx_out_fill"))),
               agc_db_max=round(max(f("agc_db")), 2), keyed_s=round(0.1 * len(rows), 1))
    return out


def local_side(stem, log=None):
    """What left the transmitter: from the capture's _tx.wav (WDSP TX output) and the parameter log."""
    out = log_side(log)
    f = stem + "_tx.wav"
    if os.path.exists(f):
        r, x = wavfile.read(f)
        m = np.hypot(x[:, 0], x[:, 1])
        m = m[m > 1e-3]
        if len(m):
            pk, rms = m.max(), np.sqrt(np.mean(m ** 2))
            out.update(tx_peak_dbfs=round(float(20 * np.log10(pk)), 2), tx_rms=round(float(rms), 4),
                       tx_crest_db=round(float(20 * np.log10(pk / rms)), 2),
                       tx_at_ceiling_pct=round(100.0 * float(np.mean(m >= 0.98)), 3))
    shape = subprocess.run([sys.executable, os.path.join(HERE, "tx_shape.py"), os.path.basename(stem).split("_")[0]],
                           capture_output=True, text=True, env=dict(os.environ, RADE_CAPTURES=os.path.dirname(stem)))
    for line in shape.stdout.splitlines():
        if "centre" in line and "tilt" in line:
            out["tx_shape"] = line.strip()
        if line.strip().startswith("->"):
            out["tx_shape_pass"] = "PASS" in line
    return out


def kiwi_namespace(a, wave, duration, out):
    return types.SimpleNamespace(
        host=a.host, port=a.port, password=None, freq=a.freq, wave=wave, duration=int(duration), profiles=None, gain=a.kiwi_gain,
        launch_delay=1.0, kiwiclient="~/sdr/kiwiclient", out=out, call=a.call, ref=None, tx_freq=None, invert=None,
        min_snr=None, max_loss=None, try_other=False, session=None, gps=a.gps)


def run_one(a, ctl, wave, level_db, n, p16, p48, clip_s, work):
    rid = "%s_%s_%+gdB" % ("r%02d" % n, wave, level_db)
    kdir = os.path.join(work, rid)
    os.makedirs(kdir, exist_ok=True)
    row = {"run": rid, "wave": wave, "level_db": level_db, "freq_khz": a.freq, "clip_s": round(clip_s, 2)}
    if ctl:
        ctl.set(wave=1 if wave == "v1" else 2, dial=int(round(a.freq * 1000)), drive_db=level_db, trim_db=a.trim,
                agc=1 if a.agc else 0, tx=1)
        row["capture_stem"] = ctl.capture(True)["stem"]
        row["log_file"] = ctl.log(True)["file"]
    ns = kiwi_namespace(a, wave, clip_s + a.margin, kdir)
    th = threading.Thread(target=lambda: setattr(ns, "session", kiwi_rx.record(ns)), daemon=True)
    th.start()
    t_rec = time.time()
    for _ in range(300):                               # until the recorders are connected and delivering
        files = glob.glob(os.path.join(kdir, "*", "iq.wav"))
        if files and os.path.getsize(files[0]) > 100000:
            break
        time.sleep(0.1)
    time.sleep(2.0)
    if ctl:
        try:
            t = ctl.play(p48, 0.0)
        except RuntimeError as e:
            row["error"] = str(e)
            t = {}
        row.update({k: round(v, 3) if isinstance(v, float) else v for k, v in t.items()})
        row["status_after"] = ctl.status().get("tx")
        ctl.capture(False)
        ctl.log(False)
    else:
        time.sleep(clip_s)
    th.join(timeout=clip_s + a.margin + 120)
    sess = getattr(ns, "session", None)
    row["kiwi_session"] = sess or ""
    if ctl and row.get("capture_stem"):
        row["capture_stem"] = resolve(row["capture_stem"], "rade-captures")
        row["log_file"] = resolve(row.get("log_file", ""), "rade-logs")
        row.update(local_side(row["capture_stem"], row["log_file"]))
    if sess:
        ns.ref = p16
        ns.session = sess
        try:
            kiwi_rx.analyze(ns, sess)
        except Exception as e:                           # analysis problems are results too
            row["kiwi_error"] = str(e)
        try:
            res = json.load(open(os.path.join(sess, "analysis.json")))
            meta = json.load(open(os.path.join(sess, "session.json")))
        except OSError:
            res, meta = [], {}
        try:                                   # band power on / off from the fixed-gain recording: SNR independent of librade's estimate
            ov = air_weak.over_of(sess, "iq-fixed")
            z, fs = air_weak.load_iq(os.path.join(sess, "iq-fixed.wav"))
            lv = air_weak.band_levels(z, ov, wave, bool(meta["expected_invert"]), fs) if ov else None
            if lv:
                row.update(band_snr_db=round(lv["band_snr_db"], 1), band_sig_dbfs=round(lv["sig_dbfs"], 1),
                           band_noise_dbfs=round(lv["noise_dbfs"], 1))
        except Exception as e:
            row["band_error"] = str(e)
        try:                                   # GPS-stamped: where the RF started and ended on the UTC axis, against the engine's events
            if meta.get("gps"):
                row.update(air_timing.run_timing(row, sess, wave, meta))
        except Exception as e:
            row["timing_error"] = str(e)
        t0 = datetime.datetime.strptime(meta["start_utc"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc).timestamp() if meta else 0
        for r in res:
            if not r["profile"].startswith("iq"):
                continue
            tag = r["profile"].replace("-", "_")
            if r["overs"]:
                o = max(r["overs"], key=lambda x: x["duration_s"])
                row.update({tag + "_sync_pct": o["sync_pct"], tag + "_snr_mean": o["snr_mean_db"], tag + "_snr_min": o["snr_min_db"],
                            tag + "_snr_max": o["snr_max_db"], tag + "_foff": o["foff_mean_hz"], tag + "_eoo_ok": o["eoo_ok"],
                            tag + "_call_ok": o["call_ok"]})
                for k in ("feature_loss", "speech_env_corr_vs_ideal"):
                    if k in o:
                        row[tag + "_" + k] = o[k]
                if tag == "iq":
                    row["kiwi_over_start_s"] = o["start_s"]        # from the start of the recording, not tied to the key
            else:
                row.update({tag + "_sync_pct": 0.0, tag + "_overs": 0})
    return row


def reanalyze(session):
    """Recompute the local side of every run of a session (paths resolved, parameter log read) and rewrite the results."""
    try:
        rows = json.load(open(os.path.join(session, "results.json")))
    except ValueError:
        rows = list(csv.DictReader(open(os.path.join(session, "results.csv"))))
    for r in rows:
        if r.get("capture_stem"):
            r["capture_stem"] = resolve(r["capture_stem"], "rade-captures")
            r["log_file"] = resolve(r.get("log_file", ""), "rade-logs")
            r.update(local_side(r["capture_stem"], r["log_file"]))
    cols = []
    for r in rows:
        cols += [k for k in r if k not in cols]
    with open(os.path.join(session, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    open(os.path.join(session, "results.json"), "w").write(json.dumps(rows, indent=1, default=float))
    print("rewrote", os.path.join(session, "results.csv"))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--reanalyze":
        return reanalyze(sys.argv[2])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--freq", type=float, required=True, help="dial, kHz (the Kiwi is tuned to it too)")
    ap.add_argument("--wave", default="v1,v2")
    ap.add_argument("--levels", default="0", help="drive_db values, -30..+6 (0 = the defined level, the ALC just reached)")
    ap.add_argument("--clip", default=DEFAULT_CLIP)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--trim", type=float, default=0.0)
    ap.add_argument("--agc", action="store_true", help="leave the mic leveller on (off by default: a fixed level)")
    ap.add_argument("--call", default="EI6IZ")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--margin", type=float, default=22.0, help="Kiwi recording seconds beyond the clip")
    ap.add_argument("--kiwi-gain", type=int, default=50, help="manual gain (dB) of the fixed-gain Kiwi profile")
    ap.add_argument("--gps", choices=["auto", "on", "off"], default="auto", help="GNSS-stamped Kiwi IQ (needs a GPS solution on the Kiwi)")
    ap.add_argument("--host", default="kiwi.minish.org")
    ap.add_argument("--port", type=int, default=8074)
    ap.add_argument("--out", default=os.path.join(ROOT, "rade-captures", "air"))
    ap.add_argument("--yes", action="store_true", help="do not wait for the operator's go-ahead")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    waves = [w.strip().lower() for w in a.wave.split(",")]
    levels = [float(v) for v in a.levels.split(",")]
    session = os.path.join(a.out, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "_%gkHz" % a.freq)
    os.makedirs(session, exist_ok=True)
    p16, p48, clip_s = prepare(a.clip, a.seconds, session)
    plan = [(w, lv) for _ in range(a.repeat) for w in waves for lv in levels]
    ctl = None
    if not a.dry_run:
        ctl = rade_ctl.Ctl()
        st = ctl.status()
        print("piHPSDR: armed=%s drive %s %%, dial %s, can_key=%s (%s)" % (st["armed"], st["tx_drive_pct"], st["dial"],
                                                                         st["can_key"], st["refusal"]))
        if st["armed"] != "1":
            raise SystemExit("the operator has to tick Test harness in the RADE window")
    est = len(plan) * (clip_s + a.margin + 25)
    print("%d runs of a %.0f s clip on %g kHz, about %.0f min: %s" % (len(plan), clip_s, a.freq, est / 60,
                                                                    ", ".join("%s %+g dB" % p for p in plan)))
    if not a.yes and not a.dry_run:
        input("operator: monitoring, ANT2, drive as set - press Enter to start (Ctrl-C to stop) ")
    rows = []
    try:
        for n, (w, lv) in enumerate(plan, 1):
            print("--- run %d/%d: %s at %+g dB" % (n, len(plan), w.upper(), lv))
            row = run_one(a, ctl, w, lv, n, p16, p48, clip_s, session)
            rows.append(row)
            print("    %s" % {k: row[k] for k in row if k in ("done", "tx_peak_dbfs", "band_snr_db", "iq_sync_pct", "iq_snr_mean", "iq_snr_min",
                                                               "iq_eoo_ok", "iq_call_ok", "tx_start_latency_ms", "tail_after_eoo_ms", "error")})
    except KeyboardInterrupt:
        print("stopped")
    finally:
        if ctl:
            try:
                ctl.abort()
            except Exception:
                pass
            ctl.close()
    cols = []
    for r in rows:
        cols += [k for k in r if k not in cols]
    with open(os.path.join(session, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    open(os.path.join(session, "results.json"), "w").write(json.dumps(rows, indent=1, default=float))
    print("results:", os.path.join(session, "results.csv"))


if __name__ == "__main__":
    main()

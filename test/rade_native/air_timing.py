#!/usr/bin/env python3
"""Timing of a transmitted over, from a GPS-stamped Kiwi recording and the transmit engine's own event times.

The Kiwi's IQ recording (kiwirecorder --kiwi-wav) carries GNSS time per block, so the RF can be placed on the UTC axis to
about a millisecond (the time stamps scatter by 0.1 us; the PC's clock, chrony-disciplined to a LAN server, is within about a
millisecond; the propagation delay of a ground wave over a few km is microseconds). The engine's events (rade_native_tx_times()
and the harness's key/unkey times) are on the PC's clock. So:

  RF onset / end   from the modem-band power of the recording: the times at which it passes -20, -10 and -3 dB below the
                   steady level (rising) and again (falling), 1 ms resolution, 5 ms smoothing
  engine_to_wdsp_out   the capture's WDSP TX output (_tx.wav, with _txblocks.csv for the clock): when the first modem energy left
                   WDSP - ev_out0 (WDSP's own delay); wdsp_out_to_rf_onset is then the rest: the radio protocol, the radio, the air
  wdsp_out_last_to_rf_end   the same at the end: equal to wdsp_out_to_rf_onset if the radio sent everything WDSP gave it;
                   less means the radio was released with samples still on their way (a clipped tail)
  unkey_after_wdsp_last   ev_unkey (the engine's first unkeyed block) - the last modem energy out of WDSP: negative = released before WDSP had finished.
                   (mox_off_after_wdsp_last is the harness's key-up REQUEST, made before the engine's end-of-over hold: always about -500 ms, not an early release)
  tx_start_latency   RF onset (-10 dB) - ev_out0  (the first modem sample handed to WDSP)   : WDSP + radio, no engine delay
  tx_end_latency     RF end   (-10 dB) - ev_eoo_out (the last end-of-over sample handed to WDSP): should equal the start
                   latency; shorter means the tail was cut (the radio released before the pipeline had emptied)
  tail_after_eoo     RF end - (ev_eoo_out + tx_start_latency): >= 0 means the end-of-over frame went out whole, and how much more
                   RF followed (the TX filter's ring-down, WDSP's slew, the hold)
  hold_after_eoo     the time the engine held the radio after the last EOO sample went to WDSP: ev_done - ev_eoo_out
  mox_off_to_rf_end  t_mox_off -> RF end (how long after the key-up call the RF lasted)
  rise_ms / fall_ms  the 10 -> 90 % rise of the envelope and 90 -> 10 % fall (amplitude): onset and end are slewed, not abrupt
  sync_after_onset   the decoder's first sync (UTC, end of its block) - RF onset: the time librade needs to acquire
  speech_start_clipped_ms / speech_end_missing_ms   from the decoded features against the speech sent (kiwi_rx.feature_profile)

  air_timing.py SESSION            recompute the timing columns of every run of an air_test session
"""
import json, os, sys
import numpy as np
from scipy.signal import firwin, lfilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kiwi_gps_wav as KG
import kiwi_rx

BW = {"v1": 1600.0, "v2": 1000.0}


def envelope(rec, wave, inv, delta_hz=0.0, ms=5.0):
    """Modem-band power against UTC time at 1 ms steps: (t_unix, p). The band is mixed to baseband and low-passed."""
    fs = rec.fs
    z = rec.z.astype(np.complex128)
    n = np.arange(len(z))
    fc = kiwi_rx.FC[wave]
    if inv:
        z = np.conj(z)
    z = z * np.exp(-2j * np.pi * (fc + delta_hz) * n / fs)
    h = firwin(481, BW[wave] / 2.0 + 60.0, fs=fs)
    y = lfilter(h, 1.0, z)[240:]                 # group delay 240 samples removed
    p = np.abs(y) ** 2
    k = max(1, int(round(ms * 1e-3 * fs)))
    ps = np.convolve(p, np.ones(k) / k, mode="same")
    step = int(round(fs / 1000.0))
    idx = np.arange(0, len(ps), step)
    return rec.time(idx), ps[idx]


def edges(t, p, ref_t0=None, ref_t1=None):
    """RF onset and end at -20/-10/-3 dB below the steady level, rise and fall times. t, p from envelope()."""
    floor = float(np.percentile(p, 5))
    top = float(np.percentile(p, 90))
    if top < floor * 10:
        return None
    steady = float(np.median(p[p > top * 0.3]))
    out = {"floor_db": 10 * np.log10(floor + 1e-30), "steady_db": 10 * np.log10(steady + 1e-30),
           "snr_db": 10 * np.log10(steady / floor)}
    above = np.where(p > floor * 4.0)[0]
    # the over: the longest run of samples above +6 dB over the floor (gaps under 200 ms joined)
    runs, start = [], above[0]
    for a, b in zip(above[:-1], above[1:]):
        if b - a > 200:
            runs.append((start, a)); start = b
    runs.append((start, above[-1]))
    i0, i1 = max(runs, key=lambda r: r[1] - r[0])
    for name, db in (("m20", -20.0), ("m10", -10.0), ("m3", -3.0)):
        thr = steady * 10 ** (db / 10.0)
        up = i0 + int(np.argmax(p[i0:i1] > thr)) if np.any(p[i0:i1] > thr) else None
        dn = i1 - int(np.argmax(p[i0:i1][::-1] > thr)) if np.any(p[i0:i1] > thr) else None
        out["onset_" + name] = float(t[up]) if up is not None else None
        out["end_" + name] = float(t[dn]) if dn is not None else None
    # amplitude rise/fall 10 -> 90 %: -20 dB to -0.9 dB of power
    def tcross(db, rising):
        thr = steady * 10 ** (db / 10.0)
        if rising:
            k = np.where(p[i0 - 200:i0 + 2000] > thr)[0]
            return float(t[i0 - 200 + k[0]]) if len(k) else None
        k = np.where(p[max(0, i1 - 2000):i1 + 200] > thr)[0]
        return float(t[max(0, i1 - 2000) + k[-1]]) if len(k) else None
    a, b = tcross(-20.0, True), tcross(-0.9, True)
    c, d = tcross(-0.9, False), tcross(-20.0, False)
    out["rise_ms"] = round((b - a) * 1000, 1) if a and b else None
    out["fall_ms"] = round((d - c) * 1000, 1) if c and d else None
    out["rf_duration_s"] = out["end_m10"] - out["onset_m10"] if out["end_m10"] and out["onset_m10"] else None
    return out


def kiwi_offset_s():
    """The Kiwi's time axis is late by this many seconds (kiwi_time_check.py, against MSF 60 kHz); 0 if never measured."""
    try:
        d = json.load(open(os.path.join(HERE, "out", "kiwi_time_offset.json")))
        return float(d["offset_ms"]) / 1000.0
    except (OSError, ValueError, KeyError):
        return 0.0


def wdsp_out_times(stem):
    """When the first and the last modem energy left WDSP, from the capture's _tx.wav (WDSP's TX output block) and _txblocks.csv
    (when each block arrived). The block times pace the samples: t(k) = a + k / rate with a the median over the blocks.
    Returns None if the files are missing."""
    import csv
    from scipy.io import wavfile
    f, b = stem + "_tx.wav", stem + "_txblocks.csv"
    if not (os.path.exists(f) and os.path.exists(b)):
        return None
    rows = list(csv.DictReader(open(b)))
    if len(rows) < 5:
        return None
    unix = np.array([float(r["unix"]) for r in rows]); idx = np.array([float(r["sample_index"]) for r in rows])
    rate = float(rows[0]["rate"])
    a = float(np.median(unix - idx / rate))
    jitter_ms = float(np.std((unix - idx / rate) - a) * 1000.0)
    fs, x = wavfile.read(f)
    m = np.hypot(x[:, 0], x[:, 1]).astype(np.float64)
    k = int(0.002 * fs)
    ms = np.convolve(m, np.ones(k) / k, mode="same")
    top = float(np.percentile(ms, 90))
    if top <= 0:
        return None
    on = np.where(ms > 0.1 * top)[0]
    return {"first": a + on[0] / rate, "last": a + on[-1] / rate, "jitter_ms": jitter_ms, "rate": rate}


def run_timing(row, sess, wave, meta):
    """The timing columns for one run: row has the harness's times; sess is the Kiwi session directory."""
    res = {}
    f = os.path.join(sess, "iq-fixed.wav")
    if not (meta.get("gps") and os.path.exists(f)):
        return res
    try:
        rec = KG.read_kiwi_gps_wav(f, meta["start_unix"])
    except ValueError as e:
        return {"gps_error": str(e)}
    inv = bool(meta["expected_invert"])
    t, p = envelope(rec, wave, inv, 1000.0 * (meta["tx_freq_khz"] - meta["freq_khz"]))
    e = edges(t, p)
    if e is None:
        return {"gps_error": "no over in the recording"}
    off = kiwi_offset_s()
    for k in list(e):
        if k.startswith(("onset_", "end_")) and e[k] is not None:
            e[k] -= off                              # the Kiwi's axis is late: what it calls t happened at t - off
    res["kiwi_time_offset_ms"] = round(off * 1000, 2)
    res.update(gps_solution_pct=round(100 * rec.solution), gps_stamp_scatter_us=round(rec.resid_us, 2), kiwi_fs_hz=round(rec.fs, 4),
               rf_onset_unix=round(e["onset_m10"], 4), rf_end_unix=round(e["end_m10"], 4),
               rf_onset_m20_unix=round(e["onset_m20"], 4) if e["onset_m20"] else None,
               rf_end_m20_unix=round(e["end_m20"], 4) if e["end_m20"] else None,
               rf_rise_ms=e["rise_ms"], rf_fall_ms=e["fall_ms"], rf_duration_s=round(e["rf_duration_s"], 3),
               rf_snr_db=round(e["snr_db"], 1))
    g = lambda k: row.get(k) or 0.0
    stem = row.get("capture_stem")
    w = wdsp_out_times(stem) if stem else None
    if w:
        res["wdsp_out_first_unix"] = round(w["first"], 4)
        res["wdsp_out_last_unix"] = round(w["last"], 4)
        res["wdsp_block_clock_jitter_ms"] = round(w["jitter_ms"], 1)
        if g("ev_out0"):
            res["engine_to_wdsp_out_ms"] = round((w["first"] - g("ev_out0")) * 1000, 1)
        res["wdsp_out_to_rf_onset_ms"] = round((e["onset_m10"] - w["first"]) * 1000, 1)
        res["wdsp_out_last_to_rf_end_ms"] = round((e["end_m10"] - w["last"]) * 1000, 1)
        if g("ev_eoo_out"):
            res["eoo_engine_to_wdsp_out_ms"] = round((w["last"] - g("ev_eoo_out")) * 1000, 1)
        if g("t_mox_off"):
            res["mox_off_after_wdsp_last_ms"] = round((g("t_mox_off") - w["last"]) * 1000, 1)
        if g("ev_unkey"):
            res["unkey_after_wdsp_last_ms"] = round((g("ev_unkey") - w["last"]) * 1000, 1)
    if g("ev_out0"):
        res["tx_start_latency_ms"] = round((e["onset_m10"] - g("ev_out0")) * 1000, 1)
    if g("ev_key"):
        res["key_to_rf_onset_ms"] = round((e["onset_m10"] - g("ev_key")) * 1000, 1)
    if g("t_mox_on"):
        res["mox_on_to_rf_onset_ms"] = round((e["onset_m10"] - g("t_mox_on")) * 1000, 1)
    if g("ev_eoo_out"):
        res["tx_end_latency_ms"] = round((e["end_m10"] - g("ev_eoo_out")) * 1000, 1)
        if "tx_start_latency_ms" in res:
            res["tail_after_eoo_ms"] = round(res["tx_end_latency_ms"] - res["tx_start_latency_ms"], 1)
    if g("ev_done") and g("ev_eoo_out"):
        res["hold_after_eoo_ms"] = round((g("ev_done") - g("ev_eoo_out")) * 1000, 1)
    if g("t_mox_off"):
        res["mox_off_to_rf_end_ms"] = round((e["end_m10"] - g("t_mox_off")) * 1000, 1)
    if g("ev_unkey"):
        res["unkey_to_rf_end_ms"] = round((e["end_m10"] - g("ev_unkey")) * 1000, 1)
    # the decoder's view, from the analysis of the same recording
    try:
        an = json.load(open(os.path.join(sess, "analysis.json")))
        for r in an:
            if r["profile"] == "iq-fixed" and r["overs"]:
                o = max(r["overs"], key=lambda x: x["duration_s"])
                if o.get("start_unix"):
                    res["sync_after_onset_ms"] = round((o["start_unix"] - off - e["onset_m10"]) * 1000, 1)
                if o.get("eoo_at_unix"):
                    res["eoo_decoded_after_rf_end_ms"] = round((o["eoo_at_unix"] - off - e["end_m10"]) * 1000, 1)
                    res["eoo_decoded_vs_ev_eoo_out_ms"] = round((o["eoo_at_unix"] - off - g("ev_eoo_out")) * 1000, 1) if g("ev_eoo_out") else None
                for k in ("speech_start_clipped_ms", "speech_end_missing_ms", "rx_start_vs_speech_frames", "loss_first_250ms",
                          "loss_first_1s", "loss_middle", "loss_last_1s", "foff_mean_hz"):
                    if k in o:
                        res["fx_" + k] = o[k]
                if o.get("foff_mean_hz") is not None:
                    res["tx_freq_error_hz"] = o["foff_mean_hz"]
                    res["tx_freq_error_ppm"] = round(o["foff_mean_hz"] / (meta["tx_freq_khz"] * 1000.0) * 1e6, 3)
    except (OSError, ValueError):
        pass
    return res


def main():
    import csv
    sess = sys.argv[1]
    rows = json.load(open(os.path.join(sess, "results.json")))
    for r in rows:
        ks = r.get("kiwi_session")
        if not ks:
            continue
        meta = json.load(open(os.path.join(ks, "session.json")))
        r.update(run_timing(r, ks, r["wave"], meta))
    cols = []
    for r in rows:
        cols += [k for k in r if k not in cols]
    with open(os.path.join(sess, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols); w.writeheader(); w.writerows(rows)
    open(os.path.join(sess, "results.json"), "w").write(json.dumps(rows, indent=1, default=float))
    print("rewrote", os.path.join(sess, "results.csv"))


if __name__ == "__main__":
    main()

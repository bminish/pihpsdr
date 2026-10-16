#!/usr/bin/env python3
"""air_timing against a synthetic GPS-stamped Kiwi recording with a burst whose RF start and end are known: a V2 modem
signal with raised-cosine ramps of 25 ms, in noise, written in the Kiwi --kiwi-wav layout with block time stamps.
The onset and end are recovered, on the UTC axis, to within a few ms; the Kiwi's real sample rate (not 12 kHz) is used."""
import os, struct, sys, tempfile
import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kiwi_gps_wav as KG
import air_timing as AT

fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

rng = np.random.default_rng(5)
d = np.fromfile(os.path.join(HERE, "out", "v2_bpf.iq.f32"), dtype=np.float32)
m8 = (d[0::2] + 1j * d[1::2])[2000:2000 + 8000 * 6]
fs_true = 11998.937
sig = resample_poly(m8, 3, 2)
sig = sig / np.sqrt(np.mean(np.abs(sig) ** 2))
T_RAMP = 0.025
n_total = int(14.0 * fs_true)
t_on, t_off = 4.0, 4.0 + len(sig) / fs_true          # seconds from the start of the file: the burst's RF start and end
x = (rng.standard_normal(n_total) + 1j * rng.standard_normal(n_total)) * 0.01
i0 = int(round(t_on * fs_true))
burst = sig.copy()
nr = int(T_RAMP * fs_true)
ramp = 0.5 * (1 - np.cos(np.pi * np.arange(nr) / nr))
burst[:nr] *= ramp
burst[-nr:] *= ramp[::-1]
x[i0:i0 + len(burst)] += burst * 0.3
# the Kiwi layout: fmt, then (kiwi, data) per block of 512 samples
base_unix = 1792155000.123456
sow0 = (base_unix - KG.GPS_EPOCH_UNIX + KG.LEAP_S) % KG.WEEK
path = os.path.join(tempfile.mkdtemp(), "synthetic.wav")
with open(path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 0) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 11999, 11999 * 4, 4, 16))
    blk = 512
    for b in range(2, n_total // blk):                 # the first two blocks of a real file are unstamped; skip them here too
        s = b * blk
        t = sow0 + s / fs_true
        z = x[s:s + blk]
        iq = np.empty(2 * blk, dtype="<i2")
        iq[0::2] = np.clip(z.real * 20000, -32768, 32767)
        iq[1::2] = np.clip(z.imag * 20000, -32768, 32767)
        f.write(b"kiwi" + struct.pack("<IBBII", 10, int(b % 2 == 0), 0, int(t), int(round((t % 1) * 1e9))))
        f.write(b"data" + struct.pack("<I", len(iq) * 2) + iq.tobytes())
# the file starts at block 2 and the reader drops two more: its sample 0 is at base + 4*blk/fs
t_file0 = base_unix + 4 * 512 / fs_true         # the reader drops the first two blocks it finds, as kiwirecorder's own reader does
rec = KG.read_kiwi_gps_wav(path, base_unix)
check(abs(rec.fs - fs_true) < 0.01, "sample rate fitted from the stamps: %.4f (true %.4f)" % (rec.fs, fs_true))
check(abs(rec.t0_unix - t_file0) < 1e-4, "UTC time of sample 0: %.6f (true %.6f, error %.1f us)" % (rec.t0_unix, t_file0, 1e6 * (rec.t0_unix - t_file0)))
t, p = AT.envelope(rec, "v2", False)
e = AT.edges(t, p)
# the true -10 dB points of a raised-cosine amplitude ramp: a = 0.316 at tau = (T / pi) * arccos(1 - 2 * 0.316)
tau = T_RAMP / np.pi * np.arccos(1 - 2 * 10 ** (-10 / 20))
# x was indexed from 0 and its sample s is stamped base + s / fs: the burst's RF start is base + t_on
on_true = base_unix + t_on + tau
off_true = base_unix + t_off - tau
check(e is not None, "an over found")
check(abs(e["onset_m10"] - on_true) < 0.005, "onset (-10 dB): %.4f, true %.4f, error %.1f ms" % (e["onset_m10"], on_true, 1000 * (e["onset_m10"] - on_true)))
check(abs(e["end_m10"] - off_true) < 0.005, "end   (-10 dB): %.4f, true %.4f, error %.1f ms" % (e["end_m10"], off_true, 1000 * (e["end_m10"] - off_true)))
check(e["rise_ms"] is not None and 12 < e["rise_ms"] < 18, "rise time %.1f ms (25 ms raised-cosine ramp: 10-90 %% of its amplitude is 14.7 ms)" % (e["rise_ms"] or -1))
# the same file through the whole analysis (to_baseband on GPS time, the decoder, the timing columns)
import json, types, shutil
import kiwi_rx
sess = tempfile.mkdtemp()
shutil.copy(path, os.path.join(sess, "iq-fixed.wav"))
json.dump({"host": "x", "port": 1, "freq_khz": 28645.0, "tx_freq_khz": 28645.0, "wave": "v2", "expected_invert": False,
           "profiles": ["iq-fixed"], "gps": True, "start_unix": base_unix, "call": None}, open(os.path.join(sess, "session.json"), "w"))
ns = types.SimpleNamespace(try_other=False, call=None, ref=None, tx_freq=None, invert=None, min_snr=None, max_loss=None, session=sess)
import contextlib, io
with contextlib.redirect_stdout(io.StringIO()):          # its report (no end-of-over frame in the synthetic signal) is not the point here
    kiwi_rx.analyze(ns, sess)
an = json.load(open(os.path.join(sess, "analysis.json")))
o = an[0]["overs"][0] if an and an[0]["overs"] else None
check(o is not None and "start_unix" in o, "the decoder found the over and gave it a UTC time")
row = {"ev_out0": on_true - 0.3, "ev_eoo_out": off_true - 0.2, "t_mox_off": off_true - 0.1}      # invented engine times, to see the columns
r = AT.run_timing(row, sess, "v2", json.load(open(os.path.join(sess, "session.json"))))
check("rf_onset_unix" in r, "timing columns: %s" % {k: r[k] for k in ("rf_onset_unix", "tx_start_latency_ms", "tx_end_latency_ms", "tail_after_eoo_ms", "sync_after_onset_ms") if k in r})
check(abs(r["tx_start_latency_ms"] - 300) < 8 and abs(r["tx_end_latency_ms"] - 200) < 8, "latencies recovered against the invented engine times (300 ms and 200 ms)")
check(0 < r.get("sync_after_onset_ms", -1) < 1500, "the decoder acquired %.0f ms after the RF onset" % r.get("sync_after_onset_ms", -1))
# WDSP's TX output (capture _tx.wav + _txblocks.csv): first and last energy placed on the clock
from scipy.io import wavfile
stem = os.path.join(tempfile.mkdtemp(), "cap")
fs_tx = 192000
nsamp = int(8 * fs_tx)
sig_tx = np.zeros((nsamp, 2), dtype=np.float32)
s0, s1 = int(1.5 * fs_tx), int(6.25 * fs_tx)
tone = (rng.standard_normal(s1 - s0) + 1j * rng.standard_normal(s1 - s0)) * 0.2
sig_tx[s0:s1, 0], sig_tx[s0:s1, 1] = tone.real, tone.imag
wavfile.write(stem + "_tx.wav", fs_tx, sig_tx)
T_a = 1792155100.5
with open(stem + "_txblocks.csv", "w") as f:
    f.write("unix,sample_index,rate\n")
    for i in range(0, nsamp, 4096):
        f.write("%.6f,%d,%d\n" % (T_a + i / fs_tx + rng.normal(0, 0.002), i, fs_tx))
w = AT.wdsp_out_times(stem)
check(abs(w["first"] - (T_a + 1.5)) < 0.004 and abs(w["last"] - (T_a + 6.25)) < 0.004,
      "WDSP output times: first %.4f (true %.4f), last %.4f (true %.4f), block clock jitter %.1f ms" % (w["first"], T_a + 1.5, w["last"], T_a + 6.25, w["jitter_ms"]))
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

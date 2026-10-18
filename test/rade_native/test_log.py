#!/usr/bin/env python3
"""The parameter log (src/rade_native_log.c): a scripted scene in, the tables out - receive samples only in sync and at 2 Hz, nothing
for the receiver without a lock, transmit only while keyed, the over with its frequency, mode and call, the events."""
import csv, glob, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

work = tempfile.mkdtemp()
r = subprocess.run([os.path.join(HERE, "test_log")], cwd=work, capture_output=True, text=True, timeout=120)
out = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l and not l.startswith("producer"))
print(r.stdout.strip().splitlines()[-1])
import tarfile
def unpack(run):
    """A stopped run is a .tar.gz (the directory is removed once the copy is made): open it where it was."""
    if not os.path.isdir(os.path.join(work, run)) and os.path.exists(os.path.join(work, run + ".tar.gz")):
        with tarfile.open(os.path.join(work, run + ".tar.gz")) as t: t.extractall(os.path.dirname(os.path.join(work, run)))
def table(run, name):
    unpack(run)
    with open(os.path.join(work, run, name)) as f:
        return list(csv.DictReader(f))
def meta(run):
    unpack(run)
    return open(os.path.join(work, run, "meta.txt")).read()

a, b = out["first"], out["second"]
check(a != b and all(os.path.exists(os.path.join(work, x + ".tar.gz")) for x in (a, b)), "each start makes a new directory: %s, %s" % (a, b))
print("run 1 (V2: 6 s in sync, a call at -3.5 dB, 4 s with no lock, 2 s keyed)")
rx = table(a, "rx.csv")
rf = table(a, "rx_full.csv")
check(len(rf) > 10 * len(rx) and any(x["sync"] == "0" for x in rf) and all(len(x) == 13 for x in rf), "rx_full.csv: every call, in sync or not (%d rows, %d in rx.csv)" % (len(rf), len(rx)))
check(all(x["delta_hat"] != "" for x in rf), "...with the V2 statistics for a V2 run")
check(10 <= len(rx) <= 14, "receive samples: %d rows for 6 s in sync (2 a second)" % len(rx))
check(all(abs(float(x["t"]) - float(y["t"]) - 0.5) < 0.2 for x, y in zip(rx[1:], rx)), "...half a second apart")
check(all(int(x["n"]) >= 10 for x in rx[1:-1]), "...each the mean of about 12 calls (n = %s)" % [x["n"] for x in rx][:4])
fade = min(float(x["snr_min"]) for x in rx)
check(fade < 5.0 and max(float(x["snr_mean"]) for x in rx) > 9, "the minimum keeps the fade (min %.1f dB, mean up to %.1f)" % (fade, max(float(x["snr_mean"]) for x in rx)))
check(all(x["delta_hat"] != "" and x["gain"] != "" for x in rx), "V2 rows carry librade's statistics")
check(max(float(x["t"]) for x in rx) < 7.5, "nothing for the receiver after sync was lost (last row at %.1f s)" % max(float(x["t"]) for x in rx))
ov = table(a, "overs.csv")
check(len(ov) == 1, "one over")
o = ov[0]
check(o["dial_hz"] == "28645000" and o["mode"] == "DIGU" and o["wave"] == "V2" and o["invert"] == "0", "with its frequency, mode and waveform: %s %s %s" % (o["dial_hz"], o["mode"], o["wave"]))
check(o["call"] == "EI6IZ" and o["call_snr"] == "-3.5" and o["eoo"] == "1", "the call at the end of the over, with a negative SNR kept: %s %s eoo=%s" % (o["call"], o["call_snr"], o["eoo"]))
check(5.0 < float(o["t_end"]) - float(o["t_start"]) < 7.0 and o["utc_start"].endswith("Z") and float(o["sync_pct"]) > 90, "duration %.1f s, sync %s %%" % (float(o["t_end"]) - float(o["t_start"]), o["sync_pct"]))
tx = table(a, "tx.csv"); txo = table(a, "tx_overs.csv")
check(3 <= len(tx) <= 6 and all(float(x["t"]) > 6.0 for x in tx), "transmit samples only while keyed: %d rows" % len(tx))
check(len(txo) == 1 and txo[0]["wave"] == "V2" and txo[0]["underruns"] == "2" and 250 <= float(txo[0]["hold_ms"]) <= 350 and txo[0]["drive_db"] == "-2.0",
      "one transmitted over: underruns %s, hold %s ms, drive %s" % (txo[0]["underruns"], txo[0]["hold_ms"], txo[0]["drive_db"]))
ev = table(a, "events.csv")
kinds = [e["kind"] for e in ev]
for k in ("wave", "dial", "mode", "sync_on", "eoo", "call", "sync_off", "key_down", "key_up", "drive", "tx_underruns"):
    check(k in kinds, "event %s" % k)
check([e["value"] for e in ev if e["kind"] == "call"] == ["EI6IZ snr=-3.5"], "the call event carries the call and its SNR")
m = meta(a)
check("waveform: V2" in m and "dropped_rows: 0" in m and "started_utc:" in m, "meta.txt: settings, start, nothing dropped")
print("run 2 (V1, the call came with no lock)")
ov = table(b, "overs.csv")
check(len(ov) == 1 and ov[0]["call"] == "G4ABC" and ov[0]["call_snr"] == "" and ov[0]["dial_hz"] == "7053000" and ov[0]["mode"] == "DIGL" and ov[0]["invert"] == "1" and ov[0]["wave"] == "V1",
      "the over has its call and no call SNR: %s" % ov)
rx = table(b, "rx.csv")
check(rx and all(x["delta_hat"] == "" and x["gain"] == "" for x in rx), "V1 rows have no V2 columns")
check(not table(b, "tx.csv") and not table(b, "tx_overs.csv"), "no transmit rows when nothing was keyed")
time.sleep(1.5)
for run in (a, b):
    check(os.path.exists(os.path.join(work, run + ".tar.gz")),
          "compressed copy %s.tar.gz" % run)
for x in (a, b): unpack(x)
sizes = sum(os.path.getsize(os.path.join(work, x, f)) for x in (a, b) for f in os.listdir(os.path.join(work, x)))
print("  size of the two runs: %d bytes" % sizes)
check("worst" in r.stdout, "producer cost measured")
cost = float(r.stdout.split("worst")[1].split("ns")[0])
check(cost < 1e6, "worst producer call %.0f ns" % cost)
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)

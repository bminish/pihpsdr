#!/usr/bin/env python3
"""tidy_logs.py on scratch directories: every run but the newest is finished (ended_utc added, compressed, copy checked, directory
removed); the newest, a run being written to, a run named in keep, and a run whose copy does not check out are left alone."""
import os, sys, tarfile, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "rade-logview"))
import tidy_logs

fails = []
def check(name, ok, info=""):
    print("  %-4s %s %s" % ("ok" if ok else "FAIL", name, info))
    if not ok: fails.append(name)

def make(root, name, ended=False, age=3600):
    d = os.path.join(root, name)
    os.makedirs(d)
    open(d + "/meta.txt", "w").write("started_utc: x\n" + ("ended_utc: y\n" if ended else ""))
    open(d + "/rx.csv", "w").write("t,snr_mean\n0.0,5\n" * 50)
    open(d + "/events.csv", "w").write("t,kind,value\n")
    t = time.time() - age
    for n in os.listdir(d):
        os.utime(os.path.join(d, n), (t, t))
    return d

def members(tgz):
    with tarfile.open(tgz) as tf:
        return sorted(m.name for m in tf.getmembers() if m.isfile())

root = tempfile.mkdtemp()
make(root, "20261001-100000")                       # killed: no ended_utc, no copy
make(root, "20261001-110000", ended=True)           # ended, copy never made
d3 = make(root, "20261001-120000", ended=True)      # a copy exists already and is good
with tarfile.open(d3 + ".tar.gz", "w:gz") as tf:
    tf.add(d3, arcname="20261001-120000")
d4 = make(root, "20261001-130000")                  # a copy exists but is damaged
open(d4 + ".tar.gz", "wb").write(b"not a tar")
make(root, "20261001-140000")                       # named in keep
make(root, "20261001-150000", age=5)                # still being written to
make(root, "20261001-160000")                       # the newest
os.makedirs(os.path.join(root, "notes"))            # not a run
open(os.path.join(root, "20261001-100000.csv"), "w").write("old single-file log")

msgs = []
done = tidy_logs.tidy(root, keep=["20261001-140000"], log=msgs.append)
left = sorted(n for n in os.listdir(root) if os.path.isdir(os.path.join(root, n)))
check("finished: the four that are over, not the others", done == ["20261001-100000", "20261001-110000", "20261001-120000", "20261001-130000"], str(done))
check("directories left: kept, busy, newest, and not-a-run", left == ["20261001-140000", "20261001-150000", "20261001-160000", "notes"], str(left))
check("each finished run has its archive with its files", all(
    members(os.path.join(root, n + ".tar.gz")) == ["%s/events.csv" % n, "%s/meta.txt" % n, "%s/rx.csv" % n]
    for n in done))
check("the damaged copy was replaced by a good one", tarfile.is_tarfile(d4 + ".tar.gz"))
with tarfile.open(os.path.join(root, "20261001-100000.tar.gz")) as tf:
    meta = tf.extractfile("20261001-100000/meta.txt").read().decode()
check("a killed run's meta.txt says when it ended, and that it was recovered", "ended_utc: " in meta and "recovered: 1" in meta, repr(meta))
with tarfile.open(os.path.join(root, "20261001-110000.tar.gz")) as tf:
    meta = tf.extractfile("20261001-110000/meta.txt").read().decode()
check("an ended run's meta.txt is not changed", meta == "started_utc: x\nended_utc: y\n", repr(meta))
check("the old single-file log is untouched", open(os.path.join(root, "20261001-100000.csv")).read() == "old single-file log")
check("no half-written copy left", not [n for n in os.listdir(root) if n.endswith(".part")])
again = tidy_logs.tidy(root, log=msgs.append)
check("a second run, with nothing kept, finishes the one that was kept (the busy one is still too new)",
      again == ["20261001-140000"], str(again))
check("and a third has nothing to do", tidy_logs.tidy(root, log=msgs.append) == [])
check("nothing in an empty or missing folder", tidy_logs.tidy(os.path.join(root, "nope")) == [] and tidy_logs.tidy(tempfile.mkdtemp()) == [])

dr = tempfile.mkdtemp()
make(dr, "20261001-100000"); make(dr, "20261001-110000")
check("--dry-run changes nothing", tidy_logs.tidy(dr, dry_run=True, log=msgs.append) == ["20261001-100000"]
      and os.path.isdir(os.path.join(dr, "20261001-100000")) and not os.path.exists(os.path.join(dr, "20261001-100000.tar.gz")))
sys.exit(1 if fails else 0)

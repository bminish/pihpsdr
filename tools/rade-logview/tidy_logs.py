#!/usr/bin/env python3
"""Finishes the RADE parameter logs that were left unfinished: rade-logs/<run>/ directories whose run is over.

    tidy_logs.py [rade-logs-dir] [--keep RUN ...] [--dry-run]

piHPSDR compresses a run (rade-logs/<run>.tar.gz) and removes its directory when the run is stopped, but not when it ends any other
way (the program killed, a crash, power loss): the directory stays, with no ended_utc in its meta.txt, and nothing cleans it up later.

The gate is "all but the newest": run directory names are start times, and only the newest can still be written to (a new run is
always newer than the one before), so every other directory is finished whatever its meta.txt says. For each of them:

  1. meta.txt gets "ended_utc:" (the time of the last file written, which is as near as it can be known) and "recovered: 1", if it has
     no ended_utc;
  2. the directory is compressed to <run>.tar.gz (written under another name and renamed, so a half-written copy is never mistaken for one);
  3. the copy is read back (every member, whole) and compared with the files it was made from, and only then is the directory removed.

A run whose copy is not good is left as it is. The newest directory is never touched; nor is one named with --keep (the viewer passes
what it was asked to open), nor one with a file changed in the last 60 s (something is still writing to it).
Returns the list of runs finished. Standard library only; safe to run at any time, and twice.
"""
import datetime
import os
import re
import shutil
import sys
import tarfile
import time

RUN = re.compile(r"^\d{8}-\d{6}$")
BUSY_S = 60.0


def runs(root):
    """The run directories in root, oldest first."""
    try:
        names = os.listdir(root)
    except OSError:
        return []
    return sorted(n for n in names if RUN.match(n) and os.path.isdir(os.path.join(root, n)))


def newest_file_time(d):
    t = 0.0
    for n in os.listdir(d):
        try:
            t = max(t, os.stat(os.path.join(d, n)).st_mtime)
        except OSError:
            pass
    return t


def mark_ended(d, last):
    """Adds ended_utc and recovered to meta.txt when there is no ended_utc. Returns True if it did."""
    meta = os.path.join(d, "meta.txt")
    try:
        text = open(meta).read()
    except OSError:
        return False
    if re.search(r"^ended_utc:", text, re.M):
        return False
    end = datetime.datetime.fromtimestamp(last or time.time(), datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    with open(meta, "a") as f:
        f.write(("" if text.endswith("\n") or not text else "\n") + "ended_utc: %s\nrecovered: 1\n" % end)
    return True


def copy_is_good(tgz, d, name):
    """The archive reads through whole and holds each file of the directory, byte for byte in size."""
    try:
        want = {n: os.path.getsize(os.path.join(d, n)) for n in os.listdir(d)
                if os.path.isfile(os.path.join(d, n))}
        got = {}
        with tarfile.open(tgz, "r:gz") as tf:
            for m in tf:
                if m.isfile():
                    data = tf.extractfile(m).read()            # reading everything checks the compression's CRC
                    got[os.path.basename(m.name)] = len(data)
        return got == want and bool(want)
    except (OSError, tarfile.TarError, EOFError):
        return False


def tidy(root, keep=(), dry_run=False, log=print):
    """Finish every run directory in root except the newest and those in keep. Returns the names finished."""
    names = runs(root)
    keep = {os.path.basename(os.path.normpath(k)) for k in keep}
    done = []
    for name in names[:-1]:                                    # the newest is the only one that can be live
        d = os.path.join(root, name)
        if name in keep:
            continue
        last = newest_file_time(d)
        if last and time.time() - last < BUSY_S:
            log("tidy_logs: %s was written to %d s ago, left alone" % (name, time.time() - last))
            continue
        tgz = d + ".tar.gz"
        if dry_run:
            log("tidy_logs: would finish %s" % name)
            done.append(name)
            continue
        try:
            mark_ended(d, last)
            if not (os.path.isfile(tgz) and copy_is_good(tgz, d, name)):     # one already made (by piHPSDR) and good is kept
                tmp = tgz + ".part"
                with tarfile.open(tmp, "w:gz") as tf:
                    tf.add(d, arcname=name)
                if not copy_is_good(tmp, d, name):
                    os.remove(tmp)
                    log("tidy_logs: the copy of %s did not check out, left as it is" % name)
                    continue
                os.replace(tmp, tgz)
            shutil.rmtree(d)
            done.append(name)
            log("tidy_logs: %s compressed to %s, directory removed" % (name, os.path.basename(tgz)))
        except OSError as e:
            log("tidy_logs: %s: %s, left as it is" % (name, e))
    return done


def default_root():
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "rade-logs"))


def main(argv):
    args = argv[1:]
    dry = "--dry-run" in args
    keep = []
    rest = []
    it = iter(args)
    for a in it:
        if a == "--keep":
            keep.append(next(it, ""))
        elif a != "--dry-run":
            rest.append(a)
    done = tidy(rest[0] if rest else default_root(), keep, dry)
    print("%d run%s %s" % (len(done), "" if len(done) == 1 else "s", "to finish" if dry else "finished"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

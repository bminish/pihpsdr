#!/usr/bin/env python3
"""
LOCAL TOOL, ours - never sent upstream. Takes every diversity setting out of
a piHPSDR props file, so the next start comes up as a new user's would (the
engine and the menu fall back to their defaults), and so a file written
before the numbering of DIV_AUTO_* and DIV_REF_* changed (LC-059, LC-060)
does not leave the menu on the wrong objective or reference.

    reset_diversity_props.py FILE.props [FILE.props ...]          say what would go
    reset_diversity_props.py --do FILE.props [FILE.props ...]     do it

Removes every line whose key starts "diversity_": the live settings
(diversity_auto_*, diversity_enabled, diversity_gain/phase/cos/sin, the
per-reference windows and thresholds) and the per-mode-group blocks
(diversity_group[N].*). Nothing else in the file is touched, and the order
of the other lines is kept.

The file is named on the command line; the tool guesses no path. piHPSDR
rewrites its props file when it exits, so close the program first or the
removal is written straight back. With --do the original is first copied to
FILE.props.pre-divreset (an existing copy of that name is kept: the oldest
one is the one worth having).
"""
import os
import shutil
import sys

PREFIX = "diversity_"


def split_key(line):
    return line.split("=", 1)[0].strip()


def process(path, do):
    with open(path, "r", errors="replace") as f:
        lines = f.readlines()

    keep, gone = [], []

    for line in lines:
        (gone if split_key(line).startswith(PREFIX) else keep).append(line)

    print("%s: %d lines, %d diversity settings %s" %
          (path, len(lines), len(gone), "removed" if do else "would be removed"))

    if not gone:
        return

    if not do:
        for line in gone:
            print("    " + line.rstrip("\n"))
        return

    backup = path + ".pre-divreset"

    if not os.path.exists(backup):
        shutil.copy2(path, backup)
        print("  original kept as " + backup)
    else:
        print("  " + backup + " already exists, left as it is")

    tmp = path + ".divreset.tmp"

    with open(tmp, "w") as f:
        f.writelines(keep)

    os.replace(tmp, path)


def main(argv):
    do = "--do" in argv
    paths = [a for a in argv if a != "--do"]

    if not paths or any(a.startswith("-") for a in paths):
        print(__doc__)
        return 2

    status = 0

    for p in paths:
        if not os.path.isfile(p):
            print("%s: not a file" % p, file=sys.stderr)
            status = 1
            continue

        process(p, do)

    if not do:
        print("\ndry run - pass --do to write")

    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

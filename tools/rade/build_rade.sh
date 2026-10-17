#!/bin/bash
# Fetch and build librade (freedv/rade_c) at upstream HEAD (rade_c is in
# rapid development and we follow it). Set RADE_REV=<commit|tag|branch> to
# build a specific revision instead, e.g. to reproduce a baseline
# (the baselines in docs/archive/rade/progress.md were measured at b3e3748).
# Output: third_party/rade_c-src (source), third_party/rade_c-build (build).
# Both are untracked.
set -e
REV=${RADE_REV:-origin/HEAD}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/third_party/rade_c-src"
BLD="$ROOT/third_party/rade_c-build"
[ -d "$SRC/.git" ] || git clone https://github.com/freedv/rade_c "$SRC"
git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q --detach "$REV"
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BLD" -j"$(nproc)"
echo "librade commit: $(git -C "$SRC" rev-parse --short HEAD)"
echo "librade: $(ls "$BLD"/src/librade.so*)"

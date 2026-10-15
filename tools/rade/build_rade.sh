#!/bin/bash
# Fetch and build librade (freedv/rade_c) at the pinned commit.
# Output: third_party/rade_c-src (source), third_party/rade_c-build (build).
# Both are untracked. Change PIN here and record it in
# docs/rade-native-progress.md.
set -e
PIN=b3e3748
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/third_party/rade_c-src"
BLD="$ROOT/third_party/rade_c-build"
[ -d "$SRC/.git" ] || git clone https://github.com/freedv/rade_c "$SRC"
git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q "$PIN"
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BLD" -j"$(nproc)"
echo "librade: $(ls "$BLD"/src/librade.so*)"

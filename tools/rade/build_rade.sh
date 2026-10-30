#!/bin/bash
# Fetch and build librade (freedv/rade_c) at upstream HEAD (rade_c is in
# rapid development and we follow it). Set RADE_REV=<commit|tag|branch> to
# build a specific revision instead, e.g. to reproduce a baseline
# (the V1 baselines in docs/archive/rade/progress.md were measured at b3e3748,
# the V2 correlator comparison in docs/diversity-radeV2-cp.md at cc17222).
# Output: third_party/rade_c-src (source), third_party/rade_c-build (build).
# Both are untracked. Run it again to update; the revision built is printed
# and written to third_party/rade_c-build/librade.rev.
# After an update, rebuild piHPSDR (make clean; make RADE_NATIVE=ON).
set -e
for tool in git cmake cc; do
  command -v "$tool" > /dev/null || { echo "build_rade.sh: '$tool' is needed and was not found" >&2; exit 1; }
done
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/third_party/rade_c-src"
BLD="$ROOT/third_party/rade_c-build"
[ -d "$SRC/.git" ] || git clone https://github.com/freedv/rade_c "$SRC"
git -C "$SRC" fetch -q origin
if [ -n "$RADE_REV" ]; then
  REV="$RADE_REV"
else
  git -C "$SRC" remote set-head origin -a > /dev/null 2>&1 || true
  REV=origin/HEAD
fi
git -C "$SRC" checkout -q --detach "$REV"
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BLD" -j"$(nproc)"
LIB=$(ls "$BLD"/src/librade.so* 2> /dev/null | head -1)
[ -n "$LIB" ] || { echo "build_rade.sh: the build finished but $BLD/src/librade.so is missing" >&2; exit 1; }
git -C "$SRC" rev-parse --short HEAD > "$BLD/librade.rev"
echo "librade commit: $(cat "$BLD/librade.rev")"
echo "librade: $LIB"

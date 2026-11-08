#!/bin/bash
# Host test of the break-in timing in src/iambic.c (docs/cw_breakin_plan.md, phase 0).
# Compiles a copy of the real src/iambic.c against the stub headers in shim/ and runs the scenarios
# one process each (the keyer has static state). About 12 seconds. Exit status 0: all expectations met.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../src"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp "$SRC/iambic.c" "$SRC/iambic.h" "$SRC/cwtime.h" "$TMP/"
gcc -O2 -Wall -Wextra -Wno-unused-parameter -DHANG_RATIO="${HANG_RATIO:-0.5}" -I"$HERE/shim" -I"$TMP" -o "$TMP/test_breakin" \
    "$HERE/test_breakin.c" "$TMP/iambic.c" -lpthread -lrt || exit 2
fail=0
for s in 1 2 3 4 5 6 7 8; do
  timeout 30 "$TMP/test_breakin" $s || fail=1
done
[ $fail = 0 ] && echo "ALL SCENARIOS PASSED" || echo "SOME EXPECTATIONS FAILED"
exit $fail

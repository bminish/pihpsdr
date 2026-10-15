#!/usr/bin/env bash
# loss_check.sh - in the manner of rade_c's test/win_loss_check.sh: a
# round trip of a speech file through TX and RX, comparing the feature
# loss of two builds of the signal path. Here the two paths are librade
# alone and librade behind the WDSP TX and RX chains (medium AGC, the
# filters in rade_native.h). PASS if the loss differs by no more than the
# tolerance; the first CLIP_START feature frames are ignored.
#
# Usage: ./loss_check.sh [speech.wav (16 kHz mono)] [tolerance]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."
B="$ROOT/third_party/rade_c-build/src"
WAV="${1:-$ROOT/third_party/rade_c-src/wav/all.wav}"
TOL="${2:-0.01}"
OUT="$HERE/out"; mkdir -p "$OUT"
make -C "$HERE" rade_tx_iq rade_rx_iq wdsp_probe >/dev/null 2>&1
rc=0
for v in v1 v2; do
  opt=""; [ "$v" = v2 ] && opt="--v2"
  echo "=== ${v^^} ==="
  "$B/rade_tx_wav" -v 0 $opt -f "$OUT/lc.$v.feat" "$WAV" "$OUT/lc.$v.real.wav" >/dev/null 2>&1
  "$HERE/rade_tx_iq" $([ "$v" = v2 ] && echo "--v2 --no_bpf") "$OUT/lc.$v.feat" "$OUT/lc.$v.iq.f32" >/dev/null 2>&1
  python3 "$HERE/t11b_chain.py" $opt --prefix lc --tag "lc_$v" --clip-start 25 --tol "$TOL" || rc=1
done
exit $rc

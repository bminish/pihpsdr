#!/bin/bash
#
# Drive cwlat over a test matrix. The things piHPSDR itself must be set to (sidetone path,
# keyer mode, break-in / PTT) are changed by the OPERATOR between groups; this script
# prompts for each. The PipeWire quantum is changed by the script (clock.force-quantum) and
# put back at the end or on Ctrl-C.
#
# usage: run_matrix.sh --sink NODE [--dest 32:0] [--wpm 22] [--out DIR]
#                      [--paths synth,old] [--keys straight,iambicA,iambicB]
#                      [--modes breakin,ptt] [--quanta 64,128,256]
#                      [--trials 60] [--squeeze-trials 36]
#                      [--kiwi HOST:PORT --kfreq KHZ]   also record each test on a KiwiSDR (capture_run.sh)
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SINK=""; DEST="32:0"; WPM=22; OUT=""
KIWI=""; KFREQ=28187
PATHS="synth,old"; KEYS="straight,iambicA,iambicB"; MODES="breakin,ptt"; QUANTA="64,128,256"
TRIALS=60; SQTRIALS=36
while [ $# -gt 0 ]; do
  case "$1" in
    --sink) SINK="$2"; shift 2;;
    --dest) DEST="$2"; shift 2;;
    --wpm) WPM="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --paths) PATHS="$2"; shift 2;;
    --keys) KEYS="$2"; shift 2;;
    --modes) MODES="$2"; shift 2;;
    --quanta) QUANTA="$2"; shift 2;;
    --kiwi) KIWI="$2"; shift 2;;
    --kfreq) KFREQ="$2"; shift 2;;
    --trials) TRIALS="$2"; shift 2;;
    --squeeze-trials) SQTRIALS="$2"; shift 2;;
    *) echo "unknown option $1"; exit 1;;
  esac
done
[ -n "$SINK" ] || { echo "need --sink NODE"; exit 1; }
[ -x "$HERE/cwlat" ] || { echo "build first: $HERE/build.sh"; exit 1; }
[ -n "$OUT" ] || OUT="$HERE/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

ORIG_Q="$(pw-metadata -n settings 2>/dev/null | sed -n "s/.*key:'clock.force-quantum' value:'\([0-9]*\)'.*/\1/p" | tail -1)"
ORIG_Q="${ORIG_Q:-0}"
restore() { pw-metadata -n settings 0 clock.force-quantum "$ORIG_Q" >/dev/null 2>&1; echo "force-quantum restored to $ORIG_Q"; }
trap 'restore; exit 130' INT TERM
trap restore EXIT

cat <<MSG
This keys the radio's CW through the virtual MIDI port (and PTT in the 'ptt' groups).
Before you continue:
  - radio in a safe state for transmit tests (dummy load; drive and PA as you have set them)
  - pihpsdr in CW mode, MIDI device (VirMIDI) enabled in its MIDI menu, keyer speed ${WPM} wpm
  - an operator present for the whole run (Ctrl-C releases every key and PTT)
Results go to: $OUT
MSG
read -r -p "Type SAFE to continue: " ans
[ "$ans" = "SAFE" ] || { echo "aborted"; exit 1; }

IFS=, read -ra PA <<< "$PATHS"; IFS=, read -ra KA <<< "$KEYS"; IFS=, read -ra MA <<< "$MODES"; IFS=, read -ra QA <<< "$QUANTA"

run() {  # run LABEL FILE test trials [extra]
  local label="$1" file="$2" test="$3" trials="$4"; shift 4
  echo "  -> $test ($trials trials)"
  if [ -n "$KIWI" ]; then
    "$HERE/capture_run.sh" --kiwi "$KIWI" --freq "$KFREQ" --dir "$OUT/captures" --name "$file" --label "$label" -- \
        --confirm-safe --sink "$SINK" --dest "$DEST" --wpm "$WPM" --test "$test" --trials "$trials" --label "$label" "$@"
    rc=$?
    cp "$OUT/captures/$file/$file.cwlat.txt" "$OUT/$file.txt" 2>/dev/null
  else
    "$HERE/cwlat" --confirm-safe --sink "$SINK" --dest "$DEST" --wpm "$WPM" --test "$test" --trials "$trials" \
        --label "$label" --csv "$OUT/$file.csv" "$@" > "$OUT/$file.txt" 2>"$OUT/$file.err"
    rc=$?
  fi
  [ $rc -eq 0 ] || echo "     cwlat exit code $rc (see $OUT/$file.err)"
}

for p in "${PA[@]}"; do for k in "${KA[@]}"; do for m in "${MA[@]}"; do
  case "$p" in synth) ptxt="Synth sidetone: TICKED";; *) ptxt="Synth sidetone: UNTICKED";; esac
  case "$k" in straight) ktxt="Paddle Mode: Straight Key";; iambicA) ktxt="Paddle Mode: Iambic Mode A";; *) ktxt="Paddle Mode: Iambic Mode B";; esac
  case "$m" in breakin) mtxt="CW break-in: ON (PTT not used)"; pttarg="";; *) mtxt="CW break-in: OFF (this test sends PTT itself)"; pttarg="--ptt";; esac
  echo
  echo "=== group: path=$p key=$k mode=$m ==="
  echo "Set pihpsdr:  $ptxt | $ktxt | $mtxt"
  read -r -p "Press Enter when set (s = skip this group): " a
  [ "$a" = "s" ] && continue
  for q in "${QA[@]}"; do
    echo " quantum $q"
    pw-metadata -n settings 0 clock.force-quantum "$q" >/dev/null 2>&1
    sleep 3
    lbl="path=$p key=$k mode=$m Q=$q"
    base="${p}_${k}_${m}_Q${q}"
    if [ "$k" = straight ]; then
      run "$lbl" "${base}_straight" straight "$TRIALS" $pttarg
    else
      run "$lbl" "${base}_dit" dit "$TRIALS" $pttarg
      run "$lbl" "${base}_dah" dah "$TRIALS" $pttarg
      run "$lbl" "${base}_squeeze" squeeze "$SQTRIALS" $pttarg
    fi
  done
done; done; done

cat "$OUT"/*.txt > "$OUT/all.txt" 2>/dev/null
echo
echo "Done. Combined text results: $OUT/all.txt  (per-trial CSVs alongside)"

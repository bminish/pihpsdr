#!/bin/bash
#
# Run ONE cwlat test while a KiwiSDR records the radio's signal (IQ, GPS time-stamped).
# Capture only - no analysis. Everything needed to line the recording up with the MIDI
# events later is written next to it.
#
# usage: capture_run.sh --kiwi HOST:PORT --freq KHZ --dir DIR --name NAME [--label TEXT] -- <cwlat args>
#   (cwlat args must not contain --events/--csv, they are added here)
#
# Output in DIR/NAME/ :
#   NAME_.wav           KiwiSDR IQ with GNSS time-stamp chunks (kiwirecorder -m iq -w)
#   NAME.events.csv     every MIDI event: monotonic and UTC send time (cwlat --events)
#   NAME.trials.csv     cwlat per-trial results (MIDI -> sink monitor)
#   NAME.cwlat.txt/.err cwlat report / messages
#   NAME.kiwi.log       kiwirecorder output
#   NAME.meta.json      start/end UTC, settings, clock-sync status, versions
#
set -u
KIWI=""; FREQ=""; DIR=""; NAME=""; LABEL=""
KIWIREC="${KIWIREC:-$HOME/sdr/kiwiclient/kiwirecorder.py}"
HERE="$(cd "$(dirname "$0")" && pwd)"
while [ $# -gt 0 ]; do
  case "$1" in
    --kiwi) KIWI="$2"; shift 2;;
    --freq) FREQ="$2"; shift 2;;
    --dir) DIR="$2"; shift 2;;
    --name) NAME="$2"; shift 2;;
    --label) LABEL="$2"; shift 2;;
    --) shift; break;;
    *) echo "capture_run: unknown option $1"; exit 2;;
  esac
done
[ -n "$KIWI" ] && [ -n "$FREQ" ] && [ -n "$DIR" ] && [ -n "$NAME" ] || { echo "capture_run: need --kiwi --freq --dir --name"; exit 2; }
KHOST="${KIWI%%:*}"; KPORT="${KIWI##*:}"
OUT="$DIR/$NAME"; mkdir -p "$OUT"

clock_status() {
  # the box's own clock is trusted (NTP-synchronised); just record what it says
  echo "utc=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ) $(timeout 3 timedatectl 2>/dev/null | grep -E 'System clock synchronized|NTP service' | tr -s ' ' | tr '\n' ';')"
}
CLK0="$(clock_status)"
START_UTC="$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"

python3 "$KIWIREC" -s "$KHOST" -p "$KPORT" -f "$FREQ" -m iq -w -d "$OUT" --fn "$NAME" --station "" -q \
    --connect-retries 3 --busy-retries 3 > "$OUT/$NAME.kiwi.log" 2>&1 &
KP=$!
CP=""
cleanup() { [ -n "$CP" ] && kill -TERM "$CP" 2>/dev/null; kill -TERM "$KP" 2>/dev/null; }
trap 'cleanup; exit 130' INT TERM

# wait until the Kiwi is delivering data (about 3 s of IQ) - do not key without a recording
for i in $(seq 1 300); do
  [ -f "$OUT/${NAME}_.wav" ] && [ "$(stat -c %s "$OUT/${NAME}_.wav" 2>/dev/null || echo 0)" -gt 150000 ] && break
  kill -0 "$KP" 2>/dev/null || { echo "capture_run: kiwirecorder exited early, see $OUT/$NAME.kiwi.log"; exit 3; }
  sleep 0.1
done
if [ ! -f "$OUT/${NAME}_.wav" ] || [ "$(stat -c %s "$OUT/${NAME}_.wav")" -le 150000 ]; then
  echo "capture_run: no Kiwi data after 30 s, not keying"; cleanup; exit 3
fi

"$HERE/cwlat" "$@" --events "$OUT/$NAME.events.csv" --csv "$OUT/$NAME.trials.csv" > "$OUT/$NAME.cwlat.txt" 2> "$OUT/$NAME.cwlat.err" &
CP=$!
wait "$CP"; RC=$?
CP=""
sleep 3                      # keep recording past the last keying
kill -TERM "$KP" 2>/dev/null; wait "$KP" 2>/dev/null
END_UTC="$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"
CLK1="$(clock_status)"

QUANT="$(pw-metadata -n settings 2>/dev/null | sed -n "s/.*key:'clock.force-quantum' value:'\([0-9]*\)'.*/\1/p" | tail -1)"
COMMIT="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null)"
CWARGS="$*"
python3 - "$OUT/$NAME.meta.json" "$NAME" "$LABEL" "$START_UTC" "$END_UTC" "$KIWI" "$FREQ" "$RC" "$QUANT" "$COMMIT" "$CWARGS" "$CLK0" "$CLK1" <<'PY'
import json, sys
f, name, label, s, e, kiwi, freq, rc, q, commit, args, c0, c1 = sys.argv[1:14]
json.dump({
  "name": name, "label": label, "cwlat_exit_code": int(rc),
  "start_utc": s, "end_utc": e,
  "kiwi": kiwi, "kiwi_freq_khz": float(freq), "kiwi_file": name + "_.wav",
  "kiwi_wav_note": "IQ, 12 kHz, 2 ch int16 interleaved; repeated [kiwi chunk (10 bytes: GNSS time-stamp) + data chunk]; GNSS time is GPS seconds-of-week (no leap-second correction)",
  "pw_force_quantum": q, "repo_commit": commit, "cwlat_args": args,
  "clock_status_start": c0, "clock_status_end": c1,
}, open(f, "w"), indent=2)
PY
exit "$RC"

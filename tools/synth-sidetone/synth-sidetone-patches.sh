#!/bin/bash
#
# The synth side tone work as a patch series against upstream (docs/synth-sidetone/README.md).
#
#   tools/synth-sidetone/synth-sidetone-patches.sh export DIR    write the series into DIR (git format-patch)
#   tools/synth-sidetone/synth-sidetone-patches.sh check [REF]   does it still apply to REF (default: master), compile, and pass the unit test?
#
# The series is the branch feature/synth-sidetone on top of master (override with SYNTH_BASE / SYNTH_WORK):
#   1  the feature   (src/, Makefile)                       applies on its own
#   2  docs, test and tools                                 cannot conflict
#   3  optional, local: PipeWire playback quantum 128       one constant
# The repository does not keep patch files; they are generated here.
#
set -e
cd "$(git rev-parse --show-toplevel)"
BASE=${SYNTH_BASE:-master}
WORK=${SYNTH_WORK:-feature/synth-sidetone}

case "$1" in
export)
  [ -n "$2" ] || { echo "usage: $0 export DIR"; exit 2; }
  mkdir -p "$2"
  rm -f "$2"/0*.patch
  git format-patch -q -o "$2" "$BASE..$WORK"
  echo "series: $WORK ($(git rev-parse --short "$WORK")) on $BASE ($(git rev-parse --short "$BASE"))"
  for f in "$2"/0*.patch; do printf '%-70s %5d lines  %2d files\n' "$(basename "$f")" "$(wc -l < "$f")" "$(grep -c '^diff --git' "$f")"; done
  ;;
check)
  ref="${2:-$BASE}"
  tmp=$(mktemp -d)
  git format-patch -q -o "$tmp/p" "$BASE..$WORK"
  git worktree add -q --detach "$tmp/tree" "$ref"
  cd "$tmp/tree"
  ok=1
  echo "trying the series on $ref ($(git rev-parse --short HEAD))"
  if git -c user.name=check -c user.email=check@localhost am -q --3way "$tmp"/p/0*.patch > "$tmp/am.log" 2>&1; then
    echo "  apply:   all $(ls "$tmp"/p/0*.patch | wc -l) patches apply"
  else
    ok=0; echo "  apply:   FAILS:"; sed 's/^/           /' "$tmp/am.log" | head -20
  fi
  if [ $ok = 1 ]; then
    printf 'GPIO=OFF\nAUDIO=PIPEWIRE\n' > make.config.pihpsdr
    for a in PIPEWIRE PULSE PORTAUDIO; do
      objs="src/transmitter.o src/cw_menu.o src/radio.o src/actions.o"
      [ $a = PIPEWIRE ] && objs="$objs src/pipewire.o src/sidetone.o"
      if make -B AUDIO=$a -k $objs > "$tmp/build-$a.log" 2>&1; then
        echo "  build:   AUDIO=$a: the touched files compile"
      else
        ok=0; echo "  build:   AUDIO=$a FAILS:"; grep -E "error" "$tmp/build-$a.log" | head -10 | sed 's/^/           /'
      fi
    done
    if gcc -O2 -I src -o "$tmp/test_sidetone" test/sidetone/test_sidetone.c src/sidetone.c -lm 2> "$tmp/test.log" && "$tmp/test_sidetone" > "$tmp/test.out"; then
      echo "  test:    unit test runs: $(grep -m1 'jitter +-3 ms' "$tmp/test.out" | sed 's/  */ /g')"
    else
      ok=0; echo "  test:    unit test FAILS"; head -5 "$tmp/test.log"
    fi
    if gcc -O2 -I src -o "$tmp/test_sidetone_idle" test/sidetone/test_sidetone_idle.c src/sidetone.c -lm 2> "$tmp/test2.log" && "$tmp/test_sidetone_idle" > "$tmp/test2.out"; then
      echo "  test:    idle/start-of-TX test: $(tail -1 "$tmp/test2.out")"
    else
      ok=0; echo "  test:    idle/start-of-TX test FAILS"; head -5 "$tmp/test2.log"; grep FAIL "$tmp/test2.out" | head -5
    fi
  fi
  cd - > /dev/null
  git worktree remove --force "$tmp/tree"
  rm -rf "$tmp"
  if [ $ok = 1 ]; then echo "OK: $ref takes the synth side tone series"; exit 0; fi
  echo "NOT OK"; exit 1
  ;;
*)
  echo "usage: $0 export DIR | check [REF]"; exit 2
  ;;
esac

#!/bin/bash
#
# The native RADE work as a set of patches against whatever upstream is now.
#
#   tools/rade/rade-patches.sh export DIR      write the patch set into DIR
#   tools/rade/rade-patches.sh check [REF]     does it still apply to REF (default upstream/master) and compile?
#
# The work is kept in three layers (docs/rade/upstreaming.md):
#   1 hooks   the lines in files upstream owns (HOOK_FILES): small, each behind #ifdef RADE_NATIVE; the only layer that can conflict
#   2 core    files that are only ours and sit in src/ and the top directory (src/rade_*, rade_native.mk): cannot conflict
#   3 extras  tools/, test/, docs/ (reporter, log viewer, test harness, documents): cannot conflict, nothing in them is built into piHPSDR
#
# BASE is the commit the work started from: the parent of the first commit that added docs/rade-native.md or src/rade_native.c. It is only used to
# say what "ours" is; REF is where the patches are tried.
#
set -e
cd "$(git rev-parse --show-toplevel)"

HOOK_FILES="Makefile src/actions.c src/actions.h src/new_menu.c src/new_menu.h src/radio.c src/receiver.c src/transmitter.c"
CORE_PATHS="src/rade_*.c src/rade_*.h rade_native.mk"
EXTRA_PATHS="tools test docs/rade docs/archive/rade"

base() {
  if [ -n "$RADE_BASE" ]; then echo "$RADE_BASE"; return; fi
  git rev-parse "$(git log --diff-filter=A --format=%H -- docs/rade-native.md src/rade_native.c | tail -1)^"
}

make_patches() {          # $1 = directory, $2 = what the work is made against (default BASE), $3 = the work (default HEAD)
  local out="$1" b="${2:-$(base)}" w="${3:-HEAD}"
  mkdir -p "$out"
  git diff --binary "$b" "$w" -- $HOOK_FILES > "$out/0001-rade-native-hooks.patch"
  git diff --binary "$b" "$w" -- $CORE_PATHS > "$out/0002-rade-native-core.patch"
  git diff --binary "$b" "$w" -- $EXTRA_PATHS ':!*/out/*' > "$out/0003-rade-native-tools-tests-docs.patch"
  for f in "$out"/000*.patch; do printf '%-45s %6d lines  %3d files\n' "$(basename "$f")" "$(wc -l < "$f")" "$(grep -c '^diff --git' "$f")"; done
}

case "$1" in
export)
  [ -n "$2" ] || { echo "usage: $0 export DIR"; exit 2; }
  echo "work: $(git rev-parse --short HEAD) against $(git rev-parse --short "$(base)")"
  make_patches "$2"
  ;;
check)
  ref="${2:-upstream/master}"
  [ "$2" ] || git fetch -q upstream
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp/patches"' EXIT
  make_patches "$tmp/patches" > /dev/null
  wt="$tmp/tree"
  git worktree add -q --detach "$wt" "$ref"
  echo "trying the patches on $ref ($(git rev-parse --short "$ref"))"
  cd "$wt"
  ok=1
  if git apply --3way "$tmp/patches/0001-rade-native-hooks.patch" > "$tmp/hooks.log" 2>&1; then
    echo "  hooks:  apply cleanly"
  else
    ok=0
    echo "  hooks:  CONFLICTS (or a file moved):"; grep -E "conflict|error" "$tmp/hooks.log" | sed 's/^/          /'
  fi
  git apply "$tmp/patches/0002-rade-native-core.patch" && echo "  core:   applies" || { ok=0; echo "  core:   FAILS to apply"; }
  git apply "$tmp/patches/0003-rade-native-tools-tests-docs.patch" && echo "  extras: apply" || { ok=0; echo "  extras: FAIL to apply"; }
  if [ $ok = 1 ]; then
    # objects only (no linking): enough to catch an upstream API we call having changed
    ln -s "$OLDPWD/third_party" third_party 2>/dev/null || true
    objs="src/rade_native.o src/rade_native_menu.o src/rade_eoo_text.o src/rade_v2_text.o src/rade_launch.o src/rade_native_log.o src/rade_native_test.o src/receiver.o src/transmitter.o src/radio.o src/actions.o src/new_menu.o"
    if make RADE_NATIVE=ON -k $objs > "$tmp/build.log" 2>&1; then
      echo "  build:  all RADE objects and the files with hooks compile"
    else
      ok=0
      echo "  build:  FAILS:"; grep -E "error|Error" "$tmp/build.log" | head -20 | sed 's/^/          /'
    fi
  fi
  cd "$OLDPWD"
  if [ $ok = 1 ]; then git worktree remove --force "$wt"; rm -rf "$tmp"; echo "OK: $ref takes the RADE patches"; exit 0; fi
  echo "NOT OK. The tree with the patches half applied is $wt (remove with: git worktree remove --force $wt)"
  exit 1
  ;;
*)
  echo "usage: $0 export DIR | check [REF]"; exit 2
  ;;
esac

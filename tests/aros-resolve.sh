#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# RESOLVE's prediction against the real loader, on hosted AROS. Two builds
# of one library, pkgtest.library 1.0 and 2.30 (tools/build-aros-testlib.sh),
# and C:TestLib, which really calls OpenLibrary() and prints the version it
# got. Each boot starts with nothing loaded:
#
#   boot 1  a file with no resident tag first in the path, 2.30 in SYS:Libs:
#           RESOLVE says the file is passed over and SYS:Libs taken; TestLib
#           gets 2.30. Then RESOLVE reports the copy in memory.
#   boot 2  1.0 in the program's libs/, 2.30 in SYS:Libs: RESOLVE says libs/
#           is taken; TestLib gets 1.0. Then RESOLVE reports the loaded 1.0 as
#           the winner even from another directory, and with VERSION 2.30
#           refuses (18), telling to flush it.
#
# What AROS did is read back on the host. Leaves the hosted tree as it found
# it, and refuses to run while another instance is up.
set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_build=${AROS_BUILD:-"$HOME/Source/aros-project/aros-build"}
aros_tree="$aros_build/bin/darwin-aarch64/AROS"
macaros_root=${MACAROS_ROOT:-"$repo_root/../Macaros"}
[ -x "$macaros_root/graft/aros-ctl" ] || macaros_root="$HOME/aros-next/research/src/Macaros"
control="$macaros_root/graft/aros-ctl"
aros_pkg="$repo_root/build/aros/Pkg"
v1="$repo_root/build/aros/pkgtest-1.0.library"
v2="$repo_root/build/aros/pkgtest-2.30.library"
testlib="$repo_root/build/aros/TestLib"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-aros-resolve.XXXXXX")
share="$work/share"
aros_started=0
placed=""
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    for f in $placed; do [ -e "$f" ] && rm -f "$f"; done
    if [ "${PKG_KEEP:-0}" = 1 ]; then echo "aros-resolve: keeping $work" >&2; return; fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
for need in "$aros_pkg" "$v1" "$v2" "$testlib" "$control"; do
    [ -e "$need" ] || { echo "aros-resolve: missing $need (sh tools/build-aros.sh; sh tools/build-aros-testlib.sh)" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || { echo "aros-resolve: a hosted AROS is running; refusing to disturb it" >&2; exit 75; }
for f in "$aros_tree/C/Pkg" "$aros_tree/Libs/pkgtest.library"; do
    [ ! -e "$f" ] || { echo "aros-resolve: $f exists; refusing to replace it" >&2; exit 73; }
done
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

mkdir -p "$share"
cp "$v1" "$share/pkgtest-1.0.library"
cp "$testlib" "$share/TestLib"
# the negative control: the 2.30 build with its resident tag's marker cleared
python3 - "$v2" "$share/broken.library" <<'PY'
import sys
b = bytearray(open(sys.argv[1], "rb").read())
i = b.find(b"\xfc\x4a")
while i >= 0 and i % 8:
    i = b.find(b"\xfc\x4a", i + 1)
b[i:i + 2] = b"\0\0"
open(sys.argv[2], "wb").write(b)
PY
cp "$aros_pkg" "$aros_tree/C/Pkg";                  placed="$placed $aros_tree/C/Pkg"
cp "$v2" "$aros_tree/Libs/pkgtest.library";        placed="$placed $aros_tree/Libs/pkgtest.library"

boot() {  # boot <name> <startup lines>
    rm -f "$share/done"
    AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$2
C:Echo done >MacRW:done" "$control" run > /dev/null
    aros_started=1
    w=0
    while [ ! -f "$share/done" ] && [ $w -lt 90 ]; do sleep 1; w=$((w + 1)); done
    "$control" stop > /dev/null 2>&1 || true
    aros_started=0
    [ -f "$share/done" ];                             ok $? "$1 ran to its end in $w s"
}

echo "aros-resolve: boot 1, a file with no resident tag first in the path"
boot "boot 1" 'C:FailAt 21
C:MakeDir RAM:bad RAM:bad/libs
C:Copy MacRW:broken.library RAM:bad/libs/pkgtest.library QUIET
C:Pkg RESOLVE pkgtest.library FROM RAM:bad MACHINE >MacRW:b1-predict.out
C:CD RAM:bad
MacRW:TestLib pkgtest.library >MacRW:b1-load.out
C:CD SYS:
C:Pkg RESOLVE pkgtest.library FROM RAM:bad MACHINE >MacRW:b1-after.out'
has "$share/b1-predict.out" '^winner: [^ ]*:Libs/pkgtest.library 2.30$' \
  && has "$share/b1-predict.out" 'RAM:bad/libs/pkgtest.library not a library (no resident tag)'
                                                      ok $? "RESOLVE predicts: the file with no tag passed over, SYS:Libs/pkgtest.library 2.30 taken"
has "$share/b1-load.out" 'opened -- version 2.30';   ok $? "and OpenLibrary really gets 2.30 ($(head -1 "$share/b1-load.out" 2>/dev/null))"
has "$share/b1-after.out" '^loaded: pkgtest.library 2.30 ' && has "$share/b1-after.out" '^winner: memory 2.30$'
                                                      ok $? "once loaded, RESOLVE reports the copy in memory as the winner"

echo "aros-resolve: boot 2, an older copy in the program's libs/"
boot "boot 2" 'C:FailAt 21
C:MakeDir RAM:game RAM:game/libs RAM:empty
C:Copy MacRW:pkgtest-1.0.library RAM:game/libs/pkgtest.library QUIET
C:Pkg RESOLVE pkgtest.library FROM RAM:game MACHINE >MacRW:b2-predict.out
C:CD RAM:game
MacRW:TestLib pkgtest.library >MacRW:b2-load.out
C:CD SYS:
C:Pkg RESOLVE pkgtest.library FROM RAM:empty MACHINE >MacRW:b2-after.out
C:Pkg RESOLVE pkgtest.library FROM RAM:empty VERSION 2.30 MACHINE >MacRW:b2-old.out
C:Echo "$RC" >MacRW:b2-old.rc
C:Pkg RESOLVE pkgtest.library FROM RAM:game VERSION 2.30 >MacRW:b2-human.out'
has "$share/b2-predict.out" '^winner: RAM:game/libs/pkgtest.library 1.0$' \
  && has "$share/b2-predict.out" 'hides the newer [^ ]*:Libs/pkgtest.library 2.30'
                                                      ok $? "RESOLVE predicts: RAM:game/libs/pkgtest.library 1.0 taken, hiding SYS:Libs 2.30"
has "$share/b2-load.out" 'opened -- version 1.0';    ok $? "and OpenLibrary really gets 1.0 ($(head -1 "$share/b2-load.out" 2>/dev/null))"
has "$share/b2-after.out" '^winner: memory 1.0$' && has "$share/b2-after.out" 'not consulted: the copy in memory is used'
                                                      ok $? "the loaded 1.0 wins even from a directory with no copy, files not consulted"
[ "$(tr -d ' \r\n' < "$share/b2-old.rc" 2>/dev/null)" = 18 ] && has "$share/b2-old.out" 'Avail FLUSH'
                                                      ok $? "asked for 2.30 while 1.0 is loaded: refused with 18, telling to flush it"

echo
echo "aros-resolve: $checks checks, $fails failures"
if [ "$fails" -ne 0 ] || [ "${PKG_SHOW:-0}" = 1 ]; then
    for f in "$share"/*.out; do [ -f "$f" ] && { echo "--- $(basename "$f")"; cat "$f"; }; done
fi
[ "$fails" -eq 0 ]

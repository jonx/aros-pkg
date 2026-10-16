#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# M3: Pkg installs the real AFS+ handler on hosted AROS, and AROS loads the
# handler Pkg installed. Four boots of hosted AROS, so "restart" means a restart:
#
#   boot 1  install revision 14; mount; the running handler reports 14.
#           upgrade to 15 while the volume is mounted: the file is replaced,
#           the loaded handler keeps running, and still reports 14.
#   boot 2  the same volume after a restart reports 15.
#           upgrade to 16, a handler that cannot load.
#   boot 3  the handler fails to start; the startup sequence sees the error
#           and runs Pkg ROLLBACK, which falls back to 15.
#   boot 4  after the fallback and a restart, the volume reports 15 again.
#
# Which handler is running is read from the handler itself, through the AFS+
# extension transport, by PkgHandlerRev. The root is on MacRW:, the host share,
# because a restart must not erase it. AFS+ is used read-only; the shared hosted
# AROS tree is left as it was found, apart from two commands copied into C: for
# the duration and removed after.
#
# Needs two handler packages built by AFS+'s own tools/package-aros-alpha0.sh at
# interface revisions 14 and 15:
#   PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/aros-handler.sh

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_tree="${AROS_BUILD:-$HOME/Source/aros-project/aros-build}/bin/darwin-aarch64/AROS"
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
v14="${PKG_HANDLER_V14:?set PKG_HANDLER_V14 to a revision 14 handler package}"
v15="${PKG_HANDLER_V15:?set PKG_HANDLER_V15 to a revision 15 handler package}"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-aros-handler.XXXXXX")
share="$work/share"
aros_started=0
copied=""

cleanup() {
    status=$?
    [ "$aros_started" = 0 ] || "$control" stop >/dev/null 2>&1 || true
    for f in $copied; do [ ! -e "$f" ] || unlink "$f"; done
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "aros-handler: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$repo_root/build/aros/PkgHandlerRev" \
            "$v14/afsplus-handler" "$v15/afsplus-handler" "$v14/Unit19" "$v15/Unit19" "$control"; do
    [ -e "$need" ] || { echo "aros-handler: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-handler: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
for c in Pkg PkgHandlerRev; do
    [ ! -e "$aros_tree/C/$c" ] || { echo "aros-handler: $aros_tree/C/$c exists; refusing to replace it" >&2; exit 73; }
done

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# ---- host side ---------------------------------------------------------

mkdir -p "$share/images" "$work/d14/L" "$work/d15/L" "$work/d16/L"
cp "$v14/afsplus-handler" "$work/d14/L/afsplus-handler"
cp "$v15/afsplus-handler" "$work/d15/L/afsplus-handler"
printf 'this is not a loadable handler\n' > "$work/d16/L/afsplus-handler"
# AFS+ changes its on-disk format without keeping legacy readers, and it did
# between these two builds: a revision 14 handler refuses an image made by the
# revision 15 tools. So each boot gets an image in its handler's own format. The
# claim under test is which handler binary AROS loads, and that does not depend
# on the image; the first run, which gave both boots the newer image, is what
# showed the refusal.
cp "$v14/Unit19" "$share/images/Unit19"
cmp -s "$work/d14/L/afsplus-handler" "$work/d15/L/afsplus-handler"
[ $? -ne 0 ];                                         ok $? "the two handler builds differ"

"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
export PKG_SIGNKEY="$work/dev.key"
for v in 14 15 16; do
    "$host_pkg" PUBLISH "$work/d$v" CHANNEL "$share/channel" \
        NAME afsplus-handler VERSION "$v" KIND device > /dev/null
                                                      ok $? "publish handler $v"
done

# The DOSDriver points at the handler Pkg installs, under the root on MacRW:.
sed 's|^FileSystem *=.*|FileSystem      = MacRW:sys/L/afsplus-handler|' \
    "$v15/AFSPLUS19" > "$share/AFSPKG19"
has "$share/AFSPKG19" 'MacRW:sys/L/afsplus-handler';  ok $? "the DOSDriver names the installed handler"

for c in Pkg PkgHandlerRev; do
    cp "$repo_root/build/aros/$c" "$aros_tree/C/$c"
    copied="$copied $aros_tree/C/$c"
done

PRE='C:FailAt 21
Assign "FDSK:" "MacRW:images"'
# Mount loads the handler named by the DOSDriver there and then, so the file
# must be in place first: installing before mounting is the order that works,
# and the first run of this test, which mounted first, showed it.
MOUNTCMD='C:Mount MacRW:AFSPKG19 >MacRW:mount.out'
MOUNT="$PRE
$MOUNTCMD"

boot() {  # n startup
    AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$2
C:Echo done >MacRW:boot$1.done" "$control" run > /dev/null 2>&1
    aros_started=1
    w=0
    while [ ! -f "$share/boot$1.done" ] && [ "$w" -lt 90 ]; do sleep 1; w=$((w + 1)); done
    "$control" stop > /dev/null 2>&1 || true
    aros_started=0
    [ -f "$share/boot$1.done" ];                      ok $? "boot $1 ran its startup to the end"
}

# ---- boot 1 ------------------------------------------------------------

echo "aros-handler: boot 1"
boot 1 "$PRE
C:Pkg INSTALL afsplus-handler VERSION 14 ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:b1-install.out
$MOUNTCMD
C:PkgHandlerRev AFSPKG19: >MacRW:b1-rev-before.out
C:Pkg UPGRADE afsplus-handler VERSION 15 ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:b1-upgrade.out
C:PkgHandlerRev AFSPKG19: >MacRW:b1-rev-after.out
C:Pkg VERIFY afsplus-handler ROOT MacRW:sys >MacRW:b1-verify.out"

has "$share/b1-install.out" 'installed afsplus-handler 14';     ok $? "boot 1: Pkg installed revision 14"
has "$share/b1-rev-before.out" '^revision 14$';                 ok $? "boot 1: AROS runs the handler Pkg installed, revision 14"
has "$share/b1-upgrade.out" 'upgraded afsplus-handler from 14 to 15'; ok $? "boot 1: upgrade to 15 while mounted"
has "$share/b1-rev-after.out" '^revision 14$';                  ok $? "boot 1: the loaded handler keeps running until restart"
cmp -s "$work/d15/L/afsplus-handler" "$share/sys/L/afsplus-handler"; ok $? "boot 1: the file on disk is already 15"
has "$share/b1-verify.out" 'all intact';                        ok $? "boot 1: verify passes"

# ---- boot 2 ------------------------------------------------------------

echo "aros-handler: boot 2"
cp "$v15/Unit19" "$share/images/Unit19"
boot 2 "$MOUNT
C:PkgHandlerRev AFSPKG19: >MacRW:b2-rev.out
C:Pkg UPGRADE afsplus-handler VERSION 16 ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:b2-upgrade.out"

has "$share/b2-rev.out" '^revision 15$';                        ok $? "boot 2: after a restart the volume is served by 15"
has "$share/b2-upgrade.out" 'upgraded afsplus-handler from 15 to 16'; ok $? "boot 2: upgrade to a handler that cannot load"

# ---- boot 3 ------------------------------------------------------------

echo "aros-handler: boot 3"
boot 3 "$MOUNT
C:PkgHandlerRev AFSPKG19: >MacRW:b3-rev.out
If ERROR
    C:Echo failed >MacRW:b3-start.status
    C:Pkg ROLLBACK afsplus-handler ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:b3-rollback.out
Else
    C:Echo started >MacRW:b3-start.status
EndIf"

[ "$(cat "$share/b3-start.status" 2>/dev/null)" = failed ];     ok $? "boot 3: the broken handler fails to start, and AmigaDOS sees it"
has "$share/b3-rollback.out" 'rolled back afsplus-handler from 16 to 15'; ok $? "boot 3: the startup sequence falls back with Pkg ROLLBACK"
cmp -s "$work/d15/L/afsplus-handler" "$share/sys/L/afsplus-handler"; ok $? "boot 3: the file on disk is 15 again"

# ---- boot 4 ------------------------------------------------------------

echo "aros-handler: boot 4"
boot 4 "$MOUNT
C:PkgHandlerRev AFSPKG19: >MacRW:b4-rev.out
C:Pkg VERIFY afsplus-handler ROOT MacRW:sys >MacRW:b4-verify.out
C:Pkg LIST ROOT MacRW:sys >MacRW:b4-list.out"

has "$share/b4-rev.out" '^revision 15$';                        ok $? "boot 4: after the fallback and a restart the volume is served by 15"
has "$share/b4-verify.out" 'all intact';                        ok $? "boot 4: verify passes"
has "$share/b4-list.out" '^afsplus-handler  *15  *device';      ok $? "boot 4: the database says 15, kind device"

echo
echo "aros-handler: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$share"/*.out "$share"/*.status; do
        [ -f "$f" ] && { echo "--- $(basename "$f")"; cat "$f"; }
    done
fi
[ "$fails" -eq 0 ]

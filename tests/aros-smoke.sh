#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# First run of the Pkg client on hosted AROS. The channel is published on the
# host by the host build of pkg; the AROS build installs from it, through the
# MacRW: host share, into RAM:, then verifies, lists, copies the installed file
# back out for a byte comparison on the host, removes, and refuses a tampered
# payload. Results are read back on the host, never taken from AROS's word.
#
# Leaves the shared hosted AROS tree as it found it, and refuses to run while
# another instance is up.

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_build=${AROS_BUILD:-"$HOME/Source/aros-project/aros-build"}
aros_tree="$aros_build/bin/darwin-aarch64/AROS"
macaros_root=${MACAROS_ROOT:-"$repo_root/../Macaros"}
control="$macaros_root/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
aros_pkg="$repo_root/build/aros/Pkg"
target="$aros_tree/C/Pkg"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-aros-smoke.XXXXXX")
share="$work/share"
aros_started=0
installed=0

cleanup() {
    status=$?
    if [ "$aros_started" = 1 ]; then "$control" stop >/dev/null 2>&1 || true; fi
    if [ "$installed" = 1 ] && [ -e "$target" ]; then unlink "$target"; fi
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "aros-smoke: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$host_pkg" "$aros_pkg" "$control"; do
    [ -x "$need" ] || { echo "aros-smoke: missing $need (make; sh tools/build-aros.sh)" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-smoke: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
[ ! -e "$target" ] || { echo "aros-smoke: $target already exists; refusing to replace it" >&2; exit 73; }

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

# Host side: a key, a drawer, a channel inside the share.
mkdir -p "$share" "$work/drawer/C" "$work/drawer/Libs"
printf 'binary\000$VER: Hello 1.2 (18.9.2026)\000tail' > "$work/drawer/C/Hello"
printf 'library data\n' > "$work/drawer/Libs/data.txt"
# Script and Delete-forbidden from .ameta, Execute forbidden from the host
# mode (0644), and a Latin-1 comment: AROS must end up with exactly these.
chmod 644 "$work/drawer/C/Hello"
printf 'ameta 1\nfile Hello\nprot 0x00000041\ncomment Says%%20hello%%20%%C3%%A9\n' > "$work/drawer/C/.ameta"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/drawer" CHANNEL "$share/channel" KIND application > /dev/null
mkdir -p "$work/placed/Games/Placed" "$share/games"
printf 'placed application\n' > "$work/placed/Games/Placed/Placed"
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/placed" CHANNEL "$share/placement-channel" KIND application NAME placed VERSION 1 > /dev/null
digest=$(awk '$1=="hello"{print $4}' "$share/channel/index")
payload=$(awk '/^Payload:/{print $2}' "$share/channel/objects/$digest.manifest")
cp -R "$share/channel" "$share/tampered"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$share/tampered/objects/$payload.pkg"

cp "$aros_pkg" "$target"
installed=1

echo "aros-smoke: booting hosted AROS"
AROS_CTL_HOST_FOLDER="$share" \
AROS_CTL_STARTUP_EXTRA='C:FailAt 21
C:Pkg LIST ROOT RAM:root >MacRW:list0.out
C:Pkg INSTALL hello ROOT RAM:root CHANNEL MacRW:channel >MacRW:install.out
If ERROR
    C:Echo fail >MacRW:install.rc
Else
    C:Echo pass >MacRW:install.rc
EndIf
C:Pkg VERIFY hello ROOT RAM:root >MacRW:verify.out
C:List RAM:root/C >MacRW:attrs.out
C:Pkg LIST ROOT RAM:root >MacRW:list.out
C:Copy RAM:root/C/Hello MacRW:hello.copy
C:Pkg INSTALL hello ROOT RAM:other CHANNEL MacRW:tampered >MacRW:tamper.out
If ERROR
    C:Echo refused >MacRW:tamper.rc
Else
    C:Echo accepted >MacRW:tamper.rc
EndIf
C:Pkg REMOVE hello ROOT RAM:root >MacRW:remove.out
C:List RAM:root ALL >MacRW:after-remove.out
C:Pkg INSTALL placed ROOT RAM:placement CHANNEL MacRW:placement-channel AT MacRW:games >MacRW:placement-install.out
C:Pkg VERIFY placed ROOT RAM:placement >MacRW:placement-verify.out
C:Copy MacRW:games/Placed/Placed MacRW:placed.copy
C:Pkg REMOVE placed ROOT RAM:placement >MacRW:placement-remove.out
C:Echo done >MacRW:done' \
    "$control" run > /dev/null
aros_started=1

waited=0
while [ ! -f "$share/done" ] && [ "$waited" -lt 60 ]; do sleep 1; waited=$((waited + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                 ok $? "the startup ran to its end within 60 s"

has "$share/list0.out" 'nothing installed';           ok $? "an empty root lists as empty"
[ "$(cat "$share/install.rc" 2>/dev/null)" = pass ];  ok $? "install returns success to AmigaDOS"
has "$share/install.out" 'installed hello 1.2';       ok $? "install reports what it did"
has "$share/install.out" 'signed by';                 ok $? "the Ed25519 signature was verified on AROS"
has "$share/verify.out" 'all intact';                 ok $? "verify passes on AROS"
LC_ALL=C grep -a -q -- ' s--rw-- ' "$share/attrs.out"; ok $? "AROS holds the protection word from .ameta and the host mode: s--rw--"
printf ': Says hello \351\n' > "$work/comment.want"
LC_ALL=C grep -a -q -F -f "$work/comment.want" "$share/attrs.out"
                                                      ok $? "and the comment, in Latin-1"
has "$share/list.out" '^hello';                       ok $? "list shows the package"
cmp -s "$work/drawer/C/Hello" "$share/hello.copy";    ok $? "the installed bytes equal the drawer's, compared on the host"
[ "$(cat "$share/tamper.rc" 2>/dev/null)" = refused ]; ok $? "a tampered payload is refused, and AmigaDOS sees the error"
has "$share/tamper.out" "expected $payload";           ok $? "the refusal names the expected digest"
has "$share/remove.out" 'removed hello 1.2';          ok $? "remove reports what it did"
! has "$share/after-remove.out" 'Hello';              ok $? "nothing of the package is left in RAM:root"
has "$share/placement-install.out" 'installed placed 1'; ok $? "AT installs through an AROS assign on another volume"
has "$share/placement-verify.out" 'all intact';       ok $? "AROS verify follows its recorded destination"
cmp -s "$work/placed/Games/Placed/Placed" "$share/placed.copy"; ok $? "relocated AROS bytes match the signed package"
has "$share/placement-remove.out" 'removed placed 1'; ok $? "AROS remove follows its recorded destination"
[ ! -e "$share/games/Placed/Placed" ];               ok $? "AROS removal clears the relocated file"

echo
echo "aros-smoke: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$share"/*.out "$share"/*.rc; do
        [ -f "$f" ] && { echo "--- $(basename "$f")"; cat "$f"; }
    done
fi
[ "$fails" -eq 0 ]

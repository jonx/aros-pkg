#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Goal 2: an application arrives, its dependencies arrive by Pkg, it runs, and
# it is checked from outside, with no ARexx anywhere.
#
#   1. On macOS: Guru, the alert decoder that ships with identify.library in
#      the AROS sources, is published as a signed FFS image at 2.0 and 2.1,
#      depending on identify.library, published as a system component. The
#      AROS FFS handler is published as a component too: hosted AROS has none.
#   2. On hosted AROS, one AmigaDOS startup script and nothing else: Pkg
#      bootstraps and installs itself, installs Guru with its dependency,
#      the image is mounted and Guru runs from it, then upgrade, rollback, the
#      refusals, Pkg removed with Guru still running, and last Guru removed
#      and the library taken out as an orphan.
#   3. Every Pkg step runs with MACHINE, and every exit code is read by
#      AmigaDOS into $RC, both checked here against the class table.
#   4. A tampered image and a badly signed dependency are refused, and Guru
#      runs after Pkg has removed itself.
#
# What decides pass or fail is read on the host, from files AROS wrote into
# the share: Guru's output against the strings in its own sources, the
# volume's listing, the root's files, and the exit codes.
#
# Needs: make; sh tools/build-aros.sh; and the AROS FFS handler and the
# identify pieces in build/aros (see README, "Goal 2").

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
aros_tree=${AROS_TREE:-"$HOME/Source/aros-project/aros-build/bin/darwin-aarch64/AROS"}
host_pkg="$repo_root/build/pkg"
b="$repo_root/build/aros"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-goal2.XXXXXX")
share="$work/share"
aros_started=0

cleanup() {
    status=$?
    [ "$aros_started" = 0 ] || "$control" stop >/dev/null 2>&1 || true
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "goal2: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$host_pkg" "$b/Pkg" "$b/afs-handler" "$b/identify/Guru" "$b/identify/Function" \
            "$b/identify/identify.library" "$control"; do
    [ -e "$need" ] || { echo "goal2: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "goal2: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# ---- the premises ----------------------------------------------------------

echo "goal2 0: what the hosted system lacks"
[ ! -e "$aros_tree/Libs/identify.library" ];          ok $? "hosted AROS ships no identify.library: it can only come from Pkg"
[ ! -e "$aros_tree/C/rexx" ] && [ ! -e "$aros_tree/C/RX" ] && [ ! -e "$aros_tree/System/RexxMast" ]
                                                      ok $? "hosted AROS ships no ARexx interpreter: no rexx, no RX, no RexxMast"

# ---- 1. macOS publishes ----------------------------------------------------

echo "goal2 1: macOS publishes"
mkdir -p "$share/out" "$work/afs/L" "$work/id/Libs" "$work/g20/C" "$work/g21/C" "$work/self/C"
cp "$b/afs-handler" "$work/afs/L/afs-handler"
cp "$b/identify/identify.library" "$work/id/Libs/identify.library"
cp "$b/identify/Guru" "$work/g20/C/Guru"
printf 'Guru decodes an Amiga alert code.\n' > "$work/g20/ReadMe"
cp "$b/identify/Guru" "$b/identify/Function" "$work/g21/C/"
printf 'Guru decodes an Amiga alert code. Function names a library function.\n' > "$work/g21/ReadMe"
cp "$b/Pkg" "$work/self/C/Pkg"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
"$host_pkg" KEYGEN FILE "$work/other.key" > /dev/null
export PKG_SIGNKEY="$work/dev.key"
CH="$share/channel"
"$host_pkg" PUBLISH "$work/afs" CHANNEL "$CH" NAME afs-handler VERSION 41.7 KIND device > /dev/null
                                                      ok $? "the AROS FFS handler published as a device component, 41.7 from its \$VER"
"$host_pkg" PUBLISH "$work/id" CHANNEL "$CH" NAME identify VERSION 37.1 KIND library > /dev/null
                                                      ok $? "identify.library published as a library component, 37.1 from its \$VER"
"$host_pkg" PUBLISH "$work/g20" CHANNEL "$CH" KIND image DEPENDS "identify >= 37.1" > "$work/p20"
                                                      ok $? "Guru 2.0 published as an image depending on identify >= 37.1"
grep -q 'taken from \$VER: in C/Guru' "$work/p20";    ok $? "name and version read from Guru's own \$VER"
"$host_pkg" PUBLISH "$work/g21" CHANNEL "$CH" NAME guru VERSION 2.1 KIND image DEPENDS "identify >= 37.1" > /dev/null
                                                      ok $? "Guru 2.1, adding the Function tool, published the same way"
"$host_pkg" PUBLISH "$work/self" CHANNEL "$CH" NAME pkg VERSION 1 KIND application > /dev/null
                                                      ok $? "Pkg itself published"

# The image sizes, and so the mount geometry, come from the signed manifests.
blocks_of() {
    m=$(awk -v v="$1" '$1=="guru" && $2==v{print $4}' "$CH/index")
    awk '/^File: .* guru\.hdf$/{print $3 / 512}' "$CH/objects/$m.manifest"
}
n20=$(blocks_of 2.0)
n21=$(blocks_of 2.1)
[ -n "$n20" ] && [ -n "$n21" ] && [ "$n20" != "$n21" ]; ok $? "the two images differ in size: $n20 and $n21 blocks"
# The mount entries are written on AROS by Pkg MOUNTLIST; the host only
# checks their geometry against these sizes.

# A copy of the channel with one byte of the 2.1 image flipped, and one where
# the dependency's signature is damaged.
cp -R "$CH" "$share/tampered"
m21=$(awk '$1=="guru" && $2=="2.1"{print $4}' "$CH/index")
p21=$(awk '/^Payload:/{print $2}' "$CH/objects/$m21.manifest")
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=1; open(p,'wb').write(b)
" "$share/tampered/objects/$p21.pkg"
cp -R "$CH" "$share/badsig"
mid=$(awk '$1=="identify"{print $4}' "$CH/index")
python3 -c "
import sys
p=sys.argv[1]; s=open(p).read(); i=s.index('Signature: ')+11
c='0' if s[i]!='0' else '1'; open(p,'w').write(s[:i]+c+s[i+1:])
" "$share/badsig/objects/$mid.sig"

gzip -c "$b/Pkg" > "$share/Pkg.gz"
! ls "$share" | grep -i -q rexx;                      ok $? "nothing in the share is an ARexx interpreter"

# ---- 2 to 4. hosted AROS, one AmigaDOS script ------------------------------

P='MacRW:sys/C/Pkg'
S='ROOT MacRW:sys CHANNEL MacRW:channel MACHINE'
step() {  # step <name> <command...>: the command, its output and its $RC
    printf '%s >MacRW:out/%s.o\nC:Echo "$RC" >MacRW:out/%s.rc\n' "$*" "$1" "$1" | sed "s|^$1 ||"
}
script="C:FailAt 21
C:MakeDir RAM:boot
C:Copy MacRW:Pkg.gz RAM:boot/Pkg.gz
C:minigzip -d RAM:boot/Pkg.gz
$(step s01 RAM:boot/Pkg INSTALL pkg $S)
$(step s02 $P INSTALL afs-handler $S)
$(step s03 $P INSTALL guru VERSION 2.0 $S)
$(step s04 $P LIST ROOT MacRW:sys MACHINE)
C:MakeDir RAM:fdsk
Assign FDSK: RAM:fdsk
$(step m20 $P MOUNTLIST guru ROOT MacRW:sys UNIT 20 OUT RAM:GURU0 MACHINE)
C:Copy RAM:GURU0 MacRW:out/GURU0.ml
C:Protect MacRW:sys/guru.hdf w SUB
C:MakeLink RAM:fdsk/Unit20 MacRW:sys/guru.hdf
C:Mount RAM:GURU0
$(step r19 GURU0:C/Guru 04000001)
Assign LIBS: MacRW:sys/Libs ADD
$(step r20 GURU0:C/Guru 04000001)
C:List GURU0: ALL >MacRW:out/l20.o
C:Eject GURU0:
$(step s05 $P UPGRADE guru $S)
$(step m21 $P MOUNTLIST guru ROOT MacRW:sys UNIT 21 OUT RAM:GURU1 MACHINE)
C:Copy RAM:GURU1 MacRW:out/GURU1.ml
C:Protect MacRW:sys/guru.hdf w SUB
C:MakeLink RAM:fdsk/Unit21 MacRW:sys/guru.hdf
C:Mount RAM:GURU1
$(step r21 GURU1:C/Guru 04000001)
C:List GURU1: ALL >MacRW:out/l21.o
C:Eject GURU1:
$(step s06 $P ROLLBACK guru $S)
$(step m22 $P MOUNTLIST guru ROOT MacRW:sys UNIT 22 OUT RAM:GURU2 MACHINE)
C:Copy RAM:GURU2 MacRW:out/GURU2.ml
C:Protect MacRW:sys/guru.hdf w SUB
C:MakeLink RAM:fdsk/Unit22 MacRW:sys/guru.hdf
C:Mount RAM:GURU2
$(step r22 GURU2:C/Guru 04000001)
C:List GURU2: ALL >MacRW:out/l22.o
$(step s07 $P VERIFY guru ROOT MacRW:sys MACHINE)
$(step s08 $P UPGRADE guru ROOT MacRW:sys CHANNEL MacRW:tampered MACHINE)
$(step s09 $P INSTALL guru ROOT MacRW:other CHANNEL MacRW:badsig MACHINE)
$(step s10 $P REMOVE pkg ROOT MacRW:sys MACHINE)
$(step r23 GURU2:C/Guru 04000001)
C:Eject GURU2:
$(step s11 RAM:boot/Pkg REMOVE identify ROOT MacRW:sys MACHINE)
$(step s12 RAM:boot/Pkg REMOVE guru ROOT MacRW:sys MACHINE)
$(step s13 RAM:boot/Pkg REMOVE ORPHANS ROOT MacRW:sys MACHINE)
$(step s14 RAM:boot/Pkg LIST ROOT MacRW:sys MACHINE)
C:Echo done >MacRW:done"
printf '%s\n' "$script" > "$work/startup.txt"

echo "goal2 2-4: hosted AROS, one AmigaDOS script"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 240 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                 ok $? "the AmigaDOS script ran to its end"

O="$share/out"
code() { tr -d ' \r' < "$O/$1.rc" 2>/dev/null; }
exits() {  # exits <step> <code> <what>
    [ "$(code "$1")" = "$2" ];                        ok $? "$3: \$RC $2 (got $(code "$1"))"
}

exits s01 0 "the bootstrapped Pkg installs itself"
has "$O/s01.o" '^name: pkg$' && has "$O/s01.o" '^result: installed$'
                                                      ok $? "and says so; the steps after it run the managed copy"
exits s02 0 "the FFS handler installed as a component"
exits s03 0 "Guru 2.0 installed"
has "$O/s03.o" '^dependency: identify 37.1$' && has "$O/s03.o" '^result: installed$'
                                                      ok $? "and identify 37.1 came with it as a dependency"
has "$O/s04.o" '^package: guru 2.0 image 1 explicit$' && has "$O/s04.o" '^package: identify 37.1 library 1 dependency$'
                                                      ok $? "the database lists the image and the library it pulled in"

# Guru's output, against the strings in its own sources, for alert 04000001:
# exec/alerts.h:140 AN_BadGadget; tools/catalogs/IdentifyTools.cd
# MSG_GURU_RESULT; catalogs/Identify.cd MSG_ALERT_RECOVERY, MSG_AG_GENERAL and
# MSG_AN_BADGADGET; the subsystem table in idalert.c. A dead-end CPU alert
# such as 80000005 would print "Unknown" here: idalert.c stores ACPU_DivZero
# with the dead-end bit and searches with that bit masked off, an
# identify.library defect recorded in the README.
cat > "$work/guru.expected" <<'EOF'
Alert Code: 04000001
Type:       Recoverable
Subsystem:  intuition.library
General:    General fault
Specified:  Recovery form of AN_GadgetType
EOF
ml_ok() {  # ml_ok <device> <step> <unit> <blocks>: Pkg's entry and the steps it gave
    has "$O/$1.ml" "^Unit            = $3\$" && has "$O/$1.ml" "^HighCyl         = $(($4 / 32 - 1))\$" \
        && has "$O/$1.ml" '^FileSystem      = MacRW:sys/L/afs-handler$' \
        && has "$O/$2.o" "^step: MakeLink RAM:fdsk/Unit$3 MacRW:sys/guru.hdf\$" \
        && has "$O/$2.o" '^step: Assign LIBS: MacRW:sys/Libs ADD$'
}
guru_ok() { sed 's/^ *//; s/ *$//; /^$/d' "$O/$1.o" 2>/dev/null | cmp -s - "$work/guru.expected"; }
! guru_ok r19 && has "$O/r19.o" 'Could not open version 37 or higher of library "identify.library"'
                                                      ok $? "control: before the root's Libs is visible, Guru cannot open identify.library"
exits r19 20 "control: and AmigaDOS sees the failure"
ml_ok GURU0 m20 20 "$n20";                             ok $? "Pkg MOUNTLIST on AROS: the 2.0 entry, handler from the root, and its steps"
guru_ok r20;                                          ok $? "Guru 2.0 runs from the mounted image and decodes 04000001 as its sources say"
exits r20 0 "Guru 2.0"
grep -q 'Guru' "$O/l20.o" && ! grep -q 'Function' "$O/l20.o"
                                                      ok $? "the 2.0 volume holds Guru and not Function"
exits s05 0 "the upgrade to 2.1"
has "$O/s05.o" '^from: 2.0$' && has "$O/s05.o" '^version: 2.1$'; ok $? "reports 2.0 to 2.1"
ml_ok GURU1 m21 21 "$n21";                             ok $? "Pkg MOUNTLIST: the 2.1 entry with 2.1's own geometry"
guru_ok r21;                                          ok $? "Guru 2.1 runs from its image with the same output"
grep -q 'Function' "$O/l21.o";                        ok $? "the 2.1 volume holds Function"
exits s06 0 "the rollback"
has "$O/s06.o" '^result: rolled-back$' && has "$O/s06.o" '^version: 2.0$'; ok $? "reports the return to 2.0"
ml_ok GURU2 m22 22 "$n20";                             ok $? "Pkg MOUNTLIST: the rolled-back entry"
guru_ok r22;                                          ok $? "Guru runs from the rolled-back image"
! grep -q 'Function' "$O/l22.o";                      ok $? "the rolled-back volume no longer holds Function"
exits s07 0 "verify after three mounts"
has "$O/s07.o" '^result: intact$';                    ok $? "the image is intact after being mounted and run: nothing wrote to it"

exits s08 12 "a tampered image refused as integrity"
has "$O/s08.o" '^class: integrity$';                  ok $? "and says integrity"
exits s09 13 "a badly signed dependency refused as signature"
has "$O/s09.o" 'identify 37.1';                       ok $? "and names the dependency"
[ ! -e "$share/other" ] || [ -z "$(find "$share/other" -type f ! -path '*/.pkg/*')" ]
                                                      ok $? "and nothing of Guru or identify was placed"

exits s10 0 "Pkg removes itself"
[ ! -e "$share/sys/C/Pkg" ];                          ok $? "and is gone from the root"
guru_ok r23;                                          ok $? "Guru still runs with Pkg absent"
exits r23 0 "Guru without Pkg"

exits s11 16 "removing identify while Guru needs it refused as dependency"
has "$O/s11.o" 'needed by guru 2.0';                  ok $? "and names Guru"
exits s12 0 "Guru removed"
has "$O/s12.o" '^orphan: identify 37.1$';             ok $? "which leaves identify reported as an orphan"
exits s13 0 "orphans removed"
has "$O/s13.o" '^package: identify 37.1$' && has "$O/s13.o" '^count: 1$'; ok $? "identify taken out, and nothing else"
[ ! -e "$share/sys/guru.hdf" ] && [ ! -e "$share/sys/Libs/identify.library" ]
                                                      ok $? "on disk: no image and no library left"
[ -f "$share/sys/L/afs-handler" ] && has "$O/s14.o" '^package: afs-handler 41.7 device 1 explicit$' && has "$O/s14.o" '^count: 1$'
                                                      ok $? "the FFS handler, installed by name, stays"

# Every step was a MACHINE step: stdout only key: value lines.
bad=0
for f in "$O"/s*.o; do LC_ALL=C grep -a -q -v -E '^[a-z][a-z-]*: ' "$f" && bad=1; done
[ "$bad" = 0 ];                                       ok $? "every Pkg step answered in key: value lines only"
! grep -q -i -E 'rexx|PORT' "$work/startup.txt";      ok $? "the script names no ARexx and opens no port"

echo
echo "goal2: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$O"/*; do echo "--- $(basename "$f")"; LC_ALL=C cat "$f"; done
fi
[ "$fails" -eq 0 ]

#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# One change of a root at a time. While another process holds the root's
# lock, INSTALL, UPGRADE, ROLLBACK, REPAIR and REMOVE are refused at once
# (15, next: retry-later) and change nothing; a dry run takes no lock. The
# lock leaves nothing in the root once the change is done.

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
T=$(mktemp -d)
trap 'kill $holder 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
field() { sed -n "s/^$1: *//p" "$2" | head -1; }

"$P" KEYGEN FILE "$T/k.key" > /dev/null
for v in 1.0 1.1; do
    mkdir -p "$T/src-$v/C"; echo "tool $v" > "$T/src-$v/C/tool"
    "$P" PUBLISH "$T/src-$v" CHANNEL "$T/ch" NAME tool VERSION "$v" KIND application SHORT "lock test" \
        LICENSE MIT SIGN "$T/k.key" MACHINE > /dev/null || exit 1
done
R="$T/r"; mkdir -p "$R"
"$P" INSTALL tool ROOT "$R" CHANNEL "$T/ch" VERSION 1.0 MACHINE > /dev/null
[ ! -e "$R/.pkg/lock" ]; ok $? "a finished change leaves no lock behind"

# Another process holds the lock, as a pkg in the middle of a change does.
python3 -c '
import fcntl, sys, time
f = open(sys.argv[1], "a"); fcntl.flock(f, fcntl.LOCK_EX); open(sys.argv[2], "w").close(); time.sleep(60)
' "$R/.pkg/lock" "$T/held" &
holder=$!
i=0; while [ ! -e "$T/held" ] && [ $i -lt 100 ]; do i=$((i + 1)); python3 -c 'import time; time.sleep(0.05)'; done
for cmd in "UPGRADE tool" "ROLLBACK tool" "REPAIR tool" "REMOVE tool" "INSTALL tool"; do
    ch="CHANNEL $T/ch"; [ "$cmd" = "REMOVE tool" ] && ch=
    "$P" $cmd ROOT "$R" $ch MACHINE > "$T/out" 2>&1; rc=$?
    [ $rc -eq 15 ] && [ "$(field next "$T/out")" = retry-later ]
    ok $? "$cmd while the root is locked: refused 15, retry-later (got $rc)"
done
"$P" VERIFY tool ROOT "$R" MACHINE > "$T/v" 2>&1
[ "$(field version "$T/v")" = 1.0 ]; ok $? "and the root still holds 1.0"
"$P" UPGRADE tool ROOT "$R" CHANNEL "$T/ch" DRYRUN MACHINE > "$T/out" 2>&1
[ $? -eq 0 ] && [ "$(field result "$T/out")" = would-upgrade ]; ok $? "a dry run takes no lock"
kill $holder; wait $holder 2>/dev/null
"$P" UPGRADE tool ROOT "$R" CHANNEL "$T/ch" MACHINE > "$T/out" 2>&1
[ $? -eq 0 ] && [ "$(field version "$T/out")" = 1.1 ]; ok $? "once it is released, the change runs"
[ ! -e "$R/.pkg/lock" ]; ok $? "and leaves no lock behind"

echo "lock: $checks checks, $fails failed"
[ $fails -eq 0 ]

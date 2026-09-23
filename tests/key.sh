#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# KEY: the requester names the publisher's key, and pkg trusts no one else
# for that package or on the way to it. The package must be signed by KEY,
# a pin must be KEY (ACCEPTKEY confirms a change), a dependency with no pin
# is refused until it is installed with its own KEY, and REPAIR puts files
# back only from the key the root pins.

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
field() { sed -n "s/^$1: *//p" "$2" | head -1; }
keyof() { "$P" KEYINFO "$1" MACHINE | sed -n 's/^key: *//p;s/^public: *//p' | head -1; }

"$P" KEYGEN FILE "$T/a.key" > /dev/null; A=$(keyof "$T/a.key")
"$P" KEYGEN FILE "$T/b.key" > /dev/null; B=$(keyof "$T/b.key")
pub() { # pub <channel> <name> <version> <keyfile> [DEPENDS spec] [ACCEPTKEY k]
    ch=$1; n=$2; v=$3; k=$4; shift 4
    d="$T/src-$n-$v"; mkdir -p "$d/C"; echo "$n $v" > "$d/C/$n"
    "$P" PUBLISH "$d" CHANNEL "$ch" NAME "$n" VERSION "$v" KIND application SHORT "key test" \
        LICENSE MIT SIGN "$k" "$@" MACHINE > "$T/pub.out" 2>&1 || { cat "$T/pub.out"; exit 1; }
}
pub "$T/ch" tool 1.0 "$T/a.key"
pub "$T/ch" tool 1.1 "$T/a.key"
pub "$T/ch" lib 1.0 "$T/b.key"
pub "$T/ch" app 1.0 "$T/a.key" DEPENDS "lib"
pub "$T/other" tool 1.0 "$T/b.key"
pub "$T/other" tool 1.2 "$T/b.key"

run() { name=$1; shift; "$P" "$@" MACHINE > "$T/$name" 2>&1; }
empty() { [ ! -d "$1/.pkg/db" ] || [ -z "$(ls "$1/.pkg/db")" ]; }

run i1 INSTALL tool ROOT "$T/r1" CHANNEL "$T/ch" VERSION 1.0 KEY "$A"
[ $? -eq 0 ] && [ "$(head -c 64 "$T/r1/.pkg/keys/tool")" = "$A" ]
ok $? "INSTALL with the signer's KEY installs and pins it"
run i2 INSTALL tool ROOT "$T/r2" CHANNEL "$T/ch" KEY "$B"
[ $? -eq 14 ] && [ "$(field class "$T/i2")" = key ] && empty "$T/r2"
ok $? "INSTALL with another KEY is refused (14, key) and changes nothing"
installed() { "$P" VERIFY tool ROOT "$1" MACHINE > "$T/v" 2>&1; field version "$T/v"; }
run u0 UPGRADE tool ROOT "$T/r1" CHANNEL "$T/ch" KEY "$B"
[ $? -eq 14 ] && [ "$(installed "$T/r1")" = 1.0 ]
ok $? "UPGRADE to a version not signed by KEY is refused (14)"
run u2 UPGRADE tool ROOT "$T/r1" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 0 ] && [ "$(field version "$T/u2")" = 1.1 ]
ok $? "UPGRADE with the pinned KEY upgrades"
run u1 UPGRADE tool ROOT "$T/r1" CHANNEL "$T/other" KEY "$B"
[ $? -eq 14 ] && [ "$(installed "$T/r1")" = 1.1 ] && [ "$(field pinned "$T/u1")" = "$A" ]
ok $? "UPGRADE signed by KEY, but KEY is not the pin: refused (14)"
grep -q "ACCEPTKEY" "$T/u1"; ok $? "that refusal names ACCEPTKEY"
run u3 UPGRADE tool ROOT "$T/r1" CHANNEL "$T/other" KEY "$B" ACCEPTKEY "$B"
[ $? -eq 0 ] && [ "$(installed "$T/r1")" = 1.2 ] && [ "$(head -c 64 "$T/r1/.pkg/keys/tool")" = "$B" ]
ok $? "with ACCEPTKEY confirming it, the new key is pinned"

run d1 INSTALL app ROOT "$T/r3" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 14 ] && [ "$(field next "$T/d1")" = install-dependency-first ] && empty "$T/r3" \
    && [ "$(field dependency "$T/d1")" = "lib 1.0" ]
ok $? "a dependency with no pin is refused with KEY (install-dependency-first), nothing changed"
run d2 INSTALL lib ROOT "$T/r3" CHANNEL "$T/ch" KEY "$B"
ok $? "the dependency installed first with its own KEY"
run d3 INSTALL app ROOT "$T/r3" CHANNEL "$T/ch" KEY "$A"
ok $? "then the package installs with its KEY"

mkdir -p "$T/r4"
run p0 INSTALL tool ROOT "$T/r4" CHANNEL "$T/ch" VERSION 1.0 KEY "$A"
rm -f "$T/r4/C/tool"
run p2 REPAIR tool ROOT "$T/r4" CHANNEL "$T/other"
[ $? -eq 14 ] && [ ! -e "$T/r4/C/tool" ]
ok $? "REPAIR from a channel signed by another key than the pin puts nothing back (14)"
run p3 REPAIR tool ROOT "$T/r4" CHANNEL "$T/ch" KEY "$B"
[ $? -eq 14 ] && [ ! -e "$T/r4/C/tool" ]
ok $? "REPAIR with a KEY that is not the signer puts nothing back (14)"
run p4 REPAIR tool ROOT "$T/r4" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 0 ] && [ -e "$T/r4/C/tool" ]
ok $? "REPAIR with the right KEY puts the file back"

run k1 INSTALL tool ROOT "$T/r5" CHANNEL "$T/ch" KEY "ABC"
[ $? -eq 20 ]; ok $? "a KEY that is not 64 lowercase hex digits is refused (20)"
run k2 INSTALL tool app ROOT "$T/r5" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 20 ]; ok $? "KEY with several packages is refused (20)"
run k3 UPGRADE ALL ROOT "$T/r1" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 20 ]; ok $? "UPGRADE ALL with KEY is refused (20)"
run k4 REPAIR ALL ROOT "$T/r4" CHANNEL "$T/ch" KEY "$A"
[ $? -eq 20 ]; ok $? "REPAIR ALL with KEY is refused (20)"

echo "key: $checks checks, $fails failed"
[ $fails -eq 0 ]

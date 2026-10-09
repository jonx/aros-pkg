#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The examples are run as tests: examples/basic.c end to end,
# examples/browse.c against a channel with an image and its dependency,
# through a listing, an install, a typo, a cancel and a damaged entry, and
# examples/events.c, an install and an upgrade on a worker thread whose
# events a UI thread shows.

set -u
PKG=${PKG:-./build/pkg}
B=./build
for need in "$PKG" "$B/example-basic" "$B/example-browse" "$B/example-events"; do
    [ -x "$need" ] || { echo "examples: missing $need (make build/pkg build/example-basic build/example-browse build/example-events)" >&2; exit 69; }
done
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-examples.XXXXXX")
trap 'rm -rf "$T"' EXIT
checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

$B/example-basic "$T/basic" > "$T/basic.out" 2>&1
[ $? -eq 0 ] && has "$T/basic.out" 'install: done' && has "$T/basic.out" 'package: hello 1.0 application 1 explicit' \
    && has "$T/basic.out" 'install helo: refused, not-found (11)'
                                                      ok $? "basic publishes, installs, lists, and reads a refusal"

sh tests/contract-channel.sh "$PKG" "$T/w" "$T" > /dev/null
$B/example-browse list "$T/channel" "$T/root" > "$T/l1" 2>&1
[ $? -eq 0 ] && has "$T/l1" '^happ  *1  *image' && has "$T/l1" 'happ 1 needs hlib >= 1.0'
                                                      ok $? "browse lists the channel with dependencies"
$B/example-browse install happ "$T/channel" "$T/root" > "$T/i1" 2>&1
[ $? -eq 0 ] && has "$T/i1" 'Installed happ 1' && has "$T/i1" '  hlib 1.0' && has "$T/i1" 'happ.hdf'
                                                      ok $? "browse installs happ, names hlib it brought, and the image"
$B/example-browse list "$T/channel" "$T/root" > "$T/l2" 2>&1
grep -q '^happ .* installed$' "$T/l2" && grep -q '^hlib .* installed$' "$T/l2"
                                                      ok $? "and the list then says both are installed"
$B/example-browse install hap "$T/channel" "$T/root" > "$T/i2" 2>&1
[ $? -eq 11 ] && has "$T/i2" 'Did you mean: happ?'; ok $? "a typo is answered with the near name"
# libpkg asks: resolving happ, resolving hlib, before placing hlib, before
# placing happ. The fourth time, hlib is in place and must be taken out.
PKG_BROWSE_TRACE=1 $B/example-browse install happ "$T/channel" "$T/root2" 4 > "$T/i3" 2>&1
[ $? -eq 10 ] && has "$T/i3" 'Cancel pressed' && has "$T/i3" 'took hlib 1.0 back out' \
    && [ ! -e "$T/root2/Libs/hlib.library" ] \
    && [ ! -e "$T/root2/.pkg/keys/hlib" ];            ok $? "Cancel after hlib is placed leaves neither hlib nor its pinned key"
$B/example-browse list "$T/tampered" > "$T/l3" 2>&1
[ $? -eq 12 ] && has "$T/l3" 'integrity';           ok $? "a damaged entry is shown as such, and the exit code says so"

$B/example-events "$T/events" > "$T/ev" 2>&1
[ $? -eq 0 ] && has "$T/ev" '^install (operation' && has "$T/ev" '^install: ok, code 0' \
    && has "$T/ev" '^upgrade: ok, code 0' && has "$T/ev" 'place hello 1.1' && has "$T/ev" 'ok: 1 files' \
    && has "$T/ev" '^returned 0 and 0'
                                                      ok $? "events: a worker's events reach the UI thread, in order, begin to end"

echo "examples: $checks checks, $fails failures"
[ "$fails" -eq 0 ]

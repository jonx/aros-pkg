#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# A change cut at every write, then the same command again. pkg is ended
# before each rename and unlink it makes, in turn (tests/interrupt/cut.c),
# as a crash or a reset would, and the recovery is only ever to repeat the
# command that was cut, once or after a second cut at the same point.
# ROLLBACK is the exception: it goes back and forth, so it is repeated only
# when the version it was asked for is not the one installed. It
# must then leave the version asked for, intact, pinned to the publisher,
# with the right previous version and nothing set aside. Right after a cut,
# ROLLBACK must go back to the version before the one the database names,
# or refuse: never to another.
#
# Two builds: this one, cut as a process ends; and one built as for AROS
# (PKG_RENAME_NOT_ATOMIC), cut as AROS renames over a file, deleting the
# target first. A process ending is not power loss: the handler's cache is
# not in this test, and power loss needs a record of the device's writes.
#
# Needs: make, a C compiler; macOS (interpose) or Linux (LD_PRELOAD).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then :; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
cc=${CC:-cc}

case $(uname) in
Darwin) lib="$T/cut.dylib"; "$cc" -dynamiclib -O1 -o "$lib" "$repo_root/tests/interrupt/cut.c"; pre=DYLD_INSERT_LIBRARIES ;;
*) lib="$T/cut.so"; "$cc" -shared -fPIC -O1 -o "$lib" "$repo_root/tests/interrupt/cut.c" -ldl; pre=LD_PRELOAD ;;
esac
( cd "$repo_root" && "$cc" -std=c99 -O1 -Iinclude -Ithird_party/bzip2 -DPKG_RENAME_NOT_ATOMIC \
    -DPKG_VERSION_PATCH='"0"' -DPKG_BUILD='"0"' -DPKG_BUILD_DAY='"0"' \
    -o "$T/pkg-aros" $(make -s print-sources) ) || { echo "interrupt: cannot build the AROS-rename pkg"; exit 1; }

# One package in three versions: a file that changes each time, one that
# never does, one only 1.0 has, and two 1.1 brings.
"$P" KEYGEN FILE "$T/k.key" > /dev/null
key=$("$P" KEYINFO "$T/k.key" MACHINE | sed -n 's/^key: *//p;s/^public: *//p' | head -1)
for v in 1.0 1.1 1.2; do
    d="$T/src-$v"; mkdir -p "$d/C" "$d/Libs"
    echo "a $v" > "$d/C/A"; echo same > "$d/C/B"
    case $v in
    1.0) echo "c only in 1.0" > "$d/Libs/C" ;;
    *) echo "d new in 1.1" > "$d/Libs/D"; echo "e $v" > "$d/C/E" ;;
    esac
    "$P" PUBLISH "$d" CHANNEL "$T/ch" NAME multi VERSION "$v" KIND application SHORT "cut test" \
        LICENSE MIT SIGN "$T/k.key" MACHINE > /dev/null || { echo "interrupt: cannot publish $v"; exit 1; }
done

field() { sed -n "s/^$1: *//p" "$2" | head -1; }
installed() { "$1" VERIFY multi ROOT "$2" MACHINE > "$2.v" 2>&1; echo "$(field result "$2.v") $(field version "$2.v")"; }
pin_of() { cat "$1/.pkg/keys/multi" 2>/dev/null || cat "$1/.pkg/keys/.multi.pkgbak" 2>/dev/null; }

# setup <pkg> <root> <scenario>: the root before the command under test.
setup() {
    mkdir -p "$2"
    case $3 in
    install) ;;
    upgrade) "$1" INSTALL multi ROOT "$2" CHANNEL "$T/ch" VERSION 1.0 MACHINE > /dev/null ;;
    upgrade2|rollback) "$1" INSTALL multi ROOT "$2" CHANNEL "$T/ch" VERSION 1.0 MACHINE > /dev/null
        "$1" UPGRADE multi ROOT "$2" CHANNEL "$T/ch" VERSION 1.1 MACHINE > /dev/null ;;
    repair) "$1" INSTALL multi ROOT "$2" CHANNEL "$T/ch" VERSION 1.1 MACHINE > /dev/null
        echo tampered > "$2/C/A"; rm -f "$2/C/E" ;;
    esac
}
# The command, what it must end at, and the versions the root held, in order.
command_of() {
    case $1 in
    install) echo "INSTALL multi VERSION 1.0" ;;
    upgrade) echo "UPGRADE multi VERSION 1.1" ;;
    upgrade2) echo "UPGRADE multi VERSION 1.2" ;;
    rollback) echo "ROLLBACK multi" ;;
    repair) echo "REPAIR multi" ;;
    esac
}
target_of() { case $1 in install) echo 1.0 ;; upgrade) echo 1.1 ;; upgrade2) echo 1.2 ;; rollback) echo 1.0 ;; repair) echo 1.1 ;; esac; }
history_of() { case $1 in install) echo "1.0" ;; upgrade) echo "1.0 1.1" ;; upgrade2) echo "1.0 1.1 1.2" ;; rollback) echo "1.0 1.1 1.0" ;; repair) echo "1.1" ;; esac; }
prev_of() { case $1 in upgrade) echo 1.0 ;; upgrade2) echo 1.1 ;; rollback) echo 1.1 ;; *) echo - ;; esac; }
# The version before v in the history, the last time the root held v.
before() {
    b=-; last=-
    for h in $2; do [ "$h" = "$1" ] && b=$last; last=$h; done
    echo "$b"
}

run() { # run <pkg> <root> <scenario> [cut] [mode]
    set -- "$1" "$2" "$3" "${4:-}" "${5:-exit}"
    if [ -n "$4" ]; then
        env "$pre=$lib" PKGCUT_AT="$4" PKGCUT_MODE="$5" "$1" $(command_of "$3") ROOT "$2" CHANNEL "$T/ch" MACHINE > "$2.out" 2>&1
    else
        "$1" $(command_of "$3") ROOT "$2" CHANNEL "$T/ch" MACHINE > "$2.out" 2>&1
    fi
}

total=0
for build in plain aros; do
    if [ $build = plain ]; then pk=$P; mode=exit; else pk=$T/pkg-aros; mode=aros; fi
    for sc in install upgrade upgrade2 rollback repair; do
        # How many writes the command makes, uncut.
        R="$T/count"; rm -rf "$R" "$T/log"; setup "$pk" "$R" $sc
        env "$pre=$lib" PKGCUT_LOG="$T/log" "$pk" $(command_of $sc) ROOT "$R" CHANNEL "$T/ch" MACHINE > /dev/null 2>&1
        writes=$(wc -l < "$T/log" | tr -d ' ')
        [ "$writes" -gt 0 ]; ok $? "$build $sc: the command writes something ($writes)"
        n=1
        while [ $n -le "$writes" ]; do
            for times in 1 2; do
                what="$build $sc cut at write $n of $writes"
                [ $times = 2 ] && what="$what, twice"
                R="$T/r"; rm -rf "$R" "$R".*; setup "$pk" "$R" $sc
                run "$pk" "$R" $sc $n $mode
                # Right after the cut: ROLLBACK goes where the database says, or nowhere.
                rm -rf "$T/copy"; cp -R "$R" "$T/copy"
                set -- $(installed "$pk" "$T/copy"); db=${2:--}
                "$pk" ROLLBACK multi ROOT "$T/copy" CHANNEL "$T/ch" MACHINE > "$T/copy.rb" 2>&1; rbrc=$?
                set -- $(installed "$pk" "$T/copy"); after=${2:--}
                want=$(before "$db" "$(history_of $sc)")
                [ $rbrc -ne 0 -a "$after" = "$db" ] || [ $rbrc -eq 0 -a "$after" = "$want" ]
                ok $? "$what: ROLLBACK right after it went from $db to $after (allowed: refuse, or $want)"
                [ $times = 2 ] && run "$pk" "$R" $sc $n $mode
                if [ $sc = rollback ] && [ "$(installed "$pk" "$R")" = "intact 1.0" ]; then
                    rc=0; echo "result: already" > "$R.out"
                else
                    run "$pk" "$R" $sc
                    rc=$?
                fi
                [ $rc -eq 0 ]; ok $? "$what: the same command again succeeds ($(field result "$R.out") $(field class "$R.out"))"
                [ "$(installed "$pk" "$R")" = "intact $(target_of $sc)" ]; ok $? "$what: $(target_of $sc) intact ($(installed "$pk" "$R"))"
                p=$(pin_of "$R")
                [ "$p" = "$key" ]; ok $? "$what: pinned to the publisher"
                if [ "$(prev_of $sc)" != - ]; then
                    [ "$(head -1 "$R/.pkg/prev/multi" 2>/dev/null)" = "$(prev_of $sc)" ]
                    ok $? "$what: previous version $(prev_of $sc) (found $(head -1 "$R/.pkg/prev/multi" 2>/dev/null))"
                fi
                # REPAIR keeps the one real change, the tampered file; nothing else is set aside.
                aside=$(cd "$R" && find . -name '*.pkgold' -o -name '*.pkgnew' | sort | tr '\n' ' ')
                if [ $sc = repair ]; then
                    [ "$aside" = "./C/A.pkgold " ] && [ "$(cat "$R/C/A.pkgold")" = tampered ]
                    ok $? "$what: only the tampered file set aside ($aside)"
                else
                    [ -z "$aside" ]; ok $? "$what: nothing set aside ($aside)"
                fi
            done
            n=$((n + 1))
            total=$((total + 1))
        done
    done
done
echo "  interrupt: $total cut points, $checks checks, $fails failed"
[ $fails -eq 0 ]

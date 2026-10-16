#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Build the AROS pieces goal 2 publishes, from the AROS sources, with the hosted
# build's own mmake, and leave the shared hosted tree as it was found.
#
#   build/aros/afs-handler             rom/filesys/afs, the AROS FFS handler.
#                                      The hosted darwin build ships none, so
#                                      an FFS image cannot mount without it.
#   build/aros/identify/identify.library, Guru, Function
#                                      workbench/libs/identify and its tools.
#                                      Hosted AROS ships no identify.library.
#
# mmake writes into the hosted tree. Every file it adds where a booted system
# looks (L:, Libs:, C:, Locale:) is copied out and then deleted, with the
# directories it created, so the system other runs boot is the one they
# expect. What it adds under Developer/ (identify's headers, fd file and link
# library) stays: nothing boots from there, and deleting generated headers
# left mmake regenerating their dependencies in an endless loop, which is how
# the first version of this script failed.
#
#   AROS_BUILD=~/Source/aros-project/aros-build sh tools/build-aros-extras.sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_build=${AROS_BUILD:-"$HOME/Source/aros-project/aros-build"}
tree="bin/darwin-aarch64/AROS"
out="$repo_root/build/aros"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-extras.XXXXXX")
trap 'rm -rf "$work"' EXIT

[ -d "$aros_build/$tree" ] || { echo "build-aros-extras: no hosted build at $aros_build" >&2; exit 69; }
cd "$aros_build"
runtime() { find "$tree" "$@" ! -path "$tree/Developer/*" | sort; }
runtime -type f > "$work/before"
runtime -type d > "$work/dirs-before"

make kernel-fs-afs > "$work/afs.log" 2>&1 || { tail -20 "$work/afs.log"; exit 1; }
make workbench-libs-identify-tools > "$work/identify.log" 2>&1 || { tail -20 "$work/identify.log"; exit 1; }

mkdir -p "$out/identify"
cp "$tree/L/afs-handler" "$out/afs-handler"
cp "$tree/Libs/identify.library" "$tree/C/Guru" "$tree/C/Function" "$out/identify/"

runtime -type f > "$work/after"
comm -13 "$work/before" "$work/after" | while IFS= read -r f; do rm "$f"; done
runtime -type d > "$work/dirs-after"
comm -13 "$work/dirs-before" "$work/dirs-after" | sort -r | while IFS= read -r d; do rmdir "$d"; done
runtime -type f > "$work/restored"
cmp -s "$work/before" "$work/restored" || { echo "build-aros-extras: the booted tree was not restored" >&2; exit 1; }

echo "build-aros-extras: $out/afs-handler"
echo "build-aros-extras: $out/identify/{identify.library,Guru,Function}"
echo "build-aros-extras: the booted part of the hosted tree is as it was"

#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Cross-build Regina's static `rexx` interpreter for aarch64 AROS, from the AROS
# contrib sources, read-only. Output: build/aros/rexx.
#
# Hosted AROS ships rexxsyslib.library but no interpreter. The `rexx` program
# needs no RexxMast: Regina's amifuncs.c resolves `ADDRESS <name>` to a public
# message port of that name and sends it RXCOMM messages, which is exactly what
# the PKG port serves. The file list and defines are the ones the contrib
# mmakefile.src gives for its `contrib-regina-rexx` target.
#
# Regina is third-party code, so its warnings are not ours to fix: it builds
# with -w, where Pkg itself builds with -Werror.
#
# One patch is applied, to a copy: tools/aros/regina-arexx-rc.patch. For an
# ARexx port Regina set RC to the command's RESULT string instead of the
# host's numeric rm_Result1, so every successful command read as a failure to
# a script testing RC. The patch gives RC the number and RESULT the string, as
# ARexx specifies. It is kept as a file so it can be offered upstream.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
regina=${REGINA_ROOT:-"$repo_root/../aros-contrib/regina"}
aros_build=${AROS_BUILD:-"$HOME/Source/aros-project/aros-build"}
aros_crosstools=${AROS_CROSSTOOLS:-"$HOME/Source/aros-project/aros-crosstools"}
sdk=${PKG_AROS_SDK_ROOT:-"$aros_build/bin/darwin-aarch64"}
build_tools=${PKG_AROS_BUILD_TOOLS_ROOT:-"$sdk/tools"}
aros_target=${PKG_AROS_TARGET:-aarch64-unknown-aros}
# The default is the hosted Darwin profile, which reserves x18. Native Apple
# AROS leaves x18 to the compiler; its builder passes its own flags.
aros_arch_flags=${PKG_AROS_ARCH_FLAGS:--mcmodel=large -ffixed-x18}
aros_clang="$aros_crosstools/bin/clang"
aros_cross_lib=${PKG_AROS_CROSS_LIB:-"$aros_crosstools/lib/generic"}
developer="$sdk/AROS/Developer"
out="$repo_root/build/aros"
obj="$repo_root/build/aros/regina-obj"

[ -f "$regina/rexx.c" ] || { echo "build-aros-regina: no Regina sources at $regina" >&2; exit 69; }
mkdir -p "$out" "$obj"
copy="$repo_root/build/aros/regina-src"
rm -rf "$copy"
cp -R "$regina" "$copy"
(cd "$copy" && patch -s -p1 < "$repo_root/tools/aros/regina-arexx-rc.patch")
regina="$copy"

files="funcs builtin error variable interprt debug dbgfuncs memory parsing files
misc unxfuncs cmsfuncs os2funcs shell rexxext stack tracing interp cmath convert
strings library strmath signals macros envir expr instore yaccsrc lexsrc wrappers
options rexxbif arxfuncs amifuncs os_amiga rexx nosaa mt_notmt"

objects=""
for f in $files; do
    # shellcheck disable=SC2086
    COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
        "$aros_clang" --target="$aros_target" $aros_arch_flags \
        -O2 -std=gnu99 -w \
        -isystem "$developer/include" \
        -isystem "$sdk/gen/include" \
        -isystem "$sdk/gen/include/aros/posixc" \
        -isystem "$developer/include/aros/stdc" \
        -I "$regina" \
        -D_GNU_SOURCE -DNO_EXTERNAL_QUEUES \
        -DREGINA_VERSION_DATE='"31 Dec 2009"' \
        -DREGINA_VERSION_MAJOR='"3"' -DREGINA_VERSION_MINOR='"5"' \
        -DREGINA_VERSION_SUPP='""' \
        -c "$regina/$f.c" -o "$obj/$f.o"
    objects="$objects $obj/$f.o"
done

# shellcheck disable=SC2086
COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
    "$aros_clang" --target="$aros_target" $aros_arch_flags \
    -nostartfiles -nodefaultlibs \
    -L "$developer/lib" -L "$aros_cross_lib" \
    "$developer/lib/startup.o" $objects \
    -o "$out/rexx" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lrexxsyslib -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport \
    -Wl,--end-group -lclang_rt.builtins-aarch64
chmod 755 "$out/rexx"
echo "build-aros-regina: $out/rexx"

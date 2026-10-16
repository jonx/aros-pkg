#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Build AROS's own C:Unpack, from the AROS sources, read-only, for the goal's
# bootstrap. Output: build/aros/Unpack.
#
# The hosted build's C:Unpack does not load: the shell answers "file is not
# executable" even with no arguments, while commands built beside it load. Its
# recipe links with -static, which the other C: commands do not use. This
# builds the same six sources with the recipe Pkg itself uses and no -static.
# It stays the tool AROS ships, read from the same source, so the bootstrap
# still demonstrates the adopted container read by AROS's own reader.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src=${AROS_UNPACK_SRC:-"$repo_root/../aros-apple-core/workbench/c/Unpack"}
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

[ -f "$src/unpack.c" ] || { echo "build-aros-unpack: no Unpack sources at $src" >&2; exit 69; }
mkdir -p "$out"

# shellcheck disable=SC2086
COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
    "$aros_clang" --target="$aros_target" $aros_arch_flags \
    -O2 -std=gnu99 -w \
    -isystem "$developer/include" \
    -isystem "$sdk/gen/include" \
    -isystem "$developer/include/aros/stdc" \
    -I "$src" -DADATE='"18.09.2026"' \
    -nostartfiles -nodefaultlibs \
    -L "$developer/lib" -L "$aros_cross_lib" \
    "$src/unpack.c" "$src/gui.c" "$src/package.c" "$src/bzip2.c" \
    "$src/file.c" "$src/support.c" \
    -o "$out/Unpack" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lbz2_nostdio -lstdc -ldos -lexec -laros -lautoinit -llibinit \
    -lintuition -lgraphics -lutility -lamiga -larossupport \
    -Wl,--end-group -lclang_rt.builtins-aarch64
chmod 755 "$out/Unpack"
echo "build-aros-unpack: $out/Unpack"

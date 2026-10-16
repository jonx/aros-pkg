#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# pkgtest.library for hosted aarch64 AROS, at two versions, 1.0 and 2.30,
# for tests/aros-resolve.sh: build/aros/pkgtest-1.0.library and
# build/aros/pkgtest-2.30.library. Same toolchain as tools/build-aros.sh.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_build=${AROS_BUILD:-"$HOME/Source/aros-project/aros-build"}
aros_crosstools=${AROS_CROSSTOOLS:-"$HOME/Source/aros-project/aros-crosstools"}
sdk="$aros_build/bin/darwin-aarch64"
developer="$sdk/AROS/Developer"
mkdir -p "$repo_root/build/aros"
for v in 1.0 2.30; do
    COMPILER_PATH="$sdk/tools:$aros_crosstools/bin" \
        "$aros_crosstools/bin/clang" --target=aarch64-unknown-aros -mcmodel=large -ffixed-x18 \
        -O2 -std=gnu11 -Wall -Wextra -Werror \
        -isystem "$developer/include" -isystem "$sdk/gen/include" -isystem "$developer/include/aros/stdc" \
        -nostartfiles -nodefaultlibs -DVER="${v%.*}" -DREV="${v#*.}" \
        "$repo_root/tests/aros/pkgtest-lib.c" -o "$repo_root/build/aros/pkgtest-$v.library"
    echo "build-aros-testlib: build/aros/pkgtest-$v.library"
done
# C:TestLib, from the AROS sources: OpenLibrary(name, version), and the version got
aros_src=${AROS_SRC:-"$repo_root/../aros-upstream"}
[ -f "$aros_src/workbench/c/TestLib.c" ] || aros_src="$HOME/aros-next/research/src/aros-upstream"
COMPILER_PATH="$sdk/tools:$aros_crosstools/bin" \
    "$aros_crosstools/bin/clang" --target=aarch64-unknown-aros -mcmodel=large -ffixed-x18 \
    -O2 -std=gnu11 -Wall -Wextra \
    -isystem "$developer/include" -isystem "$sdk/gen/include" -isystem "$developer/include/aros/stdc" \
    -nostartfiles -nodefaultlibs -L "$developer/lib" -L "$aros_crosstools/lib/generic" \
    "$developer/lib/startup.o" "$aros_src/workbench/c/TestLib.c" -o "$repo_root/build/aros/TestLib" \
    -Wl,--allow-multiple-definition -Wl,--start-group -ldos -lexec -laros -lautoinit -llibinit -lamiga -larossupport -Wl,--end-group \
    -lclang_rt.builtins-aarch64
echo "build-aros-testlib: build/aros/TestLib"

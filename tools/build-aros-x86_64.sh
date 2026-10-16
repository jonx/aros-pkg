#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Cross-build Pkg for native AROS on x86_64 (pc-x86_64), the build that runs
# under QEMU. Output: build/aros-x86_64/Pkg.
#
#   AROS_X86_64_SDK   the Developer directory of an x86_64 AROS system, from
#                     the nightly AROS-<date>-linux-x86_64-system archive:
#                     same ABI for programs as pc-x86_64. Default:
#                     ~/aros-native/AROS-*-linux-x86_64-system/Developer.
#   LLVM              a clang that knows the x86_64-unknown-aros triple; the
#                     Homebrew LLVM does. Default: /opt/homebrew/opt/llvm.
#
# Linking needs collect-aros, AROS's linker wrapper, built for x86_64: the one
# a hosted aarch64 build makes is fixed to aarch64. It is built here from the
# AROS sources, with the hosted build's own environment (the paths to ld.lld
# and the LLVM tools, which handle every architecture) and the CPU switched.

set -eu
# The version this binary says it is, as the Makefile makes it: PATCH and BUILD
# may be set to pin a reproducible build.
verflags="-DPKG_VERSION_PATCH=\"${PATCH:-0}\" -DPKG_BUILD=\"${BUILD:-$(date -u +%Y%m%d)}\" -DPKG_BUILD_DAY=\"${BUILDDAY:-$(date -u +%d.%m.%Y)}\""

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# AROS's sources sit beside the repository; from a git worktree, beside the
# repository the worktree belongs to.
main_root=$(cd "$repo_root" && git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')
aros_src=${AROS_SRC:-"$repo_root/../aros-upstream"}
[ -d "$aros_src" ] || [ -z "$main_root" ] || aros_src="$main_root/../aros-upstream"
hosted_env=${AROS_HOSTED_ENV:-"$HOME/Source/aros-project/aros-build/bin/darwin-aarch64/gen/tools/collect-aros/env.h"}
llvm=${LLVM:-/opt/homebrew/opt/llvm}
sdk=${AROS_X86_64_SDK:-$(ls -d "$HOME"/aros-native/AROS-*-linux-x86_64-system/Developer 2>/dev/null | tail -1)}
mbedtls="$repo_root/third_party/mbedtls"
out="$repo_root/build/aros-x86_64"
tools="$out/tools"

for need in "$llvm/bin/clang" "$sdk/lib/startup.o" "$hosted_env" "$aros_src/tools/collect-aros/collect-aros.c"; do
    [ -e "$need" ] || { echo "build-aros-x86_64: missing $need" >&2; exit 69; }
done
mkdir -p "$out/obj" "$out/mbedtls" "$tools"

# The certificate authorities Pkg carries.
sh "$repo_root/tools/ca-bundle-c.sh" "$repo_root/third_party/cacert/cacert.pem" > "$out/pkg_cabundle.c"

# collect-aros for x86_64.
sed 's/#define TARGET_CPU_aarch64/#define TARGET_CPU_x86_64/' "$hosted_env" > "$tools/env.h"
grep -q 'TARGET_CPU_x86_64' "$tools/env.h" || { echo "build-aros-x86_64: env.h not switched" >&2; exit 1; }
(cd "$aros_src/tools/collect-aros" && cc -O2 -w -I "$tools" -I . -DOBJLIBDIR=\""$sdk/lib"\" \
    collect-aros.c misc.c backend-generic.c docommand-exec.c gensets.c -o "$tools/collect-aros")

cd "$repo_root"
# the sources the Makefile keeps (sources.mk), and the AROS ones
sources=$(make -s -C "$repo_root" print-sources)
for f in $sources src/pkg_tls_aros.c "$out/pkg_cabundle.c"; do
    # The AROS clang predefines these; the stock one, which knows the triple
    # but not the platform, does not, and without __AROS__ Pkg would take its
    # POSIX output path, which reaches no AmigaDOS redirection.
    "$llvm/bin/clang" --target=x86_64-unknown-aros -mcmodel=large -O2 -std=gnu11 $verflags \
        -D__AROS__=1 -D__AROS=1 -DAROS=1 -DAMIGA=1 -D_AMIGA=1 \
        -Wall -Wextra -Werror \
        -isystem "$sdk/include" -isystem "$sdk/include/aros/posixc" -isystem "$sdk/include/aros/stdc" \
        -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' -isystem "$mbedtls" -isystem "$mbedtls/include" \
        -I include -I third_party/bzip2 -c "$f" -o "$out/obj/$(basename "$f" .c).o"
done
# Mbed TLS, which https runs on: third_party/mbedtls, compiled for size with
# Pkg's own configuration, rebuilt when an object is older than its source.
# MBEDTLS_TEST_SW_INET_PTON: AROS's posixc has no inet_pton (it lives in
# miami.library), so Mbed TLS reads addresses with its own.
for c in "$mbedtls"/library/*.c; do
    o="$out/mbedtls/$(basename "$c" .c).o"
    [ "$o" -nt "$c" ] && [ "$o" -nt "$mbedtls/pkg_mbedtls_config.h" ] && continue
    "$llvm/bin/clang" --target=x86_64-unknown-aros -mcmodel=large -Os -std=gnu11 \
        -D__AROS__=1 -D__AROS=1 -DAROS=1 -DAMIGA=1 -D_AMIGA=1 \
        -isystem "$sdk/include" -isystem "$sdk/include/aros/posixc" -isystem "$sdk/include/aros/stdc" \
        -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' -DMBEDTLS_TEST_SW_INET_PTON \
        -I "$mbedtls" -I "$mbedtls/include" -I "$mbedtls/library" -c "$c" -o "$o"
done
"$tools/collect-aros" -o "$out/Pkg" "$sdk/lib/startup.o" "$out"/obj/*.o "$out"/mbedtls/*.o -L"$sdk/lib" \
    --allow-multiple-definition --start-group \
    -lrexxsyslib -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport --end-group
chmod 755 "$out/Pkg"
echo "build-aros-x86_64: $out/Pkg"

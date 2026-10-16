#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Cross-build the Pkg client for AROS aarch64, hosted or native, with the
# recipe the AFS+ handler's own C commands use. Output: build/aros/Pkg.
#
# The host layer is the POSIX one: AROS links against its posixc library, so
# opendir, stat, rename and mkdir are there. Whether their path semantics match
# what the client assumes is what the hosted run exists to establish.

set -eu
# The version this binary says it is, as the Makefile makes it: PATCH and BUILD
# may be set to pin a reproducible build.
verflags="-DPKG_VERSION_PATCH=\"${PATCH:-0}\" -DPKG_BUILD=\"${BUILD:-$(date -u +%Y%m%d)}\" -DPKG_BUILD_DAY=\"${BUILDDAY:-$(date -u +%d.%m.%Y)}\""

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
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
mbedtls="$repo_root/third_party/mbedtls"
out="$repo_root/build/aros"

for need in "$aros_clang" "$developer/lib/startup.o" "$sdk/gen/config/target.cfg"; do
    [ -e "$need" ] || { echo "build-aros: missing $need" >&2; exit 69; }
done

mkdir -p "$out"
cd "$repo_root"

# The certificate authorities Pkg carries, as C.
sh tools/ca-bundle-c.sh third_party/cacert/cacert.pem > "$out/pkg_cabundle.c"

# Mbed TLS, which https runs on: third_party/mbedtls, compiled for size with
# Pkg's own configuration, one object per source, rebuilt when it is older.
# MBEDTLS_TEST_SW_INET_PTON: AROS's posixc has no inet_pton (it lives in
# miami.library), so Mbed TLS reads addresses with its own.
mkdir -p "$out/mbedtls"
for c in "$mbedtls"/library/*.c; do
    o="$out/mbedtls/$(basename "$c" .c).o"
    [ "$o" -nt "$c" ] && [ "$o" -nt "$mbedtls/pkg_mbedtls_config.h" ] && continue
    # shellcheck disable=SC2086 -- the platform profile supplies separate flags.
    COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
        "$aros_clang" --target="$aros_target" $aros_arch_flags $verflags \
        -Os -std=gnu11 -c \
        -isystem "$developer/include" \
        -isystem "$sdk/gen/include" \
        -isystem "$sdk/gen/include/aros/posixc" \
        -isystem "$developer/include/aros/stdc" \
        -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' \
        -DMBEDTLS_TEST_SW_INET_PTON \
        -I "$mbedtls" -I "$mbedtls/include" -I "$mbedtls/library" \
        "$c" -o "$o"
done

# The sources the Makefile keeps (sources.mk), relative to the repository.
cd "$repo_root"
sources=$(make -s print-sources)
# shellcheck disable=SC2086 -- the platform profile supplies separate flags,
# and $sources is a list of paths without spaces.
COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
    "$aros_clang" --target="$aros_target" $aros_arch_flags $verflags \
    -O2 -std=gnu11 \
    -Wall -Wextra -Werror \
    -DMBEDTLS_CONFIG_FILE='"pkg_mbedtls_config.h"' \
    -isystem "$developer/include" \
    -isystem "$sdk/gen/include" \
    -isystem "$sdk/gen/include/aros/posixc" \
    -isystem "$developer/include/aros/stdc" \
    -nostartfiles -nodefaultlibs \
    -L "$developer/lib" -L "$aros_cross_lib" \
    -I include -I third_party/bzip2 -isystem "$mbedtls" -isystem "$mbedtls/include" \
    "$developer/lib/startup.o" \
    $sources \
    src/pkg_tls_aros.c "$out/pkg_cabundle.c" "$out"/mbedtls/*.o \
    -o "$out/Pkg" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lrexxsyslib -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport \
    -Wl,--end-group -lclang_rt.builtins-aarch64
chmod 755 "$out/Pkg"
echo "build-aros: $out/Pkg"

# The test probe, built against the AFS+ client library, read-only.
afsplus=${AFSPLUS_ROOT:-"$repo_root/../afsplus"}
if [ -f "$afsplus/native/aros/client/afsplus_client.c" ]; then
    # shellcheck disable=SC2086
    COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
        "$aros_clang" --target="$aros_target" $aros_arch_flags $verflags \
        -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-pointer-sign \
        -isystem "$developer/include" \
        -isystem "$sdk/gen/include" \
        -isystem "$sdk/gen/include/aros/posixc" \
        -isystem "$developer/include/aros/stdc" \
        -nostartfiles -nodefaultlibs \
        -L "$developer/lib" -L "$aros_cross_lib" \
        -I "$afsplus/api" -I "$afsplus/native/aros/client" \
        "$developer/lib/startup.o" \
        tools/aros/pkg_handler_rev.c "$afsplus/native/aros/client/afsplus_client.c" \
        -o "$out/PkgHandlerRev" \
        -Wl,--allow-multiple-definition -Wl,--start-group \
        -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
        -lautoinit -llibinit -lutility -lamiga -larossupport \
        -Wl,--end-group -lclang_rt.builtins-aarch64
    chmod 755 "$out/PkgHandlerRev"
    echo "build-aros: $out/PkgHandlerRev"
else
    echo "build-aros: no AFS+ tree at $afsplus; PkgHandlerRev skipped"
fi

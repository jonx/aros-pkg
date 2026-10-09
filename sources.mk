# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Every source of pkg, once. The Makefile includes this; the AROS build
# scripts and the tests that compile pkg themselves ask `make print-sources`
# for the same list, so a file added here reaches every build at once.

# libpkg's operations, cut along their own sections: see src/pkg_internal.h.
LIBMODS = src/pkg_lib_core.c src/pkg_lib_keys.c src/pkg_lib_build.c src/pkg_lib_channel.c \
          src/pkg_lib_root.c src/pkg_lib_verbs.c src/pkg_lib_resolve.c src/pkg_lib_status.c \
          src/pkg_lib_search.c src/pkg_lib_api.c src/pkg_lib_push.c \
          src/pkg_lib_events.c
LIB  = $(LIBMODS) src/pkg_activity.c src/pkg_update.c src/pkg_environment.c
# Portable C99: everything except the host filesystem layer.
CORE = src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c \
       src/pkg_manifest.c src/pkg_image.c src/pkg_ameta.c src/pkg_archive.c src/pkg_bzip2.c \
       src/pkg_pkginfo.c src/pkg_args.c
# The command line's own.
CLI  = src/pkg_selfupdate.c
# The host layer. POSIX covers macOS, Linux and AROS; Windows has its own.
HOST = src/pkg_fs_posix.c src/pkg_out.c src/pkg_port.c src/pkg_style.c

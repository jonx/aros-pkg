# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude -Ithird_party/bzip2 $(VERFLAGS)

# The version this binary says it is: the release in include/pkg.h, a patch
# number raised by hand when a release changes, and the day it was built.
# PATCH=n BUILD=20260920 on the command line pins them for a reproducible build.
PATCH    ?= 0
BUILD    ?= $(shell date -u +%Y%m%d)
BUILDDAY ?= $(shell date -u +%d.%m.%Y)
VERFLAGS  = -DPKG_VERSION_PATCH='"$(PATCH)"' -DPKG_BUILD='"$(BUILD)"' -DPKG_BUILD_DAY='"$(BUILDDAY)"' 

# The sources, listed once for every build: see sources.mk.
include sources.mk
HDR  = $(wildcard include/*.h) $(wildcard src/*.h)

UNITS = test_args test_activity test_container test_sha256 test_manifest test_ed25519 test_image test_ameta

.PHONY: all print-sources check-symbols check-cross test test-run test-ubsan check-portability check-m68k check-image check check-aros clean install aros-channel

all: build/pkg

# The list, for a build that is not this Makefile (the AROS scripts, the
# tests that compile pkg themselves): `make -s print-sources`.
print-sources:
	@echo src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# Every check this tree knows how to run.
check: check-portability check-symbols check-cross test test-ubsan check-m68k check-image

# A program links libpkg.a next to its own code and libc, so a global of the
# library named warn or hint would take the program's calls: libc has warn.
# Every global the library defines is named pkg_, pkgi_ (shared between its
# own modules, see src/pkg_internal.h), PKG_ or BZ2_, and bz_internal_error,
# which bzip2 asks the program to define. Verified to be able to fail: a
# function made global without its prefix is named here and exits 1.
check-symbols: build/libpkg.a
	@want=$$(for o in $(LIBOBJ); do basename $$o; done | sort); \
	have=$$(ar t build/libpkg.a | grep '\.o$$' | sort); \
	if [ "$$want" != "$$have" ]; then \
		echo "check-symbols: FAIL, libpkg.a holds other objects than sources.mk lists"; \
		printf '%s\n' "$$have" | grep -vxF "$$want" | sed 's/^/  not listed: /'; \
		printf '%s\n' "$$want" | grep -vxF "$$have" | sed 's/^/  missing:    /'; exit 1; fi
	@bad=$$(nm -g $(LIBOBJ) | awk 'NF==3 && $$2 != "U" {print $$3}' | sed 's/^_//' \
		| grep -vE '^(pkg_|pkgi_|PKG_|BZ2_|bz_internal_error$$)' | sort -u); \
	n=$$(nm -g $(LIBOBJ) | awk 'NF==3 && $$2 != "U"' | wc -l); \
	if [ "$$n" -eq 0 ]; then echo "check-symbols: FAIL, nm listed no symbol at all"; exit 1; fi; \
	if [ -n "$$bad" ]; then echo "check-symbols: FAIL, globals without the library's prefix:"; echo "$$bad"; exit 1; fi; \
	echo "check-symbols: PASS, $$n globals, every one prefixed"

# The builds the host compiler cannot see: each has failed on a function only
# its own #ifdef leaves unused, and -Werror makes that an error. Windows and
# both AROS CPUs, compiled every time, not at release. A missing toolchain
# fails here, naming it: a check that skips is a check that passes.
check-cross:
	@command -v $(WINCC) > /dev/null || { echo "check-cross: FAIL, $(WINCC) is not installed (brew install mingw-w64)"; exit 1; }
	@rm -f build/pkg.exe && $(MAKE) --no-print-directory build/pkg.exe > build/cross-windows.log 2>&1 \
		|| { cat build/cross-windows.log; echo "check-cross: FAIL, Windows"; exit 1; }
	@sh tools/build-aros.sh > build/cross-aros-aarch64.log 2>&1 \
		|| { tail -20 build/cross-aros-aarch64.log; echo "check-cross: FAIL, AROS aarch64 (see tools/build-aros.sh)"; exit 1; }
	@sh tools/build-aros-x86_64.sh > build/cross-aros-x86_64.log 2>&1 \
		|| { tail -20 build/cross-aros-x86_64.log; echo "check-cross: FAIL, AROS x86_64 (see tools/build-aros-x86_64.sh)"; exit 1; }
	@echo "check-cross: PASS, Windows x86_64, AROS aarch64 and AROS x86_64 build with -Werror"

build/pkg: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# libpkg for other programs: a static library and pkg.h. The host layer is
# part of it, since every operation touches files.
# Exactly the listed objects, never whatever lies in build/lib-obj: after a
# source was split or renamed, its old object stayed there, went into the
# archive ahead of the new ones, and a program linked the old code.
LIBOBJ = $(patsubst %.c,build/lib-obj/%.o,$(notdir $(LIB) $(CORE) $(HOST)))

build/libpkg.a: $(LIB) $(CORE) $(HOST) $(HDR)
	@rm -rf build/lib-obj && mkdir -p build/lib-obj
	@for f in $(LIB) $(CORE) $(HOST); do \
		$(CC) $(CFLAGS) $(CPPFLAGS) -c $$f -o build/lib-obj/$$(basename $$f .c).o || exit 1; \
	done
	@rm -f $@
	ar rcs $@ $(LIBOBJ)

# Install the tool, the library and the agents' skill: `make install`, or
# `make install PREFIX=/usr/local`. Nothing else is needed at run time.
PREFIX ?= $(HOME)/.local

install: build/pkg build/libpkg.a
	@mkdir -p $(PREFIX)/bin $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/share/pkg/skills/pkg
	cp build/pkg $(PREFIX)/bin/pkg
	cp include/pkg.h include/pkg_environment.h $(PREFIX)/include/
	cp build/libpkg.a $(PREFIX)/lib/libpkg.a
	cp skills/pkg/SKILL.md $(PREFIX)/share/pkg/skills/pkg/SKILL.md
	@echo "installed pkg into $(PREFIX)/bin; if that is not on PATH, add it"

# A channel that puts Pkg itself on AROS machines: `make aros-channel
# CHANNEL=<dir>`. See tools/make-aros-channel.sh.
aros-channel: build/pkg
	sh tools/make-aros-channel.sh "$(CHANNEL)"

# The examples, built against libpkg.a as another program would.
build/example-%: examples/%.c build/libpkg.a include/pkg.h include/pkg_activity.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

# Windows, cross-built with mingw-w64. tools/make-windows-kit.sh wraps it in a
# test kit to run on a Windows machine.
WINCC ?= x86_64-w64-mingw32-gcc
WINHOST = src/pkg_fs_win32.c src/pkg_out.c src/pkg_port.c src/pkg_style.c

build/pkg.exe: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(WINHOST) $(HDR)
	@mkdir -p build
	$(WINCC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(WINHOST) \
		-lbcrypt -ladvapi32 -lshell32

# macOS, one universal binary for Apple silicon and Intel.
build/pkg-macos: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	cc $(CFLAGS) $(CPPFLAGS) -arch arm64 -arch x86_64 -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# Linux, static against musl, cross-built with zig so no Linux toolchain is
# needed here.
build/pkg-linux-%: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	zig cc -target $*-linux-musl $(CFLAGS) $(CPPFLAGS) -static -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# The library through pkg.h alone, as another program would use it.
build/test_api: tests/test_api.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_api.c $(LIB) $(CORE) $(HOST)

build/test_update: tests/test_update.c build/libpkg.a $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

build/test_update_config: tests/test_update_config.c src/pkg_update.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_update_config.c src/pkg_update.c

build/test_environment: tests/test_environment.c build/libpkg.a $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

build/test_activity: tests/test_activity.c src/pkg_activity.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_activity.c src/pkg_activity.c

build/test_%: tests/test_%.c $(CORE) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CORE)

# `test` removes every binary first, deliberately. Editing a source and running
# the test inside the same second left a stale binary here once, and the test
# then reported the previous state of the code. A build this small buys nothing
# by being incremental, and a test that can report the wrong state is worse
# than a slow one.
test:
	@config=$$(mktemp -d); trap 'rm -rf "$$config"' EXIT HUP INT TERM; \
		XDG_CONFIG_HOME="$$config" $(MAKE) --no-print-directory test-run

test-run:
	@rm -f build/pkg build/test_api $(UNITS:%=build/%)
	@$(MAKE) --no-print-directory build/pkg build/test_api $(UNITS:%=build/%)
	@for t in $(UNITS) test_api; do echo "== $$t"; ./build/$$t || exit 1; done
	@echo "== e2e"
	@PKG=./build/pkg sh tests/e2e.sh
	@echo "== key"
	@PKG=./build/pkg sh tests/key.sh
	@echo "== interrupt"
	@PKG=./build/pkg sh tests/interrupt.sh
	@echo "== deps"
	@PKG=./build/pkg sh tests/deps.sh
	@echo "== placement"
	@PKG=./build/pkg sh tests/placement.sh
	@echo "== crossarch"
	@PKG=./build/pkg sh tests/crossarch.sh
	@echo "== status"
	@PKG=./build/pkg sh tests/status.sh
	@echo "== activity"
	@PKG=./build/pkg sh tests/activity.sh
	@PKG=./build/pkg python3 tests/activity-network.py
	@echo "== network"
	@PKG=./build/pkg sh tests/network.sh
	@echo "== channels"
	@PKG=./build/pkg sh tests/channels.sh
	@echo "== search"
	@PKG=./build/pkg sh tests/search.sh
	@echo "== push"
	@PKG=./build/pkg sh tests/push.sh
	@echo "== hostile"
	@PKG=./build/pkg sh tests/hostile.sh
	@echo "== resolve"
	@PKG=./build/pkg sh tests/resolve.sh
	@echo "== catalogue"
	@PKG=./build/pkg sh tests/catalogue.sh
	@echo "== pkginfo"
	@PKG=./build/pkg sh tests/pkginfo.sh
	@echo "== ssh-sign"
	@sh tests/ssh-sign.sh
	@echo "== docs-links"
	@sh tests/docs-links.sh
	@echo "== grammar"
	@PKG=./build/pkg python3 tests/grammar.py
	@echo "== docs-examples"
	@PKG=./build/pkg sh tests/docs-examples.sh
	@echo "== archive"
	@rm -f build/test_archive
	@$(MAKE) --no-print-directory build/test_archive
	@sh tests/archive.sh
	@echo "== fastarchive"
	@PKG=./build/pkg sh tests/fastarchive.sh
	@echo "== installer onboarding"
	@python3 tests/installer-onboarding.py
	@echo "== environments"
	@$(MAKE) --no-print-directory build/test_environment
	@./build/test_environment
	@PKG=./build/pkg python3 tests/environments_cli.py
	@echo "== selfupdate"
	@$(MAKE) --no-print-directory build/test_update build/test_update_config build/example-selfupdate
	@./build/test_update_config
	@PKG=./build/pkg python3 tests/selfupdate.py
	@PKG=./build/pkg python3 tests/selfupdate-bootstrap.py
	@echo "== examples"
	@rm -f build/libpkg.a build/example-basic build/example-browse
	@$(MAKE) --no-print-directory build/example-basic build/example-browse
	@PKG=./build/pkg sh tests/examples.sh

# The image writer judged by amitools, an FFS written apart from it. Needs the
# amitools virtualenv described at the top of tests/image.sh.
check-image: build/pkg
	@PKG=./build/pkg sh tests/image.sh

# Byte order is expressed in pkg_be32_get and pkg_be32_put and nowhere else.
# This refuses the constructs that would quietly reintroduce a host-order
# dependency. Verified to be able to fail: adding one of these to a source
# makes it report that file and exit non-zero.
#
# A socket address is the one thing outside the package format whose order
# the host's own network layer fixes, and htons is how it is written there.
# Such a line carries ALLOWED as a comment, so it says why it is there and
# every other use still fails.
FORBIDDEN = (hton[sl]|ntoh[sl]|__bswap|__builtin_bswap|BYTE_ORDER|BIG_END|LITTLE_END)
ALLOWED   = a socket port, not the package format

check-portability:
	@if grep -rnE '$(FORBIDDEN)' src include tests | grep -v '$(ALLOWED)'; then \
		echo "check-portability: FAIL, host byte order reached the sources"; \
		exit 1; \
	else \
		echo "check-portability: PASS, byte order lives only in the accessors"; \
	fi

# Unit tests and the host runs again, under the sanitizers. The
# misaligned-buffer test is the point for the container: a struct mapped over
# the stream would pass the plain build and fail here, the way it would fault
# on a 68000.
SAN = -O1 -g -fsanitize=undefined,address -fno-omit-frame-pointer

test-ubsan:
	@mkdir -p build/san
	@for t in $(UNITS); do \
		case $$t in \
			test_activity) with="src/pkg_activity.c" ;; \
			*)             with="$(CORE)" ;; \
		esac; \
		$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
			-o build/san/$$t tests/$$t.c $$with || exit 1; \
		./build/san/$$t > /dev/null || { echo "test-ubsan: $$t FAILED"; exit 1; }; \
	done
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/test_api tests/test_api.c $(LIB) $(CORE) $(HOST)
	@./build/san/test_api > /dev/null || { echo "test-ubsan: test_api FAILED"; exit 1; }
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/pkg src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)
	@PKG=./build/san/pkg sh tests/e2e.sh > build/san/e2e.log 2>&1 \
		|| { tail -20 build/san/e2e.log; echo "test-ubsan: e2e FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/deps.sh > build/san/deps.log 2>&1 \
		|| { tail -20 build/san/deps.log; echo "test-ubsan: deps FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/status.sh > build/san/status.log 2>&1 \
		|| { tail -20 build/san/status.log; echo "test-ubsan: status FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/image.sh > build/san/image.log 2>&1 \
		|| { tail -20 build/san/image.log; echo "test-ubsan: image FAILED"; exit 1; }
	@echo "test-ubsan: PASS, units, e2e, deps, status and image under -fsanitize=undefined,address"

# A big-endian COMPILE of the portable core. Running on a big-endian target is
# separate work and is not claimed here.
M68K_CC ?= $(HOME)/aros-m68k-build/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc

check-m68k:
	@if [ -x "$(M68K_CC)" ]; then \
		mkdir -p build/m68k; \
		for f in $(CORE); do \
			$(M68K_CC) -std=c99 -Wall -Wextra -Werror -O2 $(CPPFLAGS) \
				-c $$f -o build/m68k/$$(basename $$f .c).o || exit 1; \
		done; \
		echo "check-m68k: PASS, the portable core builds for a big-endian target"; \
	else \
		echo "check-m68k: SKIP, no m68k compiler at $(M68K_CC)"; \
	fi

# The client on hosted AROS: needs the AROS build under ~/aros-build, the
# crosstools under ~/aros-crosstools, and no hosted instance already running.
# Kept out of `check` because it boots an operating system.
check-aros: build/pkg
	@sh tools/build-aros.sh
	@sh tests/aros-smoke.sh

clean:
	rm -rf build

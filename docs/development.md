<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->


# Developing pkg

How pkg is built and tested, on the host, on hosted AROS and on native AROS,
how it is used as a library, and the state of its pieces. For using or
publishing with pkg, start at the [README](../README.md). Related:
[the container](container.md), [AROS defects found while building pkg](aros-defects.md),
[how pkg was established](history.md), [GOAL.md](../GOAL.md), [OPEN.md](../OPEN.md).

## Setting up

On the development machine, once:

```sh
make install                      # pkg, pkg.h, libpkg.a and the skill into ~/.local
pkg KEYGEN FILE ~/.pkg-dev.key    # the publisher key, once per publisher
```

To put pkg on AROS machines, build it for their CPUs (`sh tools/build-aros.sh`
for aarch64, `sh tools/build-aros-x86_64.sh` for x86_64) and make a channel
that carries it. Both compile Mbed TLS in, from `third_party/mbedtls` with the
configuration beside it (`pkg_mbedtls_config.h`: a TLS 1.2 and 1.3 client,
nothing else), which is what lets AROS read and push over `https`; nothing is
fetched or built apart, and the same recipe serves every CPU.

```sh
PKG_SIGNKEY=~/.pkg-dev.key make aros-channel CHANNEL=<dir>
```

On the AROS machine, with that directory reachable (a shared folder, a disk,
an image, a network share) under the name that machine gives it, one line:

```
Execute <ch>/Install-Pkg <ch>             ; Work:channel, say
Execute DEPOT:Install-Pkg DEPOT:          ; a volume that is the channel
```

The script finds the build that runs on that machine and it installs the
signed `pkg` package into `SYS:` (or a root given after the channel); from
then on pkg is a package like any other, upgraded with `Pkg UPGRADE pkg`.
`tests/native-x86_64.sh` does exactly this on native AROS, where the aarch64
build is tried first and does not run.

## Build and test

```sh
make test
```

`cc -std=c99 -Wall -Wextra -Werror`. `make check` runs everything: the
portability grep, the unit tests, the end-to-end run, all of them again under
`-fsanitize=undefined,address`, and a big-endian compile of the portable core.
`make test` removes the binaries before rebuilding, deliberately: see the
comment in the `Makefile`.

### What the end-to-end run proves, and how it was checked

`tests/e2e.sh` publishes a drawer, installs it into a root, lists, verifies,
edits a file, verifies again, removes, and then runs the refusals: a republished
version with different bytes, a payload with one byte flipped, a second install,
an overwrite of a file already in the root, an unknown package, an absent
version, and three channel entries **forged in Python** rather than by `pkg`: a
traversal in the manifest, a container disagreeing with its manifest, and a
payload aimed at the package database.

Its oracles are independent of the code: `shasum -a 256` for every digest, `cmp`
for installed bytes, and Python for the forged entries.

The suite was run against three deliberate defects, each built separately:

| Defect | Result |
|---|---|
| Whole-payload digest check disabled | 1 check fails, the one pinning that refusal's message. The tampered payload is **still refused**, by the per-file digests, so the two layers are independent and each is exercised |
| Unsafe-path refusal disabled | 4 checks fail, including "nothing written outside the root": with the guard gone, `../evil` **was** written outside it |
| macOS metadata filter disabled | 5 checks fail, exactly the metadata ones |
| Signature verification disabled | The altered signature installs, and its check fails. The unsigned refusal still holds, correctly, since it comes from the missing file |
| Key pinning disabled | 6 checks fail, all in the substituted-key section |
| Edited-file protection disabled | 3 checks fail, exactly the edited-file ones |

`tests/status.sh` (STATUS and UPGRADE ALL, 66 checks at the time; 67 since UPGRADE ALL goes as far as possible) was run the same way
against 28 deliberate defects, each on a copy of the tree: every state
misjudged in turn, the root's CPU ignored, withdrawn versions picked,
dependency order lost, the refusal handling broken,
the items not sent, `DRYRUN` ignored, a `getchar()` added, downgrades let
through, STATUS writing into the root or failing when updates exist, and
each usage refusal removed. Each made its own checks fail. The stdin check
reads the offset of a descriptor the shell shares with the command: a
program that reads its stdin moves it.

A first run of that last control reported 38 failures. The mutated binary had
been built without `-Werror` and its compiler output cut off, so the 38 measured
a broken build and said nothing about the filter. Rerun with the build verified
first, it gave the 5 above. Recorded because a control that fails for the wrong
reason looks exactly like one that works.

## Use, on macOS

```sh
make
./build/pkg KEYGEN FILE ~/.pkg-dev.key          # once
export PKG_SIGNKEY=~/.pkg-dev.key
./build/pkg PUBLISH ~/dev/MyTool CHANNEL ~/pkg-channel KIND image
./build/pkg INSTALL mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg LIST ROOT ~/aros-root
./build/pkg VERIFY mytool ROOT ~/aros-root
./build/pkg UPGRADE mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg STATUS ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg UPGRADE ALL ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg ROLLBACK mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg REMOVE mytool ROOT ~/aros-root
```

**Every package is signed; there is no development mode.** A development key
is a real Ed25519 key, written readable by its owner alone. The signature covers
the manifest, and the manifest names the payload digest, so one signature covers
every byte installed. The first install of a package into a root pins the key
that signed it, in `.pkg/keys`; a later install or upgrade signed by another key
is refused with both keys printed, and goes through only with `ACCEPTKEY`
followed by the new key in full.

`UPGRADE` with no `VERSION` takes the highest published version; with `VERSION`
it takes exactly that one. An older version needs `DOWNGRADE`. `ROLLBACK` returns
to the version installed before the last change, fetched again from the channel,
which never changes a published version. An upgrade that would overwrite a file
the user edited is refused before anything moves.

`KIND` is required on the first `PUBLISH` of a package; later versions take
`KIND` and `DEPENDS` from the last one published, and say so with
`kind-from:` and `depends-from:` (`DEPENDS none` for a version that needs
nothing). The kinds: `image` for a program people run, one
volume to mount; `application` for a program as loose files; `library`,
`device` (handlers too), `class`, `font`, `catalog`, `startup`, `boot`,
`data`, `sdk`, `slave`. A missing or unknown kind is refused with the list,
and a near miss (`handler`, `tool`) with the kind to use.

Name and version come from the `$VER:` cookie when `NAME` and `VERSION` are
not given. Keywords are case-insensitive, AmigaDOS style.

A channel is a directory, created by the first `PUBLISH` into it: `index`
holds one `name version arch digest` line per published version and CPU,
where the digest is the SHA-256 of that version's manifest. `objects/`
holds, per version, `<manifest digest>.manifest` (named by its own
SHA-256), `.sig` (its signature) and, once withdrawn, `.withdrawn` and
`.withdrawn.sig`; and per payload `<payload digest>.pkg`, named by its own
SHA-256, which the manifest's `Payload:` line gives. A package whose files
stay in an archive has no `.pkg`: its manifest's `Source:` names
`archives/<name>` in the channel. A root keeps its own database in `.pkg/db`, so a machine can
hold several roots without interference.

Host metadata the Amiga side has no use for, `.DS_Store` and AppleDouble `._`
files, is left out of every package and counted in the publish report. Without
that, the same drawer would give different manifests on two Macs.

## On hosted AROS

`sh tools/build-aros.sh` cross-builds `Pkg` for aarch64 AROS, and `PkgHandlerRev`,
a test probe that asks a mounted AFS+ volume which handler revision serves it.

`make check-aros` boots hosted AROS once: a package published and signed on
macOS is installed through the `MacRW:` share, verified, listed, removed, and a
tampered payload is refused with AmigaDOS seeing the error.

`tests/aros-handler.sh` is M3. It installs the real AFS+ handler with pkg, over
four boots, so that a restart is a restart:

| Boot | What happens | What the handler itself reports |
|---|---|---|
| 1 | Install revision 14, mount; upgrade to 15 while mounted | 14, then still 14: the loaded handler runs on until restart |
| 2 | Restart | 15 |
| 3 | Upgrade to a 16 that cannot load; it fails to start, and the startup sequence runs `Pkg ROLLBACK` | the start fails, AmigaDOS sees it |
| 4 | Restart after the fallback | 15 |

It needs two handler packages built by AFS+'s own `tools/package-aros-alpha0.sh`,
at interface revisions 14 and 15:

```sh
PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/aros-handler.sh
```

Four things the hosted runs established, none of them guessed beforehand:

- **Output.** posixc's `stdout` reached nothing a shell redirection could see;
  output goes through `dos.library` to `Output()`, refusals included.
- **Exit codes.** A refusal is `RETURN_ERROR` (10), since `If ERROR` tests for
  10 and a POSIX 1 would pass as success.
- **errno.** posixc reports no `EEXIST` for an existing directory and no `ENOENT`
  from `opendir` on an absent one; existence is tested, never inferred.
- **Formats.** AFS+ changes its on-disk format without keeping legacy readers,
  so a revision 14 handler refuses an image written by revision 15 tools. For
  the tool this is the concrete case behind `[PKG22]` item 5: rolling a handler
  back across a format change leaves volumes unreadable, so a package that owns
  an on-disk format has to declare whether its state survives a downgrade, and
  ROLLBACK has to honour that declaration. Not built yet; recorded as the next
  piece of the version model.

## Native AROS in QEMU

`sh tools/build-aros-x86_64.sh` builds pkg for native AROS on x86_64 with the
Homebrew LLVM (which knows the `x86_64-unknown-aros` triple but predefines none
of the AROS macros, so the script does) against the SDK of a nightly
linux-x86_64 system, linking with a `collect-aros` it builds for x86_64 from the
AROS sources.

`tests/native-x86_64.sh` runs goal 2 on an unmodified nightly pc-x86_64 AROS in
QEMU, nothing hosted and no host share: Guru, Function and identify.library are
taken out of the ISO itself, published on the host (the architecture read from
their ELF headers: x86_64), and the ISO is rebuilt without them, with the
channel, pkg and the sequence in `S:User-Startup`. From its own startup AROS
installs Guru with its dependency, runs Guru from the image mounted through pkg
MOUNTLIST with the system's own FFS (no handler component: the native system
has one), upgrades, rolls back, verifies, refuses to remove identify while Guru
needs it, and removes Guru and the orphan. Results leave through the second
serial port. 27 checks; a control shows Guru cannot open identify.library
before the root's libraries are reachable.

## Windows, macOS and Linux

`make build/pkg.exe` cross-builds pkg for x86_64 Windows with mingw-w64;
`src/pkg_fs_win32.c` is the host layer. Paths stay UTF-8 inside pkg and go
through the wide API, so names outside the ANSI code page work, and the
command line is read back as UTF-16 for the same reason. Replacement is
`MoveFileExW` with write-through, a signing key is created with a DACL for
its owner alone from the first instant, randomness comes from
`BCryptGenRandom`, and output is in binary mode so a newline stays one byte.
`make build/pkg-macos` builds a universal binary, and
`make build/pkg-linux-x86_64` or `-aarch64` a static Linux one with zig.

`sh tools/make-test-kit.sh` writes `build/pkg-test-kit.zip`: pkg for the
four targets, the contract channel, `tests/contract-steps.txt` (the sequence
hosted AROS runs too), the reference answers, and `run.ps1` for PowerShell
5.1 on Windows or PowerShell 7 anywhere. It compares every exit code and
machine output byte for byte, then checks the key's permissions (ACL on
Windows, mode 0600 elsewhere), a root named outside ASCII, and an image
written on the host against the reference bytes, and writes `report.txt`.
Run here with PowerShell 7 on macOS: 87 checks, 0 failures, and one altered
expectation is caught.

Not run yet: Windows and Linux. No machine of either here, and Wine's
Homebrew casks were withdrawn on 2026-09-01. The owner runs the kit.

### AROS itself as packages, on native AROS

`tools/aros/publish-system.sh <tree> <channel> <nightly date>` publishes a
pc-x86_64 boot ISO's tree as the nine packages of
`tools/aros/split-system.txt` (boot, base, prefs, fonts, locale, tools,
demos, extras, sdk), versioned `0+<date>`. Two files matter more than they
look: `AROS.boot`, which goes with aros-boot, since dos.library boots only
from a volume whose `AROS.boot` names its CPU; and
`boot/grub/i386-pc/core.img`, a configuration file, since `Install-grub2`
writes into it where GRUB lies on the disk: REPAIR must never put the
unpatched one back.

`tests/native-system.sh` checks it in QEMU from an empty disk, all of it in
a Shell window on the Workbench screen: partition, format and copy as
InstallAROS does, pkg adopting each package, GRUB, a reboot from the disk;
there VERIFY ALL, a program deleted and one overwritten by accident and
Shell-Startup edited, VERIFY ALL naming each, REPAIR ALL, and the restored
program running. `PKG_DISPLAY=cocoa` shows the screen while it runs; it is
recorded either way (`tools/qemu-frames.py`), and `tools/qemu-video.sh`
makes the video.

## Several CPUs

How a channel holds one version for several CPUs and how a root picks its
build is in [Distributing builds for several platforms](distributing.md);
`tests/crossarch.sh`, 20 checks, proves it.

## As a library

`include/pkg.h` is the interface; the operations live in `src/pkg_lib_*.c`,
one file per part of the work (output and refusals, keys, building a package,
channels, the root, the verbs, RESOLVE, STATUS, SEARCH, the interface, PUSH),
and what those files share with each other, and with nothing else, is declared
in `src/pkg_internal.h`; `src/pkg_main.c`, the `pkg` command, only turns words
into `pkg_options` and prints what comes back. The sources are listed once, in
`sources.mk`, for every build.

What each verb takes is written once, as a template in the manner of
AmigaDOS's ReadArgs, in the verb table of `src/pkg_main.c`; `src/pkg_args.c`
reads a command's words against it. A word a verb does not take is refused by
name. `pkg HELP` draws the usage from the templates, `pkg HELP MACHINE` prints
every verb's `verb:`, `syntax:` and `template:` as records, `pkg <verb> ?` shows
one, and `PKG_CHECK_WORDS=1` judges a command's words without running it.
`tests/grammar.py` holds `docs/reference.md` and each `docs/commands/` page to
the templates, and `tests/test_args.c` holds the reader to its rules. A graphical front end, an installer or an agent's tool
adapter links the library (`make build/libpkg.a`) and calls `pkg_install`,
`pkg_show` and the rest with a sink: structured, it receives the same fields
the command line prints with `MACHINE`; otherwise sentences for a person; and,
if it asks, a trace of every step and its [progress events](events.md). The
library writes nothing to stdout and reads no environment. `tests/test_api.c`
drives it as a front end would; `tests/test_events.c` checks every event of
every operation it runs, through `tests/events.sh`.

## Examples

`examples/basic.c` is the shortest useful program on libpkg: a key, a
publish, an install, a listing, and a refusal read with its `next`.
`examples/browse.c` is a package browser in the shape a graphical front end
takes: items with their fields apart, whether each version is installed,
suggestions for a mistyped name, a Cancel button, the trace. It was first
written by an agent from `pkg.h` alone, as a test of that header, then
adopted. `examples/events.c` runs an install and an upgrade on a worker
thread and shows their progress events on the main one, through a queue, as
a window would. All three build against `build/libpkg.a` and run in
`make test` (`tests/examples.sh`).

## Diagnosing

`TRACE <file>` on any command, or `PKG_TRACE=<file>` (`-` for stderr), writes
the operation's own account: every file read, written, moved or deleted,
every check with what was expected and found, every choice with its reason,
and each refusal as it happens. It never mixes into stdout.

## State

Goals and milestones: [GOAL.md](../GOAL.md). What remains: [OPEN.md](../OPEN.md). **Goal 1 is met**: all four milestones, and the whole sequence passes as one run, `tests/goal.sh`, 25 checks. **Goal 2 is met** within its agreed line: an application arrives with its dependencies and runs, checked from outside, with no ARexx anywhere (`tests/goal2.sh`, 51 checks), the contract is identical on AROS, macOS and through the ARexx port (`tests/aros-contract.sh`, 152 checks), and the Windows, macOS and Linux kit is ready; its Windows run is the owner's.

| Piece | State |
|---|---|
| `.pkg` container, reader and writer | Built, with its test |
| Byte-order discipline and its checks | Built |
| SHA-256 | Built, checked against the NIST vectors |
| Text manifest | Built, strict parser, with its test |
| Host filesystem layer | POSIX, used on macOS, Linux and AROS through its posixc library |
| `MANIFEST`, `PUBLISH`, `INSTALL ROOT`, `LIST`, `VERIFY`, `REMOVE` | Built, end-to-end test on macOS |
| SHA-512 and Ed25519 | Built, checked against FIPS and the RFC 8032 vectors |
| `KEYGEN`, `SIGN`, signed `PUBLISH`, key pinned per package in the root | Built, end-to-end test |
| `UPGRADE`, `ROLLBACK`, `DOWNGRADE`, `EXACT` and `COMPATIBLE` selection | Built, end-to-end test |
| AROS client | Built with `tools/build-aros.sh`; `make check-aros` and `tests/aros-handler.sh` on hosted AROS |
| ARexx port `PKG` | Built: `Pkg PORT` on AROS, every verb, RESULT on success, RC the class code and `LASTERROR` on refusal |
| Machine contract, `MACHINE` or `PKG_OUTPUT=machine` | Built: `tests/e2e.sh` on macOS, `tests/aros-contract.sh` compares macOS and hosted AROS line for line |
| Image route, `KIND image` and `IMAGE` | Built: FFS images, validated by amitools in `tests/image.sh`, mounted by the AROS FFS handler in `tests/goal2.sh` |
| `Depends`, resolution, orphans, `REMOVE ORPHANS` | Built: `tests/deps.sh` on macOS, `tests/goal2.sh` on hosted AROS |
| Removing pkg itself, and moving to an incompatible pkg | Built: [Removing pkg](removing.md), proven by `tests/aros-remove-pkg.sh` on hosted AROS |
| `STATUS`, `UPGRADE ALL`: checking and updating a root, unattended | Built: `tests/status.sh` on macOS, 67 checks; on hosted AROS in the contract, AmigaDOS and ARexx |

## The version a build says it is

`PKG_VERSION_RELEASE` in `include/pkg.h` is the release, and stays put until
one is declared. What a binary reports is that release, a patch number and the
day it was built: `1.8.0+20260923`. The build passes the last two in, so a
binary always says which day it came from, and pkg orders them as it orders any
version: `1.8.0+20260923` comes after `1.8`, a later day after an earlier one,
and a raised patch after both.

```sh
make build/pkg && ./build/pkg VERSION     # pkg 1.8.0+20260923 (23.09.2026)
```

Raise `PATCH` when a release changes in a way people should be able to ask for
by version; the day moves on its own. Both can be pinned, for a build that must
come out byte for byte the same as an earlier one:

```sh
make PATCH=1 BUILD=20260923 BUILDDAY=23.09.2026 build/pkg
PATCH=1 BUILD=20260923 BUILDDAY=23.09.2026 sh tools/build-aros.sh
```

A build that says nothing carries `+00000000`, and nothing published ever
should: `tools/make-aros-channel.sh` refuses it.

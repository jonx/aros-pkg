<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# pkg in a development loop

You build on a computer and test on an AROS machine: a native one, an
emulator, or AROS hosted beside your editor. Every round trip carries new
files to the machine, and after a crash, a reset or an interrupted copy you
need to know what the machine is really running. pkg turns that round trip
into something you can check at both ends. This guide shows the loop, then
what each step buys you.

## Why a package rather than a copy

Copying a drawer works until it does not. The copy stops half way, an older
library stays behind, a file was edited by hand on the target, or you are no
longer sure which build is on the machine. With pkg each build is a signed
package, each file is checked against its digest before it is written, and
the machine keeps a record of what it installed. So:

- the state of the machine is a question with an answer: `VERIFY` says whether
  every file is the one the package holds, after any crash or reset;
- a bad build goes back with `ROLLBACK`, a damaged file with `REPAIR`;
- an interrupted change is finished by running the same command again;
- only builds signed by your key are accepted, so a stale or foreign binary
  cannot slip in.

## The loop

**1. Make a key once**, on the computer you build on, and keep it outside the
repository:

```sh
pkg KEYGEN FILE ~/.config/pkg/dev.key
export PKG_SIGNKEY=~/.config/pkg/dev.key
```

**2. Publish each build into a channel of your own.** A channel is a
directory; the first `PUBLISH` creates it. Look first with `DRYRUN`: it names
every file that changes against the version before, without writing anything.

```sh
pkg PUBLISH build/MyTool CHANNEL dev KIND application DRYRUN
pkg PUBLISH build/MyTool CHANNEL dev KIND application
```

**3. Try it on the computer before the machine.** The same pkg installs into
a scratch root on macOS, Linux or Windows, so a broken package is caught before
it travels:

```sh
pkg INSTALL mytool ROOT scratch CHANNEL dev
pkg VERIFY mytool ROOT scratch
```

**4. Carry the channel to the machine** by any route that keeps files whole: a
shared folder, a USB stick, a disk image, an HTTPS server, or your own cable.
pkg checks every file it reads, so the route does not have to be trusted.

**5. Install or upgrade on the machine**, into a root you use for testing
rather than `SYS:`, so a failed experiment never touches the system:

```amigados
Pkg UPGRADE mytool ROOT Work:Dev CHANNEL Work:Channels/dev DRYRUN
Pkg UPGRADE mytool ROOT Work:Dev CHANNEL Work:Channels/dev
Pkg VERIFY mytool ROOT Work:Dev
```

**6. When something goes wrong**, ask before you guess:

```amigados
Pkg VERIFY ALL ROOT Work:Dev
Pkg REPAIR mytool ROOT Work:Dev CHANNEL Work:Channels/dev
Pkg ROLLBACK mytool ROOT Work:Dev CHANNEL Work:Channels/dev
```

After a crash in the middle of a change, run the same `INSTALL`, `UPGRADE` or
`ROLLBACK` again: pkg finishes it, and says how many files were already in
place (`resumed-files:`).

## Split what you ship

Make one package per part that changes on its own: the large runtime that
rarely changes, and the small tools you rebuild every hour. A new tool is then
a small package to carry, not the whole system, and `STATUS` shows which parts
of the machine are behind the channel:

```amigados
Pkg STATUS ROOT Work:Dev CHANNEL Work:Channels/dev
```

Keep out of pkg what the machine needs before it can run pkg: the kernel, the
boot files and anything loaded before DOS. pkg changes what runs after boot;
the boot itself is yours to change and to recover.

## Where it pays off

**Developing a file system.** A handler under test crashes, hangs, or leaves
a volume it cannot read again, and after the reset you no longer know whether
the handler on disk is the build you meant to test. Keep the handler and its
mount entry in a package installed into a root on another volume than the one
under test. After each reset, `VERIFY` says whether the binary is the one you
published; `ROLLBACK` brings back the last build that mounted; and a
package of test images gives every run the same starting bytes, since
`REPAIR` puts back an image a failed run changed:

```sh
pkg PUBLISH testimages CHANNEL dev KIND data
```

```amigados
Pkg REPAIR testimages ROOT Work:Dev CHANNEL Work:Channels/dev
```

**Porting AROS to a new machine.** Early on there is no network, the storage
driver is new, and resets are frequent. The channel is a directory, so it
travels on whatever works that day: a USB stick, a card, a disk image, a cable
of your own. The boot image stays outside pkg, and everything above DOS is a
package: once the machine reaches a Shell, userland is updated without
rebuilding or reflashing the boot image, and a reset in the middle of an update
is finished by repeating it. One channel serves every CPU you build for
([Distributing builds](distributing.md)), so the new port and the machines you
already have read the same channel.

**Working on a library.** A program that crashes may simply be loading an
older copy of your library from somewhere else in `LIBS:`. `RESOLVE` shows
which copy the program gets and why:

```amigados
Pkg RESOLVE mylib.library ROOT Work:Dev
```

**Finding the build that broke something.** Every version you publish stays
in the channel, so going back is an install, not a rebuild. Step through the
builds on the machine until the fault appears:

```amigados
Pkg UPGRADE mytool ROOT Work:Dev CHANNEL Work:Channels/dev VERSION 1.4 DOWNGRADE
```

**Several test machines.** A native machine, an emulator and a hosted AROS
read the same channel; `STATUS` on each says which ones are behind, and
`VERIFY` which one was changed by hand.

## Let a script or an agent drive it

- `MACHINE` gives `key: value` lines only, the same on every system, and the
  exit code says what kind of refusal it was
  ([Exit codes](reference.md#exit-codes)). A test harness reads those instead
  of scraping text.
- In an unattended run, name the publisher you expect with
  `KEY <your public key>` on `INSTALL`, `UPGRADE` or `ROLLBACK`: a package
  signed by another key is refused, even on its first install
  ([Signatures and trust](signing.md)).
- One change of a root runs at a time: a second pkg that would change the same
  root is refused with 15 and `next: retry-later`, so two scripts cannot mix
  their files.
- After a change, `flushed:` says whether it was written back to the medium:
  `flush`, `host`, or `no` when the file system cannot be asked to.

## Keep evidence

What a run proves is worth keeping beside your test results:

- `LIST` with `MACHINE` names each installed package and its version, and
  `SHOW` each published version with its signer;
- `VERIFY` after a reboot shows the change survived it, not only that it ran;
- `SHOW` with `ROOT` marks which published version a root holds, so the
  channel and the machine can be compared line by line.

## What to expect on a busy machine

pkg checks every digest and decompresses every archive, which takes CPU for a
large package. On AROS, tasks below pkg's priority wait until it is done; a
service that must keep answering during a long change (a remote console, a
test link) should run above it, or pkg should run below it. A program that is
already loaded in memory keeps running the old copy until it is restarted,
and a resident module or library in use needs a reboot to be replaced.

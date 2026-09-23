<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Open items

What is known to remain, in one place, to fix when the time comes. Each item
says where it was seen and what would close it. Written 2026-09-18, after
goal 2.

## AROS defects not fixed

Details in [docs/aros-defects.md](docs/aros-defects.md).

| Item | Seen in | What would close it |
|---|---|---|
| `C:Unpack` does not load on hosted aarch64 ("file is not executable"), shipped or rebuilt | goal 1 bootstrap; board thread 17 | Find why the loader refuses it (hunk or ELF flags, relocation); then bootstrap through `Unpack` as first designed |
| posixc `stdout` reaches no shell redirection | first AROS runs | Fix in posixc, or document that CLI tools must write through `Output()` |
| posixc reports no `EEXIST` or `ENOENT` where POSIX does | first AROS runs | Fix the errno mapping in posixc |
| A command that cannot load leaves `$RC` unchanged | goal 2, four-case check | Compare with AmigaOS 3.x first; if AmigaOS sets a failure code, set it in the AROS shell too |
| The AROS Installer cannot install most scripts, nor run unattended, nor report what it wrote or whether it failed | 2026-09-18, the Installer modes | Implement the missing commands, a real unattended mode, exit codes and icon start; details in [docs/aros-defects.md](docs/aros-defects.md) |
| collect-aros writes ELF ABI version 1 whatever the ABI, and the loader never checks it | 2026-09-18, the ABI field | Write the real ABI version per target in collect-aros, and have the loader refuse a mismatch; Pkg will not rely on it either way |
| The darwin hosted build ships no FFS handler | goal 2 | Add `kernel-fs-afs` to the hosted build, or say why it is left out |

## Proposed upstream, waiting

| PR | Fix | Local branch |
|---|---|---|
| aros-development-team/AROS#1238 | identify.library decodes dead-end alerts | `~/aros-pr-identify`, `fix/identify-deadend-alerts` on jonx/AROS |
| aros-development-team/contrib#64 | Regina gives RC the port's number, RESULT its string | `~/aros-pr-regina`, `fix/regina-arexx-rc` on jonx/contrib |

When both are merged: drop `tools/aros/regina-arexx-rc.patch` and the copy
step in `tools/build-aros-regina.sh`, decode a dead-end alert in
`tests/goal2.sh` again, and remove the two worktrees.

## Native AROS runs

- **pc-x86_64 in QEMU**: done 2026-09-18, `tests/native-x86_64.sh`, 31
  checks, Pkg bootstrapping itself from a channel named by its root
  (`DEPOT:`). Two AROS defects found on the way, in [docs/aros-defects.md](docs/aros-defects.md): `Lock()`
  misses later directories of a multi-directory assign, and on the CD-booted
  native system a RAM: directory added to `LIBS:` is not searched by the
  library loader. Worth a look in dos.library before an upstream report.
- **m68k**, later, at the owner's request: an Amiga emulator (UAE) with the
  AROS m68k build in `~/aros-m68k-build`; QEMU does not emulate an Amiga.

## Pkg, not built yet

- **Runs on Windows and Linux.** `build/pkg-test-kit.zip` is ready; only macOS
  has run it. The owner runs it on Windows.
- **A version rule for formats.** A package that owns an on-disk format must
  declare whether its state survives a downgrade, and ROLLBACK must honour it
  (`[PKG22]` item 5; README, "On hosted AROS").
- **Block-compressed images.** The image is stored whole; chunk size and codec
  are still open in the planning repository's packaging README.
- **Images over about 49 MB.** Bitmap extension blocks are not written, and
  such an image is refused.
- **Mounting as one command.** MOUNTLIST writes the entry and lists the
  `step:` commands, but AmigaDOS cannot easily run those lines from Pkg's
  output, so a person or an agent copies them into a script. Pkg could write
  that script too (an agent in round 5 wrote one by hand, checked on the host
  only).
- **A drawer on a mounted AFS+ volume.** The AFS+ macOS driver exposes each
  file's comment and protection word as `afsplus.aros.comment` and
  `afsplus.aros.protection` xattrs (afsplus e026786, ADR-120). PUBLISH could
  read them as the native source there, before `.ameta`; today such a drawer
  needs a `.ameta` like any host folder.
- **The single-file bootstrap, rest of `[PKG23]`**: a database location for
  read-only media, self-upgrade with a fallback, a lock against two Pkg runs
  on one root.
- **An upgrade that is refused at dependency level only names `UPGRADE`**; it
  does not offer to upgrade the dependency in the same run.
- **Commands for the download cache.** Only `PKG_CACHE` and the paths in the
  docs manage it today. Wanted: a verb (or `STATUS`) that prints where the cache
  is on this machine and how big it is, including when AROS fell back to
  `RAM:pkg-cache`; a `NOCACHE` switch that neither reads nor keeps downloads
  for one run; and a way to clean the cache, all of it or only what no
  installed package still needs (old versions, archives from past nightlies).
  `docs/reference.md` still says `T:pkg-cache` on AROS, while the code uses
  `SYS:.pkg/cache`, falling back to `RAM:pkg-cache`.
- **Network channels, publishing from a GitHub link, Aminet and AmigaOS
  interoperability, WHDLoad, paid packages, licences listing**: designed in
  the planning repository, none built.
- **No `[PKG*]` gate is claimed.** Goals 1 and 2 touch many gates, but none has
  its `pkg-*.json` verdict artifact; `STATUS.md` in aros-next still says spec.

## Guidance added from the agent rounds

Rounds 1 to 5 each had an agent use Pkg from the skill alone, on a different
task. What they got wrong turned into refusals and hints rather than more
text: `KIND` is required on PUBLISH (a missing kind used to default to
application), a near-miss kind is refused with the right one, `hint:` lines
after KEYGEN, a publish that creates its channel, an image install and
MOUNTLIST, `content:` lines in an image's dry run, and a warning when a
program kind holds no executable. Still to watch in the next rounds: whether
agents choose a root (`SYS:` or another) without asking, and whether they
make a new key when the person's cannot be found. Round 6 put another program
in a game's drawer; the agent caught it from a version warning, and since
then a file whose `$VER` names another program is warned about, and a name
taken from such a cookie that would replace a published package of another
kind is refused. Round 7 handed a teammate's "fix" that was the published
build plus one byte, with only a colleague's key at hand; the agent refused
and asked. Since then a dry run needs no key and names the key the real
publish needs, an image's manifest lists the files inside it, and the dry
run compares a new version with the last one file by file; the missing key
is class 14 (key), no longer 20.

## Icon-launched installation scripts

Provide an installation script and small launcher icon for LunaPaint from contrib.
The requested target is AROS x86_64 SMP. UNVERIFIED: the repository location
(suggested path `gfx/Multimedia/lunapaint`), package/channel identity and target ABI.
Confirm these before choosing the signed package to install.

Use a shared package/channel description with a launcher for each supported desktop.
AROS uses an icon and ToolTypes; other platforms need their own launcher formats.
A tooltip supplies descriptive text. ToolTypes supply configuration.
The launcher must expose progress, errors and user confirmation, preserve signature
checks, and handle a missing pkg executable. Acceptance includes a double-click
installation of LunaPaint on the requested AROS target and explicit platform tests
for every additional launcher supplied.

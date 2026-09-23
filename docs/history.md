<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# How pkg was established

The record of the goal sequences that proved pkg, kept as history. The goals
themselves are in [GOAL.md](../GOAL.md); what remains in [OPEN.md](../OPEN.md).
Nothing here is a promise about the current tool: the guides describe that.

## The goal sequence

`tests/goal.sh` runs the goal as one sequence that passes or fails:

1. On macOS, the AFS+ handler at interface revisions 14 and 15 is published into
   a directory channel, signed with a development key. An attacker publishes a
   16 into the same channel with another key, and a copy of the channel has one
   payload byte flipped.
2. On hosted AROS, one boot: `Pkg` bootstraps from a plain archive with no
   package manager present, installs itself as a signed package, and serves
   the `PKG` ARexx port from that managed copy.
3. One ARexx script, `tests/goal.rexx`, drives install 14, verify, upgrade 15,
   verify, rollback 14 through the port, checking RC and the database at every
   step, and exits 10 at the first disagreement.
4. Inside that script the tampered payload and the substituted key are refused
   with RC 12 and RC 14, their class codes, each for its own reason, read back
   with `LASTERROR`.

Afterwards AROS mounts the volume with the handler pkg left in place and it
reports revision 14. A second boot runs the same script with one expectation
sabotaged, and must see it stop at that line with an error reaching AmigaDOS:
a sequence that cannot fail proves nothing.

What it took, beyond the tool:

- **An ARexx interpreter.** Hosted AROS ships `rexxsyslib.library` and no
  interpreter. `tools/build-aros-regina.sh` cross-builds Regina's static `rexx`
  from the AROS contrib sources, read-only. Regina resolves `ADDRESS <name>` to a
  public port and sends it `RXCOMM` messages, so no RexxMast is needed.
- **A Regina fix.** For an ARexx port Regina set RC to the command's RESULT
  string instead of the host's numeric `rm_Result1`, so every successful
  command read as a failure. `tools/aros/regina-arexx-rc.patch` gives RC the
  number and RESULT the string, as ARexx specifies; it is applied to a copy at
  build time and kept as a file to offer upstream.
- **A different bootstrap.** AROS's own `C:Unpack` reads the `.pkg` container
  this tool adopted and was the first choice. On hosted aarch64 AROS it does not
  load: the shell answers "file is not executable" for the shipped binary and
  for one rebuilt from its sources with `tools/build-aros-unpack.sh`. The
  bootstrap uses the `minigzip` AROS ships instead: `Pkg` is one file, so that
  file compressed is its plain archive. The `Unpack` defect is AROS's; see
  [AROS defects found while building pkg](aros-defects.md).

```sh
sh tools/build-aros.sh && sh tools/build-aros-regina.sh
PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/goal.sh
```

## Goal 2: an application and its dependency, with no ARexx

`tests/goal2.sh`, 51 checks, one boot, driven by the AmigaDOS startup and
nothing else. Hosted AROS installs no ARexx interpreter, and the test checks
that none is present and that the script names none.

1. On macOS, Guru, the alert decoder that ships with identify.library in the
   AROS sources, is published as a signed image at 2.0 and 2.1 (2.1 adds the
   `Function` tool), depending on `identify >= 37.1`, published as a library
   component. The AROS FFS handler is published as a device component, since
   the hosted build ships none. Every version is the one in the binary's
   `$VER`.
2. On hosted AROS: `Pkg` bootstraps and installs itself; installs the FFS
   handler; installs Guru 2.0, which brings identify in first. The image is
   write-protected, mounted through `fdsk.device`, and Guru runs from it. A
   control run first, before the root's `Libs` joins `LIBS:`, must fail with
   "Could not open version 37 or higher of library identify.library" and RC 20.
   Then upgrade to 2.1, rollback to 2.0, each image ejected, replaced and
   mounted again, Guru run and the volume listed each time.
3. `VERIFY` finds the image intact after three mounts: nothing wrote to it.
4. A tampered image is refused with 12, a badly signed dependency with 13 and
   nothing placed. `Pkg` removes itself and Guru still runs. Last, a
   bootstrapped `Pkg` refuses to remove identify while Guru needs it (16),
   removes Guru, reports identify as an orphan, and `REMOVE ORPHANS` takes it
   out; the FFS handler, installed by name, stays.

Guru's output is compared on the host with the strings in its own sources:
`exec/alerts.h`, the two catalogue descriptions and the table in `idalert.c`.

```sh
make && sh tools/build-aros.sh && sh tools/build-aros-extras.sh
sh tests/goal2.sh
```

### The image route

`KIND image` turns the drawer into a Fast File System volume, `DOS\3`, written
once in memory by `src/pkg_image.c`, and the package holds that one file,
`<name>.hdf`. `pkg IMAGE <drawer> OUT <file>` writes the same file without a
channel. Geometry is fixed (512-byte blocks, 32 per track, two reserved), so
the size in the signed manifest gives the mount entry. The same drawer always
gives the same bytes. The manifest also lists the files inside the image,
one `Content: <sha256> <size> <path>` line each, the syntax of `File:`, so
that the dry run of a new version can say which files changed since the
last one.

### Channels over the network

`CHANNEL http://host/pkg` (or `https://`) reads a channel served over the
network exactly like a directory channel, file for file: SHOW, INSTALL,
UPGRADE, ROLLBACK, STATUS and UPGRADE ALL all take one. Files are fetched
into a cache (`PKG_CACHE`, else `~/.cache/pkg`, `%LOCALAPPDATA%\pkg-cache`
on Windows) the first time they are needed; those named by their digest
are never fetched again, the index and withdrawals are fetched once per
run. Every check applies as for a local channel: nothing downloaded is
trusted before the signature and the digests say so. Plain HTTP is spoken
by pkg itself (redirects and chunked replies included), which is enough
since integrity comes from the signatures and what 68k machines need;
HTTPS goes through the system's `curl` on macOS, Linux and Windows. An
unreachable host is refused with 17, a URL with no channel with 11.
PUBLISH and WITHDRAW refuse a URL: they write a channel on this machine,
which PUSH sends to a portal. AROS reads network channels in a later step;
there, a channel is a directory for now. `tests/network.sh` runs all this
against a local server.

### PUSH: a local channel to a portal

`pkg PUSH CHANNEL <local dir> TO https://<portal>/<channel>` sends a channel
published on this machine to a portal that serves channels (the API agreed
with the portal: plan, files, commit). It asks the portal which files it
lacks, sends only those, a file over 32 MiB in parts that resume where the
portal says it got to, then commits the local index; the portal merges it,
checks the result with pkg itself and answers in this program's records
(`published:`, `refused:`, `summary:`), which PUSH relays. Signing never
leaves this machine. The key is the publisher's, in `PKG_PUSHKEY`: it goes
in a header file readable by its owner alone, never on a command line, and
only over https (plain http is accepted to this machine alone, for tests).
A second push of the same channel sends nothing. `tests/push.sh` runs it
against `tests/push_server.py`, a stand-in for the portal.

### Packages whose files stay in someone else's archive

`PUBLISH "<archive>!/<path>" FILES "a,b"` publishes the files under a path
inside a `.tar` or `.tar.bz2` archive, only those under the paths `FILES`
names: a nightly contrib archive becomes one package per component without
being unpacked or copied. The signed manifest lists every file with its
digest and names the archive with `Source: <archive name>!/<path>` instead of
a `Payload:`; no container is written. The channel keeps the archive as
`archives/<name>`. `INSTALL` reads the archive once, takes the listed files
out and checks each against the manifest, so an archive changed since
publishing is refused (12) and one the channel lacks is said so (11). Owner
Execute comes from the archive's mode bits. The reader is `src/pkg_archive.c`
over libbzip2 1.0.8, vendored unmodified in `third_party/bzip2`;
`tests/archive.sh` checks it against archives the system's tar and bzip2
write and, given `PKG_NIGHTLY_CONTRIB`, against a whole nightly.

`SHOW` checks each archive once, for every entry whose files it holds (the
2026-09-18 contrib channel: 104 entries in one 80-second read, where one read
per entry took over five minutes). `SHOW CHANNEL <dir> METADATA` checks
manifests, signatures, withdrawals and payloads without reading archives,
and marks those entries `archive: unchecked` (one second on the same
channel); `SHOW CHANNEL <dir> ARCHIVE <name>` checks only the entries whose
files are in that archive. A portal accepts a push on the first and checks
archives with the second afterwards.

**An archive downloaded from where its makers publish it.** With
`UPSTREAM <url>` at publish (for contrib, the nightly's SourceForge
download), the manifest also records `Archive: <sha256> <size> <url>`,
signed like the rest; `<archive>.sha256` beside the archive keeps its digest
so that a hundred packages from one nightly hash it once. The channel then
needs no copy of the archive: a portal holds only the index and the
manifests, and PUSH leaves out an archive every package of which has an
`Archive:` line. INSTALL looks for the archive in a local channel first,
then in the cache (`upstream/<sha256>/<name>`), then downloads it from the
URL, once, and keeps it only when its size and SHA-256 are the signed ones
(12 otherwise, with the archive deleted); each file is still checked against
its own digest. SHOW without a copy reports `archive: <name> <version>
upstream <url>` and counts nothing bad. On AROS itself the download waits
for network channels there. `tests/network.sh`, section `upstream`;
`tests/push.sh`, section `upstream`.

A version may carry a build after `+`, such as the date of the nightly a
component was taken from: `41.7+20260918`. It orders after the version, so
`41.7 < 41.7+20260917 < 41.7+20260918 < 41.8`, and it is not compared with
the program's `$VER`.

### Protection bits and comments

Each file carries its AROS protection word and comment, where they differ
from the default: `Protect: 0x00000041 S/Go` and `Comment: Starts%20the%20tool
S/Go` lines in the signed manifest, for the package's files and for the files
inside its image. On AROS they come from the file system; on a host from the
drawer's `.ameta` files (the format of the planning repository's
`docs/features/file-metadata/ameta.md`, whose reference cases are vendored in
`tests/ameta-corpus` and run by `tests/test_ameta.c`), with owner Execute
from the host mode. A malformed or stale `.ameta` line refuses the publish,
naming it, as does a comment AROS cannot store (over 79 characters, or
outside Latin-1). INSTALL applies them with SetProtection and SetComment on
AROS, and writes the host mode and `.ameta` in a host root, under a directory
lock; REMOVE takes the entries out. An image carries them in its FFS file
headers. `tests/aros-smoke.sh` checks on hosted AROS that `List` shows the
word and the comment the drawer gave.

FFS rather than AFS+, which settles one of the open questions in the planning
repository's packaging README. An application image is read-only, written
once, and has to outlive handler revisions; AFS+ changes its on-disk format
without keeping legacy readers, and a revision 14 handler already refuses a
revision 15 image. Every AmigaOS, AROS and MorphOS FFS reads a `DOS\3`
volume, and UAE mounts it as a hardfile. Block compression, the other open
question, is not done: the image is stored whole.

The writer is judged by readers written apart from it: amitools (installed
with `pip install amitools`; `tests/image.sh` and `tools/ffs-validate.py` say
so when it is missing) validates and unpacks every image in `tests/image.sh`, and the AROS FFS handler mounts them
in `tests/goal2.sh`.

### Configuration files

`CONFIG "S/Startup-Sequence,Prefs/Env-Archive"` at publish names the files
people edit, one by one or by folder; each becomes a `Config: <path>` line
in the signed manifest, and a new version inherits the list, as it does
KIND. A name that matches none of the package's files is refused, and so is
CONFIG on an image, which is never edited in place. When INSTALL, UPGRADE or
ROLLBACK finds a configuration file edited, it leaves the edit where it is:
the new version's copy goes beside it as `<path>.pkgnew` (`config-new:`),
or nowhere when the new version ships the file unchanged (`config-kept:`),
and the rest of the package is placed. A first install over a system that
already has the file keeps it the same way. Any other edited file still
stops the upgrade with 15, and the refusal names CONFIG. `tests/e2e.sh`,
section `config_files`.

### Checking and repairing a whole system

`VERIFY ALL` checks every installed package, one line each, and names each
file that differs with its package: `changed:`, `missing:`, and `edited:`
for a configuration file, which is not damage. It refuses with 12 when a
package is damaged. `REPAIR <name>` or `REPAIR ALL` takes the installed
version from the channel, checks its signature, and puts back each missing
or changed file, after checking its bytes against the digest the root
recorded at install. A changed file is kept beside as `<file>.pkgold`, never
overwritten; when a `.pkgold` is already there, the file is left as it is,
with a warning, and REPAIR refuses with 12: the package is not repaired.
Edited configuration files stay as they are. Like UPGRADE
ALL, REPAIR ALL goes as far as it can and names what it could not repair.
`tests/e2e.sh`, section `repair`.

### One change of a root at a time

INSTALL, UPGRADE, ROLLBACK, REPAIR and REMOVE take the root's lock before
they change it, without waiting: while another pkg holds it, the command is
refused at once (15, `next: retry-later`) and changes nothing. Two runs
would otherwise share `.pkg/staging/<name>` and interleave their records. A
dry run writes nothing and takes no lock. On AROS the lock is a public
semaphore named after the root's full path, so a reset leaves none behind;
elsewhere it is `.pkg/lock`, released when the process ends and removed
while still held, so a root keeps nothing of it. `tests/lock.sh`.

Before it lets go of the lock, pkg writes what the change wrote back to the
medium and says how, as `flushed:`. On AROS it sends ACTION_FLUSH to the
root's handler (`flush`); a handler that does not know it has its DOS
device inhibited and released (`inhibit`), which writes the handler's cache
and the device back, as FAT's needs. Elsewhere the host keeps its own files
(`host`). `flushed: no`, with a warning, means a power cut could still lose
the change: a caller that needs it to survive one requires another answer.
Neither is atomic across a power cut; they only shorten the time a
finished change lives in a cache.

### An interrupted change

A change cut at any point, by a crash, a reset or a killed process, is
finished by giving the same command again. Nothing else is needed, and
nothing is set aside:

- A file that already holds the new version's own bytes counts as placed
  (`resumed-files:`), so the repeated command goes on from where the cut
  one stopped instead of refusing the files as edited.
- The database entry is written last: until it is, the root holds the old
  version, and the command is not done. The previous-version record and
  the key pin are written before it, so a cut can leave them ahead of the
  database, never behind it.
- The previous-version record also names the change under way,
  `to <version> <verb>`. ROLLBACK refuses while that change is unfinished,
  since there is nothing to go back from yet, and finishes an interrupted
  ROLLBACK. ROLLBACK itself goes back and forth, so after a cut it is given
  again only when the version it was asked for is not the one installed.
- On AROS, `rename()` over a file is `DeleteFile` then `Rename`, which a cut
  in between leaves with no file at all. So pkg first renames the old
  record aside as `.<name>.pkgbak`, then puts the new one in place, and
  every reader of a record that is missing reads its backup.

`tests/interrupt.sh` ends pkg before each rename and unlink of INSTALL,
UPGRADE, a second UPGRADE, ROLLBACK and REPAIR, once and twice at the same
point, in a build cut as a process ends and one built and cut as AROS
renames, and checks the version, the pin, the previous version and that
ROLLBACK right after the cut goes where the database says or nowhere. A
process that ends is not power loss: what the handler still held in its
cache is not in that test.

### Executables of several CPUs

One package is one architecture, so a drawer holding executables for two
CPUs is refused. The refusal counts the files of each CPU and names one.
Two kinds are exempt when ARCH names the machine: `boot`, whose GRUB
stages for i386-pc boot an x86_64 system, and `sdk`, which carries the
libraries of every target it compiles for. Hunk files that AROS loads as
data on any CPU are not counted as 68k programs: data-only hunk files such
as deficons.prefs, classic fonts (their code starts with the
`moveq #n,d0; rts` stub) and keymaps in a `Keymaps` drawer.

### An AROS installed without pkg

InstallAROS copies the system with no package database. When INSTALL finds
one of the package's files already there, byte for byte the package's, it
takes it over (`adopted: <count>`); from then on that version upgrades and
rolls back like any other. A file already there with other content is
refused with 15, as before, and left alone; so is an identical file that
another installed package lists, since one file has one owner and removing
either package would take it away from the other. `tests/e2e.sh`, section
`adopt`.

### Dependencies

`Depends: <name>` or `Depends: <name> >= <version>` in the manifest, set with
`DEPENDS "a >= 1.0, b"` at publish. An install, upgrade or rollback plans the
whole graph first, fetching and verifying every package, and places nothing
until all of it is settled; then dependencies go in before what needs them.
A failure while placing takes the new dependencies back out. Refused with 16,
nothing applied: a dependency the channel lacks, a version it cannot meet, an
installed version too old (the refusal names `UPGRADE`), and a cycle, named
with its path. A package installed as a dependency is marked in `.pkg/auto`;
`REMOVE` refuses while something needs a package and names what, reports what
it leaves orphaned, and `REMOVE ORPHANS` takes those out, repeating until none
is left. `tests/deps.sh`, 44 checks.

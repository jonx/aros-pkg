<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# INSTALL

Install a package and what it depends on.
```
pkg INSTALL <name>... ROOT <root> [AT <dir>] [CHANNEL <channel>] [VERSION v] [ARCH cpu] [KEY <public key>] [ACCEPTKEY <key>] [UNPACKED <dir>] [DOWNGRADE] [DRYRUN]
```

## What it does

`INSTALL` takes the newest version of `<name>` the channel offers for the
root's CPU (or `VERSION v`), checks its signature and every file's digest,
installs the packages it depends on first, then places its files under the
root and records the package in the root's database (`.pkg/`).

- A file already in place and byte for byte the package's is **adopted**:
  nothing is written, and the package is recorded as installed. This is how
  a system copied by hand comes under pkg afterwards.
- A file already in place with other content, belonging to no package, is a
  conflict (exit 15): pkg overwrites nothing it did not install.
- A package installed only as a dependency is marked so; `REMOVE ORPHANS`
  takes it out when nothing needs it any more. Installing it explicitly
  later keeps it for itself.
- The first version installed pins the publisher's key for that package;
  a later version signed by another key is refused (exit 14) until
  `ACCEPTKEY <public key>` names the new one. That decision is the person's.
- `KEY <public key>` says who publishes the package, instead of trusting
  whoever signed its first version: the version must be signed by it
  (exit 14), a key already pinned must be it (exit 14; `ACCEPTKEY` with it
  confirms a change), and it is pinned before the package is placed. It is
  the key of that package alone, so a dependency this root has no key for
  is refused (exit 14, `next: install-dependency-first`) and installed
  first with its own `KEY`.
- Installing the version already installed succeeds and says so
  (`result: unchanged`); a newer one asks for `UPGRADE` (exit 15).
- A withdrawn version is not installed unless `VERSION` names it (exit 18).

Without `CHANNEL` it reads the channels the root lists ([CHANNEL](channel.md)), in order; `CHANNEL <channel>` means that channel alone.

## Application placement

`AT <absolute-directory>` installs a supported application drawer in that
existing directory. Dependencies use the selected root. The root records
the destination for upgrades, verification, repair, rollback and removal.
See [Application placement](../placement.md) for layout rules and examples.

## Several names at once

`INSTALL a b c` installs each in turn. It goes as far as it can: a name it
cannot install is reported with its reason, the rest are installed anyway,
and the exit code is the worst class any of them refused with. `VERSION`,
`KEY`, `ACCEPTKEY` and `DOWNGRADE` are decisions about one package and are refused
here (exit 20); give them to `INSTALL <name>` alone.

Packages whose files live in one large archive (the contrib channel) gain
most: the archive is read once, and the block map that first read leaves
behind means every later name takes only the blocks its own files lie in.

## Where the files come from

A package published from someone else's archive says, once a command and
with the path in full, where its files were read: the archive downloaded
into the cache, the copy the channel carries, the block map, or a directory
you unpacked yourself. The cache directory is named with it, so you know
what to delete and what `PKG_CACHE` moves.

`UNPACKED <dir>` reads the files from a directory holding what the archive
holds, so each file is at `<dir>/<prefix>/<path>` where `<prefix>` is the
part after `!/` in the manifest's `Source`. Every file is still weighed and
hashed against the signed manifest before anything is written; one that
differs is refused (exit 12) by name. Unpack it into the cache and you need
not name it again:

```
mkdir -p ~/.cache/pkg/upstream/<sha256>/<archive>.d
tar xjf <archive> -C ~/.cache/pkg/upstream/<sha256>/<archive>.d
```

`<sha256>` is the archive's digest, which `pkg SHOW <name> CHANNEL <channel>
MACHINE` prints as `archive:`.

## Keywords

| Keyword | Meaning |
|---|---|
| `ROOT <dir>` | the system installed into: `SYS:` on AROS, a directory elsewhere |
| `AT <dir>` | existing absolute parent directory for one application drawer |
| `CHANNEL <dir\|url>` | where the package is published |
| `VERSION v` | this version instead of the newest |
| `ARCH cpu` | the root's CPU, when the root has never been told and pkg cannot know it |
| `KEY <public key>` | the publisher's key, the only one trusted for this package |
| `ACCEPTKEY <key>` | accept a publisher key other than the one pinned |
| `UNPACKED <dir>` | read the files from a directory the archive was unpacked into |
| `DRYRUN` | every check, no write; the result reads `would install` |

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
helloworld 1.1 is already installed in aros
$ pkg INSTALL notes ROOT aros CHANNEL channel VERSION 1.0 DRYRUN
would install notes 1.0 into aros: 1 file, payload cfac33cc9d05, signed by 5ff18d3fe14e383e
  image    notes.hdf, 32 blocks
  hint: to run it, mount the image: MOUNTLIST notes ROOT aros OUT <file> writes the mount entry and lists the steps
$ pkg INSTALL nosuch ROOT aros CHANNEL channel    # exits 11
pkg install: nosuch is not in the channel channel
  next: check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, pkg LIST ROOT <dir> what a root holds
$ pkg INSTALL helloworld ROOT aros CHANNEL channel MACHINE
result: unchanged
name: helloworld
version: 1.1
```

## Records (`MACHINE`)

`result:` (`installed`, `unchanged`, `would-install`, `refused`), `name:`,
`version:`, `root:`, `files:`, `payload:` or `source:`, `signer:`;
`dependency:` for each package brought along; `adopted:`,
`unchanged-files:`, `config-kept:`, `config-new:` when files were already
there; `image:` and `blocks:` for an image; `first-signer:` and `pinned:`
around a key refusal; `archive-from:` and `cache:` when the files came out
of an archive. With several names, `package:` or `refused:` for each, then
`installed:`, `not-installed:`, `count:` and `summary:`.

## Refusals

| Exit | When |
|---|---|
| 11 | no such package or version in the channel; a dependency the channel does not offer |
| 12 | a manifest, payload or archive that does not match what was signed |
| 13 | unsigned, or a signature that does not verify |
| 14 | signed by another key than the one pinned; `next: ask-requester` |
| 15 | a file in the way; the version installed is newer (`next: use-upgrade`) |
| 16 | a dependency too old, missing, or in a cycle |
| 17 | the file system refused |
| 18 | the version is withdrawn and `VERSION` did not name it |

## Related

[STATUS](status.md), [UPGRADE](upgrade.md), [REMOVE](remove.md),
[MOUNTLIST](mountlist.md) for images, [Using pkg](../using.md#install),
[Distributing builds](../distributing.md) for `ARCH`.

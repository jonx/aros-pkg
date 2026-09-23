<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# UPGRADE

Move a package, or every package, to a newer version. With no package name,
update the running pkg executable. `pkg u` is the short form.
```
pkg UPGRADE
pkg U
pkg UPGRADE <name> ROOT <root> [CHANNEL <channel>] [VERSION v] [ARCH cpu] [DOWNGRADE] [KEY <public key>] [ACCEPTKEY <key>] [UNPACKED <dir>] [DRYRUN]
pkg UPGRADE ALL    ROOT <root> [CHANNEL <channel>] [ARCH cpu] [DRYRUN]
```

## What it does

`pkg UPGRADE` and `pkg U` announce the executable being updated, fetch its
platform build and verify the publisher's signature before replacement. The
installation directory determines which users receive that update. A protected
directory requires permission to replace its executable.

For a named package, `ROOT` explicitly chooses the package database. It can be
omitted when [environment configuration](../environments.md) identifies the
root. The command announces the selected root and its source before work begins.

`UPGRADE <name>` replaces the installed version with the newest the channel
offers for the root's CPU, or with `VERSION v`. Files the person edited
(`CONFIG` files, or any file whose content changed since install) are kept:
the new one is put beside as `<file>.pkgnew`, and the result says `kept`.
The previous version is recorded so `ROLLBACK` can return to it. An image
package is replaced whole; a machine that has it mounted must eject it
first.

An older version is refused (exit 18) unless `DOWNGRADE` says so. A version
signed by another key is refused (exit 14) unless `ACCEPTKEY` names it.
With `KEY`, the new version must be signed by that key and the pin must be
it, as for [INSTALL](install.md).

`UPGRADE ALL` upgrades every package the channel has a newer version for, a
package before what depends on it. It never downgrades and never accepts a
new key. It goes as far as it can: a package it cannot upgrade is named
with its reason, what depends on it is skipped and named, and the rest are
upgraded. Exit 0 when everything offered was taken, otherwise the worst
class of its refusals, which is the rule for every batch: a command given
several things to do goes as far as it can, reports each, and exits with
the worst that happened rather than the first.

Without `CHANNEL` it reads the channels the root lists
([CHANNEL](channel.md)), and says which channel each new version comes
from. A package two of those channels offer under different keys is not
upgraded; it is reported like any other package that needs the requester
(exit 14), and the rest go ahead.

A package whose files live in someone else's archive is read the way
`INSTALL` reads it: from the archive in the cache, from the block map the
first read left beside it, or from a directory `UNPACKED <dir>` names. Where
the files came from is said once a command, with the path in full. See
[INSTALL](install.md).

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel VERSION 1.0
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
$ printf 'Greeting=Hi\n' > aros/S/HelloWorld.prefs
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
upgraded helloworld from 1.0 to 1.1 in aros: 1 placed, 0 removed, 1 kept
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel VERSION 1.0    # exits 18
pkg upgrade: helloworld 1.0 is older than the installed 1.1; nothing was changed. Going back a version is the requester's decision
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel VERSION 1.0 DOWNGRADE
  kept     S/HelloWorld.prefs (edited; helloworld 1.0's version is beside it as S/HelloWorld.prefs.pkgnew)
downgraded helloworld from 1.1 to 1.0 in aros: 1 placed, 0 removed, 1 kept
$ pkg UPGRADE ALL ROOT aros CHANNEL channel DRYRUN
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
  helloworld would upgrade from 1.0 to 1.1: 1 placed, 0 removed, 1 kept
would update 1 package
$ pkg UPGRADE ALL ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
  helloworld upgraded from 1.0 to 1.1: 1 placed, 0 removed, 1 kept
updated 1 package
```

## Records (`MACHINE`)

One package: `result:` (`upgraded`, `downgraded`, `unchanged`, `would-upgrade`),
`name:`, `version:`, `from:`, `placed:`, `removed:`, `kept:`,
`config-kept:`, `config-new:`, `image:`. `ALL`: one `package: name from to`
per package upgraded, `not-upgraded:` and `skipped:` with reasons,
`count:`, `upgraded:`, `summary:`.

## Refusals

| Exit | When |
|---|---|
| 11 | the package is not installed (`next: use-install`); no newer version |
| 14 | a new publisher key; `next: ask-requester` |
| 15 | a file the upgrade would replace was edited and is not a `CONFIG` file |
| 16 | a dependency the new version needs cannot be met |
| 18 | an older version without `DOWNGRADE`; the installed version was withdrawn and nothing newer is offered |

## Related

[ROLLBACK](rollback.md), [STATUS](status.md), [Using pkg](../using.md#keep-it-current).

<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# REPAIR

Put missing and changed files back from the channel.
```
pkg REPAIR <name> ROOT <root> [CHANNEL <channel>] [ARCH cpu] [KEY <public key>] [UNPACKED <dir>] [DRYRUN]
pkg REPAIR ALL    ROOT <root> [CHANNEL <channel>] [ARCH cpu] [UNPACKED <dir>] [DRYRUN]
```

## What it does

For each file `VERIFY` would report missing or changed, fetches the
installed version's payload from the channel, checks it, and writes the
file back. A changed file is not thrown away: it is kept beside as
`<file>.pkgold` (`aside`), then the original is `restored`. Edited
configuration files are left alone. A package whose installed version the
channel no longer offers cannot be repaired from it (exit 11); the others
are. Files come back only from the key the root pins for the package, and
from `KEY` when it is given: a channel whose copy another key signed puts
nothing back (exit 14).

Without `CHANNEL` it reads the channels the root lists ([CHANNEL](channel.md)), in order; `CHANNEL <channel>` means that channel alone.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ rm aros/C/HelloWorld
$ printf 'broken' > aros/Libs/hello.library
$ pkg REPAIR ALL ROOT aros CHANNEL channel DRYRUN
  hellolib 1 file put back
  helloworld 1 file put back
2 files would be put back in 2 packages
$ pkg REPAIR ALL ROOT aros CHANNEL channel
  aside    Libs/hello.library -> Libs/hello.library.pkgold
  restored Libs/hello.library
  hellolib 1 file put back
  restored C/HelloWorld
  helloworld 1 file put back
2 files put back in 2 packages; the changed ones kept beside as <file>.pkgold
$ pkg VERIFY ALL ROOT aros
Package     Version  Files    State
hellolib    1.0      1 file   intact
helloworld  1.1      2 files  intact
2 packages, 3 files, all intact
```

## Records (`MACHINE`)

`result:` (`repaired`, `unchanged`, `would-repair`, `refused`), `restored:`
and `set-aside:` per file, `package: name repaired|intact`, `refused: name`,
`summary:`.

## Related

[VERIFY](verify.md), [Using pkg](../using.md#check-and-repair).

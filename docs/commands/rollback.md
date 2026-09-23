<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# ROLLBACK

Return a package to the version installed before.
```
pkg ROLLBACK <name> ROOT <root> [CHANNEL <channel>] [VERSION v] [ARCH cpu] [ACCEPTKEY <key>] [DRYRUN]
```

## What it does

Every `UPGRADE` records the version it replaced. `ROLLBACK` installs that
version again from the channel, with the same care for edited files as an
upgrade, and records the version it replaced in turn, so a second
`ROLLBACK` goes forward again: one step, in either direction, not a
history. The previous version must still be in the channel.

After an interrupted change, ROLLBACK refuses until that change is
finished, and finishes an interrupted ROLLBACK itself
([history](../history.md#an-interrupted-change)).

Without `CHANNEL` it reads the channels the root lists ([CHANNEL](channel.md)), in order; `CHANNEL <channel>` means that channel alone.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel VERSION 1.0
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
$ pkg ROLLBACK helloworld ROOT aros CHANNEL channel    # exits 11
pkg rollback: helloworld (null) has no previous version recorded in aros; nothing to roll back to
  next: check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, pkg LIST ROOT <dir> what a root holds
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
upgraded helloworld from 1.0 to 1.1 in aros: 2 placed, 0 removed
$ pkg ROLLBACK helloworld ROOT aros CHANNEL channel
rolled back helloworld from 1.1 to 1.0 in aros: 2 placed, 0 removed
```

## Records (`MACHINE`)

`result: rolled-back` (or `would-roll-back`), `name:`, `version:`, `from:`,
`placed:`, `removed:`, `kept:`.

## Refusals

| Exit | When |
|---|---|
| 11 | not installed; no previous version recorded; the previous version is no longer in the channel |
| 12 | the rollback record is damaged |

## Related

[UPGRADE](upgrade.md), [Using pkg](../using.md#go-back).

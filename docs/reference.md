# Reference

Every command, keyword and environment variable of pkg, the output a
program reads, and the ARexx port. For how to use them, see
[Using pkg](using.md) and [Publishing packages](publishing.md).

## Command form

```
pkg <VERB> [<name>] [KEYWORD value ...] [SWITCH ...]
```

Verbs, keywords and switches are case-insensitive and can come in any
order after the verb. A value with spaces goes in double quotes. `pkg HELP`
prints a summary.

## Verbs

Each verb links to its own page: what it does, its keywords, examples
that run, what it records, its refusals ([all of them](commands/README.md)).

### Installing and keeping software

| Verb | Form | Does |
|---|---|---|
| [`INSTALL`](commands/install.md) | `INSTALL <name>... ROOT <root> [AT <dir>] [CHANNEL <channel>] [VERSION v] [ARCH cpu] [KEY <public key>] [ACCEPTKEY <key>] [UNPACKED <dir>] [DOWNGRADE] [DRYRUN]` | Installs a package and what it depends on. Takes over files already present that are identical to the package's. Several names are installed in turn, as far as the command can get. |
| [`UPGRADE`](commands/upgrade.md) | `UPGRADE <name> ROOT <root> [CHANNEL <channel>] [VERSION v] [ARCH cpu] [DOWNGRADE] [KEY <public key>] [ACCEPTKEY <key>] [UNPACKED <dir>] [DRYRUN]` | Moves an installed package to the newest version, or to `VERSION`; an older one only with `DOWNGRADE`. |
| [`UPGRADE ALL`](commands/upgrade.md) | `UPGRADE ALL ROOT <root> [CHANNEL <channel>] [ARCH cpu]` | Upgrades every package that has a newer version, as far as it can; never downgrades, never accepts a new key. |
| [`ROLLBACK`](commands/rollback.md) | `ROLLBACK <name> ROOT <root> [CHANNEL <channel>] [VERSION v] [ARCH cpu] [KEY <public key>] [ACCEPTKEY <key>] [DRYRUN]` | Returns a package to the version installed before its last change. |
| [`STATUS`](commands/status.md) | `STATUS [<name>] ROOT <root> [CHANNEL <channel>] [ARCH cpu]` | Compares what is installed with the channel: `current`, `upgradable`, `withdrawn`, `not-offered` or `edited`. Exits 0 whether or not updates exist. |
| [`LIST`](commands/list.md) | `LIST ROOT <root>` | Lists what is installed. |
| [`VERIFY`](commands/verify.md) | `VERIFY <name>\|ALL ROOT <root>` | Checks each installed file against its package; names what is missing, changed, moved or edited. |
| [`REPAIR`](commands/repair.md) | `REPAIR <name>\|ALL ROOT <root> [CHANNEL <channel>] [ARCH cpu] [KEY <public key>] [UNPACKED <dir>] [DRYRUN]` | Puts missing and changed files back from the channel, keeping a changed one as `<file>.pkgold`. |
| [`REMOVE`](commands/remove.md) | `REMOVE <name> ROOT <root> [DRYRUN]` | Removes a package, keeping any file that was changed; refuses while another package needs it. |
| [`REMOVE ORPHANS`](commands/remove.md) | `REMOVE ORPHANS ROOT <root> [DRYRUN]` | Removes the packages installed only as dependencies that nothing needs any more. |
| [`MOUNTLIST`](commands/mountlist.md) | `MOUNTLIST <name> ROOT <root> [OUT <file>] [UNIT n] [HANDLER <path>] [DRYRUN]` | Writes the AmigaDOS mount entry for an installed image, and lists the steps to mount it. |
| [`SEARCH`](commands/search.md) | `SEARCH <word>... [CHANNEL <channel>] [ROOT <root>] [ARCH cpu]` | The packages whose name, `Short`, `Tags`, `Category`, `Description` or `Provides` hold every word. Exits 0 whether or not any match. |
| [`CHANNEL`](commands/channel.md) | `CHANNEL ADD\|LIST\|REMOVE [<channel>] [NAME <name>] ROOT <root> [DRYRUN]` | The channels this root reads when `CHANNEL` is left out, in the order they were added. `ADD` checks the channel can be read and refuses a duplicate, and the channel takes a short name, its own or `NAME`'s, which `CHANNEL <name>` then stands for. |
| [`SHOW`](commands/show.md) | `SHOW [<name>] [CHANNEL <channel>] [ROOT <root>] [METADATA] [ARCHIVE <archive>]` | Lists and checks what a channel offers; with `ROOT`, marks what is installed. |
| [`RESOLVE`](commands/resolve.md) | `RESOLVE <library>\|<program> [VERSION v] [ROOT <root>] [FROM <dir>] [CHANNEL <channel>] [ARCH cpu]` | Shows which copy of a library AROS would give a program, walking the places the loader looks, with a verdict and a next step for each; a program instead of a library checks every library it names. Exits 0, 11 when found nowhere, 18 when the copy taken is too old or unusable. See [Libraries](libraries.md). |

### Publishing

| Verb | Form | Does |
|---|---|---|
| [`KEYGEN`](commands/keygen.md) | `KEYGEN FILE <keyfile>` | Makes a signing key, readable by its owner alone. |
| [`KEYINFO`](commands/keyinfo.md) | `KEYINFO FILE <keyfile> [SSH]` | Prints the public key a key file holds; `SSH` prints it as an `ssh-ed25519` line. |
| [`MANIFEST`](commands/manifest.md) | `MANIFEST <drawer> [NAME n] [VERSION v] [ARCH cpu] [KIND k] [DEPENDS "..."] [CONFIG "..."] [FILES "..."] [BUILD <n>] [UPSTREAM <url>] [INFO <file>] [SHORT "..."] [DESCRIPTION <file>] [CATEGORY c] [TAGS "..."] [AUTHOR "..."] [LICENSE l] [DISTRIBUTION d] [HOMEPAGE <url>] [REPOSITORY <url>] [ICON <file>] [SCREENSHOT <file>] [README <file>] [CHANGES <file>]` | Prints the description a publish would sign; writes nothing. |
| [`PUBLISH`](commands/publish.md), [`PACKAGE`](commands/publish.md) | `PUBLISH <drawer> CHANNEL <channel> [KIND k] [NAME n] [VERSION v] [ARCH cpu] [DEPENDS "..."] [CONFIG "..."] [FILES "..."] [BUILD <n>] [UPSTREAM <url>] [INFO <file>] [SHORT "..."] [DESCRIPTION <file>] [CATEGORY c] [TAGS "..."] [AUTHOR "..."] [LICENSE l] [DISTRIBUTION d] [HOMEPAGE <url>] [REPOSITORY <url>] [ICON <file>] [SCREENSHOT <file>] [README <file>] [CHANGES <file>] [SIGN <keyfile>] [ACCEPTKEY <key>] [DRYRUN]` | Signs and publishes a drawer, or the paths `FILES` names in it, into a channel; the drawer can be `"<archive>!/<path>"`. `PACKAGE` is the same verb under another name: nothing reaches a portal until `PUSH`. |
| [`WITHDRAW`](commands/withdraw.md) | `WITHDRAW <name> VERSION v CHANNEL <channel> [ARCH cpu] [SIGN <keyfile>] [DRYRUN]` | Marks a published version as withdrawn. |
| [`PUSH`](commands/push.md) | `PUSH CHANNEL <channel> TO <url> [SIGN <keyfile>]` | Uploads a local channel to a portal: over `https` with the key in `PKG_PUSHKEY`, or, over `http` too, with requests signed by the publisher's key. |
| [`IMAGE`](commands/image.md) | `IMAGE <drawer> OUT <file> [NAME <volume>]` | Writes a drawer as an FFS disk image. |
| [`SIGN`](commands/sign.md) | `SIGN <file> KEY <keyfile> OUT <sigfile> [SSH NAMESPACE <ns>]` | Signs any file with a key; `SSH` writes a signature `ssh-keygen -Y verify` checks. |
| [`CHECKSIG`](commands/checksig.md) | `CHECKSIG <file> FILE <sigfile> [KEY <public key>]` | Checks a signature `SIGN` made, and says whose it is. |

### Other

| Verb | Form | Does |
|---|---|---|
| [`PORT`](commands/port.md) | `PORT [<portname>]` | On AROS, serves the verbs on an ARexx port, `PKG` by default. |
| [`ENV`](commands/env.md) | `ENV ADD\|LIST\|REMOVE\|DEFAULT [<name>] [ROOT <root>] [SYSTEM]` | Registers and selects installation environments. `DEFAULT` chooses the default environment. |
| `HELP` | `HELP [<verb>] [MACHINE]` | The usage, drawn from the templates the verbs read their words with; `HELP <verb>`, or `<verb> ?`, one verb and its template. `HELP MACHINE` is pkg's own account of everything it takes, for a program: `version:`, then per verb `verb:`, `what:`, `syntax:`, `template:`, one `place: <required\|optional> <one\|many> <shown>` per word taken by place, one `keyword: <NAME> <required\|optional> <shown>` and one `switch: <NAME>` per word it takes; `verb:` records for HELP, VERSION and PORT; one `global:` per word every verb takes. |
| `VERSION` | `VERSION [MACHINE]` | Which build this is: the release, the patch, the day it was built. `--version` and `-v` say the same. Records: `result: version`, `version:`, `built:`. |

Environment selection is described in [Environments](environments.md). An explicit
`ROOT` takes precedence; `ENVIRONMENT <name>` selects a registered environment.
`UPGRADE` without a package name, or `U`, updates the running pkg executable.

## Keywords

| Keyword | Value | Used by |
|---|---|---|
| `ENVIRONMENT` | a registered environment name | commands using an installation root |
| `SYSTEM` | switch selecting machine configuration | `ENV` |
| `ROOT` | the system installed into: `SYS:`, or a directory | installing verbs, `LIST`, `VERIFY`, `REMOVE`, `SHOW` |
| `AT` | existing absolute directory outside the selected root for one application drawer; [placement is remembered](placement.md) | `INSTALL` |
| `CHANNEL` | a directory, an `http://` or `https://` address (on AROS `http://` only, with the network started), or the short name the root's list knows one by. The installing verbs, `SHOW` and `SEARCH` read the root's list ([`CHANNEL ADD`](commands/channel.md)) when it is left out; given, it means that channel alone | installing and publishing verbs, `SEARCH` |
| `VERSION` | a version: dotted numbers, and an optional `+build` | `INSTALL`, `UPGRADE`, `PUBLISH`, `MANIFEST`, `WITHDRAW` |
| `ARCH` | a CPU: `x86_64`, `i386`, `aarch64`, `arm`, `ppc`, `m68k`, or `generic` | installing verbs, `PUBLISH`, `MANIFEST`, `WITHDRAW` |
| `NAME` | a package name; for `IMAGE`, the volume name; for `CHANNEL ADD`, the short name the root will know the channel by | `PUBLISH`, `MANIFEST`, `IMAGE`, `CHANNEL ADD` |
| `KIND` | `image`, `application`, `library`, `device`, `class`, `font`, `catalog`, `startup`, `boot`, `data`, `sdk` or `slave` | `PUBLISH`, `MANIFEST` |
| `DEPENDS` | `"name >= version, name"`, or `none` | `PUBLISH`, `MANIFEST` |
| `CONFIG` | `"path, drawer"`: the files people edit | `PUBLISH`, `MANIFEST` |
| `FILES` | `"path, path"`: the paths of the drawer or archive that make up the package | `PUBLISH`, `MANIFEST` |
| `BUILD` | dotted numbers, such as the date of a nightly, added as `+build` | `PUBLISH` |
| `UPSTREAM` | the `http` or `https` address an archive is published at | `PUBLISH` from an archive |
| `SHORT` | one line, at most 40 characters | `PUBLISH`, `MANIFEST` |
| `DESCRIPTION` | a text file: the package's description | `PUBLISH`, `MANIFEST` |
| `CATEGORY` | an Aminet type and sub-directory: `util/misc` | `PUBLISH`, `MANIFEST` |
| `TAGS` | `"word, word"`, lowercase, 16 at most | `PUBLISH`, `MANIFEST` |
| `AUTHOR` | `"name, name"`: who wrote the program | `PUBLISH`, `MANIFEST` |
| `HOMEPAGE` | an `http` or `https` address | `PUBLISH`, `MANIFEST` |
| `REPOSITORY` | an `http` or `https` address: the source | `PUBLISH`, `MANIFEST` |
| `LICENSE` | an SPDX expression: `MIT`, `GPL-2.0-or-later` | `PUBLISH`, `MANIFEST` |
| `DISTRIBUTION` | `open-source`, `freeware`, `shareware`, `public-domain`, `commercial`, `demo` or `other` | `PUBLISH`, `MANIFEST` |
| `CHANGES` | a text file: what this version changes | `PUBLISH`, `MANIFEST` |
| `ICON` | a path of the package: its icon | `PUBLISH`, `MANIFEST` |
| `SCREENSHOT` | `"path, path"` of the package | `PUBLISH`, `MANIFEST` |
| `README` | an Aminet `.readme`: fills `Short`, `Author`, `Type` and the text | `PUBLISH`, `MANIFEST` |
| `INFO` | a `.pkginfo` a port carries: the name, version, kind, catalogue fields, `Depends`, `Files` and `Config`; keywords win over it | `PUBLISH`, `MANIFEST` |
| `FROM` | the program's directory, for its current directory and `PROGDIR:` | `RESOLVE` |
| `ARCHIVE` | the name of an archive in the channel | `SHOW` |
| `UNPACKED` | a directory holding what a `Source` archive holds, so that `<dir>/<prefix>/<path>` is each file; every file is still weighed and hashed against the signed manifest | `INSTALL`, `UPGRADE`, `REPAIR` |
| [`SIGN`](commands/sign.md) | a key file; the default is `PKG_SIGNKEY` | `PUBLISH`, `WITHDRAW`, `PUSH` |
| `ACCEPTKEY` | a public key in full, 64 hexadecimal digits | `INSTALL`, `UPGRADE`, `PUBLISH` |
| `FILE` | a key file | `KEYGEN`, `KEYINFO` |
| `KEY` | a key file; for `CHECKSIG`, the public key expected; for `INSTALL`, `UPGRADE`, `ROLLBACK` and `REPAIR`, the publisher's public key in full, the only one trusted for that package and the reason a dependency with no pinned key is refused | `SIGN`, `CHECKSIG`, `INSTALL`, `UPGRADE`, `ROLLBACK`, `REPAIR` |
| `SSH` | OpenSSH's formats instead of pkg's | `SIGN`, `KEYINFO` |
| `NAMESPACE` | what an SSH signature is for, the word `ssh-keygen -Y verify -n` names | `SIGN` with `SSH` |
| `OUT` | a file to write | `SIGN`, `IMAGE`, `MOUNTLIST` |
| `TO` | the portal channel's address | `PUSH` |
| `UNIT` | the unit number of the image device | `MOUNTLIST` |
| `HANDLER` | the file system handler the image is mounted with | `MOUNTLIST` |
| `TRACE` | a file, or `-` for the error output | any verb |
| `LOG` | a file | any verb |

PUBLISH also writes one line no keyword gives: `Provides: <name>.library`
(or `.device`) for each library or device the package ships in `Libs/` or
`Devs/`. A program's package that depends on it then counts as providing
that library, and `RESOLVE ... CHANNEL` names the package that provides a
missing one.

## Switches

| Switch | Does |
|---|---|
| `ALL` | `UPGRADE ALL`, `VERIFY ALL`, `REPAIR ALL`: every installed package |
| `ADD`, `LIST`, `REMOVE` | `CHANNEL`: add a channel to the root's list, show the list, take one off it |
| `ORPHANS` | `REMOVE ORPHANS` |
| `DOWNGRADE` | allows `UPGRADE` to an older version |
| `DRYRUN` | runs every check of a verb that changes something, and changes nothing |
| `METADATA` | `SHOW`: checks everything but archives |
| `MACHINE` | answers in `key: value` lines (below) |

`TRACE <file>` writes every step pkg takes, every file it reads, writes or
removes, and every check and choice, to find out why something happened.
`LOG <file>` appends everything printed to a file as well, without the
activity line; AROS has no `tee`.

## Environment

| Variable | Meaning |
|---|---|
| `PKG_SIGNKEY` | the key file `PUBLISH` and `WITHDRAW` sign with when `SIGN` is not given |
| `PKG_PUSHKEY` | the portal key `PUSH` sends; never put it on the command line |
| `PKG_OUTPUT` | `machine`: the same as `MACHINE` on every command |
| `PKG_TRACE` | the same as `TRACE <file>` on every command |
| `PKG_PROGRESS` | `1`: draw the activity line wherever the output goes; anything else switches it off. Unset, it appears at a terminal or an AROS Shell window and nowhere else |
| `PKG_PROGRESS_AFTER` | how long a step must last, in milliseconds, before the activity line appears; 500 when unset, `0` to draw it at once |
| `PKG_CHECK_WORDS` | `1`: the words of the command are read against the verb's template and nothing is run: exit 0 and `result: words-taken`, or the usage refusal a real run would give (20). For a test that holds every command a page shows to the parser itself |
| `PKG_COLOR` | `always` or `never`: colour and marks whatever the output is. Without it, a terminal gets them and a pipe, a file or the ARexx port gets plain text. `NO_COLOR` and `TERM=dumb` turn them off too |
| `COLUMNS` | the width long lines are wrapped at on a terminal, 80 when unset |
| `PKG_LIBS_PATH` | `RESOLVE` on a host: the directories of `LIBS:`, separated by `:`; else `ROOT/Libs` and `ROOT/Classes` |
| `PKG_CACHE` | where downloaded channel files are kept; else `$XDG_CACHE_HOME/pkg`, `~/.cache/pkg`, `%LOCALAPPDATA%\pkg-cache` on Windows, `T:pkg-cache` on AROS |

On AROS, set them with `SetEnv`.

## While pkg works

A step that takes longer than half a second draws one line, rewritten in
place, and erased when the step ends:

```
(O) downloading AROS-20260919-contrib.tar.bz2  212 of 637 MB  6 MB/s  1:10 left
 O  reading the archive  38%
 o  checking contrib-nightly  57 of 208
 .  waiting for aros-pkg.azurewebsites.net
```

The mark is the project's logo, pulsing: it advances when the work does, so
a mark that has stopped moving means pkg is stuck and not merely slow. A
percentage, a rate and a time left are stated only where pkg knows the
whole. A wait with nothing to report, such as opening a connection, says so
once and does not pretend to advance.

The line is never drawn in `MACHINE` output, never written to a `LOG` file
and never shown when the output is not a terminal. `PKG_PROGRESS=1` draws it
anyway, `PKG_PROGRESS_AFTER` sets the wait before it appears, and
`PKG_COLOR=never` leaves it in plain ASCII with no escape sequence.

## What the output looks like

At a terminal, a result carries a mark and its figures on the line under it,
a refusal is marked and names the command, a hint follows an arrow, and a
list is a table with a header. Piped or redirected, the same lines are plain
text, one per line, with `warning:`, `hint:` and `next:` spelled out, so a
script or a log reads them; the tables keep their columns. `LOG <file>`
always receives the plain form. On AROS the console shows the same
structure with its own means: bold, italic, inverse video and the screen's
pens.

## Exit codes

| Code | Class | Meaning |
|---|---|---|
| 0 | | done |
| 10 | refused | a refusal that fits no class below |
| 11 | not-found | no such package, version, channel or file |
| 12 | integrity | a file or description that does not match its digest; an unsafe path |
| 13 | signature | unsigned, or a signature that does not verify |
| 14 | key | signed by another key than the one accepted before |
| 15 | conflict | something in the way: a file present or edited, a package already installed or still needed |
| 16 | dependency | a dependency missing, too old, or in a cycle |
| 17 | io | the file system or the network failed |
| 18 | policy | a downgrade without `DOWNGRADE` |
| 20 | usage | an unknown verb, a missing or wrong keyword |

On AROS every refusal is 10 or more: `If ERROR` catches them all, and
`$RC` holds the code.

## Machine-readable output

With `MACHINE`, or `PKG_OUTPUT=machine`, the standard output carries only
`key: value` lines and the error output stays empty. The format is the same
on every system.

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel MACHINE
dependency: hellolib 1.0
result: installed
name: helloworld
version: 1.1
root: aros
files: 2
payload: 0542ac0ba511cb6ad4a11ec54a84ebe0432713e122ce612c2f7b26510ead9dcc
signer: 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111
$ pkg LIST ROOT aros MACHINE
result: listed
package: hellolib 1.0 library 1 dependency
package: helloworld 1.1 application 2 explicit
count: 2
$ pkg INSTALL nosuch ROOT aros CHANNEL channel MACHINE    # exits 11
result: refused
class: not-found
code: 11
reason: nosuch is not in the channel channel
next: check-name
```

Every answer has a `result:` line: `installed`, `upgraded`, `downgraded`,
`rolled-back`, `unchanged`, `listed`, `intact`, `damaged`, `moved`,
`repaired`, `removed`, `published`, `withdrawn`, `created`, `shown`,
`signed`, `empty` or `refused`; `would-...` under `DRYRUN`. A refusal adds
`class:`, `code:`, `reason:` and `next:`, what to do: `ask-requester`
(a decision for the person), `check-name`, `fix-command`, `use-install`,
`use-upgrade`, `retry-later` (another pkg is changing the root),
`install-dependency-first` (a dependency's publisher is not known yet,
under `KEY`), `stop` or `report`. Beside them:

| Key | Meaning |
|---|---|
| `summary:` | one sentence saying what happened, for a person |
| `warning:` | something to check before going on |
| `note:` | a fact worth passing on |
| `hint:` | what usually comes next; never a way around a refusal |
| `package:` | `LIST`, `STATUS`, `VERIFY ALL`, `UPGRADE ALL`, `SEARCH`: one line per package. `STATUS` and `UPGRADE ALL` add the channel the newer version comes from when several channels are read |
| `channel:` | `CHANNEL LIST`: one line per channel, in order |
| `entry:` | `SHOW`: one line per published version, with its state and signer |
| `refused:`, `skipped:` | `UPGRADE ALL`: a package not upgraded, and why; one that waits for it |
| `missing:`, `changed:`, `edited:`, `moved:` | `VERIFY`: a file and what is wrong with it |
| `restored:`, `set-aside:` | `REPAIR`: a file put back; a changed one kept as `.pkgold` |
| `adopted:`, `unchanged-files:` | files already in place, left as they are |
| `resumed-files:` | files an interrupted change had already placed, byte for byte |
| `flushed:` | after a change: `flush`, `inhibit` or `host`, how it was written back to the medium; `no` when it could not be |
| `config-kept:`, `config-new:` | an edited configuration file kept; the new one set beside it |
| `short:`, `category:`, `tag:`, `author:`, `homepage:`, `repository:`, `license:`, `distribution:`, `description:`, `changes:` | `SHOW <name>`: the catalogue fields of the newest version, one line per value |
| `candidate:` | `RESOLVE`: one place the loader looks: path, exists, version, package, chosen; with `verdict:` and `next-step:` lines naming the path |
| `winner:`, `satisfies:`, `loaded:` | `RESOLVE`: the copy taken (`memory` for a loaded one), whether it meets `VERSION`, a copy in memory on AROS |
| `program:`, `library:` | `RESOLVE <program>`: the program, then each library it names, where it is taken from and the verdict |
| `kind-from:`, `depends-from:`, `config-from:`, `about-from:` | `PUBLISH`: the fields taken from the version published before |
| `ignored:` | `SHOW`: a manifest key pkg does not know, kept in the signed text and never acted on; a misspelt known key shows up here |

## The ARexx port

On AROS, `Pkg PORT` opens a public ARexx port, `PKG` unless you name
another, and serves the same verbs until it receives `QUIT`. A command is
the words you would type after `pkg`; `RESULT` is what the command line
would print, `RC` its exit code, and a refusal leaves `RESULT` unset, with
its text in `LASTERROR`. No verb needs ARexx.

```amigados
Run >NIL: Pkg PORT
rx "address PKG; options results; 'LIST ROOT SYS: MACHINE'; say result"
```

## The library

pkg is also a C library, `libpkg`: `include/pkg.h` declares one function per
verb, taking the same options as a structure, and answers through
callbacks with the same records `MACHINE` prints. `make build/libpkg.a`
builds it; `examples/basic.c` is the shortest program on it.

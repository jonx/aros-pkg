<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Installer and pkg: the interface between them

**Status: proposal. Nothing on this page is built yet**, on either side. It
is written to be agreed with the maintainers of the AROS `Installer` before
anyone codes it.

The AmigaOS `Installer` runs the scripts that thousands of existing programs
ship with. pkg fetches, verifies, resolves, records and removes software.
The proposal keeps each one doing its own job. Installer stays the engine
that runs a script and knows what that script changes. pkg stays the manager
around it. A small interface joins them, so pkg can run any existing
Installer script as a person would, record every change the script makes,
and undo those changes later.

- [What Installer offers today](#what-installer-offers-today)
- [What pkg needs from Installer](#what-pkg-needs-from-installer)
  - [1. A driven session](#1-a-driven-session)
  - [2. A manifest that can be undone](#2-a-manifest-that-can-be-undone)
  - [3. Return codes that say how it ended](#3-return-codes-that-say-how-it-ended)
  - [4. A plan in pretend mode](#4-a-plan-in-pretend-mode)
- [What pkg does with it](#what-pkg-does-with-it)
- [What stays out of reach](#what-stays-out-of-reach)
- [Questions for the Installer side](#questions-for-the-installer-side)

## What Installer offers today

Installer 44.10, the AROS branch that implements the V44 language, gives pkg
three things to build on:

- `MANIFEST/K <file>`: one line per item the script created: a file (`F`),
  a directory (`D`), an assign (`A`), a `S:User-Startup` section (`S`) or an
  icon (`T`). File, directory and icon paths are canonical.
- `PRETEND/S`: a dry run with no question about it.
- User levels. At novice level the script's defaults are taken without
  asking.

Three things are missing. The manifest lists what was created, but not what
existed before, so an install cannot be undone. Every page still waits for
a click. The return code does not tell a finished install from an abandoned
one.

## What pkg needs from Installer

### 1. A driven session

A switch, `DRIVER/S`, opens no window. Each page the script would show
becomes an event on standard output. The caller answers on standard input.
Lines are `key: value`, the form of pkg's own `MACHINE` output, and one
blank line ends each event. The protocol needs no ARexx, because pkg runs on
systems that have none.

```text
event: askchoice
id: 3
prompt: Which version do you want to install?
choice: 0 68020
choice: 1 68040
choice: 2 AROS native
default: 2
help: The native version is faster on AROS.

answer: 2
```

| Event | From | The caller answers |
|---|---|---|
| `message` | `(message)`, `(welcome)` | `ok` |
| `askbool` | `(askbool)` | `answer: 0` or `1` |
| `askchoice` | `(askchoice)` | `answer: <n>` |
| `askoptions` | `(askoptions)` | `answer: <bitmask>` |
| `askstring`, `asknumber` | `(askstring)`, `(asknumber)` | `answer: <text>` or `<n>` |
| `askdir`, `askfile` | `(askdir)`, `(askfile)` | `answer: <path>` |
| `askdisk` | `(askdisk)` | `answer: <path>`, where the caller has the volume |
| `confirm` | any confirm-gated function | `answer: 1` to proceed, `0` to skip |
| `progress` | `(copyfiles)`, `(copylib)` | nothing; the event is information only |
| `decided` | a question the user level does not show | nothing; it says which default was taken |

Each question carries its `default:`, and the answer `default` takes it.
Two answers are always allowed: `abort` ends the run as the Abort button
does, and `back` works where the page offers Back. User levels keep their
meaning. At novice level a question the script would not show to a novice
is not asked; it is reported as `decided`, so the caller knows what the
script chose.

### 2. A manifest that can be undone

The manifest grows a first line, `manifest: 2`, and records the state
before each change. A new argument, `BACKUP/K <dir>`, names a directory the
caller owns. Before Installer replaces, deletes or renames a file, or
changes its protection bits or icon, it copies the old file there.

```text
manifest: 2
D SYS:Programs/Foo
F SYS:Programs/Foo/Foo
R LIBS:foo.library was=<sha256> backup=0001
X S:Foo-old.prefs was=<sha256> backup=0002
N SYS:Programs/Foo/Foo.guide from=SYS:Programs/Foo/Foo-1.guide
P C:FooTool was=----rwed
T SYS:Programs/Foo/Foo backup=0003
A Foo: SYS:Programs/Foo
S Foo
E run "C:FooSetup INIT" rc=0
end: done
```

| Kind | Meaning | Undone by |
|---|---|---|
| `F` | a file created where none existed | deleting it |
| `D` | a directory created | deleting it when empty |
| `R` | a file replaced; `was=` is the old content's digest, `backup=` its copy | putting the copy back |
| `X` | a file deleted | putting the copy back |
| `N` | a file renamed; `from=` is its old name | renaming it back |
| `P` | protection bits changed; `was=` gives the old bits | setting them back |
| `T` | an icon's tool types changed, or an icon written over an existing one | putting the copy back |
| `A` | an assign made, with its target | removing it |
| `S` | a `S:User-Startup` section added | removing the section |
| `E` | an external command run by `(run)`, `(execute)` or `(rexx)`, with its return code | nothing: see below |

Each line is written and flushed as the change happens. After a crash or a
power cut, the manifest is still true up to its last line. The last line,
`end:`, says how the run ended: `done`, `aborted` or `failed`. A manifest
with no `end:` line was interrupted. A digest is SHA-256 in lowercase
hexadecimal. `was=` may be left out where computing it costs too much; the
backup copy is what matters.

### 3. Return codes that say how it ended

| Code | Meaning |
|---|---|
| 0 | the script ran to its end, or ended itself with `(exit)` |
| 5 | abandoned: Abort, or the caller's `abort` |
| 10 | the script stopped with `(abort)` or a script error |
| 20 | Installer failed: script unreadable, manifest or backup not writable, protocol error |

These follow the AmigaDOS levels, `WARN`, `ERROR` and `FAIL`. A pkg script
can therefore use `If WARN` and `$RC` on them as it does on pkg's own codes.

### 4. A plan in pretend mode

With `PRETEND` and `MANIFEST`, Installer writes the lines the real run would
write, under a first line `plan: 2` instead of `manifest: 2`. pkg shows that
plan before anything changes: the files a package would replace, and the
commands it would run.

## What pkg does with it

A package of kind `installer` carries the program's files and its original
script, signed like any other package. pkg handles it in six steps:

1. It verifies the package, resolves its dependencies, and unpacks the
   payload into a staging directory.
2. Under `DRYRUN`, it runs the script with `DRIVER PRETEND MANIFEST` and
   reports the plan, including every `R`, `X` and `E` line.
3. It runs the script for real with `DRIVER MANIFEST BACKUP`. The script's
   questions are answered from the package's configuration, then by the
   person or the agent driving pkg (`next: ask-requester`). An `askdisk` is
   answered with the staging directory.
4. It keeps the manifest and the backups in its database under the root.
   It records the package as installed only on return code 0 with
   `end: done`. On any other outcome it undoes what the manifest recorded.
5. `REMOVE` undoes the manifest from its last line to its first. A file pkg
   is about to delete or restore is first checked against the state the
   manifest recorded. A file the person edited since the install is set
   aside, not overwritten.
6. `UPGRADE` runs the new version's script over the installed one and keeps,
   for each path, the backup from the first install, so `REMOVE` still
   returns the system to its state before the package.

## What stays out of reach

A program started by `(run)`, `(execute)` or `(rexx)` does whatever it
does, and no manifest can see it. pkg reports each `E` line at install time.
It marks the package as not fully reversible and says so before a
`REMOVE`.

Software installed by hand, or by Installer before this interface existed,
has no manifest. pkg can list it and leave it alone, but cannot remove it.

## Questions for the Installer side

- Should the driven session use standard input and output, or also an Exec
  message port for a graphical front end? The protocol above works over
  both.
- Can the `R`, `X`, `P` and `T` lines and their backups be written for
  `(copylib)`, which already decides by version whether to replace?
- Should `(startup)` record the full text of the section it added, so
  removal does not depend on Installer's section markers?
- Which of the V44 functions (`(openwbobject)`, `(showmedia)`, `(reboot)`)
  should a driven session refuse, answer, or report as events?

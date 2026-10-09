---
name: pkg
description: Drive pkg, the AROS package tool, on behalf of a person - publish a program or a system component into a channel, install, upgrade, roll back, verify and remove it in a root, check a root for updates and update everything (unattended too), inspect a channel, mount an application image, on macOS, Linux, Windows or AROS. Use when asked to package, ship, install, update or remove AROS software, or to explain a pkg refusal.
---

# pkg, for agents

pkg moves AROS software from where it is built to where it runs, signed at
every step. Most people will never type its commands; you will. Decisions that belong to whoever requested the work (called the requester
below: a person, or a supervising agent that holds the authority) are never
taken by the tool, and must not be taken by you. The tool is
built so that the safe path is the easy one: every refusal tells you what to
do next, every changing command can be tried first with `DRYRUN`, and nothing
it prints hands you a command that overrides a safeguard. Follow what it says.

## Getting the tool

`pkg` is one executable (`Pkg` on AROS). On a development machine, in its
repository, `make install` puts `pkg` in `~/.local/bin`, with `pkg.h`,
`libpkg.a` and this skill beside it (`include/`, `lib/`, `share/pkg/`);
`make install PREFIX=<dir>` puts them under another directory, for a person
who wants nothing system-wide. Then `<prefix>/bin` must be on `PATH`; say so
to the person rather than editing their shell profile. Without installing,
`make` builds `build/pkg`. `pkg HELP` names the version and lists every verb
and keyword.

To put pkg on an AROS machine: build it for each CPU (`sh tools/build-aros.sh`
for aarch64, `sh tools/build-aros-x86_64.sh` for x86_64; `make` alone builds
only the host's `pkg`), then `make aros-channel CHANNEL=<dir>` with
`PKG_SIGNKEY` set. On AROS the directory has another name, the one that
machine gives it (a shared volume `DEPOT:`, a drawer `Work:channel`); the
person, or a startup script, runs `Execute <ch>/Install-Pkg <ch>` with that
name, `Execute DEPOT:Install-Pkg DEPOT:` for a volume root. Never tell the
person a host path to type on AROS. Never copy a pkg binary into C: by hand:
installed through its channel, it is verified and can upgrade itself.
pkg in a channel is signed like any package, by whoever makes that
channel; a machine that installs it from there trusts that key for `pkg`.
Putting pkg and the person's programs in the same channel is the simple
case: one location for the friend, one key.
Programs that link pkg instead of running it use `include/pkg.h`, which
lists every field each operation answers; its `item` callback gives the
multi-field records with their fields apart; `examples/basic.c` and
`examples/browse.c` show both uses.

## The three habits

1. **Add `MACHINE`** to every command. stdout then holds only `key: value`
   lines, with a `result:` field in every answer; stderr stays empty.
2. **Read the exit code straight away** (on AROS, `$RC`): 0 is done, 10 to 18
   a refusal whose number is its class, 20 a wrong command. `$?` after a pipe
   is the pipe's.
3. **On a refusal, do what `next:` says. On a success, read the `hint:`** lines:
   they say what usually comes next (keep the new key, check the channel,
   mount the image), and anything worth telling the person.
   `warning:` lines are things to check before going on; `note:` lines are
   facts worth passing on (a newer version published for another CPU).

| `next:` | What you do |
|---|---|
| `stop` | Stop. Report the `reason:` to the person as it is. The bytes or signatures are not what was published; no keyword, copy or other channel makes that safe |
| `ask-requester` | Take the `reason:` to whoever requested this: the person, or the agent that gave you the task, if it holds the authority. Wait for their answer. It is their decision: a new publisher key, going back a version, a file of theirs where the package installs (move it, or choose another root), a file they edited, removing something others need |
| `fix-command` | Your command is wrong; `pkg HELP` has the spelling. Keywords have no dashes: `ROOT <dir>`, not `--root` |
| `check-name` | The name is not there. Near names come as `suggest:` fields; `pkg SHOW CHANNEL <dir>` lists a channel, `pkg LIST ROOT <dir>` a root |
| `use-upgrade` | Another version is installed; `UPGRADE` moves it, if that is what the person asked for |
| `use-install` | It is not installed; `INSTALL` it, if that is what the person asked for |
| `report` | Report the `reason:` |

A refusal reads `result: refused`, `class:`, `code:`, `reason:`, `next:`. A
key refusal also gives the keys involved (`pinned:` or `first-signer:`, and
`signer:`): show them to the person. The key that signed a package's first
version in a channel is presumed its publisher's, so a first install, or a
publish, signed by another key is refused too. If the person confirms with
the publisher, by another route than the channel, that the other key is
theirs, repeat the command with `ACCEPTKEY <that key, in full>`. Never
before.

When something behaves unexpectedly, add `TRACE <file>` (or set
`PKG_TRACE=<file>`, `-` for stderr): the file then tells every step, every
file touched, every check with what was expected and found, and every
choice with its reason. Read it before guessing. To follow a long command
while it runs, `PKG_EVENTS=<file>` writes its progress events, one line per
step begun, advanced, waiting or ended, with the outcome of each.

## Words

- **Drawer**: a directory laid out as it will be installed: `Libs/foo.library`
  for a library, `L/foo-handler` for a handler, `C/Tool` and `Tool.info` for
  a program. Flat files are laid out into these first.
- **Channel**: a directory with `index` and `objects/`; published into,
  installed from. Copy, sync or serve it as it is. A root can keep a list of
  the channels it reads (`CHANNEL ADD`), and then `CHANNEL` is left off the
  line; give `CHANNEL` when the person named one channel and meant it.
- **Root**: where packages are installed; its database is `<root>/.pkg/`.
- **Key**: the publisher's Ed25519 key file, readable by its owner alone.
- **Package name**: lower case, `a-z 0-9 + . _ -`. Read from a `$VER`
  cookie it is lower-cased (`Guru` becomes `guru`, `identify.library`
  stays). Other commands refer to it by exactly that name.

**Kinds.** The first version of a package names one; pkg refuses to guess.
Later versions take it from the last one published (`kind-from:`).

| `KIND` | For | Installed as |
|---|---|---|
| `image` | a program people run (a tool, an application, a game) | one read-only volume, `<name>.hdf`, mounted to run |
| `application` | a program installed as loose files into the root, when mounting is not wanted | its files, in place |
| `library`, `device`, `class`, `font`, `catalog` | system components other programs use: `Libs/`, `Devs/` or `L/` (a handler, a filesystem is `device`), `Classes/`, `Fonts/`, `Locale/` | their files, in their fixed place |
| `startup`, `boot` | pieces the system runs at startup | their files |
| `data`, `sdk`, `slave` | documents and data; headers and link libraries; WHDLoad slaves | their files |

When unsure between `image` and `application`, it is `image`; the
requester decides if they want otherwise. Keep a package's kind from one
version to the next: pkg warns when it changes.

**Several CPUs.** One version may be published for several CPUs (aarch64
for hosted AROS, x86_64 for native, m68k); the architecture is read from
the binaries. When installing from a host into a root for an AROS machine,
pass `ARCH <cpu>` the first time; the root remembers it. pkg running on
AROS knows its own CPU and needs no `ARCH` (so `Install-Pkg` passes none).
A refusal "offered for several CPUs" means exactly that. Publish each CPU's build separately,
same name and version.

**Two routes.** A system component (library, device or handler, class, font) is
published with its kind and installed into its fixed place. An application
is published with `KIND image`: it travels as one read-only volume, mounted
to run, and names the components it needs with `DEPENDS`.

## Rules

0. **An `ignored:` record is not an error.** A manifest may carry keys for
   other distribution systems; pkg keeps them and reports them. If an
   ignored key looks like a known one misspelt (`Categroy`), tell the
   requester; do not fix the manifest yourself.
1. **Publishing is permanent.** A channel has no unpublish. Run the command
   with `DRYRUN` first and read `name:`, `version:`, `kind:`,
   `architecture:`, `depends:`, `file:` and `signer:`; compare them with the
   versions already published (`SHOW <name> CHANNEL <dir>`). When unsure,
   publish to a scratch channel first.
2. **Never pass `ACCEPTKEY` or `DOWNGRADE` on your own.** pkg never suggests
   them; only the person's answer does.
3. **Never change `.pkg/` in a root, or `objects/` in a channel, by hand**;
   reading them does no harm, but `LIST`, `SHOW` and `TRACE` say the same
   more clearly. After everything is removed a root still holds the keys
   pinned for each package: that is on purpose.
4. **`REMOVE ORPHANS` removes packages.** Run it when the person asked to
   clean up what is left; otherwise show them the `orphan:` lines `REMOVE`
   printed.
5. **One key per publisher, kept.** Every later version of a package must be
   signed with the key that signed its first version; pkg refuses to publish
   it otherwise. So: if the package already exists in the channel (`SHOW
   <name> CHANNEL <dir>` names its signer), find that key and use it, never
   make a new one (`pkg KEYINFO FILE <keyfile>` names the public key a file
   holds). Make a key with KEYGEN only for someone who has never published,
   into a channel where their packages do not exist yet. In a team, each
   package belongs to whoever published it first; whether the team shares
   one key or each person keeps their own is the requester's call, and
   worth asking once. A key file you can read is not a key you may use:
   when the only key at hand is a colleague's, ask the requester before
   signing with it, even when they are in a hurry. A `DRYRUN` needs no key
   (`signer: none`, and `first-signer:` names the key the real publish
   will need), so find out what a publish would do before the key question
   is settled. Keep a key where the person keeps secrets (outside the
   channel and any repository), readable by them alone, backed up; never
   print, copy or commit it. The `public:` line is what may be shared.
6. **Check the result, not only the code.** On AROS a command that cannot
   even load leaves `$RC` as it was. A step has succeeded when its output
   has the `result:` you expect.

## Publishing

```sh
pkg KEYGEN FILE <keyfile> MACHINE          # once per publisher; never overwrites
export PKG_SIGNKEY=<keyfile>
pkg PUBLISH <drawer> CHANNEL <channel> KIND library DRYRUN MACHINE
pkg PUBLISH <drawer> CHANNEL <channel> KIND library MACHINE
```

`PACKAGE` is the same verb under another name, and says more plainly what
it does: the package goes into a channel on this disk, and nothing reaches
a portal until `PUSH`. Never tell the person their package is published on
a portal because `PUBLISH` succeeded.

Name and version come from the drawer's `$VER` cookie when it has one; pkg
refuses with 20, listing them, when cookies name different programs. Then
pass `NAME`, and `VERSION` follows from that program's cookie; if the only
cookie has another name (`afs.handler` packaged as `afs-handler`), its
version still counts. The architecture comes from the executables' own
headers (`aarch64`, `x86_64`, `m68k`, ...), `generic` when there are none;
mixed CPUs are refused. Host metadata is left out and each file named in a
`left-out:` line: names starting with `.` (`.DS_Store`, `.git`,
`.backdrop`), `Icon\r`, `Thumbs.db`, `desktop.ini`. `Name.info` icons stay.

PUBLISH creates the channel directory if it does not exist (`hint:` says
so). A program shows `architecture: generic` only when pkg found no
executable header in it: for a real AROS binary that means the drawer holds
something else than the build (a script, a copy, the wrong file); check.

An application:

```sh
pkg PUBLISH <drawer> CHANNEL <channel> NAME guru VERSION 2.0 KIND image \
            DEPENDS "identify.library >= 37.1" DRYRUN MACHINE
```

The volume's top level is the drawer, laid out as the program expects to
find itself when mounted; its own helpers, icons and documents go inside,
and only what other programs share goes into `DEPENDS`, by package name.
The dry run lists the files that go into the image as `content:` lines.
On macOS the filesystem ignores case: never create `GURU` beside `Guru`.

**A channel on the network:** give its URL as `CHANNEL https://host/pkg`
(or plain http) to every reading verb, on AROS as on every other system;
nothing else changes, and nothing is trusted before its signature and
digests are checked. PUBLISH and
WITHDRAW refuse a URL: publish into a local directory, then PUSH it.

**Publishing to a portal:** publish into a local directory as usual, then
`pkg PUSH CHANNEL <dir> TO https://<portal>/<channel>` with the publisher's
portal key in `PKG_PUSHKEY` (ask the requester for it; never invent or
print it). PUSH sends only what the portal lacks and relays its answer.

**From someone else's archive** (a nightly contrib `.tar.bz2`):
`PUBLISH "<archive>!/<top dir>" FILES "Extras/App,Prefs/Env-Archive/SYS/Packages/App"`
publishes those paths as one package without unpacking anything; copy or
link the archive into the channel as `archives/<name>` first. Add
`UPSTREAM <url>` (the archive's download URL) when the channel will go to a
portal: installs download the archive from there and PUSH does not upload it. Keep the
archive's own layout: files install at the paths the archive gives them. A
nightly's version is the component's `$VER` plus the date: `2.1+20260918`.

**A port that carries a `.pkginfo`** (`tools/contrib/PKGINFO.md`): `PUBLISH
<drawer> INFO <file>` takes the name, version, kind, catalogue fields,
`Depends`, `Config` and the package's paths (`Files`) from that file, so
nothing else is needed on the line. A keyword you give wins over the file.
`INFO "!/<path>"` reads it out of the archive a `"<archive>!/<top>"` drawer
comes from.

**Checking a system**: `VERIFY ALL ROOT <root>` for everything installed;
`REPAIR ALL ROOT <root> CHANNEL <channel>` puts back what is missing or
changed and keeps each change as `<file>.pkgold`. Tell the requester about
every `.pkgold`: the change may have been someone's work.

**An AROS installed with InstallAROS**: install the packages of the
version it was installed from; the files already there are taken over
(`adopted:`). A refusal with 15 names each file that differs: report it.

**Files people edit** (S/Startup-Sequence, Prefs/Env-Archive): declare
them at the first publish with `CONFIG "S/Startup-Sequence,Prefs/Env-Archive"`;
later versions inherit the list. An upgrade then keeps a person's edit and
sets the new copy beside it as `<path>.pkgnew`, reported as `config-new:`.
Tell the requester about each `.pkgnew`: merging it is their call. An
UPGRADE refused with 15 because a file "was edited" means that file was not
declared: never overwrite the edit, report it.

**Protection bits and comments.** On AROS, pkg publishes each file's own.
On a Mac, Linux or Windows, the files have none, so they come from a text
file named `.ameta` in each drawer directory (one per directory, the format
of the planning repository's `docs/features/file-metadata/ameta.md`):

```
ameta 1
file Go
prot 0x00000040
comment Starts%20the%20tool
```

`prot` is the AROS protection word (0x40 Script, 0x01 Delete forbidden, ...),
the comment is percent-escaped UTF-8. Owner Execute comes from the host mode
instead: `chmod +x` the programs, and a file without it installs with Execute
forbidden. Everything else is allowed unless `.ameta` says so. pkg refuses a
malformed or stale `.ameta` line, a comment over 79 characters or outside
Latin-1, naming it; it never packages `.ameta` itself. The attributes go in
the signed manifest (`Protect:`, `Comment:`), into an image's file headers,
onto the files on AROS, and into `.ameta` again when installing into a
folder on a host.

**The wrong program in the drawer** is the commonest slip: an old build, or
another program copied under the new one's name. pkg warns when a file's
`$VER` names another program than its file name says, and refuses a name
taken from a cookie that would replace a published package of another kind.
Read the dry run's `name:`, `version:` and `version-from:` against what the
person said, and ask when they disagree; `NAME` silences nothing, it only
names the package.

**A new version** takes `KIND` and `DEPENDS` from the last version
published in the channel, and says so (`kind-from:`, `depends-from:`); pass
them only to change them, `DEPENDS none` for a version that needs nothing.
`ARCH` comes from the binaries as always. Before publishing it,
check that the drawer holds the new build: pkg warns when `VERSION`
contradicts the program's own `$VER` cookie (often the old build copied by
mistake), when a dependency of the previous version is missing, and when
the kind changes. The dry run compares the new version with the last one
published, file by file: `compared-with:`, then `changed:` (with old and new
size), `same:`, `added:`, `gone:`. A program that should have changed and
shows `same:`, or changed by a few bytes while its `$VER` still gives the old
version (pkg warns about that one by name), is the old build: ask rather
than publish it.

**One CPU ahead of another** (an x86_64 fix, the aarch64 build not ready):
publish the new version for the CPU that has it, and nothing for the other.
Roots of the other CPU stay on their version; UPGRADE there answers
`unchanged` with a `note:` that the newer version exists for another CPU,
and asking for it there is refused as "published for x86_64 only". Publish
the other build later, same name and version, when it is ready.

## Installing and changing

```sh
pkg SEARCH   <word>... [CHANNEL <channel>] [ROOT <root>] MACHINE
pkg CHANNEL  ADD|LIST|REMOVE [<channel>] ROOT <root> MACHINE
pkg INSTALL  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg UPGRADE  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg UPGRADE  ALL ROOT <root> CHANNEL <channel> MACHINE
pkg STATUS   [<name>] ROOT <root> CHANNEL <channel> MACHINE
pkg ROLLBACK <name> ROOT <root> CHANNEL <channel> MACHINE
pkg VERIFY   <name> ROOT <root> MACHINE
pkg LIST     ROOT <root> MACHINE
pkg REMOVE   <name> ROOT <root> MACHINE
pkg REMOVE   ORPHANS ROOT <root> MACHINE
pkg SHOW     [<name>] CHANNEL <channel> [ROOT <root>] MACHINE
pkg KEYINFO  FILE <keyfile> MACHINE
```

Every one that changes something takes `DRYRUN`. Without `VERSION`, the
highest version. INSTALL settles every dependency before placing a file
(`dependency:` lines) and places nothing if anything is refused; installing
what is already installed answers `unchanged` and succeeds, so a retry is
safe. ROLLBACK goes back one step, to the version installed before the last
change. SHOW checks every entry of a channel as INSTALL would, without
installing, and exits 12 or 13 if any is damaged: use it before trusting a
channel, mirror or copy. Its `entry:` lines read `name version kind
architecture status signer`, status `ok` or a class name, and with `ROOT
<dir>` a last field, `installed`, `other-version` or `no`; `depends:` lines
read `package version needs [>= min]`, and a `warning:` names a package
signed by more than one key. `LIST`'s `package:` lines read `name version
kind files explicit|dependency`.

| Verb | `result:` on success |
|---|---|
| KEYGEN, KEYINFO | `created`; `shown` |
| MANIFEST | `shown`, then the manifest's own fields |
| PUBLISH | `published`; `unchanged` for the exact content already there; `repaired` when that content's objects were damaged |
| WITHDRAW | `withdrawn`, or `unchanged` |
| INSTALL | `installed`, `unchanged`, or `kept` (a dependency now kept for itself) |
| UPGRADE, ROLLBACK | `upgraded`, `downgraded`, `rolled-back`, or `unchanged` |
| UPGRADE ALL | `upgraded`, a `package:` line each, `count:`; `unchanged` with `count: 0` when nothing is upgradable |
| STATUS | `shown`, `package:` lines, `count:`, `upgradable:` |
| SEARCH | `shown`, `package: name version arch [channel] short` lines, `count:`; exit 0 with `count: 0` when nothing matches, which is an answer, not a refusal |
| CHANNEL | `added`, `shown` with `channel:` lines, or `removed` |
| VERIFY | `intact`; `moved` (0) when the person moved the drawer by hand, their right, nothing to repair; `damaged` with exit 12 and `changed:`/`missing:` lines |
| LIST | `listed`, `package:` lines, `count:` |
| REMOVE | `removed`, `orphan:` lines for what nothing needs any more |
| REMOVE ORPHANS | `removed`, a `package:` line each, `count:` |
| SHOW | `shown`, `entry:` lines, `count:`, `bad:` |
| IMAGE, MOUNTLIST | `created`, or `shown` without `OUT` |

Under `DRYRUN` the results read `would-publish`, `would-install`,
`would-upgrade`, `would-remove` and so on. The `result:` field is not always
the first line: look for it by key.

## Checking and updating everything

When the person asks whether their software is up to date, or to update
everything, or to set that up to happen by itself:

```sh
pkg STATUS ROOT <root> CHANNEL <channel> MACHINE
pkg UPGRADE ALL ROOT <root> CHANNEL <channel> DRYRUN MACHINE
pkg UPGRADE ALL ROOT <root> CHANNEL <channel> MACHINE
```

STATUS changes nothing and exits 0 whether or not updates exist. Its
`package:` lines read `name installed available state`; `available` is
what UPGRADE would take, `-` when the channel offers nothing for it. When
the root lists more than one channel, the channel the new version comes
from is a last field on the `package:` lines of STATUS and UPGRADE ALL.

UPGRADE ALL goes as far as it can and exits with the **worst** class of
what it refused, not the first; so does `INSTALL a b c`. Read every
`refused:` and `skipped:` line, not the exit code alone.

A package two listed channels offer under different keys is refused (14,
`ask-requester`) naming both channels and both keys, and is never settled
here: take it to the person, who says which key is the publisher's.

| `state` | What to tell the person |
|---|---|
| `current` | nothing to do |
| `upgradable` | UPGRADE ALL (or UPGRADE `<name>`) takes `available` |
| `withdrawn` | the publisher withdrew the installed version and offers nothing newer; whether to ROLLBACK or wait is their decision, and UPGRADE ALL never goes back |
| `not-offered` | the channel no longer has the package; it keeps working, it just gets no updates from there |
| `edited` | files were changed since install (VERIFY names them); if `available` is higher, the upgrade is refused while it would replace one of them |

`upgradable:` is how many packages UPGRADE ALL would attempt. UPGRADE ALL
upgrades them one at a time, a package before what depends on it, each
exactly as UPGRADE `<name>` would, and says `package: name from version` for
each one done. It never downgrades and never accepts a new key: it refuses
`VERSION`, `DOWNGRADE`, `ACCEPTKEY` and a name with 20. It goes as far as
it can: a package that needs a decision (a key change, an edited file) is
listed as `refused: <name> <version> <class> <reason>`, one that needs it
waits (`skipped: <name> <version> <waits-for>`), and everything else is
upgraded. `summary:` says it in a sentence. When something was not
upgraded, the answer is a refusal with the first one's class and `next:`,
and that is the exit code: do what `next:` says, and once the requester
has decided (a new key: on their word, `UPGRADE <that name> ...
ACCEPTKEY <key>`), run UPGRADE ALL again; it takes what waited.

**Unattended.** pkg has no scheduler and no daemon, and needs neither:
nothing it does ever prompts or reads stdin, so a startup script, cron,
launchd or the Task Scheduler runs these lines as they are. When the
person asks for updates to happen by themselves, write that line into
their scheduler or `S:User-Startup` with `MACHINE` and the output kept in
a file, and tell them where. Choose with them between STATUS alone (report,
let them decide) and UPGRADE ALL (update, report what was done); the
latter only if they asked for it. On AROS:

```
C:FailAt 21
Pkg UPGRADE ALL ROOT SYS: CHANNEL DEPOT: MACHINE >T:pkg-update.out
If ERROR
  Echo "Pkg stopped: see T:pkg-update.out"
EndIf
```

A channel that is not reachable (`DEPOT:` not mounted) is refused with 11,
never read as "nothing offered". An unattended run never takes the
decisions that belong to the requester: it stops, and the report says why.

## When a published version is broken

Nothing published is ever deleted; three things are possible instead.

- **It works badly** (crashes, wrong behaviour): its publisher withdraws it.
  `pkg WITHDRAW <name> VERSION <v> CHANNEL <dir> MACHINE`, signed with the
  key that signed that version (`DRYRUN` first). It stays in the channel,
  but INSTALL and UPGRADE no longer pick it, asking for it by version is
  refused, and SHOW marks it `withdrawn`. Machines that already have it
  ROLLBACK, or UPGRADE once a fixed version is out. Withdrawing is the
  requester's decision.
- **Its files in the channel are damaged** (SHOW says `integrity` or
  `signature`): its publisher publishes the same drawer again, same name and
  version, same key. pkg writes the damaged objects again and answers
  `repaired`. Without the original drawer and key it cannot be repaired:
  say so, and WITHDRAW it if the key is at hand.
- **Meanwhile**, anyone can install an intact version by asking for it
  with `VERSION`. A withdrawn version is skipped on its own; a damaged one
  is not: pkg refuses rather than quietly installing something older.

## Running an application image on AROS

INSTALL of an image answers `image:` (the file, at the top of the root) and
`blocks:`. Then, on AROS:

```
Pkg MOUNTLIST guru ROOT SYS:Apps OUT RAM:GURU MACHINE
```

writes the mount entry with the image's own geometry, and its `step:` lines
are the commands to run, in order: make a directory for `FDSK:`, link the
image there as `UnitN`, write-protect it, add the root's `Libs` to `LIBS:`
when its dependencies live there, and `Mount RAM:GURU`. The device is named
after the mountlist file: `GURU:`, and the volume is the drawer as it was
published. Run the program as `GURU:C/Guru`, or `Path GURU:C ADD`; for the
program, `PROGDIR:` is `GURU:C`, so data it finds beside itself must be in
`C/` in the drawer, and data it finds by a path from the volume
(`GURU:Data/levels`) must be where that path says. Only the program's
author knows which; ask when the drawer does not make it clear. If the program then cannot open a library its package
brought (seen on a native system booted from CD, where a RAM: directory added
to `LIBS:` is not searched), `CD <root>` before running it: the library loader
also looks in `libs/` under the current directory. Before an upgrade or
rollback replaces the image, `Eject GURU:`; afterwards run MOUNTLIST again,
since the size may change. The FFS handler is the system's; native AROS has
one. Hosted AROS built on macOS has none: build one on the host
(`sh tools/build-aros-extras.sh` writes `build/aros/afs-handler`), publish
it as `KIND device` with its file at `L/afs-handler` in the drawer, install
it into the root, and MOUNTLIST names it; or pass `HANDLER <path>`. The
`hint:` lines of MOUNTLIST say when no handler is installed.

**The whole flow**, for a person who will type on AROS, with `SYS:` as the
root (one database, libraries straight into `LIBS:`; another root such as
`Work:Apps` keeps the system drawer untouched, at the cost of an `Assign
LIBS: <root>/Libs ADD`, which MOUNTLIST adds to the steps):

```
Execute DEPOT:Install-Pkg DEPOT:                          ; once: Pkg itself
Pkg INSTALL guru ROOT SYS: CHANNEL DEPOT:                 ; guru and what it depends on
Pkg MOUNTLIST guru ROOT SYS: OUT RAM:GURU                 ; prints the steps
MakeDir RAM:fdsk
Assign FDSK: RAM:fdsk
MakeLink RAM:fdsk/Unit20 SYS:guru.hdf
Protect SYS:guru.hdf w SUB
Mount RAM:GURU
GURU:C/Guru 04000001
```

The step lines are the ones MOUNTLIST printed; copy them from its output
rather than from here, since unit and paths follow the arguments. They can
go into a script the person runs after each boot, since mounts do not
survive one; INSTALL answers `unchanged` when run again. Before giving a
person such a script, run the same lines against a scratch root on the host
(`ROOT <tmp> ARCH <cpu>`), and say which lines were checked on AROS and
which only on the host.

## On AROS without ARexx

An AmigaDOS script is enough:

```
C:FailAt 21
Pkg INSTALL guru ROOT SYS:Apps CHANNEL Work:channel MACHINE >T:pkg.out
C:Echo "$RC" >T:pkg.rc
```

Every refusal is at least 10, so `If ERROR` catches them all. A root other
than `SYS:` becomes visible by adding its directories: `Assign LIBS:
<root>/Libs ADD`, likewise `Devs:` and `L:`, and `Path <root>/C ADD`.

## The ARexx port, where ARexx is installed

`Run Pkg PORT` opens the port `PKG`. `ADDRESS PKG` takes the same commands;
with `options results`, RESULT is the output and RC the class code, and
`LASTERROR` returns the last refusal, as a record when the command had
`MACHINE`. `QUIT` closes the port. Nothing requires ARexx.

## Checking a new host

`tools/make-test-kit.sh` writes `build/pkg-test-kit.zip`, which runs the
contract sequence on Windows, macOS or Linux with `run.ps1` and writes
`report.txt`. Ask the person to run it on a host pkg has not met before.

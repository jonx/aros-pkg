<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Moving an existing distribution to pkg

You already ship software: LhA archives on Aminet, a nightly tarball, an
`Installer` script, a directory people copy into `SYS:`, or a package
manager of your own. This guide says what maps onto what, what pkg takes
over as it is, and what you change. Read [Publishing packages](publishing.md)
for the commands.

## What stays where it is

**Your archives.** pkg does not need to host your files. `PUBLISH` from an
archive with `UPSTREAM <url>` signs a description of the files and records
where the archive is; a machine downloads the archive from your server,
once, checks it against the size and SHA-256 you signed, and installs from
it. Your hosting, mirrors and download statistics stay as they are
([Packages from someone else's archive](publishing.md#packages-from-someone-elses-archive)).

**Your version numbers.** pkg reads the version from the `$VER:` string of
the program, the one `Version` prints on AROS. Keep numbering as you do. For
a nightly, `BUILD <date>` appends the date (`2.1+20260918`) so each nightly
is a newer version and an unchanged nightly publishes nothing.

**Your `.readme`.** An Aminet-style `.readme` beside the program supplies
the catalogue: `README <file>` at `PUBLISH` takes its `Short:`, `Author:`,
`Type:` and text for the fields you do not give explicitly
([Describe your package](publishing.md#describe-your-package)).

**Machines already set up.** A machine where your files are already in
place does not reinstall them. `INSTALL` compares each file with the
package's and *adopts* the identical ones: nothing is written, and the
package is recorded as installed, so `VERIFY`, `UPGRADE` and `REMOVE` work
from then on. This is how an AROS system installed by InstallAROS is put
under pkg after the fact: install the `aros-base` and `aros-tools` packages
over it, and the answer lists the files as `adopted`.

## What changes

**One drawer is one package.** pkg installs a drawer's files under the root
as they are laid out in the drawer (`C/`, `Libs/`, `S/`, your own drawer).
An archive that contains several independent programs becomes several
packages: `FILES` picks the paths of each.

**Dependencies are declared.** If your program needs a library that ships
separately, say so: `DEPENDS "sdl2 >= 2.0"`. pkg installs it first and
refuses to remove it while your program needs it. `MANIFEST` warns when a
program opens a `.library` that no dependency provides, and `RESOLVE` shows
what a machine would load ([Libraries](libraries.md)).

**Configuration files are named.** Files a person edits (`S/Startup`,
preferences) are declared with `CONFIG`, so an upgrade keeps the edited copy
and puts the new one beside it instead of overwriting.

**Signing.** Every version is signed with your key. The first version a
machine installs pins that key for the package; a later version signed by
another key is refused until the person accepts it. Make the key once
(`KEYGEN`), keep it out of every channel and repository, back it up
([Your key](publishing.md#your-key)).

## From an `Installer` script

An `Installer` script copies files, asks questions and edits
`S:User-Startup`. The copying part is what a package is; the questions are
what `CONFIG` and defaults are for. Today you write the package by hand:
a drawer laid out as the script would have left the system, plus `CONFIG`
for what the script asked about. A converter that reads `Installer` scripts
at build time is planned and not built; until then the script stays with
your archive for people who install by hand. For scripts that do more than
copy files, pkg is to run the script itself through Installer and record what
it changes: [Installer and pkg](installer-api.md), a proposal.

## From another package manager

| You have | With pkg |
|---|---|
| A repository index | a *channel*: a directory of signed manifests and files, served by any web server or a share ([Channels](channels.md)) |
| Package metadata | the manifest, derived from the drawer by `MANIFEST`; catalogue fields as keywords or from a `.readme` |
| Install scripts | none: a package is files; what a script decided is a `CONFIG` file or a default in the program |
| Dependency solving | `DEPENDS` with minimum versions, resolved at `INSTALL`; no ranges, no conflicts, no virtual packages |
| Uninstall | `REMOVE`, which keeps files the person edited and refuses while another package needs the one removed |
| A database of installed files | the root's `.pkg/` directory; `VERIFY` checks every file against it, `REPAIR` restores from the channel |
| Keys and trust | Ed25519 per publisher, pinned per package on each machine |

Migrate one package at a time: publish it into a channel of your own, install
it into a scratch root on your Mac or PC (`ROOT scratch`), read `LIST`,
`VERIFY` and `SHOW`, and only then point machines at the channel. `DRYRUN`
on any command shows what it would do without writing.

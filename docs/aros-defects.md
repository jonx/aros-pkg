<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# AROS defects found while building pkg

Each observed on hosted aarch64 AROS built from `jonx/AROS`, branch
`aarch64-darwin-graft`, unless the row says native pc-x86_64 (the
2026-09-18 nightly in QEMU). Two are fixed and proposed upstream, at the owner's
request (2026-09-18); the others are not reported.

| Where | What happens | Seen in | Worked around by |
|---|---|---|---|
| `C:Unpack` | Does not load: "file is not executable", for the shipped binary and for one rebuilt from its sources | goal 1 bootstrap; board thread 17 | Bootstrap through `minigzip` |
| identify.library, `IdAlert` | Every dead-end CPU alert decodes as "Unknown": `idalert.c` stores `ACPU_DivZero` and its neighbours with the dead-end bit (0x80000005) and searches with that bit masked off (`id & 0x7fffffff`), so the entry never matches | goal 2, `Guru 80000005` | The test decodes a recoverable alert, 04000001. Fixed: aros-development-team/AROS#1238, checked on hosted AROS (80000005 now reads Divide by zero, 84000001 Unknown gadget type) |
| posixc `stdout` | Output written through posixc reaches no shell redirection | first AROS runs | Output goes through `dos.library` `Output()` |
| posixc `errno` | No `EEXIST` for an existing directory, no `ENOENT` from `opendir` on an absent one | first AROS runs | Existence is tested, never inferred from errno |
| Regina, aros-contrib | For an ARexx port, RC is set to the RESULT string instead of the numeric `rm_Result1` | goal 1 | `tools/aros/regina-arexx-rc.patch`; proposed as aros-development-team/contrib#64 |
| The darwin hosted build | Ships no FFS handler at all, so no FFS volume can mount | goal 2 | `tools/build-aros-extras.sh` builds `rom/filesys/afs` |
| The shell, `$RC` | A command that cannot be loaded (file not found, volume not mounted) leaves `$RC` at its previous value, 0 or 10 alike, so a script reads success after it; not yet compared with AmigaOS | goal 2, then a four-case check | Scripts check each step's output, not only `$RC` |
| dos.library, `Lock()` on a multi-directory assign | A name found only in a later directory of the assign is not found by `Lock()` (`List`), while `Open()` (`Type`) finds it. Hosted: `Assign X: SYS:Libs`, `Assign X: RAM:L2 ADD`, `Echo x >RAM:L2/f`; `List X:f` fails, `Type X:f` works | a probe while testing native AROS | Nothing in pkg depends on it |
| `workbench/utilities/Installer` (V43.3) | Implements no `copyfiles`, `copylib`, `foreach`, `protect`, `tooltype` and more: each prints "Unimplemented command" and the script goes on; always opens its window, and waits at the welcome page, `(exit)` and every error even at novice level; a question with no default silently takes 0 or the first choice; Abort exits 0 like success, errors exit -1; its log does not list the files it wrote; started from a script's icon, it reads only its own icon and exits silently (`main.c:82`) | reading the source for the Installer modes, 2026-09-18 | AROS#1293 and #1297 (open) fix most of this; what pkg still needs is in [Installer and pkg](installer-api.md) |
| `tools/collect-aros`, the linker wrapper | Writes ELF `EI_ABIVERSION` 1 into every program whatever ABI it was built for (`set_os_and_abi`, a constant), and the ELF loader (`rom/dos/internalloadseg_elf.c`) never reads it, so a binary does not tell which ABI it needs | reading headers for the ABI field | pkg does not read the ABI from binaries; the publisher declares it |
| GRUB on an FFS system partition, native pc-x86_64 | Booting from the installed disk stops in GRUB: "alloc magic is broken at 0x7ffbde70", then "Aborted". The partition is FFSIntl, InstallAROS's default, 2 or 4 GB, in a logical partition of an MBR disk, GRUB put there by `Install-grub2` as InstallAROS runs it; the same with no pkg step at all. On SFS the same disk boots | `tests/native-system.sh`, a control run with `PKG_ADOPT=0 PKG_SYSFS=FFSIntl` | The test's system partition is SFS, InstallAROS's other choice |
| The pc-x86_64 ISO on FFS | The ISO carries names over 30 characters (`Prefs/Presets/Wallpapers/2007-comp-entries/04-Mithrandir_wallpaperupdated.jpg`, `Fonts/Work Sans ExtraLight Italic.font`, 42 in all), which FFS cannot hold: `Copy`, and so InstallAROS, shortens them without a word | `tests/native-system.sh` on FFS | pkg refuses to place such a file, saying why; on SFS they fit |
| FFS handler, native pc-x86_64 | Writing a few hundred small files, each under a temporary name then renamed, into new directories: the handler stops answering after a few hundred, at full CPU, and never comes back (twice, at the same kind of point); not isolated in a test of its own yet | `tests/native-system.sh`, before pkg stopped rewriting files already in place | pkg writes staging files under their own names and no longer rewrites files already there |
| Library search, native pc-x86_64 booted from CD | A library in a directory added to `LIBS:` is not found by OpenLibrary, nor by `Version identify.library`, whether the directory is in RAM: (`Assign LIBS: RAM:sys/Libs ADD`) or on a hard disk (`Assign LIBS: DH0:root/Libs ADD`, found again with Regina); the same assign works on hosted AROS | `tests/native-x86_64.sh`, `tests/native-contrib.sh` | The run changes to the root first: the loader also searches `libs/` under the current directory |

What would close the open ones is tracked in [OPEN.md](../OPEN.md).

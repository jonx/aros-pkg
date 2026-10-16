#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# End to end on the host. M1: publish a drawer into a channel that is a
# directory, install it into a root, list, verify, damage, remove, and the
# refusals. M2: signatures, key pinning, upgrade, rollback, downgrade.
#
# Independent oracles, so this does more than replay what the code intends:
# `shasum -a 256` for digests, `cmp` for installed bytes, `stat` for the key
# file's mode, and channel entries forged by Python rather than by pkg for the
# traversal cases. The forged entries are validly signed, so that what refuses
# them is the path check under test and not a missing signature.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-e2e.XXXXXX")
trap 'rm -rf "$T"' EXIT
# The suite runs as if this machine had no environment registered: what a
# command does without ROOT is part of what is checked here, and a default
# environment in the person's own configuration would answer for it. Without
# this the run reports what that one machine is set up to do.
export XDG_CONFIG_HOME="$T/config"

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

KEY="$T/dev.key"
EVIL="$T/other.key"
$PKG KEYGEN FILE "$KEY" > "$T/kg" 2>&1 || { echo "cannot create the key"; exit 1; }
$PKG KEYGEN FILE "$EVIL" > /dev/null 2>&1
PUB=$(awk '/^Public:/{print $2}' "$KEY")
EVILPUB=$(awk '/^Public:/{print $2}' "$EVIL")
export PKG_SIGNKEY="$KEY"

echo "keys"
[ "$(stat -f %Lp "$KEY" 2>/dev/null || stat -c %a "$KEY")" = "600" ]
                                                      ok $? "key file readable by the owner alone"
has "$T/kg" "public key $PUB";                        ok $? "keygen prints the public key"
$PKG KEYGEN FILE "$KEY" > "$T/kg2" 2>&1
[ $? -eq 15 ];                                         ok $? "an existing key is never overwritten"
[ "$(awk '/^Public:/{print $2}' "$KEY")" = "$PUB" ];  ok $? "and the key is unchanged"

echo "publish_install_verify_remove"

D="$T/drawer"
mkdir -p "$D/C" "$D/Libs" "$D/S"
printf 'binary\000$VER: Hello 1.2 (18.9.2026)\000tail' > "$D/C/Hello"
printf 'library data\n' > "$D/Libs/data.txt"
printf 'echo started\n' > "$D/S/My Startup"
printf 'finder junk' > "$D/.DS_Store"
printf 'appledouble' > "$D/Libs/._data.txt"
mkdir -p "$D/.git/objects"; printf 'repo' > "$D/.git/objects/x"
printf 'workbench state' > "$D/.backdrop"
printf 'an icon' > "$D/C.info"
CH="$T/channel"
R="$T/root"

$PKG MANIFEST "$D" > "$T/m1" 2>&1;                    ok $? "manifest succeeds"
has "$T/m1" '^Name: hello$';                          ok $? "name from \$VER:, lowercased"
has "$T/m1" '^Version: 1.2$';                         ok $? "version from \$VER:"
! has "$T/m1" 'DS_Store';                             ok $? ".DS_Store left out"
! has "$T/m1" '\._data';                              ok $? "AppleDouble left out"
! has "$T/m1" '\.git' && ! has "$T/m1" 'backdrop';    ok $? "a hidden directory and the Workbench .backdrop left out"
has "$T/m1" ' C\.info$';                              ok $? "an Amiga icon, Name.info, is kept"
has "$T/m1" ' S/My Startup$';                         ok $? "a path with a space"
grep '^File:' "$T/m1" | awk '{print $4" "$5}' > "$T/order"
sort "$T/order" | cmp -s - "$T/order";                ok $? "File lines sorted"

want=$(shasum -a 256 "$D/Libs/data.txt" | cut -d' ' -f1)
has "$T/m1" "^File: $want 13 Libs/data.txt$";         ok $? "digest agrees with shasum"

$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/pub" 2>&1;      ok $? "publish succeeds"
has "$T/pub" 'packaged hello 1.2';                   ok $? "publish reports what it did"
has "$T/pub" "signed by $(echo "$PUB" | cut -c1-16)"; ok $? "publish names the signing key"
has "$T/pub" 'left out \.DS_Store,' && has "$T/pub" 'left out Libs/\._data\.txt,'
                                                      ok $? "publish names each host file it left out"
digest=$(awk '$1=="hello"{print $4}' "$CH/index")
payload=$(awk '/^Payload:/{print $2}' "$CH/objects/$digest.manifest")
[ "$(shasum -a 256 "$CH/objects/$digest.manifest" | cut -d' ' -f1)" = "$digest" ]
                                                      ok $? "the index names the manifest by its shasum"
[ "$(shasum -a 256 "$CH/objects/$payload.pkg" | cut -d' ' -f1)" = "$payload" ]
                                                      ok $? "the manifest names the payload by its shasum"
cmp -s "$T/m1" "$CH/objects/$digest.manifest";        ok $? "MANIFEST equals what PUBLISH stored"
has "$CH/objects/$digest.sig" "^Signer: $PUB$";       ok $? "signature file names its signer"

cp -R "$D" "$T/elsewhere"
$PKG MANIFEST "$T/elsewhere" > "$T/m2" 2>&1
cmp -s "$T/m1" "$T/m2";                               ok $? "same drawer elsewhere, same manifest"

$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/pub2" 2>&1;     ok $? "republishing identical bytes is not an error"
has "$T/pub2" 'nothing to do';                        ok $? "and says so"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/inst" 2>&1
                                                      ok $? "install succeeds"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "C/Hello byte-identical"
cmp -s "$D/S/My Startup" "$R/S/My Startup";           ok $? "S/My Startup byte-identical"
[ ! -e "$R/.DS_Store" ];                              ok $? "host metadata not installed"
[ ! -e "$R/.pkg/staging/hello" ];                     ok $? "staging cleaned up"
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$PUB" ];    ok $? "the signing key is pinned on first install"

$PKG LIST ROOT "$R" > "$T/list" 2>&1;                 ok $? "list succeeds"
has "$T/list" '^hello  *1.2  *application  *4 files'; ok $? "list shows the package"

$PKG VERIFY hello ROOT "$R" > "$T/ver" 2>&1;          ok $? "verify passes when intact"
has "$T/ver" 'all intact';                            ok $? "and says so"

printf 'edited by the user\n' > "$R/Libs/data.txt"
$PKG VERIFY hello ROOT "$R" > "$T/ver2" 2>&1
[ $? -eq 12 ];                                         ok $? "verify fails after an edit"
has "$T/ver2" 'changed  Libs/data.txt';               ok $? "and names the file"

$PKG REMOVE hello ROOT "$R" > "$T/rm" 2>&1;           ok $? "remove succeeds"
has "$T/rm" 'kept     Libs/data.txt';                 ok $? "the edited file is kept"
[ -f "$R/Libs/data.txt" ];                            ok $? "and is still there"
[ ! -e "$R/C" ];                                      ok $? "emptied directories are pruned"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "database entry gone"
rm -f "$R/Libs/data.txt"; rmdir "$R/Libs" 2>/dev/null

echo "cookies"

mkdir -p "$T/two/C"
printf 'x\000$VER: Guru 2.0 (31.5.2011)\000' > "$T/two/C/Guru"
printf 'y\000$VER: Function 2.0 (31.5.2011)\000' > "$T/two/C/Function"
$PKG MANIFEST "$T/two" > "$T/two.m" 2>&1
[ $? -eq 20 ] && has "$T/two.m" 'C/Function (function 2.0), C/Guru (guru 2.0)'
                                                      ok $? "two programs' cookies, no NAME: refused with 20, both listed"
$PKG MANIFEST "$T/two" NAME guru > "$T/two.m2" 2>&1
has "$T/two.m2" '^Name: guru$' && has "$T/two.m2" '^Version: 2.0$'
                                                      ok $? "NAME alone takes the version from that program's cookie"
mkdir -p "$T/one/L"
printf 'h\000$VER: afs.handler 41.7 (1.1.2026)\000' > "$T/one/L/afs-handler"
$PKG MANIFEST "$T/one" NAME afs-handler > "$T/one.m" 2>&1
has "$T/one.m" '^Name: afs-handler$' && has "$T/one.m" '^Version: 41.7$'
                                                      ok $? "NAME differing from the only cookie keeps that cookie's version"

# Architecture from the executables' own headers.
mkdir -p "$T/elf/C" "$T/mix/C"
python3 -c "
import sys
elf = bytearray(64); elf[0:4] = b'\x7fELF'; elf[4] = 2; elf[5] = 1; elf[18] = 183
open(sys.argv[1], 'wb').write(bytes(elf) + b'\0\$VER: Tool 1.0 (1.1.2026)\0')
# a minimal 68k program: HUNK_HEADER, one hunk, HUNK_CODE with rts, HUNK_END
hunk = bytes.fromhex('000003f3 00000000 00000001 00000000 00000000 00000001 000003e9 00000001 4e754e71 000003f2'.replace(' ', ''))
open(sys.argv[2], 'wb').write(hunk)
# hunk-format data with no code, as deficons.prefs is: bound to no CPU
open(sys.argv[3], 'wb').write(bytes.fromhex('000003f3 00000000 00000001 00000000 00000000 00000001 000003ea 00000001 70726f6a 000003f2'.replace(' ', '')))
" "$T/elf/C/Tool" "$T/mix/C/Old" "$T/hunkdata"
cp "$T/elf/C/Tool" "$T/mix/C/Tool"
$PKG MANIFEST "$T/elf" > "$T/elf.m" 2>&1
has "$T/elf.m" '^Architecture: aarch64$';            ok $? "an aarch64 ELF executable makes the package aarch64"
$PKG MANIFEST "$T/mix" NAME tool > "$T/mix.m" 2>&1
[ $? -eq 20 ] && has "$T/mix.m" 'm68k in 1 file, such as C/Old' && has "$T/mix.m" 'aarch64 in 1 file'
                                                      ok $? "aarch64 and 68k hunk executables in one drawer: refused with 20, each CPU counted and named"
$PKG MANIFEST "$T/mix" NAME tool KIND boot ARCH aarch64 > "$T/mix.m3" 2>&1
[ $? -eq 0 ] && has "$T/mix.m3" '^Architecture: aarch64$'
                                                      ok $? "a boot package may hold other CPUs' loader stages when ARCH names its machine"
$PKG MANIFEST "$T/mix" NAME tool KIND boot > "$T/mix.m4" 2>&1
[ $? -eq 20 ] && has "$T/mix.m4" 'KIND and ARCH';     ok $? "without ARCH it is still refused, saying what would allow it"
mkdir -p "$T/hd/C" "$T/hd/Prefs"; cp "$T/elf/C/Tool" "$T/hd/C/Tool"; cp "$T/hunkdata" "$T/hd/Prefs/deficons.prefs"
$PKG MANIFEST "$T/hd" > "$T/hd.m" 2>&1
[ $? -eq 0 ] && has "$T/hd.m" '^Architecture: aarch64$'; ok $? "a hunk file of data only, like deficons.prefs, is bound to no CPU"
mkdir -p "$T/hf/C" "$T/hf/Fonts/topaz" "$T/hf/Devs/Keymaps"; cp "$T/elf/C/Tool" "$T/hf/C/Tool"
python3 -c "
import sys
# a classic font: its code starts with moveq #100,d0 and rts
open(sys.argv[1], 'wb').write(bytes.fromhex('000003f3 00000000 00000001 00000000 00000000 00000002 000003e9 00000002 70644e75 00000000 000003f2'.replace(' ', '')))
" "$T/hf/Fonts/topaz/8"
cp "$T/mix/C/Old" "$T/hf/Devs/Keymaps/usa"
$PKG MANIFEST "$T/hf" > "$T/hf.m" 2>&1
[ $? -eq 0 ] && has "$T/hf.m" '^Architecture: aarch64$'; ok $? "a classic font and a keymap are data any CPU loads, not 68k programs"
$PKG MANIFEST "$T/elf" ARCH m68k > "$T/elf.m2" 2>&1
[ $? -eq 20 ] && has "$T/elf.m2" 'built for aarch64'; ok $? "ARCH contradicting the executables is refused"
$PKG MANIFEST "$D" > "$T/gen.m" 2>&1
has "$T/gen.m" '^Architecture: generic$';            ok $? "a drawer with no executable header stays generic"
$PKG HELP > "$T/help" 2>"$T/help.e"
[ $? -eq 0 ] && has "$T/help" 'DEPENDS' && [ ! -s "$T/help.e" ]
                                                      ok $? "HELP prints the usage on stdout and succeeds"

echo "refusals"

printf 'different\n' > "$D/Libs/data.txt"
$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/rep" 2>&1
[ $? -eq 15 ];                                         ok $? "republishing a version with new bytes refused"
has "$T/rep" 'already published with a different payload' && has "$T/rep" 'was not raised'
                                                      ok $? "and says why: the version came from a \$VER not raised, or the old build"
printf 'library data\n' > "$D/Libs/data.txt"

env -u PKG_SIGNKEY $PKG PUBLISH "$D" CHANNEL "$T/ch-nokey" KIND application > "$T/nokey" 2>&1
[ $? -eq 14 ];                                         ok $? "publishing without a key refused, class key"
has "$T/nokey" 'same key';                            ok $? "and says to use the publisher's key"
[ ! -e "$T/ch-nokey/index" ];                         ok $? "and nothing was published"

cp "$CH/objects/$payload.pkg" "$T/good.pkg"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$CH/objects/$payload.pkg"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/tamper" 2>&1
[ $? -eq 12 ];                                         ok $? "tampered payload refused"
has "$T/tamper" "expected $payload";                  ok $? "the refusal names the expected digest"
[ ! -e "$R/C/Hello" ];                                ok $? "and nothing was installed"
cp "$T/good.pkg" "$CH/objects/$payload.pkg"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/twice" 2>&1
[ $? -eq 0 ] && has "$T/twice" 'already installed';  ok $? "installing the installed version again succeeds and says so"
$PKG REMOVE hello ROOT "$R" > /dev/null 2>&1

mkdir -p "$R/C"; printf 'mine' > "$R/C/Hello"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/clash" 2>&1
[ $? -eq 15 ];                                         ok $? "overwrite refused"
[ "$(cat "$R/C/Hello")" = "mine" ];                   ok $? "the existing file is untouched"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing is recorded"
rm -rf "$R"

$PKG INSTALL nosuch ROOT "$R" CHANNEL "$CH" > "$T/none" 2>&1
[ $? -eq 11 ];                                         ok $? "unknown package refused"
$PKG INSTALL hello VERSION 9 ROOT "$R" CHANNEL "$CH" > "$T/v9" 2>&1
[ $? -eq 11 ];                                         ok $? "absent version refused"
has "$T/v9" 'versions offered: 1.2';                  ok $? "and the versions on offer are listed"

# Forged entries, written by Python and signed with the development key, so
# the signature is valid and the path check is what must refuse them.
forge() {  # name container_path manifest_path
    d=$(python3 - "$CH" "$1" "$2" "$3" <<'PY'
import sys, hashlib, struct, os
ch, name, cpath, mpath = sys.argv[1:]
data = b'evil\n'
body = struct.pack('>I', len(cpath)) + cpath.encode() + b'\0' + struct.pack('>I', len(data)) + data
pkg = b'PKG\x01' + struct.pack('>I', 8 + len(body)) + body
d = hashlib.sha256(pkg).hexdigest()
fd = hashlib.sha256(data).hexdigest()
man = ('Format: pkg-manifest 1\nName: %s\nVersion: 1\nArchitecture: generic\n'
       'Kind: data\nPayload: %s\nFile: %s %d %s\n') % (name, d, fd, len(data), mpath)
md = hashlib.sha256(man.encode()).hexdigest()
open(os.path.join(ch, 'objects', d + '.pkg'), 'wb').write(pkg)
open(os.path.join(ch, 'objects', md + '.manifest'), 'w').write(man)
open(os.path.join(ch, 'index'), 'a').write('%s 1 generic %s\n' % (name, md))
print(md)
PY
)
    $PKG SIGN "$CH/objects/$d.manifest" KEY "$KEY" OUT "$CH/objects/$d.sig" > /dev/null
}

forge evil-a "../evil" "../evil"
$PKG INSTALL evil-a ROOT "$R" CHANNEL "$CH" > "$T/ea" 2>&1
[ $? -eq 12 ];                                         ok $? "traversal in the manifest refused"
has "$T/ea" "'\.\.' component";                       ok $? "for the traversal, not the signature"

forge evil-b "../evil" "evil"
$PKG INSTALL evil-b ROOT "$R" CHANNEL "$CH" > "$T/eb" 2>&1
[ $? -eq 12 ];                                         ok $? "container disagreeing with its manifest refused"
has "$T/eb" 'where the manifest lists';               ok $? "for the disagreement, not the signature"
[ ! -e "$T/evil" ] && [ ! -e "$R/evil" ];             ok $? "nothing written inside or outside the root"

forge evil-c ".pkg/db/hello" ".pkg/db/hello"
$PKG INSTALL evil-c ROOT "$R" CHANNEL "$CH" > "$T/ec" 2>&1
[ $? -eq 12 ];                                         ok $? "a payload writing into the database refused"
has "$T/ec" 'inside .pkg';                            ok $? "for the database path, not the signature"

echo "identical_payloads"

# Two versions whose bytes are identical keep separate manifests and signatures
# over one shared payload. The first layout keyed manifests by the payload, so
# the second publish overwrote the first's manifest; this is the case that
# found it.
mkdir -p "$T/same/Libs"; printf 'same bytes\n' > "$T/same/Libs/x.txt"
$PKG PUBLISH "$T/same" CHANNEL "$CH" NAME same VERSION 1 KIND data > /dev/null 2>&1
                                                      ok $? "publish same 1"
$PKG PUBLISH "$T/same" CHANNEL "$CH" NAME same VERSION 2 KIND data > /dev/null 2>&1
                                                      ok $? "publish same 2, identical bytes"
[ "$(awk '$1=="same"{print $4}' "$CH/index" | sort -u | wc -l | tr -d ' ')" = 2 ]
                                                      ok $? "two index entries, two distinct manifests"
$PKG INSTALL same VERSION 1 ROOT "$T/r-same" CHANNEL "$CH" > "$T/same1" 2>&1
                                                      ok $? "version 1 installs"
has "$T/same1" 'installed same 1';                    ok $? "and is version 1"
$PKG UPGRADE same VERSION 2 ROOT "$T/r-same" CHANNEL "$CH" > "$T/same2" 2>&1
                                                      ok $? "version 2 installs over it"
has "$T/same2" 'upgraded same from 1 to 2';           ok $? "and is version 2"

echo "signatures"

cp "$CH/objects/$digest.sig" "$T/good.sig"
python3 -c "
import sys
p=sys.argv[1]; s=open(p).read(); i=s.index('Signature: ')+11
c='0' if s[i]!='0' else '1'; open(p,'w').write(s[:i]+c+s[i+1:])
" "$CH/objects/$digest.sig"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/sigt" 2>&1
[ $? -eq 13 ];                                         ok $? "an altered signature refused"
has "$T/sigt" 'does not verify';                      ok $? "and says why"
cp "$T/good.sig" "$CH/objects/$digest.sig"

rm "$CH/objects/$digest.sig"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/unsig" 2>&1
[ $? -eq 13 ];                                         ok $? "an unsigned package refused"
has "$T/unsig" 'is not signed';                       ok $? "and says why"
cp "$T/good.sig" "$CH/objects/$digest.sig"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing was installed by either"

echo "upgrade_rollback"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
                                                      ok $? "install 1.2"
cp -R "$D" "$T/v13"
printf 'binary 2\000$VER: Hello 1.3 (19.9.2026)\000tail' > "$T/v13/C/Hello"
rm "$T/v13/S/My Startup"; rmdir "$T/v13/S"
printf 'new in 1.3\n' > "$T/v13/Libs/extra.txt"
$PKG PUBLISH "$T/v13" CHANNEL "$CH" KIND application > /dev/null 2>&1; ok $? "publish 1.3"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/up" 2>&1
                                                      ok $? "upgrade to 1.3"
has "$T/up" 'upgraded hello from 1.2 to 1.3';         ok $? "upgrade reports both versions"
cmp -s "$T/v13/C/Hello" "$R/C/Hello";                 ok $? "C/Hello is the 1.3 bytes"
[ -f "$R/Libs/extra.txt" ];                           ok $? "a file new in 1.3 appears"
[ ! -e "$R/S/My Startup" ] && [ ! -e "$R/S" ];        ok $? "a file dropped in 1.3 is removed, with its directory"
$PKG VERIFY hello ROOT "$R" > /dev/null 2>&1;         ok $? "verify passes on 1.3"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/up2" 2>&1
                                                      ok $? "upgrading when current is not an error"
has "$T/up2" 'already at 1.3';                        ok $? "and says so"

$PKG ROLLBACK hello ROOT "$R" CHANNEL "$CH" > "$T/rb" 2>&1
                                                      ok $? "rollback succeeds"
has "$T/rb" 'rolled back hello from 1.3 to 1.2';      ok $? "rollback reports both versions"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "C/Hello is the 1.2 bytes again"
cmp -s "$D/S/My Startup" "$R/S/My Startup";           ok $? "the file 1.3 dropped is back"
[ ! -e "$R/Libs/extra.txt" ];                         ok $? "the file 1.3 added is gone"
$PKG VERIFY hello ROOT "$R" > /dev/null 2>&1;         ok $? "verify passes on 1.2"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG UPGRADE hello VERSION 1.2 ROOT "$R" CHANNEL "$CH" > "$T/dg" 2>&1
[ $? -eq 18 ];                                         ok $? "EXACT to an older version refused without DOWNGRADE"
has "$T/dg" "requester's decision" && ! has "$T/dg" 'add DOWNGRADE'
                                                      ok $? "and leaves the choice to the requester, with no keyword to paste"
$PKG UPGRADE hello VERSION 1.2 DOWNGRADE ROOT "$R" CHANNEL "$CH" > "$T/dg2" 2>&1
                                                      ok $? "EXACT with DOWNGRADE succeeds"
has "$T/dg2" 'downgraded hello from 1.3 to 1.2';      ok $? "and calls it a downgrade"

printf 'my own edit\n' > "$R/C/Hello"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/conf" 2>&1
[ $? -eq 15 ];                                         ok $? "upgrade over an edited file both versions ship refused"
[ "$(cat "$R/C/Hello")" = "my own edit" ];            ok $? "the edit is untouched"
has "$T/conf" 'was edited since';                     ok $? "and the refusal names the file"
cp "$D/C/Hello" "$R/C/Hello"

echo "substituted_key"

cp -R "$T/v13" "$T/v14"
printf 'binary 3\000$VER: Hello 1.4 (20.9.2026)\000tail' > "$T/v14/C/Hello"
before=$(cat "$CH/index")
PKG_SIGNKEY="$EVIL" $PKG PUBLISH "$T/v14" CHANNEL "$CH" KIND application MACHINE > "$T/p14" 2>&1
[ $? -eq 14 ] && has "$T/p14" '^next: ask-requester$' && [ "$(cat "$CH/index")" = "$before" ]
                                                      ok $? "publishing hello with another key than its earlier versions: 14, ask-requester, nothing published"
PKG_SIGNKEY="$EVIL" $PKG PUBLISH "$T/v14" CHANNEL "$CH" KIND application ACCEPTKEY "$EVILPUB" > /dev/null 2>&1
                                                      ok $? "a second key can publish 1.4 when ACCEPTKEY names it"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/sub" 2>&1
[ $? -eq 14 ];                                         ok $? "an upgrade signed by a substituted key refused"
has "$T/sub" "pinned $PUB";                           ok $? "the refusal prints the pinned key"
has "$T/sub" "signer $EVILPUB";                       ok $? "and the new signer"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "and nothing was changed"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$PUB" > "$T/sub2" 2>&1
[ $? -eq 14 ];                                         ok $? "ACCEPTKEY naming the wrong key does not open the door"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$EVILPUB" > "$T/sub3" 2>&1
                                                      ok $? "ACCEPTKEY naming the new key in full accepts it"
has "$T/sub3" 'changed by explicit ACCEPTKEY';        ok $? "and says so"
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$EVILPUB" ]; ok $? "and the new key is now the pinned one"

echo "machine_contract"

# The contract an agent reads on every host: stdout holds only "key: value"
# lines, stderr stays empty, and a refusal's code field equals the exit status.
# The format check is shown able to fail on the human output of the same verb.
M="$T/mroot"
only_kv() { ! LC_ALL=C grep -a -v -E '^[a-z][a-z-]*: ' "$1" > /dev/null; }
mrun() {  # mrun <name> <args...>: stdout to $T/<name>.o, stderr to $T/<name>.e
    n=$1; shift
    $PKG "$@" > "$T/$n.o" 2> "$T/$n.e"
}

mrun mi INSTALL hello VERSION 1.2 ROOT "$M" CHANNEL "$CH" MACHINE
                                                      ok $? "MACHINE install exits 0"
only_kv "$T/mi.o" && [ ! -s "$T/mi.e" ];              ok $? "and prints only key: value lines, nothing on stderr"
has "$T/mi.o" '^result: installed$' && has "$T/mi.o" '^version: 1.2$' \
    && has "$T/mi.o" "^signer: $PUB\$";               ok $? "with the result, the version and the full signer key"
$PKG REMOVE hello ROOT "$M" > /dev/null 2>&1
$PKG INSTALL hello VERSION 1.2 ROOT "$M" CHANNEL "$CH" > "$T/human.o" 2>&1
! only_kv "$T/human.o";                               ok $? "control: the human output fails the same format check"

PKG_OUTPUT=machine $PKG LIST ROOT "$M" > "$T/ml.o" 2> "$T/ml.e"
                                                      ok $? "PKG_OUTPUT=machine list exits 0"
only_kv "$T/ml.o" && has "$T/ml.o" '^result: listed$' && has "$T/ml.o" '^package: hello 1.2 ' \
    && has "$T/ml.o" '^count: 1$'
                                                      ok $? "the environment variable selects the same contract"
mrun mv VERIFY hello ROOT "$M" MACHINE
has "$T/mv.o" '^result: intact$';                     ok $? "verify reports intact"
mrun mu UPGRADE hello VERSION 1.3 ROOT "$M" CHANNEL "$CH" MACHINE
has "$T/mu.o" '^result: upgraded$' && has "$T/mu.o" '^from: 1.2$' && has "$T/mu.o" '^version: 1.3$'
                                                      ok $? "upgrade reports from and to"
mrun mr ROLLBACK hello ROOT "$M" CHANNEL "$CH" MACHINE
has "$T/mr.o" '^result: rolled-back$' && has "$T/mr.o" '^version: 1.2$'
                                                      ok $? "rollback reports rolled-back"

refusal() {  # refusal <name> <code> <class> <what>
    rc=$3; name=$1
    only_kv "$T/$name.o" && [ ! -s "$T/$name.e" ] && has "$T/$name.o" '^result: refused$' \
        && has "$T/$name.o" "^class: $4\$" && has "$T/$name.o" "^code: $2\$" \
        && has "$T/$name.o" '^reason: .' && [ "$rc" -eq "$2" ]
    ok $? "$5 exits $2 and says class $4, code $2"
}
mrun x1 INSTALL hello ROOT "$M" CHANNEL "$CH" MACHINE;          refusal x1 15 $? conflict "a second install"
# A root set up outside pkg: the package's path holds other content. Refused,
# and with TAKEOVER the file becomes the package's, byte for byte.
TO="$T/takeover"; mkdir -p "$TO/C"; printf 'an older build' > "$TO/C/Hello"
mrun t1 INSTALL hello VERSION 1.2 ROOT "$TO" CHANNEL "$CH" MACHINE; refusal t1 15 $? conflict "a differing file in the root"
has "$T/t1.o" 'TAKEOVER';                             ok $? "and the reason names TAKEOVER"
! cmp -s "$TO/C/Hello" "$D/C/Hello";                  ok $? "the file was not touched"
mrun t2 INSTALL hello VERSION 1.2 ROOT "$TO" CHANNEL "$CH" TAKEOVER MACHINE
                                                      ok $? "TAKEOVER installs"
has "$T/t2.o" '^result: installed$' && has "$T/t2.o" '^taken-over: 1$'
                                                      ok $? "and counts the file taken over"
cmp -s "$TO/C/Hello" "$D/C/Hello";                    ok $? "the file now holds the package's bytes"
mrun t3 VERIFY hello ROOT "$TO" MACHINE && has "$T/t3.o" '^result: intact$'
                                                      ok $? "and the root verifies intact"
mrun x2 INSTALL nosuch ROOT "$M" CHANNEL "$CH" MACHINE;         refusal x2 11 $? not-found "an unknown package"
mrun x3 UPGRADE hello ROOT "$M" CHANNEL "$CH" MACHINE;          refusal x3 14 $? key "1.4 from another key"
has "$T/x3.o" "^reason: .*$EVILPUB";                  ok $? "the one-line reason still names the new signer"
mrun x4 FROB MACHINE;                                           refusal x4 20 $? usage "an unknown verb"
mrun x6 INSTALL a b MACHINE;                                    refusal x6 20 $? usage "a usage error found before MACHINE"
# A word that is not a verb is named, with the nearest verb when it is a slip
# of the fingers and the way to a newer build when it is not. The whole usage
# printed instead told nobody which of the two had happened: a tester read
# "ENV is not recognised" on an older build as the keywords being case
# sensitive, which they have never been.
has "$T/x4.o" '^reason: "FROB" is not a verb of pkg '
                                                      ok $? "the word that is not a verb is named, with the version that does not know it"
has "$T/x4.o" 'pkg U updates pkg itself';             ok $? "and a word near no verb points at a newer build"
mrun x7 INSTAL MACHINE;                                         refusal x7 20 $? usage "a mistyped verb"
has "$T/x7.o" 'did you mean INSTALL?';                ok $? "and the verb it was nearest to is offered"
mrun x8 instal MACHINE
has "$T/x8.o" 'did you mean INSTALL?';                ok $? "whatever case it was typed in"
mrun x9 ENVS MACHINE
has "$T/x9.o" 'did you mean ENV?';                    ok $? "including the verbs that are answered apart from the table"
# The first question asked of a tool that surprised someone. It used to be
# answered by accident: an unknown word printed the whole usage, whose first
# line is the version. Now the usage is not printed, so it is answered on
# purpose, and MACHINE gets it as records.
$PKG VERSION > "$T/v1.o" 2>&1
[ $? = 0 ] && grep -q "^pkg [0-9]" "$T/v1.o";                ok $? "VERSION says which build this is, and exits 0"
$PKG --version > "$T/v2.o" 2>&1
cmp -s "$T/v1.o" "$T/v2.o";                           ok $? "and --version says the same"
mrun v3 VERSION MACHINE
only_kv "$T/v3.o" && has "$T/v3.o" '^result: version$' && has "$T/v3.o" '^version: [0-9]'
                                                      ok $? "MACHINE gets it as records, not as a sentence"
$PKG RESOLVE hello VERSION 1.2 CHANNEL "$CH" MACHINE > "$T/v4.o" 2>&1
! has "$T/v4.o" 'not a word';                         ok $? "control: VERSION is still the keyword after a verb that takes it"

mrun xb UNINSTALL MACHINE
has "$T/xb.o" 'pkg says REMOVE';                      ok $? "a word another tool uses is answered with the verb pkg has for it"
! has "$T/xb.o" 'INSTALL?';                           ok $? "and never with the verb it merely resembles: UNINSTALL is two edits from INSTALL"
$PKG FROB > "$T/xa.o" 2>&1
has "$T/xa.o" 'is not a verb' && ! has "$T/xa.o" '^Installing and keeping software$'
                                                      ok $? "a person is told the same, instead of a page of usage"
has "$T/xa.o" '^pkg: ';                               ok $? "and the tool names itself once, not twice"
# NAME machine names a package: the word after a keyword is its value,
# never the MACHINE switch. MANIFEST takes NAME, and prints for a person.
mkdir -p "$T/nmd/C"; printf 'x\n' > "$T/nmd/C/X"
$PKG MANIFEST "$T/nmd" NAME machine VERSION 1.0 KIND data > "$T/nm.o" 2>&1
has "$T/nm.o" '^Name: machine' && ! has "$T/nm.o" '^result:'
                                                      ok $? "NAME machine is a value, and leaves the human output alone"
# A keyword a verb does not take is refused, where it used to be accepted and
# ignored: PUSH did not honour DRYRUN, and a dry run of it pushed for real.
$PKG LIST ROOT "$M" NAME x MACHINE > "$T/nt.o" 2>&1
[ $? -eq 20 ] && has "$T/nt.o" '^reason: NAME is not a word LIST takes; it takes ROOT$'
                                                      ok $? "a keyword the verb does not take is refused, naming what it takes"
$PKG PUSH CHANNEL "$CH" TO http://127.0.0.1:9 DRYRUN MACHINE > "$T/pd.o" 2>&1
[ $? -eq 20 ] && has "$T/pd.o" 'DRYRUN is not a word PUSH takes'
                                                      ok $? "PUSH DRYRUN is refused before anything is sent: PUSH has no dry run"
$PKG INSTALL hello ROT "$M" MACHINE > "$T/rot.o" 2>&1
[ $? -eq 20 ] && has "$T/rot.o" 'did you mean ROOT?'
                                                      ok $? "a keyword misspelt in capitals is caught, not installed as a package"
# A slip must never reach the default environment's root, the person's own
# system: before, "INSTALL foo ROTO /tmp/r" took ROTO and /tmp/r as two more
# names and went on in the default root. Checked with a default environment
# registered, and the root it names untouched.
mkdir -p "$T/envcfg/aros-pkg" "$T/sysroot"
printf 'default=sys\n\n[sys]\nroot=%s\n' "$T/sysroot" > "$T/envcfg/aros-pkg/environments.conf"
XDG_CONFIG_HOME="$T/envcfg" $PKG INSTALL hello ROTO "$T/r9" CHANNEL "$CH" > "$T/slip.o" 2>&1
[ $? -eq 20 ] && has "$T/slip.o" 'did you mean ROOT?' && ! has "$T/slip.o" 'Environment:' && [ ! -d "$T/sysroot/.pkg" ]
                                                      ok $? "a misspelt ROOT is refused before any root is chosen, and the default root is untouched"
XDG_CONFIG_HOME="$T/envcfg" $PKG INSTALL hello "$T/r9" CHANNEL "$CH" > "$T/slip2.o" 2>&1
[ $? -eq 20 ] && has "$T/slip2.o" 'is not a package name' && [ ! -d "$T/sysroot/.pkg" ]
                                                      ok $? "a path where a package name belongs is refused, not installed as a name"
$PKG INSTALL Hello ROOT "$M" MACHINE > "$T/case.o" 2>&1
[ $? -eq 20 ] && has "$T/case.o" 'did you mean hello?'
                                                      ok $? "a name in capitals is answered with its lower case"
$PKG LIST ROOT "" MACHINE > "$T/empty.o" 2>&1
[ $? -eq 20 ] && has "$T/empty.o" 'ROOT is given an empty value'
                                                      ok $? "an empty value is refused, instead of an empty root"
$PKG '?' > "$T/qq.o" 2>&1
[ $? -eq 0 ] && has "$T/qq.o" '^INSTALL  *PACKAGE/M/A,ROOT/K'
                                                      ok $? "pkg ? lists every verb's template, as a C: command answers ?"
$PKG INSTALL ? > "$T/q.o" 2>&1
[ $? -eq 0 ] && has "$T/q.o" 'template: PACKAGE/M/A,ROOT/K'
                                                      ok $? "INSTALL ? answers with the template, as an AmigaDOS command does"
printf 'edited\n' > "$M/C/Hello"
mrun x5 VERIFY hello ROOT "$M" MACHINE
[ $? -eq 12 ] && has "$T/x5.o" '^result: damaged$' && has "$T/x5.o" '^changed: C/Hello$'
                                                      ok $? "a damaged install exits 12 and names the file"
mrun mx REMOVE hello ROOT "$M" MACHINE
has "$T/mx.o" '^result: removed$' && has "$T/mx.o" '^kept: C/Hello$'
                                                      ok $? "remove reports the edited file it kept"

echo "agent_safety"

# What an agent needs from the tool itself: a next step on every refusal, no
# ready-made command that overrides a safeguard, suggestions instead of
# guesses, dry runs that write nothing, and retries that are harmless.
A="$T/aroot"
$PKG INSTALL hello VERSION 1.2 ROOT "$A" CHANNEL "$CH" > /dev/null 2>&1
$PKG UPGRADE hello ROOT "$A" CHANNEL "$CH" MACHINE > "$T/k14" 2>&1
[ $? -eq 14 ] && has "$T/k14" '^next: ask-requester$' && has "$T/k14" "^signer: $EVILPUB\$" \
    && has "$T/k14" "^pinned: $PUB\$";                ok $? "a key refusal: next ask-requester, pinned and signer as their own fields"
! has "$T/k14" "ACCEPTKEY $EVILPUB";                  ok $? "and no ACCEPTKEY with the key filled in to paste"
$PKG UPGRADE hello ROOT "$A" CHANNEL "$CH" > "$T/k14h" 2>&1
has "$T/k14h" 'next: ask whoever requested this';                ok $? "the human refusal ends with the same next step"
$PKG INSTALL hello VERSION 1.2 ROOT "$A" CHANNEL "$CH" MACHINE > "$T/again" 2>&1
[ $? -eq 0 ] && has "$T/again" '^result: unchanged$'; ok $? "a retried INSTALL of the same version succeeds, unchanged"
$PKG INSTALL hello VERSION 1.3 ROOT "$A" CHANNEL "$CH" MACHINE > "$T/other" 2>&1
[ $? -eq 15 ] && has "$T/other" '^next: use-upgrade$'; ok $? "INSTALL of a newer version than installed: 15, next use-upgrade"
$PKG UPGRADE nosuch ROOT "$A" CHANNEL "$CH" MACHINE > "$T/ni" 2>&1
[ $? -eq 11 ] && has "$T/ni" '^next: use-install$';   ok $? "UPGRADE of what is not installed: 11, next use-install"
$PKG INSTALL hell ROOT "$A" CHANNEL "$CH" MACHINE > "$T/near" 2>&1
[ $? -eq 11 ] && has "$T/near" 'did you mean hello' && has "$T/near" '^next: check-name$'
                                                      ok $? "a near name is suggested, not guessed"
$PKG INSTALL hello --root "$A" MACHINE > "$T/dash" 2>&1
[ $? -eq 20 ] && has "$T/dash" 'perhaps ROOT' && has "$T/dash" '^next: fix-command$'
                                                      ok $? "--root is answered with the Pkg spelling, ROOT"
cp -R "$CH" "$T/chx"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$T/chx/objects/$payload.pkg"
$PKG INSTALL hello VERSION 1.2 ROOT "$T/xroot" CHANNEL "$T/chx" MACHINE > "$T/x12" 2>&1
[ $? -eq 12 ] && has "$T/x12" '^next: stop$';          ok $? "an integrity refusal says stop"

$PKG INSTALL hello VERSION 1.3 ROOT "$T/droot" CHANNEL "$CH" DRYRUN MACHINE > "$T/di" 2>&1
[ $? -eq 0 ] && has "$T/di" '^result: would-install$' && [ ! -e "$T/droot" ]
                                                      ok $? "INSTALL DRYRUN says would-install and creates nothing"
before=$(cat "$CH/index")
mkdir -p "$T/v15/C"; printf 'binary 5\000$VER: Hello 1.5 (1.10.2026)\000' > "$T/v15/C/Hello"
$PKG PUBLISH "$T/v15" CHANNEL "$CH" KIND application DRYRUN MACHINE > "$T/dp" 2>&1
[ $? -eq 0 ] && has "$T/dp" '^result: would-publish$' && has "$T/dp" '^depends: none$' \
    && has "$T/dp" '^version: 1.5$' && [ "$(cat "$CH/index")" = "$before" ]
                                                      ok $? "PUBLISH DRYRUN shows name, version, no dependencies, and leaves the channel as it was"
$PKG REMOVE hello ROOT "$A" DRYRUN MACHINE > "$T/dr" 2>&1
[ $? -eq 0 ] && has "$T/dr" '^result: would-remove$' && [ -f "$A/C/Hello" ] && [ -f "$A/.pkg/db/hello" ]
                                                      ok $? "REMOVE DRYRUN says would-remove and removes nothing"

echo "first_trust"
# A fresh root trusts no key for hello. The channel's first hello was signed
# by PUB; the highest, 1.4, by EVIL. Trusting EVIL is the person's decision.
$PKG INSTALL hello ROOT "$T/fresh" CHANNEL "$CH" MACHINE > "$T/ft" 2>&1
[ $? -eq 14 ] && has "$T/ft" '^next: ask-requester$' && has "$T/ft" "^first-signer: $PUB\$" \
    && [ ! -e "$T/fresh/C/Hello" ];                  ok $? "a first install signed by another key than the first version: 14, nothing placed"
$PKG INSTALL hello VERSION 1.2 ROOT "$T/fresh" CHANNEL "$CH" MACHINE > "$T/ft2" 2>&1
                                                      ok $? "the version signed by the first version's key installs"
$PKG INSTALL hello ROOT "$T/fresh2" CHANNEL "$CH" ACCEPTKEY "$EVILPUB" MACHINE > "$T/ft3" 2>&1
[ $? -eq 0 ] && has "$T/ft3" '^version: 1.4$';        ok $? "and the newer key once ACCEPTKEY names it"
$PKG SHOW hello CHANNEL "$CH" MACHINE > "$T/fts" 2>&1
has "$T/fts" '^warning: hello is signed by more than one key';
                                                      ok $? "SHOW warns that hello has more than one signer"
env -u PKG_SIGNKEY $PKG PUBLISH "$D" CHANNEL "$T/nokeych" KIND application MACHINE > "$T/nk" 2>&1
[ $? -eq 14 ] && has "$T/nk" '^next: ask-requester$' && has "$T/nk" 'same key';
                                                      ok $? "no signing key: the refusal says to find the publisher's key, not make one"
$PKG KEYINFO FILE "$KEY" MACHINE > "$T/ki" 2>&1
[ $? -eq 0 ] && has "$T/ki" "^public: $PUB\$" && ! has "$T/ki" 'Seed';
                                                      ok $? "KEYINFO names the public key a key file holds, and nothing else"

echo "trace"
TR="$T/troot"
$PKG INSTALL hello VERSION 1.2 ROOT "$TR" CHANNEL "$CH" MACHINE TRACE "$T/trace.log" > "$T/tro" 2>"$T/tre"
[ $? -eq 0 ] && ! LC_ALL=C grep -a -v -E '^[a-z][a-z-]*: ' "$T/tro" > /dev/null && [ ! -s "$T/tre" ]
                                                      ok $? "TRACE leaves stdout the pure MACHINE contract, and stderr empty"
has "$T/trace.log" "picked hello 1.2 from $CH: the version asked for" && has "$T/trace.log" 'signature verifies, signer' \
    && has "$T/trace.log" "move .* -> $TR/C/Hello"; ok $? "the trace tells the choice, the checks and each file moved"
PKG_TRACE="$T/env.log" $PKG INSTALL hello VERSION 1.2 ROOT "$T/xr2" CHANNEL "$T/chx" > /dev/null 2>&1
has "$T/env.log" 'refused, integrity (12), next stop';  ok $? "PKG_TRACE works too, and a refusal is traced with its class"
$PKG LIST ROOT "$TR" TRACE > "$T/tru" 2>&1
[ $? -eq 20 ];                                        ok $? "TRACE without a file is a usage error"

echo "guidance"
$PKG PUBLISH "$D" CHANNEL "$T/gch" MACHINE > "$T/g1" 2>&1
[ $? -eq 20 ] && has "$T/g1" 'no KIND given' && has "$T/g1" 'KIND image' && has "$T/g1" '^next: fix-command$' \
    && [ ! -e "$T/gch" ];                             ok $? "PUBLISH without KIND is refused with the kinds listed, and creates nothing"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND handler MACHINE > "$T/g2" 2>&1
[ $? -eq 20 ] && has "$T/g2" 'the kind for that is device'; ok $? "KIND handler is refused with the kind to use"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application DRYRUN MACHINE > "$T/g3" 2>&1
has "$T/g3" '^hint: there is no channel at .* publishing creates it' && [ ! -e "$T/gch" ]
                                                      ok $? "a dry run into a missing channel says it would create it"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application MACHINE > "$T/g4" 2>&1
has "$T/g4" '^hint: the channel .* did not exist and was created'; ok $? "and the publish says it did"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application MACHINE > "$T/g5" 2>&1
! has "$T/g5" '^hint: the channel';                   ok $? "no such hint once the channel exists"
mkdir -p "$T/gscript/S"; printf 'Echo hi\n' > "$T/gscript/S/Go"
$PKG PUBLISH "$T/gscript" CHANNEL "$T/gch" NAME go VERSION 1 KIND application DRYRUN MACHINE > "$T/g6" 2>&1
has "$T/g6" '^warning: no executable in the drawer'; ok $? "an application with no executable is warned about"
$PKG PUBLISH "$T/gscript" CHANNEL "$T/gch" NAME go VERSION 1 KIND data DRYRUN MACHINE > "$T/g7" 2>&1
! has "$T/g7" '^warning:';                            ok $? "data with no executable is not"
$PKG PUBLISH "$D" CHANNEL "$T/gch" NAME himg VERSION 1 KIND image DRYRUN MACHINE > "$T/g8" 2>&1
has "$T/g8" '^content: C/Hello [0-9]' && has "$T/g8" '^file: himg.hdf ';
                                                      ok $? "an image dry run lists the files that go into the image"
$PKG KEYGEN FILE "$T/gkey" MACHINE > "$T/g9" 2>&1
has "$T/g9" '^hint: every later version .* signed with it'; ok $? "KEYGEN says to keep the key"
$PKG PUBLISH "$D" CHANNEL "$T/gch" NAME himg VERSION 1 KIND image > /dev/null 2>&1
$PKG INSTALL himg ROOT "$T/groot" CHANNEL "$T/gch" MACHINE > "$T/g10" 2>&1
has "$T/g10" '^hint: to run it, mount the image: MOUNTLIST himg'; ok $? "installing an image says how to run it"
$PKG MOUNTLIST himg ROOT "$T/groot" MACHINE > "$T/g11" 2>&1
has "$T/g11" '^hint: Mount reads the entry from a file' && has "$T/g11" '^hint: no FFS handler';
                                                      ok $? "MOUNTLIST without OUT says to save it, and warns of the missing handler"

echo "wrong_program"
# A game drawer holding another program by mistake: its cookie names that program.
mkdir -p "$T/game/C" "$T/wch"
# an x86_64 ELF header, then the cookie of hello: another program
python3 -c "
import sys
b = bytearray(64); b[0:4] = b'\\x7fELF'; b[4] = 2; b[5] = 1; b[18] = 62
open(sys.argv[1], 'wb').write(bytes(b) + b'\\x00\$VER: hello 1.2 (18.9.2026)\\x00')
" "$T/game/C/Asteroids"
$PKG PUBLISH "$T/game" CHANNEL "$T/wch" KIND image NAME asteroids VERSION 1.0 DRYRUN MACHINE > "$T/w1" 2>&1
has "$T/w1" '^warning: C/Asteroids carries the \$VER cookie of hello'
                                                      ok $? "an executable whose cookie names another program is warned about"
$PKG PUBLISH "$D" CHANNEL "$T/wch" KIND application MACHINE > /dev/null 2>&1
$PKG PUBLISH "$T/game" CHANNEL "$T/wch" KIND image VERSION 9 MACHINE > "$T/w2" 2>&1
[ $? -eq 20 ] && has "$T/w2" 'comes from the \$VER cookie in C/Asteroids' && has "$T/w2" 'add NAME' \
    && [ "$(grep -c . "$T/wch/index")" = 1 ]; ok $? "a cookie name that would replace a package of another kind is refused"
$PKG PUBLISH "$D" CHANNEL "$T/wch" KIND application NAME hello2 VERSION 1 DRYRUN MACHINE > "$T/w3" 2>&1
! has "$T/w3" '^warning: C/Hello carries';            ok $? "a cookie that matches its file name raises nothing"
$PKG SHOW nosuch CHANNEL "$T/wch" MACHINE > "$T/w4" 2>&1
[ $? -eq 0 ] && has "$T/w4" '^hint: no package is published as nosuch'; ok $? "SHOW of a name not published says so"

echo "next_version"
# A teammate's 2.1 that is the published 2.0 with a byte added, its $VER not raised.
NV="$T/nv"; mkdir -p "$NV/v2/C" "$NV/v21/C"
printf 'bin\000$VER: Tool 2.0 (1.1.2026)\000' > "$NV/v2/C/Tool"; printf 'data\n' > "$NV/v2/C/Tool.cfg"
cp -R "$NV/v2/C" "$NV/v21/"; printf 'x' >> "$NV/v21/C/Tool"
$PKG PUBLISH "$NV/v2" CHANNEL "$NV/ch" KIND image MACHINE > /dev/null 2>&1
grep -q '^Content: [0-9a-f]* [0-9]* C/Tool$' "$NV/ch/objects/"*.manifest
                                                      ok $? "an image's manifest lists the files inside it"
env -u PKG_SIGNKEY $PKG PUBLISH "$NV/v21" CHANNEL "$NV/ch" KIND image VERSION 2.1 DRYRUN MACHINE > "$T/n1" 2>&1
[ $? -eq 0 ] && has "$T/n1" '^result: would-publish$' && has "$T/n1" '^signer: none$' \
    && has "$T/n1" "^first-signer: $PUB\$" && has "$T/n1" '^hint: the real publish must be signed with the key that signed tool 2.0'
                                                      ok $? "a dry run needs no key, and names the key the real publish needs"
has "$T/n1" '^compared-with: tool 2.0$' && has "$T/n1" '^changed: C/Tool [0-9]* [0-9]*$' && has "$T/n1" '^same: C/Tool.cfg$'
                                                      ok $? "and compares the new version with the last, file by file"
has "$T/n1" '^warning: the \$VER cookie in C/Tool says 2.0, the version already published' \
    && ! has "$T/n1" '^version-from:' && has "$T/n1" '^name-from: C/Tool$'
                                                      ok $? "a \$VER not raised is named as such; the version is not claimed to come from it"
$PKG PUBLISH "$NV/v2" CHANNEL "$NV/ch" KIND image VERSION 2.2 DRYRUN MACHINE > "$T/n2" 2>&1
has "$T/n2" '^warning: every file is identical to tool 2.0'; ok $? "a new version with nothing changed is warned about"
$PKG INSTALL tool ROOT "$NV/r" CHANNEL "$NV/ch" > /dev/null 2>&1
$PKG PUBLISH "$NV/v21" CHANNEL "$NV/ch" KIND image VERSION 2.1 > /dev/null 2>&1
$PKG UPGRADE tool ROOT "$NV/r" CHANNEL "$NV/ch" MACHINE > "$T/n3" 2>&1
has "$T/n3" '^hint: the image tool.hdf is replaced: a machine that has it mounted must Eject it'
                                                      ok $? "UPGRADE of an image says to eject it and mount it again"

echo "moved_by_hand"
# The Amiga tradition: an installed drawer moved elsewhere by the person.
MV="$T/mv"; mkdir -p "$MV/d/Tool/C"
printf 'x\000$VER: tool 1.0 (1.1.2026)\000' > "$MV/d/Tool/C/Tool"; printf 'doc' > "$MV/d/Tool/ReadMe"
$PKG PUBLISH "$MV/d" CHANNEL "$MV/ch" KIND application > /dev/null 2>&1
$PKG INSTALL tool ROOT "$MV/r" CHANNEL "$MV/ch" > /dev/null 2>&1
mkdir -p "$MV/r/Work"; mv "$MV/r/Tool" "$MV/r/Work/"
$PKG VERIFY tool ROOT "$MV/r" MACHINE > "$T/mv1" 2>&1
[ $? -eq 0 ] && has "$T/mv1" '^result: moved$' && has "$T/mv1" '^moved: Tool/C/Tool Work/Tool/C/Tool$' \
    && ! has "$T/mv1" '^missing:';                   ok $? "VERIFY reports a hand-moved drawer as moved, not damaged"
$PKG LIST ROOT "$MV/r" MACHINE | grep -q '^package: tool 1.0 '; ok $? "and the package stays listed"
printf 'y' >> "$MV/r/Work/Tool/ReadMe"
$PKG VERIFY tool ROOT "$MV/r" MACHINE > "$T/mv2" 2>&1
[ $? -eq 12 ] && has "$T/mv2" '^moved: Tool/C/Tool' && has "$T/mv2" '^missing: Tool/ReadMe$'
                                                      ok $? "a moved file that was also changed is not taken for the package's"
$PKG REMOVE tool ROOT "$MV/r" > /dev/null 2>&1
[ -f "$MV/r/Work/Tool/C/Tool" ];                      ok $? "REMOVE leaves the moved files where the person put them"

echo "inherit"
# A new version is the same package: kind and dependencies come from the last one.
IH="$T/ih"; mkdir -p "$IH/l/Libs" "$IH/a/C" "$IH/b/C"
printf 'l' > "$IH/l/Libs/h.library"
printf 'x\000$VER: tool 1.0 (1.1.2026)\000' > "$IH/a/C/Tool"; printf 'x\000$VER: tool 1.1 (1.1.2026)\000' > "$IH/b/C/Tool"
$PKG PUBLISH "$IH/l" CHANNEL "$IH/ch" NAME h VERSION 1 KIND library > /dev/null 2>&1
$PKG PUBLISH "$IH/a" CHANNEL "$IH/ch" KIND image DEPENDS "h >= 1" > /dev/null 2>&1
$PKG PUBLISH "$IH/b" CHANNEL "$IH/ch" DRYRUN MACHINE > "$T/ih1" 2>&1
[ $? -eq 0 ] && has "$T/ih1" '^kind: image$' && has "$T/ih1" '^depends: h >= 1$' \
    && has "$T/ih1" '^kind-from: tool 1.0$' && has "$T/ih1" '^depends-from: tool 1.0$' && ! has "$T/ih1" '^warning:.*DEPENDS'
                                                      ok $? "a new version takes kind and dependencies from the last one, and says so"
$PKG PUBLISH "$IH/b" CHANNEL "$IH/ch" DEPENDS none DRYRUN MACHINE > "$T/ih2" 2>&1
has "$T/ih2" '^depends: none$' && has "$T/ih2" '^warning: tool 1.0 depends on h, and this version does not' \
    && ! has "$T/ih2" '^depends-from:';               ok $? "DEPENDS none drops them, with the warning"
$PKG PUBLISH "$IH/b" CHANNEL "$IH/ch" NAME tool KIND application DRYRUN MACHINE > "$T/ih3" 2>&1
has "$T/ih3" '^kind: application$' && has "$T/ih3" '^warning: tool 1.0 was published as kind image' \
    && ! has "$T/ih3" '^kind-from:';                  ok $? "KIND given wins, with the change warned about"

echo "amiga_attributes"
# Protection and comment travel from a drawer's .ameta (and, for owner
# Execute, the host mode) into the signed manifest, and out again at install.
AT="$T/at"; mkdir -p "$AT/d/C" "$AT/d/S"
printf 'x\000$VER: tool 1.0 (1.1.2026)\000' > "$AT/d/C/Tool"; chmod 755 "$AT/d/C/Tool"
printf 'Echo hi\n' > "$AT/d/S/Go"; chmod 644 "$AT/d/S/Go"
printf 'doc' > "$AT/d/ReadMe"; chmod 644 "$AT/d/ReadMe"
printf 'ameta 1\nfile Go\nprot 0x00000041\ncomment Starts%%20the%%20tool%%20%%C3%%A9\n' > "$AT/d/S/.ameta"
$PKG PUBLISH "$AT/d" CHANNEL "$AT/ch" KIND application MACHINE > "$T/at1" 2>&1
AM=$(ls "$AT/ch/objects/"*.manifest)
[ $? -eq 0 ] && grep -q '^Protect: 0x00000043 S/Go$' $AM && grep -q '^Comment: Starts%20the%20tool%20%C3%A9 S/Go$' $AM \
    && grep -q '^Protect: 0x00000002 ReadMe$' $AM && ! grep -q 'Protect: .* C/Tool' $AM && ! has "$T/at1" 'left-out: S/.ameta'
                                                      ok $? "the manifest carries .ameta's bits and comment, owner Execute from the host mode"
$PKG INSTALL tool ROOT "$AT/r" CHANNEL "$AT/ch" > /dev/null 2>&1
printf 'ameta 1\nfile Go\nprot 0x00000043\ncomment Starts%%20the%%20tool%%20%%C3%%A9\n' > "$T/at.want"
cmp -s "$AT/r/S/.ameta" "$T/at.want" && [ -x "$AT/r/C/Tool" ] && [ ! -x "$AT/r/ReadMe" ] && [ ! -e "$AT/r/.ameta" ]
                                                      ok $? "INSTALL into a host root writes .ameta and the host mode, nothing where all is default"
$PKG REMOVE tool ROOT "$AT/r" > /dev/null 2>&1
[ ! -e "$AT/r/S" ];                                   ok $? "REMOVE takes the entries out, and the empty drawer with them"
$PKG PUBLISH "$AT/d" CHANNEL "$AT/chi" NAME timg VERSION 1 KIND image > /dev/null 2>&1
$PKG INSTALL timg ROOT "$AT/ri" CHANNEL "$AT/chi" > /dev/null 2>&1
python3 - "$AT/ri/timg.hdf" <<'PYEOF'
import struct, sys
img = open(sys.argv[1], 'rb').read()
for off in range(0, len(img), 512):
    b = img[off:off + 512]
    L = lambda i: struct.unpack('>I', b[4 * i:4 * i + 4])[0]
    n = b[432]; name = b[433:433 + n]
    if L(0) == 2 and L(127) == 0xFFFFFFFD and name == b'Go':
        c = b[328]; comment = b[329:329 + c]
        sys.exit(0 if L(80) == 0x43 and comment == 'Starts the tool \u00e9'.encode('latin-1') else 1)
sys.exit(2)
PYEOF
                                                      ok $? "an image's file headers carry the word and the comment, in Latin-1"
cp -R "$AT/d" "$AT/bad"
printf 'ameta 1\nfile Go\nprot 0xZZ\n' > "$AT/bad/S/.ameta"
$PKG PUBLISH "$AT/bad" CHANNEL "$AT/chb" KIND application MACHINE > "$T/at2" 2>&1
[ $? -eq 20 ] && has "$T/at2" 'S/.ameta, line 3: bad-number';  ok $? "a malformed .ameta line refuses the publish, naming it"
printf 'ameta 1\nfile Gone\ncomment x\n' > "$AT/bad/S/.ameta"
$PKG PUBLISH "$AT/bad" CHANNEL "$AT/chb" KIND application MACHINE > "$T/at3" 2>&1
[ $? -eq 20 ] && has "$T/at3" 'names Gone, which is not in the drawer'; ok $? "so does a stale entry"
printf 'ameta 1\nfile Go\ncomment %s\n' "$(printf 'x%.0s' $(seq 1 80))" > "$AT/bad/S/.ameta"
$PKG PUBLISH "$AT/bad" CHANNEL "$AT/chb" KIND application MACHINE > "$T/at4" 2>&1
[ $? -eq 20 ] && has "$T/at4" 'longer than 79 characters';   ok $? "a comment AROS would cut is refused"
printf 'ameta 1\nfile Go\ncomment %%E2%%82%%AC\n' > "$AT/bad/S/.ameta"
$PKG PUBLISH "$AT/bad" CHANNEL "$AT/chb" KIND application MACHINE > "$T/at5" 2>&1
[ $? -eq 20 ] && has "$T/at5" 'outside Latin-1';               ok $? "and one with a character outside Latin-1"
printf 'ameta 1\nfile Go\nuid 501\n' > "$AT/bad/S/.ameta"
$PKG PUBLISH "$AT/bad" CHANNEL "$AT/chb" KIND application DRYRUN MACHINE > "$T/at6" 2>&1
[ $? -eq 0 ] && has "$T/at6" '^warning: S/.ameta gives an owner'; ok $? "an owner is warned about, not carried"

echo "from_archive"
# A package whose files stay in someone else's archive, as a nightly contrib
# archive: the manifest names archive!/path, the channel keeps the archive.
FA="$T/fa"; mkdir -p "$FA/src/Top/Extras/App/C" "$FA/src/Top/Prefs/Env-Archive/SYS/Packages" "$FA/src/Top/Extras/Other"
printf 'x\000$VER: app 2.1 (1.1.2026)\000' > "$FA/src/Top/Extras/App/C/App"; chmod 755 "$FA/src/Top/Extras/App/C/App"
printf 'doc' > "$FA/src/Top/Extras/App/ReadMe"
printf 'Extras:App\n' > "$FA/src/Top/Prefs/Env-Archive/SYS/Packages/App"
printf 'other' > "$FA/src/Top/Extras/Other/Thing"
(cd "$FA/src" && COPYFILE_DISABLE=1 tar -cjf "$FA/nightly.tar.bz2" Top)
mkdir -p "$FA/ch/archives" && cp "$FA/nightly.tar.bz2" "$FA/ch/archives/"
$PKG PUBLISH "$FA/nightly.tar.bz2!/Top" FILES "Extras/App,Prefs/Env-Archive/SYS/Packages/App" CHANNEL "$FA/ch" \
    NAME app VERSION 2.1+20260918 KIND application MACHINE > "$T/fa1" 2>&1
FM=$(ls "$FA/ch/objects/"*.manifest 2>/dev/null)
[ $? -eq 0 ] && grep -q '^Source: nightly.tar.bz2!/Top$' $FM && ! grep -q '^Payload:' $FM \
    && [ "$(grep -c '^File: ' $FM)" = 3 ] && ! ls "$FA/ch/objects/" | grep -q '\.pkg$'
                                                      ok $? "PUBLISH archive!/path writes a signed manifest naming the archive, and no payload"
grep -q '^Protect: 0x00000002 Extras/App/ReadMe$' $FM && ! grep -q 'Protect: .* Extras/App/C/App' $FM
                                                      ok $? "owner Execute comes from the archive's mode bits"
$PKG INSTALL app ROOT "$FA/r" CHANNEL "$FA/ch" MACHINE > "$T/fa2" 2>&1
[ $? -eq 0 ] && cmp -s "$FA/r/Extras/App/C/App" "$FA/src/Top/Extras/App/C/App" \
    && [ -f "$FA/r/Prefs/Env-Archive/SYS/Packages/App" ] && [ ! -e "$FA/r/Extras/Other" ]
                                                      ok $? "INSTALL takes exactly those files out of the archive"
$PKG VERIFY app ROOT "$FA/r" | grep -q 'all intact';  ok $? "and VERIFY finds them intact"
! has "$T/fa1" 'warning: VERSION';                   ok $? "a nightly build suffix (+20260918) does not contradict the \$VER cookie"
$PKG PUBLISH "$FA/nightly.tar.bz2!/Top" FILES "Extras/App" CHANNEL "$FA/ch2" NAME app VERSION 1 KIND application MACHINE > "$T/fa3" 2>&1
[ $? -eq 20 ] && has "$T/fa3" 'put the archive there';  ok $? "publishing into a channel that lacks the archive is refused, saying where it goes"
printf 'tampered' > "$FA/src/Top/Extras/App/ReadMe"
(cd "$FA/src" && COPYFILE_DISABLE=1 tar -cjf "$FA/ch/archives/nightly.tar.bz2" Top)
$PKG INSTALL app ROOT "$FA/r2" CHANNEL "$FA/ch" MACHINE > "$T/fa4" 2>&1
[ $? -eq 12 ] && [ ! -e "$FA/r2/Extras/App/ReadMe" ]; ok $? "an archive whose files changed since publishing is refused, nothing placed"
# Nightly builds: the version is the program's $VER plus the build; a build
# whose files equal the last version's is not published; publishing reads the
# archive's index after the first time.
cp "$FA/nightly.tar.bz2" "$FA/ch/archives/nightly.tar.bz2"
$PKG PUBLISH "$FA/ch/archives/nightly.tar.bz2!/Top" FILES "Extras/Other" CHANNEL "$FA/ch" NAME other BUILD 20260918 \
    KIND data MACHINE > "$T/fb1" 2>&1
has "$T/fb1" '^version: 0+20260918$' && [ -f "$FA/ch/archives/nightly.tar.bz2.pkgidx" ]
                                                      ok $? "BUILD with no \$VER gives 0+build, and the archive's index is kept beside it"
$PKG PUBLISH "$FA/ch/archives/nightly.tar.bz2!/Top" FILES "Extras/App,Prefs/Env-Archive/SYS/Packages/App" CHANNEL "$FA/ch" \
    NAME app BUILD 20260920 MACHINE > "$T/fb2" 2>&1
[ $? -eq 0 ] && has "$T/fb2" '^result: unchanged$' && has "$T/fb2" '^version: 2.1+20260920$' && has "$T/fb2" '^same-as: app 2.1+20260918$' \
    && ! grep -q ' 2.1+20260920 ' "$FA/ch/index";       ok $? "a nightly whose files equal the last version's publishes nothing"
python3 -c "
import sys
p=sys.argv[1]; s=open(p).read().split(chr(10)); s[1]=s[1].replace(' ', ' X', 0); open(p,'w').write(chr(10).join(s))
" "$FA/ch/archives/nightly.tar.bz2.pkgidx"
touch -t 202001010000 "$FA/ch/archives/nightly.tar.bz2"
$PKG PUBLISH "$FA/ch/archives/nightly.tar.bz2!/Top" FILES "Extras/Other" CHANNEL "$FA/ch" NAME other2 BUILD 1 KIND data \
    TRACE "$T/fb3.trace" MACHINE > /dev/null 2>&1
grep -q 'indexing ' "$T/fb3.trace";                   ok $? "an archive changed since its index was made is indexed again"
# SHOW checks each archive once for all its entries; METADATA reads no
# archive and says so; ARCHIVE keeps only that archive's entries.
$PKG SHOW CHANNEL "$FA/ch" TRACE "$T/fs1.trace" MACHINE > "$T/fs1" 2>&1
[ "$(grep -c 'in one read' "$T/fs1.trace")" = 1 ] && has "$T/fs1" '^bad: 0$'
                                                      ok $? "SHOW reads the archive once for every entry drawing on it"
$PKG SHOW CHANNEL "$FA/ch" METADATA MACHINE > "$T/fs2" 2>&1
[ $? -eq 0 ] && has "$T/fs2" '^archive: app 2.1+20260918 unchecked$' && ! grep -q 'in one read' "$T/fs2"
                                                      ok $? "METADATA checks signatures only, and marks archive entries unchecked"
cp "$FA/ch/archives/nightly.tar.bz2" "$T/fs.keep"
mkdir -p "$T/fsx/Top/Extras/App/C" && printf 'x\000$VER: app 2.1 (1.1.2026)\000!' > "$T/fsx/Top/Extras/App/C/App"
(cd "$T/fsx" && tar -cjf "$FA/ch/archives/nightly.tar.bz2" Top)
$PKG PUBLISH "$D" CHANNEL "$FA/ch" KIND application > /dev/null 2>&1   # a package with a payload of its own
$PKG SHOW CHANNEL "$FA/ch" ARCHIVE nightly.tar.bz2 MACHINE > "$T/fs3" 2>&1
[ $? -eq 12 ] && has "$T/fs3" '^problem: app .*holds a different Top/Extras/App/C/App' \
    && has "$T/fs3" '^entry: other2 ' && ! has "$T/fs3" '^entry: hello '
                                                      ok $? "ARCHIVE checks only that archive's entries, and names the file that differs"
cp "$T/fs.keep" "$FA/ch/archives/nightly.tar.bz2"
$PKG SHOW CHANNEL "$FA/ch" ARCHIVE other.tar.bz2 MACHINE > "$T/fs4" 2>&1
has "$T/fs4" '^count: 0$';                            ok $? "and another archive's name selects none of them"
rm "$FA/ch/archives/nightly.tar.bz2"
$PKG INSTALL app ROOT "$FA/r3" CHANNEL "$FA/ch" MACHINE > "$T/fa5" 2>&1
[ $? -eq 11 ] && has "$T/fa5" 'which the channel does not have'; ok $? "an archive missing from the channel is said so"

echo "clean_input"
# What a person types or edits by hand: spaces around a value go, a line
# break or tab inside is refused naming where, a hand-edited index with CRLF
# and no final newline is read, and every record stays one line.
CI="$T/ci"; mkdir -p "$CI/d/C"
printf 'x\000$VER: tidy 1.0 (1.1.2026)\000' > "$CI/d/C/Tidy"
$PKG PUBLISH "$CI/d" CHANNEL "$CI/ch" KIND "application " MACHINE > "$T/ci1" 2>&1
[ $? -eq 0 ] && has "$T/ci1" '^result: published$';  ok $? "a value with a trailing space is taken without it"
$PKG PUBLISH "$CI/d" CHANNEL "$CI/ch" KIND application NAME "$(printf 'ti\ndy')" MACHINE > "$T/ci2" 2>&1
[ $? -eq 20 ] && has "$T/ci2" 'NAME holds a line break at character 3';  ok $? "a line break inside a value is refused, naming the keyword and where"
$PKG PUBLISH "$CI/d" CHANNEL "$CI/ch" KIND application DEPENDS "$(printf 'a,\tb')" MACHINE > "$T/ci3" 2>&1
[ $? -eq 20 ] && has "$T/ci3" 'DEPENDS holds a tab';  ok $? "and so is a tab"
python3 -c "
import sys
p=sys.argv[1]; b=open(p,'rb').read().replace(b'\n', b'\r\n').rstrip(b'\r\n'); open(p,'wb').write(b)
" "$CI/ch/index"
$PKG SHOW CHANNEL "$CI/ch" MACHINE > "$T/ci4" 2>&1
[ $? -eq 0 ] && has "$T/ci4" '^entry: tidy 1.0 ';   ok $? "a hand-edited index with CRLF and no last newline is read"
$PKG INSTALL tidy ROOT "$CI/r" CHANNEL "$CI/ch" > /dev/null 2>&1
$PKG KEYGEN FILE "$CI/k2" > /dev/null
mkdir -p "$CI/d2/C"; printf 'x\000$VER: tidy 2.0 (1.1.2026)\000' > "$CI/d2/C/Tidy"
PKG_SIGNKEY="$CI/k2" $PKG PUBLISH "$CI/d2" CHANNEL "$CI/ch" ACCEPTKEY "$(awk '/^Public:/{print $2}' "$CI/k2")" > /dev/null 2>&1
$PKG UPGRADE tidy ROOT "$CI/r" CHANNEL "$CI/ch" MACHINE > "$T/ci5" 2>&1
[ $? -eq 14 ] && ! grep -v -E '^[a-z][a-z-]*: ' "$T/ci5" | grep -q .
                                                      ok $? "a refusal whose reason spans lines is still one record per line"

echo "files_of_a_tree"
# One package out of a larger tree, as a system image's folders become packages.
mkdir -p "$T/tree/C" "$T/tree/Libs" "$T/tree/Fonts"
printf 'x\000$VER: dir 1.0 (1.1.2026)\000' > "$T/tree/C/Dir"; printf 'l' > "$T/tree/Libs/a.library"; printf f > "$T/tree/Fonts/x.font"
$PKG PUBLISH "$T/tree" FILES "C,Libs" CHANNEL "$T/treech" NAME base VERSION 1 KIND application MACHINE > "$T/tr1" 2>&1
TM=$(ls "$T/treech/objects/"*.manifest)
[ $? -eq 0 ] && grep -q ' C/Dir$' $TM && grep -q ' Libs/a.library$' $TM && ! grep -q 'Fonts' $TM
                                                      ok $? "FILES takes only the paths it names out of a directory tree"

echo "config_files"
# A package declares the files people edit; an upgrade keeps their edit and
# sets the new version down beside it.
CF="$T/cf"; mkdir -p "$CF/d1/S" "$CF/d1/Prefs/Env-Archive" "$CF/d1/C" "$CF/d2/S" "$CF/d2/Prefs/Env-Archive" "$CF/d2/C"
printf 'x\000$VER: sys 1.0 (1.1.2026)\000' > "$CF/d1/C/Sys"
printf 'x\000$VER: sys 1.1 (1.1.2026)\000' > "$CF/d2/C/Sys"
printf 'Assign Old: SYS:\n' > "$CF/d1/S/Startup-Sequence"; printf 'Assign New: SYS:\n' > "$CF/d2/S/Startup-Sequence"
printf 'screen 1\n' > "$CF/d1/Prefs/Env-Archive/screenmode.prefs"; cp "$CF/d1/Prefs/Env-Archive/screenmode.prefs" "$CF/d2/Prefs/Env-Archive/"
$PKG PUBLISH "$CF/d1" CHANNEL "$CF/ch" KIND application CONFIG "S/Startup-Sequence, Prefs/Env-Archive/" MACHINE > "$T/cf1" 2>&1
[ $? -eq 0 ] && grep -q '^config-files: 2$' "$T/cf1" && grep -q '^Config: S/Startup-Sequence$' "$CF/ch/objects/"*.manifest
                                                      ok $? "CONFIG marks a file and a folder's files in the signed manifest"
$PKG PUBLISH "$CF/d2" CHANNEL "$CF/ch" MACHINE > "$T/cf2" 2>&1
[ $? -eq 0 ] && grep -q '^config-from: sys 1.0$' "$T/cf2" && grep -q '^config-files: 2$' "$T/cf2"
                                                      ok $? "a new version inherits its configuration files"
$PKG PUBLISH "$CF/d1" CHANNEL "$CF/ch2" KIND application CONFIG "S/Startup-Sequnce" MACHINE > "$T/cf3" 2>&1
[ $? -eq 20 ] && grep -q 'no file or folder of this package' "$T/cf3"
                                                      ok $? "a CONFIG name that matches nothing is refused, saying why"
$PKG INSTALL sys VERSION 1.0 ROOT "$CF/r" CHANNEL "$CF/ch" > /dev/null 2>&1
printf 'Assign Old: SYS:\nAssign Mine: Work:\n' > "$CF/r/S/Startup-Sequence"
printf 'screen 2\n' > "$CF/r/Prefs/Env-Archive/screenmode.prefs"
$PKG UPGRADE sys ROOT "$CF/r" CHANNEL "$CF/ch" MACHINE > "$T/cf4" 2>&1
[ $? -eq 0 ] && grep -q '^version: 1.1$' "$T/cf4" && grep -q 'Assign Mine' "$CF/r/S/Startup-Sequence" \
  && cmp -s "$CF/r/S/Startup-Sequence.pkgnew" "$CF/d2/S/Startup-Sequence" && cmp -s "$CF/r/C/Sys" "$CF/d2/C/Sys"
                                                      ok $? "an edited configuration file is kept, the new one set beside it, the rest upgraded"
grep -q '^config-new: S/Startup-Sequence.pkgnew$' "$T/cf4" && grep -q '^config-kept: Prefs/Env-Archive/screenmode.prefs$' "$T/cf4" \
  && [ ! -e "$CF/r/Prefs/Env-Archive/screenmode.prefs.pkgnew" ] && grep -q 'screen 2' "$CF/r/Prefs/Env-Archive/screenmode.prefs"
                                                      ok $? "an edit of a file the new version leaves unchanged is kept, with nothing beside it"
mkdir -p "$CF/r2/S"; printf 'mine\n' > "$CF/r2/S/Startup-Sequence"
$PKG INSTALL sys ROOT "$CF/r2" CHANNEL "$CF/ch" MACHINE > "$T/cf5" 2>&1
[ $? -eq 0 ] && grep -q mine "$CF/r2/S/Startup-Sequence" && [ -f "$CF/r2/S/Startup-Sequence.pkgnew" ]
                                                      ok $? "a first install keeps a configuration file already there"
$PKG PUBLISH "$CF/d1" CHANNEL "$CF/ch3" KIND application MACHINE > /dev/null 2>&1
$PKG INSTALL sys ROOT "$CF/r3" CHANNEL "$CF/ch3" > /dev/null 2>&1
printf 'edit\n' > "$CF/r3/S/Startup-Sequence"
$PKG PUBLISH "$CF/d2" CHANNEL "$CF/ch3" MACHINE > /dev/null 2>&1
$PKG UPGRADE sys ROOT "$CF/r3" CHANNEL "$CF/ch3" MACHINE > "$T/cf6" 2>&1
[ $? -eq 15 ] && grep -q 'declares it with CONFIG' "$T/cf6"
                                                      ok $? "without CONFIG an edit still stops the upgrade, and the refusal names CONFIG"
LN="$T/ln"; mkdir -p "$LN/d1/S" "$LN/d2/S"
printf 'a\n' > "$LN/d1/S/averylongconfigname12.prefs"; printf 'b\n' > "$LN/d2/S/averylongconfigname12.prefs"
$PKG PUBLISH "$LN/d1" CHANNEL "$LN/ch" NAME lng VERSION 1 KIND data CONFIG S > /dev/null 2>&1
$PKG PUBLISH "$LN/d2" CHANNEL "$LN/ch" NAME lng VERSION 2 > /dev/null 2>&1
$PKG INSTALL lng VERSION 1 ROOT "$LN/r" CHANNEL "$LN/ch" > /dev/null 2>&1
printf 'mine\n' > "$LN/r/S/averylongconfigname12.prefs"
$PKG UPGRADE lng ROOT "$LN/r" CHANNEL "$LN/ch" MACHINE > "$T/ln1" 2>&1
[ $? -eq 0 ] && grep -q '^config-new: S/averylongconfigname12.p.pkgnew$' "$T/ln1" && [ -f "$LN/r/S/averylongconfigname12.p.pkgnew" ]
                                                      ok $? "a .pkgnew name is shortened to the 30 characters an FFS name may have"

echo "adopt"
# An AROS set up with InstallAROS: the files are there, no package owns them.
AD="$T/ad"; mkdir -p "$AD/d1/C" "$AD/d1/Libs" "$AD/d2/C" "$AD/d2/Libs" "$AD/o/Libs"
printf 'x\000$VER: base 1.0 (1.1.2026)\000' > "$AD/d1/C/Base"; printf 'lib1' > "$AD/d1/Libs/b.library"
printf 'x\000$VER: base 1.1 (1.1.2026)\000' > "$AD/d2/C/Base"; printf 'lib2' > "$AD/d2/Libs/b.library"
printf 'lib1' > "$AD/o/Libs/b.library"
$PKG PUBLISH "$AD/d1" CHANNEL "$AD/ch" KIND application > /dev/null 2>&1
$PKG PUBLISH "$AD/d2" CHANNEL "$AD/ch" > /dev/null 2>&1
$PKG PUBLISH "$AD/o" CHANNEL "$AD/ch" NAME other VERSION 1 KIND library > /dev/null 2>&1
mkdir -p "$AD/r"; cp -R "$AD/d1/" "$AD/r/"
$PKG INSTALL base VERSION 1.0 ROOT "$AD/r" CHANNEL "$AD/ch" MACHINE > "$T/ad1" 2>&1
[ $? -eq 0 ] && grep -q '^adopted: 2$' "$T/ad1"; ok $? "files already there, identical to the package's, are adopted"
$PKG UPGRADE base ROOT "$AD/r" CHANNEL "$AD/ch" > /dev/null 2>&1 && $PKG ROLLBACK base ROOT "$AD/r" CHANNEL "$AD/ch" > /dev/null 2>&1
[ $? -eq 0 ] && cmp -s "$AD/r/C/Base" "$AD/d1/C/Base" && cmp -s "$AD/r/Libs/b.library" "$AD/d1/Libs/b.library"
                                                      ok $? "an adopted system upgrades and rolls back"
$PKG INSTALL other ROOT "$AD/r" CHANNEL "$AD/ch" MACHINE > "$T/ad2" 2>&1
[ $? -eq 15 ] && grep -q 'belongs to base, which is installed' "$T/ad2"
                                                      ok $? "a file another installed package owns is never adopted"
ino() { ls -i "$1" | awk '{print $1}'; }
mkdir -p "$AD/r3"; cp -R "$AD/d1/" "$AD/r3/"; i1=$(ino "$AD/r3/Libs/b.library")
$PKG INSTALL base VERSION 1.0 ROOT "$AD/r3" CHANNEL "$AD/ch" > /dev/null 2>&1
[ "$(ino "$AD/r3/Libs/b.library")" = "$i1" ] && [ -z "$(ls "$AD/r3/.pkg/staging/base" 2>/dev/null)" ]
                                                      ok $? "an adopted file is left where it is, not written again"
mkdir -p "$AD/d3/C" "$AD/d3/Libs"; cp "$AD/d2/C/Base" "$AD/d3/C/Base"; printf 'lib3' > "$AD/d3/Libs/b.library"
printf 'x\000$VER: base 1.2 (1.1.2026)\000' > "$AD/d3/C/Base"; cp "$AD/d1/Libs/b.library" "$AD/d3/Libs/b.library"
$PKG PUBLISH "$AD/d3" CHANNEL "$AD/ch" > /dev/null 2>&1
$PKG UPGRADE base VERSION 1.2 ROOT "$AD/r3" CHANNEL "$AD/ch" MACHINE > "$T/ad5" 2>&1
[ $? -eq 0 ] && grep -q '^unchanged-files: 1$' "$T/ad5" && [ "$(ino "$AD/r3/Libs/b.library")" = "$i1" ] && cmp -s "$AD/r3/C/Base" "$AD/d3/C/Base"
                                                      ok $? "an upgrade writes only the files that changed"
mkdir -p "$AD/r2/C"; printf 'not the same' > "$AD/r2/C/Base"
$PKG INSTALL base ROOT "$AD/r2" CHANNEL "$AD/ch" MACHINE > "$T/ad3" 2>&1
[ $? -eq 15 ] && grep -q 'not the same' "$AD/r2/C/Base"; ok $? "a different file already there is still refused, and left alone"

echo "remove pkg itself"
# Pkg removes its own package, and says what stays behind: the guide
# docs/removing.md turns that into a procedure.
RM="$T/rmpkg"; mkdir -p "$RM/d/C" "$RM/o/Libs"
printf 'x\000$VER: Pkg 1.5 (1.1.2026)\000' > "$RM/d/C/Pkg"; printf 'lib' > "$RM/o/Libs/o.library"
$PKG PUBLISH "$RM/d" CHANNEL "$RM/ch" NAME pkg VERSION 1.5 KIND application > /dev/null 2>&1
$PKG PUBLISH "$RM/o" CHANNEL "$RM/ch" NAME other VERSION 1 KIND library > /dev/null 2>&1
$PKG INSTALL pkg ROOT "$RM/r" CHANNEL "$RM/ch" > /dev/null 2>&1
$PKG INSTALL other ROOT "$RM/r" CHANNEL "$RM/ch" > /dev/null 2>&1
$PKG REMOVE pkg ROOT "$RM/r" > "$T/rm1" 2>&1
[ $? -eq 0 ] && grep -q 'hint: pkg is gone, but .*\.pkg still holds' "$T/rm1" && [ ! -e "$RM/r/C/Pkg" ]
                                                      ok $? "REMOVE pkg takes the file out and says what .pkg still holds"
[ -f "$RM/r/.pkg/keys/pkg" ] && [ ! -e "$RM/r/.pkg/db/pkg" ] && [ -f "$RM/r/.pkg/db/other" ] \
  && [ -f "$RM/r/Libs/o.library" ]
                                                      ok $? "the pinned key outlives the package; the other package's record and files stay"
$PKG REMOVE other ROOT "$RM/r" > "$T/rm2" 2>&1
[ $? -eq 0 ] && ! grep -q 'hint: pkg is gone' "$T/rm2"
                                                      ok $? "control: removing any other package says nothing of the kind"
$PKG INSTALL pkg ROOT "$RM/r" CHANNEL "$RM/ch" MACHINE > "$T/rm3" 2>&1
[ $? -eq 0 ] && grep -q '^hint: pkg is gone' "$T/rm3" 2>/dev/null; rc=$?
$PKG REMOVE pkg ROOT "$RM/r" MACHINE > "$T/rm4" 2>&1
[ $? -eq 0 ] && grep -q '^hint: pkg is gone, but ' "$T/rm4" && [ "$rc" -ne 0 ]
                                                      ok $? "MACHINE carries it as a hint: record, and only on the removal"

echo "repair"
RP="$T/rp"; mkdir -p "$RP/d/C" "$RP/d/Libs" "$RP/d/S"
printf 'x\000$VER: sysr 1.0 (1.1.2026)\000' > "$RP/d/C/Sysr"; printf 'lib' > "$RP/d/Libs/r.library"; printf 'seq\n' > "$RP/d/S/Startup-Sequence"
$PKG PUBLISH "$RP/d" CHANNEL "$RP/ch" KIND application CONFIG S/Startup-Sequence > /dev/null 2>&1
$PKG INSTALL sysr ROOT "$RP/r" CHANNEL "$RP/ch" > /dev/null 2>&1
rm "$RP/r/C/Sysr"; printf 'broken' > "$RP/r/Libs/r.library"; printf 'mine\n' > "$RP/r/S/Startup-Sequence"
$PKG VERIFY ALL ROOT "$RP/r" MACHINE > "$T/rp1" 2>&1
[ $? -eq 12 ] && has "$T/rp1" '^missing: sysr C/Sysr$' && has "$T/rp1" '^changed: sysr Libs/r.library$' \
  && has "$T/rp1" '^edited: sysr S/Startup-Sequence$'; ok $? "VERIFY ALL names each damaged file with its package, an edited configuration apart"
$PKG REPAIR ALL ROOT "$RP/r" CHANNEL "$RP/ch" MACHINE > "$T/rp2" 2>&1
[ $? -eq 0 ] && cmp -s "$RP/r/C/Sysr" "$RP/d/C/Sysr" && cmp -s "$RP/r/Libs/r.library" "$RP/d/Libs/r.library" \
  && grep -q broken "$RP/r/Libs/r.library.pkgold" && grep -q mine "$RP/r/S/Startup-Sequence"
                                                      ok $? "REPAIR puts files back, keeps a change as .pkgold, leaves the configuration edit"
$PKG VERIFY ALL ROOT "$RP/r" MACHINE > "$T/rp3" 2>&1
[ $? -eq 0 ] && has "$T/rp3" '^result: intact$';     ok $? "and VERIFY ALL then finds the system intact"
$PKG REPAIR ALL ROOT "$RP/r" CHANNEL "$RP/ch" MACHINE > "$T/rp4" 2>&1
[ $? -eq 0 ] && has "$T/rp4" '^result: unchanged$';  ok $? "a second REPAIR has nothing to do"
printf 'again' > "$RP/r/Libs/r.library"
$PKG REPAIR sysr ROOT "$RP/r" CHANNEL "$RP/ch" MACHINE > "$T/rp5" 2>&1
grep -q again "$RP/r/Libs/r.library" && grep -q broken "$RP/r/Libs/r.library.pkgold"
                                                      ok $? "an earlier .pkgold is never overwritten: the file is left as it is"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]

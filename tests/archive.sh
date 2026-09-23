#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The archive reader against archives the system's own tar and bzip2 write:
# ustar prefixes, GNU long names, pax paths, several bzip2 streams back to
# back, a plain tar; and refused when truncated or damaged. With
# PKG_NIGHTLY_CONTRIB set to a nightly contrib .tar.bz2, also its whole
# listing against tar's.

set -u
export COPYFILE_DISABLE=1   # macOS tar would add AppleDouble "._" members
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-archive.XXXXXX")
trap 'rm -rf "$T"' EXIT
R=./build/test_archive
checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}

mkdir -p "$T/src/Top/Extras/App/C" "$T/src/Top/$(printf 'd%.0s' $(seq 1 60))/$(printf 'e%.0s' $(seq 1 60))"
printf 'binary' > "$T/src/Top/Extras/App/C/App"; chmod 755 "$T/src/Top/Extras/App/C/App"
printf 'readme' > "$T/src/Top/Extras/App/ReadMe"; chmod 644 "$T/src/Top/Extras/App/ReadMe"
long="$T/src/Top/$(printf 'd%.0s' $(seq 1 60))/$(printf 'e%.0s' $(seq 1 60))/$(printf 'f%.0s' $(seq 1 120))"
printf 'long' > "$long"
dd if=/dev/urandom of="$T/src/Top/big" bs=1024 count=3000 2>/dev/null
(cd "$T/src" && tar --format=ustar -cf "$T/u.tar" Top/Extras) 2>/dev/null
(cd "$T/src" && tar --format=pax -cf "$T/p.tar" Top)
(cd "$T/src" && tar --format=gnutar -cf "$T/g.tar" Top) 2>/dev/null || cp "$T/p.tar" "$T/g.tar"
for f in u p g; do bzip2 -k "$T/$f.tar"; done

(cd "$T/src" && find Top -type f | sort) > "$T/want"
$R "$T/p.tar.bz2" | awk '{print $3}' | sort > "$T/got"
cmp -s "$T/want" "$T/got";                           ok $? "a pax .tar.bz2 lists every file, long names included"
$R "$T/g.tar.bz2" | awk '{print $3}' | sort > "$T/got"
cmp -s "$T/want" "$T/got";                           ok $? "so does a GNU tar"
$R "$T/u.tar" | grep -q '^6 755 Top/Extras/App/C/App$'; ok $? "a plain ustar tar lists size, mode and path"
$R "$T/p.tar.bz2" "${long#$T/src/}" | cmp -s - "$long"; ok $? "a file with a long pax name comes out byte for byte"
$R "$T/p.tar.bz2" Top/big | cmp -s - "$T/src/Top/big";  ok $? "a 3 MB file spanning bzip2 blocks comes out byte for byte"

# Two bzip2 streams back to back, cut in the middle of the tar, as parallel
# compressors write them.
half=$(( $(wc -c < "$T/p.tar") / 2 ))
head -c "$half" "$T/p.tar" | bzip2 > "$T/c.tar.bz2"
tail -c "+$((half + 1))" "$T/p.tar" | bzip2 >> "$T/c.tar.bz2"
$R "$T/c.tar.bz2" Top/big | cmp -s - "$T/src/Top/big";  ok $? "concatenated bzip2 streams read as one"

head -c 200000 "$T/p.tar.bz2" > "$T/t.tar.bz2"
$R "$T/t.tar.bz2" > /dev/null 2> "$T/e1"
[ $? -eq 1 ] && grep -q 'refused' "$T/e1";           ok $? "a truncated archive is refused"
cp "$T/p.tar.bz2" "$T/d.tar.bz2"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0x55; open(p,'wb').write(b)
" "$T/d.tar.bz2"
$R "$T/d.tar.bz2" > /dev/null 2> "$T/e2"
[ $? -eq 1 ] && grep -q 'damaged\|checksum' "$T/e2"; ok $? "a damaged bzip2 stream is refused"

# A PAX length with enough decimal digits to overflow size_t must not read
# beyond the header buffer; the next ordinary member still parses.
python3 - "$T/overflow.tar" <<'PYTAR'
import io, sys, tarfile
body = b"0" * 65520 + b"100 path=x\n" + b" " * 5
with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as archive:
    pax = tarfile.TarInfo("pax")
    pax.type = tarfile.XHDTYPE
    pax.size = len(body)
    archive.addfile(pax, io.BytesIO(body))
    member = tarfile.TarInfo("safe")
    member.size = 1
    archive.addfile(member, io.BytesIO(b"x"))
PYTAR
$R "$T/overflow.tar" | grep -q '^1 .* safe$';       ok $? "an overflowing pax length stays within its header"

if [ -n "${PKG_NIGHTLY_CONTRIB:-}" ]; then
    $R "$PKG_NIGHTLY_CONTRIB" | awk '{print $1, $3}' | sort > "$T/n.mine"
    tar -tvjf "$PKG_NIGHTLY_CONTRIB" | grep -v '^d' | awk '{print $5, $NF}' | sort > "$T/n.tar"
    cmp -s "$T/n.mine" "$T/n.tar";                   ok $? "the nightly contrib archive lists as tar lists it"
fi

echo "archive: $checks checks, $fails failures"
[ "$fails" -eq 0 ]

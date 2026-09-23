<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signatures and trust

What is signed, by whom, what is checked where, what a key change means for
you, and how to check a package yourself without pkg.

## What is signed

A package version is described by its **manifest**: name, version, CPU,
kind, dependencies, and every file with its size and SHA-256. The publisher
signs the manifest with an Ed25519 key; the signature is a small file beside
it, `<digest>.sig`:

```
Signer: 43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077
Signature: db6a2def66e78c15ceccbf022c74953d93b72bf8c15fa1055ef903e5d7355d47...
```

The files themselves are not signed one by one; they do not need to be.
The manifest lists each file's SHA-256, the signature covers the manifest,
and pkg refuses any file whose bytes do not hash to what the manifest says.
So a valid signature on the manifest vouches for every byte of the package.

The channel's `index` names each version by the SHA-256 of its manifest, and
the payload container by the SHA-256 of its bytes. Nothing in a channel is
ever rewritten except the index: a version, once published, is immutable.

## Who signs

**The publisher, and nobody else.** A publisher makes a key once
([`KEYGEN`](commands/keygen.md)), keeps the secret half with their secrets,
and signs every version they publish with it. The public half, 64 hex
digits, is the publisher's identity: it is what `SHOW` prints under *Signer*
and what the portal shows on a package page.

The portal holds no signing key. It cannot sign a package, alter one, or
sign a replacement: it can only refuse to publish. The same is true of
anyone who copies or mirrors a channel: the files are what the publisher
signed, or pkg refuses them.

Nobody vouches for who a publisher *is*. The key is the identity; a name
beside it on the portal is a label its maintainers attached when they gave
that publisher a push key. Two keys sign what the portal serves today. If it matters to you that a
package comes from where it says, compare the key with these:

| Key | Publisher | Signs |
|---|---|---|
| `43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077` | JKN, the author of pkg | the `pkg` channel: pkg itself, from 1.1 |
| `a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7` | aros-development-team | the `contrib-nightly` channel |

The portal's [publisher pages](https://aros-pkg.azurewebsites.net/publishers)
show the same keys and everything each has signed.

## What is checked, and where

**On your machine, by pkg, at every install, upgrade, verify and repair:**

- the signature of the manifest, under the key the `.sig` names (refused
  with exit 13 when absent or invalid);
- every file of the package against the size and SHA-256 in the manifest
  (exit 12 when one differs), before anything is written;
- that the signer is the key **pinned** for this package on this machine
  (exit 14 when it is not; see below).

**On the portal, before a push is published:** the portal runs pkg itself
on the uploaded channel (`SHOW ... METADATA`), so the same signature and
digest checks apply; then two rules of its own: a package keeps the key of
its first version (a push signed by another key is refused), and a
published file never changes (a push that would replace one is refused).

**On the way:** nothing. A channel may travel over plain `http`, a USB
stick or a shared drawer; the connection proves nothing and does not have
to. This is why a channel read from a USB stick is as safe as one read over
`https`.

## Key pinning, and what a key change means for you

The first version of a package you install pins its signer's key in your
root, for that package. From then on:

- a version signed by the same key installs, upgrades and repairs as usual;
- a version signed by **another** key is refused (exit 14), whoever
  published it and wherever it comes from, and pkg prints both keys;
- `REPAIR` puts files back only from a version signed by the pinned key.

You can also pin the key before the first install: `KEY <public key>` on
`INSTALL`, `UPGRADE` or `ROLLBACK` names the publisher you expect, and pkg
refuses the package if another key signed it. A dependency with no pin yet is
not covered by that `KEY`: install it first with its own
([INSTALL](commands/install.md)).

That refusal is the point of the system: it is what stops a channel or a
mirror from replacing a program under you. It is also what you see when a
publisher legitimately changes their key. Only you can tell the two apart,
by asking the publisher through a channel you trust (their web page, their
repository, a person you know). If the new key is theirs, accept it once:

```
pkg UPGRADE hello ROOT SYS: CHANNEL DEPOT:channel ACCEPTKEY <the new key>
```

pkg never accepts a new key by itself, and never prints a command with the
new key filled in for you to paste; an assistant that drives pkg is told
the same ([pkg with an AI assistant](agents.md)).

Pre-release builds of pkg (0.3 to 0.5) were signed by the aros-development-team
key and are no longer in the `pkg` channel; pkg 1.1 was its first version. A
test machine that still has one of those builds sees the refusal above on its
next `UPGRADE pkg` and accepts the JKN key with `ACCEPTKEY`. A fresh install
needs nothing: the channel's first version and its newest carry the same key.

## When a publisher loses a key

There is no recovery: a lost secret key cannot sign again, and the portal
cannot re-sign anything. The publisher makes a new key, announces its
public half where their users can see it, and publishes the next version
with it; the portal's maintainers replace the old key with the new one in
the push permissions of that publisher's channels (the first-key rule is
applied per package, so this needs the maintainers, not a trick). Every
machine that has the package sees exit 14 once and accepts the new key
with `ACCEPTKEY`. Nothing already installed is affected.

Back your key up. `KEYGEN` writes one file, readable by you alone; a copy
in a password manager or on an encrypted disk is enough.

## The installers

`curl ... | sh` and `install.ps1` run before pkg exists on the machine, so
they cannot use pkg to check what they download. They fetch
`Bootstrap/SHA256SUMS` and `Bootstrap/SHA256SUMS.sig` from the channel and
verify the signature with OpenSSH, which every macOS, Linux and Windows
has, against the channel owner's key embedded in the script; then they
check the binary's SHA-256 against its line. A machine without
`ssh-keygen -Y` refuses to install, unless `PKG_SKIP_VERIFY=1` says to go
on with a warning. The same check by hand:

```sh
B=https://aros-pkg.azurewebsites.net/pkg
curl -fsSO $B/Bootstrap/SHA256SUMS && curl -fsSO $B/Bootstrap/SHA256SUMS.sig
printf 'jkn ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEPFUJZ7wY3+x886fNASl9CUUP02Sg402OWK72I87zB3\n' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I jkn -n aros-pkg-bootstrap -s SHA256SUMS.sig < SHA256SUMS
curl -fsSO $B/Bootstrap/linux-x86_64/pkg && mkdir -p Bootstrap/linux-x86_64 && mv pkg Bootstrap/linux-x86_64/
grep linux-x86_64 SHA256SUMS | shasum -a 256 -c -
```

```
Good "aros-pkg-bootstrap" signature for jkn with ED25519 key SHA256:6iGtIJqhQKmOGWvUH+AN3RaNLBoPloyMk82VCfmz//o
Bootstrap/linux-x86_64/pkg: OK
```

The `ssh-ed25519` line is JKN's key as the portal's trust page prints it;
the same key in pkg's own hex form is `43c55096...3077` above.

`pkg SIGN <file> KEY <keyfile> OUT <sig> SSH NAMESPACE <ns>` writes such a
signature and `pkg KEYINFO FILE <keyfile> SSH` prints the key in
`ssh-ed25519` form, so your own channel's bootstraps can be signed the
same way ([`SIGN`](commands/sign.md), [`KEYINFO`](commands/keyinfo.md)).

## Checking a package by hand

pkg does this at every install, and `SHOW` does it for a whole channel:

```console
$ pkg SHOW CHANNEL https://aros-pkg.azurewebsites.net/pkg
Package  Version  Kind         Arch     Status  Signer
pkg      1.7.0    application  aarch64  ok      5ff18d3fe14e383e
pkg      1.7.0    application  x86_64   ok      5ff18d3fe14e383e
```

To check without trusting pkg at all, three files and one script suffice.
`tools/verify-manifest.py` in the repository is plain Python 3 with no
module beyond the standard library; it implements the Ed25519 check from
RFC 8032 in sixty readable lines, so you can read what it does:

```sh
B=https://aros-pkg.azurewebsites.net/pkg
curl -fsSO $B/index
d=$(awk '$1=="pkg" && $2=="1.1" && $3=="x86_64" {print $4}' index)   # the manifest's digest
curl -fsSO $B/objects/$d.manifest
curl -fsSO $B/objects/$d.sig
python3 tools/verify-manifest.py $d.manifest $d.sig 43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077
```

```
digest:    the manifest is the one its name says, sha256 5ffcf631a2769496
signature: valid, made by 43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077
signer:    the expected key
result:    the manifest is what its publisher signed
```

Change one byte of the manifest and it says `do not trust this package`.
From there, the manifest's `File:` lines give the SHA-256 of every file;
`shasum -a 256` on an installed file, or on a file taken out of the `.pkg`
container, completes the check.

## Related

[`KEYGEN`](commands/keygen.md), [`KEYINFO`](commands/keyinfo.md),
[`SIGN`](commands/sign.md) for a detached signature on any file,
[Publishing packages](publishing.md#your-key), [Channels](channels.md),
[the container](container.md) for the byte layout of a `.pkg`.

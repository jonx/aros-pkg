/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Oracles written by others, long before this code: FIPS 180 examples for
 * SHA-512, and the first three Ed25519 test vectors of RFC 8032, section 7.1.
 * A vector checks the public key derived from the seed, the exact signature
 * bytes (Ed25519 is deterministic), and that the signature verifies.
 */

#include "pkg_ed25519.h"
#include "pkg_sha512.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
static void ok(int c, const char *what) { checks++; if (!c) { failures++; printf("  FAIL %s\n", what); } }

static void unhex(unsigned char *out, const char *hex)
{
    size_t i, n = strlen(hex) / 2;
    for (i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static void sha512_vectors(void)
{
    unsigned char d[64], want[64];
    printf("sha512_vectors\n");
    pkg_sha512(d, "", 0);
    unhex(want, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    ok(memcmp(d, want, 64) == 0, "empty");
    pkg_sha512(d, "abc", 3);
    unhex(want, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    ok(memcmp(d, want, 64) == 0, "abc");
}

struct vec { const char *seed, *pk, *msg, *sig; };

static const struct vec rfc8032[] = {
    { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" }
};

static void rfc8032_vectors(void)
{
    size_t v;
    printf("rfc8032_vectors\n");
    for (v = 0; v < sizeof rfc8032 / sizeof rfc8032[0]; v++) {
        unsigned char seed[32], pk[32], sk[64], want_pk[32], msg[8], sig[64], want_sig[64];
        size_t mlen = strlen(rfc8032[v].msg) / 2;
        unhex(seed, rfc8032[v].seed);
        unhex(want_pk, rfc8032[v].pk);
        unhex(msg, rfc8032[v].msg);
        unhex(want_sig, rfc8032[v].sig);
        pkg_ed25519_keypair(pk, sk, seed);
        ok(memcmp(pk, want_pk, 32) == 0, "public key from seed");
        pkg_ed25519_sign(sig, msg, mlen, sk);
        ok(memcmp(sig, want_sig, 64) == 0, "signature bytes");
        ok(pkg_ed25519_verify(want_sig, msg, mlen, want_pk) == 0, "RFC signature verifies");
    }
}

static void forgeries_are_refused(void)
{
    unsigned char seed[32], pk[32], sk[64], sig[64], bad[64], pk2[32], sk2[64];
    unsigned char msg[] = "Payload: 0123456789abcdef";
    size_t n = sizeof msg - 1;
    int i;

    printf("forgeries_are_refused\n");
    for (i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 1);
    pkg_ed25519_keypair(pk, sk, seed);
    pkg_ed25519_sign(sig, msg, n, sk);
    ok(pkg_ed25519_verify(sig, msg, n, pk) == 0, "own signature verifies");

    msg[n - 1] ^= 1;
    ok(pkg_ed25519_verify(sig, msg, n, pk) != 0, "one message bit changed");
    msg[n - 1] ^= 1;

    memcpy(bad, sig, 64); bad[0] ^= 1;
    ok(pkg_ed25519_verify(bad, msg, n, pk) != 0, "one bit of R changed");
    memcpy(bad, sig, 64); bad[40] ^= 1;
    ok(pkg_ed25519_verify(bad, msg, n, pk) != 0, "one bit of S changed");

    seed[0] ^= 1;
    pkg_ed25519_keypair(pk2, sk2, seed);
    ok(pkg_ed25519_verify(sig, msg, n, pk2) != 0, "another key");

    /* With the identity public key, R=identity and S=0 used to verify any
     * message. Noncanonical encodings of that same point must fail too. */
    {
        unsigned char identity[32] = {1}, noncanonical[32], forged[64] = {1};
        ok(pkg_ed25519_verify(forged, msg, n, identity) != 0,
           "identity-key forgery refused");
        memset(noncanonical, 0xff, sizeof noncanonical);
        noncanonical[0] = 0xee; noncanonical[31] = 0x7f;
        ok(pkg_ed25519_verify(forged, msg, n, noncanonical) != 0,
           "noncanonical identity refused");
    }

    /* S + L is the same scalar modulo L; RFC 8032 requires refusing it. */
    {
        static const unsigned char Lb[32] = {
            0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2,
            0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
        unsigned carry = 0;
        memcpy(bad, sig, 64);
        for (i = 0; i < 32; i++) {
            unsigned s = (unsigned)bad[32 + i] + Lb[i] + carry;
            bad[32 + i] = (unsigned char)s;
            carry = s >> 8;
        }
        ok(carry == 0 && pkg_ed25519_verify(bad, msg, n, pk) != 0,
           "S + L, the malleable twin, refused");
    }
}

int main(void)
{
    sha512_vectors();
    rfc8032_vectors();
    forgeries_are_refused();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

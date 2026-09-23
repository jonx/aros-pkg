/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Field elements are sixteen 16-bit limbs held in int64_t, as in TweetNaCl.
 */

#include "pkg_ed25519.h"
#include "pkg_sha512.h"

#include <stdint.h>
#include <string.h>

typedef unsigned char u8;
typedef int64_t gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
/* The group order, little-endian. */
static const int64_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static void set25519(gf r, const gf a)
{
    int i;
    for (i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o)
{
    int i;
    int64_t c;
    for (i = 0; i < 16; i++) {
        o[i] += 65536;
        c = o[i] >> 16;
        if (i < 15)
            o[i + 1] += c - 1;
        else
            o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t t, c = ~((int64_t)b - 1);
    int i;
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *o, const gf n)
{
    int i, j, b;
    gf m, t;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)((t[i] >> 8) & 0xff);
    }
}

static int vn32(const u8 *x, const u8 *y)
{
    unsigned d = 0;
    int i;
    for (i = 0; i < 32; i++) d |= (unsigned)(x[i] ^ y[i]);
    return (int)((1u & ((d - 1u) >> 8)) - 1u);   /* 0 equal, -1 differ */
}

static int neq25519(const gf a, const gf b)
{
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return vn32(c, d);
}

static u8 par25519(const gf a)
{
    u8 d[32];
    pack25519(d, a);
    return (u8)(d[0] & 1);
}

static void unpack25519(gf o, const u8 *n)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] * 256);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b)
{
    int64_t t[31];
    int i, j;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf in)
{
    gf c;
    int a;
    set25519(c, in);
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, in);
    }
    set25519(o, c);
}

static void pow2523(gf o, const gf in)
{
    gf c;
    int a;
    set25519(c, in);
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, in);
    }
    set25519(o, c);
}

static void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], u8 b)
{
    int i;
    for (i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(u8 *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (u8)(par25519(tx) << 7);
}

static void scalarmult(gf p[4], gf q[4], const u8 *s)
{
    int i;
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (i = 255; i >= 0; --i) {
        u8 b = (u8)((s[i / 8] >> (i & 7)) & 1);
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const u8 *s)
{
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static void modL(u8 *r, int64_t x[64])
{
    int64_t carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (u8)(x[i] & 255);
    }
}

static void reduce(u8 *r)
{
    int64_t x[64];
    int i;
    for (i = 0; i < 64; i++) x[i] = (int64_t)r[i];
    for (i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

static void expand(u8 d[64], const u8 *seed)
{
    pkg_sha512(d, seed, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

void pkg_ed25519_keypair(unsigned char pk[32], unsigned char sk[64],
                         const unsigned char seed[32])
{
    u8 d[64];
    gf p[4];
    expand(d, seed);
    scalarbase(p, d);
    pack(pk, p);
    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);
}

void pkg_ed25519_sign(unsigned char sig[64], const unsigned char *msg, size_t len,
                      const unsigned char sk[64])
{
    u8 d[64], h[64], r[64];
    int64_t x[64];
    gf p[4];
    struct pkg_sha512 c;
    int i, j;

    expand(d, sk);

    pkg_sha512_init(&c);
    pkg_sha512_update(&c, d + 32, 32);
    pkg_sha512_update(&c, msg, len);
    pkg_sha512_final(&c, r);
    reduce(r);
    scalarbase(p, r);
    pack(sig, p);

    pkg_sha512_init(&c);
    pkg_sha512_update(&c, sig, 32);
    pkg_sha512_update(&c, sk + 32, 32);
    pkg_sha512_update(&c, msg, len);
    pkg_sha512_final(&c, h);
    reduce(h);

    for (i = 0; i < 64; i++) x[i] = 0;
    for (i = 0; i < 32; i++) x[i] = (int64_t)r[i];
    for (i = 0; i < 32; i++)
        for (j = 0; j < 32; j++)
            x[i + j] += (int64_t)h[i] * (int64_t)d[j];
    modL(sig + 32, x);
}

static int unpackneg(gf r[4], const u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

/* Canonical encoding and prime-order subgroup membership. unpackneg accepts
 * y values reduced modulo p, and a small-order public key can otherwise
 * verify a signature without anyone knowing a signing secret. */
static int valid_point(gf out[4], const u8 encoded[32], int public_key)
{
    static const u8 prime[32] = {
        0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
    static const u8 identity[32] = {1};
    u8 order[32], packed[32];
    gf q[4], product[4];
    int i;

    for (i = 31; i >= 0; i--) {
        u8 y = i == 31 ? (u8)(encoded[i] & 0x7f) : encoded[i];
        if (y < prime[i]) break;
        if (y > prime[i]) return -1;
    }
    if (i < 0 || unpackneg(out, encoded)) return -1;
    /* Zero x has only the even encoding. */
    if ((encoded[31] & 0x80) && !neq25519(out[0], gf0)) return -1;
    pack(packed, out);
    if (public_key && !vn32(packed, identity)) return -1;
    for (i = 0; i < 32; i++) order[i] = (u8)L[i];
    for (i = 0; i < 4; i++) set25519(q[i], out[i]);
    scalarmult(product, q, order);
    pack(packed, product);
    return vn32(packed, identity) ? -1 : 0;
}

/* S must be canonical: strictly below L. */
static int s_below_L(const u8 *s)
{
    int i;
    for (i = 31; i >= 0; i--) {
        if ((int64_t)s[i] < L[i]) return 1;
        if ((int64_t)s[i] > L[i]) return 0;
    }
    return 0;   /* equal to L */
}

int pkg_ed25519_verify(const unsigned char sig[64], const unsigned char *msg,
                       size_t len, const unsigned char pk[32])
{
    u8 t[32], h[64];
    gf p[4], q[4];
    struct pkg_sha512 c;

    if (!s_below_L(sig + 32)) return -1;
    if (valid_point(q, pk, 1)) return -1;
    if (valid_point(p, sig, 0)) return -1;

    pkg_sha512_init(&c);
    pkg_sha512_update(&c, sig, 32);
    pkg_sha512_update(&c, pk, 32);
    pkg_sha512_update(&c, msg, len);
    pkg_sha512_final(&c, h);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);
    return vn32(sig, t) ? -1 : 0;
}

/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* A tar reader over a byte source that is either the file itself or its
 * bzip2 decompression (third_party/bzip2, BZ_NO_STDIO). Handles ustar names
 * with their prefix, GNU long names ('L') and pax path records ('x'), and
 * streams concatenated bzip2 streams as the parallel compressors write
 * them. Portable C99.
 *
 * The same reader writes a block map while it walks, and reads from the
 * middle of the archive with one. A bzip2 stream is a chain of blocks, each
 * self-contained and starting, at any bit of a byte, with the 48 bits
 * 0x314159265359. The walk looks for those starts in the bytes it is about
 * to hand to libbzip2 and stops handing them over there, so libbzip2 runs
 * dry exactly at a block boundary; the boundary counts only when libbzip2 is
 * then reading that block's header at that very bit, which is what tells a
 * real start from the same 48 bits occurring inside a block. Reading from a
 * block later needs no stream of its own: a four-byte "BZh<level>" goes in
 * front of the block's bits, moved up to a byte, and the read stops before
 * the combined CRC, which such a stream cannot match. Every block's own CRC
 * is still checked by libbzip2. */

#include "pkg_archive.h"
#define BZ_NO_STDIO 1
#include "bzlib.h"
#include "bzlib_private.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libbzip2 without stdio asks the program what to do on an internal error. */
void bz_internal_error(int errcode)
{
    (void)errcode;
    abort();
}

void (*pkg_archive_on_read)(long long done, long long total);

#define INBUF  (1u << 16)
#define MAXCAND 64                      /* block starts noticed in one input buffer */
#define BZ_BLOCK_MAGIC 0x314159265359ull
#define SKIP_AHEAD (1ull << 26)         /* how far a read runs on rather than restart */

struct blockrec {
    unsigned long long bit, ustart;
    unsigned           stream;          /* the stream header it belongs to */
};

struct streamrec {
    unsigned long long start, end;      /* byte offsets; end is where the next stream starts */
    int                level;           /* the digit of "BZh<level>" */
};

/* What the walk writes down: every member's place in the decompressed
 * stream, and every bzip2 block start. */
struct map_build {
    char               *out;            /* the member lines, as text */
    size_t              len, cap;
    struct blockrec    *b;
    size_t              nb, bcap;
    struct streamrec   *st;
    size_t              ns, scap;
    int                 oom;
};

struct src {
    FILE          *f;
    int            bz;          /* 1: bzip2 */
    bz_stream      s;
    int            open;        /* a bzip2 stream is initialised */
    int            eof;
    unsigned char  in[INBUF];
    char          *err;
    size_t         errlen;
    unsigned long long uout;    /* decompressed bytes handed to the caller */
    size_t         in_len, in_pos;
    unsigned long long in_base; /* file offset of in[0] */

    /* mapping, while walking */
    struct map_build  *mb;
    unsigned long long reg;             /* the last eight bytes, for the bit search */
    unsigned long long cand[MAXCAND];
    int                ncand, candi;
    unsigned long long stop_bit;        /* the block start the bytes were stopped at */

    /* reading from the middle */
    int                mid;             /* 1: started at a block, not at the file's start */
    unsigned           shift;           /* bits the stream was moved up by */
    unsigned long long raw_pos, raw_end, fsize;
    unsigned char      prev;            /* the raw byte the next moved one needs */
    int                hdr_left;        /* the made-up "BZh<level>" still to hand over */
    char               hdr[4];
};

static int fail(struct src *r, const char *msg)
{
    if (r->errlen) snprintf(r->err, r->errlen, "%s", msg);
    return -1;
}

/* ---- the block map, while walking ------------------------------------- */

static void mb_put(struct map_build *mb, const char *s)
{
    size_t n = strlen(s);
    if (mb->oom) return;
    if (mb->len + n + 1 > mb->cap) {
        size_t nc = mb->cap ? mb->cap * 2 : 65536;
        char *g;
        while (nc < mb->len + n + 1) nc *= 2;
        g = (char *)realloc(mb->out, nc);
        if (g == NULL) { mb->oom = 1; return; }
        mb->out = g;
        mb->cap = nc;
    }
    memcpy(mb->out + mb->len, s, n + 1);
    mb->len += n;
}

static void mb_block(struct map_build *mb, unsigned long long bit, unsigned long long ustart)
{
    if (mb->oom || mb->ns == 0) return;
    if (mb->nb == mb->bcap) {
        size_t nc = mb->bcap ? mb->bcap * 2 : 256;
        struct blockrec *g = (struct blockrec *)realloc(mb->b, nc * sizeof *g);
        if (g == NULL) { mb->oom = 1; return; }
        mb->b = g;
        mb->bcap = nc;
    }
    mb->b[mb->nb].bit = bit;
    mb->b[mb->nb].ustart = ustart;
    mb->b[mb->nb].stream = (unsigned)(mb->ns - 1);
    mb->nb++;
}

static void mb_stream(struct map_build *mb, unsigned long long start, int level)
{
    if (mb->oom) return;
    if (mb->ns == mb->scap) {
        size_t nc = mb->scap ? mb->scap * 2 : 8;
        struct streamrec *g = (struct streamrec *)realloc(mb->st, nc * sizeof *g);
        if (g == NULL) { mb->oom = 1; return; }
        mb->st = g;
        mb->scap = nc;
    }
    if (mb->ns > 0) mb->st[mb->ns - 1].end = start;
    mb->st[mb->ns].start = start;
    mb->st[mb->ns].end = 0;
    mb->st[mb->ns].level = level;
    mb->ns++;
}

static void mb_free(struct map_build *mb)
{
    free(mb->out);
    free(mb->b);
    free(mb->st);
    free(mb);
}

/* The map's text: a line for each block, then the lines the walk collected
 * for the members. */
static char *mb_finish(struct map_build *mb, unsigned long long fsize)
{
    char *text;
    size_t blen, i, at = 0;
    if (mb->oom || mb->nb == 0 || mb->ns == 0) return NULL;
    mb->st[mb->ns - 1].end = fsize;
    blen = mb->nb * 96u + 1u;
    text = (char *)malloc(blen + mb->len + 1);
    if (text == NULL) return NULL;
    for (i = 0; i < mb->nb; i++) {
        const struct streamrec *st = &mb->st[mb->b[i].stream];
        at += (size_t)snprintf(text + at, blen - at, "b %llu %llu %d %llu\n", mb->b[i].bit,
                               mb->b[i].ustart, st->level, st->end);
    }
    memcpy(text + at, mb->out, mb->len);
    text[at + mb->len] = '\0';
    return text;
}

/* Every bit position in this buffer where the 48-bit block magic could
 * start. A position is only a candidate here; the walk confirms it. */
static void scan(struct src *r, const unsigned char *b, size_t n)
{
    size_t i;
    unsigned long long reg = r->reg;
    r->ncand = r->candi = 0;
    for (i = 0; i < n; i++) {
        int k;
        reg = reg << 8 | b[i];
        for (k = 0; k < 8; k++) {
            if (((reg >> k) & 0xFFFFFFFFFFFFull) == BZ_BLOCK_MAGIC) {
                unsigned long long fo = r->in_base + i;
                unsigned long long p = (fo + 1) * 8ull - 48ull - (unsigned long long)k;
                if (r->ncand < MAXCAND) r->cand[r->ncand++] = p;
            }
        }
    }
    r->reg = reg;
}

/* Bytes handed to libbzip2 so far. */
static unsigned long long fed(const struct src *r)
{
    return r->in_base + r->in_pos;
}

/* libbzip2 has run dry where a block start was thought to be: it is one if
 * the decoder is reading that block's header, at that bit and no other. */
static void confirm(struct src *r)
{
    const DState *d = (const DState *)r->s.state;
    if (d != NULL && d->state >= BZ_X_BLKHDR_1 && d->state <= BZ_X_BLKHDR_6) {
        unsigned long long n = (unsigned long long)(d->state - BZ_X_BLKHDR_1);
        if (fed(r) * 8ull - (unsigned long long)d->bsLive == r->stop_bit + 8ull * n)
            mb_block(r->mb, r->stop_bit, r->uout);
    }
    r->stop_bit = 0;
}

/* Hand libbzip2 the bytes up to the next block start, that start's own
 * straddling byte included, so that it runs dry there and nowhere else. */
static void hand(struct src *r)
{
    size_t upto = r->in_len;
    r->stop_bit = 0;
    while (r->candi < r->ncand) {
        unsigned long long p = r->cand[r->candi];
        unsigned long long by = p / 8 + (p % 8 ? 1 : 0);
        if (by <= r->in_base + r->in_pos) { r->candi++; continue; }
        if (by < r->in_base + r->in_len) { upto = (size_t)(by - r->in_base); r->stop_bit = p; }
        break;
    }
    r->s.next_in = (char *)r->in + r->in_pos;
    r->s.avail_in = (unsigned)(upto - r->in_pos);
    r->in_pos = upto;
}

/* ---- the byte source -------------------------------------------------- */

/* The next input, moved up when the read started inside a stream. 0 bytes at
 * the end of what may be read. */
static size_t refill_mid(struct src *r)
{
    size_t k, i;
    unsigned char raw[INBUF];
    if (r->hdr_left) {
        memcpy(r->in, r->hdr, 4);
        r->hdr_left = 0;
        return 4;
    }
    if (r->raw_pos >= r->raw_end) return 0;
    k = (size_t)(r->raw_end - r->raw_pos < INBUF ? r->raw_end - r->raw_pos : INBUF);
    k = fread(raw, 1, k, r->f);
    if (k == 0) return 0;
    r->raw_pos += k;
    if (r->shift == 0) {
        memcpy(r->in, raw, k);
        return k;
    }
    for (i = 0; i < k; i++) {
        r->in[i] = (unsigned char)(r->prev << r->shift | raw[i] >> (8 - r->shift));
        r->prev = raw[i];
    }
    return k;
}

/* How far through the file the read has got. A walk from the front counts
 * the bytes it has taken in against the file's size; a read of a few members
 * out of the middle has no whole to be a share of, and says so. */
static void report(const struct src *r)
{
    if (pkg_archive_on_read == NULL)
        return;
    if (r->mid)
        pkg_archive_on_read((long long)r->raw_pos, -1);
    else
        pkg_archive_on_read((long long)(r->in_base + r->in_len), (long long)r->fsize);
}

static void refill(struct src *r)
{
    size_t k;
    if (r->mb != NULL && r->in_pos < r->in_len) { hand(r); return; }
    r->in_base += r->in_len;
    k = r->mid ? refill_mid(r) : fread(r->in, 1, INBUF, r->f);
    r->in_len = k;
    r->in_pos = 0;
    if (k == 0) { r->eof = 1; r->s.avail_in = 0; return; }
    report(r);
    if (r->mb != NULL) { scan(r, r->in, k); hand(r); return; }
    r->s.next_in = (char *)r->in;
    r->s.avail_in = (unsigned)k;
    r->in_pos = k;
}

/* A stream built round one block holds no more than that stream: its bits
 * end without the combined CRC they would need, and libbzip2 either refuses
 * them or runs dry. Where a read started inside such a stream and the file
 * carries another after it, carry on there; that one starts on a byte and is
 * read as it stands. 1 when the reader moved on. */
static int cross_stream(struct src *r)
{
    if (!r->mid || r->s.avail_in != 0 || r->raw_pos < r->raw_end || r->raw_end >= r->fsize)
        return 0;
    if (r->open) { BZ2_bzDecompressEnd(&r->s); r->open = 0; }
    if (fseek(r->f, (long)r->raw_end, SEEK_SET) != 0)
        return 0;
    r->shift = 0;
    r->raw_pos = r->raw_end;
    r->raw_end = r->fsize;
    r->in_len = r->in_pos = 0;
    r->eof = 0;
    return 1;
}

/* Fill exactly n bytes; 1 on success, 0 at a clean end before any byte, -1
 * on damage or a short end. */
static int get(struct src *r, unsigned char *out, size_t n)
{
    unsigned long long uout0 = r->uout;
    size_t got = 0;
    if (!r->bz) {
        got = fread(out, 1, n, r->f);
        r->uout += got;
        if (got == n) return 1;
        return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
    }
    while (got < n) {
        int rc;
        if (r->s.avail_in == 0 && !r->eof && r->stop_bit == 0)
            refill(r);
        if (!r->open) {
            if (r->s.avail_in == 0 && r->eof)
                return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
            if (BZ2_bzDecompressInit(&r->s, 0, 0) != BZ_OK)
                return fail(r, "cannot start bzip2 decompression");
            r->open = 1;
            if (r->mb != NULL) {
                int level = r->s.avail_in >= 4 ? r->s.next_in[3] - '0' : 9;
                mb_stream(r->mb, fed(r) - r->s.avail_in, level >= 1 && level <= 9 ? level : 9);
            }
        }
        r->s.next_out = (char *)out + got;
        r->s.avail_out = (unsigned)(n - got);
        rc = BZ2_bzDecompress(&r->s);
        got = n - r->s.avail_out;
        r->uout = uout0 + got;
        if (r->mb != NULL && r->stop_bit != 0 && r->s.avail_in == 0 && r->s.avail_out > 0)
            confirm(r);
        if (rc == BZ_STREAM_END) {
            /* another stream may follow: parallel compressors write several */
            BZ2_bzDecompressEnd(&r->s);
            r->open = 0;
            if (r->s.avail_in == 0 && r->eof && got < n)
                return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
        } else if (rc != BZ_OK) {
            if (cross_stream(r)) continue;
            return fail(r, "the bzip2 data is damaged");
        } else if (r->s.avail_in == 0 && r->eof && r->s.avail_out > 0) {
            if (cross_stream(r)) continue;
            return fail(r, "the bzip2 data ends before its stream does");
        }
    }
    return 1;
}

static unsigned long long octal(const unsigned char *p, size_t n)
{
    unsigned long long v = 0;
    size_t i = 0;
    if (p[0] & 0x80) {                   /* GNU base-256 for large sizes */
        for (i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned)(p[i] - '0');
    return v;
}

static int skip(struct src *r, unsigned long long n)
{
    unsigned char buf[4096];
    while (n > 0) {
        size_t k = n > sizeof buf ? sizeof buf : (size_t)n;
        if (get(r, buf, k) != 1) return r->err[0] ? -1 : fail(r, "the archive is truncated");
        n -= k;
    }
    return 0;
}

/* The value of the "path" record of a pax header block, if any. */
static void pax_path(const unsigned char *b, size_t n, char *out, size_t ol)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && b[j] >= '0' && b[j] <= '9') {
            size_t digit = (size_t)(b[j++] - '0');
            if (len > (SIZE_MAX - digit) / 10) return;
            len = len * 10 + digit;
        }
        if (len == 0 || len > n - i || j >= i + len || b[j] != ' '
            || b[i + len - 1] != '\n') return;
        j++;
        if (i + len - j > 6 && memcmp(b + j, "path=", 5) == 0) {
            size_t vl = i + len - j - 6;   /* without the newline */
            if (vl >= ol) vl = ol - 1;
            memcpy(out, b + j + 5, vl);
            out[vl] = '\0';
        }
        i += len;
    }
}

int pkg_archive_walk_map(const char *file, pkg_archive_want_fn want, pkg_archive_data_fn data,
                         void *ctx, char **map, char *err, size_t errlen)
{
    struct src *r = (struct src *)calloc(1, sizeof *r);
    unsigned char h[512], magic[3];
    char name[4096], longname[4096];
    int rc = -1, zeros = 0;

    if (errlen) err[0] = '\0';
    if (map) *map = NULL;
    if (r == NULL) { if (errlen) snprintf(err, errlen, "out of memory"); return -1; }
    r->err = err;
    r->errlen = errlen;
    r->f = fopen(file, "rb");
    if (r->f == NULL) { fail(r, "cannot open the archive"); free(r); return -1; }
    if (fseek(r->f, 0, SEEK_END) == 0) {              /* the whole a share is of */
        long t = ftell(r->f);
        if (t > 0) r->fsize = (unsigned long long)t;
    }
    rewind(r->f);
    report(r);
    if (fread(magic, 1, 3, r->f) == 3 && magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h')
        r->bz = 1;
    rewind(r->f);
    if (map != NULL && r->bz) {
        r->mb = (struct map_build *)calloc(1, sizeof *r->mb);
        if (r->mb == NULL) {
            fail(r, "out of memory");
            fclose(r->f);
            free(r);
            return -1;
        }
    }
    longname[0] = '\0';
    for (;;) {
        struct pkg_archive_entry e;
        unsigned long long size, pad, at;
        int g, w;
        char type;
        g = get(r, h, sizeof h);
        at = r->uout;                                  /* where this member's data begins */
        if (g == 0) { rc = 0; break; }                 /* no end blocks: accept */
        if (g < 0) break;
        if (h[0] == '\0') {                              /* two zero blocks end it */
            if (++zeros == 2) { rc = 0; break; }
            continue;
        }
        zeros = 0;
        {
            unsigned long long sum = 0, want_sum = octal(h + 148, 8);
            int k;
            for (k = 0; k < 512; k++) sum += (k >= 148 && k < 156) ? 32u : h[k];
            if (sum != want_sum) { fail(r, "a tar header has a bad checksum"); break; }
        }
        type = (char)h[156];
        size = octal(h + 124, 12);
        pad = (512 - size % 512) % 512;
        if (type == 'L' || type == 'x' || type == 'g') {
            unsigned char *b;
            if (size > 65536) { fail(r, "an extended tar header is too long"); break; }
            b = (unsigned char *)malloc((size_t)size + 1);
            if (b == NULL || get(r, b, (size_t)size) != 1 || skip(r, pad) != 0) {
                free(b);
                if (!err[0]) fail(r, "the archive is truncated");
                break;
            }
            b[size] = '\0';
            if (type == 'L') snprintf(longname, sizeof longname, "%s", (const char *)b);
            else if (type == 'x') pax_path(b, (size_t)size, longname, sizeof longname);
            free(b);
            continue;
        }
        if (longname[0]) {
            snprintf(name, sizeof name, "%s", longname);
            longname[0] = '\0';
        } else if (memcmp(h + 257, "ustar", 5) == 0 && h[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        } else {
            snprintf(name, sizeof name, "%.100s", (const char *)h);
        }
        e.path = name;
        e.size = size;
        e.mode = (unsigned)octal(h + 100, 8);
        e.is_dir = type == '5';
        if (type != '0' && type != '\0' && type != '5' && type != '7') {
            /* links, devices and the like: named, never delivered */
            if (skip(r, size + pad) != 0) break;
            continue;
        }
        if (e.is_dir) { size_t l = strlen(name); if (l && name[l - 1] == '/') name[l - 1] = '\0'; }
        if (r->mb != NULL && !e.is_dir && strchr(name, '\n') == NULL) {
            char line[4400];
            snprintf(line, sizeof line, "f %llu %llu %o %s\n", at, size, e.mode, name);
            mb_put(r->mb, line);
        }
        w = want ? want(&e, ctx) : 0;
        if (w < 0) { if (errlen) err[0] = '\0'; break; }
        if (w == 1 && !e.is_dir) {
            unsigned char buf[16384];
            unsigned long long left = size;
            int stop = 0;
            while (left > 0) {
                size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
                if (get(r, buf, k) != 1) { if (!err[0]) fail(r, "the archive is truncated"); stop = 1; break; }
                if (data && data(&e, buf, k, ctx) != 0) { stop = 2; break; }
                left -= k;
            }
            if (stop == 1) break;
            if (stop == 2) { if (errlen) err[0] = '\0'; break; }
            if (data && data(&e, buf, 0, ctx) != 0) { if (errlen) err[0] = '\0'; break; }
            if (skip(r, pad) != 0) break;
        } else if (skip(r, size + pad) != 0) {
            break;
        }
    }
    if (r->open) BZ2_bzDecompressEnd(&r->s);
    if (r->mb != NULL) {
        if (rc == 0 && map != NULL) {
            unsigned long long fsize = 0;
            if (fseek(r->f, 0, SEEK_END) == 0) {
                long t = ftell(r->f);
                if (t > 0) fsize = (unsigned long long)t;
            }
            *map = mb_finish(r->mb, fsize);
        }
        mb_free(r->mb);
    }
    fclose(r->f);
    free(r);
    return rc;
}

int pkg_archive_walk(const char *file, pkg_archive_want_fn want, pkg_archive_data_fn data,
                     void *ctx, char *err, size_t errlen)
{
    return pkg_archive_walk_map(file, want, data, ctx, NULL, err, errlen);
}

/* ---- reading with a block map ----------------------------------------- */

struct mapped {
    char              *path;
    unsigned long long uoff, size;
    unsigned           mode;
};

static int by_uoff(const void *a, const void *b)
{
    const struct mapped *x = (const struct mapped *)a, *y = (const struct mapped *)b;
    return x->uoff < y->uoff ? -1 : x->uoff > y->uoff ? 1 : 0;
}

/* Put the reader where `uoff` is: run on from where it stands when that is
 * not far, else start again at the block that holds the point. */
static int seek_to(struct src *r, const char *file, const struct blockrec *bl,
                   const struct streamrec *sr, size_t nb, unsigned long long uoff)
{
    size_t i, pick = 0;
    unsigned char one;
    if (r->f != NULL && r->open && r->uout <= uoff && uoff - r->uout <= SKIP_AHEAD)
        return skip(r, uoff - r->uout);
    for (i = 0; i < nb; i++) {
        if (bl[i].ustart > uoff) break;
        pick = i;
    }
    if (bl[pick].ustart > uoff) return -1;
    if (r->open) { BZ2_bzDecompressEnd(&r->s); r->open = 0; }
    if (r->f != NULL) fclose(r->f);
    memset(&r->s, 0, sizeof r->s);
    r->f = fopen(file, "rb");
    if (r->f == NULL) return -1;
    r->eof = 0;
    r->bz = 1;
    r->mid = 1;
    r->in_len = r->in_pos = 0;
    r->in_base = 0;
    r->uout = bl[pick].ustart;
    r->shift = (unsigned)(bl[pick].bit % 8);
    r->raw_pos = bl[pick].bit / 8;
    r->raw_end = sr[pick].end;
    if (r->raw_end <= r->raw_pos) return -1;
    r->hdr[0] = 'B'; r->hdr[1] = 'Z'; r->hdr[2] = 'h';
    r->hdr[3] = (char)('0' + sr[pick].level);
    r->hdr_left = 1;
    if (fseek(r->f, (long)r->raw_pos, SEEK_SET) != 0) return -1;
    if (r->shift != 0) {
        if (fread(&one, 1, 1, r->f) != 1) return -1;
        r->prev = one;
        r->raw_pos++;
    }
    return skip(r, uoff - bl[pick].ustart);
}

int pkg_archive_read_mapped(const char *file, const char *map, pkg_archive_want_fn want,
                            pkg_archive_data_fn data, void *ctx, char *err, size_t errlen)
{
    struct blockrec *bl = NULL;
    struct streamrec *sr = NULL;
    struct mapped *mm = NULL;
    size_t nb = 0, bcap = 0, nm = 0, mcap = 0, i;
    const char *p;
    struct src *r = NULL;
    unsigned long long fsize = 0;
    int rc = 1;

    if (errlen) err[0] = '\0';
    for (p = map; p != NULL && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (p[0] == 'b' && p[1] == ' ') {
            unsigned long long bit, ust, send;
            int level;
            if (sscanf(p + 2, "%llu %llu %d %llu", &bit, &ust, &level, &send) == 4
                && level >= 1 && level <= 9) {
                if (nb == bcap) {
                    size_t nc = bcap ? bcap * 2 : 256;
                    struct blockrec *g = (struct blockrec *)realloc(bl, nc * sizeof *g);
                    struct streamrec *h;
                    if (g != NULL) bl = g;
                    h = (struct streamrec *)realloc(sr, nc * sizeof *h);
                    if (h != NULL) sr = h;
                    if (g == NULL || h == NULL) goto out;
                    bcap = nc;
                }
                bl[nb].bit = bit;
                bl[nb].ustart = ust;
                bl[nb].stream = 0;
                sr[nb].start = 0;
                sr[nb].end = send;
                sr[nb].level = level;
                if (send > fsize) fsize = send;
                nb++;
            }
        } else if (p[0] == 'f' && p[1] == ' ') {
            unsigned long long uoff, size;
            unsigned mode;
            int off = 0;
            if (sscanf(p + 2, "%llu %llu %o %n", &uoff, &size, &mode, &off) == 3 && off > 0
                && ln > (size_t)off + 2) {
                struct pkg_archive_entry e;
                size_t pl = ln - (size_t)off - 2;
                char *path = (char *)malloc(pl + 1);
                if (path == NULL) goto out;
                memcpy(path, p + 2 + off, pl);
                path[pl] = '\0';
                e.path = path;
                e.size = size;
                e.mode = mode;
                e.is_dir = 0;
                if (want != NULL && want(&e, ctx) == 1) {
                    if (nm == mcap) {
                        size_t nc = mcap ? mcap * 2 : 16;
                        struct mapped *g = (struct mapped *)realloc(mm, nc * sizeof *g);
                        if (g == NULL) { free(path); goto out; }
                        mm = g;
                        mcap = nc;
                    }
                    mm[nm].path = path;
                    mm[nm].uoff = uoff;
                    mm[nm].size = size;
                    mm[nm].mode = mode;
                    nm++;
                } else {
                    free(path);
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (nb == 0 || nm == 0) goto out;
    qsort(mm, nm, sizeof *mm, by_uoff);
    r = (struct src *)calloc(1, sizeof *r);
    if (r == NULL) goto out;
    r->err = err;
    r->errlen = errlen;
    r->fsize = fsize;
    for (i = 0; i < nm; i++) {
        struct pkg_archive_entry e;
        unsigned char buf[16384];
        unsigned long long left = mm[i].size;
        e.path = mm[i].path;
        e.size = mm[i].size;
        e.mode = mm[i].mode;
        e.is_dir = 0;
        if (want != NULL && want(&e, ctx) != 1) goto out;
        if (left > 0 && seek_to(r, file, bl, sr, nb, mm[i].uoff) != 0) goto out;
        while (left > 0) {
            size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
            if (get(r, buf, k) != 1) goto out;
            if (data != NULL && data(&e, buf, k, ctx) != 0) goto out;
            left -= k;
        }
        if (data != NULL && data(&e, buf, 0, ctx) != 0) goto out;
    }
    rc = 0;
out:
    if (errlen) err[0] = '\0';          /* the map is an optimisation: never a refusal */
    if (r != NULL) {
        if (r->open) BZ2_bzDecompressEnd(&r->s);
        if (r->f != NULL) fclose(r->f);
        free(r);
    }
    for (i = 0; i < nm; i++) free(mm[i].path);
    free(mm);
    free(bl);
    free(sr);
    return rc;
}

int pkg_archive_split(const char *s, char *archive, size_t al, char *inner, size_t il)
{
    const char *bang = strstr(s, "!/");
    size_t n;
    if (bang == NULL)
        return 0;
    n = (size_t)(bang - s);
    if (n == 0 || n >= al) return 0;
    memcpy(archive, s, n);
    archive[n] = '\0';
    snprintf(inner, il, "%s", bang + 2);
    n = strlen(inner);
    while (n > 0 && inner[n - 1] == '/') inner[--n] = '\0';
    return 1;
}

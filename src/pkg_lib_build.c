/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Building a package from a drawer or an archive: files, attributes, CPU,
 * $VER, libraries it opens, catalogue fields, .pkginfo.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- building a package from a drawer --------------------------------- */

void leave_out(const char *rel, int is_dir, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (!is_dir && (strcmp(base, ".ameta") == 0 || strncmp(base, ".ameta.", 7) == 0))
        return;                     /* read for the attributes, never packaged */
    char **g = (char **)realloc(d->left_out, (d->nleft + 1) * sizeof *g);
    size_t n = strlen(rel) + 2;
    char *s = (char *)malloc(n);
    if (g == NULL || s == NULL) { free(s); if (g) d->left_out = g; return; }
    d->left_out = g;
    snprintf(s, n, "%s%s", rel, is_dir ? "/" : "");
    d->left_out[d->nleft++] = s;
}

static int files_match(const char *files, const char *rel);

int load_one(const char *rel, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    char *full;
    const char *why = pkg_check_path(rel);

    if (d->files != NULL && !files_match(d->files, rel))
        return 0;                   /* FILES: only the paths it names, of a larger tree */
    if (why != NULL) {
        snprintf(d->err, sizeof d->err, "\"%s\": %s", rel, why);
        return -1;
    }
    if (d->n == d->cap) {
        size_t ncap = d->cap ? d->cap * 2u : 32u;
        struct loaded *w = (struct loaded *)realloc(d->v, ncap * sizeof *w);
        if (w == NULL) return -1;
        d->v = w;
        d->cap = ncap;
    }
    memset(&d->v[d->n], 0, sizeof d->v[d->n]);     /* every field, the new ones too */
    full = pkg_join(d->root, rel);
    d->v[d->n].rel = full ? pkg_join("", rel) : NULL;
    if (full == NULL || d->v[d->n].rel == NULL
        || pkg_fs_read(full, &d->v[d->n].data, &d->v[d->n].len) != 0) {
        snprintf(d->err, sizeof d->err, "cannot read \"%s\"", rel);
        free(full);
        return -1;
    }
    free(full);
    d->v[d->n].prot = 0;
    d->v[d->n].comment = NULL;
    d->v[d->n].host_exec = -2;
    d->n++;
    return 0;
}
static int cookie(const struct loaded *f, char *name, size_t nl, char *ver, size_t vl);

static int files_match(const char *files, const char *rel)
{
    const char *p = files;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n > 0 && p[n - 1] == '/') n--;
        if (n > 0 && strncmp(rel, p, n) == 0 && (rel[n] == '\0' || rel[n] == '/'))
            return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static const char *arch_intern(const char *a)
{
    static const char *const known[] = { "i386", "m68k", "ppc", "ppc64", "arm", "x86_64", "aarch64" };
    size_t i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strcmp(a, known[i]) == 0) return known[i];
    return NULL;
}

#define IDX_HEAD 65536u      /* enough of a file's start to read its CPU */
#define IDX_WIN  8192u       /* the window $VER cookies are looked for in */
#define IDX_KEEP 256u        /* what a cookie may need after "$VER: " */

/* Each file is read once, in pieces, and never held whole: a digest built
 * as it goes, its first bytes for the CPU, and a sliding window for its
 * $VER. An archive claiming a file of any size costs the same memory. */
struct idx_build {
    char              *out;         /* the index text being written */
    size_t             len, cap;
    struct pkg_sha256  sha;
    unsigned char      head[IDX_HEAD];
    size_t             hlen;
    unsigned long long total;
    unsigned char      win[IDX_WIN];
    size_t             wlen;
    int                found, started;
    char               cn[65], cv[64];
    int                oom;
};

/* Look for a cookie starting in win[0 .. limit). */
static void ib_scan(struct idx_build *ib, size_t limit)
{
    size_t j;
    for (j = 0; !ib->found && j < limit && j + 6 <= ib->wlen; j++) {
        struct loaded f;
        if (ib->win[j] != '$' || memcmp(ib->win + j, "$VER: ", 6) != 0)
            continue;
        memset(&f, 0, sizeof f);
        f.data = ib->win + j;
        f.len = ib->wlen - j;
        if (cookie(&f, ib->cn, sizeof ib->cn, ib->cv, sizeof ib->cv))
            ib->found = 1;
    }
}

static void ib_put(struct idx_build *ib, const char *s)
{
    size_t n = strlen(s);
    if (ib->oom) return;
    if (ib->len + n + 1 > ib->cap) {
        size_t nc = ib->cap ? ib->cap * 2 : 65536;
        char *g;
        while (nc < ib->len + n + 1) nc *= 2;
        g = (char *)realloc(ib->out, nc);
        if (g == NULL) { ib->oom = 1; return; }
        ib->out = g;
        ib->cap = nc;
    }
    memcpy(ib->out + ib->len, s, n + 1);
    ib->len += n;
}

static int ib_want(const struct pkg_archive_entry *e, void *ctx)
{
    (void)ctx;
    return e->is_dir ? 0 : 1;
}

static int ib_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct idx_build *ib = (struct idx_build *)ctx;
    if (!ib->started) {
        pkg_sha256_init(&ib->sha);
        ib->hlen = ib->wlen = 0;
        ib->total = 0;
        ib->found = 0;
        ib->started = 1;
    }
    if (len > 0) {
        size_t at = 0;
        pkg_sha256_update(&ib->sha, buf, len);
        ib->total += len;
        if (ib->hlen < IDX_HEAD) {
            size_t k = IDX_HEAD - ib->hlen < len ? IDX_HEAD - ib->hlen : len;
            memcpy(ib->head + ib->hlen, buf, k);
            ib->hlen += k;
        }
        while (!ib->found && at < len) {
            size_t k = IDX_WIN - ib->wlen < len - at ? IDX_WIN - ib->wlen : len - at;
            memcpy(ib->win + ib->wlen, buf + at, k);
            ib->wlen += k;
            at += k;
            if (ib->wlen == IDX_WIN) {
                /* every start with IDX_KEEP bytes after it, then slide */
                ib_scan(ib, IDX_WIN - IDX_KEEP);
                memmove(ib->win, ib->win + IDX_WIN - IDX_KEEP, IDX_KEEP);
                ib->wlen = IDX_KEEP;
            }
        }
        return 0;
    }
    {
        /* The end of the file: what publishing needs of it, nothing of its bytes. */
        unsigned char dg[PKG_SHA256_LEN];
        char hex[PKG_SHA256_HEXLEN + 1], line[5200];
        const char *arch;
        size_t k;
        if (!ib->found)
            ib_scan(ib, ib->wlen);
        pkg_sha256_final(&ib->sha, dg);
        for (k = 0; k < PKG_SHA256_LEN; k++)
            snprintf(hex + 2 * k, 3, "%02x", dg[k]);
        arch = file_arch(ib->head, ib->hlen);
        snprintf(line, sizeof line, "%s %llu %o %s %s %s %s\n", hex, ib->total,
                 e->mode, arch ? arch : "-", ib->found ? ib->cn : "-", ib->found ? ib->cv : "-", e->path);
        ib_put(ib, line);
        ib->started = 0;
    }
    return ib->oom ? -1 : 0;
}

/* An archive read through, as a FETCH step counted in its stored bytes: the
 * reader reports them as it goes, against the file's size. */
static void archive_step(const char *archive)
{
    const char *base = strrchr(archive, '/');
    struct pkg_fs_id id;
    ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_ARCHIVE, base != NULL ? base + 1 : archive, NULL, NULL,
             PKG_MEASURE_BYTES_READ, 0, 0);
    if (pkg_fs_identity(archive, &id) == 0 && id.exists)
        ev_total(id.size);
}

/* The archive's index, built when missing or stale; the caller frees it. */
static char *archive_index(const char *archive, char *err, size_t errlen)
{
    struct pkg_fs_id id;
    char *ip = NULL, head[128], *text = NULL;
    unsigned char *buf = NULL;
    size_t len = 0;
    struct idx_build *ib;
    char *text_out;
    int rc;

    if (pkg_fs_identity(archive, &id) != 0 || !id.exists) {
        snprintf(err, errlen, "cannot read %s", archive);
        return NULL;
    }
    /* The version changes whenever what it records is computed differently. */
    snprintf(head, sizeof head, "pkgidx 4 %llu %lld\n", id.size, id.mtime_s);
    ip = (char *)malloc(strlen(archive) + 8);
    if (ip == NULL) { snprintf(err, errlen, "out of memory"); return NULL; }
    snprintf(ip, strlen(archive) + 8, "%s.pkgidx", archive);
    if (pkg_fs_read(ip, &buf, &len) == 0 && len > strlen(head)
        && memcmp(buf, head, strlen(head)) == 0) {
        text = (char *)realloc(buf, len + 1);
        if (text != NULL) text[len] = '\0';
        tr("archive index %s is current", ip);
        free(ip);
        return text;
    }
    free(buf);
    tr("indexing %s: every file read once, for its digest, mode, CPU and $VER", archive);
    ib = (struct idx_build *)calloc(1, sizeof *ib);
    if (ib == NULL) { snprintf(err, errlen, "out of memory"); free(ip); return NULL; }
    ib_put(ib, head);
    archive_step(archive);
    rc = pkg_archive_walk(archive, ib_want, ib_data, ib, err, errlen);
    ev_end(rc == 0 ? PKG_OUTCOME_OK : PKG_OUTCOME_FAILED);
    if (rc != 0 || ib->oom) {
        if (ib->oom && !err[0]) snprintf(err, errlen, "out of memory");
        free(ib->out); free(ib); free(ip);
        return NULL;
    }
    if (pkg_fs_write_atomic(ip, ib->out, ib->len) != 0)
        tr("the archive index could not be kept at %s; it is used for this run only", ip);
    free(ip);
    text_out = ib->out;
    free(ib);
    return text_out;
}

static int load_archive(struct drawer *d, const char *archive, const char *prefix, const char *files)
{
    char err[300], *idx, *line, *next;
    size_t pl = strlen(prefix);
    err[0] = '\0';
    idx = archive_index(archive, err, sizeof err);
    if (idx == NULL) {
        snprintf(d->err, sizeof d->err, "%.200s: %.280s", archive, err[0] ? err : "cannot read it");
        return -1;
    }
    next = strchr(idx, '\n');                      /* past the header */
    for (line = next ? next + 1 : NULL; line && *line; line = next) {
        char hex[PKG_SHA256_HEXLEN + 1], arch[16], cn[65], cv[64];
        unsigned long size;
        unsigned mode;
        int off = 0;
        const char *path, *rel, *c;
        struct loaded *f;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (sscanf(line, "%64s %lu %o %15s %64s %63s %n", hex, &size, &mode, arch, cn, cv, &off) != 6
            || off == 0)
            continue;
        path = line + off;
        rel = path;
        if (pl) {
            if (strncmp(path, prefix, pl) != 0 || path[pl] != '/') continue;
            rel = path + pl + 1;
        }
        if (files && !files_match(files, rel)) continue;
        for (c = rel; c; c = strchr(c, '/') ? strchr(c, '/') + 1 : NULL)
            if (*c == '.') break;
        if (c) { leave_out(rel, 0, d); continue; }     /* hidden, as a drawer's */
        if (pkg_check_path(rel) != NULL) {
            snprintf(d->err, sizeof d->err, "\"%.200s\": %s", rel, pkg_check_path(rel));
            free(idx);
            return -1;
        }
        if (d->n == d->cap) {
            size_t ncap = d->cap ? d->cap * 2u : 32u;
            struct loaded *w = (struct loaded *)realloc(d->v, ncap * sizeof *w);
            if (w == NULL) { free(idx); return -1; }
            d->v = w;
            d->cap = ncap;
        }
        f = &d->v[d->n];
        memset(f, 0, sizeof *f);
        f->rel = pkg_join("", rel);
        if (f->rel == NULL) { free(idx); return -1; }
        f->len = size;
        f->host_exec = (mode & 0100) != 0;
        f->pre = 1;
        snprintf(f->pre_digest, sizeof f->pre_digest, "%s", hex);
        f->pre_arch = strcmp(arch, "-") ? arch_intern(arch) : NULL;
        if (strcmp(cn, "-")) snprintf(f->pre_cookie, sizeof f->pre_cookie, "%s %s", cn, cv);
        d->n++;
    }
    free(idx);
    return 0;
}

int by_rel(const void *a, const void *b)
{
    return strcmp(((const struct loaded *)a)->rel, ((const struct loaded *)b)->rel);
}

void drawer_free(struct drawer *d)
{
    size_t i;
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); free(d->v[i].comment); }
    free(d->v);
    for (i = 0; i < d->nleft; i++) free(d->left_out[i]);
    free(d->left_out);
}

/* The first "$VER: name version" cookie of one file, lower-cased. */
static int cookie(const struct loaded *f, char *name, size_t nl, char *ver, size_t vl)
{
    const unsigned char *p = f->data;
    size_t j;
    if (f->pre) {
        const char *sp = strchr(f->pre_cookie, ' ');
        if (sp == NULL) return 0;
        snprintf(name, nl, "%.*s", (int)(sp - f->pre_cookie), f->pre_cookie);
        snprintf(ver, vl, "%s", sp + 1);
        return 1;
    }
    for (j = 0; j + 6 < f->len; j++) {
        size_t k = j + 6, a = 0, b = 0;
        if (memcmp(p + j, "$VER: ", 6) != 0)
            continue;
        while (k < f->len && p[k] > ' ' && p[k] < 0x7F && a + 1 < nl)
            name[a++] = (char)tolower(p[k++]);
        name[a] = '\0';
        while (k < f->len && p[k] == ' ') k++;
        /* digits and dots, then one +build if the program carries one:
         * "$VER: pkg 1.7.0+20260920" is published as that whole version, not
         * as 1.7.0, so two builds of one day and the next are two versions. */
        while (k < f->len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.' || p[k] == '+') && b + 1 < vl)
            ver[b++] = (char)p[k++];
        while (b > 0 && (ver[b - 1] == '.' || ver[b - 1] == '+')) b--;
        ver[b] = '\0';
        if (a > 0 && b > 0 && pkg_check_name(name) == NULL && pkg_check_version(ver) == NULL)
            return 1;
    }
    return 0;
}

/* Name and version from the drawer's $VER cookies. With `want` (NAME given),
 * the cookie of that name gives the version. Without it, the cookies must all
 * agree: two programs in one drawer, each with its own cookie, would otherwise
 * make the package whichever sorts first, silently. 1 found, 0 none, -1 the
 * cookies disagree, listed in `seen`. */
int find_ver(const struct drawer *d, const char *want, char *name, size_t nl,
             char *ver, size_t vl, const char **from, char *seen, size_t sl)
{
    size_t i, at = 0, count = 0;
    int found = 0, clash = 0;
    char n[65], v[64], only_v[64];
    const char *only_from = NULL;

    seen[0] = '\0';
    for (i = 0; i < d->n; i++) {
        if (!cookie(&d->v[i], n, sizeof n, v, sizeof v))
            continue;
        if (at + 100 < sl)
            at += (size_t)snprintf(seen + at, sl - at, "%s%s (%s %s)", at ? ", " : "",
                                   d->v[i].rel, n, v);
        if (count++ == 0) {
            snprintf(only_v, sizeof only_v, "%s", v);
            only_from = d->v[i].rel;
        }
        if (want != NULL) {
            if (!found && strcmp(n, want) == 0) {
                snprintf(name, nl, "%s", n);
                snprintf(ver, vl, "%s", v);
                *from = d->v[i].rel;
                found = 1;
            }
        } else if (!found) {
            snprintf(name, nl, "%s", n);
            snprintf(ver, vl, "%s", v);
            *from = d->v[i].rel;
            found = 1;
        } else if (strcmp(n, name) != 0 || pkg_version_cmp(v, ver) != 0) {
            clash = 1;
        }
    }
    if (want != NULL && !found && count == 1) {
        /* One program in the drawer, packaged under another name than its
         * cookie gives ("afs.handler" as afs-handler): its version stands. */
        snprintf(name, nl, "%s", want);
        snprintf(ver, vl, "%s", only_v);
        *from = only_from;
        return 1;
    }
    if (want != NULL && !found && count > 1)
        return -1;
    return clash ? -1 : found;
}

/* The CPU a file is built for, from its own header: ELF's e_machine, or the
 * hunk format every 68k Amiga executable has. NULL for data and scripts. */
const char *file_arch(const unsigned char *p, size_t len)
{
    if (len >= 20 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') {
        unsigned m = p[5] == 2 ? (unsigned)p[18] << 8 | p[19] : (unsigned)p[19] << 8 | p[18];
        switch (m) {
        case 3:   return "i386";
        case 4:   return "m68k";
        case 20:  return "ppc";
        case 21:  return "ppc64";
        case 40:  return "arm";
        case 62:  return "x86_64";
        case 183: return "aarch64";
        default:  return NULL;
        }
    }
    if (len >= 24 && pkg_be32_get(p) == 0x000003F3ul) {
        /* HUNK_HEADER: resident library names (a zero-terminated list of
         * counted strings), the table size, first and last hunk, one size
         * per hunk, then the first hunk's type. A program starts with
         * HUNK_CODE; a hunk file of data only, such as deficons.prefs, is
         * not tied to a CPU. */
        size_t at = 4, n;
        unsigned long first, last, type;
        while (at + 4 <= len && (n = pkg_be32_get(p + at)) != 0) {
            at += 4 + 4 * (size_t)n;
            if (n > len) return NULL;
        }
        if (at + 16 > len) return NULL;
        first = pkg_be32_get(p + at + 8);
        last = pkg_be32_get(p + at + 12);
        if (last < first || last - first > 4096) return NULL;
        at += 16 + 4 * (size_t)(last - first + 1);
        if (at + 4 > len) return NULL;
        type = pkg_be32_get(p + at) & 0x3FFFFFFFul;
        if (type != 0x3E9ul)                        /* HUNK_CODE */
            return NULL;
        /* A classic font starts its code with moveq #n,d0 and rts: a stub
         * that returns at once, before the data diskfont.library reads. */
        if (at + 12 <= len && p[at + 8] == 0x70 && p[at + 10] == 0x4E && p[at + 11] == 0x75)
            return NULL;
        return "m68k";
    }
    return NULL;
}

/* The CPU a file of the drawer is built for, NULL for none. A hunk file in
 * a Keymaps drawer is a keymap, data keymap.library loads on any CPU. */
static const char *entry_arch(const struct loaded *f)
{
    const char *a = f->pre ? f->pre_arch : file_arch(f->data, f->len);
    if (a != NULL && strcmp(a, "m68k") == 0
        && (strncmp(f->rel, "Keymaps/", 8) == 0 || strstr(f->rel, "/Keymaps/") != NULL))
        return NULL;
    return a;
}

/* The drawer's architecture: the one its executables share, "generic" when
 * it has none. 1 found, 0 none, -1 more than one: `seen` then says how many
 * files each CPU has, with one of them. */
static int drawer_arch(const struct drawer *d, const char **arch, const char **from,
                       char *seen, size_t sl)
{
    const char *cpu[8], *ex[8];
    size_t cnt[8], nc = 0, i, k, at = 0;
    *arch = NULL;
    seen[0] = '\0';
    for (i = 0; i < d->n; i++) {
        const char *a = entry_arch(&d->v[i]);
        if (a == NULL)
            continue;
        if (*arch == NULL) {
            *arch = a;
            *from = d->v[i].rel;
        }
        for (k = 0; k < nc && strcmp(cpu[k], a) != 0; k++) ;
        if (k == nc && nc < 8) { cpu[nc] = a; ex[nc] = d->v[i].rel; cnt[nc++] = 0; }
        if (k < nc) cnt[k]++;
    }
    for (k = 0; nc > 1 && k < nc && at + 1 < sl; k++)
        at += (size_t)snprintf(seen + at, sl - at, "%s%s in %lu file%s, such as %.200s", k ? "; " : "",
                               cpu[k], (unsigned long)cnt[k], cnt[k] == 1 ? "" : "s", ex[k]);
    return nc > 1 ? -1 : *arch != NULL;
}

/* DEPENDS "a >= 1.0, b": each item a name, alone or with ">=" and the lowest
 * version that will do, spaces optional around ">=". */
static int add_depends(struct pkg_manifest *m, const char *list)
{
    const char *p = list, *why;
    if (list != NULL && strcmp(list, "none") == 0)
        return 0;                   /* DEPENDS none: a version that needs nothing */
    while (p != NULL && *p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[160], norm[170], dn[65], dv[64], *ge;
        size_t a = 0, b;
        if (len >= sizeof item)
            return refuse_c(20, "a DEPENDS item is too long");
        memcpy(item, p, len);
        item[len] = '\0';
        p = end ? end + 1 : NULL;
        while (item[a] == ' ') a++;
        b = strlen(item);
        while (b > a && item[b - 1] == ' ') item[--b] = '\0';
        if (b == a)
            continue;
        ge = strstr(item + a, ">=");
        if (ge != NULL) {
            char *l = ge, *r = ge + 2;
            while (l > item + a && l[-1] == ' ') l--;
            while (*r == ' ') r++;
            *l = '\0';
            snprintf(norm, sizeof norm, "%s >= %s", item + a, r);
        } else {
            snprintf(norm, sizeof norm, "%s", item + a);
        }
        if ((why = pkg_parse_dep(norm, dn, sizeof dn, dv, sizeof dv)) != NULL)
            return refuse_c(20, "DEPENDS \"%s\": %s", norm, why);
        if (pkg_manifest_add_dep(m, dn, dv) != 0)
            return refuse("out of memory");
    }
    pkg_manifest_sort(m);
    if ((why = pkg_check_deps(m)) != NULL)
        return refuse_c(20, "DEPENDS: %s", why);
    return 0;
}

/* Replace the drawer's files by one: the drawer as an FFS volume named after
 * the package, <name>.hdf. */
int to_image(struct drawer *d, const char *name)
{
    struct pkg_image_entry *e = calloc(d->n ? d->n : 1, sizeof *e);
    unsigned char *img;
    size_t len, i;
    char err[300], rel[80];

    if (e == NULL) return refuse("out of memory");
    char (*latin)[PKG_COMMENT_MAX + 1] = calloc(d->n ? d->n : 1, sizeof *latin);
    if (latin == NULL) { free(e); return refuse("out of memory"); }
    for (i = 0; i < d->n; i++) {
        e[i].path = d->v[i].rel;
        e[i].data = d->v[i].data;
        e[i].len = d->v[i].len;
        e[i].protect = (unsigned long)(d->v[i].prot & 0xFFFFFFFFull);
        if (d->v[i].comment != NULL)
            pkg_comment_latin1(d->v[i].comment, latin[i], sizeof latin[i]);
        e[i].comment = latin[i];
    }
    if (pkg_image_build(e, d->n, name, &img, &len, err, sizeof err) != 0) {
        free(e);
        free(latin);
        return refuse_c(20, "the drawer cannot become an image: %s", err);
    }
    free(e);
    free(latin);
    snprintf(rel, sizeof rel, "%s.hdf", name);
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); free(d->v[i].comment); }
    d->v[0].rel = malloc(strlen(rel) + 1);
    if (d->v[0].rel == NULL) { free(img); d->n = 0; return refuse("out of memory"); }
    strcpy(d->v[0].rel, rel);
    d->v[0].data = img;
    d->v[0].len = len;
    d->v[0].prot = 0;
    d->v[0].comment = NULL;
    d->n = 1;
    return 0;
}
const struct index *inherit_ix;
const char *inherit_channel;

/* INFO: which fields this .pkginfo gave, for the line under the result. */
void say_info_from(const struct built *b)
{
    char what[300];
    int cat = b->about_from[0] && strcmp(b->about_from, b->info_from) == 0;
    if (b->info_from[0] == '\0') return;
    snprintf(what, sizeof what, "%s%s%s", b->info_fields,
             b->info_fields[0] && cat ? " and " : "", cat ? "the catalogue fields" : "");
    if (what[0] != '\0')
        say_detail("%s from %s", what, b->info_from);
}

void built_free(struct built *b)
{
    size_t i;
    pkg_manifest_free(&b->m);
    free(b->pkg);
    free(b->text);
    for (i = 0; i < b->nleft; i++) free(b->left_out[i]);
    free(b->left_out);
    b->left_out = NULL;
    b->nleft = 0;
}

/* Letters and digits only, lower case: "afs.handler" and "afs-handler" agree. */
static void squash(const char *in, char *out, size_t ol)
{
    size_t o = 0;
    for (; *in && o + 1 < ol; in++)
        if ((*in >= 'a' && *in <= 'z') || (*in >= '0' && *in <= '9'))
            out[o++] = *in;
        else if (*in >= 'A' && *in <= 'Z')
            out[o++] = (char)(*in + 32);
    out[o] = '\0';
}

/* An executable whose $VER names another program than its file name is
 * usually the wrong file copied into the drawer. Scripts and data carry the
 * cookies of whatever they belong to, so only executables are checked. */
static void check_cookie_names(const struct drawer *d)
{
    size_t i;
    char n[65], v[64], sn[65], sf[80];
    for (i = 0; i < d->n; i++) {
        const char *base = strrchr(d->v[i].rel, '/');
        base = base ? base + 1 : d->v[i].rel;
        if (entry_arch(&d->v[i]) == NULL
            || !cookie(&d->v[i], n, sizeof n, v, sizeof v))
            continue;
        squash(n, sn, sizeof sn);
        squash(base, sf, sizeof sf);
        if (sn[0] && sf[0] && strstr(sf, sn) == NULL && strstr(sn, sf) == NULL)
            warn("%s carries the $VER cookie of %s %s, another program than its file name "
                 "says: is it the file you meant to publish?", d->v[i].rel, n, v);
    }
}

int present_in(const unsigned char *name, size_t len, void *ctx)
{
    const struct present_ctx *pc = (const struct present_ctx *)ctx;
    char n[512], *p;
    int there;
    if (len >= sizeof n) return 1;
    memcpy(n, name, len);
    n[len] = '\0';
    p = pkg_join(pc->dir, n);
    there = p != NULL && pkg_fs_exists(p);
    free(p);
    return there;
}

char *pkg_strdup(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    if (p != NULL) strcpy(p, s);
    return p;
}

static void latin1_to_utf8(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 3 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = '\0';
}

/* The protection word and comment each file of the drawer is published
 * with. On AROS, the file system's own. Elsewhere, the publishing profile of
 * ameta.md: owner Execute from the host mode, where there is one, every
 * other bit and the comment from the directory's .ameta, AROS defaults
 * otherwise. A signed package does not drop metadata silently: a malformed
 * or stale .ameta line, or a comment AROS cannot store, is refused. */
int drawer_attrs(struct drawer *d)
{
    char done[64][512];
    size_t ndone = 0, i;
    int warned_owner = 0;
    for (i = 0; i < d->n; i++) {
        struct loaded *f = &d->v[i];
        char *full = d->root ? pkg_join(d->root, f->rel) : NULL, latin[PKG_COMMENT_MAX + 1], cbuf[4 * PKG_COMMENT_MAX + 1];
        const char *slash = strrchr(f->rel, '/'), *base = slash ? slash + 1 : f->rel;
        char dirrel[512], *dir, *ap;
        unsigned char *buf = NULL;
        size_t len = 0, k;
        struct pkg_ameta a;
        struct pkg_ameta_entry *e;
        int native, ex, seen = 0;
        if (d->root == NULL) {          /* from an archive: its mode, AROS defaults */
            f->prot = pkg_ameta_publish_word(NULL, 1, f->host_exec == 1);
            free(full);
            continue;
        }
        if (full == NULL) return refuse("out of memory");
        native = pkg_fs_amiga_get(full, &f->prot, latin, sizeof latin);
        if (native < 0) { free(full); return refuse_c(17, "cannot read the attributes of \"%s\"", f->rel); }
        if (native == 1) {
            if (latin[0]) {
                latin1_to_utf8(latin, cbuf, sizeof cbuf);
                f->comment = pkg_strdup(cbuf);
            }
            free(full);
            continue;
        }
        ex = pkg_fs_owner_exec(full);
        free(full);
        snprintf(dirrel, sizeof dirrel, "%.*s", slash ? (int)(slash - f->rel) : 0, f->rel);
        dir = pkg_join(d->root, dirrel);
        ap = dir ? pkg_join(dir, ".ameta") : NULL;
        if (ap == NULL) { free(dir); return refuse("out of memory"); }
        pkg_ameta_init(&a);
        if (pkg_fs_exists(ap)) {
            struct present_ctx pc;
            if (pkg_fs_read(ap, &buf, &len) != 0 || pkg_ameta_parse(buf, len, &a) != 0) {
                free(buf); free(ap); free(dir); pkg_ameta_free(&a);
                return refuse_c(17, "cannot read %s/.ameta", dirrel[0] ? dirrel : ".");
            }
            free(buf);
            for (k = 0; k < ndone; k++)
                if (strcmp(done[k], dirrel) == 0) seen = 1;
            if (!a.usable || a.nr > 0) {
                refuse_c(20, "%s/.ameta, line %u: %s. A published package must not drop metadata; "
                         "correct the line", dirrel[0] ? dirrel : ".", a.nr ? a.r[0].line : 1,
                         a.nr ? a.r[0].reason : "unreadable");
                free(ap); free(dir); pkg_ameta_free(&a);
                return 1;
            }
            pc.dir = dir;
            pkg_ameta_mark_stale(&a, present_in, &pc);
            for (k = 0; !seen && k < a.n; k++) {
                char esc[1600];
                pkg_ameta_escape(a.e[k].name, a.e[k].name_len, esc);
                if (a.e[k].stale) {
                    refuse_c(20, "%s/.ameta names %s, which is not in the drawer (renamed or deleted "
                             "on the host?); correct or remove that section", dirrel[0] ? dirrel : ".", esc);
                    free(ap); free(dir); pkg_ameta_free(&a);
                    return 1;
                }
                if ((a.e[k].has_uid || a.e[k].has_gid) && !warned_owner) {
                    warn("%s/.ameta gives an owner, which a package does not carry: installed files "
                         "belong to whoever installs them", dirrel[0] ? dirrel : ".");
                    warned_owner = 1;
                }
                {
                    char raw[512], *sub;
                    snprintf(raw, sizeof raw, "%.*s", (int)a.e[k].name_len, (const char *)a.e[k].name);
                    sub = pkg_join(dir, raw);
                    /* a directory's own attributes: no manifest line holds them */
                    if (sub != NULL && pkg_fs_is_dir(sub)
                        && ((a.e[k].prot & ~PKG_AMETA_RECORD_MASK) || a.e[k].comment_len))
                        warn("%s/.ameta gives attributes to the directory %s, which a package does "
                             "not carry", dirrel[0] ? dirrel : ".", esc);
                    free(sub);
                }
            }
            if (!seen && ndone < 64)
                snprintf(done[ndone++], sizeof done[0], "%s", dirrel);
        }
        e = pkg_ameta_find(&a, (const unsigned char *)base, strlen(base));
        f->prot = pkg_ameta_publish_word(e, ex >= 0, ex == 1);
        if (e != NULL && e->comment_len) {
            char *c = (char *)malloc(e->comment_len + 1);
            if (c == NULL) { free(ap); free(dir); pkg_ameta_free(&a); return refuse("out of memory"); }
            memcpy(c, e->comment, e->comment_len);
            c[e->comment_len] = '\0';
            if (memchr(c, '\0', e->comment_len) != NULL
                || pkg_comment_latin1(c, latin, sizeof latin) < 0) {
                refuse_c(20, "the comment of %s in %s/.ameta is longer than %d characters or holds "
                         "a character outside Latin-1: AROS would cut or change it at install",
                         f->rel, dirrel[0] ? dirrel : ".", PKG_COMMENT_MAX);
                free(c); free(ap); free(dir); pkg_ameta_free(&a);
                return 1;
            }
            f->comment = c;
        }
        tr("attributes of %s: prot 0x%08llx%s%s", f->rel, f->prot, f->comment ? ", comment " : "",
           f->comment ? f->comment : "");
        pkg_ameta_free(&a);
        free(ap);
        free(dir);
    }
    return 0;
}

int ascii_casecmp_n(const char *x, const char *y, size_t n)
{
    for (; n > 0; n--, x++, y++) {
        int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
        if (cx != cy || cx == 0) return cx - cy;
    }
    return 0;
}

int ascii_casecmp(const char *x, const char *y)
{
    for (; *x && *y; x++, y++) {
        int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
        if (cx != cy) return cx - cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* Marks the configuration files: each comma-separated entry of `list` is a
 * file of the package or a folder holding some. A name that matches nothing
 * is refused when the command gave it (`strict`), and let go when it came
 * from the last version, whose file may be gone. */
static int mark_config(struct pkg_manifest *m, const char *list, int strict)
{
    const char *p = list;
    while (*p) {
        size_t n, i, hits = 0;
        char pat[1024];
        while (*p == ' ' || *p == ',') p++;
        n = strcspn(p, ",");
        while (n > 0 && p[n - 1] == ' ') n--;
        if (n == 0) break;
        if (n >= sizeof pat)
            return refuse_c(20, "CONFIG holds a name longer than %u characters", (unsigned)sizeof pat - 1);
        memcpy(pat, p, n);
        pat[n] = '\0';
        while (n > 1 && pat[n - 1] == '/') pat[--n] = '\0';
        for (i = 0; i < m->nfiles; i++) {
            const char *f = m->files[i].path;
            if (strcmp(f, pat) == 0 || (strncmp(f, pat, n) == 0 && f[n] == '/')) {
                m->files[i].config = 1;
                hits++;
            }
        }
        if (hits == 0 && strict)
            return refuse_c(20, "CONFIG names \"%s\", which is no file or folder of this package; "
                            "give paths as the package installs them, such as S/Startup-Sequence "
                            "or Prefs/Env-Archive", pat);
        tr("config %s: %lu file%s", pat, (unsigned long)hits, hits == 1 ? "" : "s");
        p += strcspn(p, ",");
    }
    return 0;
}

/* ---- libraries a program opens ------------------------------------------ */

#include "pkg_syslibs.h"

int is_system_lib(const char *n)
{
    size_t i;
    for (i = 0; pkg_system_libs[i]; i++)
        if (ascii_casecmp(pkg_system_libs[i], n) == 0) return 1;
    return 0;
}

int strs_has_nocase(const struct pkg_strs *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (ascii_casecmp(l->v[i], s) == 0) return 1;
    return 0;
}

/* The library and device names a file holds as C strings, "SDL2.library"
 * followed by its NUL: what a program passes to OpenLibrary or OpenDevice. */
void lib_names(const unsigned char *p, size_t len, struct pkg_strs *out)
{
    static const char *const suffix[] = { ".library", ".device" };
    size_t j, k;
    for (j = 0; j < len; j++) {
        for (k = 0; k < 2; k++) {
            size_t sl = strlen(suffix[k]), a;
            char name[80];
            if (j + sl >= len || memcmp(p + j, suffix[k], sl) != 0 || p[j + sl] != '\0')
                continue;
            a = j;
            while (a > 0 && j - a < 60 && (isalnum(p[a - 1]) || p[a - 1] == '_' || p[a - 1] == '-'
                                         || p[a - 1] == '.' || p[a - 1] == '+'))
                a--;
            if (a == j || (a > 0 && p[a - 1] > ' ' && p[a - 1] < 0x7F && p[a - 1] != ':' && p[a - 1] != '/'))
                continue;           /* no name before it, or the tail of a longer word */
            snprintf(name, sizeof name, "%.*s%s", (int)(j - a), (const char *)p + a, suffix[k]);
            if (pkg_check_libname(name) == NULL && !strs_has_nocase(out, name))
                pkg_strs_add(out, name);
        }
    }
}

static int strcmp_p(const void *x, const void *y)
{
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

/* ---- the catalogue fields at publish ------------------------------------ */

static int valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        int k = p[i] < 0x80 ? 0 : (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2
              : (p[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + (size_t)k >= n + (k ? 0 : 1)) return 0;
        for (i++; k > 0; k--, i++)
            if (i >= n || (p[i] & 0xC0) != 0x80) return 0;
    }
    return 1;
}

/* The lines of a text a person wrote, as the manifest carries them: UTF-8
 * (Latin-1, which Amiga text often is, converted), tabs as spaces, no
 * trailing space, one empty line at most between paragraphs, none at
 * either end. From `start` on, when the text has a header before it. */
static int text_lines(const unsigned char *buf, size_t len, size_t start, struct pkg_strs *out)
{
    char line[1024];
    size_t i = start, o = 0;
    int latin = !valid_utf8(buf, len), blank = 0;
    for (; i <= len; i++) {
        unsigned c = i < len ? buf[i] : '\n';
        if (c == '\r') continue;
        if (c == '\n') {
            while (o > 0 && line[o - 1] == ' ') o--;
            line[o] = '\0';
            if (o == 0) {
                blank = out->n > 0;
            } else {
                if (blank && pkg_strs_add(out, "") != 0) return -1;
                blank = 0;
                if (pkg_strs_add(out, line) != 0) return -1;
            }
            o = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7F) continue;
        if (o == 0 && c == ' ') continue;          /* no leading space either */
        if (latin && c >= 0x80) {
            if (o + 2 < sizeof line) { line[o++] = (char)(0xC0 | (c >> 6)); line[o++] = (char)(0x80 | (c & 0x3F)); }
        } else if (o + 1 < sizeof line) {
            line[o++] = (char)c;
        }
    }
    return 0;
}

static int text_file(const char *path, struct pkg_strs *out, const char *kw)
{
    unsigned char *buf;
    size_t len;
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(11, "%s names %s, which cannot be read", kw, path);
    if (text_lines(buf, len, 0, out) != 0) { free(buf); return refuse("out of memory"); }
    free(buf);
    return 0;
}

/* "a, b, c": each item trimmed, empty ones dropped. */
static int list_items(const char *list, struct pkg_strs *out)
{
    const char *p = list;
    while (*p) {
        char item[512];
        size_t n = strcspn(p, ",");
        size_t a = 0, b = n;
        while (a < b && p[a] == ' ') a++;
        while (b > a && p[b - 1] == ' ') b--;
        if (b - a >= sizeof item) return refuse_c(20, "a list item is longer than %u characters", (unsigned)sizeof item - 1);
        if (b > a) {
            memcpy(item, p + a, b - a);
            item[b - a] = '\0';
            if (pkg_strs_add(out, item) != 0) return refuse("out of memory");
        }
        p += n;
        if (*p == ',') p++;
    }
    return 0;
}

static int strs_copy(struct pkg_strs *dst, const struct pkg_strs *src)
{
    size_t i;
    for (i = 0; i < src->n; i++)
        if (pkg_strs_add(dst, src->v[i]) != 0) return -1;
    return 0;
}

/* An Aminet .readme: "Short:", "Author:", "Type:" and the others in the
 * header, then after the first empty line the text. */
static int aminet_readme(const char *path, struct pkg_about *a)
{
    unsigned char *buf;
    size_t len, i = 0, body = 0;
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(11, "README names %s, which cannot be read", path);
    while (i < len) {
        size_t e = i, k;
        char key[16], val[512];
        while (e < len && buf[e] != '\n') e++;
        if (e == i || (e == i + 1 && buf[i] == '\r')) { body = e + 1; break; }
        for (k = i; k < e && buf[k] != ':' && k - i < sizeof key - 1; k++) key[k - i] = (char)buf[k];
        key[k - i] = '\0';
        if (k < e && buf[k] == ':') {
            struct pkg_strs one = { NULL, 0 };
            size_t vs = k + 1, ve = e;
            while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
            if (ve - vs >= sizeof val) ve = vs + sizeof val - 1;
            if (text_lines(buf + vs, ve - vs, 0, &one) == 0 && one.n > 0) {
                snprintf(val, sizeof val, "%s", one.v[0]);
                if (ascii_casecmp(key, "Short") == 0 && a->short_desc == NULL)
                    a->short_desc = pkg_strdup(val);
                else if (ascii_casecmp(key, "Author") == 0 && a->authors.n == 0)
                    list_items(val, &a->authors);
                else if (ascii_casecmp(key, "Type") == 0 && a->category == NULL) {
                    char *sp = strchr(val, ' ');
                    if (sp) *sp = '\0';        /* "util/arc  (more)" */
                    if (pkg_check_category(val) == NULL) a->category = pkg_strdup(val);
                }
            }
            pkg_strs_free(&one);
        }
        i = e + 1;
    }
    if (body > 0 && body < len && a->description.n == 0) {
        /* the text, less the readme's own heading over it: "DESCRIPTION",
         * "Description" and its line of "=" */
        size_t k = 0, i2;
        text_lines(buf, len, body, &a->description);
        while (k < a->description.n) {
            const char *l = a->description.v[k];
            int rule = l[0] != '\0';
            for (i2 = 0; l[i2] && rule; i2++) rule = l[i2] == '=' || l[i2] == '-' || l[i2] == '*';
            if (!(ascii_casecmp(l, "description") == 0 || ascii_casecmp(l, "description:") == 0
                  || rule || l[0] == '\0'))
                break;
            k++;
        }
        if (k > 0) {
            for (i2 = 0; i2 < k; i2++) free(a->description.v[i2]);
            memmove(a->description.v, a->description.v + k, (a->description.n - k) * sizeof *a->description.v);
            a->description.n -= k;
        }
    }
    free(buf);
    return 0;
}

/* ---- INFO: a .pkginfo the port carries --------------------------------- */

/* What INFO named, parsed, for the length of this build; freed when the next
 * build reads one, since compose_about borrows its strings. */
static struct pkg_pkginfo cur_info;
static int have_info;
static char info_shown[1200];

/* One file's bytes out of an archive, for INFO "!/<path in archive>". */
struct grab {
    const char    *want;
    unsigned char *data;
    size_t         len, cap;
    int            found, oom;
};

static int grab_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct grab *g = (struct grab *)ctx;
    return !e->is_dir && strcmp(e->path, g->want) == 0;
}

static int grab_data(const struct pkg_archive_entry *e, const unsigned char *buf,
                     size_t len, void *ctx)
{
    struct grab *g = (struct grab *)ctx;
    (void)e;
    if (len == 0) { g->found = 1; return -1; }      /* it is whole: stop the walk */
    if (g->len + len > g->cap) {
        size_t ncap = g->cap ? g->cap : 4096;
        unsigned char *w;
        while (ncap < g->len + len) ncap *= 2u;
        w = (unsigned char *)realloc(g->data, ncap);
        if (w == NULL) { g->oom = 1; return -1; }
        g->data = w;
        g->cap = ncap;
    }
    memcpy(g->data + g->len, buf, len);
    g->len += len;
    return 0;
}

/* Read and parse the file INFO names: a path of the file system, or
 * "!/<path>" inside the archive the drawer is a prefix of. */
static int read_info(const struct pkg_options *a)
{
    char err[400], arch_file[1024], arch_prefix[1024];
    unsigned char *buf = NULL;
    size_t len = 0;
    int line = 0, rc;

    if (have_info) { pkg_pkginfo_free(&cur_info); have_info = 0; }
    if (a->info[0] == '!' && a->info[1] == '/') {
        struct grab g;
        memset(&g, 0, sizeof g);
        if (!pkg_archive_split(a->target ? a->target : "", arch_file, sizeof arch_file,
                               arch_prefix, sizeof arch_prefix))
            return refuse_c(20, "INFO \"%s\" reads the file out of the archive the package's "
                            "files come from, and this is no archive: publish from "
                            "\"<archive>!/<top dir>\", or give INFO a path of your file system",
                            a->info);
        g.want = a->info + 2;
        snprintf(info_shown, sizeof info_shown, "%s!/%s", arch_file, g.want);
        tr("reading %s out of %s", g.want, arch_file);
        err[0] = '\0';
        archive_step(arch_file);
        rc = pkg_archive_walk(arch_file, grab_want, grab_data, &g, err, sizeof err);
        ev_end(rc == 0 || g.found ? PKG_OUTCOME_OK : PKG_OUTCOME_FAILED);
        if (rc != 0 && !g.found) {
            free(g.data);
            if (g.oom) return refuse("out of memory");
            return refuse_c(err[0] ? 12 : 11, "%s", err[0] ? err : "no file at that path in the archive");
        }
        if (!g.found) {
            free(g.data);
            return refuse_c(11, "the archive %s holds no \"%s\"; INFO names the path as the "
                            "archive names it", arch_file, g.want);
        }
        buf = g.data;
        len = g.len;
    } else {
        snprintf(info_shown, sizeof info_shown, "%s", a->info);
        if (!pkg_fs_exists(a->info))
            return refuse_c(11, "no .pkginfo at \"%s\"", a->info);
        if (pkg_fs_read(a->info, &buf, &len) != 0)
            return refuse_c(17, "cannot read \"%s\"", a->info);
    }
    rc = pkg_pkginfo_parse((const char *)buf, len, &cur_info, err, sizeof err, &line);
    free(buf);
    if (rc != 0) {
        pkg_pkginfo_free(&cur_info);
        /* the publisher's own file, like a keyword's value: a mistake to correct */
        if (line > 0)
            return refuse_c(20, "%s, line %d: %s. Correct the file", info_shown, line, err);
        return refuse_c(20, "%s: %s. Correct the file", info_shown, err);
    }
    have_info = 1;
    tr("INFO %s: name %s, version %s, kind %s, %lu file%s", info_shown,
       cur_info.name ? cur_info.name : "-", cur_info.version ? cur_info.version : "-",
       cur_info.kind ? cur_info.kind : "-", (unsigned long)cur_info.files.n,
       cur_info.files.n == 1 ? "" : "s");
    return 0;
}

static int set_once(char **slot, const char *v)
{
    free(*slot);
    *slot = NULL;
    if (v == NULL || ascii_casecmp(v, "none") == 0)
        return 0;
    return (*slot = pkg_strdup(v)) == NULL ? -1 : 0;
}

/* The catalogue fields of the version being published: the keywords, then
 * the Aminet readme, then the last version published. */
static int compose_about(const struct pkg_options *a, const char *name, const char *arch,
                         struct pkg_about *out, char *from, size_t fl)
{
    struct pkg_manifest em;
    struct pkg_about rd;
    const char *why = NULL;
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&rd, 0, sizeof rd);
    from[0] = '\0';
    tr("catalogue fields: name %s, arch %s, channel index %s", name ? name : "-", arch ? arch : "-", inherit_ix ? "read" : "none");
    if (name != NULL && last_published(name, arch, &em)) {
        struct pkg_about *p = &em.about;
        int any = p->short_desc || p->description.n || p->category || p->tags.n || p->authors.n
                  || p->homepage || p->repository || p->license || p->distribution
                  || p->icon || p->screenshots.n;
        if (set_once(&out->short_desc, p->short_desc) || strs_copy(&out->description, &p->description)
            || set_once(&out->category, p->category) || strs_copy(&out->tags, &p->tags)
            || strs_copy(&out->authors, &p->authors) || set_once(&out->homepage, p->homepage)
            || set_once(&out->repository, p->repository) || set_once(&out->license, p->license)
            || set_once(&out->distribution, p->distribution) || set_once(&out->icon, p->icon)
            || strs_copy(&out->screenshots, &p->screenshots)) {
            pkg_manifest_free(&em);
            return refuse("out of memory");
        }
        if (any) snprintf(from, fl, "%s %s", em.name, em.version);
        pkg_manifest_free(&em);
    }
    if (a->readme != NULL) {
        if (aminet_readme(a->readme, &rd) != 0) return 1;
        if (rd.short_desc) set_once(&out->short_desc, rd.short_desc);
        if (rd.category) set_once(&out->category, rd.category);
        if (rd.authors.n) { pkg_strs_free(&out->authors); strs_copy(&out->authors, &rd.authors); }
        if (rd.description.n) { pkg_strs_free(&out->description); strs_copy(&out->description, &rd.description); }
        free(rd.short_desc); free(rd.category);
        pkg_strs_free(&rd.authors); pkg_strs_free(&rd.description);
        from[0] = '\0';
    }
    if (have_info) {
        /* what the port says about itself: over the readme and over the last
         * version, under the keywords below */
        const struct pkg_about *pi = &cur_info.about;
        int any = pi->short_desc || pi->description.n || pi->category || pi->tags.n
                  || pi->authors.n || pi->homepage || pi->repository || pi->license
                  || pi->distribution || pi->changes.n || pi->icon || pi->screenshots.n;
        if (pi->short_desc) set_once(&out->short_desc, pi->short_desc);
        if (pi->category) set_once(&out->category, pi->category);
        if (pi->homepage) set_once(&out->homepage, pi->homepage);
        if (pi->repository) set_once(&out->repository, pi->repository);
        if (pi->license) set_once(&out->license, pi->license);
        if (pi->distribution) set_once(&out->distribution, pi->distribution);
        if (pi->icon) set_once(&out->icon, pi->icon);
        if (pi->description.n) { pkg_strs_free(&out->description); strs_copy(&out->description, &pi->description); }
        if (pi->changes.n) { pkg_strs_free(&out->changes); strs_copy(&out->changes, &pi->changes); }
        if (pi->tags.n) { pkg_strs_free(&out->tags); strs_copy(&out->tags, &pi->tags); }
        if (pi->authors.n) { pkg_strs_free(&out->authors); strs_copy(&out->authors, &pi->authors); }
        if (pi->screenshots.n) { pkg_strs_free(&out->screenshots); strs_copy(&out->screenshots, &pi->screenshots); }
        if (any) snprintf(from, fl, "%s", info_shown);
        else from[0] = '\0';
    }
    if ((a->short_desc && set_once(&out->short_desc, a->short_desc))
        || (a->category && set_once(&out->category, a->category))
        || (a->homepage && set_once(&out->homepage, a->homepage))
        || (a->repository && set_once(&out->repository, a->repository))
        || (a->license && set_once(&out->license, a->license))
        || (a->distribution && set_once(&out->distribution, a->distribution))
        || (a->icon && set_once(&out->icon, a->icon)))
        return refuse("out of memory");
    if (a->description) {
        pkg_strs_free(&out->description);
        if (ascii_casecmp(a->description, "none") != 0 && text_file(a->description, &out->description, "DESCRIPTION") != 0)
            return 1;
    }
    if (a->changes && text_file(a->changes, &out->changes, "CHANGES") != 0)
        return 1;
    if (a->tags) {
        struct pkg_strs t = { NULL, 0 };
        pkg_strs_free(&out->tags);
        if (ascii_casecmp(a->tags, "none") != 0) {
            if (list_items(a->tags, &t) != 0) return 1;
            for (i = 0; i < t.n; i++) {
                char *c;
                for (c = t.v[i]; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
                if (pkg_strs_add(&out->tags, t.v[i]) != 0) { pkg_strs_free(&t); return refuse("out of memory"); }
            }
            pkg_strs_free(&t);
        }
    }
    if (a->author) {
        pkg_strs_free(&out->authors);
        if (ascii_casecmp(a->author, "none") != 0 && list_items(a->author, &out->authors) != 0) return 1;
    }
    if (a->screenshot) {
        pkg_strs_free(&out->screenshots);
        if (ascii_casecmp(a->screenshot, "none") != 0 && list_items(a->screenshot, &out->screenshots) != 0) return 1;
    }
    /* the rules the manifest parser applies, said here with the keyword */
    if (out->short_desc && (why = pkg_check_about_text("SHORT", out->short_desc, 40)) != NULL) return refuse_c(20, "%s", why);
    if (out->category && (why = pkg_check_category(out->category)) != NULL) return refuse_c(20, "CATEGORY: %s", why);
    if (out->homepage && (why = pkg_check_url("HOMEPAGE", out->homepage)) != NULL) return refuse_c(20, "%s", why);
    if (out->repository && (why = pkg_check_url("REPOSITORY", out->repository)) != NULL) return refuse_c(20, "%s", why);
    if (out->license && (why = pkg_check_license(out->license)) != NULL) return refuse_c(20, "LICENSE: %s", why);
    if (out->distribution && (why = pkg_check_distribution(out->distribution)) != NULL) return refuse_c(20, "DISTRIBUTION: %s", why);
    if (out->tags.n > 16) return refuse_c(20, "TAGS holds %lu tags; 16 at most", (unsigned long)out->tags.n);
    for (i = 0; i < out->tags.n; i++) {
        size_t k;
        if ((why = pkg_check_tag(out->tags.v[i])) != NULL) return refuse_c(20, "TAGS: \"%s\": %s", out->tags.v[i], why);
        for (k = 0; k < i; k++)
            if (strcmp(out->tags.v[k], out->tags.v[i]) == 0) return refuse_c(20, "TAGS names \"%s\" twice", out->tags.v[i]);
    }
    for (i = 0; i < out->authors.n; i++)
        if ((why = pkg_check_about_text("AUTHOR", out->authors.v[i], 80)) != NULL) return refuse_c(20, "%s", why);
    for (i = 0; i < out->description.n; i++)
        if ((why = pkg_check_about_text("a DESCRIPTION line", out->description.v[i], 1000)) != NULL) return refuse_c(20, "%s", why);
    for (i = 0; i < out->changes.n; i++)
        if ((why = pkg_check_about_text("a CHANGES line", out->changes.v[i], 1000)) != NULL) return refuse_c(20, "%s", why);
    return 0;
}

/* An archive's SHA-256 and size, kept in <archive>.sha256 beside it
 * ("sha256 <hex> <size> <mtime>") so that a hundred packages published from
 * one nightly read it once. Recomputed when the archive changes. */
static int archive_digest(const char *archive, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size)
{
    struct pkg_fs_id id;
    char *sp, line[200], want[120];
    unsigned char *buf = NULL;
    size_t len = 0, k;
    int n;

    if (pkg_fs_identity(archive, &id) != 0 || !id.exists) return -1;
    sp = malloc(strlen(archive) + 8);
    if (sp == NULL) return -1;
    snprintf(sp, strlen(archive) + 8, "%s.sha256", archive);
    n = snprintf(want, sizeof want, " %llu %lld\n", id.size, id.mtime_s);
    if (pkg_fs_read(sp, &buf, &len) == 0 && len == 7 + PKG_SHA256_HEXLEN + (size_t)n
        && memcmp(buf, "sha256 ", 7) == 0 && memcmp(buf + 7 + PKG_SHA256_HEXLEN, want, (size_t)n) == 0) {
        for (k = 0; k < PKG_SHA256_HEXLEN; k++) hex[k] = (char)buf[7 + k];
        hex[PKG_SHA256_HEXLEN] = '\0';
        *size = id.size;
        free(buf);
        free(sp);
        return 0;
    }
    free(buf);
    tr("hashing %s", archive);
    {
        const char *base = strrchr(archive, '/');
        int hashed;
        ev_begin(PKG_PHASE_PREPARE, PKG_OBJECT_ARCHIVE, base != NULL ? base + 1 : archive, NULL, NULL,
                 PKG_MEASURE_BYTES_HASHED, id.size, 1);
        hashed = file_digest(archive, hex, size, 1) == 0;
        ev_end(hashed ? PKG_OUTCOME_OK : PKG_OUTCOME_FAILED);
        if (!hashed) { free(sp); return -1; }
    }
    n = snprintf(line, sizeof line, "sha256 %s%s", hex, want);
    if (pkg_fs_write_atomic(sp, line, (size_t)n) != 0)
        warn("the archive's digest could not be kept in %s; it is computed again next time", sp);
    free(sp);
    return 0;
}

int build_package(const struct pkg_options *a, struct built *out)
{
    static struct pkg_options eff;   /* the options with what INFO gave filled in */
    int info_files = 0;
    struct drawer d;
    struct pkg_writer *w;
    char vname[65], vver[64];
    const char *from = NULL, *name, *version, *arch, *kind, *why;
    const char *kind_src, *deps_src;
    char arch_file[1024], arch_prefix[1024];
    char ikind[32], ideps[1024], ifrom[160], iconf[4096];
    const char *conf_src;
    char payload[PKG_SHA256_HEXLEN + 1];
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&d, 0, sizeof d);
    pkg_manifest_init(&out->m);
    if (a->target == NULL)
        return refuse_c(20, "name the drawer to package");
    if (have_info) { pkg_pkginfo_free(&cur_info); have_info = 0; }
    if (a->info != NULL) {
        /* A keyword on the line wins over the file; the file wins over the
         * readme and over the last version published. */
        if (read_info(a) != 0)
            return 1;
        eff = *a;
        if (eff.name == NULL && cur_info.name) eff.name = cur_info.name;
        if (eff.version == NULL && cur_info.version) eff.version = cur_info.version;
        if (eff.kind == NULL && cur_info.kind) eff.kind = cur_info.kind;
        if (eff.depends == NULL && cur_info.depends && cur_info.depends[0]) eff.depends = cur_info.depends;
        if (eff.files == NULL && cur_info.files.n) eff.files = cur_info.files_line;
        if (eff.config == NULL && cur_info.config.n) eff.config = cur_info.config_line;
        info_files = eff.files == cur_info.files_line && cur_info.files.n > 0;
        snprintf(out->info_from, sizeof out->info_from, "%s", info_shown);
        {
            /* what of it was used, for PUBLISH to say where the fields came from */
            const struct { int given; const char *word; } gave[] = {
                { eff.name == cur_info.name && cur_info.name != NULL, "name" },
                { eff.version == cur_info.version && cur_info.version != NULL, "version" },
                { eff.kind == cur_info.kind && cur_info.kind != NULL, "kind" },
                { eff.depends == cur_info.depends && cur_info.depends != NULL, "dependencies" },
                { info_files, "files" },
                { eff.config == cur_info.config_line && cur_info.config.n > 0, "config" }
            };
            size_t g, at = 0;
            for (g = 0; g < sizeof gave / sizeof gave[0]; g++)
                if (gave[g].given && at < sizeof out->info_fields)
                    at += (size_t)snprintf(out->info_fields + at, sizeof out->info_fields - at,
                                           "%s%s", at ? ", " : "", gave[g].word);
        }
        a = &eff;
    }
    kind_src = a->kind;
    deps_src = a->depends;
    conf_src = a->config;
    if (a->upstream != NULL && !pkg_archive_split(a->target, arch_file, sizeof arch_file, arch_prefix,
                                                  sizeof arch_prefix))
        return refuse_c(20, "UPSTREAM says where the archive a package's files stay in is published; "
                        "a drawer has no archive. Publish from \"<archive>!/<top dir>\"");
    if (pkg_archive_split(a->target, arch_file, sizeof arch_file, arch_prefix, sizeof arch_prefix)) {
        const char *base = strrchr(arch_file, '/');
        char src[2200];
        base = base ? base + 1 : arch_file;
        if (!pkg_fs_exists(arch_file))
            return refuse_c(20, "no archive at \"%s\"", arch_file);
        d.root = NULL;                  /* no file system behind these files */
        if (load_archive(&d, arch_file, arch_prefix, a->files) != 0) {
            refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the archive");
            drawer_free(&d);
            return 1;
        }
        snprintf(src, sizeof src, "%s!/%s", base, arch_prefix);
        pkg_manifest_set(&out->m.source, src);
        if (a->upstream != NULL) {
            char hex[PKG_SHA256_HEXLEN + 1];
            unsigned long long asz;
            size_t q;
            const char *u = a->upstream;
            if (strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0) {
                drawer_free(&d);
                return refuse_c(20, "UPSTREAM must be the http or https URL the archive is downloaded from");
            }
            for (q = 0; u[q]; q++)
                if ((unsigned char)u[q] <= ' ' || u[q] == 0x7F) {
                    drawer_free(&d);
                    return refuse_c(20, "UPSTREAM holds a space or a control character; write spaces in a URL as %%20");
                }
            if (archive_digest(arch_file, hex, &asz) != 0) {
                drawer_free(&d);
                return refuse_c(17, "cannot read %s to take its SHA-256", arch_file);
            }
            out->m.archive_sha = pkg_strdup(hex);
            out->m.archive_url = pkg_strdup(u);
            out->m.archive_size = asz;
            if (out->m.archive_sha == NULL || out->m.archive_url == NULL) {
                drawer_free(&d);
                return refuse("out of memory");
            }
        }
        tr("drawer from %s: %lu files under %s", arch_file, (unsigned long)d.n,
           arch_prefix[0] ? arch_prefix : "its top");
    } else {
        if (!pkg_fs_is_dir(a->target))
            return refuse_c(20, "\"%s\" is not a directory", a->target);
        d.root = a->target;
        d.files = a->files;
        ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_FILE, a->target, NULL, NULL, PKG_MEASURE_NONE, 0, 0);
        if (pkg_fs_walk(a->target, load_one, leave_out, &d, &out->skipped, d.err, sizeof d.err) != 0) {
            refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
            ev_end(PKG_OUTCOME_FAILED);
            drawer_free(&d);
            return 1;
        }
        ev_end(PKG_OUTCOME_OK);
    }
    if (info_files) {
        /* Every path the .pkginfo claims must be there: a port that moved a
         * file would otherwise publish a package quietly missing it. */
        size_t f;
        for (f = 0; f < cur_info.files.n; f++) {
            const char *want = cur_info.files.v[f];
            size_t wl = strlen(want), k;
            int found = 0;
            for (k = 0; k < d.n && !found; k++)
                found = strcmp(d.v[k].rel, want) == 0
                        || (strncmp(d.v[k].rel, want, wl) == 0 && d.v[k].rel[wl] == '/');
            if (!found) {
                refuse_c(11, "%s names \"%s\" in Files, and \"%s\" holds no such file or "
                         "drawer; correct the .pkginfo, or publish what it describes",
                         info_shown, want, a->target);
                drawer_free(&d);
                return 1;
            }
        }
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->target);
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);
    if (drawer_attrs(&d) != 0) { drawer_free(&d); return 1; }
    {
        size_t t;
        tr("drawer %s: %lu files, %lu left out", a->target, (unsigned long)d.n,
           (unsigned long)d.nleft);
        for (t = 0; t < d.nleft; t++)
            tr("left out %s, host metadata", d.left_out[t]);
    }
    out->left_out = d.left_out;          /* reported by PUBLISH, freed with out */
    out->nleft = d.nleft;
    d.left_out = NULL;
    d.nleft = 0;

    name = a->name;
    version = a->version;
    check_cookie_names(&d);
    {
        char seen[600];
        int got = find_ver(&d, name, vname, sizeof vname, vver, sizeof vver, &from,
                           seen, sizeof seen);
        int needed = name == NULL || version == NULL;
        if (!needed && got < 0)
            got = 0;
        if (got < 0 && a->build != NULL) {
            got = 0;        /* a nightly component whose cookies name other programs: 0+build */
            tr("no $VER cookie of %s among %s: its version is 0+%s", name ? name : "it", seen, a->build);
        }
        if (got < 0 && name != NULL) {
            drawer_free(&d);
            return refuse_c(20, "none of the drawer's $VER cookies is %s: %s. Add VERSION",
                            name, seen);
        }
        if (got < 0) {
            drawer_free(&d);
            return refuse_c(20, "the $VER cookies in the drawer name different programs or "
                            "versions: %s. Say which this package is with NAME and VERSION", seen);
        }
        if (got > 0) {
            char vmain[64];
            /* the build a nightly adds (+20260918) is not the program's own */
            snprintf(vmain, sizeof vmain, "%.*s", version ? (int)strcspn(version, "+") : 0,
                     version ? version : "");
            if (version != NULL && pkg_version_cmp(vmain, vver) != 0)
                warn("VERSION %s, but the $VER cookie in %s says %s: is this the build "
                     "you meant to ship?", version, from, vver);
            if (version == NULL)
                snprintf(out->ver_from, sizeof out->ver_from, "%s", from);
            if (name == NULL)
                snprintf(out->name_from, sizeof out->name_from, "%s", from);
            snprintf(out->cookie_ver, sizeof out->cookie_ver, "%s", vver);
            snprintf(out->cookie_file, sizeof out->cookie_file, "%s", from);
            if (name == NULL) name = vname;
            if (version == NULL) version = vver;
        }
    }
    {
        char seen[1200];
        const char *found_arch = NULL, *afrom = NULL;
        int got = drawer_arch(&d, &found_arch, &afrom, seen, sizeof seen);
        /* A boot package carries the loader stages the firmware runs (GRUB's
         * i386-pc for an x86_64 system), an SDK the libraries of each target
         * it compiles for: their CPU is the machine ARCH names. */
        const char *k0 = a->kind;
        int several_ok;
        if (k0 == NULL && a->arch != NULL && inherit_ix != NULL && name != NULL
            && inherited(name, a->arch, ikind, sizeof ikind, ideps, sizeof ideps, ifrom, sizeof ifrom,
                         iconf, sizeof iconf))
            k0 = ikind;
        several_ok = k0 != NULL && a->arch != NULL && (strcmp(k0, "boot") == 0 || strcmp(k0, "sdk") == 0);
        if (got < 0 && several_ok) {
            tr("executables for several CPUs (%s), accepted for KIND %s, ARCH %s", seen, k0, a->arch);
            got = 0;
        } else if (got < 0) {
            drawer_free(&d);
            return refuse_c(20, "the drawer holds executables for different CPUs: %s. One "
                            "package is one architecture: publish each separately (FILES picks "
                            "the paths). A boot or sdk package may hold several, when KIND and "
                            "ARCH name the kind and the machine it is for", seen);
        }
        if (a->arch != NULL && got > 0 && strcmp(a->arch, found_arch) != 0 && !several_ok) {
            /* afrom points into the drawer: refuse before freeing it. */
            refuse_c(20, "ARCH %s, but %s is built for %s", a->arch, afrom, found_arch);
            drawer_free(&d);
            return 1;
        }
        arch = a->arch ? a->arch : got > 0 ? found_arch : "generic";
        tr("architecture %s: %s", arch, a->arch ? "given with ARCH"
           : got > 0 ? afrom : "no executable header found");
        if (a->arch == NULL && got > 0)
            snprintf(out->arch_from, sizeof out->arch_from, "%s", afrom);
    }
    if (inherit_ix != NULL && name != NULL && (a->kind == NULL || a->depends == NULL || a->config == NULL)
        && inherited(name, arch, ikind, sizeof ikind, ideps, sizeof ideps, ifrom, sizeof ifrom,
                     iconf, sizeof iconf)) {
        /* A new version is the same package: what it is and what it needs
         * come from the last one published, unless the command says. */
        if (a->kind == NULL) {
            kind_src = ikind;
            snprintf(out->kind_from, sizeof out->kind_from, "%s", ifrom);
        }
        if (a->depends == NULL) {
            deps_src = ideps;
            snprintf(out->deps_from, sizeof out->deps_from, "%s", ifrom);
        }
        if (a->config == NULL && iconf[0]) {
            conf_src = iconf;
            snprintf(out->config_from, sizeof out->config_from, "%s", ifrom);
        }
    }
    if (kind_src == NULL && strcmp(verb_name, "publish") == 0) {
        drawer_free(&d);
        return refuse_c(20, "no KIND given, and no version of %s is published in this channel to "
                        "take it from; pkg does not guess what a package is. KIND image for "
                        "a program people run, installed as one volume to mount; application "
                        "for a program installed as loose files; library, device (handlers "
                        "too), class, font or catalog for what other programs use, in Libs, "
                        "Devs or L, Classes, Fonts, Locale; startup or boot for what the system "
                        "runs as it starts; data, sdk or slave", name ? name : "this package");
    }
    kind = kind_src ? kind_src : "application";
    if (out->m.source != NULL && strcmp(kind, "image") == 0) {
        drawer_free(&d);
        return refuse_c(20, "files taken from an archive install at the paths the archive gives "
                        "them; an image is made from a drawer. Use another kind, or unpack the "
                        "drawer and publish it as an image");
    }
    if (name == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no NAME given and no $VER: cookie found; add NAME <name>");
    }
    if (a->build != NULL) {
        /* A nightly: the component's own version, and the build it came from. */
        static char with_build[160];
        size_t k;
        for (k = 0; a->build[k] && ((a->build[k] >= '0' && a->build[k] <= '9') || a->build[k] == '.'); k++) ;
        if (a->build[0] == '\0' || a->build[k] != '\0') {
            drawer_free(&d);
            return refuse_c(20, "BUILD must be dotted numbers, such as the date of the nightly: 20260918");
        }
        snprintf(with_build, sizeof with_build, "%.*s+%s", version ? (int)strcspn(version, "+") : 1,
                 version ? version : "0", a->build);
        version = with_build;
    }
    if (version == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no VERSION given and no $VER: cookie found; add VERSION <version>");
    }
    if ((why = pkg_check_name(name)) || (why = pkg_check_version(version))
        || (why = pkg_check_arch(arch))) {
        drawer_free(&d);
        return refuse_c(20, "%s", why);
    }
    if ((why = pkg_check_kind(kind)) != NULL) {
        static const char *const alias[][2] = {
            { "handler", "device" }, { "filesystem", "device" }, { "driver", "device" },
            { "lib", "library" }, { "libs", "library" }, { "mui", "class" },
            { "datatype", "class" }, { "gadget", "class" }, { "locale", "catalog" },
            { "program", "image" }, { "tool", "image" }, { "app", "image" }, { "game", "image" },
            { "headers", "sdk" }, { "docs", "data" }
        };
        size_t al;
        for (al = 0; al < sizeof alias / sizeof alias[0]; al++)
            if (ascii_casecmp(kind, alias[al][0]) == 0) {
                drawer_free(&d);
                return refuse_c(20, "no kind \"%s\"; the kind for that is %s (a program people "
                                "run is image, or application to install it as loose files)",
                                kind, alias[al][1]);
            }
        drawer_free(&d);
        return refuse_c(20, "%s", why);
    }

    pkg_manifest_set(&out->m.name, name);
    pkg_manifest_set(&out->m.version, version);
    pkg_manifest_set(&out->m.architecture, arch);
    pkg_manifest_set(&out->m.kind, kind);
    if (add_depends(&out->m, deps_src) != 0) {
        drawer_free(&d);
        return 1;
    }
    if (a->config != NULL && strcmp(kind, "image") == 0 && ascii_casecmp(a->config, "none") != 0) {
        drawer_free(&d);
        return refuse_c(20, "CONFIG is for files installed loose, which a person may edit; an "
                        "image is mounted read-only and never edited in place");
    }
    if (strcmp(kind, "image") == 0) {
        /* The manifest names what is inside the image too, so that a new
         * version can be compared with the last one file by file. */
        for (i = 0; i < d.n; i++) {
            char hex[PKG_SHA256_HEXLEN + 1];
            pkg_sha256_hex(d.v[i].data, d.v[i].len, hex);
            if (pkg_manifest_add_content(&out->m, d.v[i].rel, hex,
                                         (unsigned long long)d.v[i].len) != 0) {
                drawer_free(&d);
                return refuse("out of memory");
            }
            out->m.content[out->m.ncontent - 1].prot = d.v[i].prot;
            if (d.v[i].comment != NULL)
                out->m.content[out->m.ncontent - 1].comment = pkg_strdup(d.v[i].comment);
        }
    }
    if (strcmp(kind, "image") == 0 && to_image(&d, name) != 0) {
        drawer_free(&d);
        return 1;
    }

    w = pkg_writer_new();
    if (w == NULL) { drawer_free(&d); return refuse("out of memory"); }
    for (i = 0; i < d.n; i++) {
        char hex[PKG_SHA256_HEXLEN + 1];
        if (d.v[i].pre) snprintf(hex, sizeof hex, "%s", d.v[i].pre_digest);
        else pkg_sha256_hex(d.v[i].data, d.v[i].len, hex);
        if (pkg_manifest_add_file(&out->m, d.v[i].rel, hex, (unsigned long long)d.v[i].len) != 0
            || (!d.v[i].pre && pkg_writer_add(w, d.v[i].rel, d.v[i].data, d.v[i].len) != PKG_OK)) {
            pkg_writer_free(w); drawer_free(&d);
            return refuse("out of memory");
        }
        out->m.files[out->m.nfiles - 1].prot = d.v[i].prot;
        if (d.v[i].comment != NULL)
            out->m.files[out->m.nfiles - 1].comment = pkg_strdup(d.v[i].comment);
    }
    {
        /* Provides: the libraries and devices it ships where LIBS: and DEVS:
         * look; then what its programs open that nothing here provides */
        struct pkg_strs opened = { NULL, 0 };
        for (i = 0; i < d.n; i++) {
            const char *rel = d.v[i].rel, *slash = strchr(rel, '/'), *bn = slash ? slash + 1 : "";
            size_t bl = strlen(bn);
            if (slash && !strchr(bn, '/')
                && ((((size_t)(slash - rel) == 4 && ascii_casecmp_n(rel, "Libs", 4) == 0) && bl > 8
                     && ascii_casecmp(bn + bl - 8, ".library") == 0)
                    || (((size_t)(slash - rel) == 4 && ascii_casecmp_n(rel, "Devs", 4) == 0) && bl > 7
                        && ascii_casecmp(bn + bl - 7, ".device") == 0))
                && pkg_check_libname(bn) == NULL && !strs_has_nocase(&out->m.provides, bn))
                pkg_strs_add(&out->m.provides, bn);
        }
        if (out->m.provides.n > 1)
            qsort(out->m.provides.v, out->m.provides.n, sizeof *out->m.provides.v, strcmp_p);
        for (i = 0; i < d.n; i++) {
            struct pkg_strs mine = { NULL, 0 }, missing = { NULL, 0 };
            size_t k, q;
            if (d.v[i].pre || file_arch(d.v[i].data, d.v[i].len) == NULL)
                continue;           /* only executables; an archive's files are not read here */
            lib_names(d.v[i].data, d.v[i].len, &mine);
            for (k = 0; k < mine.n; k++) {
                const char *ln = mine.v[k];
                int given = strs_has_nocase(&out->m.provides, ln) || is_system_lib(ln);
                for (q = 0; !given && q < out->m.ndeps; q++) {
                    struct pkg_manifest dm;
                    if (last_published(out->m.deps[q].name, arch, &dm)) {
                        given = strs_has_nocase(&dm.provides, ln);
                        pkg_manifest_free(&dm);
                    }
                }
                if (!given) pkg_strs_add(&missing, ln);
            }
            if (missing.n) {
                char list[600];
                size_t at = 0;
                for (k = 0; k < missing.n && at < sizeof list; k++)
                    at += (size_t)snprintf(list + at, sizeof list - at, "%s%s",
                                           k == 0 ? "" : k + 1 == missing.n ? " and " : ", ", missing.v[k]);
                warn("%s opens %s and no dependency provides %s: DEPENDS on the package that "
                     "ships %s, or ship %s in Libs", d.v[i].rel, list, missing.n == 1 ? "it" : "them",
                     missing.n == 1 ? "it" : "them", missing.n == 1 ? "it" : "them");
            }
            pkg_strs_free(&mine);
            pkg_strs_free(&missing);
        }
        pkg_strs_free(&opened);
    }
    drawer_free(&d);
    if (conf_src != NULL && strcmp(kind, "image") != 0 && ascii_casecmp(conf_src, "none") != 0
        && mark_config(&out->m, conf_src, out->config_from[0] == '\0') != 0) {
        pkg_writer_free(w);
        return 1;
    }
    for (i = 0; i < out->m.nfiles; i++)
        out->nconfig += out->m.files[i].config ? 1 : 0;
    if (compose_about(a, name, arch, &out->m.about, out->about_from, sizeof out->about_from) != 0) {
        pkg_writer_free(w);
        return 1;
    }
    {
        /* ICON and SCREENSHOT name files of the package, or of its image */
        const struct pkg_about *ab = &out->m.about;
        size_t k, f, np = (ab->icon ? 1 : 0) + ab->screenshots.n;
        for (k = 0; k < np; k++) {
            const char *pic = (ab->icon && k == 0) ? ab->icon : ab->screenshots.v[k - (ab->icon ? 1 : 0)];
            int found = 0;
            for (f = 0; f < out->m.nfiles && !found; f++) found = strcmp(out->m.files[f].path, pic) == 0;
            for (f = 0; f < out->m.ncontent && !found; f++) found = strcmp(out->m.content[f].path, pic) == 0;
            if (!found) {
                pkg_writer_free(w);
                return refuse_c(20, "%s names \"%s\", which is no file of this package; give the "
                                "path as the package installs it", (ab->icon && k == 0) ? "ICON" : "SCREENSHOT", pic);
            }
        }
    }
    if (pkg_writer_finish(w, &out->pkg, &out->pkg_len) != PKG_OK) {
        pkg_writer_free(w);
        return refuse_c(17, "cannot assemble the container");
    }
    pkg_writer_free(w);
    pkg_sha256_hex(out->pkg, out->pkg_len, payload);
    if (out->m.source == NULL)          /* an archive's files are fetched from it */
        pkg_manifest_set(&out->m.payload, payload);
    if (pkg_manifest_emit(&out->m, &out->text, &out->text_len) != 0)
        return refuse("out of memory");
    return 0;
}

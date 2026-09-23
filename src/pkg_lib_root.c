/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * A root: the machine it is for, its database, placing and removing files,
 * Amiga attributes, and dependencies.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- the machine a root is for ------------------------------------------ *
 *
 * A channel may hold one version of a program for several CPUs. The
 * operation picks among the entries built for the root's machine, and
 * `generic` ones. That machine is ARCH when given; else what the root
 * recorded at its first install of a CPU-specific package; else, when Pkg
 * runs on AROS, its own CPU; else unknown, and then a package offered for
 * more than one CPU is refused until ARCH says which. */

const char *target_arch;    /* NULL: any */
char root_arch[32];

static const char *native_arch(void)
{
#if defined(__AROS__)
# if defined(__x86_64__)
    return "x86_64";
# elif defined(__aarch64__)
    return "aarch64";
# elif defined(__i386__)
    return "i386";
# elif defined(__arm__)
    return "arm";
# elif defined(__powerpc__) || defined(__PPC__)
    return "ppc";
# elif defined(__mc68000__) || defined(__m68k__)
    return "m68k";
# endif
#endif
    return NULL;
}

int arch_matches(const struct entry *e)
{
    return target_arch == NULL || strcmp(e->arch, target_arch) == 0
        || strcmp(e->arch, "generic") == 0;
}

int resolve_arch(const struct pkg_options *a)
{
    char *p = pkg_join(a->root, ".pkg/arch");
    unsigned char *buf;
    size_t len;
    root_arch[0] = '\0';
    target_arch = NULL;
    if (p != NULL && pkg_fs_exists(p) && pkg_fs_read(p, &buf, &len) == 0) {
        sscanf((const char *)buf, "%31s", root_arch);
        free(buf);
    }
    free(p);
    if (a->arch != NULL) {
        const char *why = pkg_check_arch(a->arch);
        if (why != NULL)
            return refuse_c(20, "ARCH \"%s\": %s", a->arch, why);
        if (root_arch[0] && strcmp(root_arch, a->arch) != 0)
            return refuse_c(20, "%s is a root for %s machines; ARCH %s names another", a->root,
                            root_arch, a->arch);
        target_arch = a->arch;
        tr("for %s machines: ARCH says so", target_arch);
    } else if (root_arch[0]) {
        target_arch = root_arch;
        tr("for %s machines: the root recorded it", target_arch);
    } else {
        target_arch = native_arch();
        tr("for %s", target_arch ? "this machine's CPU" : "any CPU: none is known yet");
    }
    return 0;
}

/* Record the root's machine once a CPU-specific package is installed. */
void record_arch(const char *root, const char *arch)
{
    char *p;
    if (root_arch[0] || strcmp(arch, "generic") == 0)
        return;
    p = pkg_join(root, ".pkg/arch");
    if (p != NULL) {
        char line[40];
        int n = snprintf(line, sizeof line, "%s\n", arch);
        if (pkg_fs_write_atomic(p, line, (size_t)n) == 0)
            tr("%s is now a root for %s machines", root, arch);
        free(p);
    }
}

/* With no machine known, a package offered for more than one CPU cannot be
 * chosen for. 1 when that is the case, the CPUs listed. */
int arch_ambiguous(const struct index *ix, const char *name, char *list, size_t len)
{
    size_t i, j, at = 0;
    int n = 0;
    list[0] = '\0';
    if (target_arch != NULL)
        return 0;
    for (i = 0; i < ix->n; i++) {
        int seen = 0;
        if (strcmp(ix->e[i].name, name) != 0 || strcmp(ix->e[i].arch, "generic") == 0)
            continue;
        for (j = 0; j < i; j++)
            if (strcmp(ix->e[j].name, name) == 0 && strcmp(ix->e[j].arch, ix->e[i].arch) == 0)
                seen = 1;
        if (seen)
            continue;
        n++;
        if (at + 40 < len)
            at += (size_t)snprintf(list + at, len - at, "%s%s", at ? ", " : "", ix->e[i].arch);
    }
    return n > 1;
}

/* The key this root pinned for a package, if any. */
int pinned_key(const char *root, const char *name, char out[65])
{
    char *p = root ? root_path(root, "keys", name) : NULL;
    unsigned char *buf;
    size_t len;
    int got = 0;
    if (p != NULL && pkg_fs_read(p, &buf, &len) == 0) {
        if (len >= 64u) {
            memcpy(out, buf, 64);
            out[64] = '\0';
            got = 1;
        }
        free(buf);
    }
    free(p);
    return got;
}

/* Pick the entry for name: EXACT when version is given, else the highest.
 *
 * With several channels listed, each is asked in turn and the answers are
 * weighed together. Two channels offering the same package under different
 * keys is the case adding a channel must never settle by itself: it is
 * refused, naming both, unless this root has already pinned a key for that
 * name, in which case only the channels whose chosen version carries that
 * key count. Among what is left the newest version wins, and list order
 * breaks a tie. */
static int pick_quiet;     /* the version was already chosen and traced */
int pick_refused;   /* pick refused; the caller must not say "not found" */
char pick_why[2400];/* the refusal's reason, for a caller that reports per package */

const struct entry *pick(const struct index *ix, const char *name, const char *version)
{
    const struct entry *best[PKG_MAX_CHANNELS];
    char signer[PKG_MAX_CHANNELS][65];
    const struct entry *p = NULL;
    size_t i, c, ncand = 0;
    char pin[65];
    int have_pin;

    pick_refused = 0;
    pick_why[0] = '\0';
    for (c = 0; c < PKG_MAX_CHANNELS; c++) {
        best[c] = NULL;
        signer[c][0] = '\0';
    }
    for (i = 0; i < ix->n; i++) {
        const struct entry *e = &ix->e[i];
        const struct entry **b;
        if (strcmp(e->name, name) != 0 || !arch_matches(e) || e->ch >= PKG_MAX_CHANNELS)
            continue;
        b = &best[e->ch];
        if (version) {
            if (pkg_version_cmp(e->version, version) == 0)
                *b = e;
        } else if (is_withdrawn(e)) {
            tr("skipping %s %s: withdrawn by its publisher", e->name, e->version);
        } else if (*b == NULL || pkg_version_cmp(e->version, (*b)->version) > 0) {
            *b = e;
        }
    }
    for (c = 0; c < PKG_MAX_CHANNELS; c++)
        if (best[c] != NULL)
            ncand++;
    if (ncand > 1) {
        /* Only then is a signature file read: one channel is the usual case
         * and must cost nothing. */
        have_pin = pinned_key(pick_root, name, pin);
        for (c = 0; c < PKG_MAX_CHANNELS; c++)
            if (best[c] != NULL)
                claimed_signer(chans[c], best[c]->digest, signer[c]);
        if (have_pin) {
            for (c = 0; c < PKG_MAX_CHANNELS; c++)
                if (best[c] != NULL && strcmp(signer[c], pin) != 0) {
                    tr("%s %s in %s is signed by %s, not by the key this root pinned: not counted",
                       name, best[c]->version, chans[c], signer[c][0] ? signer[c] : "no one");
                    best[c] = NULL;
                }
        } else {
            size_t first = PKG_MAX_CHANNELS;
            for (c = 0; c < PKG_MAX_CHANNELS; c++) {
                if (best[c] == NULL)
                    continue;
                if (first == PKG_MAX_CHANNELS) { first = c; continue; }
                if (strcmp(signer[first], signer[c]) != 0) {
                    pick_refused = 1;
                    snprintf(pick_why, sizeof pick_why,
                             "%s is offered by two channels under different keys, and this root "
                             "has pinned no key for it yet.\n"
                             "  %s %s in %s, signed by %s\n"
                             "  %s %s in %s, signed by %s\n"
                             "Nothing was changed. Taking either one would decide which of them "
                             "is the publisher, and adding a channel is never a way to replace "
                             "someone's package: only whoever requested this can say which key "
                             "is right, by asking the publisher by another route than these "
                             "channels. Install from that channel alone once, with CHANNEL, and "
                             "the root pins its key from then on",
                             name,
                             name, best[first]->version, chans[first],
                             signer[first][0] ? signer[first] : "no one",
                             name, best[c]->version, chans[c],
                             signer[c][0] ? signer[c] : "no one");
                    refuse_n(14, "ask-requester", "%s", pick_why);
                    return NULL;
                }
            }
        }
    }
    for (c = 0; c < PKG_MAX_CHANNELS; c++) {
        if (best[c] == NULL)
            continue;
        if (p == NULL || pkg_version_cmp(best[c]->version, p->version) > 0)
            p = best[c];        /* newest wins; list order breaks a tie */
    }
    if (pick_quiet)
        ;
    else if (p != NULL)
        tr("picked %s %s from %s: %s", p->name, p->version, chan_of(p),
           version ? "the version asked for" : "the highest offered");
    else
        tr("no channel offers %s%s%s", name, version ? " " : "", version ? version : "");
    return p;
}

/* Names in the channel close to `name`: one contains the other, or they
 * agree up to the first '.', '-' or '_' ("identify" and "identify.library"). */
static int close_name(const char *a, const char *b)
{
    size_t la = strcspn(a, ".-_"), lb = strcspn(b, ".-_");
    size_t shorter = strlen(a) < strlen(b) ? strlen(a) : strlen(b);
    if (strstr(a, b) != NULL || strstr(b, a) != NULL)
        return 1;
    /* Two edits in a short name is another name, not a typo: "hap" is two
     * edits from "hcat" and one from "happ". */
    if (pkg_name_edits(a, b) <= (shorter <= 4 ? 1 : 2))
        return 1;
    return la == lb && la >= 3 && strncmp(a, b, la) == 0;
}

void say_not_found(const struct index *ix, const char *name, const char *version,
                   const char *channel)
{
    size_t i, j, at = 0;
    char offered[512], other[200];
    size_t ot = 0;
    offered[0] = other[0] = '\0';
    for (i = 0; i < ix->n; i++) {
        if (strcmp(ix->e[i].name, name) != 0)
            continue;
        if (arch_matches(&ix->e[i])) {
            if (at + 72 < sizeof offered)
                at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                       at ? ", " : "; versions offered: ", ix->e[i].version);
        } else if (version != NULL && pkg_version_cmp(ix->e[i].version, version) == 0
                   && ot + 40 < sizeof other) {
            ot += (size_t)snprintf(other + ot, sizeof other - ot, "%s%s", ot ? ", " : "",
                                   ix->e[i].arch);
        }
    }
    if (ot > 0) {
        refuse_n(PKGRC_NOTFOUND, "report", "%s %s is published for %s only, not for %s machines%s",
                 name, version, other, target_arch, offered);
        return;
    }
    if (at == 0) {
        for (i = 0; i < ix->n && at + 72 < sizeof offered; i++) {
            int seen = 0;
            if (!close_name(ix->e[i].name, name))
                continue;
            for (j = 0; j < i; j++)
                if (strcmp(ix->e[j].name, ix->e[i].name) == 0) seen = 1;
            if (!seen) {
                at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                       at ? ", " : "; did you mean ", ix->e[i].name);
                rec_item("suggest", ix->e[i].name, "name", ix->e[i].name, NULL);
            }
        }
    }
    refuse_c(PKGRC_NOTFOUND, "%s%s%s is not in the channel %s%s", name,
             version ? " " : "", version ? version : "", channel, offered);
}

void fetched_free(struct fetched *f)
{
    pkg_manifest_free(&f->m);
    free(f->pkg);
    free(f->mtext);
}

/* Where the files of a Source package are read from, and where that is
 * kept: said once a command, with the path in full, since a person looking
 * for a 600 MB download or for a drawer to delete needs to know where it is. */
const char *opt_unpacked;   /* UNPACKED <dir>, for this call */
static const char *from_kind;      /* where locate_archive found the archive */
int told_source;

static void from_note(const char *kind, const char *path)
{
    char *cache;
    if (told_source)
        return;
    told_source = 1;
    cache = pkg_cache_dir();
    if (machine) {
        kv("archive-from", "%s %s", kind, path);
        if (cache != NULL) kv("cache", "%s", cache);
    } else if (strcmp(kind, "download") == 0 && cache != NULL) {
        /* Where a downloaded archive is kept, said once. What pkg is doing
         * is the step's own sentence, printed before the work starts. */
        say_kind(PKG_LINE_NOTE, "%s\n", "the cache is %s; PKG_CACHE names another place for it",
                 cache);
    }
    free(cache);
}

struct arch_fetch {
    const struct pkg_manifest *m;
    const char    *prefix;
    unsigned char **data;           /* per manifest file */
    size_t        *len, *cap;
    long           cur;
    long           oversize;        /* a file longer than its manifest says, or -1 */
};

static int af_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct arch_fetch *af = (struct arch_fetch *)ctx;
    size_t pl = strlen(af->prefix), i;
    const char *rel = e->path;
    if (e->is_dir) return 0;
    if (pl) {
        if (strncmp(e->path, af->prefix, pl) != 0 || e->path[pl] != '/') return 0;
        rel = e->path + pl + 1;
    }
    for (i = 0; i < af->m->nfiles; i++)
        if (af->data[i] == NULL && strcmp(af->m->files[i].path, rel) == 0) {
            af->cur = (long)i;
            return 1;
        }
    return 0;
}

static int af_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct arch_fetch *af = (struct arch_fetch *)ctx;
    size_t i = (size_t)af->cur;
    (void)e;
    /* Never hold more than the signed manifest says the file is: an archive
     * claiming more is refused as soon as it passes that size. */
    if (af->len[i] + len > af->m->files[i].size) {
        af->oversize = (long)i;
        return -1;
    }
    if (af->len[i] + len + 1 > af->cap[i]) {
        size_t nc = af->cap[i] ? af->cap[i] : 4096;
        unsigned char *g;
        while (nc < af->len[i] + len + 1) nc *= 2;
        g = (unsigned char *)realloc(af->data[i], nc);
        if (g == NULL) return -1;
        af->data[i] = g;
        af->cap[i] = nc;
    }
    if (len) memcpy(af->data[i] + af->len[i], buf, len);
    af->len[i] += len;
    return 0;
}

/* A package whose files are in someone else's archive: take them out of it
 * and assemble the container the rest of the install reads, so every file
 * is checked against the signed manifest the same way. */
/* Where to read a Source archive. A local channel's own copy first, as its
 * publisher left it. Then, when the manifest says where the archive is
 * published upstream: a copy downloaded before, in the cache under its
 * SHA-256; else a download from that URL, kept only when its size and
 * SHA-256 are the ones signed. Last, the channel's copy over the network.
 * Each file is checked against its own digest when it is read, whatever the
 * archive's origin. NULL with the reason given. */
static char *locate_archive(const char *channel, const struct pkg_manifest *m, const char *an,
                            const char *what)
{
    char *ap, *cache, *dir, *dest, hex[PKG_SHA256_HEXLEN + 1], rel[1100];
    char upstream_error[sizeof net_err];
    unsigned long long size = 0;
    int rc;

    if (!is_url(channel) || m->archive_sha == NULL) {
        ap = archive_path(channel, an);
        if (ap != NULL && pkg_fs_exists(ap)) {
            from_kind = "channel";
            return ap;
        }
        if (m->archive_sha == NULL) {
            refuse_c(11, "%s comes from the archive %s, which the channel does not have (expected "
                     "at %s)", what, an, ap ? ap : "archives/");
            free(ap);
            return NULL;
        }
        free(ap);
    }
    cache = pkg_cache_dir();
    if (cache == NULL) { refuse("out of memory"); return NULL; }
    snprintf(rel, sizeof rel, "upstream/%s", m->archive_sha);
    dir = pkg_join(cache, rel);
    free(cache);
    dest = dir ? pkg_join(dir, an) : NULL;
    if (dest == NULL) { free(dir); refuse("out of memory"); return NULL; }
    if (pkg_fs_exists(dest)) {
        tr("%s: the archive %s is in the cache", what, an);
        from_kind = "cache";
        free(dir);
        return dest;
    }
    pkg_fs_mkdirs(dir);
    free(dir);
    if (!machine)
        say_kind(PKG_LINE_NOTE, "%s\n", "downloading %s (%llu MB) from %s, once for every package it holds", an,
            (m->archive_size + 524288ull) / 1048576ull, m->archive_url);
    rc = net_get_watched(m->archive_url, dest, net_err, sizeof net_err);
    if (rc == 0 && file_digest(dest, hex, &size) == 0
        && size == m->archive_size && strcmp(hex, m->archive_sha) == 0) {
        tr("%s: downloaded %s, %llu bytes, SHA-256 as signed", what, m->archive_url, size);
        from_kind = "download";
        return dest;
    }
    if (rc == 0) {
        pkg_fs_unlink(dest);
        refuse_c(12, "the archive downloaded from %s is not the one %s was signed with: %llu bytes "
                 "and SHA-256 %s, where the manifest says %llu and %s. It was deleted; nothing was "
                 "installed", m->archive_url, what, size, hex, m->archive_size, m->archive_sha);
        free(dest);
        return NULL;
    }
    pkg_fs_unlink(dest);
    snprintf(upstream_error, sizeof upstream_error, "%s",
             rc == 1 ? "not found or unavailable" : net_err[0] ? net_err : "download failed");
    tr("%s: %s: %s", what, m->archive_url, upstream_error);
    if (is_url(channel)) {
        /* the channel may carry a copy of its own */
        int missing;
        net_err[0] = '\0';
        ap = archive_path(channel, an);
        if (ap != NULL && pkg_fs_exists(ap)) { free(dest); from_kind = "channel"; return ap; }
        missing = ap != NULL; /* A missing download also returns its cache path. */
        free(ap);
        refuse_c(rc == 1 && missing ? 11 : 17,
                 "%s comes from the archive %s. Upstream %s: %s; channel fallback "
                 "%s%sarchives/%s: %s. Retry when either location is available, or "
                 "use UNPACKED <dir> with the extracted archive",
                 what, an, m->archive_url, upstream_error, channel,
                 channel[strlen(channel) - 1] == '/' ? "" : "/", an,
                 missing ? "not found or unavailable" : net_err[0] ? net_err : "download failed");
    } else {
        refuse_c(rc == 1 ? 11 : 17, "%s comes from the archive %s, which could not be downloaded from %s: %s",
                 what, an, m->archive_url, upstream_error);
    }
    free(dest);
    return NULL;
}

/* The block map of an archive, kept beside it as <archive>.pkgmap. Its
 * header ties it to the archive it was made from: the size and time on this
 * machine, and the SHA-256 the signed manifest gives the archive. A map that
 * does not answer to all three is not read, and one a read cannot use is
 * thrown away and made again. */
static char *map_file(const char *archive)
{
    size_t n = strlen(archive) + 8;
    char *p = (char *)malloc(n);
    if (p != NULL) snprintf(p, n, "%s.pkgmap", archive);
    return p;
}

static int map_head(const char *archive, const struct pkg_manifest *m, char *head, size_t hl)
{
    struct pkg_fs_id id;
    if (pkg_fs_identity(archive, &id) != 0 || !id.exists)
        return -1;
    snprintf(head, hl, "pkgmap 1 %llu %lld %s\n", id.size, id.mtime_s,
             m->archive_sha ? m->archive_sha : "-");
    return 0;
}

static char *map_read(const char *archive, const struct pkg_manifest *m)
{
    char head[160], *mp = map_file(archive), *text = NULL;
    unsigned char *buf = NULL;
    size_t len = 0, hl;
    if (mp == NULL || map_head(archive, m, head, sizeof head) != 0) { free(mp); return NULL; }
    hl = strlen(head);
    if (pkg_fs_read(mp, &buf, &len) == 0 && len > hl && memcmp(buf, head, hl) == 0) {
        text = (char *)malloc(len - hl + 1);
        if (text != NULL) { memcpy(text, buf + hl, len - hl); text[len - hl] = '\0'; }
    }
    free(buf);
    free(mp);
    return text;
}

static void map_write(const char *archive, const struct pkg_manifest *m, const char *text)
{
    char head[160], *mp = map_file(archive), *all;
    size_t hl, tl;
    if (mp == NULL || text == NULL || map_head(archive, m, head, sizeof head) != 0) { free(mp); return; }
    hl = strlen(head);
    tl = strlen(text);
    all = (char *)malloc(hl + tl);
    if (all != NULL) {
        memcpy(all, head, hl);
        memcpy(all + hl, text, tl);
        if (pkg_fs_write_atomic(mp, all, hl + tl) != 0)
            tr("the block map could not be kept at %s; the archive is read whole every time", mp);
        else
            tr("wrote the block map %s, %lu bytes", mp, (unsigned long)(hl + tl));
        free(all);
    }
    free(mp);
}

static void map_drop(const char *archive)
{
    char *mp = map_file(archive);
    if (mp != NULL) { pkg_fs_unlink(mp); free(mp); }
}

/* What the fast path guarantees: every file it took out weighs and hashes
 * as the signed manifest says. Anything else and the map is wrong, not the
 * archive, so the archive is read whole instead. */
static int af_as_signed(const struct arch_fetch *af)
{
    size_t i;
    for (i = 0; i < af->m->nfiles; i++) {
        char hex[PKG_SHA256_HEXLEN + 1];
        if (af->data[i] == NULL || (unsigned long long)af->len[i] != af->m->files[i].size)
            return 0;
        pkg_sha256_hex(af->data[i], af->len[i], hex);
        if (strcmp(hex, af->m->files[i].digest) != 0)
            return 0;
    }
    return af->m->nfiles > 0;
}

static void af_clear(struct arch_fetch *af, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { free(af->data[i]); af->data[i] = NULL; af->len[i] = af->cap[i] = 0; }
    af->cur = 0;
    af->oversize = -1;
}

/* A person may unpack the archive themselves, with tar, and have Pkg read
 * the drawer instead. Every file is weighed and hashed against the signed
 * manifest before anything is written, exactly as from the archive. */
static int fetch_from_dir(const char *dir, struct fetched *f, const char *prefix, const char *what)
{
    struct pkg_writer *w = pkg_writer_new();
    size_t i;
    int rc = 1;

    if (w == NULL) { refuse("out of memory"); return 1; }
    for (i = 0; i < f->m.nfiles; i++) {
        char rel[2200], hex[PKG_SHA256_HEXLEN + 1], *full;
        unsigned char *buf = NULL;
        size_t len = 0;
        snprintf(rel, sizeof rel, "%s%s%s", prefix, prefix[0] ? "/" : "", f->m.files[i].path);
        full = pkg_join(dir, rel);
        if (full == NULL) { refuse("out of memory"); goto done; }
        if (pkg_fs_read(full, &buf, &len) != 0) {
            refuse_c(11, "the unpacked archive at %s lacks %s, which %s's signed manifest lists; "
                     "unpack the archive again, or leave UNPACKED off and let pkg read the "
                     "archive. Nothing was installed", dir, rel, what);
            free(full);
            goto done;
        }
        free(full);
        pkg_sha256_hex(buf, len, hex);
        if ((unsigned long long)len != f->m.files[i].size
            || strcmp(hex, f->m.files[i].digest) != 0) {
            refuse_c(12, "%s in the unpacked archive at %s is not the file %s was signed with: "
                     "%lu bytes and SHA-256 %s, where the manifest says %llu and %s. Nothing was "
                     "installed", rel, dir, what, (unsigned long)len, hex, f->m.files[i].size,
                     f->m.files[i].digest);
            free(buf);
            goto done;
        }
        if (pkg_writer_add(w, f->m.files[i].path, buf, len) != PKG_OK) {
            free(buf);
            refuse("out of memory");
            goto done;
        }
        free(buf);
    }
    if (pkg_writer_finish(w, &f->pkg, &f->pkg_len) != PKG_OK) { refuse("out of memory"); goto done; }
    tr("%s: %lu files taken from the unpacked archive at %s", what, (unsigned long)f->m.nfiles, dir);
    rc = 0;
done:
    pkg_writer_free(w);
    return rc;
}

/* The drawer an unpacked archive was put in: UNPACKED on the line, else the
 * one beside the cached archive, so it need not be named again. NULL when
 * there is none. */
static char *unpacked_dir(const struct pkg_manifest *m, const char *an)
{
    char *cache, *dir, rel[1200];
    if (opt_unpacked != NULL)
        return pkg_join(opt_unpacked, "");
    if (m->archive_sha == NULL)
        return NULL;
    cache = pkg_cache_dir();
    if (cache == NULL) return NULL;
    snprintf(rel, sizeof rel, "upstream/%s/%s.d", m->archive_sha, an);
    dir = pkg_join(cache, rel);
    free(cache);
    if (dir != NULL && pkg_fs_is_dir(dir))
        return dir;
    free(dir);
    return NULL;
}

static int fetch_from_archive(const char *channel, struct fetched *f, const char *what)
{
    char an[1024], prefix[1024], err[300];
    char *ap, *ud, *map = NULL;
    struct arch_fetch af;
    struct pkg_writer *w = NULL;
    size_t i, n = f->m.nfiles ? f->m.nfiles : 1;
    int rc = 1, used_map = 0;

    pkg_archive_split(f->m.source, an, sizeof an, prefix, sizeof prefix);
    ud = unpacked_dir(&f->m, an);
    if (ud != NULL) {
        from_note("unpacked", ud);
        tr("%s: reading its files out of the unpacked archive at %s", what, ud);
        rc = fetch_from_dir(ud, f, prefix, what);
        free(ud);
        return rc;
    }
    from_kind = "channel";
    ap = locate_archive(channel, &f->m, an, what);
    if (ap == NULL)
        return 1;
    memset(&af, 0, sizeof af);
    af.m = &f->m;
    af.prefix = prefix;
    af.oversize = -1;
    af.data = (unsigned char **)calloc(n, sizeof *af.data);
    af.len = (size_t *)calloc(n, sizeof *af.len);
    af.cap = (size_t *)calloc(n, sizeof *af.cap);
    if (af.data == NULL || af.len == NULL || af.cap == NULL) { refuse("out of memory"); goto done; }
    map = map_read(ap, &f->m);
    if (map != NULL) {
        tr("%s: %s has a block map; reading only the blocks its files lie in", what, ap);
        doing("reading", "the archive");
        rc = pkg_archive_read_mapped(ap, map, af_want, af_data, &af, err, sizeof err);
        did();
        if (rc == 0 && af_as_signed(&af)) {
            used_map = 1;
        } else {
            tr("%s: the block map of %s did not serve this read; reading the archive whole and "
               "writing it again", what, ap);
            af_clear(&af, n);
            map_drop(ap);
        }
        free(map);
        map = NULL;
    }
    from_note(used_map ? "map" : from_kind, ap);
    if (!used_map) {
        const char *base = strrchr(ap, '/');
        tr("%s: reading its files out of %s", what, ap);
        doing_bytes("reading the archive", base != NULL ? base + 1 : ap, file_bytes(ap));
        rc = pkg_archive_walk_map(ap, af_want, af_data, &af, &map, err, sizeof err);
        did();
        if (rc != 0) {
            if (af.oversize >= 0)
                refuse_c(12, "the archive %s holds a %s longer than the %llu bytes %s's signed manifest "
                         "gives it; it was not read further. Nothing was installed", ap,
                         f->m.files[af.oversize].path, f->m.files[af.oversize].size, what);
            else
                refuse_c(12, "the archive %s is refused: %s. Nothing was installed", ap, err[0] ? err : "unreadable");
            goto done;
        }
        map_write(ap, &f->m, map);
    }
    w = pkg_writer_new();
    if (w == NULL) { refuse("out of memory"); goto done; }
    for (i = 0; i < f->m.nfiles; i++) {
        if (af.data[i] == NULL) {
            refuse_c(12, "the archive %s lacks %s/%s, which %s's signed manifest lists. Nothing was "
                     "installed", ap, prefix, f->m.files[i].path, what);
            goto done;
        }
        if (pkg_writer_add(w, f->m.files[i].path, af.data[i], af.len[i]) != PKG_OK) {
            refuse("out of memory");
            goto done;
        }
    }
    if (pkg_writer_finish(w, &f->pkg, &f->pkg_len) != PKG_OK) { refuse("out of memory"); goto done; }
    tr("%s: %lu files taken from %s", what, (unsigned long)f->m.nfiles, an);
    rc = 0;
done:
    free(map);
    if (w) pkg_writer_free(w);
    for (i = 0; af.data && i < n; i++) free(af.data[i]);
    free(af.data); free(af.len); free(af.cap);
    free(ap);
    return rc;
}

int show_defers_archives;    /* set by SHOW, which checks archives in one pass each */

int fetch_manifest(const char *channel, const struct entry *e, struct fetched *f)
{
    char *mo = object_path(channel, e->digest, "manifest");
    char *so = object_path(channel, e->digest, "sig");
    char hex[PKG_SHA256_HEXLEN + 1], err[300], what[160];
    int rc = 1;

    memset(f, 0, sizeof *f);
    pkg_manifest_init(&f->m);
    snprintf(what, sizeof what, "%s %s", e->name, e->version);
    memcpy(f->digest, e->digest, sizeof f->digest);
    if (mo == NULL || so == NULL) {
        if (net_failed) refuse_c(17, "cannot read package metadata: %s", net_err);
        else refuse("out of memory");
        goto out;
    }

    /* 1. The manifest the index names, byte for byte. */
    if (pkg_fs_read(mo, &f->mtext, &f->mlen) != 0) {
        refuse_c(11, "the channel lists %s but its manifest is missing", what);
        goto out;
    }
    pkg_sha256_hex(f->mtext, f->mlen, hex);
    if (strcmp(hex, e->digest) != 0) {
        refuse_c(12, "the manifest of %s does not match the channel index; expected %s, found %s. "
               "Nothing was installed", what, e->digest, hex);
        goto out;
    }
    tr("%s: manifest %s matches the index", what, e->digest);
    /* 2. Signed. */
    if (check_sig(so, f->mtext, f->mlen, f->signer, what) != 0)
        goto out;
    tr("%s: signature verifies, signer %s", what, f->signer);
    /* 3. And saying what the index says it is. */
    if (pkg_manifest_parse((const char *)f->mtext, f->mlen, &f->m, err, sizeof err) != 0) {
        refuse_c(12, "the manifest of %s is refused: %s", what, err);
        goto out;
    }
    {
        size_t k;
        for (k = 0; k < f->m.ignored.n; k++)
            tr("%s: ignored the key %s, which pkg does not know", what, f->m.ignored.v[k]);
    }
    if (strcmp(f->m.name, e->name) != 0 || pkg_version_cmp(f->m.version, e->version) != 0
        || strcmp(f->m.architecture, e->arch) != 0
        || (f->m.payload == NULL) == (f->m.source == NULL)) {
        refuse_c(12, "the manifest of %s disagrees with the channel index about what it is", what);
        goto out;
    }
    rc = 0;
out:
    free(mo); free(so);
    return rc;
}

int fetch(const char *channel, const struct entry *e, struct fetched *f)
{
    char *po = NULL;
    char hex[PKG_SHA256_HEXLEN + 1], what[160];
    int rc = 1;
    if (fetch_manifest(channel, e, f) != 0) return 1;
    snprintf(what, sizeof what, "%s %s", e->name, e->version);
    /* 4. The payload the signed manifest names, or its files in the archive
     *    it names, each checked against the manifest when it is placed. */
    if (f->m.source != NULL) {
        /* SHOW checks archives afterwards, each read once for all its entries */
        rc = show_defers_archives ? 0 : fetch_from_archive(channel, f, what);
        goto out;
    }
    po = object_path(channel, f->m.payload, "pkg");
    if (po == NULL) { refuse("out of memory"); goto out; }
    if (pkg_fs_read(po, &f->pkg, &f->pkg_len) != 0) {
        refuse_c(11, "the channel lists %s but its payload is missing", what);
        goto out;
    }
    pkg_sha256_hex(f->pkg, f->pkg_len, hex);
    if (strcmp(hex, f->m.payload) != 0) {
        refuse_c(12, "the payload of %s does not match its signed manifest; expected %s, found %s. "
               "Nothing was installed", what, f->m.payload, hex);
        goto out;
    }
    tr("%s: payload %s matches the signed manifest", what, hex);
    rc = 0;
out:
    free(po);
    return rc;
}

/* ---- the root --------------------------------------------------------- */

char *root_path(const char *root, const char *dir, const char *name)
{
    char rel[160];
    snprintf(rel, sizeof rel, ".pkg/%s/%s", dir, name);
    return pkg_join(root, rel);
}
struct placement *placements;
static const char *placement_root;
const char *requested_at;

void placements_clear(void)
{
    while (placements) {
        struct placement *p = placements;
        placements = p->next;
        free(p->name); free(p->prefix); free(p->parent); free(p);
    }
    placement_root = NULL;
    requested_at = NULL;
}

static int placement_matches(const char *prefix, const char *path)
{
    size_t n = strlen(prefix);
    return ascii_casecmp_n(prefix, path, n) == 0
        && (path[n] == '/' || path[n] == '\0' || ascii_casecmp(path + n, ".info") == 0);
}

struct placement *placement_named(const char *name)
{
    struct placement *p;
    for (p = placements; p; p = p->next)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

static struct placement *placement_for(const char *root, const char *path)
{
    struct placement *p;
    if (!placement_root || strcmp(root, placement_root)) return NULL;
    for (p = placements; p; p = p->next)
        if (placement_matches(p->prefix, path)) return p;
    return NULL;
}

static const char *placement_relative(const struct placement *p, const char *path)
{
    const char *base = strrchr(p->prefix, '/');
    return path + (base ? (size_t)(base + 1 - p->prefix) : 0);
}

char *installed_path(const char *root, const char *path)
{
    struct placement *p = placement_for(root, path);
    return p ? pkg_join(p->parent, placement_relative(p, path)) : pkg_join(root, path);
}

static void installed_prune(const char *root, const char *path)
{
    struct placement *p = placement_for(root, path);
    if (p) {
        size_t n = strlen(p->prefix);
        if (path[n] == '/') {
            char *drawer = pkg_join(p->parent, placement_relative(p, p->prefix));
            if (drawer) pkg_fs_prune_empty_parents(drawer, path + n + 1);
            free(drawer);
        }
    } else pkg_fs_prune_empty_parents(root, path);
}

static int placement_absolute(const char *path)
{
    const char *c;
    if (!path || !*path || strchr(path, '\n') || strchr(path, '\r')) return 0;
    if (path[0] == '/' || (path[0] == '\\' && path[1] == '\\')) return 1;
#if defined(_WIN32)
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        return path[2] == '/' || path[2] == '\\';
#endif
    c = strchr(path, ':');
    return c && c > path && !memchr(path, '/', (size_t)(c - path));
}

int placement_available(const struct placement *p)
{
    char *drawer = installed_path(placement_root, p->prefix);
    char canonical[4096];
    int ok = pkg_fs_canonical_dir(p->parent, canonical, sizeof canonical) == 0
        && strcmp(canonical, p->parent) == 0
        && pkg_fs_path_is_link(p->parent) == 0
        && drawer && pkg_fs_path_is_link(drawer) == 0
        && (p->fresh || pkg_fs_is_dir(drawer));
    if (!ok) refuse_c(17, "the recorded destination for %s at %s is unavailable; mount or restore "
                       "that directory before continuing", p->name, drawer ? drawer : p->parent);
    free(drawer);
    return ok ? 0 : 1;
}

int placements_load(const char *root)
{
    char *dir;
    char **names = NULL;
    size_t n = 0, i;
    int rc = 0;
    placement_root = root;
    if (!root) return 0;
    dir = pkg_join(root, ".pkg/placements");
    if (!dir) return refuse("out of memory");
    if (!pkg_fs_exists(dir)) { free(dir); return 0; }
    if (pkg_fs_list(dir, &names, &n)) { free(dir); return refuse_c(17, "cannot read placements in %s", root); }
    for (i = 0; i < n && !rc; i++) {
        char *path = pkg_join(dir, names[i]), *text = NULL, *nl, *end;
        unsigned char *data = NULL;
        size_t len = 0;
        struct placement *p = NULL, *other;
        if (pkg_check_name(names[i]) || !path || pkg_fs_read(path, &data, &len)
            || !len || memchr(data, 0, len)) {
            rc = refuse_c(17, "cannot read placement for %s", names[i]);
        } else {
            text = malloc(len + 1);
            if (text) { memcpy(text, data, len); text[len] = 0; }
            nl = text ? strchr(text, '\n') : NULL;
            if (nl) *nl++ = 0;
            end = nl ? strchr(nl, '\n') : NULL;
            if (end) *end++ = 0;
            if (!nl || !end || *end || pkg_check_path(text) || !placement_absolute(nl))
                rc = refuse_c(17, "invalid placement record for %s", names[i]);
            else if (!(p = calloc(1, sizeof *p)) || !(p->name = pkg_strdup(names[i]))
                     || !(p->prefix = pkg_strdup(text)) || !(p->parent = pkg_strdup(nl)))
                rc = refuse("out of memory");
            if (!rc) {
                for (other = placements; other; other = other->next)
                    if (placement_matches(other->prefix, p->prefix)
                        || placement_matches(p->prefix, other->prefix)) {
                        rc = refuse_c(15, "overlapping placement records for %s and %s", p->name, other->name);
                        break;
                    }
            }
            if (!rc) {
                char *db = root_path(root, "db", p->name);
                p->fresh = db && !pkg_fs_exists(db);
                free(db);
                p->next = placements; placements = p; p = NULL;
            }
            if (p) { free(p->name); free(p->prefix); free(p->parent); free(p); }
        }
        free(path); free(data); free(text);
    }
    for (i = 0; i < n; i++) free(names[i]);
    free(names); free(dir);
    return rc;
}

static int placement_layout(const struct placement *p, const struct pkg_manifest *m)
{
    size_t i;
    if (!m->kind || strcmp(m->kind, "application") || !m->nfiles)
        return refuse_c(20, "AT requires an application package with one self-contained drawer: %s", m->name);
    for (i = 0; i < m->nfiles; i++)
        if (!placement_matches(p->prefix, m->files[i].path)
            || strcmp(p->prefix, m->files[i].path) == 0)
            return refuse_c(15, "%s ships %s outside its recorded application drawer %s; "
                            "its placement cannot be preserved", m->name, m->files[i].path, p->prefix);
    return 0;
}

/* Check every existing component, including the final file, before any
 * physical operation. A substituted link must never redirect ownership. */
static int placement_safe_file(const struct placement *p, const char *logical)
{
    const char *relative = placement_relative(p, logical);
    char *path = pkg_join(p->parent, relative);
    size_t i, start = strlen(p->parent);
    if (!path) return refuse("out of memory");
    for (i = start; ; i++) {
        int end = path[i] == 0;
        if (end || path[i] == '/') {
            char saved = path[i];
            int linked;
            path[i] = 0;
            linked = pkg_fs_path_is_link(path);
            path[i] = saved;
            if (linked != 0) {
                refuse_c(17, "unsafe or unreadable destination path %s for %s; restore the original directory layout",
                         path, p->name);
                free(path); return 1;
            }
        }
        if (end) break;
    }
    free(path);
    return 0;
}

/* Resolve the existing parent of a prospective file, including aliases.
 * Missing tail directories are appended after the canonical ancestor. */
char *placement_physical(const char *root, const char *logical)
{
    char *path = installed_path(root, logical), *probe, canonical[4096];
    size_t cut;
    if (!path) return NULL;
    probe = pkg_strdup(path);
    if (!probe) { free(path); return NULL; }
    cut = strlen(probe);
    while (cut) {
        char *slash = strrchr(probe, '/'), *colon = strrchr(probe, ':');
        if (slash && (!colon || slash > colon)) {
            cut = (size_t)(slash - probe);
            if (cut == 0) { probe[1] = 0; cut = 1; }
            else probe[cut] = 0;
        } else if (colon) { cut = (size_t)(colon + 1 - probe); probe[cut] = 0; }
        else { cut = 0; strcpy(probe, "."); }
        if (!pkg_fs_canonical_dir(probe, canonical, sizeof canonical)) {
            const char *tail = path + cut;
            char *out;
            while (*tail == '/') tail++;
            out = pkg_join(canonical, tail);
            free(path); free(probe); return out;
        }
        if (cut == 1 && probe[0] == '/') break;
        if (colon && cut == (size_t)(colon + 1 - probe)) break;
    }
    free(probe);
    return path;
}

int placement_check(const struct pkg_manifest *m)
{
    struct placement *p;
    size_t i;
    for (p = placements; p; p = p->next) {
        if (strcmp(m->name, p->name) == 0) {
            if (placement_available(p) || placement_layout(p, m)) return 1;
            for (i = 0; i < m->nfiles; i++)
                if (placement_safe_file(p, m->files[i].path)) return 1;
        } else {
            char *drawer = pkg_join(p->parent, placement_relative(p, p->prefix));
            if (!drawer) return refuse("out of memory");
            for (i = 0; i < m->nfiles; i++) {
                char *physical = placement_physical(placement_root, m->files[i].path);
                int overlap = placement_matches(p->prefix, m->files[i].path)
                    || (physical && (placement_matches(drawer, physical) || placement_matches(physical, drawer)));
                free(physical);
                if (overlap) {
                    free(drawer);
                    return refuse_c(15, "%s's path %s overlaps the drawer reserved for %s",
                                    m->name, m->files[i].path, p->name);
                }
            }
            free(drawer);
        }
    }
    return 0;
}

int load_installed(const char *root, const char *name, struct pkg_manifest *m,
                   int quiet)
{
    char *p = root_path(root, "db", name), err[300];
    unsigned char *buf;
    size_t len;
    int rc;

    pkg_manifest_init(m);
    if (p == NULL)
        return refuse("out of memory");
    if (pkg_fs_read(p, &buf, &len) != 0) {
        free(p);
        return quiet ? 1 : refuse_n(11, strcmp(verb_name, "upgrade") == 0 ? "use-install" : "check-name",
                                    "%s is not installed in %s", name, root);
    }
    free(p);
    rc = pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
    free(buf);
    if (rc != 0)
        return refuse_c(12, "the database entry for %s is damaged: %s", name, err);
    return 0;
}

/* The signer a channel entry's signature file claims, unverified: enough to
 * notice that two versions of one package name different publishers. */
int claimed_signer(const char *channel, const char *digest, char out[65])
{
    char *so = object_path(channel, digest, "sig");
    unsigned char *buf;
    size_t len;
    int ok = 0;
    if (so != NULL && pkg_fs_read(so, &buf, &len) == 0) {
        ok = len < 512u && sscanf((const char *)buf, "Signer: %64s", out) == 1;
        free(buf);
    }
    free(so);
    return ok;
}

/* The other keys that sign versions of `name` in the channel, listed in
 * `others`. The count of them. */
int other_signers(const struct index *ix, const char *name,
                  const char *signer, char *others, size_t olen)
{
    size_t i, at = 0;
    int n = 0;
    char claim[65], seen[8][65];
    others[0] = '\0';
    for (i = 0; i < ix->n; i++) {
        int j, dup = 0;
        if (strcmp(ix->e[i].name, name) != 0 || !claimed_signer(chan_of(&ix->e[i]), ix->e[i].digest, claim))
            continue;
        if (strcmp(claim, signer) == 0)
            continue;
        for (j = 0; j < n && j < 8; j++)
            if (strcmp(seen[j], claim) == 0) dup = 1;
        if (dup)
            continue;
        if (n < 8) snprintf(seen[n], sizeof seen[n], "%s", claim);
        n++;
        if (at + 90 < olen)
            at += (size_t)snprintf(others + at, olen - at, "%s%s (on %s)", at ? ", " : "",
                                   claim, ix->e[i].version);
    }
    return n;
}

/* The text a withdrawal signs: the entry it names, exactly. */
static int withdrawal_text(const struct entry *e, char *out, size_t len)
{
    return snprintf(out, len, "Withdrawn: %s %s %s\n", e->name, e->version, e->digest);
}

/* A withdrawal counts when its text names this entry and its signature
 * verifies with the key that signed the entry: only the publisher of a
 * version can withdraw it. */
static int withdrawal_valid(const char *channel, const struct entry *e)
{
    char *wo = object_path(channel, e->digest, "withdrawn"), *ws = NULL;
    char want[300], signer[65], entry_signer[65];
    unsigned char *buf = NULL;
    size_t len;
    int n = withdrawal_text(e, want, sizeof want), ok = 0, q = quiet, rc = refused_class;
    const char *nx = refused_next;

    /* The signature is only asked for once the withdrawal itself is there:
     * over a network each of these is a request. */
    if (wo != NULL && pkg_fs_exists(wo) && pkg_fs_read(wo, &buf, &len) == 0
        && len == (size_t)n && memcmp(buf, want, len) == 0
        && (ws = object_path(channel, e->digest, "withdrawn.sig")) != NULL) {
        quiet = 1;
        ok = check_sig(ws, buf, len, signer, "") == 0
             && claimed_signer(channel, e->digest, entry_signer)
             && strcmp(signer, entry_signer) == 0;
        quiet = q;
        refused_class = rc;
        refused_next = nx;
        if (!ok)
            tr("a withdrawal of %s %s is present but not signed by its publisher: ignored",
               e->name, e->version);
    }
    free(buf); free(wo); free(ws);
    return ok;
}

/* Whether this version was withdrawn, asked of the channel the first time
 * it is wanted and remembered on the entry. A withdrawal is two files that
 * are usually not there, so over the network each entry costs a request
 * that comes back "not found"; an index holds hundreds of entries and an
 * operation looks at a few, so they are asked for one at a time, not all at
 * once when the index is read. */
/* The channel's list of withdrawals, read the first time one matters. NULL
 * when the channel serves none or serves one this Pkg does not understand:
 * the caller then asks entry by entry, as it always did. */
static const char *withdrawals_of(const struct entry *e)
{
    static const char header[] = "Format: pkg-withdrawals 1\n";
    const char *channel = chan_of(e);
    unsigned char *buf = NULL;
    size_t len = 0;
    char *path;

    if (e->ch >= nchans)
        return NULL;
    if (wstate[e->ch] != 0)
        return wstate[e->ch] > 0 ? wlist[e->ch] : NULL;
    wstate[e->ch] = -1;                         /* whatever happens, asked once */
    path = chan_file(channel, "withdrawals");
    if (path != NULL && pkg_fs_exists(path) && pkg_fs_read(path, &buf, &len) == 0) {
        if (len >= sizeof header - 1 && memcmp(buf, header, sizeof header - 1) == 0) {
            size_t dl = len - (sizeof header - 1);
            wlist[e->ch] = (char *)malloc(dl + 1);
            if (wlist[e->ch] != NULL) {
                memcpy(wlist[e->ch], buf + sizeof header - 1, dl);
                wlist[e->ch][dl] = '\0';
            }
            if (wlist[e->ch] != NULL) {
                wstate[e->ch] = 1;
                tr("%s lists what it has withdrawn: asked once, not once per version", channel);
            }
        } else {
            tr("%s serves a withdrawals file this Pkg does not read: asking version by version",
               channel);
        }
    }
    free(buf);
    free(path);
    return wstate[e->ch] > 0 ? wlist[e->ch] : NULL;
}

/* The digest on a line of its own: a substring match would take a digest
 * that merely holds this one, which no 64-hex line can, but the line ends
 * are what make it exact. */
static int listed(const char *list, const char *digest)
{
    const char *p = list;
    size_t n = strlen(digest);
    while ((p = strstr(p, digest)) != NULL) {
        if ((p == list || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\0' || p[n] == '\r'))
            return 1;
        p += n;
    }
    return 0;
}

int is_withdrawn(const struct entry *e)
{
    struct entry *m = (struct entry *)e;        /* the answer is cached on the entry */
    if (m->withdrawn < 0) {
        const char *list = withdrawals_of(e);
        m->withdrawn = list != NULL && !listed(list, e->digest)
                       ? 0                      /* the channel says it withdrew nothing here */
                       : withdrawal_valid(chan_of(e), e);
    }
    return m->withdrawn;
}

/* Withdraw a published version: it stays in the channel, as everything
 * published does, but INSTALL and UPGRADE no longer pick it, asking for it
 * by version is refused, and SHOW marks it. Signed by the key that signed
 * the version, so only its publisher can. */
int cmd_withdraw(const struct pkg_options *a)
{
    struct index ix;
    struct key k;
    const struct entry *e = NULL;
    char text[300], signer[65], *wo = NULL, *ws = NULL;
    size_t i;
    int n, rc = 1;

    if (a->target == NULL)  return refuse_c(20, "name the package to withdraw");
    if (a->version == NULL) return refuse_c(20, "name the version with VERSION <v>: a withdrawal names one version");
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (is_url(a->channel))
        return refuse_c(20, "WITHDRAW writes into a channel on this machine; withdraw in the "
                        "directory you publish from, then PUSH it to %s", a->channel);
    if (load_key(a->sign, &k) != 0) return 1;
    if (read_index(a->channel, &ix) != 0) { memset(&k, 0, sizeof k); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->target) == 0 && pkg_version_cmp(ix.e[i].version, a->version) == 0
            && (a->arch == NULL || strcmp(ix.e[i].arch, a->arch) == 0)) {
            if (e != NULL && strcmp(e->arch, ix.e[i].arch) != 0) {
                refuse_c(20, "%s %s is published for several CPUs; say which with ARCH",
                         a->target, a->version);
                goto out;
            }
            e = &ix.e[i];
        }
    if (e == NULL) {
        say_not_found(&ix, a->target, a->version, a->channel);
        goto out;
    }
    if (is_withdrawn(e)) {
        kv("result", "unchanged");
        kv("name", "%s", e->name);
        kv("version", "%s", e->version);
        if (!machine)
            say_result("%s %s is already withdrawn from %s", e->name, e->version, a->channel);
        rc = 0;
        goto out;
    }
    if (!claimed_signer(a->channel, e->digest, signer) || strcmp(signer, k.pkhex) != 0) {
        kv("signer", "%s", k.pkhex);
        refuse_n(14, "ask-requester", "%s %s was signed by %s, and only that key can withdraw it; "
                 "this key is %s. Nothing was changed", e->name, e->version,
                 signer, k.pkhex);
        goto out;
    }
    n = withdrawal_text(e, text, sizeof text);
    if (dry_run) {
        kv("result", "would-withdraw");
    } else {
        wo = object_path(a->channel, e->digest, "withdrawn");
        ws = object_path(a->channel, e->digest, "withdrawn.sig");
        if (wo == NULL || ws == NULL || pkg_fs_write_atomic(wo, text, (size_t)n) != 0
            || write_sig(ws, &k, (const unsigned char *)text, (size_t)n) != 0) {
            refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
            goto out;
        }
        kv("result", "withdrawn");
    }
    kv("name", "%s", e->name);
    kv("version", "%s", e->version);
    kv("channel", "%s", a->channel);
    if (!machine)
        say_result("%s %s %s from %s: it stays in the channel, and nothing installs it any more",
            dry_run ? "would withdraw" : "withdrew", e->name, e->version, a->channel);
    rc = 0;
out:
    memset(&k, 0, sizeof k);
    free(wo); free(ws);
    free(ix.e);
    return rc;
}

/* A publisher key as KEY gives it: 64 lowercase hexadecimal digits. */
int pkg_is_key_hex(const char *s)
{
    size_t i;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return s[64] == '\0';
}

/* The key pinned for a package in this root, and the rule that governs it. */
static int check_pin(const char *root, const char *name, const char *signer,
                     const char *acceptkey)
{
    char *p = root_path(root, "keys", name);
    unsigned char *buf;
    size_t len;
    char pinned[65];

    if (p == NULL)
        return refuse("out of memory");
    if (pkg_fs_read(p, &buf, &len) != 0) {
        free(p);
        tr("no key pinned yet for %s in %s: %s will be pinned", name, root, signer);
        return 0;                     /* first use: pinned after a successful apply */
    }
    free(p);
    if (len < 64u) { free(buf); return refuse_c(12, "the pinned key for %s is damaged", name); }
    memcpy(pinned, buf, 64);
    pinned[64] = '\0';
    free(buf);
    if (strcmp(pinned, signer) == 0) {
        tr("signer of %s is the key pinned in %s", name, root);
        return 0;
    }
    if (acceptkey != NULL && strcmp(acceptkey, signer) == 0) {
        tr("signer of %s differs from the pinned key; accepted because acceptkey names it", name);
        kv("key-changed", "%s %s", pinned, signer);
        if (!machine)
        say_kind(PKG_LINE_DETAIL, "  %s\n", "key for %s changed by explicit ACCEPTKEY\n    was %s\n    now %s",
               name, pinned, signer);
        return 0;
    }
    kv("pinned", "%s", pinned);
    kv("signer", "%s", signer);
    return refuse_c(14, "%s is signed by a different key from the one pinned in %s.\n"
                  "  pinned %s\n  signer %s\n"
                  "Nothing was changed. Either the publisher changed keys or someone else "
                  "signed this; only whoever requested this can tell, by asking the publisher by another "
                  "route than this channel. If they confirm the new key, it is accepted with "
                  "ACCEPTKEY and the key in full", name, root, pinned, signer);
}

static int write_pin(const char *root, const char *name, const char *signer)
{
    char *p = root_path(root, "keys", name), line[80];
    int rc, n = snprintf(line, sizeof line, "%s\n", signer);
    if (p == NULL)
        return -1;
    rc = pkg_fs_write_atomic(p, line, (size_t)n);
    free(p);
    return rc;
}

/* 0 intact, 1 changed, 2 missing. */
int file_state(const char *root, const char *path, const char *digest,
               unsigned long long size)
{
    char *p = installed_path(root, path), hex[PKG_SHA256_HEXLEN + 1];
    unsigned char *buf;
    size_t len;
    int rc;

    if (p == NULL || pkg_fs_read(p, &buf, &len) != 0) { free(p); return 2; }
    free(p);
    pkg_sha256_hex(buf, len, hex);
    rc = ((unsigned long long)len == size && strcmp(hex, digest) == 0) ? 0 : 1;
    free(buf);
    return rc;
}

const struct pkg_file *find_file(const struct pkg_manifest *m, const char *path)
{
    size_t lo = 0, hi = m->nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2u;
        int c = strcmp(m->files[mid].path, path);
        if (c == 0) return &m->files[mid];
        if (c < 0) lo = mid + 1u; else hi = mid;
    }
    return NULL;
}

struct walk_ctx {
    const struct pkg_manifest *m;
    size_t i;
    char   err[400];
};

static int check_entry(const struct pkg_entry *e, void *ctx)
{
    struct walk_ctx *w = (struct walk_ctx *)ctx;
    char hex[PKG_SHA256_HEXLEN + 1];
    const struct pkg_file *f;

    if (w->i >= w->m->nfiles) {
        snprintf(w->err, sizeof w->err, "the container holds \"%s\", which the manifest does not list", e->path);
        return 1;
    }
    f = &w->m->files[w->i];
    if (strcmp(f->path, e->path) != 0) {
        snprintf(w->err, sizeof w->err, "the container holds \"%s\" where the manifest lists \"%s\"",
                 e->path, f->path);
        return 1;
    }
    pkg_sha256_hex(e->data, e->data_len, hex);
    if ((unsigned long long)e->data_len != f->size || strcmp(hex, f->digest) != 0) {
        snprintf(w->err, sizeof w->err, "\"%s\" does not match its manifest digest", e->path);
        return 1;
    }
    w->i++;
    return 0;
}

struct stage_ctx {
    size_t                     done, todo;  /* files written so far, of how many */
    const char                *staging;
    const struct pkg_manifest *m;
    const unsigned char       *keep;    /* files not to stage: already in place */
    char                       err[400];
};

static int stage_entry(const struct pkg_entry *e, void *ctx)
{
    struct stage_ctx *s = (struct stage_ctx *)ctx;
    char *p;
    if (s->keep != NULL) {
        const struct pkg_file *pf = find_file(s->m, e->path);
        if (pf != NULL && s->keep[pf - s->m->files] >= 2)
            return 0;           /* stays as it is: nothing to write */
    }
    counting(s->done++, s->todo);
    p = pkg_join(s->staging, e->path);
    if (p == NULL || pkg_fs_write_new(p, e->data, e->data_len) != 0) {
        snprintf(s->err, sizeof s->err, "cannot stage \"%s\": %s", e->path, strerror(errno));
        free(p);
        return 1;
    }
    free(p);
    return 0;
}

/* Move a root from `old` (NULL for a fresh install) to the package in `f`.
 * Install, upgrade and rollback all come through here, so they share every
 * check. Refuses before touching anything when:
 *   - the container disagrees with its manifest in any entry;
 *   - a new path exists in the root and belongs to no previous version;
 *   - a path both versions own was edited by the user since install.
 * Files the old version owned and the new one drops are removed when
 * unchanged, and kept, with a note, when the user edited them. */
/* ---- Amiga attributes on the installed files -------------------------- */

/* Record one file's word and comment in its directory's .ameta, under the
 * directory lock with the identity check (ameta.md, Writing). A default
 * word and no comment remove the entry. 0, or -1 with a warning given. */
static int ameta_set(const char *root, const char *rel, unsigned long long prot, const char *comment)
{
    const char *slash = strrchr(rel, '/');
    const char *base = slash ? slash + 1 : rel;
    char *dir, *path;
    int attempt, rc = -1;
    void *lock;
    dir = installed_path(root, rel);
    if (dir) {
        char *ds = strrchr(dir, '/'), *dc = strrchr(dir, ':');
        if (ds && (!dc || ds > dc)) *ds = 0;
        else if (dc) dc[1] = 0;
        else { free(dir); dir = pkg_strdup(root); }
    }
    path = dir ? pkg_join(dir, ".ameta") : NULL;
    if (path == NULL) { free(dir); return -1; }
    if ((prot & ~PKG_AMETA_RECORD_MASK) == 0 && (comment == NULL || !comment[0]) && !pkg_fs_exists(path)) {
        free(path); free(dir);
        return 0;                       /* nothing to record, nothing to take back */
    }
    lock = pkg_fs_lock_dir(dir);
    for (attempt = 0; attempt < 5 && rc < 0; attempt++) {
        struct pkg_fs_id id;
        struct pkg_ameta a;
        unsigned char *buf = NULL;
        size_t len = 0;
        char *out = NULL;
        size_t olen = 0;
        struct present_ctx pc;
        int r;
        if (pkg_fs_identity(path, &id) != 0) break;
        pkg_ameta_init(&a);
        if (id.exists && t_read(path, &buf, &len) != 0) break;
        if (id.exists && (pkg_ameta_parse(buf, len, &a) != 0 || !a.usable)) {
            warn("%s is not an .ameta this pkg can write: %s's attributes were not recorded",
                 path, rel);
            free(buf); pkg_ameta_free(&a);
            attempt = 5;
            break;
        }
        free(buf);
        pc.dir = dir;
        pkg_ameta_mark_stale(&a, present_in, &pc);
        if ((prot & ~PKG_AMETA_RECORD_MASK) == 0 && (comment == NULL || !comment[0])) {
            pkg_ameta_delete(&a, (const unsigned char *)base, strlen(base));
        } else {
            struct pkg_ameta_entry *e = pkg_ameta_get(&a, (const unsigned char *)base, strlen(base));
            if (e == NULL) { pkg_ameta_free(&a); break; }
            e->prot = prot;
            e->has_prot = 1;
            pkg_ameta_set_comment(e, (const unsigned char *)(comment ? comment : ""),
                                  comment ? strlen(comment) : 0);
        }
        if (pkg_ameta_emit(&a, &out, &olen) != 0) { pkg_ameta_free(&a); break; }
        pkg_ameta_free(&a);
        r = pkg_fs_replace_if_same(path, &id, out, olen);
        free(out);
        if (r == 0) { rc = 0; tr("recorded the attributes of %s in %s", rel, path); }
        else if (r < 0) break;
    }
    pkg_fs_unlock_dir(lock);
    if (rc != 0 && attempt < 5)
        warn("the attributes of %s could not be recorded in %s", rel, path);
    free(path);
    free(dir);
    return rc;
}

/* Give each installed file its protection and comment: on AROS the file
 * system holds them; elsewhere the host mode takes owner Execute and the
 * directory's .ameta the rest. */
void attrs_one(const char *root, const struct pkg_file *f)
{
    char latin[PKG_COMMENT_MAX + 1], *full = installed_path(root, f->path);
    int r;
    if (full == NULL) return;
    latin[0] = '\0';
    if (f->comment) pkg_comment_latin1(f->comment, latin, sizeof latin);
    r = pkg_fs_amiga_set(full, f->prot, latin);
    if (r < 0) {
        warn("the protection or comment of %s could not be set", f->path);
    } else if (r == 0) {
        if (pkg_fs_set_owner_exec(full, (f->prot & PKG_AMETA_OWNER_EXECUTE) == 0) != 0)
            warn("the host mode of %s could not be set", f->path);
        ameta_set(root, f->path, f->prot, f->comment);
    }
    free(full);
}

static void apply_attrs(const char *root, const struct pkg_manifest *m)
{
    size_t i;
    for (i = 0; i < m->nfiles; i++)
        attrs_one(root, &m->files[i]);
}

void installed_free(struct installed *in)
{
    size_t i;
    for (i = 0; i < in->n; i++)
        pkg_manifest_free(&in->m[i]);
    free(in->m);
    in->m = NULL;
    in->n = 0;
}

/* Every package in the root's database. */
int load_all(const char *root, struct installed *in)
{
    char *dir = pkg_join(root, ".pkg/db"), **names;
    size_t n, i;

    in->m = NULL;
    in->n = 0;
    if (dir == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(dir)) {
        free(dir);
        return 0;
    }
    if (pkg_fs_list(dir, &names, &n) != 0) {
        free(dir);
        return refuse_c(17, "cannot read the database of %s", root);
    }
    free(dir);
    in->m = calloc(n ? n : 1, sizeof *in->m);
    if (in->m == NULL) {
        for (i = 0; i < n; i++) free(names[i]);
        free(names);
        return refuse("out of memory");
    }
    for (i = 0; i < n; i++) {
        if (load_installed(root, names[i], &in->m[in->n], 1) == 0)
            in->n++;
        free(names[i]);
    }
    free(names);
    return 0;
}

int placement_prepare(const char *root, const struct pkg_manifest *m)
{
    struct placement *p;
    struct installed in;
    char canonical[4096], *prefix = NULL, *to = NULL, *icon = NULL;
    size_t i;
    int rc = 1;
    if (!requested_at) return placement_check(m);
    if (!placement_absolute(requested_at) || pkg_fs_canonical_dir(requested_at, canonical, sizeof canonical))
        return refuse_c(20, "AT must name an existing absolute destination directory: %s", requested_at);
    if ((p = placement_named(m->name)) != NULL) {
        if (p->fresh && !strcmp(p->parent, canonical)) return placement_check(m);
        return refuse_c(15, "%s already has a recorded destination at %s; retry INSTALL without AT", m->name, p->parent);
    }
    {
        char cr[4096];
        if (!pkg_fs_canonical_dir(root, cr, sizeof cr)) {
            size_t n = strlen(cr);
            int reserved = !ascii_casecmp_n(cr, canonical, n)
                && (!canonical[n] || canonical[n] == '/' || cr[n - 1] == ':' || cr[n - 1] == '/');
            if (reserved) return refuse_c(20, "AT must name a destination outside the package root");
        }
    }
    if (!m->kind || strcmp(m->kind, "application") || !m->nfiles)
        return refuse_c(20, "AT requires an application package with one self-contained drawer: %s", m->name);
    for (i = 0; i < m->nfiles; i++) {
        char *candidate = pkg_strdup(m->files[i].path), *slash;
        size_t n;
        if (!candidate) { free(prefix); return refuse("out of memory"); }
        n = strlen(candidate);
        if (n > 5 && !ascii_casecmp(candidate + n - 5, ".info")) candidate[n - 5] = 0;
        else if ((slash = strrchr(candidate, '/')) != NULL) *slash = 0;
        else candidate[0] = 0;
        if (!prefix) prefix = pkg_strdup(candidate);
        else {
            while (*prefix && !(strncmp(candidate, prefix, strlen(prefix)) == 0
                   && (candidate[strlen(prefix)] == 0 || candidate[strlen(prefix)] == '/'))) {
                slash = strrchr(prefix, '/');
                if (slash) *slash = 0; else *prefix = 0;
            }
        }
        free(candidate);
        if (!prefix || !*prefix) break;
    }
    if (!prefix || !*prefix) {
        free(prefix);
        return refuse_c(20, "%s does not contain one relocatable application drawer", m->name);
    }
    {
        const char *slash = strchr(prefix, '/'), *base = strrchr(prefix, '/');
        size_t n = slash ? (size_t)(slash - prefix) : strlen(prefix);
        static const char *system[] = {"C", "S", "L", "Libs", "Devs", "Classes", "Fonts", "Locale", "Prefs", "WBStartup", NULL};
        static const char *group[] = {"Extras", "Games", "Applications", "Utilities", "Tools", "Demos", NULL};
        int j;
        for (j = 0; system[j]; j++)
            if (strlen(system[j]) == n && !ascii_casecmp_n(prefix, system[j], n)) break;
        if (system[j]) { free(prefix); return refuse_c(20, "AT cannot relocate system files from %s", system[j]); }
        base = base ? base + 1 : prefix;
        for (j = 0; group[j]; j++)
            if (!ascii_casecmp(base, group[j])) break;
        if (group[j]) { free(prefix); return refuse_c(20, "%s has a shared category drawer; AT needs one application drawer", m->name); }
    }
    p = calloc(1, sizeof *p);
    if (!p) { free(prefix); return refuse("out of memory"); }
    p->name = pkg_strdup(m->name); p->prefix = prefix; p->parent = pkg_strdup(canonical); p->fresh = 1;
    if (!p->name || !p->parent) { refuse("out of memory"); goto out; }
    if (placement_layout(p, m)) goto out;
    /* Every logical prefix remains exclusive in this root. This also keeps
     * package ownership unambiguous when two parents alias the same volume. */
    if (load_all(root, &in)) goto out;
    for (i = 0; i < in.n; i++) {
        size_t j;
        for (j = 0; j < in.m[i].nfiles; j++)
            if (placement_matches(prefix, in.m[i].files[j].path)) break;
        if (j < in.m[i].nfiles) {
            refuse_c(15, "%s's drawer overlaps files belonging to %s", m->name, in.m[i].name);
            installed_free(&in); goto out;
        }
    }
    installed_free(&in);
    {
        struct placement *other;
        for (other = placements; other; other = other->next)
            if (placement_matches(other->prefix, prefix) || placement_matches(prefix, other->prefix)) {
                refuse_c(15, "%s's drawer overlaps the placement of %s", m->name, other->name); goto out;
            }
    }
    to = pkg_join(canonical, placement_relative(p, prefix));
    if (to) {
        icon = malloc(strlen(to) + 6);
        if (icon) { strcpy(icon, to); strcat(icon, ".info"); }
    }
    if (!to || !icon) { refuse("out of memory"); goto out; }
    {
        struct placement *other;
        for (other = placements; other; other = other->next) {
            char *their = pkg_join(other->parent, placement_relative(other, other->prefix));
            int overlap = their && (placement_matches(their, to) || placement_matches(to, their));
            free(their);
            if (overlap) { refuse_c(15, "AT destination is reserved for %s", other->name); goto out; }
        }
    }
    if (pkg_fs_exists(to) || pkg_fs_exists(icon)
        || pkg_fs_path_is_link(to) != 0 || pkg_fs_path_is_link(icon) != 0) {
        refuse_c(15, "AT destination %s or its drawer icon already exists; choose an empty destination", to); goto out;
    }
    if (load_all(root, &in)) goto out;
    for (i = 0; i < in.n; i++) {
        size_t j;
        for (j = 0; j < in.m[i].nfiles; j++) {
            char *physical = placement_physical(root, in.m[i].files[j].path);
            int overlap = physical && (placement_matches(to, physical) || placement_matches(physical, to));
            free(physical);
            if (overlap) {
                refuse_c(15, "AT destination overlaps files owned by %s", in.m[i].name);
                installed_free(&in); goto out;
            }
        }
    }
    installed_free(&in);
    p->next = placements; placements = p;
    kv("placement", "%s %s", prefix, to);
    if (!machine) say_item("destination", "%s -> %s", prefix, to);
    p = NULL;
    rc = 0;
out:
    if (p) { free(p->name); free(p->prefix); free(p->parent); free(p); }
    free(to); free(icon);
    return rc;
}

static int placement_save(const char *root, const struct pkg_manifest *m)
{
    struct placement *p = placement_named(m->name);
    char *path, *text;
    size_t n;
    int rc;
    if (!p || !p->fresh || dry_run) return 0;
    n = strlen(p->prefix) + strlen(p->parent) + 3;
    text = malloc(n);
    path = root_path(root, "placements", m->name);
    if (!text || !path) { free(text); free(path); return refuse("out of memory"); }
    snprintf(text, n, "%s\n%s\n", p->prefix, p->parent);
    rc = pkg_fs_write_atomic(path, text, n - 1);
    free(path); free(text);
    return rc ? refuse_c(17, "cannot record destination for %s; nothing was placed", m->name) : 0;
}

/* The name beside `path` that Pkg sets a file down under: `path` plus
 * `suffix`, the file name shortened when needed to stay within the 30
 * characters an FFS name may have. Allocated. */
char *beside(const char *path, const char *suffix)
{
    const char *base = strrchr(path, '/');
    size_t dl = base ? (size_t)(base - path) + 1 : 0, bl = strlen(path) - dl, sl = strlen(suffix);
    char *out;
    if (bl + sl > 30) bl = 30 > sl ? 30 - sl : 1;
    out = malloc(dl + bl + sl + 1);
    if (out == NULL) return NULL;
    memcpy(out, path, dl + bl);
    memcpy(out + dl + bl, suffix, sl + 1);
    return out;
}

static int apply(const char *root, const struct pkg_manifest *old, const struct fetched *f,
                 unsigned long *placed, unsigned long *dropped, unsigned long *kept)
{
    const struct pkg_manifest *m = &f->m;
    struct walk_ctx wc;
    struct stage_ctx sc;
    char *staging, *dbp;
    unsigned long adopted;  /* files already there, byte for byte the package's */
    struct installed others;/* loaded when a file is already there */
    int others_loaded = 0;
    unsigned long same;     /* files the old and new versions share, intact on disk */
    unsigned long resumed;  /* files an interrupted change already placed, byte for byte */
    int fs;
    unsigned char *keep;    /* per file: 1 a person's configuration kept, the new one set
                               beside it; 2 kept, and the new version is what they edited;
                               3 already in place, byte for byte: neither staged nor moved */
    int stopped;
    size_t i;
    enum pkg_status st;

    *placed = *dropped = *kept = 0;
    if (placement_check(m)) return 1;
    wc.m = m; wc.i = 0; wc.err[0] = '\0';
    st = pkg_read(f->pkg, f->pkg_len, check_entry, &wc, &stopped);
    if (st != PKG_OK || wc.i != m->nfiles)
        return refuse_c(12, "the payload of %s %s is refused: %s; nothing was changed", m->name, m->version,
                      st == PKG_E_STOPPED ? wc.err
                      : st != PKG_OK ? pkg_strstatus(st) : "the manifest lists files the container lacks");

    keep = calloc(m->nfiles ? m->nfiles : 1, 1);
    if (keep == NULL)
        return refuse("out of memory");
    adopted = 0;
    same = 0;
    resumed = 0;
    others.m = NULL;
    others.n = 0;
    doing_things("checking", m->name, (long long)m->nfiles, "file");
    for (i = 0; i < m->nfiles; i++) {
        const struct pkg_file *of = old ? find_file(old, m->files[i].path) : NULL;
        counting(i, m->nfiles);
        if (m->files[i].config) {
            /* A configuration file: a person's version stays where it is. */
            if (of != NULL && file_state(root, of->path, of->digest, of->size) == 1)
                keep[i] = strcmp(of->digest, m->files[i].digest) == 0 ? 2 : 1;
            else if (of == NULL) {
                int cs = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size);
                if (cs == 1) keep[i] = 1;
                else if (cs == 0) { keep[i] = 3; adopted++; }
            } else if (strcmp(of->digest, m->files[i].digest) == 0 && of->size == m->files[i].size
                       && file_state(root, of->path, of->digest, of->size) == 0) {
                keep[i] = 3;
                same++;
            }
            continue;
        }
        if (of == NULL) {
            char *t = installed_path(root, m->files[i].path);
            int there = t ? pkg_fs_exists(t) : 1;
            free(t);
            if (there && file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 0) {
                /* Already there with the package's own bytes, as an AROS set up
                 * with InstallAROS holds its files: the package takes it over,
                 * unless another installed package lists it. */
                const char *owner = NULL;
                size_t k;
                if (!others_loaded) {
                    if (load_all(root, &others) != 0) { free(keep); return 1; }
                    others_loaded = 1;
                }
                for (k = 0; k < others.n && owner == NULL; k++)
                    if (strcmp(others.m[k].name, m->name) != 0
                        && find_file(&others.m[k], m->files[i].path) != NULL)
                        owner = others.m[k].name;
                if (owner == NULL) {
                    adopted++;
                    keep[i] = 3;
                    continue;
                }
                refuse_c(15, "\"%s\" belongs to %s, which is installed; %s %s ships it too, and "
                         "nothing was changed. One file has one owner: the publisher of one of "
                         "the two leaves it out", m->files[i].path, owner, m->name, m->version);
                installed_free(&others);
                free(keep);
                return 1;
            }
            if (there) {
                free(keep);
                installed_free(&others);
                return refuse_c(15, "\"%s\" already exists in %s with other content than %s %s "
                              "ships, and no installed package lists it; nothing was changed. It "
                              "may be the requester's own file, or another version's",
                              m->files[i].path, root, m->name, m->version);
            }
        } else if ((fs = file_state(root, of->path, of->digest, of->size)) == 0) {
            if (strcmp(of->digest, m->files[i].digest) == 0 && of->size == m->files[i].size) {
                keep[i] = 3;            /* the same in both versions, and intact */
                same++;
            }
        } else if (fs == 1 && file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 0) {
            /* Not an edit: this change, interrupted, already put the new
             * version's own bytes here. Repeating it goes on from there. */
            keep[i] = 3;
            resumed++;
        } else if (fs == 1) {
            free(keep);
            installed_free(&others);
            return refuse_c(15, "\"%s\" was edited since %s %s was installed, and %s %s ships it too; "
                          "nothing was changed. The edit belongs to whoever made it: the requester decides whether "
                          "to keep it elsewhere first. A publisher who means it to be edited declares it "
                          "with CONFIG, and pkg then keeps the edit", of->path, old->name, old->version,
                          m->name, m->version);
        }
    }
    if (adopted) {
        if (machine) kv("adopted", "%lu", adopted);
        else say_item("adopted", "%lu file%s already there, identical to %s %s's", adopted,
                 adopted == 1 ? "" : "s", m->name, m->version);
    }
    if (resumed) {
        if (machine) kv("resumed-files", "%lu", resumed);
        else say_item("resumed", "%lu file%s already placed by an interrupted change", resumed,
                 resumed == 1 ? "" : "s");
    }
    if (same) {
        if (machine) kv("unchanged-files", "%lu", same);
        else say_item("unchanged", "%lu file%s the same in %s and %s, not written again", same,
                 same == 1 ? "" : "s", old->version, m->version);
    }
    for (i = 0; i < m->nfiles; i++)
        if (keep[i] == 1 || keep[i] == 2) {
            (*kept)++;
            char *nw = keep[i] == 1 ? beside(m->files[i].path, ".pkgnew") : NULL;
            if (machine) {
                kv("config-kept", "%s", m->files[i].path);
                if (nw) kv("config-new", "%s", nw);
            } else if (nw) {
                say_item("kept", "%s (edited; %s %s's version is beside it as %s)",
                    m->files[i].path, m->name, m->version, nw);
            } else if (keep[i] == 1) {
                say_item("kept", "%s (edited)", m->files[i].path);
            } else {
                say_item("kept", "%s (edited; %s %s ships it unchanged)", m->files[i].path,
                    m->name, m->version);
            }
            free(nw);
        }

    installed_free(&others);
    did();
    tr("%s %s: every file checked against the container, nothing in the way", m->name, m->version);
    if (dry_run) {
        /* Every check above has passed; say what would move, move nothing. */
        *placed = 0;
        for (i = 0; i < m->nfiles; i++)
            if (!keep[i]) (*placed)++;
        for (i = 0; old != NULL && i < old->nfiles; i++)
            if (find_file(m, old->files[i].path) == NULL)
                (*dropped)++;
        free(keep);
        return 0;
    }
    {
        char rel[128];
        snprintf(rel, sizeof rel, ".pkg/staging/%s", m->name);
        struct placement *p = placement_named(m->name);
        if (p) {
            unsigned char nonce[8];
            if (pkg_fs_random(nonce, sizeof nonce)) {
                free(keep); return refuse_c(17, "cannot choose a private staging directory");
            }
            snprintf(rel, sizeof rel, ".pkg-at-%02x%02x%02x%02x%02x%02x%02x%02x",
                     nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
            staging = pkg_join(p->parent, rel);
            if (staging && pkg_fs_exists(staging)) {
                free(staging); free(keep); return refuse_c(17, "staging directory already exists");
            }
        } else staging = pkg_join(root, rel);
    }
    if (staging == NULL || pkg_fs_rmtree(staging) != 0) {
        free(staging);
        free(keep);
        return refuse_c(17, "cannot prepare staging in %s", root);
    }
    sc.staging = staging; sc.m = m; sc.keep = keep; sc.err[0] = '\0';
    sc.done = 0;
    for (i = 0, sc.todo = 0; i < m->nfiles; i++)
        if (keep[i] < 2) sc.todo++;
    doing_things("writing", m->name, (long long)m->nfiles, "file");
    if (pkg_read(f->pkg, f->pkg_len, stage_entry, &sc, &stopped) != PKG_OK) {
        did();
        refuse_c(PKGRC_IO, "%s; nothing was changed", sc.err);
        pkg_fs_rmtree(staging);
        free(staging);
        free(keep);
        return 1;
    }
    did();

    if (placement_save(root, m)) {
        pkg_fs_rmtree(staging); free(staging); free(keep); return 1;
    }
    doing_things("placing", m->name, (long long)m->nfiles, "file");
    for (i = 0; i < m->nfiles; i++) {
        char *from, *to;
        int good;
        counting(i, m->nfiles);
        from = pkg_join(staging, m->files[i].path);
        to = installed_path(root, m->files[i].path);
        if (keep[i] == 2 || keep[i] == 3) {
            free(from);
            free(to);
            continue;
        }
        if (keep[i] == 1 && to != NULL) {
            char *nw = beside(to, ".pkgnew");
            free(to);
            to = nw;
        }
        if (to != NULL && pkg_fs_path_is_link(to) != 0) {
            free(from); free(to); free(staging); free(keep);
            return refuse_c(17, "unsafe destination for %s", m->files[i].path);
        }
        if (to != NULL && pkg_fs_exists(to))
            pkg_fs_unprotect(to);           /* the version it replaces may forbid Delete */
        good = from && to && pkg_fs_rename(from, to) == 0;
        free(from);
        free(to);
        if (!good) {
            refuse_c(17, "cannot place \"%s\": %s. %lu of %lu files were placed",
                   m->files[i].path, strerror(errno), (unsigned long)i, (unsigned long)m->nfiles);
            free(staging);
            free(keep);
            return 1;
        }
        if (!keep[i]) (*placed)++;
    }
    did();
    free(keep);

    if (old != NULL) {
        for (i = 0; i < old->nfiles; i++) {
            const struct pkg_file *of = &old->files[i];
            int s;
            if (find_file(m, of->path) != NULL)
                continue;
            s = file_state(root, of->path, of->digest, of->size);
            if (s == 0) {
                char *p = installed_path(root, of->path);
                if (p != NULL && pkg_fs_unlink(p) == 0) {
                    (*dropped)++;
                    installed_prune(root, of->path);
                }
                free(p);
            } else if (s == 1) {
                (*kept)++;
                if (machine) kv("kept", "%s", of->path);
                else say_item("kept", "%s (edited, and no longer part of %s)", of->path, m->name);
            }
        }
    }

    /* The database entry is written last: it is what says the change is
     * done. A cut before it leaves the old entry, and the same operation
     * repeated finishes the change (the files already placed count as
     * placed). So the previous version and the pin go first: a cut can
     * leave them ahead of the database, never behind it. */
    if (old != NULL) {
        /* "<old>" then "to <new> <verb>": until the database entry follows,
         * the record says which change is under way (readers of the first
         * word alone see the previous version, as before). */
        char *pp = root_path(root, "prev", m->name), line[200];
        int n = snprintf(line, sizeof line, "%s\nto %s %s\n", old->version, m->version, verb_name);
        /* Before the database, a failure stops the change: the files are
         * placed, the database is the old one, and repeating the command
         * finishes it once the volume can be written again. */
        if (pp == NULL || pkg_fs_write_atomic(pp, line, (size_t)n) != 0) {
            refuse_c(17, "the files are placed but the previous version could not be recorded: "
                     "%s; the database still names %s %s, and the same command again finishes the "
                     "change", strerror(errno), old->name, old->version);
            free(pp);
            free(staging);
            return 1;
        }
        free(pp);
    }
    if (write_pin(root, m->name, f->signer) != 0) {
        refuse_c(17, "the files are placed but the signing key could not be pinned: %s; the same "
                 "command again finishes the change", strerror(errno));
        free(staging);
        return 1;
    }
    dbp = root_path(root, "db", m->name);
    if (dbp == NULL || pkg_fs_write_atomic(dbp, f->mtext, f->mlen) != 0) {
        refuse_c(17, "the files are placed but the database entry could not be written: %s",
               strerror(errno));
        free(dbp);
        free(staging);
        return 1;
    }
    free(dbp);
    apply_attrs(root, m);
    pkg_fs_rmtree(staging);
    free(staging);
    return 0;
}

/* ---- dependencies ----------------------------------------------------- */

/* A package installed only because another needed it carries a mark in
 * .pkg/auto. REMOVE ORPHANS takes out marked packages nothing depends on. */
int is_auto(const char *root, const char *name)
{
    char *p = root_path(root, "auto", name);
    int there = p != NULL && pkg_fs_exists(p);
    free(p);
    return there;
}

void set_auto(const char *root, const char *name, int on)
{
    char *p = root_path(root, "auto", name);
    if (p == NULL)
        return;
    if (on) {
        if (pkg_fs_write_atomic(p, "dependency\n", 11) != 0)
            warn("%s could not be marked as a dependency", name);
    } else if (pkg_fs_exists(p)) {
        pkg_fs_unlink(p);
    }
    free(p);
}

/* The installed packages that name `name` among their Depends, comma-joined. */
int needed_by(const struct installed *in, const char *name, char *out, size_t len)
{
    size_t i, j, at = 0;
    int found = 0;
    out[0] = '\0';
    for (i = 0; i < in->n; i++)
        for (j = 0; j < in->m[i].ndeps; j++) {
            if (strcmp(in->m[i].deps[j].name, name) != 0)
                continue;
            /* Found counts whether or not the name still fits in `out`. */
            if (at + 70 < len)
                at += (size_t)snprintf(out + at, len - at, "%s%s %s", found ? ", " : "",
                                       in->m[i].name, in->m[i].version);
            found = 1;
        }
    return found;
}

void plan_free(struct plan *p)
{
    size_t i;
    for (i = 0; i < p->n; i++)
        fetched_free(&p->f[i]);
    free(p->f);
    free(p->dep);
}

/* Plan `name`: exactly `exact` when given, else the highest the channel offers,
 * which must be at least `min`. `from` is the package that needs it, NULL for
 * the one the person named. */
static int plan_one(struct plan *p, const char *name, const char *min, const char *exact,
                    const char *from)
{
    const struct entry *e;
    struct fetched f;
    struct pkg_manifest cur;
    const char *have;
    size_t i;

    if (cancelled("while resolving, before anything was placed"))
        return 1;
    for (i = 0; i < p->depth; i++) {
        if (strcmp(p->stack[i], name) == 0) {
            char path[512];
            size_t at = 0, j;
            for (j = i; j < p->depth && at + 70 < sizeof path; j++)
                at += (size_t)snprintf(path + at, sizeof path - at, "%s -> ", p->stack[j]);
            snprintf(path + at, sizeof path - at, "%s", name);
            return refuse_c(16, "the dependencies form a cycle: %s; nothing was changed", path);
        }
    }
    if (from != NULL)
        tr("%s needs %s%s%s", from, name, min ? " >= " : "", min ? min : "");
    if (from != NULL) {
        if (load_installed(p->root, name, &cur, 1) == 0) {
            int low;
            have = planned_version(name);
            if (have == NULL)
                have = cur.version;
            low = min != NULL && pkg_version_cmp(have, min) < 0;
            if (!low)
                tr("%s is satisfied by the installed %s %s", name, name, have);
            if (low)
                refuse_c(16, "%s needs %s >= %s, and %s has %s %s; nothing was changed. "
                         "Upgrading %s would change it for everything that uses it",
                         from, name, min, p->root, name, have, name);
            pkg_manifest_free(&cur);
            return low;
        }
        for (i = 0; i < p->n; i++)
            if (strcmp(p->f[i].m.name, name) == 0) {
                tr("%s is already part of this plan, at %s", name, p->f[i].m.version);
                if (min != NULL && pkg_version_cmp(p->f[i].m.version, min) < 0)
                    return refuse_c(16, "%s needs %s >= %s, and this install brings %s %s; "
                                    "nothing was changed", from, name, min, name, p->f[i].m.version);
                return 0;
            }
    }
    e = pick(p->ix, name, exact);
    if (e == NULL && pick_refused)
        return 1;
    if (e == NULL) {
        if (from == NULL) {
            say_not_found(p->ix, name, exact, chans_text());
            return 1;
        }
        return refuse_c(16, "%s depends on %s%s%s, which the channel %s does not offer; "
                        "nothing was changed", from, name, min ? " >= " : "", min ? min : "",
                        chans_text());
    }
    if (is_withdrawn(e))
        return refuse_n(18, "ask-requester", "%s %s was withdrawn by its publisher in %s; nothing "
                        "was changed. Installing it anyway is the requester's decision, and pkg "
                        "does not take it", e->name, e->version, chan_of(e));
    if (min != NULL && pkg_version_cmp(e->version, min) < 0)
        return refuse_c(16, "%s needs %s >= %s, and the highest the channel offers is %s; "
                        "nothing was changed", from ? from : "the request", name, min, e->version);
    if (p->depth >= sizeof p->stack / sizeof p->stack[0])
        return refuse_c(16, "the dependencies of %s go more than %u levels deep", name,
                        (unsigned)(sizeof p->stack / sizeof p->stack[0]));
    if (fetch(chan_of(e), e, &f) != 0) {
        fetched_free(&f);
        return 1;
    }
    if (p->key != NULL) {
        /* KEY says who publishes the named package, and trusts no one else
         * on the way: a dependency this root has no key for is refused, and
         * installed first with its own publisher's KEY. */
        char pinned[65];
        int have = pinned_key(p->root, e->name, pinned);
        if (from == NULL && strcmp(f.signer, p->key) != 0) {
            kv("signer", "%s", f.signer);
            kv("expected", "%s", p->key);
            refuse_c(14, "%s %s is signed by %s, not by the KEY given; nothing was changed",
                     e->name, e->version, f.signer);
            fetched_free(&f);
            return 1;
        }
        if (from == NULL && have && strcmp(pinned, p->key) != 0
            && (p->acceptkey == NULL || strcmp(p->acceptkey, p->key) != 0)) {
            kv("pinned", "%s", pinned);
            kv("expected", "%s", p->key);
            refuse_c(14, "this root pins %s for %s, and KEY names %s; nothing was changed. If the "
                     "publisher confirmed the new key, ACCEPTKEY with it in full accepts it",
                     pinned, e->name, p->key);
            fetched_free(&f);
            return 1;
        }
        if (from != NULL && !have) {
            kv("dependency", "%s %s", e->name, e->version);
            refuse_n(14, "install-dependency-first", "%s needs %s %s, and this root trusts no key "
                     "for %s yet: KEY names %s's publisher alone. Nothing was changed; install %s "
                     "first, with its own publisher's KEY", from, e->name, e->version, e->name,
                     from, e->name);
            fetched_free(&f);
            return 1;
        }
    }
    if (check_pin(p->root, e->name, f.signer, p->acceptkey) != 0) {
        fetched_free(&f);
        return 1;
    }
    {
        /* Nothing pinned yet: the first install would trust this signer from
         * now on. The key that signed the channel's first version of the
         * package is presumed the publisher's; a first install signed by
         * another key is the case a key added later by someone else would
         * make, and trusting it is the requester's decision. */
        char *kp = root_path(p->root, "keys", e->name), first_signer[65];
        const struct entry *oldest = NULL;
        int first = kp != NULL && !pkg_fs_exists(kp);
        size_t o;
        free(kp);
        for (o = 0; first && o < p->ix->n; o++)
            if (strcmp(p->ix->e[o].name, e->name) == 0 && p->ix->e[o].ch == e->ch
                && (oldest == NULL || pkg_version_cmp(p->ix->e[o].version, oldest->version) < 0))
                oldest = &p->ix->e[o];
        if (first && oldest != NULL && oldest != e
            && claimed_signer(chan_of(oldest), oldest->digest, first_signer)
            && strcmp(first_signer, f.signer) != 0
            && (p->acceptkey == NULL || strcmp(p->acceptkey, f.signer) != 0)
            && (p->key == NULL || strcmp(p->key, f.signer) != 0)) {
            kv("signer", "%s", f.signer);
            kv("first-signer", "%s", first_signer);
            refuse_n(14, "ask-requester", "%s %s is signed by %s, but %s %s, the first version in "
                     "this channel, was signed by %s. This root trusts no key for %s yet, and "
                     "the first install would trust this one from now on; only the requester can "
                     "say which key is the publisher's. Nothing was changed", e->name, e->version,
                     f.signer, e->name, oldest->version, first_signer, e->name);
            fetched_free(&f);
            return 1;
        }
    }
    p->stack[p->depth++] = f.m.name;
    pick_quiet = 0;
    for (i = 0; i < f.m.ndeps; i++) {
        if (plan_one(p, f.m.deps[i].name, f.m.deps[i].min, NULL, f.m.name) != 0) {
            p->depth--;
            fetched_free(&f);
            return 1;
        }
    }
    p->depth--;
    {
        struct fetched *g = realloc(p->f, (p->n + 1) * sizeof *g);
        int *d;
        if (g == NULL) { fetched_free(&f); return refuse("out of memory"); }
        p->f = g;
        d = realloc(p->dep, (p->n + 1) * sizeof *d);
        if (d == NULL) { fetched_free(&f); return refuse("out of memory"); }
        p->dep = d;
        p->f[p->n] = f;
        p->dep[p->n] = from != NULL;
        p->n++;
    }
    return 0;
}

/* Remove a package's unchanged files and its records. Edited files stay. */
int remove_files(const char *root, const struct pkg_manifest *m,
                 size_t *removed, size_t *kept, size_t *gone, int report)
{
    size_t i;
    int failed = 0;
    char *dbp, *pp;
    *removed = *kept = *gone = 0;
    if (placement_check(m)) return 1;
    for (i = 0; i < m->nfiles; i++) {
        int s = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size);
        if (s == 0 && dry_run) {
            (*removed)++;
        } else if (s == 0) {
            char *p = installed_path(root, m->files[i].path);
            if (p != NULL)
                pkg_fs_unprotect(p);            /* a Delete-forbidden file is still Pkg's to remove */
            if (p != NULL && pkg_fs_unlink(p) == 0) {
                (*removed)++;
                ameta_set(root, m->files[i].path, 0, NULL);   /* its entry goes too */
                installed_prune(root, m->files[i].path);
            } else {
                failed = 1;
                if (p != NULL && report)
                    warn("%s could not be deleted: %s", m->files[i].path, strerror(errno));
            }
            free(p);
        } else if (s == 1) {
            (*kept)++;
            if (!report)
                continue;
            if (machine) kv("kept", "%s", m->files[i].path);
            else say_item("kept", "%s (changed since install, so it is yours now)", m->files[i].path);
        } else {
            (*gone)++;
        }
    }
    if (dry_run) return 0;
    if (failed) return refuse_c(17, "some files of %s could not be removed; its database and placement records were kept", m->name);
    dbp = root_path(root, "db", m->name);
    if (dbp == NULL || pkg_fs_unlink(dbp) != 0) {
        free(dbp);
        return refuse_c(17, "the files of %s are removed but its database entry could not be: %s",
                        m->name, strerror(errno));
    }
    free(dbp);
    {
        struct placement *p = placement_named(m->name);
        if (p) {
            char *rel = pkg_join(placement_relative(p, p->prefix), ".pkg-removed");
            if (rel) pkg_fs_prune_empty_parents(p->parent, rel);
            free(rel);
        }
    }
    pp = root_path(root, "prev", m->name);
    if (pp != NULL && pkg_fs_exists(pp)) pkg_fs_unlink(pp);
    free(pp);
    pp = root_path(root, "placements", m->name);
    if (pp && pkg_fs_exists(pp) && pkg_fs_unlink(pp))
        warn("the placement record for %s could not be removed", m->name);
    free(pp);
    set_auto(root, m->name, 0);
    /* The pinned key stays: reinstalling the package later is still held to it. */
    return 0;
}

/* Carry out a plan. The last entry is the named package, moved from `cur`
 * (NULL for a fresh install); the others are new dependencies. A failure
 * takes the dependencies this run placed back out. */
int run_plan(struct plan *p, const struct pkg_manifest *cur,
             unsigned long *placed, unsigned long *dropped, unsigned long *kept)
{
    size_t i;
    int *had_pin = calloc(p->n ? p->n : 1, sizeof *had_pin);
    if (had_pin == NULL)
        return refuse("out of memory");
    for (i = 0; i < p->n; i++)
        if (placement_check(&p->f[i].m)) { free(had_pin); return 1; }
    for (i = 0; i < p->n; i++) {
        int last = i + 1 == p->n;
        unsigned long pl, dr, ke;
        char when[200], *kp = root_path(p->root, "keys", p->f[i].m.name);
        had_pin[i] = kp != NULL && pkg_fs_exists(kp);
        free(kp);
        snprintf(when, sizeof when, "before placing %s %s%s", p->f[i].m.name, p->f[i].m.version,
                 i > 0 ? "; what this operation had placed was taken back out" : "");
        if (cancelled(when) || apply(p->root, last ? cur : NULL, &p->f[i], &pl, &dr, &ke) != 0) {
            /* Undo in reverse: files, records, and the keys this operation
             * pinned, so the root is as it was. */
            while (!dry_run && i-- > 0) {
                size_t r, k, g;
                remove_files(p->root, &p->f[i].m, &r, &k, &g, 0);
                if (!had_pin[i]) {
                    char *pin = root_path(p->root, "keys", p->f[i].m.name);
                    if (pin != NULL) pkg_fs_unlink(pin);
                    free(pin);
                }
                tr("took %s %s back out", p->f[i].m.name, p->f[i].m.version);
            }
            free(had_pin);
            return 1;
        }
        if (!last) {
            if (!dry_run)
                set_auto(p->root, p->f[i].m.name, 1);
        } else {
            *placed = pl;
            *dropped = dr;
            *kept = ke;
        }
    }
    free(had_pin);
    /* Only now, with every package in place, say which dependencies came in:
     * a front end never shows a package that is about to be taken out. */
    for (i = 0; i + 1 < p->n; i++) {
        if (machine) {
            char j[140];
            snprintf(j, sizeof j, "%s %s", p->f[i].m.name, p->f[i].m.version);
            rec_item("dependency", j, "name", p->f[i].m.name, "version", p->f[i].m.version, NULL);
        } else {
            say_item(dry_run ? "would add" : "added", "%s %s, a dependency",
                p->f[i].m.name, p->f[i].m.version);
        }
    }
    return 0;
}

int plan_target(struct plan *p, const struct pkg_options *a, const struct index *ix,
                const char *name, const char *exact)
{
    int rc;
    memset(p, 0, sizeof *p);
    p->root = a->root;
    p->acceptkey = a->acceptkey;
    p->key = a->key;
    p->ix = ix;
    if (a->key != NULL && !pkg_is_key_hex(a->key))
        return refuse_c(20, "KEY is the publisher's public key: 64 lowercase hexadecimal digits");
    /* The caller picked `exact` and traced why. */
    pick_quiet = 1;
    rc = plan_one(p, name, NULL, exact, NULL);
    pick_quiet = 0;
    return rc;
}

/* The marked packages nothing installed depends on. */
size_t find_orphans(const struct installed *in, const char *root, size_t *which)
{
    size_t i, n = 0;
    char who[1];
    for (i = 0; i < in->n; i++)
        if (is_auto(root, in->m[i].name) && !needed_by(in, in->m[i].name, who, sizeof who))
            which[n++] = i;
    return n;
}

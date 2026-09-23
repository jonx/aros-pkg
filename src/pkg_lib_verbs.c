/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The verbs that change a root or a channel: publish, withdraw, install,
 * upgrade, rollback, list, verify, repair, remove.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- verbs ------------------------------------------------------------ */

/* The mount entry of an installed image, and the AmigaDOS steps around it.
 * Pkg's part in an application ends with the image in place; this only
 * writes down what mounting it takes, with the geometry read from the signed
 * manifest, so nobody has to work it out. */
int cmd_mountlist(const struct pkg_options *a)
{
    struct pkg_manifest m;
    struct installed in;
    char text[1600], handler[512], fdsk[64], unitbuf[16], *img;
    const char *unit = a->unit ? a->unit : "20";
    unsigned long blocks;
    size_t i, j;
    int n, libs = 0;

    if (a->target == NULL)  return refuse_c(20, "name the installed image");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (strspn(unit, "0123456789") != strlen(unit) || strlen(unit) == 0 || strlen(unit) > 4)
        return refuse_c(20, "UNIT is a number, the fdsk.device unit");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    if (strcmp(m.kind, "image") != 0 || m.nfiles != 1) {
        refuse_c(20, "%s is a %s, not an image; only an image is mounted", m.name, m.kind);
        pkg_manifest_free(&m);
        return 1;
    }
    blocks = (unsigned long)(m.files[0].size / PKG_IMAGE_BLOCK);
    handler[0] = '\0';
    if (a->handler != NULL) {
        snprintf(handler, sizeof handler, "%s", a->handler);
    } else if (load_all(a->root, &in) == 0) {
        /* An FFS handler installed into the same root as a component. */
        for (i = 0; i < in.n && !handler[0]; i++)
            for (j = 0; j < in.m[i].nfiles; j++)
                if (strcmp(in.m[i].kind, "device") == 0
                    && strcmp(in.m[i].files[j].path, "L/afs-handler") == 0) {
                    char *h = pkg_join(a->root, "L/afs-handler");
                    if (h) snprintf(handler, sizeof handler, "%s", h);
                    free(h);
                }
        for (i = 0; i < in.n; i++)
            for (j = 0; j < m.ndeps; j++)
                if (strcmp(in.m[i].name, m.deps[j].name) == 0)
                    libs = 1;
        installed_free(&in);
    }
    n = snprintf(text, sizeof text,
        "%s%s%s"
        "Device          = fdsk.device\n"
        "Unit            = %s\n"
        "Flags           = 0\n"
        "Surfaces        = 1\n"
        "BlocksPerTrack  = %u\n"
        "LowCyl          = 0\n"
        "HighCyl         = %lu\n"
        "Reserved        = 2\n"
        "BlockSize       = %u\n"
        "Buffers         = 20\n"
        "BufMemType      = 1\n"
        "Mask            = 0\n"
        "StackSize       = 16384\n"
        "Priority        = 5\n"
        "GlobVec         = -1\n"
        "DosType         = 0x444F5303\n"
        "Activate        = 1\n",
        handler[0] ? "FileSystem      = " : "", handler, handler[0] ? "\n" : "",
        unit, PKG_IMAGE_TRACK, blocks / PKG_IMAGE_TRACK - 1, PKG_IMAGE_BLOCK);
    img = pkg_join(a->root, m.files[0].path);
    snprintf(unitbuf, sizeof unitbuf, "Unit%s", unit);
    snprintf(fdsk, sizeof fdsk, "RAM:fdsk");
    if (a->out != NULL && !dry_run && pkg_fs_write_atomic(a->out, text, (size_t)n) != 0) {
        free(img);
        pkg_manifest_free(&m);
        return refuse_c(17, "cannot write \"%s\": %s", a->out, strerror(errno));
    }
    kv("result", "%s", a->out ? res("created", "would-create") : "shown");
    kv("name", "%s", m.name);
    kv("image", "%s", img ? img : m.files[0].path);
    kv("blocks", "%lu", blocks);
    kv("highcyl", "%lu", blocks / PKG_IMAGE_TRACK - 1);
    kv("unit", "%s", unit);
    kv("handler", "%s", handler[0] ? handler : "none: the system's FFS for DOS\\3");
    if (a->out) kv("file", "%s", a->out);
    kv("step", "MakeDir %s", fdsk);
    kv("step", "Assign FDSK: %s", fdsk);
    kv("step", "MakeLink %s/%s %s", fdsk, unitbuf, img ? img : m.files[0].path);
    kv("step", "Protect %s w SUB", img ? img : m.files[0].path);
    if (libs) kv("step", "Assign LIBS: %s/Libs ADD", a->root);
    kv("step", "Mount %s", a->out ? a->out : "<this mountlist, saved as a file named after the device>");
    if (!machine) {
        if (a->out == NULL)
            say_raw(text, (size_t)n);
        else
            say_result("%s %s, the mount entry for %s (%lu blocks)",
                    dry_run ? "would write" : "wrote", a->out, m.name, blocks);
        say_kind(PKG_LINE_NOTE, "\n%s\n", "On AROS, the device is named after the mountlist file:\n"
                "  MakeDir %s\n  Assign FDSK: %s\n  MakeLink %s/%s %s\n  Protect %s w SUB\n%s%s%s"
                "  Mount %s", fdsk, fdsk, fdsk, unitbuf, img ? img : "", img ? img : "",
                libs ? "  Assign LIBS: " : "", libs ? a->root : "", libs ? "/Libs ADD\n" : "",
                a->out ? a->out : "<file>");
    }
    if (a->out == NULL)
        hint("Mount reads the entry from a file named after the device: add OUT <file>, "
             "for example OUT RAM:%s, and pkg writes it", m.name);
    if (!handler[0])
        hint("no FFS handler is installed in this root, so the entry relies on the system's. "
             "Native AROS has one; hosted AROS built on macOS has none: there, install one "
             "into the root as a device package, or name one with HANDLER <path>");
    free(img);
    pkg_manifest_free(&m);
    return 0;
}

/* What a channel offers, each version checked the way INSTALL would check
 * it: manifest against the index, signature, payload against the manifest.
 * Nothing is installed. Exits with the class of the first bad entry. */
/* One row of SHOW: an entry, its checks, and what was found. */
struct show_row {
    struct fetched f;
    int            rc;          /* 0, or 1 with cls and reason */
    int            cls;
    char           reason[2048];
    const char    *here;
    int            selected;
    int            archive_checked;
    int            upstream_only;   /* its archive is only upstream: not read here */
};

/* One file an archive must hold, for one row. */
struct expect {
    const char *path;           /* inside the archive: <prefix>/<file> */
    char       *owned;          /* the string path points into */
    const char *digest;
    unsigned long long size;
    size_t      row;
    int         seen;
};

static int by_expect(const void *x, const void *y)
{
    return strcmp(((const struct expect *)x)->path, ((const struct expect *)y)->path);
}

struct arch_check {
    struct expect     *ex;
    size_t             n;
    long               cur, last;   /* every entry claiming this path: cur .. last */
    struct pkg_sha256  sha;
    unsigned long long got;
};

static int ac_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct arch_check *ac = (struct arch_check *)ctx;
    struct expect key, *hit;
    if (e->is_dir) return 0;
    key.path = e->path;
    hit = (struct expect *)bsearch(&key, ac->ex, ac->n, sizeof *ac->ex, by_expect);
    if (hit == NULL) return 0;
    ac->cur = ac->last = (long)(hit - ac->ex);
    /* two packages may list the same file: all of them get this one read */
    while (ac->cur > 0 && strcmp(ac->ex[ac->cur - 1].path, e->path) == 0) ac->cur--;
    while (ac->last + 1 < (long)ac->n && strcmp(ac->ex[ac->last + 1].path, e->path) == 0) ac->last++;
    pkg_sha256_init(&ac->sha);
    ac->got = 0;
    return 1;
}

static int ac_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct arch_check *ac = (struct arch_check *)ctx;
    struct expect *x = &ac->ex[ac->cur];
    (void)e;
    if (len > 0) {
        ac->got += len;
        if (ac->got <= 0xFFFFFFFFFFFFull)   /* the sizes are compared at the end */
            pkg_sha256_update(&ac->sha, buf, len);
        return 0;
    }
    {
        unsigned char dg[PKG_SHA256_LEN];
        char hex[PKG_SHA256_HEXLEN + 1];
        size_t k;
        long c;
        pkg_sha256_final(&ac->sha, dg);
        for (k = 0; k < PKG_SHA256_LEN; k++) snprintf(hex + 2 * k, 3, "%02x", dg[k]);
        for (c = ac->cur; c <= ac->last; c++)
            ac->ex[c].seen = (ac->got == ac->ex[c].size && strcmp(hex, ac->ex[c].digest) == 0) ? 1 : 2;
        (void)x;
    }
    return 0;
}

/* SHOW <name>: the catalogue fields of its newest version, as a person
 * reads them on the portal, and as records for a program. */
static void show_about(const struct index *ix, const char *name)
{
    const struct entry *best = NULL;
    struct pkg_manifest m;
    const struct pkg_about *ab;
    unsigned char *buf;
    size_t len, o;
    char *mp, err[200];

    for (o = 0; o < ix->n; o++)
        if (strcmp(ix->e[o].name, name) == 0
            && (best == NULL || pkg_version_cmp(ix->e[o].version, best->version) > 0))
            best = &ix->e[o];
    if (best == NULL || (mp = object_path(chan_of(best), best->digest, "manifest")) == NULL)
        return;
    if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); return; }
    free(mp);
    if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) != 0) { free(buf); return; }
    free(buf);
    ab = &m.about;
    if (machine) {
        size_t i;
        if (ab->short_desc) kv("short", "%s", ab->short_desc);
        if (ab->category) kv("category", "%s", ab->category);
        for (i = 0; i < ab->tags.n; i++) kv("tag", "%s", ab->tags.v[i]);
        for (i = 0; i < ab->authors.n; i++) kv("author", "%s", ab->authors.v[i]);
        if (ab->homepage) kv("homepage", "%s", ab->homepage);
        if (ab->repository) kv("repository", "%s", ab->repository);
        if (ab->license) kv("license", "%s", ab->license);
        if (ab->distribution) kv("distribution", "%s", ab->distribution);
        for (i = 0; i < ab->description.n; i++) kv("description", "%s", ab->description.v[i]);
        for (i = 0; i < ab->changes.n; i++) kv("changes", "%s", ab->changes.v[i]);
    } else {
        size_t i;
        char line[1100];
        size_t at;
        if (ab->short_desc) say_kind(PKG_LINE_DETAIL, "  %s\n", "%s %s: %s", m.name, m.version, ab->short_desc);
        if (ab->category) say_kind(PKG_LINE_DETAIL, "  %s\n", "category   %s", ab->category);
        if (ab->tags.n) {
            for (i = 0, at = 0; i < ab->tags.n && at < sizeof line; i++)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? ", " : "", ab->tags.v[i]);
            say_kind(PKG_LINE_DETAIL, "  %s\n", "tags       %s", line);
        }
        if (ab->authors.n) {
            for (i = 0, at = 0; i < ab->authors.n && at < sizeof line; i++)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? ", " : "", ab->authors.v[i]);
            say_kind(PKG_LINE_DETAIL, "  %s\n", "author     %s", line);
        }
        if (ab->license) say_kind(PKG_LINE_DETAIL, "  %s\n", "license    %s%s%s", ab->license,
                                  ab->distribution ? ", " : "", ab->distribution ? ab->distribution : "");
        if (ab->homepage) say_kind(PKG_LINE_DETAIL, "  %s\n", "homepage   %s", ab->homepage);
        if (ab->repository) say_kind(PKG_LINE_DETAIL, "  %s\n", "repository %s", ab->repository);
        if (ab->description.n) say_kind(PKG_LINE_DETAIL, "%s\n", "%s", "");
        for (i = 0; i < ab->description.n; i++)
            say_kind(PKG_LINE_DETAIL, "  %s\n", "%s", ab->description.v[i]);
        if (ab->changes.n) say_kind(PKG_LINE_DETAIL, "  %s\n", "changes in %s:", m.version);
        for (i = 0; i < ab->changes.n; i++)
            say_kind(PKG_LINE_DETAIL, "    %s\n", "%s", ab->changes.v[i]);
    }
    pkg_manifest_free(&m);
}

int cmd_show(const struct pkg_options *a)
{
    struct index ix;
    struct show_row *row = NULL;
    size_t i, shown = 0, bad = 0;
    int first_bad = 0;

    if (open_channels(a, &ix) != 0) return 1;
    row = (struct show_row *)calloc(ix.n ? ix.n : 1, sizeof *row);
    if (row == NULL) { free(ix.e); return refuse("out of memory"); }
    kv("result", "shown");

    /* 1. Each entry: manifest against the index, signature, withdrawal, and
     *    the payload of a .pkg package. */
    show_defers_archives = 1;
    doing_things("checking", chan_label(a->channel), (long long)ix.n, "version");
    for (i = 0; i < ix.n; i++) {
        struct show_row *r = &row[i];
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        counting(i, ix.n);
        if (cancelled("while checking the channel, which was not changed")) {
            show_defers_archives = 0;
            did();
            for (i = 0; i < ix.n; i++) if (row[i].selected) fetched_free(&row[i].f);
            free(row); free(ix.e);
            return 1;
        }
        quiet = 1;
        refused_class = 0;
        quiet_reason[0] = '\0';
        r->rc = fetch(chan_of(&ix.e[i]), &ix.e[i], &r->f);
        quiet = 0;
        r->cls = refused_class;
        snprintf(r->reason, sizeof r->reason, "%s", quiet_reason);
        refused_class = 0;
        r->selected = 1;
        if (a->archive != NULL) {
            /* only what this archive holds */
            char an[1024], ap[1024];
            if (r->rc != 0 || r->f.m.source == NULL
                || !pkg_archive_split(r->f.m.source, an, sizeof an, ap, sizeof ap)
                || strcmp(an, a->archive) != 0) {
                fetched_free(&r->f);
                r->selected = 0;
            }
        }
    }
    did();
    show_defers_archives = 0;

    /* 2. Each archive once: every file of every entry that names it. */
    if (!a->metadata) {
        size_t r0;
        for (r0 = 0; r0 < ix.n; r0++) {
            char an[1024], ap[1024], *path;
            struct arch_check ac;
            size_t n = 0, cap = 0, r1, fl;
            char err[300];
            int rc;
            if (!row[r0].selected || row[r0].rc != 0 || row[r0].f.m.source == NULL
                || row[r0].archive_checked)
                continue;
            pkg_archive_split(row[r0].f.m.source, an, sizeof an, ap, sizeof ap);
            memset(&ac, 0, sizeof ac);
            for (r1 = r0; r1 < ix.n; r1++) {
                char bn[1024], bp[1024];
                struct pkg_manifest *m = &row[r1].f.m;
                if (!row[r1].selected || row[r1].rc != 0 || m->source == NULL
                    || !pkg_archive_split(m->source, bn, sizeof bn, bp, sizeof bp) || strcmp(bn, an) != 0)
                    continue;
                row[r1].archive_checked = 1;
                for (fl = 0; fl < m->nfiles; fl++) {
                    size_t pl = strlen(bp) + strlen(m->files[fl].path) + 2;
                    if (n == cap) {
                        struct expect *g;
                        cap = cap ? cap * 2 : 256;
                        g = (struct expect *)realloc(ac.ex, cap * sizeof *g);
                        if (g == NULL) break;
                        ac.ex = g;
                    }
                    ac.ex[n].owned = (char *)malloc(pl);
                    if (ac.ex[n].owned == NULL) break;
                    snprintf(ac.ex[n].owned, pl, "%s%s%s", bp, bp[0] ? "/" : "", m->files[fl].path);
                    ac.ex[n].path = ac.ex[n].owned;
                    ac.ex[n].digest = m->files[fl].digest;
                    ac.ex[n].size = m->files[fl].size;
                    ac.ex[n].row = r1;
                    ac.ex[n].seen = 0;
                    n++;
                }
            }
            ac.n = n;
            qsort(ac.ex, n, sizeof *ac.ex, by_expect);
            path = archive_path(chan_of(&ix.e[r0]), an);
            if ((path == NULL || !pkg_fs_exists(path)) && n > 0 && row[ac.ex[0].row].f.m.archive_sha) {
                /* published upstream: a copy downloaded before, else nothing to read here */
                const struct pkg_manifest *um = &row[ac.ex[0].row].f.m;
                char *cd = pkg_cache_dir(), rel[1200];
                free(path);
                snprintf(rel, sizeof rel, "upstream/%s/%s", um->archive_sha, an);
                path = cd ? pkg_join(cd, rel) : NULL;
                free(cd);
                if (path == NULL || !pkg_fs_exists(path)) {
                    for (fl = 0; fl < n; fl++) {
                        row[ac.ex[fl].row].upstream_only = 1;
                        ac.ex[fl].seen = 1;
                    }
                }
            }
            tr("checking %lu files of %s in one read", (unsigned long)n, an);
            if (n > 0 && row[ac.ex[0].row].upstream_only) {
                tr("%s is published upstream and not downloaded here: not read", an);
            } else if (path == NULL || !pkg_fs_exists(path)) {
                for (fl = 0; fl < n; fl++) ac.ex[fl].seen = 3;
            } else if (doing("reading", "the archive"),
                       rc = pkg_archive_walk(path, ac_want, ac_data, &ac, err, sizeof err),
                       did(), rc != 0) {
                for (fl = 0; fl < n; fl++) if (ac.ex[fl].seen == 0) ac.ex[fl].seen = 4;
            }
            for (fl = 0; fl < n; fl++) {
                struct show_row *r = &row[ac.ex[fl].row];
                if (ac.ex[fl].seen == 1 || r->rc != 0)
                    continue;
                r->rc = 1;
                if (ac.ex[fl].seen == 3) {
                    r->cls = 11;
                    snprintf(r->reason, sizeof r->reason, "it comes from the archive %s, which the "
                             "channel does not have", an);
                } else {
                    r->cls = 12;
                    snprintf(r->reason, sizeof r->reason, "the archive %s %s %s, which the signed "
                             "manifest lists", an, ac.ex[fl].seen == 2 ? "holds a different" :
                             ac.ex[fl].seen == 4 ? "could not be read for" : "lacks", ac.ex[fl].path);
                }
            }
            for (fl = 0; fl < n; fl++) free(ac.ex[fl].owned);
            free(ac.ex);
            free(path);
        }
    }

    /* 3. What was found, entry by entry. */
    {
        static const int widths[] = { 20, 8, 11, 8, 10, 0 };
        if (!machine && ix.n > 0)
            tbl_head(widths, "Package\tVersion\tKind\tArch\tStatus\tSigner");
    }
    for (i = 0; i < ix.n; i++) {
        struct show_row *r = &row[i];
        struct fetched *fp = &r->f;
        const char *status = "ok", *here = NULL;
        int rc = r->rc;
        if (!r->selected)
            continue;
        shown++;
        if (rc != 0) {
            status = class_name(r->cls);
            bad++;
            if (!first_bad) first_bad = r->cls;
        } else if (is_withdrawn(&ix.e[i])) {
            status = "withdrawn";
        }
        if (a->root != NULL) {
            /* Against a root: is this the version installed there? */
            struct pkg_manifest cur;
            int q = quiet;
            quiet = 1;
            if (load_installed(a->root, ix.e[i].name, &cur, 1) == 0) {
                here = pkg_version_cmp(cur.version, ix.e[i].version) == 0
                       && (strcmp(cur.architecture, ix.e[i].arch) == 0
                           || strcmp(ix.e[i].arch, "generic") == 0) ? "installed"
                       : "other-version";
                pkg_manifest_free(&cur);
            } else {
                here = "no";
            }
            quiet = q;
            refused_class = 0;
        }
        if (machine) {
            char j[400];
            snprintf(j, sizeof j, "%s %s %s %s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                     rc == 0 ? fp->m.kind : "-", rc == 0 ? fp->m.architecture : "-", status,
                     rc == 0 ? fp->signer : "-", here ? " " : "", here ? here : "");
            rec_item("entry", j, "name", ix.e[i].name, "version", ix.e[i].version,
                     "kind", rc == 0 ? fp->m.kind : "-", "architecture", rc == 0 ? fp->m.architecture : "-",
                     "status", status, "signer", rc == 0 ? fp->signer : "-",
                     here ? "installed" : NULL, here, NULL);
            if (rc == 0 && fp->m.source != NULL && a->metadata) {
                snprintf(j, sizeof j, "%s %s unchecked", ix.e[i].name, ix.e[i].version);
                rec_item("archive", j, "package", ix.e[i].name, "version", ix.e[i].version,
                         "state", "unchecked", NULL);
            } else if (rc == 0 && r->upstream_only) {
                snprintf(j, sizeof j, "%s %s upstream %s", ix.e[i].name, ix.e[i].version,
                         fp->m.archive_url);
                rec_item("archive", j, "package", ix.e[i].name, "version", ix.e[i].version,
                         "state", "upstream", "url", fp->m.archive_url, NULL);
            }
            if (rc == 0) {
                size_t d;
                for (d = 0; d < fp->m.ndeps; d++) {
                    snprintf(j, sizeof j, "%s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                             fp->m.deps[d].name, fp->m.deps[d].min ? " >= " : "",
                             fp->m.deps[d].min ? fp->m.deps[d].min : "");
                    rec_item("depends", j, "package", ix.e[i].name, "version", ix.e[i].version,
                             "needs", fp->m.deps[d].name, "min", fp->m.deps[d].min ? fp->m.deps[d].min : "",
                             NULL);
                }
            } else {
                char jp[2600];
                snprintf(jp, sizeof jp, "%s %s %s", ix.e[i].name, ix.e[i].version, r->reason);
                rec_item("problem", jp, "package", ix.e[i].name, "version", ix.e[i].version,
                         "reason", r->reason, NULL);
            }
        } else {
            tbl_row("%s\t%s\t%s\t%s\t%s\t%.16s%s", ix.e[i].name, ix.e[i].version,
                    rc == 0 ? fp->m.kind : "-", rc == 0 ? fp->m.architecture : "-", status,
                    rc == 0 ? fp->signer : "-",
                    rc == 0 && fp->m.source != NULL && a->metadata ? "  (archive not checked)" : "");
            if (rc != 0)
                say_kind(PKG_LINE_DETAIL, "  %s\n", "%s", r->reason);
        }
        if (rc == 0 && fp->m.ignored.n > 0) {
            /* lines for other systems: kept, signed, not acted on; a typo shows here */
            char keys[600];
            size_t k, at = 0;
            for (k = 0; k < fp->m.ignored.n && at < sizeof keys; k++)
                at += (size_t)snprintf(keys + at, sizeof keys - at, "%s%s", k ? ", " : "", fp->m.ignored.v[k]);
            if (machine) {
                for (k = 0; k < fp->m.ignored.n; k++)
                    kv("ignored", "%s %s %s", ix.e[i].name, ix.e[i].version, fp->m.ignored.v[k]);
            } else {
                say_kind(PKG_LINE_DETAIL, "  %s\n", "ignored, for other systems: %s", keys);
            }
        }
        fetched_free(fp);
    }
    if (!machine && ix.n > 0)
        tbl_end();
    free(row);
    for (i = 0; i < ix.n; i++) {
        char others[600], claim[65];
        size_t j;
        int earlier = 0;
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        for (j = 0; j < i; j++)
            if (strcmp(ix.e[j].name, ix.e[i].name) == 0) earlier = 1;
        if (earlier || !claimed_signer(chan_of(&ix.e[i]), ix.e[i].digest, claim))
            continue;
        if (other_signers(&ix, ix.e[i].name, claim, others, sizeof others) > 0) {
            if (machine)
                kv("warning", "%s is signed by more than one key: %s on %s, and %s", ix.e[i].name,
                   claim, ix.e[i].version, others);
            else
                say_kind(PKG_LINE_WARNING, "  warning: %s\n", "%s is signed by more than one key: %.16s on %s, and %s",
                    ix.e[i].name, claim, ix.e[i].version, others);
        }
    }
    if (a->target != NULL && shown > 0)
        show_about(&ix, a->target);
    kv("count", "%lu", (unsigned long)shown);
    kv("bad", "%lu", (unsigned long)bad);
    if (!machine && shown == 0)
        say_result("%s%s offers nothing%s%s", a->channel, "", a->target ? " named " : "",
                a->target ? a->target : "");
    if (shown == 0 && a->target != NULL)
        hint("no package is published as %s in this channel: before a first publish, the "
             "name is free; otherwise check the spelling with SHOW CHANNEL alone", a->target);
    free(ix.e);
    refused_class = first_bad;
    if (bad == 0)
        return 0;
    refused_next = next_default(first_bad);
    if (!machine)
        say_err("pkg show: %lu of %lu entries fail their checks\n  next: %s\n",
                (unsigned long)bad, (unsigned long)shown, next_words(refused_next));
    else
        kv("next", "%s", refused_next);
    return 1;
}

/* The image alone, for a person who wants the file without a channel. */
int cmd_image(const struct pkg_options *a)
{
    struct drawer d;
    unsigned skipped = 0;
    const char *vol;

    if (a->target == NULL) return refuse_c(20, "name the drawer to turn into an image");
    if (a->out == NULL) return refuse_c(20, "name the image file with OUT <file>");
    if (!pkg_fs_is_dir(a->target)) return refuse_c(20, "\"%s\" is not a directory", a->target);
    memset(&d, 0, sizeof d);
    d.root = a->target;
    if (pkg_fs_walk(a->target, load_one, leave_out, &d, &skipped, d.err, sizeof d.err) != 0) {
        refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
        drawer_free(&d);
        return 1;
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);
    if (drawer_attrs(&d) != 0) { drawer_free(&d); return 1; }
    {
        char vn[65], vv[64], seen[400];
        const char *vf;
        vol = a->name;
        if (vol == NULL && find_ver(&d, NULL, vn, sizeof vn, vv, sizeof vv, &vf, seen, sizeof seen) > 0) {
            static char named[65];
            snprintf(named, sizeof named, "%s", vn);
            vol = named;
        }
        if (vol == NULL)
            vol = "image";
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->target);
    }
    if (to_image(&d, vol) != 0) { drawer_free(&d); return 1; }
    if (pkg_fs_write_atomic(a->out, d.v[0].data, d.v[0].len) != 0) {
        drawer_free(&d);
        return refuse_c(17, "cannot write \"%s\": %s", a->out, strerror(errno));
    }
    kv("result", "created");
    kv("file", "%s", a->out);
    kv("volume", "%s", vol);
    kv("blocks", "%lu", (unsigned long)(d.v[0].len / PKG_IMAGE_BLOCK));
    if (!machine)
        say_result("wrote %s: volume %s, %lu blocks of %u bytes", a->out, vol,
                (unsigned long)(d.v[0].len / PKG_IMAGE_BLOCK), PKG_IMAGE_BLOCK);
    drawer_free(&d);
    return 0;
}

int cmd_manifest(const struct pkg_options *a)
{
    struct built b;
    if (build_package(a, &b) != 0) { built_free(&b); return 1; }
    if (machine) {
        /* The manifest is already "Key: value" lines: one field each. */
        size_t at = 0;
        kv("result", "shown");
        while (at < b.text_len) {
            const char *ls = b.text + at, *nl = memchr(ls, '\n', b.text_len - at);
            size_t ll = nl ? (size_t)(nl - ls) : b.text_len - at;
            const char *colon = memchr(ls, ':', ll);
            if (colon != NULL && colon + 2 <= ls + ll) {
                char key[32];
                size_t kl = (size_t)(colon - ls);
                if (kl < sizeof key) {
                    memcpy(key, ls, kl);
                    key[kl] = '\0';
                    kv(key, "%.*s", (int)(ll - kl - 2), colon + 2);
                }
            }
            at += ll + 1;
        }
    } else {
        say_raw(b.text, b.text_len);
    }
    built_free(&b);
    return 0;
}

/* 1 when the file at `path` holds exactly the bytes whose SHA-256 is `digest`. */
static int object_good(const char *path, const char *digest)
{
    unsigned char *buf;
    size_t len;
    char hex[PKG_SHA256_HEXLEN + 1];
    if (path == NULL || pkg_fs_read(path, &buf, &len) != 0)
        return 0;
    pkg_sha256_hex(buf, len, hex);
    free(buf);
    return strcmp(hex, digest) == 0;
}

/* The same drawer published again as the same version. Nothing to do when
 * the channel holds it intact; when its manifest, payload or signature is
 * missing or damaged, and the key is the publisher's (the key of the
 * package's first version here), those objects are written again: the bytes
 * are the ones the index already names, so nothing published changes. */
static int republish(const struct pkg_options *a, const struct index *ix, const struct built *b,
                     const struct key *k, const char *mdigest)
{
    char *mo = object_path(a->channel, mdigest, "manifest");
    char *po = b->m.payload ? object_path(a->channel, b->m.payload, "pkg") : NULL;
    char *so = object_path(a->channel, mdigest, "sig");
    char signer[65], first_signer[65];
    const struct entry *oldest = NULL;
    int good_m, good_p, good_s = 0, rc = 0;
    size_t o;

    good_m = object_good(mo, mdigest);
    good_p = b->m.payload ? object_good(po, b->m.payload) : 1;   /* else the archive holds it */
    if (so != NULL) {
        int q = quiet;
        quiet = 1;
        good_s = check_sig(so, (const unsigned char *)b->text, b->text_len, signer, "") == 0;
        quiet = q;
        refused_class = 0;
        refused_next = NULL;
    }
    if (good_m && good_p && good_s) {
        kv("result", "unchanged");
        kv("name", "%s", b->m.name);
        kv("version", "%s", b->m.version);
        if (!machine)
            say_result("%s %s is already published with this exact content; nothing to do",
                b->m.name, b->m.version);
        goto out;
    }
    for (o = 0; o < ix->n; o++)
        if (strcmp(ix->e[o].name, b->m.name) == 0
            && (oldest == NULL || pkg_version_cmp(ix->e[o].version, oldest->version) < 0))
            oldest = &ix->e[o];
    if (oldest == NULL || !claimed_signer(a->channel, oldest->digest, first_signer)
        || strcmp(first_signer, k->pkhex) != 0) {
        rc = refuse_n(14, "ask-requester", "%s %s in %s is damaged, and only its publisher's key "
                      "(the key of its first version here) may write it again; this key is %s. "
                      "Nothing was changed", b->m.name, b->m.version, a->channel, k->pkhex);
        goto out;
    }
    tr("repairing %s %s: manifest %s, payload %s, signature %s", b->m.name, b->m.version,
       good_m ? "intact" : "damaged", good_p ? "intact" : "damaged", good_s ? "intact" : "damaged");
    if (dry_run) {
        kv("result", "would-repair");
    } else {
        if ((!good_m && pkg_fs_write_atomic(mo, b->text, b->text_len) != 0)
            || (!good_p && pkg_fs_write_atomic(po, b->pkg, b->pkg_len) != 0)
            || (!good_s && write_sig(so, k, (const unsigned char *)b->text, b->text_len) != 0)) {
            rc = refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
            goto out;
        }
        kv("result", "repaired");
    }
    kv("name", "%s", b->m.name);
    kv("version", "%s", b->m.version);
    if (!good_m) kv("repaired", "manifest");
    if (!good_p) kv("repaired", "payload");
    if (!good_s) kv("repaired", "signature");
    if (!machine)
        say_result("%s %s %s in %s:%s%s%s written again from the same bytes",
            dry_run ? "would repair" : "repaired", b->m.name, b->m.version, a->channel,
            good_m ? "" : " manifest", good_p ? "" : " payload", good_s ? "" : " signature");
out:
    free(mo); free(po); free(so);
    return rc;
}

/* A new version against the highest one published: the files that changed,
 * and the slips that show there (the old build again, a $VER not raised). */
/* Whether two versions say the same about themselves: dependencies,
 * Provides and the catalogue fields, all but files and identity. */
static int same_description(const struct pkg_manifest *x, const struct pkg_manifest *y)
{
    struct pkg_manifest tx = *x, ty = *y;
    char *ox = NULL, *oy = NULL;
    size_t lx = 0, ly = 0;
    int same;
    tx.version = ty.version = (char *)"0";
    tx.payload = ty.payload = NULL;
    tx.source = ty.source = NULL;
    tx.archive_sha = ty.archive_sha = NULL;
    tx.nfiles = ty.nfiles = 0;
    tx.ncontent = ty.ncontent = 0;
    if (pkg_manifest_emit(&tx, &ox, &lx) != 0 || pkg_manifest_emit(&ty, &oy, &ly) != 0) {
        free(ox);
        free(oy);
        return 0;
    }
    same = lx == ly && memcmp(ox, oy, lx) == 0;
    free(ox);
    free(oy);
    return same;
}

/* A build may move identical program files into a new upstream archive.
 * Its signed retrieval information must advance with that archive. */
static int same_optional_text(const char *x, const char *y)
{
    return x == NULL || y == NULL ? x == y : strcmp(x, y) == 0;
}

static int same_source(const struct pkg_manifest *x, const struct pkg_manifest *y)
{
    return same_optional_text(x->source, y->source)
        && same_optional_text(x->archive_url, y->archive_url)
        && same_optional_text(x->archive_sha, y->archive_sha)
        && x->archive_size == y->archive_size;
}

static long compare_last(const struct pkg_manifest *em, const struct built *b)
{
    const struct pkg_file *nv = b->m.ncontent ? b->m.content : b->m.files;
    const struct pkg_file *ov = em->ncontent ? em->content : em->files;
    size_t nn = b->m.ncontent ? b->m.ncontent : b->m.nfiles;
    size_t on = em->ncontent ? em->ncontent : em->nfiles;
    size_t i, j, changed = 0;
    int comparable = (b->m.ncontent > 0) == (em->ncontent > 0);

    if (pkg_version_cmp(b->m.version, em->version) == 0)
        return -1;
    if (b->cookie_ver[0] && pkg_version_cmp(b->cookie_ver, em->version) == 0)
        warn("the $VER cookie in %s says %s, the version already published: this is the %s build "
             "again, changed or not, or a new build whose $VER was not raised. Ask which before "
             "publishing it as %s", b->cookie_file, em->version, em->version, b->m.version);
    if (!comparable) {
        if (dry_run)
            hint("%s %s was published before its image listed its files; nothing to compare "
                 "file by file", em->name, em->version);
        return -1;
    }
    if (dry_run)
        kv("compared-with", "%s %s", em->name, em->version);
    for (i = 0; i < nn; i++) {
        for (j = 0; j < on; j++)
            if (strcmp(nv[i].path, ov[j].path) == 0)
                break;
        if (j == on) {
            changed++;
            if (dry_run) kv("added", "%s %llu", nv[i].path, nv[i].size);
        } else if (strcmp(nv[i].digest, ov[j].digest) != 0 || nv[i].prot != ov[j].prot
                   || strcmp(nv[i].comment ? nv[i].comment : "", ov[j].comment ? ov[j].comment : "") != 0) {
            changed++;
            if (dry_run) kv("changed", "%s %llu %llu", nv[i].path, ov[j].size, nv[i].size);
        } else if (dry_run) {
            kv("same", "%s", nv[i].path);
        }
    }
    for (j = 0; j < on; j++) {
        for (i = 0; i < nn; i++)
            if (strcmp(nv[i].path, ov[j].path) == 0)
                break;
        if (i == nn) {
            changed++;
            if (dry_run) kv("gone", "%s", ov[j].path);
        }
    }
    if (changed == 0 && b->m.source == NULL)
        warn("every file is identical to %s %s's: nothing changed but the version number",
             em->name, em->version);
    return (long)changed;
}

int cmd_publish(const struct pkg_options *a)
{
    struct built b;
    struct index ix;
    struct key k;
    size_t i;
    char *po = NULL, *mo = NULL, *so = NULL, s12[13];
    char mdigest[PKG_SHA256_HEXLEN + 1];
    int rc = 1, new_channel, keyless = 0;
    char same_as[160] = "";

    if (a->channel == NULL)
        return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (is_url(a->channel))
        return refuse_c(20, "PUBLISH writes into a channel on this machine; publish into a "
                        "directory, then PUSH it to %s", a->channel);
    new_channel = !pkg_fs_exists(a->channel);
    memset(&k, 0, sizeof k);
    if (a->sign == NULL && dry_run) {
        /* A preview needs no key: nobody should pick up someone else's key
         * only to see what a publish would do. */
        keyless = 1;
        snprintf(k.pkhex, sizeof k.pkhex, "none");
    } else if (load_key(a->sign, &k) != 0)
        return 1;
    if (read_index(a->channel, &ix) != 0) return 1;
    inherit_ix = &ix;
    inherit_channel = a->channel;
    rc = build_package(a, &b);
    inherit_ix = NULL;
    inherit_channel = NULL;
    if (rc != 0) { free(ix.e); built_free(&b); return 1; }
    rc = 1;
    if (b.m.source != NULL && b.m.archive_sha == NULL) {
        /* The files stay in the archive; installers find it in the channel. */
        char an[1024], ai[1024], *ap;
        pkg_archive_split(b.m.source, an, sizeof an, ai, sizeof ai);
        ap = archive_path(a->channel, an);
        if (ap == NULL || !pkg_fs_exists(ap)) {
            refuse_c(20, "the files of %s come from %s, which installers look for as %s; put the "
                     "archive there (a copy or a link) and publish again", b.m.name, an,
                     ap ? ap : "archives/<name> in the channel");
            free(ap);
            free(ix.e);
            built_free(&b);
            return 1;
        }
        free(ap);
    }
    if (strcmp(b.m.architecture, "generic") == 0
        && (strcmp(b.m.kind, "image") == 0 || strcmp(b.m.kind, "application") == 0
            || strcmp(b.m.kind, "library") == 0 || strcmp(b.m.kind, "device") == 0
            || strcmp(b.m.kind, "class") == 0))
        warn("no executable in the drawer: kind %s is usually a program, and pkg found no ELF or "
             "hunk header, so it is published as generic, for every CPU. Check the drawer holds "
             "the build, not a script or a placeholder", b.m.kind);
    /* A version is named by its manifest, which names its payload. */
    pkg_sha256_hex(b.text, b.text_len, mdigest);

    for (i = 0; i < ix.n; i++) {
        if (strcmp(ix.e[i].name, b.m.name) == 0
            && pkg_version_cmp(ix.e[i].version, b.m.version) == 0
            && strcmp(ix.e[i].arch, b.m.architecture) == 0) {
            if (strcmp(ix.e[i].digest, mdigest) == 0) {
                rc = republish(a, &ix, &b, &k, mdigest);
            } else {
                if (b.ver_from[0])
                    refuse_c(15, "%s %s is already published with a different payload, and %s "
                             "came from the $VER cookie in %s: either this is a new build whose "
                             "$VER was not raised (raise it, or give VERSION), or it is the old "
                             "build, changed; check which before publishing",
                             b.m.name, ix.e[i].version, ix.e[i].version, b.ver_from);
                else
                    refuse_c(15, "%s %s is already published with a different payload; a "
                             "published version never changes, so publish this as a new version",
                             b.m.name, ix.e[i].version);
            }
            free(ix.e);
            built_free(&b);
            return rc;
        }
    }

    {
        /* A later version signed by another key than the channel's first
         * version of the package is refused by every root that trusts that
         * key. Changing keys is the publisher's decision, stated with
         * ACCEPTKEY and the new key. */
        const struct entry *oldest = NULL;
        char first_signer[65];
        size_t o;
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, b.m.name) == 0
                && (oldest == NULL || pkg_version_cmp(ix.e[o].version, oldest->version) < 0))
                oldest = &ix.e[o];
        if (keyless && oldest != NULL && claimed_signer(a->channel, oldest->digest, first_signer)) {
            kv("first-signer", "%s", first_signer);
            hint("the real publish must be signed with the key that signed %s %s, %s: find out "
                 "from whoever requested this whose key that is and whether it is theirs to use",
                 b.m.name, oldest->version, first_signer);
        } else if (keyless)
            hint("the real publish needs a key: the requester's, if they have published before");
        if (!keyless && oldest != NULL && claimed_signer(a->channel, oldest->digest, first_signer)
            && strcmp(first_signer, k.pkhex) != 0
            && (a->acceptkey == NULL || strcmp(a->acceptkey, k.pkhex) != 0)) {
            kv("signer", "%s", k.pkhex);
            kv("first-signer", "%s", first_signer);
            refuse_n(14, "ask-requester", "%s %s, the first version in %s, is signed by %s, and this "
                     "key is %s: every machine that trusts the first key would refuse this "
                     "version. Sign it with the publisher's key; changing keys is the requester's "
                     "decision. Nothing was published", b.m.name, oldest->version, a->channel,
                     first_signer, k.pkhex);
            free(ix.e);
            built_free(&b);
            memset(&k, 0, sizeof k);
            return 1;
        }
    }
    {
        /* Compared with the highest version already published: a kind that
         * changes (files installed loose, then an image) or a dependency that
         * disappears is usually a slip, and it is said before it ships. */
        size_t o, d2, d3;
        const struct entry *last = NULL;
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, b.m.name) == 0
                && (last == NULL || pkg_version_cmp(ix.e[o].version, last->version) > 0))
                last = &ix.e[o];
        if (last != NULL) {
            char *mp = object_path(a->channel, last->digest, "manifest"), err[200];
            unsigned char *mbuf;
            size_t mlen;
            struct pkg_manifest em;
            if (mp != NULL && pkg_fs_read(mp, &mbuf, &mlen) == 0) {
                if (pkg_manifest_parse((const char *)mbuf, mlen, &em, err, sizeof err) == 0) {
                    if (strcmp(em.kind, b.m.kind) != 0 && a->name == NULL) {
                        /* The name came from a $VER cookie: another program's
                         * cookie would make this package replace that one. */
                        refuse_c(20, "the name %s comes from the $VER cookie in %s, and %s is "
                                 "already published as kind %s, not %s: this would replace it "
                                 "wherever it is installed. If the drawer holds the right "
                                 "program, add NAME <its package name>; nothing was published",
                                 b.m.name, b.name_from[0] ? b.name_from : "the drawer",
                                 em.name, em.kind, b.m.kind);
                        pkg_manifest_free(&em);
                        free(mbuf);
                        free(mp);
                        free(ix.e);
                        built_free(&b);
                        memset(&k, 0, sizeof k);
                        return 1;
                    }
                    if (compare_last(&em, &b) == 0 && a->build != NULL
                        && strcmp(em.kind, b.m.kind) == 0 && strcmp(em.architecture, b.m.architecture) == 0
                        && same_description(&em, &b.m) && same_source(&em, &b.m)) {
                        /* Identical files and retrieval information need no new build. */
                        snprintf(same_as, sizeof same_as, "%s %s", em.name, em.version);
                    }
                    if (strcmp(em.kind, b.m.kind) != 0)
                        warn("%s %s was published as kind %s, and this version is kind %s",
                             em.name, em.version, em.kind, b.m.kind);
                    for (d2 = 0; d2 < em.ndeps; d2++) {
                        int kept_dep = 0;
                        for (d3 = 0; d3 < b.m.ndeps; d3++)
                            if (strcmp(em.deps[d2].name, b.m.deps[d3].name) == 0)
                                kept_dep = 1;
                        if (!kept_dep)
                            warn("%s %s depends on %s, and this version does not: DEPENDS is "
                                 "not carried from one version to the next", em.name,
                                 em.version, em.deps[d2].name);
                    }
                    pkg_manifest_free(&em);
                }
                free(mbuf);
            }
            free(mp);
        }
    }
    if (same_as[0]) {
        kv("result", "unchanged");
        kv("name", "%s", b.m.name);
        kv("version", "%s", b.m.version);
        kv("same-as", "%s", same_as);
        if (!machine)
            say_result("%s %s: every file is that of %s, so no new version is published", b.m.name,
                b.m.version, same_as);
        rc = 0;
        goto out;
    }
    if (dry_run) {
        size_t d;
        kv("result", "would-publish");
        kv("name", "%s", b.m.name);
        kv("version", "%s", b.m.version);
        kv("kind", "%s", b.m.kind);
        kv("architecture", "%s", b.m.architecture);
        kv("channel", "%s", a->channel);
        if (b.m.ndeps == 0)
            kv("depends", "none");
        for (d = 0; d < b.m.ndeps; d++)
            kv("depends", "%s%s%s", b.m.deps[d].name, b.m.deps[d].min ? " >= " : "",
               b.m.deps[d].min ? b.m.deps[d].min : "");
        for (d = 0; d < b.m.nfiles; d++)
            kv("file", "%s %llu", b.m.files[d].path, b.m.files[d].size);
        for (d = 0; d < b.m.ncontent; d++)
            kv("content", "%s %llu", b.m.content[d].path, b.m.content[d].size);
        kv("signer", "%s", k.pkhex);
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.name_from[0]) kv("name-from", "%s", b.name_from);
        if (b.kind_from[0]) kv("kind-from", "%s", b.kind_from);
        if (b.nconfig) kv("config-files", "%lu", (unsigned long)b.nconfig);
        if (b.config_from[0]) kv("config-from", "%s", b.config_from);
        if (b.about_from[0]) kv("about-from", "%s", b.about_from);
        if (b.info_from[0]) kv("info-from", "%s", b.info_from);
        if (b.info_fields[0]) kv("info-fields", "%s", b.info_fields);
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
        if (b.arch_from[0]) kv("arch-from", "%s", b.arch_from);
        for (d = 0; d < b.nleft; d++)
            kv("left-out", "%s", b.left_out[d]);
        if (!machine) {
            say_result("would publish %s %s (%s, %s) to %s, signed by %.16s", b.m.name, b.m.version,
                    b.m.kind, b.m.architecture, a->channel, k.pkhex);
            for (d = 0; d < b.m.ndeps; d++)
                say_item("depends", "%s%s%s", b.m.deps[d].name, b.m.deps[d].min ? " >= " : "",
                        b.m.deps[d].min ? b.m.deps[d].min : "");
            if (b.m.ndeps == 0)
                say_item("depends", "nothing");
            for (d = 0; d < b.m.nfiles; d++)
                say_item("file", "%s (%llu bytes)", b.m.files[d].path, b.m.files[d].size);
            for (d = 0; d < b.m.ncontent; d++)
                say_item("content", "%s (%llu bytes)", b.m.content[d].path, b.m.content[d].size);
            for (d = 0; d < b.nleft; d++)
                say_item("left out", "%s", b.left_out[d]);
            if (b.ver_from[0] || b.name_from[0])
                say_detail("%s from $VER: in %s", b.ver_from[0] && b.name_from[0] ? "name and version"
                    : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
            if (b.kind_from[0] || b.deps_from[0])
                say_detail("%s from %s, published before", b.kind_from[0] && b.deps_from[0] ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies", b.kind_from[0] ? b.kind_from : b.deps_from);
            say_info_from(&b);
        }
        if (new_channel)
            hint("there is no channel at %s yet: publishing creates it. Check it is the one "
                 "meant", a->channel);
        rc = 0;
        goto out;
    }
    po = b.m.payload ? object_path(a->channel, b.m.payload, "pkg")   /* content-addressed */
                     : pkg_strdup("");                                 /* the archive holds it */
    mo = object_path(a->channel, mdigest, "manifest");      /* one per version */
    so = object_path(a->channel, mdigest, "sig");
    if (po == NULL || mo == NULL || so == NULL
        || (b.m.payload && pkg_fs_write_atomic(po, b.pkg, b.pkg_len) != 0)
        || pkg_fs_write_atomic(mo, b.text, b.text_len) != 0
        || write_sig(so, &k, (const unsigned char *)b.text, b.text_len) != 0) {
        refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
        goto out;
    }
    {
        struct entry *w = (struct entry *)realloc(ix.e, (ix.n + 1u) * sizeof *w);
        if (w == NULL) { refuse("out of memory"); goto out; }
        ix.e = w;
        snprintf(ix.e[ix.n].name, sizeof ix.e[ix.n].name, "%s", b.m.name);
        snprintf(ix.e[ix.n].version, sizeof ix.e[ix.n].version, "%s", b.m.version);
        snprintf(ix.e[ix.n].arch, sizeof ix.e[ix.n].arch, "%s", b.m.architecture);
        snprintf(ix.e[ix.n].digest, sizeof ix.e[ix.n].digest, "%s", mdigest);
        ix.n++;
    }
    if (write_index(a->channel, &ix) != 0) {
        refuse_c(17, "cannot write the channel index: %s", strerror(errno));
        goto out;
    }
    if (b.m.payload) short12(b.m.payload, s12);
    kv("result", "published");
    kv("name", "%s", b.m.name);
    kv("version", "%s", b.m.version);
    kv("channel", "%s", a->channel);
    kv("manifest", "%s", mdigest);
    if (b.m.payload) kv("payload", "%s", b.m.payload);
    else kv("source", "%s", b.m.source);
    kv("signer", "%s", k.pkhex);
    kv("files", "%lu", (unsigned long)b.m.nfiles);
    /* "packaged into" rather than "published to" for a channel on this
     * machine: the package is made and signed, and nothing has left the
     * machine until PUSH sends it. The record stays "published", which is
     * what the act is called and what reads it expects. */
    if (!machine)
    say_result("packaged %s %s into %s: %lu file%s, %s%s, signed by %.16s", b.m.name,
           b.m.version, a->channel, (unsigned long)b.m.nfiles, b.m.nfiles == 1 ? "" : "s",
           b.m.payload ? "payload " : "from ",
           b.m.payload ? s12 : b.m.source, k.pkhex);
    if (b.arch_from[0] && machine)
        kv("arch-from", "%s", b.arch_from);
    else if (b.arch_from[0])
        say_item("architecture", "%s, read from %s", b.m.architecture, b.arch_from);
    if (machine) {
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.name_from[0]) kv("name-from", "%s", b.name_from);
        if (b.kind_from[0]) kv("kind-from", "%s", b.kind_from);
        if (b.nconfig) kv("config-files", "%lu", (unsigned long)b.nconfig);
        if (b.config_from[0]) kv("config-from", "%s", b.config_from);
        if (b.about_from[0]) kv("about-from", "%s", b.about_from);
        if (b.info_from[0]) kv("info-from", "%s", b.info_from);
        if (b.info_fields[0]) kv("info-fields", "%s", b.info_fields);
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
    } else if (b.ver_from[0] || b.name_from[0]) {
        say_detail("%s taken from $VER: in %s", b.ver_from[0] && b.name_from[0] ? "name and version"
            : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
    }
    if (!machine && (b.kind_from[0] || b.deps_from[0]))
        say_detail("%s from %s, published before", b.kind_from[0] && b.deps_from[0]
            ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies",
            b.kind_from[0] ? b.kind_from : b.deps_from);
    if (!machine)
        say_info_from(&b);
    for (i = 0; i < b.nleft; i++) {
        if (machine)
            kv("left-out", "%s", b.left_out[i]);
        else
            say_item("left out", "%s, host metadata no Amiga uses", b.left_out[i]);
    }
    if (new_channel)
        hint("the channel %s did not exist and was created", a->channel);
    /* PUBLISH says "published", and a person who has only ever sent packages
     * to a portal reads that as "it is online now". It is not: the package
     * is in a channel on this disk, which is a channel like any other, and
     * PUSH is what sends it to a portal. */
    if (!dry_run)
        hint("the package is in the local channel %s: any machine that can read that directory "
             "installs from it with INSTALL %s ROOT <root> CHANNEL <that directory, as the "
             "machine names it>, and PUSH CHANNEL %s TO <portal channel> sends it to a portal",
             a->channel, b.m.name, a->channel);
    rc = 0;
out:
    memset(&k, 0, sizeof k);
    free(po); free(mo); free(so);
    free(ix.e);
    built_free(&b);
    return rc;
}

static int need_root_channel(const struct pkg_options *a)
{
    if (a->target == NULL)     return refuse_c(20, "name the package");
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->version && pkg_check_version(a->version))
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    return 0;
}

/* One package. `line` is NULL for INSTALL <name>, which reports for itself;
 * with several names the batch reports instead, and this leaves its sentence
 * there. */
static int install_one(const struct pkg_options *a, const char *target, char *line, size_t ll)
{
    struct index ix;
    const struct entry *e;
    struct plan p;
    struct pkg_manifest cur;
    unsigned long placed, dropped, kept;
    char s12[13];
    int rc = 1;

    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, &ix) != 0) return 1;
    {
        char cpus[200];
        if (arch_ambiguous(&ix, target, cpus, sizeof cpus)) {
            refuse_c(20, "%s is offered for several CPUs (%s), and nothing says which machine %s "
                     "is for: add ARCH <cpu>; the root remembers it from then on", target,
                     cpus, a->root);
            free(ix.e);
            return 1;
        }
    }
    e = pick(&ix, target, a->version);
    if (e == NULL) {
        if (!pick_refused) say_not_found(&ix, target, a->version, chans_text());
        free(ix.e);
        return 1;
    }
    if (load_installed(a->root, e->name, &cur, 1) == 0) {
        if (requested_at) {
            pkg_manifest_free(&cur); free(ix.e);
            return refuse_c(15, "%s is already installed; AT applies to a fresh installation", target);
        }
        if (is_auto(a->root, cur.name)) {
            /* Asked for by name now: no longer an orphan candidate. */
            set_auto(a->root, cur.name, 0);
            if (line != NULL) {
                snprintf(line, ll, "%s was installed as a dependency; it is now kept for itself",
                         cur.version);
            } else {
                kv("result", "kept");
                kv("name", "%s", cur.name);
                kv("version", "%s", cur.version);
                if (!machine)
                    say_result("%s %s was installed as a dependency; it is now kept for itself",
                            cur.name, cur.version);
            }
            pkg_manifest_free(&cur);
            free(ix.e);
            return 0;
        }
        if (pkg_version_cmp(cur.version, e->version) == 0) {
            /* The state asked for is the state there: a repeated INSTALL, as
             * an agent retrying after a timeout sends, succeeds and changes
             * nothing. */
            if (line != NULL) {
                snprintf(line, ll, "%s is already installed", cur.version);
            } else {
                kv("result", "unchanged");
                kv("name", "%s", cur.name);
                kv("version", "%s", cur.version);
                if (!machine)
                    say_result("%s %s is already installed in %s", cur.name, cur.version, a->root);
            }
            pkg_manifest_free(&cur);
            free(ix.e);
            return 0;
        }
        refuse_n(15, pkg_version_cmp(e->version, cur.version) > 0 ? "use-upgrade" : "ask-requester",
                 "%s %s is installed in %s, not %s; nothing was changed",
                 cur.name, cur.version, a->root, e->version);
        pkg_manifest_free(&cur);
        free(ix.e);
        return 1;
    }
    if (plan_target(&p, a, &ix, e->name, e->version) == 0
        && placement_prepare(a->root, &p.f[p.n - 1].m) == 0
        && run_plan(&p, NULL, &placed, &dropped, &kept) == 0) {
        const struct fetched *t = &p.f[p.n - 1];
        struct fetched f = *t;
        if (f.m.payload) short12(f.m.payload, s12);
        if (!dry_run)
            record_arch(a->root, p.f[p.n - 1].m.architecture);
        if (line != NULL) {
            snprintf(line, ll, "%s %s: %lu file%s, signed by %.16s",
                     dry_run ? "would install" : "installed", f.m.version, placed,
                     placed == 1 ? "" : "s", f.signer);
        } else {
        kv("result", "%s", res("installed", "would-install"));
        kv("name", "%s", f.m.name);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("files", "%lu", placed);
        if (f.m.payload) kv("payload", "%s", f.m.payload);
        else kv("source", "%s", f.m.source);
        kv("signer", "%s", f.signer);
        if (!machine)
        say_result("%s %s %s into %s: %lu file%s, %s%s, signed by %.16s",
               dry_run ? "would install" : "installed",
               f.m.name, f.m.version, a->root, placed, placed == 1 ? "" : "s",
               f.m.payload ? "payload " : "from ",
               f.m.payload ? s12 : f.m.source, f.signer);
        }
        if (line == NULL && strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1) {
            kv("image", "%s", f.m.files[0].path);
            kv("blocks", "%llu", f.m.files[0].size / PKG_IMAGE_BLOCK);
            if (!machine)
                say_item("image", "%s, %llu blocks", f.m.files[0].path,
                        f.m.files[0].size / PKG_IMAGE_BLOCK);
            hint("to run it, mount the image: MOUNTLIST %s ROOT %s OUT <file> writes the "
                 "mount entry and lists the steps", f.m.name, a->root);
        }
        rc = 0;
    }
    plan_free(&p);
    free(ix.e);
    return rc;
}


/* INSTALL a b c. Every name is tried, whatever the ones before it did, and
 * each is reported; the code is the worst class any of them refused with,
 * so a caller sees the worst that happened, not the first. One archive read
 * serves every name that comes out of it, since the first read leaves the
 * block map behind. */
static int install_many(const struct pkg_options *a)
{
    unsigned i, n = a->nalso + 1, done = 0, bad = 0;
    int worst = 0;
    const char *worst_next = NULL;
    char refused_names[600];
    size_t at = 0;

    refused_names[0] = '\0';
    if (need_root_channel(a) != 0) return 1;
    if (a->version != NULL || a->acceptkey != NULL || a->downgrade || a->key != NULL)
        return refuse_c(20, "%s is a decision about one package, never about several at once. "
                        "Give it to INSTALL <name> alone",
                        a->version != NULL ? "VERSION" : a->acceptkey != NULL ? "ACCEPTKEY"
                        : a->key != NULL ? "KEY" : "DOWNGRADE");
    for (i = 0; i < n; i++) {
        const char *name = i == 0 ? a->target : a->also[i - 1];
        char line[600];
        int rc;
        line[0] = '\0';
        quiet = 1;
        quiet_reason[0] = '\0';
        rc = install_one(a, name, line, sizeof line);
        quiet = 0;
        if (rc == 0) {
            done++;
            if (machine) {
                char jn[700];
                snprintf(jn, sizeof jn, "%s %s", name, line);
                rec_item("package", jn, "name", name, "result", line, NULL);
            } else {
                say_pkgline(name, "%s", line);
            }
        } else {
            char *q;
            bad++;
            for (q = quiet_reason; *q; q++)
                if (*q == '\n') *q = ' ';
            if (refused_class > worst) { worst = refused_class; worst_next = refused_next; }
            if (machine) {
                char jn[2700], codes[8];
                snprintf(codes, sizeof codes, "%d", refused_class);
                snprintf(jn, sizeof jn, "%s %s %s", name, class_name(refused_class), quiet_reason);
                rec_item("refused", jn, "name", name, "class", class_name(refused_class),
                         "code", codes, "reason", quiet_reason,
                         "next", refused_next ? refused_next : next_default(refused_class), NULL);
            } else {
                say_pkgline(name, "not installed: %s", quiet_reason);
            }
            if (at + 80 < sizeof refused_names)
                at += (size_t)snprintf(refused_names + at, sizeof refused_names - at, "%s%s (%s)",
                                       at ? ", " : "", name, class_name(refused_class));
        }
        refused_class = 0;
        refused_next = NULL;
    }
    {
        char summary[900];
        if (bad == 0)
            snprintf(summary, sizeof summary, "%s %u package%s into %s",
                     dry_run ? "would install" : "installed", done, done == 1 ? "" : "s", a->root);
        else
            snprintf(summary, sizeof summary, "%s %u of %u package%s into %s; not installed: %s. "
                     "Everything else went ahead", dry_run ? "would install" : "installed", done, n,
                     n == 1 ? "" : "s", a->root, refused_names);
        if (bad == 0) {
            kv("result", "%s", res("installed", "would-install"));
        } else {
            refused_class = worst;
            refused_next = worst_next;
            kv("result", "refused");
            kv("class", "%s", class_name(worst));
            kv("code", "%d", worst);
        }
        kv("root", "%s", a->root);
        kv("installed", "%u", done);
        kv("not-installed", "%u", bad);
        kv("count", "%u", done);
        kv("summary", "%s", summary);
        if (bad > 0)
            kv("next", "%s", worst_next ? worst_next : next_default(worst));
        if (!machine)
            say_result("%s", summary);
        return bad == 0 ? 0 : 1;
    }
}

int cmd_install(const struct pkg_options *a)
{
    if (a->target == NULL)
        return refuse_c(20, "INSTALL takes the name of a package, or several names");
    if (a->nalso > 0)
        return install_many(a);
    return install_one(a, a->target, NULL, 0);
}

static int move_to(const struct pkg_options *a, const struct index *ix, const struct entry *e,
                   struct pkg_manifest *cur, const char *verb)
{
    struct plan p;
    unsigned long placed, dropped, kept;
    int rc = 1;

    if (plan_target(&p, a, ix, e->name, e->version) == 0
        && run_plan(&p, cur, &placed, &dropped, &kept) == 0) {
        struct fetched f = p.f[p.n - 1];
        kv("result", "%s", verb[0] == 'u' ? res("upgraded", "would-upgrade")
                           : verb[0] == 'd' ? res("downgraded", "would-downgrade")
                           : res("rolled-back", "would-roll-back"));
        kv("name", "%s", f.m.name);
        kv("from", "%s", cur->version);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("placed", "%lu", placed);
        kv("removed", "%lu", dropped);
        kv("signer", "%s", f.signer);
        if (!machine) {
        {
        char keptw[40];
        keptw[0] = '\0';
        if (kept) snprintf(keptw, sizeof keptw, ", %lu kept", kept);
        say_result("%s%s %s from %s to %s in %s: %lu placed, %lu removed%s",
               dry_run ? "would have " : "", verb, f.m.name,
               cur->version, f.m.version, a->root, placed, dropped, keptw);
        }
        }
        if (strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1)
            hint("the image %s is replaced: a machine that has it mounted must Eject it %s, "
                 "and MOUNTLIST %s ROOT %s writes the new entry, since its size may change",
                 f.m.files[0].path, dry_run ? "first" : "and mount it again", f.m.name, a->root);
        rc = 0;
    }
    plan_free(&p);
    return rc;
}

int cmd_upgrade(const struct pkg_options *a)
{
    struct index ix;
    const struct entry *e;
    struct pkg_manifest cur;
    int rc = 1, c;

    if (a->all) return upgrade_all(a);
    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (load_installed(a->root, a->target, &cur, 0) != 0) return 1;
    if (target_arch == NULL && strcmp(cur.architecture, "generic") != 0)
        target_arch = cur.architecture;       /* stay on the installed CPU */
    if (open_channels(a, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    e = pick(&ix, a->target, a->version);
    if (e == NULL) {
        if (!pick_refused) say_not_found(&ix, a->target, a->version, chans_text());
        goto out;
    }
    c = pkg_version_cmp(e->version, cur.version);
    if (c == 0) {
        size_t o;
        kv("result", "unchanged");
        kv("name", "%s", cur.name);
        kv("version", "%s", cur.version);
        if (!machine)
            say_result("%s is already at %s", cur.name, cur.version);
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, cur.name) == 0 && !arch_matches(&ix.e[o])
                && pkg_version_cmp(ix.e[o].version, cur.version) > 0 && !is_withdrawn(&ix.e[o])) {
                if (machine)
                    kv("note", "%s %s is published for %s, not for this root's CPU", ix.e[o].name,
                       ix.e[o].version, ix.e[o].arch);
                else
                    say_detail("%s %s is published for %s, not yet for this root's CPU",
                        ix.e[o].name, ix.e[o].version, ix.e[o].arch);
            }
        rc = 0;
        goto out;
    }
    if (c < 0 && !a->downgrade) {
        refuse_c(18, "%s %s is older than the installed %s; nothing was changed. Going back "
               "a version is the requester's decision", e->name, e->version, cur.version);
        goto out;
    }
    rc = move_to(a, &ix, e, &cur, c < 0 ? "downgraded" : "upgraded");
out:
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

int cmd_rollback(const struct pkg_options *a)
{
    struct pkg_manifest cur;
    struct index ix;
    struct entry prev;
    const struct entry *e = NULL;
    char *pp;
    unsigned char *buf;
    size_t len, i;
    int rc = 1;

    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (load_installed(a->root, a->target, &cur, 0) != 0) return 1;
    if (target_arch == NULL && strcmp(cur.architecture, "generic") != 0)
        target_arch = cur.architecture;       /* back on the installed CPU */
    pp = root_path(a->root, "prev", a->target);
    if (pp == NULL || pkg_fs_read(pp, &buf, &len) != 0) {
        free(pp);
        pkg_manifest_free(&cur);
        return refuse_c(11, "%s %s has no previous version recorded in %s; nothing to roll back to",
                      a->target, cur.version, a->root);
    }
    free(pp);
    memset(&prev, 0, sizeof prev);
    {
        char tmp[200], to[64] = "", how[16] = "";
        const char *nl;
        size_t n = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        free(buf);
        if (sscanf(tmp, "%63s", prev.version) != 1 || pkg_check_version(prev.version) != NULL) {
            pkg_manifest_free(&cur);
            return refuse_c(12, "the rollback record for %s is damaged", a->target);
        }
        /* The change that wrote the record: "to <version> <verb>". */
        nl = strchr(tmp, '\n');
        if (nl != NULL && sscanf(nl + 1, "to %63s %15s", to, how) == 2 && pkg_check_version(to) != NULL)
            to[0] = how[0] = '\0';
        if (pkg_version_cmp(prev.version, cur.version) == 0) {
            /* A change cut after it wrote this record and before its
             * database entry: it is not done, so nothing is to go back
             * from. An interrupted ROLLBACK is finished by this one. */
            if (strcmp(how, "rollback") == 0 && to[0] && pkg_version_cmp(to, cur.version) != 0) {
                tr("finishing the interrupted rollback of %s to %s", a->target, to);
                snprintf(prev.version, sizeof prev.version, "%s", to);
            } else {
                pkg_manifest_free(&cur);
                if (to[0])
                    return refuse_c(11, "%s: an interrupted %s to %s is not finished; repeat it, "
                                  "then ROLLBACK", a->target, how, to);
                return refuse_c(11, "the rollback record for %s names %s, the version installed; an "
                              "interrupted change left it: repeat that change, then ROLLBACK",
                              a->target, prev.version);
            }
        }
    }
    if (open_channels(a, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->target) == 0 && pkg_version_cmp(ix.e[i].version, prev.version) == 0
            && (strcmp(ix.e[i].arch, cur.architecture) == 0 || strcmp(ix.e[i].arch, "generic") == 0))
            e = &ix.e[i];
    if (e == NULL)
        refuse_c(11, "the previous version of %s, %s, is no longer in %s",
               a->target, prev.version, chans_text());
    else
        rc = move_to(a, &ix, e, &cur, "rolled back");
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

int cmd_list(const struct pkg_options *a)
{
    char *dir, **names;
    size_t n, i;

    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    dir = pkg_join(a->root, ".pkg/db");
    if (dir == NULL || pkg_fs_list(dir, &names, &n) != 0) {
        free(dir);
        return refuse_c(17, "cannot read the database of %s", a->root);
    }
    free(dir);
    kv("result", "listed");
    if (!machine && n > 0) {
        static const int widths[] = { 24, 10, 12, 0 };
        tbl_head(widths, "Package\tVersion\tKind\tFiles");
    }
    for (i = 0; i < n; i++) {
        struct pkg_manifest m;
        if (load_installed(a->root, names[i], &m, 0) == 0) {
            int dep = is_auto(a->root, m.name);
            if (machine) {
                char j[300], files[24];
                snprintf(files, sizeof files, "%lu", (unsigned long)m.nfiles);
                snprintf(j, sizeof j, "%s %s %s %s %s", m.name, m.version, m.kind, files,
                         dep ? "dependency" : "explicit");
                rec_item("package", j, "name", m.name, "version", m.version, "kind", m.kind,
                         "files", files, "reason", dep ? "dependency" : "explicit", NULL);
            }
            else
                tbl_row("%s\t%s\t%s\t%lu file%s%s", m.name, m.version, m.kind,
                        (unsigned long)m.nfiles, m.nfiles == 1 ? "" : "s", dep ? ", a dependency" : "");
            pkg_manifest_free(&m);
        }
        free(names[i]);
    }
    free(names);
    if (!machine && n > 0)
        tbl_end();
    if (n == 0 && !machine)
        say_result("nothing installed in %s", a->root);
    kv("count", "%lu", (unsigned long)n);
    return 0;
}

/* Files a person moved by hand: found elsewhere in the root under the same
 * name with the same bytes. Moving software is the Amiga tradition; Pkg
 * reports it and claims nothing. */
struct moved_search {
    const char                *root;
    const struct pkg_manifest *m;
    const unsigned char       *missing;   /* per file: 1 if missing */
    char                     **found;     /* per file: where it is now */
};

static const char *base_of(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int moved_one(const char *rel, void *ctx)
{
    struct moved_search *ms = (struct moved_search *)ctx;
    size_t i;
    if (strncmp(rel, ".pkg/", 5) == 0)
        return 0;
    for (i = 0; i < ms->m->nfiles; i++)
        if (ms->missing[i] && ms->found[i] == NULL
            && strcmp(base_of(rel), base_of(ms->m->files[i].path)) == 0
            && file_state(ms->root, rel, ms->m->files[i].digest, ms->m->files[i].size) == 0) {
            ms->found[i] = (char *)malloc(strlen(rel) + 1);
            if (ms->found[i] != NULL)
                memcpy(ms->found[i], rel, strlen(rel) + 1);
            break;
        }
    return 0;
}

/* VERIFY ALL: every installed package, a line each; the files that differ
 * named with their package. Moved files are looked for by VERIFY <name>. */
static int verify_all(const struct pkg_options *a)
{
    struct installed in;
    size_t p, i, bad = 0, files = 0, edited = 0;
    char first[200] = "";

    if (load_all(a->root, &in) != 0) return 1;
    if (in.n == 0) {
        installed_free(&in);
        kv("result", "empty");
        summary_line("no package is installed in %s", a->root);
        return 0;
    }
    if (!machine && in.n > 0) {
        static const int widths[] = { 24, 10, 9, 0 };
        tbl_head(widths, "Package\tVersion\tFiles\tState");
    }
    for (p = 0; p < in.n; p++) {
        const struct pkg_manifest *m = &in.m[p];
        size_t changed = 0, missing = 0, pedited = 0;
        unsigned char *state = (unsigned char *)calloc(m->nfiles ? m->nfiles : 1, 1);
        if (state == NULL) { installed_free(&in); return refuse_c(17, "out of memory"); }
        /* 1. every file's state, so the package's line can come first */
        for (i = 0; i < m->nfiles; i++) {
            int s = file_state(a->root, m->files[i].path, m->files[i].digest, m->files[i].size);
            state[i] = (unsigned char)s;
            if (s == 1 && m->files[i].config) pedited++;
            else if (s == 1) changed++;
            else if (s == 2) missing++;
        }
        edited += pedited;
        files += m->nfiles;
        /* 2. the package's line */
        if (changed + missing) {
            if (bad++ == 0)
                snprintf(first, sizeof first, "%s: %s", m->name, damage(changed, missing));
            if (machine) kv("package", "%s %s damaged %lu %lu", m->name, m->version,
                            (unsigned long)changed, (unsigned long)missing);
            else tbl_row("%s\t%s\t%lu file%s\t%s", m->name, m->version, (unsigned long)m->nfiles,
                         m->nfiles == 1 ? "" : "s", damage(changed, missing));
        } else if (machine) {
            kv("package", "%s %s intact", m->name, m->version);
        } else {
            tbl_row("%s\t%s\t%lu file%s\t%s", m->name, m->version, (unsigned long)m->nfiles,
                    m->nfiles == 1 ? "" : "s", pedited ? "intact, configuration edited" : "intact");
        }
        /* 3. its files that are not as installed */
        for (i = 0; i < m->nfiles; i++) {
            if (state[i] == 1 && m->files[i].config) {
                if (machine) kv("edited", "%s %s", m->name, m->files[i].path);
                else say_item("edited", "%s (a configuration file)", m->files[i].path);
            } else if (state[i] == 1) {
                if (machine) kv("changed", "%s %s", m->name, m->files[i].path);
                else say_item("changed", "%s", m->files[i].path);
            } else if (state[i] == 2) {
                if (machine) kv("missing", "%s %s", m->name, m->files[i].path);
                else say_item("missing", "%s", m->files[i].path);
            }
        }
        free(state);
    }
    if (!machine && in.n > 0)
        tbl_end();
    kv("packages", "%lu", (unsigned long)in.n);
    kv("files", "%lu", (unsigned long)files);
    if (bad == 0) {
        kv("result", "intact");
        summary_line("%lu package%s, %lu file%s, all intact%s", (unsigned long)in.n,
           in.n == 1 ? "" : "s", (unsigned long)files, files == 1 ? "" : "s",
           edited ? "; configuration files edited, as people do" : "");
        installed_free(&in);
        return 0;
    }
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    {
        char buf[400];
        snprintf(buf, sizeof buf, "%lu of %lu package%s damaged, first %s", (unsigned long)bad,
                 (unsigned long)in.n, in.n == 1 ? "" : "s", first);
        kv("summary", "%s", buf);
        if (!machine)
            say_problem("%lu of %lu package%s damaged", (unsigned long)bad,
                        (unsigned long)in.n, in.n == 1 ? "" : "s");
    }
    hint("REPAIR ALL ROOT <dir> CHANNEL <dir> puts missing and changed files back from the channel. "
         "VERIFY <name> also says which missing files were moved by hand");
    installed_free(&in);
    return 1;
}

int cmd_verify(const struct pkg_options *a)
{
    struct pkg_manifest m;
    size_t i, changed = 0, missing = 0, edited = 0;

    if (a->all && a->root != NULL) return verify_all(a);
    if (a->target == NULL)  return refuse_c(20, "name the package to verify, or VERIFY ALL");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("files", "%lu", (unsigned long)m.nfiles);
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 1 && m.files[i].config) {
            edited++;           /* a configuration file: editing it is what it is for */
            if (machine) kv("edited", "%s", m.files[i].path);
            else say_item("edited", "%s (a configuration file)", m.files[i].path);
        } else if (s == 1) {
            changed++;
            if (machine) kv("changed", "%s", m.files[i].path);
            else say_item("changed", "%s", m.files[i].path);
        }
        if (s == 2)
            missing++;          /* reported below, once moved files are known */
    }
    if (missing > 0) {
        unsigned char *miss = (unsigned char *)calloc(m.nfiles, 1);
        char **found = (char **)calloc(m.nfiles, sizeof *found);
        size_t moved = 0;
        if (miss != NULL && found != NULL) {
            struct moved_search ms;
            unsigned skipped;
            char err[200];
            for (i = 0; i < m.nfiles; i++)
                miss[i] = file_state(a->root, m.files[i].path, m.files[i].digest,
                                     m.files[i].size) == 2;
            ms.root = a->root; ms.m = &m; ms.missing = miss; ms.found = found;
            pkg_fs_walk(a->root, moved_one, NULL, &ms, &skipped, err, sizeof err);
            for (i = 0; i < m.nfiles; i++)
                if (found[i] != NULL) {
                    moved++;
                    if (machine) kv("moved", "%s %s", m.files[i].path, found[i]);
                    else say_item("moved", "%s -> %s", m.files[i].path, found[i]);
                } else if (miss[i]) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say_item("missing", "%s", m.files[i].path);
                }
        } else {
            for (i = 0; i < m.nfiles; i++)
                if (file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size) == 2) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say_item("missing", "%s", m.files[i].path);
                }
        }
        for (i = 0; found != NULL && i < m.nfiles; i++) free(found[i]);
        free(found);
        free(miss);
        if (moved == missing && changed == 0) {
            kv("result", "moved");
            if (!machine)
                say_result("%s %s: moved by hand, every file intact where it is now", m.name, m.version);
            hint("moving an installed drawer is the person's right, and the package stays listed. "
                 "REMOVE and UPGRADE act on the places pkg recorded: the moved files are left "
                 "where they are, and an upgrade installs beside them");
            pkg_manifest_free(&m);
            return 0;
        }
    }
    if (changed + missing == 0) {
        kv("result", "intact");
        if (edited) kv("edited-config", "%lu", (unsigned long)edited);
        if (!machine)
            say_result("%s %s: %lu file%s, all intact%s", m.name, m.version, (unsigned long)m.nfiles,
                m.nfiles == 1 ? "" : "s", edited ? ", configuration files edited as people do" : "");
        pkg_manifest_free(&m);
        return 0;
    }
    if (!machine) {
        say_problem("%s %s is damaged: %s of %lu file%s", m.name, m.version,
                    damage(changed, missing), (unsigned long)m.nfiles, m.nfiles == 1 ? "" : "s");
        hint("REPAIR %s ROOT <dir> CHANNEL <dir> puts the files back from the channel", m.name);
    }
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    pkg_manifest_free(&m);
    return 1;
}

/* ---- REPAIR ------------------------------------------------------------ *
 *
 * Puts an installed version's own files back from the channel: the missing
 * ones, and the changed ones, whose change is kept beside as <file>.pkgold.
 * An edited configuration file is the person's and stays as it is. Every
 * file written is checked against the digest the root recorded at install. */

struct repair_ctx {
    const char                *root;
    const struct pkg_manifest *m;
    unsigned char             *need;    /* per file of m: 1 missing, 2 changed */
    unsigned long              restored, aside, left;
    char                       err[400];
};

static int repair_entry(const struct pkg_entry *e, void *ctx)
{
    struct repair_ctx *c = (struct repair_ctx *)ctx;
    const struct pkg_file *pf = find_file(c->m, e->path);
    char hex[PKG_SHA256_HEXLEN + 1], *to, *slash;
    size_t k;

    if (pf == NULL || !c->need[pf - c->m->files])
        return 0;
    k = (size_t)(pf - c->m->files);
    pkg_sha256_hex(e->data, e->data_len, hex);
    if ((unsigned long long)e->data_len != pf->size || strcmp(hex, pf->digest) != 0) {
        snprintf(c->err, sizeof c->err, "the channel's copy of \"%s\" is not the file installed", e->path);
        return 1;
    }
    to = installed_path(c->root, pf->path);
    if (to == NULL) { snprintf(c->err, sizeof c->err, "out of memory"); return 1; }
    if (c->need[k] == 2) {
        char *old = beside(to, ".pkgold"), *oldrel = beside(pf->path, ".pkgold");
        int good;
        if (old == NULL || oldrel == NULL) {
            free(old); free(oldrel); free(to);
            snprintf(c->err, sizeof c->err, "out of memory");
            return 1;
        }
        if (pkg_fs_exists(old)) {
            /* An earlier change is set aside there already: never lose one. */
            warn("%s already holds an earlier change; %s is left as it is", oldrel, pf->path);
            c->left++;
            free(old); free(oldrel);
            free(to);
            return 0;
        }
        pkg_fs_unprotect(to);
        good = pkg_fs_rename(to, old) == 0;
        free(old);
        if (!good) {
            snprintf(c->err, sizeof c->err, "cannot set \"%s\" aside: %s", pf->path, strerror(errno));
            free(oldrel);
            free(to);
            return 1;
        }
        c->aside++;
        if (machine) kv("set-aside", "%s %s", pf->path, oldrel);
        else say_item("aside", "%s -> %s", pf->path, oldrel);
        free(oldrel);
    }
    slash = strrchr(to, '/');
    if (slash != NULL) {
        *slash = '\0';
        pkg_fs_mkdirs(to);
        *slash = '/';
    }
    if (pkg_fs_write_atomic(to, e->data, e->data_len) != 0) {
        snprintf(c->err, sizeof c->err, "cannot write \"%s\": %s", pf->path, strerror(errno));
        free(to);
        return 1;
    }
    free(to);
    attrs_one(c->root, pf);
    c->restored++;
    if (machine) kv("restored", "%s", pf->path);
    else say_item("restored", "%s", pf->path);
    return 0;
}

/* One package: 0 intact or repaired, 1 refused (reason given). */
int repair_one(const struct pkg_options *a, const struct index *ix, const char *name,
               unsigned long *restored, unsigned long *aside)
{
    struct pkg_manifest m;
    struct fetched f;
    struct repair_ctx c;
    const struct entry *e;
    size_t i, needed = 0;
    int stopped, rc = 1;

    *restored = *aside = 0;
    if (load_installed(a->root, name, &m, 0) != 0) return 1;
    c.need = calloc(m.nfiles ? m.nfiles : 1, 1);
    if (c.need == NULL) { pkg_manifest_free(&m); return refuse("out of memory"); }
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 2 || (s == 1 && !m.files[i].config)) {
            c.need[i] = (unsigned char)(s == 2 ? 1 : 2);
            needed++;
        }
    }
    if (needed == 0) {
        free(c.need);
        pkg_manifest_free(&m);
        return 0;
    }
    e = pick(ix, m.name, m.version);
    if (e == NULL && pick_refused)
        goto out;
    if (e == NULL) {
        refuse_c(11, "%s %s is installed and the channel no longer offers it, so its %lu damaged "
                 "file%s cannot be put back; UPGRADE to a version the channel has", m.name,
                 m.version, (unsigned long)needed, needed == 1 ? "" : "s");
        goto out;
    }
    if (fetch(chan_of(e), e, &f) != 0)
        goto out;
    {
        /* Files put back come from the key this root trusts, and KEY's. */
        char pinned[65];
        if (pinned_key(a->root, m.name, pinned) && strcmp(pinned, f.signer) != 0) {
            kv("pinned", "%s", pinned);
            kv("signer", "%s", f.signer);
            refuse_c(14, "the channel's %s %s is signed by %s, not by the key this root pins, %s; "
                     "nothing was put back", m.name, m.version, f.signer, pinned);
            fetched_free(&f);
            goto out;
        }
        if (a->key != NULL && strcmp(a->key, f.signer) != 0) {
            kv("signer", "%s", f.signer);
            kv("expected", "%s", a->key);
            refuse_c(14, "the channel's %s %s is signed by %s, not by the KEY given; nothing was "
                     "put back", m.name, m.version, f.signer);
            fetched_free(&f);
            goto out;
        }
    }
    if (!dry_run) {
        c.root = a->root; c.m = &m; c.restored = c.aside = c.left = 0; c.err[0] = '\0';
        if (pkg_read(f.pkg, f.pkg_len, repair_entry, &c, &stopped) != PKG_OK) {
            refuse_c(c.err[0] && strstr(c.err, "is not the file") ? 12 : 17, "%s %s: %s; %lu file%s "
                     "put back before it", m.name, m.version, c.err[0] ? c.err : "the payload is unreadable",
                     c.restored, c.restored == 1 ? "" : "s");
            fetched_free(&f);
            goto out;
        }
        *restored = c.restored;
        *aside = c.aside;
        if (c.left) {
            /* Put back is not intact: say so, not "repaired". */
            refuse_c(PKGRC_INTEGRITY, "%s %s: %lu damaged file%s left as %s, since a .pkgold beside "
                     "it already holds an earlier change; move that aside, then REPAIR again",
                     m.name, m.version, c.left, c.left == 1 ? " is" : "s are", c.left == 1 ? "it is" : "they are");
            fetched_free(&f);
            goto out;
        }
    } else {
        *restored = (unsigned long)needed;
    }
    fetched_free(&f);
    rc = 0;
out:
    free(c.need);
    pkg_manifest_free(&m);
    return rc;
}

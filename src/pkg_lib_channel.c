/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Channels: reading one, a directory or a URL, several at once, and the
 * list a root keeps.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- channel ---------------------------------------------------------- */

/* ---- the channels of one operation ------------------------------------ *
 *
 * An index may hold the entries of several channels at once (the list a
 * root keeps, see below), so every entry carries the channel it came from
 * and nothing downstream has to be told which one to fetch an object from.
 * The channels of the running operation are kept here, in the order they
 * were read; `ch` is a place in this table. */
char *chans[PKG_MAX_CHANNELS];
size_t nchans;

static int chan_id(const char *url)
{
    size_t i;
    for (i = 0; i < nchans; i++)
        if (strcmp(chans[i], url) == 0)
            return (int)i;
    if (nchans >= PKG_MAX_CHANNELS)
        return -1;
    chans[nchans] = pkg_strdup(url);
    if (chans[nchans] == NULL)
        return -1;
    return (int)nchans++;
}

/* What a channel says it has withdrawn: one file, fetched once, listing the
 * digests of the versions whose publisher signed a withdrawal. It is a hint
 * and never the authority: what it names is still fetched and checked as
 * before, and a channel that does not serve one (a drawer, a copy, an older
 * portal) is read exactly as it was, one probe per entry. Absence is never
 * read as "nothing is withdrawn". */
char *wlist[PKG_MAX_CHANNELS];       /* the digests, 64 hex each, as served */
signed char wstate[PKG_MAX_CHANNELS];/* 0 not asked, 1 usable, -1 ask per entry */

const char *chan_of(const struct entry *e)
{
    return e->ch < nchans ? chans[e->ch] : "";
}

void chans_clear(void)
{
    while (nchans > 0) {
        free(wlist[nchans - 1]);
        wlist[nchans - 1] = NULL;
        wstate[nchans - 1] = 0;
        free(chans[--nchans]);
    }
}

/* The channels being read, for a sentence: one name, or all of them. */
const char *chans_text(void)
{
    static char text[900];
    size_t i, at = 0;
    text[0] = '\0';
    for (i = 0; i < nchans && at + 80 < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", chans[i]);
    return text;
}

/* ---- channels over the network ---------------------------------------- *
 *
 * A channel may be a URL: http://host/pkg reads exactly like a directory
 * channel, file for file. Its files are fetched into a cache (pkg_cache_dir,
 * one directory per channel URL) the first time they are needed, and every
 * check applies to them as to local ones: the manifest against the index,
 * the signature, every file against the manifest. Files named by a digest
 * never change, so they are fetched once; the index and withdrawals can,
 * so they are fetched once per run. */
int is_url(const char *ch)
{
    return ch != NULL && (strncmp(ch, "http://", 7) == 0 || strncmp(ch, "https://", 8) == 0);
}

/* What to call a channel on the activity line: the last part of its path or
 * URL, which is the name a person knows it by, with no trailing separator. */
const char *chan_label(const char *ch)
{
    static char out[80];
    const char *p;
    size_t n;
    if (ch == NULL || *ch == '\0')
        return "the channel";
    n = strlen(ch);
    while (n > 0 && (ch[n - 1] == '/' || ch[n - 1] == ':'))
        n--;
    for (p = ch + n; p > ch && p[-1] != '/' && p[-1] != ':'; p--)
        ;
    if (p == ch + n)
        return "the channel";
    snprintf(out, sizeof out, "%.*s", (int)(ch + n - p), p);
    return out;
}

char net_err[400];           /* why the last fetch failed, for the refusal */
int net_failed;
static char fetched_once[16][65];  /* mutable files already fetched in this run */
size_t nfetched_once;

/* What a channel's file is, for the step that downloads it. */
static int object_kind(const char *rel)
{
    size_t n = strlen(rel);
    if (strcmp(rel, "index") == 0) return PKG_OBJECT_INDEX;
    if (strncmp(rel, "archives/", 9) == 0) return PKG_OBJECT_ARCHIVE;
    if (n > 4 && strcmp(rel + n - 4, ".sig") == 0) return PKG_OBJECT_SIGNATURE;
    if (n > 9 && strcmp(rel + n - 9, ".manifest") == 0) return PKG_OBJECT_MANIFEST;
    if (n > 4 && strcmp(rel + n - 4, ".pkg") == 0) return PKG_OBJECT_PAYLOAD;
    return PKG_OBJECT_FILE;
}

char *chan_file(const char *channel, const char *rel)
{
    char *cache, sub[PKG_SHA256_HEXLEN + 1], url[2300], fetch_id[65], *dir, *local;
    size_t k, rl = strlen(rel);
    int mutable_file, again = 1, rc;

    if (!is_url(channel))
        return pkg_join(channel, rel);
    cache = pkg_cache_dir();
    if (cache == NULL) return NULL;
    pkg_sha256_hex((const unsigned char *)channel, strlen(channel), sub);
    sub[16] = '\0';                 /* one directory per channel URL */
    dir = pkg_join(cache, sub);
    free(cache);
    local = dir ? pkg_join(dir, rel) : NULL;
    free(dir);
    if (local == NULL) return NULL;
    if (snprintf(url, sizeof url, "%s%s%s", channel,
                 channel[strlen(channel) - 1] == '/' ? "" : "/", rel) >= (int)sizeof url) {
        snprintf(net_err, sizeof net_err, "channel URL is too long");
        net_failed = 1;
        free(local);
        return NULL;
    }
    pkg_sha256_hex((const unsigned char *)url, strlen(url), fetch_id);
    mutable_file = strcmp(rel, "index") == 0
                   || strcmp(rel, "withdrawals") == 0
                   || (rl > 10 && strcmp(rel + rl - 10, ".withdrawn") == 0)
                   || (rl > 14 && strcmp(rel + rl - 14, ".withdrawn.sig") == 0);
    if (mutable_file) {
        for (k = 0; k < nfetched_once; k++)
            if (strcmp(fetched_once[k], fetch_id) == 0) again = 0;
    } else if (pkg_fs_exists(local)) {
        again = 0;                  /* named by what it holds: a cached copy is the file */
    }
    if (!again)
        return local;
    {
        /* the cache directory for this file */
        char *slash = strrchr(local, '/');
        if (slash) { *slash = '\0'; pkg_fs_mkdirs(local); *slash = '/'; }
    }
    rc = net_get_watched(url, local, object_kind(rel), net_err, sizeof net_err);
    tr("fetched %s: %s", url, rc == 0 ? "ok" : rc == 1 ? "not there" : net_err);
    if (rc == 1)
        pkg_fs_unlink(local);       /* not published there: no stale copy either */
    if (rc >= 0 && mutable_file && nfetched_once < 16)
        snprintf(fetched_once[nfetched_once++], sizeof fetched_once[0], "%s", fetch_id);
    if (rc < 0) {
        net_failed = 1;
        if (!mutable_file) pkg_fs_unlink(local);
        free(local);
        return NULL;
    }
    return local;
}

int read_index(const char *channel, struct index *ix)
{
    char *path;
    unsigned char *buf = NULL;
    size_t len = 0, at = 0;
    unsigned line = 0;
    int id = chan_id(channel);

    ix->e = NULL;
    ix->n = 0;
    net_err[0] = '\0';
    if (id < 0)
        return refuse_c(20, "no more than %d channels can be read at once",
                        PKG_MAX_CHANNELS);
    path = chan_file(channel, "index");
    if (path == NULL)
        return net_failed ? refuse_c(17, "cannot reach the channel %s: %s", channel, net_err)
                          : refuse("out of memory");
    if (!pkg_fs_exists(path)) {
        free(path);
        if (is_url(channel) && net_err[0])
            return refuse_c(17, "cannot reach the channel %s: %s", channel, net_err);
        if (is_url(channel))
            return refuse_n(11, "report", "there is no channel at %s: it has no index", channel);
        return 0;                 /* a channel with nothing published yet */
    }
    if (pkg_fs_read(path, &buf, &len) != 0) {
        free(path);
        return refuse_c(17, "cannot read the channel index");
    }
    free(path);
    while (at < len) {
        const char *ls = (const char *)buf + at;
        const char *nl = (const char *)memchr(ls, '\n', len - at);
        size_t digit;
        size_t ll = nl ? (size_t)(nl - ls) : len - at;
        struct entry en;
        char tmp[256];
        struct entry *w;

        at += ll + 1u;
        line++;
        if (ll == 0u)
            continue;
        /* A hand-edited index may lack its last newline, or end its lines
         * with CR: both are read; what is written back is always clean. */
        if (ll >= sizeof tmp) {
            free(buf); free(ix->e); ix->e = NULL; ix->n = 0;
            return refuse_c(12, "the channel index is malformed at line %u", line);
        }
        memcpy(tmp, ls, ll);
        tmp[ll] = '\0';
        if (sscanf(tmp, "%64s %63s %31s %64s", en.name, en.version, en.arch, en.digest) != 4
            || pkg_check_name(en.name) || pkg_check_version(en.version)
            || pkg_check_arch(en.arch) || strlen(en.digest) != PKG_SHA256_HEXLEN) {
            free(buf); free(ix->e); ix->e = NULL; ix->n = 0;
            return refuse_c(12, "the channel index is malformed at line %u", line);
        }
        for (digit = 0; digit < PKG_SHA256_HEXLEN; digit++) {
            if (!isxdigit((unsigned char)en.digest[digit])) {
                free(buf); free(ix->e); ix->e = NULL; ix->n = 0;
                return refuse_c(12, "the channel index has a malformed digest at line %u", line);
            }
        }
        w = (struct entry *)realloc(ix->e, (ix->n + 1u) * sizeof *w);
        if (w == NULL) { free(buf); free(ix->e); ix->e = NULL; ix->n = 0; return refuse("out of memory"); }
        ix->e = w;
        en.withdrawn = -1;          /* not asked yet: see is_withdrawn */
        en.ch = (unsigned char)id;
        ix->e[ix->n++] = en;
    }
    free(buf);
    return 0;
}

static int by_entry(const void *a, const void *b)
{
    const struct entry *x = (const struct entry *)a, *y = (const struct entry *)b;
    int c = strcmp(x->name, y->name);
    if (c == 0) c = pkg_version_cmp(x->version, y->version);
    return c ? c : strcmp(x->arch, y->arch);
}

int write_index(const char *channel, struct index *ix)
{
    char *path = pkg_join(channel, "index");
    size_t i, cap = ix->n * 200u + 1u, len = 0;
    char *buf = (char *)malloc(cap);
    int rc;

    if (path == NULL || buf == NULL) { free(path); free(buf); return -1; }
    qsort(ix->e, ix->n, sizeof ix->e[0], by_entry);
    for (i = 0; i < ix->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s %s %s %s\n",
                                ix->e[i].name, ix->e[i].version, ix->e[i].arch, ix->e[i].digest);
    rc = pkg_fs_write_atomic(path, buf, len);
    free(path);
    free(buf);
    return rc;
}

/* ---- the channels a root keeps ---------------------------------------- *
 *
 * A machine that installs from the same places every day should not have to
 * type CHANNEL on every command. A root keeps its own list in
 * .pkg/channels, one channel per line in the order they were added, beside
 * .pkg/arch and .pkg/keys: the list belongs to the root, so an AROS system
 * keeps its own in SYS:.pkg and a test root keeps another. CHANNEL on the
 * line still means that channel and no other. */

/* A list starts empty, names included: a slot whose name was never set is
 * freed like any other, and an uninitialised one aborts the program. */
void chanlist_init(struct chanlist *c)
{
    memset(c, 0, sizeof *c);
}

void chanlist_free(struct chanlist *c)
{
    while (c->n > 0) {
        c->n--;
        free(c->v[c->n]);
        free(c->name[c->n]);
        c->name[c->n] = NULL;
    }
}

/* Read the list; a root without one has an empty list, which is not a
 * refusal. */
int chanlist_read(const char *root, struct chanlist *c)
{
    char *p = pkg_join(root, ".pkg/channels"), *tab;
    unsigned char *buf;
    size_t len, at = 0;

    chanlist_init(c);
    if (p == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(p)) { free(p); return 0; }
    if (pkg_fs_read(p, &buf, &len) != 0) {
        refuse_c(17, "cannot read the channel list %s", p);
        free(p);
        return 1;
    }
    free(p);
    while (at < len) {
        const char *ls = (const char *)buf + at;
        const char *nl = (const char *)memchr(ls, '\n', len - at);
        size_t ll = nl ? (size_t)(nl - ls) : len - at;
        char line[1200];
        at += ll + 1u;
        while (ll > 0 && (ls[ll - 1] == '\r' || ls[ll - 1] == ' '))
            ll--;
        if (ll == 0u)
            continue;
        if (ll >= sizeof line || c->n >= PKG_MAX_CHANNELS) {
            free(buf);
            chanlist_free(c);
            return refuse_c(12, "the channel list in %s is malformed, or holds more than %d "
                            "channels; edit %s/.pkg/channels, or remove it and add the channels "
                            "again", root, PKG_MAX_CHANNELS, root);
        }
        memcpy(line, ls, ll);
        line[ll] = '\0';
        /* "<name>\t<channel>", or the whole line when the channel has no
         * name: a list written before names existed reads unchanged, and a
         * tab cannot be in either part, so the two never run together. */
        tab = strchr(line, '\t');
        if (tab != NULL) {
            *tab = '\0';
            c->name[c->n] = pkg_strdup(line);
            if (c->name[c->n] == NULL) { free(buf); chanlist_free(c); return refuse("out of memory"); }
        } else {
            c->name[c->n] = NULL;
        }
        c->v[c->n] = pkg_strdup(tab ? tab + 1 : line);
        if (c->v[c->n] == NULL) { free(buf); chanlist_free(c); return refuse("out of memory"); }
        c->n++;
    }
    free(buf);
    return 0;
}

/* The channels of a list, for a sentence: by name where they have one,
 * since that is what a person types. */
const char *chanlist_text(const struct chanlist *c)
{
    static char text[900];
    size_t i, at = 0;
    text[0] = '\0';
    for (i = 0; i < c->n && at + 80 < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s",
                               at ? ", " : "", c->name[i] ? c->name[i] : c->v[i]);
    return text;
}

/* A name a channel may be called by on a command line: letters, digits, a
 * dash, an underscore or a dot, and never something that could be a path or
 * a URL instead. */
int chan_name_ok(const char *s)
{
    size_t n = 0;
    if (s == NULL || *s == '\0')
        return 0;
    for (; *s; s++, n++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')
              || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.'))
            return 0;
    return n <= 63u;
}

/* The name a channel gives itself: the last part of its address, which is
 * what a channel is called anyway ("https://.../contrib-nightly" is the
 * contrib-nightly channel, "work:channels/mine" is mine). Empty when
 * nothing usable comes out, and the caller then asks for a name. */
void chan_name_of(const char *channel, char *out, size_t len)
{
    const char *end = channel + strlen(channel), *start;
    size_t n;
    out[0] = '\0';
    while (end > channel && (end[-1] == '/' || end[-1] == '\\'))
        end--;
    start = end;
    while (start > channel && start[-1] != '/' && start[-1] != '\\' && start[-1] != ':')
        start--;
    n = (size_t)(end - start);
    if (n == 0 || n >= len)
        return;
    memcpy(out, start, n);
    out[n] = '\0';
    if (!chan_name_ok(out))
        out[0] = '\0';
}

int chanlist_write(const char *root, const struct chanlist *c)
{
    char *dir = pkg_join(root, ".pkg"), *p = pkg_join(root, ".pkg/channels");
    char *buf;
    size_t i, cap = 8, len = 0;
    int rc;

    for (i = 0; i < c->n; i++)
        cap += strlen(c->v[i]) + (c->name[i] ? strlen(c->name[i]) + 1u : 0u) + 1u;
    buf = (char *)malloc(cap);
    if (dir == NULL || p == NULL || buf == NULL) { free(dir); free(p); free(buf); return -1; }
    pkg_fs_mkdirs(dir);
    free(dir);
    for (i = 0; i < c->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s%s%s\n",
                                c->name[i] ? c->name[i] : "", c->name[i] ? "\t" : "", c->v[i]);
    rc = pkg_fs_write_atomic(p, buf, len);
    free(p);
    free(buf);
    return rc;
}

/* The root whose pinned keys govern a choice among several channels, set
 * when the channels are opened. */
const char *pick_root;

/* The channels an operation reads from: CHANNEL when it was given, and then
 * that channel alone; else the root's list, in its order. The index that
 * comes back holds every channel's entries, each remembering where it came
 * from. */
int open_channels(const struct pkg_options *a, struct index *ix)
{
    struct chanlist cl;
    size_t i;

    ix->e = NULL;
    ix->n = 0;
    pick_root = a->root;
    if (a->channel != NULL)
        return read_index(a->channel, ix);
    if (a->root == NULL)
        return refuse_c(20, "name the channel with CHANNEL <dir|url>");
    if (chanlist_read(a->root, &cl) != 0)
        return 1;
    if (cl.n == 0) {
        chanlist_free(&cl);
        return refuse_c(20, "name the channel with CHANNEL <dir|url>, or add it to this root "
                        "once and leave CHANNEL out from then on: "
                        "CHANNEL ADD <dir|url> ROOT %s", a->root);
    }
    for (i = 0; i < cl.n; i++) {
        struct index one;
        struct entry *w;
        if (!is_url(cl.v[i]) && !pkg_fs_is_dir(cl.v[i])) {
            free(ix->e);
            ix->e = NULL;
            ix->n = 0;
            refuse_c(11, "%s lists the channel %s, and there is no channel there: not "
                            "mounted, or moved. Nothing was checked or changed; "
                            "CHANNEL REMOVE %s ROOT %s takes it off the list",
                            a->root, cl.v[i], cl.v[i], a->root);
            chanlist_free(&cl);
            return 1;
        }
        if (read_index(cl.v[i], &one) != 0) {
            free(ix->e);
            ix->e = NULL;
            ix->n = 0;
            chanlist_free(&cl);
            return 1;
        }
        if (one.n == 0) { free(one.e); continue; }
        w = (struct entry *)realloc(ix->e, (ix->n + one.n) * sizeof *w);
        if (w == NULL) {
            free(one.e); free(ix->e); ix->e = NULL; ix->n = 0;
            chanlist_free(&cl);
            return refuse("out of memory");
        }
        ix->e = w;
        memcpy(ix->e + ix->n, one.e, one.n * sizeof *one.e);
        ix->n += one.n;
        free(one.e);
    }
    tr("reading %lu channel%s listed in %s: %s", (unsigned long)cl.n, cl.n == 1 ? "" : "s",
       a->root, chans_text());
    chanlist_free(&cl);
    return 0;
}

char *object_path(const char *channel, const char *digest, const char *ext)
{
    char rel[128];
    snprintf(rel, sizeof rel, "objects/%s.%s", digest, ext);
    return chan_file(channel, rel);
}

/* Where installers find the archive a Source line names: archives/<name>
 * in the channel. */
char *archive_path(const char *channel, const char *name)
{
    char rel[1100];
    if (strchr(name, '/') != NULL || name[0] == '.')
        return NULL;
    snprintf(rel, sizeof rel, "archives/%s", name);
    return chan_file(channel, rel);
}

/* The kind and dependencies of the highest version of a package published in
 * the channel, for the CPU given when it has one there. */
/* The manifest of the highest version of a package published in the
 * channel PUBLISH writes to, for the CPU given when it has one there. 1 and
 * `em` filled, or 0. */
int last_published(const char *name, const char *arch, struct pkg_manifest *em)
{
    const struct entry *best = NULL;
    size_t o;
    int pass;
    char *mp, err[200];
    unsigned char *buf;
    size_t len;

    if (inherit_ix == NULL)
        return 0;
    for (pass = 0; pass < 2 && best == NULL; pass++)
        for (o = 0; o < inherit_ix->n; o++) {
            const struct entry *e = &inherit_ix->e[o];
            if (strcmp(e->name, name) != 0 || (pass == 0 && strcmp(e->arch, arch) != 0))
                continue;
            if (best == NULL || pkg_version_cmp(e->version, best->version) > 0)
                best = e;
        }
    if (best == NULL)
        return 0;
    mp = object_path(inherit_channel, best->digest, "manifest");
    if (mp == NULL || pkg_fs_read(mp, &buf, &len) != 0) { free(mp); return 0; }
    free(mp);
    if (pkg_manifest_parse((const char *)buf, len, em, err, sizeof err) != 0) { free(buf); return 0; }
    free(buf);
    return 1;
}

int inherited(const char *name, const char *arch, char *kind, size_t kl,
              char *deps, size_t dl, char *from, size_t fl, char *conf, size_t cl)
{
    size_t d, at = 0;
    struct pkg_manifest em;

    if (!last_published(name, arch, &em))
        return 0;
    snprintf(kind, kl, "%s", em.kind);
    deps[0] = '\0';
    for (d = 0; d < em.ndeps && at < dl; d++)
        at += (size_t)snprintf(deps + at, dl - at, "%s%s%s%s", d ? ", " : "", em.deps[d].name,
                               em.deps[d].min ? " >= " : "", em.deps[d].min ? em.deps[d].min : "");
    if (em.ndeps == 0)
        snprintf(deps, dl, "none");
    snprintf(from, fl, "%s %s", em.name, em.version);
    conf[0] = '\0';
    for (d = 0, at = 0; d < em.nfiles; d++)
        if (em.files[d].config && at + strlen(em.files[d].path) + 2 < cl)
            at += (size_t)snprintf(conf + at, cl - at, "%s%s", at ? "," : "", em.files[d].path);
    tr("inherited from %s %s: kind %s, depends %s, config %s", em.name, em.version, kind, deps,
       conf[0] ? conf : "none");
    pkg_manifest_free(&em);
    return 1;
}

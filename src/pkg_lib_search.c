/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * SEARCH, locally and through a portal's search API.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- SEARCH ------------------------------------------------------------ */

static int hit_add(struct hits *h, const struct hit *x)
{
    size_t i;
    for (i = 0; i < h->n; i++)
        if (strcmp(h->v[i].name, x->name) == 0 && strcmp(h->v[i].chan, x->chan) == 0)
            return 0;               /* one row per package per channel */
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 32;
        struct hit *w = (struct hit *)realloc(h->v, cap * sizeof *w);
        if (w == NULL) return -1;
        h->v = w;
        h->cap = cap;
    }
    h->v[h->n++] = *x;
    return 0;
}

static int by_hit(const void *x, const void *y)
{
    const struct hit *a = (const struct hit *)x, *b = (const struct hit *)y;
    int c = ascii_casecmp(a->name, b->name);
    return c ? c : strcmp(a->chan, b->chan);
}

/* Does `text` hold `word`, whatever the case? */
static int holds_word(const char *text, const char *word)
{
    size_t wl = strlen(word), i;
    if (text == NULL) return 0;
    for (i = 0; text[i]; i++)
        if (ascii_casecmp_n(text + i, word, wl) == 0)
            return 1;
    return 0;
}

static int strs_hold_word(const struct pkg_strs *l, const char *word)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (holds_word(l->v[i], word))
            return 1;
    return 0;
}

/* Every field SEARCH looks in, for one word. */
static int manifest_holds(const struct pkg_manifest *m, const char *word)
{
    const struct pkg_about *ab = &m->about;
    return holds_word(m->name, word) || holds_word(ab->short_desc, word)
        || holds_word(ab->category, word) || strs_hold_word(&ab->tags, word)
        || strs_hold_word(&ab->description, word) || strs_hold_word(&m->provides, word);
}

/* One channel read package by package: the index, then the manifest of each
 * newest version that is not withdrawn. Directory channels and plain web
 * servers are read this way, and so is a portal whose API did not answer. */
static int search_local(const char *channel, const char *const *words, unsigned nwords,
                        struct hits *h)
{
    struct index ix;
    size_t i, j;

    if (read_index(channel, &ix) != 0) { refused_class = 0; refused_next = NULL; return 1; }
    for (i = 0; i < ix.n; i++) {
        const struct entry *best = NULL;
        struct pkg_manifest m;
        struct hit x;
        unsigned char *buf;
        char *mp, err[200];
        size_t len;
        unsigned w;
        int seen = 0, all = 1;

        for (j = 0; j < i; j++)
            if (strcmp(ix.e[j].name, ix.e[i].name) == 0) seen = 1;
        if (seen)
            continue;
        for (j = 0; j < ix.n; j++) {
            const struct entry *e = &ix.e[j];
            if (strcmp(e->name, ix.e[i].name) != 0 || is_withdrawn(e) || !arch_matches(e))
                continue;
            if (best == NULL || pkg_version_cmp(e->version, best->version) > 0)
                best = e;
        }
        if (best == NULL)
            continue;
        if (cancelled("while reading the channel; nothing was changed")) { free(ix.e); return 1; }
        if ((mp = object_path(channel, best->digest, "manifest")) == NULL)
            continue;
        if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
        free(mp);
        if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) != 0) {
            free(buf);
            continue;
        }
        free(buf);
        for (w = 0; w < nwords && all; w++)
            if (!manifest_holds(&m, words[w]))
                all = 0;
        if (all) {
            memset(&x, 0, sizeof x);
            snprintf(x.name, sizeof x.name, "%s", m.name);
            snprintf(x.version, sizeof x.version, "%s", best->version);
            snprintf(x.chan, sizeof x.chan, "%s", channel);
            snprintf(x.short_desc, sizeof x.short_desc, "%s",
                     m.about.short_desc ? m.about.short_desc : "-");
            /* every CPU this version is published for */
            for (j = 0; j < ix.n; j++) {
                size_t at = strlen(x.arch);
                if (strcmp(ix.e[j].name, m.name) != 0
                    || pkg_version_cmp(ix.e[j].version, best->version) != 0
                    || strstr(x.arch, ix.e[j].arch) != NULL)
                    continue;
                if (at + strlen(ix.e[j].arch) + 2 < sizeof x.arch)
                    snprintf(x.arch + at, sizeof x.arch - at, "%s%s", at ? "," : "",
                             ix.e[j].arch);
            }
            if (hit_add(h, &x) != 0) { pkg_manifest_free(&m); free(ix.e); return refuse("out of memory"); }
        }
        pkg_manifest_free(&m);
    }
    free(ix.e);
    return 0;
}

/* ---- the portal's search API ------------------------------------------- *
 *
 * A portal channel holds hundreds of packages, and reading every manifest
 * over the network to answer one question is minutes of waiting. The portal
 * answers the same question itself at /api/search (portal/src/Portal/Api,
 * Channels/Search.cs), so that is asked first: one request per word, the
 * answers intersected by name, which is Pkg's rule that every word must
 * match, expressed in the portal's own index, description and files
 * included. Anything unusable in the answer and the channel is read the
 * long way instead, without a word about it: the result is the same, only
 * slower.
 *
 * Pkg carries no JSON library and gains none for this. The reader below
 * takes exactly the fields these records hold, and refuses anything it does
 * not recognise rather than guessing. */

/* The value of "key" in the JSON object at *p, which must be a string,
 * copied unescaped into out. 1 when it was found. */
static int json_str(const char *obj, const char *key, char *out, size_t ol)
{
    char pat[64];
    const char *p;
    size_t at = 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(obj, pat);
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && at + 1 < ol) {
        if (*p == '\\') {
            p++;
            if (*p == '\0') break;
            out[at++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            p++;
            continue;
        }
        out[at++] = *p++;
    }
    out[at] = '\0';
    return 1;
}

/* "key": [ "a", "b" ] -> "a,b". */
static int json_strs(const char *obj, const char *key, char *out, size_t ol)
{
    char pat[64];
    const char *p;
    size_t at = 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(obj, pat);
    out[0] = '\0';
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            if (at && at + 1 < ol) out[at++] = ',';
            while (*p && *p != '"' && at + 1 < ol) {
                if (*p == '\\') p++;
                if (*p) out[at++] = *p++;
            }
        }
        if (*p) p++;
    }
    out[at] = '\0';
    return 1;
}

/* The end of the JSON object that starts at `p` (which points at its '{'),
 * or NULL when it does not end. Strings and their escapes are stepped over
 * so a brace inside a description does not close the record. */
static const char *json_object_end(const char *p)
{
    int depth = 0, instr = 0;
    for (; *p; p++) {
        if (instr) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') instr = 0;
            continue;
        }
        if (*p == '"') instr = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p;
            if (depth < 0) return NULL;
        }
    }
    return NULL;
}

/* https://host/contrib-nightly -> the site, and the channel's name there. */
static int portal_parts(const char *url, char *site, size_t sl, char *name, size_t nl)
{
    const char *after = strstr(url, "://"), *slash;
    size_t n;
    if (after == NULL) return 0;
    after += 3;
    slash = strrchr(after, '/');
    if (slash == NULL || slash[1] == '\0') return 0;
    n = (size_t)(slash - url);
    if (n + 1 >= sl) return 0;
    memcpy(site, url, n);
    site[n] = '\0';
    snprintf(name, nl, "%s", slash + 1);
    return name[0] != '\0';
}

static void url_escape(const char *s, char *out, size_t ol)
{
    size_t at = 0;
    for (; *s && at + 4 < ol; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~')
            out[at++] = (char)c;
        else
            at += (size_t)snprintf(out + at, ol - at, "%%%02X", c);
    }
    out[at] = '\0';
}

/* One /api/search request, its records appended to `h`, keeping only the
 * ones this channel published. -1 when the answer was not usable and the
 * channel must be read the long way instead. */
static int api_words(const char *channel, const char *const *words, unsigned nwords,
                     struct hits *h)
{
    char site[900], cname[200], *cache, *local = NULL;
    struct hits got[8];
    unsigned w, nq = nwords ? nwords : 1;
    int rc = -1;
    size_t i, j;

    memset(got, 0, sizeof got);
    if (nq > 8) nq = 8;
    if (!portal_parts(channel, site, sizeof site, cname, sizeof cname))
        return -1;
    cache = pkg_cache_dir();
    if (cache == NULL) return -1;
    local = pkg_join(cache, "search.json");
    free(cache);
    if (local == NULL) return -1;
    for (w = 0; w < nq; w++) {
        char url[2300], q[600], arch[80], err[400];
        unsigned char *buf;
        size_t len;
        const char *p, *end;
        url_escape(nwords ? words[w] : "", q, sizeof q);
        arch[0] = '\0';
        if (target_arch != NULL)
            snprintf(arch, sizeof arch, "&arch=%s", target_arch);
        snprintf(url, sizeof url, "%s/api/search?q=%s&channel=%s%s", site, q, cname, arch);
        if (pkg_net_get(url, local, err, sizeof err) != 0) {
            tr("the portal's search API did not answer (%s): reading %s the long way", err,
               channel);
            goto out;
        }
        if (pkg_fs_read(local, &buf, &len) != 0)
            goto out;
        p = strstr((const char *)buf, "\"results\":");
        if (p == NULL) { free(buf); tr("the portal's answer holds no results: reading %s the long way", channel); goto out; }
        p = strchr(p, '[');
        if (p == NULL) { free(buf); goto out; }
        end = (const char *)buf + len;
        for (p++; p < end && *p; ) {
            const char *stop;
            char rec[9000], chn[200];
            struct hit x;
            size_t rl;
            while (p < end && *p && *p != '{' && *p != ']') p++;
            if (p >= end || *p != '{') break;
            stop = json_object_end(p);
            if (stop == NULL) break;
            rl = (size_t)(stop - p) + 1u;
            if (rl >= sizeof rec) { p = stop + 1; continue; }
            memcpy(rec, p, rl);
            rec[rl] = '\0';
            p = stop + 1;
            memset(&x, 0, sizeof x);
            if (!json_str(rec, "name", x.name, sizeof x.name)
                || !json_str(rec, "version", x.version, sizeof x.version)
                || !json_str(rec, "channel", chn, sizeof chn)) {
                free(buf);
                tr("a record of the portal's answer is not what pkg expects: reading %s the long way",
                   channel);
                goto out;
            }
            if (strcmp(chn, cname) != 0)
                continue;
            json_strs(rec, "archs", x.arch, sizeof x.arch);
            if (!json_str(rec, "short", x.short_desc, sizeof x.short_desc) || !x.short_desc[0])
                snprintf(x.short_desc, sizeof x.short_desc, "%s", "-");
            if (x.arch[0] == '\0') snprintf(x.arch, sizeof x.arch, "%s", "-");
            snprintf(x.chan, sizeof x.chan, "%s", channel);
            if (hit_add(&got[w], &x) != 0) { free(buf); goto out; }
        }
        free(buf);
    }
    /* every word must match: a name that is in every answer */
    for (i = 0; i < got[0].n; i++) {
        int all = 1;
        for (w = 1; w < nq && all; w++) {
            int here = 0;
            for (j = 0; j < got[w].n && !here; j++)
                if (strcmp(got[w].v[j].name, got[0].v[i].name) == 0)
                    here = 1;
            all = here;
        }
        if (all && hit_add(h, &got[0].v[i]) != 0)
            goto out;
    }
    tr("%s answered for %u word%s through its search API", channel, nq, nq == 1 ? "" : "s");
    rc = 0;
out:
    for (w = 0; w < nq; w++)
        free(got[w].v);
    pkg_fs_unlink(local);
    free(local);
    return rc;
}

int cmd_search(const struct pkg_options *a)
{
    struct chanlist cl;
    struct hits h;
    const char *words[16];
    unsigned nwords = 0, w;
    size_t i;
    int rc = 1, several;
    char line[900];

    memset(&h, 0, sizeof h);
    if (a->target == NULL)
        return refuse_c(20, "SEARCH takes the words to look for: SEARCH <word>...");
    words[nwords++] = a->target;
    for (w = 0; w < a->nalso && nwords < sizeof words / sizeof words[0]; w++)
        words[nwords++] = a->also[w];
    if (a->arch != NULL) {
        const char *why = pkg_check_arch(a->arch);
        if (why != NULL)
            return refuse_c(20, "ARCH \"%s\": %s", a->arch, why);
        target_arch = a->arch;
    } else if (a->root != NULL) {
        if (resolve_arch(a) != 0)
            return 1;
    } else {
        target_arch = NULL;
    }
    chanlist_init(&cl);
    if (a->channel != NULL) {
        cl.v[cl.n] = pkg_strdup(a->channel);
        if (cl.v[cl.n] == NULL) return refuse("out of memory");
        cl.n++;
    } else if (a->root == NULL) {
        return refuse_c(20, "name the channel to search with CHANNEL <dir|url>, or the root "
                        "whose channels to search with ROOT <dir>");
    } else {
        if (chanlist_read(a->root, &cl) != 0)
            return 1;
        if (cl.n == 0) {
            chanlist_free(&cl);
            return refuse_c(20, "%s lists no channel and no CHANNEL was given; "
                            "CHANNEL ADD <dir|url> ROOT %s adds one", a->root, a->root);
        }
    }
    several = cl.n > 1;
    for (i = 0; i < cl.n; i++) {
        if (is_url(cl.v[i]) && api_words(cl.v[i], words, nwords, &h) == 0)
            continue;
        if (search_local(cl.v[i], words, nwords, &h) != 0 && refused_class)
            goto out;
    }
    qsort(h.v, h.n, sizeof h.v[0], by_hit);
    kv("result", "shown");
    if (!machine && h.n > 0) {
        static const int widths[] = { 22, 14, 10, 0 };
        if (several) {
            static const int wide[] = { 22, 14, 10, 34, 0 };
            tbl_head(wide, "Package\tVersion\tArch\tShort\tChannel");
        } else {
            tbl_head(widths, "Package\tVersion\tArch\tShort");
        }
    }
    for (i = 0; i < h.n; i++) {
        const struct hit *x = &h.v[i];
        if (machine) {
            /* the short description last: it is the only field with spaces */
            if (several) {
                snprintf(line, sizeof line, "%s %s %s %s %s", x->name, x->version, x->arch,
                         x->chan, x->short_desc);
                rec_item("package", line, "name", x->name, "version", x->version, "arch", x->arch,
                         "channel", x->chan, "short", x->short_desc, NULL);
            } else {
                snprintf(line, sizeof line, "%s %s %s %s", x->name, x->version, x->arch,
                         x->short_desc);
                rec_item("package", line, "name", x->name, "version", x->version, "arch", x->arch,
                         "short", x->short_desc, NULL);
            }
        } else if (several) {
            tbl_row("%s\t%s\t%s\t%s\t%s", x->name, x->version, x->arch, x->short_desc, x->chan);
        } else {
            tbl_row("%s\t%s\t%s\t%s", x->name, x->version, x->arch, x->short_desc);
        }
    }
    if (!machine && h.n > 0)
        tbl_end();
    kv("count", "%lu", (unsigned long)h.n);
    {
        char asked[700];
        size_t at = 0;
        asked[0] = '\0';
        for (w = 0; w < nwords && at + 40 < sizeof asked; w++)
            at += (size_t)snprintf(asked + at, sizeof asked - at, "%s%s", at ? " " : "", words[w]);
        if (h.n == 0) {
            kv("summary", "nothing matches %s", asked);
            if (!machine)
                say_result("nothing in %s matches %s", chanlist_text(&cl), asked);
            hint("every word must match, in the name, the short description, the tags, the "
                 "category, the description or what the package provides; fewer words match more");
        } else {
            kv("summary", "%lu package%s match%s %s", (unsigned long)h.n, h.n == 1 ? "" : "s",
               h.n == 1 ? "es" : "", asked);
            if (!machine)
                say_result("%lu package%s match%s %s", (unsigned long)h.n, h.n == 1 ? "" : "s",
                           h.n == 1 ? "es" : "", asked);
        }
    }
    rc = 0;
out:
    free(h.v);
    chanlist_free(&cl);
    return rc;
}

int cmd_status(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    size_t i, shown = 0, upgradable = 0, at_e = 0, at_w = 0, at_c = 0;
    char edited[400], withdrawn[400], conflicts[400];
    const char *base;
    int rc = 1;

    if (keep_current_setup(a, &ix, &in) != 0) return 1;
    base = target_arch;
    edited[0] = withdrawn[0] = conflicts[0] = '\0';
    if (a->target != NULL) {
        for (i = 0; i < in.n && strcmp(in.m[i].name, a->target) != 0; i++)
            ;
        if (i == in.n) {
            refuse_n(11, "use-install", "%s is not installed in %s", a->target, a->root);
            goto out;
        }
    }
    kv("result", "shown");
    if (!machine && in.n > 0) {
        static const int widths[] = { 24, 12, 0 };
        tbl_head(widths, "Package\tInstalled\tState");
    }
    for (i = 0; i < in.n; i++) {
        struct standing s;
        const char *avail;
        if (a->target != NULL && strcmp(in.m[i].name, a->target) != 0)
            continue;
        if (cancelled("while comparing the root with the channel; nothing was changed"))
            goto out;
        stand(a->root, &ix, base, &in.m[i], &s);
        avail = s.offer ? s.offer->version : "-";
        shown++;
        if (s.newer)
            upgradable++;
        if (s.edited && at_e + 70 < sizeof edited)
            at_e += (size_t)snprintf(edited + at_e, sizeof edited - at_e, "%s%s", at_e ? ", " : "",
                                     s.m->name);
        if (s.conflict && at_c + 70 < sizeof conflicts)
            at_c += (size_t)snprintf(conflicts + at_c, sizeof conflicts - at_c, "%s%s",
                                     at_c ? ", " : "", s.m->name);
        if (s.withdrawn && !s.newer && at_w + 70 < sizeof withdrawn)
            at_w += (size_t)snprintf(withdrawn + at_w, sizeof withdrawn - at_w, "%s%s %s",
                                     at_w ? ", " : "", s.m->name, s.m->version);
        if (machine) {
            char j[500];
            const char *from = nchans > 1 && s.offer ? chan_of(s.offer) : NULL;
            snprintf(j, sizeof j, "%s %s %s %s%s%s", s.m->name, s.m->version, avail, s.state,
                     from ? " " : "", from ? from : "");
            if (from != NULL)
                rec_item("package", j, "name", s.m->name, "installed", s.m->version,
                         "available", avail, "state", s.state, "channel", from, NULL);
            else
                rec_item("package", j, "name", s.m->name, "installed", s.m->version,
                         "available", avail, "state", s.state, NULL);
            if (s.conflict)
                kv("warning", "%s", s.why);
            if (s.withdrawn && s.newer)
                note("%s %s, installed, was withdrawn by its publisher; UPGRADE takes %s",
                     s.m->name, s.m->version, avail);
        } else {
            char what[300], from[200];
            from[0] = '\0';
            if (nchans > 1 && s.offer != NULL)
                snprintf(from, sizeof from, " from %s", chan_of(s.offer));
            if (strcmp(s.state, "conflict") == 0)
                snprintf(what, sizeof what, "two channels offer it under different keys");
            else if (strcmp(s.state, "upgradable") == 0)
                snprintf(what, sizeof what, "upgradable to %s%s", avail, from);
            else if (strcmp(s.state, "withdrawn") == 0)
                snprintf(what, sizeof what, "withdrawn by its publisher%s%s",
                         s.offer ? "; the channel offers " : ", and nothing else is offered",
                         s.offer ? avail : "");
            else if (strcmp(s.state, "not-offered") == 0)
                snprintf(what, sizeof what, "no longer offered by the channel");
            else if (strcmp(s.state, "edited") == 0)
                snprintf(what, sizeof what, "files edited since install%s%s", s.newer ? "; " : "",
                         s.newer ? "upgradable to " : "");
            else
                snprintf(what, sizeof what, "current");
            tbl_row("%s\t%s\t%s%s%s", s.m->name, s.m->version, what,
                strcmp(s.state, "edited") == 0 && s.newer ? avail : "",
                s.withdrawn && s.newer ? " (the installed version was withdrawn)" : "");
        }
    }
    if (!machine && in.n > 0)
        tbl_end();
    kv("count", "%lu", (unsigned long)shown);
    kv("upgradable", "%lu", (unsigned long)upgradable);
    if (shown == 0)
        kv("summary", "nothing is installed in this root");
    else if (upgradable == 0)
        kv("summary", "everything is up to date: %lu package%s, none with a newer version",
           (unsigned long)shown, shown == 1 ? "" : "s");
    else
        kv("summary", "%lu of %lu package%s can be updated", (unsigned long)upgradable,
           (unsigned long)shown, shown == 1 ? "" : "s");
    if (!machine) {
        if (shown == 0)
            say_result("nothing installed in %s", a->root);
        else if (upgradable == 0)
            say_result("%lu package%s in %s, all up to date with %s", (unsigned long)shown,
                shown == 1 ? "" : "s", a->root, chans_text());
        else
            say_result("%lu of %lu package%s in %s can be updated from %s", (unsigned long)upgradable,
                (unsigned long)shown, shown == 1 ? "" : "s", a->root, chans_text());
    }
    if (upgradable > 0) {
        if (a->channel != NULL)
            hint("UPGRADE ALL ROOT %s CHANNEL %s upgrades every one of them, a package before "
                 "what depends on it; with DRYRUN it only says what it would do", a->root,
                 a->channel);
        else
            hint("UPGRADE ALL ROOT %s upgrades every one of them from this root's channels, a "
                 "package before what depends on it; with DRYRUN it only says what it would do",
                 a->root);
    }
    if (at_e > 0)
        hint("files were edited in %s since install: VERIFY <name> names them. An upgrade that "
             "would replace an edited file is refused; what to do with the edit is the "
             "requester's decision", edited);
    if (at_c > 0)
        hint("%s: two of this root's channels offer it under different keys, so nothing is "
             "chosen for it. INSTALL it from the channel whose key is the publisher's, with "
             "CHANNEL <that one>, and this root pins that key from then on; CHANNEL LIST ROOT %s "
             "shows the channels", conflicts, a->root);
    if (at_w > 0)
        hint("%s: withdrawn by the publisher, with nothing newer offered. Going back to the "
             "previous version (ROLLBACK) or waiting for a fixed one is the requester's decision",
             withdrawn);
    rc = 0;
out:
    installed_free(&in);
    free(ix.e);
    return rc;
}

/* The manifest a channel entry names, unverified: only for ordering. The
 * upgrade itself fetches and checks it in full. */
static void offered_manifest(const char *channel, const struct entry *e, struct pkg_manifest *m)
{
    char *mp = object_path(channel, e->digest, "manifest"), err[200];
    unsigned char *buf;
    size_t len;
    pkg_manifest_init(m);
    if (mp != NULL && pkg_fs_read(mp, &buf, &len) == 0) {
        pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
        free(buf);
    }
    free(mp);
}

static int names_dep(const struct pkg_manifest *m, const char *name)
{
    size_t d;
    for (d = 0; d < m->ndeps; d++)
        if (strcmp(m->deps[d].name, name) == 0)
            return 1;
    return 0;
}

/* UPGRADE ALL, as far as possible: one item per package upgraded (package),
 * refused (with its class and reason) or waiting for a refused one
 * (skipped), then counts and a summary sentence. When something was not
 * upgraded, the result is a refusal with the first one's class and next,
 * which is the exit code; running it again once the requester has decided
 * takes what waited. */
int upgrade_all(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    struct standing *st = NULL;
    struct pkg_manifest *nm = NULL;
    size_t *cand = NULL, *order = NULL, ncand = 0, i, j, pos, done = 0;
    unsigned char *emitted = NULL;
    const char *base;
    int rc = 1, worst_class = 0;
    const char *worst_next = NULL;
    size_t nrefused = 0, nskipped = 0, nconflict = 0, at_r = 0;
    unsigned char *failed = NULL;
    char refused_names[600];

    refused_names[0] = '\0';
    if (a->target != NULL)
        return refuse_c(20, "UPGRADE ALL upgrades every package a newer version is offered for; "
                        "name no package with it (UPGRADE <name> upgrades one)");
    if (a->version != NULL)
        return refuse_c(20, "VERSION names one package's version; UPGRADE ALL takes, for each "
                        "package, the version UPGRADE <name> would take");
    if (a->downgrade || a->acceptkey != NULL || a->key != NULL)
        return refuse_c(20, "%s is a decision about one package, never about all of them at "
                        "once: UPGRADE ALL never downgrades and never accepts a new key. Give it "
                        "to UPGRADE <name>", a->downgrade ? "DOWNGRADE"
                        : a->key != NULL ? "KEY" : "ACCEPTKEY");
    if (keep_current_setup(a, &ix, &in) != 0) return 1;
    base = target_arch;
    st = (struct standing *)calloc(in.n ? in.n : 1, sizeof *st);
    cand = (size_t *)calloc(in.n ? in.n : 1, sizeof *cand);
    order = (size_t *)calloc(in.n ? in.n : 1, sizeof *order);
    emitted = (unsigned char *)calloc(in.n ? in.n : 1, 1);
    nm = (struct pkg_manifest *)calloc(in.n ? in.n : 1, sizeof *nm);
    failed = (unsigned char *)calloc(in.n ? in.n : 1, 1);
    planned = (struct planned_up *)calloc(in.n ? in.n : 1, sizeof *planned);
    nplanned = 0;
    if (st == NULL || cand == NULL || order == NULL || emitted == NULL || nm == NULL || planned == NULL
        || failed == NULL) {
        refuse("out of memory");
        goto out;
    }
    for (i = 0; i < in.n; i++) {
        stand(a->root, &ix, base, &in.m[i], &st[i]);
        if (st[i].conflict) {
            /* Two listed channels disagree about it: reported like any other
             * package that needs the requester, and every other goes ahead. */
            char one[2400], *q;
            nrefused++;
            nconflict++;
            snprintf(one, sizeof one, "%s", st[i].why);
            for (q = one; *q; q++)
                if (*q == '\n') *q = ' ';
            if (14 > worst_class) { worst_class = 14; worst_next = "ask-requester"; }
            if (machine) {
                char jn[2600];
                snprintf(jn, sizeof jn, "%s %s %s %s", in.m[i].name, in.m[i].version,
                         class_name(14), one);
                rec_item("refused", jn, "name", in.m[i].name, "installed", in.m[i].version,
                         "class", class_name(14), "code", "14", "reason", one,
                         "next", "ask-requester", NULL);
            } else {
                say_pkgline(in.m[i].name, "not upgraded: %s", one);
            }
            if (at_r + 80 < sizeof refused_names)
                at_r += (size_t)snprintf(refused_names + at_r, sizeof refused_names - at_r,
                                         "%s%s (%s)", at_r ? ", " : "", in.m[i].name,
                                         class_name(14));
        } else if (st[i].newer) {
            offered_manifest(chan_of(st[i].offer), st[i].offer, &nm[ncand]);
            cand[ncand++] = i;
        } else if (st[i].withdrawn) {
            note("%s %s was withdrawn by its publisher, and nothing newer is offered; going back "
                 "is the requester's decision, and UPGRADE ALL never does it", in.m[i].name,
                 in.m[i].version);
        }
    }
    /* A package before what depends on it: by the dependencies of the
     * version it moves to and of the one installed. Candidates are in name
     * order, the database's; a cycle is left to the plan, which names it. */
    for (pos = 0; pos < ncand; pos++) {
        size_t next = ncand;
        for (i = 0; i < ncand && next == ncand; i++) {
            int blocked = 0;
            if (emitted[i])
                continue;
            for (j = 0; j < ncand && !blocked; j++) {
                const char *dn = st[cand[j]].m->name;
                if (j != i && !emitted[j]
                    && (names_dep(&nm[i], dn) || names_dep(st[cand[i]].m, dn)))
                    blocked = 1;
            }
            if (!blocked)
                next = i;
        }
        for (i = 0; next == ncand && i < ncand; i++)
            if (!emitted[i])
                next = i;
        emitted[next] = 1;
        order[pos] = next;
        tr("upgrade %lu of %lu: %s %s to %s", (unsigned long)pos + 1, (unsigned long)ncand,
           st[cand[next]].m->name, st[cand[next]].m->version, st[cand[next]].offer->version);
    }
    for (pos = 0; pos < ncand; pos++) {
        const struct standing *s = &st[cand[order[pos]]];
        struct plan p;
        unsigned long placed, dropped, kept;
        size_t f;
        int ok_plan, blocked_by = -1;
        /* A package whose new version needs one that could not be upgraded
         * waits: it is skipped, and said why. */
        for (f = 0; f < pos && blocked_by < 0; f++)
            if (failed[f] && names_dep(&nm[order[pos]], st[cand[order[f]]].m->name))
                blocked_by = (int)f;
        if (blocked_by >= 0) {
            const char *dn = st[cand[order[blocked_by]]].m->name;
            failed[pos] = 1;
            nskipped++;
            if (machine) {
                char jn[300];
                snprintf(jn, sizeof jn, "%s %s %s", s->m->name, s->m->version, dn);
                rec_item("skipped", jn, "name", s->m->name, "installed", s->m->version,
                         "waits-for", dn, NULL);
            } else {
                say_pkgline(s->m->name, "skipped: it needs %s, which could not be upgraded", dn);
            }
            continue;
        }
        arch_for(base, s->m);
        quiet = 1;
        ok_plan = plan_target(&p, a, &ix, s->m->name, s->offer->version) == 0
                  && run_plan(&p, s->m, &placed, &dropped, &kept) == 0;
        quiet = 0;
        if (!ok_plan) {
            char codes[8];
            plan_free(&p);
            failed[pos] = 1;
            nrefused++;
            /* The owner's rule for a batch: go as far as possible, report
             * each, and exit with the worst class of the refusals. */
            if (refused_class > worst_class) {
                worst_class = refused_class;
                worst_next = refused_next;
            }
            snprintf(codes, sizeof codes, "%d", refused_class);
            {
                /* one line, as every record: a reason may have several */
                char *q;
                for (q = quiet_reason; *q; q++)
                    if (*q == '\n') *q = ' ';
            }
            if (machine) {
                char jn[2600];
                /* the reason last, since it has spaces: "<name> <version> <class> <reason>" */
                snprintf(jn, sizeof jn, "%s %s %s %s", s->m->name, s->m->version,
                         class_name(refused_class), quiet_reason);
                rec_item("refused", jn, "name", s->m->name, "installed", s->m->version,
                         "class", class_name(refused_class), "code", codes, "reason", quiet_reason,
                         "next", refused_next ? refused_next : next_default(refused_class), NULL);
            } else {
                say_pkgline(s->m->name, "not upgraded: %s", quiet_reason);
            }
            if (at_r + 80 < sizeof refused_names)
                at_r += (size_t)snprintf(refused_names + at_r, sizeof refused_names - at_r, "%s%s (%s)",
                                         at_r ? ", " : "", s->m->name, class_name(refused_class));
            refused_class = 0;
            refused_next = NULL;
            continue;
        }
        if (machine) {
            char jn[500];
            const char *from = nchans > 1 ? chan_of(s->offer) : NULL;
            snprintf(jn, sizeof jn, "%s %s %s%s%s", s->m->name, s->m->version, s->offer->version,
                     from ? " " : "", from ? from : "");
            if (from != NULL)
                rec_item("package", jn, "name", s->m->name, "from", s->m->version,
                         "version", s->offer->version, "channel", from, NULL);
            else
                rec_item("package", jn, "name", s->m->name, "from", s->m->version,
                         "version", s->offer->version, NULL);
        } else {
            char keptw[40];
            keptw[0] = '\0';
        if (kept) snprintf(keptw, sizeof keptw, ", %lu kept", kept);
            say_pkgline(s->m->name, "%s from %s to %s%s%s: %lu placed, %lu removed%s",
                dry_run ? "would upgrade" : "upgraded",
                s->m->version, s->offer->version,
                nchans > 1 ? " from " : "", nchans > 1 ? chan_of(s->offer) : "",
                placed, dropped, keptw);
        }
        {
            const struct fetched *f = &p.f[p.n - 1];
            if (strcmp(f->m.kind, "image") == 0 && f->m.nfiles == 1)
                hint("the image %s is replaced: a machine that has it mounted must Eject it %s, "
                     "and MOUNTLIST %s ROOT %s writes the new entry, since its size may change",
                     f->m.files[0].path, dry_run ? "first" : "and mount it again", f->m.name, a->root);
        }
        plan_free(&p);
        if (dry_run) {
            planned[nplanned].name = s->m->name;
            planned[nplanned].version = s->offer->version;
            nplanned++;
        }
        done++;
    }
    {
        char summary[900];
        size_t tried = ncand + nconflict;
        if (nrefused + nskipped == 0 && tried == 0)
            snprintf(summary, sizeof summary, "nothing needs an update: %lu package%s, none with a "
                     "newer version in the channel", (unsigned long)in.n, in.n == 1 ? "" : "s");
        else if (nrefused + nskipped == 0)
            snprintf(summary, sizeof summary, "%s %lu package%s", dry_run ? "would update" : "updated",
                     (unsigned long)done, done == 1 ? "" : "s");
        else
        {
            char waiting[80] = "";
            if (nskipped)
                snprintf(waiting, sizeof waiting, "; %lu waiting for one of them", (unsigned long)nskipped);
            snprintf(summary, sizeof summary, "%s %lu of %lu package%s; not upgraded: %s%s. "
                     "Everything else went ahead", dry_run ? "would update" : "updated",
                     (unsigned long)done, (unsigned long)tried, tried == 1 ? "" : "s",
                     refused_names, waiting);
        }
        if (nrefused + nskipped == 0) {
            kv("result", "%s", tried == 0 ? "unchanged" : res("upgraded", "would-upgrade"));
        } else {
            /* Some needed a decision: the answer is a refusal, with the
             * worst class of them, so the exit code and next say what to do. */
            refused_class = worst_class;
            refused_next = worst_next;
            kv("result", "refused");
            kv("class", "%s", class_name(worst_class));
            kv("code", "%d", worst_class);
        }
        kv("upgraded", "%lu", (unsigned long)done);
        kv("not-upgraded", "%lu", (unsigned long)(nrefused + nskipped));
        kv("count", "%lu", (unsigned long)done);
        kv("summary", "%s", summary);
        if (nrefused + nskipped > 0)
            kv("next", "%s", worst_next ? worst_next : next_default(worst_class));
        if (!machine)
            say_result("%s", summary);
        rc = nrefused + nskipped == 0 ? 0 : 1;
    }
out:
    for (i = 0; nm != NULL && i < ncand; i++)
        pkg_manifest_free(&nm[i]);
    free(nm);
    free(st);
    free(cand);
    free(order);
    free(emitted);
    free(failed);
    free(planned);
    planned = NULL;
    nplanned = 0;
    installed_free(&in);
    free(ix.e);
    return rc;
}

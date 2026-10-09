/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * PUSH: a local channel sent to a portal.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- PUSH: a local channel to a portal ---------------------------------- *
 *
 * The portal serves a channel file for file; PUSH sends what a local one
 * has and it lacks: plan (which files the portal needs), files (large ones
 * in parts that resume), commit (the local index, merged by the portal,
 * which checks the result with Pkg itself). The key travels in a header
 * file, never on a command line, and only over https, or http to this
 * machine for tests. The portal's answers are records in this program's
 * own form, relayed as they come. */

#define PUSH_PART_DEFAULT (32ul << 20)

static int push_path_ok(const char *rel)
{
    size_t n = strlen(rel);
    if (strncmp(rel, "objects/", 8) == 0)
        return 1;
    if (strncmp(rel, "archives/", 9) == 0)
        return !(n > 7 && strcmp(rel + n - 7, ".pkgidx") == 0) && !(n > 7 && strcmp(rel + n - 7, ".sha256") == 0)
           && !(n > 7 && strcmp(rel + n - 7, ".pkgmap") == 0);
    if (strcmp(rel, "Bootstrap/SHA256SUMS") == 0 || strcmp(rel, "Bootstrap/SHA256SUMS.sig") == 0)
        return 1;   /* the bootstraps' digests, signed for ssh-keygen -Y verify */
    if (strncmp(rel, "Bootstrap/", 10) == 0) {
        /* Bootstrap/<cpu>/Pkg for AROS, Bootstrap/<platform>/pkg or pkg.exe for a host */
        const char *plat = rel + 10, *slash = strchr(plat, '/');
        size_t k;
        if (slash == NULL || slash == plat || strchr(slash + 1, '/') != NULL)
            return 0;
        for (k = 0; plat + k < slash; k++)
            if (!((plat[k] >= 'a' && plat[k] <= 'z') || (plat[k] >= '0' && plat[k] <= '9')
                  || plat[k] == '-' || plat[k] == '_'))
                return 0;
        /* the file itself: Pkg on AROS, as AmigaDOS names a command, pkg on a host */
        return strcmp(slash + 1, "Pkg") == 0 || strcmp(slash + 1, "pkg") == 0
               || strcmp(slash + 1, "pkg.exe") == 0;
    }
    return strcmp(rel, "Install-Pkg") == 0 || strcmp(rel, "ReadMe") == 0;
}

struct push_list { char **rel; size_t n, cap; };

static int push_collect(const char *rel, void *ctx)
{
    struct push_list *pl = (struct push_list *)ctx;
    if (!push_path_ok(rel)) return 0;
    if (pl->n == pl->cap) {
        size_t nc = pl->cap ? pl->cap * 2 : 64;
        char **g = (char **)realloc(pl->rel, nc * sizeof *g);
        if (g == NULL) return -1;
        pl->rel = g;
        pl->cap = nc;
    }
    pl->rel[pl->n] = pkg_strdup(rel);
    return pl->rel[pl->n++] ? 0 : -1;
}

/* A file hashed to be named, as a PREPARE step against its size. */
static void hash_step(const char *path, const char *rel)
{
    struct pkg_fs_id id;
    char name[120];
    short_name(name, sizeof name, rel);
    ev_begin(PKG_PHASE_PREPARE, PKG_OBJECT_FILE, name, NULL, NULL, PKG_MEASURE_BYTES_HASHED, 0, 0);
    if (path != NULL && pkg_fs_identity(path, &id) == 0 && id.exists)
        ev_total(id.size);
}

/* sha256 and size of a file read in pieces: an archive is large. */
int file_digest(const char *path, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size,
                int counted)
{
    FILE *f = fopen(path, "rb");
    unsigned char buf[65536], dg[PKG_SHA256_LEN];
    struct pkg_sha256 c;
    size_t n, k;
    if (f == NULL) return -1;
    pkg_sha256_init(&c);
    *size = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        pkg_sha256_update(&c, buf, n);
        *size += n;
        if (counted) ev_count(*size);
    }
    fclose(f);
    pkg_sha256_final(&c, dg);
    for (k = 0; k < PKG_SHA256_LEN; k++) snprintf(hex + 2 * k, 3, "%02x", dg[k]);
    return 0;
}

/* The value of the first "key: " record in an answer file, or NULL. */
static char *answer_field(const char *path, const char *key, char *out, size_t ol)
{
    unsigned char *buf;
    size_t len, kl = strlen(key);
    const char *p, *end;
    if (pkg_fs_read(path, &buf, &len) != 0) return NULL;
    for (p = (const char *)buf, end = p + len; p < end; ) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll > kl + 1 && memcmp(p, key, kl) == 0 && p[kl] == ':' && p[kl + 1] == ' ') {
            snprintf(out, ol, "%.*s", (int)(ll - kl - 2), p + kl + 2);
            free(buf);
            return out;
        }
        p = nl ? nl + 1 : end;
    }
    free(buf);
    return NULL;
}

/* An archive whose every package records where it is published upstream
 * (Archive:) is downloaded from there by installs: PUSH leaves it out. */
static void push_drop_upstream(const char *channel, struct push_list *pl)
{
    struct index ix;
    size_t e, i, o;
    if (read_index(channel, &ix) != 0) { refused_class = 0; return; }
    for (i = 0, o = 0; i < pl->n; i++) {
        const char *rel = pl->rel[i];
        int upstream = 0, local = 0;
        if (strncmp(rel, "archives/", 9) == 0) {
            for (e = 0; e < ix.n; e++) {
                char *mp = object_path(channel, ix.e[e].digest, "manifest"), err[200], an[1024], ap[1024];
                unsigned char *buf;
                size_t len;
                struct pkg_manifest m;
                if (mp == NULL || pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
                free(mp);
                if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) == 0) {
                    if (m.source && pkg_archive_split(m.source, an, sizeof an, ap, sizeof ap)
                        && strcmp(an, rel + 9) == 0) {
                        if (m.archive_sha) upstream++; else local++;
                    }
                    pkg_manifest_free(&m);
                }
                free(buf);
            }
        }
        if (upstream > 0 && local == 0) {
            tr("push leaves out %s: its %d packages download it from upstream", rel, upstream);
            free(pl->rel[i]);
            continue;
        }
        pl->rel[o++] = pl->rel[i];
    }
    pl->n = o;
    free(ix.e);
}

/* How a push proves who sends it. Over https: the portal's key, a secret, in
 * each request. Over plain http nothing secret may travel, so each request is
 * signed with the publisher's own key instead, which is also how a publisher
 * with no portal key pushes: the portal gives a session, and the signature
 * covers the method, the path, the session, a number that only grows and the
 * SHA-256 of the body, so a request cannot be altered, replayed or moved to
 * another address. */
struct push_auth {
    const char *bearer;         /* https: PKG_PUSHKEY */
    struct key  k;              /* http: the signing key */
    char        session[80];
    unsigned long seq;
    const char *hdr;            /* the header file each request is sent with */
};

static int push_send(struct push_auth *pa, const char *method, const char *url, const char *body,
                     const char *range, const char *out, int *code, char *err, size_t errlen)
{
    char h[3200];
    if (pa->bearer != NULL) {
        snprintf(h, sizeof h, "Authorization: Bearer %s\n%s%s%s", pa->bearer,
                 range ? "Content-Range: " : "", range ? range : "", range ? "\n" : "");
    } else {
        char digest[PKG_SHA256_HEXLEN + 1], text[2800], sighex[2 * PKG_ED25519_SIG + 1];
        unsigned char sig[PKG_ED25519_SIG];
        unsigned long long size;
        const char *path = strstr(url, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        if (path == NULL || file_digest(body, digest, &size, 0) != 0) {
            snprintf(err, errlen, "cannot read %s to sign the request", body);
            return -1;
        }
        pa->seq++;
        snprintf(text, sizeof text, "pkg-push-1\n%s\n%s\n%s\n%lu\n%s\n%s\n", method, path, pa->session,
                 pa->seq, digest, range ? range : "-");
        pkg_ed25519_sign(sig, (const unsigned char *)text, strlen(text), pa->k.sk);
        tohex(sig, sizeof sig, sighex);
        snprintf(h, sizeof h, "Authorization: Pkg-Signature key=%s,session=%s,seq=%lu,sha256=%s,sig=%s\n%s%s%s",
                 pa->k.pkhex, pa->session, pa->seq, digest, sighex,
                 range ? "Content-Range: " : "", range ? range : "", range ? "\n" : "");
    }
    if (pkg_fs_write_private(pa->hdr, h, strlen(h)) != 0) {
        snprintf(err, errlen, "cannot write the request's headers where only this user reads them");
        return -1;
    }
    if (pkg_net_send(method, url, body, pa->hdr, out, code, err, errlen) != 0)
        return -1;
    if (*code == 426) {           /* the portal no longer takes this Pkg: its words, not a number */
        char why[500] = "", nx[500] = "";
        answer_field(out, "reason", why, sizeof why);
        answer_field(out, "next", nx, sizeof nx);
        snprintf(err, errlen, "%s%s%s", why[0] ? why : "the portal asks for a newer pkg", nx[0] ? ". " : "", nx);
        return -1;
    }
    return 0;
}

static int cmd_push(const struct pkg_options *a)
{
    struct push_auth pa;
    int signed_push;
    struct push_list pl = { NULL, 0, 0 };
    char *cache = NULL, *tmp = NULL, *hdr = NULL, *plan = NULL, *out = NULL, *part = NULL, *ix = NULL;
    char url[2300], err[400], line[2200];
    const char *base;
    unsigned skipped = 0;
    unsigned long long sent_bytes = 0;
    unsigned long partsz = PUSH_PART_DEFAULT;
    size_t i, sent = 0, nneed = 0;
    int code = 0, rc = 1;
    FILE *f;

    if (a->channel == NULL) return refuse_c(20, "name the local channel with CHANNEL <dir>");
    if (a->to == NULL) return refuse_c(20, "name the portal's channel with TO <url>");
    if (is_url(a->channel))
        return refuse_c(20, "CHANNEL is the channel on this machine that PUSH sends; the portal's is TO");
    if (strncmp(a->to, "https://", 8) != 0 && strncmp(a->to, "http://", 7) != 0)
        return refuse_c(20, "TO is the portal's channel, an https:// or http:// address: %s", a->to);
    memset(&pa, 0, sizeof pa);
    /* The portal's key where it is safe to send: https, or a test portal on this
     * machine. Anywhere else, and whenever there is no such key, each request is
     * signed with the publisher's own key. */
    {
        int has_key = a->pushkey != NULL && a->pushkey[0] != '\0';
        int local = strncmp(a->to, "http://127.0.0.1", 16) == 0 || strncmp(a->to, "http://localhost", 16) == 0;
        signed_push = !(has_key && (strncmp(a->to, "https://", 8) == 0 || local));
    }
    if (signed_push) {
        if (a->sign == NULL || a->sign[0] == '\0') {
            /* the portal's own page on becoming a publisher: scheme and host of TO */
            const char *h = strstr(a->to, "://") + 3, *e = strchr(h, '/');
            int bl = (int)(e ? (size_t)(e - a->to) : strlen(a->to));
            return refuse_n(14, "ask-requester", "no key to push with. A portal takes pushes only from "
                            "publishers its maintainers have registered, and nobody can register themselves "
                            "yet: ask them, as %.*s/publishers explains. Once registered, sign the push with SIGN "
                            "<keyfile> (or PKG_SIGNKEY), the key you registered, or over https with "
                            "PKG_PUSHKEY, the key the portal gave you. Never make a key up", bl, a->to);
        }
        if (load_key(a->sign, &pa.k) != 0)
            return 1;
    } else {
        pa.bearer = a->pushkey;
    }
    ix = pkg_join(a->channel, "index");
    if (ix == NULL || !pkg_fs_exists(ix)) {
        free(ix);
        return refuse_c(11, "%s has no index: nothing is published there to push", a->channel);
    }
    if (getenv("PKG_PUSH_PART_BYTES"))            /* tests make parts small */
        partsz = strtoul(getenv("PKG_PUSH_PART_BYTES"), NULL, 10);
    if (partsz < 1024) partsz = 1024;
    base = a->to;
    if (pkg_fs_walk(a->channel, push_collect, NULL, &pl, &skipped, err, sizeof err) != 0) {
        refuse_c(17, "cannot read %s: %s", a->channel, err);
        goto out;
    }
    push_drop_upstream(a->channel, &pl);
    cache = pkg_cache_dir();
    {
        char name[64];
        snprintf(name, sizeof name, "push-%ld", (long)time(NULL));
        tmp = cache ? pkg_join(cache, name) : NULL;
    }
    if (tmp == NULL || pkg_fs_mkdirs(tmp) != 0) { refuse_c(17, "cannot make a working directory"); goto out; }
    hdr = pkg_join(tmp, "headers");
    plan = pkg_join(tmp, "plan");
    out = pkg_join(tmp, "answer");
    part = pkg_join(tmp, "part");
    if (hdr == NULL || plan == NULL || out == NULL || part == NULL) { refuse_c(17, "out of memory"); goto out; }
    pa.hdr = hdr;
    if (signed_push) {
        /* the session: asked for by public key, answered with a number to sign against */
        snprintf(line, sizeof line, "key: %s\n", pa.k.pkhex);
        snprintf(url, sizeof url, "%s/_push/session", base);
        if (pkg_fs_write_private(plan, line, strlen(line)) != 0 || pkg_fs_write_private(hdr, "", 0) != 0
            || pkg_net_send("POST", url, plan, hdr, out, &code, err, sizeof err) != 0) {
            refuse_c(17, "cannot reach %s: %s", base, err);
            goto out;
        }
        if (code != 200 || answer_field(out, "session", pa.session, sizeof pa.session) == NULL) {
            char why[600];
            refuse_n(14, "ask-requester", "the portal gave no session for the key %.16s (HTTP %d)%s%s", pa.k.pkhex, code,
                     answer_field(out, "reason", why, sizeof why) ? ": " : "", answer_field(out, "reason", why, sizeof why) ? why : "");
            goto out;
        }
    }

    /* 1. plan: every file, its digest and size; the portal says what it needs */
    f = fopen(plan, "wb");
    if (f == NULL) { refuse_c(17, "cannot write the plan"); goto out; }
    for (i = 0; i < pl.n; i++) {
        char hex[PKG_SHA256_HEXLEN + 1], *full = pkg_join(a->channel, pl.rel[i]);
        unsigned long long size;
        int hashed;
        hash_step(full, pl.rel[i]);
        hashed = full != NULL && file_digest(full, hex, &size, 1) == 0;
        ev_end(hashed ? PKG_OUTCOME_OK : PKG_OUTCOME_FAILED);
        if (!hashed) {
            free(full); fclose(f);
            refuse_c(17, "cannot read %s", pl.rel[i]);
            goto out;
        }
        fprintf(f, "%s %s %llu\n", pl.rel[i], hex, size);
        free(full);
    }
    fclose(f);
    snprintf(url, sizeof url, "%s/_push/plan", base);
    if (push_send(&pa, "POST", url, plan, NULL, out, &code, err, sizeof err) != 0) {
        refuse_c(17, "cannot reach %s: %s", base, err);
        goto out;
    }
    if (code == 401 || code == 403) {
        char why[600];
        refuse_n(14, "ask-requester", "the portal refused the key (HTTP %d)%s%s", code,
                 answer_field(out, "reason", why, sizeof why) ? ": " : "", answer_field(out, "reason", why, sizeof why) ? why : "");
        goto out;
    }
    if (code != 200) { refuse_c(17, "the portal answered the plan with HTTP %d", code); goto out; }

    /* 2. the files it needs, whole or in parts */
    {
        unsigned char *ans;
        size_t alen;
        char **need = NULL;
        const char *p2, *end;
        if (pkg_fs_read(out, &ans, &alen) != 0) { refuse_c(17, "cannot read the plan's answer"); goto out; }
        for (p2 = (const char *)ans, end = p2 + alen; p2 < end; ) {
            const char *nl = memchr(p2, '\n', (size_t)(end - p2));
            size_t ll = nl ? (size_t)(nl - p2) : (size_t)(end - p2);
            if (ll > 6 && memcmp(p2, "need: ", 6) == 0) {
                char **g = (char **)realloc(need, (nneed + 1) * sizeof *g);
                if (g == NULL) break;
                need = g;
                need[nneed] = (char *)malloc(ll - 5);
                if (need[nneed] == NULL) break;
                snprintf(need[nneed], ll - 5, "%.*s", (int)(ll - 6), p2 + 6);
                nneed++;
            }
            p2 = nl ? nl + 1 : end;
        }
        free(ans);
        /* every file the portal needs, each a step of its own in bytes it acknowledged */
        ev_begin(PKG_PHASE_UPLOAD, PKG_OBJECT_CHANNEL, chan_label(a->channel), NULL, NULL,
                 PKG_MEASURE_FILES_SENT, nneed, 1);
        for (i = 0; i < nneed; i++) {
            char *full = pkg_join(a->channel, need[i]), hex[PKG_SHA256_HEXLEN + 1], result[64], rec[64];
            char shown[120];
            unsigned long long size = 0, off = 0;
            int ok_file = 0;
            if (!push_path_ok(need[i]) || full == NULL || file_digest(full, hex, &size, 0) != 0) {
                warn("the portal asked for %s, which this channel does not send", need[i]);
                free(full);
                continue;
            }
            snprintf(url, sizeof url, "%s/_push/files/%s", base, need[i]);
            short_name(shown, sizeof shown, need[i]);
            ev_begin(PKG_PHASE_UPLOAD, PKG_OBJECT_FILE, shown, NULL, NULL, PKG_MEASURE_BYTES_SENT,
                     size, 1);
            if (size <= partsz) {
                if (push_send(&pa, "PUT", url, full, NULL, out, &code, err, sizeof err) == 0 && code == 200
                    && answer_field(out, "result", result, sizeof result)
                    && (strcmp(result, "received") == 0 || strcmp(result, "unchanged") == 0))
                    ok_file = 1;
            } else {
                /* in parts, each resuming where the portal says it got to */
                FILE *src = fopen(full, "rb");
                while (src != NULL && off < size) {
                    unsigned long long end2 = off + partsz < size ? off + partsz : size;
                    FILE *pf = fopen(part, "wb");
                    char buf[65536], ph[2600];
                    unsigned long long left = end2 - off;
                    if (pf == NULL) break;
                    fseek(src, (long)off, SEEK_SET);
                    while (left > 0) {
                        size_t k = fread(buf, 1, left < sizeof buf ? (size_t)left : sizeof buf, src);
                        if (k == 0) break;
                        fwrite(buf, 1, k, pf);
                        left -= k;
                    }
                    fclose(pf);
                    snprintf(ph, sizeof ph, "bytes %llu-%llu/%llu", off, end2 - 1, size);
                    if (push_send(&pa, "PUT", url, part, ph, out, &code, err, sizeof err) != 0 || code != 200
                        || answer_field(out, "result", result, sizeof result) == NULL)
                        break;
                    if (strcmp(result, "received") == 0 || strcmp(result, "unchanged") == 0) { ok_file = 1; break; }
                    if (strcmp(result, "partial") != 0 || answer_field(out, "received", rec, sizeof rec) == NULL)
                        break;
                    off = strtoull(rec, NULL, 10);         /* the portal's count, so a resume skips */
                    ev_count(off);
                }
                if (src) fclose(src);
            }
            if (!ok_file) {
                char why[600];
                refuse_c(17, "the portal did not take %s: %s", need[i],
                         answer_field(out, "reason", why, sizeof why) ? why : code ? "an unexpected answer" : err);
                ev_end(PKG_OUTCOME_FAILED);
                ev_end(PKG_OUTCOME_FAILED);
                for (; i < nneed; i++) free(need[i]);
                free(need);
                free(full);
                goto out;
            }
            ev_count(size);
            ev_end(PKG_OUTCOME_OK);
            ev_count(i + 1);
            sent++;
            sent_bytes += size;
            free(full);
            free(need[i]);
        }
        ev_count(nneed);
        ev_end(PKG_OUTCOME_OK);
        free(need);
    }

    /* 3. commit: the local index; the portal merges, checks and answers */
    snprintf(url, sizeof url, "%s/_push/commit", base);
    if (push_send(&pa, "POST", url, ix, NULL, out, &code, err, sizeof err) != 0) {
        refuse_c(17, "cannot reach %s to commit: %s", base, err);
        goto out;
    }
    {
        unsigned char *ans;
        size_t alen;
        char result[64] = "", cls[16] = "", nextw[40] = "", summary[1200] = "";
        const char *p2, *end;
        if (pkg_fs_read(out, &ans, &alen) != 0) { refuse_c(17, "cannot read the commit's answer"); goto out; }
        /* relayed record by record: the portal speaks this program's form */
        for (p2 = (const char *)ans, end = p2 + alen; p2 < end; ) {
            const char *nl = memchr(p2, '\n', (size_t)(end - p2));
            size_t ll = nl ? (size_t)(nl - p2) : (size_t)(end - p2);
            char k[64], v[2048];
            const char *colon = memchr(p2, ':', ll);
            if (colon && (size_t)(colon - p2) < sizeof k && colon + 1 < p2 + ll && colon[1] == ' ') {
                snprintf(k, sizeof k, "%.*s", (int)(colon - p2), p2);
                snprintf(v, sizeof v, "%.*s", (int)(ll - (size_t)(colon - p2) - 2), colon + 2);
                if (strcmp(k, "result") == 0) snprintf(result, sizeof result, "%.63s", v);
                else if (strcmp(k, "code") == 0) snprintf(cls, sizeof cls, "%.15s", v);
                else if (strcmp(k, "next") == 0) snprintf(nextw, sizeof nextw, "%.39s", v);
                else if (strcmp(k, "summary") == 0) snprintf(summary, sizeof summary, "%.1199s", v);
                if (machine && sink && sink->record)
                    { one_line(v); sink->record(sink->user, k, v); }
                else if (strcmp(k, "refused") == 0 || strcmp(k, "published") == 0)
                    say_item(k, "%s", v);
            }
            p2 = nl ? nl + 1 : end;
        }
        free(ans);
        if (result[0] == '\0' && code != 200) {
            /* no record at all: the portal itself failed, or something in front of it answered */
            refuse_c(17, "%s answered the commit with HTTP %d and no record of what it did: nothing says "
                     "the push was published. Read the channel with SHOW, and push again: what was "
                     "sent is not sent twice", base, code);
            goto out;
        }
        kv("uploaded", "%lu", (unsigned long)sent);
        kv("uploaded-bytes", "%llu", sent_bytes);
        if (!machine)
        {
            say_result("%s", summary[0] ? summary : result);
            say_detail("%lu file%s sent to %s (%llu bytes)",
                (unsigned long)sent, sent == 1 ? "" : "s", base, sent_bytes);
        }
        if (code == 401 || code == 403) {
            refused_class = 14;
            refused_next = "ask-requester";
            rc = 1;
        } else if (strcmp(result, "refused") == 0 || code != 200) {
            refused_class = cls[0] ? atoi(cls) : 10;
            refused_next = strcmp(nextw, "stop") == 0 ? "stop" : strcmp(nextw, "fix-command") == 0 ? "fix-command"
                         : strcmp(nextw, "ask-requester") == 0 ? "ask-requester" : "report";
            rc = 1;
        } else {
            rc = 0;
        }
    }
out:
    memset(&pa, 0, sizeof pa);
    if (hdr) pkg_fs_unlink(hdr);
    if (tmp) pkg_fs_rmtree(tmp);
    for (i = 0; i < pl.n; i++) free(pl.rel[i]);
    free(pl.rel);
    free(cache); free(tmp); free(hdr); free(plan); free(out); free(part); free(ix);
    return rc;
}

int pkg_push(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "push", cmd_push, o); }

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The interface pkg.h declares, and the read-only update checks.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- the interface in pkg.h ------------------------------------------- */

int cancelled(const char *when)
{
    if (sink == NULL || sink->cancel == NULL || !sink->cancel(sink->user))
        return 0;
    refuse_n(PKGRC_REFUSED, "report", "cancelled by the caller %s", when);
    return 1;
}

/* Every operation starts from the same clean state and ends with its code. */
/* What a program passes, like what a person types, holds no line break or
 * control character: each would end up in a path, a name or a record. */
static int options_clean(const struct pkg_options *o)
{
    const struct { const char *what, *v; } f[] = {
        { "the name", o->target }, { "ROOT", o->root }, { "CHANNEL", o->channel }, { "AT", o->at },
        { "NAME", o->name }, { "VERSION", o->version }, { "ARCH", o->arch }, { "KIND", o->kind },
        { "DEPENDS", o->depends }, { "SIGN", o->sign }, { "FILE", o->file }, { "KEY", o->key },
        { "NAMESPACE", o->nspace },
        { "OUT", o->out }, { "ACCEPTKEY", o->acceptkey }, { "UNIT", o->unit },
        { "HANDLER", o->handler }, { "FILES", o->files }, { "BUILD", o->build },
        { "ARCHIVE", o->archive }, { "TO", o->to }, { "PKG_PUSHKEY", o->pushkey },
        { "CONFIG", o->config }, { "UPSTREAM", o->upstream },
        { "SHORT", o->short_desc }, { "DESCRIPTION", o->description }, { "CATEGORY", o->category },
        { "TAGS", o->tags }, { "AUTHOR", o->author }, { "HOMEPAGE", o->homepage },
        { "REPOSITORY", o->repository }, { "LICENSE", o->license },
        { "DISTRIBUTION", o->distribution }, { "CHANGES", o->changes }, { "ICON", o->icon },
        { "SCREENSHOT", o->screenshot }, { "README", o->readme }, { "INFO", o->info },
        { "FROM", o->from }
    };
    size_t i, j;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        for (j = 0; f[i].v && f[i].v[j]; j++)
            if ((unsigned char)f[i].v[j] < 0x20 || (unsigned char)f[i].v[j] == 0x7F)
                return refuse_c(20, "%s holds a %s at character %lu; give it without",
                                f[i].what, f[i].v[j] == '\n' || f[i].v[j] == '\r' ? "line break"
                                : f[i].v[j] == '\t' ? "tab" : "control character",
                                (unsigned long)j + 1);
    return 0;
}

/* The network's trace lines, as the operation's own. */
static void net_trace_line(const char *line)
{
    tr("%s", line);
}

int call(const struct pkg_sink *s, const char *verb, op_fn fn, const struct pkg_options *o)
{
    static const struct pkg_options none;
    int rc;
    sink = s;
    verb_name = verb;
    machine = s != NULL && s->structured;
    dry_run = o != NULL && o->dryrun;
    refused_class = 0;
    refused_next = NULL;
    pending_next = NULL;
    quiet = 0;
    target_arch = NULL;
    root_arch[0] = '\0';
    told_source = 0;
    pick_root = NULL;
    pick_refused = 0;
    chans_clear();
    nfetched_once = 0;
    net_failed = 0;
    net_err[0] = '\0';
    opt_unpacked = o != NULL ? o->unpacked : NULL;
    pkg_fs_on_trace = s != NULL && s->trace != NULL ? net_trace_line : NULL;
    /* The activity line, for as long as this operation runs and no longer. */
    if (!machine && s != NULL && s->progress) {
        pkg_activity_to(activity_show, activity_say, NULL);
        pkg_fs_on_transfer = on_transfer;
        pkg_fs_on_wait = on_wait;
        pkg_fs_on_tick = pkg_activity_tick;
        pkg_archive_on_read = on_archive_read;
    }
    placements_clear();
    requested_at = o ? o->at : NULL;
    if (requested_at && (strcmp(verb, "install") || (o && (o->all || o->nalso))))
        rc = refuse_c(20, "AT is supported by INSTALL with one named application");
    else if (options_clean(o != NULL ? o : &none) || placements_load(o ? o->root : NULL))
        rc = 1;
    else {
        struct placement *p;
        rc = 0;
        if (!strcmp(verb, "verify") || !strcmp(verb, "repair") || !strcmp(verb, "upgrade")
            || !strcmp(verb, "rollback") || !strcmp(verb, "remove") || !strcmp(verb, "install")) {
            for (p = placements; p && !rc; p = p->next)
                if (!o || !o->target || o->all || !strcmp(o->target, p->name)) {
                    struct pkg_manifest m;
                    rc = placement_available(p);
                    if (!rc && load_installed(o->root, p->name, &m, 1) == 0) {
                        rc = placement_check(&m);
                        pkg_manifest_free(&m);
                    }
                    if (!rc) {
                        char *to = installed_path(o->root, p->prefix);
                        kv("placement", "%s %s", p->prefix, to ? to : p->parent);
                        if (!machine) say_item("destination", "%s -> %s", p->prefix, to ? to : p->parent);
                        free(to);
                    }
                }
        }
        if (!rc) {
            /* One change of a root at a time: a second pkg changing the same
             * root would share its staging and interleave its records. A dry
             * run writes nothing and takes no lock. */
            void *lock = NULL;
            if (o != NULL && o->root != NULL && !o->dryrun && pkg_fs_is_dir(o->root)
                && (!strcmp(verb, "install") || !strcmp(verb, "upgrade") || !strcmp(verb, "rollback")
                    || !strcmp(verb, "repair") || !strcmp(verb, "remove"))) {
                int busy;
                lock = pkg_fs_lock_root(o->root, &busy);
                if (lock == NULL)
                    rc = busy ? refuse_n(15, "retry-later", "another pkg is changing %s now; nothing "
                                         "was changed. Give the command again once it is done", o->root)
                              : refuse_c(17, "the root %s cannot be locked for this change: %s",
                                         o->root, strerror(errno));
            }
            if (!rc) rc = fn(o != NULL ? o : &none);
            pkg_fs_unlock_root(lock);
        }
    }
    placements_clear();
    rc = rc == 0 ? PKGRC_OK : refused_class ? refused_class : PKGRC_REFUSED;
    did();
    pkg_activity_to(NULL, NULL, NULL);
    pkg_fs_on_transfer = NULL;
    pkg_fs_on_wait = NULL;
    pkg_fs_on_tick = NULL;
    pkg_archive_on_read = NULL;
    pkg_net_idle_close();               /* nothing the network holds open outlives the operation */
    pkg_fs_on_trace = NULL;
    chans_clear();
    sink = NULL;
    return rc;
}

/* ---- read-only update checks ------------------------------------------ */

static struct pkg_update_found *update_found;

static int update_text(char **out, const char *text)
{
    *out = pkg_strdup(text ? text : "");
    return *out != NULL ? 0 : refuse("out of memory");
}

static int update_changes(char **out, const struct pkg_strs *lines)
{
    size_t i, size = 1, at = 0;
    for (i = 0; i < lines->n; i++) {
        size_t n = strlen(lines->v[i]);
        if (n > (size_t)-1 - size - 1) return refuse("changes text is too large");
        size += n + 1;
    }
    *out = malloc(size);
    if (*out == NULL) return refuse("out of memory");
    for (i = 0; i < lines->n; i++) {
        size_t n = strlen(lines->v[i]);
        if (i) (*out)[at++] = '\n';
        memcpy(*out + at, lines->v[i], n);
        at += n;
    }
    (*out)[at] = '\0';
    return 0;
}

static int cmd_update_check(const struct pkg_options *a)
{
    struct pkg_update_found *r = update_found;
    struct pkg_manifest cur;
    struct index ix = { NULL, 0 };
    struct fetched offered;
    const struct entry *e = NULL;
    char pin[65], *path;
    size_t i;
    int rc = 1, loaded_index = 0;

    pkg_manifest_init(&cur);
    memset(&offered, 0, sizeof offered);
    if (a->target == NULL || pkg_check_name(a->target) != NULL
        || a->root == NULL || !a->root[0] || (a->channel && !a->channel[0]))
        return refuse_c(20, "provide a valid package name and installation root");
    path = root_path(a->root, "db", a->target);
    if (path == NULL) return refuse("out of memory");
    if (!pkg_fs_exists(path)) {
        free(path);
        r->state = PKG_UPDATE_NOT_MANAGED;
        return 0;
    }
    free(path);
    if (load_installed(a->root, a->target, &cur, 0) != 0) goto out;
    if (strcmp(cur.name, a->target) != 0) {
        refuse_c(12, "the installed database entry names another package");
        goto out;
    }
    if (update_text(&r->installed, cur.version) != 0) goto out;
    if (!pinned_key(a->root, a->target, pin)) {
        refuse_c(14, "the installed package has no readable pinned publisher key");
        goto out;
    }
    for (i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)pin[i])) {
            refuse_c(12, "the pinned publisher key is malformed");
            goto out;
        }
    }
    if (resolve_arch(a) != 0) goto out;
    if (target_arch == NULL)
        target_arch = cur.architecture;
    if (a->channel && !is_url(a->channel) && !pkg_fs_is_dir(a->channel)) {
        r->state = PKG_UPDATE_UNREACHABLE;
        refuse_c(11, "cannot read the channel at %s", a->channel);
        goto out;
    }
    if (open_channels(a, &ix) != 0) {
        if (refused_class == 11 || refused_class == 17) r->state = PKG_UPDATE_UNREACHABLE;
        goto out;
    }
    loaded_index = 1;
    for (i = 0; i < ix.n; i++) {
        const struct entry *old = &ix.e[i];
        if (strcmp(old->name, cur.name) == 0
            && pkg_version_cmp(old->version, cur.version) == 0
            && strcmp(old->arch, cur.architecture) == 0 && is_withdrawn(old)) {
            struct fetched verified;
            int valid = fetch_manifest(chan_of(old), old, &verified) == 0;
            if (valid && strcmp(verified.signer, pin) == 0) r->installed_withdrawn = 1;
            fetched_free(&verified);
            if (!valid) goto out;
        }
    }
    /* A check reports a changed key on the highest compatible offer.
     * Installation's trusted-key filtering would hide that information. */
    for (i = 0; i < ix.n; i++) {
        const struct entry *candidate = &ix.e[i];
        if (strcmp(candidate->name, a->target) || !arch_matches(candidate)
            || is_withdrawn(candidate)) continue;
        if (e == NULL || pkg_version_cmp(candidate->version, e->version) > 0)
            e = candidate;
    }
    if (net_failed) {
        r->state = PKG_UPDATE_UNREACHABLE;
        refuse_c(17, "cannot finish reading update metadata: %s", net_err);
        goto out;
    }
    if (e == NULL) {
        r->state = r->installed_withdrawn ? PKG_UPDATE_WITHDRAWN : PKG_UPDATE_NOT_OFFERED;
        rc = 0;
        goto out;
    }
    if (fetch_manifest(chan_of(e), e, &offered) != 0) goto out;
    if (update_text(&r->offered, offered.m.version) != 0
        || update_text(&r->channel, chan_of(e)) != 0
        || update_text(&r->signer, offered.signer) != 0
        || update_text(&r->homepage, offered.m.about.homepage) != 0
        || update_text(&r->short_desc, offered.m.about.short_desc) != 0
        || update_changes(&r->changes, &offered.m.about.changes) != 0) goto out;
    snprintf(r->manifest, sizeof r->manifest, "%s", e->digest);
    for (i = 0; i < offered.m.nfiles; i++) {
        if (offered.m.files[i].size > ~0ull - r->installed_bytes) {
            refuse_c(12, "the offered package's file sizes overflow the byte count");
            goto out;
        }
        r->installed_bytes += offered.m.files[i].size;
    }
    if (offered.m.archive_sha != NULL) {
        r->download_size_known = 1;
        r->download_bytes = offered.m.archive_size;
    }
    r->newer = pkg_version_cmp(offered.m.version, cur.version) > 0;
    if (strcmp(offered.signer, pin) != 0) r->state = PKG_UPDATE_KEY_CHANGED;
    else if (r->newer) r->state = PKG_UPDATE_AVAILABLE;
    else if (r->installed_withdrawn) r->state = PKG_UPDATE_WITHDRAWN;
    else r->state = PKG_UPDATE_NONE;
    rc = 0;
out:
    if (net_failed) r->state = PKG_UPDATE_UNREACHABLE;
    if (loaded_index) free(ix.e);
    fetched_free(&offered);
    pkg_manifest_free(&cur);
    return rc;
}

static void update_record(void *user, const char *key, const char *value)
{
    struct pkg_update_found *r = user;
    if (!strcmp(key, "reason")) snprintf(r->error, sizeof r->error, "%s", value);
}

int pkg_update_check(const struct pkg_update *u, struct pkg_update_found *found)
{
    struct pkg_sink output;
    struct pkg_options options;
    int code;
    if (found == NULL) return PKG_UPDATE_ERROR;
    pkg_update_found_free(found);
    found->state = PKG_UPDATE_ERROR;
    if (u == NULL) {
        found->code = PKG_RC_USAGE;
        snprintf(found->error, sizeof found->error, "provide the update configuration");
        return found->state;
    }
    memset(&output, 0, sizeof output);
    memset(&options, 0, sizeof options);
    output.record = update_record;
    output.user = found;
    output.structured = 1;
    options.target = u->package;
    options.root = u->root;
    options.channel = u->channel;
    update_found = found;
    code = call(&output, "update-check", cmd_update_check, &options);
    update_found = NULL;
    found->code = code;
    if (code && found->state != PKG_UPDATE_UNREACHABLE) found->state = PKG_UPDATE_ERROR;
    return found->state;
}

int pkg_keygen   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keygen", cmd_keygen, o); }
int pkg_sign     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "sign", cmd_sign, o); }
int pkg_checksig (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "checksig", cmd_checksig, o); }
int pkg_keyinfo  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keyinfo", cmd_keyinfo, o); }
int pkg_withdraw (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "withdraw", cmd_withdraw, o); }
int pkg_manifest (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "manifest", cmd_manifest, o); }
int pkg_publish  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "publish", cmd_publish, o); }
int pkg_install  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "install", cmd_install, o); }
int pkg_upgrade  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "upgrade", cmd_upgrade, o); }
int pkg_rollback (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "rollback", cmd_rollback, o); }
int pkg_list     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "list", cmd_list, o); }
int pkg_verify   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "verify", cmd_verify, o); }
int pkg_repair   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "repair", cmd_repair, o); }
int pkg_resolve  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "resolve", cmd_resolve, o); }
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "remove", cmd_remove, o); }
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "image", cmd_image, o); }
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "mountlist", cmd_mountlist, o); }
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "show", cmd_show, o); }
int pkg_status   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "status", cmd_status, o); }
int pkg_channel  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "channel", cmd_channel, o); }

char *pkg_channel_named(const char *root, const char *name, char *names, unsigned long names_len)
{
    struct chanlist cl;
    char *found = NULL;
    size_t i, at = 0;
    int q = quiet;

    if (names != NULL && names_len > 0)
        names[0] = '\0';
    if (root == NULL || !chan_name_ok(name))
        return NULL;
    /* A root with no list, or one that cannot be read, simply knows no
     * names: this answers a question, it does not refuse anything. */
    quiet = 1;
    if (chanlist_read(root, &cl) != 0) { quiet = q; return NULL; }
    quiet = q;
    for (i = 0; i < cl.n; i++) {
        if (cl.name[i] == NULL)
            continue;
        if (found == NULL && ascii_casecmp(cl.name[i], name) == 0)
            found = pkg_strdup(cl.v[i]);
        if (names != NULL && at + 2u < (size_t)names_len)
            at += (size_t)snprintf(names + at, (size_t)names_len - at, "%s%s", at ? ", " : "", cl.name[i]);
    }
    chanlist_free(&cl);
    return found;
}
int pkg_search   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "search", cmd_search, o); }

static const char *usage_reason;
static int usage_op(const struct pkg_options *o)
{
    (void)o;
    return refuse_c(PKGRC_USAGE, "%s", usage_reason);
}

const char *pkg_field(int n, const char *const *keys, const char *const *values, const char *key)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(keys[i], key) == 0)
            return values[i];
    return NULL;
}

int pkg_usage_error(const struct pkg_sink *s, const char *verb, const char *reason)
{
    usage_reason = reason;
    return call(s, verb, usage_op, NULL);
}

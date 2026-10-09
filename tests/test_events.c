/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Progress events, as a front end receives them: through pkg.h alone, with
 * a sink whose `event` copies every event it is lent. Run by tests/events.sh,
 * which serves EVENTS_DIR over HTTP at EVENTS_HTTP.
 *
 * Every operation run here has its whole stream checked by stream_ok: one
 * OP_BEGIN first, one OP_END last, every step begun is ended once and in
 * order, its parent is the step open when it began, its ids grow, its
 * counters never go back, a known total is never passed, units agree with
 * measures, and no step is abandoned. Then each case checks what it is
 * about:
 *
 *   install, upgrade   the phases in order, the counts against the result
 *                      records and the disk, the write-back to the medium
 *   nothing to do      an unchanged upgrade places nothing
 *   dry run            no phase that writes is announced
 *   refusals           integrity (payload, manifest), a missing payload, a
 *                      policy refusal, a write refused by the file system,
 *                      arguments: the step that failed, the code, no success
 *   restore            taken back when a later package fails; a restore
 *                      that fails is said to fail
 *   cancel             at the boundaries libpkg asks at
 *   forms              no callback, structured, text, progress on and off:
 *                      the same results, the same steps
 *   successive calls   operation numbers, step numbers, no callback kept
 *   re-entry           a call from a callback is refused on its own sink
 *   network            known and unknown lengths, an empty file, the cache,
 *                      several packages, a slow server: the wait before
 *                      the bytes
 *   pace               a test clock: frozen, nothing but begins and ends
 *                      and the forced updates; running, every update
 *   simulated backend  more than 4 GiB, a restart, a wrong total, a URL
 *                      with credentials, fed through the transfer hooks
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void ok(int c, const char *what)
{
    checks++;
    if (!c) { failures++; printf("  FAIL %s\n", what); }
}

/* ---- the recorder: a copy of every event, as a UI keeps them ----------- */

struct copy {
    struct pkg_event e;
    char verb[24], package[72], version[64], object[200];
};

#define MAXEV 20000
static struct copy ev[MAXEV];
static int nev, lost;

static const char *dup_into(char *out, size_t ol, const char *in)
{
    if (in == NULL) return NULL;
    snprintf(out, ol, "%s", in);
    return out;
}

/* What a case wants done from inside the callback. */
static void (*on_each)(const struct pkg_event *e);

static void record_event(void *user, const struct pkg_event *e)
{
    struct copy *c;
    (void)user;
    if (nev == MAXEV) { lost++; return; }
    c = &ev[nev++];
    c->e = *e;
    c->e.verb = dup_into(c->verb, sizeof c->verb, e->verb);
    c->e.package = dup_into(c->package, sizeof c->package, e->package);
    c->e.version = dup_into(c->version, sizeof c->version, e->version);
    c->e.object = dup_into(c->object, sizeof c->object, e->object);
    if (on_each) on_each(e);
}

/* Records, as one text, for comparing two forms of the same operation. */
static char recs[65536];
static size_t nrecs;
static void record_rec(void *user, const char *key, const char *value)
{
    (void)user;
    if (nrecs + strlen(key) + strlen(value) + 3 < sizeof recs)
        nrecs += (size_t)snprintf(recs + nrecs, sizeof recs - nrecs, "%s=%s\n", key, value);
}
static const char *rec_field(const char *key)
{
    static char v[400];
    char want[64];
    const char *p;
    snprintf(want, sizeof want, "%s=", key);
    for (p = recs; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (strncmp(p, want, strlen(want)) == 0) {
            size_t n = strcspn(p + strlen(want), "\n");
            snprintf(v, sizeof v, "%.*s", (int)n, p + strlen(want));
            return v;
        }
    return NULL;
}

static int texts, progress_lines;
static void record_text(void *user, int is_error, const char *text)
{
    (void)user; (void)is_error; (void)text;
    texts++;
}
static void record_line(void *user, int kind, int is_error, const char *text)
{
    (void)user; (void)is_error; (void)text;
    texts++;
    if (kind == PKG_LINE_PROGRESS) progress_lines++;
}

static long long clock_now, clock_step;
static long long test_clock(void *user)
{
    (void)user;
    clock_now += clock_step;
    return clock_now;
}

static int cancel_now;
static int ask_cancel(void *user)
{
    (void)user;
    return cancel_now;
}

static void reset(void)
{
    nev = lost = 0;
    nrecs = 0;
    recs[0] = '\0';
    texts = progress_lines = 0;
    on_each = NULL;
    cancel_now = 0;
    clock_now = 0;
    clock_step = 0;
}

static struct pkg_sink sink_for(int structured)
{
    struct pkg_sink s;
    memset(&s, 0, sizeof s);
    s.structured = structured;
    s.record = record_rec;
    s.text = record_text;
    s.event = record_event;
    s.cancel = ask_cancel;
    return s;
}

/* ---- the stream's rules ------------------------------------------------ */

static int unit_of(int measure)
{
    switch (measure) {
    case PKG_MEASURE_NONE: return PKG_UNIT_NONE;
    case PKG_MEASURE_BYTES_TRANSFERRED: case PKG_MEASURE_BYTES_READ: case PKG_MEASURE_BYTES_VERIFIED:
    case PKG_MEASURE_BYTES_HASHED: case PKG_MEASURE_BYTES_SENT: return PKG_UNIT_BYTES;
    case PKG_MEASURE_ENTRIES_CHECKED: return PKG_UNIT_ENTRIES;
    case PKG_MEASURE_PACKAGES_RESTORED: return PKG_UNIT_PACKAGES;
    default: return PKG_UNIT_FILES;
    }
}

static int stream_bad(const char *why, int at)
{
    printf("    stream: %s at event %d of %d\n", why, at, nev);
    return 0;
}

static int stream_ok(void)
{
    unsigned long open[64], last_id = 0, op;
    unsigned long long done[64];
    int depth = 0, i, begins = 0, ends = 0;
    if (lost) return stream_bad("events lost by the recorder", nev);
    if (nev < 2) return stream_bad("fewer than two events", 0);
    op = ev[0].e.op;
    for (i = 0; i < nev; i++) {
        const struct pkg_event *e = &ev[i].e;
        if (e->size != sizeof(struct pkg_event)) return stream_bad("size", i);
        if (e->op != op) return stream_bad("another operation's number", i);
        if (e->verb == NULL) return stream_bad("no verb", i);
        if ((i == 0) != (e->type == PKG_EVENT_OP_BEGIN)) return stream_bad("OP_BEGIN not first, or twice", i);
        if ((i == nev - 1) != (e->type == PKG_EVENT_OP_END)) return stream_bad("OP_END not last, or twice", i);
        switch (e->type) {
        case PKG_EVENT_OP_BEGIN:
            if (e->step != 0) return stream_bad("an operation event with a step", i);
            break;
        case PKG_EVENT_OP_END:
            if (depth != 0) return stream_bad("the operation ended with steps open", i);
            if ((e->code == 0) != (e->outcome == PKG_OUTCOME_OK)) return stream_bad("code and outcome disagree", i);
            if (e->outcome == PKG_OUTCOME_CANCELLED && !e->cancel) return stream_bad("cancelled without cancel", i);
            break;
        case PKG_EVENT_STEP_BEGIN:
            if (e->step <= last_id) return stream_bad("a step id that does not grow", i);
            if (e->parent != (depth ? open[depth - 1] : 0)) return stream_bad("a parent that is not the open step", i);
            if (e->attempt < 1) return stream_bad("attempt 0", i);
            if (e->done != 0) return stream_bad("a step that begins past zero", i);
            if (e->unit != unit_of(e->measure)) return stream_bad("unit and measure disagree", i);
            if (e->phase <= PKG_PHASE_NONE || e->phase > PKG_PHASE_UPLOAD) return stream_bad("no phase", i);
            if (depth == 64) return stream_bad("too deep", i);
            last_id = e->step;
            open[depth] = e->step;
            done[depth++] = 0;
            begins++;
            break;
        case PKG_EVENT_STEP_PROGRESS:
        case PKG_EVENT_STEP_WAIT:
        case PKG_EVENT_STEP_END:
            if (depth == 0 || e->step != open[depth - 1]) return stream_bad("an event for a step not open", i);
            if (e->done < done[depth - 1]) return stream_bad("a counter that went back", i);
            if (e->total_known && e->done > e->total) return stream_bad("done past a known total", i);
            if (!e->total_known && e->total != 0) return stream_bad("an unknown total that says a number", i);
            if (e->unit != unit_of(e->measure)) return stream_bad("unit and measure disagree", i);
            done[depth - 1] = e->done;
            if (e->type == PKG_EVENT_STEP_WAIT && e->wait != PKG_WAIT_NONE && e->object_kind != PKG_OBJECT_HOST)
                return stream_bad("a wait that names no host", i);
            if (e->type == PKG_EVENT_STEP_END) {
                if (e->outcome == PKG_OUTCOME_NONE || e->outcome == PKG_OUTCOME_ABANDONED)
                    return stream_bad("a step ended without its outcome", i);
                depth--;
                ends++;
            } else if (e->outcome != PKG_OUTCOME_NONE) {
                return stream_bad("an outcome before the end", i);
            }
            break;
        default:
            return stream_bad("an unknown type", i);
        }
    }
    return begins == ends;
}

/* ---- questions about the stream --------------------------------------- */

static const struct pkg_event *op_end(void)
{
    return nev > 0 && ev[nev - 1].e.type == PKG_EVENT_OP_END ? &ev[nev - 1].e : NULL;
}

static int steps(int phase)
{
    int i, n = 0;
    for (i = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_BEGIN && ev[i].e.phase == phase) n++;
    return n;
}

static int count_type(int type)
{
    int i, n = 0;
    for (i = 0; i < nev; i++)
        if (ev[i].e.type == type) n++;
    return n;
}

/* The end of the nth step of a phase, optionally of one object kind. */
static const struct pkg_event *end_of(int phase, int kind, int nth)
{
    int i;
    for (i = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_END && ev[i].e.phase == phase
            && (kind < 0 || ev[i].e.object_kind == kind) && nth-- == 0)
            return &ev[i].e;
    return NULL;
}

static int all_ok(void)
{
    int i;
    for (i = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_END && ev[i].e.outcome != PKG_OUTCOME_OK) return 0;
    return 1;
}

/* Every step ok, or a file a channel need not have (a withdrawal). */
static int none_failed(void)
{
    int i;
    for (i = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_END && ev[i].e.outcome != PKG_OUTCOME_OK
            && ev[i].e.outcome != PKG_OUTCOME_ABSENT) return 0;
    return 1;
}

/* "resolve verify fetch verify / verify inspect ...": the phases begun, a
 * slash where the nesting comes back to the top. */
static void phases(char *out, size_t ol)
{
    int i;
    size_t at = 0;
    out[0] = '\0';
    for (i = 0; i < nev && at + 12 < ol; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_BEGIN)
            at += (size_t)snprintf(out + at, ol - at, "%s%s", at ? " " : "", pkg_phase_name(ev[i].e.phase));
}

static int has_phases(const char *want)
{
    char got[4096];
    phases(got, sizeof got);
    if (strcmp(got, want) == 0) return 1;
    printf("    phases: %s\n    wanted: %s\n", got, want);
    return 0;
}

/* ---- setting up -------------------------------------------------------- */

static char top[600];

static void path_of(char *out, size_t ol, const char *rel)
{
    snprintf(out, ol, "%s/%s", top, rel);
}

static void mkdirs(const char *path)
{
    char p[900], *s;
    snprintf(p, sizeof p, "%s", path);
    for (s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text)
{
    char p[900], *slash;
    FILE *f;
    path_of(p, sizeof p, rel);
    slash = strrchr(p, '/');
    *slash = '\0';
    mkdirs(p);
    *slash = '/';
    f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int exists(const char *rel)
{
    char p[900];
    struct stat st;
    path_of(p, sizeof p, rel);
    return stat(p, &st) == 0;
}

static long long size_of(const char *rel)
{
    char p[900];
    struct stat st;
    path_of(p, sizeof p, rel);
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

static int sh(const char *fmt, const char *a, const char *b)
{
    char cmd[2400];
    snprintf(cmd, sizeof cmd, fmt, a, b);
    return system(cmd);
}

static int publish(const char *drawer, const char *channel, const char *name, const char *version,
                   const char *depends)
{
    struct pkg_sink s;
    struct pkg_options o;
    char d[900], c[900], k[900];
    path_of(d, sizeof d, drawer);
    path_of(c, sizeof c, channel);
    path_of(k, sizeof k, "key");
    memset(&s, 0, sizeof s);
    s.structured = 1;
    memset(&o, 0, sizeof o);
    o.target = d; o.channel = c; o.sign = k;
    o.name = name; o.version = version; o.kind = "data"; o.depends = depends;
    return pkg_publish(&s, &o);
}

/* The first object of a channel with that extension. */
static int object_with(const char *channel, const char *ext, char *rel, size_t rl)
{
    char dir[900];
    DIR *d;
    struct dirent *de;
    snprintf(rel, rl, "%s/objects", channel);
    path_of(dir, sizeof dir, rel);
    d = opendir(dir);
    if (d == NULL) return 0;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name), el = strlen(ext);
        if (n > el && strcmp(de->d_name + n - el, ext) == 0) {
            snprintf(rel, rl, "%s/objects/%s", channel, de->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static void flip_byte(const char *rel)
{
    char p[900];
    FILE *f;
    int c;
    path_of(p, sizeof p, rel);
    f = fopen(p, "r+b");
    if (f == NULL) return;
    fseek(f, 20, SEEK_SET);
    c = fgetc(f);
    fseek(f, 20, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
}

static struct pkg_options opts(const char *target, const char *root, const char *channel)
{
    static char r[900], c[900];
    struct pkg_options o;
    memset(&o, 0, sizeof o);
    o.target = target;
    if (root) { path_of(r, sizeof r, root); o.root = r; }
    if (channel) {
        if (strncmp(channel, "http", 4) == 0) snprintf(c, sizeof c, "%s", channel);
        else path_of(c, sizeof c, channel);
        o.channel = c;
    }
    return o;
}

/* ---- the cases --------------------------------------------------------- */

static void install_and_upgrade(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    int rc;

    printf("install and upgrade\n");
    reset();
    o = opts("hello", "r1", "ch");
    o.version = "1.0";
    rc = pkg_install(&s, &o);
    ok(rc == 0 && stream_ok(), "INSTALL: a whole, well-formed stream");
    ok(has_phases("resolve verify fetch verify verify inspect prepare place record"),
       "INSTALL into a new root: resolution with the manifest and payload checked, then the "
       "payload's files, the root, staging, placing and the records, in that order");
    ok(all_ok(), "every step ended ok");
    e = end_of(PKG_PHASE_PLACE, -1, 0);
    ok(e && e->total_known && e->done == 3 && e->total == 3 && rec_field("files")
       && strcmp(rec_field("files"), "3") == 0, "PLACE counted the 3 files the result says were placed");
    e = end_of(PKG_PHASE_FETCH, PKG_OBJECT_PAYLOAD, 0);
    ok(e && e->unit == PKG_UNIT_BYTES && e->measure == PKG_MEASURE_BYTES_READ && e->total_known
       && e->done == e->total && e->done > 0, "the payload read is counted in bytes against its size");
    e = end_of(PKG_PHASE_VERIFY, PKG_OBJECT_MANIFEST, 0);
    ok(e && e->parent != 0 && e->package && strcmp(e->package, "hello") == 0
       && e->version && strcmp(e->version, "1.0") == 0, "the manifest's check names its package, inside resolution");
    e = op_end();
    ok(e && e->code == 0 && e->outcome == PKG_OUTCOME_OK && e->restore == PKG_RESTORE_NONE && !e->cancel,
       "OP_END: code 0, ok, nothing to restore");
    ok(exists("r1/C/Hello") && exists("r1/C/Old"), "and the files are on disk");

    reset();
    o = opts("hello", "r1", "ch");
    rc = pkg_upgrade(&s, &o);
    ok(rc == 0 && stream_ok(), "UPGRADE: a whole, well-formed stream");
    ok(has_phases("resolve verify fetch verify verify inspect prepare place remove record flush"),
       "UPGRADE in an existing root: the old version's dropped file removed, the records, and the "
       "write-back to the medium last");
    e = end_of(PKG_PHASE_REMOVE, -1, 0);
    ok(e && e->measure == PKG_MEASURE_FILES_PROCESSED && e->done == 1 && e->total == 1
       && e->version && strcmp(e->version, "1.0") == 0, "REMOVE dealt with the one file 1.1 drops, of 1.0");
    e = end_of(PKG_PHASE_PLACE, -1, 0);
    ok(e && e->done == 1 && e->total == 1 && strcmp(rec_field("placed"), "1") == 0,
       "PLACE counted only the file that changed, as the result does");
    e = end_of(PKG_PHASE_FLUSH, PKG_OBJECT_ROOT, 0);
    ok(e && e->outcome == PKG_OUTCOME_OK && strcmp(rec_field("flushed"), "no") != 0,
       "FLUSH ended ok, and the result says the change was written back");
    ok(!exists("r1/C/Old") && size_of("r1/C/Hello") == 10, "and the disk holds 1.1");

    printf("nothing to do\n");
    reset();
    rc = pkg_upgrade(&s, &o);
    ok(rc == 0 && stream_ok() && steps(PKG_PHASE_PLACE) == 0 && steps(PKG_PHASE_PREPARE) == 0
       && strcmp(rec_field("result"), "unchanged") == 0, "an upgrade with nothing to do places nothing");

    printf("dry run\n");
    reset();
    o = opts("hello", "r2", "ch");
    o.dryrun = 1;
    rc = pkg_install(&s, &o);
    ok(rc == 0 && stream_ok() && ev[0].e.dryrun && op_end()->dryrun, "a dry run says so, first and last");
    ok(steps(PKG_PHASE_PREPARE) + steps(PKG_PHASE_PLACE) + steps(PKG_PHASE_RECORD)
       + steps(PKG_PHASE_FLUSH) == 0 && steps(PKG_PHASE_INSPECT) == 1,
       "and announces no phase that writes");
    ok(!exists("r2/C/Hello"), "nor writes");
}

static void refusals(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    char rel[600];
    int rc;

    printf("integrity\n");
    reset();
    o = opts("hello", "r3", "chbadpay");
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_VERIFY, PKG_OBJECT_PAYLOAD, 0);
    ok(rc == 12 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->code == 12,
       "a payload that is not the one signed: its check fails with 12");
    ok(op_end()->code == 12 && op_end()->outcome == PKG_OUTCOME_FAILED && steps(PKG_PHASE_PLACE) == 0
       && !exists("r3/C/Hello"), "the operation ends 12, nothing placed");
    e = end_of(PKG_PHASE_RESOLVE, -1, 0);
    ok(e && e->outcome == PKG_OUTCOME_FAILED, "and the resolution around it failed too");

    reset();
    o = opts("hello", "r3", "chbadman");
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_VERIFY, PKG_OBJECT_MANIFEST, 0);
    ok(rc == 12 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->code == 12
       && steps(PKG_PHASE_FETCH) == 0, "a manifest that is not the one indexed fails its check, nothing read after");

    printf("read error\n");
    reset();
    o = opts("hello", "r3", "chnopay");
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_FETCH, PKG_OBJECT_PAYLOAD, 0);
    ok(rc == 11 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->code == 11
       && !e->total_known, "a payload that is not there: the read fails, 11, and no size was invented");

    printf("policy\n");
    reset();
    o = opts("hello", "r1", "ch");
    o.version = "1.0";
    rc = pkg_upgrade(&s, &o);
    e = end_of(PKG_PHASE_RESOLVE, -1, 0);
    ok(rc == 18 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->code == 18
       && op_end()->code == 18 && steps(PKG_PHASE_PLACE) == 0, "a downgrade not asked for: 18, nothing placed");

    printf("arguments\n");
    reset();
    o = opts(NULL, NULL, "ch");
    rc = pkg_install(&s, &o);
    ok(rc == 20 && stream_ok() && nev == 2 && op_end()->code == 20, "INSTALL with no name: begun and ended, 20");
    reset();
    o = opts("hel\nlo", NULL, "ch");
    rc = pkg_install(&s, &o);
    ok(rc == 20 && stream_ok() && nev == 2 && op_end()->code == 20, "a line break in a name: 20");
    reset();
    rc = pkg_list(&s, NULL);
    ok(rc == 20 && stream_ok() && nev == 2, "no options at all: 20");
    reset();
    rc = pkg_usage_error(&s, "install", "a front end found this wrong");
    ok(rc == 20 && stream_ok() && nev == 2 && strcmp(ev[0].e.verb, "install") == 0,
       "pkg_usage_error: its own operation, 20");
    (void)rel;
}

/* The case for restore: happ needs hlib. hlib goes in, then happ's file
 * cannot be placed, since C/ refuses writes. */
static char lock_dir[900];
static void lock_on_restore(const struct pkg_event *e)
{
    if (e->type == PKG_EVENT_STEP_BEGIN && e->phase == PKG_PHASE_RESTORE)
        chmod(lock_dir, 0555);
}

static void cancel_after_hlib(const struct pkg_event *e)
{
    if (e->type == PKG_EVENT_STEP_END && e->phase == PKG_PHASE_RECORD && e->package
        && strcmp(e->package, "hlib") == 0)
        cancel_now = 1;
}

static void restore_and_cancel(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    char dir[900];
    int rc;

    printf("write refused, restore done\n");
    reset();
    mkdirs((path_of(dir, sizeof dir, "r4/C"), dir));
    chmod(dir, 0555);
    o = opts("happ", "r4", "ch");
    rc = pkg_install(&s, &o);
    chmod(dir, 0755);
    e = end_of(PKG_PHASE_PLACE, -1, 1);
    ok(rc == 17 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->code == 17
       && e->package && strcmp(e->package, "happ") == 0 && e->done == 0,
       "happ's file cannot be placed: PLACE fails with 17, having placed nothing");
    e = end_of(PKG_PHASE_RESTORE, -1, 0);
    ok(e && e->outcome == PKG_OUTCOME_OK && e->unit == PKG_UNIT_PACKAGES && e->done == 1 && e->total == 1,
       "hlib, placed before, is taken back out: RESTORE ok, 1 of 1");
    ok(op_end()->code == 17 && op_end()->restore == PKG_RESTORE_DONE, "OP_END: 17, restore done");
    ok(!exists("r4/Libs/hlib.library") && !exists("r4/.pkg/keys/hlib") && !exists("r4/.pkg/db/hlib"),
       "and the disk agrees: no hlib, no record, no pinned key");

    printf("restore fails\n");
    reset();
    mkdirs((path_of(dir, sizeof dir, "r5/C"), dir));
    chmod(dir, 0555);
    path_of(lock_dir, sizeof lock_dir, "r5/Libs");
    on_each = lock_on_restore;
    o = opts("happ", "r5", "ch");
    rc = pkg_install(&s, &o);
    chmod(dir, 0755);
    chmod(lock_dir, 0755);
    e = end_of(PKG_PHASE_RESTORE, -1, 0);
    ok(rc == 17 && stream_ok() && e && e->outcome == PKG_OUTCOME_FAILED && e->done == 0,
       "hlib cannot be taken back out: RESTORE fails, none restored");
    ok(op_end()->code == 17 && op_end()->restore == PKG_RESTORE_FAILED,
       "OP_END keeps the first refusal's code and says the restore failed");
    ok(rec_field("warning") && strstr(rec_field("warning"), "could not be taken back out") != NULL,
       "and a warning says what stayed");
    ok(exists("r5/Libs/hlib.library"), "which the disk confirms: nothing claimed that did not happen");

    printf("cancel\n");
    reset();
    on_each = cancel_after_hlib;
    o = opts("happ", "r6", "ch");
    rc = pkg_install(&s, &o);
    e = op_end();
    ok(rc == 10 && stream_ok() && e->outcome == PKG_OUTCOME_CANCELLED && e->cancel && e->code == 10,
       "Cancel once hlib is placed: stopped before happ, 10, cancelled");
    ok(e->restore == PKG_RESTORE_DONE && end_of(PKG_PHASE_RESTORE, -1, 0)
       && end_of(PKG_PHASE_RESTORE, -1, 0)->outcome == PKG_OUTCOME_OK, "with hlib taken back out");
    ok(steps(PKG_PHASE_PLACE) == 1 && !exists("r6/Libs/hlib.library"),
       "happ was never placed, and hlib is gone");

    reset();
    cancel_now = 1;
    o = opts("happ", "r6", "ch");
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_RESOLVE, -1, 0);
    ok(rc == 10 && stream_ok() && e && e->outcome == PKG_OUTCOME_CANCELLED
       && op_end()->restore == PKG_RESTORE_NONE, "Cancel while resolving: the resolution ends cancelled, nothing to restore");
}

static void forms(void)
{
    struct pkg_sink s;
    struct pkg_options o;
    char a[65536], first[4096], second[4096];
    char *p;
    int rc1, rc2;

    printf("forms\n");
    /* the same install, without and with the callback: the same records */
    reset();
    s = sink_for(1);
    s.event = NULL;
    o = opts("hello", "r7", "ch");
    rc1 = pkg_install(&s, &o);
    snprintf(a, sizeof a, "%s", recs);
    ok(nev == 0, "no callback, no event");
    reset();
    s = sink_for(1);
    o = opts("hello", "r8", "ch");
    rc2 = pkg_install(&s, &o);
    while ((p = strstr(a, "/r7")) != NULL) p[2] = '8';
    ok(rc1 == 0 && rc2 == 0 && strcmp(a, recs) == 0, "the records are the same with and without events");
    phases(first, sizeof first);

    /* text, no progress */
    reset();
    s = sink_for(0);
    o = opts("hello", "r9", "ch");
    rc1 = pkg_install(&s, &o);
    phases(second, sizeof second);
    ok(rc1 == 0 && stream_ok() && texts > 0 && nrecs == 0 && strcmp(first, second) == 0,
       "the text form has the same steps, and no record");

    /* text with progress, through `line` */
    reset();
    s = sink_for(0);
    s.text = NULL;
    s.line = record_line;
    s.progress = 1;
    o = opts("many", "r9", "ch");
    rc1 = pkg_install(&s, &o);
    ok(rc1 == 0 && stream_ok() && progress_lines > 0, "progress on: activity lines, and the events too");

    /* progress on, no callback: the activity line still comes from the events */
    reset();
    s.event = NULL;
    o = opts("many", "r10", "ch");
    rc1 = pkg_install(&s, &o);
    ok(rc1 == 0 && nev == 0 && progress_lines > 0, "progress on without a callback: the line is still drawn");

    /* structured with progress set: nothing drawn, events sent */
    reset();
    s = sink_for(1);
    s.line = record_line;
    s.progress = 1;
    o = opts("many", "r11", "ch");
    rc1 = pkg_install(&s, &o);
    ok(rc1 == 0 && stream_ok() && progress_lines == 0 && nev > 0, "structured: events, never a drawn line");
}

static struct pkg_sink *inner_sink;
static int inner_rc = -1;
static void call_from_callback(const struct pkg_event *e)
{
    if (e->type == PKG_EVENT_STEP_BEGIN && inner_rc < 0) {
        struct pkg_options o;
        memset(&o, 0, sizeof o);
        o.root = "/nonexistent";
        inner_rc = pkg_list(inner_sink, &o);
    }
}

static int inner_events, inner_code, inner_seen_op;
static void inner_event(void *user, const struct pkg_event *e)
{
    (void)user;
    inner_events++;
    if (e->type == PKG_EVENT_OP_END) inner_code = e->code;
    inner_seen_op = (int)e->op;
}

static void calls(void)
{
    struct pkg_sink s = sink_for(1), other;
    struct pkg_options o;
    unsigned long op1;
    int rc;

    printf("successive calls\n");
    reset();
    o = opts(NULL, "r1", NULL);
    rc = pkg_list(&s, &o);
    op1 = ev[0].e.op;
    ok(rc == 0 && stream_ok(), "LIST: begun and ended");
    reset();
    memset(&other, 0, sizeof other);
    other.structured = 1;
    other.event = inner_event;
    inner_events = 0;
    rc = pkg_list(&other, &o);
    ok(rc == 0 && nev == 0 && inner_events == 2 && (unsigned long)inner_seen_op == op1 + 1,
       "the next call: its own sink only, the next operation number");
    reset();
    o = opts("hello", "r12", "ch");
    rc = pkg_install(&s, &o);
    ok(rc == 0 && stream_ok() && ev[0].e.op == op1 + 2 && ev[1].e.step == 1,
       "and the one after: steps numbered from 1 again");

    printf("re-entry\n");
    reset();
    inner_events = 0;
    inner_code = 0;
    inner_rc = -1;
    inner_sink = &other;
    on_each = call_from_callback;
    o = opts("hello", "r13", "ch");
    rc = pkg_install(&s, &o);
    ok(inner_rc == 20 && inner_events == 2 && inner_code == 20,
       "an operation called from a callback is refused, 20, on its own sink");
    ok(rc == 0 && stream_ok() && exists("r13/C/Hello"), "and the operation it interrupted went on unharmed");
}

static void network(const char *http)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    char url[900];
    int rc, i, n, waited, order_ok;

    printf("network, known lengths\n");
    reset();
    snprintf(url, sizeof url, "%s/known/ch", http);
    o = opts("happ", "n1", url);
    rc = pkg_install(&s, &o);
    ok(rc == 0 && stream_ok() && none_failed(),
       "happ and hlib over HTTP: a whole stream, every step ok (or absent: no withdrawal published)");
    for (i = 0, n = 0, order_ok = 1; i < nev; i++) {
        e = &ev[i].e;
        if (e->type == PKG_EVENT_STEP_END && e->measure == PKG_MEASURE_BYTES_TRANSFERRED
            && e->outcome == PKG_OUTCOME_OK) {
            n++;
            if (!e->total_known || e->done != e->total) order_ok = 0;
        }
    }
    ok(n >= 7 && order_ok, "every download knew its length and reached it (index, manifests, signatures, payloads)");
    for (i = 0, n = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_BEGIN && ev[i].e.phase == PKG_PHASE_FETCH
            && ev[i].e.object_kind == PKG_OBJECT_PAYLOAD && ev[i].e.measure == PKG_MEASURE_BYTES_TRANSFERRED
            && ev[i].e.package != NULL)
            n++;
    ok(n == 2, "two payloads downloaded, each naming its package");

    printf("network, unknown lengths\n");
    reset();
    snprintf(url, sizeof url, "%s/chunked/ch", http);
    o = opts("many", "n2", url);
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0, order_ok = 1; i < nev; i++) {
        e = &ev[i].e;
        if (e->measure == PKG_MEASURE_BYTES_TRANSFERRED && e->type != PKG_EVENT_STEP_WAIT) {
            if (e->total_known) order_ok = 0;
            if (e->type == PKG_EVENT_STEP_END && e->object_kind == PKG_OBJECT_PAYLOAD) n = (int)e->done;
        }
    }
    ok(rc == 0 && stream_ok() && order_ok, "chunked replies: no total ever claimed");
    ok(n > 4096, "and the payload's bytes are all counted all the same");

    printf("network, cache\n");
    reset();
    snprintf(url, sizeof url, "%s/known/ch", http);
    o = opts("happ", "n3", url);
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_BEGIN && ev[i].e.measure == PKG_MEASURE_BYTES_TRANSFERRED
            && (ev[i].e.object_kind == PKG_OBJECT_MANIFEST || ev[i].e.object_kind == PKG_OBJECT_PAYLOAD))
            n++;
    ok(rc == 0 && stream_ok() && n == 0 && steps(PKG_PHASE_FETCH) > 0,
       "a second install from the same channel downloads no manifest and no payload: they are read from the cache");

    printf("network, an empty file\n");
    reset();
    snprintf(url, sizeof url, "%s/known/empty", http);
    o = opts("hello", "n4", url);
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_FETCH, PKG_OBJECT_INDEX, 0);
    ok(rc == 11 && stream_ok() && e && e->outcome == PKG_OUTCOME_OK && e->total_known && e->total == 0
       && e->done == 0, "an empty index: a known total of 0, reached, then not found (11)");

    printf("network, absent\n");
    reset();
    snprintf(url, sizeof url, "%s/known/nowhere", http);
    o = opts("hello", "n5", url);
    rc = pkg_install(&s, &o);
    e = end_of(PKG_PHASE_FETCH, PKG_OBJECT_INDEX, 0);
    ok(rc == 11 && stream_ok() && e && e->outcome == PKG_OUTCOME_ABSENT,
       "a channel with no index: the download ends absent, the operation not found");

    printf("network, a slow server and a frozen clock\n");
    reset();
    s.clock = test_clock;            /* never moves: only what must be sent is sent */
    snprintf(url, sizeof url, "%s/slow/ch", http);
    o = opts("many", "n6", url);
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0, waited = 0, order_ok = 1; i < nev; i++) {
        e = &ev[i].e;
        if (e->measure != PKG_MEASURE_BYTES_TRANSFERRED) continue;
        if (e->type == PKG_EVENT_STEP_BEGIN) waited = 0;
        if (e->type == PKG_EVENT_STEP_WAIT && e->wait == PKG_WAIT_NETWORK) waited = 1;
        if (e->type == PKG_EVENT_STEP_PROGRESS && e->done > 0 && !waited) order_ok = 0;
        if (e->type == PKG_EVENT_STEP_PROGRESS) n++;
    }
    ok(rc == 0 && stream_ok() && order_ok && count_type(PKG_EVENT_STEP_WAIT) > 0,
       "each download says it waits for the server before any byte arrives");
    ok(n <= 2 * steps(PKG_PHASE_FETCH), "with the clock frozen, a download sends no more than its total arriving");
    for (i = 0, order_ok = 1; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_WAIT && ev[i].e.wait
            && (ev[i].e.object == NULL || strcmp(ev[i].e.object, "127.0.0.1") != 0))
            order_ok = 0;
    ok(order_ok, "and names the host it waits for");
}

static void pace(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    int rc, i, n;

    printf("pace\n");
    reset();
    s.clock = test_clock;            /* frozen */
    o = opts("many", "p1", "ch");
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_PROGRESS && ev[i].e.unit == PKG_UNIT_FILES) n++;
    e = end_of(PKG_PHASE_PLACE, -1, 0);
    ok(rc == 0 && stream_ok() && n == 0 && e && e->done == 300 && e->total == 300,
       "300 files with the clock frozen: no update at all, and the end still says 300 of 300");

    reset();
    s.clock = test_clock;
    clock_step = PKG_EVENT_GAP_MS;   /* every reading a full gap later */
    o = opts("many", "p2", "ch");
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_PROGRESS && ev[i].e.phase == PKG_PHASE_PLACE) n++;
    ok(rc == 0 && stream_ok() && n >= 299, "with the clock running a gap per reading, PLACE reports every file");

    reset();
    s.clock = test_clock;
    clock_step = PKG_EVENT_GAP_MS / 10;
    o = opts("many", "p3", "ch");
    rc = pkg_install(&s, &o);
    for (i = 0, n = 0; i < nev; i++)
        if (ev[i].e.type == PKG_EVENT_STEP_PROGRESS && ev[i].e.phase == PKG_PHASE_PLACE) n++;
    ok(rc == 0 && stream_ok() && n >= 20 && n <= 40, "and a tenth of a gap per reading: about one in ten");
}

/* The block map of an archive turns out not to serve: the read starts over,
 * as attempt 2 of the same step. */
static void archive_retry(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    char map[900], ap[900];
    FILE *f;
    int rc, i, retried = 0, second = 0;

    printf("archive retry\n");
    path_of(ap, sizeof ap, "chsrc/archives/src.tar.bz2");
    path_of(map, sizeof map, "chsrc/archives/src.tar.bz2.pkgmap");
    reset();
    o = opts("srcpkg", "a1", "chsrc");
    rc = pkg_install(&s, &o);
    ok(rc == 0 && stream_ok() && end_of(PKG_PHASE_FETCH, PKG_OBJECT_ARCHIVE, 0) != NULL,
       "a package from an archive: the archive read is a FETCH step");
    f = fopen(map, "rb");
    if (f != NULL) {                 /* keep its header, spoil the rest */
        char line[300];
        if (fgets(line, sizeof line, f) == NULL) line[0] = '\0';
        fclose(f);
        f = fopen(map, "wb");
        if (f != NULL) {
            fputs(line, f);
            fputs("garbage that maps nothing\n", f);
            fclose(f);
        }
    }
    reset();
    o = opts("srcpkg", "a2", "chsrc");
    rc = pkg_install(&s, &o);
    for (i = 0; i < nev; i++) {
        e = &ev[i].e;
        if (e->type == PKG_EVENT_STEP_END && e->object_kind == PKG_OBJECT_ARCHIVE
            && e->outcome == PKG_OUTCOME_RETRIED) retried++;
        if (e->type == PKG_EVENT_STEP_BEGIN && e->object_kind == PKG_OBJECT_ARCHIVE && e->attempt == 2) second++;
    }
    ok(rc == 0 && stream_ok() && retried == 1 && second == 1,
       "a block map that does not serve: that read ends RETRIED, and attempt 2 reads it whole");
    (void)ap;
}

/* The transfer layer fed by hand, through the same hooks the network and
 * the archive reader call: a white-box check, with libpkg's internal names. */
extern const struct pkg_sink *pkgi_sink;
void pkgi_ev_op_begin(const char *verb, int draw_line);
int  pkgi_ev_op_end(int rc);
void pkgi_ev_begin(int phase, int kind, const char *object, const char *package,
                   const char *version, int measure, unsigned long long total, int known);
void pkgi_ev_end(int outcome);
void pkgi_on_transfer(long long done, long long total);
void pkgi_on_wait(const char *host);

static void simulated(void)
{
    struct pkg_sink s = sink_for(1);
    const unsigned long long six = 6ull << 30, five = 5ull << 30;
    const struct pkg_event *e;
    int i, n, waits;

    printf("simulated backend\n");
    reset();
    s.clock = test_clock;
    clock_step = 1;
    pkgi_sink = &s;
    pkgi_ev_op_begin("simulated", 0);
    pkgi_ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_PAYLOAD, "https://joe:secret@mirror.example/big.pkg?token=abc",
                  NULL, NULL, PKG_MEASURE_BYTES_TRANSFERRED, 0, 0);
    pkgi_on_wait("joe:secret@mirror.example");
    pkgi_on_wait("joe:secret@mirror.example");
    pkgi_on_wait("joe:secret@mirror.example");
    pkgi_on_transfer(0, (long long)six);
    for (i = 1; i <= 6000; i++)
        pkgi_on_transfer((long long)(six / 6000 * (unsigned long long)i), (long long)six);
    pkgi_on_transfer((long long)six, (long long)six);
    pkgi_ev_end(PKG_OUTCOME_OK);

    pkgi_ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_PAYLOAD, "again.pkg", NULL, NULL,
                  PKG_MEASURE_BYTES_TRANSFERRED, 0, 0);
    pkgi_on_transfer((long long)five, (long long)six);
    pkgi_on_transfer(1ll << 30, (long long)six);      /* started over */
    pkgi_on_transfer((long long)six, (long long)six);
    pkgi_ev_end(PKG_OUTCOME_OK);

    pkgi_ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_FILE, "liar", NULL, NULL,
                  PKG_MEASURE_BYTES_TRANSFERRED, 0, 0);
    pkgi_on_transfer(10, -1);
    pkgi_on_transfer(20, 100);                       /* the whole becomes known */
    pkgi_on_transfer(200, 100);                      /* and turns out wrong */
    pkgi_on_transfer(300, 100);
    pkgi_ev_end(PKG_OUTCOME_OK);

    pkgi_ev_begin(PKG_PHASE_FETCH, PKG_OBJECT_FILE, "changed", NULL, NULL,
                  PKG_MEASURE_BYTES_TRANSFERRED, 0, 0);
    pkgi_on_transfer(10, 100);
    pkgi_on_transfer(20, 150);                       /* another whole */
    pkgi_ev_end(PKG_OUTCOME_OK);
    pkgi_ev_op_end(0);
    pkgi_sink = NULL;

    ok(stream_ok(), "a whole, well-formed stream");
    e = end_of(PKG_PHASE_FETCH, -1, 0);
    ok(sizeof e->done >= 8 && e->done == six && e->total == six && e->total_known,
       "6 GiB of 6 GiB, to the byte: nothing truncated at 4 GiB");
    ok(e->object && strstr(e->object, "secret") == NULL && strstr(e->object, "joe") == NULL
       && strstr(e->object, "token") == NULL && strstr(e->object, "big.pkg") != NULL,
       "the object keeps the file's name, never the user, the password or the query");
    for (i = 0, n = 0, waits = 0; i < nev; i++) {
        if (ev[i].e.step != 1) continue;
        if (ev[i].e.type == PKG_EVENT_STEP_PROGRESS) n++;
        if (ev[i].e.type == PKG_EVENT_STEP_WAIT) {
            waits++;
            if (ev[i].e.wait && (ev[i].e.object == NULL || strcmp(ev[i].e.object, "mirror.example") != 0))
                waits += 100;
        }
    }
    ok(waits == 2, "a wait said once however often it is reported, its host without credentials, and its end");
    ok(n >= 55 && n <= 65, "6000 updates a millisecond apart: about one per gap, 60");
    e = end_of(PKG_PHASE_FETCH, -1, 1);
    ok(e && e->outcome == PKG_OUTCOME_RETRIED && e->done == five && e->attempt == 1,
       "a transfer that starts over ends its attempt RETRIED at what it had");
    e = end_of(PKG_PHASE_FETCH, -1, 2);
    ok(e && e->outcome == PKG_OUTCOME_OK && e->attempt == 2 && e->done == six
       && e->object && strcmp(e->object, "again.pkg") == 0, "and attempt 2 of the same object runs to the end");
    e = end_of(PKG_PHASE_FETCH, -1, 3);
    ok(e && !e->total_known && e->done == 300, "a total passed: unknown from then on, never above 100%");
    e = end_of(PKG_PHASE_FETCH, -1, 4);
    ok(e && !e->total_known && e->done == 20, "a total that changes: unknown from then on");
    ok(strcmp(pkg_phase_name(PKG_PHASE_UPLOAD), "upload") == 0 && strcmp(pkg_phase_name(99), "unknown") == 0
       && strcmp(pkg_measure_name(PKG_MEASURE_PACKAGES_RESTORED), "packages-restored") == 0
       && strcmp(pkg_outcome_name(PKG_OUTCOME_ABANDONED), "abandoned") == 0,
       "every value has a name, and a value from a later library is 'unknown'");
}

static void other_verbs(void)
{
    struct pkg_sink s = sink_for(1);
    struct pkg_options o;
    const struct pkg_event *e;
    char p[900];
    int rc;

    printf("rollback, verify, repair, remove, upgrade all, show\n");
    reset();
    o = opts("hello", "v1", "ch");
    o.version = "1.0";
    rc = pkg_install(&s, &o);
    reset();
    o = opts("hello", "v1", "ch");
    rc = pkg_upgrade(&s, &o);
    reset();
    o = opts("hello", "v1", "ch");
    rc = pkg_rollback(&s, &o);
    ok(rc == 0 && stream_ok() && all_ok()
       && has_phases("resolve verify fetch verify verify inspect prepare place record flush"),
       "ROLLBACK: the same steps as an upgrade, and 1.1 dropped no file of 1.0's");
    ok(op_end()->code == 0 && exists("v1/C/Old"), "and 1.0 is back on disk");

    reset();
    o = opts("hello", "v1", NULL);
    rc = pkg_verify(&s, &o);
    e = end_of(PKG_PHASE_INSPECT, -1, 0);
    ok(rc == 0 && stream_ok() && has_phases("inspect") && e && e->measure == PKG_MEASURE_FILES_CHECKED
       && e->done == 3 && e->total == 3, "VERIFY: one inspection, 3 files of 3");

    path_of(p, sizeof p, "v1/C/Old");
    unlink(p);
    reset();
    rc = pkg_verify(&s, &o);
    e = end_of(PKG_PHASE_INSPECT, -1, 0);
    ok(rc == 12 && stream_ok() && e && e->outcome == PKG_OUTCOME_OK && op_end()->code == 12,
       "VERIFY of a damaged package: the inspection was made (ok), the answer is 12");
    ok(steps(PKG_PHASE_INSPECT) == 2, "and a second inspection, of the whole root, looked for the file moved");

    reset();
    o = opts("hello", "v1", "ch");
    rc = pkg_repair(&s, &o);
    e = end_of(PKG_PHASE_PLACE, -1, 0);
    ok(rc == 0 && stream_ok() && e && e->measure == PKG_MEASURE_FILES_WRITTEN && e->done == 1 && e->total == 1
       && exists("v1/C/Old"), "REPAIR: the missing file written back, 1 of 1");

    reset();
    o = opts(NULL, "v1", "ch");
    o.all = 1;
    rc = pkg_upgrade(&s, &o);
    ok(rc == 0 && stream_ok() && steps(PKG_PHASE_RESOLVE) == 2 && end_of(PKG_PHASE_RESOLVE, -1, 1)->package
       && strcmp(end_of(PKG_PHASE_RESOLVE, -1, 1)->package, "hello") == 0 && steps(PKG_PHASE_PLACE) == 1,
       "UPGRADE ALL: the root resolved, then hello on its own, then placed");

    reset();
    o = opts("hello", "v1", NULL);
    rc = pkg_remove(&s, &o);
    e = end_of(PKG_PHASE_REMOVE, -1, 0);
    ok(rc == 0 && stream_ok() && has_phases("remove record flush") && e && e->done == 2 && e->total == 2
       && !exists("v1/C/Hello"), "REMOVE: its files dealt with, 2 of 2, the records, the write-back");

    reset();
    o = opts(NULL, NULL, "ch");
    rc = pkg_show(&s, &o);
    e = end_of(PKG_PHASE_VERIFY, PKG_OBJECT_CHANNEL, 0);
    ok(rc == 0 && stream_ok() && e && e->measure == PKG_MEASURE_ENTRIES_CHECKED && e->done == 5 && e->total == 5,
       "SHOW: the channel's 5 entries checked, each with its own manifest and payload steps inside");
}

/* ---- the scene --------------------------------------------------------- */

static int scene(void)
{
    struct pkg_sink s;
    struct pkg_options o;
    char k[900], rel[600], a[900], b[900];
    int i;

    memset(&s, 0, sizeof s);
    s.structured = 1;
    memset(&o, 0, sizeof o);
    path_of(k, sizeof k, "key");
    o.file = k;
    if (pkg_keygen(&s, &o) != 0) return __LINE__;
    put("d/hello10/C/Hello", "hello 1.0\n");
    put("d/hello10/C/Hello.doc", "doc\n");
    put("d/hello10/C/Old", "old\n");
    put("d/hello11/C/Hello", "hello 1.1\n");
    put("d/hello11/C/Hello.doc", "doc\n");
    put("d/hlib/Libs/hlib.library", "hlib\n");
    put("d/happ/C/Happ", "happ\n");
    for (i = 0; i < 300; i++) {
        char f[64];
        snprintf(f, sizeof f, "d/many/Many/f%03d", i);
        put(f, "one of many files, long enough to be a few bytes\n");
    }
    if (publish("d/hello10", "ch", "hello", "1.0", NULL) || publish("d/hello11", "ch", "hello", "1.1", NULL)
        || publish("d/hlib", "ch", "hlib", "1.0", NULL) || publish("d/happ", "ch", "happ", "1.0", "hlib >= 1.0")
        || publish("d/many", "ch", "many", "1.0", NULL))
        return __LINE__;
    /* damaged copies */
    if (publish("d/hello10", "chbadpay", "hello", "1.0", NULL) || !object_with("chbadpay", ".pkg", rel, sizeof rel))
        return __LINE__;
    flip_byte(rel);
    if (publish("d/hello10", "chbadman", "hello", "1.0", NULL) || !object_with("chbadman", ".manifest", rel, sizeof rel))
        return __LINE__;
    flip_byte(rel);
    if (publish("d/hello10", "chnopay", "hello", "1.0", NULL) || !object_with("chnopay", ".pkg", rel, sizeof rel))
        return __LINE__;
    path_of(a, sizeof a, rel);
    unlink(a);
    /* a package whose files are in an archive */
    put("srcdir/Top/S/Startup", "startup\n");
    put("srcdir/Top/S/Other", "other\n");
    path_of(a, sizeof a, "srcdir");
    path_of(b, sizeof b, "chsrc/archives");
    mkdirs(b);
    path_of(b, sizeof b, "chsrc/archives/src.tar.bz2");
    if (sh("cd '%s' && COPYFILE_DISABLE=1 tar cjf '%s' Top", a, b) != 0) return __LINE__;
    {
        char from[900];
        path_of(from, sizeof from, "chsrc/archives/src.tar.bz2");
        snprintf(rel, sizeof rel, "%s!/Top", from);
        memset(&o, 0, sizeof o);
        path_of(a, sizeof a, "chsrc");
        o.target = rel; o.channel = a; o.sign = k;
        o.name = "srcpkg"; o.version = "1.0"; o.kind = "data";
        if (pkg_publish(&s, &o) != 0) return __LINE__;
    }
    /* what the server serves: the channel, and one with an empty index */
    path_of(a, sizeof a, "ch");
    path_of(b, sizeof b, "empty");
    if (sh("mkdir -p '%s' && : > '%s/index'", b, b) != 0) return __LINE__;
    return 0;
}

int main(void)
{
    const char *dir = getenv("EVENTS_DIR"), *http = getenv("EVENTS_HTTP");
    int line;
    if (dir == NULL || http == NULL) {
        printf("test_events: run by tests/events.sh, which serves EVENTS_DIR at EVENTS_HTTP\n");
        return 69;
    }
    snprintf(top, sizeof top, "%s", dir);
    if ((line = scene()) != 0) {
        printf("test_events: cannot set the scene up in %s (tests/test_events.c:%d)\n", top, line);
        return 1;
    }
    install_and_upgrade();
    refusals();
    restore_and_cancel();
    other_verbs();
    forms();
    calls();
    network(http);
    pace();
    archive_retry();
    simulated();
    printf("events: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

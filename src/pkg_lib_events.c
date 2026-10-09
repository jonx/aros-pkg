/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Progress events: what an operation is doing, as the facts pkg.h lists
 * under "Progress events". Every step an operation takes that a front end
 * may want to show begins, advances and ends here, and nowhere else.
 *
 * Pkg is its own first consumer: the activity line a person sees at a
 * terminal (pkg_activity.c, PKG_LINE_PROGRESS) is drawn from these same
 * events, so it can show nothing the events do not say.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* One open step. Its strings are copies, so an event can lend them to the
 * callback whatever the code that began the step has freed since. */
struct ev_step {
    unsigned long id, parent;
    unsigned attempt;
    int phase, kind, measure;
    char package[72], version[64], object[160];
    int has_package, has_version, has_object;
    unsigned long long done, total;
    int known;            /* total is meaningful */
    int invalid;          /* a total turned out wrong: unknown for the rest of the step */
    long long sent_at;    /* when the last event of this step left */
    int wait;             /* PKG_WAIT_*, while a wait is under way */
    char host[96];
    unsigned long cancels;/* the operation's cancels when the step began */
    int drawn;            /* the activity line shows it */
};

#define EV_DEPTH 32

static unsigned long op_seq;         /* the last number given to an operation */
static unsigned long op_id;          /* the running operation's */
static const char   *op_verb;
static int           listening;      /* a sink event callback, or the activity line */
static int           render;         /* the activity line draws from the events */
static unsigned long step_seq;
static struct ev_step stack[EV_DEPTH];
static int           depth, overflow;
static unsigned long cancels;        /* the caller's cancel said yes, this many times */
static int           restore_state;
/* The step that last ended RETRIED: the next to begin with the same phase
 * and object is its next attempt. */
static struct { int phase, kind; char object[160]; unsigned attempt; } retried;
static const char   *ctx_package, *ctx_version;

/* ---- names ----------------------------------------------------------- */

static const char *name_in(const char *const *names, size_t n, int v)
{
    return v >= 0 && (size_t)v < n && names[v] != NULL ? names[v] : "unknown";
}
#define NAME_IN(t, v) name_in((t), sizeof (t) / sizeof (t)[0], (v))

const char *pkg_event_name(int type)
{
    static const char *const t[] = { NULL, "op-begin", "step-begin", "step-progress",
                                     "step-wait", "step-end", "op-end" };
    return NAME_IN(t, type);
}

const char *pkg_phase_name(int phase)
{
    static const char *const t[] = { "none", "resolve", "fetch", "verify", "inspect", "prepare",
                                     "place", "remove", "record", "restore", "flush", "upload" };
    return NAME_IN(t, phase);
}

const char *pkg_unit_name(int unit)
{
    static const char *const t[] = { "none", "bytes", "files", "entries", "packages" };
    return NAME_IN(t, unit);
}

const char *pkg_measure_name(int measure)
{
    static const char *const t[] = { "none", "bytes-transferred", "bytes-read", "bytes-verified",
                                     "bytes-hashed", "bytes-sent", "files-verified",
                                     "files-checked", "files-written", "files-placed",
                                     "files-processed", "files-sent", "entries-checked",
                                     "packages-restored" };
    return NAME_IN(t, measure);
}

const char *pkg_object_name(int kind)
{
    static const char *const t[] = { "none", "index", "manifest", "signature", "payload",
                                     "archive", "channel", "file", "root", "host" };
    return NAME_IN(t, kind);
}

const char *pkg_wait_name(int wait)
{
    static const char *const t[] = { "none", "network", "other" };
    return NAME_IN(t, wait);
}

const char *pkg_outcome_name(int outcome)
{
    static const char *const t[] = { "none", "ok", "failed", "cancelled", "retried", "absent",
                                     "abandoned" };
    return NAME_IN(t, outcome);
}

const char *pkg_restore_name(int restore)
{
    static const char *const t[] = { "none", "done", "failed", "incomplete" };
    return NAME_IN(t, restore);
}

static int unit_of(int measure)
{
    switch (measure) {
    case PKG_MEASURE_BYTES_TRANSFERRED: case PKG_MEASURE_BYTES_READ:
    case PKG_MEASURE_BYTES_VERIFIED: case PKG_MEASURE_BYTES_HASHED: case PKG_MEASURE_BYTES_SENT:
        return PKG_UNIT_BYTES;
    case PKG_MEASURE_FILES_VERIFIED: case PKG_MEASURE_FILES_CHECKED: case PKG_MEASURE_FILES_WRITTEN:
    case PKG_MEASURE_FILES_PLACED: case PKG_MEASURE_FILES_PROCESSED: case PKG_MEASURE_FILES_SENT:
        return PKG_UNIT_FILES;
    case PKG_MEASURE_ENTRIES_CHECKED:
        return PKG_UNIT_ENTRIES;
    case PKG_MEASURE_PACKAGES_RESTORED:
        return PKG_UNIT_PACKAGES;
    default:
        return PKG_UNIT_NONE;
    }
}

/* ---- what may be said ------------------------------------------------- */

/* A name for an event: the part of a URL that names the file, never who
 * asked for it or what the query carried. "user:pw@host" keeps "host";
 * "file.pkg?token=x" keeps "file.pkg". Control characters become spaces. */
static void clean_copy(char *out, size_t ol, const char *in)
{
    const char *at, *end;
    size_t n;
    if (in == NULL) { out[0] = '\0'; return; }
    at = strstr(in, "://");
    if (at != NULL) in = at + 3;
    end = in + strcspn(in, "?#");
    for (at = in; at < end; at++)
        if (*at == '@') in = at + 1;
    n = (size_t)(end - in);
    if (n >= ol) n = ol - 1;
    memcpy(out, in, n);
    out[n] = '\0';
    one_line(out);
}

static long long now_ms(void)
{
    if (sink != NULL && sink->clock != NULL)
        return sink->clock(sink->user);
    return pkg_fs_now_ms();
}

/* ---- the activity line, drawn from the events ------------------------- */

/* The words the activity line says for a step, or NULL for a step it does
 * not show: it shows the steps that have something to count, as it always
 * has, and leaves the short ones to the result sentence. */
static const char *words_for(const struct pkg_event *e)
{
    switch (e->phase) {
    case PKG_PHASE_FETCH:
        if (e->measure == PKG_MEASURE_BYTES_TRANSFERRED) return "downloading";
        return e->object_kind == PKG_OBJECT_ARCHIVE ? "reading the archive" : NULL;
    case PKG_PHASE_VERIFY:
        return e->measure == PKG_MEASURE_ENTRIES_CHECKED ? "checking" : NULL;
    case PKG_PHASE_INSPECT:
        return e->measure == PKG_MEASURE_FILES_CHECKED ? "checking" : NULL;
    case PKG_PHASE_PREPARE:
        return e->measure == PKG_MEASURE_FILES_WRITTEN ? "writing"
             : e->measure == PKG_MEASURE_BYTES_HASHED ? "hashing" : NULL;
    case PKG_PHASE_PLACE:
        return e->measure == PKG_MEASURE_FILES_PLACED ? "placing" : NULL;
    case PKG_PHASE_UPLOAD:
        return "uploading";
    default:
        return NULL;
    }
}

static void draw(const struct pkg_event *e, struct ev_step *s)
{
    switch (e->type) {
    case PKG_EVENT_STEP_BEGIN: {
        const char *w = words_for(e);
        if (w == NULL) return;
        s->drawn = 1;
        pkg_activity_step(w, e->object ? e->object : e->package,
                          e->total_known ? (long long)e->total : 0,
                          e->unit == PKG_UNIT_BYTES ? PKG_ACTIVITY_BYTES : PKG_ACTIVITY_THINGS, NULL);
        return;
    }
    case PKG_EVENT_STEP_PROGRESS:
        if (!s->drawn) return;
        if (e->unit == PKG_UNIT_BYTES)
            pkg_activity_bytes((long long)e->done, e->total_known ? (long long)e->total : -1);
        else
            pkg_activity_count(e->done, e->total_known ? e->total : 0, NULL);
        return;
    case PKG_EVENT_STEP_WAIT:
        if (s->drawn) pkg_activity_waiting(e->wait ? e->object : NULL);
        return;
    case PKG_EVENT_STEP_END:
        if (s->drawn) pkg_activity_done();
        return;
    default:
        return;
    }
}

/* ---- sending ---------------------------------------------------------- */

static void emit_event(int type, struct ev_step *s, int outcome)
{
    struct pkg_event e;
    memset(&e, 0, sizeof e);
    e.size = (unsigned)sizeof e;
    e.type = type;
    e.op = op_id;
    e.verb = op_verb;
    e.dryrun = dry_run;
    e.step = s->id;
    e.parent = s->parent;
    e.attempt = s->attempt;
    e.phase = s->phase;
    e.package = s->has_package ? s->package : NULL;
    e.version = s->has_version ? s->version : NULL;
    e.object_kind = s->kind;
    e.object = s->has_object ? s->object : NULL;
    if (type == PKG_EVENT_STEP_WAIT) {
        e.object_kind = PKG_OBJECT_HOST;
        e.object = s->host[0] ? s->host : NULL;
        e.wait = s->wait;
    }
    e.unit = unit_of(s->measure);
    e.measure = s->measure;
    e.done = s->done;
    e.total = s->known ? s->total : 0;
    e.total_known = s->known;
    e.outcome = outcome;
    if (outcome == PKG_OUTCOME_FAILED || outcome == PKG_OUTCOME_CANCELLED)
        e.code = refused_class;
    s->sent_at = now_ms();
    if (sink != NULL && sink->event != NULL)
        sink->event(sink->user, &e);
    if (render)
        draw(&e, s);
}

static struct ev_step *top(void)
{
    return depth > 0 && overflow == 0 ? &stack[depth - 1] : NULL;
}

/* ---- the operation ---------------------------------------------------- */

void ev_op_begin(const char *verb, int draw_line)
{
    struct pkg_event e;
    op_id = ++op_seq;
    op_verb = verb;
    render = draw_line;
    listening = render || (sink != NULL && sink->event != NULL);
    step_seq = 0;
    depth = overflow = 0;
    cancels = 0;
    retried.attempt = 0;
    restore_state = PKG_RESTORE_NONE;
    ctx_package = ctx_version = NULL;
    if (sink == NULL || sink->event == NULL)
        return;
    memset(&e, 0, sizeof e);
    e.size = (unsigned)sizeof e;
    e.type = PKG_EVENT_OP_BEGIN;
    e.op = op_id;
    e.verb = verb;
    e.dryrun = dry_run;
    sink->event(sink->user, &e);
}

int ev_op_end(int rc)
{
    struct pkg_event e;
    /* A step still open has not said how it ended. The operation failing
     * is how it ended; succeeding with a step open is a fault of this
     * library, said as such rather than passed off as success. */
    while (depth > 0)
        ev_end(rc == 0 ? PKG_OUTCOME_ABANDONED : PKG_OUTCOME_FAILED);
    if (sink != NULL && sink->event != NULL) {
        memset(&e, 0, sizeof e);
        e.size = (unsigned)sizeof e;
        e.type = PKG_EVENT_OP_END;
        e.op = op_id;
        e.verb = op_verb;
        e.dryrun = dry_run;
        e.code = rc;
        e.outcome = rc == 0 ? PKG_OUTCOME_OK : cancels > 0 ? PKG_OUTCOME_CANCELLED
                  : PKG_OUTCOME_FAILED;
        e.restore = restore_state;
        e.cancel = cancels > 0;
        sink->event(sink->user, &e);
    }
    listening = render = 0;
    ctx_package = ctx_version = NULL;
    return rc;
}

/* A call made while another runs, from one of its callbacks: answered on
 * its own sink alone, since every state the library keeps belongs to the
 * operation under way. */
int ev_reentered(const struct pkg_sink *s, const char *verb)
{
    struct pkg_event e;
    if (s == NULL || s->event == NULL)
        return PKG_RC_USAGE;
    memset(&e, 0, sizeof e);
    e.size = (unsigned)sizeof e;
    e.type = PKG_EVENT_OP_BEGIN;
    e.op = ++op_seq;
    e.verb = verb;
    s->event(s->user, &e);
    e.type = PKG_EVENT_OP_END;
    e.code = PKG_RC_USAGE;
    e.outcome = PKG_OUTCOME_FAILED;
    s->event(s->user, &e);
    return PKG_RC_USAGE;
}

int ev_listening(void)
{
    return listening;
}

void ev_cancel_seen(void)
{
    cancels++;
}

void ev_restored(int state)
{
    static const int rank[] = { 0, 1, 3, 2 };     /* none, done, failed, incomplete */
    if (state >= 0 && state <= PKG_RESTORE_INCOMPLETE && rank[state] > rank[restore_state])
        restore_state = state;
}

void ev_context(const char *package, const char *version)
{
    ctx_package = package;
    ctx_version = version;
}

/* ---- steps ------------------------------------------------------------ */

static void fill(struct ev_step *s, int phase, int kind, const char *object,
                 const char *package, const char *version, int measure)
{
    if (package == NULL) { package = ctx_package; if (version == NULL) version = ctx_version; }
    s->phase = phase;
    s->kind = kind;
    s->measure = measure;
    s->has_package = package != NULL;
    s->has_version = version != NULL;
    s->has_object = object != NULL;
    clean_copy(s->package, sizeof s->package, package);
    clean_copy(s->version, sizeof s->version, version);
    clean_copy(s->object, sizeof s->object, object);
}

static void open_step(struct ev_step *s, unsigned attempt, unsigned long long total, int known)
{
    s->id = ++step_seq;
    s->parent = depth > 1 ? stack[depth - 2].id : 0;
    s->attempt = attempt;
    s->done = 0;
    s->total = known ? total : 0;
    s->known = known != 0;
    s->invalid = 0;
    s->wait = PKG_WAIT_NONE;
    s->host[0] = '\0';
    s->cancels = cancels;
    s->drawn = 0;
    emit_event(PKG_EVENT_STEP_BEGIN, s, PKG_OUTCOME_NONE);
}

void ev_begin(int phase, int kind, const char *object, const char *package,
              const char *version, int measure, unsigned long long total, int known)
{
    struct ev_step *s;
    unsigned attempt;
    if (overflow || depth == EV_DEPTH) { overflow++; return; }
    s = &stack[depth++];
    if (!listening) return;
    fill(s, phase, kind, object, package, version, measure);
    attempt = retried.attempt && retried.phase == phase && retried.kind == kind
              && strcmp(retried.object, s->object) == 0 ? retried.attempt + 1 : 1;
    retried.attempt = 0;
    open_step(s, attempt, total, known);
}

/* The counters moved: sent now when `now` says so, else when the gap since
 * the step last spoke allows. */
static void moved(struct ev_step *s, int now)
{
    if (!now && now_ms() - s->sent_at < PKG_EVENT_GAP_MS)
        return;                          /* the end, or a later update, carries it */
    emit_event(PKG_EVENT_STEP_PROGRESS, s, PKG_OUTCOME_NONE);
}

void ev_count(unsigned long long done)
{
    struct ev_step *s = top();
    int now = 0;
    if (s == NULL || !listening || done <= s->done)
        return;                          /* a counter never goes back within a step */
    s->done = done;
    if (s->known && s->done > s->total) {
        s->known = 0;
        s->invalid = 1;
        now = 1;
    }
    moved(s, now);
}

void ev_total(unsigned long long total)
{
    struct ev_step *s = top();
    if (s == NULL || !listening || s->invalid || (s->known && s->total == total))
        return;
    if (s->known || s->done > total) {   /* a second, different whole: neither is believed */
        s->known = 0;
        s->invalid = 1;
    } else {
        s->known = 1;
        s->total = total;
    }
    moved(s, 1);
}

void ev_end(int outcome)
{
    struct ev_step *s;
    if (overflow) { overflow--; return; }
    if (depth == 0) return;
    s = &stack[depth - 1];
    if (listening) {
        if (s->wait != PKG_WAIT_NONE)
            s->wait = PKG_WAIT_NONE;     /* the end says it: no separate "wait over" */
        if (outcome == PKG_OUTCOME_FAILED && cancels > s->cancels)
            outcome = PKG_OUTCOME_CANCELLED;
        emit_event(PKG_EVENT_STEP_END, s, outcome);
        if (outcome == PKG_OUTCOME_RETRIED) {
            retried.phase = s->phase;
            retried.kind = s->kind;
            snprintf(retried.object, sizeof retried.object, "%s", s->object);
            retried.attempt = s->attempt;
        }
    }
    depth--;
}

int ev_end_phase(int phase, int outcome)
{
    struct ev_step *s = top();
    if (s == NULL || (listening && s->phase != phase))
        return 0;
    ev_end(outcome);
    return 1;
}

int ev_depth(void)
{
    return depth + overflow;
}

int ev_unwind(int to, int rc)
{
    while (ev_depth() > to)
        ev_end(rc == 0 ? PKG_OUTCOME_OK : PKG_OUTCOME_FAILED);
    return rc;
}

/* Bytes from the layers that move them: the network and the archive
 * reader. They report absolute counts and a whole, -1 when unknown. A count
 * that goes back means the work started again: the step ends RETRIED and the
 * same step begins anew, attempt + 1. */
void ev_bytes(long long done, long long total)
{
    struct ev_step *s = top();
    int now = 0, changed;
    if (s == NULL || !listening || unit_of(s->measure) != PKG_UNIT_BYTES || done < 0)
        return;
    if (s->wait != PKG_WAIT_NONE) {
        s->wait = PKG_WAIT_NONE;
        emit_event(PKG_EVENT_STEP_WAIT, s, PKG_OUTCOME_NONE);
    }
    changed = (unsigned long long)done != s->done;
    if ((unsigned long long)done < s->done) {
        struct ev_step again = *s;
        emit_event(PKG_EVENT_STEP_END, s, PKG_OUTCOME_RETRIED);
        *s = again;
        open_step(s, again.attempt + 1, 0, 0);
        changed = 1;
    }
    if (total >= 0 && !s->invalid) {
        if (!s->known) {
            s->known = 1;
            s->total = (unsigned long long)total;
            now = 1;
        } else if (s->total != (unsigned long long)total) {
            s->known = 0;
            s->invalid = 1;
            now = 1;
        }
    }
    s->done = (unsigned long long)done;
    if (s->known && s->done > s->total) {
        s->known = 0;
        s->invalid = 1;
        now = 1;
    }
    if (now || changed)
        moved(s, now);
}

/* A wait begins (host) or ends (NULL). Said once each way: the network
 * layer calls this on every pass of its polling loop. */
void ev_wait(const char *host)
{
    struct ev_step *s = top();
    char clean[sizeof s->host];
    if (s == NULL || !listening)
        return;
    if (host == NULL) {
        if (s->wait == PKG_WAIT_NONE)
            return;
        s->wait = PKG_WAIT_NONE;
        emit_event(PKG_EVENT_STEP_WAIT, s, PKG_OUTCOME_NONE);
        return;
    }
    clean_copy(clean, sizeof clean, host);
    if (s->wait == PKG_WAIT_NETWORK && strcmp(s->host, clean) == 0)
        return;
    s->wait = PKG_WAIT_NETWORK;
    snprintf(s->host, sizeof s->host, "%s", clean);
    emit_event(PKG_EVENT_STEP_WAIT, s, PKG_OUTCOME_NONE);
}

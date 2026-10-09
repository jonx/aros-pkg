/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * events: progress events, the way a window would use them. The operation
 * runs on a worker thread; its event callback copies each event into a
 * queue, and the main thread, standing for a UI's own loop, takes them out
 * and shows them. No toolkit: the "window" is stdout.
 *
 *   cc -Iinclude -pthread examples/events.c build/libpkg.a -o events
 *   ./events /tmp/pkg-events
 *
 * It makes a key, publishes a drawer of a few files as hello 1.0 and 1.1,
 * installs 1.0, then upgrades to 1.1, in a directory given on the command
 * line, which must not exist yet. What to take from it:
 *   - the callback runs on the operation's thread, and the event and its
 *     strings are only lent for the call: it copies what it keeps;
 *   - it never calls libpkg, and never waits for the UI longer than it
 *     takes to make room in the queue;
 *   - the UI knows a step by (op, step), shows a share only for a step whose
 *     total is known, and an indeterminate mark otherwise;
 *   - OP_END is the answer: its code is what the call returns.
 * See docs/events.md.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the copy a UI keeps --------------------------------------------- */

struct copied {
    int type, phase, unit, outcome, code, restore, total_known, wait;
    unsigned long op, step, parent;
    unsigned attempt;
    unsigned long long done, total;
    char verb[16], package[72], version[64], object[160];
};

static void keep(char *out, size_t ol, const char *in)
{
    snprintf(out, ol, "%s", in != NULL ? in : "");
}

/* ---- the queue between the two threads --------------------------------- */

#define QLEN 64

static struct copied q[QLEN];
static unsigned qhead, qtail;            /* qtail - qhead events are waiting */
static int finished;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;

/* pkg_sink.event: on the operation's thread. Copy, queue, return. A full
 * queue makes the operation wait for the UI to take one; nothing is
 * dropped, so a begin always has its end. */
static void on_event(void *user, const struct pkg_event *e)
{
    struct copied c;
    (void)user;
    memset(&c, 0, sizeof c);
    c.type = e->type; c.phase = e->phase; c.unit = e->unit; c.outcome = e->outcome;
    c.code = e->code; c.restore = e->restore; c.total_known = e->total_known; c.wait = e->wait;
    c.op = e->op; c.step = e->step; c.parent = e->parent; c.attempt = e->attempt;
    c.done = e->done; c.total = e->total;
    keep(c.verb, sizeof c.verb, e->verb);
    keep(c.package, sizeof c.package, e->package);
    keep(c.version, sizeof c.version, e->version);
    keep(c.object, sizeof c.object, e->object);
    pthread_mutex_lock(&qlock);
    while (qtail - qhead == QLEN)
        pthread_cond_wait(&qcond, &qlock);
    q[qtail++ % QLEN] = c;
    pthread_cond_broadcast(&qcond);
    pthread_mutex_unlock(&qlock);
}

/* ---- the worker: the operations, one after the other ------------------- */

struct job { const char *channel, *root; int install_rc, upgrade_rc; };

static void *work(void *arg)
{
    struct job *j = (struct job *)arg;
    struct pkg_sink sink;
    struct pkg_options o;
    memset(&sink, 0, sizeof sink);
    sink.structured = 1;                 /* the answer as records; nobody reads them here */
    sink.event = on_event;
    memset(&o, 0, sizeof o);
    o.target = "hello"; o.version = "1.0"; o.root = j->root; o.channel = j->channel;
    j->install_rc = pkg_install(&sink, &o);
    o.version = NULL;
    j->upgrade_rc = pkg_upgrade(&sink, &o);
    pthread_mutex_lock(&qlock);
    finished = 1;
    pthread_cond_broadcast(&qcond);
    pthread_mutex_unlock(&qlock);
    return NULL;
}

/* ---- the UI: its own loop, its own pace -------------------------------- */

static void show(const struct copied *c)
{
    switch (c->type) {
    case PKG_EVENT_OP_BEGIN:
        printf("%s (operation %lu)\n", c->verb, c->op);
        break;
    case PKG_EVENT_STEP_BEGIN:
        printf("  %*s%s %s%s%s%s%s\n", c->parent ? 2 : 0, "", pkg_phase_name(c->phase), c->package,
               c->version[0] ? " " : "", c->version, c->object[0] ? " " : "", c->object);
        break;
    case PKG_EVENT_STEP_PROGRESS:
        if (c->total_known && c->total > 0)
            printf("  %*s  %llu%% of %llu %s\n", c->parent ? 2 : 0, "", c->done * 100 / c->total,
                   c->total, pkg_unit_name(c->unit));
        else
            printf("  %*s  %llu %s so far\n", c->parent ? 2 : 0, "", c->done, pkg_unit_name(c->unit));
        break;
    case PKG_EVENT_STEP_WAIT:
        printf("  %*s  %s%s\n", c->parent ? 2 : 0, "", c->wait ? "waiting for " : "working again",
               c->wait ? c->object : "");
        break;
    case PKG_EVENT_STEP_END:
        if (c->unit != PKG_UNIT_NONE)
            printf("  %*s  %s: %llu %s\n", c->parent ? 2 : 0, "", pkg_outcome_name(c->outcome), c->done,
                   pkg_unit_name(c->unit));
        else
            printf("  %*s  %s\n", c->parent ? 2 : 0, "", pkg_outcome_name(c->outcome));
        break;
    case PKG_EVENT_OP_END:
        printf("%s: %s, code %d%s%s\n", c->verb, pkg_outcome_name(c->outcome), c->code,
               c->restore ? ", restore " : "", c->restore ? pkg_restore_name(c->restore) : "");
        break;
    }
}

static void ui_loop(void)
{
    for (;;) {
        struct copied c;
        pthread_mutex_lock(&qlock);
        while (qtail == qhead && !finished)
            pthread_cond_wait(&qcond, &qlock);
        if (qtail == qhead) {            /* finished, and nothing left to show */
            pthread_mutex_unlock(&qlock);
            return;
        }
        c = q[qhead++ % QLEN];
        pthread_cond_broadcast(&qcond);  /* room for the worker */
        pthread_mutex_unlock(&qlock);
        show(&c);                        /* drawn outside the lock */
    }
}

/* ---- setting the scene ------------------------------------------------- */

static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    fputs(text, f);
    return fclose(f);
}

static int publish(const char *drawer, const char *channel, const char *key, const char *version)
{
    struct pkg_sink sink;
    struct pkg_options o;
    memset(&sink, 0, sizeof sink);
    sink.structured = 1;
    memset(&o, 0, sizeof o);
    o.target = drawer; o.channel = channel; o.sign = key;
    o.name = "hello"; o.version = version; o.kind = "data";
    return pkg_publish(&sink, &o);
}

int main(int argc, char **argv)
{
    char key[512], drawer[512], channel[512], root[512], file[600];
    struct pkg_sink quiet;
    struct pkg_options o;
    struct job j;
    pthread_t worker;
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <new directory>\n", argv[0]);
        return 20;
    }
    if (mkdir(argv[1], 0700) != 0) {
        fprintf(stderr, "%s must not exist yet\n", argv[1]);
        return 20;
    }
    snprintf(key, sizeof key, "%s/key", argv[1]);
    snprintf(drawer, sizeof drawer, "%s/drawer", argv[1]);
    snprintf(channel, sizeof channel, "%s/channel", argv[1]);
    snprintf(root, sizeof root, "%s/root", argv[1]);
    mkdir(drawer, 0700);
    memset(&quiet, 0, sizeof quiet);
    quiet.structured = 1;
    memset(&o, 0, sizeof o);
    o.file = key;
    if (pkg_keygen(&quiet, &o) != 0) return 1;
    for (i = 0; i < 5; i++) {
        snprintf(file, sizeof file, "%s/part%d", drawer, i);
        if (put(file, "a part of hello\n") != 0) return 1;
    }
    if (publish(drawer, channel, key, "1.0") != 0) return 1;
    snprintf(file, sizeof file, "%s/part0", drawer);
    if (put(file, "a better part of hello\n") != 0) return 1;
    if (publish(drawer, channel, key, "1.1") != 0) return 1;

    j.channel = channel; j.root = root;
    if (pthread_create(&worker, NULL, work, &j) != 0) return 1;
    ui_loop();
    pthread_join(worker, NULL);
    printf("returned %d and %d\n", j.install_rc, j.upgrade_rc);
    return j.install_rc != 0 ? j.install_rc : j.upgrade_rc;
}

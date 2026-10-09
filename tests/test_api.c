/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * libpkg driven the way a graphical front end would drive it: through pkg.h
 * alone, with a sink that keeps every record, no command line, no stdout.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void ok(int c, const char *what) { checks++; if (!c) { failures++; printf("  FAIL %s\n", what); } }

/* A front end's view: every field of the last answer, and whether any text
 * arrived at all. */
struct seen {
    char keys[64][32];
    char values[64][300];
    int  n, texts;
};

static void rec(void *user, const char *key, const char *value)
{
    struct seen *s = (struct seen *)user;
    if (s->n < 64) {
        snprintf(s->keys[s->n], sizeof s->keys[0], "%s", key);
        snprintf(s->values[s->n], sizeof s->values[0], "%s", value);
        s->n++;
    }
}

static void txt(void *user, int is_error, const char *text)
{
    (void)is_error;
    (void)text;
    ((struct seen *)user)->texts++;
}

static int items_ok, cancel_after = -1, asked;

/* An entry must arrive with its fields apart, in their order. */
static void itm(void *user, const char *kind, int n, const char *const *k, const char *const *v)
{
    (void)user;
    if (strcmp(kind, "entry") == 0 && n >= 7 && strcmp(k[0], "name") == 0
        && strcmp(v[0], "tool") == 0 && strcmp(k[4], "status") == 0 && strcmp(v[4], "ok") == 0
        && strcmp(k[6], "installed") == 0 && strcmp(v[6], "installed") == 0)
        items_ok = 1;
}

/* STATUS: the package's fields apart, by name. */
static int status_ok;
static void itm_status(void *user, const char *kind, int n, const char *const *k, const char *const *v)
{
    const char *name = pkg_field(n, k, v, "name"), *inst = pkg_field(n, k, v, "installed");
    const char *avail = pkg_field(n, k, v, "available"), *state = pkg_field(n, k, v, "state");
    (void)user;
    if (strcmp(kind, "package") == 0 && name && inst && avail && state && strcmp(name, "tool") == 0
        && strcmp(inst, "1.0") == 0 && strcmp(avail, "1.1") == 0 && strcmp(state, "upgradable") == 0)
        status_ok = 1;
}

static int stop_now(void *user)
{
    (void)user;
    return cancel_after >= 0 && asked++ >= cancel_after;
}

static int traced;
static void trc(void *user, const char *line)
{
    (void)user;
    if (strstr(line, "install: picked tool 1.0") != NULL)
        traced = 1;
}

static const char *field(const struct seen *s, const char *key)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->keys[i], key) == 0)
            return s->values[i];
    return NULL;
}

static int is(const struct seen *s, const char *key, const char *value)
{
    const char *v = field(s, key);
    return v != NULL && strcmp(v, value) == 0;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void)
{
    char dirbuf[400], *dir = dirbuf, p[512], key[512], drawer[512], channel[512], root[512];
    const char *tmp = getenv("TMPDIR");
    struct seen s;
    struct pkg_sink sink = { .record = rec, .text = txt, .user = &s, .structured = 1 };
    struct pkg_options o;
    struct stat st;
    int rc;

    snprintf(dirbuf, sizeof dirbuf, "%s/pkg-api.%ld", tmp ? tmp : "/tmp", (long)getpid());
    if (mkdir(dir, 0700) != 0) { printf("cannot make %s\n", dir); return 1; }
    snprintf(key, sizeof key, "%s/dev.key", dir);
    snprintf(drawer, sizeof drawer, "%s/drawer", dir);
    snprintf(channel, sizeof channel, "%s/channel", dir);
    snprintf(root, sizeof root, "%s/root", dir);
    snprintf(p, sizeof p, "%s/C", drawer);
    mkdir(drawer, 0755);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/C/Tool", drawer);
    put(p, "tool\n");

    printf("structured\n");
    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.file = key;
    rc = pkg_keygen(&sink, &o);
    ok(rc == PKG_RC_OK && is(&s, "result", "created") && field(&s, "public") != NULL,
       "keygen answers with result and the public key");
    ok(s.texts == 0, "and the structured form sends no text");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.target = drawer; o.channel = channel; o.sign = key;
    o.name = "tool"; o.version = "1.0"; o.kind = "application"; o.dryrun = 1;
    rc = pkg_publish(&sink, &o);
    ok(rc == 0 && is(&s, "result", "would-publish") && is(&s, "depends", "none"),
       "a dry-run publish describes itself");
    ok(stat(channel, &st) != 0, "and writes no channel");

    memset(&s, 0, sizeof s);
    o.dryrun = 0;
    rc = pkg_publish(&sink, &o);
    ok(rc == 0 && is(&s, "result", "published") && is(&s, "name", "tool"), "publish");

    memset(&s, 0, sizeof s);
    o.name = "to\nol";
    rc = pkg_publish(&sink, &o);
    ok(rc == 20 && is(&s, "result", "refused") && strstr(field(&s, "reason"), "line break") != NULL,
       "a value holding a line break is refused through the library too");
    o.name = "tool";

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.target = "tool"; o.root = root; o.channel = channel;
    rc = pkg_install(&sink, &o);
    ok(rc == 0 && is(&s, "result", "installed") && is(&s, "version", "1.0"), "install");
    ok(strcmp(s.keys[0], "result") == 0 || field(&s, "dependency") != NULL,
       "fields arrive in order, result first when nothing precedes it");
    memset(&s, 0, sizeof s);
    rc = pkg_install(&sink, &o);
    ok(rc == 0 && is(&s, "result", "unchanged"), "installing again changes nothing and succeeds");

    memset(&s, 0, sizeof s);
    o.target = "nosuch";
    rc = pkg_upgrade(&sink, &o);
    ok(rc == PKG_RC_NOTFOUND && is(&s, "result", "refused") && is(&s, "class", "not-found")
       && is(&s, "code", "11") && is(&s, "next", "use-install") && field(&s, "reason") != NULL,
       "a refusal: class, code, reason and next, as fields");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.root = root;
    rc = pkg_list(&sink, &o);
    ok(rc == 0 && is(&s, "package", "tool 1.0 application 1 explicit") && is(&s, "count", "1"),
       "list");

    memset(&s, 0, sizeof s);
    rc = pkg_usage_error(&sink, "install", "a front end found no package name");
    ok(rc == PKG_RC_USAGE && is(&s, "class", "usage") && is(&s, "next", "fix-command"),
       "a front end's own usage error has the same shape");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    sink.trace = trc;
    o.target = "tool"; o.root = root; o.channel = channel; o.dryrun = 1;
    rc = pkg_install(&sink, &o);
    sink.trace = NULL;
    ok(rc == 0 && traced, "the trace callback hears the operation's choices");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    sink.item = itm;
    o.channel = channel; o.root = root;
    rc = pkg_show(&sink, &o);
    sink.item = NULL;
    ok(rc == 0 && items_ok, "SHOW against a root: each entry's fields apart, installed among them");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.target = "tol"; o.root = root; o.channel = channel;
    rc = pkg_install(&sink, &o);
    ok(rc == PKG_RC_NOTFOUND && is(&s, "suggest", "tool"), "a typo gets the near name as a suggest field");

    {
        char root2[600];
        snprintf(root2, sizeof root2, "%s/root2", dir);
        memset(&s, 0, sizeof s);
        memset(&o, 0, sizeof o);
        o.target = "tool"; o.root = root2; o.channel = channel;
        sink.cancel = stop_now;
        cancel_after = 1;
        asked = 0;
        rc = pkg_install(&sink, &o);
        sink.cancel = NULL;
        snprintf(p, sizeof p, "%s/C/Tool", root2);
        ok(rc == PKG_RC_REFUSED && is(&s, "next", "report") && stat(p, &st) != 0,
           "cancel stops the install with nothing placed");
    }
    {
        /* A cancel after the dependency is placed takes it back out, with
         * the key it pinned, and no dependency item is ever sent. */
        char lib[600], app[600], root3[600];
        snprintf(lib, sizeof lib, "%s/lib", dir);
        snprintf(app, sizeof app, "%s/app", dir);
        snprintf(root3, sizeof root3, "%s/root3", dir);
        snprintf(p, sizeof p, "%s/Libs", lib); mkdir(lib, 0755); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/Libs/t.library", lib); put(p, "lib\n");
        snprintf(p, sizeof p, "%s/C", app); mkdir(app, 0755); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/C/App", app); put(p, "app\n");
        memset(&o, 0, sizeof o);
        o.channel = channel; o.sign = key; o.kind = "library"; o.target = lib;
        o.name = "tlib"; o.version = "1";
        pkg_publish(&sink, &o);
        o.kind = "application"; o.target = app; o.name = "tapp"; o.depends = "tlib";
        pkg_publish(&sink, &o);
        memset(&s, 0, sizeof s);
        memset(&o, 0, sizeof o);
        o.target = "tapp"; o.root = root3; o.channel = channel;
        sink.cancel = stop_now;
        cancel_after = 3;       /* resolving tapp, tlib; placing tlib; then stop */
        asked = 0;
        rc = pkg_install(&sink, &o);
        sink.cancel = NULL;
        snprintf(p, sizeof p, "%s/Libs/t.library", root3);
        {
            char pin[700];
            snprintf(pin, sizeof pin, "%s/.pkg/keys/tlib", root3);
            ok(rc == PKG_RC_REFUSED && stat(p, &st) != 0 && stat(pin, &st) != 0
               && field(&s, "dependency") == NULL && strstr(field(&s, "reason"), "taken back out") != NULL,
               "cancel after a dependency: file and pinned key taken back out, no dependency item sent");
        }
    }
    {
        /* A newer tool in the channel: STATUS says so and changes nothing;
         * UPGRADE with all set, as a dry run, would take it. */
        snprintf(p, sizeof p, "%s/C/Tool", drawer);
        put(p, "tool, better\n");
        memset(&o, 0, sizeof o);
        o.target = drawer; o.channel = channel; o.sign = key; o.name = "tool"; o.version = "1.1";
        o.kind = "application";
        ok(pkg_publish(&sink, &o) == 0, "publish tool 1.1");
        memset(&s, 0, sizeof s);
        memset(&o, 0, sizeof o);
        o.root = root; o.channel = channel;
        sink.item = itm_status;
        rc = pkg_status(&sink, &o);
        sink.item = NULL;
        ok(rc == 0 && is(&s, "result", "shown") && status_ok && is(&s, "count", "1")
           && is(&s, "upgradable", "1"), "pkg_status: tool 1.0, 1.1 available, upgradable, fields apart");
        memset(&s, 0, sizeof s);
        o.all = 1; o.dryrun = 1;
        rc = pkg_upgrade(&sink, &o);
        ok(rc == 0 && is(&s, "result", "would-upgrade") && is(&s, "package", "tool 1.0 1.1")
           && is(&s, "count", "1"), "pkg_upgrade with all, dry run: would-upgrade tool");
        snprintf(p, sizeof p, "%s/C/Tool", root);
        {
            FILE *f = fopen(p, "rb");
            char b[32] = "";
            if (f) { if (fgets(b, sizeof b, f) == NULL) b[0] = '\0'; fclose(f); }
            ok(strcmp(b, "tool\n") == 0, "and the dry run left tool 1.0 in place");
        }
    }
    ok(strstr(pkg_next_words("check-name"), "pkg ") == NULL
       && strstr(pkg_next_words("use-upgrade"), "UPGRADE") == NULL,
       "pkg_next_words names no command, for any front end");

    printf("text\n");
    sink.structured = 0;
    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.root = root;
    rc = pkg_list(&sink, &o);
    ok(rc == 0 && s.texts > 0 && s.n == 0, "the text form sends text and no fields");

    printf("names\n");
    ok(strcmp(pkg_class_name(PKG_RC_KEY), "key") == 0 && strcmp(pkg_class_name(0), "ok") == 0,
       "pkg_class_name");
    ok(strstr(pkg_next_words("ask-requester"), "decision") != NULL
       && strcmp(pkg_next_words(NULL), pkg_next_words("report")) == 0, "pkg_next_words");
    {
        const char *k[] = { "name", "version" }, *v[] = { "tool", "1.0" };
        ok(strcmp(pkg_field(2, k, v, "version"), "1.0") == 0 && pkg_field(2, k, v, "kind") == NULL,
           "pkg_field");
    }

    snprintf(p, sizeof p, "rm -rf '%s'", dir);
    if (system(p) != 0) printf("  note: could not remove %s\n", dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Pkg, command line: a client of libpkg (pkg.h). It turns the words of a
 * command into pkg_options, prints what the library answers, and returns its
 * code as the exit status. Keywords follow AmigaDOS usage and are
 * case-insensitive; `pkg HELP` lists them.
 *
 * MACHINE on the line, or PKG_OUTPUT=machine in the environment, selects the
 * library's structured form, printed as "key: value" lines on stdout.
 * Otherwise the text form goes to stdout, refusals and warnings to stderr.
 * PKG_SIGNKEY is the default signing key. PORT serves the same verbs on an
 * ARexx port on AROS.
 */

#define _POSIX_C_SOURCE 200809L
#include "pkg.h"
#include "pkg_environment.h"
#include "pkg_selfupdate.h"
#include "pkg_activity.h"
#include "pkg_args.h"
#include "pkg_fs.h"
#include "pkg_manifest.h"
#include "pkg_out.h"
#include "pkg_port.h"
#include "pkg_style.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AmigaDOS version cookie: `Version C:Pkg` reads it, and publishing Pkg
 * with Pkg takes its name and version from it. */
const char pkg_version_cookie[] = "$VER: pkg " PKG_VERSION_STRING " (" PKG_BUILD_DAY ")";

#ifdef __AROS__
static const int on_aros = 1;
#else
static const int on_aros = 0;
#endif
#ifdef __AROS__
#include <proto/dos.h>
#elif defined(_WIN32)
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

static const char *chosen_environment;
static int environment_system;
static const char *verb_name = "pkg";
static int machine;
static int serving_port;   /* PORT: output is captured, never a terminal */

/* LOG <file>: everything printed is also appended to that file (AROS has
 * no tee), less the progress counter, which only a watching person needs. */
static const char *log_path;
static FILE *log_file;

static void to_log(const char *text)
{
    if (log_path == NULL || text[0] == '\r')
        return;
    if (log_file == NULL)
        log_file = fopen(log_path, "a");
    if (log_file != NULL) {
        fputs(text, log_file);
        fflush(log_file);
    }
}

static void print_record(void *user, const char *key, const char *value)
{
    (void)user;
    pkg_out("%s: %s\n", key, value);
    if (log_path != NULL) { to_log(key); to_log(": "); to_log(value); to_log("\n"); }
}

static void print_text(void *user, int is_error, const char *text)
{
    (void)user;
    if (is_error) pkg_err("%s", text); else pkg_out("%s", text);
    to_log(text);
}

/* The styled line to the screen, the plain one to the log. */
static void write_styled(int is_error, const char *styled, const char *plain)
{
    if (is_error) pkg_err("%s", styled); else pkg_out("%s", styled);
    to_log(plain);
}

/* What the command line says on its own account, outside the library's
 * lines: one method per kind of thing being said, so that the look of a
 * question or of a note is decided in pkg_style.c and nowhere else. Before
 * these, the prompts went out through pkg_out and came out unstyled, which
 * is how a question ended up looking like a figure. In MACHINE mode a
 * question is a record like any other: nothing is asked there.
 *
 *   say_asked     a question, waiting for an answer on the same line
 *   say_quiet     a figure or a path that supports an answer
 *   say_noted     a remark worth reading, in passing
 *   say_done      the thing asked for happened */
static void say_kind_v(int kind, const char *key, const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (machine) {
        if (key != NULL) print_record(NULL, key, buf);
        return;
    }
    pkg_style_line(write_styled, kind, 0, buf);
}

#define SAY_METHOD(name, kind, key)                       \
    static void name(const char *fmt, ...)                \
    {                                                     \
        va_list ap;                                       \
        va_start(ap, fmt);                                \
        say_kind_v((kind), (key), fmt, ap);               \
        va_end(ap);                                       \
    }

SAY_METHOD(say_asked, PKG_LINE_QUESTION, "question")
SAY_METHOD(say_quiet, PKG_LINE_DETAIL,   NULL)
SAY_METHOD(say_noted, PKG_LINE_NOTE,     "note")
SAY_METHOD(say_done,  PKG_LINE_RESULT,   "result")

static void print_line(void *user, int kind, int is_error, const char *text)
{
    (void)user;
    pkg_style_line(write_styled, kind, is_error, text);
}

/* TRACE <file>, or PKG_TRACE=<file>: the library's account of each step,
 * appended to that file; "-" sends it to stderr. Never mixed into stdout,
 * so the MACHINE contract stays clean. */
static const char *trace_path;
static FILE *trace_file;

static void print_trace(void *user, const char *line)
{
    (void)user;
    if (strcmp(trace_path, "-") == 0) {
        pkg_err("trace %s\n", line);
        return;
    }
    if (trace_file == NULL)
        trace_file = fopen(trace_path, "a");
    if (trace_file != NULL) {
        fprintf(trace_file, "%s\n", line);
        fflush(trace_file);
    }
}

static struct pkg_sink out_sink = { print_record, print_text, NULL, 0, NULL, NULL, NULL, 0, print_line };

/* A usage error found while reading the words, answered like any refusal. */
static int usage_errorf(const char *fmt, ...)
{
    char why[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    out_sink.structured = machine;
    pkg_usage_error(&out_sink, verb_name, why);
    return -1;
}

/* ---- arguments -------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* A value typed by a person, or pasted: surrounding spaces go, and a line
 * break, tab or other control character inside is refused, naming where, so
 * nothing invisible reaches a name, a path or a record. */
static int clean_value(const char *what, char *v)
{
    size_t n = strlen(v), i, lead = strspn(v, " ");
    while (n > lead && (v[n - 1] == ' ' || v[n - 1] == '\r' || v[n - 1] == '\n'))
        v[--n] = '\0';
    if (lead) memmove(v, v + lead, n - lead + 1);
    for (i = 0; v[i]; i++)
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] == 0x7F)
            return usage_errorf("%s holds a %s at character %lu; type it again without it",
                                what, v[i] == '\n' || v[i] == '\r' ? "line break" :
                                v[i] == '\t' ? "tab" : "control character", (unsigned long)i + 1);
    return 0;
}

/* ---- the verbs -------------------------------------------------------- */

/* Every verb pkg takes, and the words each one takes, written once. The
 * template is the contract (pkg_args.h): the words of a command are read
 * against it and a word the verb does not take is refused by name, where it
 * used to be accepted and ignored; `pkg HELP` draws the usage from it, and
 * `pkg HELP MACHINE` lists it for the test that holds docs/reference.md to
 * it. A verb of two words (CHANNEL ADD, ENV LIST) has a template of its own,
 * so each says exactly what it takes.
 *
 * MACHINE, TRACE and LOG are taken on every verb, ENVIRONMENT on every verb
 * that takes a root: they are appended here, and left out of each verb's own
 * line of usage, which lists them once. */
struct verb {
    const char *group;      /* the usage heading this verb opens, or NULL */
    const char *verb;       /* as typed: "INSTALL", "CHANNEL ADD" */
    const char *name;       /* as the library and a refusal name it */
    int (*fn)(const struct pkg_sink *, const struct pkg_options *);
    const char *tmpl;
    const char *what;       /* one line of usage */
};

#define TMPL_ANY   ",MACHINE/S/G,TRACE/K/G,LOG/K/G"
#define TMPL_ROOTED   ",ENVIRONMENT/K/G" TMPL_ANY
/* What PUBLISH and MANIFEST say about a package, beside its files. */
#define TMPL_DESCRIBE "NAME/K=n,VERSION/K,ARCH/K,KIND/K,DEPENDS/K=\"a >= 1, b\",CONFIG/K=\"S/Startup-Sequence\"," \
                 "FILES/K=\"C,Libs\",BUILD/K=<date>,UPSTREAM/K=<url>,INFO/K=<file>,SHORT/K,DESCRIPTION/K=<file>," \
                 "CATEGORY/K,TAGS/K,AUTHOR/K,LICENSE/K,DISTRIBUTION/K,HOMEPAGE/K=<url>,REPOSITORY/K=<url>," \
                 "ICON/K=<file>,SCREENSHOT/K=<file>,README/K=<file>,CHANGES/K=<file>"

static int environment_command(const struct pkg_options *a);
static int env_verb(const struct pkg_sink *s, const struct pkg_options *a)
{
    (void)s;
    return environment_command(a);
}

static const struct verb verbs[] = {
    { "Installing and keeping software", "INSTALL", "install", pkg_install,
      "PACKAGE/M/A,ROOT/K/R,AT/K,CHANNEL/K,VERSION/K,ARCH/K,KEY/K=<public key>,ACCEPTKEY/K,UNPACKED/K,DOWNGRADE/S,DRYRUN/S" TMPL_ROOTED,
      "install packages and what they depend on; several names go as far as they can" },
    { NULL, "STATUS", "status", pkg_status,
      "PACKAGE,ROOT/K/R,CHANNEL/K,ARCH/K" TMPL_ROOTED,
      "what is installed and what has a newer version; exit 0 either way" },
    { NULL, "UPGRADE", "upgrade", pkg_upgrade,
      "PACKAGE,ALL/S,ROOT/K/R,CHANNEL/K,VERSION/K,ARCH/K,DOWNGRADE/S,KEY/K=<public key>,ACCEPTKEY/K,UNPACKED/K,DRYRUN/S" TMPL_ROOTED,
      "a package, or ALL of them, to the newest version; with no name and no root, pkg itself" },
    { NULL, "ROLLBACK", "rollback", pkg_rollback,
      "PACKAGE/A,ROOT/K/R,CHANNEL/K,VERSION/K,ARCH/K,KEY/K=<public key>,ACCEPTKEY/K,DRYRUN/S" TMPL_ROOTED,
      "back to the version installed before" },
    { NULL, "LIST", "list", pkg_list, "ROOT/K/R" TMPL_ROOTED, "what a root holds" },
    { NULL, "VERIFY", "verify", pkg_verify, "PACKAGE,ALL/S,ROOT/K/R" TMPL_ROOTED,
      "every installed file against its signed manifest" },
    { NULL, "REPAIR", "repair", pkg_repair,
      "PACKAGE,ALL/S,ROOT/K/R,CHANNEL/K,ARCH/K,KEY/K=<public key>,UNPACKED/K,DRYRUN/S" TMPL_ROOTED, "put damaged files back" },
    { NULL, "REMOVE", "remove", pkg_remove, "PACKAGE,ORPHANS/S,ROOT/K/R,DRYRUN/S" TMPL_ROOTED,
      "a package, or the ORPHANS nothing needs; with no name and no root, pkg itself" },
    { NULL, "SHOW", "show", pkg_show, "PACKAGE,CHANNEL/K,ROOT/K,METADATA/S,ARCHIVE/K" TMPL_ROOTED,
      "what a channel offers, each entry checked" },
    { NULL, "SEARCH", "search", pkg_search, "WORDS/M/A,CHANNEL/K,ROOT/K,ARCH/K" TMPL_ROOTED,
      "the packages every word matches; exit 0 whether or not any do" },
    { NULL, "RESOLVE", "resolve", pkg_resolve,
      "LIBRARY/A,VERSION/K,ROOT/K,FROM/K,CHANNEL/K,ARCH/K" TMPL_ROOTED,
      "which copy of a library a program gets, and why" },
    { NULL, "MOUNTLIST", "mountlist", pkg_mountlist,
      "PACKAGE/A=<image>,ROOT/K/R,OUT/K,UNIT/K,HANDLER/K,DRYRUN/S" TMPL_ROOTED,
      "the Mount entry for an installed image" },
    { "Channels and environments", "CHANNEL ADD", "channel", pkg_channel,
      "CHANNEL/A/W=<dir|url>,NAME/K=<name>,ROOT/K/R,DRYRUN/S" TMPL_ROOTED,
      "a channel this root reads when CHANNEL is left out; it takes a short name, its own or NAME's" },
    { NULL, "CHANNEL LIST", "channel", pkg_channel, "ROOT/K/R" TMPL_ROOTED,
      "the channels this root reads, in order, with their names" },
    { NULL, "CHANNEL REMOVE", "channel", pkg_channel,
      "CHANNEL/A/W=<dir|url|name>,ROOT/K/R,DRYRUN/S" TMPL_ROOTED, "a channel off this root's list" },
    { NULL, "ENV ADD", "env", env_verb, "NAME/A,ROOT/K/A,SYSTEM/S" TMPL_ANY,
      "a root registered under a name, so ENVIRONMENT <name> selects it" },
    { NULL, "ENV LIST", "env", env_verb, "SYSTEM/S" TMPL_ANY, "the registered roots, and the default" },
    { NULL, "ENV REMOVE", "env", env_verb, "NAME/A,SYSTEM/S" TMPL_ANY, "a registered root forgotten" },
    { NULL, "ENV DEFAULT", "env", env_verb, "NAME/A,SYSTEM/S" TMPL_ANY,
      "the root a command uses when neither ROOT nor ENVIRONMENT says" },
    { "Publishing", "KEYGEN", "keygen", pkg_keygen, "FILE/K/A=<keyfile>" TMPL_ANY,
      "a signing key, readable by you alone" },
    { NULL, "KEYINFO", "keyinfo", pkg_keyinfo, "KEYFILE,FILE/K=<keyfile>,SSH/S" TMPL_ANY,
      "the public key a key file holds; SSH: as ssh-ed25519" },
    { NULL, "MANIFEST", "manifest", pkg_manifest, "DRAWER/A," TMPL_DESCRIBE TMPL_ANY,
      "the manifest PUBLISH would sign, to read before publishing" },
    { NULL, "PUBLISH", "publish", pkg_publish,
      "DRAWER/A,CHANNEL/K/A=<dir>," TMPL_DESCRIBE ",SIGN/K=<keyfile>,ACCEPTKEY/K,DRYRUN/S" TMPL_ANY,
      "a drawer published as a version; the channel is created when missing" },
    /* PACKAGE is PUBLISH under the name a person who has not pushed yet
     * expects; the same operation, the same records. */
    { NULL, "PACKAGE", "package", pkg_publish,
      "DRAWER/A,CHANNEL/K/A=<dir>," TMPL_DESCRIBE ",SIGN/K=<keyfile>,ACCEPTKEY/K,DRYRUN/S" TMPL_ANY,
      "PUBLISH under another name: nothing reaches a portal until PUSH" },
    { NULL, "WITHDRAW", "withdraw", pkg_withdraw,
      "PACKAGE/A,VERSION/K/A,CHANNEL/K/A=<dir>,ARCH/K,SIGN/K=<keyfile>,DRYRUN/S" TMPL_ANY,
      "a version nothing installs any more" },
    { NULL, "PUSH", "push", pkg_push, "CHANNEL/K/A=<dir>,TO/K/A=<url>,SIGN/K=<keyfile>" TMPL_ANY,
      "a channel to the portal: https with PKG_PUSHKEY, or signed requests" },
    { NULL, "SIGN", "sign", pkg_sign, "FILE/A,KEY/K/A=<keyfile>,OUT/K/A=<sigfile>,SSH/S,NAMESPACE/K=<ns>" TMPL_ANY,
      "a detached signature; SSH: one ssh-keygen -Y verify checks" },
    { NULL, "CHECKSIG", "checksig", pkg_checksig, "SIGNED/A=<file>,FILE/K/A=<sigfile>,KEY/K=<public key>" TMPL_ANY,
      "whether SIGN's signature over a file is good, and whose it is" },
    { NULL, "IMAGE", "image", pkg_image, "DRAWER/A,OUT/K/A,NAME/K=<volume>" TMPL_ANY,
      "an FFS volume image of a drawer" },
};
#define NVERBS (sizeof verbs / sizeof verbs[0])

/* How a value is shown in usage, when the template does not say. */
static const char *shown(const char *name)
{
    static const struct { const char *name, *shown; } s[] = {
        { "PACKAGE", "<name>" }, { "DRAWER", "<drawer>" }, { "WORDS", "<word>" },
        { "LIBRARY", "<library>|<program>" }, { "FILE", "<file>" }, { "KEYFILE", "<keyfile>" },
        { "CHANNEL", "<dir|url|name>" }, { "ROOT", "<dir>" }, { "AT", "<dir>" },
        { "UNPACKED", "<dir>" }, { "FROM", "<dir>" }, { "VERSION", "v" }, { "ARCH", "cpu" },
        { "ACCEPTKEY", "<hex>" }, { "KIND", "k" }, { "OUT", "<file>" }, { "UNIT", "n" },
        { "HANDLER", "<path>" }, { "ARCHIVE", "<name>" }, { "NAME", "<name>" }
    };
    size_t i;
    for (i = 0; i < sizeof s / sizeof s[0]; i++)
        if (strcmp(s[i].name, name) == 0)
            return s[i].shown;
    return "<text>";
}

/* Where each keyword's value goes in pkg_options. */
static const struct { const char *kw; size_t off; } kws[] = {
    { "ROOT",      offsetof(struct pkg_options, root) },
    { "AT",        offsetof(struct pkg_options, at) },
    { "CHANNEL",   offsetof(struct pkg_options, channel) },
    { "NAME",      offsetof(struct pkg_options, name) },
    { "VERSION",   offsetof(struct pkg_options, version) },
    { "ARCH",      offsetof(struct pkg_options, arch) },
    { "KIND",      offsetof(struct pkg_options, kind) },
    { "FILE",      offsetof(struct pkg_options, file) },
    { "SIGN",      offsetof(struct pkg_options, sign) },
    { "KEY",       offsetof(struct pkg_options, key) },
    { "NAMESPACE", offsetof(struct pkg_options, nspace) },
    { "OUT",       offsetof(struct pkg_options, out) },
    { "ACCEPTKEY", offsetof(struct pkg_options, acceptkey) },
    { "DEPENDS",   offsetof(struct pkg_options, depends) },
    { "UNIT",      offsetof(struct pkg_options, unit) },
    { "HANDLER",   offsetof(struct pkg_options, handler) },
    { "FILES",     offsetof(struct pkg_options, files) },
    { "BUILD",     offsetof(struct pkg_options, build) },
    { "ARCHIVE",   offsetof(struct pkg_options, archive) },
    { "TO",        offsetof(struct pkg_options, to) },
    { "CONFIG",    offsetof(struct pkg_options, config) },
    { "UPSTREAM",  offsetof(struct pkg_options, upstream) },
    { "SHORT",     offsetof(struct pkg_options, short_desc) },
    { "DESCRIPTION", offsetof(struct pkg_options, description) },
    { "CATEGORY",  offsetof(struct pkg_options, category) },
    { "TAGS",      offsetof(struct pkg_options, tags) },
    { "AUTHOR",    offsetof(struct pkg_options, author) },
    { "HOMEPAGE",  offsetof(struct pkg_options, homepage) },
    { "REPOSITORY", offsetof(struct pkg_options, repository) },
    { "LICENSE",   offsetof(struct pkg_options, license) },
    { "DISTRIBUTION", offsetof(struct pkg_options, distribution) },
    { "CHANGES",   offsetof(struct pkg_options, changes) },
    { "ICON",      offsetof(struct pkg_options, icon) },
    { "SCREENSHOT", offsetof(struct pkg_options, screenshot) },
    { "README",    offsetof(struct pkg_options, readme) },
    { "INFO",      offsetof(struct pkg_options, info) },
    { "FROM",      offsetof(struct pkg_options, from) },
    { "UNPACKED",  offsetof(struct pkg_options, unpacked) }
};

/* And each switch. */
static const struct { const char *sw; size_t off; } sws[] = {
    { "DRYRUN",    offsetof(struct pkg_options, dryrun) },
    { "DOWNGRADE", offsetof(struct pkg_options, downgrade) },
    { "ORPHANS",   offsetof(struct pkg_options, orphans) },
    { "ALL",       offsetof(struct pkg_options, all) },
    { "SSH",       offsetof(struct pkg_options, ssh) },
    { "METADATA",  offsetof(struct pkg_options, metadata) }
};

/* Whether a word is a keyword or a switch of any verb: typed in capitals
 * where this verb does not take it, it is refused by name, since the person
 * meant it as a keyword and it would otherwise become a package's name. */
static int keyword_anywhere(const char *word)
{
    size_t i;
    for (i = 0; i < sizeof kws / sizeof kws[0]; i++)
        if (ieq(word, kws[i].kw)) return 1;
    for (i = 0; i < sizeof sws / sizeof sws[0]; i++)
        if (ieq(word, sws[i].sw)) return 1;
    return ieq(word, "SYSTEM") || ieq(word, "ENVIRONMENT") || ieq(word, "MACHINE")
        || ieq(word, "TRACE") || ieq(word, "LOG");
}

static int ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i] == '\0' || b[i] == '\0' || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* The verb a command names: two words first (CHANNEL ADD), then one. */
static const struct verb *find_verb(int argc, char **argv, int *words)
{
    size_t i;
    for (i = 0; i < NVERBS; i++) {
        const char *sp = strchr(verbs[i].verb, ' ');
        if (sp != NULL) {
            size_t n = (size_t)(sp - verbs[i].verb);
            if (argc > 2 && strlen(argv[1]) == n && ieq_n(argv[1], verbs[i].verb, n)
                && ieq(argv[2], sp + 1)) {
                *words = 2;
                return &verbs[i];
            }
        } else if (ieq(argv[1], verbs[i].verb)) {
            *words = 1;
            return &verbs[i];
        }
    }
    return NULL;
}

/* Whether any verb begins with this word and a second one (CHANNEL, ENV). */
static int verb_of_two(const char *word, char *seconds, size_t len)
{
    size_t i, at = 0;
    int found = 0;
    seconds[0] = '\0';
    for (i = 0; i < NVERBS; i++) {
        const char *sp = strchr(verbs[i].verb, ' ');
        if (sp == NULL || strlen(word) != (size_t)(sp - verbs[i].verb)
            || !ieq_n(word, verbs[i].verb, (size_t)(sp - verbs[i].verb)))
            continue;
        found = 1;
        if (at + 16 < len)
            at += (size_t)snprintf(seconds + at, len - at, "%s%s", at ? ", " : "", sp + 1);
    }
    return found;
}

/* The template of a verb, read; it is pkg's own text, so a failure is a
 * mistake in pkg, said as such. */
static int verb_args(const struct verb *v, struct pkg_args *a)
{
    char err[200];
    if (pkg_args_template(v->tmpl, a, err, sizeof err) != 0)
        return usage_errorf("the template of %s is wrong, which is a mistake in pkg: %s", v->verb, err);
    return 0;
}

/* MACHINE anywhere a switch can stand, the value of a keyword excepted:
 * `NAME machine` names a package. Read before the words are, so that a
 * usage error found on the way is answered as records too. */
static int wants_machine(const struct verb *v, int first, int argc, char **argv)
{
    struct pkg_args t;
    char err[200];
    int i;
    if (v == NULL || pkg_args_template(v->tmpl, &t, err, sizeof err) != 0) {
        for (i = first; i < argc; i++)
            if (ieq(argv[i], "MACHINE")) return 1;
        return 0;
    }
    for (i = first; i < argc; i++) {
        struct pkg_arg *it = pkg_args_item(&t, argv[i]);
        if (it != NULL && !(it->flags & PKG_ARG_SWITCH) && i + 1 < argc)
            i++;
        else if (ieq(argv[i], "MACHINE"))
            return 1;
    }
    return 0;
}

/* INSTALL and SEARCH take several words. They are kept here, since
 * pkg_options only points at them. */
static const char *also[PKG_ARGS_WORDS];

/* The words of a command, read against its verb's template, into options.
 * A verb of two words hands the library its second word as the target, as
 * the library has always taken CHANNEL ADD: target ADD, the channel after. */
static int read_words(const struct verb *v, int words, int argc, char **argv, struct pkg_options *a)
{
    static struct pkg_args t;
    char err[600];
    size_t i, k;

    memset(a, 0, sizeof *a);
    if (verb_args(v, &t) != 0)
        return -1;
    if (pkg_args_read(&t, v->verb, 1 + words, argc, argv, keyword_anywhere, err, sizeof err) != 0)
        return usage_errorf("%s", err);
    if (words == 2)
        a->target = strchr(v->verb, ' ') + 1;
    for (i = 0; i < t.n; i++) {
        struct pkg_arg *it = &t.item[i];
        if (!it->set)
            continue;
        if (!(it->flags & (PKG_ARG_KEY | PKG_ARG_SWITCH))) {
            /* taken by place: the target, then what follows it */
            size_t n = (it->flags & PKG_ARG_MULTI) ? it->nvalues : 1, j;
            for (j = 0; j < n; j++) {
                char *w = (char *)((it->flags & PKG_ARG_MULTI) ? it->values[j] : it->value);
                if (clean_value("the name", w) != 0)
                    return -1;
                /* A word in the place of a package is held to what a package
                 * name is, here, before a root is chosen: "INSTALL foo ROTO
                 * /tmp/r" once took ROTO and /tmp/r as two more names and went
                 * on in the default environment's root, the person's own
                 * system. A path, a volume or a word in capitals is never a
                 * package name. */
                if (strcmp(it->name, "PACKAGE") == 0) {
                    const char *why = pkg_check_name(w);
                    if (why != NULL) {
                        char lower[72];
                        size_t q;
                        for (q = 0; w[q] && q + 1 < sizeof lower; q++)
                            lower[q] = (char)tolower((unsigned char)w[q]);
                        lower[q] = '\0';
                        if (strcmp(lower, w) != 0 && pkg_check_name(lower) == NULL)
                            return usage_errorf("\"%s\" is not a package name: names are in lower case; "
                                                "did you mean %s?", w, lower);
                        return usage_errorf("\"%s\" is not a package name, which %s takes there: %s",
                                            w, v->verb, why);
                    }
                }
                if (a->target == NULL) {
                    a->target = w;
                } else {
                    if (a->nalso >= PKG_ARGS_WORDS)
                        return usage_errorf("%s takes at most %d names at once", v->verb, PKG_ARGS_WORDS);
                    also[a->nalso++] = w;
                    a->also = also;
                }
            }
            continue;
        }
        if (it->flags & PKG_ARG_SWITCH) {
            if (strcmp(it->name, "MACHINE") == 0) { machine = 1; continue; }
            if (strcmp(it->name, "SYSTEM") == 0) { environment_system = 1; continue; }
            for (k = 0; k < sizeof sws / sizeof sws[0]; k++)
                if (strcmp(it->name, sws[k].sw) == 0)
                    *(int *)(void *)((char *)a + sws[k].off) = 1;
            continue;
        }
        if (clean_value(it->name, (char *)it->value) != 0)
            return -1;
        if (it->value[0] == '\0')
            return usage_errorf("%s is given an empty value", it->name);
        if (strcmp(it->name, "TRACE") == 0) { trace_path = it->value; out_sink.trace = print_trace; continue; }
        if (strcmp(it->name, "LOG") == 0) { log_path = it->value; continue; }
        if (strcmp(it->name, "ENVIRONMENT") == 0) { chosen_environment = it->value; continue; }
        for (k = 0; k < sizeof kws / sizeof kws[0]; k++)
            if (strcmp(it->name, kws[k].kw) == 0)
                *(const char **)(void *)((char *)a + kws[k].off) = it->value;
    }
    if (a->at != NULL && a->nalso)
        return usage_errorf("AT takes one package; install each application separately");
    if (a->sign == NULL)
        a->sign = getenv("PKG_SIGNKEY");
    if (a->pushkey == NULL)
        a->pushkey = getenv("PKG_PUSHKEY");
    return 0;
}

/* ---- usage ------------------------------------------------------------ */

static int usage_is_error = 1;

static void usage_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (usage_is_error) pkg_verr(fmt, ap);
    else { char buf[2048]; vsnprintf(buf, sizeof buf, fmt, ap); pkg_out("%s", buf); }
    va_end(ap);
}

/* A verb's line of usage: its name in bold, its syntax wrapped under it at
 * the terminal's width, then what it does, dimmed. */
static void usage_verb(const struct verb *v, int with_template)
{
    int e = usage_is_error, width = pkg_style_caps(e)->width, col;
    const char *b = pkg_style_sgr(e, "1"), *d = pkg_style_sgr(e, "2"), *r = pkg_style_sgr(e, "0");
    struct pkg_args t;
    char syn[1600], err[200], *w;

    if (pkg_args_template(v->tmpl, &t, err, sizeof err) != 0)
        return;
    pkg_args_syntax(&t, shown, syn, sizeof syn);
    usage_line("  %s%-14s%s", b, v->verb, r);
    col = 16;
    /* word by word, never splitting a bracketed group across lines */
    for (w = syn; *w; ) {
        char *end = w;
        int depth = 0;
        size_t n;
        while (*end && !(*end == ' ' && depth == 0)) {
            if (*end == '[') depth++;
            else if (*end == ']') depth--;
            end++;
        }
        n = (size_t)(end - w);
        if (col > 16 && col + 1 + (int)n > width - 1) {
            usage_line("\n%18s", "");
            col = 18;
        } else if (col > 16) {
            usage_line(" ");
            col++;
        }
        usage_line("%.*s", (int)n, w);
        col += (int)n;
        w = *end ? end + 1 : end;
    }
    usage_line("\n                %s%s%s\n", d, v->what, r);
    if (with_template)
        usage_line("                %stemplate:%s %s\n", d, r, v->tmpl);
}

static int usage(void)
{
    int e = usage_is_error;
    const char *b = pkg_style_sgr(e, "1"), *d = pkg_style_sgr(e, "2"), *r = pkg_style_sgr(e, "0");
    size_t i;
    usage_line("%s%s%s  the AROS package tool\n", b, pkg_version_cookie + 6, r);
    usage_line("%susage:%s pkg VERB [<name>] KEYWORD <value> ...   keywords in any order, any case\n",
               d, r);
    for (i = 0; i < NVERBS; i++) {
        if (verbs[i].group)
            usage_line("\n%s%s%s\n", b, verbs[i].group, r);
        usage_verb(&verbs[i], 0);
    }
    usage_line("\n%sOn any verb%s\n", b, r);
    usage_line("  %s%-14s%s %s<file>: a copy of the output%s\n", b, "LOG", r, d, r);
    usage_line("  %s%-14s%s %skey: value lines for a program; the exit code names the class of a refusal%s\n",
               b, "MACHINE", r, d, r);
    usage_line("  %s%-14s%s %s<file>: every step, file, check and choice, for finding out why; - for stderr%s\n",
               b, "TRACE", r, d, r);
    usage_line("  %s%-14s%s %s<name>: the registered root to use, on any verb that takes ROOT%s\n",
               b, "ENVIRONMENT", r, d, r);
    usage_line("\n%sKinds%s     image (a program on one volume to mount), application (loose files), library,\n"
               "          device, class, font, catalog, startup, boot, data, sdk, slave\n", b, r);
    usage_line("%sSettings%s  PKG_SIGNKEY the key SIGN defaults to; PKG_PUSHKEY; PKG_OUTPUT=machine;\n"
               "          PKG_TRACE=<file>; PKG_COLOR=always|never; PKG_PROGRESS=1\n", b, r);
    usage_line("%sExit code%s 0 done; 10 to 18 refused, the number is the class; 20 a wrong command\n", b, r);
    usage_line("%sRoots%s     ROOT wins, then ENVIRONMENT, then the default environment. pkg U updates pkg\n"
               "          itself; pkg REMOVE, with no name, uninstalls it.\n", b, r);
    usage_line("%sAsking%s    pkg HELP shows this; pkg HELP <verb>, or pkg <verb> ?, one verb and its template;\n"
               "          pkg VERSION says which build this is\n", b, r);
    usage_line("%sAROS%s      pkg PORT [<portname>] serves every verb on an ARexx port, PKG by default\n", b, r);
    return PKG_RC_USAGE;
}

/* HELP MACHINE: everything the command line takes, as records, so that a
 * program, the portal or a test asks pkg what it accepts instead of reading
 * the usage or keeping a copy of the grammar. For each verb, in order:
 *
 *   verb:     INSTALL (or CHANNEL ADD: a verb of two words)
 *   what:     one line of what it does
 *   syntax:   the syntax as usage shows it
 *   template: the template it reads its words with (pkg_args.h)
 *   place:    <required|optional> <one|many> <shown>   a word taken by place
 *   keyword:  <NAME> <required|optional> <shown>        a keyword and its value
 *   switch:   <NAME>                                    a switch
 *
 * then the three verbs outside the table (HELP, VERSION, PORT) the same way,
 * and one `global:` record per word every verb takes, with `rooted` for the
 * one only verbs that take a root do. */
static void usage_records(void)
{
    static const struct { const char *verb, *what, *syntax; } apart[] = {
        { "HELP", "the usage; HELP <verb>, one verb and its template; HELP MACHINE, these records",
          "[<verb>] [MACHINE]" },
        { "VERSION", "which build this is: the release, the patch, the day it was built", "[MACHINE]" },
        { "PORT", "every verb served on an ARexx port, PKG by default (AROS)", "[<portname>]" }
    };
    size_t i, k;
    print_record(NULL, "result", "shown");
    print_record(NULL, "version", PKG_VERSION_STRING);
    for (i = 0; i < NVERBS; i++) {
        struct pkg_args t;
        char syn[1600], err[200], line[200];
        if (pkg_args_template(verbs[i].tmpl, &t, err, sizeof err) != 0)
            continue;
        pkg_args_syntax(&t, shown, syn, sizeof syn);
        print_record(NULL, "verb", verbs[i].verb);
        print_record(NULL, "what", verbs[i].what);
        print_record(NULL, "syntax", syn);
        print_record(NULL, "template", verbs[i].tmpl);
        for (k = 0; k < t.n; k++) {
            const struct pkg_arg *it = &t.item[k];
            const char *v = it->shown[0] ? it->shown : shown(it->name);
            const char *need = (it->flags & (PKG_ARG_NEEDED | PKG_ARG_ROOTED)) ? "required" : "optional";
            if (it->flags & PKG_ARG_GLOBAL)
                continue;
            if (it->flags & PKG_ARG_SWITCH) {
                print_record(NULL, "switch", it->name);
            } else if (it->flags & PKG_ARG_KEY) {
                snprintf(line, sizeof line, "%s %s %s", it->name, need, v);
                print_record(NULL, "keyword", line);
            } else {
                snprintf(line, sizeof line, "%s %s %s", need,
                         (it->flags & PKG_ARG_MULTI) ? "many" : "one", v);
                print_record(NULL, "place", line);
            }
        }
    }
    for (i = 0; i < sizeof apart / sizeof apart[0]; i++) {
        print_record(NULL, "verb", apart[i].verb);
        print_record(NULL, "what", apart[i].what);
        print_record(NULL, "syntax", apart[i].syntax);
    }
    print_record(NULL, "global", "MACHINE switch");
    print_record(NULL, "global", "TRACE keyword <file>");
    print_record(NULL, "global", "LOG keyword <file>");
    print_record(NULL, "global", "ENVIRONMENT keyword <name> rooted");
}

/* Environment files are optional. Prompts belong to the CLI; library callers
 * use the same configuration API and present their own choices. */
static int can_ask(void)
{
    if (machine || serving_port || !pkg_out_interactive(0)) return 0;
#ifdef __AROS__
    return Input() && IsInteractive(Input());
#elif defined(_WIN32)
    return _isatty(_fileno(stdin));
#else
    return isatty(STDIN_FILENO);
#endif
}

static int answer(char *buf, size_t len)
{
#ifdef __AROS__
    if (FGets(Input(), (STRPTR)buf, (LONG)len) == NULL) return -1;
#else
    if (fgets(buf, (int)len, stdin) == NULL) return -1;
#endif
    if (strchr(buf, '\n') == NULL && strlen(buf) + 1 == len) return -1;
    return clean_value("the answer", buf);
}

static void tell_root(const char *root, const char *source, const char *name)
{
    if (machine) {
        print_record(NULL, "selected-root", root);
        print_record(NULL, "root-source", source);
        if (name) print_record(NULL, "environment", name);
    } else {
        char line[8192];
        if (name) { snprintf(line, sizeof line, "Environment: %s\n", name); print_text(NULL, 1, line); }
        snprintf(line, sizeof line, "Root: %s\nSelected from: %s\n", root, source);
        print_text(NULL, 1, line);
    }
}

static int absolute_root(const char *path, char *buf, size_t len)
{
    if (!path || !*path) return usage_errorf("ROOT needs a non-empty directory");
#ifdef __AROS__
    if (!pkg_fs_fullpath(path, buf, len)) return usage_errorf("cannot resolve root %s", path);
#else
    int n;
    char cwd[4096];
#ifdef _WIN32
    if (path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':'))
#else
    if (path[0] == '/')
#endif
        n = snprintf(buf, len, "%s", path);
    else {
#ifdef _WIN32
        if (!_getcwd(cwd, sizeof cwd)) return usage_errorf("cannot read the current directory");
#else
        if (!getcwd(cwd, sizeof cwd)) return usage_errorf("cannot read the current directory");
#endif
        n = snprintf(buf, len, "%s/%s", cwd, path);
    }
    if (n < 0 || (size_t)n >= len) return usage_errorf("root path is too long");
#endif
    return 0;
}

static int environment_command(const struct pkg_options *a)
{
    struct pkg_environments e;
    char err[1024], root[4096], saved[4096];
    const char *action = a->target ? a->target : "LIST";
    const char *name = a->nalso == 1 ? a->also[0] : NULL;
    const char *path;
    size_t i;
    int rc = PKG_RC_USAGE;
    pkg_environments_init(&e);
    if (pkg_environments_load(&e, err, sizeof err) != 0) { usage_errorf("%s", err); goto end; }
    if (ieq(action, "LIST")) {
        if (a->nalso || a->root) { usage_errorf("ENV LIST takes no name or ROOT"); goto end; }
        if (machine) {
            if (e.system_path) print_record(NULL, "system-config", e.system_path);
            if (e.user_path) print_record(NULL, "user-config", e.user_path);
        } else {
            say_quiet("system configuration: %s", e.system_path ? e.system_path : "unavailable");
            say_quiet("personal configuration: %s", e.user_path ? e.user_path : "unavailable");
        }
        for (i = 0; i < e.count; i++) {
            if (environment_system && !e.items[i].system) continue;
            tell_root(e.items[i].root, e.items[i].source, e.items[i].name);
        }
        if (e.default_name) {
            if (machine) print_record(NULL, "default-environment", e.default_name);
            else say_quiet("default environment: %s", e.default_name);
        }
        if (!e.count && !machine)
            say_noted("no environment is registered; ROOT <directory> works without this configuration");
        rc = 0; goto end;
    }
    if (!name) { usage_errorf("ENV %s needs one environment name", action); goto end; }
    if (!ieq(action, "ADD") && !ieq(action, "REMOVE") && !ieq(action, "DEFAULT")) {
        usage_errorf("ENV takes ADD, LIST, REMOVE or DEFAULT"); goto end;
    }
    if (ieq(action, "ADD")) {
        if (absolute_root(a->root, root, sizeof root) != 0) goto end;
    } else if (a->root) { usage_errorf("ROOT belongs to ENV ADD"); goto end; }
    path = environment_system || on_aros ? e.system_path : e.user_path;
    if (!path) { usage_errorf("the configuration path for this scope is unavailable"); goto end; }
    /* A copy: saving reloads the configuration, which frees the string
     * path points at, and the result below still names the file. */
    snprintf(saved, sizeof saved, "%s", path);
    path = saved;
    if (machine) print_record(NULL, "configuration", path);
    else {
        say_quiet("environment configuration: %s", path);
        say_noted("this file names roots; a package's records stay in that root's .pkg drawer");
    }
    if (ieq(action, "ADD")) rc = pkg_environments_add(&e, name, root, environment_system || on_aros, err, sizeof err);
    else if (ieq(action, "REMOVE")) rc = pkg_environments_remove(&e, name, environment_system || on_aros, err, sizeof err);
    else rc = pkg_environments_default(&e, name, environment_system || on_aros, err, sizeof err);
    if (rc != 0) { usage_errorf("%s", err); rc = PKG_RC_USAGE; }
    else if (machine) print_record(NULL, "result", "configured");
    else say_done("environment configuration saved: %s", path);
end:
    pkg_environments_free(&e);
    return rc;
}

static int root_verb(const char *verb)
{
    return ieq(verb,"install") || ieq(verb,"upgrade") || ieq(verb,"rollback")
        || ieq(verb,"list") || ieq(verb,"verify") || ieq(verb,"repair")
        || ieq(verb,"remove") || ieq(verb,"mountlist") || ieq(verb,"status")
        || ieq(verb,"channel");
}

/* CHANNEL <word> where the word is neither a URL nor a directory that is
 * there: the root may know a channel by that name, and typing the name is
 * the whole point of having one. Resolved once, here, so that every verb
 * receives a channel and the library keeps taking addresses only. */
static char *channel_name_used;          /* freed at the end of the command */

/* The verbs that read a channel: a short name means one of the root's
 * channels there. PUBLISH, WITHDRAW and PUSH write into a directory, which
 * may not exist yet, and a word there is that directory, never a name. */
static int reads_channels(void)
{
    static const char *const r[] = { "install", "upgrade", "rollback", "status", "show",
                                     "search", "resolve", "repair" };
    size_t i;
    for (i = 0; i < sizeof r / sizeof r[0]; i++)
        if (strcmp(verb_name, r[i]) == 0) return 1;
    return 0;
}

static int a_name_not_a_place(const char *s)
{
    if (!reads_channels())
        return 0;
    if (s == NULL || strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0)
        return 0;
    return !pkg_fs_is_dir(s);
}

static int resolve_channel_name(struct pkg_options *a)
{
    char names[600], *real;
    if (a->root == NULL || !a_name_not_a_place(a->channel))
        return 0;
    real = pkg_channel_named(a->root, a->channel, names, (unsigned long)sizeof names);
    if (real == NULL) {
        /* Not a name this root knows. The word is left as it was: the verb
         * refuses it as it always did, and says the names there are. */
        if (names[0] != '\0' && !pkg_fs_exists(a->channel))
            say_quiet("%s knows no channel named %s; it knows %s", a->root, a->channel, names);
        return 0;
    }
    say_quiet("channel: %s (%s)", real, a->channel);
    free(channel_name_used);
    channel_name_used = real;
    a->channel = real;
    return 0;
}

static int prepare_root(struct pkg_options *a, struct pkg_environments *e, char *buf, size_t len)
{
    const struct pkg_environment *selected = NULL;
    char err[1024];
    int rc, required = root_verb(verb_name);
    size_t i;
    if (a->root) { tell_root(a->root, "command line (ROOT)", NULL); return resolve_channel_name(a); }
    if (!required && !chosen_environment
        && !(ieq(verb_name,"show") || ieq(verb_name,"search")) && !a_name_not_a_place(a->channel))
        return 0;
    /* An explicit channel alone keeps the established catalogue-only use,
     * unless it is a word that may be a name, which needs a root to look in. */
    if (!required && a->channel && !chosen_environment && !a_name_not_a_place(a->channel))
        return 0;
    if (pkg_environments_load(e, err, sizeof err) != 0) return usage_errorf("%s; specify ROOT explicitly to choose a root", err);
    rc = pkg_environments_select(e, chosen_environment, &selected, err, sizeof err);
    if (rc == 0) {
        if (!pkg_fs_is_dir(selected->root)) return usage_errorf("root %s from %s is unavailable; mount it or specify ROOT", selected->root, selected->source);
        a->root = selected->root;
        tell_root(a->root, selected->source, selected->name);
        return resolve_channel_name(a);
    }
    if (rc < 0) return usage_errorf("%s", err);
    if (rc == 1 && !required && !chosen_environment) return 0;
    if (!can_ask()) {
        if (rc == 1 && !chosen_environment) return 0; /* existing missing-ROOT diagnostic */
        return usage_errorf("%s; specify ROOT <directory> or ENVIRONMENT <name>", err);
    }
    if (!e->count) {
        say_noted("no environment is registered");
        say_asked("Which root directory is this operation for?");
        if (answer(buf, len) != 0 || !*buf) return usage_errorf("no root chosen; specify ROOT <directory>");
        if (!pkg_fs_is_dir(buf)) return usage_errorf("root %s is unavailable", buf);
        a->root = buf; tell_root(buf, "interactive choice (not saved)", NULL);
        return resolve_channel_name(a);
    }
    for (i = 0; i < e->count; i++)
        say_quiet("%lu. %s: %s (%s)", (unsigned long)i + 1, e->items[i].name,
                  e->items[i].root, e->items[i].source);
    say_asked("Which root is this operation for? Its number:");
    if (answer(buf, len) != 0) return usage_errorf("no environment chosen");
    { char *end; unsigned long number = strtoul(buf, &end, 10);
      if (!*buf || *end || number == 0 || number > e->count) return usage_errorf("choose a listed number or specify ROOT explicitly");
      selected = &e->items[number - 1]; }
    if (!pkg_fs_is_dir(selected->root)) return usage_errorf("root %s is unavailable", selected->root);
    a->root = selected->root; tell_root(a->root, selected->source, selected->name);
    return resolve_channel_name(a);
}

/* Standalone maintenance accepts only its documented switches. */
/* Not a verb. Name the word that was not understood and the verb it is
 * nearest to, instead of printing the whole usage and leaving the person to
 * find the difference. The version is part of the answer because a verb
 * this build does not have is usually a verb a later build does. */
static void not_a_verb(const char *word)
{
    static const char *const apart[] = { "HELP", "PORT", "VERSION" };
    /* The words another tool would have taken, answered with the verb pkg
     * has for them and not with whatever they look like: UNINSTALL is two
     * edits from INSTALL, and installing is the opposite of what the person
     * came to do. */
    static const struct { const char *typed, *verb; } habit[] = {
        { "UPDATE",    "UPGRADE" }, { "UNINSTALL", "REMOVE"  },
        { "DELETE",    "REMOVE"  }, { "ERASE",     "REMOVE"  },
        { "ADD",       "INSTALL" }, { "GET",       "INSTALL" },
        { "FETCH",     "INSTALL" }, { "INFO",      "SHOW"    },
        { "FIND",      "SEARCH"  }, { "QUERY",     "SEARCH"  },
        { "LS",        "LIST"    }, { "SYNC",      "STATUS"  }
    };
    const char *near = NULL, *instead = NULL;
    size_t best = 99, j, typed_len;
    char typed[64], why[256];

    for (j = 0; j + 1 < sizeof typed && word[j] != '\0'; j++)
        typed[j] = (char)toupper((unsigned char)word[j]);
    typed[j] = '\0';
    typed_len = j;
    for (j = 0; j < sizeof habit / sizeof habit[0]; j++)
        if (strcmp(typed, habit[j].typed) == 0)
            instead = habit[j].verb;
    for (j = 0; instead == NULL && j < NVERBS + sizeof apart / sizeof apart[0]; j++) {
        char cand[24];
        const char *c = j < NVERBS ? verbs[j].verb : apart[j - NVERBS];
        size_t len, shorter, d;
        /* a verb of two words is offered by its first: CHANNEL, ENV */
        snprintf(cand, sizeof cand, "%.*s", (int)strcspn(c, " "), c);
        len = strlen(cand);
        shorter = typed_len < len ? typed_len : len;
        d = pkg_name_edits(typed, cand);
        /* The rule SHOW uses for a package name: in a short word two edits
         * make another word, not a slip. */
        if (d <= (shorter <= 4 ? 1u : 2u) && d < best) {
            best = d;
            near = j < NVERBS ? verbs[j].verb : apart[j - NVERBS];
            if (strchr(near, ' ') != NULL) {
                static char first[24];
                snprintf(first, sizeof first, "%s", cand);
                near = first;
            }
        }
    }
    if (instead != NULL)
        snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; pkg says %s",
                 word, PKG_VERSION_STRING, instead);
    else if (near != NULL)
        snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; did you mean %s?",
                 word, PKG_VERSION_STRING, near);
    else
        /* Nothing near it: either the word is nonsense, or it is a verb a
         * later build has and this one does not. The second is what brings
         * people here, so the way out is part of the answer. */
        snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; a later build may "
                 "have it, and pkg U updates pkg itself", word, PKG_VERSION_STRING);
    pkg_usage_error(&out_sink, "pkg", why);
}

/* One entry for every caller: the command line below, and the ARexx port,
 * which runs each command it receives through here. Returns the exit code:
 * 0, a refusal class from 10 to 18, or 20 for usage. */
static int run_verb(int argc, char **argv)
{
    const struct verb *v = NULL;
    struct pkg_options a;
    struct pkg_environments environments;
    char root_choice[4096];
    int saved_machine = machine, words = 0;
    int rc = PKG_RC_USAGE;
    const char *env = getenv("PKG_OUTPUT");

    chosen_environment = NULL;
    environment_system = 0;
    if (argc >= 2 && ieq(argv[1], "U"))
        argv[1] = "UPGRADE";
    /* ENV alone is ENV LIST, as it has always been. */
    if (argc == 2 && ieq(argv[1], "ENV")) {
        static char *env_list[3];
        env_list[0] = argv[0]; env_list[1] = argv[1]; env_list[2] = "LIST";
        argv = env_list;
        argc = 3;
    }
    if (argc >= 2)
        v = find_verb(argc, argv, &words);
    machine = env != NULL && ieq(env, "machine");
    if (wants_machine(v, v ? 1 + words : 2, argc, argv))
        machine = 1;
    out_sink.structured = machine;
    out_sink.line = machine ? NULL : print_line;
    pkg_style_init(!serving_port && pkg_out_interactive(0),
                   !serving_port && pkg_out_interactive(1), on_aros);
    /* The activity line's mark holds a middle dot: one Latin-1 byte on the
     * AROS console, two UTF-8 bytes under a UTF-8 locale, and a full stop
     * where neither can be trusted. */
    pkg_activity_charset(on_aros ? PKG_ACTIVITY_LATIN1
                         : pkg_style_caps(0)->utf8 ? PKG_ACTIVITY_UTF8
                         : PKG_ACTIVITY_ASCII);
    trace_path = getenv("PKG_TRACE");
    out_sink.trace = trace_path != NULL && *trace_path ? print_trace : NULL;
    /* A person at a terminal is shown the activity line while a step is
     * long. PKG_PROGRESS decides it outright when it is set: 1 draws it
     * wherever the output goes, anything else switches it off. */
    {
        const char *p = getenv("PKG_PROGRESS");
        out_sink.progress = !machine
                            && (p != NULL && *p != '\0' ? *p == '1' : pkg_fs_interactive());
    }
    if (argc < 2) {
        if (machine) pkg_usage_error(&out_sink, "pkg", "no verb given");
        else usage();
        machine = saved_machine;
        return PKG_RC_USAGE;
    }
    if (ieq(argv[1], "HELP") || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        /* Asked for: to stdout, and a success. HELP <verb> is that verb
         * alone, with its template; HELP MACHINE, every verb as records. */
        usage_is_error = 0;
        if (machine) {
            usage_records();
        } else if (argc >= 3) {
            int any = 0;
            size_t i;
            for (i = 0; i < NVERBS; i++) {
                const char *vb = verbs[i].verb, *sp = strchr(vb, ' ');
                size_t n = sp ? (size_t)(sp - vb) : strlen(vb);
                if (strlen(argv[2]) != n || !ieq_n(argv[2], vb, n))
                    continue;
                if (argc >= 4 && (sp == NULL || !ieq(argv[3], sp + 1)))
                    continue;
                usage_verb(&verbs[i], 1);
                any = 1;
            }
            if (!any) {
                usage_is_error = 1;
                not_a_verb(argv[2]);
                machine = saved_machine;
                return PKG_RC_USAGE;
            }
        } else {
            usage();
        }
        usage_is_error = 1;
        machine = saved_machine;
        return PKG_RC_OK;
    }
    /* pkg ?: every verb and its template, as any command in C: answers a
     * question mark with its own. */
    if (argc == 2 && strcmp(argv[1], "?") == 0) {
        size_t i;
        for (i = 0; i < NVERBS; i++)
            pkg_out("%-14s %s\n", verbs[i].verb, verbs[i].tmpl);
        pkg_out("%-14s %s\n%-14s %s\n%-14s %s\n", "HELP", "VERB,MACHINE/S", "VERSION", "MACHINE/S",
                "PORT", "PORTNAME");
        machine = saved_machine;
        return PKG_RC_OK;
    }
    /* Which build is this? The first question asked of a tool that did not
     * do what someone expected, and the answer to most of them. */
    if (ieq(argv[1], "VERSION") || strcmp(argv[1], "--version") == 0
        || strcmp(argv[1], "-v") == 0) {
        if (machine) {
            print_record(NULL, "result", "version");
            print_record(NULL, "version", PKG_VERSION_STRING);
            print_record(NULL, "built", PKG_BUILD_DAY);
        } else {
            pkg_out("%s\n", pkg_version_cookie + 6);
        }
        machine = saved_machine;
        return PKG_RC_OK;
    }
    if (v == NULL) {
        char seconds[80];
        if (verb_of_two(argv[1], seconds, sizeof seconds)) {
            char up[24];
            size_t k;
            for (k = 0; k + 1 < sizeof up && argv[1][k]; k++)
                up[k] = (char)toupper((unsigned char)argv[1][k]);
            up[k] = '\0';
            /* the refusal names the verb as the library would */
            {
                static char low[24];
                for (k = 0; up[k]; k++) low[k] = (char)tolower((unsigned char)up[k]);
                low[k] = '\0';
                verb_name = low;
                pkg_style_verb(low);
            }
            if (argc > 2)
                usage_errorf("\"%s\" is not one of %s's words; they are %s", argv[2], up, seconds);
            else
                usage_errorf("%s takes %s", up, seconds);
        } else {
            not_a_verb(argv[1]);
        }
        machine = saved_machine;
        return PKG_RC_USAGE;
    }
    /* <verb> ?: the verb's usage and template, as an AmigaDOS command
     * answers a question mark. */
    if (argc == 2 + words && strcmp(argv[1 + words], "?") == 0) {
        usage_is_error = 0;
        usage_verb(v, 1);
        usage_is_error = 1;
        machine = saved_machine;
        return PKG_RC_OK;
    }
    verb_name = v->name;
    pkg_style_verb(verb_name);
    pkg_environments_init(&environments);
    rc = read_words(v, words, argc, argv, &a);
    /* PKG_CHECK_WORDS=1: the words are judged and nothing is run, so a test
     * holds every command the documentation and the portal show to the
     * parser itself, and no second copy of the grammar has to be kept. */
    if (rc == 0) {
        const char *cw = getenv("PKG_CHECK_WORDS");
        if (cw != NULL && strcmp(cw, "1") == 0) {
            if (machine) {
                print_record(NULL, "result", "words-taken");
                print_record(NULL, "verb", v->verb);
            } else {
                pkg_out("%s takes these words\n", v->verb);
            }
            pkg_environments_free(&environments);
            machine = saved_machine;
            return PKG_RC_OK;
        }
    }
    if (rc == 0 && strcmp(v->name, "upgrade") == 0 && !a.root && !a.all
        && (!a.target || ieq(a.target, "pkg")) && !chosen_environment) {
        /* pkg itself: from its own trusted channel, and nothing else */
        if (serving_port || a.channel || a.version || a.arch || a.acceptkey || a.downgrade || a.unpacked)
            rc = usage_errorf("self-update uses its trusted channel; use ROOT for a managed package upgrade");
        else
            rc = pkg_selfupdate(&out_sink, a.dryrun);
    } else if (rc == 0 && strcmp(v->name, "remove") == 0 && !a.target && !a.root && !a.orphans
               && !chosen_environment) {
        if (serving_port)
            rc = usage_errorf("self-removal takes REMOVE [DRYRUN]; name a package and ROOT for other removals");
        else
            rc = pkg_selfremove(&out_sink, a.dryrun);
    } else if (rc == 0) {
        if (v->fn != env_verb)
            rc = prepare_root(&a, &environments, root_choice, sizeof root_choice);
        if (rc == 0) rc = v->fn(&out_sink, &a);
    }
    if (rc < 0) rc = PKG_RC_USAGE;
    pkg_environments_free(&environments);
    pkg_style_flush(write_styled);
    machine = saved_machine;
    return rc;
}

static int pkg_main(int argc, char **argv)
{
    int rc;

    if (pkg_host_args(&argc, &argv) != 0) {
        pkg_err("pkg: cannot read the command line\n");
        return PKG_RC_IO;
    }

    /* PORT is not a verb the port itself may run, so it is handled here.
     * Under PKG_CHECK_WORDS its words are judged and no port is opened:
     * PORT takes one port name at most. */
    if (argc >= 2 && ieq(argv[1], "PORT") && getenv("PKG_CHECK_WORDS") != NULL
        && strcmp(getenv("PKG_CHECK_WORDS"), "1") == 0) {
        if (argc > 3) {
            pkg_err("pkg port: \"%s\" is more than PORT takes; it takes one port name\n", argv[3]);
            return PKG_RC_USAGE;
        }
        pkg_out("PORT takes these words\n");
        return PKG_RC_OK;
    }
    if (argc >= 2 && ieq(argv[1], "PORT")) {
        verb_name = "port";
        serving_port = 1;
        if (argc > 3) {
            usage();
            return PKG_RC_USAGE;
        }
        rc = pkg_port_serve(argc == 3 ? argv[2] : "PKG", run_verb);
        return rc == 0 ? PKG_RC_OK : PKG_RC_REFUSED;
    }
    return run_verb(argc, argv);
}

#ifdef __AROS__
#include <proto/exec.h>
#include <exec/tasks.h>

/* The Shell gives a command 40 KB of stack unless someone typed Stack, and
 * AROS's startup has no convention for a program to ask for more. Publishing
 * (the payload, the archive readers, the manifest) goes well past that, and
 * a stack overrun on AROS is a Software Failure, not a refusal. So Pkg runs
 * on a stack of its own whenever the one it was given is smaller. */
#define PKG_STACK_BYTES (1024ul * 1024ul)

struct entry_args { int argc; char **argv; };

static IPTR on_own_stack(struct entry_args *a)
{
    return (IPTR)pkg_main(a->argc, a->argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    struct entry_args a;
    UBYTE *stack;
    int rc;

    if ((IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower >= PKG_STACK_BYTES)
        return pkg_main(argc, argv);
    stack = (UBYTE *)AllocVec(PKG_STACK_BYTES, MEMF_ANY);
    if (stack == NULL) {
        pkg_err("pkg: not enough memory for a %lu KB stack\n", PKG_STACK_BYTES / 1024ul);
        return PKG_RC_IO;
    }
    a.argc = argc;
    a.argv = argv;
    sss.stk_Lower = stack;
    sss.stk_Upper = stack + PKG_STACK_BYTES;
    sss.stk_Pointer = sss.stk_Upper;
    ssa.Args[0] = (IPTR)&a;
    rc = (int)NewStackSwap(&sss, on_own_stack, &ssa);
    FreeVec(stack);
    return rc;
}
#else
int main(int argc, char **argv)
{
    return pkg_main(argc, argv);
}
#endif

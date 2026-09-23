/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * What every operation shares: failure classes, output in both forms,
 * refusals, the activity line, and the filesystem calls traced.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#define PKGI_UNTRACED_FS   /* this file defines the traced calls */
#include "pkg_internal.h"

/* ---- exit codes and failure classes ----------------------------------- *
 *
 * One table on every host. A refusal ends with the code of its class, and the
 * code is the same number on AROS, macOS and Windows, so a script written on
 * one reads the same on the others. Every refusal code is at least 10 because
 * AmigaDOS tests for failure with `If ERROR`, which is true from 10 upwards; a
 * usage error is 20, RETURN_FAIL. On POSIX and Windows any non-zero code is a
 * failure, so the AmigaDOS constraint costs them nothing. The ARexx port
 * returns the same number as RC.
 *
 * The first refusal a command meets decides its class. */

const char *pkg_class_name(int c)
{
    switch (c) {
    case PKGRC_OK:         return "ok";
    case PKGRC_NOTFOUND:   return "not-found";
    case PKGRC_INTEGRITY:  return "integrity";
    case PKGRC_SIGNATURE:  return "signature";
    case PKGRC_KEY:        return "key";
    case PKGRC_CONFLICT:   return "conflict";
    case PKGRC_DEPENDENCY: return "dependency";
    case PKGRC_IO:         return "io";
    case PKGRC_POLICY:     return "policy";
    case PKGRC_USAGE:      return "usage";
    default:             return "refused";
    }
}

/* ---- output ----------------------------------------------------------- *
 *
 * Everything an operation says goes to the caller's sink: fields to `record`
 * in the structured form, sentences to `text` in the other. The flag the
 * code below tests is `machine`, the structured form, a name kept from the
 * command line, where it is the MACHINE keyword. */

const struct pkg_sink *sink;
const char *verb_name = "pkg";
int refused_class;
int machine;              /* the structured form */
int dry_run;               /* every check, no write */

/* Text leaves through one of two doors. A sink with `line` gets each line
 * with its role and no framing; a sink with `text` alone gets the text as
 * the command line always printed it, framing and newlines included, so
 * the ARexx port and every other embedder see no change. `plain` is that
 * framed text; `kind` and `bare` the role and the unframed line for `line`,
 * split at its line breaks. */
static void emit_line(int kind, int is_error, const char *bare)
{
    char *copy, *p, *next;
    if (sink->line == NULL)
        return;
    copy = (char *)malloc(strlen(bare) + 1u);
    if (copy == NULL)
        return;
    strcpy(copy, bare);
    for (p = copy; p != NULL; p = next) {
        next = strchr(p, '\n');
        if (next != NULL)
            *next++ = '\0';
        if (next == NULL && *p == '\0' && p != copy)
            break;                       /* the newline that ended the text */
        sink->line(sink->user, kind, is_error, p);
    }
    free(copy);
}

static void emit(int is_error, const char *fmt, va_list ap)
{
    char small[1024], *big = NULL;
    va_list cp;
    int n;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof small) {
        big = (char *)malloc((size_t)n + 1u);
        if (big == NULL)
            return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
    }
    if (sink->line != NULL)
        emit_line(PKG_LINE_TEXT, is_error, big ? big : small);
    else
        sink->text(sink->user, is_error, big ? big : small);
    free(big);
}

/* A line with a role: `fmt` gives the bare line, and `frame` how the text
 * form wraps it ("%s" for none, "  hint: %s\n" for a hint). */
static void emit_kind(int kind, int is_error, const char *frame, const char *fmt, va_list ap)
{
    char small[1024], *big = NULL, *bare;
    va_list cp;
    int n;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof small) {
        big = (char *)malloc((size_t)n + 1u);
        if (big == NULL)
            return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
    }
    bare = big ? big : small;
    if (sink->line != NULL) {
        emit_line(kind, is_error, bare);
    } else {
        size_t need = strlen(bare) + strlen(frame) + 1u;
        char *framed = (char *)malloc(need);
        if (framed != NULL) {
            snprintf(framed, need, frame, bare);
            sink->text(sink->user, is_error, framed);
            free(framed);
        }
    }
    free(big);
}

void say_kind(int kind, const char *frame, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_kind(kind, 0, frame, fmt, ap);
    va_end(ap);
}

/* What the operation did, one sentence, no newline. */
/* A result that is bad news, drawn as such; still the operation's answer. */
/* "1 changed", "2 missing" or "1 changed, 2 missing". */
const char *damage(size_t changed, size_t missing)
{
    static char text[80];
    if (changed && missing)
        snprintf(text, sizeof text, "%lu changed, %lu missing", (unsigned long)changed,
                 (unsigned long)missing);
    else if (changed)
        snprintf(text, sizeof text, "%lu changed", (unsigned long)changed);
    else
        snprintf(text, sizeof text, "%lu missing", (unsigned long)missing);
    return text;
}
/* A line under a result: a count, a source, a key. */
/* A file or a package under a result, "kept\tC/Hello (edited)": a word,
 * a tab, the rest. The text form pads the word to eight columns, as the
 * command line always did. */
void say_item(const char *word, const char *fmt, ...)
{
    char rest[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rest, sizeof rest, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_ITEM, "%s", "%s\t%s", word, rest);
    else
        say_kind(PKG_LINE_ITEM, "  %s\n", "%-8s %s", word, rest);
}
/* A package's line in a batch: its name, then what became of it. */
void say_pkgline(const char *name, const char *fmt, ...)
{
    char rest[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rest, sizeof rest, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_ITEM, "%s", "%s\t%s", name, rest);
    else
        say_kind(PKG_LINE_ITEM, "%s\n", "%-24s %s", name, rest);
}
/* A remark under a result. */
/* A table: its header, its rows, cells apart by tabs, and its end. The text
 * form prints the rows with the column widths the command line always
 * used, `widths`, and no header. */
static const int *table_widths;
void tbl_head(const int *widths, const char *cells)
{
    table_widths = widths;
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_HEAD, "%s", "%s", cells);
}
void tbl_row(const char *fmt, ...)
{
    char cells[4096], line[4600], *p, *tab;
    int col = 0, at = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cells, sizeof cells, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL) {
        say_kind(PKG_LINE_ROW, "%s", "%s", cells);
        return;
    }
    for (p = cells; p != NULL; p = tab, col++) {
        int w = table_widths ? table_widths[col] : 0;
        tab = strchr(p, '\t');
        if (tab != NULL)
            *tab++ = '\0';
        if (tab != NULL)
            at += snprintf(line + at, sizeof line - (size_t)at, "%-*s ", w, p);
        else
            at += snprintf(line + at, sizeof line - (size_t)at, "%s", p);
        if ((size_t)at >= sizeof line - 1u)
            break;
    }
    say_kind(PKG_LINE_ROW, "%s\n", "%s", line);
}
void tbl_end(void)
{
    table_widths = NULL;
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_END, "%s", "%s", "");
}

/* The operation's account of itself, for the sink's trace, if it has one. */
void tr(const char *fmt, ...)
{
    char line[1400];
    int n;
    va_list ap;
    if (sink == NULL || sink->trace == NULL)
        return;
    n = snprintf(line, sizeof line, "%s: ", verb_name);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    sink->trace(sink->user, line);
}

/* Text for a person: the answer, or a refusal or warning. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(0, fmt, ap);
    va_end(ap);
}

/* ---- the activity line ------------------------------------------------ *
 *
 * What pkg is doing while a step takes time, and that the step is
 * advancing: see pkg_activity.h. A step that can be long says what it is
 * doing with `doing`, hands the module the work as it advances, and ends
 * with `did`. The module alone decides when a line appears, how fast the
 * mark moves and what the measure says; this file knows only the verbs.
 *
 * It leaves on the sink's own channel for it, so a program using libpkg
 * receives the same texts and draws them in its own way. Nothing is drawn in
 * machine output, and nothing where nobody is watching: pkg_sink.progress
 * says whether anybody is. */
/* The sentence that announces a step: an ordinary line that stays. */
void activity_say(void *user, const char *text)
{
    (void)user;
    if (sink == NULL || machine)
        return;
    say_kind(PKG_LINE_NOTE, "%s\n", "%s", text);
}

void activity_show(void *user, const char *text)
{
    (void)user;
    if (sink == NULL)
        return;
    if (sink->line != NULL)
        sink->line(sink->user, PKG_LINE_PROGRESS, 0, text);
    else if (text[0] != '\0')
        say("\r  %s", text);
    else
        say("\r%*s\r", 60, "");
}

/* A step begins. The two forms say how big the work is when that is known,
 * so the sentence that announces it can be printed before the wait and not
 * after it: bytes for a file, a count of things otherwise. */
/* A file's size, for the sentence that announces the work, or 0 when it
 * cannot be had: a step that does not know says nothing about its size. */
long long file_bytes(const char *path)
{
    FILE *f = fopen(path, "rb");
    long long n = 0;
    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) == 0)
        n = (long long)ftell(f);
    fclose(f);
    return n > 0 ? n : 0;
}

void doing(const char *verb, const char *object)
{
    pkg_activity_step(verb, object, 0, PKG_ACTIVITY_NOTHING, NULL);
}

void doing_bytes(const char *verb, const char *object, long long whole)
{
    pkg_activity_step(verb, object, whole, PKG_ACTIVITY_BYTES, NULL);
}

void doing_things(const char *verb, const char *object, long long n, const char *word)
{
    pkg_activity_step(verb, object, n, PKG_ACTIVITY_THINGS, word);
}

void did(void)
{
    pkg_activity_done();
}

/* A counter under a step already named: "checking hello  57 of 208". */
void counting(size_t i, size_t n)
{
    pkg_activity_count((unsigned long long)i, (unsigned long long)n, NULL);
}

/* The last part of a path, which is the name a person gave the file. */
static const char *base_name(const char *path)
{
    const char *p = path + strlen(path);
    while (p > path && p[-1] != '/' && p[-1] != '\\' && p[-1] != ':')
        p--;
    return *p != '\0' ? p : path;
}

/* A file named for the activity line. A channel names what it holds by its
 * digest, and sixty-four hex characters say nothing on a line: those are cut
 * to the first twelve, the form the result sentences use ("payload
 * c9e2bc15e3dc"). Every other name is left as it is. */
void short_name(char *out, size_t ol, const char *path)
{
    const char *name = base_name(path), *dot = strrchr(name, '.');
    size_t stem = dot != NULL ? (size_t)(dot - name) : strlen(name);
    size_t i;
    if (stem == PKG_SHA256_HEXLEN) {
        for (i = 0; i < stem; i++)
            if (!isxdigit((unsigned char)name[i]))
                break;
        if (i == stem) {
            snprintf(out, ol, "%.12s%s", name, dot != NULL ? dot : "");
            return;
        }
    }
    snprintf(out, ol, "%s", name);
}

/* The work advancing, from the layers that do it. */
void on_transfer(long long done, long long total)
{
    pkg_activity_bytes(done, total);
}
void on_wait(const char *host)
{
    pkg_activity_waiting(host);
}
void on_archive_read(long long done, long long total)
{
    pkg_activity_bytes(done, total);       /* an archive is read in bytes, and says so */
}

/* A file arriving over the network, named as a person would name it. */
int net_get_watched(const char *url, const char *dest, char *err, size_t errlen)
{
    char name[120];
    int rc;
    short_name(name, sizeof name, url);
    doing("downloading", name);
    rc = pkg_net_get(url, dest, err, errlen);
    did();
    return rc;
}

void say_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(1, fmt, ap);
    va_end(ap);
}

void say_raw(const char *buf, size_t len)
{
    char *s;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    s = (char *)malloc(len + 1u);
    if (s == NULL)
        return;
    memcpy(s, buf, len);
    s[len] = '\0';
    if (sink->line != NULL)
        emit_line(PKG_LINE_TEXT, 0, s);
    else
        sink->text(sink->user, 0, s);
    free(s);
}

/* A result word, or its conditional under a dry run: "installed" becomes
 * "would-install", so a dry run can never be read as the real thing. */
const char *res(const char *done, const char *would)
{
    return dry_run ? would : done;
}

/* A result field, in the structured form only: "key: value", the
 * manifest's syntax. */
/* A record is one line whatever went into it: a reason with line breaks, a
 * name or comment a person typed with a tab or a stray control character.
 * Every value leaves through here or rec_item, so none can break the
 * key: value form a reader relies on. */
void one_line(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F)
            *s = ' ';
}

void kv(const char *key, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    if (!machine || sink == NULL || sink->record == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    one_line(buf);
    sink->record(sink->user, key, buf);
}

/* The closing sentence: a summary record for a program, the sentence
 * itself for a person. */
void summary_line(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kv("summary", "%s", buf);
    if (!machine)
        say_result("%s", buf);
}

/* A record with several fields: the joined value goes to `record`, as the
 * command line prints it, and the fields one by one to `item`, for a program
 * that should not have to split strings. Key/value pairs, NULL-terminated. */
void rec_item(const char *kind, const char *joined, ...)
{
    const char *keys[12], *vals[12];
    char *clean[12], line[4096];
    int n = 0, i;
    va_list ap;
    if (!machine || sink == NULL)
        return;
    snprintf(line, sizeof line, "%s", joined);
    one_line(line);
    if (sink->record != NULL)
        sink->record(sink->user, kind, line);
    if (sink->item == NULL)
        return;
    va_start(ap, joined);
    while (n < 12) {
        const char *k = va_arg(ap, const char *);
        const char *v;
        if (k == NULL)
            break;
        v = va_arg(ap, const char *);
        keys[n] = k;
        clean[n] = (char *)malloc(strlen(v ? v : "") + 1);
        if (clean[n] != NULL) { strcpy(clean[n], v ? v : ""); one_line(clean[n]); }
        vals[n] = clean[n] ? clean[n] : "";
        n++;
    }
    va_end(ap);
    sink->item(sink->user, kind, n, keys, vals);
    for (i = 0; i < n; i++) free(clean[i]);
}

/* What an agent, or a person, should do after a refusal. Every refusal says
 * it, as `next` in the structured form and as a last line in the text one,
 * so the answer to "what now?" never has to be guessed from the reason's
 * wording. The reasons themselves never hand over a ready-made command that
 * overrides a safeguard (ACCEPTKEY, DOWNGRADE, removing something): those
 * steps belong to whoever requested the operation, a person or the agent
 * that launched this one, and `ask-requester` says so. */
const char *next_default(int cls)
{
    switch (cls) {
    case PKGRC_NOTFOUND:   return "check-name";
    case PKGRC_INTEGRITY:  return "stop";
    case PKGRC_SIGNATURE:  return "stop";
    case PKGRC_KEY:        return "ask-requester";
    case PKGRC_CONFLICT:   return "ask-requester";
    case PKGRC_DEPENDENCY: return "ask-requester";
    case PKGRC_POLICY:     return "ask-requester";
    case PKGRC_USAGE:      return "fix-command";
    default:               return "report";
    }
}

/* The command line's sentence for a `next` value, naming its commands. */
const char *next_cli_words(const char *next)
{
    if (next == NULL)
        return "report this to whoever requested it";
    if (strcmp(next, "stop") == 0)
        return "stop here: the bytes or signatures are not what was published, and "
               "no keyword or other channel makes that safe";
    if (strcmp(next, "ask-requester") == 0)
        return "ask whoever requested this (the person, or the agent that launched you); "
               "it is their decision, not a step to take for them";
    if (strcmp(next, "fix-command") == 0)
        return "fix the command; pkg HELP lists the verbs and keywords";
    if (strcmp(next, "check-name") == 0)
        return "check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, "
               "pkg LIST ROOT <dir> what a root holds";
    if (strcmp(next, "use-upgrade") == 0)
        return "to move to that version, UPGRADE instead of INSTALL";
    if (strcmp(next, "use-install") == 0)
        return "it is not installed there; INSTALL it instead";
    if (strcmp(next, "retry-later") == 0)
        return "give the same command again once the other change is done";
    if (strcmp(next, "install-dependency-first") == 0)
        return "INSTALL that dependency first, with KEY naming its own publisher's key, "
               "then this package";
    return "report this to whoever requested it";
}

/* The same, in words for any front end: no command is named. */
const char *pkg_next_words(const char *next)
{
    if (next == NULL || strcmp(next, "report") == 0)
        return "Nothing more can be done from here.";
    if (strcmp(next, "stop") == 0)
        return "Do not go on: this is not what its publisher published, and no other copy "
               "or option makes it safe.";
    if (strcmp(next, "ask-requester") == 0)
        return "This is your decision. Read the reason; if you agree, confirm and it will "
               "be done that way.";
    if (strcmp(next, "fix-command") == 0)
        return "Something in the request was wrong; correct it and try again.";
    if (strcmp(next, "check-name") == 0)
        return "That name is not there. Choose from what is offered.";
    if (strcmp(next, "use-upgrade") == 0)
        return "Another version is installed; upgrade it to this one instead.";
    if (strcmp(next, "use-install") == 0)
        return "It is not installed yet; install it instead.";
    if (strcmp(next, "retry-later") == 0)
        return "Another change of this system is under way; try again when it is done.";
    if (strcmp(next, "install-dependency-first") == 0)
        return "It needs another package whose publisher is not known yet; install that one "
               "first, from its own publisher.";
    return "Nothing more can be done from here.";
}

const char *pending_next;   /* set by refuse_n for the next refusal */
const char *refused_next;
int quiet;                  /* SHOW checks entries without reporting refusals */
char quiet_reason[2048];

int refuse_c(int cls, const char *fmt, ...)
{
    char buf[2048], *q;
    va_list ap;
    const char *next = pending_next ? pending_next : next_default(cls);
    pending_next = NULL;
    if (refused_class == 0) {
        refused_class = cls;
        refused_next = next;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tr("refused, %s (%d), next %s: %s", class_name(cls), cls, next, buf);
    if (quiet) {
        snprintf(quiet_reason, sizeof quiet_reason, "%s", buf);
        return 1;
    }
    if (machine) {
        char code[8];
        one_line(buf);
        (void)q;
        snprintf(code, sizeof code, "%d", refused_class);
        if (sink && sink->record) {
            sink->record(sink->user, "result", "refused");
            sink->record(sink->user, "class", class_name(refused_class));
            sink->record(sink->user, "code", code);
            sink->record(sink->user, "reason", buf);
            sink->record(sink->user, "next", refused_next);
        }
        return 1;
    }
    if (sink && sink->line) {
        emit_line(PKG_LINE_REFUSAL, 1, buf);
        emit_line(PKG_LINE_NEXT, 1, next_words(refused_next));
    } else if (sink && sink->text) {
        char *line;
        size_t n = strlen(verb_name) + strlen(buf) + strlen(next_words(refused_next)) + 32;
        line = (char *)malloc(n);
        if (line != NULL) {
            snprintf(line, n, "pkg %s: %s\n  next: %s\n", verb_name, buf, next_words(refused_next));
            sink->text(sink->user, 1, line);
            free(line);
        }
    }
    return 1;
}


/* A warning is not a failure and leaves the operation's class alone. */
void warn(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("warning", "%s", buf);
    else if (sink != NULL && sink->line != NULL)
        emit_line(PKG_LINE_WARNING, 1, buf);
    else
        say_result("pkg %s: warning: %s", verb_name, buf);
}

/* What usually comes next after a success, or what is worth telling the
 * person: never a command that overrides a safeguard. */
void hint(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("hint", "%s", buf);
    else
        say_kind(PKG_LINE_HINT, "  hint: %s\n", "%s", buf);
}

void short12(const char *hex, char out[13])
{
    memcpy(out, hex, 12);
    out[12] = '\0';
}

void tohex(const unsigned char *b, size_t n, char *hex)
{
    static const char dg[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        hex[2 * i] = dg[b[i] >> 4];
        hex[2 * i + 1] = dg[b[i] & 15];
    }
    hex[2 * n] = '\0';
}

int fromhex(unsigned char *b, size_t n, const char *hex)
{
    size_t i;
    if (hex == NULL || strlen(hex) != 2 * n)
        return -1;
    for (i = 0; i < 2 * n; i++)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
            return -1;
    for (i = 0; i < n; i++) {
        int hi = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        int lo = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        b[i] = (unsigned char)(hi * 16 + lo);
    }
    return 0;
}

/* ---- the filesystem, traced ------------------------------------------ *
 *
 * Every file operation goes through these, so the trace shows each one. */

int t_read(const char *path, unsigned char **buf, size_t *len)
{
    int rc = pkg_fs_read(path, buf, len);
    if (rc == 0) tr("read %s, %lu bytes", path, (unsigned long)*len);
    else tr("read %s: absent or unreadable", path);
    return rc;
}

int t_write(const char *path, const void *buf, size_t len)
{
    int rc = pkg_fs_write_atomic(path, buf, len);
    tr("write %s, %lu bytes%s", path, (unsigned long)len, rc == 0 ? "" : ": FAILED");
    return rc;
}

int t_write_private(const char *path, const void *buf, size_t len)
{
    int rc = pkg_fs_write_private(path, buf, len);
    tr("write %s, %lu bytes, owner only%s", path, (unsigned long)len, rc == 0 ? "" : ": FAILED");
    return rc;
}

int t_rename(const char *from, const char *to)
{
    int rc = pkg_fs_rename(from, to);
    tr("move %s -> %s%s", from, to, rc == 0 ? "" : ": FAILED");
    return rc;
}

int t_unlink(const char *path)
{
    int rc = pkg_fs_unlink(path);
    tr("delete %s%s", path, rc == 0 ? "" : ": FAILED");
    return rc;
}

int t_rmtree(const char *path)
{
    int rc = pkg_fs_rmtree(path);
    tr("clear %s%s", path, rc == 0 ? "" : ": FAILED");
    return rc;
}

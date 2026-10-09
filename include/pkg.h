/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * libpkg: Pkg as a library, for any program that wants to publish, install
 * or inspect packages without running the command line: a graphical front
 * end, an installer, an IDE plug-in, an agent's tool adapter. The `pkg`
 * command is one client of it among others, and adds nothing but argument
 * parsing, help text and the ARexx port.
 *
 * Each operation takes its options and a sink, and returns a code:
 *
 *   0        done;
 *   10..18   refused, the number naming the class (PKG_RC_*);
 *   20       the request itself is wrong (a missing option, say).
 *
 * The sink receives the answer in one of two forms, chosen by `structured`:
 *
 *   structured = 1   `record` is called once per field, in order, with the
 *                    same keys and values as the command line's MACHINE
 *                    output: a `result` field in every answer, and for a
 *                    refusal `result: refused`, `class`, `code`, `reason`,
 *                    and `next`, what to do now (stop, ask-requester,
 *                    fix-command, check-name, use-upgrade, use-install,
 *                    report). This is the form for programs.
 *   structured = 0   `text` is called with lines written for a person,
 *                    `is_error` set for refusals and warnings.
 *
 * Nothing is written to stdout or stderr, and no environment variable is
 * read: PKG_SIGNKEY, PKG_OUTPUT and PKG_TRACE belong to the command line.
 *
 * `trace`, when set, receives the operation's own account of itself, one
 * line per step, whatever the form: every file read, written, renamed or
 * removed, every check with what was expected and what was found, every
 * choice with its reason (the version picked, what satisfied a dependency,
 * which files were left out, where the architecture came from), and each
 * refusal as it happens. It is for finding out why an operation did what it
 * did; nothing in it is part of the contract, and its wording may change.
 *
 * A decision that belongs to whoever requested the operation (a person, or
 * a supervising agent that holds the authority) is never taken here. A
 * refusal whose `next` is ask-requester (a new signing key, a downgrade, a
 * file someone edited) goes to them; if they agree, the caller repeats the
 * operation with the option that expresses it (acceptkey, downgrade).
 *
 * Trust: a root pins, per package, the key that signed it the first time it
 * was installed there, and refuses another key later (14, ask-requester). The
 * key of a package's first version in a channel is presumed its
 * publisher's: a first install, or a publish, signed by another key is
 * refused the same way. `acceptkey`, with the other key in full, is how the
 * person's confirmation is passed.
 *
 * Linking: `make build/libpkg.a` builds everything, host layer included; a
 * program links that archive and nothing else.
 *
 * One operation runs at a time in a process: the library keeps the state of
 * the running operation in static storage, as the AmigaDOS tools it sits
 * beside do. A front end that runs operations in the background serialises
 * them.
 */

#ifndef PKG_H
#define PKG_H

#define PKG_API_VERSION 1   /* stays 1 until the first official release */
/* The tool's own version, as in its $VER: a release, a number raised when
 * something in that release changes, and the day this binary was built. The
 * build carries them in, so what runs always says which day it came from; the
 * defaults here are for anything that compiles pkg.h on its own. A package
 * orders them as Pkg orders any version: 1.8.0+20260923 comes after 1.8, a
 * later day after an earlier one, and a raised PKG_VERSION_PATCH after both. */
#ifndef PKG_VERSION_RELEASE
#define PKG_VERSION_RELEASE "1.8"
#endif
#ifndef PKG_VERSION_PATCH
#define PKG_VERSION_PATCH "0"
#endif
#ifndef PKG_BUILD
#define PKG_BUILD "00000000"      /* a build that did not say: never published */
#endif
#ifndef PKG_BUILD_DAY
#define PKG_BUILD_DAY "unknown day"
#endif
#define PKG_VERSION_STRING PKG_VERSION_RELEASE "." PKG_VERSION_PATCH "+" PKG_BUILD

/* What every request to a channel says about this Pkg: its version, the
 * system and the CPU it was built for, all known when it is compiled. The
 * portal counts requests by these three and keeps nothing else about who
 * made them (its /privacy page says so). */
#if defined(__AROS__)
#define PKG_UA_SYSTEM "aros"
#elif defined(__APPLE__)
#define PKG_UA_SYSTEM "macos"
#elif defined(_WIN32)
#define PKG_UA_SYSTEM "windows"
#elif defined(__linux__)
#define PKG_UA_SYSTEM "linux"
#else
#define PKG_UA_SYSTEM "other"
#endif
#if defined(__aarch64__)
#define PKG_UA_CPU "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
#define PKG_UA_CPU "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#define PKG_UA_CPU "i386"
#elif defined(__m68k__) || defined(__mc68000__)
#define PKG_UA_CPU "m68k"
#elif defined(__powerpc__) || defined(__PPC__)
#define PKG_UA_CPU "ppc"
#elif defined(__arm__)
#define PKG_UA_CPU "arm"
#else
#define PKG_UA_CPU "other"
#endif
#define PKG_USER_AGENT "Pkg/" PKG_VERSION_STRING " (" PKG_UA_SYSTEM "; " PKG_UA_CPU ")"

enum {
    PKG_RC_OK         = 0,
    PKG_RC_REFUSED    = 10,  /* a refusal with no better class */
    PKG_RC_NOTFOUND   = 11,  /* package, version, installed entry or channel object absent */
    PKG_RC_INTEGRITY  = 12,  /* a digest, container, manifest or database disagrees */
    PKG_RC_SIGNATURE  = 13,  /* unsigned, malformed or invalid signature */
    PKG_RC_KEY        = 14,  /* signed by a key other than the one pinned */
    PKG_RC_CONFLICT   = 15,  /* something is already there */
    PKG_RC_DEPENDENCY = 16,  /* a dependency cannot be satisfied, or is still needed */
    PKG_RC_IO         = 17,  /* the filesystem refused */
    PKG_RC_POLICY     = 18,  /* allowed only with an explicit option, such as downgrade */
    PKG_RC_USAGE      = 20   /* the request is wrong */
};

struct pkg_event;

/* Every callback runs on the thread that called the operation, before the
 * operation returns. The strings it receives are valid only during the
 * callback: copy what you keep. Any callback may be NULL; with structured
 * set, `text` is never called, and without it `record` and `item` are never
 * called. */
struct pkg_sink {
    void (*record)(void *user, const char *key, const char *value);
    void (*text)(void *user, int is_error, const char *text);
    void *user;
    int   structured;
    /* Optional: the operation's account of itself, see above. */
    void (*trace)(void *user, const char *line);
    /* Optional, structured form: a record that has several fields also
     * arrives here with its fields apart, so nothing has to split strings.
     * `kind` is the record's key; `keys` and `values` hold `n` fields, in the
     * order the joined value lists them. Each item arrives right after its
     * joined `record`, in the same order. The records concerned, and their
     * fields, are listed below; pkg_field finds one by name. */
    void (*item)(void *user, const char *kind, int n,
                 const char *const *keys, const char *const *values);
    /* Optional: asked between steps; non-zero stops the operation, which is
     * refused (10, next report) with anything it had placed taken back out,
     * files, records and the keys it pinned alike. It is asked once per
     * package while resolving, once before each package is placed, once
     * per entry while SHOW checks a channel, and once per package while
     * STATUS compares; placing one package's files is not interrupted.
     * With `event`, the step open then ends CANCELLED, the taking back is a
     * RESTORE step that can itself fail, and OP_END says both. */
    int  (*cancel)(void *user);
    /* Non-zero when a person watches: long steps then show a counter, a line
     * of `text` that starts with '\r' and is rewritten in place. A caller
     * that keeps the output (a log) leaves such lines out. */
    int   progress;
    /* Optional, text form: each line with its role, for a front end that
     * lays the text out (a terminal with colour, a window). When set, `text`
     * is not called; every line arrives here instead, without its newline
     * and without the framing `text` adds ("pkg install: ", "  hint: "),
     * which the front end draws in its own way. A table arrives as one
     * PKG_LINE_HEAD, its PKG_LINE_ROWs, cells separated by tabs, and a
     * PKG_LINE_END, so the front end can size the columns. `is_error`
     * follows `text`: non-zero for a refusal, its next step and a warning. */
    void (*line)(void *user, int kind, int is_error, const char *text);
    /* Optional: what the operation is doing, as typed facts, in either form
     * and whatever `progress` says. See "Progress events" below. */
    void (*event)(void *user, const struct pkg_event *e);
    /* Optional: milliseconds from a clock that never goes back, used only to
     * space out STEP_PROGRESS events. NULL uses the host's monotonic clock;
     * a test passes its own to need no sleep. */
    long long (*clock)(void *user);
};

/* The role of a line handed to pkg_sink.line. */
enum pkg_line {
    PKG_LINE_TEXT = 0,  /* a line with no particular role */
    PKG_LINE_RESULT,    /* what the operation did: "installed hello 1.2 into SYS:" */
    PKG_LINE_PROBLEM,   /* a result that is bad news: "2 of 3 packages damaged" */
    PKG_LINE_DETAIL,    /* an item under a result: "3 files, signed by 26ffb2bc" */
    PKG_LINE_ITEM,      /* a file or package under a result: "kept\tC/Hello (edited)" */
    PKG_LINE_NOTE,      /* a remark worth reading, not a warning */
    PKG_LINE_HINT,      /* what usually comes next */
    PKG_LINE_WARNING,   /* something to look at; the operation went on */
    PKG_LINE_REFUSAL,   /* why the operation was refused */
    PKG_LINE_NEXT,      /* the step after a refusal */
    PKG_LINE_HEAD,      /* a table's header, cells separated by tabs */
    PKG_LINE_ROW,       /* a table's row, cells separated by tabs */
    PKG_LINE_END,       /* the table ends; the text is empty */
    PKG_LINE_PROGRESS,  /* a counter to draw in place, empty when the step ends */
    PKG_LINE_QUESTION   /* a question waiting for an answer on the terminal, the
                           one line a person must read before typing: it carries
                           its own choices, as in "... ? [y/N]" */
};

/* ---- Progress events --------------------------------------------------- *
 *
 * pkg_sink.event receives what an operation is doing as facts a front end
 * can show without reading a sentence: the operation begins, steps begin,
 * advance, wait and end, and the operation ends. docs/events.md has the
 * sequences each verb produces and an example of a sink that queues them
 * for a UI thread.
 *
 * Order. For every call through this interface that is given a sink with
 * `event`, OP_BEGIN comes first, before any work, and exactly one OP_END
 * comes last, before the call returns: also for a refused request (20), a
 * refusal, a cancel and a failure. Between them, steps nest: a step that
 * begins while another is open has it as `parent`, and ends before it. Every
 * step that begins ends, with the outcome it really had.
 *
 * Identity. `op` numbers the operations of the process from 1. `step`
 * numbers the steps of one operation from 1, never reused within it; a step
 * is (op, step). `attempt` is 1, or more when the same work starts again
 * from the beginning (a download that restarted): the earlier attempt ends
 * RETRIED and a new step begins, so a counter never goes back within a step.
 *
 * Counters. `done` and `total` are 64 bits, absolute, in the step's `unit`,
 * counting what `measure` says and nothing else. `done` never decreases
 * within a step. `total` is meaningful only when `total_known` is non-zero;
 * a known total of 0 is an empty file, not an unknown one. A total may
 * become known during a step (a STEP_PROGRESS says so at once); a total
 * that turns out wrong becomes unknown for the rest of the step, never a
 * share above 100%. The library gives facts per step, not a percentage of
 * the whole operation: placing the files is not the end of an install, the
 * records, the attributes and the write-back to the medium follow.
 *
 * Pace. STEP_BEGIN, STEP_WAIT, STEP_END and the operation events are never
 * held back. STEP_PROGRESS is sent at most once per PKG_EVENT_GAP_MS of
 * pkg_sink.clock per step, except when a total becomes known or is found
 * wrong; STEP_END carries the last counters. A wait is said once, when it
 * begins (wait set), and once when work comes back (wait PKG_WAIT_NONE);
 * nothing is sent while nothing happens: the front end animates its own
 * indeterminate indicator.
 *
 * Lifetime. The event and every string in it are lent for the length of the
 * callback; copy what you keep. The callback runs on the operation's thread,
 * synchronously, as every callback here. It must not call a libpkg operation:
 * such a call returns PKG_RC_USAGE at once, touching nothing, with an
 * OP_BEGIN and OP_END of its own when its sink has `event`. The event is not
 * a way to stop the operation: that is `cancel`.
 *
 * Privacy. `object` names a file, an archive, a channel's object or a host,
 * never a user name, password, query string or fragment of a URL.
 *
 * Without `event` nothing is built, and the results and text are the same
 * as with it. The `size` field is sizeof(struct pkg_event) as the library
 * was built; fields are only ever added at the end. */

#define PKG_EVENT_GAP_MS 100

enum pkg_event_type {
    PKG_EVENT_OP_BEGIN = 1,   /* verb, dryrun */
    PKG_EVENT_STEP_BEGIN,     /* step, parent, attempt, phase, what it acts on, unit,
                                 measure, done (0), total if known */
    PKG_EVENT_STEP_PROGRESS,  /* step, done, total, total_known */
    PKG_EVENT_STEP_WAIT,      /* step, wait, object (the host); counters unchanged */
    PKG_EVENT_STEP_END,       /* step, final done and total, outcome, code */
    PKG_EVENT_OP_END          /* code, outcome, restore, cancel */
};

/* What a step does. A phase that does not run is not announced: a dry run
 * has no PREPARE, PLACE, RECORD or FLUSH, a local channel no download. */
enum pkg_phase {
    PKG_PHASE_NONE = 0,   /* operation events */
    PKG_PHASE_RESOLVE,    /* channels read, a version picked, dependencies planned; the
                             FETCH and VERIFY steps of every package nest in it */
    PKG_PHASE_FETCH,      /* an object downloaded, or a payload or archive read */
    PKG_PHASE_VERIFY,     /* bytes or files checked against what was signed */
    PKG_PHASE_INSPECT,    /* the root's files compared with what is recorded or coming */
    PKG_PHASE_PREPARE,    /* files written to staging; for PUSH, a file hashed */
    PKG_PHASE_PLACE,      /* files put in their place in the root */
    PKG_PHASE_REMOVE,     /* files taken out of the root */
    PKG_PHASE_RECORD,     /* the database entry, the pinned key, the previous version
                             and the files' attributes written */
    PKG_PHASE_RESTORE,    /* what this operation placed, taken back out after a refusal
                             or a cancel */
    PKG_PHASE_FLUSH,      /* the root's changes written back to its medium */
    PKG_PHASE_UPLOAD      /* PUSH: files sent to the portal */
};

enum pkg_unit {
    PKG_UNIT_NONE = 0,    /* nothing is counted: the step begins and ends */
    PKG_UNIT_BYTES,
    PKG_UNIT_FILES,
    PKG_UNIT_ENTRIES,     /* versions in a channel */
    PKG_UNIT_PACKAGES
};

/* What `done` counts. One measure per step: compressed and uncompressed
 * bytes are never mixed, nor bytes read with bytes verified. */
enum pkg_measure {
    PKG_MEASURE_NONE = 0,
    PKG_MEASURE_BYTES_TRANSFERRED, /* FETCH: body bytes received from the network */
    PKG_MEASURE_BYTES_READ,        /* FETCH: bytes of a local file read as stored; for an
                                      archive, its compressed bytes */
    PKG_MEASURE_BYTES_VERIFIED,    /* VERIFY: bytes hashed against a signed digest */
    PKG_MEASURE_BYTES_HASHED,      /* PREPARE (PUSH): bytes hashed to name a file */
    PKG_MEASURE_BYTES_SENT,        /* UPLOAD: bytes the portal acknowledged */
    PKG_MEASURE_FILES_VERIFIED,    /* VERIFY: payload files checked against the manifest */
    PKG_MEASURE_FILES_CHECKED,     /* INSPECT: root files compared, each read and hashed */
    PKG_MEASURE_FILES_WRITTEN,     /* PREPARE, PLACE (REPAIR): files written */
    PKG_MEASURE_FILES_PLACED,      /* PLACE: files moved into place */
    PKG_MEASURE_FILES_PROCESSED,   /* REMOVE: files dealt with, whether removed, kept as
                                      edited or found already gone */
    PKG_MEASURE_FILES_SENT,        /* UPLOAD: files the portal took */
    PKG_MEASURE_ENTRIES_CHECKED,   /* VERIFY (SHOW): channel entries checked */
    PKG_MEASURE_PACKAGES_RESTORED  /* RESTORE: packages taken back out */
};

/* What `object` names. */
enum pkg_object {
    PKG_OBJECT_NONE = 0,
    PKG_OBJECT_INDEX,     /* a channel's index */
    PKG_OBJECT_MANIFEST,
    PKG_OBJECT_SIGNATURE,
    PKG_OBJECT_PAYLOAD,
    PKG_OBJECT_ARCHIVE,   /* a source archive, by its file name */
    PKG_OBJECT_CHANNEL,   /* a channel, by its last path component */
    PKG_OBJECT_FILE,      /* any other file, by its name */
    PKG_OBJECT_ROOT,      /* the root, as given */
    PKG_OBJECT_HOST       /* STEP_WAIT: the machine waited for */
};

enum pkg_wait {
    PKG_WAIT_NONE = 0,    /* the wait is over: the step works again */
    PKG_WAIT_NETWORK,     /* finding a host, connecting, a TLS handshake, the first byte */
    PKG_WAIT_OTHER
};

enum pkg_outcome {
    PKG_OUTCOME_NONE = 0, /* not an end */
    PKG_OUTCOME_OK,
    PKG_OUTCOME_FAILED,   /* refused or failed; `code` has the class */
    PKG_OUTCOME_CANCELLED,/* the caller's cancel stopped it at a boundary */
    PKG_OUTCOME_RETRIED,  /* the work started again: a step with attempt + 1 follows */
    PKG_OUTCOME_ABSENT,   /* FETCH: the object is not published there, which is not an
                             error by itself (a channel with no withdrawals) */
    PKG_OUTCOME_ABANDONED /* the operation ended with the step open; never expected */
};

/* OP_END: whether what the operation changed was taken back. */
enum pkg_restore {
    PKG_RESTORE_NONE = 0,  /* nothing was left to take back: success, a dry run, or a
                              refusal before anything was placed */
    PKG_RESTORE_DONE,      /* every package this operation placed was taken back out */
    PKG_RESTORE_FAILED,    /* taking back was tried and left something in place */
    PKG_RESTORE_INCOMPLETE /* a package was left partly placed and was not taken back;
                              the same operation repeated finishes it */
};

struct pkg_event {
    unsigned size;            /* sizeof(struct pkg_event) as the library was built */
    int type;                 /* PKG_EVENT_* */
    unsigned long op;         /* the operation, from 1 in this process */
    const char *verb;         /* "install", "upgrade", ... */
    int dryrun;               /* the operation writes nothing */
    unsigned long step;       /* the step, from 1 in this operation; 0 for OP_* */
    unsigned long parent;     /* the step it is part of, 0 for none */
    unsigned attempt;         /* 1, or more for the same work started again */
    int phase;                /* PKG_PHASE_* */
    const char *package;      /* the package acted on, or NULL */
    const char *version;      /* its version, or NULL */
    int object_kind;          /* PKG_OBJECT_* */
    const char *object;       /* what is read, written or waited for, or NULL */
    int unit;                 /* PKG_UNIT_* */
    int measure;              /* PKG_MEASURE_* */
    unsigned long long done;
    unsigned long long total;
    int total_known;
    int wait;                 /* STEP_WAIT: PKG_WAIT_* */
    int outcome;              /* STEP_END, OP_END: PKG_OUTCOME_* */
    int code;                 /* OP_END: what the call returns. STEP_END failed: the
                                 class of the operation's first refusal */
    int restore;              /* OP_END: PKG_RESTORE_* */
    int cancel;               /* OP_END: the caller's cancel stopped the operation */
};

/* Names for logs and tests, "fetch", "bytes-read", "failed"...; "unknown"
 * for a value this library does not know. */
const char *pkg_event_name(int type);
const char *pkg_phase_name(int phase);
const char *pkg_unit_name(int unit);
const char *pkg_measure_name(int measure);
const char *pkg_object_name(int kind);
const char *pkg_wait_name(int wait);
const char *pkg_outcome_name(int outcome);
const char *pkg_restore_name(int restore);

/* Every option any operation takes; each operation reads the ones it needs
 * and ignores the rest. Strings are borrowed for the length of the call. */
struct pkg_options {
    const char *target;     /* the package, drawer, file or image operated on */
    const char *root;       /* the directory packages are installed into */
    const char *at;         /* INSTALL: absolute parent for the application's drawer */
    const char *channel;    /* the directory packages are published into */
    const char *name, *version, *arch, *kind;  /* publish: identity, else from the drawer;
                                                  publish refuses a NULL kind (20) */
    const char *depends;    /* publish: "a >= 1.0, b" */
    const char *sign;       /* publish: the signing key file */
    const char *file;       /* keygen: the key file to create */
    const char *key;        /* sign: the key file */
    const char *nspace;     /* sign SSH: the namespace the signature is made for */
    const char *out;        /* sign, image, mountlist: the file to write */
    const char *acceptkey;  /* the publisher key, in full, the person confirmed: install
                               from a new key, trust one of several, or publish with a new one */
    const char *unit;       /* mountlist: the fdsk.device unit, default 20 */
    const char *handler;    /* mountlist: the FFS handler's path, default found in the root */
    const char *files;      /* publish from "archive!/prefix": the paths under the prefix
                               that make this package, comma-separated; all when NULL.
                               The manifest then names the archive (Source) and the
                               channel keeps it as archives/<name> */
    const char *build;      /* publish: the build the files come from, such as a nightly's
                               date; the version becomes <version or $VER or 0>+<build>, and
                               a build whose files equal the last version's is not published */
    /* publish, manifest: the catalogue fields (see pkg_manifest.h, struct
     * pkg_about). Each is taken from the last version published when not
     * given, except changes; "none" drops the inherited value. description
     * and changes name text files; tags, author and screenshot are
     * comma-separated lists; readme names an Aminet .readme, whose Short,
     * Author, Type and text fill what the keywords leave out. */
    const char *short_desc, *description, *category, *tags, *author, *homepage;
    const char *repository, *license, *distribution, *changes, *icon, *screenshot;
    const char *readme;
    const char *info;       /* publish, manifest: a .pkginfo file (pkg_pkginfo.h) the port
                               carries, holding the name, version, kind, catalogue fields,
                               dependencies, Files and Config. A keyword on the command line
                               wins over it, and it wins over the readme and over what the
                               last version published carried. "!/<path>" reads it from the
                               archive the drawer is "<archive>!/<prefix>" of */
    const char *from;       /* resolve: the program's directory, and the current one */
    const char *unpacked;   /* install, upgrade, repair from an archive: a directory holding
                               what the archive holds, so <dir>/<prefix>/<path> is each file.
                               Read instead of the archive, every file still weighed and
                               hashed against the signed manifest. Pkg also looks for
                               <cache>/upstream/<sha256>/<name>.d without being told */
    const char *upstream;   /* publish from an archive: the http(s) URL the archive is
                               downloaded from, recorded with its SHA-256 and size in the
                               signed manifest; installs download it from there, and PUSH
                               leaves it out */
    const char *config;     /* publish: the configuration files, paths or folders, comma-
                               separated: a person's edit of one survives an upgrade, the new
                               version set down beside it as <file>.pkgnew. Inherited from the
                               last version published when not given */
    const char *archive;    /* show: only the entries whose files are in this archive of the
                               channel (its name in archives/), that archive read once */
    const char *to;         /* push: the channel's URL on the portal (https, or http to this
                               machine for tests) */
    const char *pushkey;    /* push: the publisher's portal key; the CLI takes PKG_PUSHKEY */
    int metadata;           /* show: manifests, signatures and payloads only; source archives
                               are not read, and their entries say archive: unchecked */
    int downgrade;          /* upgrade: moving to an older version was asked for */
    int orphans;            /* remove: remove what nothing needs, instead of target */
    int dryrun;             /* every check, no write; results read would-... */
    int ssh;                /* sign, keyinfo: OpenSSH's formats, for ssh-keygen -Y verify */
    const char *const *also;/* install: the further names on the line, target being the
                               first; nalso of them. Each is installed in turn, the command
                               goes as far as it can, and its code is the worst class */
    unsigned    nalso;
    int all;                /* upgrade: every package a newer version is offered for,
                               target NULL; refused (20) with version, downgrade or
                               acceptkey, which are decisions about one package */
};

/* What each operation answers in the structured form. `result` is always
 * there but not always first: look it up by key. A refusal is result
 * refused, class, code, reason, next, sometimes preceded by fields that
 * help (pinned, signer, first-signer on a key refusal; suggest on a name not
 * found). The return code and these fields agree, except SHOW, which answers
 * `shown` with every entry and returns the class of the first bad one.
 * Fields marked [item] also reach `item`, with the field names in brackets.
 *
 *   keygen     result created; file; public
 *   keyinfo    result shown; file; public                (file or target)
 *   sign       result signed; file; signer
 *   manifest   result shown; then each manifest line as a field (Name, ...)
 *   publish    result published: name, version, channel, manifest, payload,
 *              signer, files, [arch-from], [version-from], [name-from] (the
 *              file whose $VER gave the version or the name), [kind-from],
 *              [depends-from] (the published version they were taken from
 *              when kind or depends was not given), left-out per file.
 *              result unchanged: name, version (that exact content is there).
 *              result repaired: name, version, repaired per object written
 *              again (manifest, payload, signature), when that version's
 *              objects were damaged and the key is its publisher's.
 *   withdraw   result withdrawn (or unchanged): name, version, channel. The
 *              version stays; nothing picks it by default, asking for it
 *              is refused (18, ask-requester), and SHOW gives status
 *              withdrawn.
 *              dryrun: result would-publish, name, version, kind,
 *              architecture, channel, depends (or "none"), file per file,
 *              signer ("none" when no key was given: a dry run needs none),
 *              [first-signer] when keyless and the package exists, left-out;
 *              content (path size) per file inside the image, for kind
 *              image. Against the highest published version: compared-with
 *              (name version), then added (path size), changed (path
 *              oldsize newsize), same (path), gone (path) per file; inside
 *              the image for kind image.
 *   install    result installed: name, version, root, files, payload, signer,
 *              and image, blocks for an image; before it, one [item]
 *              dependency (name version) per dependency this install
 *              placed; those already there are not listed. Dependency
 *              items are sent only once every package is in place, so a
 *              refused or cancelled install sends none. A refused install
 *              into a root that did not exist may leave the root with an
 *              empty .pkg directory.
 *              result unchanged or kept: name, version.
 *   upgrade,   result upgraded, downgraded, rolled-back or unchanged: name,
 *   rollback   from, version, root, placed, removed, signer, with
 *              dependency items as for install.
 *   upgrade    with all set: every installed package whose channel offers a
 *   all        higher version, as upgrade would pick it (the root's CPU,
 *              withdrawn versions skipped), upgraded exactly as upgrade
 *              with that name would, a package before what depends on it.
 *              Per package done, [item] package (name from version), after
 *              its dependency items; then result upgraded, count. Nothing
 *              to do: result unchanged, count 0. A note per installed
 *              version withdrawn with nothing newer: never downgraded.
 *              Goes as far as possible: a package that needs a decision
 *              (a key change, an edited file) is not upgraded, [item]
 *              refused (name installed class reason, with code and next);
 *              one whose new version needs a refused one waits, [item]
 *              skipped (name installed waits-for); every other goes ahead.
 *              Then upgraded, not-upgraded, count, summary (a sentence);
 *              result upgraded or unchanged, or, when something was not
 *              upgraded, result refused with the class, code and next of
 *              the first refusal, which is also the exit code. Calling
 *              again once the requester has decided takes what waited.
 *   status     every installed package (or only target) against channel:
 *              result shown; [item] package (name installed available
 *              state), available "-" when the channel offers nothing for
 *              it; state current, upgradable (available is what upgrade
 *              would take), withdrawn (the installed version was withdrawn
 *              by its publisher, nothing newer offered), not-offered (the
 *              channel has no version for it), or edited (a file differs
 *              from what was installed, by size or digest, as verify
 *              checks; available higher means an upgrade is offered too).
 *              note when an installed withdrawn version has a newer one;
 *              count; upgradable, the packages upgrade all would attempt
 *              (upgradable, and edited ones with a higher available);
 *              hint how to upgrade them. Code 0 whether or not anything is
 *              upgradable. A channel directory that does not exist is
 *              refused (11), as for upgrade all, rather than read as
 *              offering nothing.
 *   list       result listed; [item] package (name version kind files
 *              reason), reason explicit or dependency; count. A root that
 *              does not exist lists nothing and succeeds.
 *   verify     name, version, files; changed and missing per file; moved
 *              (recorded path, where it is now) per file found elsewhere in
 *              the root with the same name and bytes; result intact, moved
 *              (every missing file found moved, nothing changed: code 0), or
 *              damaged with code 12.
 *   remove     result removed: name, version, root, removed, gone, kept per
 *              edited file left in place, [item] orphan (name version) per
 *              package nothing needs any more. With orphans set: [item]
 *              package (name version) per package removed, then result
 *              removed, count.
 *   image      result created: file, volume, blocks
 *   mountlist  result created (with out) or shown: name, image, blocks,
 *              highcyl, unit, handler, file, step per AmigaDOS command.
 *   show       result shown; [item] entry per channel entry, fields name,
 *              version, kind, architecture, status, signer, and, only when
 *              root is given, installed. status is ok, withdrawn or a class
 *              name; installed is installed, other-version or no; kind,
 *              architecture and signer are "-" when the entry cannot be
 *              read. [item] depends (package version needs min) per
 *              dependency, min empty when any version will do; [item] problem (package version reason)
 *              per bad entry; warning when a package has several signers;
 *              count; bad. A name the channel lacks shows count 0.
 *   any        hint: what usually comes next, or what to tell the person
 *              (after keygen: keep the key; publish into a channel that did
 *              not exist: it was created; install of an image and
 *              mountlist: how to mount it, a missing FFS handler). warning:
 *              something to check before going on. note: a fact worth
 *              passing on. None of them is ever a command that overrides a
 *              safeguard. Without MACHINE the CLI prints them as text.
 *   any        [item] suggest (name), before a not-found refusal. The
 *              refusal's reason names the same suggestions: it is complete
 *              by itself; the items are for offering them as choices.
 *
 * `next` takes the values stop, ask-requester, fix-command, check-name,
 * use-upgrade, use-install and report; a value added later is shown with
 * pkg_next_words. dryrun turns results into would-publish, would-install,
 * would-upgrade, would-downgrade, would-roll-back, would-remove,
 * would-create. INSTALL creates the root if it does not exist. */

int pkg_keygen   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_sign     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_checksig (const struct pkg_sink *s, const struct pkg_options *o);  /* target, file: the .sig; key: the signer expected */
int pkg_keyinfo  (const struct pkg_sink *s, const struct pkg_options *o);  /* file: the public key it holds */
int pkg_manifest (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_publish  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_withdraw (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_install  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_upgrade  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_rollback (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_list     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_verify   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_repair   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_resolve  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o);
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_push     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_status   (const struct pkg_sink *s, const struct pkg_options *o);  /* root, channel, [target] */
int pkg_channel  (const struct pkg_sink *s, const struct pkg_options *o);  /* root, target ADD|LIST|REMOVE, also[0] the channel */
int pkg_search   (const struct pkg_sink *s, const struct pkg_options *o);  /* target and also: the words; [channel] [root] [arch] */

/* The channel a root knows by that short name, for a front end that lets a
 * person type the name instead of the address. Returns a string the caller
 * frees, or NULL when the root lists no channel by that name, which is not
 * an error: the word is then whatever else it was going to be. Reads the
 * root's list and nothing more; it prints nothing and refuses nothing.
 * `names`, when given, is filled with the names the root does know, comma
 * separated, for a refusal that can say them. */
char *pkg_channel_named(const char *root, const char *name, char *names, unsigned long names_len);

/* A refusal of the request itself, answered in the same form as the
 * operations' own: for a front end that validates its input first. Returns
 * PKG_RC_USAGE. `verb` names the operation, as in "install". */
int pkg_usage_error(const struct pkg_sink *s, const char *verb, const char *reason);

/* "not-found" for 11, and so on; "ok" for 0. */
const char *pkg_class_name(int code);

/* A sentence addressed to the person, for a `next` value, naming no command,
 * so any front end can show it; NULL or an unknown value gives the one for
 * report. (The command line's text form has its own wording.) */
const char *pkg_next_words(const char *next);

/* The value of field `key` among an item's fields, or NULL. */
const char *pkg_field(int n, const char *const *keys, const char *const *values, const char *key);

/* Read-only update checks for a program installed by Pkg.
 * Strings assigned in code are borrowed. from_file owns its parsed strings
 * until pkg_update_free; initialize before the first use. Root and package
 * are required. A NULL channel selects the root's configured channels. */
struct pkg_update {
    const char *package;
    const char *channel;
    const char *root;
    void *_owned;
};

enum pkg_update_state {
    PKG_UPDATE_NONE = 0,       /* no newer version among the compatible offers */
    PKG_UPDATE_AVAILABLE,
    PKG_UPDATE_NOT_MANAGED,
    PKG_UPDATE_UNREACHABLE,
    PKG_UPDATE_WITHDRAWN,
    PKG_UPDATE_KEY_CHANGED,
    PKG_UPDATE_NOT_OFFERED,
    PKG_UPDATE_ERROR
};

/* All strings are owned by this result and live until found_free or the
 * next check. Initialize before the first check. Changes describes the
 * offered version. installed_bytes is the signed sum of its file sizes;
 * download_bytes is valid only when download_size_known is nonzero.
 * A check reads metadata and may populate the network cache. Installed
 * files, the root database and pinned keys are unchanged. Serialize calls
 * with every other libpkg operation, as required by the library API. */
struct pkg_update_found {
    int state;
    int code;                 /* PKG_RC_* on error; zero for a completed check */
    int installed_withdrawn;
    int newer;
    char *installed, *offered, *changes, *signer, *channel;
    char *homepage, *short_desc;
    unsigned long long installed_bytes, download_bytes;
    int download_size_known;
    char manifest[65];        /* digest identifying the verified offered manifest */
    char error[512];
};

void pkg_update_init(struct pkg_update *u);
void pkg_update_free(struct pkg_update *u);
/* Reads an explicitly named file with Format: pkg-update 1, Package, Root
 * and optional Channel. Relative paths are relative to the config file.
 * Failure preserves u. Returns PKG_RC_*; err receives a diagnostic when
 * non-NULL and errlen is positive. Unknown keys are ignored. */
int pkg_update_from_file(struct pkg_update *u, const char *path,
                         char *err, unsigned long errlen);
void pkg_update_found_init(struct pkg_update_found *found);
void pkg_update_found_free(struct pkg_update_found *found);
/* Returns a PKG_UPDATE_* state, also stored in found.state. The installed
 * version comes from the root database. A different publisher key is
 * reported explicitly; the check accepts no keys and installs nothing. */
int pkg_update_check(const struct pkg_update *u, struct pkg_update_found *found);

#endif

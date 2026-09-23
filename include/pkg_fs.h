/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The host filesystem, and the only non-portable file in the tree. Everything
 * above this interface is C99; this implementation is POSIX, which covers
 * macOS and Linux. The AROS implementation of the same interface uses
 * dos.library and comes with the AROS build of the client.
 */

#ifndef PKG_FS_H
#define PKG_FS_H

#include <stddef.h>

/* Return 0, or -1 with errno set. */
int  pkg_fs_read(const char *path, unsigned char **buf, size_t *len);
int  pkg_fs_write_atomic(const char *path, const void *buf, size_t len);
/* Writes a file straight under its name, parents made: for a directory only
 * Pkg reads (staging), where no temporary name and rename are needed. */
int  pkg_fs_write_new(const char *path, const void *buf, size_t len);
/* Non-zero when standard output is a terminal a person watches. */
int  pkg_fs_interactive(void);
/* Whether a library (or, with `device`, a device) of that name is in memory
 * now: 1 with its version, revision and open count, 0 not loaded, -1 when
 * this system cannot tell (any host but AROS). */
int  pkg_fs_loaded(const char *name, int device, unsigned *version, unsigned *revision,
                   unsigned *opencnt);
/* On AROS, the full path a name reaches (an assign such as LIBS: resolved);
 * elsewhere, or when it does not exist, 0. */
int  pkg_fs_fullpath(const char *path, char *out, size_t ol);
/* Resolve an existing directory to its absolute physical path.
 * Symlinks/assigns are resolved. Returns 0, or -1 with errno set. */
int  pkg_fs_canonical_dir(const char *path, char *out, size_t len);
/* 1 for a symlink or Windows reparse point, 0 for other/missing paths,
 * -1 for an inspection error. Inspect each parent separately. */
int  pkg_fs_path_is_link(const char *path);
int  pkg_fs_mkdirs(const char *dir);
int  pkg_fs_exists(const char *path);          /* 1 if anything is there */
int  pkg_fs_is_dir(const char *path);
int  pkg_fs_rename(const char *from, const char *to);
int  pkg_fs_unlink(const char *path);
int  pkg_fs_rmtree(const char *path);
void pkg_fs_prune_empty_parents(const char *root, const char *rel);

/* Walk regular files under root, calling fn with a '/'-separated path
 * relative to root. Symlinks and special files are refused, naming the file.
 *
 * Left out, and passed to skip (when not NULL) so the caller can name each:
 * every name starting with '.', file or directory, which covers the hidden
 * files of macOS and Unix (.DS_Store, AppleDouble "._" files, .git,
 * .Trashes) and of the Amiga Workbench (.backdrop); "Icon\r", the custom
 * folder icon of macOS; Thumbs.db and desktop.ini. Amiga icons are Name.info,
 * which never start with '.', and stay. *skipped counts what was left out. */
typedef int (*pkg_fs_walk_fn)(const char *rel, void *ctx);
typedef void (*pkg_fs_skip_fn)(const char *rel, int is_dir, void *ctx);
int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip, void *ctx,
                unsigned *skipped, char *err, size_t errlen);

/* Directory entries of dir, sorted, dotfiles excluded. Caller frees each
 * string and the array. */
int pkg_fs_list(const char *dir, char ***names, size_t *count);

/* Caller frees. */
char *pkg_join(const char *a, const char *b);

/* Replace argc/argv with the host's own view of the command line, as UTF-8.
 * On Windows the C runtime's argv is in the ANSI code page and loses names
 * outside it; elsewhere this changes nothing. 0 or -1. */
int pkg_host_args(int *argc, char ***argv);

/* Fill buf from the system's cryptographic random source. 0 or -1. */
/* Called while pkg_net_get downloads, so that a person sees a large file
 * arrive: bytes so far, and the total, -1 when the server did not say. NULL,
 * the default, asks for nothing. */
extern void (*pkg_fs_on_transfer)(long long done, long long total);

/* Called before a step of the network that blocks with nothing to report
 * while it runs: finding a host, opening the connection, the TLS handshake,
 * a curl that has not written a byte yet. `host` is the machine being waited
 * for, NULL when that step is over. NULL, the default, asks for nothing. */
extern void (*pkg_fs_on_wait)(const char *host);
extern void (*pkg_fs_on_tick)(void);
/* Wait for socket readiness while servicing the activity callback.
 * Used by the POSIX/AROS network transport, including TLS's socket layer. */
int pkg_fs_socket_wait(int socket, int writing);

/* Milliseconds counted from some moment of this run: gettimeofday under
 * POSIX and on AROS, GetTickCount64 on Windows. Only differences are used,
 * so which moment it counts from does not matter. */
long long pkg_fs_now_ms(void);

/* The network's own account of itself, for a TRACE: one line at a time,
 * with no line break of its own. NULL, the default, says nothing. */
extern void (*pkg_fs_on_trace)(const char *line);

int pkg_fs_random(void *buf, size_t len);
/* Where the system has no random source (AROS): bytes made from the moments a
 * person presses keys, asked for at the console. 0 done; -1 given up or
 * failed; -2 there is nobody to ask (no console), or none is needed here. */
int pkg_fs_random_typed(void *buf, size_t len);

/* Like pkg_fs_write_atomic, readable and writable by the owner alone. For a
 * signing key, which must not be world-readable even for the instant between
 * creation and a chmod. */
int pkg_fs_write_private(const char *path, const void *buf, size_t len);

/* ---- Amiga attributes -------------------------------------------------- */

/* 1 when the host mode sets owner execute, 0 when it clears it, -1 when the
 * host has no mode (Windows) or the file cannot be read. */
int pkg_fs_owner_exec(const char *path);
/* Set or clear owner execute in the host mode; 0, or -1. No-op where the
 * host has no mode. */
int pkg_fs_set_owner_exec(const char *path, int exec);

/* On AROS, the file's own protection word and comment (Latin-1): 1. On a
 * host that holds none of them: 0, and .ameta is where they live. -1 on
 * error. */
int pkg_fs_amiga_get(const char *path, unsigned long long *prot, char *comment, size_t cl);
/* On AROS, SetProtection and SetComment: 1. Elsewhere 0, nothing done. */
int pkg_fs_amiga_set(const char *path, unsigned long long prot, const char *comment_latin1);

/* On AROS, clear the protection word, so a file Pkg installed with Delete or
 * Write forbidden can be replaced or removed by it. Elsewhere nothing. */
void pkg_fs_unprotect(const char *path);

/* What .ameta writers compare before replacing it (see ameta.md, Writing). */
struct pkg_fs_id {
    int                exists;
    unsigned long long dev, ino, size;
    long long          mtime_s, mtime_ns;
};
int pkg_fs_identity(const char *path, struct pkg_fs_id *id);
/* Write buf to a temporary file beside path, then, if path still has the
 * identity `before`, rename it over path (buf NULL: delete path). 0 done,
 * 1 path changed meanwhile and nothing was replaced, -1 error. */
int pkg_fs_replace_if_same(const char *path, const struct pkg_fs_id *before,
                           const void *buf, size_t len);
/* An exclusive lock on a directory, where the host has flock; NULL
 * otherwise, and the identity check alone guards the write. */
void *pkg_fs_lock_dir(const char *dir);
void  pkg_fs_unlock_dir(void *lock);
/* The one change a root takes at a time, without waiting: a handle, or
 * NULL with *busy 1 when another process holds it (0: it could not be
 * made). On AROS a public semaphore named after the root's full path, gone
 * at a reset; elsewhere <root>/.pkg/lock, released when the process ends. */
void *pkg_fs_lock_root(const char *root, int *busy);
void  pkg_fs_unlock_root(void *lock);
/* The root's volume written back to the medium after a change. On AROS:
 * ACTION_FLUSH to its handler ("flush"); a handler that does not know it,
 * as FAT did not, has its device inhibited and released ("inhibit"), which
 * writes its cache and the device back. Elsewhere the host keeps its own
 * files ("host"). NULL: it could not be written back. */
const char *pkg_fs_flush_root(const char *root);

/* ---- the network ------------------------------------------------------ */

/* Fetch url into the file dest, whole, replacing it only once complete.
 * http:// is spoken here; https:// goes through the system's curl where
 * there is one. 0 fetched, 1 the server has no such file (404), -1 with a
 * reason in err. Redirects are followed. */
int pkg_net_get(const char *url, const char *dest, char *err, size_t errlen);

/* Send a request through the system's curl: method (POST or PUT), the body
 * from body_file (NULL: none), extra headers one per line in header_file
 * (NULL: none; a file, so a secret never shows in a process list), the
 * answer into out_file. *code is the HTTP status. 0 when a status came back,
 * -1 with a reason when none did. */
int pkg_net_send(const char *method, const char *url, const char *body_file,
                 const char *header_file, const char *out_file, int *code,
                 char *err, size_t errlen);

/* Close whatever connections the client is holding open for its next
 * request. Called when an operation ends, so nothing outlives it. */
void pkg_net_idle_close(void);

/* Where downloaded channel files are kept: PKG_CACHE, or the host's usual
 * cache directory. Caller frees. */
char *pkg_cache_dir(void);

#endif

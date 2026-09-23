/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#define _POSIX_C_SOURCE 200809L
/* flock, and the nanosecond modification time .ameta writers compare. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#include "pkg_fs.h"
#include "pkg.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if !defined(__AROS__)
#include <sys/file.h>
#endif
#ifdef __AROS__
#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#endif

int pkg_host_args(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

char *pkg_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    /* No separator after "RAM:" either: on AmigaDOS a leading '/' in the rest
     * of a path means the parent directory, so "RAM:/C" is not "RAM:C". */
    int slash = la > 0 && a[la - 1] != '/' && a[la - 1] != ':';
    char *p = (char *)malloc(la + (size_t)slash + lb + 1u);
    if (p == NULL)
        return NULL;
    memcpy(p, a, la);
    if (slash)
        p[la] = '/';
    memcpy(p + la + (size_t)slash, b, lb + 1u);
    return p;
}

int pkg_fs_read(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p = NULL;
    size_t cap = 0, n = 0;

    if (f == NULL)
        return -1;
    for (;;) {
        size_t got;
        if (n == cap) {
            size_t ncap = cap ? cap * 2u : 65536u;
            unsigned char *q = (unsigned char *)realloc(p, ncap);
            if (q == NULL) { free(p); fclose(f); errno = ENOMEM; return -1; }
            p = q;
            cap = ncap;
        }
        got = fread(p + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) { free(p); fclose(f); errno = EIO; return -1; }
            break;
        }
    }
    fclose(f);
    *buf = p;
    *len = n;
    return 0;
}

/* Existence is tested before each mkdir, never inferred from errno after it.
 * AROS's posixc does not report EEXIST for a directory that is already there,
 * which the first hosted run showed: the first file of a package staged, the
 * second failed on the parents the first had just created. */
static int mkdir_one(const char *p)
{
    if (pkg_fs_is_dir(p))
        return 0;
    if (mkdir(p, 0755) == 0)
        return 0;
    return pkg_fs_is_dir(p) ? 0 : -1;
}

int pkg_fs_mkdirs(const char *dir)
{
    char *p = strdup(dir), *s;
    if (p == NULL)
        return -1;
    for (s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir_one(p) != 0) { free(p); return -1; }
            *s = '/';
        }
    }
    if (mkdir_one(p) != 0) { free(p); return -1; }
    free(p);
    return 0;
}

static int mkparents(const char *path)
{
    char *p = strdup(path), *slash;
    int rc = 0;
    if (p == NULL)
        return -1;
    slash = strrchr(p, '/');
    if (slash != NULL && slash != p) {
        *slash = '\0';
        rc = pkg_fs_mkdirs(p);
    }
    free(p);
    return rc;
}

static int write_atomic_mode(const char *path, const void *buf, size_t len, int mode);

#ifdef __AROS__
int pkg_fs_loaded(const char *name, int device, unsigned *version, unsigned *revision,
                  unsigned *opencnt)
{
    struct Library *lib;
    int found = 0;
    Forbid();
    lib = (struct Library *)FindName(device ? &SysBase->DeviceList : &SysBase->LibList, (CONST_STRPTR)name);
    if (lib != NULL) {
        *version = lib->lib_Version;
        *revision = lib->lib_Revision;
        *opencnt = lib->lib_OpenCnt;
        found = 1;
    }
    Permit();
    return found;
}

int pkg_fs_fullpath(const char *path, char *out, size_t ol)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    int ok;
    if (l == BNULL) return 0;
    ok = NameFromLock(l, (STRPTR)out, (LONG)ol) != 0;
    UnLock(l);
    return ok;
}
#else
int pkg_fs_loaded(const char *name, int device, unsigned *version, unsigned *revision,
                  unsigned *opencnt)
{
    (void)name; (void)device; (void)version; (void)revision; (void)opencnt;
    return -1;
}

int pkg_fs_fullpath(const char *path, char *out, size_t ol)
{
    (void)path; (void)out; (void)ol;
    return 0;
}
#endif

int pkg_fs_canonical_dir(const char *path, char *out, size_t len)
{
    if (!path || !out || !len) { errno = EINVAL; return -1; }
    out[0] = 0;
#ifdef __AROS__
    {
        BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
        struct FileInfoBlock *fib;
        int rc = -1;
        if (lock == BNULL) { errno = ENOENT; return -1; }
        fib = AllocDosObject(DOS_FIB, NULL);
        if (!fib) { UnLock(lock); errno = ENOMEM; return -1; }
        if (!Examine(lock, fib)) errno = EIO;
        else if (fib->fib_DirEntryType <= 0) errno = ENOTDIR;
        else if (len > 0x7fffffffUL || !NameFromLock(lock, (STRPTR)out, (LONG)len))
            errno = ENAMETOOLONG;
        else rc = 0;
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        if (rc) out[0] = 0;
        return rc;
    }
#else
    {
        char *resolved = realpath(path, NULL);
        struct stat st;
        if (!resolved) return -1;
        if (stat(resolved, &st) != 0) { free(resolved); return -1; }
        if (!S_ISDIR(st.st_mode)) { free(resolved); errno = ENOTDIR; return -1; }
        if (strlen(resolved) >= len) { free(resolved); errno = ENAMETOOLONG; return -1; }
        memcpy(out, resolved, strlen(resolved) + 1u);
        free(resolved);
        return 0;
    }
#endif
}

int pkg_fs_interactive(void)
{
    return isatty(1);
}

int pkg_fs_write_new(const char *path, const void *buf, size_t len)
{
    const unsigned char *b = (const unsigned char *)buf;
    int fd;
    if (mkparents(path) != 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    while (len > 0) {
        ssize_t w = write(fd, b, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(path); return -1;
        }
        b += w;
        len -= (size_t)w;
    }
    if (close(fd) != 0) { unlink(path); return -1; }
    return 0;
}

int pkg_fs_write_atomic(const char *path, const void *buf, size_t len)
{
    return write_atomic_mode(path, buf, len, 0644);
}

int pkg_fs_write_private(const char *path, const void *buf, size_t len)
{
    return write_atomic_mode(path, buf, len, 0600);
}

#ifdef __AROS__
/* The fallback for an AROS without entropy.resource, which getentropy reads.
 * A signing key needs 256 bits nobody can guess, and the clock, the task's
 * address and free memory are all guessable. What is not is when a person
 * presses keys: each moment is read from the CPU's cycle counter
 * (nanoseconds, where a person is exact to milliseconds) and from the system
 * clock, and hashed with what was typed. A key press is credited 4 bits with
 * a cycle counter, 2 with only the clock, and none when it repeats the last
 * one, as a held key does; typing goes on until 256 bits are credited. PGP
 * on the Amiga made keys this way. */
#include <devices/timer.h>
#include <proto/timer.h>
#include "pkg_sha512.h"

struct Device *TimerBase;

static unsigned long long cycles(void)
{
#if defined(__aarch64__)
    unsigned long long v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
#else
    return 0;
#endif
}

int pkg_fs_random_typed(void *buf, size_t len)
{
    BPTR in = Input(), out = Output();
    struct MsgPort *port = NULL;
    struct timerequest *tr = NULL;
    struct pkg_sha512 pool;
    unsigned char digest[PKG_SHA512_LEN], last = 0;
    struct { struct EClockVal e; unsigned long long c; struct DateStamp d; APTR task; IPTR mem; unsigned char key; } ev;
    const int per_key = cycles() != 0 ? 4 : 2;
    int need = 256, rc = -1, opened = 0;
    char line[96];

    if (len > sizeof digest || !IsInteractive(in) || !IsInteractive(out))
        return -2;
    if ((port = CreateMsgPort()) == NULL
        || (tr = (struct timerequest *)CreateIORequest(port, sizeof *tr)) == NULL
        || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0) != 0)
        goto out;
    opened = 1;
    TimerBase = tr->tr_node.io_Device;

    pkg_sha512_init(&pool);
    FPuts(out, (CONST_STRPTR)"This AROS has no random source, so the key is made from the moments you press keys.\n"
                             "Type anything, at random, until the count reaches 0. What you type is not kept.\n");
    Flush(out);
    SetMode(in, 1);
    while (need > 0) {
        unsigned char c;
        snprintf(line, sizeof line, "\r  %3d  ", (need + per_key - 1) / per_key);
        FPuts(out, (CONST_STRPTR)line);
        Flush(out);
        if (Read(in, &c, 1) != 1 || c == 3)     /* end of input, or Ctrl-C */
            break;
        memset(&ev, 0, sizeof ev);
        ReadEClock(&ev.e);
        ev.c = cycles();
        DateStamp(&ev.d);
        ev.task = FindTask(NULL);
        ev.mem = AvailMem(MEMF_ANY);
        ev.key = c;
        pkg_sha512_update(&pool, &ev, sizeof ev);
        if (c != last)
            need -= per_key;
        last = c;
    }
    SetMode(in, 0);
    FPuts(out, (CONST_STRPTR)"\r       \r");
    Flush(out);
    if (need <= 0) {
        pkg_sha512_final(&pool, digest);
        memcpy(buf, digest, len);
        rc = 0;
    }
    memset(&pool, 0, sizeof pool);
    memset(digest, 0, sizeof digest);
    memset(&ev, 0, sizeof ev);
out:
    if (opened) CloseDevice((struct IORequest *)tr);
    if (tr) DeleteIORequest((struct IORequest *)tr);
    if (port) DeleteMsgPort(port);
    return rc;
}
#else
int pkg_fs_random_typed(void *buf, size_t len)
{
    (void)buf; (void)len;
    return -2;
}
#endif

void (*pkg_fs_on_transfer)(long long done, long long total);
void (*pkg_fs_on_wait)(const char *host);
void (*pkg_fs_on_tick)(void);

/* Milliseconds, for the activity line's pace and for the trace's own account
 * of where a request's time went. Only differences are used. */
long long pkg_fs_now_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* What pkg is waiting for, where it can say so and nothing else can. */
static void waiting(const char *host)
{
    if (pkg_fs_on_wait != NULL)
        pkg_fs_on_wait(host);
}

int pkg_fs_random(void *buf, size_t len)
{
#ifdef __AROS__
    /* AROS gathers entropy in entropy.resource, and posixc hands it out
     * through getentropy, which takes 256 bytes at a time. An AROS older
     * than that has neither, and the caller falls back to typed keys. */
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        size_t k = len > 256 ? 256 : len;
        if (getentropy(p, k) != 0)
            return -1;
        p += k;
        len -= k;
    }
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (f == NULL)
        return -1;
    got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
#endif
}

/* A temporary name beside `path`, short whatever the file's name: FFS takes
 * names of 30 characters at most, and a file's own name may already be that
 * long. "<dir>/.pkg" and eight hex digits, created exclusively. */
static int open_tmp_beside(const char *path, char *tmp, size_t tl, int mode)
{
    static unsigned long counter;
    const char *slash = strrchr(path, '/');
    int dl, fd = -1, tries;
#ifdef __AROS__
    /* "RAM:GURU0" is in RAM:'s root: without the volume the temporary name
     * would land in the current directory, and the rename cross volumes */
    const char *colon = strrchr(path, ':');
    if (colon != NULL && (slash == NULL || colon > slash))
        slash = colon;
#endif
    dl = slash ? (int)(slash - path) + 1 : 0;
    for (tries = 0; tries < 16 && fd < 0; tries++) {
        unsigned char r[4];
        unsigned long v;
        if (pkg_fs_random(r, sizeof r) == 0)
            v = (unsigned long)r[0] << 24 | (unsigned long)r[1] << 16 | (unsigned long)r[2] << 8 | r[3];
        else    /* no random source: the process and a counter */
            v = ((unsigned long)getpid() * 2654435761ul + ++counter * 40503ul + (unsigned long)time(NULL))
                & 0xFFFFFFFFul;
        snprintf(tmp, tl, "%.*s.pkg%08lx", dl, path, v);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    return fd;
}

static int write_atomic_mode(const char *path, const void *buf, size_t len, int mode)
{
    size_t lp = strlen(path);
    char *tmp = (char *)malloc(lp + 16u);
    int fd;
    const unsigned char *b = (const unsigned char *)buf;

    if (tmp == NULL)
        return -1;
    if (mkparents(path) != 0) { free(tmp); return -1; }
    fd = open_tmp_beside(path, tmp, lp + 16u, mode);
    if (fd < 0) { free(tmp); return -1; }
    while (len > 0) {
        ssize_t w = write(fd, b, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp); free(tmp); return -1;
        }
        b += w;
        len -= (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int pkg_fs_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* stat, following links: a directory reached through a symlink is still a
 * directory to create files under. On macOS /var is exactly that, a link to
 * /private/var, and every temporary directory lives beneath it. The walk that
 * builds a package keeps its own lstat, since there a link must be refused. */
int pkg_fs_path_is_link(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    return S_ISLNK(st.st_mode) ? 1 : 0;
}

int pkg_fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int pkg_fs_rename(const char *from, const char *to)
{
    if (mkparents(to) != 0)
        return -1;
    return rename(from, to);
}

int pkg_fs_unlink(const char *path)
{
    return unlink(path);
}

int pkg_fs_rmtree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (!pkg_fs_exists(path))
        return 0;
    if (lstat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    d = opendir(path);
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char *c;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        c = pkg_join(path, e->d_name);
        if (c == NULL || pkg_fs_rmtree(c) != 0) { free(c); closedir(d); return -1; }
        free(c);
    }
    closedir(d);
    return rmdir(path);
}

void pkg_fs_prune_empty_parents(const char *root, const char *rel)
{
    char *r = strdup(rel), *slash;
    if (r == NULL)
        return;
    while ((slash = strrchr(r, '/')) != NULL) {
        char *full;
        *slash = '\0';
        full = pkg_join(root, r);
        if (full == NULL || rmdir(full) != 0) { free(full); break; }
        free(full);
    }
    free(r);
}

/* What pkg_fs.h says a walk leaves out: the same rule on every host, since
 * drawers travel between them. */
static int skip_host_metadata(const char *name)
{
    return name[0] == '.' || strcmp(name, "Icon\r") == 0
        || strcasecmp(name, "Thumbs.db") == 0 || strcasecmp(name, "desktop.ini") == 0;
}
static int walk(const char *root, const char *rel, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip,
                void *ctx, unsigned *skipped, char *err, size_t errlen)
{
    char *dir = rel[0] ? pkg_join(root, rel) : strdup(root);
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (dir == NULL)
        return -1;
    d = opendir(dir);
    if (d == NULL) {
        snprintf(err, errlen, "cannot open \"%s\": %s", dir, strerror(errno));
        free(dir);
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char *child_rel, *child;
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        child_rel = rel[0] ? pkg_join(rel, e->d_name) : strdup(e->d_name);
        child = child_rel ? pkg_join(root, child_rel) : NULL;
        if (child != NULL && skip_host_metadata(e->d_name)) {
            if (skipped) (*skipped)++;
            if (skip) skip(child_rel, lstat(child, &st) == 0 && S_ISDIR(st.st_mode), ctx);
        } else if (child == NULL || lstat(child, &st) != 0) {
            snprintf(err, errlen, "cannot read \"%s\"", child ? child : e->d_name);
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = walk(root, child_rel, fn, skip, ctx, skipped, err, errlen);
        } else if (S_ISREG(st.st_mode)) {
            rc = fn(child_rel, ctx);
        } else {
            snprintf(err, errlen, "\"%s\" is a symlink or special file; a package holds regular files only",
                     child_rel);
            rc = -1;
        }
        free(child_rel);
        free(child);
    }
    closedir(d);
    free(dir);
    return rc;
}

int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip, void *ctx,
                unsigned *skipped, char *err, size_t errlen)
{
    if (skipped)
        *skipped = 0;
    if (errlen)
        err[0] = '\0';
    return walk(root, "", fn, skip, ctx, skipped, err, errlen);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int pkg_fs_list(const char *dir, char ***names, size_t *count)
{
    DIR *d;
    struct dirent *e;
    char **v = NULL;
    size_t n = 0, cap = 0;

    *names = NULL;
    *count = 0;
    if (!pkg_fs_is_dir(dir))
        return 0;                 /* absent: an empty list, whatever errno says */
    d = opendir(dir);
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2u : 16u;
            char **w = (char **)realloc(v, ncap * sizeof *w);
            if (w == NULL) break;
            v = w;
            cap = ncap;
        }
        v[n] = strdup(e->d_name);
        if (v[n] == NULL) break;
        n++;
    }
    closedir(d);
    if (n > 1u)
        qsort(v, n, sizeof *v, cmp_str);
    *names = v;
    *count = n;
    return 0;
}

/* ---- Amiga attributes -------------------------------------------------- */

int pkg_fs_owner_exec(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (st.st_mode & S_IXUSR) != 0;
}

int pkg_fs_set_owner_exec(const char *path, int exec)
{
    struct stat st;
    mode_t m;
    if (stat(path, &st) != 0)
        return -1;
    m = exec ? (st.st_mode | S_IXUSR) : (st.st_mode & ~(mode_t)S_IXUSR);
    return m == st.st_mode ? 0 : chmod(path, m & 07777);
}

#ifdef __AROS__
int pkg_fs_amiga_get(const char *path, unsigned long long *prot, char *comment, size_t cl)
{
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int rc = -1;
    if (lock == BNULL)
        return -1;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib != NULL && Examine(lock, fib)) {
        *prot = (unsigned long long)(ULONG)fib->fib_Protection;
        snprintf(comment, cl, "%s", (const char *)fib->fib_Comment);
        rc = 1;
    }
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

int pkg_fs_amiga_set(const char *path, unsigned long long prot, const char *comment_latin1)
{
    if (!SetProtection((CONST_STRPTR)path, (ULONG)(prot & 0xFFFFFFFFull)))
        return -1;
    if (!SetComment((CONST_STRPTR)path, (CONST_STRPTR)(comment_latin1 ? comment_latin1 : "")))
        return -1;
    return 1;
}
#else
int pkg_fs_amiga_get(const char *path, unsigned long long *prot, char *comment, size_t cl)
{
    (void)path; (void)prot; (void)comment; (void)cl;
    return 0;
}

int pkg_fs_amiga_set(const char *path, unsigned long long prot, const char *comment_latin1)
{
    (void)path; (void)prot; (void)comment_latin1;
    return 0;
}
#endif

void pkg_fs_unprotect(const char *path)
{
#ifdef __AROS__
    SetProtection((CONST_STRPTR)path, 0);
#else
    (void)path;
#endif
}

int pkg_fs_identity(const char *path, struct pkg_fs_id *id)
{
    struct stat st;
    memset(id, 0, sizeof *id);
    if (stat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    id->exists = 1;
    id->dev = (unsigned long long)st.st_dev;
    id->ino = (unsigned long long)st.st_ino;
    id->size = (unsigned long long)st.st_size;
#if defined(__APPLE__)
    id->mtime_s = (long long)st.st_mtimespec.tv_sec;
    id->mtime_ns = (long long)st.st_mtimespec.tv_nsec;
#elif defined(__AROS__)
    id->mtime_s = (long long)st.st_mtime;
#else
    id->mtime_s = (long long)st.st_mtim.tv_sec;
    id->mtime_ns = (long long)st.st_mtim.tv_nsec;
#endif
    return 0;
}

static int same_id(const struct pkg_fs_id *a, const struct pkg_fs_id *b)
{
    return a->exists == b->exists && (!a->exists
        || (a->dev == b->dev && a->ino == b->ino && a->size == b->size
            && a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns));
}

int pkg_fs_replace_if_same(const char *path, const struct pkg_fs_id *before,
                           const void *buf, size_t len)
{
    struct pkg_fs_id now;
    char *tmp = NULL;
    if (buf != NULL) {
        size_t pl = strlen(path);
        int fd = -1, tries;
        tmp = (char *)malloc(pl + 16);
        if (tmp == NULL)
            return -1;
        /* ".ameta." and a random suffix, created exclusively: what mkstemp
         * does, written out because AROS's C library has no mkstemp. */
        for (tries = 0; tries < 8 && fd < 0; tries++) {
            unsigned char r[4];
            if (pkg_fs_random(r, sizeof r) != 0) {
                /* no random source: a name only has to be unused */
                static unsigned long counter;
                unsigned long v = (unsigned long)getpid() * 2654435761ul + ++counter * 40503ul + (unsigned long)time(NULL);
                r[0] = (unsigned char)(v >> 24); r[1] = (unsigned char)(v >> 16); r[2] = (unsigned char)(v >> 8); r[3] = (unsigned char)v;
            }
            snprintf(tmp, pl + 16, "%s.%02x%02x%02x%02x", path, r[0], r[1], r[2], r[3]);
            fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
        }
        if (fd < 0) { free(tmp); return -1; }
        if (write(fd, buf, len) != (ssize_t)len || fsync(fd) != 0) {
            close(fd); unlink(tmp); free(tmp);
            return -1;
        }
        close(fd);
        chmod(tmp, 0644);
    }
    if (pkg_fs_identity(path, &now) != 0 || !same_id(&now, before)) {
        if (tmp) { unlink(tmp); free(tmp); }
        return 1;
    }
    if (tmp != NULL) {
        int rc = rename(tmp, path);
        if (rc != 0) unlink(tmp);
        free(tmp);
        return rc == 0 ? 0 : -1;
    }
    return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
}

void *pkg_fs_lock_dir(const char *dir)
{
#if defined(__AROS__)
    (void)dir;
    return NULL;
#else
    int *h, fd = open(dir, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (flock(fd, LOCK_EX) != 0 || (h = (int *)malloc(sizeof *h)) == NULL) {
        close(fd);
        return NULL;
    }
    *h = fd;
    return h;
#endif
}

void pkg_fs_unlock_dir(void *lock)
{
#if !defined(__AROS__)
    if (lock != NULL) {
        flock(*(int *)lock, LOCK_UN);
        close(*(int *)lock);
        free(lock);
    }
#else
    (void)lock;
#endif
}

/* ---- the network ------------------------------------------------------ */

static int net_connect(int s, const void *address, unsigned int length);

void (*pkg_fs_on_trace)(const char *line);

/* A line for the TRACE, when one is running. */
static void nettr(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (pkg_fs_on_trace == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pkg_fs_on_trace(line);
}

#if defined(__AROS__)
/* The network on AROS is bsdsocket.library, which a TCP/IP stack provides
 * once it is started (AROSTCP; on a hosted AROS, the host's own sockets).
 * The library is opened once and kept for the program's life, because the
 * connections under it are: see the pool below. */
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include "pkg_tls.h"

struct Library *SocketBase;

static int net_start(char *err, size_t errlen)
{
    if (SocketBase != NULL)
        return 0;
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
    if (SocketBase == NULL) {
        snprintf(err, errlen, "this machine's network is not started: bsdsocket.library does not open. "
                 "Start the network (AROSTCP), or copy the channel to a volume and name that drawer");
        return -1;
    }
    return 0;
}

/* AROS socket bases and resolver storage belong to the task that opens
 * them. The worker owns its base and copies the answer before closing it. */
struct host_lookup {
    pthread_mutex_t mutex;
    const char *host;
    struct in_addr address;
    int done, found;
};

static void *lookup_host(void *user)
{
    struct host_lookup *lookup = user;
    struct Library *SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
    struct in_addr address;
    int found = 0;
    if (SocketBase != NULL) {
        struct hostent *he = gethostbyname((char *)lookup->host);
        if (he != NULL && he->h_addrtype == AF_INET && he->h_length == sizeof address
            && he->h_addr_list != NULL && he->h_addr_list[0] != NULL) {
            memcpy(&address, he->h_addr_list[0], sizeof address);
            found = 1;
        }
        CloseLibrary(SocketBase);
    }
    pthread_mutex_lock(&lookup->mutex);
    if (found) lookup->address = address;
    lookup->found = found;
    lookup->done = 1;
    pthread_mutex_unlock(&lookup->mutex);
    return NULL;
}

static int resolve_host(const char *host, struct in_addr *address)
{
    struct host_lookup lookup;
    pthread_t worker;
    int done;
    memset(&lookup, 0, sizeof lookup);
    lookup.host = host;
    if (pthread_mutex_init(&lookup.mutex, NULL) != 0) return -1;
    if (pthread_create(&worker, NULL, lookup_host, &lookup) != 0) {
        pthread_mutex_destroy(&lookup.mutex);
        return -1;
    }
    do {
        pthread_mutex_lock(&lookup.mutex);
        done = lookup.done;
        pthread_mutex_unlock(&lookup.mutex);
        if (!done) {
            if (pkg_fs_on_tick) pkg_fs_on_tick();
            Delay(5);
        }
    } while (!done);
    pthread_join(worker, NULL);
    pthread_mutex_destroy(&lookup.mutex);
    if (lookup.found) *address = lookup.address;
    return lookup.found ? 0 : -1;
}

static int net_open(const char *host, const char *port, int tls, void **tlsh, char *err, size_t errlen)
{
    static int prepared;                /* the authorities are read once per process */
    struct sockaddr_in sa;
    long long t0, t1;
    int s;

    *tlsh = NULL;
    if (net_start(err, errlen) != 0)
        return -1;
    /* Finding the host, the connection and the handshake all block with
     * nothing to report: say what is being waited for, once. */
    waiting(host);
    t0 = pkg_fs_now_ms();
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)atoi(port));  /* a socket port, not the package format */
    sa.sin_addr.s_addr = inet_addr((char *)host);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        if (resolve_host(host, &sa.sin_addr) != 0) {
            snprintf(err, errlen, "cannot find the host %s: check the network's name servers", host);
            return -1;
        }
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || net_connect(s, &sa, sizeof sa) != 0) {
        if (s >= 0) CloseSocket(s);
        snprintf(err, errlen, "cannot connect to %s:%s", host, port);
        return -1;
    }
    t1 = pkg_fs_now_ms();
    nettr("net: connect %s:%s in %lld ms", host, port, t1 - t0);
    if (tls) {
        long long t2;
        if (pkg_tls_prepare(err, errlen) != 0) {
            CloseSocket(s);
            return -1;
        }
        t2 = pkg_fs_now_ms();
        if (prepared++ == 0)            /* the once-per-process work, and what it cost */
            nettr("net: the certificate authorities, read once: %lld ms", t2 - t1);
        *tlsh = pkg_tls_open(s, host, err, errlen);
        if (*tlsh == NULL) {
            CloseSocket(s);
            return -1;
        }
        nettr("net: TLS handshake with %s in %lld ms", host, pkg_fs_now_ms() - t2);
    }
    waiting(NULL);
    return s;
}
static ssize_t net_read(int s, void *t, void *buf, size_t n)
{
    if (t != NULL) return pkg_tls_read((struct pkg_tls *)t, buf, n);
    if (pkg_fs_socket_wait(s, 0) != 0) return -1;
    return (ssize_t)recv(s, buf, (LONG)n, 0);
}
static ssize_t net_write(int s, void *t, const void *buf, size_t n)
{
    return t != NULL ? pkg_tls_write((struct pkg_tls *)t, buf, n) : (ssize_t)send(s, (APTR)buf, (LONG)n, 0);
}
static void net_close(int s, void *t)
{
    if (SocketBase == NULL) return;
    if (t != NULL) pkg_tls_close((struct pkg_tls *)t);
    CloseSocket(s);
}

/* Downloads are kept on the system volume so that a reboot does not fetch
 * them again; RAM: when that volume cannot be written (a CD, a full disk). */
char *pkg_cache_dir(void)
{
    const char *e = getenv("PKG_CACHE");
    char *p = (char *)malloc(64 + (e ? strlen(e) : 0));
    if (p == NULL) return NULL;
    if (e && *e) { strcpy(p, e); return p; }
    mkdir("SYS:.pkg", 0755);
    if (mkdir("SYS:.pkg/cache", 0755) == 0 || pkg_fs_is_dir("SYS:.pkg/cache"))
        strcpy(p, "SYS:.pkg/cache");
    else
        strcpy(p, "RAM:pkg-cache");
    return p;
}
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;

char *pkg_cache_dir(void)
{
    const char *e = getenv("PKG_CACHE"), *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    size_t n = 32 + (e ? strlen(e) : 0) + (x ? strlen(x) : 0) + (h ? strlen(h) : 0);
    char *p = (char *)malloc(n);
    if (p == NULL) return NULL;
    if (e && *e) snprintf(p, n, "%s", e);
    else if (x && *x) snprintf(p, n, "%s/pkg", x);
    else if (h && *h) snprintf(p, n, "%s/.cache/pkg", h);
    else snprintf(p, n, "/tmp/pkg-cache");
    return p;
}

static int send_https(const char *method, const char *url, const char *body_file,
                      const char *header_file, const char *out_file, int *code,
                      char *err, size_t errlen)
{
    char data[1100], hdr[1100], codebuf[32];
    char *argv[24];
    int n = 0, st, fd, saved;
    pid_t pid;
    posix_spawn_file_actions_t fa;
    char codefile[] = "/tmp/pkg-code.XXXXXX";

    argv[n++] = "curl"; argv[n++] = "-sS"; argv[n++] = "-A"; argv[n++] = PKG_USER_AGENT;
    argv[n++] = "-X"; argv[n++] = (char *)method;
    if (body_file) {
        /* streamed from the file, never held whole: an archive is large */
        snprintf(data, sizeof data, "%s", body_file);
        argv[n++] = "-T"; argv[n++] = data;
        argv[n++] = "-H"; argv[n++] = "Content-Type: application/octet-stream";
        argv[n++] = "-H"; argv[n++] = "Expect:";
    }
    if (header_file) {
        snprintf(hdr, sizeof hdr, "@%s", header_file);
        argv[n++] = "-H"; argv[n++] = hdr;
    }
    argv[n++] = "-o"; argv[n++] = (char *)out_file;
    argv[n++] = "-w"; argv[n++] = "%{http_code}";
    argv[n++] = (char *)url;
    argv[n] = NULL;
    fd = mkstemp(codefile);
    if (fd < 0) { snprintf(err, errlen, "cannot make a temporary file"); return -1; }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 1);
    saved = posix_spawnp(&pid, "curl", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd);
    if (saved != 0) { unlink(codefile); snprintf(err, errlen, "PUSH needs curl, which is not on this machine's PATH"); return -1; }
    for (;;) {
        pid_t w = waitpid(pid, &st, pkg_fs_on_tick ? WNOHANG : 0);
        if (w == pid) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            unlink(codefile); snprintf(err, errlen, "curl did not finish"); return -1;
        }
        pkg_fs_on_tick();
        usleep(100000);
    }
    if (!WIFEXITED(st)) { unlink(codefile); snprintf(err, errlen, "curl did not finish"); return -1; }
    fd = open(codefile, O_RDONLY);
    n = fd >= 0 ? (int)read(fd, codebuf, sizeof codebuf - 1) : 0;
    if (fd >= 0) close(fd);
    unlink(codefile);
    codebuf[n > 0 ? n : 0] = '\0';
    *code = atoi(codebuf);
    if (*code == 0) {
        snprintf(err, errlen, "no answer from %s (curl exit %d)", url, WEXITSTATUS(st));
        return -1;
    }
    return 0;
}

/* The host a URL names, for the line that says what pkg is waiting for. */
static void url_host(const char *url, char *out, size_t ol)
{
    const char *p = strstr(url, "://"), *e;
    size_t n;
    out[0] = '\0';
    if (p == NULL)
        return;
    p += 3;
    for (e = p; *e && *e != '/' && *e != ':'; e++)
        ;
    n = (size_t)(e - p);
    if (n >= ol) n = ol - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* The Content-Length of the answer curl is writing, from the headers it
 * dumps as they arrive: the last one in the file, because a redirect's
 * headers come before the answer's. -1 while it is not known, which is what
 * a chunked answer stays. The file is read while curl writes it, so a value
 * counts only once its line has ended. */
static long long dumped_length(const char *path)
{
    unsigned char *b;
    size_t len, at = 0;
    long long clen = -1, complete = -1;
    int status = 0, chunked = 0;
    if (pkg_fs_read(path, &b, &len) != 0)
        return -1;
    while (at < len) {
        const unsigned char *end = memchr(b + at, '\n', len - at);
        char line[1024];
        size_t n;
        if (end == NULL) break;
        n = (size_t)(end - b) - at;
        if (n && b[at + n - 1] == '\r') n--;
        if (n < sizeof line) {
            memcpy(line, b + at, n); line[n] = '\0';
            if (!strncmp(line, "HTTP/", 5)) {
                const char *space = strchr(line, ' ');
                status = space ? atoi(space + 1) : 0;
                clen = complete = -1; chunked = 0;
            } else if (!strncasecmp(line, "Content-Length:", 15)) {
                clen = strtoll(line + 15, NULL, 10);
            } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
                chunked = 1;
            } else if (n == 0 && status >= 200 && status < 300) {
                complete = chunked ? -1 : clen;
            }
        }
        at = (size_t)(end - b) + 1;
    }
    free(b);
    return complete > 0 ? complete : -1;
}

/* https: the system's curl, with no shell in between. */
static int get_with_curl(const char *url, const char *tmp, char *err, size_t errlen)
{
    /* -s alone: a 404 for a file that may not exist (a withdrawal) is no
     * error, and the exit code says the rest. -D: the answer's headers, so
     * that a person is told how much of the file is still to come. */
    char hdrs[2200], host[300];
    char *argv[] = { "curl", "-s", "-f", "-L", "--max-redirs", "5", "-A", PKG_USER_AGENT,
                     "-D", hdrs, "-o", (char *)tmp, (char *)url, NULL };
    pid_t pid;
    int st, said_waiting = 0;
    long long clen = -1;
    snprintf(hdrs, sizeof hdrs, "%.2190s.head", tmp);
    unlink(hdrs);
    url_host(url, host, sizeof host);
    if (pkg_fs_on_wait != NULL && host[0]) { waiting(host); said_waiting = 1; }
    if (posix_spawnp(&pid, "curl", NULL, NULL, argv, environ) != 0) {
        snprintf(err, errlen, "https needs curl, which is not on this machine's PATH");
        waiting(NULL);
        return -1;
    }
    /* curl says nothing (-s): the growing file is what a watching person is shown */
    for (;;) {
        pid_t w = waitpid(pid, &st, pkg_fs_on_transfer ? WNOHANG : 0);
        struct stat sb;
        if (w == pid) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errlen, "curl did not finish"); unlink(hdrs); return -1;
        }
        if (clen < 0 && pkg_fs_on_transfer) {
            clen = dumped_length(hdrs);
            if (clen > 0) {
                waiting(NULL); said_waiting = 0;
                pkg_fs_on_transfer(0, clen);
            }
        }
        if (stat(tmp, &sb) == 0 && sb.st_size > 0 && pkg_fs_on_transfer) {
            if (said_waiting) { waiting(NULL); said_waiting = 0; }
            if (clen < 0) clen = dumped_length(hdrs);
            pkg_fs_on_transfer((long long)sb.st_size, clen);
        } else if (said_waiting) {
            waiting(host);       /* nothing yet: the mark says pkg is alive */
        }
        if (pkg_fs_on_tick) pkg_fs_on_tick();
        usleep(100000);
    }
    waiting(NULL);
    unlink(hdrs);
    if (!WIFEXITED(st)) {
        snprintf(err, errlen, "curl did not finish");
        return -1;
    }
    if (WEXITSTATUS(st) == 22) {                /* -f: an HTTP error; Pkg asks for files that may not exist */
        char words[400] = "";
        int c = 0;
        if (send_https("GET", url, NULL, NULL, tmp, &c, words, sizeof words) == 0 && c == 426) {
            unsigned char *b; size_t bl;
            if (pkg_fs_read(tmp, &b, &bl) == 0) {
                const char *r = strstr((const char *)b, "reason: "), *nx = strstr((const char *)b, "next: ");
                int rl = r ? (int)strcspn(r + 8, "\r\n") : 0, nl = nx ? (int)strcspn(nx + 6, "\r\n") : 0;
                snprintf(err, errlen, "%.*s%s%.*s", rl ? rl : 40, rl ? r + 8 : "the server asks for a newer pkg",
                         nl ? ". " : "", nl, nl ? nx + 6 : "");
                free(b);
            }
            unlink(tmp);
            return -1;
        }
        unlink(tmp);
        return 1;
    }
    if (WEXITSTATUS(st) != 0) {
        snprintf(err, errlen, "curl failed with exit code %d fetching %s", WEXITSTATUS(st), url);
        return -1;
    }
    return 0;
}

/* The socket under the HTTP client: a name and a port in, a stream out. */
static int net_open(const char *host, const char *port, int tls, void **tlsh, char *err, size_t errlen)
{
    struct addrinfo hints, *ai = NULL, *a;
    long long t0 = pkg_fs_now_ms();
    int s = -1;
    (void)tls;                                  /* https here is curl's, never this socket's */
    *tlsh = NULL;
    waiting(host);                              /* a name lookup and a connect both block */
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) {
        snprintf(err, errlen, "cannot find the host %s", host);
        return -1;
    }
    for (a = ai; a && s < 0; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s >= 0 && net_connect(s, a->ai_addr, (unsigned int)a->ai_addrlen) != 0) { close(s); s = -1; }
    }
    freeaddrinfo(ai);
    if (s < 0) snprintf(err, errlen, "cannot connect to %s:%s", host, port);
    else nettr("net: connect %s:%s in %lld ms", host, port, pkg_fs_now_ms() - t0);
    waiting(NULL);
    return s;
}
static ssize_t net_read(int s, void *t, void *buf, size_t n)
{
    (void)t;
    if (pkg_fs_socket_wait(s, 0) != 0) return -1;
    return read(s, buf, n);
}
static ssize_t net_write(int s, void *t, const void *buf, size_t n) { (void)t; return write(s, buf, n); }
static void net_close(int s, void *t) { (void)t; close(s); }
#define get_https get_with_curl

#endif

/* A timeout gives the display a turn even when no bytes have arrived. */
int pkg_fs_socket_wait(int socket, int writing)
{
    if (socket < 0 || socket >= FD_SETSIZE) { errno = EINVAL; return -1; }
    for (;;) {
        fd_set fds;
        struct timeval timeout;
        int rc;
        FD_ZERO(&fds);
        FD_SET(socket, &fds);
        timeout.tv_sec = 0; timeout.tv_usec = 100000;
#ifdef __AROS__
        rc = WaitSelect(socket + 1, writing ? NULL : &fds,
                        writing ? &fds : NULL, NULL, &timeout, NULL);
#else
        rc = select(socket + 1, writing ? NULL : &fds,
                    writing ? &fds : NULL, NULL, &timeout);
#endif
        if (rc > 0) return 0;
        if (rc < 0) {
#ifdef __AROS__
            if (Errno() == EINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (pkg_fs_on_tick) pkg_fs_on_tick();
    }
}

/* The connection can take time before its first readable byte. */
static int net_connect(int s, const void *address, unsigned int length)
{
    int rc, error = 0;
    socklen_t size = sizeof error;
#ifdef __AROS__
    LONG nonblock = 1;
    if (IoctlSocket(s, FIONBIO, (char *)&nonblock) != 0) return -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
#endif
    rc = connect(s, (const struct sockaddr *)address, length);
#ifdef __AROS__
    if (rc < 0 && (Errno() == EINPROGRESS || Errno() == EWOULDBLOCK)) {
#else
    if (rc < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
#endif
        rc = pkg_fs_socket_wait(s, 1);
        if (rc == 0 && (getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&error, &size) != 0 || error))
            rc = -1;
    }
#ifdef __AROS__
    nonblock = 0;
    if (IoctlSocket(s, FIONBIO, (char *)&nonblock) != 0) rc = -1;
#else
    if (fcntl(s, F_SETFL, flags) < 0) rc = -1;
#endif
    return rc;
}

/* ---- the HTTP client, the same on every system ------------------------ *
 *
 * HTTP/1.1 with the connection held open. A channel is hundreds of small
 * files, and on AROS, where this client does the https itself, a new TCP
 * connection and a full TLS handshake for each of them cost about a second
 * apiece. So a connection is kept after a response and the next request for
 * the same scheme, host and port is sent down it.
 *
 * What that asks of the reader: the end of a response must be known from
 * its framing, never from the close of the connection. A response with a
 * Content-Length is read to exactly that many bytes, a chunked one to its
 * zero chunk, and one with neither is read to the close and the connection
 * is then dropped. The body of an answer Pkg does not keep (a 404 for a
 * withdrawal that is not there, a redirect) is read and discarded for the
 * same reason.
 *
 * A connection is never reused across a host, a port or a scheme: on https
 * the certificate is checked once, during the handshake, and a session
 * belongs to the host it was checked against. A kept connection may have
 * been closed by the server in the meantime; that shows as a failure before
 * any of the response has arrived, and a GET is then sent once more on a
 * fresh connection.
 *
 * PKG_NO_KEEPALIVE=1 turns the reuse off and is what the test's negative
 * control uses. */

#define NET_POOL 2                      /* the channel's host, and an upstream one */
#define NET_BUF  16384

struct netconn {
    int   used;
    int   sock;
    void *tls;
    int   scheme_tls;
    char  host[256];
    char  port[8];
    char  buf[NET_BUF];                 /* what was read past what was wanted */
    size_t len, at;
    unsigned long stamp;                /* for the slot to give up first */
};

static struct netconn pool[NET_POOL];
static unsigned long net_clock;

static int keepalive_off(void)
{
    const char *e = getenv("PKG_NO_KEEPALIVE");
    return e != NULL && e[0] == '1';
}

static void conn_drop(struct netconn *c)
{
    if (!c->used)
        return;
    net_close(c->sock, c->tls);
    c->used = 0;
    c->tls = NULL;
    c->len = c->at = 0;
}

void pkg_net_idle_close(void)
{
    int i;
    for (i = 0; i < NET_POOL; i++)
        conn_drop(&pool[i]);
}

/* The connection for this request: the one already open to this host, or a
 * new one. *reused says which, so a failure before the response can be told
 * from a server that is not there. */
static struct netconn *conn_take(const char *host, const char *port, int tls, int allow_reuse,
                                 int *reused, char *err, size_t errlen)
{
    struct netconn *c = NULL;
    int i;

    *reused = 0;
    if (allow_reuse && !keepalive_off()) {
        for (i = 0; i < NET_POOL; i++) {
            struct netconn *p = &pool[i];
            if (p->used && p->scheme_tls == tls && strcmp(p->host, host) == 0
                && strcmp(p->port, port) == 0) {
                if (p->at < p->len) {   /* bytes left over: its framing is not trusted */
                    conn_drop(p);
                    break;
                }
                p->stamp = ++net_clock;
                *reused = 1;
                nettr("net: the connection to %s://%s:%s is still open, and is used again",
                      tls ? "https" : "http", host, port);
                return p;
            }
        }
    }
    /* Never two connections to the same place at once: one is all a client
     * needs, and a server that answers one connection at a time would never
     * reach the second. */
    for (i = 0; i < NET_POOL; i++)
        if (pool[i].used && pool[i].scheme_tls == tls && strcmp(pool[i].host, host) == 0
            && strcmp(pool[i].port, port) == 0)
            conn_drop(&pool[i]);
    for (i = 0; i < NET_POOL; i++)
        if (!pool[i].used) { c = &pool[i]; break; }
    if (c == NULL) {
        c = &pool[0];
        for (i = 1; i < NET_POOL; i++)
            if (pool[i].stamp < c->stamp) c = &pool[i];
        conn_drop(c);
    }
    c->sock = net_open(host, port, tls, &c->tls, err, errlen);
    if (c->sock < 0)
        return NULL;
    c->used = 1;
    c->scheme_tls = tls;
    c->len = c->at = 0;
    c->stamp = ++net_clock;
    snprintf(c->host, sizeof c->host, "%s", host);
    snprintf(c->port, sizeof c->port, "%s", port);
    return c;
}

/* Reading through the connection's own buffer, so that what came in after
 * one response is there for the next. */
static ssize_t conn_read(struct netconn *c, void *buf, size_t n)
{
    if (c->at == c->len) {
        ssize_t k;
        if (n >= NET_BUF)               /* a large read goes straight through */
            return net_read(c->sock, c->tls, buf, n);
        k = net_read(c->sock, c->tls, c->buf, sizeof c->buf);
        if (k <= 0)
            return k;
        c->len = (size_t)k;
        c->at = 0;
    }
    {
        size_t k = c->len - c->at;
        if (k > n) k = n;
        memcpy(buf, c->buf + c->at, k);
        c->at += k;
        return (ssize_t)k;
    }
}

/* One line ending in CRLF, without it. -1 when the connection ended first. */
static int conn_line(struct netconn *c, char *line, size_t n)
{
    size_t at = 0;
    for (;;) {
        char ch;
        if (conn_read(c, &ch, 1) != 1)
            return -1;
        if (ch == '\n') {
            while (at > 0 && line[at - 1] == '\r') at--;
            line[at] = '\0';
            return 0;
        }
        if (at + 1 < n)
            line[at++] = ch;
    }
}

/* Where a response's body goes: a file, memory (the first `cap` bytes of
 * it, for the words of a 426), or nowhere. */
struct sink_body {
    int    fd;
    char  *mem;
    size_t cap, len;
    long long total;
};

static int body_put(struct sink_body *b, const char *buf, size_t n)
{
    b->total += (long long)n;
    if (b->fd >= 0) {
        size_t at = 0;
        while (at < n) {
            ssize_t w = write(b->fd, buf + at, n - at);
            if (w <= 0) return -1;
            at += (size_t)w;
        }
        return 0;
    }
    if (b->mem != NULL && b->len < b->cap) {
        size_t k = b->cap - b->len;
        if (k > n) k = n;
        memcpy(b->mem + b->len, buf, k);
        b->len += k;
        b->mem[b->len] = '\0';
    }
    return 0;
}

/* The body, by its framing. 0 read whole and the connection may be used
 * again, 1 read whole but the connection must be dropped, -1 a failure. */
static int read_body(struct netconn *c, long long clen, int chunked, struct sink_body *b,
                     const char *host, char *err, size_t errlen)
{
    char buf[65536];
    ssize_t n;

    if (pkg_fs_on_transfer && b->fd >= 0)
        pkg_fs_on_transfer(0, chunked ? -1 : clen);
    if (chunked) {
        for (;;) {
            char line[64];
            unsigned long size;
            long long got = 0;
            if (conn_line(c, line, sizeof line) != 0) {
                snprintf(err, errlen, "%s ended a chunked reply early", host);
                return -1;
            }
            size = strtoul(line, NULL, 16);
            if (size == 0)
                break;
            while (got < (long long)size) {
                size_t want = (size_t)((long long)size - got);
                if (want > sizeof buf) want = sizeof buf;
                n = conn_read(c, buf, want);
                if (n <= 0) { snprintf(err, errlen, "%s ended a chunked reply early", host); return -1; }
                if (body_put(b, buf, (size_t)n) != 0) { snprintf(err, errlen, "cannot write the download: %s", strerror(errno)); return -1; }
                got += n;
                if (pkg_fs_on_transfer && b->fd >= 0)
                    pkg_fs_on_transfer(b->total, -1);
            }
            if (conn_line(c, buf, sizeof buf) != 0) { snprintf(err, errlen, "%s ended a chunked reply early", host); return -1; }
        }
        for (;;) {                       /* the trailer, up to the empty line */
            char line[256];
            if (conn_line(c, line, sizeof line) != 0) { snprintf(err, errlen, "%s ended a chunked reply early", host); return -1; }
            if (line[0] == '\0') break;
        }
        return 0;
    }
    if (clen >= 0) {
        long long got = 0;
        while (got < clen) {
            size_t want = (size_t)(clen - got);
            if (want > sizeof buf) want = sizeof buf;
            n = conn_read(c, buf, want);
            if (n <= 0) {
                snprintf(err, errlen, "%s sent %lld of %lld bytes", host, got, clen);
                return -1;
            }
            if (body_put(b, buf, (size_t)n) != 0) { snprintf(err, errlen, "cannot write the download: %s", strerror(errno)); return -1; }
            got += n;
            if (pkg_fs_on_transfer && b->fd >= 0)
                pkg_fs_on_transfer(b->total, clen);
        }
        return 0;
    }
    /* neither a length nor chunks: the close is the end, and the connection
     * cannot be used again */
    while ((n = conn_read(c, buf, sizeof buf)) > 0) {
        if (body_put(b, buf, (size_t)n) != 0) { snprintf(err, errlen, "cannot write the download: %s", strerror(errno)); return -1; }
        if (pkg_fs_on_transfer && b->fd >= 0)
            pkg_fs_on_transfer(b->total, -1);
    }
    return 1;
}

/* One request with a body, for PUSH: the headers of header_file, the body
 * of body_file, the answer's body in out_file and its status in *code. */
int pkg_net_send(const char *method, const char *url, const char *body_file,
                 const char *header_file, const char *out_file, int *code,
                 char *err, size_t errlen)
{
    char host[256], port[8], head[8192], buf[65536];
    const char *p, *slash, *colon, *path;
    unsigned char *hdrs = NULL;
    size_t hl, hlen = 0, hdrlen = 0;
    struct stat sb;
    char *body;
    FILE *bf = NULL, *of = NULL;
    ssize_t n;
    void *tlsh = NULL;
    int s, rc = -1, tls = strncmp(url, "https://", 8) == 0;

#if !defined(__AROS__)
    if (tls)                                    /* the host systems hand https to curl */
        return send_https(method, url, body_file, header_file, out_file, code, err, errlen);
#endif
    if (!tls && strncmp(url, "http://", 7) != 0) { snprintf(err, errlen, "cannot send to %s: only http:// and https://", url); return -1; }
    snprintf(port, sizeof port, "%s", tls ? "443" : "80");
    p = url + (tls ? 8 : 7); slash = strchr(p, '/'); path = slash ? slash : "/";
    hl = slash ? (size_t)(slash - p) : strlen(p);
    colon = memchr(p, ':', hl);
    if (hl == 0 || hl >= sizeof host) { snprintf(err, errlen, "no host in %s", url); return -1; }
    if (colon) { snprintf(port, sizeof port, "%.*s", (int)(hl - (size_t)(colon - p) - 1), colon + 1); hl = (size_t)(colon - p); }
    snprintf(host, sizeof host, "%.*s", (int)hl, p);
    if (stat(body_file, &sb) != 0 || (bf = fopen(body_file, "rb")) == NULL) { snprintf(err, errlen, "cannot read %s", body_file); return -1; }
    if (header_file != NULL) pkg_fs_read(header_file, &hdrs, &hdrlen);
    /* A push asks for a connection of its own, and says so: its body may be
     * large and is streamed from a file, so a request that stopped halfway
     * cannot simply be sent again. It is never put in the pool. */
    s = net_open(host, port, tls, &tlsh, err, errlen);
    if (s < 0) { fclose(bf); free(hdrs); return -1; }
    {
        /* header_file holds "Name: value\n" lines; the wire wants \r\n */
        char req[8192];
        size_t at = (size_t)snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " PKG_USER_AGENT "\r\nConnection: close\r\n"
                                     "Content-Type: application/octet-stream\r\nContent-Length: %lld\r\n",
                                     method, path, host, (long long)sb.st_size), i;
        for (i = 0; i < hdrlen && at + 4 < sizeof req; i++) {
            if (hdrs[i] == '\n') { req[at++] = '\r'; req[at++] = '\n'; }
            else if (hdrs[i] != '\r') req[at++] = (char)hdrs[i];
        }
        if (at + 2 >= sizeof req) { snprintf(err, errlen, "the request's headers are too long"); goto done; }
        req[at++] = '\r'; req[at++] = '\n';
        if (net_write(s, tlsh, req, at) != (ssize_t)at) { snprintf(err, errlen, "cannot send to %s", host); goto done; }
    }
    for (;;) {
        size_t k = fread(buf, 1, sizeof buf, bf), sent = 0;
        if (k == 0) break;
        while (sent < k) {
            n = net_write(s, tlsh, buf + sent, k - sent);
            if (n <= 0) { snprintf(err, errlen, "%s stopped taking the upload", host); goto done; }
            sent += (size_t)n;
        }
    }
    /* the whole answer: these are short records */
    for (;;) {
        if (hlen + 1 >= sizeof head) break;
        n = net_read(s, tlsh, head + hlen, sizeof head - 1 - hlen);
        if (n <= 0) break;
        hlen += (size_t)n;
    }
    head[hlen] = '\0';
    if (sscanf(head, "HTTP/%*s %d", code) != 1 || (body = strstr(head, "\r\n\r\n")) == NULL) {
        snprintf(err, errlen, "%s did not answer HTTP", host);
        goto done;
    }
    body += 4;
    if (strstr(head, "chunked") != NULL && strstr(head, "chunked") < body) {
        /* a short chunked answer: drop the size lines */
        char *w = body, *r = body;
        for (;;) {
            unsigned long size = strtoul(r, NULL, 16);
            char *eol = strstr(r, "\r\n");
            if (eol == NULL || size == 0 || eol + 2 + size > head + hlen) break;
            memmove(w, eol + 2, size);
            w += size;
            r = eol + 2 + size + 2;
        }
        *w = '\0';
    }
    of = fopen(out_file, "wb");
    if (of == NULL) { snprintf(err, errlen, "cannot write %s", out_file); goto done; }
    fwrite(body, 1, strlen(body), of);
    fclose(of);
    rc = 0;
done:
    net_close(s, tlsh);
    fclose(bf);
    free(hdrs);
    return rc;
}

/* One GET on one connection. 0 fetched, 1 the server has no such file,
 * 3 a redirect (Location in `location`), 4 a connection that had been kept
 * open was not there any more and the GET may be sent again on a new one,
 * -1 with a reason. */
static int http_get_once(const char *url, int tls, int fd, char *location, size_t ll,
                         int allow_reuse, char *err, size_t errlen)
{
    char host[256], port[8], req[2300], head[8192], mem[2048];
    const char *p = url + (tls ? 8 : 7), *slash = strchr(p, '/'), *colon;
    const char *path = slash ? slash : "/";
    size_t hl = slash ? (size_t)(slash - p) : strlen(p), hlen = 0, rl;
    int code = 0, chunked = 0, close_wanted = 0, reused = 0, keep, old_http = 0;
    long long clen = -1, t0;
    struct netconn *c;
    struct sink_body body;
    char *hdrend;
    ssize_t n;

    colon = memchr(p, ':', hl);
    snprintf(port, sizeof port, "%s", tls ? "443" : "80");
    if (hl == 0 || hl >= sizeof host) { snprintf(err, errlen, "no host in %s", url); return -1; }
    if (colon) {
        snprintf(port, sizeof port, "%.*s", (int)(hl - (size_t)(colon - p) - 1), colon + 1);
        hl = (size_t)(colon - p);
    }
    snprintf(host, sizeof host, "%.*s", (int)hl, p);
    c = conn_take(host, port, tls, allow_reuse, &reused, err, errlen);
    if (c == NULL) return -1;
    t0 = pkg_fs_now_ms();
    rl = (size_t)snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " PKG_USER_AGENT
                          "\r\nConnection: keep-alive\r\n\r\n", path, host);
    if (net_write(c->sock, c->tls, req, rl) != (ssize_t)rl) {
        conn_drop(c);
        if (reused) return 4;
        snprintf(err, errlen, "cannot send to %s", host);
        return -1;
    }
    /* the head, up to the blank line */
    for (;;) {
        if (hlen + 1 >= sizeof head) { conn_drop(c); snprintf(err, errlen, "an oversized reply from %s", host); return -1; }
        n = conn_read(c, head + hlen, sizeof head - 1 - hlen);
        if (n <= 0) {
            conn_drop(c);
            if (reused && hlen == 0) return 4;   /* the server had closed it: send it again */
            snprintf(err, errlen, "%s closed the connection early", host);
            return -1;
        }
        hlen += (size_t)n;
        head[hlen] = '\0';
        if ((hdrend = strstr(head, "\r\n\r\n")) != NULL) { hdrend += 4; break; }
    }
    /* what came in after the head belongs to the body, and to the next
     * response after that: put it back in front of the connection's buffer */
    {
        size_t over = hlen - (size_t)(hdrend - head);
        if (over > 0) {
            char spill[NET_BUF];
            size_t rest = c->len - c->at;
            if (over + rest > sizeof spill) { conn_drop(c); snprintf(err, errlen, "an oversized reply from %s", host); return -1; }
            memcpy(spill, hdrend, over);
            memcpy(spill + over, c->buf + c->at, rest);
            memcpy(c->buf, spill, over + rest);
            c->at = 0;
            c->len = over + rest;
        }
    }
    if (sscanf(head, "HTTP/%*s %d", &code) != 1) { conn_drop(c); snprintf(err, errlen, "%s did not answer HTTP", host); return -1; }
    old_http = strncmp(head, "HTTP/1.1", 8) != 0;
    {
        char *line = strstr(head, "\r\n");
        while (line && line + 2 < hdrend - 2) {
            char *eol = strstr(line + 2, "\r\n");
            size_t k = eol ? (size_t)(eol - (line + 2)) : 0;
            if (k > 15 && strncasecmp(line + 2, "Content-Length:", 15) == 0) clen = atoll(line + 17);
            if (k > 18 && strncasecmp(line + 2, "Transfer-Encoding:", 18) == 0
                && strstr(line + 2, "chunked") && (size_t)(strstr(line + 2, "chunked") - (line + 2)) < k)
                chunked = 1;
            if (k > 11 && strncasecmp(line + 2, "Connection:", 11) == 0) {
                char v[64];
                size_t vl = k - 11 < sizeof v - 1 ? k - 11 : sizeof v - 1, z;
                memcpy(v, line + 13, vl);
                v[vl] = '\0';
                for (z = 0; v[z]; z++) v[z] = (char)tolower((unsigned char)v[z]);
                if (strstr(v, "close") != NULL) close_wanted = 1;
            }
            if (k > 9 && strncasecmp(line + 2, "Location:", 9) == 0 && location) {
                const char *v = line + 11;
                while (*v == ' ') v++;
                snprintf(location, ll, "%.*s", (int)(eol - v), v);
            }
            line = eol;
        }
    }
    if (chunked) clen = -1;              /* the chunks frame it, not a length */
    /* 204 and 304 carry no body whatever they say; nothing else Pkg asks for
     * is bodiless. */
    if (code == 204 || code == 304) clen = 0;
    memset(&body, 0, sizeof body);
    body.fd = -1;
    if (code == 200) {
        body.fd = fd;
    } else {
        body.mem = mem;                  /* the first words of a refusal */
        body.cap = sizeof mem - 1;
        mem[0] = '\0';
    }
    n = read_body(c, clen, chunked, &body, host, err, errlen);
    if (n < 0) { conn_drop(c); return -1; }
    keep = n == 0 && !close_wanted && !old_http && !keepalive_off();
    nettr("net: %s %s answered %d, %lld bytes in %lld ms, on a %s connection%s", host, path, code,
          body.total, pkg_fs_now_ms() - t0, reused ? "kept" : "new", keep ? "" : ", which ends here");
    if (!keep)
        conn_drop(c);
    if (code == 426) {
        /* the portal's own words: "reason: ..." and "next: ..." */
        const char *r = strstr(mem, "reason: "), *nx = strstr(mem, "next: ");
        int rlen = r ? (int)strcspn(r + 8, "\r\n") : 0, nl = nx ? (int)strcspn(nx + 6, "\r\n") : 0;
        snprintf(err, errlen, "%.*s%s%.*s", rlen ? rlen : 40, rlen ? r + 8 : "the server asks for a newer pkg",
                 nl ? ". " : "", nl, nl ? nx + 6 : "");
        return -1;
    }
    if (code == 404 || code == 410) return 1;
    if (code >= 300 && code < 400) return 3;
    if (code != 200) { snprintf(err, errlen, "%s answered HTTP %d for %s", host, code, path); return -1; }
    return 0;
}

int pkg_net_get(const char *url, const char *dest, char *err, size_t errlen)
{
    size_t dl = strlen(dest);
    char *tmp = (char *)malloc(dl + 8), cur[2100], loc[2100];
    int hops, tries, rc = -1, fd, tls;

    if (tmp == NULL) { snprintf(err, errlen, "out of memory"); return -1; }
    snprintf(tmp, dl + 8, "%s.part", dest);
    snprintf(cur, sizeof cur, "%s", url);
    for (hops = 0; hops < 6; hops++) {
        tls = strncmp(cur, "https://", 8) == 0;
#if !defined(__AROS__)
        if (tls) {                              /* the host systems hand https to curl */
            rc = get_https(cur, tmp, err, errlen);
            break;
        }
#endif
        if (!tls && strncmp(cur, "http://", 7) != 0) {
            snprintf(err, errlen, "cannot fetch %s: only http:// and https:// are read", cur);
            rc = -1;
            break;
        }
        /* Once on whatever connection is open, and, if that one turned out
         * to have been closed at the other end, once more on a new one: a
         * GET asks for a file and changes nothing, so sending it again is
         * the same request. */
        for (tries = 0; tries < 2; tries++) {
            fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno)); rc = -1; break; }
            loc[0] = '\0';
            rc = http_get_once(cur, tls, fd, loc, sizeof loc, tries == 0, err, errlen);
            close(fd);
            if (rc != 4) break;
            nettr("net: %s had closed the connection that was being kept; asking again on a new one", cur);
        }
        if (rc == 4) { snprintf(err, errlen, "cannot send to %s", cur); rc = -1; }
        if (rc != 3) break;
        if (loc[0] == '\0') { snprintf(err, errlen, "a redirect with no Location"); rc = -1; break; }
        if (loc[0] == '/') {                     /* same host */
            const char *h = strchr(cur + 8, '/');
            size_t prefix = h ? (size_t)(h - cur) : strlen(cur);
            snprintf(cur + prefix, sizeof cur - prefix, "%s", loc);
        } else {
            snprintf(cur, sizeof cur, "%s", loc);
        }
        rc = -1;
        snprintf(err, errlen, "too many redirects");
    }
    if (rc == 0 && rename(tmp, dest) != 0) { snprintf(err, errlen, "cannot keep %s: %s", dest, strerror(errno)); rc = -1; }
    if (rc != 0) unlink(tmp);
    free(tmp);
    return rc;
}

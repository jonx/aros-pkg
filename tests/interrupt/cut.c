/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Preloaded into pkg by tests/interrupt.sh: counts every rename and unlink
 * and ends the process at number PKGCUT_AT, before that call happens, as a
 * crash or a reset would. PKGCUT_MODE=aros cuts a rename the way AROS does
 * one over an existing file, DeleteFile then Rename: the target is deleted,
 * then the process ends. PKGCUT_LOG, if set, gets one line per call. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int calls;

static int cut_here(const char *op, const char *a, const char *b)
{
    const char *log = getenv("PKGCUT_LOG"), *at = getenv("PKGCUT_AT");
    calls++;
    if (log != NULL) {
        FILE *f = fopen(log, "a");
        if (f != NULL) {
            fprintf(f, "%d %s %s %s\n", calls, op, a, b ? b : "-");
            fclose(f);
        }
    }
    return at != NULL && calls == atoi(at);
}

#ifdef __APPLE__
static int cut_rename(const char *a, const char *b)
{
    if (cut_here("rename", a, b)) {
        const char *mode = getenv("PKGCUT_MODE");
        if (mode != NULL && strcmp(mode, "aros") == 0)
            unlink(b);              /* not interposed inside this library */
        _exit(137);
    }
    return rename(a, b);
}

static int cut_unlink(const char *a)
{
    if (cut_here("unlink", a, NULL))
        _exit(137);
    return unlink(a);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement, *original;
} interposers[] = {
    { (const void *)cut_rename, (const void *)rename },
    { (const void *)cut_unlink, (const void *)unlink },
};
#else
#include <dlfcn.h>
#include <sys/syscall.h>

int rename(const char *a, const char *b)
{
    static int (*real)(const char *, const char *);
    if (real == NULL)
        real = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "rename");
    if (cut_here("rename", a, b)) {
        const char *mode = getenv("PKGCUT_MODE");
        if (mode != NULL && strcmp(mode, "aros") == 0)
            syscall(SYS_unlinkat, -100, b, 0);
        _exit(137);
    }
    return real(a, b);
}

int unlink(const char *a)
{
    static int (*real)(const char *);
    if (real == NULL)
        real = (int (*)(const char *))dlsym(RTLD_NEXT, "unlink");
    if (cut_here("unlink", a, NULL))
        _exit(137);
    return real(a);
}
#endif

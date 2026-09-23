/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The host filesystem on Windows. Paths inside Pkg are UTF-8 with '/' as the
 * separator, which Windows accepts; every call converts to UTF-16 and uses the
 * wide API, so names outside the ANSI code page survive. Replacement is
 * MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, the
 * Windows form of write-then-rename. A signing key is created with a DACL that
 * grants its owner alone, from the first instant, as 0600 does on POSIX.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include "pkg_fs.h"
#include "pkg.h"

#include <windows.h>
#include <io.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_errno(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:     errno = ENOENT; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:  errno = EACCES; break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:        errno = EEXIST; break;
    case ERROR_DIR_NOT_EMPTY:      errno = ENOTEMPTY; break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:        errno = ENOMEM; break;
    default:                       errno = EIO; break;
    }
}

/* UTF-8 to UTF-16, caller frees. */
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) { errno = EINVAL; return NULL; }
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w == NULL) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/* UTF-16 to UTF-8, caller frees. */
static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) { errno = EINVAL; return NULL; }
    s = (char *)malloc((size_t)n);
    if (s == NULL) { errno = ENOMEM; return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

int pkg_host_args(int *argc, char ***argv)
{
    int n, i;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **v;
    if (w == NULL)
        return -1;
    v = (char **)calloc((size_t)n + 1u, sizeof *v);
    if (v == NULL) { LocalFree(w); return -1; }
    for (i = 0; i < n; i++) {
        v[i] = narrow(w[i]);
        if (v[i] == NULL) { LocalFree(w); return -1; }
    }
    LocalFree(w);
    *argc = n;
    *argv = v;              /* lives as long as the process */
    return 0;
}

char *pkg_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int slash = la > 0 && a[la - 1] != '/' && a[la - 1] != '\\' && a[la - 1] != ':';
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
    wchar_t *w = wide(path);
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *p;
    size_t done = 0;

    if (w == NULL)
        return -1;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) { set_errno(); return -1; }
    if (!GetFileSizeEx(h, &size) || (unsigned long long)size.QuadPart > (size_t)-1 / 2) {
        set_errno(); CloseHandle(h); return -1;
    }
    p = (unsigned char *)malloc((size_t)size.QuadPart + 1u);
    if (p == NULL) { CloseHandle(h); errno = ENOMEM; return -1; }
    while (done < (size_t)size.QuadPart) {
        DWORD want = (DWORD)(((size_t)size.QuadPart - done) > 0x10000000u
                             ? 0x10000000u : (size_t)size.QuadPart - done);
        DWORD got = 0;
        if (!ReadFile(h, p + done, want, &got, NULL) || got == 0) {
            set_errno(); free(p); CloseHandle(h); return -1;
        }
        done += got;
    }
    CloseHandle(h);
    *buf = p;
    *len = done;
    return 0;
}

int pkg_fs_exists(const char *path)
{
    wchar_t *w = wide(path);
    DWORD a;
    if (w == NULL)
        return 0;
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

int pkg_fs_path_is_link(const char *path)
{
    wchar_t *w = wide(path);
    DWORD attrs, error;
    if (!w) return -1;
    attrs = GetFileAttributesW(w);
    error = GetLastError();
    free(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 0;
        SetLastError(error); set_errno(); return -1;
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}

int pkg_fs_is_dir(const char *path)
{
    wchar_t *w = wide(path);
    DWORD a;
    if (w == NULL)
        return 0;
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int mkdir_one(const char *p)
{
    wchar_t *w;
    int ok;
    if (pkg_fs_is_dir(p))
        return 0;
    w = wide(p);
    if (w == NULL)
        return -1;
    ok = CreateDirectoryW(w, NULL);
    if (!ok) set_errno();
    free(w);
    return ok || pkg_fs_is_dir(p) ? 0 : -1;
}

int pkg_fs_mkdirs(const char *dir)
{
    char *p = _strdup(dir), *s;
    if (p == NULL)
        return -1;
    s = p;
    /* Past a drive ("C:") or a UNC prefix ("//server/share"), which are not
     * directories to create. */
    if (s[0] && s[1] == ':') s += 2;
    if ((s[0] == '/' || s[0] == '\\') && (s[1] == '/' || s[1] == '\\')) {
        int part = 0;
        s += 2;
        while (*s && part < 2) { if (*s == '/' || *s == '\\') part++; s++; }
    }
    for (s = s + 1; *s; s++) {
        if (*s == '/' || *s == '\\') {
            char c = *s;
            *s = '\0';
            if (s[-1] != ':' && mkdir_one(p) != 0) { free(p); return -1; }
            *s = c;
        }
    }
    if (mkdir_one(p) != 0) { free(p); return -1; }
    free(p);
    return 0;
}

static int mkparents(const char *path)
{
    char *p = _strdup(path), *a, *b, *cut;
    int rc = 0;
    if (p == NULL)
        return -1;
    a = strrchr(p, '/');
    b = strrchr(p, '\\');
    cut = a > b ? a : b;
    if (cut != NULL && cut != p && cut[-1] != ':') {
        *cut = '\0';
        rc = pkg_fs_mkdirs(p);
    }
    free(p);
    return rc;
}

static int write_atomic_sd(const char *path, const void *buf, size_t len, int private_key)
{
    size_t lp = strlen(path);
    char *tmp = (char *)malloc(lp + 24u);
    wchar_t *wt = NULL, *wp = NULL;
    HANDLE h;
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    const unsigned char *b = (const unsigned char *)buf;
    int rc = -1;

    if (tmp == NULL)
        return -1;
    if (mkparents(path) != 0) { free(tmp); return -1; }
    snprintf(tmp, lp + 24u, "%s.tmp%lu", path, (unsigned long)GetCurrentProcessId());
    wt = wide(tmp);
    wp = wide(path);
    if (wt == NULL || wp == NULL)
        goto out;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    if (private_key) {
        /* Protected DACL, full access for the owner, nothing inherited. */
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)",
                                                                   SDDL_REVISION_1, &sd, NULL)) {
            set_errno();
            goto out;
        }
        sa.lpSecurityDescriptor = sd;
    }
    h = CreateFileW(wt, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { set_errno(); goto out; }
    while (len > 0) {
        DWORD want = (DWORD)(len > 0x10000000u ? 0x10000000u : len), put = 0;
        if (!WriteFile(h, b, want, &put, NULL)) {
            set_errno(); CloseHandle(h); DeleteFileW(wt); goto out;
        }
        b += put;
        len -= put;
    }
    if (!FlushFileBuffers(h)) { set_errno(); CloseHandle(h); DeleteFileW(wt); goto out; }
    CloseHandle(h);
    if (!MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_errno();
        DeleteFileW(wt);
        goto out;
    }
    rc = 0;
out:
    if (sd != NULL) LocalFree(sd);
    free(wt);
    free(wp);
    free(tmp);
    return rc;
}

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

int pkg_fs_canonical_dir(const char *path, char *out, size_t len)
{
    wchar_t *input, *resolved;
    const wchar_t *start;
    char *utf8;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD needed, got;
    size_t n, i;
    int unc;
    if (!path || !out || !len) { errno = EINVAL; return -1; }
    out[0] = 0;
    input = wide(path);
    if (!input) return -1;
    handle = CreateFileW(input, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(input);
    if (handle == INVALID_HANDLE_VALUE) { set_errno(); return -1; }
    if (!GetFileInformationByHandle(handle, &info)) {
        set_errno(); CloseHandle(handle); return -1;
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle); errno = ENOTDIR; return -1;
    }
    needed = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed) { set_errno(); CloseHandle(handle); return -1; }
    resolved = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof *resolved);
    if (!resolved) { CloseHandle(handle); errno = ENOMEM; return -1; }
    got = GetFinalPathNameByHandleW(handle, resolved, needed + 1u,
                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!got || got > needed) {
        if (!got) set_errno(); else errno = ENAMETOOLONG;
        free(resolved); CloseHandle(handle); return -1;
    }
    CloseHandle(handle);
    /* Convert extended DOS drive and UNC prefixes into portable pkg paths. */
    unc = wcsncmp(resolved, L"\\\\?\\UNC\\", 8) == 0;
    start = resolved;
    if (unc) start += 8;
    else if (wcsncmp(resolved, L"\\\\?\\", 4) == 0 && got >= 7 && resolved[5] == L':') start += 4;
    else { free(resolved); errno = EINVAL; return -1; }
    utf8 = narrow(start);
    free(resolved);
    if (!utf8) return -1;
    n = strlen(utf8);
    if (n + (unc ? 2u : 0u) >= len) { free(utf8); errno = ENAMETOOLONG; return -1; }
    if (unc) { out[0] = '/'; out[1] = '/'; }
    memcpy(out + (unc ? 2u : 0u), utf8, n + 1u);
    free(utf8);
    for (i = 0; out[i]; i++) if (out[i] == '\\') out[i] = '/';
    return 0;
}

int pkg_fs_interactive(void)
{
    return _isatty(_fileno(stdout));
}

int pkg_fs_write_new(const char *path, const void *buf, size_t len)
{
    /* The wide call, like every other here: fopen would read the UTF-8 name
     * in the machine's code page, and "français" would name a drawer that
     * does not exist. */
    wchar_t *w;
    FILE *f;
    if (mkparents(path) != 0) return -1;
    w = wide(path);
    if (w == NULL) return -1;
    f = _wfopen(w, L"wb");
    free(w);
    if (f == NULL) return -1;
    if ((len > 0 && fwrite(buf, 1, len, f) != len) || fclose(f) != 0) {
        pkg_fs_unlink(path);
        return -1;
    }
    return 0;
}

int pkg_fs_write_atomic(const char *path, const void *buf, size_t len)
{
    return write_atomic_sd(path, buf, len, 0);
}

int pkg_fs_write_private(const char *path, const void *buf, size_t len)
{
    return write_atomic_sd(path, buf, len, 1);
}

int pkg_fs_rename(const char *from, const char *to)
{
    wchar_t *wf, *wt;
    int ok;
    if (mkparents(to) != 0)
        return -1;
    wf = wide(from);
    wt = wide(to);
    ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) set_errno();
    free(wf);
    free(wt);
    return ok ? 0 : -1;
}

int pkg_fs_unlink(const char *path)
{
    wchar_t *w = wide(path);
    int ok;
    if (w == NULL)
        return -1;
    ok = DeleteFileW(w);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        /* A read-only attribute blocks deletion on Windows but not the removal
         * of a directory entry on POSIX; Pkg removes files it placed. */
        SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
        ok = DeleteFileW(w);
    }
    if (!ok) set_errno();
    free(w);
    return ok ? 0 : -1;
}

static int rmdir_one(const char *path)
{
    wchar_t *w = wide(path);
    int ok;
    if (w == NULL)
        return -1;
    ok = RemoveDirectoryW(w);
    if (!ok) set_errno();
    free(w);
    return ok ? 0 : -1;
}

/* Directory entries as UTF-8, "." and ".." excluded, with their attributes. */
struct dent { char *name; DWORD attr; };

static int read_dir(const char *dir, struct dent **out, size_t *count)
{
    char *pattern = pkg_join(dir, "*");
    wchar_t *w = pattern ? wide(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    struct dent *v = NULL;
    size_t n = 0, cap = 0;

    free(pattern);
    *out = NULL;
    *count = 0;
    if (w == NULL)
        return -1;
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        set_errno();
        return -1;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2u : 16u;
            struct dent *g = (struct dent *)realloc(v, ncap * sizeof *g);
            if (g == NULL) break;
            v = g;
            cap = ncap;
        }
        v[n].name = narrow(fd.cFileName);
        v[n].attr = fd.dwFileAttributes;
        if (v[n].name == NULL) break;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *out = v;
    *count = n;
    return 0;
}

static void free_dir(struct dent *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(v[i].name);
    free(v);
}

int pkg_fs_rmtree(const char *path)
{
    struct dent *v;
    size_t n, i;
    int rc = 0;

    if (!pkg_fs_exists(path))
        return 0;
    if (!pkg_fs_is_dir(path))
        return pkg_fs_unlink(path);
    if (read_dir(path, &v, &n) != 0)
        return -1;
    for (i = 0; i < n && rc == 0; i++) {
        char *c = pkg_join(path, v[i].name);
        rc = c ? pkg_fs_rmtree(c) : -1;
        free(c);
    }
    free_dir(v, n);
    return rc == 0 ? rmdir_one(path) : -1;
}

void pkg_fs_prune_empty_parents(const char *root, const char *rel)
{
    char *r = _strdup(rel), *slash;
    if (r == NULL)
        return;
    while ((slash = strrchr(r, '/')) != NULL) {
        char *full;
        *slash = '\0';
        full = pkg_join(root, r);
        if (full == NULL || rmdir_one(full) != 0) { free(full); break; }
        free(full);
    }
    free(r);
}

/* What pkg_fs.h says a walk leaves out: the same rule on every host, since
 * drawers travel between them. */
static int skip_host_metadata(const char *name)
{
    return name[0] == '.' || strcmp(name, "Icon\r") == 0
        || _stricmp(name, "Thumbs.db") == 0 || _stricmp(name, "desktop.ini") == 0;
}
static int walk(const char *root, const char *rel, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip,
                void *ctx, unsigned *skipped, char *err, size_t errlen)
{
    char *dir = rel[0] ? pkg_join(root, rel) : _strdup(root);
    struct dent *v;
    size_t n, i;
    int rc = 0;

    if (dir == NULL)
        return -1;
    if (read_dir(dir, &v, &n) != 0) {
        snprintf(err, errlen, "cannot open \"%s\": %s", dir, strerror(errno));
        free(dir);
        return -1;
    }
    for (i = 0; i < n && rc == 0; i++) {
        char *child_rel = rel[0] ? pkg_join(rel, v[i].name) : _strdup(v[i].name);
        if (child_rel == NULL) {
            rc = -1;
        } else if (skip_host_metadata(v[i].name)) {
            if (skipped) (*skipped)++;
            if (skip) skip(child_rel, (v[i].attr & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
        } else if (v[i].attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            snprintf(err, errlen, "\"%s\" is a link or junction; a package holds regular files only",
                     child_rel);
            rc = -1;
        } else if (v[i].attr & FILE_ATTRIBUTE_DIRECTORY) {
            rc = walk(root, child_rel, fn, skip, ctx, skipped, err, errlen);
        } else {
            rc = fn(child_rel, ctx);
        }
        free(child_rel);
    }
    free_dir(v, n);
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
    struct dent *v;
    size_t n, i, k = 0;
    char **out;

    *names = NULL;
    *count = 0;
    if (!pkg_fs_is_dir(dir))
        return 0;
    if (read_dir(dir, &v, &n) != 0)
        return -1;
    out = (char **)calloc(n ? n : 1, sizeof *out);
    if (out == NULL) { free_dir(v, n); return -1; }
    for (i = 0; i < n; i++) {
        if (v[i].name[0] == '.') { free(v[i].name); continue; }
        out[k++] = v[i].name;
    }
    free(v);
    if (k > 1u)
        qsort(out, k, sizeof *out, cmp_str);
    *names = out;
    *count = k;
    return 0;
}

void (*pkg_fs_on_transfer)(long long done, long long total);
void (*pkg_fs_on_wait)(const char *host);
void (*pkg_fs_on_tick)(void);
void (*pkg_fs_on_trace)(const char *line);

/* Milliseconds since the machine started: only differences are used. */
long long pkg_fs_now_ms(void)
{
    return (long long)GetTickCount64();
}

/* Windows hands every request to curl.exe, which holds nothing open of its
 * own between two of them: there is nothing here to close. */
void pkg_net_idle_close(void)
{
}

int pkg_fs_random_typed(void *buf, size_t len)
{
    (void)buf; (void)len;
    return -2;   /* Windows has a random source */
}

int pkg_fs_random(void *buf, size_t len)
{
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

/* ---- Amiga attributes -------------------------------------------------- */

/* Windows has no mode: .ameta decides every bit (ameta.md, Publishing). */
int pkg_fs_owner_exec(const char *path)
{
    (void)path;
    return -1;
}

int pkg_fs_set_owner_exec(const char *path, int exec)
{
    (void)path;
    (void)exec;
    return 0;
}

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

void pkg_fs_unprotect(const char *path)
{
    (void)path;
}

int pkg_fs_identity(const char *path, struct pkg_fs_id *id)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = wide(path);
    BOOL ok;
    memset(id, 0, sizeof *id);
    if (w == NULL)
        return -1;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    id->exists = 1;
    id->size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    id->mtime_s = (long long)(((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32)
                              | a.ftLastWriteTime.dwLowDateTime);
    return 0;
}

int pkg_fs_replace_if_same(const char *path, const struct pkg_fs_id *before,
                           const void *buf, size_t len)
{
    struct pkg_fs_id now;
    char *tmp = NULL;
    int rc;
    if (buf != NULL) {
        size_t pl = strlen(path);
        unsigned char r[4];
        tmp = (char *)malloc(pl + 16);
        if (tmp == NULL || pkg_fs_random(r, sizeof r) != 0) { free(tmp); return -1; }
        snprintf(tmp, pl + 16, "%s.%02x%02x%02x%02x", path, r[0], r[1], r[2], r[3]);
        if (pkg_fs_write_atomic(tmp, buf, len) != 0) { free(tmp); return -1; }
    }
    if (pkg_fs_identity(path, &now) != 0 || now.exists != before->exists
        || (now.exists && (now.size != before->size || now.mtime_s != before->mtime_s))) {
        if (tmp) { pkg_fs_unlink(tmp); free(tmp); }
        return 1;
    }
    if (tmp != NULL) {
        rc = pkg_fs_rename(tmp, path);
        if (rc != 0) pkg_fs_unlink(tmp);
        free(tmp);
        return rc;
    }
    return (pkg_fs_unlink(path) == 0 || !pkg_fs_exists(path)) ? 0 : -1;
}

void *pkg_fs_lock_dir(const char *dir)
{
    (void)dir;
    return NULL;
}

void pkg_fs_unlock_dir(void *lock)
{
    (void)lock;
}

/* <root>/.pkg/lock opened with no sharing: a second opener is refused. */
void *pkg_fs_lock_root(const char *root, int *busy)
{
    char *dir = pkg_join(root, ".pkg"), *path = dir ? pkg_join(dir, "lock") : NULL;
    wchar_t *w;
    HANDLE h;
    *busy = 0;
    if (path == NULL || pkg_fs_mkdirs(dir) != 0 || (w = wide(path)) == NULL) {
        free(dir); free(path);
        return NULL;
    }
    h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w); free(dir); free(path);
    if (h == INVALID_HANDLE_VALUE) {
        *busy = GetLastError() == ERROR_SHARING_VIOLATION;
        return NULL;
    }
    return (void *)h;
}

void pkg_fs_unlock_root(void *lock)
{
    if (lock != NULL)
        CloseHandle((HANDLE)lock);
}

/* ---- the network ------------------------------------------------------ */

#include <process.h>

/* An environment variable as UTF-8, caller frees: getenv would give the
 * machine's code page, which is not what the rest of Pkg takes paths in. */
static char *env_utf8(const char *name)
{
    wchar_t *wn = wide(name);
    const wchar_t *v = wn ? _wgetenv(wn) : NULL;
    free(wn);
    return v != NULL && v[0] != L'\0' ? narrow(v) : NULL;
}

/* curl.exe with these arguments, waited for. The arguments go as UTF-16, so
 * a path under C:\Users\Michał arrives whole, and one holding a space is
 * quoted, since Windows hands a program one command line and not a list. */
static intptr_t run_curl(const char *const *argv)
{
    const wchar_t *wargv[32];
    wchar_t *own[32];
    int n, k;
    intptr_t rc = -1;
    for (n = 0; argv[n] != NULL && n < 31; n++) {
        const char *a = argv[n];
        int quote = (strchr(a, ' ') != NULL || strchr(a, '\t') != NULL) && a[0] != '"';
        if (quote) {
            size_t l = strlen(a);
            char *q = (char *)malloc(l + 3);
            if (q == NULL) { own[n] = NULL; break; }
            q[0] = '"'; memcpy(q + 1, a, l); q[l + 1] = '"'; q[l + 2] = '\0';
            own[n] = wide(q);
            free(q);
        } else {
            own[n] = wide(a);
        }
        if (own[n] == NULL) break;
        wargv[n] = own[n];
    }
    if (argv[n] == NULL) {
        wargv[n] = NULL;
        rc = _wspawnvp(_P_WAIT, L"curl.exe", wargv);
    } else {
        errno = EINVAL;
    }
    for (k = 0; k < n; k++) free(own[k]);
    return rc;
}

/* curl.exe ships with Windows 10 and later: http and https both go through
 * it, with no shell in between. */
int pkg_net_get(const char *url, const char *dest, char *err, size_t errlen)
{
    size_t dl = strlen(dest);
    char *tmp = (char *)malloc(dl + 8);
    intptr_t rc;
    if (tmp == NULL) { snprintf(err, errlen, "out of memory"); return -1; }
    snprintf(tmp, dl + 8, "%s.part", dest);
    if (mkparents(tmp) != 0) { free(tmp); snprintf(err, errlen, "cannot create the cache directory"); return -1; }
    /* curl.exe is waited for, so nothing here can watch the file grow: what
     * a person is told is the machine being waited for, once. */
    if (pkg_fs_on_wait != NULL) {
        const char *p = strstr(url, "://");
        char host[300];
        size_t n = 0;
        if (p != NULL) {
            p += 3;
            while (p[n] && p[n] != '/' && p[n] != ':' && n + 1 < sizeof host) n++;
            memcpy(host, p, n);
        }
        host[n] = '\0';
        if (host[0]) pkg_fs_on_wait(host);
    }
    {
        const char *argv[] = { "curl.exe", "-s", "-f", "-L", "--max-redirs", "5",
                               "-A", PKG_USER_AGENT, "-o", tmp, url, NULL };
        rc = run_curl(argv);
    }
    if (pkg_fs_on_wait != NULL) pkg_fs_on_wait(NULL);
    if (rc == -1) { free(tmp); snprintf(err, errlen, "fetching a channel needs curl.exe, part of Windows 10 and later"); return -1; }
    if (rc == 22) { pkg_fs_unlink(tmp); free(tmp); return 1; }
    if (rc != 0) { pkg_fs_unlink(tmp); free(tmp); snprintf(err, errlen, "curl failed with exit code %d fetching %s", (int)rc, url); return -1; }
    if (pkg_fs_rename(tmp, dest) != 0) { free(tmp); snprintf(err, errlen, "cannot keep %s", dest); return -1; }
    free(tmp);
    return 0;
}

char *pkg_cache_dir(void)
{
    char *e = env_utf8("PKG_CACHE"), *l = env_utf8("LOCALAPPDATA");
    size_t n = 32 + (e ? strlen(e) : 0) + (l ? strlen(l) : 0);
    char *p = (char *)malloc(n);
    if (p != NULL) {
        if (e) snprintf(p, n, "%s", e);
        else if (l) snprintf(p, n, "%s\\pkg-cache", l);
        else snprintf(p, n, "pkg-cache");
    }
    free(e);
    free(l);
    return p;
}

int pkg_net_send(const char *method, const char *url, const char *body_file,
                 const char *header_file, const char *out_file, int *code,
                 char *err, size_t errlen)
{
    char data[1100], hdr[1100], codefile[1100];
    const char *argv[24];
    int n = 0;
    intptr_t rc;
    FILE *f;
    snprintf(codefile, sizeof codefile, "%s.code", out_file);
    argv[n++] = "curl.exe"; argv[n++] = "-sS"; argv[n++] = "-A"; argv[n++] = PKG_USER_AGENT;
    argv[n++] = "-X"; argv[n++] = method;
    if (body_file) {
        snprintf(data, sizeof data, "%s", body_file);
        argv[n++] = "-T"; argv[n++] = data;
        argv[n++] = "-H"; argv[n++] = "Content-Type: application/octet-stream";
        argv[n++] = "-H"; argv[n++] = "Expect:";
    }
    if (header_file) { snprintf(hdr, sizeof hdr, "@%s", header_file); argv[n++] = "-H"; argv[n++] = hdr; }
    argv[n++] = "-o"; argv[n++] = out_file;
    argv[n++] = "-w"; argv[n++] = "%{http_code}";
    argv[n++] = url;
    argv[n] = NULL;
    {
        /* the status code goes to stdout: send it to a file */
        wchar_t *wc = wide(codefile);
        FILE *saved = wc ? _wfreopen(wc, L"w", stdout) : NULL;
        free(wc);
        rc = run_curl(argv);
        (void)saved;
        fflush(stdout);
    }
    if (rc == -1) { snprintf(err, errlen, "PUSH needs curl.exe, part of Windows 10 and later"); return -1; }
    {
        wchar_t *wc = wide(codefile);
        f = wc ? _wfopen(wc, L"r") : NULL;
        free(wc);
    }
    *code = 0;
    if (f) { if (fscanf(f, "%d", code) != 1) *code = 0; fclose(f); }
    pkg_fs_unlink(codefile);
    if (*code == 0) { snprintf(err, errlen, "no answer from %s", url); return -1; }
    return 0;
}

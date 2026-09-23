/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * RESOLVE: which copy of a library a program gets, and why.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- RESOLVE ----------------------------------------------------------- *
 *
 * What OpenLibrary(<name>, <version>) gives a program, and why, for a
 * developer whose program does not start or opens the wrong library. The
 * places LDDemon looks, in its order (rom/lddemon/lddemon.c, LDLoad): the
 * caller's current directory and its libs/ (devs/ for a device), the
 * program's directory and its libs/, then LIBS: (DEVS:). A library already
 * in memory wins over every file. Each file gets the checks the loader
 * makes: LoadSeg reads it only if it is an executable for this CPU; it is
 * a library only if it holds a resident tag of the right type; the first
 * file that passes is taken whatever its version, and the open fails if
 * that copy is older than asked. On a host, FROM stands for the program's
 * directory and the current one, ROOT for SYS:, and LIBS: is ROOT/Libs then
 * ROOT/Classes, or the directories PKG_LIBS_PATH lists. */

enum { C_NOFILE, C_DIR, C_UNREADABLE, C_NOTEXEC, C_WRONGCPU, C_NORESIDENT, C_WRONGTYPE, C_OK };

struct resolution {
    struct cand c[16];
    size_t      n;
    size_t      taken;          /* the candidate the loader takes, or (size_t)-1 */
    int         loaded;         /* 1 in memory, 0 not, -1 cannot tell */
    char        loaded_ver[32];
    unsigned    opencnt;
    int         rc;             /* 0, 11 nothing, 18 too old or the taken file fails */
    int         shadows;        /* the copy taken hides a newer one found later */
    char        summary[300];
    char        offer[160];     /* "sdl2 2.30" when a channel has a package providing it */
    char        offer_chan[600];/* the channel that offers it */
};

/* "SDL2.library 2.30 (1.1.2026)" -> "2.30": the version of a $VER cookie
 * whatever its name looks like. */
static void file_version(const unsigned char *p, size_t len, char *ver, size_t vl)
{
    size_t j;
    ver[0] = '\0';
    for (j = 0; j + 6 < len; j++) {
        size_t k = j + 6, b = 0;
        if (memcmp(p + j, "$VER: ", 6) != 0) continue;
        while (k < len && p[k] > ' ' && p[k] < 0x7F) k++;          /* the name */
        while (k < len && p[k] == ' ') k++;
        while (k < len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.') && b + 1 < vl)
            ver[b++] = (char)p[k++];
        while (b > 0 && ver[b - 1] == '.') b--;
        ver[b] = '\0';
        if (b > 0 && pkg_check_version(ver) == NULL) return;
        ver[0] = '\0';
    }
}

/* A resident tag in the file's bytes, laid out as the file's CPU lays out
 * struct Resident: 0x4AFC, then after the two pointers rt_Flags,
 * rt_Version, rt_Type. The type of the first plausible one, and its
 * version; -1 when there is none. */
static int find_resident(const unsigned char *p, size_t len, int *version)
{
    int be, align, flags_at;
    size_t o;
    if (len >= 20 && memcmp(p, "\x7f" "ELF", 4) == 0) {
        unsigned mach = p[5] == 2 ? (unsigned)p[18] << 8 | p[19] : (unsigned)p[19] << 8 | p[18];
        be = p[5] == 2;
        if (mach == 4) { align = 2; flags_at = 10; }           /* m68k: packed on 2 */
        else if (p[4] == 2) { align = 8; flags_at = 24; }      /* 64-bit pointers */
        else { align = 4; flags_at = 12; }
    } else if (len >= 4 && pkg_be32_get(p) == 0x000003F3ul) {
        be = 1; align = 2; flags_at = 10;                      /* a hunk file: m68k */
    } else {
        return -1;
    }
    for (o = 0; o + (size_t)flags_at + 4 <= len; o += (size_t)align) {
        int flags, type;
        if (!(be ? (p[o] == 0x4A && p[o + 1] == 0xFC) : (p[o] == 0xFC && p[o + 1] == 0x4A)))
            continue;
        flags = p[o + flags_at];
        type = p[o + flags_at + 2];
        if ((type == 3 || type == 8 || type == 9) && (flags & 0x30) == 0) {
            *version = p[o + flags_at + 1];
            return type;
        }
    }
    return -1;
}

static void cand_add(struct resolution *r, const char *where, const char *path)
{
    size_t k;
    if (r->n >= sizeof r->c / sizeof r->c[0]) return;
    for (k = 0; k < r->n; k++)          /* FROM is both the current and the program directory */
        if (strcmp(r->c[k].path, path) == 0) return;
    memset(&r->c[r->n], 0, sizeof r->c[r->n]);
    r->c[r->n].rtype = -1;
    snprintf(r->c[r->n].where, sizeof r->c[r->n].where, "%s", where);
    snprintf(r->c[r->n].path, sizeof r->c[r->n].path, "%s", path);
    r->n++;
}

/* A package of the channels whose newest version provides `name`:
 * "sdl2 2.30", and the channel it is in. */
static int channel_offers(const struct pkg_options *a, const char *name, char *out, size_t ol,
                          char *where, size_t wl)
{
    struct index ix;
    size_t i, j;
    int found = 0, rc;
    where[0] = '\0';
    if (a->channel == NULL && a->root == NULL) return 0;
    /* RESOLVE offers this as an extra: a root with no channel to ask is not
     * a refusal here, and nothing about it is printed. */
    quiet = 1;
    rc = open_channels(a, &ix);
    quiet = 0;
    quiet_reason[0] = '\0';
    if (rc != 0) { refused_class = 0; refused_next = NULL; return 0; }
    for (i = 0; i < ix.n && !found; i++) {
        const struct entry *best = &ix.e[i];
        struct pkg_manifest m;
        char *mp, err[200];
        unsigned char *buf;
        size_t len;
        for (j = 0; j < ix.n; j++)
            if (strcmp(ix.e[j].name, best->name) == 0 && pkg_version_cmp(ix.e[j].version, best->version) > 0)
                best = &ix.e[j];
        if (best != &ix.e[i]) continue;
        if ((mp = object_path(chan_of(best), best->digest, "manifest")) == NULL) continue;
        if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
        free(mp);
        if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) == 0) {
            if (strs_has_nocase(&m.provides, name)) {
                snprintf(out, ol, "%s %s", m.name, m.version);
                snprintf(where, wl, "%s", chan_of(best));
                found = 1;
            }
            pkg_manifest_free(&m);
        }
        free(buf);
    }
    free(ix.e);
    return found;
}

static void resolve_one(const struct pkg_options *a, const char *name, const char *want,
                        const char *root, const char *from, const struct installed *in,
                        struct resolution *r)
{
    const char *base = strrchr(name, ':'), *mach = target_arch;
    char buf[1200];
    size_t i, rl, better = (size_t)-1;
    int device;
    unsigned lv = 0, lr = 0;

    memset(r, 0, sizeof *r);
    r->taken = (size_t)-1;
    base = base ? base + 1 : name;
    if (strrchr(base, '/')) base = strrchr(base, '/') + 1;
    rl = strlen(base);
    device = rl > 7 && ascii_casecmp(base + rl - 7, ".device") == 0;
    if (strchr(name, ':') != NULL) {
#if defined(__AROS__)
        cand_add(r, "the path given", name);
#else
        if (ascii_casecmp_n(name, "SYS:", 4) == 0 && root != NULL)
            snprintf(buf, sizeof buf, "%s/%s", root, name + 4);
        else if (ascii_casecmp_n(name, "PROGDIR:", 8) == 0 && from != NULL)
            snprintf(buf, sizeof buf, "%s/%s", from, name + 8);
        else
            snprintf(buf, sizeof buf, "%s", name);
        cand_add(r, "the path given", buf);
#endif
    } else {
        const char *sub = device ? "devs" : "libs";
        if (from != NULL && from[0]) {
            const char *sep = from[strlen(from) - 1] == ':' ? "" : "/";
            snprintf(buf, sizeof buf, "%s%s%s", from, sep, name);
            cand_add(r, "PROGDIR:", buf);
            snprintf(buf, sizeof buf, "%s%s%s/%s", from, sep, sub, name);
            cand_add(r, device ? "PROGDIR:devs/" : "PROGDIR:libs/", buf);
        }
#if defined(__AROS__)
        if (from == NULL) {
            cand_add(r, "current directory", name);
            snprintf(buf, sizeof buf, "%s/%s", sub, name);
            cand_add(r, device ? "current dir devs/" : "current dir libs/", buf);
        }
        snprintf(buf, sizeof buf, "%s%s", device ? "DEVS:" : "LIBS:", name);
        cand_add(r, device ? "DEVS:" : "LIBS:", buf);
#else
        {
            const char *list = getenv("PKG_LIBS_PATH");
            if (list != NULL && !device) {
                const char *q = list;
                while (*q) {
                    size_t k = strcspn(q, ":");
                    if (k > 0) {
                        snprintf(buf, sizeof buf, "%.*s/%s", (int)k, q, name);
                        cand_add(r, "LIBS:", buf);
                    }
                    q += k;
                    if (*q == ':') q++;
                }
            } else if (root != NULL && device) {
                snprintf(buf, sizeof buf, "%s/Devs/%s", root, name);
                cand_add(r, "DEVS:", buf);
            } else if (root != NULL) {
                snprintf(buf, sizeof buf, "%s/Libs/%s", root, name);
                cand_add(r, "LIBS: (SYS:Libs)", buf);
                snprintf(buf, sizeof buf, "%s/Classes/%s", root, name);
                cand_add(r, "LIBS: (SYS:Classes)", buf);
            }
        }
#endif
    }

    /* each file, as the loader sees it */
    for (i = 0; i < r->n; i++) {
        struct cand *c = &r->c[i];
        unsigned char *data;
        size_t len, k;
        int rv = -1;
        if (!pkg_fs_exists(c->path)) { c->state = C_NOFILE; continue; }
        if (pkg_fs_is_dir(c->path)) { c->state = C_DIR; continue; }
        if (pkg_fs_read(c->path, &data, &len) != 0) { c->state = C_UNREADABLE; continue; }
        file_version(data, len, c->ver, sizeof c->ver);
        c->cpu = file_arch(data, len);
        c->rtype = find_resident(data, len, &rv);
        free(data);
        if (!c->ver[0] && rv >= 0) snprintf(c->ver, sizeof c->ver, "%d", rv);
        if (c->cpu == NULL) c->state = C_NOTEXEC;
        else if (mach != NULL && strcmp(c->cpu, mach) != 0) c->state = C_WRONGCPU;
        else if (c->rtype < 0) c->state = C_NORESIDENT;
        else if (c->rtype != (device ? 3 : 9)) c->state = C_WRONGTYPE;
        else c->state = C_OK;
#if defined(__AROS__)
        /* which directory of the LIBS: (DEVS:) assign holds it */
        if (ascii_casecmp_n(c->path, "LIBS:", 5) == 0 || ascii_casecmp_n(c->path, "DEVS:", 5) == 0)
            pkg_fs_fullpath(c->path, c->path, sizeof c->path);
#endif
        if (root != NULL && in != NULL) {
            /* the installed package that lists it, when it lies in the root */
            size_t rlen = strlen(root);
            const char *rel = NULL;
            if (ascii_casecmp(root, "SYS:") == 0 && strchr(c->path, ':'))
                rel = strchr(c->path, ':') + 1;
            else if (strncmp(c->path, root, rlen) == 0 && (c->path[rlen] == '/' || root[rlen - 1] == ':'))
                rel = c->path + rlen + (c->path[rlen] == '/');
            for (k = 0; rel != NULL && k < in->n && !c->pkg[0]; k++) {
                size_t f;
                for (f = 0; f < in->m[k].nfiles; f++)
                    if (ascii_casecmp(in->m[k].files[f].path, rel) == 0) {
                        snprintf(c->pkg, sizeof c->pkg, "%s %s", in->m[k].name, in->m[k].version);
                        break;
                    }
            }
            if (!c->pkg[0] && placements) {
                char *candidate = placement_physical("", c->path);
                for (k = 0; candidate && k < in->n && !c->pkg[0]; k++) {
                    size_t f;
                    if (!placement_named(in->m[k].name)) continue;
                    for (f = 0; f < in->m[k].nfiles; f++) {
                        char *physical = placement_physical(root, in->m[k].files[f].path);
                        int same = physical && !ascii_casecmp(candidate, physical);
                        free(physical);
                        if (same) {
                            snprintf(c->pkg, sizeof c->pkg, "%s %s", in->m[k].name, in->m[k].version);
                            break;
                        }
                    }
                }
                free(candidate);
            }
        }
    }
    r->loaded = pkg_fs_loaded(base, device, &lv, &lr, &r->opencnt);
    if (r->loaded == 1) snprintf(r->loaded_ver, sizeof r->loaded_ver, "%u.%u", lv, lr);
    channel_offers(a, base, r->offer, sizeof r->offer, r->offer_chan, sizeof r->offer_chan);

    /* the loader's walk: what each place gives, and where it stops */
    for (i = 0; i < r->n; i++) {
        struct cand *c = &r->c[i];
        if (r->loaded == 1) {
            snprintf(c->verdict, sizeof c->verdict, "%s", c->state == C_NOFILE ? "no file"
                     : "not consulted: the copy in memory is used");
            continue;
        }
        if (r->taken != (size_t)-1) {
            snprintf(c->verdict, sizeof c->verdict, "%s", c->state == C_NOFILE ? "no file"
                     : "not reached: found earlier");
            if (c->state == C_OK && want && c->ver[0] && pkg_version_cmp(c->ver, want) >= 0 && better == (size_t)-1)
                better = i;
            continue;
        }
        switch (c->state) {
        case C_NOFILE: snprintf(c->verdict, sizeof c->verdict, "no file"); break;
        case C_DIR: snprintf(c->verdict, sizeof c->verdict, "a directory, passed over"); break;
        case C_UNREADABLE: snprintf(c->verdict, sizeof c->verdict, "unreadable, passed over"); break;
        case C_NOTEXEC:
            snprintf(c->verdict, sizeof c->verdict, "not a program file: LoadSeg fails, passed over");
            snprintf(c->next, sizeof c->next, "remove %s: it is no library, and hides nothing now, but "
                     "confuses whoever looks", c->path);
            break;
        case C_WRONGCPU:
            snprintf(c->verdict, sizeof c->verdict, "wrong CPU (%s build on a %s system): LoadSeg "
                     "fails, passed over", c->cpu, mach);
            snprintf(c->next, sizeof c->next, "this file is a %s build; rebuild it for %s AROS, or "
                     "remove it", c->cpu, mach);
            break;
        case C_NORESIDENT:
            snprintf(c->verdict, sizeof c->verdict, "not a library (no resident tag): passed over; an "
                     "AROS without that check stops here and the open fails");
            snprintf(c->next, sizeof c->next, "remove %s, or replace it with the real %s", c->path, base);
            break;
        case C_WRONGTYPE:
            r->taken = i;
            c->fails = 1;
            snprintf(c->verdict, sizeof c->verdict, "taken, but it is a %s, not a %s: the open fails",
                     c->rtype == 3 ? "device" : c->rtype == 9 ? "library" : "resource",
                     device ? "device" : "library");
            snprintf(c->next, sizeof c->next, "remove %s: it is not the %s the program asks for",
                     c->path, device ? "device" : "library");
            break;
        default:
            r->taken = i;
            if (want != NULL && c->ver[0] && pkg_version_cmp(c->ver, want) < 0) {
                c->fails = 1;
                snprintf(c->verdict, sizeof c->verdict, "taken, too old (%s, %s needed): the open fails",
                         c->ver, want);
            } else {
                snprintf(c->verdict, sizeof c->verdict, "taken%s", want != NULL && !c->ver[0]
                         ? " (its version cannot be read here)" : "");
            }
            {
                /* an older copy found first hides a newer one: say so, asked or not */
                size_t k;
                for (k = i + 1; k < r->n && !c->fails; k++)
                    if (r->c[k].state == C_OK && r->c[k].ver[0] && c->ver[0]
                        && pkg_version_cmp(r->c[k].ver, c->ver) > 0) {
                        r->shadows = 1;
                        snprintf(c->verdict, sizeof c->verdict, "taken, and hides the newer %s %s",
                                 r->c[k].path, r->c[k].ver);
                        snprintf(c->next, sizeof c->next, "remove %s: %s %s is newer and would "
                                 "then be taken", c->path, r->c[k].path, r->c[k].ver);
                        break;
                    }
            }
        }
    }

    /* the answer, and what to do */
    if (r->loaded == 1) {
        if (want != NULL && pkg_version_cmp(r->loaded_ver, want) < 0) {
            size_t k, newer = (size_t)-1;
            for (k = 0; k < r->n; k++)
                if (r->c[k].state == C_OK && r->c[k].ver[0] && pkg_version_cmp(r->c[k].ver, want) >= 0) { newer = k; break; }
            r->rc = 18;
            snprintf(r->summary, sizeof r->summary, "%s is in memory at %s, older than the %s asked "
                     "for: every program gets that copy", base, r->loaded_ver, want);
            if (newer != (size_t)-1) {
                char nx[400];
                snprintf(nx, sizeof nx, "Avail FLUSH, then start the program again: the copy in "
                         "memory (%s) is older than this file (%s)", r->loaded_ver, r->c[newer].ver);
                memcpy(r->c[newer].next, nx, sizeof nx);
            }
        } else {
            snprintf(r->summary, sizeof r->summary, "%s is in memory at %s (opened %u times): every "
                     "program gets that copy, whatever the files say", base, r->loaded_ver, r->opencnt);
        }
        return;
    }
    if (r->taken == (size_t)-1) {
        r->rc = 11;
        snprintf(r->summary, sizeof r->summary, "%s is found nowhere: none of the %lu places the "
                 "loader looks holds a %s it can use", base, (unsigned long)r->n, device ? "device" : "library");
        if (r->n > 0) {
            struct cand *last = &r->c[r->n - 1];
            if (!last->next[0]) {
                if (r->offer[0])
                    snprintf(last->next, sizeof last->next, "install it: package %s provides %s "
                             "(pkg INSTALL %.*s ROOT <root> CHANNEL %.150s)", r->offer, base,
                             (int)strcspn(r->offer, " "), r->offer, r->offer_chan);
                else
                    snprintf(last->next, sizeof last->next, "install %s%s%s into %s", base,
                             want ? " " : "", want ? want : "", device ? "SYS:Devs" : "SYS:Libs");
            }
        }
        return;
    }
    {
        struct cand *t = &r->c[r->taken];
        if (!t->fails) {
            snprintf(r->summary, sizeof r->summary, "%s resolves to %s%s%s", base, t->path,
                     t->ver[0] ? " " : "", t->ver);
            return;
        }
        r->rc = 18;
        snprintf(r->summary, sizeof r->summary, "%s resolves to %s%s%s, and the open fails: %s",
                 base, t->path, t->ver[0] ? " " : "", t->ver, t->verdict);
        if (!t->next[0]) {
            char nx[400];
            if (better != (size_t)-1)
                snprintf(nx, sizeof nx, "remove %s: %s %s is newer and would be taken",
                         t->path, r->c[better].path, r->c[better].ver);
            else if (r->offer[0])
                snprintf(nx, sizeof nx, "install a newer one: package %s provides %s "
                         "(pkg UPGRADE or INSTALL %.*s ROOT <root> CHANNEL %.150s)", r->offer, base,
                         (int)strcspn(r->offer, " "), r->offer, r->offer_chan);
            else
                snprintf(nx, sizeof nx, "copy or install a %s %s or newer into %s, and "
                         "remove %s", base, want, device ? "SYS:Devs" : "SYS:Libs", t->path);
            memcpy(t->next, nx, sizeof nx);
        }
    }
}

static void resolve_print(const struct resolution *r, const char *base)
{
    size_t i;
    if (r->loaded == 1 && machine) {
        char j[200];
        snprintf(j, sizeof j, "%s %s %u", base, r->loaded_ver, r->opencnt);
        rec_item("loaded", j, "name", base, "version", r->loaded_ver, NULL);
    }
    if (machine) {
        for (i = 0; i < r->n; i++) {
            const struct cand *c = &r->c[i];
            char j[1400];
            int chosen = r->loaded != 1 && i == r->taken;
            snprintf(j, sizeof j, "%s %s %s %s %s", c->path, c->state == C_NOFILE ? "no" : "yes",
                     c->ver[0] ? c->ver : "-", c->pkg[0] ? c->pkg : "-", chosen ? "chosen" : "-");
            rec_item("candidate", j, "path", c->path, "exists", c->state == C_NOFILE ? "no" : "yes",
                     "version", c->ver[0] ? c->ver : "-", "package", c->pkg[0] ? c->pkg : "-",
                     "chosen", chosen ? "yes" : "no", "where", c->where, "verdict", c->verdict,
                     "next", c->next[0] ? c->next : "-", NULL);
            kv("verdict", "%s %s", c->path, c->verdict);
            if (c->next[0]) kv("next-step", "%s %s", c->path, c->next);
        }
        if (r->loaded == 1) kv("winner", "memory %s", r->loaded_ver);
        else if (r->taken != (size_t)-1) kv("winner", "%s %s", r->c[r->taken].path,
                                            r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-");
        return;
    }
    {
        static const int widths[] = { 20, 40, 8, 18, 0 };
        tbl_head(widths, "Where\tFile\tVersion\tPackage\tVerdict");
        if (r->loaded == 1)
            tbl_row("%s\t%s\t%s\t%s\t%s", "memory", base, r->loaded_ver, "-", "taken: already loaded");
        for (i = 0; i < r->n; i++) {
            const struct cand *c = &r->c[i];
            tbl_row("%s\t%s\t%s\t%s\t%s", c->where, c->path, c->ver[0] ? c->ver : "-",
                    c->state == C_NOFILE ? "-" : c->pkg[0] ? c->pkg : "not from a package", c->verdict);
        }
        tbl_end();
        for (i = 0; i < r->n; i++)
            if (r->c[i].next[0]) say_detail("%s: %s", r->c[i].where, r->c[i].next);
    }
}

int cmd_resolve(const struct pkg_options *a)
{
    struct installed in;
    struct resolution *r;
    const char *name = a->target, *root = a->root, *from = a->from;
    unsigned char *data = NULL;
    size_t len = 0;
    int program = 0, rc = 0;
    char fromdir[1100];

    if (name == NULL) return refuse_c(20, "name the library or device, RESOLVE SDL2.library, or a "
                                      "program, RESOLVE Work:Game/Game");
    if (a->version != NULL && pkg_check_version(a->version) != NULL)
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    {
        size_t nl = strlen(name);
        int libname = (nl > 8 && ascii_casecmp(name + nl - 8, ".library") == 0)
                      || (nl > 7 && ascii_casecmp(name + nl - 7, ".device") == 0);
        if (!libname && pkg_fs_exists(name) && !pkg_fs_is_dir(name) && pkg_fs_read(name, &data, &len) == 0) {
            if (file_arch(data, len) == NULL) {
                free(data);
                return refuse_c(20, "%s is neither a library name nor a program: RESOLVE takes "
                                "SDL2.library, or the path of an executable", name);
            }
            program = 1;
        }
    }
#if defined(__AROS__)
    if (root == NULL) root = "SYS:";
#else
    if (root == NULL && getenv("PKG_LIBS_PATH") == NULL) {
        free(data);
        return refuse_c(20, "name the directory that stands for SYS: with ROOT <dir>, or list the "
                        "directories of LIBS: in PKG_LIBS_PATH");
    }
#endif
    if (program && from == NULL) {
        /* the program's own directory is its PROGDIR: */
        const char *sl = strrchr(name, '/'), *co = strrchr(name, ':');
        const char *cut = sl && (!co || sl > co) ? sl : co;
        if (cut == NULL) snprintf(fromdir, sizeof fromdir, ".");
        else snprintf(fromdir, sizeof fromdir, "%.*s", (int)(cut - name + (cut == co)), name);
        from = fromdir;
    }
#if !defined(__AROS__)
    if (from == NULL) from = ".";
#endif
    {
        const char *w = a->arch;
        struct pkg_options aa = *a;
        aa.arch = w;
        aa.root = root;
        if (resolve_arch(&aa) != 0) { free(data); return 1; }
    }
    in.m = NULL;
    in.n = 0;
    if (root != NULL) {
        char *dbd = pkg_join(root, ".pkg/db");
        if (dbd != NULL && pkg_fs_exists(dbd) && load_all(root, &in) != 0) { free(dbd); free(data); return 1; }
        free(dbd);
    }
    r = (struct resolution *)malloc(sizeof *r);
    if (r == NULL) { installed_free(&in); free(data); return refuse("out of memory"); }

    if (!program) {
        const char *base = strrchr(name, ':');
        base = base ? base + 1 : name;
        resolve_one(a, name, a->version, root, from, &in, r);
        resolve_print(r, base);
        if (a->version != NULL)
            kv("satisfies", "%s", r->rc == 0 ? "yes" : "no");
        rc = r->rc;
        if (rc == 0) {
            kv("result", "resolved");
            kv("summary", "%s", r->summary);
            if (!machine) say_result("%s", r->summary);
        } else {
            refuse_c(rc, "%s", r->summary);
        }
    } else {
        struct pkg_strs names = { NULL, 0 };
        size_t i, bad = 0, first = 0;
        static const int widths[] = { 22, 36, 8, 0 };
        lib_names(data, len, &names);
        kv("program", "%s", name);
        if (!machine) tbl_head(widths, "Library\tTaken from\tVersion\tVerdict");
        for (i = 0; i < names.n; i++) {
            const char *verdict;
            char where[1100];
            resolve_one(a, names.v[i], NULL, root, from, &in, r);
            if (r->loaded == 1) snprintf(where, sizeof where, "memory");
            else if (r->taken != (size_t)-1) snprintf(where, sizeof where, "%s", r->c[r->taken].path);
            else snprintf(where, sizeof where, "-");
            verdict = r->loaded == 1 ? "loaded" : r->taken != (size_t)-1 ? r->c[r->taken].verdict
                    : is_system_lib(names.v[i]) ? "part of AROS, not in this root" : "found nowhere";
            if (r->rc != 0 && !(r->rc == 11 && is_system_lib(names.v[i]))) {
                if (!bad++) first = (size_t)r->rc;
            }
            if (machine) {
                char j[1400];
                snprintf(j, sizeof j, "%s %s %s", names.v[i], where,
                         r->loaded == 1 ? r->loaded_ver : r->taken != (size_t)-1 && r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-");
                rec_item("library", j, "name", names.v[i], "from", where, "verdict", verdict, NULL);
            } else {
                tbl_row("%s\t%s\t%s\t%s", names.v[i], where,
                        r->loaded == 1 ? r->loaded_ver : r->taken != (size_t)-1 && r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-",
                        verdict);
            }
            if ((r->rc != 0 && !(r->rc == 11 && is_system_lib(names.v[i]))) || r->shadows) {
                size_t k;
                for (k = 0; k < r->n; k++)
                    if (r->c[k].next[0]) {
                        if (machine) kv("next-step", "%s %s", names.v[i], r->c[k].next);
                        else say_detail("%s: %s", names.v[i], r->c[k].next);
                    }
            }
        }
        if (!machine) tbl_end();
        if (names.n == 0) {
            kv("result", "resolved");
            if (!machine) say_result("%s names no library or device", name);
        } else if (bad == 0) {
            kv("result", "resolved");
            kv("summary", "all %lu libraries %s names resolve", (unsigned long)names.n, name);
            if (!machine) say_result("all %lu libraries %s names resolve", (unsigned long)names.n, name);
        } else {
            rc = (int)first;
            refuse_c(rc, "%lu of the %lu libraries %s names would fail to open; RESOLVE <library> "
                     "shows each place the loader looks", (unsigned long)bad, (unsigned long)names.n, name);
        }
        pkg_strs_free(&names);
    }
    free(r);
    free(data);
    installed_free(&in);
    return rc == 0 ? 0 : 1;
}

int cmd_repair(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    size_t p, npk = 0, refused = 0;
    unsigned long total = 0, total_aside = 0, fixed_pk = 0;
    int first = 0;
    char firstwhy[200] = "";

    if (!a->all && a->target == NULL) return refuse_c(20, "name the package to repair, or REPAIR ALL");
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->all && a->key != NULL)
        return refuse_c(20, "KEY is the publisher key of one package; REPAIR ALL checks each against "
                        "the key this root pins for it. Give KEY to REPAIR <name>");
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, &ix) != 0) return 1;
    if (a->all) {
        if (load_all(a->root, &in) != 0) { free(ix.e); return 1; }
    } else {
        in.m = NULL;
        in.n = 0;
    }
    npk = a->all ? in.n : 1;
    for (p = 0; p < npk; p++) {
        const char *name = a->all ? in.m[p].name : a->target;
        unsigned long r = 0, s = 0;
        if (repair_one(a, &ix, name, &r, &s) != 0) {
            refused++;
            if (!first) {
                first = refused_class ? refused_class : 17;
                snprintf(firstwhy, sizeof firstwhy, "%s", name);
            }
            if (machine) kv("refused", "%s", name);
            refused_class = 0;
            continue;
        }
        total += r;
        total_aside += s;
        if (r) fixed_pk++;
        if (machine) kv("package", "%s %s", name, r ? "repaired" : "intact");
        else if (r) say_pkgline(name, "%lu file%s put back", r, r == 1 ? "" : "s");
    }
    installed_free(&in);
    free(ix.e);
    kv("restored", "%lu", total);
    kv("set-aside", "%lu", total_aside);
    if (refused) {
        refused_class = first;
        kv("result", "refused");
        summary_line("%lu file%s put back in %lu package%s; %lu package%s could not be repaired, "
           "first %s", total, total == 1 ? "" : "s", fixed_pk, fixed_pk == 1 ? "" : "s",
           (unsigned long)refused, refused == 1 ? "" : "s", firstwhy);
        return 1;
    }
    kv("result", total ? (dry_run ? "would-repair" : "repaired") : "unchanged");
    if (total == 0)
        summary_line("nothing needed repair: every file is the one installed, or a configuration "
           "file someone edited");
    else
        summary_line("%lu file%s %sput back in %lu package%s%s", total, total == 1 ? "" : "s",
           dry_run ? "would be " : "", fixed_pk, fixed_pk == 1 ? "" : "s",
           total_aside ? "; the changed ones kept beside as <file>.pkgold" : "");
    return 0;
}

/* Take one package out of an in-memory list, as removing it would. */
static void drop_installed(struct installed *in, const char *name)
{
    size_t i;
    for (i = 0; i < in->n; i++) {
        if (strcmp(in->m[i].name, name) != 0)
            continue;
        pkg_manifest_free(&in->m[i]);
        memmove(&in->m[i], &in->m[i + 1], (in->n - i - 1) * sizeof in->m[0]);
        in->n--;
        return;
    }
}

static int remove_orphans(const struct pkg_options *a)
{
    struct installed in;
    size_t *which, n, i, total = 0, nfailed = 0, at_f = 0;
    int loaded = 0, first_class = 0;
    const char *first_next = NULL;
    char failed[2048];              /* " name name ... ": those that could not go, not tried again */

    failed[0] = '\0';

    for (;;) {
        /* A dry run works on the list in memory, since nothing leaves the disk. */
        if (!(dry_run && loaded) && load_all(a->root, &in) != 0)
            return 1;
        loaded = 1;
        which = malloc((in.n ? in.n : 1) * sizeof *which);
        if (which == NULL) { installed_free(&in); return refuse("out of memory"); }
        n = find_orphans(&in, a->root, which);
        {
            /* the ones that already failed are not tried again */
            size_t kept_n = 0;
            for (i = 0; i < n; i++) {
                char key[80];
                snprintf(key, sizeof key, " %s ", in.m[which[i]].name);
                if (strstr(failed, key) == NULL)
                    which[kept_n++] = which[i];
            }
            n = kept_n;
        }
        for (i = 0; i < n; i++) {
            const struct pkg_manifest *m = &in.m[which[i]];
            size_t r, k, g;
            int ok_rm;
            quiet = 1;
            ok_rm = remove_files(a->root, m, &r, &k, &g, 1) == 0;
            quiet = 0;
            if (!ok_rm) {
                /* this one stays, with its reason; the others still go */
                char *q, jn[2600], codes[8];
                for (q = quiet_reason; *q; q++) if (*q == '\n') *q = ' ';
                if (first_class == 0) { first_class = refused_class; first_next = refused_next; }
                snprintf(codes, sizeof codes, "%d", refused_class);
                snprintf(jn, sizeof jn, "%s %s %s %s", m->name, m->version, class_name(refused_class),
                         quiet_reason);
                if (machine)
                    rec_item("refused", jn, "name", m->name, "version", m->version, "class",
                             class_name(refused_class), "code", codes, "reason", quiet_reason, NULL);
                else
                    say_pkgline(m->name, "not removed: %s", quiet_reason);
                if (at_f + 80 < sizeof failed)
                    at_f += (size_t)snprintf(failed + at_f, sizeof failed - at_f, "%s%s ",
                                             at_f ? "" : " ", m->name);
                nfailed++;
                refused_class = 0;
                refused_next = NULL;
                continue;
            }
            total++;
            if (machine)
            {
                char j[140];
                snprintf(j, sizeof j, "%s %s", m->name, m->version);
                rec_item("package", j, "name", m->name, "version", m->version, NULL);
            }
            else
                say_pkgline(m->name, "%s %s, which nothing needed (%lu file%s%s)",
                        m->version, dry_run ? "would be removed" : "removed",
                        (unsigned long)r, r == 1 ? "" : "s", k ? ", edited files kept" : "");
        }
        if (dry_run) {
            char names[64][65];
            size_t k2, nn = n < 64 ? n : 64;
            for (k2 = 0; k2 < nn; k2++)
                snprintf(names[k2], sizeof names[k2], "%s", in.m[which[k2]].name);
            for (k2 = 0; k2 < nn; k2++)
                drop_installed(&in, names[k2]);
        }
        free(which);
        if (!dry_run || n == 0)
            installed_free(&in);
        if (n == 0)
            break;              /* removing one orphan can orphan its own dependencies */
    }
    {
        char summary[600];
        if (total == 0 && nfailed == 0)
            snprintf(summary, sizeof summary, "nothing to remove: every installed package is "
                     "wanted or needed by one that is");
        else if (nfailed == 0)
            snprintf(summary, sizeof summary, "%s %lu package%s nothing needed any more",
                     dry_run ? "would remove" : "removed", (unsigned long)total, total == 1 ? "" : "s");
        else
            snprintf(summary, sizeof summary, "%s %lu package%s nothing needed; %lu could not be "
                     "removed:%s", dry_run ? "would remove" : "removed", (unsigned long)total,
                     total == 1 ? "" : "s", (unsigned long)nfailed, failed);
        if (nfailed == 0) {
            kv("result", "%s", total ? res("removed", "would-remove") : "unchanged");
        } else {
            refused_class = first_class;
            refused_next = first_next;
            kv("result", "refused");
            kv("class", "%s", class_name(first_class));
            kv("code", "%d", first_class);
        }
        kv("count", "%lu", (unsigned long)total);
        kv("summary", "%s", summary);
        if (nfailed)
            kv("next", "%s", first_next ? first_next : next_default(first_class));
        if (!machine)
            say_result("%s", summary);
    }
    return nfailed ? 1 : 0;
}

int cmd_remove(const struct pkg_options *a)
{
    struct pkg_manifest m;
    struct installed in;
    size_t removed, kept, gone, i, n, *which;
    char who[400];

    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (a->orphans && a->target == NULL) return remove_orphans(a);
    if (a->target == NULL)  return refuse_c(20, "name the package to remove, or ORPHANS");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    if (load_all(a->root, &in) != 0) { pkg_manifest_free(&m); return 1; }
    if (needed_by(&in, m.name, who, sizeof who)) {
        installed_free(&in);
        refuse_c(16, "%s is needed by %s; nothing was removed. Removing it would break %s",
                 m.name, who, strchr(who, ',') ? "them" : "it");
        pkg_manifest_free(&m);
        return 1;
    }
    installed_free(&in);
    if (remove_files(a->root, &m, &removed, &kept, &gone, 1) != 0) {
        pkg_manifest_free(&m);
        return 1;
    }
    kv("result", "%s", res("removed", "would-remove"));
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("root", "%s", a->root);
    kv("removed", "%lu", (unsigned long)removed);
    kv("gone", "%lu", (unsigned long)gone);
    if (!machine) {
    {
    char tail[80];
    int at = 0;
    tail[0] = '\0';
    if (kept) at += snprintf(tail + at, sizeof tail - (size_t)at, ", %lu kept", (unsigned long)kept);
    if (gone) snprintf(tail + at, sizeof tail - (size_t)at, ", %lu already gone", (unsigned long)gone);
    say_result("%s %s %s from %s: %lu file%s %s%s", dry_run ? "would remove" : "removed", m.name,
           m.version, a->root, (unsigned long)removed, removed == 1 ? "" : "s",
           dry_run ? "to remove" : "removed", tail);
    }
    }
    /* Say what this leaves behind; removing it is a separate, explicit act. */
    if (load_all(a->root, &in) == 0) {
        if (dry_run)
            drop_installed(&in, m.name);
        which = malloc((in.n ? in.n : 1) * sizeof *which);
        n = which ? find_orphans(&in, a->root, which) : 0;
        for (i = 0; i < n; i++) {
            if (machine)
            {
                char j[140];
                snprintf(j, sizeof j, "%s %s", in.m[which[i]].name, in.m[which[i]].version);
                rec_item("orphan", j, "name", in.m[which[i]].name, "version",
                         in.m[which[i]].version, NULL);
            }
            else
                say_detail("%s %s is no longer needed by anything; REMOVE ORPHANS takes it out",
                        in.m[which[i]].name, in.m[which[i]].version);
        }
        free(which);
        installed_free(&in);
    }
    /* Pkg removing itself leaves the root's records and pinned keys, which a
     * later Pkg picks up; say so, since nothing else would. */
    if (!dry_run && strcmp(m.name, "pkg") == 0)
        hint("pkg is gone, but %s%s.pkg still holds what is installed, the keys pinned for it "
             "and the downloads; a later pkg takes over from there, and the guide to removing "
             "pkg says what to delete when nothing should stay", a->root,
             *a->root && strchr(":/", a->root[strlen(a->root) - 1]) ? "" : "/");
    pkg_manifest_free(&m);
    return 0;
}

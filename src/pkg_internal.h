/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * What the modules of libpkg share with each other and with nothing else.
 * The library is one program cut along its own sections, one file each:
 *
 *   pkg_lib_core.c       failure classes, output, refusals, the activity line
 *   pkg_lib_keys.c       signing keys, and OpenSSH's formats
 *   pkg_lib_build.c      a package built from a drawer or an archive
 *   pkg_lib_channel.c    channels, one or several, local or over the network
 *   pkg_lib_root.c       a root: its machine, its database, its files, dependencies
 *   pkg_lib_verbs.c      the verbs that change a root or a channel
 *   pkg_lib_resolve.c    RESOLVE
 *   pkg_lib_status.c     STATUS, UPGRADE ALL, and the CHANNEL command
 *   pkg_lib_search.c     SEARCH, locally and through a portal
 *   pkg_lib_api.c        the operations pkg.h declares, and update checks
 *   pkg_lib_push.c       PUSH
 *
 * A function or variable one module uses from another is declared here
 * and is otherwise private to its file. Each is linked under the prefix
 * pkgi_, set by a macro below, so that a program linking libpkg.a never
 * meets a global named warn or hint: libc has warn, and the program's calls
 * would reach ours. `make check-symbols` refuses any global of the library
 * not named pkg_, pkgi_, PKG_ or BZ2_.
 *
 * libpkg: every operation Pkg performs, behind the interface in pkg.h. The
 * command line (pkg_main.c) and anything else that links this reach the same
 * code, and so the same checks.
 *
 * DEPENDS takes a comma-separated list, each "name" or "name >= version". An
 * install resolves the whole graph, fetching and verifying every package,
 * before it places a single file; dependencies go in first and are marked as
 * such in `.pkg/auto`, and removing orphans takes out the ones nothing needs.
 *
 * KIND image turns the drawer into a read-only FFS volume, pkg_image.h, and
 * the package holds that one file, <name>.hdf.
 *
 * A channel is a directory: `index` holds one "name version digest" line per
 * published version, `objects/` holds each payload, its manifest and its
 * signature under the payload's SHA-256.
 *
 * Every package is signed; there is no development mode. The signature covers
 * the manifest, and the manifest names the payload digest, so one signature
 * covers every byte installed. A root pins the key that signed each package the
 * first time it was installed, in `.pkg/keys`, and refuses a different key
 * later unless acceptkey names the new key in full.
 *
 * Selection: with no version, the highest published version (COMPATIBLE with
 * any). With a version, exactly that one (EXACT) or a refusal.
 */

#ifndef PKG_INTERNAL_H
#define PKG_INTERNAL_H

#include "pkg.h"
#include "pkg_activity.h"
#include "pkg_container.h"
#include "pkg_ed25519.h"
#include "pkg_sha512.h"
#include "pkg_fs.h"
#include "pkg_image.h"
#include "pkg_manifest.h"
#include "pkg_ameta.h"
#include "pkg_archive.h"
#include "pkg_sha256.h"
#include "pkg_pkginfo.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Link names: every name declared below is linked as pkgi_<name>. */
#define activity_say pkgi_activity_say
#define activity_show pkgi_activity_show
#define arch_ambiguous pkgi_arch_ambiguous
#define arch_for pkgi_arch_for
#define arch_matches pkgi_arch_matches
#define archive_path pkgi_archive_path
#define ascii_casecmp pkgi_ascii_casecmp
#define ascii_casecmp_n pkgi_ascii_casecmp_n
#define attrs_one pkgi_attrs_one
#define attrs_report pkgi_attrs_report
#define beside pkgi_beside
#define build_package pkgi_build_package
#define built_free pkgi_built_free
#define by_rel pkgi_by_rel
#define call pkgi_call
#define cancelled pkgi_cancelled
#define chan_file pkgi_chan_file
#define chan_label pkgi_chan_label
#define chan_name_of pkgi_chan_name_of
#define chan_name_ok pkgi_chan_name_ok
#define chan_of pkgi_chan_of
#define chanlist_free pkgi_chanlist_free
#define chanlist_init pkgi_chanlist_init
#define chanlist_read pkgi_chanlist_read
#define chanlist_text pkgi_chanlist_text
#define chanlist_write pkgi_chanlist_write
#define chans pkgi_chans
#define chans_clear pkgi_chans_clear
#define chans_text pkgi_chans_text
#define check_sig pkgi_check_sig
#define claimed_signer pkgi_claimed_signer
#define cmd_channel pkgi_cmd_channel
#define cmd_checksig pkgi_cmd_checksig
#define cmd_image pkgi_cmd_image
#define cmd_install pkgi_cmd_install
#define cmd_keygen pkgi_cmd_keygen
#define cmd_keyinfo pkgi_cmd_keyinfo
#define cmd_list pkgi_cmd_list
#define cmd_manifest pkgi_cmd_manifest
#define cmd_mountlist pkgi_cmd_mountlist
#define cmd_publish pkgi_cmd_publish
#define cmd_remove pkgi_cmd_remove
#define cmd_repair pkgi_cmd_repair
#define cmd_resolve pkgi_cmd_resolve
#define cmd_rollback pkgi_cmd_rollback
#define cmd_search pkgi_cmd_search
#define cmd_show pkgi_cmd_show
#define cmd_sign pkgi_cmd_sign
#define cmd_status pkgi_cmd_status
#define cmd_upgrade pkgi_cmd_upgrade
#define cmd_verify pkgi_cmd_verify
#define cmd_withdraw pkgi_cmd_withdraw
#define counting pkgi_counting
#define damage pkgi_damage
#define did pkgi_did
#define doing pkgi_doing
#define doing_bytes pkgi_doing_bytes
#define doing_things pkgi_doing_things
#define drawer_attrs pkgi_drawer_attrs
#define drawer_free pkgi_drawer_free
#define dry_run pkgi_dry_run
#define fetch pkgi_fetch
#define fetch_manifest pkgi_fetch_manifest
#define fetched_free pkgi_fetched_free
#define file_arch pkgi_file_arch
#define file_bytes pkgi_file_bytes
#define file_digest pkgi_file_digest
#define file_state pkgi_file_state
#define find_file pkgi_find_file
#define find_orphans pkgi_find_orphans
#define find_ver pkgi_find_ver
#define fromhex pkgi_fromhex
#define hint pkgi_hint
#define inherit_channel pkgi_inherit_channel
#define inherit_ix pkgi_inherit_ix
#define inherited pkgi_inherited
#define installed_free pkgi_installed_free
#define installed_path pkgi_installed_path
#define is_auto pkgi_is_auto
#define is_system_lib pkgi_is_system_lib
#define is_url pkgi_is_url
#define is_withdrawn pkgi_is_withdrawn
#define keep_current_setup pkgi_keep_current_setup
#define kv pkgi_kv
#define last_published pkgi_last_published
#define leave_out pkgi_leave_out
#define lib_names pkgi_lib_names
#define load_all pkgi_load_all
#define load_installed pkgi_load_installed
#define load_key pkgi_load_key
#define load_one pkgi_load_one
#define machine pkgi_machine
#define nchans pkgi_nchans
#define needed_by pkgi_needed_by
#define net_err pkgi_net_err
#define net_failed pkgi_net_failed
#define net_get_watched pkgi_net_get_watched
#define next_cli_words pkgi_next_cli_words
#define next_default pkgi_next_default
#define nfetched_once pkgi_nfetched_once
#define note pkgi_note
#define nplanned pkgi_nplanned
#define object_path pkgi_object_path
#define on_archive_read pkgi_on_archive_read
#define on_transfer pkgi_on_transfer
#define on_wait pkgi_on_wait
#define one_line pkgi_one_line
#define open_channels pkgi_open_channels
#define opt_unpacked pkgi_opt_unpacked
#define other_signers pkgi_other_signers
#define pending_next pkgi_pending_next
#define pick pkgi_pick
#define pick_refused pkgi_pick_refused
#define pick_root pkgi_pick_root
#define pick_why pkgi_pick_why
#define pinned_key pkgi_pinned_key
#define placement_available pkgi_placement_available
#define placement_check pkgi_placement_check
#define placement_named pkgi_placement_named
#define placement_physical pkgi_placement_physical
#define placement_prepare pkgi_placement_prepare
#define placements pkgi_placements
#define placements_clear pkgi_placements_clear
#define placements_load pkgi_placements_load
#define plan_free pkgi_plan_free
#define plan_target pkgi_plan_target
#define planned pkgi_planned
#define planned_version pkgi_planned_version
#define present_in pkgi_present_in
#define quiet pkgi_quiet
#define quiet_reason pkgi_quiet_reason
#define read_index pkgi_read_index
#define rec_item pkgi_rec_item
#define record_arch pkgi_record_arch
#define refuse_c pkgi_refuse_c
#define refused_class pkgi_refused_class
#define refused_next pkgi_refused_next
#define remove_files pkgi_remove_files
#define repair_one pkgi_repair_one
#define requested_at pkgi_requested_at
#define res pkgi_res
#define resolve_arch pkgi_resolve_arch
#define root_arch pkgi_root_arch
#define root_path pkgi_root_path
#define run_plan pkgi_run_plan
#define say_err pkgi_say_err
#define say_info_from pkgi_say_info_from
#define say_item pkgi_say_item
#define say_kind pkgi_say_kind
#define say_not_found pkgi_say_not_found
#define say_pkgline pkgi_say_pkgline
#define say_raw pkgi_say_raw
#define set_auto pkgi_set_auto
#define short12 pkgi_short12
#define short_name pkgi_short_name
#define show_defers_archives pkgi_show_defers_archives
#define sink pkgi_sink
#define stand pkgi_stand
#define strs_has_nocase pkgi_strs_has_nocase
#define summary_line pkgi_summary_line
#define t_read pkgi_t_read
#define t_rename pkgi_t_rename
#define t_rmtree pkgi_t_rmtree
#define t_unlink pkgi_t_unlink
#define t_write pkgi_t_write
#define t_write_private pkgi_t_write_private
#define target_arch pkgi_target_arch
#define tbl_end pkgi_tbl_end
#define tbl_head pkgi_tbl_head
#define tbl_row pkgi_tbl_row
#define to_image pkgi_to_image
#define tohex pkgi_tohex
#define told_source pkgi_told_source
#define tr pkgi_tr
#define upgrade_all pkgi_upgrade_all
#define verb_name pkgi_verb_name
#define warn pkgi_warn
#define wlist pkgi_wlist
#define write_index pkgi_write_index
#define write_sig pkgi_write_sig
#define wstate pkgi_wstate

#define class_name pkg_class_name
#define say_result(...) say_kind(PKG_LINE_RESULT, "%s\n", __VA_ARGS__)
#define say_problem(...) say_kind(PKG_LINE_PROBLEM, "%s\n", __VA_ARGS__)
#define say_detail(...) say_kind(PKG_LINE_DETAIL, "  %s\n", __VA_ARGS__)
#define say_note(...) say_kind(PKG_LINE_NOTE, "  note: %s\n", __VA_ARGS__)
#define next_words next_cli_words
#define PKG_MAX_CHANNELS 24
/* A refusal whose next step differs from its class's usual one. */
#define refuse_n(cls, nx, ...) (pending_next = (nx), refuse_c((cls), __VA_ARGS__))
#define refuse(...) refuse_c(PKGRC_REFUSED, __VA_ARGS__)

/* Every module but the one that defines them calls the filesystem
 * through the traced wrappers, so TRACE shows each file read, written,
 * renamed or removed. */
#ifndef PKGI_UNTRACED_FS
#define pkg_fs_read          t_read
#define pkg_fs_write_atomic  t_write
#define pkg_fs_write_private t_write_private
#define pkg_fs_rename        t_rename
#define pkg_fs_unlink        t_unlink
#define pkg_fs_rmtree        t_rmtree
#endif

enum {
    PKGRC_OK         = PKG_RC_OK,
    PKGRC_REFUSED    = PKG_RC_REFUSED,
    PKGRC_NOTFOUND   = PKG_RC_NOTFOUND,
    PKGRC_INTEGRITY  = PKG_RC_INTEGRITY,
    PKGRC_SIGNATURE  = PKG_RC_SIGNATURE,
    PKGRC_KEY        = PKG_RC_KEY,
    PKGRC_CONFLICT   = PKG_RC_CONFLICT,
    PKGRC_DEPENDENCY = PKG_RC_DEPENDENCY,
    PKGRC_IO         = PKG_RC_IO,
    PKGRC_POLICY     = PKG_RC_POLICY,
    PKGRC_USAGE      = PKG_RC_USAGE
};




struct key {
    unsigned char pk[PKG_ED25519_PUBLIC];
    unsigned char sk[PKG_ED25519_SECRET];
    char          pkhex[2 * PKG_ED25519_PUBLIC + 1];
};



struct loaded {
    char          *rel;
    unsigned char *data;
    size_t         len;
    unsigned long long prot;    /* the AROS protection word it is published with */
    char          *comment;     /* its comment, UTF-8; NULL for none */
    int            host_exec;   /* from an archive's mode: 1, 0; -2 ask the file system */
    /* From an archive's metadata index, with no data loaded: */
    int            pre;
    char           pre_digest[PKG_SHA256_HEXLEN + 1];
    const char    *pre_arch;    /* NULL: no executable header */
    char           pre_cookie[140];  /* "name version", or "" */
};

struct drawer {
    const char    *root;
    const char    *files;       /* FILES: the paths of the tree that make the package */
    struct loaded *v;
    size_t         n, cap;
    char         **left_out;        /* host files not packaged, dirs with '/' */
    size_t         nleft;
    char           err[512];
};

/* What PUBLISH lets a new version inherit: the channel it publishes into.
 * NULL for MANIFEST and IMAGE, which know no channel. */
struct index;

struct built {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    char          *text;
    size_t         text_len;
    char           ver_from[512];   /* the version was read from this file's $VER */
    char           name_from[512];  /* the name was */
    char           cookie_ver[64];  /* what that $VER says, whatever was used */
    char           cookie_file[512];
    char           arch_from[512];
    char           kind_from[160];  /* KIND taken from this published version */
    char           deps_from[160];  /* DEPENDS too */
    char           config_from[160]; /* CONFIG too */
    char           about_from[1200];/* the catalogue fields too, or the .pkginfo */
    char           info_from[1200]; /* INFO: the .pkginfo the fields were read from */
    char           info_fields[200];/*   and which of them it gave */
    size_t         nconfig;         /* configuration files */
    unsigned       skipped;
    char         **left_out;
    size_t         nleft;
};

struct present_ctx { const char *dir; };



struct entry {
    char name[65];
    char version[64];
    char arch[32];      /* one version may be published for several CPUs */
    char digest[PKG_SHA256_HEXLEN + 1];
    int  withdrawn;     /* its publisher signed a withdrawal; -1 until asked */
    unsigned char ch;   /* the channel it was read from: chan_of() names it */
};

struct index {
    struct entry *e;
    size_t        n;
};



struct chanlist {
    char  *v[PKG_MAX_CHANNELS];       /* the channel: a directory or a URL */
    char  *name[PKG_MAX_CHANNELS];    /* the short name it answers to, or NULL */
    size_t n;
};

/* A fetched, verified package.
 *
 * The index names a version by the digest of its MANIFEST, never of its
 * payload. The manifest is the signed object and it names the payload, so the
 * chain runs index -> manifest -> payload, each link checked. Payloads stay
 * content-addressed and may be shared: two versions with identical bytes, or
 * two packages shipping the same files, keep separate manifests and signatures
 * over one payload object. Keying manifests by the payload, as the first layout
 * did, let the second of two such publishes overwrite the first's manifest and
 * signature; a diagnostic run with two identical handler builds found it. */
struct fetched {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    unsigned char *mtext;
    size_t         mlen;
    char           signer[65];
    char           digest[PKG_SHA256_HEXLEN + 1];   /* the manifest's */
};

/* Placement records keep signed logical paths separate from physical paths.
 * A root reserves each relocated drawer prefix for its recorded package. */
struct placement {
    char *name, *prefix, *parent;
    int fresh;
    struct placement *next;
};

struct installed {
    struct pkg_manifest *m;
    size_t               n;
};

/* What an install, upgrade or rollback will do, decided and verified in full
 * before anything is placed: dependencies first, the named package last. */
struct plan {
    struct fetched *f;
    int            *dep;         /* 1: pulled in as a dependency */
    size_t          n;
    const char     *stack[32];   /* the path being resolved, for cycles */
    size_t          depth;
    const char     *root, *acceptkey;
    const char     *key;         /* KEY: the publisher's key for the named package */
    const struct index *ix;
};

struct cand {
    char        where[48];      /* as AROS names the place: "PROGDIR:libs/", "LIBS:" */
    char        path[1100];     /* the file looked for */
    int         state;
    char        ver[64];        /* from its $VER, else its resident's version, or "" */
    const char *cpu;            /* from its header, or NULL */
    int         rtype;          /* the resident's node type, or -1 */
    char        pkg[80];        /* the installed package that lists it, or "" */
    char        verdict[160];
    char        next[400];
    int         fails;          /* this row is why the open fails */
};



struct planned_up { const char *name, *version; };

/* One installed package against the channel. */
struct standing {
    const struct pkg_manifest *m;
    const struct entry *offer;   /* what UPGRADE <name> would take; NULL: nothing */
    const char *state;           /* current, upgradable, withdrawn, not-offered, edited */
    int newer;                   /* offer is higher than the installed version */
    int withdrawn;               /* the installed version was withdrawn by its publisher */
    int edited;                  /* a file differs from the installed manifest */
    int conflict;                /* two listed channels offer it under different keys */
    char why[2400];              /* the conflict, in full */
};



struct hit {
    char name[65];
    char version[64];
    char arch[120];
    char short_desc[200];
    char chan[700];
};

struct hits {
    struct hit *v;
    size_t      n, cap;
};

typedef int (*op_fn)(const struct pkg_options *);

/* pkg_lib_core.c */
extern const struct pkg_sink *sink;
extern const char *verb_name;
extern int refused_class;
extern int machine;
extern int dry_run;
void say_kind(int kind, const char *frame, const char *fmt, ...);
const char *damage(size_t changed, size_t missing);
void say_item(const char *word, const char *fmt, ...);
void say_pkgline(const char *name, const char *fmt, ...);
void tbl_head(const int *widths, const char *cells);
void tbl_row(const char *fmt, ...);
void tbl_end(void);
void tr(const char *fmt, ...);
void activity_say(void *user, const char *text);
void activity_show(void *user, const char *text);
long long file_bytes(const char *path);
void doing(const char *verb, const char *object);
void doing_bytes(const char *verb, const char *object, long long whole);
void doing_things(const char *verb, const char *object, long long n, const char *word);
void did(void);
void counting(size_t i, size_t n);
void short_name(char *out, size_t ol, const char *path);
void on_transfer(long long done, long long total);
void on_wait(const char *host);
void on_archive_read(long long done, long long total);
int net_get_watched(const char *url, const char *dest, char *err, size_t errlen);
void say_err(const char *fmt, ...);
void say_raw(const char *buf, size_t len);
const char *res(const char *done, const char *would);
void one_line(char *s);
void kv(const char *key, const char *fmt, ...);
void summary_line(const char *fmt, ...);
void rec_item(const char *kind, const char *joined, ...);
const char *next_default(int cls);
const char *next_cli_words(const char *next);
extern const char *pending_next;
extern const char *refused_next;
extern int quiet;
extern char quiet_reason[2048];
int refuse_c(int cls, const char *fmt, ...);
void warn(const char *fmt, ...);
void hint(const char *fmt, ...);
void short12(const char *hex, char out[13]);
void tohex(const unsigned char *b, size_t n, char *hex);
int fromhex(unsigned char *b, size_t n, const char *hex);
int t_read(const char *path, unsigned char **buf, size_t *len);
int t_write(const char *path, const void *buf, size_t len);
int t_write_private(const char *path, const void *buf, size_t len);
int t_rename(const char *from, const char *to);
int t_unlink(const char *path);
int t_rmtree(const char *path);

/* pkg_lib_keys.c */
int cmd_keygen(const struct pkg_options *a);
int load_key(const char *path, struct key *k);
int write_sig(const char *path, const struct key *k,
              const unsigned char *msg, size_t len);
int check_sig(const char *sigpath, const unsigned char *msg, size_t len,
              char signer[65], const char *what);
int cmd_keyinfo(const struct pkg_options *a);
int cmd_sign(const struct pkg_options *a);
int cmd_checksig(const struct pkg_options *a);

/* pkg_lib_build.c */
void leave_out(const char *rel, int is_dir, void *ctx);
int load_one(const char *rel, void *ctx);
int by_rel(const void *a, const void *b);
void drawer_free(struct drawer *d);
int find_ver(const struct drawer *d, const char *want, char *name, size_t nl,
             char *ver, size_t vl, const char **from, char *seen, size_t sl);
/* A drawer that is a part of an archive: "archive!/prefix", optionally only
 * the paths FILES names under it. Paths are relative to the prefix, as they
 * install; owner Execute comes from the archive's own mode bits.
 *
 * Publishing needs each file's digest, size, mode, CPU and $VER, never its
 * bytes: the first read of an archive writes them to <archive>.pkgidx, keyed
 * to the archive's size and time, and every later publish from it reads that
 * index instead of the archive. Installers never use the index: they read
 * the archive and check each file against the signed manifest. */
const char *file_arch(const unsigned char *p, size_t len);
int to_image(struct drawer *d, const char *name);
extern const struct index *inherit_ix;
extern const char *inherit_channel;
void say_info_from(const struct built *b);
void built_free(struct built *b);
int present_in(const unsigned char *name, size_t len, void *ctx);
char *pkg_strdup(const char *s);
int drawer_attrs(struct drawer *d);
int ascii_casecmp_n(const char *x, const char *y, size_t n);
int ascii_casecmp(const char *x, const char *y);
int is_system_lib(const char *n);
int strs_has_nocase(const struct pkg_strs *l, const char *s);
void lib_names(const unsigned char *p, size_t len, struct pkg_strs *out);
int build_package(const struct pkg_options *a, struct built *out);

/* pkg_lib_channel.c */
extern char *chans[PKG_MAX_CHANNELS];
extern size_t nchans;
extern char *wlist[PKG_MAX_CHANNELS];
extern signed char wstate[PKG_MAX_CHANNELS];
const char *chan_of(const struct entry *e);
void chans_clear(void);
const char *chans_text(void);
int is_url(const char *ch);
const char *chan_label(const char *ch);
extern char net_err[400];
extern int net_failed;
extern size_t nfetched_once;
char *chan_file(const char *channel, const char *rel);
int read_index(const char *channel, struct index *ix);
int write_index(const char *channel, struct index *ix);
void chanlist_init(struct chanlist *c);
void chanlist_free(struct chanlist *c);
int chanlist_read(const char *root, struct chanlist *c);
const char *chanlist_text(const struct chanlist *c);
int chan_name_ok(const char *s);
void chan_name_of(const char *channel, char *out, size_t len);
int chanlist_write(const char *root, const struct chanlist *c);
extern const char *pick_root;
int open_channels(const struct pkg_options *a, struct index *ix);
char *object_path(const char *channel, const char *digest, const char *ext);
char *archive_path(const char *channel, const char *name);
int last_published(const char *name, const char *arch, struct pkg_manifest *em);
int inherited(const char *name, const char *arch, char *kind, size_t kl,
              char *deps, size_t dl, char *from, size_t fl, char *conf, size_t cl);

/* pkg_lib_root.c */
extern const char *target_arch;
extern char root_arch[32];
int arch_matches(const struct entry *e);
int resolve_arch(const struct pkg_options *a);
void record_arch(const char *root, const char *arch);
int arch_ambiguous(const struct index *ix, const char *name, char *list, size_t len);
int pinned_key(const char *root, const char *name, char out[65]);
extern int pick_refused;
extern char pick_why[2400];
const struct entry *pick(const struct index *ix, const char *name, const char *version);
void say_not_found(const struct index *ix, const char *name, const char *version,
                   const char *channel);
void fetched_free(struct fetched *f);
extern const char *opt_unpacked;
extern int told_source;
extern int show_defers_archives;
int fetch_manifest(const char *channel, const struct entry *e, struct fetched *f);
int fetch(const char *channel, const struct entry *e, struct fetched *f);
char *root_path(const char *root, const char *dir, const char *name);
extern struct placement *placements;
extern const char *requested_at;
void placements_clear(void);
struct placement *placement_named(const char *name);
char *installed_path(const char *root, const char *path);
int placement_available(const struct placement *p);
int placements_load(const char *root);
char *placement_physical(const char *root, const char *logical);
int placement_check(const struct pkg_manifest *m);
int load_installed(const char *root, const char *name, struct pkg_manifest *m,
                   int quiet);
int claimed_signer(const char *channel, const char *digest, char out[65]);
int other_signers(const struct index *ix, const char *name,
                  const char *signer, char *others, size_t olen);
int is_withdrawn(const struct entry *e);
int cmd_withdraw(const struct pkg_options *a);
int file_state(const char *root, const char *path, const char *digest,
               unsigned long long size);
const struct pkg_file *find_file(const struct pkg_manifest *m, const char *path);
void attrs_one(const char *root, const struct pkg_file *f);
void attrs_report(const char *name);
void installed_free(struct installed *in);
int load_all(const char *root, struct installed *in);
int placement_prepare(const char *root, const struct pkg_manifest *m);
char *beside(const char *path, const char *suffix);
int is_auto(const char *root, const char *name);
void set_auto(const char *root, const char *name, int on);
int needed_by(const struct installed *in, const char *name, char *out, size_t len);
void plan_free(struct plan *p);
int remove_files(const char *root, const struct pkg_manifest *m,
                 size_t *removed, size_t *kept, size_t *gone, int report);
int run_plan(struct plan *p, const struct pkg_manifest *cur,
             unsigned long *placed, unsigned long *dropped, unsigned long *kept);
int pkg_is_key_hex(const char *s);
int plan_target(struct plan *p, const struct pkg_options *a, const struct index *ix,
                const char *name, const char *exact);
size_t find_orphans(const struct installed *in, const char *root, size_t *which);

/* pkg_lib_verbs.c */
int cmd_mountlist(const struct pkg_options *a);
int cmd_show(const struct pkg_options *a);
int cmd_image(const struct pkg_options *a);
int cmd_manifest(const struct pkg_options *a);
int cmd_publish(const struct pkg_options *a);
int cmd_install(const struct pkg_options *a);
int cmd_upgrade(const struct pkg_options *a);
int cmd_rollback(const struct pkg_options *a);
int cmd_list(const struct pkg_options *a);
int cmd_verify(const struct pkg_options *a);
int repair_one(const struct pkg_options *a, const struct index *ix, const char *name,
               unsigned long *restored, unsigned long *aside);

/* pkg_lib_resolve.c */
int cmd_resolve(const struct pkg_options *a);
int cmd_repair(const struct pkg_options *a);
int cmd_remove(const struct pkg_options *a);

/* pkg_lib_status.c */
extern struct planned_up *planned;
extern size_t nplanned;
/* During a dry-run UPGRADE ALL, the version a package upgraded earlier in the
 * same run would be at; NULL otherwise. Defined with UPGRADE ALL below. */
const char *planned_version(const char *name);
void arch_for(const char *base, const struct pkg_manifest *m);
void stand(const char *root, const struct index *ix, const char *base,
           const struct pkg_manifest *m, struct standing *s);
void note(const char *fmt, ...);
int keep_current_setup(const struct pkg_options *a, struct index *ix, struct installed *in);
int cmd_channel(const struct pkg_options *a);

/* pkg_lib_search.c */
int cmd_search(const struct pkg_options *a);
int cmd_status(const struct pkg_options *a);
int upgrade_all(const struct pkg_options *a);

/* pkg_lib_api.c */
/* The caller's cancel callback, asked between steps. */
int cancelled(const char *when);
int call(const struct pkg_sink *s, const char *verb, op_fn fn, const struct pkg_options *o);

/* pkg_lib_push.c */
int file_digest(const char *path, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size);

#endif

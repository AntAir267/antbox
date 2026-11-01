/*
 * disc_install.c - the data installer. See disc_install.h.
 */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#endif

#include "disc_install.h"
#include "xdvdfs.h"
#include "xr_sha256.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define close _close
#define fdopen _fdopen
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#define PATH_N      2048
#define CHUNK       (1u << 20)
#define JOURNAL     "install.journal"
#define JOURNAL_TAG "xr-install-journal 1"
#define MARKER      ".complete"
#define MARKER_TAG  "xr-install 1"
#define EXE_MAX     (256u << 20)
#define HEAD_N      65536

typedef struct {
    const xr_install_opts *o;
    xr_install_result *r;
    const xr_manifest *m;
    xdvdfs_image *img;
    char     src_root[PATH_N];      /* a folder source: where its files are */
    char     files_dir[PATH_N];     /* dest/subdir */
    char     made_dir[PATH_N];      /* the last directory made */
    uint8_t *buf;
    uint64_t total, done;
    double   said;
    FILE    *journal;
} inst;

/* ── the platform ──────────────────────────────────────────────────── */

/* 1 when `p` exists. */
static int p_stat(const char *p, int *dir, uint64_t *size)
{
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(p, &st) != 0)
        return 0;
    if (dir)
        *dir = (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    if (dir)
        *dir = S_ISDIR(st.st_mode);
#endif
    if (size)
        *size = (uint64_t)st.st_size;
    return 1;
}

static int p_mkdir(const char *p)
{
    int dir = 0;
#if defined(_WIN32)
    if (_mkdir(p) == 0)
        return 1;
#else
    if (mkdir(p, 0755) == 0)
        return 1;
#endif
    if (errno == EEXIST && p_stat(p, &dir, NULL) && dir)
        return 1;
    return 0;
}

/* Makes `tmp` (a writable copy); on ENOENT makes its parent first, then
 * tries again. The directories above one that exists are never looked at:
 * some Android ROMs hide /storage/emulated/0/Android/data from the app
 * (stat fails) although the app's own folder under it is there (BO-229). */
static int p_mkdirs_at(char *tmp)
{
    size_t i;
    int ok;

    if (p_mkdir(tmp))
        return 1;
    if (errno != ENOENT)
        return 0;
    for (i = strlen(tmp); i > 0; i--)
        if (tmp[i] == '/' || tmp[i] == '\\')
            break;
    if (i == 0 || (i == 2 && tmp[1] == ':'))
        return 0;   /* no parent left to make */
    tmp[i] = '\0';
    ok = p_mkdirs_at(tmp);
    tmp[i] = '/';
    return ok && p_mkdir(tmp);
}

/* `p` and the directories above it. errno says why not. */
static int p_mkdirs(const char *p)
{
    char tmp[PATH_N];
    size_t n = strlen(p);
    int dir = 0;

    if (!n || n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (p_stat(p, &dir, NULL))
        return dir ? 1 : (errno = ENOTDIR, 0);
    memcpy(tmp, p, n + 1);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\'))
        tmp[--n] = '\0';
    return p_mkdirs_at(tmp);
}

/* Over an existing file too. */
static int p_replace(const char *from, const char *to)
{
#if defined(_WIN32)
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING))
        return 1;
    errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
    return 0;
#else
    return rename(from, to) == 0;
#endif
}

/* The file's data to the disk before it gets its name. */
static int p_sync(FILE *f)
{
    if (fflush(f) != 0)
        return 0;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int p_free(const char *dir, uint64_t *avail)
{
#if defined(_WIN32)
    ULARGE_INTEGER a;
    if (!GetDiskFreeSpaceExA(dir, &a, NULL, NULL))
        return 0;
    *avail = a.QuadPart;
#else
    struct statvfs sv;
    if (statvfs(dir, &sv) != 0)
        return 0;
    *avail = (uint64_t)sv.f_bavail * sv.f_frsize;
#endif
    return 1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
    return !*a && !*b;
}

/* Each entry of `dir` but . and ..; stops when fn returns 0. */
typedef int (*entry_fn)(const char *dir, const char *name, void *ctx);

static void p_list(const char *dir, entry_fn fn, void *ctx)
{
#if defined(_WIN32)
    char pat[PATH_N];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && !fn(dir, fd.cFileName, ctx))
            break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d)
        return;
    while ((de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && !fn(dir, de->d_name, ctx))
            break;
    closedir(d);
#endif
}

typedef struct {
    const char *want;
    char *out;
    size_t n;
    int found;
} find_ctx;

static int find_entry(const char *dir, const char *name, void *vctx)
{
    find_ctx *c = (find_ctx *)vctx;

    if (!same_nocase(name, c->want))
        return 1;
    c->found = snprintf(c->out, c->n, "%s/%s", dir, name) < (int)c->n;
    return 0;
}

/* dir/name whatever the case of name on the disk. */
static int p_find_nocase(const char *dir, const char *name, char *out, size_t n)
{
    find_ctx c;

    c.want = name;
    c.out = out;
    c.n = n;
    c.found = 0;
    p_list(dir, find_entry, &c);
    return c.found;
}

/* ── results ───────────────────────────────────────────────────────── */

const char *xr_install_code_name(int code)
{
    static const char *const names[XR_INSTALL_CODES] = {
        "ok", "no-source", "source-unreadable", "not-xbox-image", "xbox360-disc", "no-xbe",
        "wrong-game", "wrong-version", "truncated-image", "damaged-source", "disk-full",
        "no-permission", "write-failed", "cancelled", "internal"
    };
    return code >= 0 && code < XR_INSTALL_CODES ? names[code] : "internal";
}

static int set_fail(inst *in, int code, const char *fmt, ...)
{
    va_list ap;
    char *s;

    va_start(ap, fmt);
    vsnprintf(in->r->message, sizeof in->r->message, fmt, ap);
    va_end(ap);
    for (s = in->r->message; *s; s++)       /* one line: it goes into a line protocol */
        if (*s == '\n' || *s == '\r')
            *s = ' ';
    in->r->code = code;
    return code;
}

static void say(inst *in, const char *fmt, ...)
{
    char text[PATH_N];
    va_list ap;

    if (!in->o->log)
        return;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    in->o->log(in->o->ctx, text);
}

static void advance(inst *in, uint64_t n, const char *text)
{
    double f;

    in->done += n;
    f = in->total ? (double)in->done / (double)in->total : 1.0;
    if (f > 1.0)
        f = 1.0;
    if (in->o->progress && (f - in->said >= 0.002 || (f >= 1.0 && in->said < 1.0))) {
        in->said = f;
        in->o->progress(in->o->ctx, f, text);
    }
}

static int cancelled(inst *in)
{
    return in->o->cancelled && in->o->cancelled(in->o->ctx);
}

static int fail_cancelled(inst *in)
{
    return set_fail(in, XR_INSTALL_CANCELLED, "Cancelled. What was copied is kept: the next "
                    "install goes on from there.");
}

/* "3" for 3 GB, "2.5" for 2.5 GB, "25" for 25 GB: bytes as the words say them. */
static const char *gb_text(uint64_t bytes, char out[16])
{
    double gb = (double)bytes / 1e9;

    snprintf(out, 16, gb < 10 ? "%.1f" : "%.0f", gb);
    if (gb < 10 && strlen(out) > 2 && !strcmp(out + strlen(out) - 2, ".0"))
        out[strlen(out) - 2] = '\0';
    return out;
}

/* A write that failed, by errno. */
static int fail_write(inst *in, const char *path)
{
    int e = errno;

    if (e == ENOSPC
#ifdef EDQUOT
        || e == EDQUOT
#endif
        )
        return set_fail(in, XR_INSTALL_DISK_FULL, "The disk is full: %s could not be written. "
                        "Free some space and start again; the install goes on where it stopped.",
                        path);
    if (e == EACCES || e == EPERM || e == EROFS)
        return set_fail(in, XR_INSTALL_NO_PERMISSION, "No permission to write %s. Choose another "
                        "folder, or change the permissions of this one.", path);
    return set_fail(in, XR_INSTALL_WRITE_FAILED, "Writing %s failed: %s.", path, strerror(e));
}

uint64_t xr_manifest_bytes(const xr_manifest *m)
{
    uint64_t n = 0;
    unsigned i;

    for (i = 0; i < m->count; i++)
        n += m->files[i].size;
    return n;
}

static int manifest_find(const xr_manifest *m, const char *path, int nocase)
{
    unsigned i;

    for (i = 0; i < m->count; i++)
        if (nocase ? same_nocase(m->files[i].path, path) : !strcmp(m->files[i].path, path))
            return (int)i;
    return -1;
}

/* ── the source ────────────────────────────────────────────────────── */

typedef struct {
    const xdvdfs_file *xf;
    FILE    *fp;
    uint64_t size, pos;
} src_file;

/* root/rel, and when that is not there, each part of rel by any case (a
 * folder extracted on a case-sensitive disk by a tool that changed it). */
static int folder_path(const char *root, const char *rel, char *out, size_t n)
{
    char cur[PATH_N], next[PATH_N], part[XDVDFS_PATH];
    int dir = 0;

    if (snprintf(out, n, "%s/%s", root, rel) < (int)n && p_stat(out, &dir, NULL) && !dir)
        return 1;
    snprintf(cur, sizeof cur, "%s", root);
    while (*rel) {
        size_t k = strcspn(rel, "/");
        if (k >= sizeof part)
            return 0;
        memcpy(part, rel, k);
        part[k] = '\0';
        if (!p_find_nocase(cur, part, next, sizeof next))
            return 0;
        snprintf(cur, sizeof cur, "%s", next);
        rel += k + (rel[k] == '/');
    }
    return snprintf(out, n, "%s", cur) < (int)n && p_stat(out, &dir, NULL) && !dir;
}

/* 1 open, 0 not in the source, -1 there and unreadable. */
static int src_open(inst *in, const char *rel, src_file *sf)
{
    memset(sf, 0, sizeof *sf);
    if (in->o->source_open) {
        int fd = in->o->source_open(in->o->ctx, rel, &sf->size);
        if (fd == -1)
            return 0;
        if (fd < 0)
            return -1;
        sf->fp = fdopen(fd, "rb");
        if (!sf->fp) {
            close(fd);
            return -1;
        }
        setvbuf(sf->fp, NULL, _IONBF, 0);
    } else if (in->img) {
        sf->xf = xdvdfs_find(in->img, rel);
        if (!sf->xf)
            return 0;
        sf->size = sf->xf->size;
    } else {
        char path[PATH_N];
        if (!folder_path(in->src_root, rel, path, sizeof path) || !p_stat(path, NULL, &sf->size))
            return 0;
        sf->fp = fopen(path, "rb");
        if (!sf->fp)
            return -1;
        setvbuf(sf->fp, NULL, _IONBF, 0);
    }
    return 1;
}

static int src_read(inst *in, src_file *sf, void *buf, size_t n)
{
    int ok = sf->xf ? xdvdfs_read(in->img, sf->xf, sf->pos, buf, n)
                    : fread(buf, 1, n, sf->fp) == n;
    sf->pos += n;
    return ok;
}

static void src_close(src_file *sf)
{
    if (sf->fp)
        fclose(sf->fp);
    sf->fp = NULL;
}

typedef struct {
    const char *exe;
    char *out;
    size_t n;
    int found;
} root_ctx;

static int root_entry(const char *dir, const char *name, void *vctx)
{
    root_ctx *c = (root_ctx *)vctx;
    char sub[PATH_N], hit[PATH_N];
    int is_dir = 0;

    if (snprintf(sub, sizeof sub, "%s/%s", dir, name) >= (int)sizeof sub
        || !p_stat(sub, &is_dir, NULL) || !is_dir || !p_find_nocase(sub, c->exe, hit, sizeof hit))
        return 1;
    c->found = snprintf(c->out, c->n, "%s", sub) < (int)c->n;
    return 0;
}

/* The folder that holds the executable: `path` or one level below it (an
 * extracted disc is often wrapped in one more folder). */
static int folder_root(const char *path, const char *exe, char *out, size_t n)
{
    char hit[PATH_N];
    root_ctx c;

    if (p_find_nocase(path, exe, hit, sizeof hit))
        return snprintf(out, n, "%s", path) < (int)n;
    c.exe = exe;
    c.out = out;
    c.n = n;
    c.found = 0;
    p_list(path, root_entry, &c);
    return c.found;
}

static const char *base_name(const char *p)
{
    const char *s = p + strlen(p);

    while (s > p && s[-1] != '/' && s[-1] != '\\')
        s--;
    return s;
}

/* What an executable that is not the manifest's is: its certificate's title,
 * title id and region (the XBE header: "XBEH", base address at 0x104, the
 * certificate's address at 0x118; in it the title id at +8, the title as 40
 * UTF-16 characters at +0x0C and the region flags at +0xA0). */
static int fail_identity(inst *in, const uint8_t *head, size_t n, const char *got_hex)
{
    const xr_manifest *m = in->m;
    const char *want_hex = m->files[manifest_find(m, m->exe, 1)].sha256;
    uint32_t base, cert, title_id, region;
    char title[48], where[64] = "";
    size_t c, i;

    if (n < 0x180 || memcmp(head, "XBEH", 4))
        return set_fail(in, XR_INSTALL_DAMAGED, "The %s of this disc is not an Xbox executable: "
                        "the image or folder is damaged, or not a disc of %s.", m->exe, m->title);
    base = head[0x104] | (uint32_t)head[0x105] << 8 | (uint32_t)head[0x106] << 16
         | (uint32_t)head[0x107] << 24;
    cert = head[0x118] | (uint32_t)head[0x119] << 8 | (uint32_t)head[0x11A] << 16
         | (uint32_t)head[0x11B] << 24;
    c = (size_t)(cert - base);
    if (cert < base || c + 0xB0 > n)
        return set_fail(in, XR_INSTALL_DAMAGED, "The %s of this disc has no readable "
                        "certificate: the image or folder is damaged.", m->exe);
    title_id = head[c + 8] | (uint32_t)head[c + 9] << 8 | (uint32_t)head[c + 10] << 16
             | (uint32_t)head[c + 11] << 24;
    for (i = 0; i < 40; i++) {
        unsigned ch = head[c + 0x0C + i * 2] | (unsigned)head[c + 0x0D + i * 2] << 8;
        if (!ch)
            break;
        title[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
    }
    title[i] = '\0';
    region = head[c + 0xA0] | (uint32_t)head[c + 0xA1] << 8 | (uint32_t)head[c + 0xA2] << 16;
    if (region & 1)
        strcat(where, "North America");
    if (region & 2)
        strcat(where, *where ? " + Japan" : "Japan");
    if (region & 4)
        strcat(where, *where ? " + Europe/rest of world" : "Europe/rest of world");
    if (!*where)
        snprintf(where, sizeof where, "region flags 0x%X", (unsigned)region);

    if (title_id != m->title_id)
        return set_fail(in, XR_INSTALL_WRONG_GAME, "This is the disc of \"%s\" (title id "
                        "0x%08X), not of %s. Give the disc image of %s, %s.",
                        title, (unsigned)title_id, m->title, m->title, m->build);
    return set_fail(in, XR_INSTALL_WRONG_VERSION, "This is %s, but not the version this program "
                    "was made for: its %s is for %s and has the SHA-256 %.16s..., where %s has "
                    "%.16s.... Other regions, other pressings and patched or trimmed executables "
                    "cannot be used; only the %s disc can.",
                    m->title, m->exe, where, got_hex, m->build, want_hex, m->build);
}

/* Open the source and make sure it is the manifest's disc: its executable,
 * byte for byte. */
static int identify(inst *in)
{
    const xr_install_opts *o = in->o;
    const xr_manifest *m = in->m;
    char err[512] = "", hex[65];
    uint8_t digest[32], *head;
    xr_sha256 sha;
    src_file sf;
    uint64_t left;
    size_t head_n = 0;
    int dir = 0, rc, mi = manifest_find(m, m->exe, 1);

    if (mi < 0)
        return set_fail(in, XR_INSTALL_INTERNAL, "The manifest does not list %s.", m->exe);
    if (o->source_open) {
        /* A document tree: no path; the callback finds the executable. */
        uint64_t sz;
        int fd = o->source_open(o->ctx, m->exe, &sz);
        if (fd < 0 && fd != -2) {
            fd = o->source_open(o->ctx, "default.xex", &sz);
            if (fd >= 0) {
                close(fd);
                return set_fail(in, XR_INSTALL_XBOX360, "This folder is an Xbox 360 disc (it "
                                "has a default.xex), not an original Xbox one.");
            }
            return set_fail(in, XR_INSTALL_NO_EXE, "There is no %s in the chosen folder or one "
                            "folder below it: this is not an extracted Xbox disc.", m->exe);
        }
        if (fd >= 0)
            close(fd);
        rc = XDVDFS_OK;
    } else if (o->source_fd >= 0) {
        rc = xdvdfs_open_fd(o->source_fd, &in->img, err, sizeof err);
    } else if (!o->source || !p_stat(o->source, &dir, NULL)) {
        return set_fail(in, XR_INSTALL_NO_SOURCE, "%s does not exist.", o->source ? o->source : "?");
    } else if (dir) {
        if (!folder_root(o->source, m->exe, in->src_root, sizeof in->src_root)) {
            char hit[PATH_N];
            if (p_find_nocase(o->source, "default.xex", hit, sizeof hit))
                return set_fail(in, XR_INSTALL_XBOX360, "This folder is an Xbox 360 disc (it has "
                                "a default.xex), not an original Xbox one.");
            return set_fail(in, XR_INSTALL_NO_EXE, "There is no %s in %s or one folder below it: "
                            "this is not an extracted Xbox disc.", m->exe, o->source);
        }
        rc = XDVDFS_OK;
    } else {
        errno = 0;
        rc = xdvdfs_open(o->source, &in->img, err, sizeof err);
    }
    in->r->image = in->img != NULL;
    switch (rc) {
    case XDVDFS_OK:
        break;
    case XDVDFS_E_OPEN:
        return set_fail(in, XR_INSTALL_SOURCE_UNREADABLE, "%s: %s.",
                        o->source ? o->source : "The disc image", err);
    case XDVDFS_E_NOT_XBOX:
        return set_fail(in, XR_INSTALL_NOT_XBOX, "%s is not an Xbox disc image: it has no Xbox "
                        "file system. Give the .iso of the game disc, or the folder it was "
                        "extracted to.", o->source ? base_name(o->source) : "This file");
    case XDVDFS_E_TRUNCATED:
        return set_fail(in, XR_INSTALL_TRUNCATED, "The disc image is incomplete: %s. Copy or "
                        "download it again.", err);
    case XDVDFS_E_NOMEM:
        return set_fail(in, XR_INSTALL_INTERNAL, "Out of memory reading the disc image.");
    default:
        return set_fail(in, XR_INSTALL_DAMAGED, "The disc image is damaged: %s.", err);
    }
    if (in->img)
        in->r->image_base = xdvdfs_base(in->img);

    rc = src_open(in, m->exe, &sf);
    if (rc == 0) {
        if (in->img && xdvdfs_find(in->img, "default.xex"))
            return set_fail(in, XR_INSTALL_XBOX360, "This is an Xbox 360 disc (it has a "
                            "default.xex), not an original Xbox one.");
        return set_fail(in, XR_INSTALL_NO_EXE, "The disc image has no %s: it is not a game disc "
                        "of the original Xbox.", m->exe);
    }
    if (rc < 0 || sf.size > EXE_MAX) {
        src_close(&sf);
        return set_fail(in, XR_INSTALL_SOURCE_UNREADABLE, "The %s of the source cannot be read.",
                        m->exe);
    }
    head = (uint8_t *)malloc(HEAD_N);
    if (!head) {
        src_close(&sf);
        return set_fail(in, XR_INSTALL_INTERNAL, "Out of memory.");
    }
    xr_sha256_init(&sha);
    for (left = sf.size; left; ) {
        size_t n = left > CHUNK ? CHUNK : (size_t)left;
        if (!src_read(in, &sf, in->buf, n)) {
            src_close(&sf);
            free(head);
            return set_fail(in, XR_INSTALL_SOURCE_UNREADABLE, "Reading %s from the source "
                            "failed.", m->exe);
        }
        if (!head_n) {
            head_n = n > HEAD_N ? HEAD_N : n;
            memcpy(head, in->buf, head_n);
        }
        xr_sha256_update(&sha, in->buf, n);
        left -= n;
    }
    src_close(&sf);
    xr_sha256_final(&sha, digest);
    xr_sha256_hex(digest, hex);
    rc = same_nocase(hex, m->files[mi].sha256) ? XR_INSTALL_OK : fail_identity(in, head, head_n, hex);
    free(head);
    return rc;
}

/* ── the journal and the marker ────────────────────────────────────── */

static void dest_path(inst *in, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", in->o->dest, name);
}

/* The files an earlier run finished: have[i] = 1. The journal is then
 * written again with those lines alone, so a line cut short by a kill is
 * gone, and opened to go on. */
static int journal_open(inst *in, uint8_t *have)
{
    char path[PATH_N], line[XDVDFS_PATH + 32], want[160];
    FILE *f;
    unsigned i;

    dest_path(in, JOURNAL, path, sizeof path);
    snprintf(want, sizeof want, "%s %s\n", JOURNAL_TAG, in->m->id);
    f = fopen(path, "rb");
    if (f) {
        if (fgets(line, sizeof line, f) && !strcmp(line, want)) {
            while (fgets(line, sizeof line, f)) {
                size_t n = strlen(line);
                int k;
                if (n < 5 || line[n - 1] != '\n' || strncmp(line, "ok ", 3))
                    continue;
                line[n - 1] = '\0';
                k = manifest_find(in->m, line + 3, 0);
                if (k >= 0)
                    have[k] = 1;
            }
        }
        fclose(f);
    }
    in->journal = fopen(path, "wb");
    if (!in->journal)
        return fail_write(in, path);
    fputs(want, in->journal);
    for (i = 0; i < in->m->count; i++)
        if (have[i])
            fprintf(in->journal, "ok %s\n", in->m->files[i].path);
    if (fflush(in->journal) != 0)
        return fail_write(in, path);
    return XR_INSTALL_OK;
}

static int journal_add(inst *in, const char *rel)
{
    if (fprintf(in->journal, "ok %s\n", rel) < 0 || fflush(in->journal) != 0) {
        char path[PATH_N];
        dest_path(in, JOURNAL, path, sizeof path);
        return fail_write(in, path);
    }
    return XR_INSTALL_OK;
}

int xr_install_marker_read(const char *dest, xr_install_marker *m)
{
    char path[PATH_N], line[256], key[32], value[160];
    int tagged = 0;
    FILE *f;

    memset(m, 0, sizeof *m);
    snprintf(path, sizeof path, "%s/%s", dest, MARKER);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fgets(line, sizeof line, f) && !strncmp(line, MARKER_TAG, strlen(MARKER_TAG)))
        tagged = 1;
    while (tagged && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %159[^\r\n]", key, value) != 2)
            continue;
        if (!strcmp(key, "manifest"))
            snprintf(m->manifest, sizeof m->manifest, "%s", value);
        else if (!strcmp(key, "version"))
            snprintf(m->version, sizeof m->version, "%s", value);
        else if (!strcmp(key, "files"))
            m->files = (unsigned)strtoul(value, NULL, 10);
        else if (!strcmp(key, "bytes"))
            m->bytes = strtoull(value, NULL, 10);
    }
    fclose(f);
    return tagged && m->manifest[0];
}

static int marker_write(inst *in)
{
    char path[PATH_N], part[PATH_N + 8];
    FILE *f;
    int ok;

    dest_path(in, MARKER, path, sizeof path);
    snprintf(part, sizeof part, "%s.part", path);
    f = fopen(part, "wb");
    if (!f)
        return fail_write(in, part);
    ok = fprintf(f, "%s\nmanifest %s\nversion %s\nfiles %u\nbytes %llu\n", MARKER_TAG, in->m->id,
                 in->o->version && *in->o->version ? in->o->version : "unknown", in->m->count,
                 (unsigned long long)in->total) > 0;
    ok = p_sync(f) && ok;
    if (fclose(f) != 0 || !ok || !p_replace(part, path)) {
        remove(part);
        return fail_write(in, path);
    }
    return XR_INSTALL_OK;
}

/* ── the files ─────────────────────────────────────────────────────── */

static void file_path(inst *in, const char *rel, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", in->files_dir, rel);
}

/* Is the file already in the destination what the manifest says?
 * 1 yes, 0 no, -1 cancelled. */
static int verify_in_place(inst *in, const xr_manifest_file *mf, const char *path)
{
    uint8_t digest[32];
    char hex[65];
    xr_sha256 sha;
    uint64_t left = mf->size;
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    setvbuf(f, NULL, _IONBF, 0);
    xr_sha256_init(&sha);
    while (left) {
        size_t n = left > CHUNK ? CHUNK : (size_t)left;
        if (cancelled(in)) {
            fclose(f);
            return -1;
        }
        if (fread(in->buf, 1, n, f) != n) {
            fclose(f);
            return 0;
        }
        xr_sha256_update(&sha, in->buf, n);
        left -= n;
        advance(in, n, mf->path);
    }
    fclose(f);
    xr_sha256_final(&sha, digest);
    xr_sha256_hex(digest, hex);
    return same_nocase(hex, mf->sha256);
}

static int copy_file(inst *in, const xr_manifest_file *mf, const char *path)
{
    char part[PATH_N + 8], dir[PATH_N], hex[65];
    uint8_t digest[32];
    xr_sha256 sha;
    src_file sf;
    uint64_t left;
    FILE *out;
    char *slash;
    int rc = src_open(in, mf->path, &sf);

    if (rc == 0)
        return set_fail(in, XR_INSTALL_DAMAGED, "%s is missing from the %s: it is not a complete "
                        "copy of the disc.", mf->path, in->img ? "disc image" : "folder");
    if (rc < 0)
        return set_fail(in, XR_INSTALL_SOURCE_UNREADABLE, "%s in the folder cannot be read: %s.",
                        mf->path, strerror(errno));
    if (sf.size != mf->size) {
        src_close(&sf);
        return set_fail(in, XR_INSTALL_DAMAGED, "%s has %llu bytes in the %s and %llu on the "
                        "disc: the copy is damaged or was changed.", mf->path,
                        (unsigned long long)sf.size, in->img ? "disc image" : "folder",
                        (unsigned long long)mf->size);
    }
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    if (strcmp(dir, in->made_dir)) {
        if (!p_mkdirs(dir)) {
            src_close(&sf);
            return fail_write(in, dir);
        }
        snprintf(in->made_dir, sizeof in->made_dir, "%s", dir);
    }
    snprintf(part, sizeof part, "%s.part", path);
    out = fopen(part, "wb");
    if (!out) {
        src_close(&sf);
        return fail_write(in, part);
    }
    setvbuf(out, NULL, _IONBF, 0);
    xr_sha256_init(&sha);
    rc = XR_INSTALL_OK;
    for (left = mf->size; left && rc == XR_INSTALL_OK; ) {
        size_t n = left > CHUNK ? CHUNK : (size_t)left;
        errno = 0;
        if (cancelled(in)) {
            rc = fail_cancelled(in);
        } else if (!src_read(in, &sf, in->buf, n)) {
            rc = set_fail(in, XR_INSTALL_SOURCE_UNREADABLE, "Reading %s from the %s failed%s%s. "
                          "If it is on a disc or a network drive, copy it to this machine first.",
                          mf->path, in->img ? "disc image" : "folder", errno ? ": " : "",
                          errno ? strerror(errno) : "");
        } else if (fwrite(in->buf, 1, n, out) != n) {
            rc = fail_write(in, part);
        } else {
            xr_sha256_update(&sha, in->buf, n);
            left -= n;
            advance(in, n, mf->path);
        }
    }
    src_close(&sf);
    if (rc == XR_INSTALL_OK && !p_sync(out))
        rc = fail_write(in, part);
    if (fclose(out) != 0 && rc == XR_INSTALL_OK)
        rc = fail_write(in, part);
    if (rc == XR_INSTALL_OK) {
        xr_sha256_final(&sha, digest);
        xr_sha256_hex(digest, hex);
        if (!same_nocase(hex, mf->sha256))
            rc = set_fail(in, XR_INSTALL_DAMAGED, "%s in the %s is not what the disc holds (its "
                          "SHA-256 is %.16s..., the disc's %.16s...): the copy is damaged or was "
                          "changed. Make the image again from the disc.", mf->path,
                          in->img ? "disc image" : "folder", hex, mf->sha256);
        else if (!p_replace(part, path))
            rc = fail_write(in, path);
    }
    if (rc != XR_INSTALL_OK)
        remove(part);
    return rc;
}

/* The marker's install, still whole: every file there with its size. */
static int files_present(inst *in)
{
    char path[PATH_N];
    uint64_t size;
    unsigned i;
    int dir;

    for (i = 0; i < in->m->count; i++) {
        file_path(in, in->m->files[i].path, path, sizeof path);
        if (!p_stat(path, &dir, &size) || dir || size != in->m->files[i].size)
            return 0;
    }
    return 1;
}

static int run(inst *in)
{
    const xr_install_opts *o = in->o;
    const xr_manifest *m = in->m;
    xr_install_result *r = in->r;
    xr_install_marker mark;
    char path[PATH_N];
    uint8_t *have;
    uint64_t present = 0, avail = 0;
    char a[16], b[16], c[16];
    unsigned i;
    int rc;

    rc = identify(in);
    if (rc != XR_INSTALL_OK || o->identify_only)
        return rc;
    if (in->img) {
        const xdvdfs_file *f = xdvdfs_files(in->img);
        for (i = 0; i < xdvdfs_count(in->img); i++)
            if (manifest_find(m, f[i].path, 1) < 0)
                r->extra++;
        if (r->extra)
            say(in, "%u files of the image are not part of the game's disc and are left out",
                r->extra);
    }

    if (!o->dest || !*o->dest)
        return set_fail(in, XR_INSTALL_INTERNAL, "No destination folder.");
    if (o->subdir && *o->subdir)
        snprintf(in->files_dir, sizeof in->files_dir, "%s/%s", o->dest, o->subdir);
    else
        snprintf(in->files_dir, sizeof in->files_dir, "%s", o->dest);
    if (!p_mkdirs(in->files_dir))
        return fail_write(in, in->files_dir);

    if (xr_install_marker_read(o->dest, &mark)) {
        if (!strcmp(mark.manifest, m->id) && files_present(in)) {
            r->already = 1;
            return XR_INSTALL_OK;
        }
        say(in, "the folder says it is installed, but not all of it is there: checking it again");
        dest_path(in, MARKER, path, sizeof path);
        remove(path);
    }

    say(in, "sha256 %s", xr_sha256_backend());   /* which code checks the files */
    have = (uint8_t *)calloc(m->count ? m->count : 1, 1);
    if (!have)
        return set_fail(in, XR_INSTALL_INTERNAL, "Out of memory.");
    rc = journal_open(in, have);
    /* have[i]: 1 whole (the journal), 2 there with the right size and to be
     * hashed, 0 to be copied. */
    for (i = 0; i < m->count && rc == XR_INSTALL_OK; i++) {
        uint64_t size;
        int dir;
        file_path(in, m->files[i].path, path, sizeof path);
        if (p_stat(path, &dir, &size) && !dir && size == m->files[i].size) {
            if (!have[i])
                have[i] = 2;
            present += size;
        } else {
            have[i] = 0;
        }
    }
    if (rc == XR_INSTALL_OK && o->free_needed > present && p_free(o->dest, &avail)
        && avail < o->free_needed - present)
        rc = set_fail(in, XR_INSTALL_DISK_FULL, "Not enough free space in %s: Takedawn needs "
                      "%s GB free (the game uses %s GB), and %s GB is free. Free up space on "
                      "this disk.", o->dest, gb_text(o->free_needed - present, a),
                      gb_text(in->total, b), gb_text(avail, c));

    for (i = 0; i < m->count && rc == XR_INSTALL_OK; i++) {
        const xr_manifest_file *mf = &m->files[i];
        file_path(in, mf->path, path, sizeof path);
        if (cancelled(in)) {
            rc = fail_cancelled(in);
            break;
        }
        if (have[i] == 1) {
            r->kept++;
            advance(in, mf->size, mf->path);
            continue;
        }
        if (have[i] == 2) {
            uint64_t before = in->done;
            int ok = verify_in_place(in, mf, path);
            if (ok < 0) {
                rc = fail_cancelled(in);
                break;
            }
            if (ok) {
                r->verified++;
                r->bytes_verified += mf->size;
                rc = journal_add(in, mf->path);
                continue;
            }
            in->done = before;          /* not it: copied below */
        }
        rc = copy_file(in, mf, path);
        if (rc == XR_INSTALL_OK) {
            r->copied++;
            r->bytes_copied += mf->size;
            rc = journal_add(in, mf->path);
        }
    }
    free(have);
    if (in->journal)
        fclose(in->journal);
    in->journal = NULL;
    if (rc != XR_INSTALL_OK)
        return rc;
    rc = marker_write(in);
    if (rc == XR_INSTALL_OK) {
        dest_path(in, JOURNAL, path, sizeof path);
        remove(path);
        if (in->o->progress && in->said < 1.0)
            in->o->progress(in->o->ctx, 1.0, "done");
    }
    return rc;
}

int xr_install(const xr_install_opts *o, xr_install_result *r)
{
    inst in;

    memset(&in, 0, sizeof in);
    memset(r, 0, sizeof *r);
    in.o = o;
    in.r = r;
    in.m = o->manifest;
    in.total = xr_manifest_bytes(o->manifest);
    in.buf = (uint8_t *)malloc(CHUNK);
    if (!in.buf) {
        set_fail(&in, XR_INSTALL_INTERNAL, "Out of memory.");
        return r->code;
    }
    run(&in);
    if (in.img)
        xdvdfs_close(in.img);
    if (in.journal)
        fclose(in.journal);
    free(in.buf);
    return r->code;
}

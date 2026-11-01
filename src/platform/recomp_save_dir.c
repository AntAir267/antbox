/*
 * recomp_save_dir.c - the save root by platform, the one-time move from the
 * working directory's saves/, and the backups taken at start.
 * See recomp_save_dir.h.
 *
 * The directories are each platform's place for per-user app data. Windows
 * and macOS as UnleashedRecomp (hedge-dev/UnleashedRecomp
 * UnleashedRecomp/user/paths.cpp, BuildUserPath: FOLDERID_RoamingAppData,
 * ~/Library/Application Support); on Linux the XDG data directory, which
 * the kernel's own default (kernel_path.c) already uses, rather than its
 * ~/.config, since saves are data and not configuration.
 */
#include "recomp_save_dir.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define rs_mkdir(p) _mkdir(p)
#define rs_rmdir(p) _rmdir(p)
#define rs_unlink(p) _unlink(p)
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
#define rs_mkdir(p) mkdir(p, 0755)
#define rs_rmdir(p) rmdir(p)
#define rs_unlink(p) unlink(p)
#endif

#define RS_PATH 1024

static void mkdir_p(const char *path)
{
    char tmp[RS_PATH];
    size_t i, n = strlen(path);

    if (!n || n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if ((tmp[i] == '/' || tmp[i] == '\\') && !(i == 2 && tmp[1] == ':')) {
            char c = tmp[i];
            tmp[i] = '\0';
            rs_mkdir(tmp);
            tmp[i] = c;
        }
    }
    rs_mkdir(tmp);
}

static int is_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Each entry of `dir` but . and .., in no particular order. Stops early,
 * returning 0, when fn does. */
typedef int (*entry_fn)(const char *dir, const char *name, int dir_entry, void *ctx);

static int for_each(const char *dir, entry_fn fn, void *ctx)
{
#if defined(_WIN32)
    char pat[RS_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int ok = 1;

    snprintf(pat, sizeof pat, "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (!fn(dir, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx)) {
            ok = 0;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    int ok = 1;

    if (!d)
        return 1;
    while ((de = readdir(d)) != NULL) {
        char p[RS_PATH];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!fn(dir, de->d_name, is_dir(p), ctx)) {
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok;
#endif
}

/* The first file a copy could not make, and why: the save history's log
 * line names it, so a failure on a player's machine says what held it. */
typedef struct {
    char     path[RS_PATH];
    unsigned code;                  /* Windows: GetLastError(); else errno */
    int      waits;                 /* retries left for the whole copy (Windows) */
} CopyFail;

#define COPY_WAITS 10               /* x 50 ms: one copy waits half a second at most */

static void copy_failed(CopyFail *fail, const char *path, unsigned code)
{
    if (fail && !fail->path[0]) {
        snprintf(fail->path, sizeof fail->path, "%s", path);
        fail->code = code;
    }
}

#if defined(_WIN32)
/* A source file is read sharing everything, delete included: the CRT's
 * fopen shares reading and writing but not deleting, so it could not open a
 * file a handle with DELETE access has open, and while it had one open
 * an open asking for that access failed. A file a handle shares nothing of
 * (another program's, a scanner's, the title's own for a moment) gets a
 * short retry, bounded for the whole copy (COPY_WAITS): the history copy
 * runs on the save guard's thread before the save is replaced and before
 * the quit's two-second wait ends, and giving up loses only this copy. */
static HANDLE open_source(const char *from, CopyFail *fail)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD e = 0;

    for (;;) {
        h = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        e = GetLastError();
        if ((e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION && e != ERROR_ACCESS_DENIED)
            || fail->waits <= 0)
            break;
        fail->waits--;
        Sleep(50);
    }
    SetLastError(e);
    return INVALID_HANDLE_VALUE;
}

static int copy_file(const char *from, const char *to, CopyFail *fail)
{
    HANDLE in = open_source(from, fail), out;
    char buf[65536];
    DWORD n, w;
    int ok = 1;

    if (in == INVALID_HANDLE_VALUE) {
        copy_failed(fail, from, GetLastError());
        return 0;
    }
    out = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        copy_failed(fail, to, GetLastError());
        CloseHandle(in);
        return 0;
    }
    for (;;) {
        if (!ReadFile(in, buf, sizeof buf, &n, NULL)) {
            copy_failed(fail, from, GetLastError());
            ok = 0;
            break;
        }
        if (!n)
            break;
        if (!WriteFile(out, buf, n, &w, NULL) || w != n) {
            copy_failed(fail, to, GetLastError());
            ok = 0;
            break;
        }
    }
    CloseHandle(in);
    if (!CloseHandle(out) && ok) {
        copy_failed(fail, to, GetLastError());
        ok = 0;
    }
    return ok;
}
#else
static int copy_file(const char *from, const char *to, CopyFail *fail)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    if (!in) {
        copy_failed(fail, from, (unsigned)errno);
        return 0;
    }
    out = fopen(to, "wb");
    if (!out) {
        copy_failed(fail, to, (unsigned)errno);
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            copy_failed(fail, to, (unsigned)errno);
            ok = 0;
            break;
        }
    if (ferror(in)) {
        copy_failed(fail, from, (unsigned)errno);
        ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) {
        copy_failed(fail, to, (unsigned)errno);
        ok = 0;
    }
    return ok;
}
#endif

/* A file copied over a save file that is there: whole or not at all. The
 * bytes go to "<to>.~new" (the save guard's name for a file on its way: a
 * listing does not show it, the next start removes one left behind), are
 * flushed to the disk, and the new file is renamed over the old one. */
static int sync_file(const char *path, CopyFail *fail)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (h == INVALID_HANDLE_VALUE) {
        copy_failed(fail, path, GetLastError());
        return 0;
    }
    ok = FlushFileBuffers(h) != 0;
    if (!ok)
        copy_failed(fail, path, GetLastError());
    CloseHandle(h);
    return ok;
#else
    int fd = open(path, O_RDONLY), ok;
    if (fd < 0) {
        copy_failed(fail, path, (unsigned)errno);
        return 0;
    }
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    ok = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
    ok = fsync(fd) == 0;
#endif
    if (!ok)
        copy_failed(fail, path, (unsigned)errno);
    close(fd);
    return ok;
#endif
}

static int replace_file(const char *from, const char *to, CopyFail *fail)
{
#if defined(_WIN32)
    int tries;
    DWORD e = 0;
    for (tries = 0; tries < 25; tries++) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 1;
        e = GetLastError();
        Sleep(20);
    }
    copy_failed(fail, to, e);
    return 0;
#else
    char dir[RS_PATH];
    char *slash;
    int fd;
    if (rename(from, to) != 0) {
        copy_failed(fail, to, (unsigned)errno);
        return 0;
    }
    snprintf(dir, sizeof dir, "%s", to);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        fd = open(dir, O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return 1;
#endif
}

static int copy_file_atomic(const char *from, const char *to, CopyFail *fail)
{
    char tmp[RS_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.~new", to);
    rs_unlink(tmp);
    if (copy_file(from, tmp, fail) && sync_file(tmp, fail) && replace_file(tmp, to, fail))
        return 1;
    rs_unlink(tmp);
    return 0;
}

typedef struct {
    const char *to;
    int skip_images;
    int atomic;
    int ok;
    CopyFail *fail;
} CopyCtx;

static int copy_tree_a(const char *from, const char *to, int skip_images, int atomic, CopyFail *fail);

static int copy_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    CopyCtx *c = (CopyCtx *)vctx;
    char src[RS_PATH], dst[RS_PATH];
    size_t n = strlen(name);

    snprintf(src, sizeof src, "%s/%s", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", c->to, name);
    /* A save file on its way to the disk (kernel_saveguard.h) is not a
     * save: the file beside it is. A pending one (".~pend") is kept by the
     * guard itself, and the copy of a save tree that is put in place here
     * would carry it over the save it waits for. */
    if (!dir_entry && ((n > 5 && !strcmp(name + n - 5, ".~new")) || (n > 6 && !strcmp(name + n - 6, ".~pend"))))
        return 1;
    if (dir_entry) {
        if (!copy_tree_a(src, dst, c->skip_images, c->atomic, c->fail))
            c->ok = 0;
    } else if (!(c->skip_images && n > 4 && !strcmp(name + n - 4, ".img"))) {
        if (!(c->atomic ? copy_file_atomic(src, dst, c->fail) : copy_file(src, dst, c->fail)))
            c->ok = 0;
    }
    return 1;
}

static int copy_tree_f(const char *from, const char *to, int skip_images, CopyFail *fail)
{
    return copy_tree_a(from, to, skip_images, 0, fail);
}

static int copy_tree_a(const char *from, const char *to, int skip_images, int atomic, CopyFail *fail)
{
    CopyCtx c;
    c.to = to;
    c.skip_images = skip_images;
    c.atomic = atomic;
    c.ok = 1;
    c.fail = fail;
    mkdir_p(to);
    for_each(from, copy_entry, &c);
    return c.ok;
}

static int copy_tree(const char *from, const char *to, int skip_images)
{
    CopyFail fail;
    fail.path[0] = 0;
    fail.code = 0;
    fail.waits = COPY_WAITS;
    return copy_tree_f(from, to, skip_images, &fail);
}

static int rm_entry(const char *dir, const char *name, int dir_entry, void *ctx);

static void rm_tree(const char *path)
{
    for_each(path, rm_entry, NULL);
    rs_rmdir(path);
}

static int rm_link_dir(const char *p);

static int rm_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    char p[RS_PATH];
    (void)ctx;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    /* A link to a folder is removed, never what it points to. */
    if (dir_entry && !rm_link_dir(p))
        rm_tree(p);
    else if (!dir_entry)
        rs_unlink(p);
    return 1;
}

/* 1 when `p` was a link (a symbolic link, a junction) and the link alone is
 * gone now. */
static int rm_link_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT))
        return 0;
    RemoveDirectoryA(p);            /* the junction, not its target */
    return 1;
#else
    struct stat st;
    if (lstat(p, &st) != 0 || !S_ISLNK(st.st_mode))
        return 0;
    unlink(p);
    return 1;
#endif
}

/* ── same contents ─────────────────────────────────────────────────── */

static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[8192], bb[8192];
    int same = fa && fb;

    while (same) {
        size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na))
            same = 0;
        else if (!na)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int count_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    (void)dir; (void)name; (void)dir_entry;
    (*(int *)ctx)++;
    return 1;
}

static int same_tree(const char *a, const char *b);

typedef struct { const char *other; } SameCtx;

static int same_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    SameCtx *c = (SameCtx *)vctx;
    char pa[RS_PATH], pb[RS_PATH];
    snprintf(pa, sizeof pa, "%s/%s", dir, name);
    snprintf(pb, sizeof pb, "%s/%s", c->other, name);
    if (dir_entry)
        return is_dir(pb) && same_tree(pa, pb);
    return !is_dir(pb) && same_file(pa, pb);
}

/* Both missing counts as the same. */
static int same_tree(const char *a, const char *b)
{
    int na = 0, nb = 0;
    SameCtx c;

    if (is_dir(a) != is_dir(b))
        return 0;
    if (!is_dir(a))
        return 1;
    for_each(a, count_entry, &na);
    for_each(b, count_entry, &nb);
    if (na != nb)
        return 0;
    c.other = b;
    return for_each(a, same_entry, &c);
}

/* ── the API ───────────────────────────────────────────────────────── */

int recomp_user_dir(int kind, const char *name, char *out, unsigned out_size)
{
    int w;
#if defined(_WIN32)
    const char *base = getenv(kind == RECOMP_DIR_DATA ? "APPDATA" : "LOCALAPPDATA");
    w = snprintf(out, out_size, kind == RECOMP_DIR_CACHE ? "%s/%s/cache" : "%s/%s",
                 base && *base ? base : ".", name);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    w = snprintf(out, out_size, "%s/Library/%s/%s", home && *home ? home : ".",
                 kind == RECOMP_DIR_CACHE ? "Caches" : "Application Support", name);
#else
    char lower[128];
    const char *xdg = getenv(kind == RECOMP_DIR_CACHE ? "XDG_CACHE_HOME" : "XDG_DATA_HOME");
    const char *home = getenv("HOME");
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof lower; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';
    if (xdg && *xdg)
        w = snprintf(out, out_size, "%s/%s", xdg, lower);
    else
        w = snprintf(out, out_size, "%s/%s/%s", home && *home ? home : ".",
                     kind == RECOMP_DIR_CACHE ? ".cache" : ".local/share", lower);
#endif
    return w > 0 && (unsigned)w < out_size;
}

const char *recomp_save_dir(const char *name)
{
    static char dir[RS_PATH];
    const char *ov;

    if (dir[0])
        return dir;
    ov = recomp_env(RENV_SAVE_DIR);
    if (ov && *ov)
        snprintf(dir, sizeof dir, "%s", ov);
    else
        recomp_user_dir(RECOMP_DIR_DATA, name, dir, sizeof dir);
    {
        size_t n = strlen(dir);
        while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
            dir[--n] = '\0';
    }
    mkdir_p(dir);
    return dir;
}

static int exists(const char *p)
{
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(p, &st) == 0;
#endif
}

/* A real folder, not a link to one: the old name left behind as a link
 * (off Windows) is not a folder waiting to be moved. */
static int is_real_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)
        && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* A folder with no file anywhere under it: empty, or only empty folders
 * (a cache folder made inside it by something that asked where it is). */
static int hollow_entry(const char *dir, const char *name, int dir_entry, void *ctx);

static int is_hollow_dir(const char *p)
{
    return is_real_dir(p) && for_each(p, hollow_entry, NULL);
}

static int hollow_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    char p[RS_PATH];
    (void)ctx;
    if (!dir_entry)
        return 0;                       /* a file: not hollow */
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return is_hollow_dir(p);            /* a link to a folder is not */
}

static int remove_hollow_entry(const char *dir, const char *name, int dir_entry, void *ctx);

/* Only ever folders, and only empty ones: a file that turns up stops it. */
static int remove_hollow_dir(const char *p)
{
    if (!for_each(p, remove_hollow_entry, NULL))
        return 0;
#if defined(_WIN32)
    return RemoveDirectoryA(p) != 0;
#else
    return rmdir(p) == 0;
#endif
}

static int remove_hollow_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    char p[RS_PATH];
    (void)ctx;
    if (!dir_entry)
        return 0;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return is_real_dir(p) && remove_hollow_dir(p);
}

int recomp_dir_migrate_pending(const char *old_dir, const char *new_dir)
{
    return is_real_dir(old_dir) && (!exists(new_dir) || is_hollow_dir(new_dir));
}

int recomp_dir_migrate(const char *old_dir, const char *new_dir)
{
    char parent[RS_PATH];
    size_t n = strlen(new_dir);

    if (!recomp_dir_migrate_pending(old_dir, new_dir) || n >= sizeof parent)
        return 0;
    memcpy(parent, new_dir, n + 1);
    while (n > 1 && parent[n - 1] != '/' && parent[n - 1] != '\\')
        n--;
    if (n > 1) {
        parent[n - 1] = '\0';
        mkdir_p(parent);
    }
    /* A folder of the new name with no file in it (something asked where it
     * is before the move and made it) gives way; one with a file never does.
     * rename does not cross to another disk. */
    if (exists(new_dir) && !remove_hollow_dir(new_dir))
        return -1;
    return rename(old_dir, new_dir) == 0 ? 1 : -1;
}

int recomp_save_dir_migrate(const char *legacy, const char *dir)
{
    char from[RS_PATH], to[RS_PATH];
    FILE *note;

    snprintf(from, sizeof from, "%s/UDATA", legacy);
    snprintf(to, sizeof to, "%s/UDATA", dir);
    /* Also stops a second copy, and RECOMP_SAVE_DIR=saves (one folder). */
    if (!is_dir(from) || is_dir(to))
        return 0;
    /* Copied before: a UDATA deleted since, for a first boot, stays so. */
    snprintf(from, sizeof from, "%s/MOVED.txt", legacy);
    note = fopen(from, "r");
    if (note) {
        fclose(note);
        return 0;
    }
    if (!copy_tree(legacy, dir, 1)) {
        char part[RS_PATH];
        XR_ERROR( "[SAVE] copying %s to %s failed part way; the old folder "
                        "is untouched and is copied again next start\n", legacy, dir);
        rm_tree(to);
        snprintf(part, sizeof part, "%s/TDATA", dir);
        rm_tree(part);
        return 0;
    }
    note = fopen(from, "w");
    if (note) {
        fprintf(note, "The saves in this folder were copied to\n  %s\n"
                      "and are read from there now. This folder is no longer "
                      "used and can be deleted.\n", dir);
        fclose(note);
    }
    return 1;
}

/* A name the program gave a dated folder: YYYYMMDD-HHMMSS, maybe "-N". */
static int stamped_name(const char *name)
{
    int i;
    for (i = 0; i < 15; i++)
        if (i == 8 ? name[i] != '-' : !isdigit((unsigned char)name[i]))
            return 0;
    if (!name[15])
        return 1;
    if (name[15] != '-' || !name[16])
        return 0;
    for (i = 16; name[i]; i++)
        if (!isdigit((unsigned char)name[i]) || i > 19)
            return 0;
    return 1;
}

static int collect_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    char (*names)[32] = (char (*)[32])((void **)vctx)[0];
    int *n = (int *)((void **)vctx)[1];
    (void)dir;
    if (dir_entry && strlen(name) < 32 && *n < 256)
        snprintf(names[(*n)++], 32, "%s", name);
    return 1;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int recomp_save_backup(const char *dir, int keep, char *out, unsigned out_size)
{
    static char names[256][32];
    static const char *const parts[] = { "UDATA", "TDATA" };
    char root[RS_PATH], path[RS_PATH], src[RS_PATH], stamp[32];
    void *ctx[2];
    int n = 0, i, k, same = 0, have = 0;

    if (out && out_size)
        out[0] = '\0';
    for (k = 0; k < 2; k++) {
        snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
        have |= is_dir(src);
    }
    if (!have)
        return 0;           /* nothing saved yet */
    snprintf(root, sizeof root, "%s/backup", dir);
    mkdir_p(root);
    ctx[0] = names;
    ctx[1] = &n;
    for_each(root, collect_entry, ctx);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);

    if (n) {
        same = 1;
        for (k = 0; k < 2 && same; k++) {
            snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
            snprintf(path, sizeof path, "%s/%s/%s", root, names[n - 1], parts[k]);
            same = same_tree(src, path);
        }
    }
    if (!same) {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        int ok = 1;
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", lt);
        snprintf(path, sizeof path, "%s/%s", root, stamp);
        if (is_dir(path))
            rm_tree(path);  /* two starts in one second: the later wins */
        for (k = 0; k < 2; k++) {
            char dst[RS_PATH];
            snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
            snprintf(dst, sizeof dst, "%s/%s", path, parts[k]);
            if (is_dir(src) && !copy_tree(src, dst, 0))
                ok = 0;
        }
        if (!ok) {
            XR_ERROR( "[SAVE] backup to %s failed; removed\n", path);
            rm_tree(path);
            return 0;
        }
        if (out && out_size)
            snprintf(out, out_size, "%s", path);
        /* Listed again below with the new one. */
        n = 0;
        for_each(root, collect_entry, ctx);
        qsort(names, (size_t)n, sizeof names[0], cmp_name);
    }
    for (i = 0; keep > 0 && i < n - keep; i++) {
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        /* Only what this made: a dated, real folder (never a link, nor a
         * folder the player put there under another name). */
        if (stamped_name(names[i]) && is_real_dir(path))
            rm_tree(path);
    }
    return !same;
}

/* ── export and import (recomp_save_dir.h) ─────────────────────────── */

#define SIO_NOTE     "save-export.txt"
#define SIO_STAGED   "save-import.txt"
#define SIO_RESULT   "save-import-result.txt"
#define SIO_BEFORE   "save-before-import"

static const char *const k_sio_parts[] = { "UDATA", "TDATA" };

int recomp_save_sandbox_begin(const char *root, const char *sandbox)
{
    char from[RS_PATH], to[RS_PATH];
    int ok = 1;
    unsigned i;

    rm_tree(sandbox);
    mkdir_p(sandbox);
    for (i = 0; i < 2; i++) {
        snprintf(from, sizeof from, "%s/%s", root, k_sio_parts[i]);
        snprintf(to, sizeof to, "%s/%s", sandbox, k_sio_parts[i]);
        if (is_dir(from) && !copy_tree(from, to, 1))
            ok = 0;
        else if (!is_dir(from))
            mkdir_p(to);
    }
    return ok;
}

/* Over the player's save: each file whole or not at all (copy_file_atomic);
 * the caller keeps a history copy of the save first. */
int recomp_save_sandbox_commit(const char *sandbox, const char *root)
{
    char from[RS_PATH], to[RS_PATH];
    int ok = 1;
    unsigned i;
    CopyFail fail;

    fail.path[0] = 0;
    fail.code = 0;
    fail.waits = COPY_WAITS;
    for (i = 0; i < 2; i++) {
        snprintf(from, sizeof from, "%s/%s", sandbox, k_sio_parts[i]);
        snprintf(to, sizeof to, "%s/%s", root, k_sio_parts[i]);
        if (is_dir(from) && !copy_tree_a(from, to, 1, 1, &fail))
            ok = 0;
    }
    if (!ok && fail.path[0])
        fprintf(stderr, "[SAVE] the cheat session's copy could not be put over the save: %s (error %u)\n",
                fail.path, fail.code);
    return ok;
}

void recomp_save_sandbox_discard(const char *sandbox)
{
    rm_tree(sandbox);
}

static void sio_err(char *err, unsigned n, const char *fmt, const char *a)
{
    if (err && n)
        snprintf(err, n, fmt, a ? a : "");
}

static void sio_stamp(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out, n, "%Y%m%d-%H%M%S", lt);
}

typedef struct { const char *want; char found[128]; } SioFind;

static int sio_find_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    SioFind *f = (SioFind *)vctx;
    size_t i;
    (void)dir;
    if (!dir_entry || strlen(name) != strlen(f->want) || strlen(name) >= sizeof f->found)
        return 1;
    for (i = 0; name[i]; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)f->want[i]))
            return 1;
    snprintf(f->found, sizeof f->found, "%s", name);
    return 1;
}

/* The folder `name` of `parent`, whatever its case on this disk (the title
 * names its folder; a copy made on another system keeps that spelling, a
 * hand-made one may not). 1 with the path in `out`. */
static int sio_child(const char *parent, const char *name, char *out, size_t n)
{
    SioFind f;
    f.want = name;
    f.found[0] = 0;
    snprintf(out, n, "%s/%s", parent, name);
    if (is_dir(out))
        return 1;
    for_each(parent, sio_find_entry, &f);
    if (!f.found[0])
        return 0;
    snprintf(out, n, "%s/%s", parent, f.found);
    return is_dir(out);
}

static int sio_count_files(const char *dir);

static int sio_count_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    char p[RS_PATH];
    if (dir_entry) {
        snprintf(p, sizeof p, "%s/%s", dir, name);
        *(int *)ctx += sio_count_files(p);
    } else {
        (*(int *)ctx)++;
    }
    return 1;
}

static int sio_count_files(const char *dir)
{
    int n = 0;
    for_each(dir, sio_count_entry, &n);
    return n;
}

/* root/<part>/<title id>, when it is there. */
static int sio_part(const char *root, int part, const char *title_id, char *out, size_t n)
{
    char p[RS_PATH];
    if (!sio_child(root, k_sio_parts[part], p, sizeof p))
        return 0;
    return sio_child(p, title_id, out, n);
}

int recomp_save_has_data(const char *dir, const char *title_id)
{
    char p[RS_PATH];
    return sio_part(dir, 0, title_id, p, sizeof p) && sio_count_files(p) > 0;
}

int recomp_save_export_check(const char *folder, const char *title_id, char *err, unsigned err_size)
{
    char p[RS_PATH], line[256];
    FILE *f;

    if (!folder || !*folder || !is_dir(folder)) {
        sio_err(err, err_size, "%s is not a folder that can be read.", folder);
        return 0;
    }
    snprintf(p, sizeof p, "%s/%s", folder, SIO_NOTE);
    f = fopen(p, "r");
    if (f) {
        /* An export says whose it is. */
        int other = 0;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "title ", 6)) {
                size_t i;
                line[strcspn(line, "\r\n")] = 0;
                for (i = 0; line[6 + i] && title_id[i]; i++)
                    if (tolower((unsigned char)line[6 + i]) != tolower((unsigned char)title_id[i]))
                        break;
                other = line[6 + i] || title_id[i];
            }
        fclose(f);
        if (other) {
            sio_err(err, err_size, "%s holds a save of another game.", folder);
            return 0;
        }
    }
    if (!sio_part(folder, 0, title_id, p, sizeof p) || sio_count_files(p) == 0) {
        sio_err(err, err_size, "%s is not a save of this game (no UDATA folder of it inside).", folder);
        return 0;
    }
    return 1;
}

int recomp_save_export(const char *dir, const char *title_id, const char *dest_root, const char *note,
                       char *out, unsigned out_size, char *err, unsigned err_size)
{
    char stamp[32], dest[RS_PATH], src[RS_PATH], dst[RS_PATH];
    CopyFail fail;
    FILE *f;
    int k, ok = 1, tries;

    fail.path[0] = 0;
    fail.code = 0;
    fail.waits = COPY_WAITS;
    if (out && out_size)
        out[0] = 0;
    if (!recomp_save_has_data(dir, title_id)) {
        sio_err(err, err_size, "There is no save yet: nothing to export.%s", "");
        return 0;
    }
    mkdir_p(dest_root);
    if (!is_dir(dest_root)) {
        sio_err(err, err_size, "The folder %s cannot be made.", dest_root);
        return 0;
    }
    sio_stamp(stamp, sizeof stamp);
    snprintf(dest, sizeof dest, "%s/%s", dest_root, stamp);
    for (tries = 2; is_dir(dest) && tries < 100; tries++)       /* two in one second */
        snprintf(dest, sizeof dest, "%s/%s-%d", dest_root, stamp, tries);
    if (rs_mkdir(dest) != 0 && !is_dir(dest)) {
        sio_err(err, err_size, "Nothing can be written in %s.", dest_root);
        return 0;
    }
    for (k = 0; k < 2 && ok; k++) {
        if (!sio_part(dir, k, title_id, src, sizeof src))
            continue;
        snprintf(dst, sizeof dst, "%s/%s/%s", dest, k_sio_parts[k], title_id);
        ok = copy_tree_f(src, dst, 0, &fail);
    }
    if (ok) {
        snprintf(dst, sizeof dst, "%s/%s", dest, SIO_NOTE);
        f = fopen(dst, "w");
        if (f) {
            fprintf(f, "title %s\nmade %s\n%s%s", title_id, stamp, note ? note : "",
                    note && *note && note[strlen(note) - 1] != '\n' ? "\n" : "");
            ok = fclose(f) == 0;
        } else {
            ok = 0;
        }
    }
    if (!ok || !recomp_save_export_check(dest, title_id, NULL, 0)) {
        rm_tree(dest);
        if (fail.path[0] && err && err_size)
            /* Which file, and the system's reason: a sharing violation
             * (Windows error 32) is another program holding it. */
#if defined(_WIN32)
            snprintf(err, err_size, "The save could not be copied to %s: %s could not be copied "
                     "(Windows error %u).", dest_root, fail.path, fail.code);
#else
            snprintf(err, err_size, "The save could not be copied to %s: %s could not be copied (%s).",
                     dest_root, fail.path, strerror((int)fail.code));
#endif
        else
            sio_err(err, err_size, "The save could not be copied to %s (is the disk full or read-only?).",
                    dest_root);
        return 0;
    }
    if (out && out_size)
        snprintf(out, out_size, "%s", dest);
    return 1;
}

int recomp_save_export_newest(const char *dest_root, const char *title_id, char *out, unsigned out_size)
{
    static char names[256][32];
    char path[RS_PATH];
    void *ctx[2];
    int n = 0, i;

    if (out && out_size)
        out[0] = 0;
    if (!is_dir(dest_root))
        return 0;
    ctx[0] = names;
    ctx[1] = &n;
    for_each(dest_root, collect_entry, ctx);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);
    for (i = n - 1; i >= 0; i--) {
        snprintf(path, sizeof path, "%s/%s", dest_root, names[i]);
        if (recomp_save_export_check(path, title_id, NULL, 0)) {
            if (out && out_size)
                snprintf(out, out_size, "%s", path);
            return 1;
        }
    }
    return 0;
}

int recomp_save_import_stage(const char *dir, const char *folder, const char *title_id,
                             char *err, unsigned err_size)
{
    char p[RS_PATH];
    FILE *f;

    if (!recomp_save_export_check(folder, title_id, err, err_size))
        return 0;
    mkdir_p(dir);
    snprintf(p, sizeof p, "%s/%s", dir, SIO_STAGED);
    f = fopen(p, "w");
    if (!f || fprintf(f, "%s\n", folder) < 0 || fclose(f) != 0) {
        sio_err(err, err_size, "The import could not be noted in %s.", dir);
        return 0;
    }
    return 1;
}

int recomp_save_import_staged(const char *dir, char *folder, unsigned size)
{
    char p[RS_PATH], line[RS_PATH] = "";
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", dir, SIO_STAGED);
    f = fopen(p, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f))
        line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (folder && size)
        snprintf(folder, size, "%s", line);
    return line[0] != 0;
}

void recomp_save_import_cancel(const char *dir)
{
    char p[RS_PATH];
    snprintf(p, sizeof p, "%s/%s", dir, SIO_STAGED);
    rs_unlink(p);
}

static void sio_result(const char *dir, const char *text)
{
    char p[RS_PATH];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", dir, SIO_RESULT);
    f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

int recomp_save_import_result(const char *dir, char *text, unsigned size)
{
    char p[RS_PATH];
    FILE *f;
    size_t n;

    if (!text || !size)
        return 0;
    text[0] = 0;
    snprintf(p, sizeof p, "%s/%s", dir, SIO_RESULT);
    f = fopen(p, "r");
    if (!f)
        return 0;
    n = fread(text, 1, size - 1, f);
    fclose(f);
    text[n] = 0;
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
        text[--n] = 0;
    return n > 0;
}

int recomp_save_import_apply(const char *dir, const char *title_id, char *report, unsigned size)
{
    char folder[RS_PATH], stamp[32], before[RS_PATH], cur[RS_PATH], src[RS_PATH], dst[RS_PATH];
    char text[RS_PATH * 3], why[RS_PATH + 128] = "";
    int k, ok = 1, had[2] = { 0, 0 };

    if (report && size)
        report[0] = 0;
    if (!recomp_save_import_staged(dir, folder, sizeof folder)) {
        /* Nothing asked for: the last import's message was for the start
         * that did it. */
        snprintf(cur, sizeof cur, "%s/%s", dir, SIO_RESULT);
        rs_unlink(cur);
        return 0;
    }
    /* Whatever happens next, it is tried once. */
    recomp_save_import_cancel(dir);
    if (!recomp_save_export_check(folder, title_id, why, sizeof why)) {
        snprintf(text, sizeof text, "The save was not imported: %s The save in use is unchanged.", why);
        sio_result(dir, text);
        if (report && size)
            snprintf(report, size, "%s", text);
        return -1;
    }

    /* The save in use, aside first. */
    sio_stamp(stamp, sizeof stamp);
    snprintf(before, sizeof before, "%s/%s/%s", dir, SIO_BEFORE, stamp);
    for (k = 0; k < 2 && ok; k++) {
        if (!sio_part(dir, k, title_id, cur, sizeof cur))
            continue;
        had[k] = 1;
        snprintf(dst, sizeof dst, "%s/%s/%s", before, k_sio_parts[k], title_id);
        ok = copy_tree(cur, dst, 0) && sio_count_files(dst) == sio_count_files(cur);
    }
    if (!ok) {
        rm_tree(before);
        snprintf(text, sizeof text, "The save was not imported: the save in use could not be copied "
                 "aside first (to %s/%s). It is unchanged.", dir, SIO_BEFORE);
        sio_result(dir, text);
        if (report && size)
            snprintf(report, size, "%s", text);
        return -1;
    }

    /* The export in its place. */
    for (k = 0; k < 2 && ok; k++) {
        snprintf(cur, sizeof cur, "%s/%s/%s", dir, k_sio_parts[k], title_id);
        if (had[k]) {
            char real[RS_PATH];
            if (sio_part(dir, k, title_id, real, sizeof real))
                rm_tree(real);
        }
        if (!sio_part(folder, k, title_id, src, sizeof src))
            continue;
        ok = copy_tree(src, cur, 0) && sio_count_files(cur) == sio_count_files(src);
    }
    if (!ok) {
        /* Back to what it was. */
        int back = 1;
        for (k = 0; k < 2; k++) {
            snprintf(cur, sizeof cur, "%s/%s/%s", dir, k_sio_parts[k], title_id);
            if (is_dir(cur))
                rm_tree(cur);
            if (!had[k])
                continue;
            snprintf(src, sizeof src, "%s/%s/%s", before, k_sio_parts[k], title_id);
            if (!copy_tree(src, cur, 0))
                back = 0;
        }
        snprintf(text, sizeof text, back
                 ? "The save was not imported: %s could not be copied. The save in use was put back."
                 : "The save was not imported: %s could not be copied, and the save in use could "
                   "not be put back either: its copy is in the save-before-import folder.", folder);
        sio_result(dir, text);
        if (report && size)
            snprintf(report, size, "%s", text);
        return -1;
    }
    if (had[0] || had[1])
        snprintf(text, sizeof text, "Imported the save from %s. The save that was here is in %s.",
                 folder, before);
    else
        snprintf(text, sizeof text, "Imported the save from %s (there was no save here before).", folder);
    sio_result(dir, text);
    if (report && size)
        snprintf(report, size, "%s", text);
    return 1;
}

/* ── the save history (recomp_save_dir.h) ──────────────────────────── */

#define SH_DIR "save-history"

static time_t stamp_time(const char *name)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!stamped_name(name))
        return (time_t)-1;
    if (sscanf(name, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return (time_t)-1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void recomp_save_history_keep(const char *const *names, int n, time_t now, int *keep)
{
    int i, b, rank = 0;
    time_t day_seen[RECOMP_SAVE_HISTORY_DAYS], week_seen[RECOMP_SAVE_HISTORY_WEEKS];

    for (i = 0; i < n; i++)
        keep[i] = 0;
    for (b = 0; b < RECOMP_SAVE_HISTORY_DAYS; b++)
        day_seen[b] = 0;
    for (b = 0; b < RECOMP_SAVE_HISTORY_WEEKS; b++)
        week_seen[b] = 0;
    /* Newest first: the newest of each day and each week is the one kept. */
    for (i = n - 1; i >= 0; i--) {
        time_t t = stamp_time(names[i]);
        long age;
        if (t == (time_t)-1) {
            keep[i] = 1;                /* not one of ours: never touched */
            continue;
        }
        if (rank++ < RECOMP_SAVE_HISTORY_NEWEST)
            keep[i] = 1;
        age = t <= now ? (long)((now - t) / 86400) : 0;
        if (age < RECOMP_SAVE_HISTORY_DAYS && !day_seen[age]) {
            day_seen[age] = 1;
            keep[i] = 1;
        }
        if (age / 7 < RECOMP_SAVE_HISTORY_WEEKS && !week_seen[age / 7]) {
            week_seen[age / 7] = 1;
            keep[i] = 1;
        }
    }
}

int recomp_save_history_prune(const char *dir, time_t now)
{
    static char names[256][32];
    const char *list[256];
    int keep[256];
    char root[RS_PATH], path[RS_PATH];
    void *ctx[2];
    int n = 0, i, gone = 0;

    snprintf(root, sizeof root, "%s/%s", dir, SH_DIR);
    if (!is_real_dir(root))
        return 0;
    ctx[0] = names;
    ctx[1] = &n;
    for_each(root, collect_entry, ctx);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);
    for (i = 0; i < n; i++)
        list[i] = names[i];
    recomp_save_history_keep(list, n, now, keep);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        if (!keep[i] && stamped_name(names[i]) && is_real_dir(path)) {
            rm_tree(path);
            gone++;
        }
    }
    return gone;
}

int recomp_save_history_take(const char *dir, const char *title_id, char *out, unsigned out_size)
{
    char root[RS_PATH], err[2 * RS_PATH + 160];
    int ok;

    snprintf(root, sizeof root, "%s/%s", dir, SH_DIR);
    ok = recomp_save_export(dir, title_id, root, "kept before the game saved over it", out, out_size,
                            err, sizeof err);
    if (!ok && recomp_save_has_data(dir, title_id))
        fprintf(stderr, "[SAVE] the save could not be kept in the history: %s\n", err);
    recomp_save_history_prune(dir, time(NULL));
    return ok;
}

int recomp_save_history_dir(const char *dir, char *out, unsigned size)
{
    snprintf(out, size, "%s/%s", dir, SH_DIR);
    return is_dir(out);
}

int recomp_save_history_count(const char *dir, const char *title_id)
{
    static char names[256][32];
    char root[RS_PATH], path[RS_PATH];
    void *ctx[2];
    int n = 0, i, c = 0;

    snprintf(root, sizeof root, "%s/%s", dir, SH_DIR);
    if (!is_dir(root))
        return 0;
    ctx[0] = names;
    ctx[1] = &n;
    for_each(root, collect_entry, ctx);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        c += recomp_save_export_check(path, title_id, NULL, 0);
    }
    return c;
}

int recomp_home_dir(char *out, unsigned size)
{
    const char *h;
#if defined(__ANDROID__)
    /* An app has no home folder a file manager shows; its own external
     * files directory is what it may write without asking. */
    h = getenv("RECOMP_ANDROID_DIR");
    if (!h || !*h)
        h = getenv("XDG_DATA_HOME");
#elif defined(_WIN32)
    h = getenv("USERPROFILE");
#else
    h = getenv("HOME");
#endif
    if (!h || !*h || !out || !size)
        return 0;
    snprintf(out, size, "%s", h);
    return 1;
}


/* ── file helpers for the Xbox save importer (kernel_save_import.c) ──── */

int recomp_fs_is_dir(const char *path) { return path && is_dir(path); }
void recomp_fs_mkdir_p(const char *path) { mkdir_p(path); }
void recomp_fs_rm_tree(const char *path) { if (path && is_dir(path)) rm_tree(path); }
int recomp_fs_copy_tree(const char *from, const char *to) { return copy_tree(from, to, 0); }
int recomp_fs_each(const char *dir, int (*fn)(const char *dir, const char *name, int dir_entry, void *ctx),
                   void *ctx)
{
    return for_each(dir, fn, ctx);
}

/*
 * xst_browse.c - a folder browser's model for choosing the music folder.
 * See xbox_soundtrack.h.
 *
 * The places are those of the setup window's disc browser of the Burnout 3
 * port (home, Downloads, /run/media/<user>/<label> and /run/media/<label>,
 * /media, /mnt), with Music first and the other hosts' mounts.
 */
#include "xbox_soundtrack.h"
#include "xst_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION xsb_mutex;
#define LOCK()    EnterCriticalSection(&s_mu)
#define UNLOCK()  LeaveCriticalSection(&s_mu)
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#elif defined(__linux__)
#include <sys/vfs.h>
#endif
typedef pthread_mutex_t xsb_mutex;
#define LOCK()    pthread_mutex_lock(&s_mu)
#define UNLOCK()  pthread_mutex_unlock(&s_mu)
#endif

#if defined(_MSC_VER)
#define XSB_TLS __declspec(thread)
#else
#define XSB_TLS __thread
#endif

static xsb_mutex s_mu;
static int       s_mu_ready;

static xsb_row  *s_rows;                  /* XSB_MAX_ROWS */
static int       s_n;
static char      s_dir[1024];             /* "" on the places */
static char      s_root[1024];            /* the place that was entered; "" climbs to "/" */
static char      s_current[1024];
static int       s_open;
/* Bumped at every new list: the counting thread drops what it was doing. */
static volatile int s_gen;
/* Bumped at every close (BO-289): a counting thread of an older opening
 * leaves on its own when it next looks, and never waits to be joined; one
 * held by a slow disk (a network share) cannot hold the closing. */
static volatile int s_session;
static volatile int s_workers;            /* counting threads not yet gone */
static int       s_thread_on;
/* The counting thread's own opening and list, for xsb_stopping. */
static XSB_TLS int t_session = -1, t_gen;
static void    (*s_dir_hook)(const char *dir);

/* What this opening of the browser is for (xsb_open_with). */
static char      s_use_label[80] = "Use this folder";
static char      s_off_label[80] = "Turn custom music off";
static char      s_top_label[80], s_top_path[1024];
static int     (*s_mark)(const char *dir);

static const char *s_test_home;
static const char *const *s_test_media;
static int       s_test_roots;
static const char *s_test_slow;

/* ── small things ──────────────────────────────────────────────────── */

static int is_dir(const char *path)
{
#if defined(_WIN32)
    wchar_t w[1024];
    DWORD a;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0)
        return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Whether the folder can be listed (opened for reading). */
static int can_list(const char *path)
{
#if defined(_WIN32)
    (void)path;
    return 1;                             /* GetFileAttributesW said it is a folder */
#else
    DIR *d = opendir(path);
    if (!d)
        return 0;
    closedir(d);
    return 1;
#endif
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

static const char *leaf(const char *path)
{
    const char *s = path + strlen(path);
    while (s > path && is_sep(s[-1]))
        s--;
    while (s > path && !is_sep(s[-1]))
        s--;
    return s;
}

/* The folder above `path`, 0 when there is none ("/", "C:\"). */
static int parent_of(const char *path, char *out, size_t cap)
{
    size_t n = strlen(path);

    while (n > 1 && is_sep(path[n - 1]))
        n--;
    while (n > 0 && !is_sep(path[n - 1]))
        n--;
    if (n == 0)
        return 0;
    /* Keep a root's separator: "/" and "C:\". */
    if (n > 1 && !(n == 3 && path[1] == ':'))
        n--;
    if (n >= cap || n == strlen(path))
        return 0;
    memcpy(out, path, n);
    out[n] = 0;
    return 1;
}

static int same_path(const char *a, const char *b);

/* `dir` is the folder `path` is in. */
static int is_parent_of(const char *dir, const char *path)
{
    char up[1024];
    return parent_of(path, up, sizeof up) && same_path(up, dir);
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, n && is_sep(dir[n - 1]) ? "" : "/", name);
}

static int same_path(const char *a, const char *b)
{
    size_t x = strlen(a), y = strlen(b);
    while (x > 1 && is_sep(a[x - 1])) x--;
    while (y > 1 && is_sep(b[y - 1])) y--;
    return x == y && !strncmp(a, b, x);
}

/* ── counting ──────────────────────────────────────────────────────── */

/* What a count may look at: entries, a time, and whether it ran out or
 * left a folder unwalked (`more` either way). */
typedef struct {
    int      entries;
    uint64_t deadline;                    /* ms */
    int      out;
#if !defined(_WIN32)
    dev_t    dev;                         /* the folder above: a new device is asked about */
#endif
} xsb_budget;

static uint64_t now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* One more entry; 1 when the count has to stop (budget, time, or the
 * browser closed or moved on). */
static int spent(xsb_budget *b)
{
    if (xsb_stopping())
        return 1;
    if (!b->out && (--b->entries < 0 || now_ms() >= b->deadline))
        b->out = 1;
    return b->out;
}

static int count_dir(const char *dir, int depth, xsb_budget *budget);

/* Playable files in and below `dir`; folders on a network share are
 * counted, not walked (BO-289). */
static int count_tree(const char *dir, int depth, xsb_budget *budget)
{
    int n = 0;
    if (spent(budget))
        return 0;
    if (s_dir_hook)
        s_dir_hook(dir);
    if (xsb_stopping())
        return 0;
    {
        /* Asked where a walk starts and where it crosses onto another
         * mount (POSIX); on Windows no link is followed, so the drive the
         * row is on is the drive of every folder below it. */
        int ask = depth == 0 || s_test_slow;
#if !defined(_WIN32)
        struct stat ds;
        dev_t above = budget->dev;
        if (stat(dir, &ds) == 0) {
            ask |= ds.st_dev != above;
            budget->dev = ds.st_dev;
        }
#endif
        if (ask && xsb_slow_volume(dir))
            depth = XST_MAX_DEPTH + 1;    /* its own files only */
        n = count_dir(dir, depth, budget);
#if !defined(_WIN32)
        budget->dev = above;
#endif
    }
    return n;
}

static int count_dir(const char *dir, int depth, xsb_budget *budget)
{
    int n = 0;
#if defined(_WIN32)
    wchar_t pat[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, pat, 1000) <= 0)
        return 0;
    wcscat(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char name[780], path[1024];
        if (spent(budget))
            break;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        if (name[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* As a scan reads: no link to a folder is followed. */
            if (depth > XST_MAX_DEPTH) {
                budget->out = 1;          /* not walked: more than counted */
            } else if (depth < XST_MAX_DEPTH && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                join(path, sizeof path, dir, name);
                n += count_tree(path, depth + 1, budget);
            }
        } else if (xst_is_playable_name(name)) {
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        int dirent_is_dir;
        if (spent(budget))
            break;
        if (e->d_name[0] == '.')
            continue;
#if defined(DT_DIR)
        if (e->d_type == DT_REG) {
            n += xst_is_playable_name(e->d_name);
            continue;
        }
        dirent_is_dir = e->d_type == DT_DIR;
        if (e->d_type != DT_DIR && e->d_type != DT_LNK && e->d_type != DT_UNKNOWN)
            continue;
#else
        dirent_is_dir = 0;
#endif
        join(path, sizeof path, dir, e->d_name);
        if (!dirent_is_dir) {
            if (stat(path, &st) != 0)
                continue;
            if (S_ISREG(st.st_mode)) {
                n += xst_is_playable_name(e->d_name);
                continue;
            }
            if (!S_ISDIR(st.st_mode))
                continue;
            /* As a scan reads: no link to a folder is followed. */
            if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
                continue;
        }
        if (depth > XST_MAX_DEPTH)
            budget->out = 1;              /* not walked: more than counted */
        else if (depth < XST_MAX_DEPTH)
            n += count_tree(path, depth + 1, budget);
    }
    closedir(d);
#endif
    return n;
}

static void nap(void)
{
#if defined(_WIN32)
    Sleep(15);
#else
    usleep(15000);
#endif
}

#if defined(_WIN32)
static DWORD WINAPI count_thread(LPVOID arg)
#else
static void *count_thread(void *arg)
#endif
{
    t_session = (int)(intptr_t)arg;
    for (;;) {
        char path[1024];
        int i, found = -1, n, more;
        int (*mark)(const char *dir);
        xsb_budget budget;

        LOCK();
        if (t_session != s_session) {
            s_workers--;                  /* closed: gone without a join */
            UNLOCK();
            break;
        }
        t_gen = s_gen;
        mark = s_mark;
        for (i = 0; i < s_n; i++)
            if (s_rows[i].songs < 0 && s_rows[i].path[0]
                && (s_rows[i].kind == XSB_USE || s_rows[i].kind == XSB_DIR
                    || s_rows[i].kind == XSB_PLACE)) {
                found = i;
                snprintf(path, sizeof path, "%s", s_rows[i].path);
                break;
            }
        UNLOCK();
        if (found < 0) {
            nap();
            continue;
        }
        /* Music: the playable files in and below it. Another purpose: its
         * own yes or no for the folder itself (below 0: not known). */
        budget.entries = XSB_COUNT_BUDGET;
        budget.deadline = now_ms() + XSB_COUNT_MS;
        budget.out = 0;
        if (mark) {
            int m = mark(path);
            n = m > 0;
            more = m < 0;
        } else {
#if !defined(_WIN32)
            budget.dev = 0;
#endif
            n = count_tree(path, 0, &budget);
            more = budget.out;
        }
        LOCK();
        if (!xsb_stopping() && found < s_n) {
            s_rows[found].songs = n;
            s_rows[found].more = more;
        }
        UNLOCK();
    }
    return 0;
}

int xsb_stopping(void)
{
    return t_session != s_session || t_gen != s_gen;
}

/* ── slow volumes ──────────────────────────────────────────────────── */

#if defined(__linux__) && !defined(__ANDROID__)
/* The type /proc/self/mounts gives the mount `path` is on (the longest
 * mount point that holds it). */
static int mount_type(const char *path, char *type, size_t cap)
{
    FILE *f = fopen("/proc/self/mounts", "r");
    char line[2048], dev[1024], mnt[1024], fs[128];
    size_t best = 0;
    if (!f)
        return 0;
    type[0] = 0;
    while (fgets(line, sizeof line, f)) {
        size_t m;
        if (sscanf(line, "%1023s %1023s %127s", dev, mnt, fs) != 3)
            continue;
        m = strlen(mnt);
        if (strstr(mnt, "\\"))
            continue;                     /* an escaped name: not worth decoding */
        if (m >= best && !strncmp(path, mnt, m) && (m == 1 || path[m] == '/' || path[m] == 0)) {
            best = m;
            snprintf(type, cap, "%s", fs);
        }
    }
    fclose(f);
    return type[0] != 0;
}
#endif

/* Whether `path` is on a network share (or a user-space filesystem that
 * usually is one): such a folder is counted or checked by its own entries,
 * never walked (BO-289). Asked on the browser's thread, never the UI's: a
 * share that does not answer holds this call too. */
int xsb_slow_volume(const char *path)
{
    if (s_test_slow && path && strstr(path, s_test_slow))
        return 1;
#if defined(_WIN32)
    {
        wchar_t w[1024], root[1024];
        if (!path || (is_sep(path[0]) && is_sep(path[1])))
            return path != NULL;          /* \\server\share */
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0)
            return 0;
        if (!GetVolumePathNameW(w, root, 1024))
            return 0;
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
#elif defined(__APPLE__)
    {
        static const char *const net[] = { "smbfs", "nfs", "afpfs", "webdav", "cifs", "ftp",
                                           "macfuse", "osxfuse", "fusefs", NULL };
        struct statfs st;
        int i;
        if (!path || statfs(path, &st) != 0)
            return 0;
        if (!(st.f_flags & MNT_LOCAL))
            return 1;
        for (i = 0; net[i]; i++)
            if (!strncmp(st.f_fstypename, net[i], strlen(net[i])))
                return 1;
        return 0;
    }
#elif defined(__linux__)
    {
        struct statfs st;
        unsigned long t;
        if (!path || statfs(path, &st) != 0)
            return 0;
        t = (unsigned long)(unsigned)st.f_type;
        switch (t) {
        case 0x6969UL:                    /* NFS */
        case 0x517BUL:                    /* SMB */
        case 0xFF534D42UL:                /* CIFS */
        case 0xFE534D42UL:                /* SMB2 */
        case 0x73757245UL:                /* Coda (davfs2) */
        case 0x5346414FUL:                /* AFS */
        case 0x00C36400UL:                /* Ceph */
        case 0x01021997UL:                /* 9p */
            return 1;
        case 0x65735546UL:                /* FUSE */
#if defined(__ANDROID__)
            return 0;                     /* shared storage itself is FUSE there */
#else
        {
            /* The FUSE filesystems that reach over the network (sshfs,
             * rclone, gvfs, ...); a disk through FUSE (ntfs-3g, exfat:
             * "fuseblk") or a pool of disks (mergerfs) is local. */
            static const char *const net[] = { "sshfs", "rclone", "gvfsd-fuse", "s3fs", "smbnetfs",
                                               "curlftpfs", "davfs", "gcsfuse", "goofys", "onedriver",
                                               "google-drive", "httpdirfs", "afuse", NULL };
            char type[128];
            int i;
            if (!mount_type(path, type, sizeof type))
                return 1;                 /* not known: not walked */
            if (strncmp(type, "fuse.", 5))
                return 0;
            for (i = 0; net[i]; i++)
                if (!strncmp(type + 5, net[i], strlen(net[i])))
                    return 1;
            return 0;
        }
#endif
        }
        return 0;
    }
#else
    (void)path;
    return 0;
#endif
}

/* ── the lists ─────────────────────────────────────────────────────── */

static void add_row(xsb_kind kind, const char *label, const char *path)
{
    xsb_row *r;
    int i;

    if (s_n == XSB_MAX_ROWS)
        return;
    if (kind == XSB_PLACE) {
        /* A place that is not there (no Downloads, no card) or cannot be
         * listed (no permission) is left out, not shown as a dead row. */
        if (!is_dir(path) || !can_list(path))
            return;
        for (i = 0; i < s_n; i++)
            if (s_rows[i].kind == XSB_PLACE && same_path(s_rows[i].path, path))
                return;
    }
    r = &s_rows[s_n++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    /* The label is for a screen: composed. The path stays as the disk has it. */
    xst_utf8_compose(r->label, sizeof r->label, label);
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");
    r->songs = (kind == XSB_OFF || kind == XSB_UP) ? 0 : -1;
}

/* Every folder of `root` as a drive; with `user`, <root>/<user>/<label>
 * (how udisks mounts a card) rather than the user's folder itself. */
static void add_mounts(const char *root, const char *user)
{
    xst_names dirs = { 0 }, sub = { 0 };
    char path[1024], label[200];
    int i, k;

    if (!xst_list_dir(root, NULL, &dirs))
        return;
    for (i = 0; i < dirs.n; i++) {
        join(path, sizeof path, root, dirs.v[i]);
        if (user && *user && !strcmp(dirs.v[i], user)) {
            if (xst_list_dir(path, NULL, &sub)) {
                for (k = 0; k < sub.n; k++) {
                    char p2[1024];
                    join(p2, sizeof p2, path, sub.v[k]);
                    snprintf(label, sizeof label, "Drive: %s", sub.v[k]);
                    add_row(XSB_PLACE, label, p2);
                }
                xst_names_free(&sub);
            }
            continue;
        }
        snprintf(label, sizeof label, "Drive: %s", dirs.v[i]);
        add_row(XSB_PLACE, label, path);
    }
    xst_names_free(&dirs);
}

static void list_places(void)
{
    char path[1024], label[1100];
    const char *home;

    s_n = 0;
    s_dir[0] = s_root[0] = 0;
    if (s_current[0]) {
        if (s_off_label[0])
            add_row(XSB_OFF, s_off_label, "");
        snprintf(label, sizeof label, "Current folder: %s", leaf(s_current));
        add_row(XSB_PLACE, label, s_current);
        /* And the one it is in, one step away (BO-243: a folder of
         * soundtracks one level up is the usual next choice). */
        if (parent_of(s_current, path, sizeof path)) {
            snprintf(label, sizeof label, "The folder it is in: %s", leaf(path));
            add_row(XSB_PLACE, label, path);
        }
    }
    if (s_top_path[0])
        add_row(XSB_PLACE, s_top_label, s_top_path);
    if (s_test_roots) {
        home = s_test_home;
    } else {
#if defined(_WIN32)
        /* Wide, then UTF-8: the narrow environment is in the ANSI code page. */
        static char home_utf8[1024];
        const wchar_t *w = _wgetenv(L"USERPROFILE");
        home = w && WideCharToMultiByte(CP_UTF8, 0, w, -1, home_utf8, sizeof home_utf8, NULL, NULL) > 0
             ? home_utf8 : NULL;
#elif defined(__ANDROID__)
        home = NULL;
#else
        home = getenv("HOME");
#endif
    }
    if (home && *home) {
        join(path, sizeof path, home, "Music");
        add_row(XSB_PLACE, "Music", path);
        add_row(XSB_PLACE, "Home", home);
        join(path, sizeof path, home, "Downloads");
        add_row(XSB_PLACE, "Downloads", path);
    }
    if (s_test_roots) {
        int i;
        for (i = 0; s_test_media && s_test_media[i]; i++)
            add_mounts(s_test_media[i], home && *home ? leaf(home) : NULL);
        return;
    }
#if defined(_WIN32)
    {
        DWORD mask = GetLogicalDrives();
        int d;
        for (d = 0; d < 26; d++)
            if (mask & (1u << d)) {
                snprintf(path, sizeof path, "%c:\\", 'A' + d);
                snprintf(label, sizeof label, "Drive %c:", 'A' + d);
                add_row(XSB_PLACE, label, path);
            }
    }
#elif defined(__APPLE__)
    add_mounts("/Volumes", NULL);
#elif defined(__ANDROID__)
    /* The app's own files folder first: the one place it can read without
     * a permission, and one a PC reaches over USB: <files>/Music. */
    {
        const char *own = getenv("RECOMP_ANDROID_DIR");
        if (own && *own) {
            join(path, sizeof path, own, "Music");
            mkdir(path, 0775);
            add_row(XSB_PLACE, "This app's Music folder", path);
        }
    }
    /* Shared storage, readable as files once the player has given the app
     * the audio permission (READ_MEDIA_AUDIO, asked for by the port's
     * access hook when the row is opened); a card is /storage/<id>. */
    add_row(XSB_PLACE, "Music", "/storage/emulated/0/Music");
    add_row(XSB_PLACE, "Downloads", "/storage/emulated/0/Download");
    add_row(XSB_PLACE, "Internal storage", "/storage/emulated/0");
    {
        xst_names dirs = { 0 };
        int i;
        if (xst_list_dir("/storage", NULL, &dirs)) {
            for (i = 0; i < dirs.n; i++) {
                if (!strcmp(dirs.v[i], "emulated") || !strcmp(dirs.v[i], "self"))
                    continue;
                join(path, sizeof path, "/storage", dirs.v[i]);
                snprintf(label, sizeof label, "Card: %s", dirs.v[i]);
                add_row(XSB_PLACE, label, path);
            }
            xst_names_free(&dirs);
        }
    }
#else
    {
        const char *user = home && *home ? leaf(home) : NULL;
        add_mounts("/run/media", user);
        add_mounts("/media", user);
        add_mounts("/mnt", NULL);
    }
#endif
#if !defined(_WIN32) && !defined(__ANDROID__)
    add_row(XSB_PLACE, "Computer: /", "/");
#endif
}

static int s_newest_first;

/* The rows of `dir`; 0 (and nothing changed) when it cannot be read. */
static int list_folder(const char *dir)
{
    xst_names dirs = { 0 };
    char path[1024];
    int i;

    if (!xst_list_dir(dir, NULL, &dirs))
        return 0;
    s_n = 0;
    snprintf(path, sizeof path, "%s", dir);
    snprintf(s_dir, sizeof s_dir, "%s", path);
    add_row(XSB_USE, s_use_label, s_dir);
    add_row(XSB_UP, "..", "");
    for (i = 0; i < dirs.n; i++) {
        int k = s_newest_first ? dirs.n - 1 - i : i;
        join(path, sizeof path, s_dir, dirs.v[k]);
        if (!can_list(path))
            continue;                     /* one that would not open: no dead row */
        add_row(XSB_DIR, dirs.v[k], path);
    }
    xst_names_free(&dirs);
    return 1;
}

/* ── the interface ─────────────────────────────────────────────────── */

static void xsb_open_places(const char *current);

void xsb_set_test_hooks(void (*dir_hook)(const char *dir), const char *slow_part)
{
    s_dir_hook = dir_hook;
    s_test_slow = slow_part;
}

int xsb_workers(void)
{
    int n;
    if (!s_mu_ready)
        return 0;
    LOCK();
    n = s_workers;
    UNLOCK();
    return n;
}

void xsb_set_roots(const char *home, const char *const *media_roots)
{
    s_test_home = home;
    s_test_media = media_roots;
    s_test_roots = home || media_roots;
}

void xsb_open_with(const xsb_options *o)
{
    xsb_close();
    snprintf(s_use_label, sizeof s_use_label, "%s", o && o->use_label ? o->use_label : "Use this folder");
    snprintf(s_off_label, sizeof s_off_label, "%s", o && o->off_label ? o->off_label : "");
    snprintf(s_top_label, sizeof s_top_label, "%s", o && o->top_label ? o->top_label : "");
    snprintf(s_top_path, sizeof s_top_path, "%s", o && o->top_path ? o->top_path : "");
    s_mark = o ? o->mark : NULL;
    s_newest_first = o ? o->newest_first : 0;
    xsb_open_places(o ? o->current : NULL);
    if (o && o->start_in && *o->start_in && s_open) {
        LOCK();
        if (list_folder(o->start_in))
            s_gen++;
        UNLOCK();
    }
}

void xsb_open(const char *current)
{
    xsb_options o;
    memset(&o, 0, sizeof o);
    o.current = current;
    o.off_label = "Turn custom music off";
    xsb_open_with(&o);
}

static void xsb_open_places(const char *current)
{
    if (!s_mu_ready) {
#if defined(_WIN32)
        InitializeCriticalSection(&s_mu);
#else
        pthread_mutex_init(&s_mu, NULL);
#endif
        s_mu_ready = 1;
    }
    if (!s_rows)
        s_rows = (xsb_row *)calloc(XSB_MAX_ROWS, sizeof *s_rows);
    if (!s_rows)
        return;
    LOCK();
    snprintf(s_current, sizeof s_current, "%s", current ? current : "");
    s_gen++;
    list_places();
    s_open = 1;
    UNLOCK();
    if (!s_thread_on) {
        /* Detached: a close never waits for it (BO-289). */
        void *arg = (void *)(intptr_t)s_session;
        LOCK();
        s_workers++;
        UNLOCK();
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 0, count_thread, arg, 0, NULL);
        s_thread_on = h != NULL;
        if (h)
            CloseHandle(h);
#else
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        s_thread_on = pthread_create(&t, &at, count_thread, arg) == 0;
        pthread_attr_destroy(&at);
#endif
        if (!s_thread_on) {
            LOCK();
            s_workers--;
            UNLOCK();
        }
    }
}

void xsb_close(void)
{
    if (!s_open)
        return;
    /* No join: the thread may sit in a read of a share that does not
     * answer. It sees the new session and leaves when it next looks; what
     * it was counting is dropped (s_gen). A new opening starts its own. */
    LOCK();
    s_session++;
    s_thread_on = 0;
    s_open = 0;
    s_n = 0;
    s_gen++;
    UNLOCK();
}

int xsb_is_open(void) { return s_open; }

int xsb_rows(void)
{
    return s_open ? s_n : 0;
}

int xsb_row_at(int i, xsb_row *out)
{
    int ok = 0;
    if (!s_open)
        return 0;
    LOCK();
    if (i >= 0 && i < s_n) {
        *out = s_rows[i];
        ok = 1;
    }
    UNLOCK();
    return ok;
}

void xsb_where(char *out, size_t cap)
{
    if (!cap)
        return;
    if (!s_open) {
        out[0] = 0;
        return;
    }
    LOCK();
    snprintf(out, cap, "%s", s_dir);
    UNLOCK();
}

/* The row whose path is `path`, else the first folder row, else 0. */
static int row_of(const char *path)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (s_rows[i].path[0] && s_rows[i].kind != XSB_USE && same_path(s_rows[i].path, path))
            return i;
    return 0;
}

xsb_result xsb_back(int *select)
{
    char from[1024], up[1024];
    xsb_result res = XSB_MOVED;

    if (select)
        *select = 0;
    if (!s_open)
        return XSB_CLOSED;
    LOCK();
    if (!s_dir[0]) {
        UNLOCK();
        return XSB_CLOSED;
    }
    snprintf(from, sizeof from, "%s", s_dir);
    s_gen++;
    if (same_path(s_dir, s_root) || !parent_of(s_dir, up, sizeof up) || !list_folder(up)) {
        /* Out of the place that was entered: the places again. */
        char root[1024];
        snprintf(root, sizeof root, "%s", s_root);
        list_places();
        if (select)
            *select = row_of(root);
    } else if (select) {
        *select = row_of(from);
    }
    UNLOCK();
    return res;
}

xsb_result xsb_activate(int i, char *chosen, size_t cap, int *select)
{
    xsb_row r;
    xsb_result res = XSB_NOTHING;

    if (select)
        *select = 0;
    if (!xsb_row_at(i, &r))
        return XSB_NOTHING;
    switch (r.kind) {
    case XSB_OFF:
        return XSB_TURNED_OFF;
    case XSB_USE:
        if (chosen && cap)
            snprintf(chosen, cap, "%s", r.path);
        return XSB_CHOSEN;
    case XSB_UP:
        return xsb_back(select);
    case XSB_PLACE:
    case XSB_DIR:
        LOCK();
        s_gen++;
        if (list_folder(r.path)) {
            int near_current = r.kind == XSB_PLACE && s_current[0]
                               && (same_path(r.path, s_current) || is_parent_of(r.path, s_current));
            /* The folder in use and the one it is in are no places of
             * their own: ".." goes on up from them (BO-243). */
            if (r.kind == XSB_PLACE)
                snprintf(s_root, sizeof s_root, "%s", near_current ? "" : r.path);
            res = XSB_MOVED;
            /* On the first folder when there is one, else on "use"; in the
             * folder in use, on "..". */
            if (select)
                *select = near_current && same_path(r.path, s_current) ? 1 : s_n > 2 ? 2 : 0;
        }
        UNLOCK();
        return res;
    }
    return res;
}

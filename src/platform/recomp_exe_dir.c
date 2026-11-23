/*
 * recomp_exe_dir.c - see recomp_exe_dir.h.
 *
 * macOS: _NSGetExecutablePath, then realpath (it may name a symlink).
 * Windows: GetModuleFileNameA (under Wine a Z:\ path, which fopen takes).
 * Linux and other POSIX: readlink("/proc/self/exe").
 * Android: RECOMP_ANDROID_DIR, the app's files directory (android_host.h).
 * Anything failing: "." (the working directory, the old behaviour).
 */
#include "recomp_exe_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char s_dir[PATH_MAX];
static char s_path[PATH_MAX];       /* the executable itself, kept by resolve() */

/* Cut `p` at its last separator; 0 if it has none. */
static int cut_dir(char *p)
{
    char *s = strrchr(p, '/');
#if defined(_WIN32)
    char *b = strrchr(p, '\\');
    if (!s || (b && b > s))
        s = b;
#endif
    if (!s)
        return 0;
    if (s == p)
        s[1] = 0;           /* "/exe" -> "/" */
    else
        *s = 0;
    return 1;
}

static int resolve(char *out, size_t cap)
{
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return 0;
    strcpy(s_path, out);
    return cut_dir(out);
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0)
        return 0;
    if (!realpath(raw, out)) {
        if (strlen(raw) >= cap)
            return 0;
        strcpy(out, raw);
    }
    strcpy(s_path, out);
    return cut_dir(out);
#elif defined(__ANDROID__)
    /* The process image is the system's app_process; the app's own directory
     * is the one its activity names (android_host.h). No executable path:
     * an app cannot start itself again. */
    const char *d = getenv("RECOMP_ANDROID_DIR");
    if (!d || !*d || strlen(d) >= cap)
        return 0;
    strcpy(out, d);
    return 1;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0)
        return 0;
    out[n] = 0;
    strcpy(s_path, out);
    return cut_dir(out);
#endif
}

const char *recomp_exe_dir(void)
{
    if (!s_dir[0] && !resolve(s_dir, sizeof s_dir))
        strcpy(s_dir, ".");
    return s_dir;
}

const char *recomp_exe_path(void)
{
    (void)recomp_exe_dir();
    return s_path;
}

/* ── portable mode ─────────────────────────────────────────────────── */

static int path_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int path_is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

int recomp_portable_find(const char *exe_dir, char *out, unsigned out_size)
{
    char marker[PATH_MAX + 16];
    int w;

    if (!exe_dir || !*exe_dir || !out || !out_size)
        return 0;
    w = snprintf(marker, sizeof marker, "%s/portable.txt", exe_dir);
    if (w <= 0 || (size_t)w >= sizeof marker)
        return 0;
    if (!path_is_file(marker)) {
        w = snprintf(marker, sizeof marker, "%s/data", exe_dir);
        if (w <= 0 || (size_t)w >= sizeof marker || !path_is_dir(marker))
            return 0;
    }
    w = snprintf(out, out_size, "%s/data", exe_dir);
    return w > 0 && (unsigned)w < out_size;
}

int recomp_portable_base(const char *appimage, const char *exe_dir, char *out, unsigned out_size)
{
    size_t n;

    if (!out || !out_size)
        return 0;
    if (appimage && *appimage) {
        /* An AppImage runs from a read-only mount: "next to the program" is
         * next to the .AppImage file. */
        char tmp[PATH_MAX];

        if (strlen(appimage) >= sizeof tmp)
            return 0;
        strcpy(tmp, appimage);
        if (!cut_dir(tmp))
            strcpy(tmp, ".");           /* a bare file name: the working directory */
        n = strlen(tmp);
        if (n >= out_size)
            return 0;
        memcpy(out, tmp, n + 1);
        return 1;
    }
    if (!exe_dir || !*exe_dir || strlen(exe_dir) >= out_size)
        return 0;
    strcpy(out, exe_dir);
    return 1;
}

int recomp_portable_root(char *out, unsigned out_size)
{
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    static char root[PATH_MAX + 16];
    static int done, found;

    if (!done) {
        char base[PATH_MAX];
#if defined(_WIN32)
        const char *ai = NULL;
#else
        const char *ai = getenv("APPIMAGE");
#endif
        found = recomp_portable_base(ai, recomp_exe_dir(), base, sizeof base)
             && recomp_portable_find(base, root, sizeof root);
        done = 1;
    }
    if (!found || !out || strlen(root) >= out_size)
        return 0;
    strcpy(out, root);
    return 1;
#else
    (void)out; (void)out_size;
    return 0;
#endif
}

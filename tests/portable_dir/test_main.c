/* portable_dir: the portable-mode path logic on made folders.
 *   portable_dir_test <work dir> */
#include "recomp_exe_dir.h"
#include "recomp_save_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static void touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    char work[600], exe[700], p[800], out[900];

    if (argc < 2) {
        printf("usage: portable_dir_test <work dir>\n");
        return 2;
    }
    snprintf(work, sizeof work, "%s", argv[1]);
    mkdir(work, 0755);

    /* No marker: not portable. */
    snprintf(exe, sizeof exe, "%s/plain", work);
    mkdir(exe, 0755);
    CHECK("no marker", recomp_portable_find(exe, out, sizeof out) == 0);

    /* portable.txt: portable, the root is <exe>/data (not made by the check). */
    snprintf(exe, sizeof exe, "%s/txt", work);
    mkdir(exe, 0755);
    snprintf(p, sizeof p, "%s/portable.txt", exe);
    touch(p);
    memset(out, 0, sizeof out);
    CHECK("portable.txt", recomp_portable_find(exe, out, sizeof out) == 1);
    snprintf(p, sizeof p, "%s/data", exe);
    CHECK("portable.txt root", strcmp(out, p) == 0);
    {
        struct stat st;
        CHECK("the check makes nothing", stat(p, &st) != 0);
    }

    /* A data folder: portable. */
    snprintf(exe, sizeof exe, "%s/dir", work);
    mkdir(exe, 0755);
    snprintf(p, sizeof p, "%s/data", exe);
    mkdir(p, 0755);
    CHECK("data folder", recomp_portable_find(exe, out, sizeof out) == 1 && strcmp(out, p) == 0);

    /* A file called data is not a folder. */
    snprintf(exe, sizeof exe, "%s/datafile", work);
    mkdir(exe, 0755);
    snprintf(p, sizeof p, "%s/data", exe);
    touch(p);
    CHECK("data is a file", recomp_portable_find(exe, out, sizeof out) == 0);

    /* A portable.txt that is a folder is not the marker. */
    snprintf(exe, sizeof exe, "%s/txtdir", work);
    mkdir(exe, 0755);
    snprintf(p, sizeof p, "%s/portable.txt", exe);
    mkdir(p, 0755);
    CHECK("portable.txt is a folder", recomp_portable_find(exe, out, sizeof out) == 0);

    /* Bad input and a short buffer. */
    CHECK("NULL exe dir", recomp_portable_find(NULL, out, sizeof out) == 0);
    CHECK("empty exe dir", recomp_portable_find("", out, sizeof out) == 0);
    snprintf(exe, sizeof exe, "%s/txt", work);
    CHECK("buffer too small", recomp_portable_find(exe, out, 4) == 0);

    /* The folders of each kind. */
    CHECK("saves", recomp_portable_user_dir("C:/Tk/data", RECOMP_DIR_DATA, out, sizeof out)
                   && strcmp(out, "C:/Tk/data/Saves") == 0);
    CHECK("game data (local)", recomp_portable_user_dir("C:/Tk/data", RECOMP_DIR_LOCAL, out, sizeof out)
                   && strcmp(out, "C:/Tk/data") == 0);
    CHECK("cache", recomp_portable_user_dir("C:/Tk/data", RECOMP_DIR_CACHE, out, sizeof out)
                   && strcmp(out, "C:/Tk/data/cache") == 0);
    CHECK("user dir: short buffer", recomp_portable_user_dir("C:/Tk/data", RECOMP_DIR_DATA, out, 8) == 0);
    CHECK("user dir: no root", recomp_portable_user_dir("", RECOMP_DIR_DATA, out, sizeof out) == 0);

    /* The folder tested: an AppImage's own folder when APPIMAGE is set, else
     * the executable's (an AppImage's executable is in a read-only mount). */
    CHECK("base: appimage", recomp_portable_base("/home/deck/Games/Takedawn.AppImage",
          "/tmp/.mount_Takedx", out, sizeof out) && strcmp(out, "/home/deck/Games") == 0);
    CHECK("base: appimage in /", recomp_portable_base("/Takedawn.AppImage", "/x", out, sizeof out)
          && strcmp(out, "/") == 0);
    CHECK("base: bare appimage name", recomp_portable_base("Takedawn.AppImage", "/x", out, sizeof out)
          && strcmp(out, ".") == 0);
    CHECK("base: no appimage", recomp_portable_base(NULL, "/opt/tk", out, sizeof out)
          && strcmp(out, "/opt/tk") == 0);
    CHECK("base: empty appimage", recomp_portable_base("", "/opt/tk", out, sizeof out)
          && strcmp(out, "/opt/tk") == 0);
    CHECK("base: nothing", recomp_portable_base(NULL, "", out, sizeof out) == 0);
    CHECK("base: short buffer", recomp_portable_base("/home/deck/Games/Takedawn.AppImage",
          "/x", out, 5) == 0);

    /* Without a marker beside the AppImage nothing is portable, whatever the
     * executable's folder holds. */
    snprintf(exe, sizeof exe, "%s/txt", work);          /* has portable.txt */
    snprintf(p, sizeof p, "%s/plain/Takedawn.AppImage", work);
    CHECK("base then find: marker beside the executable is not the AppImage's",
          recomp_portable_base(p, exe, out, sizeof out)
          && recomp_portable_find(out, p, sizeof p) == 0);
    snprintf(p, sizeof p, "%s/txt/Takedawn.AppImage", work);
    CHECK("base then find: marker beside the AppImage",
          recomp_portable_base(p, "/tmp/.mount_x", out, sizeof out)
          && recomp_portable_find(out, p, sizeof p) == 1);

    /* On macOS and Android the running program is never portable. */
#if defined(__APPLE__) || defined(__ANDROID__)
    CHECK("root: not on this platform", recomp_portable_root(out, sizeof out) == 0);
#endif

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("portable_dir: all checks passed\n");
    return 0;
}

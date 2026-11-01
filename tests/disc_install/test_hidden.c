/*
 * disc_install hidden-ancestor test (BO-229, GitHub #36).
 *
 * On some Android ROMs (scoped storage / FUSE) /storage/emulated/0/Android/data
 * is hidden from the app: stat() on it fails (ENOENT or EACCES) and mkdir()
 * on it says EEXIST, although the app's own folder under it is there and
 * mkdir() of a path below it works. p_mkdirs must make such a path without
 * ever needing to stat those ancestors.
 *
 * Real file permissions cannot make that situation (a path walk needs the
 * same search rights for stat and for mkdir), so this test compiles
 * disc_install.c into itself with stat() and mkdir() routed through shims
 * that hide chosen paths, and calls the static p_mkdirs directly. Pure C,
 * no device. POSIX only (the Windows build has no such ROMs).
 */
#if defined(_WIN32)
#include <stdio.h>
int main(void) { puts("disc_install hidden: skipped on Windows"); return 0; }
#else
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>

/* What the shims hide: stat() of a path that is a proper prefix of s_cut
 * (a directory above the app folder) fails with s_stat_errno. mkdir() of
 * any path that exists answers EEXIST as usual (the real mkdir). */
static const char *s_hidden;        /* the hidden directory, no trailing slash */
static int s_stat_errno;
static int s_hidden_stats;          /* how often a hidden path was stat'ed */

static int hidden_path(const char *p)
{
    size_t n;
    if (!s_hidden)
        return 0;
    n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        n--;
    /* the hidden dir and everything above it */
    return n <= strlen(s_hidden) && !strncmp(p, s_hidden, n)
        && (s_hidden[n] == '/' || s_hidden[n] == '\0');
}

static int shim_stat(const char *p, struct stat *st)
{
    if (hidden_path(p)) {
        s_hidden_stats++;
        errno = s_stat_errno;
        return -1;
    }
    return stat(p, st);
}

#define stat(p, st)   shim_stat((p), (st))
#include "../../src/install/disc_install.c"
#undef stat

static int s_fail, s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define ROOT "disc_install_hidden_fixture"

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void sh(const char *cmd)
{
    if (system(cmd) != 0) {
        fprintf(stderr, "command failed: %s\n", cmd);
        exit(2);
    }
}

static void run_case(int err, const char *what)
{
    char hidden[256], app[256], deep[300], deeper[300];

    snprintf(hidden, sizeof hidden, ROOT "/%d/storage/Android/data", err);
    snprintf(app, sizeof app, "%s/pkg/files", hidden);
    snprintf(deep, sizeof deep, "%s/Burnout 3 Takedown/Data", app);
    snprintf(deeper, sizeof deeper, "%s/x/y/z/", app);

    /* The app's own folder exists (Android made it); everything above it is hidden. */
    {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", app);
        sh(cmd);
    }
    s_hidden = hidden;
    s_stat_errno = err;

    /* The shim does hide the ancestors, the app folder is visible. */
    {
        struct stat st;
        CHECK(shim_stat(hidden, &st) != 0 && errno == err);
        CHECK(shim_stat(app, &st) == 0);
    }

    /* Two missing levels below the app folder: made, and no ancestor of the
     * app folder is looked at. */
    s_hidden_stats = 0;
    CHECK(p_mkdirs(deep) == 1);
    CHECK(is_dir(deep));
    CHECK(s_hidden_stats == 0);
    if (s_hidden_stats)
        fprintf(stderr, "  (%s: %d stat() calls hit a hidden ancestor)\n", what, s_hidden_stats);

    /* Already there: still fine. */
    CHECK(p_mkdirs(deep) == 1);

    /* Several missing levels with a trailing slash. */
    s_hidden_stats = 0;
    CHECK(p_mkdirs(deeper) == 1);
    CHECK(is_dir(app) && is_dir(deeper));
    CHECK(s_hidden_stats == 0);

    /* A file where a directory is wanted is still refused. */
    {
        char f[300], below[320];
        FILE *fp;
        snprintf(f, sizeof f, "%s/afile", app);
        fp = fopen(f, "wb");
        if (fp)
            fclose(fp);
        snprintf(below, sizeof below, "%s/sub", f);
        CHECK(p_mkdirs(below) == 0);
    }
    s_hidden = NULL;
}

int main(void)
{
    sh("rm -rf " ROOT);
    sh("mkdir -p " ROOT);

    run_case(ENOENT, "stat ENOENT (Realme, Android 13)");
    run_case(EACCES, "stat EACCES");

    sh("rm -rf " ROOT);
    if (s_fail) {
        fprintf(stderr, "disc_install hidden: %d of %d checks FAILED\n", s_fail, s_checks);
        return 1;
    }
    printf("disc_install hidden: %d checks ok\n", s_checks);
    return 0;
}
#endif

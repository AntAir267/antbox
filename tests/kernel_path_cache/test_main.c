/*
 * E:\CACHE (Partition1\CACHE) is the title's own scratch: it goes to
 * <save>/TitleCache, beside UDATA and TDATA, not into the game dir, and the
 * three user-data rules stop at a path boundary, so Partition1\cached.dat,
 * Partition1\CACHEX\ and Partition1\UDATAx stay in the game dir (upstream
 * c18d741b, kernel_path.c rule_match). POSIX.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
static char g_game[512], g_save[512];

static void check(int ok, const char *what, const char *got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got %s", got);
    putchar('\n');
    if (!ok) failures++;
}

static const char *host(const char *xbox)
{
    static char out[1024];
    if (!xbox_translate_path(xbox, out, sizeof out))
        return "";
    return out;
}

static int under(const char *path, const char *root)
{
    size_t n = strlen(root);
    return strncmp(path, root, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static void expect_under(const char *xbox, const char *root, const char *what)
{
    const char *p = host(xbox);
    check(under(p, root), what, p);
}

int main(void)
{
    char tmpl[] = "/tmp/kpc.XXXXXX", dir[600];
    struct stat st;
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_game, sizeof g_game, "%s/game", tmpl);
    snprintf(g_save, sizeof g_save, "%s/save", tmpl);
    mkdir(g_game, 0755);
    xbox_path_init(g_game, g_save);

    snprintf(dir, sizeof dir, "%s/TitleCache", g_save);
    expect_under("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin", dir,
                 "Partition1\\CACHE\\x goes to <save>/TitleCache");
    expect_under("\\Device\\Harddisk0\\Partition1\\CACHE", dir,
                 "the bare Partition1\\CACHE too");
    check(stat(dir, &st) == 0 && S_ISDIR(st.st_mode),
          "path init made <save>/TitleCache", dir);
    snprintf(dir, sizeof dir, "%s/UDATA", g_save);
    expect_under("\\Device\\Harddisk0\\Partition1\\UDATA\\4d530004\\a.sav", dir,
                 "UDATA still goes to the save dir");
    expect_under("\\Device\\Harddisk0\\Partition1\\cached.dat", g_game,
                 "Partition1\\cached.dat stays in the game dir");
    expect_under("\\Device\\Harddisk0\\Partition1\\CACHEX\\a", g_game,
                 "Partition1\\CACHEX\\ stays in the game dir");
    expect_under("\\Device\\Harddisk0\\Partition1\\UDATAx", g_game,
                 "Partition1\\UDATAx stays in the game dir");
    expect_under("\\Device\\Harddisk0\\Partition1\\media\\x.xmv", g_game,
                 "the rest of Partition1 stays in the game dir");

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

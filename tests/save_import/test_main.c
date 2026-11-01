/* save_import: kernel_save_import.h on made Xbox save trees.
 *   save_import_test <work dir> */
#include "kernel_save_import.h"
#include "kernel.h"
#include "recomp_save_dir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

#include "zip_fixture.h"

#define ID "4541005b"
#define SAVE "57BD267AFF58"

/* What the kernel lib supplies and kernel_crypto.c reaches for. */
void xbox_log(int level, const char *subsystem, const char *fmt, ...) { (void)level; (void)subsystem; (void)fmt; }
#if !defined(_WIN32)
VOID SecureZeroMemory(PVOID p, SIZE_T n) { memset(p, 0, n); }
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[700];

static const char *P(const char *fmt, ...)
{
    static char b[8][1024];
    static int k;
    va_list ap;
    char *o = b[k++ & 7];
    char rel[600];
    va_start(ap, fmt);
    vsnprintf(rel, sizeof rel, fmt, ap);
    va_end(ap);
    snprintf(o, 1024, "%s/%s", g_work, rel);
    return o;
}

static void put(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(2); }
    fwrite(d, 1, n, f);
    fclose(f);
}

static unsigned char *get(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

/* The profile the fixture zip holds. */
static void make_profile(unsigned char *p)
{
    int i;
    memset(p, 0, 4300);
    for (i = 0; i < 300; i++)
        p[i] = (unsigned char)(((i * 31 + 7) * (i + 1)) & 0xFF);
    p[0x3e] = p[0x3f] = 0;
    memcpy(p + 0x40, "Z\0I\0P\0P\0E\0R\0\0\0", 14);
}

static int sig_ok(const unsigned char *d, size_t n)
{
    unsigned char zero[16] = { 0 }, inner[20], want[20];
    xbox_XcHMAC(zero, 16, d, (ULONG)(n - 20), NULL, 0, inner);
    xbox_XcHMAC(zero, 16, inner, 20, NULL, 0, want);
    return !memcmp(want, d + n - 20, 20);
}

static void md(const char *path) { mkdir(path, 0755); }

static int stop_now(void *ctx) { (void)ctx; return 1; }
static int all_shallow(const char *dir, void *ctx) { (void)dir; (void)ctx; return 1; }
static int shallow_udata(const char *dir, void *ctx) { (void)ctx; return strstr(dir, "4d53002a") != NULL; }

int main(int argc, char **argv)
{
    unsigned char prof[4300], *got;
    size_t n;
    char out[1024], info[400], err[800], root[1024];
    int ok;

    if (argc < 2) return 2;
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    md(g_work);
    make_profile(prof);
    memset(prof + 4280, 0xAB, 20);                 /* the console's own signature */

    /* A drive copy: E:/UDATA/<id>/<save>, the title files, TDATA/<id>, another game's folder. */
    md(P("e")); md(P("e/UDATA")); md(P("e/UDATA/" ID)); md(P("e/UDATA/" ID "/" SAVE));
    md(P("e/UDATA/4d53002a")); md(P("e/TDATA")); md(P("e/TDATA/" ID));
    put(P("e/UDATA/" ID "/" SAVE "/Profile 1"), prof, sizeof prof);
    put(P("e/UDATA/" ID "/" SAVE "/SaveMeta.xbx"), "meta", 4);
    put(P("e/UDATA/" ID "/SaveImage.xbx"), "image", 5);
    put(P("e/UDATA/" ID "/TitleMeta.xbx"), "tmeta", 5);
    put(P("e/TDATA/" ID "/opt.bin"), "opt", 3);
    put(P("e/UDATA/4d53002a/other"), "x", 1);

    md(P("root"));
    {
        /* Each shape of input finds the same save. */
        const char *shapes[] = { "e", "e/UDATA", "e/UDATA/" ID, "e/UDATA/" ID "/" SAVE };
        int i;
        for (i = 0; i < 4; i++) {
            ok = xbox_SaveImportPrepare(P("%s", shapes[i]), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
            CHECK(shapes[i], ok);
            CHECK("check agrees", xbox_SaveImportCheck(P("%s", shapes[i]), ID) == 1);
            if (!ok) { printf("  %s\n", err); continue; }
            got = get(P("root/import-work/out/UDATA/" ID "/" SAVE "/Profile 1"), &n);
            CHECK("profile out", got && n == sizeof prof);
            if (got && n == sizeof prof) {
                CHECK("body kept", !memcmp(got, prof, n - 20));
                CHECK("signature is for zero keys", sig_ok(got, n));
                CHECK("signature changed", memcmp(got + n - 20, prof + n - 20, 20) != 0);
            }
            free(got);
            CHECK("name in the report", strstr(info, "profile ZIPPER") != NULL);
            CHECK("meta kept", (got = get(P("root/import-work/out/UDATA/" ID "/" SAVE "/SaveMeta.xbx"), &n)) && n == 4);
            free(got);
            CHECK("export check", recomp_save_export_check(out, ID, NULL, 0));
        }
        CHECK("title files come along from the title folder",
              (got = get(P("root/import-work/out/UDATA/" ID "/TitleMeta.xbx"), &n)) == NULL ? 0 : 1);
    }
    /* From the drive copy: TDATA too. */
    ok = xbox_SaveImportPrepare(P("e"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("drive", ok);
    CHECK("TDATA came along", (got = get(P("root/import-work/out/TDATA/" ID "/opt.bin"), &n)) && n == 3);
    free(got);

    /* The title files of the save in use fill in what a bare save folder lacks. */
    md(P("keep")); md(P("keep/UDATA")); md(P("keep/UDATA/" ID));
    put(P("keep/UDATA/" ID "/TitleImage.xbx"), "keptimg", 7);
    ok = xbox_SaveImportPrepare(P("e/UDATA/" ID "/" SAVE), ID, P("keep"), P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("keep", ok);
    CHECK("title image from the save in use", (got = get(P("root/import-work/out/UDATA/" ID "/TitleImage.xbx"), &n)) && n == 7);
    free(got);

    /* The wrong game: the folder above the save names another title. */
    md(P("w")); md(P("w/UDATA")); md(P("w/UDATA/4d53002a")); md(P("w/UDATA/4d53002a/" SAVE));
    put(P("w/UDATA/4d53002a/" SAVE "/Profile 1"), prof, sizeof prof);
    put(P("w/UDATA/4d53002a/" SAVE "/SaveMeta.xbx"), "meta", 4);
    ok = xbox_SaveImportPrepare(P("w"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("other game refused", !ok && strstr(err, "another game") != NULL);
    CHECK("check refuses it", xbox_SaveImportCheck(P("w"), ID) == 0);
    /* Not a save, a missing file, and a short profile. */
    ok = xbox_SaveImportPrepare(P("e/UDATA/4d53002a"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("no save refused", !ok && err[0]);
    md(P("s")); md(P("s/" SAVE));
    put(P("s/" SAVE "/Profile 1"), "short", 5);
    put(P("s/" SAVE "/SaveMeta.xbx"), "meta", 4);
    ok = xbox_SaveImportPrepare(P("s"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("short profile refused", !ok && strstr(err, "not a Burnout 3 profile"));
    ok = xbox_SaveImportPrepare(P("nothing-here"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("missing path", !ok && err[0]);

    /* The zip. */
    put(P("save.zip"), k_zip, sizeof k_zip);
    ok = xbox_SaveImportPrepare(P("save.zip"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
    CHECK("zip", ok);
    if (!ok) printf("  %s\n", err);
    got = get(P("root/import-work/out/UDATA/" ID "/" SAVE "/Profile 1"), &n);
    CHECK("zip profile", got && n == 4300);
    if (got && n == 4300) {
        unsigned char exp[4300];
        make_profile(exp);
        CHECK("zip profile body (inflate)", !memcmp(got, exp, 4300 - 20));
        CHECK("zip profile signed", sig_ok(got, n));
    }
    free(got);
    CHECK("zip name", strstr(info, "profile ZIPPER") != NULL);
    CHECK("stored entry", (got = get(P("root/import-work/out/UDATA/" ID "/TitleMeta.xbx"), &n)) && n == 10);
    free(got);
    snprintf(root, sizeof root, "%s", P("root"));
    CHECK("nothing written outside", (got = get(P("evil.txt"), &n)) == NULL && (got = get(P("root/evil.txt"), &n)) == NULL
          && (got = get(P("root/import-work/evil.txt"), &n)) == NULL);
    CHECK("unzip folder removed", !recomp_fs_is_dir(P("root/import-work/unzip")));
    /* A damaged zip. */
    {
        unsigned char bad[sizeof k_zip];
        memcpy(bad, k_zip, sizeof k_zip);
        bad[100] ^= 0xFF;                          /* inside the deflated data */
        put(P("bad.zip"), bad, sizeof bad);
        ok = xbox_SaveImportPrepare(P("bad.zip"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
        CHECK("damaged zip refused", !ok && err[0]);
        put(P("notzip.zip"), "not a zip at all, really", 24);
        ok = xbox_SaveImportPrepare(P("notzip.zip"), ID, NULL, P("root"), out, sizeof out, info, sizeof info, err, sizeof err);
        CHECK("not a zip refused", !ok && strstr(err, "not a zip"));
    }
    {
        /* BO-289: the browser's bounded look. A stop, a spent budget and a
         * folder left unwalked (a network share) are "not known" (-1). */
        xbox_save_check_limits lim;
        memset(&lim, 0, sizeof lim);
        CHECK("bounded: found", xbox_SaveImportCheckBounded(P("e"), ID, &lim) == 1);
        CHECK("bounded: none", xbox_SaveImportCheckBounded(P("e/UDATA/4d53002a"), ID, &lim) == 0);
        lim.stop = stop_now;
        CHECK("bounded: stopped", xbox_SaveImportCheckBounded(P("e"), ID, &lim) == -1);
        CHECK("bounded: plain check says no", xbox_SaveImportCheck(P("e/UDATA/4d53002a"), ID) == 0);
        lim.stop = NULL;
        lim.max_entries = 3;
        CHECK("bounded: budget spent", xbox_SaveImportCheckBounded(P("e"), ID, &lim) == -1);
        lim.max_entries = 0;
        lim.shallow = all_shallow;
        CHECK("bounded: a share is not walked", xbox_SaveImportCheckBounded(P("e"), ID, &lim) == -1);
        CHECK("bounded: the save folder on a share is still seen",
              xbox_SaveImportCheckBounded(P("e/UDATA/" ID "/" SAVE), ID, &lim) == 1);
        lim.shallow = shallow_udata;
        CHECK("bounded: a share below, the save beside it",
              xbox_SaveImportCheckBounded(P("e/UDATA"), ID, &lim) == 1);
    }
    xbox_SaveImportCleanup(P("root"));
    CHECK("cleanup", !recomp_fs_is_dir(P("root/import-work")));

    printf(failures ? "save_import: %d FAILED\n" : "save_import: ok\n", failures);
    return failures ? 1 : 0;
}

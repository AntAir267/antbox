/* ps2_import: a Burnout 3 PS2 save into the Xbox profile (BO-323), on made
 * saves only (fixtures/: a made-up profile, no player's save).
 *   ps2_import_test <work dir> <fixtures dir>
 *   ps2_import_test <work dir> --samples <dir>     every file and folder in
 *       <dir> (real saves kept outside the repository) is imported, each
 *       profile in it, and checked; the PS2 data read goes to
 *       <work>/samples/<name>.dat for comparing with another tool. */
#include "kernel_save_import.h"
#include "kernel_ps2save.h"
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

#define ID "4541005b"
#define SAVE "57BD267AFF58"
#define DATA PS2SAVE_DATA_SIZE
#define XB PS2SAVE_XBOX_SIZE

void xbox_log(int level, const char *subsystem, const char *fmt, ...) { (void)level; (void)subsystem; (void)fmt; }
#if !defined(_WIN32)
VOID SecureZeroMemory(PVOID p, SIZE_T n) { memset(p, 0, n); }
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[700], g_fix[700];

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

static const char *F(const char *name)
{
    static char b[4][1024];
    static int k;
    char *o = b[k++ & 3];
    snprintf(o, 1024, "%s/%s", g_fix, name);
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

static void md(const char *path) { mkdir(path, 0755); }

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int sig_ok(const unsigned char *d, size_t n)
{
    unsigned char zero[16] = { 0 }, inner[20], want[20];
    xbox_XcHMAC(zero, 16, d, (ULONG)(n - 20), NULL, 0, inner);
    xbox_XcHMAC(zero, 16, inner, 20, NULL, 0, want);
    return !memcmp(want, d + n - 20, 20);
}

/* The Xbox profile made from `ps2` profile `slot` (0-based) is right:
 * header and records, the profile, settings as `settings` (or the PS2's with
 * the screen offset cleared), the sum, zeros, the signature. */
static int profile_ok(const unsigned char *x, size_t n, const unsigned char *ps2, int slot,
                      const unsigned char *settings)
{
    unsigned char set[PS2SAVE_SETTINGS_LEN];
    size_t i;
    if (n != XB || memcmp(x, ps2, PS2SAVE_SETTINGS) != 0)
        return 0;
    if (settings) {
        memcpy(set, settings, sizeof set);
    } else {
        memcpy(set, ps2 + PS2SAVE_SETTINGS, sizeof set);
        set[0x30] = set[0x31] = 0;
    }
    if (memcmp(x + PS2SAVE_SETTINGS, set, sizeof set) || memcmp(x + 0xE374, ps2 + 0xE374 + slot * 0x49C, 0x49C))
        return 0;
    if (ps2save_sum(x, 0xE810) != rd32(x + 0xE810))
        return 0;
    for (i = 0xE814; i < 0xE9EC; i++)
        if (x[i])
            return 0;
    return sig_ok(x, n);
}

/* A zip with one stored entry. */
static unsigned crc32_of(const unsigned char *d, size_t n)
{
    unsigned c = 0xFFFFFFFFu, k;
    size_t i;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

static void make_zip(const char *zip, const char *entry, const unsigned char *d, size_t n)
{
    FILE *f = fopen(zip, "wb");
    unsigned crc = crc32_of(d, n), nl = (unsigned)strlen(entry);
    long cd;
    w32(f, 0x04034b50); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
    w32(f, crc); w32(f, (unsigned)n); w32(f, (unsigned)n); w16(f, nl); w16(f, 0);
    fwrite(entry, 1, nl, f);
    fwrite(d, 1, n, f);
    cd = ftell(f);
    w32(f, 0x02014b50); w16(f, 20); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
    w32(f, crc); w32(f, (unsigned)n); w32(f, (unsigned)n); w16(f, nl); w16(f, 0); w16(f, 0); w16(f, 0);
    w16(f, 0); w32(f, 0); w32(f, 0);
    fwrite(entry, 1, nl, f);
    w32(f, 0x06054b50); w16(f, 0); w16(f, 0); w16(f, 1); w16(f, 1);
    w32(f, (unsigned)(ftell(f) - cd - 12)); w32(f, (unsigned)cd); w16(f, 0);
    fclose(f);
}

static int stop_now(void *ctx) { (void)ctx; return 1; }

/* One import from `input` into the work root; the profile made, or NULL. */
static unsigned char *import(const char *input, const char *keep, int slot, xbox_ps2_choice *ch, char *info,
                             char *err, size_t *n)
{
    char out[1024];
    if (!xbox_SaveImportPrepareSlot(input, ID, keep, P("root"), slot, ch, out, sizeof out, info, 400, err, 1400))
        return NULL;
    return get(P("root/import-work/out/UDATA/" ID "/" SAVE "/Profile 1"), n);
}

static int samples(const char *dir);

int main(int argc, char **argv)
{
    char info[400], err[1400];
    xbox_ps2_choice ch;
    unsigned char *a, *b, *got;
    size_t an = 0, bn = 0, n = 0;
    int i;
    static const char *const containers[] = { "a.psu", "a.max", "a.cbs", "a.xps", "a.psv" };

    if (argc < 3) {
        printf("usage: ps2_import_test <work dir> <fixtures dir> | <work dir> --samples <dir>\n");
        return 2;
    }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    recomp_fs_rm_tree(g_work);          /* what an earlier run left */
    md(g_work);
    md(P("root"));
    if (!strcmp(argv[2], "--samples") && argc > 3)
        return samples(argv[3]);
    snprintf(g_fix, sizeof g_fix, "%s", argv[2]);

    a = get(F("a.dat"), &an);
    b = get(F("b.dat"), &bn);
    if (!a || !b || an != DATA || bn != DATA) {
        printf("fixtures missing in %s\n", g_fix);
        return 2;
    }
    CHECK("fixture a is a save", ps2save_data_ok(a, an));
    CHECK("fixture b is a save", ps2save_data_ok(b, bn));

    /* Every single-save file, no save in use. */
    for (i = 0; i < (int)(sizeof containers / sizeof *containers); i++) {
        got = import(F(containers[i]), NULL, 0, &ch, info, err, &n);
        if (!got)
            printf("  %s: %s\n", containers[i], err);
        CHECK(containers[i], got && profile_ok(got, n, a, 0, NULL));
        CHECK("one profile to choose", ch.count == 1 && ch.slot[0] == 1);
        CHECK("report names it", strstr(info, "TESTER") && strstr(info, "PS2 save BASLUS-21050"));
        CHECK("settings from the PS2", strstr(info, "Settings taken from the PS2 save") != NULL);
        free(got);
        got = get(P("root/import-work/out/UDATA/" ID "/" SAVE "/SaveMeta.xbx"), &n);
        CHECK("save meta written", got && n == 2 + 2 * 26 && got[0] == 0xFF && got[2] == 'N');
        free(got);
        got = get(P("root/import-work/out/UDATA/" ID "/TitleMeta.xbx"), &n);
        CHECK("title meta written", got && n > 20);
        free(got);
        CHECK("export check", recomp_save_export_check(P("root/import-work/out"), ID, NULL, 0));
    }

    /* Memory card images, with the ECC bytes and without, from deflated
     * fixtures. */
    {
        static const char *const cards[2] = { "card-ecc.ps2.deflate", "card-plain.ps2.deflate" };
        static const size_t sizes[2] = { 8650752u, 8388608u };
        for (i = 0; i < 2; i++) {
            unsigned char *z = get(F(cards[i]), &n), *card = malloc(sizes[i]);
            int ok = z && card && xbox_SaveImportInflate(z, n, card, sizes[i]);
            CHECK("card fixture inflates", ok);
            if (ok) {
                put(P(i ? "plain.ps2" : "ecc.ps2"), card, sizes[i]);
                got = import(P(i ? "plain.ps2" : "ecc.ps2"), NULL, 0, &ch, info, err, &n);
                if (!got)
                    printf("  %s: %s\n", cards[i], err);
                CHECK(cards[i], got && profile_ok(got, n, a, 0, NULL));
                CHECK("kind in the report", strstr(info, "memory card") != NULL);
                free(got);
                if (i == 0) {
                    /* Cut short: refused with a sentence. */
                    put(P("short.ps2"), card, 4u << 20);
                    got = import(P("short.ps2"), NULL, 0, &ch, info, err, &n);
                    CHECK("cut card refused", !got && strstr(err, "cut short"));
                    free(got);
                }
            }
            free(z);
            free(card);
        }
    }

    /* A save folder, the way a PCSX2 folder card or a copy holds it, below
     * the folder chosen; and the check marks it as a PS2 save. */
    md(P("pcsx2"));
    md(P("pcsx2/Mcd001.ps2"));
    md(P("pcsx2/Mcd001.ps2/BESLES-52584"));
    put(P("pcsx2/Mcd001.ps2/BESLES-52584/BESLES-52584"), a, an);
    put(P("pcsx2/Mcd001.ps2/BESLES-52584/icon.sys"), "x", 1);
    got = import(P("pcsx2"), NULL, 0, &ch, info, err, &n);
    CHECK("PAL folder save", got && profile_ok(got, n, a, 0, NULL));
    CHECK("folder kind", strstr(info, "BESLES-52584 (PS2 save folder)") != NULL);
    free(got);
    got = import(P("pcsx2/Mcd001.ps2/BESLES-52584/"), NULL, 0, &ch, info, err, &n);
    CHECK("the save folder itself, with a trailing slash", got && profile_ok(got, n, a, 0, NULL));
    free(got);
    CHECK("check says PS2", xbox_SaveImportCheck(P("pcsx2"), ID) == 1 &&
                            xbox_SaveImportCheckBounded(P("pcsx2"), ID, NULL) == 2);
    {
        xbox_save_check_limits lim;
        memset(&lim, 0, sizeof lim);
        lim.stop = stop_now;
        CHECK("check stopped", xbox_SaveImportCheckBounded(P("pcsx2"), ID, &lim) == -1);
    }

    /* A folder with a .max in it, and a zip of one. */
    md(P("dl"));
    {
        unsigned char *m = get(F("a.max"), &n);
        put(P("dl/burnout.max"), m, n);
        CHECK("folder holding a .max is marked", xbox_SaveImportCheckBounded(P("dl"), ID, NULL) == 2);
        make_zip(P("save.zip"), "saves/burnout.max", m, n);
        free(m);
    }
    got = import(P("save.zip"), NULL, 0, &ch, info, err, &n);
    CHECK("zip of a .max", got && profile_ok(got, n, a, 0, NULL));
    free(got);
    CHECK("unzip folder removed", !recomp_fs_is_dir(P("root/import-work/unzip")));

    /* The player's settings win: a save in use with its own settings and
     * picture. */
    {
        unsigned char x[XB], set[PS2SAVE_SETTINGS_LEN];
        md(P("keep"));
        md(P("keep/UDATA"));
        md(P("keep/UDATA/" ID));
        md(P("keep/UDATA/" ID "/" SAVE));
        memset(x, 0, sizeof x);
        x[0] = 0x10;
        for (i = 0; i < (int)sizeof set; i++)
            set[i] = (unsigned char)(0xA0 + i);
        memcpy(x + PS2SAVE_SETTINGS, set, sizeof set);
        x[0xE810] = 0;
        {
            unsigned s = ps2save_sum(x, 0xE810);
            x[0xE810] = s & 255; x[0xE811] = (s >> 8) & 255; x[0xE812] = (s >> 16) & 255; x[0xE813] = s >> 24;
        }
        put(P("keep/UDATA/" ID "/" SAVE "/Profile 1"), x, sizeof x);
        put(P("keep/UDATA/" ID "/" SAVE "/SaveImage.xbx"), "picture", 7);
        put(P("keep/UDATA/" ID "/TitleMeta.xbx"), "title-meta", 10);
        got = import(F("a.psu"), P("keep"), 0, &ch, info, err, &n);
        CHECK("settings kept", got && profile_ok(got, n, a, 0, set));
        CHECK("report says kept", strstr(info, "current game settings are kept") != NULL);
        free(got);
        got = get(P("root/import-work/out/UDATA/" ID "/" SAVE "/SaveImage.xbx"), &n);
        CHECK("picture kept", got && n == 7);
        free(got);
        got = get(P("root/import-work/out/UDATA/" ID "/TitleMeta.xbx"), &n);
        CHECK("title meta kept", got && n == 10);
        free(got);
        /* A damaged save in use: its settings are not taken. */
        x[0xE334] ^= 1;
        put(P("keep/UDATA/" ID "/" SAVE "/Profile 1"), x, sizeof x);
        got = import(F("a.psu"), P("keep"), 0, &ch, info, err, &n);
        CHECK("bad save in use: PS2 settings", got && profile_ok(got, n, a, 0, NULL));
        free(got);
    }

    /* Two profiles: the choice. */
    got = import(F("b.psu"), NULL, 0, &ch, info, err, &n);
    CHECK("two profiles: asked", !got && ch.count == 2 && ch.slot[0] == 1 && ch.slot[1] == 3);
    CHECK("two profiles: listed", strstr(err, "TESTER") && strstr(err, "SECOND") && strstr(err, "3: "));
    CHECK("choice lines", strstr(ch.line[1], "SECOND") != NULL);
    got = import(F("b.psu"), NULL, 3, &ch, info, err, &n);
    CHECK("profile 3", got && profile_ok(got, n, b, 2, NULL));
    CHECK("profile 3 named", strstr(info, "SECOND") != NULL);
    free(got);
    got = import(F("b.psu"), NULL, 1, &ch, info, err, &n);
    CHECK("profile 1", got && profile_ok(got, n, b, 0, NULL));
    free(got);
    got = import(F("b.psu"), NULL, 2, &ch, info, err, &n);
    CHECK("empty profile refused", !got && strstr(err, "empty"));

    /* Refusals. */
    {
        unsigned char bad[DATA];
        memcpy(bad, a, sizeof bad);
        bad[0x100] ^= 0x55;
        md(P("bad"));
        md(P("bad/BASLUS-21050"));
        put(P("bad/BASLUS-21050/BASLUS-21050"), bad, sizeof bad);
        got = import(P("bad"), NULL, 0, &ch, info, err, &n);
        CHECK("wrong sum refused", !got && strstr(err, "damaged"));
        unsigned char *m = get(F("a.max"), &n);
        put(P("cut.max"), m, n / 2);
        free(m);
        got = import(P("cut.max"), NULL, 0, &ch, info, err, &n);
        CHECK("cut .max refused", !got && strstr(err, "damaged"));
        put(P("notes.txt"), "hello", 5);
        got = import(P("notes.txt"), NULL, 0, &ch, info, err, &n);
        CHECK("other file refused", !got && strstr(err, "not a PS2 save"));
        md(P("empty"));
        got = import(P("empty"), NULL, 0, &ch, info, err, &n);
        CHECK("empty folder refused", !got && strstr(err, "No Burnout 3 save") && strstr(err, "PS2 save"));
        CHECK("check says no", xbox_SaveImportCheckBounded(P("empty"), ID, NULL) == 0);
        /* Another game's folder on a card-like folder. */
        md(P("other"));
        md(P("other/BASLUS-20000"));
        put(P("other/BASLUS-20000/BASLUS-20000"), "data", 4);
        got = import(P("other"), NULL, 0, &ch, info, err, &n);
        CHECK("other game refused", !got && err[0]);
    }

    free(a);
    free(b);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

/* ── real saves kept outside the repository ──────────────────────── */

typedef struct {
    char names[256][256];
    int n;
} List;

static int list_cb(const char *dir, const char *name, int dir_entry, void *ctx)
{
    List *l = (List *)ctx;
    (void)dir;
    (void)dir_entry;
    if (name[0] != '.' && l->n < 256)
        snprintf(l->names[l->n++], sizeof l->names[0], "%s", name);
    return 1;
}

static int cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int samples(const char *dir)
{
    static List l;
    int i, s, imported = 0;
    md(P("samples"));
    recomp_fs_each(dir, list_cb, &l);
    qsort(l.names, (size_t)l.n, sizeof l.names[0], cmp);
    for (i = 0; i < l.n; i++) {
        char in[1024], info[400], err[1400];
        xbox_ps2_choice ch;
        ps2save_b3 *ps = malloc(sizeof *ps);
        size_t n = 0;
        unsigned char *got;
        snprintf(in, sizeof in, "%s/%s", dir, l.names[i]);
        if (ps2save_find(in, 4, NULL, ps, err, sizeof err) <= 0) {
            printf("%-28s no PS2 save: %s\n", l.names[i], err);
            free(ps);
            continue;
        }
        put(P("samples/%s.dat", l.names[i]), ps->data, DATA);
        got = import(in, NULL, 0, &ch, info, err, &n);
        if (!got && ch.count < 2) {
            printf("%-28s FAIL %s\n", l.names[i], err);
            failures++;
        }
        free(got);
        for (s = 0; s < ch.count; s++) {
            got = import(in, NULL, ch.slot[s], &ch, info, err, &n);
            if (!got || !profile_ok(got, n, ps->data, ch.slot[s] - 1, NULL)) {
                printf("%-28s FAIL profile %d: %s\n", l.names[i], ch.slot[s], got ? "content" : err);
                failures++;
            } else {
                printf("%-28s ok  %s\n", l.names[i], info);
                imported++;
            }
            free(got);
        }
        free(ps);
    }
    printf("%d profiles imported, %d failures\n", imported, failures);
    return failures ? 1 : 0;
}

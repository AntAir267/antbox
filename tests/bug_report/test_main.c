/*
 * bug_report test: xbox_BugReport over fixture inputs.
 *
 * Every input the report reads (the run logs, a crash report, the settings
 * file, the process's variables) has a fake home path, user name, full name
 * and host name planted in it, in the shapes they turn up in: a POSIX path, a
 * Windows path, a bare name, another user's home. The archive is then read
 * back with an inflate written here (not the writer's code run backwards)
 * and every member searched for them. Also: the members that must be there
 * are; a save file, a minidump, a foreign variable and a shader cache's
 * contents are not; a long log is cut to its head and tail; the frame comes
 * through byte for byte.
 */
#include "xbox_report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define t_mkdir(p) _mkdir(p)
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <sys/stat.h>
#define t_mkdir(p) mkdir((p), 0755)
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── the fixture ───────────────────────────────────────────────────── */

#define FAKE_HOME   "/home/janedoe"
#define FAKE_USER   "janedoe"
#define FAKE_FULL   "Jane Q. Doe"
#define FAKE_HOST   "janes-desktop.example.org"
#define FAKE_SHORT  "janes-desktop"

static const xbox_report_identity k_who = { FAKE_HOME, FAKE_USER, FAKE_FULL, FAKE_HOST };

/* What must be in no member, in any spelling the inputs use. */
static const char *const k_forbidden[] = {
    FAKE_HOME, FAKE_USER, "JANEDOE", "Jane Q. Doe", FAKE_SHORT, "example.org",
    "C:\\Users\\janedoe", "otherperson", "hunter2-secret", "SAVEGAME-BYTES",
    "MINIDUMP-BYTES", "PIPELINE-CACHE-BYTES", "SDL_VIDEODRIVER", "LD_PRELOAD", NULL,
};

static void write_file(const char *path, const char *text, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fwrite(text, 1, n, f);
    fclose(f);
}

static void write_str(const char *path, const char *text) { write_file(path, text, strlen(text)); }

static const char k_log_lines[] =
    "[SETTINGS] " FAKE_HOME "/.local/share/burnout3/settings.toml\n"
    "[SAVE] dir: " FAKE_HOME "/.local/share/burnout3\n"
    "[PRESENT] display 0 mode 1920x1080 at 120 Hz\n"
    "[vulkan] device up: Fake GPU 9000 (Vulkan 1.3.280, driver 0x1234), queue family 0\n"
    "[INPUT] controller \"Fake Pad\" 1234:5678 player 0 opened\n"
    "opened C:\\Users\\janedoe\\AppData\\Roaming\\Burnout3\\x.bin\n"
    "opened c:/users/JANEDOE/appdata/y.bin as " FAKE_USER " on " FAKE_HOST "\n"
    "a second account: /Users/otherperson/Library/thing and /home/otherperson\n"
    "host " FAKE_SHORT ", owner " FAKE_FULL ", mail " FAKE_USER "@example.org\n"
    "prefix Z:\\home\\janedoe\\.steam and \"" FAKE_HOME "\"\n";

static int capture_stub(const char *path, unsigned timeout_ms)
{
    static const char bmp[] = "BM-FAKE-FRAME-0123456789-not-text-/home/untouched";
    (void)timeout_ms;
    write_file(path, bmp, sizeof bmp);
    return 1;
}

static const char *settings_path(void) { return "bug_report_fixture/save/settings.toml"; }

static void extra_section(xbox_report_buf *b)
{
    xbox_report_printf(b, "gpu: Fake GPU 9000, cache at %s/.cache/burnout3\n", FAKE_HOME);
    xbox_report_dir_summary(b, "shader cache", "bug_report_fixture/cache");
}

static void make_fixture(void)
{
    char *big;
    size_t n = 0, cap = 900u * 1024u;
    int i;

    t_mkdir("bug_report_fixture");
    t_mkdir("bug_report_fixture/save");
    t_mkdir("bug_report_fixture/save/logs");
    t_mkdir("bug_report_fixture/save/crash");
    t_mkdir("bug_report_fixture/save/crash/20260102-030405-crash");
    t_mkdir("bug_report_fixture/save/crash/20260101-000000-hang");
    t_mkdir("bug_report_fixture/save/UDATA");
    t_mkdir("bug_report_fixture/cache");
    t_mkdir("bug_report_fixture/out");

    write_str("bug_report_fixture/save/logs/run.log", k_log_lines);
    /* A tail far over what the report keeps: its middle must be left out. */
    big = (char *)malloc(cap + 256);
    for (i = 0; n < cap; i++)
        n += (size_t)snprintf(big + n, 200, "[TRACE] line %06d from " FAKE_HOME "/x by %s\n", i,
                              i % 2 ? FAKE_USER : "nobody");
    write_file("bug_report_fixture/save/logs/run.tail.old.log", big, n / 2);
    write_file("bug_report_fixture/save/logs/run.tail.log", big + n / 2, n - n / 2);
    free(big);
    write_str("bug_report_fixture/save/logs/prev1.log", "PREVIOUS-RUN-ONE " FAKE_HOME "\n");
    write_str("bug_report_fixture/save/logs/prev2.log", "PREVIOUS-RUN-TWO " FAKE_HOST "\n");

    write_str("bug_report_fixture/save/crash/20260102-030405-crash/report.txt",
              "CRASH-REPORT-NEW signal 11 in " FAKE_HOME "/bin/burnout3\n");
    write_str("bug_report_fixture/save/crash/20260102-030405-crash/log.txt",
              "last lines, user " FAKE_USER "\n");
    write_str("bug_report_fixture/save/crash/20260102-030405-crash/settings.txt",
              "command line: " FAKE_HOME "/bin/burnout3\n"
              "\n[runtime table: every value set, after enhance.toml]\n"
              "RECOMP_VBLANK=1\n"
              "\n[environment: RECOMP_ BO3_ XBOX_ SDL_ ASAN_ UBSAN_]\n"
              "RECOMP_KEPT_IN_CRASH=1\n"
              "SDL_VIDEODRIVER=wayland\n"
              "BO3_ALSO_KEPT=1\n");
    write_str("bug_report_fixture/save/crash/20260102-030405-crash/minidump.dmp",
              "MINIDUMP-BYTES");
    write_str("bug_report_fixture/save/crash/20260101-000000-hang/report.txt",
              "CRASH-REPORT-OLD hang on " FAKE_SHORT "\n");

    write_str("bug_report_fixture/save/settings.toml",
              "[display]\nfullscreen = true\n# kept by " FAKE_FULL "\n"
              "[render]\nshader_cache = \"" FAKE_HOME "/.cache/burnout3\"\n");
    write_str("bug_report_fixture/save/UDATA/save.bin", "SAVEGAME-BYTES");
    write_str("bug_report_fixture/cache/pipelines.bin", "PIPELINE-CACHE-BYTES");

    set_env("RECOMP_SAVE_DIR", FAKE_HOME "/.local/share/burnout3");
    set_env("BO3_FIXTURE_VAR", "kept-bo3-value " FAKE_USER);
    set_env("SECRET_TOKEN", "hunter2-secret");
    set_env("SDL_VIDEODRIVER", "wayland");
    set_env("LD_PRELOAD", "");
}

/* ── reading the archive back ──────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    size_t n, at;
    uint32_t acc;
    int bits;
    int bad;
} bitr;

static uint32_t br_bits(bitr *r, int n)
{
    uint32_t v;
    while (r->bits < n) {
        if (r->at >= r->n) {
            r->bad = 1;
            return 0;
        }
        r->acc |= (uint32_t)r->p[r->at++] << r->bits;
        r->bits += 8;
    }
    v = r->acc & ((1u << n) - 1u);
    r->acc >>= n;
    r->bits -= n;
    return v;
}

/* RFC 1951: stored blocks and blocks with the fixed codes (all the writer
 * makes). Returns the bytes written, or (size_t)-1. */
static size_t inflate_fixed(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    bitr r = { in, n, 0, 0, 0, 0 };
    size_t o = 0;
    int last;

    do {
        uint32_t type;
        last = (int)br_bits(&r, 1);
        type = br_bits(&r, 2);
        if (type == 0) {
            uint32_t len;
            r.acc = 0;
            r.bits = 0;
            if (r.at + 4 > r.n)
                return (size_t)-1;
            len = r.p[r.at] | (uint32_t)r.p[r.at + 1] << 8;
            r.at += 4;
            if (r.at + len > r.n || o + len > cap)
                return (size_t)-1;
            memcpy(out + o, r.p + r.at, len);
            r.at += len;
            o += len;
            continue;
        }
        if (type != 1)
            return (size_t)-1;
        for (;;) {
            uint32_t code = 0, sym;
            int i;
            for (i = 0; i < 7; i++)
                code = code << 1 | br_bits(&r, 1);
            if (code <= 0x17) {
                sym = 256 + code;
            } else {
                code = code << 1 | br_bits(&r, 1);
                if (code >= 0x30 && code <= 0xBF) {
                    sym = code - 0x30;
                } else if (code >= 0xC0 && code <= 0xC7) {
                    sym = 280 + code - 0xC0;
                } else {
                    code = code << 1 | br_bits(&r, 1);
                    if (code < 0x190)
                        return (size_t)-1;
                    sym = 144 + code - 0x190;
                }
            }
            if (r.bad)
                return (size_t)-1;
            if (sym < 256) {
                if (o >= cap)
                    return (size_t)-1;
                out[o++] = (uint8_t)sym;
            } else if (sym == 256) {
                break;
            } else {
                uint32_t len, dist, dc = 0;
                if (sym > 285)
                    return (size_t)-1;
                len = lbase[sym - 257] + br_bits(&r, lext[sym - 257]);
                for (i = 0; i < 5; i++)
                    dc = dc << 1 | br_bits(&r, 1);
                if (dc > 29)
                    return (size_t)-1;
                dist = dbase[dc] + br_bits(&r, dext[dc]);
                if (dist > o || o + len > cap || r.bad)
                    return (size_t)-1;
                for (; len; len--, o++)
                    out[o] = out[o - dist];
            }
        }
    } while (!last);
    return r.bad ? (size_t)-1 : o;
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

typedef struct {
    char name[200];
    uint8_t *data;
    size_t size;
} member;

#define MEMBERS_MAX 96
static member s_m[MEMBERS_MAX];
static int s_members;

static uint8_t *s_zip;
static size_t s_zip_n;

static int read_zip(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t eocd, at;
    int i, n;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    s_zip_n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_zip = (uint8_t *)malloc(s_zip_n + 1);
    if (fread(s_zip, 1, s_zip_n, f) != s_zip_n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (s_zip_n < 22)
        return 0;
    eocd = s_zip_n - 22;
    if (rd32(s_zip + eocd) != 0x06054B50u)
        return 0;
    n = (int)rd16(s_zip + eocd + 10);
    at = rd32(s_zip + eocd + 16);
    for (i = 0; i < n && i < MEMBERS_MAX; i++) {
        const uint8_t *c = s_zip + at, *l;
        uint32_t method, crc, csize, usize, nlen, off;
        member *m = &s_m[s_members];

        if (at + 46 > s_zip_n || rd32(c) != 0x02014B50u)
            return 0;
        method = rd16(c + 10);
        crc = rd32(c + 16);
        csize = rd32(c + 20);
        usize = rd32(c + 24);
        nlen = rd16(c + 28);
        off = rd32(c + 42);
        if (nlen >= sizeof m->name)
            return 0;
        memcpy(m->name, c + 46, nlen);
        m->name[nlen] = 0;
        l = s_zip + off;
        if (off + 30 > s_zip_n || rd32(l) != 0x04034B50u || rd16(l + 26) != nlen
                || memcmp(l + 30, m->name, nlen) || rd32(l + 14) != crc
                || rd32(l + 18) != csize || rd32(l + 22) != usize)
            return 0;
        l += 30 + nlen + rd16(l + 28);
        m->data = (uint8_t *)malloc(usize + 1);
        if (method == 0) {
            if (csize != usize)
                return 0;
            memcpy(m->data, l, usize);
        } else if (method == 8) {
            if (inflate_fixed(l, csize, m->data, usize) != usize)
                return 0;
        } else {
            return 0;
        }
        m->data[usize] = 0;
        m->size = usize;
        if (crc32_ref(m->data, usize) != crc)
            return 0;
        s_members++;
        at += 46 + nlen + rd16(c + 30) + rd16(c + 32);
    }
    return s_members == n;
}

static const member *find(const char *name)
{
    int i;
    for (i = 0; i < s_members; i++)
        if (!strcmp(s_m[i].name, name))
            return &s_m[i];
    return NULL;
}

static int mem_has(const uint8_t *p, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    if (!k || k > n)
        return 0;
    for (i = 0; i + k <= n; i++)
        if (p[i] == (uint8_t)needle[0] && !memcmp(p + i, needle, k))
            return 1;
    return 0;
}

static int has(const char *name, const char *needle)
{
    const member *m = find(name);
    return m && mem_has(m->data, m->size, needle);
}

/* ── the tests ─────────────────────────────────────────────────────── */

static void test_redact(void)
{
    static const xbox_report_identity deck = { "/home/deck", "deck", NULL, "steamdeck" };
    static const xbox_report_identity ann = { "/home/ann", "ann", NULL, "box" };
    size_t n = 0;
    char *s;

    s = xbox_report_redact(&k_who, FAKE_HOME "/a b/c", strlen(FAKE_HOME "/a b/c"), &n);
    CHECK(s && !strcmp(s, "~/a b/c") && n == 7);
    free(s);
    /* A console's fixed account is nobody's name: only the path goes. */
    s = xbox_report_redact(&deck, "Steam Deck at /home/deck/x on steamdeck", 39, &n);
    CHECK(s && !strcmp(s, "Steam Deck at ~/x on steamdeck"));
    free(s);
    /* A name inside a longer word stays; on its own it goes. */
    s = xbox_report_redact(&ann, "channel banner /home/anna /home/ann ann's box.", 46, &n);
    CHECK(s && !strcmp(s, "channel banner ~ ~ <user>'s <host>."));
    free(s);
    s = xbox_report_redact(&k_who, "C:\\Users\\Some One\\AppData\\x", 27, &n);
    CHECK(s && !strcmp(s, "C:~\\AppData\\x"));
    free(s);
    s = xbox_report_redact(&k_who, "", 0, &n);
    CHECK(s && n == 0 && !*s);
    free(s);
}

/* The writer's deflate against this file's inflate, over shapes of data. */
static void test_roundtrip(void)
{
    static const size_t sizes[] = { 0, 1, 2, 3, 64, 65, 258, 259, 4096, 70000, 300000 };
    char path[] = "bug_report_fixture/out/roundtrip.zip";
    xbox_report *r = xbox_report_open(path, &k_who);
    uint8_t *src[2 * 11];
    uint32_t seed = 12345;
    unsigned i;
    int k;

    CHECK(r != NULL);
    if (!r)
        return;
    for (i = 0; i < 11; i++) {
        size_t n = sizes[i], j;
        char name[32];
        /* Text-like (repeats), and noise (must be stored, not grown). */
        for (k = 0; k < 2; k++) {
            uint8_t *p = (uint8_t *)malloc(n + 1);
            for (j = 0; j < n; j++) {
                seed = seed * 1664525u + 1013904223u;
                p[j] = k ? (uint8_t)(seed >> 24) : (uint8_t)("abcabcabd \n"[(seed >> 28) % 11]);
            }
            src[i * 2 + k] = p;
            snprintf(name, sizeof name, "%s/%u.bin", k ? "noise" : "text", i);
            CHECK(xbox_report_binary(r, name, p, n));
        }
    }
    CHECK(xbox_report_close(r));
    s_members = 0;
    CHECK(read_zip(path));
    CHECK(s_members == 22);
    for (i = 0; i < 22 && (int)i < s_members; i++) {
        CHECK(s_m[i].size == sizes[i / 2]);
        CHECK(!memcmp(s_m[i].data, src[i], s_m[i].size));
    }
    /* Text shrank: the archive is well under the 375 KB of text-like input. */
    CHECK(s_zip_n < 375000u + 375000u / 2u);
    for (i = 0; i < 22; i++)
        free(src[i]);
}

static void test_report(void)
{
    static const char *const prefixes[] = { "RECOMP_", "BO3_", NULL };
    static const char *const expect[] = {
        "README.txt", "system.txt", "settings.toml", "settings-effective.txt", "stats.txt",
        "logs/current.txt", "logs/previous-1.txt", "logs/previous-2.txt",
        "crash/20260102-030405-crash/report.txt", "crash/20260102-030405-crash/log.txt",
        "crash/20260102-030405-crash/settings.txt", "crash/20260101-000000-hang/report.txt",
        "frame.bmp", NULL,
    };
    xbox_bug_report_opts o;
    char path[1200] = "";
    const member *m;
    int i, k;

    xbox_report_env_snapshot();
    xbox_report_section("system.txt", "gpu", extra_section);
    xbox_report_attach("settings.toml", settings_path, 65536, 0);
    xbox_report_set_capture(capture_stub);

    memset(&o, 0, sizeof o);
    o.name = "fixture";
    o.title = "Fixture title, built by " FAKE_FULL " on " FAKE_HOST;
    o.save_dir = "bug_report_fixture/save";
    o.out_dir = "bug_report_fixture/out";
    o.env_prefixes = prefixes;
    o.note = "made by the test in " FAKE_HOME;
    o.screenshot = 1;
    o.who = &k_who;
    CHECK(xbox_BugReport(&o, path, sizeof path));
    CHECK(strstr(path, "fixture-report-") && strstr(path, ".zip"));

    /* Under a fixed name for the unzip test (CMakeLists.txt). */
    remove("bug_report_fixture/out/fixture.zip");
    CHECK(rename(path, "bug_report_fixture/out/fixture.zip") == 0);
    s_members = 0;
    CHECK(read_zip("bug_report_fixture/out/fixture.zip"));
    for (i = 0; expect[i]; i++) {
        if (!find(expect[i]))
            fprintf(stderr, "missing member %s\n", expect[i]);
        CHECK(find(expect[i]) != NULL);
    }
    CHECK(!find("logs/previous-3.txt"));       /* no such run */
    for (i = 0; i < s_members; i++) {
        CHECK(!strstr(s_m[i].name, "minidump"));
        CHECK(!strstr(s_m[i].name, "UDATA"));
        CHECK(!strstr(s_m[i].name, "pipelines"));
    }

    /* The privacy assertion: in no member, in no member's name, and not in
     * the archive's own bytes (a stored member is its text). */
    for (k = 0; k_forbidden[k]; k++) {
        for (i = 0; i < s_members; i++) {
            if (!strcmp(s_m[i].name, "frame.bmp"))
                continue;
            if (mem_has(s_m[i].data, s_m[i].size, k_forbidden[k])) {
                fprintf(stderr, "\"%s\" is in %s\n", k_forbidden[k], s_m[i].name);
                s_fail++;
            }
            s_checks++;
            CHECK(!strstr(s_m[i].name, k_forbidden[k]));
        }
        CHECK(!mem_has(s_zip, s_zip_n, k_forbidden[k]));
    }

    /* And what the redaction leaves is still readable. */
    CHECK(has("logs/current.txt", "[SAVE] dir: ~/.local/share/burnout3"));
    CHECK(has("logs/current.txt", "opened C:~\\AppData\\Roaming\\Burnout3\\x.bin"));
    CHECK(has("logs/current.txt", "as <user> on <host>"));
    CHECK(has("logs/current.txt", "owner <user>, mail <user>@"));
    CHECK(has("logs/current.txt", "prefix Z:~\\.steam and \"~\""));
    CHECK(has("logs/previous-1.txt", "PREVIOUS-RUN-ONE ~"));
    CHECK(has("logs/previous-2.txt", "PREVIOUS-RUN-TWO <host>"));
    CHECK(has("settings.toml", "shader_cache = \"~/.cache/burnout3\""));
    CHECK(has("settings.toml", "fullscreen = true"));
    CHECK(has("README.txt", "Fixture title, built by <user> on <host>"));
    CHECK(has("README.txt", "made by the test in ~"));
    CHECK(has("README.txt", "minidump of 14 bytes"));
    CHECK(has("README.txt", "the last frame the title presented"));

    /* The long log: its start, the note, its end; not its middle. */
    m = find("logs/current.txt");
    CHECK(m && m->size < 64u * 1024u + 256u * 1024u + 4096u);
    CHECK(has("logs/current.txt", "[SETTINGS] ~/.local/share/burnout3/settings.toml"));
    CHECK(has("logs/current.txt", "bytes left out of the report"));
    CHECK(has("logs/current.txt", "[TRACE] line 000001 from ~/x by <user>") == 0);
    if (m && m->size > 80) {
        const char *end = (const char *)m->data + m->size - 60;
        CHECK(strstr(end, "from ~/x by ") != NULL);
    }

    /* system.txt: the machine, the registered section, the log's facts. */
    CHECK(has("system.txt", "[build]"));
    CHECK(has("system.txt", "toolkit: xboxrecomp "));
    CHECK(has("system.txt", "\nos: "));
    CHECK(has("system.txt", "\ncpu: "));
    CHECK(has("system.txt", "\nmemory: "));
    CHECK(has("system.txt", "gpu: Fake GPU 9000, cache at ~/.cache/burnout3"));
    CHECK(has("system.txt", "device up: Fake GPU 9000 (Vulkan 1.3.280"));
    CHECK(has("system.txt", "[PRESENT] display 0 mode 1920x1080 at 120 Hz"));
    CHECK(has("system.txt", "[INPUT] controller \"Fake Pad\" 1234:5678"));
    /* The cache: its size, not its contents. */
    CHECK(has("system.txt", "shader cache: bug_report_fixture/cache: 1 files, 20 bytes"));

    /* The variables: the host's own, with their source; no other. */
    CHECK(has("settings-effective.txt", "RECOMP_SAVE_DIR=~/.local/share/burnout3   (environment)"));
    CHECK(has("settings-effective.txt", "BO3_FIXTURE_VAR=kept-bo3-value <user>   (environment)"));
    CHECK(!has("settings-effective.txt", "SECRET_TOKEN"));
    CHECK(!has("settings-effective.txt", "PATH="));

    /* The crash reports: both, the newest's settings without SDL_. */
    CHECK(has("crash/20260102-030405-crash/report.txt", "CRASH-REPORT-NEW signal 11 in ~/bin"));
    CHECK(has("crash/20260101-000000-hang/report.txt", "CRASH-REPORT-OLD hang on <host>"));
    CHECK(has("crash/20260102-030405-crash/settings.txt", "RECOMP_KEPT_IN_CRASH=1"));
    CHECK(has("crash/20260102-030405-crash/settings.txt", "BO3_ALSO_KEPT=1"));
    CHECK(has("crash/20260102-030405-crash/settings.txt", "RECOMP_VBLANK=1"));
    CHECK(has("crash/20260102-030405-crash/settings.txt", "command line: ~/bin/burnout3"));

    /* The frame: as the capture wrote it, untouched by the redaction. */
    m = find("frame.bmp");
    CHECK(m && m->size == 50 && !memcmp(m->data, "BM-FAKE-FRAME", 13)
          && mem_has(m->data, m->size, "/home/untouched"));

    /* Small: three quarters of a megabyte of log went in. */
    CHECK(s_zip_n < 200u * 1024u);
    fprintf(stderr, "archive: %lu bytes, %d members\n", (unsigned long)s_zip_n, s_members);

    /* A report with no title running and nothing to read still comes out. */
    memset(&o, 0, sizeof o);
    o.name = "empty";
    o.save_dir = "bug_report_fixture/nothing-here";
    o.out_dir = "bug_report_fixture/out";
    o.no_current_run = 1;
    o.who = &k_who;
    xbox_report_set_capture(NULL);
    CHECK(xbox_BugReport(&o, path, sizeof path));
    s_members = 0;
    CHECK(read_zip(path));
    CHECK(find("README.txt") && find("system.txt") && !find("frame.bmp"));
    CHECK(!find("logs/current.txt") && !find("logs/last-run.txt"));
    CHECK(has("README.txt", "no log of an earlier run was found"));
    remove(path);

    /* No save folder: refused, with the reason. */
    memset(&o, 0, sizeof o);
    CHECK(!xbox_BugReport(&o, path, sizeof path) && strstr(path, "no save folder"));
}

int main(void)
{
    make_fixture();
    test_redact();
    test_roundtrip();
    test_report();
    printf("bug_report: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

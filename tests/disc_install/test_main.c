/*
 * disc_install test: the disc image reader, the manifest check, the journal
 * and the marker (src/install), on a disc made here.
 *
 * The fixture is a five-file XDVDFS image built in memory, with an
 * executable that has a certificate (title id, title, region), and a
 * manifest computed from the same bytes. No game data. The cases: the image
 * as an xiso and behind a redump's video partition; a file that is not an
 * image, a truncated one, one with an unsafe name, one whose directory loops;
 * an install, a second run of it, a cancelled one gone on with, a file half
 * there, a journal cut short and one of another manifest; another title,
 * another version, an Xbox 360 disc, a damaged and an incomplete image, a
 * folder source; no space, no permission; a marker with a file gone.
 */
#include "disc_install.h"
#include "xdvdfs.h"
#include "xr_sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define t_mkdir(p) _mkdir(p)
#define RMRF "rmdir /s /q "
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#define t_mkdir(p) mkdir((p), 0755)
#define RMRF "rm -rf "
#endif

#define ROOT "disc_install_fixture"
#define S    XDVDFS_SECTOR

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── the fixture disc ──────────────────────────────────────────────── */

typedef struct {
    const char *path;
    uint8_t    *data;
    uint32_t    size;
    uint32_t    sector;
} fx_file;

#define FX_MAX 8
typedef struct {
    fx_file  f[FX_MAX];
    unsigned n;
} fx_disc;

static uint8_t *pattern(uint32_t size, unsigned seed)
{
    uint8_t *p = (uint8_t *)malloc(size ? size : 1);
    uint32_t i, x = seed * 2654435761u + 1;

    for (i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
    return p;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* An executable as far as the installer reads one: the header's magic, base
 * and certificate address, and the certificate's title id, title and region. */
static uint8_t *make_xbe(uint32_t size, uint32_t title_id, const char *title, uint32_t region,
                         unsigned seed)
{
    uint8_t *p = pattern(size, seed);
    unsigned i;

    memset(p, 0, 0x400);
    memcpy(p, "XBEH", 4);
    put32(p + 0x104, 0x10000);
    put32(p + 0x118, 0x10000 + 0x200);
    put32(p + 0x200 + 8, title_id);
    for (i = 0; title[i] && i < 39; i++)
        p[0x200 + 0x0C + i * 2] = (uint8_t)title[i];
    put32(p + 0x200 + 0xA0, region);
    return p;
}

static void fx_add(fx_disc *d, const char *path, uint8_t *data, uint32_t size)
{
    d->f[d->n].path = path;
    d->f[d->n].data = data;
    d->f[d->n].size = size;
    d->n++;
}

#define TITLE_ID 0x4541005B

static void fx_default(fx_disc *d)
{
    memset(d, 0, sizeof *d);
    fx_add(d, "default.xbe", make_xbe(8192, TITLE_ID, "Fixture Racer", 1, 1), 8192);
    fx_add(d, "Data/a.bin", pattern(3000, 2), 3000);
    fx_add(d, "Data/Sub/b.bin", pattern((1u << 20) * 3 + 17, 3), (1u << 20) * 3 + 17);
    fx_add(d, "Tracks/empty.dat", pattern(0, 4), 0);
    fx_add(d, "readme.txt", pattern(100, 5), 100);
}

/* The directory `dir` ("" the root) of the disc as a table. The entries hang
 * off each other by the left link and the right link in turn; `hostile`
 * replaces the first name. Returns the table's size. */
static uint32_t fx_table(const fx_disc *d, const char *dir, uint8_t *out, const uint32_t *dir_sector,
                         const uint32_t *dir_size, const char *const *dirs, unsigned ndirs,
                         const char *hostile)
{
    size_t dl = strlen(dir);
    uint32_t off = 0, prev = 0;
    unsigned i, k = 0, pass;
    int have_prev = 0;

    for (pass = 0; pass < 2; pass++) {
        unsigned count = pass ? d->n : ndirs;
        for (i = 0; i < count; i++) {
            const char *path = pass ? d->f[i].path : dirs[i];
            const char *name;
            uint32_t start, size, len;
            if (!*path || strncmp(path, dir, dl) || (dl && path[dl] != '/'))
                continue;
            name = path + dl + (dl ? 1 : 0);
            if (!*name || strchr(name, '/'))
                continue;
            if (hostile && !k)
                name = hostile;
            start = pass ? d->f[i].sector : dir_sector[i];
            size = pass ? d->f[i].size : dir_size[i];
            len = (uint32_t)strlen(name);
            if (out) {
                memset(out + off, 0, 14);
                put32(out + off + 4, start);
                put32(out + off + 8, size);
                out[off + 12] = pass ? 0x20 : 0x10;
                out[off + 13] = (uint8_t)len;
                memcpy(out + off + 14, name, len);
                if (have_prev) {
                    uint8_t *link = out + prev + (k & 1 ? 0 : 2);
                    link[0] = (uint8_t)(off / 4);
                    link[1] = (uint8_t)(off / 4 >> 8);
                }
            }
            prev = off;
            have_prev = 1;
            k++;
            off += 14 + len;
            while (off & 3) {
                if (out)
                    out[off] = 0xFF;
                off++;
            }
        }
    }
    return off;
}

/* The disc as an image file with its game partition at `base`. */
static void fx_write(fx_disc *d, const char *path, uint64_t base, const char *hostile, int loop,
                     uint64_t cut)
{
    static const char *const dirs[] = { "", "Data", "Data/Sub", "Tracks" };
    enum { ND = 4 };
    uint32_t dir_sector[ND], dir_size[ND], next = 33;
    uint8_t sec[S], *table;
    unsigned i;
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    for (i = 0; i < ND; i++) {
        dir_size[i] = fx_table(d, dirs[i], NULL, dir_sector, dir_size, dirs, ND, NULL);
        dir_sector[i] = next;
        next += (dir_size[i] + S - 1) / S + 1;
    }
    for (i = 0; i < d->n; i++) {
        d->f[i].sector = next;
        next += (d->f[i].size + S - 1) / S + 1;
    }
    memset(sec, 0, S);
    memcpy(sec, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(sec + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    put32(sec + 0x14, dir_sector[0]);
    put32(sec + 0x18, dir_size[0]);
#if defined(_WIN32)
    _fseeki64(f, (__int64)(base + 32 * S), SEEK_SET);
#else
    fseeko(f, (off_t)(base + 32 * S), SEEK_SET);
#endif
    fwrite(sec, 1, S, f);
    table = (uint8_t *)malloc(1 << 16);
    for (i = 0; i < ND; i++) {
        memset(table, 0xFF, 1 << 16);
        fx_table(d, dirs[i], table, dir_sector, dir_size, dirs, ND, i == 1 ? hostile : NULL);
        if (loop && i == 1) {           /* "Data": its second entry is its own child */
            uint32_t second = (14 + 3 + 3) & ~3u;       /* after "Sub" */
            table[second] = (uint8_t)(second / 4);
            table[second + 2] = (uint8_t)(second / 4);
        }
#if defined(_WIN32)
        _fseeki64(f, (__int64)(base + (uint64_t)dir_sector[i] * S), SEEK_SET);
#else
        fseeko(f, (off_t)(base + (uint64_t)dir_sector[i] * S), SEEK_SET);
#endif
        fwrite(table, 1, (dir_size[i] + S - 1) / S * S, f);
    }
    free(table);
    for (i = 0; i < d->n; i++) {
        uint32_t pad = (S - d->f[i].size % S) % S;
#if defined(_WIN32)
        _fseeki64(f, (__int64)(base + (uint64_t)d->f[i].sector * S), SEEK_SET);
#else
        fseeko(f, (off_t)(base + (uint64_t)d->f[i].sector * S), SEEK_SET);
#endif
        fwrite(d->f[i].data, 1, d->f[i].size, f);
        memset(sec, 0, S);
        fwrite(sec, 1, pad ? pad : S, f);
    }
    fclose(f);
    if (cut) {
#if defined(_WIN32)
        (void)cut;
#else
        struct stat st;
        if (stat(path, &st) == 0 && truncate(path, st.st_size - (off_t)cut) != 0)
            perror("truncate");
#endif
    }
}

/* ── the manifest of the fixture ───────────────────────────────────── */

static xr_manifest_file s_mf[FX_MAX];
static char s_hex[FX_MAX][65];

static void fx_manifest(const fx_disc *d, xr_manifest *m, const char *id)
{
    unsigned i;

    for (i = 0; i < d->n; i++) {
        uint8_t digest[32];
        xr_sha256 sha;
        xr_sha256_init(&sha);
        xr_sha256_update(&sha, d->f[i].data, d->f[i].size);
        xr_sha256_final(&sha, digest);
        xr_sha256_hex(digest, s_hex[i]);
        s_mf[i].path = d->f[i].path;
        s_mf[i].size = d->f[i].size;
        s_mf[i].sha256 = s_hex[i];
    }
    m->id = id;
    m->title = "Fixture Racer";
    m->build = "North America (NTSC-U) retail";
    m->title_id = TITLE_ID;
    m->exe = "default.xbe";
    m->files = s_mf;
    m->count = d->n;
}

/* ── helpers ───────────────────────────────────────────────────────── */

static int exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static int file_is(const char *p, const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(p, "rb");
    uint8_t *got;
    int same;

    if (!f)
        return 0;
    got = (uint8_t *)malloc(size + 1);
    same = fread(got, 1, size + 1, f) == size && !memcmp(got, data, size);
    fclose(f);
    free(got);
    return same;
}

static void put_file(const char *p, const void *data, size_t n, const char *mode)
{
    FILE *f = fopen(p, mode);
    if (!f) {
        fprintf(stderr, "cannot write %s\n", p);
        exit(2);
    }
    fwrite(data, 1, n, f);
    fclose(f);
}

static int s_blocks, s_cancel_at;
static double s_last;
static int s_progress_calls, s_progress_back;

static int cancel_cb(void *ctx)
{
    (void)ctx;
    return s_cancel_at && ++s_blocks >= s_cancel_at;
}

static void progress_cb(void *ctx, double f, const char *text)
{
    (void)ctx; (void)text;
    s_progress_calls++;
    if (f < s_last || f > 1.0)
        s_progress_back++;
    s_last = f;
}

static int install(const char *source, const char *dest, const xr_manifest *m, xr_install_result *r,
                   int cancel_at, uint64_t free_needed)
{
    xr_install_opts o;

    memset(&o, 0, sizeof o);
    o.source = source;
    o.source_fd = -1;
    o.dest = dest;
    o.subdir = "Game Files";
    o.manifest = m;
    o.version = "9.9.9";
    o.free_needed = free_needed;
    o.progress = progress_cb;
    o.cancelled = cancel_cb;
    s_blocks = 0;
    s_cancel_at = cancel_at;
    s_last = 0;
    s_progress_calls = s_progress_back = 0;
    return xr_install(&o, r);
}

static int installed(const fx_disc *d, const char *dest)
{
    char p[600];
    unsigned i;

    for (i = 0; i < d->n; i++) {
        snprintf(p, sizeof p, "%s/Game Files/%s", dest, d->f[i].path);
        if (!file_is(p, d->f[i].data, d->f[i].size))
            return 0;
    }
    snprintf(p, sizeof p, "%s/.complete", dest);
    return exists(p);
}

/* ── the tests ─────────────────────────────────────────────────────── */

static void test_sha256(void)
{
    static const char *const want_abc =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *const want_empty =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char *const want_million =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    static const char *const want_448 =
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    uint8_t digest[32], block[1000];
    char hex[65];
    xr_sha256 s;
    int i;

    xr_sha256_init(&s);
    xr_sha256_update(&s, "abc", 3);
    xr_sha256_final(&s, digest);
    xr_sha256_hex(digest, hex);
    CHECK(!strcmp(hex, want_abc));
    xr_sha256_init(&s);
    xr_sha256_final(&s, digest);
    xr_sha256_hex(digest, hex);
    CHECK(!strcmp(hex, want_empty));
    xr_sha256_init(&s);
    xr_sha256_update(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    xr_sha256_final(&s, digest);
    xr_sha256_hex(digest, hex);
    CHECK(!strcmp(hex, want_448));
    memset(block, 'a', sizeof block);
    xr_sha256_init(&s);
    for (i = 0; i < 1000; i++)          /* in pieces that straddle the block */
        xr_sha256_update(&s, block, sizeof block);
    xr_sha256_final(&s, digest);
    xr_sha256_hex(digest, hex);
    CHECK(!strcmp(hex, want_million));
}

static void test_reader(fx_disc *d)
{
    xdvdfs_image *img = NULL;
    const xdvdfs_file *f;
    char err[256];
    uint8_t *buf = (uint8_t *)malloc(d->f[2].size);
    unsigned i;

    fx_write(d, ROOT "/disc.iso", 0, NULL, 0, 0);
    CHECK(xdvdfs_open(ROOT "/disc.iso", &img, err, sizeof err) == XDVDFS_OK);
    if (img) {
        CHECK(xdvdfs_base(img) == 0);
        CHECK(xdvdfs_count(img) == d->n);
        for (i = 0; i + 1 < xdvdfs_count(img); i++)     /* sorted */
            CHECK(strcmp(xdvdfs_files(img)[i].path, xdvdfs_files(img)[i + 1].path) != 0);
        f = xdvdfs_find(img, "DATA\\sub/B.BIN");
        CHECK(f && f->size == d->f[2].size && !strcmp(f->path, "Data/Sub/b.bin"));
        CHECK(f && xdvdfs_read(img, f, 0, buf, f->size) && !memcmp(buf, d->f[2].data, f->size));
        CHECK(f && xdvdfs_read(img, f, 5000, buf, 100) && !memcmp(buf, d->f[2].data + 5000, 100));
        CHECK(f && !xdvdfs_read(img, f, f->size - 10, buf, 11));    /* past its end */
        f = xdvdfs_find(img, "Tracks/empty.dat");
        CHECK(f && f->size == 0);
        CHECK(!xdvdfs_find(img, "Data/Sub"));           /* a directory is not a file */
        CHECK(!xdvdfs_find(img, "nothing.bin"));
        xdvdfs_close(img);
    }

    /* A redump: the video partition in front (a hole in the file here). */
    fx_write(d, ROOT "/redump.iso", 0x18300000, NULL, 0, 0);
    CHECK(xdvdfs_open(ROOT "/redump.iso", &img, err, sizeof err) == XDVDFS_OK);
    if (img) {
        CHECK(xdvdfs_base(img) == 0x18300000);
        f = xdvdfs_find(img, "Data/a.bin");
        CHECK(f && xdvdfs_read(img, f, 0, buf, f->size) && !memcmp(buf, d->f[1].data, f->size));
        xdvdfs_close(img);
    }
    remove(ROOT "/redump.iso");

    put_file(ROOT "/notes.iso", d->f[2].data, d->f[2].size, "wb");
    CHECK(xdvdfs_open(ROOT "/notes.iso", &img, err, sizeof err) == XDVDFS_E_NOT_XBOX && !img);
    put_file(ROOT "/tiny.iso", "x", 1, "wb");
    CHECK(xdvdfs_open(ROOT "/tiny.iso", &img, err, sizeof err) == XDVDFS_E_NOT_XBOX);
    CHECK(xdvdfs_open(ROOT "/absent.iso", &img, err, sizeof err) == XDVDFS_E_OPEN);

    /* Names that would leave the folder. */
    fx_write(d, ROOT "/hostile.iso", 0, "..", 0, 0);
    CHECK(xdvdfs_open(ROOT "/hostile.iso", &img, err, sizeof err) == XDVDFS_E_CORRUPT && !img);
    fx_write(d, ROOT "/hostile.iso", 0, "../x.bin", 0, 0);
    CHECK(xdvdfs_open(ROOT "/hostile.iso", &img, err, sizeof err) == XDVDFS_E_CORRUPT);
    fx_write(d, ROOT "/hostile.iso", 0, "c:x", 0, 0);
    CHECK(xdvdfs_open(ROOT "/hostile.iso", &img, err, sizeof err) == XDVDFS_E_CORRUPT);
    fx_write(d, ROOT "/hostile.iso", 0, "a\\b", 0, 0);
    CHECK(xdvdfs_open(ROOT "/hostile.iso", &img, err, sizeof err) == XDVDFS_E_CORRUPT);

    /* A table whose links go round: the open ends. */
    fx_write(d, ROOT "/loop.iso", 0, NULL, 1, 0);
    i = (unsigned)xdvdfs_open(ROOT "/loop.iso", &img, err, sizeof err);
    CHECK(i == XDVDFS_OK || i == XDVDFS_E_CORRUPT);
    if (img)
        xdvdfs_close(img);
    img = NULL;

#if !defined(_WIN32)
    /* The last file's end is not in the image. */
    fx_write(d, ROOT "/short.iso", 0, NULL, 0, S + 60);
    CHECK(xdvdfs_open(ROOT "/short.iso", &img, err, sizeof err) == XDVDFS_E_TRUNCATED && !img);
    CHECK(strstr(err, "readme.txt") != NULL);
#endif
    free(buf);
}

#if !defined(_WIN32)
/* The source_open callback (an Android document tree): the folder at
 * ctx, files opened by descriptor, rel matched as given. */
static int tree_open(void *ctx, const char *rel, uint64_t *size)
{
    char path[700];
    struct stat st;
    int fd;

    snprintf(path, sizeof path, "%s/%s", (const char *)ctx, rel);
    if (stat(path, &st) != 0)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -2;
    *size = (uint64_t)st.st_size;
    return fd;
}

static int install_tree(const char *tree, const char *dest, const xr_manifest *m,
                        xr_install_result *r, int identify_only)
{
    xr_install_opts o;

    memset(&o, 0, sizeof o);
    o.source = "ignored";
    o.source_fd = -1;
    o.source_open = tree_open;
    o.ctx = (void *)tree;
    o.dest = dest;
    o.subdir = "Game Files";
    o.manifest = m;
    o.version = "test";
    o.identify_only = identify_only;
    return xr_install(&o, r);
}
#endif

static void test_install(fx_disc *d)
{
    xr_manifest m;
    xr_install_result r;
    xr_install_marker mark;
    char p[600];
    unsigned kept;

    fx_manifest(d, &m, "fixture-1");
    fx_write(d, ROOT "/disc.iso", 0, NULL, 0, 0);

    /* Only asked what it is: nothing is written. */
    {
        xr_install_opts o;
        memset(&o, 0, sizeof o);
        o.source = ROOT "/disc.iso";
        o.source_fd = -1;
        o.dest = ROOT "/never";
        o.manifest = &m;
        o.identify_only = 1;
        CHECK(xr_install(&o, &r) == XR_INSTALL_OK && r.image && !r.copied);
        CHECK(!exists(ROOT "/never/install.journal") && !exists(ROOT "/never/.complete"));
    }

    /* BO-229: a destination several missing folders deep (made from the
     * deepest one upward, no ancestor looked at first), with a trailing slash. */
    CHECK(install(ROOT "/disc.iso", ROOT "/deep/x/y/z/", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(installed(d, ROOT "/deep/x/y/z"));

    /* The whole install. */
    CHECK(install(ROOT "/disc.iso", ROOT "/a", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(r.copied == d->n && !r.verified && !r.kept && !r.already && r.image);
    CHECK(installed(d, ROOT "/a"));
    CHECK(!exists(ROOT "/a/install.journal"));
    CHECK(xr_install_marker_read(ROOT "/a", &mark) && !strcmp(mark.manifest, "fixture-1")
          && !strcmp(mark.version, "9.9.9") && mark.files == d->n
          && mark.bytes == xr_manifest_bytes(&m));
    CHECK(s_progress_calls > 0 && !s_progress_back && s_last == 1.0);
    /* Again: nothing to do. */
    CHECK(install(ROOT "/disc.iso", ROOT "/a", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(r.already && !r.copied && !r.verified);
    /* A file gone from under the marker: put back, the rest hashed in place. */
    remove(ROOT "/a/Game Files/Data/a.bin");
    CHECK(install(ROOT "/disc.iso", ROOT "/a", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(!r.already && r.copied == 1 && r.verified == d->n - 1);
    CHECK(installed(d, ROOT "/a"));
    /* A marker of another manifest is not this install. */
    fx_manifest(d, &m, "fixture-2");
    CHECK(install(ROOT "/disc.iso", ROOT "/a", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(!r.already && !r.copied && r.verified == d->n);
    CHECK(xr_install_marker_read(ROOT "/a", &mark) && !strcmp(mark.manifest, "fixture-2"));
    fx_manifest(d, &m, "fixture-1");

    /* Stopped in the big file, then gone on with. */
    CHECK(install(ROOT "/disc.iso", ROOT "/b", &m, &r, 7, 0) == XR_INSTALL_CANCELLED);
    CHECK(!strcmp(xr_install_code_name(r.code), "cancelled") && r.message[0]);
    CHECK(!exists(ROOT "/b/.complete") && exists(ROOT "/b/install.journal"));
    CHECK(!exists(ROOT "/b/Game Files/Data/Sub/b.bin.part"));
    CHECK(!exists(ROOT "/b/Game Files/Data/Sub/b.bin"));
    kept = r.copied;
    CHECK(kept == 2);
    CHECK(xr_install_marker_read(ROOT "/b", &mark) == 0);
    CHECK(install(ROOT "/disc.iso", ROOT "/b", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(r.kept == kept && r.copied == d->n - kept && !r.verified);
    CHECK(installed(d, ROOT "/b") && !exists(ROOT "/b/install.journal"));

    /* Killed: a .part left, a whole file without its journal line, a file of
     * the right size and the wrong bytes, a journal line cut short. */
    CHECK(install(ROOT "/disc.iso", ROOT "/c", &m, &r, 7, 0) == XR_INSTALL_CANCELLED);
    put_file(ROOT "/c/Game Files/Data/Sub/b.bin.part", "half", 4, "wb");
    put_file(ROOT "/c/Game Files/readme.txt", d->f[4].data, d->f[4].size, "wb");
    t_mkdir(ROOT "/c/Game Files/Tracks");
    {
        uint8_t *wrong = pattern(d->f[2].size, 77);
        put_file(ROOT "/c/Game Files/Data/Sub/b.bin", wrong, d->f[2].size, "wb");
        free(wrong);
    }
    put_file(ROOT "/c/install.journal", "ok Data/Sub/b.b", 15, "ab");
    CHECK(install(ROOT "/disc.iso", ROOT "/c", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(r.kept == kept && r.verified == 1 && r.copied == d->n - kept - 1);
    CHECK(installed(d, ROOT "/c"));
    CHECK(!exists(ROOT "/c/Game Files/Data/Sub/b.bin.part"));

    /* A journal of another manifest counts for nothing: all hashed in place. */
    CHECK(install(ROOT "/disc.iso", ROOT "/d", &m, &r, 0, 0) == XR_INSTALL_OK);
    remove(ROOT "/d/.complete");
    put_file(ROOT "/d/install.journal", "xr-install-journal 1 other\nok Data/a.bin\n", 41, "wb");
    CHECK(install(ROOT "/disc.iso", ROOT "/d", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(!r.kept && r.verified == d->n && !r.copied);

    /* A folder, wrapped one level down. */
    t_mkdir(ROOT "/wrap");
#if defined(_WIN32)
    CHECK(rename(ROOT "/d/Game Files", ROOT "/wrap/Game Files") == 0);
#else
    CHECK(rename(ROOT "/d/Game Files", ROOT "/wrap/GAME") == 0);
#endif
    put_file(ROOT "/wrap/notes.txt", "x", 1, "wb");
    CHECK(install(ROOT "/wrap", ROOT "/e", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(!r.image && r.copied == d->n && installed(d, ROOT "/e"));
    /* The installed folder as its own source: checked where it lies. */
    remove(ROOT "/e/.complete");
    CHECK(install(ROOT "/e/Game Files", ROOT "/e", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(!r.copied && r.verified == d->n && installed(d, ROOT "/e"));
    /* A folder with a file missing. */
    remove(ROOT "/e/Game Files/readme.txt");
    CHECK(install(ROOT "/e/Game Files", ROOT "/f", &m, &r, 0, 0) == XR_INSTALL_DAMAGED);
    CHECK(strstr(r.message, "readme.txt") && !exists(ROOT "/f/.complete"));
    t_mkdir(ROOT "/empty");
    CHECK(install(ROOT "/empty", ROOT "/f", &m, &r, 0, 0) == XR_INSTALL_NO_EXE);
    CHECK(install(ROOT "/absent.iso", ROOT "/f", &m, &r, 0, 0) == XR_INSTALL_NO_SOURCE);
    CHECK(install(ROOT "/notes.iso", ROOT "/f", &m, &r, 0, 0) == XR_INSTALL_NOT_XBOX);
    CHECK(!strcmp(xr_install_code_name(r.code), "not-xbox-image") && strstr(r.message, "notes.iso"));

#if !defined(_WIN32)
    /* A source_open tree: the same checks and the same resume as a folder. */
    CHECK(install_tree(ROOT "/wrap/GAME", ROOT "/t1", &m, &r, 1) == XR_INSTALL_OK && !r.copied);
    CHECK(!exists(ROOT "/t1/.complete"));
    CHECK(install_tree(ROOT "/wrap/GAME", ROOT "/t1", &m, &r, 0) == XR_INSTALL_OK);
    CHECK(!r.image && r.copied == d->n && installed(d, ROOT "/t1"));
    remove(ROOT "/t1/.complete");           /* stopped late: all checked in place */
    CHECK(install_tree(ROOT "/wrap/GAME", ROOT "/t1", &m, &r, 0) == XR_INSTALL_OK);
    CHECK(!r.copied && r.verified == d->n);
    CHECK(install_tree(ROOT "/empty", ROOT "/t2", &m, &r, 0) == XR_INSTALL_NO_EXE);
    CHECK(strstr(r.message, "not an extracted Xbox disc") != NULL);
    CHECK(install_tree(ROOT "/e/Game Files", ROOT "/t3", &m, &r, 0) == XR_INSTALL_DAMAGED);
    CHECK(strstr(r.message, "readme.txt") && strstr(r.message, "folder")
          && !exists(ROOT "/t3/.complete"));
#endif

    /* No room. */
    CHECK(install(ROOT "/disc.iso", ROOT "/g", &m, &r, 0, (uint64_t)1 << 60) == XR_INSTALL_DISK_FULL);
    /* The words give the two numbers apart: what must be free, and what the game uses. */
    CHECK(strstr(r.message, "needs ") && strstr(r.message, " GB free (the game uses ")
          && strstr(r.message, " GB is free. Free up space on this disk.")
          && !strstr(r.message, " more,") && !strstr(r.message, "another folder"));
    CHECK(!exists(ROOT "/g/.complete") && !exists(ROOT "/g/Game Files/default.xbe"));
#if !defined(_WIN32)
    /* A folder that cannot be written (not as root: it can write anywhere). */
    if (geteuid() != 0) {
        t_mkdir(ROOT "/locked");
        chmod(ROOT "/locked", 0555);
        CHECK(install(ROOT "/disc.iso", ROOT "/locked/x", &m, &r, 0, 0) == XR_INSTALL_NO_PERMISSION);
        CHECK(strstr(r.message, "permission") != NULL);
        chmod(ROOT "/locked", 0755);
    }
    /* An image that ends early. */
    fx_write(d, ROOT "/short.iso", 0, NULL, 0, S + 60);
    CHECK(install(ROOT "/short.iso", ROOT "/h", &m, &r, 0, 0) == XR_INSTALL_TRUNCATED);
    CHECK(!exists(ROOT "/h/install.journal"));
#endif
    snprintf(p, sizeof p, "%s", r.message);
    CHECK(!strchr(p, '\n'));
}

static void test_identity(fx_disc *d)
{
    xr_manifest m;
    xr_install_result r;
    fx_disc o;
    uint8_t *keep;

    fx_manifest(d, &m, "fixture-1");

    /* Another title. */
    o = *d;
    o.f[0].data = make_xbe(8192, 0x4D530004, "Another Game", 4, 9);
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/i", &m, &r, 0, 0) == XR_INSTALL_WRONG_GAME);
    CHECK(strstr(r.message, "Another Game") && strstr(r.message, "0x4D530004"));
    CHECK(!exists(ROOT "/i/install.journal") && !exists(ROOT "/i/Game Files/default.xbe"));
    free(o.f[0].data);
    /* The same title of another region. */
    o.f[0].data = make_xbe(8192, TITLE_ID, "Fixture Racer", 4, 10);
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/i", &m, &r, 0, 0) == XR_INSTALL_WRONG_VERSION);
    CHECK(strstr(r.message, "Europe") && !strcmp(xr_install_code_name(r.code), "wrong-version"));
    free(o.f[0].data);
    /* Not an executable at all. */
    o.f[0].data = pattern(8192, 11);
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/i", &m, &r, 0, 0) == XR_INSTALL_DAMAGED);
    free(o.f[0].data);
    /* An Xbox 360 disc; a disc with neither. */
    o = *d;
    o.f[0].path = "default.xex";
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/i", &m, &r, 0, 0) == XR_INSTALL_XBOX360);
    o.f[0].path = "game.bin";
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/i", &m, &r, 0, 0) == XR_INSTALL_NO_EXE);

    /* The right disc with one byte of one file changed. */
    o = *d;
    keep = o.f[2].data;
    o.f[2].data = (uint8_t *)malloc(o.f[2].size);
    memcpy(o.f[2].data, keep, o.f[2].size);
    o.f[2].data[o.f[2].size / 2] ^= 1;
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/j", &m, &r, 0, 0) == XR_INSTALL_DAMAGED);
    CHECK(strstr(r.message, "Data/Sub/b.bin") != NULL);
    CHECK(!exists(ROOT "/j/.complete") && !exists(ROOT "/j/Game Files/Data/Sub/b.bin")
          && !exists(ROOT "/j/Game Files/Data/Sub/b.bin.part"));
    free(o.f[2].data);
    /* A file of another size; one that is not there. */
    o.f[2].data = keep;
    o.f[2].size -= 1;
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/k", &m, &r, 0, 0) == XR_INSTALL_DAMAGED);
    o = *d;
    o.f[4].path = "Data/renamed.txt";
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/k", &m, &r, 0, 0) == XR_INSTALL_DAMAGED);
    CHECK(strstr(r.message, "readme.txt") != NULL);

    /* A file the manifest does not list is left where it is. */
    o = *d;
    fx_add(&o, "Data/extra.bin", pattern(10, 12), 10);
    fx_write(&o, ROOT "/other.iso", 0, NULL, 0, 0);
    CHECK(install(ROOT "/other.iso", ROOT "/l", &m, &r, 0, 0) == XR_INSTALL_OK);
    CHECK(r.extra == 1 && r.copied == d->n && installed(d, ROOT "/l"));
    CHECK(!exists(ROOT "/l/Game Files/Data/extra.bin"));
}

int main(void)
{
    fx_disc d;

    if (system(RMRF ROOT) != 0) {
        /* not there yet */
    }
    t_mkdir(ROOT);
    fx_default(&d);
    test_sha256();
    test_reader(&d);
    test_install(&d);
    test_identity(&d);
    printf("disc_install: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/* nv2a_texscale: the 2D art scalers and the rule that picks what they scale,
 * on synthetic textures. No GPU. */
#include "nv2a_texscale.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utime.h>

static int s_checks, s_fail;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define W 16
#define H 12

/* nv2a_texscale_key of each filter's result on filters()'s picture. */
#define GOLD_SX2 0x8a0467f21dd93aa1ull
#define GOLD_SX4 0x2c2937aeb7ca42daull
#define GOLD_BC2 0x282828e050032610ull
#define GOLD_BC4 0x471009c9b50af0eeull

#define A(c) ((c) >> 24)
#define R(c) (((c) >> 16) & 0xFFu)
#define G(c) (((c) >> 8) & 0xFFu)
#define B(c) ((c) & 0xFFu)

static int near(uint32_t a, uint32_t b, int tol)
{
    int s;
    for (s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > tol || d < -tol)
            return 0;
    }
    return 1;
}

/* An opaque white block (4..11, 3..8) on fully transparent texels of colour
 * `under`. */
static void block(uint32_t *t, uint32_t under)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = (x >= 4 && x < 12 && y >= 3 && y < 9) ? 0xFFFFFFFFu
                                                               : (under & 0x00FFFFFFu);
}

static void scaler(enum nv2a_texscale_algo algo, unsigned f)
{
    static uint32_t src[W * H], src2[W * H], d1[W * H * 16], d2[W * H * 16];
    const int OW = W * (int)f, OH = H * (int)f;
    int x, y, i, bad;

    /* One colour in, the same colour out, everywhere (edges included). */
    for (i = 0; i < W * H; i++)
        src[i] = 0xFF3C7AC8u;
    memset(d1, 0, sizeof d1);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += d1[i] != 0xFF3C7AC8u;
    CHECK(bad == 0);
    /* ... and a half-transparent one. */
    for (i = 0; i < W * H; i++)
        src[i] = 0x80FF8000u;
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += !near(d1[i], 0x80FF8000u, 1);
    CHECK(bad == 0);

    /* Alpha edges: white on transparent black. No visible texel is darker
     * than white (no fringe), the alpha stays within the block's reach, and
     * the transparent texels beside the block are white under their zero
     * alpha, so the GPU's bilinear filter has nothing dark to mix in. */
    block(src, 0x000000u);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    {
        int dark = 0, far_alpha = 0, inside = 0, bled = 0, kept = 0;
        for (y = 0; y < OH; y++) {
            for (x = 0; x < OW; x++) {
                uint32_t c = d1[y * OW + x];
                int sx = x / (int)f, sy = y / (int)f;
                if (A(c) && (R(c) < 250 || G(c) < 250 || B(c) < 250))
                    dark++;
                /* more than one source texel away from the block: nothing */
                if ((sx < 3 || sx > 12 || sy < 2 || sy > 9) && A(c))
                    far_alpha++;
                /* well inside the block: opaque */
                if (sx >= 6 && sx < 10 && sy >= 5 && sy < 7 && c != 0xFFFFFFFFu)
                    inside++;
                if (!A(c)) {
                    int nb = 0, dx, dy;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++)
                            if (x + dx >= 0 && x + dx < OW && y + dy >= 0 && y + dy < OH)
                                nb += A(d1[(y + dy) * OW + x + dx]) != 0;
                    if (nb && (c & 0x00FFFFFFu) != 0x00FFFFFFu)
                        bled++;
                    if (!nb && c != 0)
                        kept++;       /* the source's own colour, here black */
                }
            }
        }
        CHECK(dark == 0);
        CHECK(far_alpha == 0);
        CHECK(inside == 0);
        CHECK(bled == 0);
        CHECK(kept == 0);
        /* The block's coverage is kept: the alpha sums to the block's area. */
        {
            unsigned long sum = 0;
            for (i = 0; i < OW * OH; i++)
                sum += A(d1[i]);
            CHECK(sum > 255ul * 48 * f * f * 95 / 100 && sum < 255ul * 48 * f * f * 105 / 100);
        }
    }
    /* What colour the transparent texels have has no say in what is seen. */
    block(src2, 0xFF00FFu);
    CHECK(nv2a_texscale(src2, W, H, f, algo, d2) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += A(d1[i]) != A(d2[i]) || (A(d1[i]) && d1[i] != d2[i]);
    CHECK(bad == 0);
    /* ... and far from anything visible they keep it (alpha 0). */
    CHECK(d2[0] == 0x00FF00FFu && d2[OW * OH - 1] == 0x00FF00FFu);

    /* Placement: the block is symmetric about the texture's middle both
     * ways, so the result is too. A scaler that leaves the picture a part of
     * a texel off its place fails this. */
    for (bad = 0, y = 0; y < OH; y++)
        for (x = 0; x < OW; x++)
            bad += !near(d1[y * OW + x], d1[y * OW + (OW - 1 - x)], 1)
                || !near(d1[y * OW + x], d1[(OH - 1 - y) * OW + x], 1);
    CHECK(bad == 0);

    /* Borders: an opaque red left column and nothing else. The right edge
     * stays empty (no wrap), the left edge is red to the texture's edge
     * (the edge texel repeats past it, it does not fade). */
    for (i = 0; i < W * H; i++)
        src[i] = (i % W) == 0 ? 0xFFFF0000u : 0u;
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, y = 0; y < OH; y++) {
        bad += A(d1[y * OW + OW - 1]) != 0;
        bad += d1[y * OW] != 0xFFFF0000u;
    }
    CHECK(bad == 0);

    /* A ramp stays a ramp: each source texel's block averages to the texel
     * (the result lies over the source, and nothing rings). */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            src[y * W + x] = 0xFF000000u | (uint32_t)(x * 16) << 16 | (uint32_t)(x * 16) << 8
                           | (uint32_t)(x * 16);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, x = 2; x < W - 2; x++) {
        int sum = 0, k;
        for (k = 0; k < (int)f; k++)
            sum += (int)B(d1[(OH / 2) * OW + x * (int)f + k]);
        sum = (sum + (int)f / 2) / (int)f;
        bad += sum < x * 16 - 2 || sum > x * 16 + 2;
    }
    CHECK(bad == 0);
    for (bad = 0, x = 1; x < OW; x++)
        bad += B(d1[(OH / 2) * OW + x]) < B(d1[(OH / 2) * OW + x - 1]);
    CHECK(bad == 0);                                   /* monotonic */
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += A(d1[i]) != 255;
    CHECK(bad == 0);                                   /* opaque stays opaque */

    /* Determinism: a noisy picture twice. */
    {
        uint32_t seed = 12345u;
        for (i = 0; i < W * H; i++) {
            seed = seed * 1664525u + 1013904223u;
            src[i] = seed;
        }
        CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
        CHECK(nv2a_texscale(src, W, H, f, algo, d2) == 1);
        CHECK(!memcmp(d1, d2, (size_t)OW * OH * 4));
        /* a visible texel's colour never exceeds what its neighbourhood has:
         * spot check that nothing is out of range (to8 clamps; alpha 0 texels
         * carry no alpha) */
        for (bad = 0, i = 0; i < OW * OH; i++)
            bad += A(d1[i]) == 0 && (d1[i] >> 24) != 0;
        CHECK(bad == 0);
    }
    /* A 1x1 and a 1-wide texture do not read outside themselves. */
    {
        uint32_t one = 0xC0102030u, o[16], col[5] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF000000u,
                                                      0xFFFFFFFFu, 0xFF000000u }, oc[5 * 16];
        CHECK(nv2a_texscale(&one, 1, 1, f, algo, o) == 1);
        for (bad = 0, i = 0; i < (int)(f * f); i++)
            bad += !near(o[i], one, 1);
        CHECK(bad == 0);
        CHECK(nv2a_texscale(col, 1, 5, f, algo, oc) == 1);
        for (bad = 0, y = 0; y < 5 * (int)f; y++)
            for (x = 1; x < (int)f; x++)
                bad += oc[y * (int)f + x] != oc[y * (int)f];
        CHECK(bad == 0);
    }
}

/* The two scalers on one small picture: each gives its own result, and the
 * result's checksum is the recorded one (the same bytes on every machine:
 * no fused multiply-add and no library function). */
static void filters(void)
{
    static const uint64_t want[NV2A_TEXSCALE_ALGOS][2] = {
        /* 2x, 4x */
        { GOLD_SX2, GOLD_SX4 },
        { GOLD_BC2, GOLD_BC4 },
    };
    static uint32_t src[W * H], out[NV2A_TEXSCALE_ALGOS][W * H * 16];
    int x, y, a, b, f;

    /* A disc with a soft rim, a diagonal bar and a one-texel line on a clear
     * ground: curves, slopes, antialiasing and alpha at once. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int dx = 2 * x - 11, dy = 2 * y - 11, d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            if (d2 < 60)
                c = 0xFFE0A020u;
            else if (d2 < 90)
                c = 0x80E0A020u;
            if (x - y == 7 || x - y == 8)
                c = 0xFF2040C0u;
            if (y == 10 && x > 2)
                c = 0xFFFFFFFFu;
            src[y * W + x] = c;
        }
    }
    for (f = 0; f < 2; f++) {
        unsigned factor = f ? 4u : 2u;
        size_t n = (size_t)W * H * factor * factor;
        for (a = 0; a < NV2A_TEXSCALE_ALGOS; a++) {
            uint64_t sum;
            CHECK(nv2a_texscale(src, W, H, factor, (enum nv2a_texscale_algo)a, out[a]) == 1);
            sum = nv2a_texscale_key(out[a], W * factor, H * factor);
            if (sum != want[a][f])
                printf("  %s %ux: checksum %016llx, recorded %016llx\n",
                       nv2a_texscale_algo_name((enum nv2a_texscale_algo)a), factor,
                       (unsigned long long)sum, (unsigned long long)want[a][f]);
            CHECK(sum == want[a][f]);
            for (b = 0; b < a; b++)
                CHECK(memcmp(out[a], out[b], n * 4) != 0);
        }
    }
    /* The names, both ways. */
    CHECK(!strcmp(nv2a_texscale_algo_name(NV2A_TEXSCALE_SUPERXBR), "super-xbr"));
    CHECK(!strcmp(nv2a_texscale_algo_name(NV2A_TEXSCALE_BICUBIC), "bicubic"));
    for (a = 0; a < NV2A_TEXSCALE_ALGOS; a++)
        CHECK(nv2a_texscale_algo_named(nv2a_texscale_algo_name((enum nv2a_texscale_algo)a))
              == (enum nv2a_texscale_algo)a);
    CHECK(nv2a_texscale_algo_named("mmpx") == NV2A_TEXSCALE_SUPERXBR);
    CHECK(nv2a_texscale_algo_named(NULL) == NV2A_TEXSCALE_SUPERXBR);
}

/* The store: a scaled picture comes from a worker thread, and from the disk
 * cache when it is there. dir: a scratch directory (NULL: no disk tests). */
static void wait_ready(uint64_t key, uint32_t w, uint32_t h, unsigned f, uint32_t *dst,
                       int *r)
{
    int i;
    CHECK(nv2a_texscale_flush(20000) == 1);
    for (i = 0; i < 2000; i++) {
        *r = nv2a_texscale_get(key, NULL, w, h, f, NV2A_TEXSCALE_SUPERXBR, 0, dst);
        if (*r != NV2A_TEXSCALE_PENDING)
            return;
    }
}

static void wait_ready_algo(uint64_t key, uint32_t w, uint32_t h, unsigned f,
                            enum nv2a_texscale_algo algo, uint32_t *dst, int *r)
{
    int i;
    CHECK(nv2a_texscale_flush(20000) == 1);
    for (i = 0; i < 2000; i++) {
        *r = nv2a_texscale_get(key, NULL, w, h, f, algo, 0, dst);
        if (*r != NV2A_TEXSCALE_PENDING)
            return;
    }
}

static void store(const char *dir)
{
    static uint32_t src[W * H], other[W * H], ref[W * H * 16], got[W * H * 16];
    struct nv2a_texscale_store_stats st;
    char path[1200], found[1000];
    uint64_t key, key2;
    unsigned done, total;
    FILE *f;
    int r, i;

    block(src, 0x000000u);
    block(other, 0x000000u);
    other[5 * W + 6] = 0xFF112233u;
    key = nv2a_texscale_key(src, W, H);
    key2 = nv2a_texscale_key(other, W, H);
    CHECK(key != 0 && key == nv2a_texscale_key(src, W, H) && key != key2);
    CHECK(key != nv2a_texscale_key(src, H, W));          /* the size is in it */
    CHECK(nv2a_texscale(src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, ref) == 1);

    /* No disk cache: asked without texels nothing is known; with them a job
     * is queued (pending, the caller is not held), and the result is the
     * scaler's, handed over once. */
    CHECK(nv2a_texscale_cache_path(found, sizeof found) == 0);
    CHECK(nv2a_texscale_get(key, NULL, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
          == NV2A_TEXSCALE_MISS);
    CHECK(nv2a_texscale_get(0, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
          == NV2A_TEXSCALE_MISS);
    CHECK(nv2a_texscale_get(key, src, W, H, 3, NV2A_TEXSCALE_SUPERXBR, 0, got)
          == NV2A_TEXSCALE_MISS);
    r = nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got);
    CHECK(r == NV2A_TEXSCALE_PENDING);
    memset(got, 0, sizeof got);
    wait_ready(key, W, H, 4, got, &r);
    CHECK(r == NV2A_TEXSCALE_READY);
    CHECK(!memcmp(got, ref, sizeof ref));
    CHECK(nv2a_texscale_get(key, NULL, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
          == NV2A_TEXSCALE_MISS);                        /* taken: forgotten */
    nv2a_texscale_progress(&done, &total);
    CHECK(done == total && total >= 1);
    /* Made ahead of use with nowhere to keep it: not queued. */
    CHECK(nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR,
                            NV2A_TEXSCALE_DISK_ONLY, NULL) == NV2A_TEXSCALE_MISS);
    if (!dir) {
        printf("nv2a_texscale: no scratch directory given, disk cache not tested\n");
        return;
    }

    /* With a directory: the first request scales and writes a file, the
     * next one (the result was taken, so it is asked for again) reads it. */
    nv2a_texscale_cache_dir(dir);
    CHECK(nv2a_texscale_cache_path(found, sizeof found) == 1 && !strcmp(found, dir));
    nv2a_texscale_store_stats(&st);
    {
        uint64_t hits0 = st.hits, miss0 = st.misses, wr0 = st.written, bad0 = st.bad;
        CHECK(nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_PENDING);
        wait_ready(key, W, H, 4, got, &r);
        CHECK(r == NV2A_TEXSCALE_READY && !memcmp(got, ref, sizeof ref));
        nv2a_texscale_store_stats(&st);
        CHECK(st.misses == miss0 + 1 && st.written == wr0 + 1 && st.hits == hits0);
        snprintf(path, sizeof path, "%s/%016llx_%ux%u_x4_sx%u.tsc", dir,
                 (unsigned long long)key, (unsigned)W, (unsigned)H,
                 (unsigned)NV2A_TEXSCALE_VERSION);
        f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            CHECK(ftell(f) == (long)(W * H * 16 * 4 + 48));
            fclose(f);
        }
        memset(got, 0, sizeof got);
        CHECK(nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_PENDING);
        wait_ready(key, W, H, 4, got, &r);
        CHECK(r == NV2A_TEXSCALE_READY && !memcmp(got, ref, sizeof ref));
        nv2a_texscale_store_stats(&st);
        CHECK(st.hits == hits0 + 1 && st.misses == miss0 + 1 && st.written == wr0 + 1);

        /* A damaged file (one texel changed): found out by its checksum,
         * removed, scaled again, written again. */
        f = fopen(path, "r+b");
        CHECK(f != NULL);
        if (f) {
            fseek(f, 48 + 1000, SEEK_SET);
            fputc(0x5A, f);
            fclose(f);
        }
        memset(got, 0, sizeof got);
        CHECK(nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_PENDING);
        wait_ready(key, W, H, 4, got, &r);
        CHECK(r == NV2A_TEXSCALE_READY && !memcmp(got, ref, sizeof ref));
        nv2a_texscale_store_stats(&st);
        CHECK(st.bad == bad0 + 1 && st.misses == miss0 + 2 && st.written == wr0 + 2);
        /* A cut file. */
        f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f) {
            fwrite(ref, 1, 500, f);
            fclose(f);
        }
        memset(got, 0, sizeof got);
        CHECK(nv2a_texscale_get(key, src, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_PENDING);
        wait_ready(key, W, H, 4, got, &r);
        CHECK(r == NV2A_TEXSCALE_READY && !memcmp(got, ref, sizeof ref));
        nv2a_texscale_store_stats(&st);
        CHECK(st.bad == bad0 + 2);

        /* Made ahead of use: on disk, not kept in memory; a bind's request
         * later finds the file. */
        CHECK(nv2a_texscale_get(key2, other, W, H, 2, NV2A_TEXSCALE_SUPERXBR,
                                NV2A_TEXSCALE_DISK_ONLY, NULL) == NV2A_TEXSCALE_PENDING);
        CHECK(nv2a_texscale_flush(20000) == 1);
        CHECK(nv2a_texscale_get(key2, NULL, W, H, 2, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_MISS);
        nv2a_texscale_store_stats(&st);
        hits0 = st.hits;
        CHECK(nv2a_texscale_get(key2, other, W, H, 2, NV2A_TEXSCALE_SUPERXBR, 0, got)
              == NV2A_TEXSCALE_PENDING);
        wait_ready(key2, W, H, 2, got, &r);
        CHECK(r == NV2A_TEXSCALE_READY);
        nv2a_texscale_store_stats(&st);
        CHECK(st.hits == hits0 + 1);
        CHECK(nv2a_texscale(other, W, H, 2, NV2A_TEXSCALE_SUPERXBR, ref) == 1);
        CHECK(!memcmp(got, ref, (size_t)W * H * 4 * 4));
    }

    /* A file a scaler: the same texture asked for with each of the two is
     * made twice (neither is served the other's picture), lies in two
     * files, and each read back is that scaler's own result. */
    {
        static uint32_t mine[W * H * 16];
        static const char *const tag[NV2A_TEXSCALE_ALGOS] = { "sx", "bc" };
        uint64_t miss0, hits0;
        int a;
        nv2a_texscale_store_stats(&st);
        miss0 = st.misses;
        for (a = 1; a < NV2A_TEXSCALE_ALGOS; a++) {
            CHECK(nv2a_texscale_get(key, src, W, H, 4, (enum nv2a_texscale_algo)a, 0, got)
                  == NV2A_TEXSCALE_PENDING);
            CHECK(nv2a_texscale_flush(20000) == 1);
            /* asked for as Super-xBR meanwhile: not this one */
            CHECK(nv2a_texscale_get(key, NULL, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
                  == NV2A_TEXSCALE_MISS);
            CHECK(nv2a_texscale_get(key, NULL, W, H, 4, (enum nv2a_texscale_algo)a, 0, got)
                  == NV2A_TEXSCALE_READY);
            CHECK(nv2a_texscale(src, W, H, 4, (enum nv2a_texscale_algo)a, mine) == 1);
            CHECK(!memcmp(got, mine, sizeof mine));
        }
        nv2a_texscale_store_stats(&st);
        CHECK(st.misses == miss0 + 1);                   /* scaled, not found */
        hits0 = st.hits;
        for (a = 0; a < NV2A_TEXSCALE_ALGOS; a++) {
            snprintf(path, sizeof path, "%s/%016llx_%ux%u_x4_%s%u.tsc", dir,
                     (unsigned long long)key, (unsigned)W, (unsigned)H, tag[a],
                     (unsigned)NV2A_TEXSCALE_VERSION);
            f = fopen(path, "rb");
            CHECK(f != NULL);
            if (f)
                fclose(f);
            CHECK(nv2a_texscale_get(key, src, W, H, 4, (enum nv2a_texscale_algo)a, 0, got)
                  == NV2A_TEXSCALE_PENDING);
            wait_ready_algo(key, W, H, 4, (enum nv2a_texscale_algo)a, got, &r);
            CHECK(r == NV2A_TEXSCALE_READY);
            CHECK(nv2a_texscale(src, W, H, 4, (enum nv2a_texscale_algo)a, mine) == 1);
            CHECK(!memcmp(got, mine, sizeof mine));
        }
        nv2a_texscale_store_stats(&st);
        CHECK(st.hits == hits0 + 2);                     /* both from their files */
        /* ref back to what the next block compares with */
        CHECK(nv2a_texscale(other, W, H, 2, NV2A_TEXSCALE_SUPERXBR, ref) == 1);
    }

    /* The one-thread mode gives the result at once. */
    nv2a_texscale_set_sync(1);
    memset(got, 0, sizeof got);
    CHECK(nv2a_texscale_get(key2, other, W, H, 2, NV2A_TEXSCALE_SUPERXBR, 0, got)
          == NV2A_TEXSCALE_READY);
    CHECK(!memcmp(got, ref, (size_t)W * H * 4 * 4));
    nv2a_texscale_set_sync(0);

    /* The size bound: more files than it holds, the oldest go. */
    for (i = 0; i < 12; i++) {
        other[0] = 0xFF000000u | (uint32_t)(i + 1);
        CHECK(nv2a_texscale_get(nv2a_texscale_key(other, W, H), other, W, H, 4,
                                NV2A_TEXSCALE_SUPERXBR, NV2A_TEXSCALE_DISK_ONLY, NULL)
              == NV2A_TEXSCALE_PENDING);
    }
    CHECK(nv2a_texscale_flush(20000) == 1);
    nv2a_texscale_cache_trim(1u << 30);
    nv2a_texscale_store_stats(&st);
    CHECK(st.dir_bytes >= 12u * (W * H * 16 * 4 + 48));
    nv2a_texscale_cache_trim(6u * (W * H * 16 * 4 + 48));
    nv2a_texscale_store_stats(&st);
    CHECK(st.dir_bytes <= 6u * (W * H * 16 * 4 + 48) * 3 / 4 + 48);
    CHECK(st.dir_bytes > 0);

    /* Which files go: the least recently used, not the oldest written. Eight
     * files written long ago, one after another; the two oldest are then
     * read (the fonts a player sees in every run); the bound makes room for
     * three: the two that were read and the newest stay. */
    {
        uint64_t k[8];
        char pth[8][1200];
        struct utimbuf ut;
        time_t now = time(NULL);
        FILE *fp;
        nv2a_texscale_cache_trim(0);
        nv2a_texscale_store_stats(&st);
        CHECK(st.dir_bytes == 0);
        for (i = 0; i < 8; i++) {
            other[0] = 0xFF00AA00u | (uint32_t)(i + 1);
            k[i] = nv2a_texscale_key(other, W, H);
            CHECK(nv2a_texscale_get(k[i], other, W, H, 4, NV2A_TEXSCALE_SUPERXBR,
                                    NV2A_TEXSCALE_DISK_ONLY, NULL) == NV2A_TEXSCALE_PENDING);
        }
        CHECK(nv2a_texscale_flush(20000) == 1);
        for (i = 0; i < 8; i++) {
            snprintf(pth[i], sizeof pth[i], "%s/%016llx_%ux%u_x4_sx%u.tsc", dir,
                     (unsigned long long)k[i], (unsigned)W, (unsigned)H,
                     (unsigned)NV2A_TEXSCALE_VERSION);
            ut.actime = ut.modtime = now - 100000 + i * 100;
            CHECK(utime(pth[i], &ut) == 0);
        }
        for (i = 0; i < 2; i++) {
            other[0] = 0xFF00AA00u | (uint32_t)(i + 1);
            CHECK(nv2a_texscale_get(k[i], other, W, H, 4, NV2A_TEXSCALE_SUPERXBR, 0, got)
                  == NV2A_TEXSCALE_PENDING);
            wait_ready(k[i], W, H, 4, got, &r);
            CHECK(r == NV2A_TEXSCALE_READY);
        }
        nv2a_texscale_cache_trim(4u * (W * H * 16 * 4 + 48) + 3);   /* three quarters of it: three files */
        for (i = 0; i < 8; i++) {
            int there;
            fp = fopen(pth[i], "rb");
            there = fp != NULL;
            if (fp)
                fclose(fp);
            CHECK(there == (i == 0 || i == 1 || i == 7));
        }
        nv2a_texscale_store_stats(&st);
        CHECK(st.dir_bytes == 3u * (W * H * 16 * 4 + 48));
    }
    nv2a_texscale_cache_dir(NULL);
    CHECK(nv2a_texscale_cache_path(found, sizeof found) == 0);
}

int main(int argc, char **argv)
{
    static uint32_t src[W * H], dst[W * H * 16];
    unsigned i;

    /* What is not scaled: dst untouched. */
    memset(dst, 0x5A, sizeof dst);
    CHECK(nv2a_texscale(src, W, H, 3, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, W, H, 1, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, 0, H, 2, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, W, H, 2, NV2A_TEXSCALE_ALGOS, dst) == 0);
    CHECK(nv2a_texscale(NULL, W, H, 2, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    for (i = 0; i < W * H * 16; i++)
        if (dst[i] != 0x5A5A5A5Au)
            break;
    CHECK(i == W * H * 16);

    scaler(NV2A_TEXSCALE_SUPERXBR, 2);
    scaler(NV2A_TEXSCALE_SUPERXBR, 4);
    scaler(NV2A_TEXSCALE_BICUBIC, 2);
    scaler(NV2A_TEXSCALE_BICUBIC, 4);
    filters();

    /* Super-xBR follows an edge: a diagonal step, black over white, comes
     * out as a straight diagonal (each row is the row above moved one texel
     * along). The bicubic leaves the source's stair on it. */
    {
        static uint32_t a[16 * 16], x4[64 * 64], b4[64 * 64];
        int x, y, step_x = 0, step_b = 0;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                a[y * 16 + x] = x > y ? 0xFFFFFFFFu : 0xFF000000u;
        CHECK(nv2a_texscale(a, 16, 16, 4, NV2A_TEXSCALE_SUPERXBR, x4) == 1);
        CHECK(nv2a_texscale(a, 16, 16, 4, NV2A_TEXSCALE_BICUBIC, b4) == 1);
        for (y = 12; y < 50; y++)
            for (x = 12; x < 50; x++) {
                step_x += !near(x4[y * 64 + x], x4[(y + 1) * 64 + x + 1], 12);
                step_b += !near(b4[y * 64 + x], b4[(y + 1) * 64 + x + 1], 12);
            }
        CHECK(step_x == 0);
        CHECK(step_b > 0);
        /* and the two sides are still black and white */
        CHECK(x4[40 * 64 + 8] == 0xFF000000u && x4[8 * 64 + 40] == 0xFFFFFFFFu);
    }

    /* The mip chain: 2x2 boxes weighted by alpha. */
    {
        uint32_t t[16 + 4 + 1];
        for (i = 0; i < 16; i++)
            t[i] = 0;
        t[0] = 0xFFFFFFFFu;              /* one white texel in a clear block */
        t[2] = 0xFF204060u; t[3] = 0xFF204060u; t[6] = 0xFF204060u; t[7] = 0xFF204060u;
        t[8] = 0x00112233u; t[9] = 0x00112233u; t[12] = 0x00112233u; t[13] = 0x00112233u;
        nv2a_texscale_mips(t, 4, 4, 3);
        CHECK(t[16] == 0x40FFFFFFu);     /* a quarter covered, still white */
        CHECK(t[17] == 0xFF204060u);
        CHECK(t[18] == 0x00112233u);     /* nothing visible: the colours' mean */
        CHECK(t[19] == 0);
        CHECK(A(t[20]) == (0x40 + 0xFF + 2) / 4);
    }

    /* The rule: a 2D draw. */
    {
        struct nv2a_texscale_draw d = { 0, 0, 0, 640, 480, 640, 480 };
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.program = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);       /* a program or not: no matter */
        d.frame_h = 464;                                /* the presented extent's crop */
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.frame_h = 480; d.target_h = 464;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.target_h = 480; d.depth_test = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* the world, particles, flares */
        d.depth_write = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);
        d.depth_test = 0;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);
        d.depth_write = 0; d.target_w = 320; d.target_h = 240;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* an effect target */
        d.target_w = 128; d.target_h = 128;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* a reflection face */
        d.target_w = 640; d.target_h = 480; d.frame_w = d.frame_h = 0;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* before the first flip */
    }
    /* The rule: a texture. */
    CHECK(nv2a_texscale_tex_ok(1, 256, 256, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 512, 512, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 1024, 128, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 1024, 512, 1) == 0);  /* too large */
    CHECK(nv2a_texscale_tex_ok(1, 256, 256, 9) == 0);   /* mipmapped: the world's */
    CHECK(nv2a_texscale_tex_ok(0, 640, 352, 1) == 0);   /* linear: a video frame */
    CHECK(nv2a_texscale_tex_ok(1, 0, 16, 1) == 0);

    /* Art cut out with alpha, or an opaque picture. */
    {
        uint32_t t[4] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF123456u, 0xFFFFFFFFu };
        CHECK(nv2a_texscale_has_alpha(t, 4) == 0);
        t[3] = 0xFEFFFFFFu;
        CHECK(nv2a_texscale_has_alpha(t, 4) == 1);
        CHECK(nv2a_texscale_has_alpha(t, 3) == 0);
        CHECK(nv2a_texscale_has_alpha(t, 0) == 0);
        /* A photograph kept in a format with alpha: stray texels just under
         * opaque, one in a hundred. Not art. One texel in sixteen is. */
        {
            static uint32_t pic[1600];
            int k;
            for (k = 0; k < 1600; k++)
                pic[k] = 0xFF405060u;
            for (k = 0; k < 16; k++)
                pic[k * 97] = 0xE3405060u;
            CHECK(nv2a_texscale_has_alpha(pic, 1600) == 0);
            for (k = 0; k < 99; k++)
                pic[k * 16 + 1] = 0x00000000u;
            CHECK(nv2a_texscale_has_alpha(pic, 1600) == 1);       /* 115 of 1600 */
            for (k = 16; k < 99; k++)
                pic[k * 16 + 1] = 0xFF405060u;
            CHECK(nv2a_texscale_has_alpha(pic, 1600) == 0);       /* 32 of 1600 */
        }
    }
    /* A texture large enough for the rows to be shared among threads comes
     * out as its parts do alone: the top-left quarter of a 256x256 picture
     * scaled whole equals the same texels scaled as a 144x144 cut (the cut's
     * own edge reaches a few texels in, so compare well inside). */
    {
        static uint32_t big[256 * 256], cut[144 * 144], ob[1024 * 1024], oc[576 * 576];
        uint32_t seed = 99u;
        int x, y, bad = 0;
        for (i = 0; i < 256 * 256; i++) {
            seed = seed * 1664525u + 1013904223u;
            /* blobs, not noise: 8x8 cells of one colour */
            if ((i % 256) % 8 == 0)
                big[i] = seed | ((seed >> 7) & 1u ? 0xFF000000u : 0u);
            else
                big[i] = big[i - 1];
            if ((i / 256) % 8)
                big[i] = big[i - 256];
        }
        for (y = 0; y < 144; y++)
            memcpy(&cut[y * 144], &big[y * 256], 144 * 4);
        CHECK(nv2a_texscale(big, 256, 256, 4, NV2A_TEXSCALE_SUPERXBR, ob) == 1);
        CHECK(nv2a_texscale(cut, 144, 144, 4, NV2A_TEXSCALE_SUPERXBR, oc) == 1);
        for (y = 0; y < 480; y++)
            for (x = 0; x < 480; x++)
                bad += ob[y * 1024 + x] != oc[y * 576 + x];
        CHECK(bad == 0);
    }

    /* Held off: whatever is asked for, the factor stays 0 and nothing moves;
     * lifted, the setting works. (On Android the hold is the default.) */
    {
        uint32_t g0;
        (void)nv2a_texscale_factor();          /* the environment is read: off */
        g0 = nv2a_texscale_generation();
        recomp_env_set(RENV_UI_SCALE_HOLD, "1");
        CHECK(nv2a_texscale_held() == 1);
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_factor() == 0 && nv2a_texscale_generation() == g0);
        nv2a_texscale_set_factor(4);
        CHECK(nv2a_texscale_factor() == 0 && nv2a_texscale_generation() == g0);
        recomp_env_set(RENV_UI_SCALE_HOLD, "0");
        CHECK(nv2a_texscale_held() == 0);
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_factor() == 2);
        nv2a_texscale_set_factor(0);
        recomp_env_set(RENV_UI_SCALE_HOLD, NULL);
        CHECK(nv2a_texscale_held() == 0);
    }

    /* The setting: off unless asked; 2 and 4 only; a change moves the
     * generation on, the same value does not. */
    {
        uint32_t g0;
        uint64_t n, texels, usec;
        CHECK(nv2a_texscale_factor() == 0);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_SUPERXBR);
        g0 = nv2a_texscale_generation();
        CHECK(g0 != 0);
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_factor() == 2 && nv2a_texscale_generation() != g0);
        g0 = nv2a_texscale_generation();
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_generation() == g0);
        nv2a_texscale_set_factor(4);
        CHECK(nv2a_texscale_factor() == 4);
        nv2a_texscale_set_factor(3);
        CHECK(nv2a_texscale_factor() == 0);
        g0 = nv2a_texscale_generation();
        nv2a_texscale_set_algo(NV2A_TEXSCALE_BICUBIC);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_BICUBIC && nv2a_texscale_generation() != g0);
        nv2a_texscale_set_algo(NV2A_TEXSCALE_ALGOS);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_SUPERXBR);
        nv2a_texscale_stats(&n, &texels, &usec);
        CHECK(n == 0 && texels == 0);
        nv2a_texscale_note(128, 64, 4, 1500);
        nv2a_texscale_stats(&n, &texels, &usec);
        CHECK(n == 1 && texels == 128u * 64u * 16u && usec == 1500);
    }

    store(argc > 1 ? argv[1] : NULL);

    printf("nv2a_texscale: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}

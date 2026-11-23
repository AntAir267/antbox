/* nv2a_texpack: texture packs (BO-312). Keys, pack order, scale and level
 * arithmetic, the alpha rule, mips, BC compression, the jobs and the disk
 * cache, the dump. Synthetic textures, no GPU. */
#include "nv2a_texpack.h"
#include "nv2a_backend_common.h"
#include "nv2a_texmip.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../third_party/stb/stb_image_write.h"

static int s_checks, s_fail;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char s_dir[600];

static void mk(const char *p) { mkdir(p, 0755); }

static void path(char *out, size_t n, const char *sub, const char *name)
{
    snprintf(out, n, "%s/%s%s%s", s_dir, sub, *name ? "/" : "", name);
}

/* A BGRA picture: colour from (x, y), alpha a. */
static uint8_t *picture(uint32_t w, uint32_t h, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    uint8_t *p = (uint8_t *)malloc((size_t)w * h * 4);
    size_t i;
    for (i = 0; i < (size_t)w * h; i++) {
        p[i * 4] = b;
        p[i * 4 + 1] = g;
        p[i * 4 + 2] = r;
        p[i * 4 + 3] = a;
    }
    return p;
}

static void put_png(const char *sub, uint64_t hash, uint64_t palh, uint32_t w, uint32_t h,
                    uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    char name[64], p[900];
    uint8_t *px = picture(w, h, b, g, r, a);
    nv2a_texpack_key_name(hash, palh, name, sizeof name);
    path(p, sizeof p, sub, name);
    CHECK(nv2a_texpack_png_write(p, px, w, h));
    free(px);
}

/* An RGB PNG (no alpha channel). */
static void put_rgb_png(const char *sub, uint64_t hash, uint32_t w, uint32_t h, uint8_t r,
                        uint8_t g, uint8_t b)
{
    char name[64], p[900];
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 3);
    size_t i;
    for (i = 0; i < (size_t)w * h; i++) {
        px[i * 3] = r;
        px[i * 3 + 1] = g;
        px[i * 3 + 2] = b;
    }
    nv2a_texpack_key_name(hash, 0, name, sizeof name);
    path(p, sizeof p, sub, name);
    CHECK(stbi_write_png(p, (int)w, (int)h, 3, px, (int)(w * 3)));
    free(px);
}

static void names(void)
{
    uint64_t h, p;
    char out[64];
    CHECK(nv2a_texpack_parse_name("0123456789abcdef.png", &h, &p) && h == 0x0123456789abcdefull && p == 0);
    CHECK(nv2a_texpack_parse_name("0123456789ABCDEF.PNG", &h, &p) && h == 0x0123456789abcdefull);
    CHECK(nv2a_texpack_parse_name("0123456789abcdef-fedcba9876543210.png", &h, &p)
          && h == 0x0123456789abcdefull && p == 0xfedcba9876543210ull);
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef.tga", &h, &p));
    CHECK(!nv2a_texpack_parse_name("0123456789abcde.png", &h, &p));          /* 15 digits */
    CHECK(!nv2a_texpack_parse_name("0123456789abcdeg.png", &h, &p));         /* not hex */
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef_fedcba9876543210.png", &h, &p));
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef-0000000000000000.png", &h, &p));   /* a palette hash of 0 */
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef-fedcba987654321.png", &h, &p));
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef.png.txt", &h, &p));
    CHECK(!nv2a_texpack_parse_name("0123456789abcdef_m1.png", &h, &p));       /* a dumped level */
    CHECK(!nv2a_texpack_parse_name("index.tsv", &h, &p));
    CHECK(!nv2a_texpack_parse_name("", &h, &p));
    CHECK(!nv2a_texpack_parse_name(NULL, &h, &p));
    nv2a_texpack_key_name(0xABull, 0, out, sizeof out);
    CHECK(!strcmp(out, "00000000000000ab.png"));
    nv2a_texpack_key_name(0xABull, 0xCDull, out, sizeof out);
    CHECK(!strcmp(out, "00000000000000ab-00000000000000cd.png"));
    CHECK(nv2a_texpack_parse_name(out, &h, &p) && h == 0xAB && p == 0xCD);
}

static void scales(void)
{
    float lo, hi;
    uint32_t fx, fy;
    CHECK(nv2a_texpack_add_levels(256, 256, 256, 256) == 0);
    CHECK(nv2a_texpack_add_levels(512, 512, 256, 256) == 1);
    CHECK(nv2a_texpack_add_levels(1024, 1024, 256, 256) == 2);
    CHECK(nv2a_texpack_add_levels(2048, 1024, 512, 256) == 2);
    CHECK(nv2a_texpack_add_levels(2048, 2048, 256, 256) == 3);
    CHECK(nv2a_texpack_add_levels(768, 768, 256, 256) == 2);       /* 3x: log2 1.58 rounds to 2 */
    CHECK(nv2a_texpack_add_levels(1280, 1280, 256, 256) == 2);     /* 5x: 2.32 -> 2 */
    CHECK(nv2a_texpack_add_levels(1024, 512, 256, 256) == 2);      /* 4x wide, 2x high: the larger */
    CHECK(nv2a_texpack_add_levels(512, 1024, 256, 256) == 2);
    CHECK(nv2a_texpack_add_levels(128, 128, 256, 256) == 0);       /* smaller: no shift */
    CHECK(nv2a_texpack_add_levels(300, 300, 256, 256) == 0);       /* 1.17x */
    CHECK(nv2a_texpack_add_levels(0, 0, 256, 256) == 0);
    CHECK(nv2a_texpack_add_levels(8, 8, 0, 0) == 0);
    nv2a_texpack_factors(1024, 512, 256, 256, &fx, &fy);
    CHECK(fx == 40 && fy == 20);
    /* levels: the title's, above the factor's; a one-level texture gets add + 1 */
    CHECK(nv2a_texpack_levels(1024, 1024, 1, 2) == 3);
    CHECK(nv2a_texpack_levels(1024, 1024, 9, 2) == 11);
    CHECK(nv2a_texpack_levels(1024, 1024, 12, 2) == 11);            /* at most the whole chain */
    CHECK(nv2a_texpack_levels(512, 256, 1, 0) == 1);
    CHECK(nv2a_texpack_levels(1, 1, 1, 0) == 1);
    /* the sampler: min 0 stays 0, a title minimum moves down, one level is blended down */
    nv2a_texpack_lod(9, 0, 8, 2, &lo, &hi);
    CHECK(lo == 0.0f && hi == 10.0f);
    nv2a_texpack_lod(9, 1, 8, 2, &lo, &hi);
    CHECK(lo == 3.0f && hi == 10.0f);
    nv2a_texpack_lod(1, 0, 0, 2, &lo, &hi);
    CHECK(lo == 0.0f && hi == 2.0f);
    nv2a_texpack_lod(1, 0, 0, 0, &lo, &hi);
    CHECK(lo == 0.0f && hi == 0.0f);
    CHECK(nv2a_texpack_level_bytes(NV2A_TEXPACK_FMT_BGRA8, 5, 3) == 60);
    CHECK(nv2a_texpack_level_bytes(NV2A_TEXPACK_FMT_BC1, 5, 3) == 2 * 1 * 8);
    CHECK(nv2a_texpack_level_bytes(NV2A_TEXPACK_FMT_BC3, 1, 1) == 16);
    CHECK(nv2a_texpack_level_bytes(NV2A_TEXPACK_FMT_BC3, 256, 256) == 64 * 64 * 16);
}

static void order(void)
{
    char d1[600], d2[600], d3[600], e[600];
    struct nv2a_texpack_src p[3];
    uint32_t g0 = nv2a_texpack_generation(), id;
    int n;

    path(d1, sizeof d1, "A", "");
    path(d2, sizeof d2, "B", "");
    path(d3, sizeof d3, "C", "");
    path(e, sizeof e, "env", "");
    mk(d1); mk(d2); mk(d3); mk(e);
    put_png("A", 0x1111, 0, 8, 8, 1, 2, 3, 255);          /* A and B both have 1111: A wins */
    put_png("B", 0x1111, 0, 8, 8, 9, 9, 9, 255);
    put_png("B", 0x2222, 0, 8, 8, 4, 5, 6, 255);
    put_png("B", 0x3333, 0x77, 8, 8, 4, 5, 6, 255);       /* P8 key */
    put_png("B", 0x4444, 0, 8, 8, 4, 5, 6, 255);          /* bare hash for a P8 texture */
    put_png("C", 0x5555, 0, 8, 8, 4, 5, 6, 255);
    put_png("env", 0x1111, 0, 8, 8, 7, 7, 7, 255);        /* the environment pack is first */
    {   char x[900]; FILE *f;
        path(x, sizeof x, "A", "notes.txt"); f = fopen(x, "wb"); fputs("x", f); fclose(f);
        path(x, sizeof x, "A", "zzzz.png"); f = fopen(x, "wb"); fputs("x", f); fclose(f);
        path(x, sizeof x, "A", "0000000000006666.png.bak"); f = fopen(x, "wb"); fputs("x", f); fclose(f); }
    CHECK(!nv2a_texpack_active() || nv2a_texpack_find(0x9999, 0) == 0);

    p[0].dir = d1; p[0].name = "a"; p[0].alpha = 0; p[0].compress = 0;
    p[1].dir = d2; p[1].name = "b"; p[1].alpha = 0; p[1].compress = 0;
    p[2].dir = d3; p[2].name = "c"; p[2].alpha = 0; p[2].compress = 0;
    n = nv2a_texpack_set(p, 3);
    /* keys: 1111 (env), 2222, 3333-77, 4444, 5555 */
    CHECK(n == 5);
    CHECK(nv2a_texpack_active());
    CHECK(nv2a_texpack_generation() != g0 && nv2a_texpack_generation() != 0);
    id = nv2a_texpack_find(0x1111, 0);
    CHECK(id && nv2a_texpack_pack_of(id) == 0);            /* the env pack, index 0 */
    id = nv2a_texpack_find(0x2222, 0);
    CHECK(id && nv2a_texpack_pack_of(id) == 2);            /* env, A, B: B is 2 */
    id = nv2a_texpack_find(0x5555, 0);
    CHECK(id && nv2a_texpack_pack_of(id) == 3);
    CHECK(nv2a_texpack_find(0x9999, 0) == 0);
    CHECK(nv2a_texpack_find(0x6666, 0) == 0);              /* .png.bak and a bad name are not keys */
    CHECK(nv2a_texpack_find(0x3333, 0x77) != 0);
    CHECK(nv2a_texpack_find(0x3333, 0x78) == 0);           /* wrong palette, no bare file */
    CHECK(nv2a_texpack_find(0x3333, 0) == 0);
    CHECK(nv2a_texpack_find(0x4444, 0x1234) == nv2a_texpack_find(0x4444, 0));   /* P8: the bare hash */
    CHECK(nv2a_texpack_find(0x4444, 0x1234) != 0);
    /* the same without the env pack's win: A over B */
    {
        uint32_t g1 = nv2a_texpack_generation();
        n = nv2a_texpack_set(p, 0);
        CHECK(n == 1);                                      /* only the env pack */
        CHECK(nv2a_texpack_generation() != g1);
        n = nv2a_texpack_set(p, 3);
        CHECK(n == 5);
    }
    /* a folder that is not there is listed in the log and adds nothing */
    p[2].dir = "/nonexistent/texpack/dir";
    n = nv2a_texpack_set(p, 3);
    CHECK(n == 4);
    p[2].dir = d3;
    nv2a_texpack_set(p, 3);
}

/* decode helper for BC1 colour 0 of the first block: RGB565 */
static void bc_first_rgb(const uint8_t *blk, int *r, int *g, int *b)
{
    unsigned c0 = blk[0] | blk[1] << 8;
    *r = (int)((c0 >> 11) & 31) * 255 / 31;
    *g = (int)((c0 >> 5) & 63) * 255 / 63;
    *b = (int)(c0 & 31) * 255 / 31;
}

static int absd(int a, int b) { return a < b ? b - a : a - b; }

static void pixels(void)
{
    struct nv2a_texpack_img img;
    uint8_t *px;
    uint8_t alpha[4 * 4];
    uint32_t x, y;

    /* the title's alpha plane 4x4: left half 0, right half 255 */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            alpha[y * 4 + x] = x < 2 ? 0 : 255;

    /* A PNG without alpha, mode auto: the title's alpha, scaled; the colour under
     * alpha 0 is exactly the PNG's (nothing filled). */
    px = picture(16, 16, 10, 20, 30, 255);
    px[(0 * 16 + 0) * 4] = 99;                             /* a marked texel under alpha 0 */
    CHECK(nv2a_texpack_build(px, 16, 16, 0, NV2A_TEXPACK_ALPHA_AUTO, alpha, 4, 4, 1,
                             0, NV2A_TEXPACK_FMT_BGRA8, &img));
    free(px);
    CHECK(img.w == 16 && img.h == 16 && img.add == 2);
    CHECK(img.levels == 3);                                 /* 16 -> 8 -> 4 as the title has one level */
    CHECK(img.fmt == NV2A_TEXPACK_FMT_BGRA8);
    CHECK(img.bytes == (16 * 16 + 8 * 8 + 4 * 4) * 4);
    CHECK(img.data[0] == 99 && img.data[1] == 20 && img.data[2] == 30);   /* colour untouched */
    CHECK(img.data[3] == 0);                                              /* alpha 0 where the title has 0 */
    CHECK(img.data[(0 * 16 + 15) * 4 + 3] == 255);
    CHECK(img.data[(5 * 16 + 1) * 4 + 3] == 0);
    CHECK(img.data[(5 * 16 + 14) * 4 + 3] == 255);
    {   /* the edge between the halves is a ramp, not a step: bilinear like the GPU's filter */
        int mid = img.data[(5 * 16 + 7) * 4 + 3], mid2 = img.data[(5 * 16 + 8) * 4 + 3];
        CHECK(mid > 0 && mid < 255 && mid2 > mid);
    }
    nv2a_texpack_img_free(&img);

    /* mode png keeps the PNG's alpha (opaque for a file without), original ignores the PNG's */
    px = picture(8, 8, 1, 2, 3, 128);
    CHECK(nv2a_texpack_build(px, 8, 8, 1, NV2A_TEXPACK_ALPHA_AUTO, alpha, 4, 4, 1, 0, 0, &img));
    CHECK(img.data[3] == 128);                              /* auto: the file has alpha */
    nv2a_texpack_img_free(&img);
    CHECK(nv2a_texpack_build(px, 8, 8, 1, NV2A_TEXPACK_ALPHA_PNG, alpha, 4, 4, 1, 0, 0, &img));
    CHECK(img.data[3] == 128);
    nv2a_texpack_img_free(&img);
    CHECK(nv2a_texpack_build(px, 8, 8, 1, NV2A_TEXPACK_ALPHA_ORIGINAL, alpha, 4, 4, 1, 0, 0, &img));
    CHECK(img.data[3] == 0 && img.data[(0 * 8 + 7) * 4 + 3] == 255);
    nv2a_texpack_img_free(&img);
    /* original without an alpha plane (an opaque title texture): 255 */
    CHECK(nv2a_texpack_build(px, 8, 8, 1, NV2A_TEXPACK_ALPHA_ORIGINAL, NULL, 4, 4, 1, 0, 0, &img));
    CHECK(img.data[3] == 255);
    nv2a_texpack_img_free(&img);
    free(px);

    /* a 2x1 PNG against a 1-level title: levels are made by a 2x2 box of every channel */
    px = (uint8_t *)malloc(2 * 2 * 4);
    {
        uint8_t v[16] = { 0, 0, 0, 255,   100, 100, 100, 255,   200, 200, 200, 0,   40, 40, 40, 255 };
        memcpy(px, v, 16);
    }
    CHECK(nv2a_texpack_build(px, 2, 2, 1, NV2A_TEXPACK_ALPHA_PNG, NULL, 1, 1, 1, 0, 0, &img));
    CHECK(img.add == 1 && img.levels == 2 && img.bytes == (4 + 1) * 4);
    CHECK(img.data[16] == 85 && img.data[19] == 191);       /* (0+100+200+40+2)/4 = 85; (255*3+0+2)/4 = 191 */
    nv2a_texpack_img_free(&img);
    free(px);

    /* a non-power-of-two PNG, non-square: 3x on one axis, 2x on the other */
    px = picture(24, 16, 5, 5, 5, 255);
    CHECK(nv2a_texpack_build(px, 24, 16, 0, NV2A_TEXPACK_ALPHA_AUTO, NULL, 8, 8, 4, 0, 0, &img));
    CHECK(img.add == 2 && img.levels == 5);                 /* 4 + 2 = 6, the chain of 24 has 5 */
    nv2a_texpack_img_free(&img);
    free(px);

    /* an alpha-tested draw: the levels keep the test's coverage (nv2a_texmip.h) */
    px = picture(32, 32, 10, 10, 10, 255);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            px[(y * 32 + x) * 4 + 3] = (uint8_t)((x * 7 + y * 3) % 5 == 0 ? 255 : 0);   /* sparse leaves */
    {
        uint32_t cut = nv2a_texmip_cut_ref(1, 0x204u, 128);       /* alpha > 128 */
        struct nv2a_texpack_img plain, cutout;
        uint8_t *copy = (uint8_t *)malloc(32 * 32 * 4);
        memcpy(copy, px, 32 * 32 * 4);
        CHECK(cut != 0);
        CHECK(nv2a_texpack_build(px, 32, 32, 1, NV2A_TEXPACK_ALPHA_PNG, NULL, 32, 32, 6, 0, 0, &plain));
        CHECK(nv2a_texpack_build(copy, 32, 32, 1, NV2A_TEXPACK_ALPHA_PNG, NULL, 32, 32, 6, cut, 0, &cutout));
        if (plain.data && cutout.data) {
            const uint32_t *l0 = (const uint32_t *)cutout.data;
            float c0 = nv2a_texmip_coverage(l0, 32, 32, cut);
            float c2p = nv2a_texmip_coverage((const uint32_t *)plain.data + 32 * 32 + 16 * 16, 8, 8, cut);
            float c2c = nv2a_texmip_coverage(l0 + 32 * 32 + 16 * 16, 8, 8, cut);
            CHECK(c0 > 0.1f);
            CHECK(c2p < c0 * 0.5f);                          /* the box filter loses the leaves */
            CHECK(c2c > c0 * 0.6f && c2c < c0 * 1.6f);       /* the cut-out levels keep them */
            CHECK(memcmp(plain.data, cutout.data, 32 * 32 * 4) == 0);   /* level 0 is never changed */
        }
        nv2a_texpack_img_free(&plain);
        nv2a_texpack_img_free(&cutout);
        free(copy);
    }
    free(px);

    /* BC: opaque is BC1, translucent BC3; flat colour survives; sizes add up */
    px = picture(64, 64, 200, 100, 50, 255);                /* BGRA: r 50 g 100 b 200 */
    CHECK(nv2a_texpack_build(px, 64, 64, 0, NV2A_TEXPACK_ALPHA_AUTO, NULL, 64, 64, 7,
                             0, NV2A_TEXPACK_FMT_BC3, &img));
    CHECK(img.fmt == NV2A_TEXPACK_FMT_BC1);
    CHECK(img.levels == 7);
    {
        size_t want = 0;
        uint32_t l;
        int r, g, b;
        for (l = 0; l < img.levels; l++)
            want += nv2a_texpack_level_bytes(NV2A_TEXPACK_FMT_BC1, nv2a_texpack_level_dim(64, l),
                                             nv2a_texpack_level_dim(64, l));
        CHECK(img.bytes == want);
        bc_first_rgb(img.data, &r, &g, &b);
        CHECK(absd(r, 50) < 10 && absd(g, 100) < 6 && absd(b, 200) < 10);
    }
    nv2a_texpack_img_free(&img);
    px[3] = 10;                                             /* one translucent texel: BC3 */
    CHECK(nv2a_texpack_build(px, 64, 64, 1, NV2A_TEXPACK_ALPHA_PNG, NULL, 64, 64, 1,
                             0, NV2A_TEXPACK_FMT_BC1, &img));
    CHECK(img.fmt == NV2A_TEXPACK_FMT_BC3 && img.levels == 1 && img.bytes == 16 * 16 * 16);
    nv2a_texpack_img_free(&img);
    free(px);
    /* a 1x1 and a 2x3 level compress too */
    px = picture(2, 3, 9, 9, 9, 255);
    CHECK(nv2a_texpack_build(px, 2, 3, 0, 0, NULL, 1, 1, 1, 0, NV2A_TEXPACK_FMT_BC3, &img));
    CHECK(img.fmt == NV2A_TEXPACK_FMT_BC1 && img.levels == 2 && img.bytes == 8 + 8);
    nv2a_texpack_img_free(&img);
    free(px);
}

static void files_and_jobs(void)
{
    char d[600], wd[600], p[900];
    struct nv2a_texpack_src src;
    struct nv2a_texpack_req rq;
    const struct nv2a_texpack_img *img = NULL;
    uint8_t alpha[8 * 8];
    uint32_t tok, tok2, id, w, h;
    int has, r, i;
    uint8_t *px;
    struct nv2a_texpack_stats st;

    path(d, sizeof d, "J", "");
    path(wd, sizeof wd, "cache", "");
    mk(d);
    put_png("J", 0xA1, 0, 64, 32, 50, 100, 200, 255);       /* 8x4 title -> 8x */
    put_rgb_png("J", 0xA2, 16, 16, 200, 100, 50);
    put_png("J", 0xA3, 0, 256, 256, 1, 2, 3, 128);          /* big enough to be compressed */
    {   char x[900]; FILE *f; path(x, sizeof x, "J", "00000000000000a4.png");
        f = fopen(x, "wb"); fputs("not a png", f); fclose(f); }
    memset(alpha, 255, sizeof alpha);
    alpha[0] = 0;

    src.dir = d; src.name = "j"; src.alpha = NV2A_TEXPACK_ALPHA_AUTO; src.compress = NV2A_TEXPACK_COMPRESS_AUTO;
    nv2a_texpack_set(&src, 1);                              /* the env pack is first, J after */
    nv2a_texpack_set_formats(0);
    nv2a_texpack_cache_dir(wd);

    /* PNG round trip through the reader */
    path(p, sizeof p, "J", "00000000000000a1.png");
    px = nv2a_texpack_png_read(p, &w, &h, &has);
    CHECK(px && w == 64 && h == 32 && has && px[0] == 50 && px[1] == 100 && px[2] == 200 && px[3] == 255);
    free(px);
    path(p, sizeof p, "J", "00000000000000a2.png");
    px = nv2a_texpack_png_read(p, &w, &h, &has);
    CHECK(px && w == 16 && !has && px[0] == 50 && px[2] == 200 && px[3] == 255);   /* RGB file: BGRA order */
    free(px);
    path(p, sizeof p, "J", "00000000000000a4.png");
    CHECK(nv2a_texpack_png_read(p, &w, &h, &has) == NULL);
    CHECK(nv2a_texpack_png_read("/nonexistent/file.png", &w, &h, &has) == NULL);

    /* a job: uncompressed (no formats offered) */
    id = nv2a_texpack_find(0xA1, 0);
    CHECK(id != 0);
    memset(&rq, 0, sizeof rq);
    rq.id = id; rq.w = 8; rq.h = 4; rq.levels = 4; rq.alpha = NULL;
    tok = nv2a_texpack_request(&rq);
    CHECK(tok != 0);
    tok2 = nv2a_texpack_request(&rq);                       /* a second texture, the same file and size: one job */
    CHECK(tok2 == tok);
    CHECK(nv2a_texpack_flush(10000));
    r = nv2a_texpack_poll(tok, &img);
    CHECK(r == NV2A_TEXPACK_READY && img);
    if (img) {
        CHECK(img->w == 64 && img->h == 32 && img->add == 3 && img->fmt == NV2A_TEXPACK_FMT_BGRA8);
        CHECK(img->levels == 7);                            /* 4 + 3 */
        CHECK(img->data[0] == 50 && img->data[2] == 200);
    }
    nv2a_texpack_release(tok);
    CHECK(nv2a_texpack_poll(tok, &img) == NV2A_TEXPACK_READY);   /* still one reference */
    nv2a_texpack_release(tok2);
    CHECK(nv2a_texpack_poll(tok, &img) == NV2A_TEXPACK_FAILED);  /* gone */

    /* a file without alpha takes the title's alpha */
    id = nv2a_texpack_find(0xA2, 0);
    memset(&rq, 0, sizeof rq);
    rq.id = id; rq.w = 8; rq.h = 8; rq.levels = 1; rq.alpha = alpha;
    tok = nv2a_texpack_request(&rq);
    CHECK(tok && nv2a_texpack_flush(10000));
    r = nv2a_texpack_poll(tok, &img);
    CHECK(r == NV2A_TEXPACK_READY);
    if (r == NV2A_TEXPACK_READY) {
        CHECK(img->data[3] == 0 && img->data[(8 * 16 + 8) * 4 + 3] == 255);
        CHECK(img->data[0] == 50 && img->data[2] == 200);  /* colour as in the file under alpha 0 */
    }
    nv2a_texpack_release(tok);

    /* a job asked for under packs that changed since (set from another thread
     * between the lookup and the request): it fails, never reads what the id
     * names now; under the current packs it works */
    {
        uint32_t g = nv2a_texpack_generation();
        id = nv2a_texpack_find(0xA2, 0);
        memset(&rq, 0, sizeof rq);
        rq.id = id; rq.w = 8; rq.h = 8; rq.levels = 1; rq.gen = g - 1 ? g - 1 : 0xFFFFFFFFu;
        tok = nv2a_texpack_request(&rq);
        CHECK(tok && nv2a_texpack_flush(10000));
        CHECK(nv2a_texpack_poll(tok, &img) == NV2A_TEXPACK_FAILED);
        nv2a_texpack_release(tok);
        rq.gen = g;
        tok = nv2a_texpack_request(&rq);
        CHECK(tok && nv2a_texpack_flush(10000));
        CHECK(nv2a_texpack_poll(tok, &img) == NV2A_TEXPACK_READY);
        nv2a_texpack_release(tok);
    }

    /* a file that is not a PNG fails, and says so */
    id = nv2a_texpack_find(0xA4, 0);
    CHECK(id != 0);
    memset(&rq, 0, sizeof rq);
    rq.id = id; rq.w = 8; rq.h = 8; rq.levels = 1;
    tok = nv2a_texpack_request(&rq);
    CHECK(tok && nv2a_texpack_flush(10000));
    CHECK(nv2a_texpack_poll(tok, &img) == NV2A_TEXPACK_FAILED);
    nv2a_texpack_release(tok);

    /* compression and the disk cache: BC offered, a 256x256 PNG with alpha */
    nv2a_texpack_set_formats(1u << NV2A_TEXPACK_FMT_BC1 | 1u << NV2A_TEXPACK_FMT_BC3);
    id = nv2a_texpack_find(0xA3, 0);
    memset(&rq, 0, sizeof rq);
    rq.id = id; rq.w = 128; rq.h = 128; rq.levels = 8; rq.alpha = NULL;
    for (i = 0; i < 2; i++) {
        tok = nv2a_texpack_request(&rq);
        CHECK(tok && nv2a_texpack_flush(20000));
        r = nv2a_texpack_poll(tok, &img);
        CHECK(r == NV2A_TEXPACK_READY);
        if (r == NV2A_TEXPACK_READY) {
            CHECK(img->fmt == NV2A_TEXPACK_FMT_BC3 && img->w == 256 && img->add == 1);
            CHECK(img->levels == 9);
        }
        nv2a_texpack_release(tok);
        nv2a_texpack_stats(&st);
        if (i == 0)
            CHECK(st.cache_written == 1 && st.from_cache == 0);
        else
            CHECK(st.from_cache == 1);                       /* the second time from the cache */
    }
    /* a pack that says compress off stays raw */
    src.compress = NV2A_TEXPACK_COMPRESS_OFF;
    nv2a_texpack_set(&src, 1);
    id = nv2a_texpack_find(0xA3, 0);
    rq.id = id;
    tok = nv2a_texpack_request(&rq);
    CHECK(tok && nv2a_texpack_flush(20000));
    r = nv2a_texpack_poll(tok, &img);
    CHECK(r == NV2A_TEXPACK_READY && img->fmt == NV2A_TEXPACK_FMT_BGRA8);
    nv2a_texpack_release(tok);
    src.compress = NV2A_TEXPACK_COMPRESS_AUTO;
    nv2a_texpack_set(&src, 1);
}

static void dump(void)
{
    char dd[600], x[900], idx[900];
    uint8_t guest[16 * 16 * 4];
    uint32_t texels[16 * 16 + 8 * 8], pal[256];
    uint64_t hash, palh;
    FILE *f;
    char line[256];
    int lines = 0, i;

    path(dd, sizeof dd, "dump", "");
    for (i = 0; i < 16 * 16 * 4; i++)
        guest[i] = (uint8_t)(i * 7);
    for (i = 0; i < 16 * 16 + 8 * 8; i++)
        texels[i] = 0xFF000000u | (uint32_t)i * 0x010203u;
    for (i = 0; i < 256; i++)
        pal[i] = 0xFF000000u | (uint32_t)i;
    CHECK(nv2a_texpack_dump_on());
    /* a linear texture (a video frame, a screen copy) is not a pack texture: not dumped */
    nv2a_texpack_dump(guest, 0x12, 16, 16, 64, 1, NULL, 0, texels);
    /* a non-P8 texture: <hash>.png */
    CHECK(nv2a_texpack_key(guest, 0x06, 16, 16, 64, NULL, 0, &hash, &palh) && hash && palh == 0);
    nv2a_texpack_dump(guest, 0x06, 16, 16, 64, 2, NULL, 0, texels);
    nv2a_texpack_key_name(hash, 0, x, sizeof x);
    path(idx, sizeof idx, "dump", x);
    f = fopen(idx, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    {   /* and the dump's file is a pack file: parse its name, read it back */
        uint64_t h2, p2; uint32_t w, h; int has; uint8_t *px;
        CHECK(nv2a_texpack_parse_name(x, &h2, &p2) && h2 == hash);
        px = nv2a_texpack_png_read(idx, &w, &h, &has);
        CHECK(px && w == 16 && h == 16 && has);
        if (px) {
            CHECK(px[0] == (texels[0] & 0xFF) && px[1] == ((texels[0] >> 8) & 0xFF)
                  && px[2] == ((texels[0] >> 16) & 0xFF));
            free(px);
        }
    }
    /* the same bytes again: not written twice (the index has one line) */
    nv2a_texpack_dump(guest, 0x06, 16, 16, 64, 2, NULL, 0, texels);
    /* a P8 texture with two palettes: two files */
    nv2a_texpack_dump(guest, NV2A_TEX_P8, 16, 16, 16, 1, (const uint8_t *)pal, 256, texels);
    pal[5] = 0xFF123456u;
    nv2a_texpack_dump(guest, NV2A_TEX_P8, 16, 16, 16, 1, (const uint8_t *)pal, 256, texels);
    CHECK(nv2a_texpack_key(guest, NV2A_TEX_P8, 16, 16, 16, (const uint8_t *)pal, 256, &hash, &palh) && palh);
    nv2a_texpack_key_name(hash, palh, x, sizeof x);
    path(idx, sizeof idx, "dump", x);
    f = fopen(idx, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    /* the level 1 file */
    nv2a_texpack_key_name(hash, 0, x, sizeof x);
    path(idx, sizeof idx, "dump", "index.tsv");
    f = fopen(idx, "rb");
    CHECK(f != NULL);
    while (f && fgets(line, sizeof line, f))
        lines++;
    if (f) fclose(f);
    CHECK(lines == 3);
}

int main(int argc, char **argv)
{
    char env[1300], dd[600], ee[600];
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    snprintf(s_dir, sizeof s_dir, "%s", argv[1]);
    mk(s_dir);
    path(dd, sizeof dd, "dump", "");
    path(ee, sizeof ee, "env", "");
    mk(dd);
    snprintf(env, sizeof env, "texrep_dump=%s,texrep_dump_mips,texpack=%s", dd, ee);
    setenv("RECOMP_DEBUG", env, 1);
    unsetenv("SteamDeck");

    names();
    scales();
    order();
    pixels();
    files_and_jobs();
    dump();
    printf("nv2a_texpack: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/* nv2a_texmip_mode's alpha_mips_ab reads the flip count; never set here. */
#include "nv2a_pb_state.h"
const struct nv2a_pb_gpu *nv2a_pb_gpu_state(void)
{
    static struct nv2a_pb_gpu g;
    return &g;
}

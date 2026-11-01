/* nv2a_texup: the rule, the keys, the road skip list, the governor and the
 * disk store of graphics.texture_upscale (BO-245). No GPU. */
#include "nv2a_texup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_checks, s_fail;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct nv2a_texup_tex world(void)
{
    struct nv2a_texup_tex t;
    memset(&t, 0, sizeof t);
    t.size_from_format = 1;
    t.w = 256;
    t.h = 128;
    t.levels = 8;
    t.draw_3d = 1;
    return t;
}

static void test_rule(void)
{
    struct nv2a_texup_tex t = world();
    CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_OK);
    t = world(); t.size_from_format = 0;  CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_NOT_COLOUR);
    t = world(); t.sign = 1;              CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_NOT_COLOUR);
    t = world(); t.cube = 1;              CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_KIND);
    t = world(); t.linear = 1;            CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_KIND);
    t = world(); t.render_target = 1;     CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_KIND);
    t = world(); t.replaced = 1;          CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_KIND);
    t = world(); t.w = 8;                 CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_SIZE);
    t = world(); t.h = 15;                CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_SIZE);
    t = world(); t.w = 16; t.h = 16;      CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_OK);
    t = world(); t.w = 2048;              CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_SIZE);
    t = world(); t.w = 1024;              CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_OK);
    t = world(); t.ever_2d = 1;           CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_2D);
    t = world(); t.draw_3d = 0;           CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_2D);
    t = world(); t.levels = 1;            CHECK(nv2a_texup_rule(&t) == NV2A_TEXUP_OK);
}

static void test_setting(void)
{
    uint32_t g0, g;
#if !defined(_WIN32)
    unsetenv("SteamDeck");
#endif
    g0 = nv2a_texup_generation();
    CHECK(nv2a_texup_algo() == NV2A_TEXUP_OFF);
    CHECK(nv2a_texup_factor_for(64, 64) == 0);
    nv2a_texup_set_algo(NV2A_TEXUP_FSRCNNX);
    g = nv2a_texup_generation();
    CHECK(g != g0 && g != 0);
    nv2a_texup_set_algo(NV2A_TEXUP_FSRCNNX);
    CHECK(nv2a_texup_generation() == g);          /* no change, no bump */
    CHECK(nv2a_texup_factor_for(256, 256) == 2);
    nv2a_texup_set_scale(4);
    CHECK(nv2a_texup_generation() != g);
    CHECK(nv2a_texup_factor_for(256, 256) == 4);
    CHECK(nv2a_texup_factor_for(512, 256) == 2);  /* 4x only up to 256 a side */
    g = nv2a_texup_generation();
    nv2a_texup_set_roads(1);
    CHECK(nv2a_texup_generation() != g);
    nv2a_texup_set_roads(0);
#if !defined(_WIN32)
    /* A Steam Deck is offered 2x at most: 4x asked runs as 2x. */
    CHECK(nv2a_texup_max_scale() == 4);
    setenv("SteamDeck", "1", 1);
    CHECK(nv2a_texup_max_scale() == 2);
    nv2a_texup_set_scale(4);
    CHECK(nv2a_texup_scale() == 2);
    CHECK(nv2a_texup_factor_for(256, 256) == 2);
    unsetenv("SteamDeck");
    nv2a_texup_set_scale(4);
    CHECK(nv2a_texup_scale() == 4);
#endif
    nv2a_texup_set_scale(2);
    nv2a_texup_set_algo(99);
    CHECK(nv2a_texup_algo() == NV2A_TEXUP_OFF);
}

static void test_skip(const char *dir)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/roads.skip", dir);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fprintf(f, "# road surfaces\n8aae1ed31a65b0c8\na7f60a455624dcd6  HK_Road2\n");
    fclose(f);
    nv2a_texup_set_skip_file(path);
    CHECK(nv2a_texup_is_road(0x8aae1ed31a65b0c8ull));
    CHECK(nv2a_texup_is_road(0xa7f60a455624dcd6ull));
    CHECK(!nv2a_texup_is_road(0x1234ull));
    nv2a_texup_set_roads(0);
    CHECK(nv2a_texup_skip(0x8aae1ed31a65b0c8ull));    /* Off: left as it is */
    nv2a_texup_set_roads(1);
    CHECK(!nv2a_texup_skip(0x8aae1ed31a65b0c8ull));   /* On: upscaled too */
    nv2a_texup_set_roads(0);
    CHECK(!nv2a_texup_skip(0x1234ull));
    {
        /* A title's built-in list adds to the file's. */
        static const uint64_t k[] = { 0x1234ull, 0x99ull };
        nv2a_texup_set_skip_list(k, 2);
        CHECK(nv2a_texup_skip(0x1234ull) && nv2a_texup_skip(0x99ull));
        CHECK(nv2a_texup_skip(0x8aae1ed31a65b0c8ull));
        nv2a_texup_set_skip_list(NULL, 0);
        CHECK(!nv2a_texup_skip(0x1234ull));
    }
}

static void test_key(void)
{
    struct nv2a_texup_key a, b;
    char n1[200], n2[200];
    uint64_t id;
    memset(&a, 0, sizeof a);
    a.hash = 0x1122334455667788ull;
    a.w = 256; a.h = 128; a.format = 0x0F; a.algo = 1; a.scale = 2; a.wrap = 3;
    a.shaders = 0xABCDEFull;
    id = nv2a_texup_key_id(&a);
    CHECK(id != 0);
    b = a; CHECK(nv2a_texup_key_id(&b) == id);
    b = a; b.hash ^= 1;   CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.w = 128;     CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.h = 256;     CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.format = 0x0C; CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.algo = 2;    CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.scale = 4;   CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.wrap = 1;    CHECK(nv2a_texup_key_id(&b) != id);
    b = a; b.shaders = 1; CHECK(nv2a_texup_key_id(&b) != id);
    nv2a_texup_key_name(&a, n1, sizeof n1);
    b = a; b.algo = 2;
    nv2a_texup_key_name(&b, n2, sizeof n2);
    CHECK(strcmp(n1, n2) != 0);
    CHECK(strstr(n1, "1122334455667788") && strstr(n1, "fsrcnnx") && strstr(n1, "x2")
          && strstr(n1, ".tup"));
    b = a; b.shaders = 2;
    nv2a_texup_key_name(&b, n2, sizeof n2);
    CHECK(strcmp(n1, n2) != 0);                  /* a new shader: a new file */
}

#define MS 1000000ull

static void test_gov(void)
{
    struct nv2a_texup_gov g;
    uint64_t t = 1000 * MS;
    int i;
    nv2a_texup_gov_init(&g);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0);   /* nothing measured yet */
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 6.0, 16.7, 16.7, 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.5);   /* racing cap */
    nv2a_texup_gov_frame(&g, t += 16 * MS, 6.0, 16.7, 16.7, 0, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 4.0);   /* nothing 3D: the idle cap */
    /* A heavy GPU: p95 near the period leaves no room. */
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 15.0, 16.7, 16.7, 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0);
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 14.5, 16.7, 16.7, 0, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) > 0.0 && nv2a_texup_gov_budget(&g, t) < 0.3);
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 4.0, 16.7, 16.7, 100, 0, 0);
    /* A burst of uploads: 2 s of no compute. */
    nv2a_texup_gov_frame(&g, t += 16 * MS, 4.0, 16.7, 16.7, 100, 40, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0);
    CHECK(nv2a_texup_gov_budget(&g, t + 1900 * MS) == 0.0);
    CHECK(nv2a_texup_gov_budget(&g, t + 2100 * MS) == 0.5);
    t += 2100 * MS;
    /* Late frames without compute: nothing paused. */
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 0);
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 0);
    CHECK(g.pauses == 0 && nv2a_texup_gov_budget(&g, t) == 0.5);
    /* One late frame while computing: not yet; two in a row: 2 s pause. */
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 1);
    CHECK(g.pauses == 0);
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 1);
    CHECK(g.pauses == 1);
    CHECK(nv2a_texup_gov_budget(&g, t + 1900 * MS) == 0.0);
    CHECK(nv2a_texup_gov_budget(&g, t + 2100 * MS) == 0.5);
    t += 2100 * MS;
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 1);
    nv2a_texup_gov_frame(&g, t += 33 * MS, 4.0, 33.4, 16.7, 100, 0, 1);
    CHECK(g.pauses == 2);                          /* the second pause: 4 s */
    CHECK(nv2a_texup_gov_budget(&g, t + 3900 * MS) == 0.0);
    CHECK(nv2a_texup_gov_budget(&g, t + 4100 * MS) == 0.5);
    /* The cost factor follows a slow GPU at once, a fast one slowly. */
    CHECK(g.factor[NV2A_TEXUP_FSRCNNX] == 1.0);
    nv2a_texup_gov_measured(&g, NV2A_TEXUP_FSRCNNX, 0.4, 1.2);
    CHECK(g.factor[NV2A_TEXUP_FSRCNNX] > 1.9 && g.factor[NV2A_TEXUP_FSRCNNX] < 2.1);
    nv2a_texup_gov_measured(&g, NV2A_TEXUP_FSRCNNX, 0.4, 0.1);
    CHECK(g.factor[NV2A_TEXUP_FSRCNNX] < 2.0 && g.factor[NV2A_TEXUP_FSRCNNX] > 1.5);
    CHECK(g.factor[NV2A_TEXUP_NNEDI3] == 1.0);
    CHECK(nv2a_texup_gov_swap_bytes(&g) == (4u << 20));
    /* A disk result over the slice goes alone, first in its flip; never
     * after another in the same flip. */
    CHECK(nv2a_texup_swap_fits(1u << 20, 4u << 20, 4u << 20));
    CHECK(nv2a_texup_swap_fits(5u << 20, 4u << 20, 4u << 20));
    CHECK(!nv2a_texup_swap_fits(5u << 20, 3u << 20, 4u << 20));
    CHECK(nv2a_texup_swap_fits(3u << 20, 3u << 20, 4u << 20));
    CHECK(!nv2a_texup_swap_fits(2u << 20, 0, 4u << 20));
}

static void test_store(const char *dir)
{
    struct nv2a_texup_key k;
    uint32_t px[64 * 64], *got = NULL;
    size_t bytes = 0, i;
    int r, n;
    char name[200], path[1200];
    FILE *f;

    nv2a_texup_store_dir(dir);
    memset(&k, 0, sizeof k);
    k.hash = 42; k.w = 32; k.h = 32; k.format = 0x0C; k.algo = 1; k.scale = 2; k.shaders = 7;
    for (i = 0; i < 64 * 64; i++)
        px[i] = (uint32_t)(i * 2654435761u);
    /* Not there: pending, then absent. */
    r = nv2a_texup_load(&k, &got, &bytes);
    CHECK(r == NV2A_TEXUP_PENDING);
    nv2a_texup_store_flush(5000);
    r = nv2a_texup_load(&k, &got, &bytes);
    CHECK(r == NV2A_TEXUP_ABSENT);
    nv2a_texup_save(&k, px, sizeof px);
    nv2a_texup_store_flush(5000);
    for (n = 0, r = NV2A_TEXUP_PENDING; r == NV2A_TEXUP_PENDING && n < 100; n++) {
        r = nv2a_texup_load(&k, &got, &bytes);
        nv2a_texup_store_flush(5000);
    }
    CHECK(r == NV2A_TEXUP_READY && bytes == sizeof px && got && !memcmp(got, px, sizeof px));
    free(got);
    /* Another algorithm's file is another file. */
    k.algo = 2;
    r = nv2a_texup_load(&k, &got, &bytes);
    nv2a_texup_store_flush(5000);
    r = nv2a_texup_load(&k, &got, &bytes);
    CHECK(r == NV2A_TEXUP_ABSENT);
    /* A damaged file is removed, not used. */
    k.algo = 1;
    nv2a_texup_key_name(&k, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) {
        fseek(f, 200, SEEK_SET);
        fputc(0x5A, f);
        fclose(f);
    }
    r = nv2a_texup_load(&k, &got, &bytes);
    nv2a_texup_store_flush(5000);
    r = nv2a_texup_load(&k, &got, &bytes);
    CHECK(r == NV2A_TEXUP_ABSENT);
    f = fopen(path, "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* BO-245 cold hitch: the writes are paced (16 MB/s here, each 512 KB piece
 * synced) and a read goes before the writes waiting. */
static void test_store_paced(void)
{
    static uint32_t big[1024 * 512];
    struct nv2a_texup_key k;
    uint32_t *got = NULL;
    size_t bytes = 0, i;
    double t0, t1;
    int r, n;

    memset(&k, 0, sizeof k);
    k.hash = 77; k.w = 512; k.h = 256; k.format = 0x0C; k.algo = 1; k.scale = 2; k.shaders = 7;
    for (i = 0; i < 1024 * 512; i++)
        big[i] = (uint32_t)(i * 2246822519u);
    t0 = now_s();
    nv2a_texup_save(&k, big, sizeof big);   /* 2 MB: >= 125 ms at 16 MB/s */
    CHECK(nv2a_texup_store_flush(10000));
    t1 = now_s();
    CHECK(t1 - t0 >= 0.11);
    for (n = 0, r = NV2A_TEXUP_PENDING; r == NV2A_TEXUP_PENDING && n < 100; n++) {
        r = nv2a_texup_load(&k, &got, &bytes);
        nv2a_texup_store_flush(5000);
    }
    CHECK(r == NV2A_TEXUP_READY && bytes == sizeof big && got && !memcmp(got, big, sizeof big));
    free(got);
    /* Four writes (some 0.5 s), then a read: answered while writes remain. */
    for (i = 0; i < 4; i++) {
        k.hash = 100 + i;
        nv2a_texup_save(&k, big, sizeof big);
    }
    k.hash = 999;
    r = nv2a_texup_load(&k, &got, &bytes);
    CHECK(r == NV2A_TEXUP_PENDING);
    for (n = 0; r == NV2A_TEXUP_PENDING && n < 2000; n++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        r = nv2a_texup_load(&k, &got, &bytes);
    }
    CHECK(r == NV2A_TEXUP_ABSENT);
    CHECK(!nv2a_texup_store_flush(0));      /* the writes are still going */
    CHECK(nv2a_texup_store_flush(10000));
}

/* The result check: a good 2x result passes; zeros, a black result over a
 * bright title, a wrong alpha are refused; a black title's zeros pass. */
static void test_result_ok(void)
{
    enum { W = 32, H = 16 };
    static uint8_t t[W * H * 4], r[W * 4 * H * 4 * 4];  /* room for the 4x pass */
    unsigned x, y, s;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t *p = t + (y * W + x) * 4;
            p[0] = 200; p[1] = (uint8_t)(x * 7); p[2] = 40; p[3] = (uint8_t)(y & 1 ? 255 : 0);
        }
    for (s = 2; s <= 4; s += 2) {
        for (y = 0; y < H * s; y++)
            for (x = 0; x < W * s; x++)
                memcpy(r + ((size_t)y * W * s + x) * 4, t + ((y / s) * W + x / s) * 4, 4);
        CHECK(nv2a_texup_result_ok(r, t, W, H, s));
    }
    for (y = 0; y < H * 2; y++)
        for (x = 0; x < W * 2; x++)
            memcpy(r + ((size_t)y * W * 2 + x) * 4, t + ((y / 2) * W + x / 2) * 4, 4);
    r[((size_t)1 * W * 2 + 1) * 4 + 1] ^= 9;              /* a sharpened texel: fine */
    CHECK(nv2a_texup_result_ok(r, t, W, H, 2));
    memset(r, 0, sizeof r);                                /* a faulted job: zeros */
    CHECK(!nv2a_texup_result_ok(r, t, W, H, 2));
    for (y = 0; y < H * 2; y++)                             /* black, alpha kept */
        for (x = 0; x < W * 2; x++)
            r[((size_t)y * W * 2 + x) * 4 + 3] = t[((y / 2) * W + x / 2) * 4 + 3];
    CHECK(!nv2a_texup_result_ok(r, t, W, H, 2));
    memset(t, 0, sizeof t);                                /* a black, clear title */
    memset(r, 0, sizeof r);
    CHECK(nv2a_texup_result_ok(r, t, W, H, 2));
    CHECK(!nv2a_texup_result_ok(NULL, t, W, H, 2));
    CHECK(!nv2a_texup_result_ok(r, t, W, H, 3));
}

/* BO-272: the title's loading signal. */
static void test_gov_loading(void)
{
    struct nv2a_texup_gov g;
    uint64_t t = 1000 * MS;
    int i;
    nv2a_texup_set_loading(5);
    CHECK(nv2a_texup_loading() == NV2A_TEXUP_PLAY);   /* an unknown value is play */
    nv2a_texup_set_loading(NV2A_TEXUP_PRESTART);
    CHECK(nv2a_texup_loading() == NV2A_TEXUP_PRESTART);
    nv2a_texup_set_loading(NV2A_TEXUP_PLAY);
    CHECK(nv2a_texup_loading() == NV2A_TEXUP_PLAY);
    nv2a_texup_gov_init(&g);
    CHECK(nv2a_texup_gov_jobs(&g) == 1 && nv2a_texup_gov_rt_ms(&g) == 0.0);
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 2.0, 16.7, 16.7, 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.5);
    /* Play: an upload burst stops compute for 2 s; the load ends it. */
    nv2a_texup_gov_frame(&g, t += 16 * MS, 2.0, 16.7, 16.7, 100, 40, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0);
    nv2a_texup_gov_loading(&g, NV2A_TEXUP_LOADING);
    CHECK(nv2a_texup_gov_budget(&g, t) == 2.0);       /* nothing measured yet: 2 ms */
    for (i = 0; i < 3; i++)
        nv2a_texup_gov_measured(&g, NV2A_TEXUP_FSRCNNX, 1.0, 1.0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 6.0);       /* 16.7 - 2 - 2 = 12.7 room, cap 6 */
    CHECK(nv2a_texup_gov_jobs(&g) == 16 && nv2a_texup_gov_rt_ms(&g) == 6.0);
    CHECK(nv2a_texup_gov_swap_bytes(&g) == (32u << 20));
    /* Loading: bursts and the title's own late frames stop nothing. */
    nv2a_texup_gov_frame(&g, t += 16 * MS, 2.0, 16.7, 16.7, 0, 200, 1);
    CHECK(nv2a_texup_gov_budget(&g, t) == 6.0);
    for (i = 0; i < 6; i++)
        nv2a_texup_gov_frame(&g, t += 80 * MS, 2.0, 80.0, 16.7, 0, 0, 1);
    CHECK(g.pauses == 0 && nv2a_texup_gov_budget(&g, t) == 6.0);
    /* The countdown: 3D drawn; the cap 4, within the room. */
    nv2a_texup_gov_loading(&g, NV2A_TEXUP_PRESTART);
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 2.0, 16.7, 16.7, 100, 30, 1);
    CHECK(nv2a_texup_gov_budget(&g, t) == 4.0);
    CHECK(nv2a_texup_gov_jobs(&g) == 4 && nv2a_texup_gov_swap_bytes(&g) == (32u << 20));
    /* A late frame while computing: the next frame computes nothing, no
     * pause; the one after computes again. */
    nv2a_texup_gov_frame(&g, t += 30 * MS, 2.0, 30.0, 16.7, 100, 0, 1);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0 && g.pauses == 0);
    nv2a_texup_gov_frame(&g, t += 16 * MS, 2.0, 16.7, 16.7, 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 4.0);
    /* A heavy GPU in the countdown: the room, not the cap. */
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 16 * MS, 12.7, 16.7, 16.7, 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) > 1.9 && nv2a_texup_gov_budget(&g, t) < 2.1);
    /* 120 Hz: the period from the host grid. */
    CHECK(nv2a_texup_period_ms(8333333u, 1) > 8.33 && nv2a_texup_period_ms(8333333u, 1) < 8.34);
    CHECK(nv2a_texup_period_ms(0, 2) > 33.3 && nv2a_texup_period_ms(0, 2) < 33.4);
    for (i = 0; i < 60; i++)
        nv2a_texup_gov_frame(&g, t += 8 * MS, 4.0, 8.3, nv2a_texup_period_ms(8333333u, 1), 100, 0, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) > 2.3 && nv2a_texup_gov_budget(&g, t) < 2.4);
    /* Back to play: the in-race rules. */
    nv2a_texup_gov_loading(&g, NV2A_TEXUP_PLAY);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.5 && nv2a_texup_gov_jobs(&g) == 1);
    CHECK(nv2a_texup_gov_swap_bytes(&g) == (4u << 20));
    nv2a_texup_gov_frame(&g, t += 8 * MS, 3.0, 8.3, 8.33, 100, 40, 0);
    CHECK(nv2a_texup_gov_budget(&g, t) == 0.0);       /* the burst rule is back */
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    setenv("RECOMP_DEBUG", "texup_store_mbps=16", 1);
    test_rule();
    test_setting();
    test_skip(dir);
    test_key();
    test_gov();
    test_gov_loading();
    test_store(dir);
    test_store_paced();
    test_result_ok();
    printf("nv2a_texup: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

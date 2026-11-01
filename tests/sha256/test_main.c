/*
 * sha256 - xr_sha256.c: the standard's vectors with whichever code this CPU
 * gets, then the hardware code against the portable code on random buffers:
 * every length from 0 to 300 bytes, long buffers, and the same data fed in
 * pieces of random sizes. On a CPU without the instructions the comparison
 * is the portable code with itself, and the test says so.
 *
 *   sha256_test [--bench]     --bench also times 256 MB with each
 */
#include "xr_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            g_fail++; \
        } \
    } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void hash(const uint8_t *p, size_t n, char hex[65])
{
    xr_sha256 s;
    uint8_t d[32];
    xr_sha256_init(&s);
    xr_sha256_update(&s, p, n);
    xr_sha256_final(&s, d);
    xr_sha256_hex(d, hex);
}

/* The same bytes in pieces of random sizes (0 to 200 bytes each). */
static void hash_pieces(const uint8_t *p, size_t n, char hex[65])
{
    xr_sha256 s;
    uint8_t d[32];
    size_t at = 0;
    xr_sha256_init(&s);
    while (at < n) {
        size_t take = rnd() % 201;
        if (take > n - at)
            take = n - at;
        xr_sha256_update(&s, p + at, take);
        at += take;
    }
    xr_sha256_final(&s, d);
    xr_sha256_hex(d, hex);
}

static void vectors(const char *which)
{
    static const struct { const char *text; const char *hex; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
    };
    char hex[65];
    uint8_t *a = malloc(1000000);
    unsigned i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        hash((const uint8_t *)v[i].text, strlen(v[i].text), hex);
        CHECK(!strcmp(hex, v[i].hex), "%s: vector %u gave %s", which, i, hex);
    }
    memset(a, 'a', 1000000);
    hash(a, 1000000, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "%s: a million 'a' gave %s", which, hex);
    hash_pieces(a, 1000000, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "%s: a million 'a' in pieces gave %s", which, hex);
    free(a);
}

static double seconds(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    const char *hw;
    uint8_t *buf;
    char a[65], b[65];
    size_t n;
    int round, compared = 0;

    xr_sha256_use_portable(0);
    hw = xr_sha256_backend();
    printf("this CPU hashes with: %s\n", hw);
    vectors(hw);
    xr_sha256_use_portable(1);
    CHECK(!strcmp(xr_sha256_backend(), "c"), "the portable code could not be chosen");
    vectors("c");

    /* Hardware against portable. */
    buf = malloc(4 * 1024 * 1024 + 64);
    for (n = 0; n < 4 * 1024 * 1024 + 64; n++)
        buf[n] = (uint8_t)rnd();
    for (n = 0; n <= 300; n++) {
        const uint8_t *p = buf + (rnd() % 1000);       /* any alignment */
        xr_sha256_use_portable(1);
        hash(p, n, a);
        xr_sha256_use_portable(0);
        hash(p, n, b);
        CHECK(!strcmp(a, b), "%zu bytes: c %s, %s %s", n, a, hw, b);
        hash_pieces(p, n, b);
        CHECK(!strcmp(a, b), "%zu bytes in pieces: c %s, %s %s", n, a, hw, b);
        compared += 2;
    }
    for (round = 0; round < 200; round++) {
        size_t off = rnd() % 64;
        n = rnd() % (4 * 1024 * 1024);
        if (round < 150)
            n %= 70000;
        xr_sha256_use_portable(1);
        hash(buf + off, n, a);
        xr_sha256_use_portable(0);
        hash(buf + off, n, b);
        CHECK(!strcmp(a, b), "%zu bytes at +%zu: c %s, %s %s", n, off, a, hw, b);
        hash_pieces(buf + off, n, b);
        CHECK(!strcmp(a, b), "%zu bytes at +%zu in pieces: c %s, %s %s", n, off, a, hw, b);
        compared += 2;
    }
    printf("%d comparisons of %s with the portable code%s\n", compared, hw,
           strcmp(hw, "c") ? "" : " (this CPU has no SHA instructions: the portable code with itself)");

    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            double t0;
            int i;
            xr_sha256_use_portable(pass == 0);
            t0 = seconds();
            for (i = 0; i < 64; i++)
                hash(buf, 4 * 1024 * 1024, a);
            t0 = seconds() - t0;
            printf("%-10s 256 MB in %.2f s, %.0f MB/s\n", xr_sha256_backend(), t0, 256.0 / t0);
        }
    }
    free(buf);
    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("OK\n");
    return 0;
}

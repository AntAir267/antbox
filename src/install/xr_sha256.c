/*
 * xr_sha256.c - SHA-256 (FIPS 180-4), written from the standard. See
 * xr_sha256.h.
 */
#include "xr_sha256.h"

#include <stdlib.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block_c(uint32_t *state, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16
             | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void blocks_c(uint32_t state[8], const uint8_t *p, size_t n)
{
    for (; n; n--, p += 64)
        block_c(state, p);
}

/* ── the CPU's own SHA-256 instructions ──────────────────────────────
 * The same compression function, four rounds at a time; K is read four words
 * at a time in memory order, which is the order both instruction sets want.
 * W[i] (four words) = su1/msg2(su0/msg1(W[i-4], W[i-3]) ..., W[i-2], W[i-1]). */
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#define XR_SHA_HW "sha-ni"
#include <cpuid.h>
#include <immintrin.h>

__attribute__((target("sha,sse4.1,ssse3")))
static void blocks_hw(uint32_t state[8], const uint8_t *p, size_t n)
{
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128((const __m128i *)&state[0]);    /* DCBA */
    __m128i s1 = _mm_loadu_si128((const __m128i *)&state[4]);     /* HGFE */
    __m128i s0;

    tmp = _mm_shuffle_epi32(tmp, 0xB1);          /* CDAB */
    s1 = _mm_shuffle_epi32(s1, 0x1B);            /* EFGH */
    s0 = _mm_alignr_epi8(tmp, s1, 8);            /* ABEF */
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);         /* CDGH */
    for (; n; n--, p += 64) {
        const __m128i save0 = s0, save1 = s1;
        __m128i w[4];
        int i;

        for (i = 0; i < 16; i++) {
            __m128i msg;
            if (i < 4) {
                w[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), swap);
            } else {
                __m128i t = _mm_sha256msg1_epu32(w[i & 3], w[(i + 1) & 3]);
                t = _mm_add_epi32(t, _mm_alignr_epi8(w[(i + 3) & 3], w[(i + 2) & 3], 4));
                w[i & 3] = _mm_sha256msg2_epu32(t, w[(i + 3) & 3]);
            }
            msg = _mm_add_epi32(w[i & 3], _mm_loadu_si128((const __m128i *)&K[i * 4]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
        }
        s0 = _mm_add_epi32(s0, save0);
        s1 = _mm_add_epi32(s1, save1);
    }
    tmp = _mm_shuffle_epi32(s0, 0x1B);           /* FEBA */
    s1 = _mm_shuffle_epi32(s1, 0xB1);            /* DCHG */
    s0 = _mm_blend_epi16(tmp, s1, 0xF0);         /* DCBA */
    s1 = _mm_alignr_epi8(s1, tmp, 8);            /* HGFE */
    _mm_storeu_si128((__m128i *)&state[0], s0);
    _mm_storeu_si128((__m128i *)&state[4], s1);
}

static int hw_present(void)
{
    unsigned a, b, c, d;

    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 9)) || !(c & (1u << 19)))
        return 0;                                /* SSSE3, SSE4.1 */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
        return 0;
    return (b >> 29) & 1;                        /* SHA */
}

#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__)) \
    && (defined(__APPLE__) || defined(__linux__))
#define XR_SHA_HW "armv8-sha2"
#include <arm_neon.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif

#if !defined(__ARM_FEATURE_SHA2)
#if defined(__clang__)
__attribute__((target("sha2")))
#else
__attribute__((target("+sha2")))
#endif
#endif
static void blocks_hw(uint32_t state[8], const uint8_t *p, size_t n)
{
    uint32x4_t s0 = vld1q_u32(&state[0]), s1 = vld1q_u32(&state[4]);

    for (; n; n--, p += 64) {
        const uint32x4_t save0 = s0, save1 = s1;
        uint32x4_t w[4];
        int i;

        for (i = 0; i < 16; i++) {
            uint32x4_t msg, was0;
            if (i < 4)
                w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
            else
                w[i & 3] = vsha256su1q_u32(vsha256su0q_u32(w[i & 3], w[(i + 1) & 3]),
                                           w[(i + 2) & 3], w[(i + 3) & 3]);
            msg = vaddq_u32(w[i & 3], vld1q_u32(&K[i * 4]));
            was0 = s0;
            s0 = vsha256hq_u32(s0, s1, msg);
            s1 = vsha256h2q_u32(s1, was0, msg);
        }
        s0 = vaddq_u32(s0, save0);
        s1 = vaddq_u32(s1, save1);
    }
    vst1q_u32(&state[0], s0);
    vst1q_u32(&state[4], s1);
}

static int hw_present(void)
{
#if defined(__APPLE__)
    int have = 0;
    size_t len = sizeof have;
    if (sysctlbyname("hw.optional.arm.FEAT_SHA256", &have, &len, NULL, 0) != 0)
        return 0;
    return have != 0;
#else
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#endif
}
#endif

/* Which one: decided once. The hardware code must exist in this build, the
 * CPU must report the instructions, XR_SHA256=c must not ask for the portable
 * code, and the two must agree on a few blocks first; else the portable code. */
static int s_hw = -1;            /* -1: not decided */
static int s_portable_forced;

#ifdef XR_SHA_HW
static int hw_decide(void)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const char *e = getenv("XR_SHA256");
    uint8_t data[64 * 5];
    uint32_t a[8], b[8];
    unsigned i;

    if (e && !strcmp(e, "c"))
        return 0;
    if (!hw_present())
        return 0;
    for (i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 151u + (i >> 3) * 29u + 7u);
    memcpy(a, iv, sizeof iv);
    memcpy(b, iv, sizeof iv);
    blocks_c(a, data, 5);
    blocks_hw(b, data, 2);       /* in two calls: the state is carried */
    blocks_hw(b, data + 128, 3);
    return memcmp(a, b, sizeof a) == 0;
}
#endif

static void blocks(uint32_t state[8], const uint8_t *p, size_t n)
{
#ifdef XR_SHA_HW
    if (s_hw < 0)
        s_hw = hw_decide();
    if (s_hw && !s_portable_forced) {
        blocks_hw(state, p, n);
        return;
    }
#endif
    blocks_c(state, p, n);
}

const char *xr_sha256_backend(void)
{
#ifdef XR_SHA_HW
    if (s_hw < 0)
        s_hw = hw_decide();
    if (s_hw && !s_portable_forced)
        return XR_SHA_HW;
#endif
    return "c";
}

void xr_sha256_use_portable(int on)
{
    s_portable_forced = on != 0;
}

void xr_sha256_init(xr_sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0;
    s->fill = 0;
}

void xr_sha256_update(xr_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    s->bytes += n;
    if (s->fill) {
        size_t take = 64 - s->fill;
        if (take > n)
            take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += (unsigned)take;
        p += take;
        n -= take;
        if (s->fill < 64)
            return;
        blocks(s->h, s->buf, 1);
        s->fill = 0;
    }
    if (n >= 64) {
        size_t whole = n / 64;
        blocks(s->h, p, whole);
        p += whole * 64;
        n -= whole * 64;
    }
    if (n) {
        memcpy(s->buf, p, n);
        s->fill = (unsigned)n;
    }
}

void xr_sha256_final(xr_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;
    int i;

    s->buf[s->fill++] = 0x80;
    if (s->fill > 56) {
        memset(s->buf + s->fill, 0, 64 - s->fill);
        blocks(s->h, s->buf, 1);
        s->fill = 0;
    }
    memset(s->buf + s->fill, 0, 56 - s->fill);
    for (i = 0; i < 8; i++)
        s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    blocks(s->h, s->buf, 1);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void xr_sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

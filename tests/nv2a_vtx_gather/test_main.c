/*
 * nv2a_vtx_gather: the vertex gather's fast paths (BO-282) against the
 * gather as it was (ref_gather.c, toolkit e01f04c4), bit for bit.
 *
 *   1. The decode tables: nv2a_vtx_unorm8[b] is b / 255.0f for all 256
 *      bytes, nv2a_vtx_snorm16(s) is s / 32767.0f clamped to -1 for all
 *      65536 shorts, the CMP fields' tables are the old decode's quotients
 *      for all 2048 and 1024 values, each against the division done at
 *      run time.
 *   2. nv2a_vtx_decode against the old decode: every type 0..15, sizes
 *      0..7, random bytes and the bytes that make the edge values.
 *   3. Synthetic draws: random inputs (every type, sizes 0..7, strides
 *      0..64, no array offset, inline payloads cut short at every length
 *      around an element), up to 16 inputs, slots repeated.
 *   4. Recorded draws (RECOMP_DEBUG=vtx_record, an autopilot race):
 *      data/race.vtxr (the packed sample, see pack), or the files named on
 *      the command line. Each draw's guest bytes are put back at their own
 *      addresses, so the two gathers read what the game's did.
 *
 *   3b. Guest arrays: the race's vertex layouts (interleaved and separate
 *      streams) over a fake guest region at the guest's own addresses.
 *
 *   nv2a_vtx_gather_test [file.vtxr ...]
 *       with no file: data/race.vtxr, and with NV2A_TEST_RECORDINGS=<dir>
 *       also <dir>/race.vtxr (the full recording, kept outside git beside
 *       the golden frames: the release gate's run)
 *   nv2a_vtx_gather_test bench file.vtxr [reps]   ns per draw, old and new
 *   nv2a_vtx_gather_test pack in.vtxr out.vtxr     a small sample of a
 *       recording: per kind of draw (its inputs' types, sizes and strides,
 *       inline or not) up to 3 draws, an array draw's vertices cut to 48,
 *       every byte the gather reads (the vertices, the inline payloads, the
 *       SET_VERTEX_DATA values) replaced by random ones. The sample keeps
 *       the race's draws' shapes and none of the game's data, so it can go
 *       in git (data/race.vtxr); a full recording stays outside, like the
 *       golden frames.
 *
 * Exit 0 when every output is identical, 1 otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "nv2a_backend_common.h"
#include "ref_gather.h"

static int s_fail;

#define CHECK(c, ...) do { if (!(c)) { s_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* A small reproducible generator (xorshift64*). */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return (uint32_t)((s_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

/* ---- 1. the tables -------------------------------------------------- */

static void check_tables(void)
{
    volatile float d255 = 255.0f, d32767 = 32767.0f;
    int i, bad = 0;
    for (i = 0; i < 256; i++) {
        float want = (float)i / d255;
        if (f2u(nv2a_vtx_unorm8[i]) != f2u(want) && bad++ < 4)
            fprintf(stderr, "  unorm8[%d] %08X, want %08X\n", i,
                    f2u(nv2a_vtx_unorm8[i]), f2u(want));
    }
    CHECK(!bad, "%d of 256 unorm8 entries differ from b / 255.0f", bad);
    bad = 0;
    for (i = -32768; i < 32768; i++) {
        float want = (float)(int16_t)i / d32767;
        want = want < -1.0f ? -1.0f : want;
        if (f2u(nv2a_vtx_snorm16((int16_t)i)) != f2u(want) && bad++ < 4)
            fprintf(stderr, "  snorm16(%d) %08X, want %08X\n", i,
                    f2u(nv2a_vtx_snorm16((int16_t)i)), f2u(want));
    }
    CHECK(!bad, "%d of 65536 snorm16 values differ from s / 32767.0f", bad);
    bad = 0;
    {
        volatile float d1023 = 1023.0f, d511 = 511.0f;
        for (i = 0; i < 2048; i++) {
            /* The field's value as the old decode took it: the bits
             * shifted to the top and back, sign extended. */
            int32_t x = (int32_t)((uint32_t)i << 21) >> 21;
            float want = (float)x / d1023;
            if (f2u(nv2a_vtx_cmp11[i]) != f2u(want) && bad++ < 4)
                fprintf(stderr, "  cmp11[%d] %08X, want %08X\n", i,
                        f2u(nv2a_vtx_cmp11[i]), f2u(want));
        }
        for (i = 0; i < 1024; i++) {
            int32_t z = (int32_t)((uint32_t)i << 22) >> 22;
            float want = (float)z / d511;
            if (f2u(nv2a_vtx_cmp10[i]) != f2u(want) && bad++ < 4)
                fprintf(stderr, "  cmp10[%d] %08X, want %08X\n", i,
                        f2u(nv2a_vtx_cmp10[i]), f2u(want));
        }
    }
    CHECK(!bad, "%d of 3072 CMP field values differ from the division", bad);
    printf("tables: 256 unorm8, 65536 snorm16, 2048 + 1024 CMP fields checked\n");
}

/* ---- 2. nv2a_vtx_decode --------------------------------------------- */

static void check_decode(void)
{
    static const uint8_t edge[][16] = {
        { 0 }, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
        { 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80 },     /* -32768 */
        { 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80 },     /* -32767 */
        { 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F },     /* 32767 */
        { 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0xC0, 0x7F,       /* inf, NaN */
          0x01, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x00, 0x80 },     /* sNaN, -0 */
        { 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x7F },     /* denormal, max */
    };
    uint32_t type, size, n = 0;
    int r, e, bad = 0;
    for (type = 0; type < 16; type++)
        for (size = 0; size < 8; size++) {
            for (e = 0; e < (int)(sizeof edge / sizeof edge[0]) + 2000; e++) {
                uint8_t b[16];
                float o1[4], o2[4];
                int r1, r2;
                if (e < (int)(sizeof edge / sizeof edge[0]))
                    memcpy(b, edge[e], 16);
                else
                    for (r = 0; r < 16; r++)
                        b[r] = (uint8_t)rnd();
                r1 = nv2a_vtx_decode(type, size, b, o1);
                r2 = ref_vtx_decode_fn(type, size, b, o2);
                n++;
                if ((r1 != r2 || memcmp(o1, o2, 16)) && bad++ < 4)
                    fprintf(stderr, "  decode type %u size %u differs\n", type, size);
            }
        }
    CHECK(!bad, "%d nv2a_vtx_decode results differ from the old decode", bad);
    printf("decode: %u cases\n", n);
}

/* ---- the comparison of one draw ------------------------------------- */

static uint64_t s_hash = 1469598103934665603ull;
static uint64_t s_draws, s_verts;

static void hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++)
        s_hash = (s_hash ^ b[i]) * 1099511628211ull;
}

/* Both gathers of one draw; 0 when they differ. */
static int compare_draw(const struct nv2a_pb_draw *d, const int *slot, int nf4,
                        uint32_t lo, uint32_t count, const float (*inl)[4],
                        const uint8_t *mem, const char *what)
{
    size_t n = (size_t)count * (size_t)(nf4 > 0 ? (nf4 < 16 ? nf4 : 16) : 0) * 4u;
    float *a = malloc(n * 4 + 16), *b = malloc(n * 4 + 16);
    int same;
    /* Different fill, so an element the new gather never writes shows. */
    memset(a, 0xA5, n * 4 + 16);
    memset(b, 0x5A, n * 4 + 16);
    ref_vtx_gather(d, slot, nf4, lo, count, inl, mem, a);
    nv2a_vtx_gather(d, slot, nf4, lo, count, inl, mem, b);
    same = !memcmp(a, b, n * 4);
    if (!same && s_fail < 8) {
        size_t i;
        for (i = 0; i < n && f2u(a[i]) == f2u(b[i]); i++)
            ;
        fprintf(stderr, "  %s: nf4 %d lo %u count %u inline %d: float %zu (vertex %zu input %zu)"
                " old %08X new %08X\n", what, nf4, lo, count, d->inline_data, i,
                i / 4 / (size_t)nf4, i / 4 % (size_t)nf4, f2u(a[i]), f2u(b[i]));
    }
    hash_bytes(b, n * 4);
    s_draws++;
    s_verts += count;
    free(a);
    free(b);
    return same;
}

/* ---- 3. synthetic draws --------------------------------------------- */

static void check_synthetic(void)
{
    enum { MEM = 1 << 20, CASES = 40000 };
    static uint8_t mem[MEM];
    static uint32_t payload[4096];
    static const uint32_t types[] = { 0, 1, 2, 3, 4, 5, 6, 7, 2, 2, 2, 0, 1 };
    VertexAttr attr[NV_VERTEX_ATTRS];
    float inl[NV_VERTEX_ATTRS][4];
    int slot[NV_VERTEX_ATTRS];
    struct nv2a_pb_draw d;
    int c, k, bad = 0;
    uint32_t i;

    for (i = 0; i < MEM; i++)
        mem[i] = (uint8_t)rnd();
    for (c = 0; c < CASES; c++) {
        int nf4 = 1 + (int)(rnd() % 17);        /* 17: past the limit */
        uint32_t count = rnd() % 200, lo = rnd() % 64;
        memset(&d, 0, sizeof d);
        d.attr = attr;
        d.inline_data = (c % 3) == 0;
        for (k = 0; k < NV_VERTEX_ATTRS; k++) {
            VertexAttr *a = &attr[k];
            a->type = types[rnd() % (sizeof types / sizeof types[0])];
            a->size = rnd() % 8;
            a->stride = (rnd() % 5) ? 4u * (1u + rnd() % 12) : rnd() % 65;
            if (d.inline_data)
                a->offset = rnd() % 64;
            else
                a->offset = (rnd() % 8) ? 4096u + rnd() % 4096u : 0u;
            for (i = 0; i < 4; i++)
                memcpy(&inl[k][i], &(uint32_t){ rnd() }, 4);
            slot[k] = (int)(rnd() % NV_VERTEX_ATTRS);
        }
        if (d.inline_data) {
            /* A payload of some vertices' worth, cut anywhere: the inputs
             * reaching past its end take their default there. */
            for (i = 0; i < 4096; i++)
                payload[i] = rnd();
            d.inline_buf = payload;
            d.inline_bytes = 4u * (rnd() % 1024u);
            if (count * 64u + 64u > 4096u * 4u - 64u)
                count = 100;
        } else {
            uint32_t maxs = 0;
            for (k = 0; k < NV_VERTEX_ATTRS; k++)
                if (attr[k].stride > maxs)
                    maxs = attr[k].stride;
            while (count && 8192u + (lo + count) * maxs + 16u > MEM)
                count /= 2;
        }
        if (!compare_draw(&d, slot, nf4, lo, count, (const float (*)[4])inl, mem, "synthetic"))
            bad++;
    }
    CHECK(!bad, "%d of %d synthetic draws differ", bad, CASES);
    printf("synthetic: %d draws\n", CASES);
}

/* ---- 3b. guest arrays ------------------------------------------------ */

/* Draws from the title's own vertex arrays in a fake guest region at the
 * guest's own addresses (the 4 GB reservation below), as the race's layouts
 * have them: interleaved FVF streams (position, CMP normal, D3DCOLOR,
 * texture coordinates) and separate ones, every type and size, high and odd
 * offsets, large first vertices, and inputs that are off (offset 0, stride
 * 0, size 0) among them. */
static uint8_t *guest(void);

static void check_array(void)
{
    enum { CASES = 20000, REGION = 4u << 20 };
    static const struct { uint32_t n, type[6], size[6], stride; } fvf[] = {
        { 3, { 2, 6, 2 },          { 3, 1, 2 },          24 },   /* F3 CMP F2 */
        { 3, { 2, 0, 2 },          { 3, 4, 2 },          28 },   /* F3 C4 F2 */
        { 2, { 2, 2 },             { 3, 2 },             20 },   /* F3 F2 */
        { 4, { 2, 6, 0, 2 },       { 3, 1, 4, 2 },       28 },   /* F3 CMP C4 F2 */
        { 3, { 2, 2, 2 },          { 3, 2, 2 },          32 },   /* F3 F2 F2 */
        { 5, { 2, 1, 0, 4, 5 },    { 4, 3, 4, 4, 2 },    40 },   /* F4 S1x3 C4 UB4 S32Kx2 */
    };
    static const uint32_t base[] = { 0x80000000u, 0x80000004u, 0x83FC0000u, 0x00100000u, 0x0001F00Cu };
    VertexAttr attr[NV_VERTEX_ATTRS];
    float inl[NV_VERTEX_ATTRS][4];
    int slot[NV_VERTEX_ATTRS];
    struct nv2a_pb_draw d;
    uint8_t *g = guest();
    int c, k, bad = 0, n = 0;
    uint32_t i;

    for (c = 0; c < CASES; c++) {
        uint32_t f = rnd() % (sizeof fvf / sizeof fvf[0]), b = base[rnd() % 5];
        uint32_t lo = (c & 7) == 0 ? rnd() % 0x10000u : rnd() % 300u;
        uint32_t count = 1 + rnd() % 160, nf4, off = 0;
        int separate = (c % 5) == 0;
        uint32_t region;
        memset(&d, 0, sizeof d);
        memset(attr, 0, sizeof attr);
        d.attr = attr;
        nf4 = fvf[f].n + rnd() % 3;              /* with inputs the draw never set */
        if (nf4 > NV_VERTEX_ATTRS)
            nf4 = NV_VERTEX_ATTRS;
        for (k = 0; k < (int)nf4; k++) {
            VertexAttr *a = &attr[k];
            slot[k] = k;
            for (i = 0; i < 4; i++)
                memcpy(&inl[k][i], &(uint32_t){ rnd() }, 4);
            if ((uint32_t)k >= fvf[f].n)
                continue;                        /* never set: all 0 */
            a->type = fvf[f].type[k];
            a->size = fvf[f].size[k];
            if (separate) {
                /* A stream of its own, packed. */
                a->stride = a->type == 2 ? 4u * a->size : a->type == 0 || a->type == 6 ? 4u
                          : a->type == 4 ? 4u : 2u * a->size;
                a->offset = b + off;
                off += (lo + count) * a->stride + 64u;
            } else {
                a->stride = fvf[f].stride;
                a->offset = b + off;
                off += a->type == 2 ? 4u * a->size : a->type == 0 || a->type == 6 ? 4u
                     : a->type == 4 ? 4u : 2u * a->size;
            }
            switch (rnd() % 24) {                /* an input that is off */
            case 0: a->offset = 0; break;
            case 1: a->stride = 0; break;
            case 2: a->size = 0; break;
            default: break;
            }
        }
        region = separate ? off + 64u : (lo + count) * fvf[f].stride + 64u;
        if (region > REGION)
            continue;
        /* The region's bytes (and a little either side), at the guest's
         * own addresses: what the gather reads. */
        for (i = 0; i < region; i += 4) {
            uint32_t r = rnd();
            memcpy(g + b + i, &r, 4);
        }
        n++;
        if (!compare_draw(&d, slot, (int)nf4, lo, count, (const float (*)[4])inl, g, "array"))
            bad++;
    }
    CHECK(!bad, "%d of %d guest-array draws differ", bad, n);
    CHECK(n > CASES / 2, "only %d guest-array draws fitted the region", n);
    printf("guest arrays: %d draws\n", n);
}

/* ---- 4. recorded draws ---------------------------------------------- */

struct rec_in {
    int32_t  slot;
    VertexAttr a;
    float    dflt[4];
    uint32_t have, va, len;
    const uint8_t *bytes;
};

struct rec_draw {
    uint32_t nf4, lo, count, inline_data, inline_bytes, payload_len;
    const uint8_t *payload;
    struct rec_in in[NV_VERTEX_ATTRS];
};

struct rec {
    uint8_t *buf;
    size_t len, at;
};

static int rd_u32(struct rec *r, uint32_t *v)
{
    if (r->at + 4 > r->len)
        return 0;
    memcpy(v, r->buf + r->at, 4);
    r->at += 4;
    return 1;
}

static int rd_bytes(struct rec *r, uint32_t n, const uint8_t **p)
{
    if (r->at + n > r->len)
        return 0;
    *p = r->buf + r->at;
    r->at += n;
    return 1;
}

static int rec_open(const char *path, struct rec *r)
{
    FILE *f = fopen(path, "rb");
    uint32_t v;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    r->len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    r->buf = malloc(r->len ? r->len : 1);
    r->at = 0;
    if (fread(r->buf, 1, r->len, f) != r->len) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (r->len < 8 || memcmp(r->buf, "VTXR", 4))
        return 0;
    r->at = 4;
    rd_u32(r, &v);
    return v == 1;
}

/* The next draw; 0 at the end, -1 on a malformed file. */
static int rec_next(struct rec *r, struct rec_draw *d)
{
    const uint8_t *tag;
    uint32_t k, v;
    if (r->at == r->len)
        return 0;
    if (!rd_bytes(r, 4, &tag) || memcmp(tag, "DRAW", 4))
        return -1;
    if (!rd_u32(r, &d->nf4) || !rd_u32(r, &d->lo) || !rd_u32(r, &d->count)
            || !rd_u32(r, &d->inline_data) || !rd_u32(r, &d->inline_bytes)
            || d->nf4 > NV_VERTEX_ATTRS)
        return -1;
    for (k = 0; k < d->nf4; k++) {
        struct rec_in *in = &d->in[k];
        const uint8_t *fl;
        if (!rd_u32(r, &v)) return -1;
        in->slot = (int32_t)v;
        if (!rd_u32(r, &in->a.offset) || !rd_u32(r, &in->a.type)
                || !rd_u32(r, &in->a.size) || !rd_u32(r, &in->a.stride)
                || !rd_bytes(r, 16, &fl) || in->slot < 0 || in->slot >= NV_VERTEX_ATTRS)
            return -1;
        memcpy(in->dflt, fl, 16);
        in->have = 0;
    }
    d->payload = NULL;
    d->payload_len = 0;
    if (d->inline_data) {
        if (!rd_u32(r, &d->payload_len) || !rd_bytes(r, (d->payload_len + 3u) & ~3u, &d->payload))
            return -1;
    } else {
        for (k = 0; k < d->nf4; k++) {
            struct rec_in *in = &d->in[k];
            if (!rd_u32(r, &in->have))
                return -1;
            if (in->have && (!rd_u32(r, &in->va) || !rd_u32(r, &in->len)
                             || !rd_bytes(r, in->len, &in->bytes)))
                return -1;
        }
    }
    return 1;
}

/* The guest's address space, reserved and filled on demand: each draw's
 * bytes go back where the recording read them. */
static uint8_t *s_guest;

static uint8_t *guest(void)
{
    if (!s_guest) {
        void *p = mmap(NULL, (size_t)1 << 32 | 1u << 20, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON
#ifdef MAP_NORESERVE
                       | MAP_NORESERVE
#endif
                       , -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "cannot reserve 4 GB for the guest\n");
            exit(1);
        }
        s_guest = p;
    }
    return s_guest;
}

/* The draw's nv2a_pb_draw, slots and defaults, its bytes in place. */
static void rec_setup(const struct rec_draw *r, struct nv2a_pb_draw *d,
                      VertexAttr *attr, int *slot, float (*inl)[4], uint32_t *payload)
{
    uint8_t *g = guest();
    uint32_t k;
    memset(d, 0, sizeof *d);
    memset(attr, 0, sizeof(VertexAttr) * NV_VERTEX_ATTRS);
    memset(inl, 0, sizeof(float) * 4 * NV_VERTEX_ATTRS);
    d->attr = attr;
    d->inline_data = (int)r->inline_data;
    d->inline_bytes = r->inline_bytes;
    for (k = 0; k < r->nf4; k++) {
        const struct rec_in *in = &r->in[k];
        slot[k] = in->slot;
        attr[in->slot] = in->a;
        memcpy(inl[in->slot], in->dflt, 16);
        if (in->have)
            memcpy(g + in->va, in->bytes, in->len);
    }
    if (r->inline_data) {
        memset(payload, 0, NV_MAX_INLINE * 4u);
        memcpy(payload, r->payload, r->payload_len);
        d->inline_buf = payload;
    }
}

static uint32_t *s_payload;

static int check_file(const char *path)
{
    struct rec r;
    struct rec_draw rd;
    struct nv2a_pb_draw d;
    VertexAttr attr[NV_VERTEX_ATTRS];
    float inl[NV_VERTEX_ATTRS][4];
    int slot[NV_VERTEX_ATTRS], st;
    uint64_t draws0 = s_draws, verts0 = s_verts, h0;
    uint32_t bad = 0, inl_draws = 0;

    if (!rec_open(path, &r)) {
        CHECK(0, "%s: not a vtx_record file", path);
        return 0;
    }
    s_hash = 1469598103934665603ull;
    while ((st = rec_next(&r, &rd)) > 0) {
        rec_setup(&rd, &d, attr, slot, inl, s_payload);
        inl_draws += rd.inline_data != 0;
        if (!compare_draw(&d, slot, (int)rd.nf4, rd.lo, rd.count,
                          (const float (*)[4])inl, guest(), path))
            bad++;
    }
    h0 = s_hash;
    CHECK(st == 0, "%s: malformed at byte %zu", path, r.at);
    CHECK(!bad, "%s: %u draws differ", path, bad);
    printf("recorded %s: %llu draws (%u inline), %llu vertices, gathered output fnv1a %016llx\n",
           path, (unsigned long long)(s_draws - draws0), inl_draws,
           (unsigned long long)(s_verts - verts0), (unsigned long long)h0);
    free(r.buf);
    return 1;
}

/* ---- bench and pack ------------------------------------------------- */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int bench(const char *path, int reps)
{
    struct rec r;
    struct rec_draw *rds = NULL;
    size_t n = 0, cap = 0, i, maxout = 0;
    struct nv2a_pb_draw d;
    VertexAttr attr[NV_VERTEX_ATTRS];
    float inl[NV_VERTEX_ATTRS][4], *out;
    int slot[NV_VERTEX_ATTRS], rep, which;
    uint64_t verts = 0;

    if (!rec_open(path, &r))
        return 1;
    for (;;) {
        if (n == cap)
            rds = realloc(rds, (cap = cap ? cap * 2 : 1024) * sizeof *rds);
        if (rec_next(&r, &rds[n]) <= 0)
            break;
        if ((size_t)rds[n].count * rds[n].nf4 * 16u > maxout)
            maxout = (size_t)rds[n].count * rds[n].nf4 * 16u;
        verts += rds[n].count;
        n++;
    }
    out = malloc(maxout + 16);
    /* The guest bytes stay in place between reps; draws that share an
     * address (a dynamic buffer the title rewrote) read the last ones. */
    for (which = 0; which < 2; which++) {
        double best = 1e30;
        for (rep = 0; rep < reps; rep++) {
            double t0, t;
            for (i = 0; i < n; i++)
                rec_setup(&rds[i], &d, attr, slot, inl, s_payload);
            t0 = now_s();
            for (i = 0; i < n; i++) {
                const struct rec_draw *x = &rds[i];
                VertexAttr a2[NV_VERTEX_ATTRS];
                float l2[NV_VERTEX_ATTRS][4];
                int s2[NV_VERTEX_ATTRS];
                uint32_t k;
                memset(&d, 0, sizeof d);
                d.attr = a2;
                d.inline_data = (int)x->inline_data;
                d.inline_bytes = x->inline_bytes;
                d.inline_buf = s_payload;
                for (k = 0; k < x->nf4; k++) {
                    s2[k] = x->in[k].slot;
                    a2[x->in[k].slot] = x->in[k].a;
                    memcpy(l2[x->in[k].slot], x->in[k].dflt, 16);
                }
                if (which == 0)
                    ref_vtx_gather(&d, s2, (int)x->nf4, x->lo, x->count,
                                   (const float (*)[4])l2, guest(), out);
                else
                    nv2a_vtx_gather(&d, s2, (int)x->nf4, x->lo, x->count,
                                    (const float (*)[4])l2, guest(), out);
            }
            t = now_s() - t0;
            if (t < best)
                best = t;
        }
        printf("%s: %zu draws, %llu vertices: best of %d %.3f ms, %.1f ns/draw, %.2f ns/vertex\n",
               which ? "new" : "old", n, (unsigned long long)verts, reps, best * 1e3,
               best * 1e9 / (double)n, best * 1e9 / (double)verts);
    }
    (void)inl;
    free(out);
    free(rds);
    free(r.buf);
    return 0;
}

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

/* n random bytes in place of the recording's. */
static void wrand(FILE *f, uint32_t n)
{
    while (n) {
        uint32_t r = rnd(), c = n < 4 ? n : 4;
        fwrite(&r, 1, c, f);
        n -= c;
    }
}

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    uint32_t n = size < 4 ? size : 4;
    switch (type) {
    case NV2A_VTX_FLOAT: return 4 * n;
    case NV2A_VTX_D3DCOLOR: case NV2A_VTX_CMP: return 4;
    case NV2A_VTX_UBYTE: return n;
    case NV2A_VTX_S1: case NV2A_VTX_S32K: return 2 * n;
    default: return 0;
    }
}

static int pack(const char *in, const char *outp)
{
    enum { KEYS = 4096, KEEP = 3, MAXV = 48 };
    static uint64_t keys[KEYS];
    static uint8_t kept[KEYS];
    struct rec r;
    struct rec_draw rd;
    FILE *f;
    int st, nk = 0;
    uint32_t written = 0, total = 0;

    if (!rec_open(in, &r) || !(f = fopen(outp, "wb")))
        return 1;
    fwrite("VTXR", 4, 1, f);
    w32(f, 1);
    while ((st = rec_next(&r, &rd)) > 0) {
        uint64_t key = 1469598103934665603ull;
        uint32_t k, count = rd.count;
        int j;
        total++;
        key = (key ^ rd.nf4) * 1099511628211ull;
        key = (key ^ rd.inline_data) * 1099511628211ull;
        for (k = 0; k < rd.nf4; k++) {
            const VertexAttr *a = &rd.in[k].a;
            key = (key ^ (a->type | a->size << 4 | a->stride << 8
                          | (uint32_t)(a->offset != 0) << 16)) * 1099511628211ull;
        }
        for (j = 0; j < nk && keys[j] != key; j++)
            ;
        if (j == nk) {
            if (nk == KEYS)
                continue;
            keys[nk++] = key;
        }
        if (kept[j] >= KEEP)
            continue;
        kept[j]++;
        if (!rd.inline_data && count > MAXV)
            count = MAXV;
        fwrite("DRAW", 4, 1, f);
        w32(f, rd.nf4); w32(f, rd.lo); w32(f, count);
        w32(f, rd.inline_data); w32(f, rd.inline_bytes);
        for (k = 0; k < rd.nf4; k++) {
            const struct rec_in *x = &rd.in[k];
            w32(f, (uint32_t)x->slot);
            w32(f, x->a.offset); w32(f, x->a.type); w32(f, x->a.size); w32(f, x->a.stride);
            wrand(f, 16);
        }
        if (rd.inline_data) {
            w32(f, rd.payload_len);
            wrand(f, (rd.payload_len + 3u) & ~3u);
        } else {
            for (k = 0; k < rd.nf4; k++) {
                const struct rec_in *x = &rd.in[k];
                uint32_t len = count ? (count - 1u) * x->a.stride
                                       + elem_bytes(x->a.type, x->a.size) : 0;
                len = (len + 3u) & ~3u;
                if (!x->have || len > x->len) {
                    w32(f, x->have);
                    if (x->have) {
                        w32(f, x->va); w32(f, x->len);
                        wrand(f, x->len);
                    }
                    continue;
                }
                w32(f, 1); w32(f, x->va); w32(f, len);
                wrand(f, len);
            }
        }
        written++;
    }
    fclose(f);
    printf("pack: %u of %u draws, %d kinds\n", written, total, nk);
    return st < 0;
}

int main(int argc, char **argv)
{
    int i;
    s_payload = calloc(NV_MAX_INLINE, 4);
    if (argc > 2 && !strcmp(argv[1], "bench"))
        return bench(argv[2], argc > 3 ? atoi(argv[3]) : 5);
    if (argc > 3 && !strcmp(argv[1], "pack"))
        return pack(argv[2], argv[3]);
    check_tables();
    check_decode();
    check_synthetic();
    check_array();
    if (argc > 1) {
        for (i = 1; i < argc; i++)
            check_file(argv[i]);
    } else {
        /* NV2A_TEST_RECORDINGS=<dir> (the release gate): the full recording
         * there, race.vtxr, outside git like the golden frames. */
        const char *dir = getenv("NV2A_TEST_RECORDINGS");
#ifdef VTX_GATHER_DATA
        check_file(VTX_GATHER_DATA);
#endif
        if (dir && *dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/race.vtxr", dir);
            check_file(path);
        }
    }
    printf("nv2a_vtx_gather: %s (%llu draws, %llu vertices)\n", s_fail ? "FAIL" : "PASS",
           (unsigned long long)s_draws, (unsigned long long)s_verts);
    return s_fail ? 1 : 0;
}

/*
 * nv2a_pb_walk: the pushbuffer walker's block paths against the
 * word-at-a-time path they replace, draw for draw: a run of ARRAY_ELEMENT16
 * or INLINE_ARRAY parameters handed to the executor in one call (BO-282,
 * nv2a_pb_exec_run; RECOMP_DEBUG=pb_block=0 turns it off) and a run of
 * immediate-mode vertex methods (BO-282.12, nv2a_pb_exec_imm;
 * RECOMP_DEBUG=pb_imm=0).
 *
 * Each stream is walked three times, each time in a fresh process (the
 * walker and the executor keep their state in statics): a word at a time,
 * with the index block path only (int029), and with both. A backend of this
 * test hashes every draw the executor hands it -- the primitive, the index
 * list, the inline payload, the stream tag -- and the three lists of hashes
 * must be the same, as must the index counters (largest batch, batches cut
 * at NV_MAX_INDICES, indices past 0xFFFF), the walks that stopped, and a
 * hash of the inline attribute values (SET_VERTEX_DATA*'s, which a draw
 * without an array for an attribute reads) at every draw and at the end.
 *
 * Streams:
 *   split     a batch of ARRAY_ELEMENT16 words cut into two walks at every
 *             word (a run's parameters carried over a walk's end)
 *   overflow  batches past NV_MAX_INDICES, after an odd ARRAY_ELEMENT32 and
 *             after none, cut inside the run that crosses the limit
 *   inline    INLINE_ARRAY batches, one past NV_MAX_INLINE, cut inside
 *   other     an incrementing ARRAY_ELEMENT16 header, subchannel 1, words
 *             outside BEGIN/END, a walk whose end is below its start (the
 *             ring's jump back), stream marks on a header and inside a run,
 *             a run cut by the end of the contiguous window
 *   limit     a walk past the walker's 0x100000-word limit, inside a run;
 *             the next walk's END draws the batch it stopped in
 *   imm       immediate-mode batches: every vertex form (VERTEX3F/4F,
 *             DATA2F/2S/4UB/4S/4F) and attribute, a header that completes a
 *             vertex and goes on to the next attribute, headers that run
 *             out of the vertex ranges into other methods, a vertex split
 *             over two walks at every word, stream marks on a header and
 *             inside a run, subchannel 1, non-incrementing headers, words
 *             outside BEGIN/END, a batch past NV_MAX_INLINE
 *   recorded  walks recorded in a race (RECOMP_DEBUG=pb_record): the files
 *             named on the command line, else <NV2A_TEST_RECORDINGS>/race.pbrec
 *             and, when it is there, race-crash.pbrec (the release gate: the
 *             full recordings, kept outside git beside the golden frames, as
 *             they are the game's data), else data/race.pbrec if there is one
 *
 *   nv2a_pb_walk_test [file.pbrec ...]
 *   nv2a_pb_walk_test bench file.pbrec [reps]   the walks' time, all three paths
 *
 * Exit 0 when both paths agree everywhere.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kernel.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "recomp_env.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_scan(uint32_t start_va, uint32_t end_va);
uint32_t nv2a_pb_scan_stops(void);
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

/* ---- the backend: a hash a draw ------------------------------------- */

static FILE *s_out;
static uint64_t s_draws;
static uint64_t s_inl = 1469598103934665603ull;   /* the inline values at every draw */
static int s_bench;             /* timing: the draws are not hashed */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void t_on_draw(const struct nv2a_pb_draw *d)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t head[5] = { d->prim, d->idx_count, (uint32_t)d->inline_data,
                         d->inline_bytes, nv2a_pb_stream_tag() };
    s_draws++;
    if (s_bench)
        return;
    h = fnv(h, head, sizeof head);
    h = fnv(h, d->idx, (size_t)d->idx_count * 2u);
    if (d->inline_data)
        h = fnv(h, d->inline_buf, d->inline_bytes);
    fwrite(&h, 8, 1, s_out);
    s_inl = fnv(s_inl, nv2a_pb_vsh_state()->inl, sizeof nv2a_pb_vsh_state()->inl);
}

static const struct nv2a_pb_backend s_tb = { .on_draw = t_on_draw };

/* ---- guest memory and the stream builders --------------------------- */

static uint8_t *s_mem;
static uint32_t s_wp;            /* where the builders write next (a VA) */

static void put(uint32_t w)
{
    memcpy(s_mem + s_wp, &w, 4);
    s_wp += 4;
}

/* A method header: count parameters, subchannel, non-incrementing or not. */
static void hdr(uint32_t method, uint32_t count, uint32_t subch, int noninc)
{
    put((noninc ? 0x40000000u : 0u) | count << 18 | subch << 13 | method);
}

static void m1(uint32_t method, uint32_t param)
{
    hdr(method, 1, 0, 0);
    put(param);
}

/* n ARRAY_ELEMENT16 words (two indices each, from v), in headers of at most
 * 2047 parameters. */
static uint32_t ae16(uint32_t n, uint32_t v)
{
    while (n) {
        uint32_t c = n > 2047u ? 2047u : n, i;
        hdr(0x1800, c, 0, 1);
        for (i = 0; i < c; i++, v += 2)
            put((v & 0xFFFFu) | ((v + 1u) & 0xFFFFu) << 16);
        n -= c;
    }
    return v;
}

#define BASE (XBOX_CONTIG_BASE + (24u << 20))

/* The walks of a stream built at BASE, cut at the given points (VAs). */
static void walk_cut(uint32_t start, uint32_t end, const uint32_t *cut, int ncut)
{
    uint32_t at = start;
    int i;
    for (i = 0; i < ncut; i++)
        if (cut[i] > at && cut[i] < end) {
            nv2a_pb_scan(at, cut[i]);
            at = cut[i];
        }
    nv2a_pb_scan(at, end);
}

static void stream_split(void)
{
    uint32_t len, p, start;
    for (p = 0; p < 2 + 40 + 1; p++) {
        s_wp = start = BASE;
        m1(0x17FC, 5);                   /* TRIANGLES */
        ae16(40, p * 7u);
        m1(0x17FC, 0);
        len = s_wp - start;
        {
            uint32_t cut = start + 8u + 4u * p;   /* after BEGIN's two words */
            walk_cut(start, start + len, &cut, 1);
        }
    }
}

static void stream_overflow(void)
{
    int odd, c;
    for (odd = 0; odd < 2; odd++)
        for (c = 0; c < 3; c++) {
            uint32_t start = s_wp = BASE, cut[2];
            m1(0x17FC, 5);
            if (odd)
                m1(0x1808, 7);           /* one ARRAY_ELEMENT32 index */
            /* NV_MAX_INDICES / 2 + 3 words: the limit falls inside a run. */
            ae16(NV_MAX_INDICES / 2u + 3u, 0);
            m1(0x17FC, 0);
            /* A cut before the limit, one at it, one past it. */
            cut[0] = start + 8u + (odd ? 8u : 0u) + 4u * (NV_MAX_INDICES / 2u - 2u + (uint32_t)c)
                   + 4u * ((NV_MAX_INDICES / 2u) / 2047u);
            cut[1] = cut[0] + 4u * 3u;
            walk_cut(start, s_wp, cut, 2);
        }
}

static void stream_inline(void)
{
    static const uint32_t words[] = { 12, 3000, NV_MAX_INLINE + 25 };
    int i;
    for (i = 0; i < 3; i++) {
        uint32_t start = s_wp = BASE, n = words[i], j, cut;
        /* Position: float3 at slot 0, stride 0 (the inline layout is the
         * walker's), the others off. */
        m1(0x1760, 0x2u | 3u << 4);
        for (j = 1; j < 16; j++)
            m1(0x1760 + 4u * j, 0x2u);
        m1(0x17FC, 8);                   /* QUADS */
        cut = s_wp + 4u + 4u * (n / 2u);
        while (n) {
            uint32_t c = n > 2047u ? 2047u : n, k;
            hdr(0x1818, c, 0, 1);
            for (k = 0; k < c; k++)
                put(0x3F800000u + k * 17u);
            n -= c;
        }
        m1(0x17FC, 0);
        walk_cut(start, s_wp, &cut, 1);
    }
}

static void stream_other(void)
{
    uint32_t start, mid, v;
    /* An incrementing header from 0x1800: its next words are other methods. */
    start = s_wp = BASE;
    m1(0x17FC, 5);
    ae16(6, 100);
    hdr(0x1800, 3, 0, 0);
    put(0x00050004u);
    put(0x00070006u);
    put(9u);                              /* 0x1808: ARRAY_ELEMENT32 */
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
    /* Subchannel 1, and words outside BEGIN/END. */
    start = s_wp = BASE;
    hdr(0x1800, 20, 1, 1);
    for (v = 0; v < 20; v++)
        put(v);
    ae16(30, 0);
    hdr(0x1818, 30, 0, 1);
    for (v = 0; v < 30; v++)
        put(v);
    m1(0x17FC, 5);
    ae16(12, 40);
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
    /* A walk that ends below its start: a batch high up, the jump back,
     * another at the bottom. */
    s_wp = BASE;
    m1(0x17FC, 5);
    ae16(25, 500);
    m1(0x17FC, 0);
    mid = s_wp;
    s_wp = BASE + (1u << 20);
    start = s_wp;
    m1(0x17FC, 6);
    ae16(25, 900);
    m1(0x17FC, 0);
    put((BASE & 0x0FFFFFFFu) | 1u);        /* jump to BASE */
    nv2a_pb_scan(start, mid);
    /* Stream marks: one on a header, one on a word inside a run. */
    start = s_wp = BASE;
    m1(0x17FC, 5);
    ae16(10, 0);
    mid = s_wp;
    ae16(30, 20);
    m1(0x17FC, 0);
    m1(0x17FC, 5);
    ae16(30, 200);
    m1(0x17FC, 0);
    nv2a_pb_stream_mark(mid, 3);                    /* the second header */
    nv2a_pb_stream_mark(mid + 4u + 4u * 11u, 4);    /* its 12th parameter */
    nv2a_pb_scan(start, s_wp);
    nv2a_pb_stream_tag_set(0);
    /* A run at the end of the contiguous window: the walk stops there (the
     * next word is outside it), the next walk's END draws what it got. */
    start = s_wp = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - 24u;
    m1(0x17FC, 5);
    hdr(0x1800, 20, 0, 1);
    put(0x00010000u);
    put(0x00030002u);
    put(0x00050004u);
    nv2a_pb_scan(start, start + 4u * 40u);
    start = s_wp = BASE;
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
}

static void stream_limit(void)
{
    /* Batches of 1000 words, past the limit: the walk stops inside one
     * (word 0x100000), the next walk's END draws what it got. */
    uint32_t start = s_wp = BASE, b;
    for (b = 0; b < 1050u; b++) {
        m1(0x17FC, 5);
        ae16(1000, b);
        m1(0x17FC, 0);
    }
    nv2a_pb_scan(start, s_wp);
    start = s_wp;
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
}

/* Immediate-mode methods (BO-282.12). */
#define V3F   0x1500u
#define V4F   0x1518u
#define D2F   0x1880u       /* + attr * 8 */
#define D2S   0x1900u       /* + attr * 4 */
#define D4UB  0x1940u       /* + attr * 4 */
#define D4S   0x1980u       /* + attr * 8 */
#define D4F   0x1A00u       /* + attr * 16 */

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* One vertex: a colour, a texture coordinate, then the position in form
 * k % 7 (the seventh: one DATA4F header for attributes 0 and 1, the vertex
 * complete in the middle of it). */
static void imm_vertex(uint32_t k)
{
    float x = (float)(k % 37) * 3.5f - 40.0f, y = (float)(k % 11) * 7.25f, z = 0.5f + k * 0.01f;
    m1(D4UB + 3u * 4u, 0x80402010u + k * 0x01030507u);
    hdr(D2F + 9u * 8u, 2, 0, 0);
    put(fbits(k * 0.125f));
    put(fbits(1.0f - k * 0.0625f));
    switch (k % 7) {
    case 0: hdr(V4F, 4, 0, 0); put(fbits(x)); put(fbits(y)); put(fbits(z)); put(fbits(1.0f)); break;
    case 1: hdr(V3F, 3, 0, 0); put(fbits(x)); put(fbits(y)); put(fbits(z)); break;
    case 2: hdr(D4F, 4, 0, 0); put(fbits(x)); put(fbits(y)); put(fbits(z)); put(fbits(0.5f)); break;
    case 3: hdr(D2F, 2, 0, 0); put(fbits(x)); put(fbits(y)); break;
    case 4: m1(D2S, ((uint32_t)(int16_t)(-3 * (int)k) & 0xFFFFu) | (k * 9u) << 16); break;
    case 5: hdr(D4S, 2, 0, 0); put((k & 0xFFFFu) | 0xFFF0u << 16); put(7u | 0x8000u << 16); break;
    default:
        hdr(D4F, 8, 0, 0);
        put(fbits(x)); put(fbits(y)); put(fbits(z)); put(fbits(1.0f));
        put(fbits(0.25f)); put(fbits(0.5f)); put(fbits(0.75f)); put(fbits((float)k));
        break;
    }
}

static void stream_imm(void)
{
    uint32_t start, k, p, mid, n0;
    /* Every form, the batch walked whole and then split at every word of
     * its first vertices (a vertex's parameters carried over a walk's end). */
    for (p = 0; p < 1 + 60; p++) {
        start = s_wp = BASE;
        m1(0x17FC, 8);                   /* QUADS */
        for (k = 0; k < 48; k++)
            imm_vertex(k + p);
        m1(0x17FC, 0);
        {
            uint32_t cut = start + 8u + 4u * p;
            walk_cut(start, s_wp, p ? &cut : NULL, p ? 1 : 0);
        }
    }
    /* Headers that run out of the vertex ranges: DATA4F of attribute 15
     * into the texture methods, VERTEX3F into 0x150C.. and VERTEX4F, the
     * last DATA2F into DATA2S. */
    start = s_wp = BASE;
    m1(0x17FC, 5);                       /* TRIANGLES */
    for (k = 0; k < 6; k++) {
        hdr(D4F + 15u * 16u, 8, 0, 0);
        put(fbits(1.0f)); put(fbits(2.0f)); put(fbits(3.0f)); put(fbits(4.0f));
        put(0x00100000u + k * 0x1000u); put(0x0001022Au); put(0x00030303u); put(0x40000000u);
        hdr(V3F, 8, 0, 0);
        put(fbits(k * 1.0f)); put(fbits(2.0f)); put(fbits(3.0f));
        put(fbits(9.0f)); put(fbits(9.0f)); put(fbits(9.0f));
        put(fbits(5.0f)); put(fbits(6.0f));
        hdr(D2F + 15u * 8u, 4, 0, 0);
        put(fbits(0.5f)); put(fbits(0.25f)); put(0x00020001u); put(0x00040003u);
        hdr(V4F, 4, 0, 0); put(fbits(k * 2.0f)); put(fbits(1.0f)); put(fbits(0.0f)); put(fbits(1.0f));
    }
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
    /* Subchannel 1, non-incrementing headers, words outside BEGIN/END. */
    start = s_wp = BASE;
    hdr(V4F, 4, 1, 0);
    put(fbits(1.0f)); put(fbits(2.0f)); put(fbits(3.0f)); put(fbits(4.0f));
    hdr(D4F, 4, 0, 0);                   /* no primitive: nothing is kept */
    put(fbits(7.0f)); put(fbits(8.0f)); put(fbits(9.0f)); put(fbits(1.0f));
    m1(0x17FC, 6);                       /* TRIANGLE_FAN */
    for (k = 0; k < 9; k++) {
        hdr(D4UB, 3, 0, 1);              /* the position's slot three times */
        put(0x00000001u + k); put(0x00010000u + k); put(0x01000000u + k);
        hdr(D4F + 12u, 5, 0, 1);         /* w five times: five vertices */
        put(fbits(1.0f)); put(fbits(0.5f)); put(fbits(0.25f)); put(fbits(2.0f)); put(fbits(1.0f));
        imm_vertex(k * 3u);
    }
    m1(0x17FC, 0);
    nv2a_pb_scan(start, s_wp);
    /* Stream marks: one on an immediate-mode header, one on a word inside
     * a run (a DATA4F header's third), in two batches. */
    start = s_wp = BASE;
    m1(0x17FC, 8);
    for (k = 0; k < 12; k++)
        imm_vertex(k);
    mid = s_wp;
    imm_vertex(2);                       /* D4UB, D2F, D4F: the D4F header at mid + 20 */
    for (k = 0; k < 12; k++)
        imm_vertex(k + 100u);
    m1(0x17FC, 0);
    m1(0x17FC, 8);
    for (k = 0; k < 8; k++)
        imm_vertex(k + 200u);
    m1(0x17FC, 0);
    nv2a_pb_stream_mark(mid + 8u, 5);                  /* the D2F header */
    nv2a_pb_stream_mark(mid + 20u + 4u + 8u, 6);       /* the D4F run's third word */
    nv2a_pb_scan(start, s_wp);
    nv2a_pb_stream_tag_set(0);
    /* A batch past NV_MAX_INLINE vertices (the flush keeps the strip's
     * last two), cut in the middle of a vertex. */
    start = s_wp = BASE;
    m1(0x17FC, 9);                       /* QUAD_STRIP */
    n0 = NV_MAX_INLINE / (16u * 4u) + 77u;
    for (k = 0; k < n0; k++)
        imm_vertex(k);
    m1(0x17FC, 0);
    {
        uint32_t cut = start + (s_wp - start) / 8u * 4u + 4u;   /* a word near the middle */
        walk_cut(start, s_wp, &cut, 1);
    }
}

/* ---- recorded walks -------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*len ? *len : 1);
    if (fread(b, 1, *len, f) != *len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

/* Replays every walk; the number of walks, -1 for a malformed file. With
 * `t` the time in the walker (the copy into guest memory not counted). */
static long replay_file(const char *path, double *t)
{
    size_t len, at = 8;
    uint8_t *b = slurp(path, &len);
    long walks = 0;
    uint32_t v;
    if (!b || len < 8 || memcmp(b, "PBRC", 4) || (memcpy(&v, b + 4, 4), v != 1)) {
        free(b);
        return -1;
    }
    while (at < len) {
        uint32_t w[4], i;
        struct timespec t0, t1;
        if (at + 16 > len || memcmp(b + at, "WALK", 4))
            break;
        memcpy(w, b + at, 16);
        at += 16;
        for (i = 0; i < w[3]; i++) {
            uint32_t r[2];
            if (at + 8 > len)
                break;
            memcpy(r, b + at, 8);
            at += 8;
            if (at + 4ull * r[1] > len)
                break;
            memcpy(s_mem + r[0], b + at, 4ull * r[1]);
            at += 4ull * r[1];
        }
        if (i < w[3])
            break;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        nv2a_pb_scan(w[1], w[2]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t)
            *t += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        walks++;
    }
    free(b);
    return at == len ? walks : -1;
}

/* ---- one pass, in a child process ------------------------------------ */

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

/* The stream `what` a word at a time (mode 0), with the index block path
 * (1, int029) or with both block paths (2); the draws' hashes, then a
 * trailer of counters, to `out`. Runs in a child. */
static const char *const s_mode[3] = { "word at a time", "index block path", "both block paths" };

static int pass(const char *what, int mode, const char *out, int reps)
{
    static uint8_t xbe[0x1000];
    uint32_t tr[8];
    double t = 0;
    int rep;

    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe))
        return 2;
    s_mem = (uint8_t *)xbox_GetMemoryOffset();
    recomp_env_set(RENV_PB_EXEC, "1");
    recomp_env_set(RENV_PB_BLOCK, mode ? "1" : "0");
    recomp_env_set(RENV_PB_IMM, mode == 2 ? "1" : "0");
    nv2a_pb_set_backend(&s_tb);
    s_out = fopen(out, "wb");
    if (!s_out)
        return 2;
    s_bench = reps > 0;
    /* The executor's first method sets it up (its first-call path). */
    nv2a_pb_exec_method(0, 0x0100, 0);   /* NO_OPERATION */
    if (!strcmp(what, "split"))
        stream_split();
    else if (!strcmp(what, "overflow"))
        stream_overflow();
    else if (!strcmp(what, "inline"))
        stream_inline();
    else if (!strcmp(what, "other"))
        stream_other();
    else if (!strcmp(what, "limit"))
        stream_limit();
    else if (!strcmp(what, "imm"))
        stream_imm();
    else
        for (rep = 0; rep < (reps > 0 ? reps : 1); rep++)
            if (replay_file(what, &t) < 0)
                return 3;
    nv2a_pb_exec_idx_stats(&tr[0], &tr[1], &tr[2]);
    tr[3] = nv2a_pb_scan_stops();
    tr[4] = (uint32_t)s_draws;
    s_inl = fnv(s_inl, nv2a_pb_vsh_state()->inl, sizeof nv2a_pb_vsh_state()->inl);
    tr[5] = (uint32_t)s_inl;
    tr[6] = (uint32_t)(s_inl >> 32);
    tr[7] = 0xFFFFFFFFu;
    fwrite(tr, 4, 8, s_out);
    fclose(s_out);
    if (reps > 0)
        printf("  %s, %s: %d reps, %.3f ms in the walker (%.3f a pass)\n",
               what, s_mode[mode], reps, t * 1e3, t * 1e3 / reps);
    return 0;
}

static int run_pass(const char *self, const char *what, int mode, const char *out, int reps)
{
    pid_t p = fork();
    int st;
    (void)self;
    if (p == 0) {
        fflush(stdout);
        _exit(pass(what, mode, out, reps));
    }
    if (p < 0 || waitpid(p, &st, 0) != p)
        return 99;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 98;
}

/* The three passes of one stream; 1 when they agree. */
static int compare(const char *self, const char *what)
{
    char f[3][64];
    uint8_t *b[3];
    size_t l[3], i;
    int r[3], m, same = 1, ok = 1;
    for (m = 0; m < 3; m++) {
        snprintf(f[m], sizeof f[m], "/tmp/nv2a_pb_walk.%d.%d", (int)getpid(), m);
        r[m] = run_pass(self, what, m, f[m], 0);
        l[m] = 0;
        b[m] = slurp(f[m], &l[m]);
        unlink(f[m]);
        if (r[m] || !b[m] || l[m] < 32)
            ok = 0;
    }
    if (!ok) {
        printf("FAIL %s: a pass failed (exit %d, %d, %d)\n", what, r[0], r[1], r[2]);
        for (m = 0; m < 3; m++)
            free(b[m]);
        return 0;
    }
    for (m = 1; m < 3; m++)
        if (l[m] != l[0] || memcmp(b[m], b[0], l[0]))
            same = 0;
    {
        uint32_t tr[8];
        uint64_t dig = fnv(1469598103934665603ull, b[2], l[2] - 32);
        memcpy(tr, b[2] + l[2] - 32, 32);
        printf("%s %s: %u draws, largest batch %u, cut %u, past 0xFFFF %u, walks stopped %u,"
               " draws fnv1a %016llx, inline values %08x%08x\n", same ? "ok  " : "FAIL", what,
               tr[4], tr[0], tr[1], tr[2], tr[3], (unsigned long long)dig, tr[6], tr[5]);
    }
    if (!same)
        for (m = 1; m < 3; m++) {
            for (i = 0; i < l[0] && i < l[m] && b[0][i] == b[m][i]; i++)
                ;
            if (i < l[0] || i < l[m])
                printf("     %s vs %s: first difference at byte %zu (draw %zu) of %zu / %zu\n",
                       s_mode[0], s_mode[m], i, i / 8, l[0], l[m]);
        }
    for (m = 0; m < 3; m++)
        free(b[m]);
    return same;
}

int main(int argc, char **argv)
{
    static const char *synth[] = { "split", "overflow", "inline", "other", "limit", "imm" };
    int i, fails = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 2 && !strcmp(argv[1], "bench")) {
        int reps = argc > 3 ? atoi(argv[3]) : 5;
        char f[64];
        snprintf(f, sizeof f, "/tmp/nv2a_pb_walk.%d.bench", (int)getpid());
        for (i = 0; i < 3; i++)
            if (run_pass(argv[0], argv[2], i, f, reps))
                return 1;
        unlink(f);
        return 0;
    }
    for (i = 0; i < (int)(sizeof synth / sizeof synth[0]); i++)
        fails += !compare(argv[0], synth[i]);
    if (argc > 1)
        for (i = 1; i < argc; i++)
            fails += !compare(argv[0], argv[i]);
    else {
        /* NV2A_TEST_RECORDINGS=<dir> (the release gate): <dir>/race.pbrec,
         * the full recording kept outside git beside the golden frames; it
         * must be there when the variable is set. */
        const char *dir = getenv("NV2A_TEST_RECORDINGS");
        if (dir && *dir) {
            static char path[1024];
            snprintf(path, sizeof path, "%s/race.pbrec", dir);
            fails += !compare(argv[0], path);
            /* BO-282.12: the crash camera's spark shower, 4.1 M immediate-
             * mode headers; replayed when the set has it. */
            snprintf(path, sizeof path, "%s/race-crash.pbrec", dir);
            if (access(path, R_OK) == 0)
                fails += !compare(argv[0], path);
            else
                printf("skip %s: not in the recording set\n", path);
        }
#ifdef PB_WALK_DATA
        else if (access(PB_WALK_DATA, R_OK) == 0)
            fails += !compare(argv[0], PB_WALK_DATA);
        else
            printf("skip recorded: no NV2A_TEST_RECORDINGS and no %s (a recording is the"
                   " game's data and stays out of git)\n", PB_WALK_DATA);
#endif
    }
    printf("nv2a_pb_walk: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

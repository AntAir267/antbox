/**
 * nv2a_texscale.c - a title's 2D art made larger before it is uploaded.
 * See nv2a_texscale.h.
 *
 * The Super-xBR scaler here is written from Hyllian's published algorithm,
 * the three passes of his Super-xBR shaders (super-xbr-pass0/1/2, the
 * default "normal" mode, anti-ringing on), under their licence:
 *
 *   Super-xBR
 *   Copyright (c) 2015 Hyllian - sergiogdb@gmail.com
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * What is ours: the work on premultiplied colour with alpha as a fourth
 * channel and part of the edge measure, the edge margin, the 4x by two 2x
 * steps, the bicubic, and everything else in the file.
 *
 * The same texels give the same bytes on every machine: single-precision
 * arithmetic without contraction (no fused multiply-add: the pragma below and
 * -ffp-contract=off on this file), no library function but floorf and
 * fabsf. The disk cache's files and the test's checksums rely on it.
 */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
#include "nv2a_texscale.h"
#include "recomp_env.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── float pictures ──────────────────────────────────────────────────── */

/* Five floats a texel: premultiplied R G B, A, and the edge measure. */
#define CH 5
typedef struct {
    int    w, h;
    float *p;
} Pic;

static int pic_new(Pic *o, int w, int h)
{
    o->w = w;
    o->h = h;
    o->p = (float *)malloc((size_t)w * h * CH * sizeof(float));
    return o->p != NULL;
}

/* Past an edge: the edge texel. */
static inline const float *px(const Pic *s, int x, int y)
{
    if (x < 0) x = 0; else if (x >= s->w) x = s->w - 1;
    if (y < 0) y = 0; else if (y >= s->h) y = s->h - 1;
    return s->p + ((size_t)y * s->w + x) * CH;
}

/* The edge measure: luminance of the premultiplied colour (Rec. 709, as the
 * shaders) plus half the alpha, so an outline shows as an edge whatever its
 * colour, and an opaque picture is measured by its luminance alone. */
static inline void measure(float *t)
{
    t[4] = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2] + 0.5f * t[3];
}

static int pic_from_argb(Pic *o, const uint32_t *src, int w, int h)
{
    size_t i, n = (size_t)w * h;
    if (!pic_new(o, w, h))
        return 0;
    for (i = 0; i < n; i++) {
        uint32_t c = src[i];
        float a = (float)(c >> 24) * (1.0f / 255.0f);
        float *t = o->p + i * CH;
        t[0] = (float)((c >> 16) & 0xFFu) * (1.0f / 255.0f) * a;
        t[1] = (float)((c >> 8) & 0xFFu) * (1.0f / 255.0f) * a;
        t[2] = (float)(c & 0xFFu) * (1.0f / 255.0f) * a;
        t[3] = a;
        measure(t);
    }
    return 1;
}

static inline uint32_t to8(float v)
{
    int i = (int)(v * 255.0f + 0.5f);
    return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

/* Back to straight alpha. A texel with no alpha gets colour 0 here and its
 * colour from its neighbours in bleed(). */
static void pic_to_argb(const Pic *s, uint32_t *dst)
{
    size_t i, n = (size_t)s->w * s->h;
    for (i = 0; i < n; i++) {
        const float *t = s->p + i * CH;
        uint32_t a = to8(t[3]);
        if (!a) {
            dst[i] = 0;
        } else {
            float inv = 1.0f / (t[3] < 1.0f / 255.0f ? 1.0f / 255.0f : t[3]);
            dst[i] = a << 24 | to8(t[0] * inv) << 16 | to8(t[1] * inv) << 8 | to8(t[2] * inv);
        }
    }
}

/* Fully transparent texels next to visible ones take the mean colour of the
 * visible ones (alpha stays 0); the others keep the colour the source had at
 * that place. A bilinear sample across an outline then mixes the outline's
 * own colour with itself. */
static void bleed(uint32_t *dst, int w, int h, const uint32_t *src, int sw, int sh,
                  unsigned factor)
{
    int x, y;
    (void)sh;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t *o = &dst[(size_t)y * w + x];
            unsigned r = 0, g = 0, b = 0, n = 0;
            int dx, dy;
            if (*o >> 24)
                continue;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    uint32_t c;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                        continue;
                    c = dst[(size_t)yy * w + xx];
                    if (!(c >> 24))
                        continue;
                    r += (c >> 16) & 0xFFu;
                    g += (c >> 8) & 0xFFu;
                    b += c & 0xFFu;
                    n++;
                }
            }
            if (n)
                *o = ((r + n / 2) / n) << 16 | ((g + n / 2) / n) << 8 | ((b + n / 2) / n);
            else
                *o = src[(size_t)(y / (int)factor) * sw + x / (int)factor] & 0x00FFFFFFu;
        }
    }
}

/* ── rows on a few threads ───────────────────────────────────────────── */

/* A pass computes each texel from texels no pass is writing, so its rows can
 * be done in any order and on any thread with the same result. Up to
 * PAR_THREADS threads for a pass of some size (one of them the caller's);
 * a toolchain without pthreads, or a thread that does not start, does the
 * rows on the caller's. */
typedef void (*row_fn)(void *ctx, int y0, int y1);

#if !defined(_MSC_VER)
#include <pthread.h>
#define PAR_THREADS 4
#define PAR_MIN_TEXELS (64 * 1024)

typedef struct {
    row_fn fn;
    void  *ctx;
    int    y0, y1;
} ParJob;

static void *par_main(void *arg)
{
    ParJob *j = (ParJob *)arg;
    j->fn(j->ctx, j->y0, j->y1);
    return NULL;
}

static void par_rows(row_fn fn, void *ctx, int rows, int width)
{
    pthread_t th[PAR_THREADS];
    ParJob job[PAR_THREADS];
    int started[PAR_THREADS] = { 0 }, n = PAR_THREADS, i;

    if ((long)rows * width < PAR_MIN_TEXELS || rows < n * 4) {
        fn(ctx, 0, rows);
        return;
    }
    for (i = 0; i < n; i++) {
        job[i].fn = fn;
        job[i].ctx = ctx;
        job[i].y0 = (int)((long)rows * i / n);
        job[i].y1 = (int)((long)rows * (i + 1) / n);
    }
    for (i = 1; i < n; i++)
        started[i] = pthread_create(&th[i], NULL, par_main, &job[i]) == 0;
    fn(ctx, job[0].y0, job[0].y1);
    for (i = 1; i < n; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        else
            fn(ctx, job[i].y0, job[i].y1);
    }
}
#else
static void par_rows(row_fn fn, void *ctx, int rows, int width)
{
    (void)width;
    fn(ctx, 0, rows);
}
#endif

/* ── Super-xBR ───────────────────────────────────────────────────────── */

typedef struct {
    float wp[6];
    float w1, w2;       /* the two-tap filters' weights */
} XbrPass;

/* XBR_WEIGHT 1.0. Pass 0 and pass 2 share the filter weights; pass 1 swaps
 * them. */
#define XW_A (1.29633f / 10.0f)
#define XW_B (1.75068f / 10.0f)
static const XbrPass k_pass0 = { { 2.0f, 1.0f, -1.0f, 4.0f, -1.0f, 1.0f }, XW_A, XW_B / 2.0f };
static const XbrPass k_pass1 = { { 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, XW_B, XW_A / 2.0f };
static const XbrPass k_pass2 = { { 1.0f, 0.0f, 2.0f, 3.0f, -2.0f, 1.0f }, XW_A, XW_B / 2.0f };
#define XBR_EDGE_STR 0.6f

static inline float df(float a, float b)
{
    return fabsf(a - b);
}

static inline float d_wd(const float *wp, float b0, float b1, float c0, float c1,
                         float c2, float d0, float d1, float d2, float d3, float e1,
                         float e2, float e3, float f2, float f3)
{
    return wp[0] * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3))
         + wp[1] * (df(d2, d3) + df(d0, d1)) + wp[2] * (df(d1, d3) + df(d0, d2))
         + wp[3] * df(d1, d2) + wp[4] * (df(c0, c2) + df(e1, e3))
         + wp[5] * (df(b0, b1) + df(f2, f3));
}

static inline float hv_wd(const float *wp, float i1, float i2, float i3, float i4,
                          float e1, float e2, float e3, float e4)
{
    return wp[3] * (df(i1, i2) + df(i3, i4))
         + wp[0] * (df(i1, e1) + df(i2, e2) + df(i3, e3) + df(i4, e4))
         + wp[2] * (df(i1, e2) + df(i3, e4) + df(e1, i2) + df(e3, i4));
}

/* The sixteen samples, named as the shaders name them:
 *
 *     P0 B  C  P1
 *     D  E  F  F4
 *     G  H  I  I4
 *     P2 H5 I5 P3
 *
 * and the texel between E, F, H and I that they give. */
enum { P0, B_, C_, P1, D_, E_, F_, F4, G_, H_, I_, I4, P2, H5, I5, P3 };

static void xbr_kernel(const XbrPass *k, const float *const s[16], float *out)
{
    /* Sixteen equal texels give that texel (every weight set sums to 1, and
     * the clamp holds it): the empty parts of a sheet cost a compare each. */
    {
        int i = 1;
        while (i < 16 && !memcmp(s[i], s[0], 4 * sizeof(float)))
            i++;
        if (i == 16) {
            memcpy(out, s[0], CH * sizeof(float));
            return;
        }
    }
#define M(i) (s[i][4])
    float d_edge = d_wd(k->wp, M(D_), M(B_), M(G_), M(E_), M(C_), M(P2), M(H_), M(F_),
                        M(P1), M(H5), M(I_), M(F4), M(I5), M(I4))
                 - d_wd(k->wp, M(C_), M(F4), M(B_), M(F_), M(I4), M(P0), M(E_), M(I_),
                        M(P3), M(D_), M(H_), M(I5), M(G_), M(H5));
    float hv_edge = hv_wd(k->wp, M(F_), M(I_), M(E_), M(H_), M(C_), M(I5), M(B_), M(H5))
                  - hv_wd(k->wp, M(E_), M(F_), M(H_), M(I_), M(D_), M(F4), M(G_), M(I4));
#undef M
    /* smoothstep(0, XBR_EDGE_STR, |d_edge|) */
    float t = fabsf(d_edge) / (XBR_EDGE_STR + 0.000001f);
    float strength, w1a = -k->w1, w1b = k->w1 + 0.5f, w2a = -k->w2, w2b = k->w2 + 0.25f;
    int c;

    if (t > 1.0f)
        t = 1.0f;
    strength = t * t * (3.0f - 2.0f * t);
    for (c = 0; c < 4; c++) {
        float diag = d_edge >= 0.0f
            ? w1a * (s[P0][c] + s[P3][c]) + w1b * (s[E_][c] + s[I_][c])
            : w1a * (s[P2][c] + s[P1][c]) + w1b * (s[H_][c] + s[F_][c]);
        float hv = hv_edge >= 0.0f
            ? w2a * (s[C_][c] + s[B_][c] + s[I5][c] + s[H5][c])
              + w2b * (s[F_][c] + s[E_][c] + s[I_][c] + s[H_][c])
            : w2a * (s[D_][c] + s[G_][c] + s[F4][c] + s[I4][c])
              + w2b * (s[E_][c] + s[H_][c] + s[F_][c] + s[I_][c]);
        float v = diag + (hv - diag) * (1.0f - strength);
        /* Anti-ringing (XBR_ANTI_RINGING 1): within the four nearest. */
        float lo = s[E_][c], hi = s[E_][c];
        if (s[F_][c] < lo) lo = s[F_][c];
        if (s[F_][c] > hi) hi = s[F_][c];
        if (s[H_][c] < lo) lo = s[H_][c];
        if (s[H_][c] > hi) hi = s[H_][c];
        if (s[I_][c] < lo) lo = s[I_][c];
        if (s[I_][c] > hi) hi = s[I_][c];
        out[c] = v < lo ? lo : v > hi ? hi : v;
    }
    measure(out);
}

/* One 2x step: src (w x h) to dst (2w x 2h), allocated here.
 *
 * Pass 0 puts the source at the even places of a 2w x 2h grid and fills the
 * odd-odd ones from the 4x4 source texels around each. Pass 1 fills the two
 * remaining kinds from the diamond of texels around each. That picture has
 * the source texel x at 2x, a quarter of a source texel off the place a
 * texture coordinate expects; pass 2 takes the value between each four
 * texels, which moves it back, so the result lies exactly over the source.
 *
 * The edge: the source is first given a margin of XBR_PAD texels that repeat
 * its edge texels, the passes run over all of it, and the margin is cut off.
 * What a pass reads past the padded grid (clamped, in pass 1 onto a place of
 * the other kind) reaches five texels of the doubled grid at most, inside the
 * 2 * XBR_PAD that are cut. */
#define XBR_PAD 4

typedef struct {
    const Pic *in;      /* pass 0: the padded source */
    Pic       *g;       /* the doubled grid */
    Pic       *out;     /* pass 2's result */
} XbrCtx;

static void xbr_rows0(void *ctx, int y0, int y1)
{
    static const signed char o[16][2] = {
        { -1, -1 }, { 0, -1 }, { 1, -1 }, { 2, -1 },
        { -1, 0 },  { 0, 0 },  { 1, 0 },  { 2, 0 },
        { -1, 1 },  { 0, 1 },  { 1, 1 },  { 2, 1 },
        { -1, 2 },  { 0, 2 },  { 1, 2 },  { 2, 2 }
    };
    const XbrCtx *c = (const XbrCtx *)ctx;
    const float *s[16];
    int W = c->g->w, x, y, i;

    for (y = y0; y < y1; y++) {
        for (x = 0; x < c->in->w; x++) {
            memcpy(c->g->p + ((size_t)(2 * y) * W + 2 * x) * CH, px(c->in, x, y),
                   CH * sizeof(float));
            for (i = 0; i < 16; i++)
                s[i] = px(c->in, x + o[i][0], y + o[i][1]);
            xbr_kernel(&k_pass0, s, c->g->p + ((size_t)(2 * y + 1) * W + 2 * x + 1) * CH);
        }
    }
}

static void xbr_rows1(void *ctx, int y0, int y1)
{
    const XbrCtx *c = (const XbrCtx *)ctx;
    const Pic *g = c->g;
    const float *s[16];
    int W = g->w, x, y;

    for (y = y0; y < y1; y++) {
        for (x = (y & 1) ^ 1; x < W; x += 2) {
            /* g1 along x for an odd column, along y for an odd row. */
            int ax = x & 1, ay = !ax, bx = ay, by = ax;
#define AT(a, b) px(g, x + (a) * ax + (b) * bx, y + (a) * ay + (b) * by)
            s[P0] = AT(-3, 0);  s[P1] = AT(0, -3);  s[P2] = AT(0, 3);   s[P3] = AT(3, 0);
            s[B_] = AT(-2, -1); s[C_] = AT(-1, -2); s[D_] = AT(-2, 1);  s[E_] = AT(-1, 0);
            s[F_] = AT(0, -1);  s[G_] = AT(-1, 2);  s[H_] = AT(0, 1);   s[I_] = AT(1, 0);
            s[F4] = AT(1, -2);  s[I4] = AT(2, -1);  s[H5] = AT(1, 2);   s[I5] = AT(2, 1);
#undef AT
            xbr_kernel(&k_pass1, s, g->p + ((size_t)y * W + x) * CH);
        }
    }
}

static void xbr_rows2(void *ctx, int y0, int y1)
{
    static const signed char o[16][2] = {
        { -2, -2 }, { -1, -2 }, { 0, -2 }, { 1, -2 },
        { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 },
        { -2, 0 },  { -1, 0 },  { 0, 0 },  { 1, 0 },
        { -2, 1 },  { -1, 1 },  { 0, 1 },  { 1, 1 }
    };
    const XbrCtx *c = (const XbrCtx *)ctx;
    const float *s[16];
    int W = c->g->w, x, y, i;

    for (y = y0; y < y1; y++) {
        for (x = 0; x < W; x++) {
            for (i = 0; i < 16; i++)
                s[i] = px(c->g, x + o[i][0], y + o[i][1]);
            xbr_kernel(&k_pass2, s, c->out->p + ((size_t)y * W + x) * CH);
        }
    }
}

static int xbr_2x(const Pic *src, Pic *dst)
{
    Pic in, g, out;
    XbrCtx c;
    int w = src->w + 2 * XBR_PAD, h = src->h + 2 * XBR_PAD;
    int W = w * 2, H = h * 2, x, y;

    if (!pic_new(&in, w, h))
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            memcpy(in.p + ((size_t)y * w + x) * CH, px(src, x - XBR_PAD, y - XBR_PAD),
                   CH * sizeof(float));
    g.w = W;
    g.h = H;
    g.p = (float *)calloc((size_t)W * H * CH, sizeof(float));
    if (!g.p || !pic_new(&out, W, H)) {
        free(in.p);
        free(g.p);
        return 0;
    }
    c.in = &in;
    c.g = &g;
    c.out = &out;
    /* Pass 0 writes the even-even and odd-odd places from the source alone;
     * pass 1 reads those and writes the others; pass 2 reads the grid and
     * writes another picture. */
    par_rows(xbr_rows0, &c, h, w);
    free(in.p);
    par_rows(xbr_rows1, &c, H, W / 2);
    par_rows(xbr_rows2, &c, H, W);
    free(g.p);
    if (!pic_new(dst, src->w * 2, src->h * 2)) {
        free(out.p);
        return 0;
    }
    for (y = 0; y < dst->h; y++)
        memcpy(dst->p + (size_t)y * dst->w * CH,
               out.p + ((size_t)(y + 2 * XBR_PAD) * W + 2 * XBR_PAD) * CH,
               (size_t)dst->w * CH * sizeof(float));
    free(out.p);
    return 1;
}

/* ── bicubic (Catmull-Rom), for comparison ───────────────────────────── */

static void cr_weights(float t, float w[4])
{
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

static int bicubic(const Pic *src, Pic *dst, int f)
{
    int W = src->w * f, H = src->h * f, x, y, i, j, c;

    if (!pic_new(dst, W, H))
        return 0;
    for (y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f) / (float)f - 0.5f, wy[4];
        int iy = (int)floorf(fy);
        cr_weights(fy - (float)iy, wy);
        for (x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f) / (float)f - 0.5f, wx[4];
            int ix = (int)floorf(fx);
            float *o = dst->p + ((size_t)y * W + x) * CH;
            float acc[4] = { 0, 0, 0, 0 }, lo[4], hi[4];
            cr_weights(fx - (float)ix, wx);
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    const float *t = px(src, ix - 1 + i, iy - 1 + j);
                    float wgt = wx[i] * wy[j];
                    for (c = 0; c < 4; c++) {
                        acc[c] += t[c] * wgt;
                        /* the four nearest bound the result (no ringing) */
                        if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                            if ((i == 1 && j == 1) || t[c] < lo[c]) lo[c] = t[c];
                            if ((i == 1 && j == 1) || t[c] > hi[c]) hi[c] = t[c];
                        }
                    }
                }
            }
            for (c = 0; c < 4; c++)
                o[c] = acc[c] < lo[c] ? lo[c] : acc[c] > hi[c] ? hi[c] : acc[c];
            measure(o);
        }
    }
    return 1;
}

/* ── the entry ───────────────────────────────────────────────────────── */

static const char *const k_algo_names[NV2A_TEXSCALE_ALGOS] = { "super-xbr", "bicubic" };

const char *nv2a_texscale_algo_name(enum nv2a_texscale_algo algo)
{
    return (unsigned)algo < NV2A_TEXSCALE_ALGOS ? k_algo_names[algo] : k_algo_names[0];
}

enum nv2a_texscale_algo nv2a_texscale_algo_named(const char *name)
{
    int i;
    for (i = 0; name && i < NV2A_TEXSCALE_ALGOS; i++)
        if (!strcmp(name, k_algo_names[i]))
            return (enum nv2a_texscale_algo)i;
    return NV2A_TEXSCALE_SUPERXBR;
}

int nv2a_texscale(const uint32_t *src, uint32_t w, uint32_t h, unsigned factor,
                  enum nv2a_texscale_algo algo, uint32_t *dst)
{
    Pic a, b, c;

    if (!src || !dst || !w || !h || (factor != 2 && factor != 4) || w > 8192 || h > 8192
            || (unsigned)algo >= NV2A_TEXSCALE_ALGOS)
        return 0;
    if (!pic_from_argb(&a, src, (int)w, (int)h))
        return 0;
    if (algo == NV2A_TEXSCALE_BICUBIC) {
        if (!bicubic(&a, &b, (int)factor)) {
            free(a.p);
            return 0;
        }
    } else {
        if (!xbr_2x(&a, &b)) {
            free(a.p);
            return 0;
        }
        if (factor == 4) {
            if (!xbr_2x(&b, &c)) {
                free(a.p);
                free(b.p);
                return 0;
            }
            free(b.p);
            b = c;
        }
    }
    free(a.p);
    {
        /* Premultiplied colour never exceeds its alpha. */
        size_t i, n = (size_t)b.w * b.h;
        for (i = 0; i < n; i++) {
            float *t = b.p + i * CH;
            int k;
            if (t[3] < 0.0f) t[3] = 0.0f; else if (t[3] > 1.0f) t[3] = 1.0f;
            for (k = 0; k < 3; k++) {
                if (t[k] < 0.0f) t[k] = 0.0f; else if (t[k] > t[3]) t[k] = t[3];
            }
        }
    }
    pic_to_argb(&b, dst);
    free(b.p);
    bleed(dst, (int)(w * factor), (int)(h * factor), src, (int)w, (int)h, factor);
    return 1;
}

void nv2a_texscale_mips(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t *src = texels, l;
    for (l = 1; l < levels; l++) {
        uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, x, y;
        uint32_t *dst = src + (size_t)w * h;
        for (y = 0; y < dh; y++) {
            const uint32_t *r0 = src + (size_t)(h > 1 ? 2 * y : 0) * w;
            const uint32_t *r1 = src + (size_t)(h > 1 ? 2 * y + 1 : 0) * w;
            for (x = 0; x < dw; x++) {
                uint32_t x0 = w > 1 ? 2 * x : 0, x1 = w > 1 ? 2 * x + 1 : 0;
                uint32_t t[4], a = 0, r = 0, g = 0, b = 0, cr = 0, cg = 0, cb = 0;
                int i;
                t[0] = r0[x0]; t[1] = r0[x1]; t[2] = r1[x0]; t[3] = r1[x1];
                for (i = 0; i < 4; i++) {
                    uint32_t ta = t[i] >> 24;
                    a += ta;
                    r += ((t[i] >> 16) & 0xFFu) * ta;
                    g += ((t[i] >> 8) & 0xFFu) * ta;
                    b += (t[i] & 0xFFu) * ta;
                    cr += (t[i] >> 16) & 0xFFu;
                    cg += (t[i] >> 8) & 0xFFu;
                    cb += t[i] & 0xFFu;
                }
                if (a)
                    dst[(size_t)y * dw + x] = ((a + 2) / 4) << 24 | ((r + a / 2) / a) << 16
                                            | ((g + a / 2) / a) << 8 | ((b + a / 2) / a);
                else
                    dst[(size_t)y * dw + x] = ((cr + 2) / 4) << 16 | ((cg + 2) / 4) << 8
                                            | ((cb + 2) / 4);
            }
        }
        src = dst;
        w = dw;
        h = dh;
    }
}

/* ── the setting ─────────────────────────────────────────────────────── */

#define UNREAD 0xFFu
static _Atomic unsigned s_factor = UNREAD;   /* until the environment is read */
static _Atomic int      s_algo = -1;
static _Atomic uint32_t s_gen = 1;
static _Atomic uint64_t s_textures, s_texels, s_usec;

static void gen_next(void)
{
    if (atomic_fetch_add_explicit(&s_gen, 1, memory_order_relaxed) == 0xFFFFFFFFu)
        atomic_store_explicit(&s_gen, 1, memory_order_relaxed);
}

int nv2a_texscale_held(void)
{
    const char *e = recomp_env(RENV_UI_SCALE_HOLD);
    if (e && *e)
        return *e != '0';
    return 0;
}

static void held_say(unsigned factor)
{
    static _Atomic int said;
    if (factor && !atomic_exchange_explicit(&said, 1, memory_order_relaxed))
        fprintf(stderr, "[UISCALE] 2D art upscaling (%ux asked for) is held off on this"
                " platform; RECOMP_DEBUG=ui_scale_hold=0 lifts the hold\n", factor);
}

void nv2a_texscale_set_factor(unsigned factor)
{
    if (factor != 2 && factor != 4)
        factor = 0;
    if (nv2a_texscale_held()) {
        held_say(factor);
        factor = 0;
    }
    if (atomic_exchange_explicit(&s_factor, factor, memory_order_relaxed) != factor)
        gen_next();
}

/* RECOMP_GFX_UI_SCALE: off (default), 2x, 4x ("2" and "4" too). */
unsigned nv2a_texscale_factor(void)
{
    unsigned f = atomic_load_explicit(&s_factor, memory_order_relaxed);
    if (f == UNREAD) {
        const char *e = recomp_env(RENV_GFX_UI_SCALE);
        unsigned expect = UNREAD;
        f = e && e[0] == '2' ? 2u : e && e[0] == '4' ? 4u : 0u;
        if (nv2a_texscale_held()) {
            held_say(f);
            f = 0;
        }
        /* A set_factor() that came first stays. */
        if (!atomic_compare_exchange_strong_explicit(&s_factor, &expect, f,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed))
            f = expect;
    }
    return f;
}

void nv2a_texscale_set_algo(enum nv2a_texscale_algo algo)
{
    if ((unsigned)algo >= NV2A_TEXSCALE_ALGOS)
        algo = NV2A_TEXSCALE_SUPERXBR;
    if (atomic_exchange_explicit(&s_algo, (int)algo, memory_order_relaxed) != (int)algo)
        gen_next();
}

/* RECOMP_DEBUG=ui_scale_algo=bicubic: the comparison scaler. */
enum nv2a_texscale_algo nv2a_texscale_algo(void)
{
    int a = atomic_load_explicit(&s_algo, memory_order_relaxed);
    if (a < 0) {
        const char *e = recomp_env(RENV_UI_SCALE_ALGO);
        int expect = -1;
        a = (int)nv2a_texscale_algo_named(e);
        if (!atomic_compare_exchange_strong_explicit(&s_algo, &expect, a,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed))
            a = expect;
    }
    return (enum nv2a_texscale_algo)a;
}

uint32_t nv2a_texscale_generation(void)
{
    return atomic_load_explicit(&s_gen, memory_order_relaxed);
}

/* ── the rule ────────────────────────────────────────────────────────── */

int nv2a_texscale_draw_is_2d(const struct nv2a_texscale_draw *d)
{
    if (d->depth_test || d->depth_write)
        return 0;
    if (!d->frame_w || !d->frame_h)
        return 0;
    /* The frame's own surface: as wide as what the title presents and about
     * as tall (the presented extent follows the last clip, so the two differ
     * by a clip's crop: Burnout 3 presents 640x464 or 640x480 of one 640x480
     * surface). An effect target is a half or less each way. */
    return d->target_w >= d->frame_w && d->target_h * 8u >= d->frame_h * 7u;
}

int nv2a_texscale_tex_ok(int size_from_format, uint32_t w, uint32_t h, uint32_t levels)
{
    if (!size_from_format || levels != 1 || !w || !h)
        return 0;
    return (uint64_t)w * h <= NV2A_TEXSCALE_MAX_TEXELS;
}

int nv2a_texscale_has_alpha(const uint32_t *texels, size_t n)
{
    /* A sixteenth, rounded up; reached early in art, never in a picture. */
    size_t i, need = (n + 15) / 16, seen = 0;
    for (i = 0; i < n; i++)
        if ((texels[i] >> 24) != 0xFFu && ++seen >= need)
            return 1;
    return 0;
}

void nv2a_texscale_note(uint32_t w, uint32_t h, unsigned factor, uint64_t usec)
{
    atomic_fetch_add_explicit(&s_textures, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_texels, (uint64_t)w * h * factor * factor,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s_usec, usec, memory_order_relaxed);
    if (recomp_env(RENV_UI_SCALE_LOG)) {
        uint64_t n, texels, total;
        nv2a_texscale_stats(&n, &texels, &total);
        fprintf(stderr, "[UISCALE] scaled %ux%u x%u in %.1f ms; so far %llu textures, "
                "%.1f M texels (%.1f MB with their levels), %.1f ms\n", w, h, factor,
                (double)usec / 1000.0, (unsigned long long)n, (double)texels / 1e6,
                (double)texels * 4.0 * (factor == 4 ? 1.3125 : 1.25) / 1048576.0,
                (double)total / 1000.0);
    }
}

void nv2a_texscale_stats(uint64_t *textures, uint64_t *texels, uint64_t *usec)
{
    if (textures)
        *textures = atomic_load_explicit(&s_textures, memory_order_relaxed);
    if (texels)
        *texels = atomic_load_explicit(&s_texels, memory_order_relaxed);
    if (usec)
        *usec = atomic_load_explicit(&s_usec, memory_order_relaxed);
}

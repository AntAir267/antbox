/*
 * nv2a_texpack_img.c - texture packs: the pixels. PNG in and out, the title's
 * alpha scaled up, the mip chain, BC1/BC3 compression; see nv2a_texpack.h.
 * No threads, no files beyond the PNG: the tests call it directly.
 */
#include "nv2a_texpack.h"
#include "nv2a_texmip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only PNG is read (a mod's file is not trusted any more than it has to be),
 * and nothing larger than a texture a title could use. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS NV2A_TEXPACK_MAX_SIDE
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../third_party/stb/stb_image_write.h"
#define STB_DXT_IMPLEMENTATION
#include "../../third_party/stb/stb_dxt.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* The most texels a replacement may have (level 0): 8192 x 4096. */
#define MAX_TEXELS (8192u * 4096u)

uint32_t nv2a_texpack_level_dim(uint32_t d, uint32_t level)
{
    d = level >= 31 ? 0 : d >> level;
    return d ? d : 1;
}

size_t nv2a_texpack_level_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
    size_t bw = ((size_t)w + 3) / 4, bh = ((size_t)h + 3) / 4;
    switch (fmt) {
    case NV2A_TEXPACK_FMT_BC1: return bw * bh * 8;
    case NV2A_TEXPACK_FMT_BC3: return bw * bh * 16;
    default:                   return (size_t)w * h * 4;
    }
}

/* ── sizes ────────────────────────────────────────────────────────────── */

uint32_t nv2a_texpack_add_levels(uint32_t pw, uint32_t ph, uint32_t w, uint32_t h)
{
    double r, a;
    if (!w || !h || !pw || !ph)
        return 0;
    r = (double)pw / (double)w;
    a = (double)ph / (double)h;
    if (a > r)
        r = a;
    if (r < 1.0)
        return 0;
    return (uint32_t)(log2(r) + 0.5);
}

void nv2a_texpack_factors(uint32_t pw, uint32_t ph, uint32_t w, uint32_t h,
                          uint32_t *fx10, uint32_t *fy10)
{
    *fx10 = w ? (uint32_t)((uint64_t)pw * 10u / w) : 0;
    *fy10 = h ? (uint32_t)((uint64_t)ph * 10u / h) : 0;
}

uint32_t nv2a_texpack_levels(uint32_t pw, uint32_t ph, uint32_t levels, uint32_t add)
{
    uint32_t n = levels > 1 ? levels + add : add + 1, full = 1, d = pw > ph ? pw : ph;
    while (d > 1) {
        d >>= 1;
        full++;
    }
    return n > full ? full : n ? n : 1;
}

void nv2a_texpack_lod(uint32_t levels, uint32_t lod_min, uint32_t lod_max, uint32_t add,
                      float *min_lod, float *max_lod)
{
    /* The title's clamps move down by the levels added, except a minimum of
     * 0, which keeps the new detail. A title texture with one level is
     * blended down to its own size through the levels added. */
    *min_lod = levels > 1 && lod_min ? (float)(lod_min + add) : 0.0f;
    *max_lod = levels > 1 ? (float)(lod_max + add) : (float)add;
}

/* ── PNG ──────────────────────────────────────────────────────────────── */

uint8_t *nv2a_texpack_png_read(const char *path, uint32_t *w, uint32_t *h, int *has_alpha)
{
    int x = 0, y = 0, comp = 0;
    uint8_t *px;
    size_t i, n;
    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;
    if (!stbi_info_from_file(f, &x, &y, &comp) || x <= 0 || y <= 0
            || (uint32_t)x > NV2A_TEXPACK_MAX_SIDE || (uint32_t)y > NV2A_TEXPACK_MAX_SIDE
            || (uint64_t)x * (uint64_t)y > MAX_TEXELS) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    px = stbi_load_from_file(f, &x, &y, &comp, 4);
    fclose(f);
    if (!px)
        return NULL;
    n = (size_t)x * (size_t)y;
    for (i = 0; i < n; i++) {   /* RGBA bytes to BGRA */
        uint8_t r = px[i * 4];
        px[i * 4] = px[i * 4 + 2];
        px[i * 4 + 2] = r;
    }
    *w = (uint32_t)x;
    *h = (uint32_t)y;
    *has_alpha = comp == 2 || comp == 4;
    return px;
}

int nv2a_texpack_png_write(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    size_t i, n = (size_t)w * h;
    uint8_t *rgba = (uint8_t *)malloc(n * 4);
    int ok;

    if (!rgba)
        return 0;
    for (i = 0; i < n; i++) {
        rgba[i * 4]     = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4];
        rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
    ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)(w * 4u));
    free(rgba);
    return ok != 0;
}

/* ── alpha ────────────────────────────────────────────────────────────── */

void nv2a_texpack_alpha_scale(const uint8_t *alpha, uint32_t ow, uint32_t oh, uint8_t *dst,
                              uint32_t pw, uint32_t ph)
{
    uint32_t x, y;
    float kx = (float)ow / (float)pw, ky = (float)oh / (float)ph;

    for (y = 0; y < ph; y++) {
        float fy = ((float)y + 0.5f) * ky - 0.5f, wy;
        int y0, y1;
        if (fy < 0.0f)
            fy = 0.0f;
        y0 = (int)fy;
        wy = fy - (float)y0;
        if ((uint32_t)y0 >= oh - 1) {
            y0 = (int)oh - 1;
            wy = 0.0f;
        }
        y1 = y0 + 1 < (int)oh ? y0 + 1 : y0;
        for (x = 0; x < pw; x++) {
            float fx = ((float)x + 0.5f) * kx - 0.5f, wx, a, b;
            int x0, x1;
            if (fx < 0.0f)
                fx = 0.0f;
            x0 = (int)fx;
            wx = fx - (float)x0;
            if ((uint32_t)x0 >= ow - 1) {
                x0 = (int)ow - 1;
                wx = 0.0f;
            }
            x1 = x0 + 1 < (int)ow ? x0 + 1 : x0;
            a = (float)alpha[(size_t)y0 * ow + x0] * (1.0f - wx) + (float)alpha[(size_t)y0 * ow + x1] * wx;
            b = (float)alpha[(size_t)y1 * ow + x0] * (1.0f - wx) + (float)alpha[(size_t)y1 * ow + x1] * wx;
            dst[(size_t)y * pw + x] = (uint8_t)(a * (1.0f - wy) + b * wy + 0.5f);
        }
    }
}

/* ── mips ─────────────────────────────────────────────────────────────── */

/* Level l+1 from level l: a 2x2 box of every channel on its own (an odd side
 * drops its last row or column into the one before). */
static void box_down(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst, uint32_t dw,
                     uint32_t dh)
{
    uint32_t x, y;
    for (y = 0; y < dh; y++) {
        uint32_t y0 = sh > 1 ? 2 * y : 0, y1 = sh > 1 && y0 + 1 < sh ? y0 + 1 : y0;
        for (x = 0; x < dw; x++) {
            uint32_t x0 = sw > 1 ? 2 * x : 0, x1 = sw > 1 && x0 + 1 < sw ? x0 + 1 : x0, c;
            const uint8_t *a = src + ((size_t)y0 * sw + x0) * 4, *b = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t *c0 = src + ((size_t)y1 * sw + x0) * 4, *d = src + ((size_t)y1 * sw + x1) * 4;
            uint8_t *o = dst + ((size_t)y * dw + x) * 4;
            for (c = 0; c < 4; c++)
                o[c] = (uint8_t)(((uint32_t)a[c] + b[c] + c0[c] + d[c] + 2u) >> 2);
        }
    }
}

/* ── compression ──────────────────────────────────────────────────────── */

static int quality(void)
{
    static int q = -1;
    if (q < 0) {
        /* High quality costs next to nothing here (2048 x 1024 in 35 ms). */
        const char *e = getenv("TEXPACK_BC_QUALITY");
        q = e && *e == '0' ? STB_DXT_NORMAL : STB_DXT_HIGHQUAL;
    }
    return q;
}

/* One level of BGRA8 to blocks (RGBA order for the encoder). */
static void compress_level(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, int alpha)
{
    uint32_t bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4, step = alpha ? 16u : 8u;
    uint8_t block[64];

    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            uint32_t i, j;
            for (j = 0; j < 4; j++) {
                uint32_t yy = by * 4 + j < h ? by * 4 + j : h - 1;
                for (i = 0; i < 4; i++) {
                    uint32_t xx = bx * 4 + i < w ? bx * 4 + i : w - 1;
                    const uint8_t *p = src + ((size_t)yy * w + xx) * 4;
                    uint8_t *o = block + (j * 4 + i) * 4;
                    o[0] = p[2];
                    o[1] = p[1];
                    o[2] = p[0];
                    o[3] = p[3];
                }
            }
            stb_compress_dxt_block(dst + ((size_t)by * bw + bx) * step, block, alpha, quality());
        }
}

/* ── the replacement ──────────────────────────────────────────────────── */

void nv2a_texpack_img_free(struct nv2a_texpack_img *img)
{
    free(img->data);
    memset(img, 0, sizeof *img);
}

int nv2a_texpack_build(uint8_t *px, uint32_t pw, uint32_t ph, int has_alpha, int mode,
                       const uint8_t *alpha, uint32_t ow, uint32_t oh, uint32_t olevels,
                       uint32_t cut, uint32_t fmt_hint, struct nv2a_texpack_img *out)
{
    uint32_t add = nv2a_texpack_add_levels(pw, ph, ow, oh);
    uint32_t nl = nv2a_texpack_levels(pw, ph, olevels, add), l;
    size_t i, n0 = (size_t)pw * ph, raw = 0, total = 0;
    uint8_t *chain, *p;
    uint32_t fmt = NV2A_TEXPACK_FMT_BGRA8;
    int translucent = 0;

    memset(out, 0, sizeof *out);
    if (!px || !pw || !ph || n0 > MAX_TEXELS)
        return 0;
    /* Alpha: the PNG's, or the title's scaled to the PNG's size. The colour
     * is never touched. */
    if (mode == NV2A_TEXPACK_ALPHA_ORIGINAL || (mode == NV2A_TEXPACK_ALPHA_AUTO && !has_alpha)) {
        if (alpha && ow && oh) {
            uint8_t *a = (uint8_t *)malloc(n0);
            if (!a)
                return 0;
            if (ow == pw && oh == ph)
                memcpy(a, alpha, n0);
            else
                nv2a_texpack_alpha_scale(alpha, ow, oh, a, pw, ph);
            for (i = 0; i < n0; i++)
                px[i * 4 + 3] = a[i];
            free(a);
        } else {
            for (i = 0; i < n0; i++)
                px[i * 4 + 3] = 255;
        }
    }
    for (i = 0; i < n0; i++)
        if (px[i * 4 + 3] != 255) {
            translucent = 1;
            break;
        }
    /* The whole chain, BGRA8. */
    for (l = 0; l < nl; l++)
        raw += (size_t)nv2a_texpack_level_dim(pw, l) * nv2a_texpack_level_dim(ph, l) * 4u;
    chain = (uint8_t *)malloc(raw);
    if (!chain)
        return 0;
    memcpy(chain, px, n0 * 4);
    for (l = 1, p = chain; l < nl; l++) {
        uint32_t sw = nv2a_texpack_level_dim(pw, l - 1), sh = nv2a_texpack_level_dim(ph, l - 1);
        uint32_t dw = nv2a_texpack_level_dim(pw, l), dh = nv2a_texpack_level_dim(ph, l);
        box_down(p, sw, sh, p + (size_t)sw * sh * 4, dw, dh);
        p += (size_t)sw * sh * 4;
    }
    if (cut && nl > 1)
        (void)nv2a_texmip_cutout((uint32_t *)chain, pw, ph, nl, cut);
    if (fmt_hint == NV2A_TEXPACK_FMT_BC1 || fmt_hint == NV2A_TEXPACK_FMT_BC3)
        fmt = translucent ? NV2A_TEXPACK_FMT_BC3 : NV2A_TEXPACK_FMT_BC1;
    if (fmt == NV2A_TEXPACK_FMT_BGRA8) {
        out->data = chain;
        out->bytes = raw;
    } else {
        uint8_t *dst;
        for (l = 0; l < nl; l++)
            total += nv2a_texpack_level_bytes(fmt, nv2a_texpack_level_dim(pw, l),
                                              nv2a_texpack_level_dim(ph, l));
        dst = (uint8_t *)malloc(total);
        if (!dst) {
            free(chain);
            return 0;
        }
        for (l = 0, p = chain, total = 0; l < nl; l++) {
            uint32_t lw = nv2a_texpack_level_dim(pw, l), lh = nv2a_texpack_level_dim(ph, l);
            compress_level(p, lw, lh, dst + total, fmt == NV2A_TEXPACK_FMT_BC3);
            total += nv2a_texpack_level_bytes(fmt, lw, lh);
            p += (size_t)lw * lh * 4u;
        }
        free(chain);
        out->data = dst;
        out->bytes = total;
    }
    out->w = pw;
    out->h = ph;
    out->levels = nl;
    out->fmt = fmt;
    out->add = add;
    return 1;
}

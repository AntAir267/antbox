/**
 * nv2a_texmip.c - the lower levels of an alpha-tested texture, made again.
 * See nv2a_texmip.h.
 */
#include "nv2a_texmip.h"
#include "nv2a_backend_common.h"
#include "nv2a_texrep.h"
#include "nv2a_pb_state.h"
#include "recomp_env.h"
#include "host_time.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t s_made, s_ns;

static volatile int s_mode = -1;     /* -1: not read yet */
static uint32_t     s_ab;            /* alpha_mips_ab: flips per half, 0 off */

unsigned nv2a_texmip_mode(void)
{
    if (s_mode < 0) {
        const char *e = recomp_env(RENV_ALPHA_MIPS), *ab = recomp_env(RENV_ALPHA_MIPS_AB);
        s_ab = ab ? (uint32_t)strtoul(ab, NULL, 0) : 0;
        s_mode = !e ? NV2A_TEXMIP_LEVELS | NV2A_TEXMIP_BIAS
               : !strcmp(e, "0") || !strcmp(e, "off") ? 0
               : !strcmp(e, "mips") ? NV2A_TEXMIP_LEVELS
               : !strcmp(e, "bias") ? NV2A_TEXMIP_BIAS
               : NV2A_TEXMIP_LEVELS | NV2A_TEXMIP_BIAS;
    }
    /* alpha_mips_ab=n: the mode on for n flips, then off for n, and so on
     * (the same frame or the same stretch of road seen both ways in one
     * run, BO-171's comparisons). */
    if (s_ab) {
        static uint32_t said = 0xFFFFFFFFu;
        uint32_t f = nv2a_pb_gpu_state()->flips, half = (f / s_ab) & 1u;
        if (half != said) {
            said = half;
            fprintf(stderr, "[TEXMIP] ab: %s from flip %u\n", half ? "off" : "on", f);
        }
        return half ? 0u : (unsigned)s_mode;
    }
    return (unsigned)s_mode;
}

uint32_t nv2a_texmip_cut_ref(int atest_enable, uint32_t gl_func, uint32_t ref)
{
    uint32_t f = nv2a_cmp_from_gl(gl_func);
    ref &= 0xFFu;
    if (!atest_enable || ref == 0 || ref == 255
            || (f != NV2A_CMP_GREATER && f != NV2A_CMP_GEQUAL))
        return 0;
    return (1u + ref) | (f == NV2A_CMP_GEQUAL ? 0x100u : 0u);
}

static int passes(uint32_t a, uint32_t cut)
{
    uint32_t ref = (cut & 0xFFu) - 1u;
    return (cut & 0x100u) ? a >= ref : a > ref;
}

float nv2a_texmip_coverage(const uint32_t *texels, uint32_t w, uint32_t h, uint32_t cut)
{
    size_t n = (size_t)w * h, i, k = 0;
    if (!n || !cut)
        return 0.0f;
    for (i = 0; i < n; i++)
        k += (size_t)passes(texels[i] >> 24, cut);
    return (float)k / (float)n;
}

/* The alpha threshold histogram: a level's box-filtered alpha in 4096 steps. */
#define BINS 4096

int nv2a_texmip_cutout(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels,
                       uint32_t cut)
{
    size_t n0 = (size_t)w * h, pass0 = 0, i;
    float *cur, *next, *col, *ncol;      /* alpha; colour premultiplied by it */
    uint32_t *hist, *dst = texels, l, cw = w, ch = h;
    float ref, c0;

    if (levels < 2 || !cut || !n0 || w > 4096 || h > 4096)
        return 0;
    for (i = 0; i < n0; i++)
        pass0 += (size_t)passes(texels[i] >> 24, cut);
    if (pass0 == 0 || pass0 == n0)
        return 0;
    c0 = (float)pass0 / (float)n0;
    ref = (float)((cut & 0xFFu) - 1u);
    /* One block: two alpha planes (this level and the next) and two colour
     * planes of three channels, each as large as level 0 (the next level
     * needs a quarter; level 0's size keeps the arithmetic simple). */
    cur = (float *)malloc(n0 * 8 * sizeof(float));
    hist = (uint32_t *)malloc(BINS * sizeof(uint32_t));
    if (!cur || !hist) {
        free(cur);
        free(hist);
        return 0;
    }
    next = cur + n0;
    col = next + n0;
    ncol = col + 3 * n0;
    for (i = 0; i < n0; i++) {
        uint32_t t = texels[i];
        float a = (float)(t >> 24) / 255.0f;
        cur[i] = a;
        col[3 * i + 0] = (float)((t >> 16) & 0xFFu) * a;
        col[3 * i + 1] = (float)((t >> 8) & 0xFFu) * a;
        col[3 * i + 2] = (float)(t & 0xFFu) * a;
    }
    for (l = 1; l < levels; l++) {
        uint32_t dw = cw > 1 ? cw / 2 : 1, dh = ch > 1 ? ch / 2 : 1, x, y, b;
        size_t dn = (size_t)dw * dh, k, want, sum;
        float s, t, *sw;

        dst += (size_t)cw * ch;
        /* 2x2 box (a level of width or height 1 repeats its only row or
         * column, as nv2a_texrep's remip). Colour stays premultiplied. */
        for (y = 0; y < dh; y++) {
            uint32_t y0 = ch > 1 ? 2 * y : 0, y1 = ch > 1 ? 2 * y + 1 : 0;
            for (x = 0; x < dw; x++) {
                uint32_t x0 = cw > 1 ? 2 * x : 0, x1 = cw > 1 ? 2 * x + 1 : 0, c;
                size_t a = (size_t)y0 * cw + x0, bb = (size_t)y0 * cw + x1;
                size_t cc = (size_t)y1 * cw + x0, d = (size_t)y1 * cw + x1;
                size_t o = (size_t)y * dw + x;
                next[o] = (cur[a] + cur[bb] + cur[cc] + cur[d]) * 0.25f;
                for (c = 0; c < 3; c++)
                    ncol[3 * o + c] = (col[3 * a + c] + col[3 * bb + c] + col[3 * cc + c]
                                       + col[3 * d + c]) * 0.25f;
            }
        }
        /* The scale: the texel whose alpha is the want-th largest lands on
         * the reference, so want texels pass, the share level 0 has. */
        want = (size_t)((float)dn * c0 + 0.5f);
        s = 1.0f;
        if (want > 0) {
            memset(hist, 0, BINS * sizeof(uint32_t));
            for (k = 0; k < dn; k++)
                hist[(uint32_t)(next[k] * (float)(BINS - 1) + 0.5f)]++;
            for (b = BINS - 1, sum = 0; b > 0; b--) {
                sum += hist[b];
                if (sum >= want)
                    break;
            }
            /* Box alpha comes in steps (a 2x2 of a 1-bit cut-out has five
             * values), so the bin may hold many texels: all of it passes
             * (b's lower edge) or none (its upper edge), whichever lands
             * nearer the share wanted. Bin 0 is alpha 0, which no scale
             * makes pass. */
            if (b < BINS - 1 && want - (sum - hist[b]) < sum - want)
                t = ((float)b + 0.5f) / (float)(BINS - 1);
            else
                t = ((float)(b ? b : 1) - 0.5f) / (float)(BINS - 1);
            s = ((cut & 0x100u) ? ref - 0.5f + 0.01f : ref + 0.5f + 0.01f) / (255.0f * t);
            if (s > 64.0f)
                s = 64.0f;
            /* A low reference (under 16) is a "not transparent" test, which
             * the box filter can only pass more texels of: scaled down to
             * land on it, the level keeps its share against the texel's
             * alpha, but the test compares the combiners' output, and with
             * so low a reference that is likelier the texel's alpha times a
             * constant than the alpha itself. Double-S.T.E.A.L 2's tree
             * canopies pass "alpha >= 2" with their texture's alpha x 4/255
             * (a 50% cut on the texel): every level below the first failed
             * and the canopies were gone past a few metres. Such a level
             * keeps the box filter's alpha, as the title's own levels do. */
            if (s < 1.0f && ref < 16.0f)
                s = 1.0f;
        }
        for (k = 0; k < dn; k++) {
            float a = next[k], ao = a * s, r, g, bl;
            uint32_t ai;
            if (ao > 1.0f)
                ao = 1.0f;
            ai = (uint32_t)(ao * 255.0f + 0.5f);
            if (a > 0.0f) {
                r = ncol[3 * k + 0] / a;
                g = ncol[3 * k + 1] / a;
                bl = ncol[3 * k + 2] / a;
            } else {            /* no visible texel under it: its own colour */
                uint32_t o = dst[k];
                r = (float)((o >> 16) & 0xFFu);
                g = (float)((o >> 8) & 0xFFu);
                bl = (float)(o & 0xFFu);
            }
            r = r > 255.0f ? 255.0f : r;
            g = g > 255.0f ? 255.0f : g;
            bl = bl > 255.0f ? 255.0f : bl;
            dst[k] = ai << 24 | (uint32_t)(r + 0.5f) << 16 | (uint32_t)(g + 0.5f) << 8
                   | (uint32_t)(bl + 0.5f);
        }
        sw = cur; cur = next; next = sw;
        sw = col; col = ncol; ncol = sw;
        cw = dw;
        ch = dh;
    }
    free(cur < next ? cur : next);
    free(hist);
    return 1;
}

void nv2a_texmip_stats(uint64_t *made, uint64_t *ns)
{
    if (made)
        *made = atomic_load_explicit(&s_made, memory_order_relaxed);
    if (ns)
        *ns = atomic_load_explicit(&s_ns, memory_order_relaxed);
}

/* The coverage of levels 0..2 into c[3] (-1 for a level the texture has not). */
static void coverage3(const uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels,
                      uint32_t cut, float *c)
{
    uint32_t l;
    for (l = 0; l < 3; l++) {
        uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
        c[l] = l < levels ? nv2a_texmip_coverage(texels, lw, lh, cut) : -1.0f;
        texels += (size_t)lw * lh;
    }
}

int nv2a_texmip_apply(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                      uint32_t pitch, uint32_t levels, uint32_t cut, uint32_t *texels)
{
    static int log = -1;
    float before[3], after[3];
    uint64_t t0, dt;
    int r;

    if (!cut || levels < 2 || !(nv2a_texmip_mode() & NV2A_TEXMIP_LEVELS))
        return 0;
    if (log < 0)
        log = recomp_env(RENV_ALPHA_MIPS_LOG) != NULL;
    if (log)
        coverage3(texels, w, h, levels, cut, before);
    t0 = xbox_HostNowNs();
    r = nv2a_texmip_cutout(texels, w, h, levels, cut);
    dt = xbox_HostNowNs() - t0;
    if (r) {
        atomic_fetch_add_explicit(&s_made, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_ns, dt, memory_order_relaxed);
    }
    if (log) {
        coverage3(texels, w, h, levels, cut, after);
        fprintf(stderr, "[TEXMIP] %016llx fmt 0x%02x %ux%u levels %u alpha %s %u: coverage"
                " %.3f, level 1 %.3f -> %.3f, level 2 %.3f -> %.3f; %s, %.3f ms (%llu made,"
                " %.1f ms in all)\n",
                (unsigned long long)nv2a_texrep_hash(guest, color, w, h, pitch), color & 0xFFu,
                w, h, levels, (cut & 0x100u) ? ">=" : ">", (cut & 0xFFu) - 1u, before[0],
                before[1], after[1], before[2], after[2], r ? "made" : "kept",
                (double)dt / 1e6, (unsigned long long)atomic_load(&s_made),
                (double)atomic_load(&s_ns) / 1e6);
    }
    return r;
}

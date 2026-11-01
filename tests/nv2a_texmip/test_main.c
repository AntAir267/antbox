/* nv2a_texmip: a cut-out's levels 1.. keep level 0's alpha coverage at the
 * draw's reference (where a box filter loses it), level 0 is not touched,
 * colour is averaged by alpha (transparent black does not darken), and an
 * opaque texture, one level, or a test that is not a cut-out leave the
 * texture as it was. Synthetic textures only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nv2a_texmip.h"
#include "nv2a_backend_common.h"

static int s_checks, s_fail;
#define CHECK(x) do { s_checks++; if (!(x)) { s_fail++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define W 64u
#define H 64u
#define LEVELS 7u
#define GL_GREATER 0x204u
#define GL_GEQUAL  0x206u
#define GL_LESS    0x201u

static uint32_t rng = 12345u;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* A "tree": thin leaves, ~35% of the texels opaque green, the rest
 * transparent black (as a DXT1 cut-out decodes), and a box chain below. */
static void tree(uint32_t *t)
{
    uint32_t x, y, l, w = W, h = H;
    uint32_t *src = t;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = (rnd() % 100) < 35 ? 0xFF30A040u : 0x00000000u;
    for (l = 1; l < LEVELS; l++) {          /* the title's own: a plain box */
        uint32_t dw = w / 2 ? w / 2 : 1, dh = h / 2 ? h / 2 : 1;
        uint32_t *dst = src + (size_t)w * h;
        for (y = 0; y < dh; y++)
            for (x = 0; x < dw; x++) {
                uint32_t c[4] = { src[2 * y * w + 2 * x], src[2 * y * w + 2 * x + 1],
                                  src[(2 * y + 1) * w + 2 * x], src[(2 * y + 1) * w + 2 * x + 1] };
                uint32_t o = 0, s, k;
                for (s = 0; s < 32; s += 8) {
                    uint32_t sum = 0;
                    for (k = 0; k < 4; k++)
                        sum += (c[k] >> s) & 0xFFu;
                    o |= ((sum + 2) / 4) << s;
                }
                dst[y * dw + x] = o;
            }
        src = dst;
        w = dw;
        h = dh;
    }
}

int main(void)
{
    size_t n = nv2a_tex_texels(W, H, LEVELS);
    uint32_t *t = (uint32_t *)malloc(n * 4), *o = (uint32_t *)malloc(n * 4);
    uint32_t cut, l, w, h, *p;
    float c0, c1box, c;
    size_t i;

    /* The reference as the backends hand it. */
    CHECK(nv2a_texmip_cut_ref(0, GL_GREATER, 0x80) == 0);
    CHECK(nv2a_texmip_cut_ref(1, GL_LESS, 0x80) == 0);
    CHECK(nv2a_texmip_cut_ref(1, GL_GREATER, 0) == 0);
    CHECK(nv2a_texmip_cut_ref(1, GL_GEQUAL, 255) == 0);
    CHECK(nv2a_texmip_cut_ref(1, GL_GREATER, 0x80) == 0x81u);
    CHECK(nv2a_texmip_cut_ref(1, GL_GEQUAL, 0x80) == 0x181u);

    tree(t);
    memcpy(o, t, n * 4);
    cut = nv2a_texmip_cut_ref(1, GL_GREATER, 0x80);
    c0 = nv2a_texmip_coverage(t, W, H, cut);
    c1box = nv2a_texmip_coverage(t + W * H, W / 2, H / 2, cut);
    CHECK(c0 > 0.3f && c0 < 0.4f);
    CHECK(c1box < c0 - 0.1f);                 /* the box loses leaves */
    {   /* by level 2 it has lost most of them */
        float c2box = nv2a_texmip_coverage(t + W * H + (W / 2) * (H / 2), W / 4, H / 4, cut);
        CHECK(c2box < 0.5f * c0);
    }

    CHECK(nv2a_texmip_cutout(t, W, H, LEVELS, cut) == 1);
    CHECK(memcmp(t, o, W * H * 4) == 0);      /* level 0 untouched */
    for (l = 1, p = t + W * H, w = W / 2, h = H / 2; l < LEVELS; l++) {
        float cbox = nv2a_texmip_coverage(o + (p - t), w, h, cut);
        c = nv2a_texmip_coverage(p, w, h, cut);
        /* Never further from level 0 than the box; close from level 2 on
         * (level 1 of a 1-bit cut-out has only five alpha values, so its
         * share moves in steps). */
        if (w * h >= 16) {
            CHECK(fabsf(c - c0) <= fabsf(cbox - c0) + 1e-6f);
            CHECK(fabsf(c - c0) < (l == 1 ? 0.1f : 0.04f));
        }
        /* every texel that passes is the leaves' green, not darkened */
        for (i = 0; i < (size_t)w * h; i++)
            if ((p[i] >> 24) > 0x80u)
                CHECK((p[i] & 0xFFFFFFu) == 0x30A040u);
        p += (size_t)w * h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }

    /* >=: the same */
    tree(t);
    cut = nv2a_texmip_cut_ref(1, GL_GEQUAL, 0x40);
    c0 = nv2a_texmip_coverage(t, W, H, cut);
    CHECK(nv2a_texmip_cutout(t, W, H, LEVELS, cut) == 1);
    c = nv2a_texmip_coverage(t + W * H + (W / 2) * (H / 2), W / 4, H / 4, cut);
    CHECK(fabsf(c - c0) < 0.04f);

    /* Left alone: opaque, one level, no cut-out test. */
    for (i = 0; i < n; i++)
        t[i] = 0xFF000000u | (uint32_t)i;
    memcpy(o, t, n * 4);
    CHECK(nv2a_texmip_cutout(t, W, H, LEVELS, nv2a_texmip_cut_ref(1, GL_GREATER, 0x80)) == 0);
    CHECK(memcmp(t, o, n * 4) == 0);
    tree(t);
    memcpy(o, t, n * 4);
    CHECK(nv2a_texmip_cutout(t, W, H, 1, nv2a_texmip_cut_ref(1, GL_GREATER, 0x80)) == 0);
    CHECK(nv2a_texmip_cutout(t, W, H, LEVELS, 0) == 0);
    CHECK(memcmp(t, o, n * 4) == 0);

    /* A low reference (under 16): the box filter's alpha is the floor,
     * never scaled under it. The test compares the combiners' output, and
     * Double-S.T.E.A.L 2's canopies pass ">= 2" with their texel's alpha x
     * 4/255: scaled down to land on 2, no level below the first passed. */
    tree(t);
    memcpy(o, t, n * 4);
    CHECK(nv2a_texmip_cutout(t, W, H, LEVELS, nv2a_texmip_cut_ref(1, GL_GEQUAL, 2)) == 1);
    for (l = 1, p = t + W * H, w = W / 2, h = H / 2; l < LEVELS; l++) {
        const uint32_t *q = o + (p - t);
        for (i = 0; i < (size_t)w * h; i++)
            CHECK((p[i] >> 24) >= (q[i] >> 24) || (q[i] >> 24) - (p[i] >> 24) <= 1u);
        p += (size_t)w * h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }

    /* The same input, the same output. */
    tree(t);
    memcpy(o, t, n * 4);
    nv2a_texmip_cutout(t, W, H, LEVELS, 0x81u);
    nv2a_texmip_cutout(o, W, H, LEVELS, 0x81u);
    CHECK(memcmp(t, o, n * 4) == 0);

    free(t);
    free(o);
    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/* nv2a_texmip_mode's alpha_mips_ab reads the flip count; never set here. */
#include "nv2a_pb_state.h"
const struct nv2a_pb_gpu *nv2a_pb_gpu_state(void)
{
    static struct nv2a_pb_gpu g;
    return &g;
}

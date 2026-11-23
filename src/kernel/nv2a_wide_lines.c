/* BO-385: lines one guest pixel wide at any render scale, for the GPU
 * backends (nv2a_backend_common.h has the why and the contract). Its own
 * file: it runs the CPU vertex program (nv2a_vsh_cpu.c), which the unit
 * tests that link nv2a_backend_common.c do not carry. */
#include "nv2a_backend_common.h"
#include "nv2a_vsh_cpu.h"
#include "recomp_env.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* BO-385: lines one guest pixel wide at any render scale (the header). */
int nv2a_wide_lines(const struct nv2a_pb_vsh *v, enum nv2a_topology topo, uint32_t w, uint32_t hw)
{
    static int on = -1;

    if (on < 0) {
        const char *e = recomp_env(RENV_WIDE_LINES);
        on = !(e && e[0] == '0');
    }
    return on && topo == NV2A_TOPO_LINES && hw > w && !v->fog_enable;
}

uint32_t nv2a_lines_expand(const struct nv2a_pb_draw *d, const struct nv2a_pb_vsh *v,
                           const uint32_t *lidx, uint32_t n, uint32_t lo, uint32_t count,
                           const uint8_t *mem, float *out, uint32_t *oidx)
{
    /* The pass-through inputs: position, colours, the four texture sets. */
    static const int pass[NV2A_WL_F4] = { 0, 3, 4, 9, 10, 11, 12 };
    static const int all[NV2A_VSH_INPUTS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static float *in, (*c)[4];
    static uint8_t *okv;
    static uint32_t cap;
    const int nin = d->program ? NV2A_VSH_INPUTS : NV2A_WL_F4;
    uint32_t i, k = 0, nv = 0;

    if (count > cap) {
        float *a = (float *)realloc(in, (size_t)count * NV2A_VSH_INPUTS * 16u);
        uint8_t *b = a ? (uint8_t *)realloc(okv, count) : NULL;
        if (a)
            in = a;
        if (!b)
            return 0;
        okv = b;
        cap = count;
    }
    if (!c && !(c = (float (*)[4])malloc(sizeof v->c)))
        return 0;
    nv2a_vtx_gather(d, d->program ? all : pass, nin, lo, count, v->inl, mem, in);
    if (d->program) {
        /* The program writes constants only with CXT_WRITE_EN: a copy, so a
         * batch's own writes stay out of the state the GPU draws with. */
        memcpy(c, v->c, sizeof v->c);
        for (i = 0; i < count; i++) {
            float *x = in + (size_t)i * NV2A_VSH_INPUTS * 4u;
            Nv2aVshOut o;
            int q, t;

            nv2a_vsh_run((const uint32_t (*)[4])v->prog, v->start, c, v->cxt_write,
                         (const float (*)[4])x, &o);
            /* As the software rasteriser's vsh_transform: w 0 is 1, the
             * colours clamped, oD1 (0, 0, 0, 1) without specular. */
            if (o.o[NV2A_VSH_O_POS][3] == 0.0f)
                o.o[NV2A_VSH_O_POS][3] = 1.0f;
            okv[i] = o.ok && o.o[NV2A_VSH_O_POS][3] > 0.0f;
            for (q = 0; q < 4; q++)
                okv[i] &= isfinite(o.o[NV2A_VSH_O_POS][q]) != 0;
            x[0] = o.o[NV2A_VSH_O_POS][0], x[1] = o.o[NV2A_VSH_O_POS][1];
            x[2] = o.o[NV2A_VSH_O_POS][2], x[3] = 1.0f / o.o[NV2A_VSH_O_POS][3];
            for (q = 0; q < 4; q++) {
                float a = o.o[NV2A_VSH_O_D0][q], b = o.o[NV2A_VSH_O_D1][q];
                x[4 + q] = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
                x[8 + q] = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
                for (t = 0; t < 4; t++)
                    x[12 + 4 * t + q] = o.o[NV2A_VSH_O_T0 + t][q];
            }
            if (!v->spec_enable) {
                x[8] = x[9] = x[10] = 0.0f;
                x[11] = 1.0f;
            } else if (!(v->light_ctl & (1u << 17))) {
                x[11] = 1.0f;
            }
        }
    } else {
        for (i = 0; i < count; i++) {
            const float *x = in + (size_t)i * NV2A_WL_F4 * 4u;
            okv[i] = isfinite(x[0]) && isfinite(x[1]) && isfinite(x[2]);
        }
    }
    for (i = 0; i + 1 < n; i += 2) {
        uint32_t a = lidx[i], b = lidx[i + 1], e;
        const float *pa, *pb;
        float dx, dy, len, nx, ny;

        if (a >= count || b >= count || !okv[a] || !okv[b])
            continue;
        pa = in + (size_t)a * nin * 4u;
        pb = in + (size_t)b * nin * 4u;
        dx = pb[0] - pa[0], dy = pb[1] - pa[1];
        len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-6f) {
            nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
        } else {
            nx = 0.5f, ny = 0.0f;       /* a dot: half a pixel either side */
        }
        /* Corners a + n, a - n, b + n, b - n; the ends' other outputs. */
        for (e = 0; e < 4; e++) {
            const float *p = e < 2 ? pa : pb;
            float sgn = (e & 1) ? -1.0f : 1.0f;
            float *o = out + (size_t)(nv + e) * NV2A_WL_F4 * 4u;
            memcpy(o, p, NV2A_WL_F4 * 16u);
            o[0] = p[0] + sgn * nx;
            o[1] = p[1] + sgn * ny;
        }
        oidx[k++] = nv, oidx[k++] = nv + 1, oidx[k++] = nv + 2;
        oidx[k++] = nv + 2, oidx[k++] = nv + 1, oidx[k++] = nv + 3;
        nv += 4;
    }
    return k;
}

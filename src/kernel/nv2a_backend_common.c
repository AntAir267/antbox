/**
 * NV2A state logic shared by the pushbuffer backends: the out-of-line half
 * of nv2a_backend_common.h. No OS or graphics headers.
 */
#include "nv2a_backend_common.h"
#include "nv2a_pb_split.h"
#include "recomp_env.h"
#include "platform/host_time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(fmt), tb, l, n = 0;
    if (bb || d3d8_format_is_swizzled(fmt)) {
        tb = bb ? 0 : nv2a_tex_texel_bytes(nv2a_tex_linear_twin(fmt));
        if (!bb && !tb)
            return 0;
        for (l = 0; l < levels; l++) {
            n += bb ? ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb : w * h * tb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return n;
    }
    tb = nv2a_tex_texel_bytes(fmt);
    if (!tb || !pitch)
        return 0;
    return pitch * (h - 1u) + w * tb;
}

uint32_t nv2a_tex_face_stride(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels)
{
    uint32_t n = nv2a_tex_size_from_format(fmt)
               ? nv2a_tex_extent(fmt, w, h, pitch, levels ? levels : 1u)
               : pitch * h;
    return (n + 127u) & ~127u;
}

/* RECOMP_DEBUG=vtx_record=<path> (BO-282): the gathers of vtx_record_n draws
 * from vtx_record_at seconds after the first one, as tests/nv2a_vtx_gather
 * replays them: each draw's inputs and the guest bytes its streams are read
 * from, so the gather can be run again on them away from the game. All
 * little-endian 32-bit words:
 *   file  "VTXR", version 1
 *   draw  "DRAW", nf4, lo, count, inline_data, inline_bytes, then per input
 *         slot, offset, type, size, stride and the SET_VERTEX_DATA value
 *         (4 floats); then an inline draw's payload (bytes, then the bytes,
 *         a 4-byte multiple), or for each input has-bytes and, when 1, the
 *         guest address of vertex lo's element, the length and the bytes.
 * Only the render thread calls the gather, so the state is unlocked. */
static int      s_vrec = -1;            /* -1 unread, 0 off, 1 waiting or on */
static FILE    *s_vrec_f;
static uint64_t s_vrec_t0, s_vrec_at;
static uint32_t s_vrec_left;

/* The bytes nv2a_vtx_gather reads of one element of a stream. */
static uint32_t vrec_elem_bytes(uint32_t type, uint32_t size)
{
    uint32_t n = size < 4 ? size : 4;
    switch (type) {
    case NV2A_VTX_FLOAT:    return 4 * n;
    case NV2A_VTX_D3DCOLOR: return 4;
    case NV2A_VTX_UBYTE:    return n;
    case NV2A_VTX_S1:
    case NV2A_VTX_S32K:     return 2 * n;
    case NV2A_VTX_CMP:      return 4;
    default:                return 0;
    }
}

static void vrec_u32(uint32_t v) { fwrite(&v, 4, 1, s_vrec_f); }

static void vtx_record(const struct nv2a_pb_draw *d, const int *slot, int nf4,
                       uint32_t lo, uint32_t count, const float (*inl)[4],
                       const uint8_t *mem)
{
    uint64_t now = xbox_HostNowNs();
    int k;

    if (s_vrec < 0) {
        const char *path = recomp_env(RENV_VTX_RECORD);
        const char *at = recomp_env(RENV_VTX_RECORD_AT);
        const char *n = recomp_env(RENV_VTX_RECORD_N);
        s_vrec = path && *path;
        if (!s_vrec)
            return;
        s_vrec_t0 = now;
        s_vrec_at = (uint64_t)((at ? atof(at) : 60.0) * 1e9);
        s_vrec_left = n ? (uint32_t)strtoul(n, NULL, 0) : 3000u;
        s_vrec_f = fopen(path, "wb");
        if (!s_vrec_f || !s_vrec_left) {
            fprintf(stderr, "[VTXREC] cannot write %s\n", path);
            s_vrec = 0;
            return;
        }
        fwrite("VTXR", 4, 1, s_vrec_f);
        vrec_u32(1);
    }
    if (!s_vrec || now - s_vrec_t0 < s_vrec_at)
        return;
    if (nf4 > NV_VERTEX_ATTRS)
        nf4 = NV_VERTEX_ATTRS;
    fwrite("DRAW", 4, 1, s_vrec_f);
    vrec_u32((uint32_t)nf4);
    vrec_u32(lo);
    vrec_u32(count);
    vrec_u32((uint32_t)d->inline_data);
    vrec_u32(d->inline_bytes);
    for (k = 0; k < nf4; k++) {
        const VertexAttr *a = &d->attr[slot[k]];
        vrec_u32((uint32_t)slot[k]);
        vrec_u32(a->offset);
        vrec_u32(a->type);
        vrec_u32(a->size);
        vrec_u32(a->stride);
        fwrite(inl[slot[k]], 4, 4, s_vrec_f);
    }
    if (d->inline_data) {
        /* An element starting within the payload is read whole, up to 12
         * bytes past its end. */
        uint32_t n = d->inline_bytes + 12u;
        if (n > NV_MAX_INLINE * 4u)
            n = NV_MAX_INLINE * 4u;
        vrec_u32(n);
        fwrite(d->inline_buf, 1, n, s_vrec_f);
    } else {
        for (k = 0; k < nf4; k++) {
            const VertexAttr *a = &d->attr[slot[k]];
            uint64_t len = count ? (uint64_t)(count - 1u) * a->stride
                                   + vrec_elem_bytes(a->type, a->size) : 0;
            uint32_t va = a->offset + lo * a->stride;
            if (!a->size || !a->stride || !a->offset || !len || len > (32u << 20)) {
                vrec_u32(0);
                continue;
            }
            vrec_u32(1);
            vrec_u32(va);
            vrec_u32((uint32_t)((len + 3u) & ~3ull));
            fwrite(mem + va, 1, (size_t)len, s_vrec_f);
            if (len & 3u)
                fwrite("\0\0\0", 1, (size_t)(4u - (len & 3u)), s_vrec_f);
        }
    }
    if (--s_vrec_left == 0) {
        fclose(s_vrec_f);
        s_vrec_f = NULL;
        s_vrec = 0;
        fprintf(stderr, "[VTXREC] done: %s\n", recomp_env(RENV_VTX_RECORD));
    }
}

/* CMP (11:11:10 signed normalised in a dword) as nv2a_vtx_decode makes it:
 * x and y are the low and middle 11 bits over 1023, z the top 10 over 511.
 * One table entry per field value, worked out by the compiler
 * (tests/nv2a_vtx_gather checks every one against the division). */
#define CMP11(i)   ((float)((int)(i) - ((i) >= 1024 ? 2048 : 0)) / 1023.0f)
#define CMP10(i)   ((float)((int)(i) - ((i) >= 512 ? 1024 : 0)) / 511.0f)
#define T4(F, i)   F(i), F((i) + 1), F((i) + 2), F((i) + 3)
#define T16(F, i)  T4(F, i), T4(F, (i) + 4), T4(F, (i) + 8), T4(F, (i) + 12)
#define T64(F, i)  T16(F, i), T16(F, (i) + 16), T16(F, (i) + 32), T16(F, (i) + 48)
#define T256(F, i) T64(F, i), T64(F, (i) + 64), T64(F, (i) + 128), T64(F, (i) + 192)
const float nv2a_vtx_cmp11[2048] = {
    T256(CMP11, 0), T256(CMP11, 256), T256(CMP11, 512), T256(CMP11, 768),
    T256(CMP11, 1024), T256(CMP11, 1280), T256(CMP11, 1536), T256(CMP11, 1792)
};
const float nv2a_vtx_cmp10[1024] = {
    T256(CMP10, 0), T256(CMP10, 256), T256(CMP10, 512), T256(CMP10, 768)
};
#undef T256
#undef T64
#undef T16
#undef T4
#undef CMP10
#undef CMP11

/* What nv2a_vtx_gather does with an input, decided once per draw (BO-282):
 * before, every element went through the stream's NULL test, the inline
 * payload's bounds test, a test of the type and a switch on the size, and
 * every colour channel, packed normal and normalised short through
 * divisions. In a Silver Lake race 54 % of the vertices carry a CMP normal
 * and 27 % a D3DCOLOR; no stream there is a normalised short. */
enum vin_kind {
    VIN_DFLT,                   /* off: the SET_VERTEX_DATA value */
    VIN_F1, VIN_F2, VIN_F3, VIN_F4,     /* FLOAT, 1 .. 4 components */
    VIN_COLOR,                  /* D3DCOLOR */
    VIN_CMP,                    /* 11:11:10 */
    VIN_S1,                     /* normalised short, 1 .. 4 components */
    VIN_DECODE                  /* the rest: nv2a_vtx_decode */
};

struct vin {
    const uint8_t *p;           /* vertex lo's element */
    size_t         stride;
    uint32_t       kind, type, size;
    uint32_t       live;        /* vertices 0 .. live - 1 read p, the rest
                                 * (past an inline payload's end) dflt */
    const float   *dflt;        /* SET_VERTEX_DATA value */
};

/* Vertices are made a block at a time on the stack (VTILE_FLOATS, as many
 * vertices as fit), one input at a time down the block (a loop per input
 * kind, no test per element), and the block is then copied out whole: the
 * stores to `out` stay in address order, which write-combined upload memory
 * needs. 8 KB: 170 vertices of three inputs, 32 of sixteen. */
#define VTILE_FLOATS 2048u

/* Input s's elements of the n vertices from i0 (all below s->live) into t,
 * every ts floats. */
static void vin_run(const struct vin *s, uint32_t i0, uint32_t n, float *t, size_t ts)
{
    const uint8_t *p = s->p + (size_t)i0 * s->stride;
    const size_t st = s->stride;
    uint32_t j;

    /* Each element loaded into registers and stored once: a partial store
     * read back whole stalls the store forwarding. */
    switch (s->kind) {
    case VIN_F1:
        for (j = 0; j < n; j++, p += st, t += ts) {
            float x;
            memcpy(&x, p, 4);
            t[0] = x; t[1] = 0.0f; t[2] = 0.0f; t[3] = 1.0f;
        }
        break;
    case VIN_F2:
        for (j = 0; j < n; j++, p += st, t += ts) {
            float x, y;
            memcpy(&x, p, 4); memcpy(&y, p + 4, 4);
            t[0] = x; t[1] = y; t[2] = 0.0f; t[3] = 1.0f;
        }
        break;
    case VIN_F3:
        for (j = 0; j < n; j++, p += st, t += ts) {
            float x, y, z;
            memcpy(&x, p, 4); memcpy(&y, p + 4, 4); memcpy(&z, p + 8, 4);
            t[0] = x; t[1] = y; t[2] = z; t[3] = 1.0f;
        }
        break;
    case VIN_F4:
        for (j = 0; j < n; j++, p += st, t += ts)
            memcpy(t, p, 16);
        break;
    case VIN_COLOR:
        /* 0xAARRGGBB: bytes B, G, R, A, as R, G, B, A. */
        for (j = 0; j < n; j++, p += st, t += ts) {
            t[0] = nv2a_vtx_unorm8[p[2]];
            t[1] = nv2a_vtx_unorm8[p[1]];
            t[2] = nv2a_vtx_unorm8[p[0]];
            t[3] = nv2a_vtx_unorm8[p[3]];
        }
        break;
    case VIN_CMP:
        for (j = 0; j < n; j++, p += st, t += ts) {
            uint32_t w;
            memcpy(&w, p, 4);
            t[0] = nv2a_vtx_cmp11[w & 0x7FFu];
            t[1] = nv2a_vtx_cmp11[(w >> 11) & 0x7FFu];
            t[2] = nv2a_vtx_cmp10[w >> 22];
            t[3] = 1.0f;
        }
        break;
    case VIN_S1: {
        const uint32_t c = s->size < 4 ? s->size : 4u;
        for (j = 0; j < n; j++, p += st, t += ts) {
            float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            uint32_t q;
            for (q = 0; q < c; q++) {
                int16_t h;
                memcpy(&h, p + 2u * q, 2);
                v[q] = nv2a_vtx_snorm16(h);
            }
            memcpy(t, v, 16);
        }
        break;
    }
    case VIN_DECODE:
        for (j = 0; j < n; j++, p += st, t += ts)
            nv2a_vtx_decode(s->type, s->size, p, t);
        break;
    default:
        for (j = 0; j < n; j++, t += ts)
            memcpy(t, s->dflt, 16);
        break;
    }
}

void nv2a_vtx_gather(const struct nv2a_pb_draw *d, const int *slot, int nf4,
                     uint32_t lo, uint32_t count, const float (*inl)[4],
                     const uint8_t *mem, float *out)
{
    struct vin s[NV_VERTEX_ATTRS];
    float tile[VTILE_FLOATS];
    const int inl_data = d->inline_data;
    uint32_t i0, nt;
    size_t ts;
    int k;

    if (s_vrec)
        vtx_record(d, slot, nf4, lo, count, inl, mem);
    if (nf4 > NV_VERTEX_ATTRS)
        nf4 = NV_VERTEX_ATTRS;
    if (nf4 <= 0)
        return;
    ts = (size_t)nf4 * 4u;
    nt = VTILE_FLOATS / (uint32_t)ts;
    for (k = 0; k < nf4; k++) {
        const VertexAttr *a = &d->attr[slot[k]];
        uint64_t at = (uint64_t)a->offset + (uint64_t)lo * a->stride;
        s[k].dflt = inl[slot[k]];
        s[k].stride = a->stride;
        s[k].type = a->type;
        s[k].size = a->size;
        s[k].kind = VIN_DFLT;
        s[k].live = count;
        s[k].p = NULL;
        if (!a->size || !a->stride)
            continue;
        if (inl_data) {
            /* The payload's elements: vertex i's at at + i * stride is read
             * while it starts 4 bytes or more before the payload's end. */
            if (at + 4u > d->inline_bytes)
                continue;
            if ((d->inline_bytes - at - 4u) / a->stride + 1u < count)
                s[k].live = (uint32_t)((d->inline_bytes - at - 4u) / a->stride + 1u);
            s[k].p = (const uint8_t *)d->inline_buf + at;
        } else if (a->offset) {
            s[k].p = mem + at;
        } else {
            continue;
        }
        switch (a->type) {
        case NV2A_VTX_FLOAT:
            s[k].kind = a->size == 1 ? VIN_F1 : a->size == 2 ? VIN_F2
                      : a->size == 3 ? VIN_F3 : VIN_F4;
            break;
        case NV2A_VTX_D3DCOLOR: s[k].kind = VIN_COLOR;  break;
        case NV2A_VTX_CMP:      s[k].kind = VIN_CMP;    break;
        case NV2A_VTX_S1:       s[k].kind = VIN_S1;     break;
        case NV2A_VTX_UBYTE:
        case NV2A_VTX_S32K:     s[k].kind = VIN_DECODE; break;
        default:                s[k].kind = VIN_DFLT;   break;  /* no decoder */
        }
    }
    for (i0 = 0; i0 < count; i0 += nt) {
        uint32_t n = count - i0 < nt ? count - i0 : nt;
        for (k = 0; k < nf4; k++) {
            /* An input past an inline payload's end gives its dflt there. */
            uint32_t m = s[k].live <= i0 ? 0 : s[k].live - i0 < n ? s[k].live - i0 : n;
            struct vin rest = s[k];
            vin_run(&s[k], i0, m, tile + 4 * k, ts);
            rest.kind = VIN_DFLT;
            vin_run(&rest, i0 + m, n - m, tile + 4 * k + m * ts, ts);
        }
        memcpy(out, tile, (size_t)n * ts * sizeof(float));
        out += (size_t)n * ts;
    }
}

uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint64_t l[4] = { h, h ^ 1u, h ^ 2u, h ^ 3u };
    uint32_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        l[0] = (l[0] ^ w[0]) * 0x100000001B3ull;
        l[1] = (l[1] ^ w[1]) * 0x100000001B3ull;
        l[2] = (l[2] ^ w[2]) * 0x100000001B3ull;
        l[3] = (l[3] ^ w[3]) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        l[0] = (l[0] ^ p[i]) * 0x100000001B3ull;
    return (l[0] ^ (l[1] << 1) ^ (l[2] << 2) ^ (l[3] << 3)) * 0x100000001B3ull;
}

static _Atomic uint64_t s_rt_hashes;

/* RECOMP_TRACE=pb_split (nv2a_pb_split.h): set by nv2a_pb_exec.c's writer;
 * here so a test that links the shared code alone still links. */
struct nv2a_split *nv2a_split;

static uint64_t rt_own_hash(const uint8_t *mem, uint32_t addr, uint32_t pitch, uint32_t h)
{
    /* The end of the window addr lies in; an address in neither (a test's
     * own surface) is read as the seed and a write-back would read it. */
    uint64_t top = addr >= NV2A_RT_CONTIG_BASE
                       ? (uint64_t)NV2A_RT_CONTIG_BASE + NV2A_RT_CONTIG_SIZE
                 : addr < NV2A_RT_WINDOW ? NV2A_RT_WINDOW : 0x100000000ull;
    uint64_t n = (uint64_t)pitch * h, r, t0;
    struct nv2a_split *sp = nv2a_split;   /* RECOMP_TRACE=pb_split (BO-282.12) */
    if (n > top - addr)
        n = top - addr;
    atomic_fetch_add_explicit(&s_rt_hashes, 1, memory_order_relaxed);
    if (!sp)
        return nv2a_tex_hash(mem + addr, (uint32_t)n, NV2A_TEX_HASH_SEED);
    t0 = nv2a_split_tick();
    r = nv2a_tex_hash(mem + addr, (uint32_t)n, NV2A_TEX_HASH_SEED);
    sp->own_ticks += nv2a_split_tick() - t0;
    sp->own_hash++;
    sp->own_kb += (uint32_t)(n >> 10);
    return r;
}

void nv2a_rt_own_reset(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                       uint32_t pitch, uint32_t h, uint32_t flip)
{
    o->mem_hash = rt_own_hash(mem, addr, pitch, h);
    /* Knowledge of this instant, not a check for the flip: a title write
     * later in the same flip (a CPU fill before its next draw) must still
     * be seen by the next check. */
    o->checked_flip = flip - 1u;
}

int nv2a_rt_own_stale(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                      uint32_t pitch, uint32_t h, uint32_t flip)
{
    if (o->draw_flip == flip || o->checked_flip == flip)
        return 0;
    o->checked_flip = flip;
    return rt_own_hash(mem, addr, pitch, h) != o->mem_hash;
}

uint64_t nv2a_rt_own_hashes(void)
{
    return atomic_load_explicit(&s_rt_hashes, memory_order_relaxed);
}

size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++)
        n += (size_t)nv2a_tex_level_dim(w, l) * nv2a_tex_level_dim(h, l);
    return n;
}

uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, bb = d3d8_format_dxt_block_bytes(fmt);
    if (bb) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, w, &argb);
                out[(size_t)y * w + x] = argb;
            }
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb;
    } else {
        uint32_t mx, my, lin = nv2a_tex_linear_twin(fmt);
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                nv2a_tex_decode_texel(lin, mem, swizzle_deposit(x, mx) | oy,
                                      pal, pal_len, &argb);
                out[(size_t)y * w + x] = argb;
            }
        }
        return w * h * nv2a_tex_texel_bytes(lin);
    }
}

void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, l;
    if (nv2a_tex_size_from_format(fmt)) {
        for (l = 0; l < levels; l++) {
            uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
            mem += nv2a_tex_decode_level(mem, fmt, lw, lh, pal, pal_len, out);
            out += (size_t)lw * lh;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = mem + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t argb = 0;
            nv2a_tex_decode_texel(fmt, row, x, pal, pal_len, &argb);
            out[(size_t)y * w + x] = argb;
        }
    }
}

void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t)
{
    uint32_t fmt = r[0x04 / 4], ctl0 = r[0x0C / 4];
    int on = (ctl0 >> 30) & 1, mode, from_fmt;

    memset(t, 0, sizeof *t);
    t->offset = r[0] ? resolve(r[0]) : 0;
    t->color = (fmt >> 8) & 0xFF;
    from_fmt = nv2a_tex_size_from_format(t->color);
    if (from_fmt) {
        t->width  = 1u << ((fmt >> 20) & 0xF);
        t->height = 1u << ((fmt >> 24) & 0xF);
    } else {
        t->width  = r[0x1C / 4] >> 16;
        t->height = r[0x1C / 4] & 0xFFFF;
    }
    /* CONTROL1's pitch means nothing to a swizzled or DXT texture, and
     * left in a cache key a stale one splits it in two. */
    t->pitch  = (from_fmt && (flags & NV2A_STAGE_PITCH_LINEAR))
              ? 0 : r[0x10 / 4] >> 16;
    t->addr_u =  r[0x08 / 4]       & 0xF;
    t->addr_v = (r[0x08 / 4] >> 8) & 0xF;
    t->filter = r[0x14 / 4];
    t->cube = (int)((fmt >> 2) & 1u);
    t->dims = (fmt >> 4) & 0xFu;
    t->fmt_levels = (fmt >> 16) & 0xFu;
    if (!t->fmt_levels)
        t->fmt_levels = 1;
    /* P8: SET_TEXTURE_PALETTE (0x1B20). */
    if (t->color == NV2A_TEX_P8)
        nv2a_tex_palette_decode(r[0x20 / 4], resolve, &t->palette, &t->pal_len);
    t->levels = 1;
    if (from_fmt && (flags & NV2A_STAGE_MIPS)) {
        /* As xemu's pgraph_get_texture_shape: the format's count, no more
         * than MAX_LOD_CLAMP + 1, none smaller than 1x1. The clamps are
         * 4.8 fixed point, as the hardware keeps every LOD: D3D's "no
         * clamp" is MAX 0xFFF (15.99) and MIN 0, and a title that skips
         * its top levels (D3DTSS_MAXMIPLEVEL) writes MIN 0x100, 0x200,
         * 0x400 for 1, 2, 4. Read as whole levels those pinned the texture
         * to its last level: Double-S.T.E.A.L 2's distant tree canopies
         * (alpha-tested, a few texels at 4x4) were gone. Whole levels here;
         * the fraction is dropped. */
        uint32_t lu = (fmt >> 20) & 0xF, lv = (fmt >> 24) & 0xF;
        uint32_t lmax = ((ctl0 >> 6) & 0xFFF) >> 8, lmin = ((ctl0 >> 18) & 0xFFF) >> 8;
        uint32_t lv_n = (fmt >> 16) & 0xF;
        if (lv_n > lmax + 1u) lv_n = lmax + 1u;
        if (lv_n > (lu > lv ? lu : lv) + 1u) lv_n = (lu > lv ? lu : lv) + 1u;
        t->levels = lv_n ? lv_n : 1u;
        t->lod_min = lmin < t->levels - 1u ? lmin : t->levels - 1u;
        t->lod_max = lmax < t->levels - 1u ? lmax : t->levels - 1u;
    }
    t->valid = t->offset && t->width && t->height
            && (from_fmt || t->pitch)
            && (!(flags & NV2A_STAGE_P8_PALETTE)
                || t->color != NV2A_TEX_P8 || t->palette);
    if (n == 0 && !set[0x0C / 4])
        on = 1;                         /* never configured: counts as on */
    if (shader_set)
        mode = (int)((shader_prog >> (5 * n)) & 0x1F);
    else
        mode = n == 0 ? 1 : 0;
    t->raw_mode = mode;
    t->on = on;
    t->alpha_kill = (int)((ctl0 >> 2) & 1u);
    /* PASSTHRU and CLIPPLANE have no texture to be valid: they stay. The
     * GPU backends drop CLIPPLANE to 0 themselves (no per-pixel kill yet). */
    if (mode != 4 && mode != 5 && (!on || !t->valid))
        mode = 0;
    t->mode = mode;
}

uint32_t nv2a_alpha_kill_mask(const struct nv2a_stage *s, uint32_t flips)
{
    static int on = -1;
    static uint32_t ab_n;
    uint32_t m = 0;
    int n;
    if (on < 0) {
        const char *e = recomp_env(RENV_ALPHA_KILL);
        on = !(e && e[0] == '0');
        if (e && !strncmp(e, "ab", 2)) {   /* =ab:N: off in even N-flip windows, for paused A/B */
            on = 2;
            ab_n = (e[2] == ':' && atoi(e + 3) > 0) ? (uint32_t)atoi(e + 3) : 1u;
        }
    }
    if (!on || (on == 2 && !((flips / ab_n) & 1u)))
        return 0;
    for (n = 0; n < 4; n++)
        if (s[n].alpha_kill && s[n].mode && s[n].mode != 4 && s[n].mode != 5)
            m |= 1u << n;
    return m;
}

uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo)
{
    uint32_t k = 0, i;

#define PUT(x) (out[k++] = (uint32_t)(x) - base)
    switch (prim) {
    case NV_PRIM_POINTS:
        *topo = NV2A_TOPO_POINTS;
        for (i = 0; i < n; i++) PUT(v[i]);
        break;
    case NV_PRIM_LINES:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) { PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i++) { PUT(v[i]); PUT(v[i + 1]); }
        if (prim == NV_PRIM_LINE_LOOP && n > 2) { PUT(v[n - 1]); PUT(v[0]); }
        break;
    case NV_PRIM_TRIANGLES:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i += 3) { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) { PUT(v[i + 1]); PUT(v[i]); PUT(v[i + 2]); }
            else       { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 1; i + 1 < n; i++) { PUT(v[0]); PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_QUADS:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 4) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]);
            PUT(v[i]); PUT(v[i + 2]); PUT(v[i + 3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 2) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 3]);
            PUT(v[i]); PUT(v[i + 3]); PUT(v[i + 2]);
        }
        break;
    default:
        *topo = NV2A_TOPO_NONE;
        return 0;
    }
#undef PUT
    return k;
}

uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len)
{
    uint32_t h = 2166136261u, n = 0, pc, k;
    for (pc = v->start; pc < NV2A_VSH_SLOTS; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ v->prog[pc][k]) * 16777619u;
        n++;
        if (v->prog[pc][3] & 1u)        /* FINAL */
            break;
    }
    if (len)
        *len = n;
    return h;
}

int nv2a_blend_constant(int enable, uint32_t sfactor, uint32_t dfactor,
                        uint32_t color, float bf[4], int *mixed)
{
    uint32_t s = nv2a_blend_from_gl(sfactor), d = nv2a_blend_from_gl(dfactor);
    int col = s == NV2A_BF_CONST_COLOR || s == NV2A_BF_INV_CONST_COLOR
           || d == NV2A_BF_CONST_COLOR || d == NV2A_BF_INV_CONST_COLOR;
    int alp = s == NV2A_BF_CONST_ALPHA || s == NV2A_BF_INV_CONST_ALPHA
           || d == NV2A_BF_CONST_ALPHA || d == NV2A_BF_INV_CONST_ALPHA;

    *mixed = 0;
    if (!enable || (!col && !alp)) {
        bf[0] = bf[1] = bf[2] = bf[3] = 1.0f;
        return 0;
    }
    bf[3] = (float)(color >> 24) / 255.0f;
    *mixed = col && alp;
    if (col) {
        bf[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((color >>  8) & 0xFF) / 255.0f;
        bf[2] = (float)( color        & 0xFF) / 255.0f;
    } else {
        bf[0] = bf[1] = bf[2] = bf[3];
    }
    return 1;
}

void nv2a_ps_consts_fill(const struct nv2a_pb_vsh *v,
                         const struct nv2a_stage st[4],
                         struct nv2a_ps_consts *pc)
{
    int n;

    memset(pc, 0, sizeof *pc);
    for (n = 0; n < 4; n++) {
        const struct nv2a_stage *t = &st[n];
        int scaled = nv2a_tex_size_from_format(t->color);
        pc->tex_mode[n] = (uint32_t)t->mode;
        pc->tex_scale[n][0] = scaled || !t->width ? 1.0f : 1.0f / (float)t->width;
        pc->tex_scale[n][1] = scaled || !t->height ? 1.0f : 1.0f / (float)t->height;
    }
    for (n = 0; n < 8; n++) {
        nv2a_argb_to_float4(v->rc_f0[n], pc->c0[n]);
        nv2a_argb_to_float4(v->rc_f1[n], pc->c1[n]);
    }
    nv2a_argb_to_float4(v->rc_sf[0], pc->fc0);
    nv2a_argb_to_float4(v->rc_sf[1], pc->fc1);
    nv2a_argb_to_float4(v->fog_color, pc->fog_color);
    {   /* the CPU path reads the fog colour register as RGBA, alpha 1 */
        float t = pc->fog_color[0];
        pc->fog_color[0] = pc->fog_color[2];
        pc->fog_color[2] = t;
        pc->fog_color[3] = 1.0f;
    }
    pc->alpha_ref = (float)v->atest_ref / 255.0f;
    pc->alpha_test_enable = v->atest_enable ? 1u : 0u;
    pc->alpha_func = nv2a_cmp_from_gl(v->atest_func) + 1;
}

void nv2a_vp_consts_fill(const struct nv2a_pb_vsh *v, uint32_t surface_format,
                         uint32_t w, uint32_t h, struct nv2a_vp_consts *o)
{
    uint32_t func, abs_, edge_one;

    o->c[0] = 2.0f / (float)w;
    o->c[1] = -2.0f / (float)h;
    o->c[2] = 1.0f / nv2a_zmax(surface_format);
    /* Polygon offset's constant part (SET_POLYGON_OFFSET_BIAS, depth units)
     * as normalised depth, added to every vertex's z by the GPU backends
     * that read it (Metal); xemu adds it to z in the same units
     * (pgraph/glsl/psh.c depthOffset). The slope part is the backend's. */
    o->c[3] = v->poly_offset ? v->poly_bias / nv2a_zmax(surface_format) : 0.0f;
    o->c[4] = -1.0f;
    o->c[5] = 1.0f;
    /* BO-332: SET_CLIP_MIN / SET_CLIP_MAX as normalised depth (window z
     * over zmax), [0, 1] until the title sets them; u[6] 1 when
     * SET_ZMIN_MAX_CONTROL's ZCLAMP_EN (bits 7:4) is CULL, as xemu's
     * pgraph_glsl_set_clip_range_uniform_value and psh.c depth_clipping.
     * Read by the GLSL vertex program with D3D8_GLSL_CLIP_RANGE. */
    if (v->clip_set) {
        float lo = v->clip_min, hi = v->clip_max;
        o->c[6] = lo / nv2a_zmax(surface_format);
        o->c[7] = hi / nv2a_zmax(surface_format);
    } else {
        o->c[6] = 0.0f;
        o->c[7] = 1.0f;
    }
    /* Fog and specular, as the CPU path's vsh_fog_factor and vsh_transform
     * apply them (xemu vsh.c). */
    o->c[8] = v->fog_param[0];
    o->c[9] = v->fog_param[1];
    o->c[10] = v->fog_param[2];
    o->c[11] = 0.0f;
    memset(o->u, 0, sizeof o->u);
    if (nv2a_fog_mode(v->fog_mode, &func, &abs_, &edge_one)) {
        o->u[1] = func;
        o->u[2] = abs_;
        o->u[3] = edge_one;
        o->u[0] = v->fog_enable ? 1u : 0u;
    }
    o->u[4] = v->spec_enable ? 1u : 0u;
    o->u[5] = (v->light_ctl >> 17) & 1u;
    o->u[6] = ((v->zminmax >> 4) & 0xFu) == 0u ? 1u : 0u;   /* BO-332: ZCLAMP_EN CULL */
    /* W-buffering (SET_CONTROL0's Z_PERSPECTIVE_ENABLE): the depth stored is
     * w over the clip range, not z (xemu: pgraph vsh, z_perspective); u[7],
     * read as spec_ctl.w. */
    if ((v->control0 & (1u << 16)) && v->clip_max > 0.0f) {
        o->u[7] = 1u;
        o->c[11] = 1.0f / v->clip_max;
    }
}

void nv2a_report_write(uint8_t *mem, uint32_t va, uint32_t count)
{
    volatile uint32_t *r = (volatile uint32_t *)(mem + va);
    r[0] = 0;                              /* timestamp */
    r[1] = 0;
    r[2] = count;
    /* status: done, as a release store, so the count (and the rest of the
     * report) is visible before it on any host, arm64 included. */
    atomic_store_explicit((_Atomic uint32_t *)(mem + va + 12), 0u,
                          memory_order_release);
}

void nv2a_report_set_pending(uint8_t *mem, uint32_t va)
{
    ((volatile uint32_t *)(mem + va))[3] = 0xFFFFFFFFu;
}

void nv2a_occ_init(struct nv2a_occ *o, const struct nv2a_occ_ops *ops,
                   uint8_t *mem)
{
    memset(o, 0, sizeof *o);
    o->ops = ops;
    o->mem = mem;
    o->scale = nv2a_host_render_scale();
}

void nv2a_occ_set_scale(struct nv2a_occ *o, unsigned scale)
{
    o->scale = scale;
    o->scale_x2 = 0;
}

void nv2a_occ_set_scale_x2(struct nv2a_occ *o, unsigned x2)
{
    o->scale_x2 = x2 > 2 ? x2 : 0;
    if (!o->scale_x2)
        o->scale = 1;
}

uint32_t nv2a_occ_scale_count_x2(uint64_t samples, unsigned x2)
{
    if (x2 > 2 && samples) {
        uint64_t d = (uint64_t)x2 * x2;         /* the area factor, times 4 */
        samples = (samples * 4u + d / 2) / d;
        if (!samples)
            samples = 1;
    }
    return samples > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)samples;
}

void nv2a_resample_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t sw, uint32_t sh,
                             uint8_t *dst, uint32_t dst_pitch, uint32_t dw, uint32_t dh)
{
    uint32_t x, y;
    for (y = 0; y < dh; y++) {
        const uint32_t *in = (const uint32_t *)(const void *)
            (src + (size_t)(((uint64_t)y * 2u + 1u) * sh / (2u * dh)) * src_pitch);
        uint32_t *out = (uint32_t *)(void *)(dst + (size_t)y * dst_pitch);
        for (x = 0; x < dw; x++)
            out[x] = in[((uint64_t)x * 2u + 1u) * sw / (2u * dw)];
    }
}

uint32_t nv2a_occ_scale_count(uint64_t samples, unsigned scale)
{
    if (scale > 1 && samples) {
        uint64_t d = (uint64_t)scale * scale;
        /* Rounded to nearest; a few samples of a sliver still mean
         * "visible", so a non-zero count never rounds to 0. */
        samples = (samples + d / 2) / d;
        if (!samples)
            samples = 1;
    }
    return samples > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)samples;
}

static void occ_begin(struct nv2a_occ *o)
{
    void *q;
    if (o->open)
        return;
    if (o->nseg >= NV2A_OCC_SEG || !(q = o->ops->get(o->ops->ctx))) {
        o->ovf = 1;
        return;
    }
    o->ops->begin(o->ops->ctx, q);
    o->seg[o->nseg++] = q;
    o->open = 1;
}

static void occ_end(struct nv2a_occ *o)
{
    if (!o->open)
        return;
    o->ops->end(o->ops->ctx, o->seg[o->nseg - 1]);
    o->open = 0;
}

/* 1 when every query of the report has its result (summed into *count). */
static int occ_ready(struct nv2a_occ *o, const struct nv2a_occ_report *p,
                     int flush, uint32_t *count)
{
    uint64_t sum = 0;
    int i;
    for (i = 0; i < p->n; i++) {
        uint64_t v = 0;
        if (!o->ops->result(o->ops->ctx, p->q[i], flush, &v))
            return 0;
        sum += v;
    }
    *count = o->scale_x2 ? nv2a_occ_scale_count_x2(sum, o->scale_x2)
                         : nv2a_occ_scale_count(sum, o->scale);
    return 1;
}

static void occ_finish(struct nv2a_occ *o, struct nv2a_occ_report *p,
                       uint32_t count)
{
    int i;
    nv2a_report_write(o->mem, p->va, count);
    o->vis += count != 0;
    for (i = 0; i < p->n; i++)
        o->ops->put(o->ops->ctx, p->q[i]);
    p->n = 0;
}

void nv2a_occ_poll(struct nv2a_occ *o, int flush)
{
    uint64_t now;
    int i, j;
    if (!o->npend)
        return;
    now = o->ops->now_us(o->ops->ctx);
    for (i = j = 0; i < o->npend; i++) {
        struct nv2a_occ_report *p = &o->pend[i];
        uint32_t count;
        if (p->overflow) {
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else if (occ_ready(o, p, flush, &count)) {
            occ_finish(o, p, count);
        } else if (now - p->t0 > NV2A_OCC_LATE_US) {
            o->late++;
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else {
            o->pend[j++] = *p;
        }
    }
    o->npend = j;
}

void nv2a_occ_zpass(struct nv2a_occ *o, int enable)
{
    o->en = enable;
    if (enable)
        occ_begin(o);
    else
        occ_end(o);
}

void nv2a_occ_zpass_clear(struct nv2a_occ *o)
{
    int i;
    occ_end(o);
    for (i = 0; i < o->nseg; i++)
        o->ops->put(o->ops->ctx, o->seg[i]);   /* ended; Begin on reuse drops it */
    o->nseg = 0;
    o->ovf = 0;
    if (o->en)
        occ_begin(o);
}

void nv2a_occ_report(struct nv2a_occ *o, uint32_t va, int sync)
{
    struct nv2a_occ_report *p;
    int i;

    occ_end(o);
    /* Supersede a report still pending at this va before any poll: a poll
     * here could finish the old one over the title's reset. */
    for (i = 0; i < o->npend; i++)
        if (o->pend[i].va == va) {
            int k;
            for (k = 0; k < o->pend[i].n; k++)
                o->ops->put(o->ops->ctx, o->pend[i].q[k]);
            memmove(&o->pend[i], &o->pend[i + 1],
                    (size_t)(o->npend - i - 1) * sizeof o->pend[0]);
            o->npend--;
            o->super++;
            break;              /* at most one: each push supersedes */
        }
    nv2a_occ_poll(o, 0);
    if (o->npend >= NV2A_OCC_PENDING)
        nv2a_occ_poll(o, 1);
    if (o->npend >= NV2A_OCC_PENDING) {
        /* Still full: the oldest is answered as visible. */
        o->late++;
        occ_finish(o, &o->pend[0], NV2A_OCC_VISIBLE);
        memmove(&o->pend[0], &o->pend[1],
                (size_t)(--o->npend) * sizeof o->pend[0]);
    }
    /* Pending until done, whatever the title left in the status. */
    nv2a_report_set_pending(o->mem, va);
    p = &o->pend[o->npend++];
    p->va = va;
    p->n = o->nseg;
    p->overflow = o->ovf;
    p->t0 = o->ops->now_us(o->ops->ctx);
    for (i = 0; i < o->nseg; i++)
        p->q[i] = o->seg[i];
    /* The report owns them now; a report with no clear before the next one
     * counts from here (the XDK always clears first). */
    o->nseg = 0;
    o->ovf = 0;
    o->reports++;
    if (sync) {
        uint64_t t = o->ops->now_us(o->ops->ctx);
        uint32_t count;
        while (!occ_ready(o, p, 1, &count)
               && o->ops->now_us(o->ops->ctx) - t < NV2A_OCC_LATE_US)
            ;
        o->sync_us += o->ops->now_us(o->ops->ctx) - t;
        nv2a_occ_poll(o, 1);
    }
    if (o->en)
        occ_begin(o);
}

/* ---- Host options (enhancements) ------------------------------------------ */

static struct nv2a_host_opts s_host_opts;   /* zero: stock */

void nv2a_host_opts_set(const struct nv2a_host_opts *o)
{
    s_host_opts = *o;
}

const struct nv2a_host_opts *nv2a_host_opts(void)
{
    return &s_host_opts;
}

static volatile int s_aniso = -1;     /* -1: not read yet */

static int aniso_level(int n)
{
    return n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
}

int nv2a_host_aniso(void)
{
    if (s_aniso < 0) {
        const char *e = recomp_env(RENV_GFX_ANISO);
        s_aniso = e && *e ? aniso_level(atoi(e)) : 1;   /* "off" reads 0 */
    }
    return s_aniso;
}

void nv2a_host_set_aniso(int level)
{
    s_aniso = aniso_level(level);
}

static volatile int s_post_aa = -1;          /* -1: not read yet */
static volatile float s_sharpen = -1.0f;

int nv2a_host_post_aa(void)
{
    if (s_post_aa < 0) {
        const char *e = recomp_env(RENV_GFX_AA);
        s_post_aa = e && !strcmp(e, "fxaa") ? NV2A_AA_FXAA
                  : e && !strcmp(e, "smaa") ? NV2A_AA_SMAA : NV2A_AA_OFF;
    }
    return s_post_aa;
}

void nv2a_host_set_post_aa(int aa)
{
    s_post_aa = aa == NV2A_AA_FXAA || aa == NV2A_AA_SMAA ? aa : NV2A_AA_OFF;
}

float nv2a_host_sharpen(void)
{
    if (s_sharpen < 0.0f) {
        const char *e = recomp_env(RENV_GFX_SHARPEN);
        float v = e && *e ? (float)atof(e) : 0.0f;
        s_sharpen = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
    return s_sharpen;
}

void nv2a_host_set_sharpen(float strength)
{
    s_sharpen = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
}

static volatile int s_lod_mode = -1;        /* -1: not read yet */
static volatile float s_lod_value;

static void lod_read(void)
{
    const char *e = recomp_env(RENV_GFX_LOD_BIAS);
    s_lod_value = 0.0f;
    if (e && !strcmp(e, "auto")) {
        s_lod_mode = NV2A_LOD_AUTO;
    } else if (e && *e && atof(e) != 0.0) {
        float v = (float)atof(e);
        s_lod_value = v < -4.0f ? -4.0f : v > 4.0f ? 4.0f : v;
        s_lod_mode = NV2A_LOD_VALUE;
    } else {
        s_lod_mode = NV2A_LOD_OFF;
    }
}

float nv2a_host_lod_bias(void)
{
    if (s_lod_mode < 0)
        lod_read();
    if (s_lod_mode == NV2A_LOD_AUTO) {
        /* x2 is the scale in halves (1.5x is 3), so 2 / x2 is 1 / scale. */
        unsigned x2 = nv2a_host_render_scale_x2();
        return x2 > 2 ? -0.5f * log2f((float)x2 * 0.5f) : 0.0f;
    }
    return s_lod_mode == NV2A_LOD_VALUE ? s_lod_value : 0.0f;
}

void nv2a_host_set_lod_bias(int mode, float value)
{
    s_lod_value = value < -4.0f ? -4.0f : value > 4.0f ? 4.0f : value;
    s_lod_mode = mode == NV2A_LOD_AUTO ? NV2A_LOD_AUTO
               : mode == NV2A_LOD_VALUE ? NV2A_LOD_VALUE : NV2A_LOD_OFF;
}

/* ---- HDR (BO-110) ---- */
static volatile int   s_hdr_on = -1;          /* -1: not read yet */
static volatile float s_hdr_peak = -1.0f, s_hdr_paper = -1.0f, s_hdr_hud = -1.0f;
static volatile float s_hdr_pot, s_hdr_now;
/* BO-227: the headroom the levels use. macOS's current headroom reads 1.0
 * until the EDR layer is on screen, then what macOS grants; the levels
 * took it raw every present, so the peak went 1000 / 508 / 1000 (the
 * "Display" value, the shoulder, the patch with it). Now: the granted one,
 * held (nv2a_host_hdr_display). */
static volatile float s_hdr_cap;
static volatile float s_hdr_edid;      /* BO-244: the display's EDID peak, nits (macOS) */
void nv2a_host_hdr_display_peak(float edid_nits) { s_hdr_edid = edid_nits; }
static void *(*s_hdr_patch_fn)(float);

static float env_f(recomp_env_id id, float def)
{
    const char *e = recomp_env(id);
    return e && *e ? (float)atof(e) : def;
}

static int hdr_debug(void)
{
    const char *e = recomp_env(RENV_HDR);
    return e && *e && strcmp(e, "0") != 0;
}

int nv2a_host_hdr(void)
{
    if (s_hdr_on < 0) {
        const char *e = recomp_env(RENV_GFX_HDR);
        s_hdr_on = (e && *e && strcmp(e, "0") && strcmp(e, "off")) || hdr_debug() ? 1 : 0;
    }
    return s_hdr_on;
}

int nv2a_host_hdr_active(void)
{
    static int dbg = -1;
    if (dbg < 0)
        dbg = hdr_debug();
    return nv2a_host_hdr() && (dbg || s_hdr_pot > 1.0f);
}

void nv2a_host_set_hdr(int on) { s_hdr_on = on ? 1 : 0; }

void nv2a_host_set_hdr_levels(int which, float peak, float paper, float hud)
{
    (void)which;
    if (peak >= 0.0f) s_hdr_peak = peak;
    if (paper >= 0.0f) s_hdr_paper = paper;
    if (hud >= 0.0f) s_hdr_hud = hud;
}

void nv2a_host_hdr_levels(float *peak_nits, float *paper_nits, float *hud_nits)
{
    float pk, pw, hw;
    if (s_hdr_peak < 0.0f) s_hdr_peak = env_f(RENV_GFX_HDR_PEAK, 0.0f);
    if (s_hdr_paper < 0.0f) s_hdr_paper = env_f(RENV_GFX_HDR_PAPER, (float)NV2A_HDR_PAPER_DEFAULT);
    if (s_hdr_hud < 0.0f) s_hdr_hud = env_f(RENV_GFX_HDR_HUD, 0.0f);
    pw = s_hdr_paper >= 10.0f ? s_hdr_paper : (float)NV2A_HDR_PAPER_DEFAULT;
    hw = s_hdr_hud >= 10.0f ? s_hdr_hud : pw;
    pk = s_hdr_peak;
    if (pk < 10.0f) {
        /* The display's: on Vulkan the peak its EDID reports / 100
         * (fb_present_vulkan.c); on macOS (BO-244) the potential EDR
         * headroom x 100 nits = the panel's peak (fb_present_metal.m's
         * metadata puts the content's nits on the panel as they are,
         * whatever the brightness slider), else its EDID's peak; with no
         * display known (headless) 1000. A peak set by the player is taken
         * as it is, as on Vulkan. */
        pk = s_hdr_pot > 1.0f ? s_hdr_pot * 100.0f : 1000.0f;
#if defined(__APPLE__)
        if (s_hdr_pot <= 1.0f && s_hdr_edid >= 100.0f)
            pk = s_hdr_edid;
        {
            static float said;
            const char *src = s_hdr_pot > 1.0f ? "the potential EDR headroom x 100 (the panel's peak)"
                            : s_hdr_edid >= 100.0f ? "the EDID" : "none known";
            if (pk != said) {
                said = pk;
                fprintf(stderr, "[HDR] default peak %.0f nits from %s (EDID %.1f, potential %.0f, granted %.3fx:"
                        " SDR white %.0f nits)\n", pk, src, s_hdr_edid, s_hdr_pot * 100.0f, s_hdr_cap,
                        s_hdr_cap > 1.0f && s_hdr_pot > s_hdr_cap ? s_hdr_pot / s_hdr_cap * 100.0f : 100.0f);
            }
        }
#endif
    }
    if (pk < pw) pk = pw;
    if (pk < hw) pk = hw;
    *peak_nits = pk;
    *paper_nits = pw;
    *hud_nits = hw;
}

/* B1/B2/B5 of the HDR review: the decode curve, the shadows' power below
 * mid grey, the gain on light past paper white. */
static volatile float s_hdr_shadows = -1.0f, s_hdr_hl = -1.0f;
static volatile int   s_hdr_dec = -1;

void nv2a_host_set_hdr_tone(int which, float v)
{
    if (which == 0) s_hdr_shadows = v;
    else if (which == 1) s_hdr_hl = v;
    else s_hdr_dec = v != 0.0f;
}

void nv2a_host_hdr_tone(float *shadows, float *highlights, int *srgb)
{
    if (s_hdr_shadows < 0.0f) s_hdr_shadows = env_f(RENV_GFX_HDR_SHADOWS, 1.0f);
    if (s_hdr_hl < 0.0f) s_hdr_hl = env_f(RENV_GFX_HDR_HIGHLIGHTS, 1.0f);
    if (s_hdr_dec < 0) {
        const char *e = recomp_env(RENV_GFX_HDR_DECODE);
        s_hdr_dec = e && !strcmp(e, "srgb");
    }
    *shadows = s_hdr_shadows < 0.5f ? 0.5f : s_hdr_shadows > 2.0f ? 2.0f : s_hdr_shadows;
    *highlights = s_hdr_hl < 0.25f ? 0.25f : s_hdr_hl > 4.0f ? 4.0f : s_hdr_hl;
    *srgb = s_hdr_dec;
}

void nv2a_host_hdr_shape(float *shoulder, float *black_nits)
{
    static float sh = -1.0f, bl = -1.0f;
    if (sh < 0.0f) {
        sh = env_f(RENV_GFX_HDR_SHOULDER, 0.35f);
        sh = sh < 0.0f ? 0.0f : sh > 1.0f ? 1.0f : sh;
        bl = env_f(RENV_GFX_HDR_BLACK, 0.0f);
        bl = bl < 0.0f ? 0.0f : bl > 5.0f ? 5.0f : bl;
    }
    *shoulder = sh;
    *black_nits = bl;
}

void nv2a_host_hdr_display(float potential, float now)
{
    /* BO-227: the headroom macOS grants (on the PG32UCDP 1.92x of 5.08x,
     * steady), ignoring the 1.0 it reads while the layer is not on screen
     * and changes of 5% or less; the potential until a real reading. */
    float cap = s_hdr_cap;
    if (potential != s_hdr_pot || cap <= 0.0f)
        cap = potential;
    if (now > 1.0f && now < potential) {
        if (cap > now * 1.05f || cap < now * 0.95f)
            cap = now;
    } else if (now >= potential) {
        cap = potential;
    }
    if (cap != s_hdr_cap)
        fprintf(stderr, "[HDR] display headroom %.2fx (potential %.2fx, now %.2fx)\n",
                cap, potential, now);
    s_hdr_pot = potential;
    s_hdr_now = now;
    s_hdr_cap = cap;
}

float nv2a_host_hdr_potential(void) { return s_hdr_pot; }
float nv2a_host_hdr_granted(void) { return s_hdr_cap; }

void nv2a_host_set_hdr_patch_fn(void *(*fn)(float)) { s_hdr_patch_fn = fn; }
/* BO-227: where the overlay drew the patch (window pixels), for a probe. */
static volatile float s_patch_at[4];
static volatile uint32_t s_patch_at_ms;
void nv2a_host_hdr_patch_at(float x0, float y0, float x1, float y1)
{
    s_patch_at[0] = x0; s_patch_at[1] = y0; s_patch_at[2] = x1; s_patch_at[3] = y1;
    s_patch_at_ms++;
}
int nv2a_host_hdr_patch_where(float *x0, float *y0, float *x1, float *y1)
{
    static uint32_t seen;
    int fresh = s_patch_at_ms != seen;
    seen = s_patch_at_ms;
    *x0 = s_patch_at[0]; *y0 = s_patch_at[1]; *x1 = s_patch_at[2]; *y1 = s_patch_at[3];
    return fresh;
}
void *nv2a_host_hdr_patch(float nits) { return s_hdr_patch_fn ? s_hdr_patch_fn(nits) : NULL; }

/* The overlay's request for a pattern this frame (a presenter that cannot
 * draw past the overlay's own white takes it after the overlay and draws it
 * itself: Vulkan's HDR10 path). */
static struct { int on, kind; float r[4], a, b; } s_hdr_pat;

void nv2a_host_hdr_pattern(int kind, float x0, float y0, float x1, float y1, float a, float b)
{
    s_hdr_pat.kind = kind;
    s_hdr_pat.r[0] = x0; s_hdr_pat.r[1] = y0; s_hdr_pat.r[2] = x1; s_hdr_pat.r[3] = y1;
    s_hdr_pat.a = a;
    s_hdr_pat.b = b;
    s_hdr_pat.on = 1;
}

int nv2a_host_hdr_pattern_take(int *kind, float r[4], float *a, float *b)
{
    if (!s_hdr_pat.on)
        return 0;
    s_hdr_pat.on = 0;
    *kind = s_hdr_pat.kind;
    memcpy(r, s_hdr_pat.r, sizeof s_hdr_pat.r);
    *a = s_hdr_pat.a;
    *b = s_hdr_pat.b;
    return 1;
}

static volatile int s_dither = -1;          /* -1: not read yet */

int nv2a_host_dither(void)
{
    if (s_dither < 0) {
        const char *e = recomp_env(RENV_GFX_DITHER);
        s_dither = e && *e && strcmp(e, "0") && strcmp(e, "off") ? 1 : 0;
    }
    return s_dither;
}

void nv2a_host_set_dither(int on)
{
    s_dither = on ? 1 : 0;
}

int nv2a_rt_reduce_draw(uint32_t src_w, uint32_t src_h, uint32_t src_hw, uint32_t src_hh,
                        uint32_t dst_w, uint32_t dst_h)
{
    if (!src_w || !src_h || !dst_w || !dst_h)
        return 0;
    if (src_hw <= src_w && src_hh <= src_h)
        return 0;                       /* not scaled: 1x */
    return dst_w <= src_w && dst_h <= src_h && (dst_w < src_w || dst_h < src_h);
}

int nv2a_rt_reduce_on(uint32_t flips)
{
    static int mode = -1;
    static uint32_t ab, said = 0xFFFFFFFFu;
    if (mode < 0) {
        const char *e = recomp_env(RENV_RT_REDUCE), *a = recomp_env(RENV_RT_REDUCE_AB);
        ab = a ? (uint32_t)strtoul(a, NULL, 0) : 0;
        mode = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
    }
    if (ab) {
        uint32_t half = (flips / ab) & 1u;
        if (half != said) {
            said = half;
            fprintf(stderr, "[RT-REDUCE] ab: %s from flip %u\n", half ? "off" : "on", flips);
        }
        return half ? 0 : mode;
    }
    return mode;
}

unsigned nv2a_host_render_scale(void)
{
    return s_host_opts.render_scale > 1 ? s_host_opts.render_scale : 1;
}

unsigned nv2a_host_render_scale_x2(void)
{
    if (s_host_opts.render_scale_half && s_host_opts.render_scale <= 1)
        return 3;
    return 2u * nv2a_host_render_scale();
}

unsigned nv2a_host_size_x2(uint32_t w, uint32_t h, unsigned x2, uint32_t max_dim,
                           uint32_t *hw, uint32_t *hh)
{
    unsigned n = x2 > 2 ? x2 : 2;
    while (n > 2 && (((uint64_t)w * n + 1) / 2 > max_dim || ((uint64_t)h * n + 1) / 2 > max_dim))
        n--;
    *hw = (uint32_t)(((uint64_t)w * n + 1) / 2);
    *hh = (uint32_t)(((uint64_t)h * n + 1) / 2);
    return n;
}

unsigned nv2a_host_size(uint32_t w, uint32_t h, unsigned scale, uint32_t max_dim,
                        uint32_t *hw, uint32_t *hh)
{
    unsigned n = scale > 1 ? scale : 1;
    while (n > 1 && ((uint64_t)w * n > max_dim || (uint64_t)h * n > max_dim))
        n--;
    *hw = w * n;
    *hh = h * n;
    return n;
}

/* nv2a_present_frame_aspect_fn: a host that shows some frames at another
 * shape than the configured one (a title whose 3D is set up wider than its
 * menus). The backends name the frame they are about to lay out
 * (nv2a_present_frame); the function answers its width:height, or 0 for
 * "as configured". Without a function nothing here changes. */
static double (*volatile s_frame_aspect_fn)(uint32_t flip);
static volatile double s_frame_aspect;      /* of the frame being laid out; 0: configured */
static volatile double s_window_aspect;     /* nv2a_present_window_aspect; 0: configured */

void nv2a_present_frame_aspect_fn(double (*fn)(uint32_t flip))
{
    s_frame_aspect_fn = fn;
    if (!fn)
        s_frame_aspect = 0.0;
}

void nv2a_present_frame(uint32_t flip)
{
    double (*fn)(uint32_t) = s_frame_aspect_fn;
    if (fn) {
        double a = fn(flip);
        s_frame_aspect = a > 0.0 ? a : 0.0;
    }
}

void nv2a_present_window_aspect(double aspect)
{
    s_window_aspect = aspect > 0.0 ? aspect : 0.0;
}

static volatile uint64_t s_hud_squeeze;    /* vs_hash << 32 | k's bits; 0 off */

void nv2a_hud_squeeze(uint32_t vs_hash, float k)
{
    uint32_t kb;

    memcpy(&kb, &k, 4);
    s_hud_squeeze = vs_hash && k > 0.0f && k != 1.0f ? (uint64_t)vs_hash << 32 | kb : 0;
}

int nv2a_hud_squeeze_get(uint32_t *vs_hash, float *k)
{
    uint64_t v = s_hud_squeeze;
    uint32_t kb = (uint32_t)v;

    if (!v)
        return 0;
    *vs_hash = (uint32_t)(v >> 32);
    memcpy(k, &kb, 4);
    return 1;
}

/* The display aspect as aw:ah; the frame's own at FRAME. */
static void present_aspect_ratio(uint32_t src_w, uint32_t src_h, int aspect, double fa,
                                 uint32_t *aw, uint32_t *ah)
{
    if (fa > 0.0) {
        /* In 1/3600: 4:3, 16:9, 64:27 and 32:9 are whole. */
        *aw = (uint32_t)(fa * 3600.0 + 0.5);
        *ah = 3600;
        return;
    }
    switch (aspect) {
    case NV2A_ASPECT_4_3:  *aw = 4;  *ah = 3; break;
    case NV2A_ASPECT_16_9: *aw = 16; *ah = 9; break;
    case NV2A_ASPECT_16_10: *aw = 16; *ah = 10; break;
    default:               *aw = src_w; *ah = src_h; break;
    }
}

uint32_t nv2a_present_width(uint32_t src_w, uint32_t src_h)
{
    uint32_t aw, ah;
    present_aspect_ratio(src_w, src_h, s_host_opts.present_aspect, s_frame_aspect, &aw, &ah);
    if (!src_h || !ah)
        return src_w;
    return (uint32_t)(((uint64_t)src_h * aw + ah / 2) / ah);
}

/* RECOMP_PRESENT_OVERSCAN=crop|fill / RECOMP_PRESENT_FIT=fill16x10 (see
 * nv2a_backend_common.h, nv2a_present_layout). The overscan is
 * NV2A_OVERSCAN_OFF / CROP / FILL; the fit 0 / 1. */
static volatile int s_crop = -1, s_fill = -1;   /* -1: not read yet */

/* The same two, live (the overlay's Display page); the next present uses
 * them. */
void nv2a_present_set_overscan(int mode)
{
    s_crop = mode == NV2A_OVERSCAN_CROP || mode == NV2A_OVERSCAN_FILL ? mode : NV2A_OVERSCAN_OFF;
}

void nv2a_present_set_fit(int fill)
{
    s_fill = fill != 0;
}

/* present.crop and the drawn part of the frame last presented. */
static volatile int s_clip_crop = -1;           /* -1: not read yet */
static volatile uint32_t s_drawn[6];            /* x0, y0, x1, y1, w, h */

void nv2a_present_set_crop(int mode)
{
    s_clip_crop = mode != 0;
}

void nv2a_present_set_drawn(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                            uint32_t w, uint32_t h)
{
    s_drawn[0] = x0; s_drawn[1] = y0; s_drawn[2] = x1; s_drawn[3] = y1;
    s_drawn[4] = w;  s_drawn[5] = h;
}

/* The output size of the last present, for nv2a_present_visible_x: 0 until a
 * window has shown a frame (a headless run never does). */
static volatile uint32_t s_last_dw, s_last_dh;
static volatile int s_last_filter;

static int present_layout(uint32_t fw, uint32_t fh, uint32_t dw, uint32_t dh, int filter,
                          int aspect, double fa, struct nv2a_rect *src, struct nv2a_rect *dst);
static int present_rect(uint32_t src_w, uint32_t src_h, int aspect, double fa,
                        uint32_t dst_w, uint32_t dst_h, int filter, struct nv2a_rect *r);

int nv2a_present_layout_cuts(void)
{
    struct nv2a_rect s, d;
    /* Reads the environment on the first call, as the layout does. */
    present_layout(640, 480, 640, 480, NV2A_PRESENT_NEAREST, s_host_opts.present_aspect, 0.0, &s, &d);
    return s_crop != NV2A_OVERSCAN_OFF || s_fill > 0 || s_clip_crop > 0;
}

int nv2a_present_layout(uint32_t fw, uint32_t fh, uint32_t dw, uint32_t dh, int filter,
                        struct nv2a_rect *src, struct nv2a_rect *dst)
{
    s_last_dw = dw;
    s_last_dh = dh;
    s_last_filter = filter;
    return present_layout(fw, fh, dw, dh, filter, s_host_opts.present_aspect, s_frame_aspect,
                          src, dst);
}

uint32_t nv2a_present_window_width(uint32_t src_w, uint32_t src_h)
{
    double wa = s_window_aspect;
    if (wa > 0.0 && src_h)
        return (uint32_t)((double)src_h * wa + 0.5);
    return nv2a_present_width(src_w, src_h);
}

double nv2a_present_output_aspect(void)
{
    uint32_t w = s_last_dw, h = s_last_dh;
    return w && h ? (double)w / (double)h : 0.0;
}

void nv2a_present_note_output(uint32_t dw, uint32_t dh)
{
    if (!s_last_dw || !s_last_dh) {
        s_last_dw = dw;
        s_last_dh = dh;
    }
}

void nv2a_present_visible_x_at(uint32_t fw, uint32_t fh, int aspect, double *x0, double *x1)
{
    struct nv2a_rect src, dst;
    if (!fw || !fh) {
        *x0 = 0.0;
        *x1 = (double)fw;
        return;
    }
    present_layout(fw, fh, s_last_dw, s_last_dh, s_last_filter, aspect,
                   aspect == s_host_opts.present_aspect ? s_frame_aspect : 0.0, &src, &dst);
    *x0 = (double)src.x;
    *x1 = (double)(src.x + src.w);
}

void nv2a_present_visible_x_ratio(uint32_t fw, uint32_t fh, double ratio, double *x0, double *x1)
{
    struct nv2a_rect src, dst;
    if (!fw || !fh) {
        *x0 = 0.0;
        *x1 = (double)fw;
        return;
    }
    present_layout(fw, fh, s_last_dw, s_last_dh, s_last_filter, s_host_opts.present_aspect,
                   ratio > 0.0 ? ratio : 0.0, &src, &dst);
    *x0 = (double)src.x;
    *x1 = (double)(src.x + src.w);
}

void nv2a_present_visible_x(uint32_t fw, uint32_t fh, double *x0, double *x1)
{
    nv2a_present_visible_x_at(fw, fh, s_host_opts.present_aspect, x0, x1);
}

static int present_layout(uint32_t fw, uint32_t fh, uint32_t dw, uint32_t dh, int filter,
                          int aspect, double fa, struct nv2a_rect *src, struct nv2a_rect *dst)
{
    int whole, crop, fill;
    if (s_crop < 0) {
        const char *e = recomp_env(RENV_PRESENT_OVERSCAN);
        s_crop = e && !strcmp(e, "crop") ? NV2A_OVERSCAN_CROP
               : e && !strcmp(e, "fill") ? NV2A_OVERSCAN_FILL : NV2A_OVERSCAN_OFF;
    }
    if (s_fill < 0) {
        const char *e = recomp_env(RENV_PRESENT_FIT);
        s_fill = e && !strcmp(e, "fill16x10");
    }
    if (s_clip_crop < 0) {
        const char *e = recomp_env(RENV_PRESENT_CROP);
        s_clip_crop = e && !strcmp(e, "clip");
    }
    crop = s_crop;
    fill = s_fill;
    src->x = src->y = 0;
    src->w = (int32_t)fw;
    src->h = (int32_t)fh;
    if (s_clip_crop > 0 && fw && fh) {
        /* The part the title drew in, in this frame's pixels (a scaled
         * target is the guest frame N times over). */
        uint32_t x0 = s_drawn[0], y0 = s_drawn[1], x1 = s_drawn[2], y1 = s_drawn[3];
        uint32_t gw = s_drawn[4], gh = s_drawn[5];
        if (gw && gh && x1 > x0 && y1 > y0 && x1 <= gw && y1 <= gh
                && (x0 || y0 || x1 < gw || y1 < gh)) {
            src->x = (int32_t)((uint64_t)x0 * fw / gw);
            src->y = (int32_t)((uint64_t)y0 * fh / gh);
            src->w = (int32_t)((uint64_t)x1 * fw / gw) - src->x;
            src->h = (int32_t)((uint64_t)y1 * fh / gh) - src->y;
            crop = NV2A_OVERSCAN_OFF;   /* the title's border is the TV's margin */
        }
    }
    if (crop == NV2A_OVERSCAN_FILL && fw && fh) {
        /* The 640x448 band (16 of 480 rows off the top and bottom) at the
         * full width, into the box the whole frame has: a vertical stretch of
         * 480/448, nothing cut at the sides. The box is the frame's, as with
         * no overscan, so the aspect and the fit rules are the same ones. */
        int32_t cy = (int32_t)((fh * 16u + 240u) / 480u);
        src->y = cy;
        src->h = (int32_t)fh - 2 * cy;
        present_rect(fw, fh, aspect, fa, dw, dh, filter, dst);
        whole = 0;      /* not a whole multiple of the band: filtered, not nearest */
    } else if (crop && fw && fh) {
        /* 16 of 480 rows top and bottom (a TV's overscan: the 640x448 the
         * title keeps its picture in), the same share of the width. */
        int32_t cy = (int32_t)((fh * 16u + 240u) / 480u);
        int32_t cx = (int32_t)(((uint64_t)fw * 16u + 240u) / 480u);
        src->x = cx;
        src->y = cy;
        src->w = (int32_t)fw - 2 * cx;
        src->h = (int32_t)fh - 2 * cy;
    }
    if (crop != NV2A_OVERSCAN_FILL || !fw || !fh)
        whole = present_rect((uint32_t)src->w, (uint32_t)src->h, aspect, fa, dw, dh, filter, dst);
    if (fill && dst->h > 0 && (uint32_t)dst->h < dh) {
        /* Fill the height; what is wider than the window is cropped from
         * the frame's sides. */
        int64_t nw = (int64_t)dst->w * dh / dst->h;
        if (nw > (int64_t)dw) {
            int32_t keep = (int32_t)((int64_t)src->w * dw / nw);
            src->x += (src->w - keep) / 2;
            src->w = keep;
            nw = dw;
        }
        dst->x = (int32_t)(((int64_t)dw - nw) / 2);
        dst->y = 0;
        dst->w = (int32_t)nw;
        dst->h = (int32_t)dh;
        whole = 0;
    }
    return whole;
}

int nv2a_present_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int filter, struct nv2a_rect *r)
{
    return nv2a_present_rect_aspect(src_w, src_h, s_host_opts.present_aspect,
                                    dst_w, dst_h, filter, r);
}

int nv2a_present_rect_aspect(uint32_t src_w, uint32_t src_h, int aspect,
                             uint32_t dst_w, uint32_t dst_h, int filter,
                             struct nv2a_rect *r)
{
    return present_rect(src_w, src_h, aspect,
                        aspect == s_host_opts.present_aspect ? s_frame_aspect : 0.0,
                        dst_w, dst_h, filter, r);
}

/* The same with the frame's own aspect given (fa > 0), or the configured one. */
static int present_rect(uint32_t src_w, uint32_t src_h, int aspect, double fa,
                        uint32_t dst_w, uint32_t dst_h, int filter, struct nv2a_rect *r)
{
    uint64_t fw, fh;
    uint32_t aw, ah;

    r->x = r->y = r->w = r->h = 0;
    if (!src_w || !src_h || !dst_w || !dst_h)
        return 0;
    present_aspect_ratio(src_w, src_h, aspect, fa, &aw, &ah);
    /* The frame's own aspect, whatever the setting says. */
    if ((uint64_t)aw * src_h == (uint64_t)ah * src_w)
        aw = src_w, ah = src_h;
    if (filter == NV2A_PRESENT_INTEGER) {
        uint32_t k = dst_w / src_w, kh = dst_h / src_h;
        if (aw == src_w && ah == src_h) {
            if (kh < k)
                k = kh;
            if (k >= 1) {
                r->w = (int32_t)(src_w * k);
                r->h = (int32_t)(src_h * k);
                r->x = (int32_t)(dst_w - (uint32_t)r->w) / 2;
                r->y = (int32_t)(dst_h - (uint32_t)r->h) / 2;
                return 1;
            }
        } else {
            /* Whole multiples of the height, the width at the aspect. */
            for (k = kh; k >= 1; k--) {
                uint64_t w = ((uint64_t)src_h * k * aw + ah / 2) / ah;
                if (w <= dst_w) {
                    r->w = (int32_t)w;
                    r->h = (int32_t)(src_h * k);
                    r->x = (int32_t)(dst_w - (uint32_t)r->w) / 2;
                    r->y = (int32_t)(dst_h - (uint32_t)r->h) / 2;
                    return 0;
                }
            }
        }
    }
    /* Fit: the axis that limits gets the whole output, the other the
     * display aspect, rounded to the nearest pixel. */
    if ((uint64_t)dst_w * ah <= (uint64_t)dst_h * aw) {
        fw = dst_w;
        fh = ((uint64_t)dst_w * ah + aw / 2) / aw;
    } else {
        fh = dst_h;
        fw = ((uint64_t)dst_h * aw + ah / 2) / ah;
    }
    r->w = (int32_t)fw;
    r->h = (int32_t)fh;
    r->x = (int32_t)((dst_w - fw) / 2);
    r->y = (int32_t)((dst_h - fh) / 2);
    return 0;
}

void nv2a_upscale_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                            unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y;
    unsigned i, j;
    for (y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(const void *)(src + (size_t)y * src_pitch);
        uint32_t *d = (uint32_t *)(void *)(dst + (size_t)y * n * dst_pitch);
        for (x = 0; x < w; x++)
            for (i = 0; i < n; i++)
                d[(size_t)x * n + i] = s[x];
        for (j = 1; j < n; j++)
            memcpy(dst + ((size_t)y * n + j) * dst_pitch, d, (size_t)w * n * 4u);
    }
}

void nv2a_downscale_box32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                          unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y, nn = n * n, half = nn / 2;
    unsigned i, j, c;
    for (y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (x = 0; x < w; x++) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (j = 0; j < n; j++) {
                const uint8_t *s = src + ((size_t)y * n + j) * src_pitch + (size_t)x * n * 4u;
                for (i = 0; i < n; i++, s += 4)
                    for (c = 0; c < 4; c++)
                        sum[c] += s[c];
            }
            for (c = 0; c < 4; c++)
                d[(size_t)x * 4u + c] = (uint8_t)((sum[c] + half) / nn);
        }
    }
}

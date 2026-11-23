/*
 * nv2a_pb_split.h - RECOMP_TRACE=pb_split=<file> (BO-282.12): where the
 * executor's busy time goes inside a picture, one CSV line per flip.
 *
 * Two parts, both on the executor's thread:
 *
 *  - The walker (nv2a_pb_scan.c) counts every method header and its
 *    parameter words by class (state, transform constants, combiners,
 *    textures, index data, BEGIN_END, ...), and times a random quarter of
 *    the headers from the header to the next one: the decode, the handler
 *    and, for BEGIN_END, the draw it starts. The sampled ticks are scaled up
 *    by the rate in the CSV, so a class's column is an estimate whose error
 *    shrinks over many pictures, not an exact per-picture value.
 *
 *  - The Vulkan backend's vk_on_draw times the parts of a random quarter of
 *    the draws (vertex shader lookup, the target, the stages, fs_select,
 *    prim_to_list, the texture binds, ubo_place, the gather, the texture
 *    set, the pass, the pipeline lookup, the vkCmd calls), scaled the same
 *    way. Rare events are timed always: texture rehashes, uploads (count
 *    and bytes), render-target ownership hashes, cube copies.
 *
 * Off (the default), every site is one load of nv2a_split and a branch. On,
 * a sampled header costs two TSC reads and a sampled draw about twenty, so
 * the trace itself is about 0.1 ms of a picture's busy time on a desktop.
 */
#ifndef NV2A_PB_SPLIT_H
#define NV2A_PB_SPLIT_H

#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
static inline uint64_t nv2a_split_tick(void) { return __rdtsc(); }
#elif defined(__aarch64__)
static inline uint64_t nv2a_split_tick(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
#include <time.h>
static inline uint64_t nv2a_split_tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* Method classes (nv2a_split_class). */
enum {
    PBS_STATE,      /* render state not listed below (blend, cull, depth, stencil, fog...) */
    PBS_XCONST,     /* SET_TRANSFORM_CONSTANT[_LOAD] */
    PBS_XPROG,      /* SET_TRANSFORM_PROGRAM[_LOAD/_START], execution mode */
    PBS_XFF,        /* fixed-function matrices, viewport, lights, texgen */
    PBS_COMB,       /* register combiners, shader stage program, specular/fog CW */
    PBS_TEX,        /* SET_TEXTURE_* (0x1B00..0x1BFC) */
    PBS_VFMT,       /* vertex array offsets and formats */
    PBS_INDEX,      /* ARRAY_ELEMENT16/32, DRAW_ARRAYS */
    PBS_INLINE,     /* INLINE_ARRAY (vertex data in the stream) */
    PBS_BEGIN,      /* BEGIN_END: END runs the draw (on_draw is inside) */
    PBS_IMM,        /* SET_VERTEX_DATA* (immediate vertices, inline attributes) */
    PBS_SURF,       /* surface format, pitch, offsets, clip */
    PBS_CLEAR,      /* CLEAR_SURFACE and clear values */
    PBS_SYNC,       /* semaphores, reports, waits, NOP, set object, DMA */
    PBS_FLIP,       /* FLIP_* (FLIP_STALL holds for the vblank and presents) */
    PBS_OTHER,      /* any other subchannel or method */
    PBS_N
};

/* vk_on_draw's parts, in call order. */
enum {
    VKS_VS,         /* init, the vertex shader lookup (vs_cache) */
    VKS_SURFACE,    /* surface(): the target, its ownership check */
    VKS_STAGES,     /* build_stages, constants, bias, bump, hdr_draw */
    VKS_FS,         /* fs_select */
    VKS_PRIM,       /* index range and nv2a_prim_to_list */
    VKS_VPC,        /* vertex-processing constants, depth target */
    VKS_TEX,        /* the texture loop, all four stages (binds included) */
    VKS_HDRZ,       /* light gain, decal z offset */
    VKS_UBO,        /* ring_alloc and ubo_place */
    VKS_GATHER,     /* nv2a_vtx_gather and the index copy */
    VKS_TSET,       /* texture_set (descriptor set) */
    VKS_ENC,        /* begin_encoder (a new pass) */
    VKS_PSO,        /* pipeline() lookup */
    VKS_CMD,        /* dynamic state, vkCmd* calls, counters */
    VKS_N
};

/* Inside VKS_TEX (sampled draws only). */
enum { VKX_TEXBIND, VKX_CUBE, VKX_RTTEX, VKX_SAMPLER, VKX_N };

/* The draws by where their vertices came from (nv2a_pb_exec.c's
 * draw_primitive, every draw, its on_draw time): vertex arrays with
 * indices or DRAW_ARRAYS, an INLINE_ARRAY batch, an immediate-mode batch. */
enum { PBD_ARRAY, PBD_INLINE, PBD_IMM, PBD_N };

struct nv2a_split {
    uint32_t rate;                  /* 1 in rate headers / draws timed */
    uint32_t rng;                   /* xorshift32 state */
    /* walker */
    uint32_t hdr[PBS_N], words[PBS_N], hdr_timed[PBS_N];
    uint64_t hdr_ticks[PBS_N];      /* sampled, unscaled */
    uint64_t open_t0;               /* the timed header's start, 0: none */
    int      open_cls;
    /* vk_on_draw */
    uint32_t draws, draws_timed;
    uint64_t vk_ticks[VKS_N], vkx_ticks[VKX_N];
    uint32_t vkx_calls[VKX_N];
    uint64_t draw_t;                /* the timed draw's last mark, 0: not timed */
    uint32_t kd[PBD_N], kv[PBD_N];  /* draws, vertices (indices) */
    uint64_t kd_ns[PBD_N];          /* their on_draw time */
    /* always timed */
    uint32_t rehash, rehash_kb, up, up_kb, up_new, own_hash, own_kb, cube_copies, syncs;
    uint64_t rehash_ticks, up_ticks, own_ticks, sync_ticks;
};

extern struct nv2a_split *nv2a_split;   /* NULL: off */

static inline int nv2a_split_pick(struct nv2a_split *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return (x % s->rate) == 0;
}

/* A method class from its (subchannel, method). */
static inline int nv2a_split_class(uint32_t subch, uint32_t m)
{
    if (subch != 0)
        return PBS_OTHER;
    if (m == 0x17FC)                        return PBS_BEGIN;
    if (m == 0x1818)                        return PBS_INLINE;
    if (m >= 0x1800 && m < 0x1820)          return PBS_INDEX;   /* AE16, AE32, DRAW_ARRAYS */
    if (m >= 0x0B80 && m < 0x0C00)          return PBS_XCONST;
    if (m == 0x1EA4)                        return PBS_XCONST;  /* CONSTANT_LOAD */
    if (m >= 0x0B00 && m < 0x0B80)          return PBS_XPROG;
    if (m >= 0x1E90 && m < 0x1EA4)          return PBS_XPROG;   /* mode, cxt write, load, start */
    if (m >= 0x1B00 && m < 0x1C00)          return PBS_TEX;
    if (m >= 0x1720 && m < 0x17A0)          return PBS_VFMT;
    if (m >= 0x1880 && m < 0x1B00)          return PBS_IMM;
    if (m >= 0x1500 && m < 0x1720)          return PBS_IMM;     /* vertex data 3f/4f, colours, normals */
    if ((m >= 0x0260 && m < 0x0290) || (m >= 0x0A60 && m < 0x0AE0)
            || (m >= 0x1E20 && m < 0x1E80))
        return PBS_COMB;
    if (m >= 0x0400 && m < 0x0B00)          return PBS_XFF;     /* matrices, texgen, viewport */
    if (m >= 0x0C00 && m < 0x1500)          return PBS_XFF;     /* materials, lights */
    if (m >= 0x0200 && m < 0x0220)          return PBS_SURF;
    if (m >= 0x1D8C && m < 0x1DA0)          return PBS_CLEAR;   /* zstencil/colour clear values, CLEAR */
    if (m >= 0x0120 && m < 0x0140)          return PBS_FLIP;
    if (m < 0x0200 || (m >= 0x1D60 && m < 0x1D8C) || (m >= 0x17C8 && m < 0x17D4))
        return PBS_SYNC;                    /* object, NOP, DMA, semaphores, reports */
    return PBS_STATE;
}

/* The walker: a header for class cls with n parameter words starts (cls -1:
 * the walk ends). Closes a timed header, maybe opens one. */
static inline void nv2a_split_header(struct nv2a_split *s, int cls, uint32_t n)
{
    uint64_t t = 0;
    if (s->open_t0) {
        t = nv2a_split_tick();
        s->hdr_ticks[s->open_cls] += t - s->open_t0;
        s->hdr_timed[s->open_cls]++;
        s->open_t0 = 0;
    }
    if (cls < 0)
        return;
    s->hdr[cls]++;
    s->words[cls] += n;
    if (nv2a_split_pick(s)) {
        s->open_t0 = t ? t : nv2a_split_tick();
        s->open_cls = cls;
    }
}

/* vk_on_draw: the time since the last mark goes to part k. */
static inline void nv2a_split_mark(struct nv2a_split *s, int k)
{
    uint64_t t = nv2a_split_tick();
    s->vk_ticks[k] += t - s->draw_t;
    s->draw_t = t;
}

/* The executor's flip (nv2a_pb_exec.c): the picture's line is written. */
void nv2a_split_flip(uint32_t flip);

#endif /* NV2A_PB_SPLIT_H */

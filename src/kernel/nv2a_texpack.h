/**
 * nv2a_texpack.h - texture packs: PNG replacements for a title's textures
 * (BO-312; docs/mod-folders.md in the Burnout 3 repo, Texture packs).
 *
 * Dolphin's and PCSX2's way: a pack is a folder of PNG files named by the
 * texture's content hash. A texture the title uploads whose key is in a pack
 * is shown from the PNG, at the PNG's own size, instead of the title's
 * texels. Nothing else about the texture changes: the title still draws it
 * with its own sampler state, texture coordinates and blending.
 *
 * Names. <hash>.png, and for a paletted (P8) texture <hash>-<palette>.png:
 * 16 hex digits each, the hash nv2a_texrep_hash gives (the guest bytes of
 * level 0) and, for P8, nv2a_tex_hash of the palette's bytes (the title
 * recolours one set of indices with several palettes). The names are the ones
 * RECOMP_DEBUG=texrep_dump=<dir> writes, so a mod maker dumps the originals,
 * edits or upscales them and puts them in a pack under the same names. A P8
 * texture with no <hash>-<palette>.png takes <hash>.png when there is one.
 *
 * Size. Any size. The factor is read per texture from the PNG against the
 * title's level 0 (w x h), per axis, so 2x, 3x, 4x, 8x and a PNG that is 4x
 * wide and 2x high all work. The title's sampler LOD clamps move down by
 * round(log2(the larger factor)) levels (nv2a_texpack_lod). A PNG smaller than
 * the title's texture is used as it is (no shift).
 *
 * Mips. The PNG is level 0; the levels below are made when it is loaded
 * (2x2 box, every channel on its own: nothing is filled under alpha). A
 * title texture with one level gets the levels down to its own size, so the
 * sampler can still minify.
 *
 * Alpha (pack.toml: alpha = "auto" | "original" | "png"). auto: the PNG's
 * alpha when it has an alpha channel, else the title's, scaled up bilinearly
 * to the PNG's size. original: always the title's. png: always the PNG's (a
 * PNG without alpha is opaque). The colour under alpha 0 is never touched:
 * BO-170 filled it and broke the shine masks.
 *
 * Order. The packs are given in priority order (the mod order): the first pack
 * that has a key wins.
 *
 * Off the render thread. A PNG is read, decoded, scaled, mipped and, when it is
 * big, compressed by a worker thread. The backend uploads the title's own
 * texture first and swaps the replacement in when the worker is done
 * (nv2a_texpack_poll), within an upload budget a flip. Compressed results
 * (BC1 for opaque, BC3 with alpha) are kept in a disk cache, so the next
 * start reads them instead of decoding the PNG again. The backend keeps the
 * bytes of what it has swapped in under a budget (nv2a_texpack_cap_bytes) and
 * puts the least recently used texture back to the title's when it is over.
 *
 * No OS or graphics headers: shared by the Metal, Vulkan and D3D11 backends.
 */
#ifndef NV2A_TEXPACK_H
#define NV2A_TEXPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── the packs ────────────────────────────────────────────────────────── */

#define NV2A_TEXPACK_MAX_PACKS 64

enum { NV2A_TEXPACK_ALPHA_AUTO = 0, NV2A_TEXPACK_ALPHA_ORIGINAL = 1, NV2A_TEXPACK_ALPHA_PNG = 2 };
enum { NV2A_TEXPACK_COMPRESS_AUTO = 0, NV2A_TEXPACK_COMPRESS_OFF = 1 };

struct nv2a_texpack_src {
    const char *dir;      /* the folder with the PNG files (a mod's textures/) */
    const char *name;     /* for the log (the mod's id); may be NULL */
    int         alpha;    /* NV2A_TEXPACK_ALPHA_* */
    int         compress; /* NV2A_TEXPACK_COMPRESS_*: OFF keeps this pack's textures raw BGRA8 */
};

/* The packs, highest priority first (at most NV2A_TEXPACK_MAX_PACKS; copied).
 * Lists every folder's PNG names at once. RECOMP_DEBUG=texpack=<dir>[;<dir>...]
 * adds folders in front of these (a toolkit test or a mod maker without a
 * title's mod system). n = 0 clears the packs. Returns the number of keys
 * indexed. The generation moves on: a backend that has swapped a texture in
 * puts the title's back and looks again. Any thread; call it before the
 * title draws, or from the thread that owns the backend. */
int      nv2a_texpack_set(const struct nv2a_texpack_src *packs, unsigned n);
/* Never 0; moves on whenever nv2a_texpack_set changes the packs. */
uint32_t nv2a_texpack_generation(void);
/* 1 when at least one key is indexed (a backend asks nothing otherwise: a
 * title with no pack hashes nothing). */
int      nv2a_texpack_active(void);

/* ── the keys ─────────────────────────────────────────────────────────── */

/* "<16 hex>.png" or "<16 hex>-<16 hex>.png" (the extension in any case, hex in
 * either): 1 and the two numbers (palh 0 for the first form), else 0. */
int nv2a_texpack_parse_name(const char *name, uint64_t *hash, uint64_t *palh);
/* The name of a key: "<hash>.png" or, with a palette hash, "<hash>-<palh>.png"
 * (lower case hex). */
void nv2a_texpack_key_name(uint64_t hash, uint64_t palh, char *out, size_t n);
/* The key of a texture a backend decodes from guest memory: hash is
 * nv2a_texrep_hash of level 0; for P8 (pal, pal_len entries) palh is the
 * palette's hash, else 0. 0: the format has no key. */
int nv2a_texpack_key(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                     uint32_t pitch, const uint8_t *pal, uint32_t pal_len,
                     uint64_t *hash, uint64_t *palh);

/* The entry of a key in the highest pack that has it: an id (> 0), else 0.
 * A P8 key with no exact entry takes the one of the bare hash. Valid until the
 * next nv2a_texpack_set. */
uint32_t nv2a_texpack_find(uint64_t hash, uint64_t palh);
/* Which pack (the index in the list given to nv2a_texpack_set) holds an id. */
int      nv2a_texpack_pack_of(uint32_t id);

/* ── sizes and sampler levels ─────────────────────────────────────────── */

/* The levels a replacement of pw x ph has above the title's w x h:
 * round(log2(max(pw / w, ph / h))), 0 when it is not larger. */
uint32_t nv2a_texpack_add_levels(uint32_t pw, uint32_t ph, uint32_t w, uint32_t h);
/* The factor per axis as a reduced pair of numbers for the log:
 * pw / w and ph / h in tenths (40 is 4.0x). */
void     nv2a_texpack_factors(uint32_t pw, uint32_t ph, uint32_t w, uint32_t h,
                              uint32_t *fx10, uint32_t *fy10);
/* The levels a replacement has: the title's `levels` above the factor's
 * `add` (levels 1 gets add + 1, so the sampler can minify down to the
 * title's size), at most the whole chain of pw x ph. */
uint32_t nv2a_texpack_levels(uint32_t pw, uint32_t ph, uint32_t levels, uint32_t add);
/* The sampler's LOD clamps for a replaced texture of the title's `levels`
 * levels and clamps (a title minimum of 0 keeps the new detail). */
void     nv2a_texpack_lod(uint32_t levels, uint32_t lod_min, uint32_t lod_max, uint32_t add,
                          float *min_lod, float *max_lod);

/* ── the images ───────────────────────────────────────────────────────── */

enum { NV2A_TEXPACK_FMT_BGRA8 = 0, NV2A_TEXPACK_FMT_BC1 = 1, NV2A_TEXPACK_FMT_BC3 = 2 };

/* What a backend can sample: bit NV2A_TEXPACK_FMT_BC1 and _BC3 set when it
 * supports the block formats (BGRA8 always). Without them nothing is
 * compressed. The Metal backend on a Mac and Vulkan desktops have them;
 * Android's drivers mostly do not. */
void nv2a_texpack_set_formats(unsigned mask);
/* 0 or 1: RECOMP_DEBUG=texpack_compress=0 turns compression off. */
int  nv2a_texpack_compress_on(void);

/* A replacement ready to upload: levels one after the other, level 0 first,
 * each (fmt) w x h as the format stores it: BGRA8 w*h*4 bytes, BC1 and BC3
 * ceil(w/4)*ceil(h/4) blocks of 8 and 16 bytes. */
struct nv2a_texpack_img {
    uint32_t w, h;          /* level 0 */
    uint32_t levels;
    uint32_t fmt;           /* NV2A_TEXPACK_FMT_* */
    uint32_t add;           /* nv2a_texpack_add_levels against the title's size */
    uint8_t *data;
    size_t   bytes;         /* all levels */
};
size_t   nv2a_texpack_level_bytes(uint32_t fmt, uint32_t w, uint32_t h);
uint32_t nv2a_texpack_level_dim(uint32_t d, uint32_t level);   /* max(1, d >> level) */

/* The pure part of a job, for the worker and for tests: a PNG's pixels (w x h
 * BGRA8 bytes, straight alpha; `has_alpha` set from the file) turned into
 * the replacement of a title texture ow x oh with `olevels` levels.
 *   alpha:      the title's alpha plane, ow x oh bytes, or NULL (opaque)
 *   mode:       NV2A_TEXPACK_ALPHA_*
 *   cut:        an alpha-tested draw binds the texture (nv2a_texmip_cut_ref,
 *               0: none): the levels below are made with coverage-preserving
 *               alpha (nv2a_texmip.h), as for the title's own textures
 *   fmt_hint:   the format asked for (BGRA8, or BC1/BC3 to compress to; BC1
 *               is changed to BC3 when the result has any alpha below 255)
 * pixels is changed (alpha merged) and freed by the caller. out->data is
 * malloc'd; free it with nv2a_texpack_img_free. 0 on failure (no memory,
 * a size over the limit). */
int  nv2a_texpack_build(uint8_t *pixels, uint32_t w, uint32_t h, int has_alpha, int mode,
                        const uint8_t *alpha, uint32_t ow, uint32_t oh, uint32_t olevels,
                        uint32_t cut, uint32_t fmt_hint, struct nv2a_texpack_img *out);
void nv2a_texpack_img_free(struct nv2a_texpack_img *img);
/* The title's alpha plane scaled bilinearly to pw x ph (texel centres as the
 * GPU's filter reads them), into dst (pw * ph bytes). */
void nv2a_texpack_alpha_scale(const uint8_t *alpha, uint32_t ow, uint32_t oh, uint8_t *dst,
                              uint32_t pw, uint32_t ph);

/* A PNG file, decoded: w x h BGRA8, malloc'd (free()). has_alpha: the file has
 * an alpha channel. NULL when it cannot be read or is larger than
 * NV2A_TEXPACK_MAX_SIDE. */
#define NV2A_TEXPACK_MAX_SIDE 8192u
uint8_t *nv2a_texpack_png_read(const char *path, uint32_t *w, uint32_t *h, int *has_alpha);
/* w x h BGRA8 bytes written as an RGBA PNG. 1 on success. */
int      nv2a_texpack_png_write(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h);

/* ── the jobs ─────────────────────────────────────────────────────────── */

struct nv2a_texpack_req {
    uint32_t       id;         /* nv2a_texpack_find */
    uint32_t       w, h, levels;   /* the title's texture */
    uint32_t       cut;        /* nv2a_texmip_cut_ref of the draw that binds it, or 0 */
    const uint8_t *alpha;      /* its level-0 alpha, w*h bytes (copied), NULL: all 255 */
    uint32_t       gen;        /* nv2a_texpack_generation when id was found, 0: any. With it
                                * a job for packs that changed since fails rather than
                                * read another file (packs set from another thread) */
};
enum {
    NV2A_TEXPACK_PENDING = 0,  /* queued or being made: ask again later */
    NV2A_TEXPACK_READY   = 1,  /* *img is the result (valid until release) */
    NV2A_TEXPACK_FAILED  = 2   /* unreadable PNG or too large: leave the title's texture */
};
/* Starts (or joins) the job for an entry: a token > 0, or 0 when the queue is
 * full (the memory bound on queued and finished results): ask again a few
 * flips later. Several textures with the same id and size share one job.
 * Render thread. */
uint32_t nv2a_texpack_request(const struct nv2a_texpack_req *r);
int      nv2a_texpack_poll(uint32_t token, const struct nv2a_texpack_img **img);
/* The entry has taken the result or gone: drops its reference; the result is
 * freed with the last one, a queued job nobody wants is not run. */
void     nv2a_texpack_release(uint32_t token);

/* ── the budget ───────────────────────────────────────────────────────── */

/* The bytes of replaced textures a backend keeps on the graphics card
 * (RECOMP_DEBUG=texpack_cap_mb; default 1024 on desktops, 256 on handhelds
 * and Android). Over it the least recently used replaced texture goes back to
 * the title's. */
uint64_t nv2a_texpack_cap_bytes(void);
/* The bytes a backend may upload for swaps in one flip (RECOMP_DEBUG=
 * texpack_swap_mb, default 24); one result larger than that goes alone. */
size_t   nv2a_texpack_swap_bytes(void);
/* Whether a result of `bytes` may be uploaded now with `left` of the flip's
 * budget unspent: it fits, or it is the flip's first. */
int      nv2a_texpack_swap_fits(size_t bytes, size_t left);

/* ── the disk cache ───────────────────────────────────────────────────── */

/* The folder for compressed results (per user, beside the shader cache), and
 * its size bound (RECOMP_DEBUG=texpack_cache_mb, default 4096; 0: none).
 * NULL or "": no disk cache. Once, by the backend. */
void nv2a_texpack_cache_dir(const char *dir);

/* ── counters ─────────────────────────────────────────────────────────── */

struct nv2a_texpack_stats {
    uint64_t keys, packs;
    uint64_t requests, decoded, from_cache, failed, cache_written;
    uint64_t swaps, demoted, over_cap;
    uint64_t resident_bytes, original_bytes;    /* the backends' part */
    uint64_t cpu_bytes;                          /* queued and finished results */
    double   decode_ms, compress_ms;             /* the workers' time */
    unsigned queued;
};
void nv2a_texpack_stats(struct nv2a_texpack_stats *s);
struct nv2a_texpack_stats *nv2a_texpack_stats_rw(void);
/* RECOMP_DEBUG=texpack_log: one line per job and swap. */
int  nv2a_texpack_log(void);

/* For a test or a cache-filling run: waits until the workers are idle. 1 when
 * they are. */
int  nv2a_texpack_flush(unsigned timeout_ms);

/* ── the dump ─────────────────────────────────────────────────────────── */

/* With RECOMP_DEBUG=texrep_dump=<dir>: the backends call this for each texture
 * they decode from guest memory (before any replacement), and the first time
 * a key is seen it is written as <dir>/<key name> (a pack's file name: the
 * mod maker's starting point), with a line in <dir>/index.tsv:
 * hash, palette hash (0 when none), w, h, NV2A format, levels. texels: the
 * decoded level 0 (A8R8G8B8 words). texrep_dump_mips adds the title's own
 * levels as <key>_m<level>.png (not part of a pack). Cheap when the switch is
 * off (one flag). */
void nv2a_texpack_dump(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                       uint32_t pitch, uint32_t levels, const uint8_t *pal, uint32_t pal_len,
                       const uint32_t *texels);
int  nv2a_texpack_dump_on(void);

#ifdef __cplusplus
}
#endif

#endif

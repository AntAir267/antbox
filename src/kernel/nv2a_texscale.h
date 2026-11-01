/**
 * nv2a_texscale.h - a title's 2D art made larger before it is uploaded.
 *
 * PPSSPP's texture scaling, for the 2D art only: a texture the title draws
 * its HUD, menus and fonts with is scaled 2x or 4x on the CPU, once, when a
 * GPU backend uploads it (graphics.ui_scale, RECOMP_GFX_UI_SCALE; off by
 * default). The title samples these with normalised coordinates, so a larger
 * texture needs no other change; the backend keeps the guest size for
 * everything it computes from a size.
 *
 * Which textures: a rule on how a texture is used, not a list (nothing here
 * knows a title's files). nv2a_texscale_draw_is_2d() says whether a draw is
 * a 2D one, nv2a_texscale_tex_ok() whether a texture may be scaled at all;
 * a backend scales every texture a 2D draw binds.
 *
 * The scalers work on premultiplied colour (a transparent texel's colour has
 * no say), read past a texture's edge by repeating the edge texel, and give
 * back straight alpha as the title's blending expects. Fully transparent
 * output texels take the colour of the visible ones beside them, so the
 * GPU's bilinear filter does not mix a stray colour into an outline.
 *
 * Super-xBR is Hyllian's algorithm (MIT, see the .c file); the bicubic is a
 * plain Catmull-Rom for comparison.
 *
 * No OS or graphics headers: shared by the Metal and Vulkan backends.
 */
#ifndef NV2A_TEXSCALE_H
#define NV2A_TEXSCALE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The scalers. The numbers are in the cache files' headers: add at the end. */
enum nv2a_texscale_algo {
    NV2A_TEXSCALE_SUPERXBR = 0,   /* Hyllian's Super-xBR: follows edges. The one in use */
    NV2A_TEXSCALE_BICUBIC  = 1,   /* Catmull-Rom, for comparison (RECOMP_DEBUG=ui_scale_algo=bicubic) */
    NV2A_TEXSCALE_ALGOS
};
/* A scaler's name ("super-xbr", "bicubic"), and the scaler of a name
 * (SUPERXBR for one that is not known). */
const char *nv2a_texscale_algo_name(enum nv2a_texscale_algo algo);
enum nv2a_texscale_algo nv2a_texscale_algo_named(const char *name);

/* The largest texture scaled (texels of the guest's level 0). */
#define NV2A_TEXSCALE_MAX_TEXELS (512u * 512u)

/* src: w x h A8R8G8B8, straight alpha, row-major. dst: (w * factor) x
 * (h * factor), the same layout. factor 2 or 4. Returns 0 (dst untouched) for
 * another factor, an unknown algorithm, a zero size or no memory. The same
 * input gives the same output on every machine built from the same source
 * with the same compiler: plain float arithmetic, each texel computed by
 * itself (a large texture's rows are shared among up to four threads, which
 * changes no value). */
int nv2a_texscale(const uint32_t *src, uint32_t w, uint32_t h, unsigned factor,
                  enum nv2a_texscale_algo algo, uint32_t *dst);

/* Levels 1..levels-1 of a texture whose level 0 is w x h at texels, each a
 * 2x2 box of the one above, alpha-weighted (a transparent texel's colour
 * does not darken its neighbours). texels holds every level, level 0 first. */
void nv2a_texscale_mips(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels);

/* ── the setting ─────────────────────────────────────────────────────── */

/* Held off: the factor is 0 whatever is set. Off by default on every
 * platform; RECOMP_DEBUG=ui_scale_hold=1 holds it anywhere (a way to rule
 * the upscaling out when a GPU fault is chased, BO-128). The overlay does not
 * show the row while it is held. */
int nv2a_texscale_held(void);

/* 0 (off), 2 or 4; any other value is taken as off; 0 while held. Any
 * thread; a change moves the generation on. */
void     nv2a_texscale_set_factor(unsigned factor);
unsigned nv2a_texscale_factor(void);
void     nv2a_texscale_set_algo(enum nv2a_texscale_algo algo);
enum nv2a_texscale_algo nv2a_texscale_algo(void);
/* Never 0; changes whenever the factor or the algorithm does. A backend
 * keeps it with a cache entry it scaled and uploads the entry again when it
 * has moved on. */
uint32_t nv2a_texscale_generation(void);

/* ── the rule ────────────────────────────────────────────────────────── */

/* What a backend knows of the draw that binds a texture. */
struct nv2a_texscale_draw {
    int      program;        /* runs a vertex program (not pre-transformed) */
    int      depth_test;     /* depth test enabled */
    int      depth_write;    /* depth test enabled and writing */
    uint32_t target_w, target_h;   /* the colour surface drawn into (guest) */
    uint32_t frame_w, frame_h;     /* the display's frame (guest) */
};

/* A 2D draw: no depth test and no depth write, into the frame's surface
 * (not one of the title's smaller effect targets). */
int nv2a_texscale_draw_is_2d(const struct nv2a_texscale_draw *d);

/* A texture that may be scaled: decoded from guest memory by a backend's
 * ordinary 2D path (the caller never asks for a render target, a cube map or
 * a signed/bump texture), a swizzled or DXT format (a linear one is a video
 * frame or a copy of the screen), one level, and no larger than
 * NV2A_TEXSCALE_MAX_TEXELS. size_from_format: nv2a_tex_size_from_format(). */
int nv2a_texscale_tex_ok(int size_from_format, uint32_t w, uint32_t h,
                         uint32_t levels);

/* Art cut out with alpha: at least one texel in sixteen of the decoded level
 * 0 is not opaque. Not part of the rule a 2D draw is judged by (every
 * texture such a draw binds is scaled, opaque pictures included); the pass
 * that prepares loaded textures when the setting is switched on, which does
 * not know how a texture is drawn, takes only these. */
int nv2a_texscale_has_alpha(const uint32_t *texels, size_t n);

/* ── the store: off the render thread, and on disk ───────────────────── */

/* Part of every cache file's name and header: raise it when a scaler's
 * output changes, and the files of the old one are no longer read. */
#define NV2A_TEXSCALE_VERSION 2

/* A texture's key: a hash of its decoded texels (after any replacement) and
 * its size. Never 0. */
uint64_t nv2a_texscale_key(const uint32_t *texels, uint32_t w, uint32_t h);

/* The directory of the disk cache (per user, beside the shader cache: what
 * is in it is made from the user's own disc and stays there). NULL or "":
 * no disk cache. RECOMP_DEBUG=ui_scale_cache_mb=<n> sets its size bound
 * (default 256; 0: no disk cache). cache_path: 1 and the directory when one
 * is in use. */
void nv2a_texscale_cache_dir(const char *dir);
int  nv2a_texscale_cache_path(char *out, size_t n);

enum {
    NV2A_TEXSCALE_MISS    = 0,   /* not known and not queued (no texels given, or no room) */
    NV2A_TEXSCALE_READY   = 1,   /* dst holds the scaled level 0; the store forgets it */
    NV2A_TEXSCALE_PENDING = 2,   /* queued, or being made: ask again later */
    NV2A_TEXSCALE_FAILED  = 3    /* the scaler refused it: do not ask again */
};
/* For get(): the result is wanted on disk only (made ahead of use). */
#define NV2A_TEXSCALE_DISK_ONLY 1u

/* The scaled picture of the texture `key` (w x h, scaled by factor with
 * algo), without scaling on the caller's thread. A result that is ready is
 * copied to dst ((w * factor) x (h * factor) texels) and forgotten. Else,
 * with texels (the decoded level 0, copied here), a job is queued for the
 * worker thread, which loads the picture from the disk cache or scales it
 * and writes it there; without texels the call only asks. A backend uploads
 * the title's own texture meanwhile and asks again at a later bind. */
int nv2a_texscale_get(uint64_t key, const uint32_t *texels, uint32_t w, uint32_t h,
                      unsigned factor, enum nv2a_texscale_algo algo, unsigned flags,
                      uint32_t *dst);

/* Waits until the worker has nothing left to do, timeout_ms at most. 1: it
 * has nothing left. For a run that fills the cache and then exits. */
int nv2a_texscale_flush(unsigned timeout_ms);

/* The jobs of the batch in hand (a batch starts when a job comes to an idle
 * worker): done and queued in all. done == total: nothing is being made. */
void nv2a_texscale_progress(unsigned *done, unsigned *total);

struct nv2a_texscale_store_stats {
    uint64_t hits, misses;        /* jobs served from disk / scaled */
    uint64_t written, bad;        /* files written / found damaged and removed */
    uint64_t load_usec, scale_usec;
    uint64_t dir_bytes;           /* the cache directory, as last counted */
};
void nv2a_texscale_store_stats(struct nv2a_texscale_store_stats *st);

/* For tests and tools: jobs run on the caller's thread inside get() (1), and
 * the size bound applied now. */
void nv2a_texscale_set_sync(int on);
void nv2a_texscale_cache_trim(uint64_t limit_bytes);

/* Counters for a report and the tests: textures scaled, texels made, and the
 * microseconds spent scaling. note() is for a backend, after it scaled. */
void nv2a_texscale_note(uint32_t w, uint32_t h, unsigned factor, uint64_t usec);
void nv2a_texscale_stats(uint64_t *textures, uint64_t *texels, uint64_t *usec);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEXSCALE_H */

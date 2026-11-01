/**
 * nv2a_texup.h - the world's textures upscaled on the GPU while playing
 * (BO-245; docs/research/texture-upscale-design.md in the Burnout 3 repo).
 *
 * PPSSPP's way: a texture the title draws its 3D world with is shown as it
 * is at first sight; once it has stayed the same for two flips it becomes a
 * job. The job looks in a disk cache first; on a miss a GPU backend runs a
 * compute upscaler over it (FSRCNNX x2 8-0-4-1 or NNEDI3, LGPL-3.0 shader
 * files loaded at runtime, third_party/upscale_shaders) in small chunks of
 * rows, as many a frame as a frame-time governor allows, and swaps the
 * result in when it is complete. Nothing waits: a frame never waits on a
 * chunk, a file or a fence, and a texture that does not get its turn stays
 * the title's.
 *
 * The result: level 0 is the model's output (4x: levels 0 and 1, the two
 * 2x steps), the title's own levels follow unchanged, and the sampler's LOD
 * clamps move down by the levels added. Colour: the model runs on the
 * BT.709 luma of the gamma-coded RGB; the bilinear 2x colour is moved by the
 * model's luma minus its own. Alpha: the title's, nearest, never the model's
 * (alpha kill, cut-outs and shine masks stay what they are).
 *
 * This file is the shared, OS-free part: the settings, the rule, the keys,
 * the governor's arithmetic, the memory cap and the disk store (a worker
 * thread). The backends own every GPU object.
 *
 * Off (the default): nothing here is asked, nothing is hashed or queued, the
 * upload path is the one without this file.
 */
#ifndef NV2A_TEXUP_H
#define NV2A_TEXUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── the settings ─────────────────────────────────────────────────────── */

enum nv2a_texup_algo {
    NV2A_TEXUP_OFF     = 0,
    NV2A_TEXUP_FSRCNNX = 1,
    NV2A_TEXUP_NNEDI3  = 2,
    NV2A_TEXUP_ALGOS
};
const char *nv2a_texup_algo_name(int algo);   /* "off", "fsrcnnx", "nnedi3" */

/* graphics.texture_upscale; any thread. */
void nv2a_texup_set_algo(int algo);
int  nv2a_texup_algo(void);
/* graphics.texture_upscale_scale: 2 or 4 (4: inputs up to 256 a side; the
 * larger ones get 2x). */
void     nv2a_texup_set_scale(unsigned scale);
unsigned nv2a_texup_scale(void);
/* The largest scale offered here: 2 on a Steam Deck (recomp_host_is_steam_deck:
 * SteamDeck=1 or the Deck's DMI board), else 4;
 * set_scale and the environment never go past it. */
unsigned nv2a_texup_max_scale(void);
/* graphics.texup_roads: 0 (default) the title's road surfaces stay as they
 * are (the skip list), 1 they are upscaled too. */
void nv2a_texup_set_roads(int on);
int  nv2a_texup_roads(void);
/* Never 0; moves on whenever any of the three changes. A backend keeps it
 * with a texture and uploads the texture again (the title's) when it has
 * moved on, which is how Off goes back to today's textures. */
uint32_t nv2a_texup_generation(void);

/* The backends at every flip: RECOMP_DEBUG=texup_ctl=<file> (a test's live
 * switch): every 30 flips the file's numbers are the algorithm, and
 * optionally the scale and the road switch ("1 2 0"). */
void nv2a_texup_ctl(uint64_t flips);

/* A backend that can upscale says so once it is up (Metal; Vulkan in
 * phase 2); the overlay shows the rows only then. */
void nv2a_texup_set_supported(int on);
int  nv2a_texup_supported(void);

/* The title's loading signal (BO-272). The track's textures are first drawn
 * in the countdown, so their jobs are best run before the GO: a title says
 * where it is, once a frame, from any thread:
 *   NV2A_TEXUP_PLAY      its player can be driving (the default)
 *   NV2A_TEXUP_PRESTART  a stage is drawn but nobody drives yet (the
 *                        countdown before the GO)
 *   NV2A_TEXUP_LOADING   its loading screen (the stage is being set up)
 * While it is not PLAY the governor gives the upscaler a larger budget (still
 * within the frame's GPU room), several jobs a flip and the idle swap slice;
 * a title that never calls it gets the in-race rules everywhere. */
enum { NV2A_TEXUP_PLAY = 0, NV2A_TEXUP_PRESTART = 1, NV2A_TEXUP_LOADING = 2 };
void nv2a_texup_set_loading(int how);
int  nv2a_texup_loading(void);
/* graphics.texture_upscale_preload: 1 (default) the backends act on the
 * signal above; 0 they take every frame as PLAY and run as before BO-272
 * (the jobs spread through the race). Any thread. */
void nv2a_texup_set_preload(int on);
int  nv2a_texup_preload(void);

/* The factor a w x h texture gets with the current setting: 0, 2 or 4. */
unsigned nv2a_texup_factor_for(uint32_t w, uint32_t h);

/* ── the rule ─────────────────────────────────────────────────────────── */

/* What a backend knows of a texture at a bind. */
struct nv2a_texup_tex {
    int      size_from_format;   /* nv2a_tex_size_from_format: swizzled or DXT colour */
    int      linear;             /* a linear (pitch) format: a video frame, a screen copy */
    int      sign;               /* read as signed (bump, normal) */
    int      cube;
    int      replaced;           /* nv2a_texrep replaced its texels */
    int      render_target;      /* bound from a render target */
    uint32_t w, h, levels;
    int      draw_3d;            /* the draw: a vertex program and the depth test on */
    int      ever_2d;            /* a 2D draw has bound it (the HUD's, ui_scale's) */
};

enum nv2a_texup_why {
    NV2A_TEXUP_OK = 0,
    NV2A_TEXUP_NOT_COLOUR,       /* not a swizzled/DXT colour format, or signed */
    NV2A_TEXUP_KIND,             /* cube, linear, render target, replaced */
    NV2A_TEXUP_SIZE,             /* a side under 16 or over the platform's limit */
    NV2A_TEXUP_2D,               /* a 2D draw binds it, or no 3D draw does yet */
};
/* NV2A_TEXUP_OK when the texture may become a job (it must still stay the
 * same for two flips: the backend's part). */
int nv2a_texup_rule(const struct nv2a_texup_tex *t);

/* The largest side upscaled: 1024 on desktops, 512 on handhelds and
 * Android (RECOMP_DEBUG=texup_max=<n>). */
uint32_t nv2a_texup_max_side(void);

/* The road skip list: the title's texrep hashes (nv2a_texrep_hash) of its
 * road surfaces, one hex hash a line ('#' comments), loaded once from
 * nv2a_texup_set_skip_file(). 1: leave this texture as it is (in the list
 * and graphics.texup_roads off). */
void nv2a_texup_set_skip_file(const char *path);
/* A title's built-in list (its hashes are facts about its data, so a title
 * can carry them in its code); merged with the file's. Copied. */
void nv2a_texup_set_skip_list(const uint64_t *hashes, size_t n);
int  nv2a_texup_skip(uint64_t texrep_hash);
int  nv2a_texup_is_road(uint64_t texrep_hash);

/* ── the key ──────────────────────────────────────────────────────────── */

/* Part of every cache file's name: raise it when this code changes what a
 * result is (the colour rule, the alpha rule, the file layout). The shader
 * files' own hash (VERSION beside them) is the other half. */
/* 2 (BO-245): results are checked against the title's texels before they
 * are swapped in or saved (nv2a_texup_result_ok); files from before may hold
 * a result a GPU fault zeroed (the Deck's black Super Type 1, NNEDI3). */
#define NV2A_TEXUP_POLICY 2

struct nv2a_texup_key {
    uint64_t hash;               /* the backend's content hash: the guest bytes and the palette */
    uint32_t w, h;               /* the title's level 0 */
    uint32_t format;             /* the NV2A colour format */
    uint32_t algo, scale;        /* NV2A_TEXUP_*, 2 or 4 */
    uint32_t wrap;               /* bit 0: wraps in u, bit 1: in v (the edge rule the model read with) */
    uint64_t shaders;            /* nv2a_texup_shader_version() */
};
/* One number for a key (never 0), and the cache file's name. */
uint64_t nv2a_texup_key_id(const struct nv2a_texup_key *k);
void     nv2a_texup_key_name(const struct nv2a_texup_key *k, char *out, size_t n);

/* The shaders: the directory they were loaded from and the 64-bit version
 * of what is there (VERSION). set_ by the backend that loaded them. */
void        nv2a_texup_set_shaders(const char *dir, uint64_t version);
uint64_t    nv2a_texup_shader_version(void);
/* The directory to load the shader files from: RECOMP_DEBUG=texup_shaders,
 * else the user's override folder when it has a VERSION file, else the
 * first of `candidates` (NULL-terminated; the backend's bundle and
 * executable folders) with one, else the source tree's (a build from
 * source). 0: none found. */
int nv2a_texup_shader_dir(const char *const *candidates, char *out, size_t n);

/* ── the governor ─────────────────────────────────────────────────────── */

/* The numbers a backend feeds it once a flip, and what it says back. Plain
 * arithmetic, no clock of its own (tests drive it). */
#define NV2A_TEXUP_GOV_HIST 60
struct nv2a_texup_gov {
    double   gpu_ms[NV2A_TEXUP_GOV_HIST];   /* the frames' GPU time, newest at pos-1 */
    unsigned pos, n;
    double   period_ms;          /* the display's (or the title's) frame period */
    double   factor[NV2A_TEXUP_ALGOS];      /* measured / predicted, per algorithm (EMA) */
    uint64_t quiet_until_ns;     /* no compute before this (a burst of uploads) */
    uint64_t pause_until_ns;     /* no compute before this (frames over the period) */
    double   pause_s;            /* the next pause's length (doubles, up to 32 s) */
    uint64_t clean_since_ns;     /* since when no pause was needed */
    int      over_run;           /* frames over the period in a row while computing */
    int      racing;             /* the last frame drew 3D */
    int      active;             /* compute ran in the last two frames */
    double   cap_racing_ms, cap_idle_ms, margin_ms;
    uint64_t pauses, frames, frames_over, chunks_ms_x1000;
    /* BO-272: NV2A_TEXUP_PLAY / PRESTART / LOADING (nv2a_texup_gov_loading). */
    int      loading;
    int      skip;               /* PRESTART: the frame after a late one computes nothing */
    double   cap_load_ms;        /* LOADING's cap; PRESTART's is cap_prestart_ms */
    double   cap_prestart_ms;
    unsigned measured;           /* chunk buffers measured (nv2a_texup_gov_measured counted) */
};
void nv2a_texup_gov_init(struct nv2a_texup_gov *g);
/* Once a flip. gpu_ms: the GPU time of the last frame the backend knows to
 * be complete (< 0: not known). frame_ms: the host interval from the last
 * flip. draws_3d, uploads: the frame's. computed: this frame dispatched
 * chunks. */
void nv2a_texup_gov_frame(struct nv2a_texup_gov *g, uint64_t now_ns, double gpu_ms,
                          double frame_ms, double period_ms, unsigned draws_3d,
                          unsigned uploads, int computed);
/* Once a flip, before nv2a_texup_gov_frame: where the title is
 * (nv2a_texup_loading). Entering LOADING or PRESTART ends an upload-burst
 * quiet and a late-frame pause (a load is the burst). While it is not PLAY:
 *   - an upload burst does not stop compute;
 *   - LOADING: late frames are the title's own load and pause nothing;
 *     PRESTART: a late frame while computing skips the next frame's compute
 *     (no doubling pause: the window is a few seconds);
 *   - the budget is the frame's GPU room (period - margin - GPU p95) up to
 *     cap_load_ms (6) or cap_prestart_ms (4) instead of 0.5 (the cost table
 *     can be 1.5x off before the governor has measured a buffer: until it
 *     has measured three, at most 2 ms);
 *   - nv2a_texup_gov_jobs allows several new jobs a flip and the swap slice
 *     is 32 MB. */
void nv2a_texup_gov_loading(struct nv2a_texup_gov *g, int loading);
/* The jobs a backend may start this flip: 1 in play, 4 in PRESTART, 16 while
 * LOADING (a backend also stops when its own render-thread time for the
 * flip passes nv2a_texup_gov_rt_ms). */
unsigned nv2a_texup_gov_jobs(const struct nv2a_texup_gov *g);
double   nv2a_texup_gov_rt_ms(const struct nv2a_texup_gov *g);
/* The frame period a backend should give nv2a_texup_gov_frame: the host grid's
 * when the title asked for one (graphics.high_refresh: 8.33 ms at 120 Hz),
 * else the title's swap interval at 60 Hz. host_period_ns 0: none. */
double nv2a_texup_period_ms(uint64_t host_period_ns, uint32_t interval);

/* The GPU milliseconds of compute this frame may have (0: none). */
double nv2a_texup_gov_budget(const struct nv2a_texup_gov *g, uint64_t now_ns);
/* A command buffer of chunks completed: what was predicted and what the GPU
 * took. Corrects the algorithm's factor. */
void nv2a_texup_gov_measured(struct nv2a_texup_gov *g, int algo, double predicted_ms,
                             double measured_ms);
/* The bytes a flip may upload for swaps from the disk cache. */
size_t nv2a_texup_gov_swap_bytes(const struct nv2a_texup_gov *g);
/* Whether a disk result of `bytes` may be uploaded now, with `left` of the
 * flip's `full` swap budget unspent: it fits, or it is the flip's first
 * (one result larger than the slice goes alone; a 4x result of a 256 x 256
 * texture is 5 MB against the 4 MB racing slice, and waiting for room that
 * never comes held its slot for good: BO-245 Deck 4x warm). */
int nv2a_texup_swap_fits(size_t bytes, size_t left, size_t full);

/* Predicted GPU ms of one pass over n texels of its domain (before the
 * governor's factor), from the per-device table (defaults measured on an
 * Apple M5; RECOMP_DEBUG=texup_cost=<scale> multiplies them). pass: the
 * backend's pass index (nv2a_texup_pass). */
enum nv2a_texup_pass {
    NV2A_TEXUP_P_LUMA = 0,
    NV2A_TEXUP_P_FSR_FEATURE,    /* FSRCNNX hooks 0, 1 (5x5 on the luma) */
    NV2A_TEXUP_P_FSR_MODEL,      /* hooks 2..11 (3x3 on two images) */
    NV2A_TEXUP_P_FSR_SUBCONV,    /* hook 12 */
    NV2A_TEXUP_P_NNEDI3,         /* per input texel */
    NV2A_TEXUP_P_RECOMBINE,      /* per output texel */
    NV2A_TEXUP_PASSES
};
double nv2a_texup_pass_cost(int pass, double texels);
void   nv2a_texup_set_pass_cost(int pass, double ms_per_64k);

/* ── the result check ─────────────────────────────────────────────────── */

/* A result (level 0, W x H = w*scale x h*scale, RGBA8 bytes) against the
 * title's level 0 (w x h, RGBA8 bytes, as the backend blitted it into the
 * result's lower levels), on every fourth texel each way: the alpha must be
 * the title's under it (the shaders copy it, nearest), and the mean luma
 * within 0.15 of the title's (the model only sharpens). 1: keep it; 0: a
 * broken result (a GPU fault or a lost submit leaves zeros, a NaN in the
 * model black), shown as the title's texture and not saved. */
int nv2a_texup_result_ok(const uint8_t *res, const uint8_t *title, uint32_t w, uint32_t h,
                         unsigned scale);

/* ── the memory cap ───────────────────────────────────────────────────── */

/* The upscaled textures' bytes a backend may keep (MB: RECOMP_DEBUG=
 * texup_cap_mb; default 1024 on desktops, 192 on handhelds). Over it a
 * result is not swapped in (it is still saved) and the least recently
 * used upscaled texture goes back to the title's. */
uint64_t nv2a_texup_cap_bytes(void);

/* The backends' job slots in use (RECOMP_DEBUG=texup_slots, 1..8, default 8)
 * and the flips after which a finished or unread slot is given back
 * (RECOMP_DEBUG=texup_free_flips, default 600). Both only for stress tests
 * of the slot paths. */
#define NV2A_TEXUP_SLOTS_MAX 8
unsigned nv2a_texup_slots(void);
unsigned nv2a_texup_free_flips(void);

/* ── the disk store ───────────────────────────────────────────────────── */

/* The directory (beside the shader cache: made from the user's own disc, it
 * stays there) and its size bound (RECOMP_DEBUG=texup_cache_mb, default
 * 2048; 0: no disk cache). Once, by the backend. */
void nv2a_texup_store_dir(const char *dir);

enum {
    NV2A_TEXUP_ABSENT  = 0,      /* not on disk (or no disk cache) */
    NV2A_TEXUP_READY   = 1,      /* *out: the file's texels (free() them) */
    NV2A_TEXUP_PENDING = 2       /* queued or being read: ask again later */
};
/* The result file of key k: texels w*f x h*f (and for f 4 then the 2x level
 * after it), BGRA8. The first call queues the read on the worker thread. */
int  nv2a_texup_load(const struct nv2a_texup_key *k, uint32_t **out, size_t *bytes);
/* Saves a result (copied; the worker writes it). _rgba: the texels are
 * R G B A bytes (a Vulkan RGBA8 image's), turned into the file's B G R A on
 * the worker. */
void nv2a_texup_save(const struct nv2a_texup_key *k, const uint32_t *texels, size_t bytes);
void nv2a_texup_save_rgba(const struct nv2a_texup_key *k, const uint32_t *texels, size_t bytes);
/* Waits for the worker to finish what it has (tests, a cache-filling run). */
int  nv2a_texup_store_flush(unsigned timeout_ms);

/* ── counters ─────────────────────────────────────────────────────────── */

struct nv2a_texup_stats {
    uint64_t jobs, computed, from_disk, absent, saved, bad, dropped, demoted, swaps;
    uint64_t refused;            /* results nv2a_texup_result_ok turned down */
    uint64_t resident_bytes, original_bytes;   /* the upscaled textures in use, and the title's of them */
    uint64_t chunk_buffers;
    double   chunk_gpu_ms, chunk_pred_ms;      /* all chunks so far */
    double   last_s_gpu_ms;                    /* the chunks' GPU ms in the last second */
    double   load_ms, save_ms;                 /* the worker's file time */
    unsigned upscaled, queued;                 /* now */
};
void nv2a_texup_stats(struct nv2a_texup_stats *s);
/* The backends' and the store's parts of the counters. */
struct nv2a_texup_stats *nv2a_texup_stats_rw(void);

#ifdef __cplusplus
}
#endif

#endif

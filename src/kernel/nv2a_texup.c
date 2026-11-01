/*
 * nv2a_texup.c - the world's textures upscaled on the GPU: the shared part
 * (settings, rule, keys, governor, memory cap, disk store). See nv2a_texup.h.
 */
#include "nv2a_texup.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER)
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif
#endif

#define LOGP "[TEXUP] "

/* ── the settings ─────────────────────────────────────────────────────── */

static atomic_int  s_algo;
static atomic_uint s_scale = 2;
static atomic_int  s_roads;
static atomic_int  s_preload = 1;   /* graphics.texture_upscale_preload (BO-272) */
static atomic_uint s_gen = 1;

const char *nv2a_texup_algo_name(int algo)
{
    return algo == NV2A_TEXUP_FSRCNNX ? "fsrcnnx" : algo == NV2A_TEXUP_NNEDI3 ? "nnedi3" : "off";
}

/* The starting values: the environment (RECOMP_GFX_TEXTURE_UPSCALE and its
 * two companions, which settings.toml's values also reach), read once at
 * the first look, as nv2a_texscale reads RECOMP_GFX_UI_SCALE. A set_ call
 * before that wins. */
static atomic_int s_env_read;

/* The largest factor this machine is offered: 2x on a Steam Deck (BO-245:
 * 4x cold had 90-240 ms executor stalls there and the memory to match),
 * 4x elsewhere. A 4x asked for on the Deck (settings.toml, the environment,
 * a test's control file) runs as 2x. */
unsigned nv2a_texup_max_scale(void)
{
    return recomp_host_is_steam_deck() ? 2u : 4u;
}

static void env_read(void)
{
    const char *e;
    int expect = 0;
    if (atomic_load_explicit(&s_env_read, memory_order_acquire)
            || !atomic_compare_exchange_strong(&s_env_read, &expect, 1))
        return;
    e = recomp_env(RENV_GFX_TEXUP);
    if (e && !strcmp(e, "fsrcnnx"))
        atomic_store(&s_algo, NV2A_TEXUP_FSRCNNX);
    else if (e && !strcmp(e, "nnedi3"))
        atomic_store(&s_algo, NV2A_TEXUP_NNEDI3);
    e = recomp_env(RENV_GFX_TEXUP_SCALE);
    if (e && e[0] == '4' && nv2a_texup_max_scale() >= 4)
        atomic_store(&s_scale, 4u);
    e = recomp_env(RENV_GFX_TEXUP_ROADS);
    if (e && (e[0] == '1' || !strcmp(e, "true") || !strcmp(e, "on")))
        atomic_store(&s_roads, 1);
    e = recomp_env(RENV_GFX_TEXUP_PRELOAD);
    if (e && (e[0] == '0' || !strcmp(e, "false") || !strcmp(e, "off")))
        atomic_store(&s_preload, 0);
    if (atomic_load(&s_algo))
        fprintf(stderr, LOGP "texture upscale: %s %ux%s\n", nv2a_texup_algo_name(atomic_load(&s_algo)),
                atomic_load(&s_scale), atomic_load(&s_roads) ? ", roads too" : "");
}

static void bump(void)
{
    unsigned g = atomic_fetch_add(&s_gen, 1) + 1;
    if (!g)
        atomic_store(&s_gen, 1);
}

void nv2a_texup_set_algo(int algo)
{
    env_read();
    if (algo < 0 || algo >= NV2A_TEXUP_ALGOS)
        algo = NV2A_TEXUP_OFF;
    if (atomic_exchange(&s_algo, algo) != algo) {
        bump();
        fprintf(stderr, LOGP "texture upscale: %s\n", nv2a_texup_algo_name(algo));
    }
}

int nv2a_texup_algo(void)
{
    env_read();
    return atomic_load_explicit(&s_algo, memory_order_relaxed);
}

void nv2a_texup_set_scale(unsigned scale)
{
    env_read();
    scale = scale == 4 && nv2a_texup_max_scale() >= 4 ? 4u : 2u;
    if (atomic_exchange(&s_scale, scale) != scale)
        bump();
}

unsigned nv2a_texup_scale(void)
{
    env_read();
    return atomic_load_explicit(&s_scale, memory_order_relaxed);
}

void nv2a_texup_set_roads(int on)
{
    env_read();
    on = on != 0;
    if (atomic_exchange(&s_roads, on) != on)
        bump();
}

int nv2a_texup_roads(void)
{
    env_read();
    return atomic_load_explicit(&s_roads, memory_order_relaxed);
}

uint32_t nv2a_texup_generation(void)
{
    return atomic_load_explicit(&s_gen, memory_order_relaxed);
}

static atomic_int s_supported;

void nv2a_texup_set_supported(int on)
{
    atomic_store(&s_supported, on != 0);
}

int nv2a_texup_supported(void)
{
    return atomic_load(&s_supported);
}

void nv2a_texup_ctl(uint64_t flips)
{
    static int init;
    static const char *ctl;
    char buf[64] = "";
    FILE *f;
    int a = -1, sc = 0, rd = -1;
    if (!init) {
        init = 1;
        ctl = recomp_env(RENV_TEXUP_CTL);
        if (ctl && !*ctl)
            ctl = NULL;
    }
    if (!ctl || flips % 30 || (f = fopen(ctl, "rb")) == NULL)
        return;
    if (fgets(buf, sizeof buf, f) && sscanf(buf, "%d %d %d", &a, &sc, &rd) >= 1) {
        if (a != nv2a_texup_algo())
            fprintf(stderr, LOGP "control file: %d at flip %llu\n", a, (unsigned long long)flips);
        if (sc == 2 || sc == 4)
            nv2a_texup_set_scale((unsigned)sc);
        if (rd >= 0)
            nv2a_texup_set_roads(rd);
        nv2a_texup_set_algo(a);
    }
    fclose(f);
}

unsigned nv2a_texup_factor_for(uint32_t w, uint32_t h)
{
    if (!nv2a_texup_algo())
        return 0;
    if (nv2a_texup_scale() == 4 && w <= 256 && h <= 256)
        return 4;
    return 2;
}

/* ── the rule ─────────────────────────────────────────────────────────── */

static int handheld(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    return recomp_host_is_steam_deck();
#endif
}

uint32_t nv2a_texup_max_side(void)
{
    static uint32_t m;
    if (!m) {
        const char *e = recomp_env(RENV_TEXUP_MAX);
        m = e && atoi(e) >= 16 ? (uint32_t)atoi(e) : handheld() ? 512u : 1024u;
    }
    return m;
}

int nv2a_texup_rule(const struct nv2a_texup_tex *t)
{
    uint32_t m = nv2a_texup_max_side();
    if (!t->size_from_format || t->sign)
        return NV2A_TEXUP_NOT_COLOUR;
    if (t->cube || t->linear || t->render_target || t->replaced)
        return NV2A_TEXUP_KIND;
    if (t->w < 16 || t->h < 16 || t->w > m || t->h > m)
        return NV2A_TEXUP_SIZE;
    if (t->ever_2d || !t->draw_3d)
        return NV2A_TEXUP_2D;
    return NV2A_TEXUP_OK;
}

/* The road skip list: sorted hashes. */
static uint64_t *s_skip;
static size_t    s_nskip;
static int       s_skip_read;
static char      s_skip_path[1024];

static int u64cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

void nv2a_texup_set_skip_file(const char *path)
{
    snprintf(s_skip_path, sizeof s_skip_path, "%s", path ? path : "");
    s_skip_read = 0;
}

static uint64_t *s_builtin;
static size_t    s_nbuiltin;

void nv2a_texup_set_skip_list(const uint64_t *hashes, size_t n)
{
    free(s_builtin);
    s_builtin = n ? (uint64_t *)malloc(n * sizeof *s_builtin) : NULL;
    s_nbuiltin = s_builtin ? n : 0;
    if (s_builtin)
        memcpy(s_builtin, hashes, n * sizeof *s_builtin);
    s_skip_read = 0;
}

static void skip_read(void)
{
    const char *e = recomp_env(RENV_TEXUP_SKIP);
    const char *path = e && *e ? e : s_skip_path;
    char line[256];
    size_t cap = 0;
    FILE *f;

    s_skip_read = 1;
    free(s_skip);
    s_skip = NULL;
    s_nskip = 0;
    if (s_nbuiltin) {
        s_skip = (uint64_t *)malloc(s_nbuiltin * sizeof *s_skip);
        if (s_skip) {
            memcpy(s_skip, s_builtin, s_nbuiltin * sizeof *s_skip);
            s_nskip = cap = s_nbuiltin;
        }
    }
    f = path[0] ? fopen(path, "rb") : NULL;
    while (f && fgets(line, sizeof line, f)) {
        char *end;
        uint64_t h;
        if (line[0] == '#')
            continue;
        h = strtoull(line, &end, 16);
        if (end == line)
            continue;
        if (s_nskip == cap) {
            uint64_t *p = (uint64_t *)realloc(s_skip, (cap ? cap * 2 : 64) * sizeof *p);
            if (!p)
                break;
            s_skip = p;
            cap = cap ? cap * 2 : 64;
        }
        s_skip[s_nskip++] = h;
    }
    if (f)
        fclose(f);
    qsort(s_skip, s_nskip, sizeof *s_skip, u64cmp);
    if (path[0] || s_nbuiltin)
        fprintf(stderr, LOGP "road skip list%s%s: %zu textures\n", path[0] ? " " : "", path,
                s_nskip);
}

int nv2a_texup_is_road(uint64_t h)
{
    if (!s_skip_read)
        skip_read();
    return s_nskip && bsearch(&h, s_skip, s_nskip, sizeof h, u64cmp) != NULL;
}

int nv2a_texup_skip(uint64_t h)
{
    return !nv2a_texup_roads() && nv2a_texup_is_road(h);
}

/* ── the key ──────────────────────────────────────────────────────────── */

static uint64_t mix(uint64_t x, uint64_t v)
{
    x ^= v;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 29;
    return x;
}

uint64_t nv2a_texup_key_id(const struct nv2a_texup_key *k)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    x = mix(x, k->hash);
    x = mix(x, (uint64_t)k->w << 32 | k->h);
    x = mix(x, (uint64_t)k->format << 32 | k->algo << 16 | k->scale << 8 | k->wrap);
    x = mix(x, k->shaders);
    x = mix(x, NV2A_TEXUP_POLICY);
    x ^= x >> 33;
    return x ? x : 1;
}

void nv2a_texup_key_name(const struct nv2a_texup_key *k, char *out, size_t n)
{
    snprintf(out, n, "%016llx_%ux%u_f%02x_%s_x%u_w%u_s%016llx_p%u.tup",
             (unsigned long long)k->hash, k->w, k->h, k->format & 0xFF,
             nv2a_texup_algo_name((int)k->algo), k->scale, k->wrap,
             (unsigned long long)k->shaders, (unsigned)NV2A_TEXUP_POLICY);
}

static char     s_sh_dir[1024];
static uint64_t s_sh_ver;

void nv2a_texup_set_shaders(const char *dir, uint64_t version)
{
    snprintf(s_sh_dir, sizeof s_sh_dir, "%s", dir ? dir : "");
    s_sh_ver = version;
}

uint64_t nv2a_texup_shader_version(void)
{
    return s_sh_ver;
}

static int has_version(const char *dir)
{
    char p[1200];
    FILE *f;
    snprintf(p, sizeof p, "%s/VERSION", dir);
    f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int nv2a_texup_shader_dir(const char *const *cand, char *out, size_t n)
{
    const char *e = recomp_env(RENV_TEXUP_SHADERS);
    if (e && *e) {
        snprintf(out, n, "%s", e);
        return has_version(out);
    }
    for (; cand && *cand; cand++) {
        if ((*cand)[0] && has_version(*cand)) {
            snprintf(out, n, "%s", *cand);
            return 1;
        }
    }
#if defined(XBOXRECOMP_TEXUP_SRC_DIR)
    if (has_version(XBOXRECOMP_TEXUP_SRC_DIR)) {
        snprintf(out, n, "%s", XBOXRECOMP_TEXUP_SRC_DIR);
        return 1;
    }
#endif
    return 0;
}

/* ── the governor ─────────────────────────────────────────────────────── */

void nv2a_texup_gov_init(struct nv2a_texup_gov *g)
{
    const char *e = recomp_env(RENV_TEXUP_CAP_MS);
    int a;
    memset(g, 0, sizeof *g);
    g->period_ms = 1000.0 / 60.0;
    for (a = 0; a < NV2A_TEXUP_ALGOS; a++)
        g->factor[a] = 1.0;
    g->cap_racing_ms = 0.5;
    g->cap_idle_ms = 4.0;
    g->margin_ms = 2.0;
    g->pause_s = 2.0;
    g->cap_load_ms = 6.0;
    g->cap_prestart_ms = 4.0;
    /* RECOMP_DEBUG=texup_cap_ms=racing[,idle[,prestart[,loading]]] */
    if (e && *e) {
        const char *c = strchr(e, ',');
        if (atof(e) >= 0)
            g->cap_racing_ms = atof(e);
        if (c && atof(c + 1) >= 0)
            g->cap_idle_ms = atof(c + 1);
        if (c && (c = strchr(c + 1, ',')) != NULL && atof(c + 1) >= 0)
            g->cap_prestart_ms = atof(c + 1);
        if (c && (c = strchr(c + 1, ',')) != NULL && atof(c + 1) >= 0)
            g->cap_load_ms = atof(c + 1);
    }
}

/* ── the title's loading signal (BO-272) ──────────────────────────────── */

static atomic_int s_loading;

void nv2a_texup_set_loading(int how)
{
    atomic_store(&s_loading, how == NV2A_TEXUP_LOADING || how == NV2A_TEXUP_PRESTART ? how : 0);
}

int nv2a_texup_loading(void)
{
    return atomic_load(&s_loading);
}

void nv2a_texup_set_preload(int on)
{
    env_read();
    atomic_store(&s_preload, on != 0);
}

int nv2a_texup_preload(void)
{
    env_read();
    return atomic_load_explicit(&s_preload, memory_order_relaxed);
}

void nv2a_texup_gov_loading(struct nv2a_texup_gov *g, int loading)
{
    if (loading && !g->loading) {
        /* A load is the upload burst; a pause taught in play says nothing
         * about it. */
        g->quiet_until_ns = 0;
        g->pause_until_ns = 0;
        g->over_run = 0;
    }
    if (!loading)
        g->skip = 0;
    g->loading = loading;
}

unsigned nv2a_texup_gov_jobs(const struct nv2a_texup_gov *g)
{
    return g->loading == NV2A_TEXUP_LOADING ? 16u : g->loading == NV2A_TEXUP_PRESTART ? 4u : 1u;
}

double nv2a_texup_gov_rt_ms(const struct nv2a_texup_gov *g)
{
    return g->loading == NV2A_TEXUP_LOADING ? 6.0 : g->loading == NV2A_TEXUP_PRESTART ? 1.5 : 0.0;
}

double nv2a_texup_period_ms(uint64_t host_period_ns, uint32_t interval)
{
    if (host_period_ns >= 2000000u && host_period_ns <= 100000000u)
        return (double)host_period_ns / 1e6;
    return (double)(interval ? interval : 1) * 1000.0 / 60.0;
}

static double gpu_p95(const struct nv2a_texup_gov *g)
{
    double v[NV2A_TEXUP_GOV_HIST];
    unsigned i, j, n = g->n;
    if (!n)
        return -1.0;
    memcpy(v, g->gpu_ms, sizeof v);
    for (i = 1; i < n; i++) {        /* insertion sort: 60 numbers */
        double x = v[i];
        for (j = i; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
}

void nv2a_texup_gov_frame(struct nv2a_texup_gov *g, uint64_t now_ns, double gpu_ms,
                          double frame_ms, double period_ms, unsigned draws_3d,
                          unsigned uploads, int computed)
{
    int over;
    g->frames++;
    if (period_ms > 1.0)
        g->period_ms = period_ms;
    if (gpu_ms >= 0) {
        g->gpu_ms[g->pos] = gpu_ms;
        g->pos = (g->pos + 1) % NV2A_TEXUP_GOV_HIST;
        if (g->n < NV2A_TEXUP_GOV_HIST)
            g->n++;
    }
    g->racing = draws_3d > 0;
    /* A burst of new textures (a track's start, a new scene): the render
     * thread is busiest now; no compute for 2 s. */
    if (uploads >= 24 && !g->loading)
        g->quiet_until_ns = now_ns + 2000000000ull;
    /* A frame late by more than half a period while compute ran in it or in
     * the one before: two in a row pause compute, 2 s, then 4, 8 ... 32. */
    over = frame_ms > g->period_ms * 1.5;
    if (over)
        g->frames_over++;
    if (g->loading) {
        /* BO-272: the title's load makes late frames of its own; in the
         * countdown a late frame while computing skips one frame. */
        g->skip = g->loading == NV2A_TEXUP_PRESTART && over && (computed || g->active);
        g->over_run = 0;
    } else if (over && (computed || g->active)) {
        if (++g->over_run >= 2) {
            g->pause_until_ns = now_ns + (uint64_t)(g->pause_s * 1e9);
            g->pauses++;
            if (recomp_env(RENV_TEXUP_LOG))
                fprintf(stderr, LOGP "governor: two late frames while upscaling, compute paused"
                        " %.0f s\n", g->pause_s);
            g->pause_s = g->pause_s * 2 > 32 ? 32 : g->pause_s * 2;
            g->over_run = 0;
            g->clean_since_ns = now_ns;
        }
    } else if (!over) {
        g->over_run = 0;
        /* A minute without a pause: the next one starts at 2 s again. */
        if (g->clean_since_ns && now_ns - g->clean_since_ns > 60000000000ull) {
            g->pause_s = 2.0;
            g->clean_since_ns = 0;
        }
    }
    g->active = computed;
}

double nv2a_texup_gov_budget(const struct nv2a_texup_gov *g, uint64_t now_ns)
{
    double cap = g->racing ? g->cap_racing_ms : g->cap_idle_ms, p95, room;
    if (g->loading) {
        cap = g->loading == NV2A_TEXUP_LOADING ? g->cap_load_ms : g->cap_prestart_ms;
        /* The cost table is the slow end of one GPU: until the factor has
         * seen a few buffers, a large budget can be well off. */
        if (g->measured < 3 && cap > 2.0)
            cap = 2.0;
        if (g->skip)
            return 0.0;
    } else if (now_ns < g->pause_until_ns || now_ns < g->quiet_until_ns) {
        return 0.0;
    }
    if (cap <= 0)
        return 0.0;
    p95 = gpu_p95(g);
    if (p95 < 0)
        return 0.0;                  /* no frame measured yet */
    room = g->period_ms - g->margin_ms - p95;
    if (room <= 0)
        return 0.0;
    return room < cap ? room : cap;
}

void nv2a_texup_gov_measured(struct nv2a_texup_gov *g, int algo, double pred, double meas)
{
    double r;
    /* A buffer of a few tiny chunks is mostly the command buffer's own cost
     * (and a slow GPU clock while little else runs): measured against a
     * prediction that small it would push the factor up, the chunks would
     * shrink, and the overhead would win more (BO-245: NNEDI3 reached 47x
     * on a paused frame). Only a buffer of 0.1 ms or more predicted counts,
     * and the factor stays within 1/4 .. 8 of the table. */
    if (algo <= 0 || algo >= NV2A_TEXUP_ALGOS || pred < 0.1 || meas < 0)
        return;
    /* The factor is measured/raw prediction: pred was made with the old
     * factor. */
    r = meas / (pred / g->factor[algo]);
    if (r < 0.05)
        r = 0.05;
    if (r > 50)
        r = 50;
    /* A slow chunk counts at once, a fast one slowly. */
    g->factor[algo] = r > g->factor[algo] ? g->factor[algo] * 0.5 + r * 0.5
                                          : g->factor[algo] * 0.9 + r * 0.1;
    if (g->factor[algo] < 0.25)
        g->factor[algo] = 0.25;
    if (g->factor[algo] > 8.0)
        g->factor[algo] = 8.0;
    g->chunks_ms_x1000 += (uint64_t)(meas * 1000.0);
    g->measured++;
}

size_t nv2a_texup_gov_swap_bytes(const struct nv2a_texup_gov *g)
{
    return g->racing && !g->loading ? (4u << 20) : (32u << 20);
}

int nv2a_texup_swap_fits(size_t bytes, size_t left, size_t full)
{
    return bytes <= left || left >= full;
}

/* ms per 64K texels of the pass's domain: Apple M5, 16-bit intermediates,
 * at the GPU's low clock (BO-245 phase 0, texup_gpu_check: the same job
 * took 0.43 to 1.5 ms as the clock moved; these are the slow end, the
 * governor's factor brings them down as it measures). NNEDI3: the
 * single-pass wrapper over bjin's nns16-win8x4 (BO-245n), 0.29-0.49 ms. */
static double s_cost[NV2A_TEXUP_PASSES] = {
    [NV2A_TEXUP_P_LUMA]        = 0.03,
    [NV2A_TEXUP_P_FSR_FEATURE] = 0.10,
    [NV2A_TEXUP_P_FSR_MODEL]   = 0.10,
    [NV2A_TEXUP_P_FSR_SUBCONV] = 0.11,
    [NV2A_TEXUP_P_NNEDI3]      = 0.50,
    [NV2A_TEXUP_P_RECOMBINE]   = 0.04,
};

void nv2a_texup_set_pass_cost(int pass, double ms)
{
    if (pass >= 0 && pass < NV2A_TEXUP_PASSES && ms > 0)
        s_cost[pass] = ms;
}

double nv2a_texup_pass_cost(int pass, double texels)
{
    static double mul = -1;
    if (mul < 0) {
        const char *e = recomp_env(RENV_TEXUP_COST);
        mul = e && atof(e) > 0 ? atof(e) : 1.0;
    }
    if (pass < 0 || pass >= NV2A_TEXUP_PASSES)
        return 0;
    return s_cost[pass] * mul * texels / 65536.0;
}

/* ── the result check ─────────────────────────────────────────────────── */

int nv2a_texup_result_ok(const uint8_t *res, const uint8_t *title, uint32_t w, uint32_t h,
                         unsigned scale)
{
    uint64_t n = 0, bad = 0;
    double lr = 0, lt = 0;
    uint32_t x, y, W = w * scale;
    if (!res || !title || !w || !h || (scale != 2 && scale != 4))
        return 0;
    for (y = 0; y < h; y += (h >= 4 ? 4 : 1))
        for (x = 0; x < w; x += (w >= 4 ? 4 : 1)) {
            const uint8_t *t = title + ((size_t)y * w + x) * 4u;
            const uint8_t *r = res + (((size_t)y * scale + scale / 2) * W + (size_t)x * scale
                                      + scale / 2) * 4u;
            n++;
            if (r[3] != t[3])
                bad++;
            lr += 0.2126 * r[0] + 0.7152 * r[1] + 0.0722 * r[2];
            lt += 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
        }
    if (!n || bad * 100 > n)
        return 0;
    return fabs(lr - lt) / (double)n <= 0.15 * 255.0;
}

/* ── the memory cap ───────────────────────────────────────────────────── */

uint64_t nv2a_texup_cap_bytes(void)
{
    static uint64_t cap;
    if (!cap) {
        const char *e = recomp_env(RENV_TEXUP_CAP_MB);
        double mb = e && atof(e) > 0 ? atof(e) : handheld() ? 192.0 : 1024.0;
        cap = (uint64_t)(mb * 1048576.0);
    }
    return cap;
}

unsigned nv2a_texup_slots(void)
{
    static unsigned n;
    if (!n) {
        const char *e = recomp_env(RENV_TEXUP_SLOTS);
        int v = e && *e ? atoi(e) : NV2A_TEXUP_SLOTS_MAX;
        n = v < 1 ? 1u : v > NV2A_TEXUP_SLOTS_MAX ? NV2A_TEXUP_SLOTS_MAX : (unsigned)v;
    }
    return n;
}

unsigned nv2a_texup_free_flips(void)
{
    static unsigned n;
    if (!n) {
        const char *e = recomp_env(RENV_TEXUP_FREE_FLIPS);
        int v = e && *e ? atoi(e) : 600;
        n = v < 1 ? 1u : (unsigned)v;
    }
    return n;
}

/* ── counters ─────────────────────────────────────────────────────────── */

static struct nv2a_texup_stats s_st;

struct nv2a_texup_stats *nv2a_texup_stats_rw(void)
{
    return &s_st;
}

/* ── the disk store ───────────────────────────────────────────────────── */

#if !defined(_MSC_VER)

#define MAGIC   0x50555854u           /* "TXUP" */
#define JOBS_MAX 512
#define RES_MAX  64

struct tup_header {
    uint32_t magic, policy;
    uint64_t id, hash, shaders;
    uint32_t w, h, format, algo, scale, wrap;
    uint32_t bytes, pad;
    uint64_t checksum;
};

typedef struct {
    int      save;                   /* 0 load, 1 save, 2 save R G B A bytes */
    struct nv2a_texup_key k;
    uint64_t id;
    uint32_t *texels;                /* save: the copy */
    size_t   bytes;
} SJob;

typedef struct {
    uint64_t  id;
    int       state;                 /* READY or ABSENT */
    uint32_t *texels;
    size_t    bytes;
    uint64_t  stamp;
    int       used;
} SRes;

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  s_idle = PTHREAD_COND_INITIALIZER;
static SJob     s_q[JOBS_MAX];
static unsigned s_qh, s_qn;
static uint64_t s_busy_id;
static int      s_busy, s_started;
static SRes     s_res[RES_MAX];
static uint64_t s_stamp;
static char     s_dir[900];
static int      s_dir_on, s_swept;
static uint64_t s_limit = 2048ull << 20, s_dir_bytes;
static unsigned s_mbps = 16;          /* RECOMP_DEBUG=texup_store_mbps */
static size_t   s_q_save_bytes;       /* the saves' copies waiting in s_q */
static unsigned long s_unsaved;
#define SAVE_QUEUE_MAX ((size_t)256 << 20)

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t checksum(const uint32_t *p, size_t words)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    size_t i;
    for (i = 0; i < words; i++)
        x = mix(x, p[i]);
    return x;
}

/* At exit the writes still waiting go unpaced (into the page cache), for at
 * most 3 s, so a quit soon after a cold fill keeps what was made. */
static void store_exit(void)
{
    pthread_mutex_lock(&s_mx);
    s_mbps = 0;
    pthread_mutex_unlock(&s_mx);
    nv2a_texup_store_flush(3000);
}

void nv2a_texup_store_dir(const char *dir)
{
    static int hooked;
    const char *e = recomp_env(RENV_TEXUP_CACHE_MB);
    const char *r = recomp_env(RENV_TEXUP_STORE_MBPS);
    if (!hooked) {
        hooked = 1;
        atexit(store_exit);
    }
    pthread_mutex_lock(&s_mx);
    if (r && *r)
        s_mbps = atoi(r) > 0 ? (unsigned)atoi(r) : 0;
    s_dir_on = 0;
    if (dir && *dir && strlen(dir) < sizeof s_dir - 120) {
        snprintf(s_dir, sizeof s_dir, "%s", dir);
        s_dir_on = 1;
        if (e && *e)
            s_limit = (uint64_t)(atof(e) > 0 ? atof(e) : 0) * 1048576u;
        if (e && !strcmp(e, "0"))
            s_dir_on = 0;
    }
    if (s_dir_on)
        fprintf(stderr, LOGP "upscaled textures kept in %s (at most %.0f MB, written at %u MB/s;"
                " 0: unpaced)\n", s_dir, (double)s_limit / 1048576.0, s_mbps);
    pthread_mutex_unlock(&s_mx);
}

static void mkdirs(const char *dir)
{
    char p[1000];
    size_t i, n = strlen(dir);
    if (n >= sizeof p)
        return;
    memcpy(p, dir, n + 1);
    for (i = 1; i <= n; i++) {
        if (p[i] == '/' || !p[i]) {
            char c = p[i];
            p[i] = 0;
#if defined(_WIN32)
            mkdir(p);
#else
            mkdir(p, 0755);
#endif
            p[i] = c;
        }
    }
}

typedef struct {
    char     name[128];
    uint64_t bytes;
    time_t   mtime;
} Ent;

static int ent_older(const void *a, const void *b)
{
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : strcmp(x->name, y->name);
}

/* As nv2a_texscale_store.c: past the bound the least recently used files
 * go until three quarters are left; dead temporaries go. Worker thread. */
static void dir_sweep(const char *dir, uint64_t limit)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    Ent *ents = NULL;
    size_t n = 0, cap = 0, i;
    uint64_t total = 0;
    time_t now = time(NULL);

    if (!d) {
        s_dir_bytes = 0;
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char path[1100];
        struct stat st;
        size_t len = strlen(de->d_name);
        int tmp = len > 4 && !strcmp(de->d_name + len - 4, ".tmp");
        if (len >= sizeof ents->name || len < 5
                || (!tmp && strcmp(de->d_name + len - 4, ".tup")))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (tmp) {
            if (now - st.st_mtime > 600)
                remove(path);
            continue;
        }
        if (n == cap) {
            Ent *p = (Ent *)realloc(ents, (cap ? cap * 2 : 256) * sizeof *ents);
            if (!p)
                break;
            ents = p;
            cap = cap ? cap * 2 : 256;
        }
        snprintf(ents[n].name, sizeof ents[n].name, "%s", de->d_name);
        ents[n].bytes = (uint64_t)st.st_size;
        ents[n].mtime = st.st_mtime;
        total += ents[n].bytes;
        n++;
    }
    closedir(d);
    if (total > limit) {
        qsort(ents, n, sizeof *ents, ent_older);
        for (i = 0; i < n && total > limit / 4 * 3; i++) {
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", dir, ents[i].name);
            if (remove(path) == 0)
                total -= ents[i].bytes;
        }
        fprintf(stderr, LOGP "cache %s over its %.0f MB bound: %.1f MB left\n", dir,
                (double)limit / 1048576.0, (double)total / 1048576.0);
    }
    free(ents);
    s_dir_bytes = total;
}

static int file_load(const char *dir, const SJob *j, uint32_t **out, size_t *bytes)
{
    char name[200], path[1200];
    struct tup_header hd;
    FILE *f;
    uint32_t *px = NULL;
    int ok = 0;

    nv2a_texup_key_name(&j->k, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(&hd, 1, sizeof hd, f) == sizeof hd && hd.magic == MAGIC
            && hd.policy == NV2A_TEXUP_POLICY && hd.id == j->id && hd.hash == j->k.hash
            && hd.shaders == j->k.shaders && hd.w == j->k.w && hd.h == j->k.h
            && hd.scale == j->k.scale && hd.algo == j->k.algo && hd.bytes % 4 == 0
            && hd.bytes == (uint32_t)j->bytes
            && (px = (uint32_t *)malloc(hd.bytes)) != NULL
            && fread(px, 1, hd.bytes, f) == hd.bytes && fgetc(f) == EOF
            && checksum(px, hd.bytes / 4) == hd.checksum)
        ok = 1;
    fclose(f);
    if (!ok) {
        free(px);
        s_st.bad++;
        fprintf(stderr, LOGP "cache file %s is cut or damaged: removed\n", path);
        remove(path);
        return 0;
    }
    utime(path, NULL);
    *out = px;
    *bytes = hd.bytes;
    return 1;
}

/* The worker's writes, paced (BO-245): pieces of 512 KB, each synced to
 * the device before the next, at most `mbps` MB/s. Unpaced, a race's cold
 * fill (130 MB at 2x, 330 MB at 4x) sat in the page cache and went to the
 * Deck's SD card in one writeback some 30 s later; every other write or
 * read on that card waited behind it (the frame-time trace's fflush on the
 * executor: 70-240 ms, the cold 2x/4x hitches), and the guest's streamed
 * reads and the AppImage's code pages come from the same card. Synced, one
 * piece at most is in flight and the worker is the thread that waits.
 * mbps 0: as before, no sync, no pause. */
#define PIECE ((size_t)512 << 10)

static void sync_fd(int fd)
{
#if defined(_WIN32)
    _commit(fd);
#elif defined(__APPLE__)
    fsync(fd);
#else
    fdatasync(fd);
#endif
}

static void sleep_us(uint64_t us)
{
#if defined(_WIN32)
    Sleep((DWORD)((us + 999u) / 1000u));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

static int paced_write(FILE *f, const void *p, size_t n, unsigned mbps)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t t0 = now_us();
    size_t done = 0;
    while (done < n) {
        size_t k = n - done < PIECE ? n - done : PIECE;
        if (fwrite(b + done, 1, k, f) != k)
            return 0;
        done += k;
        if (mbps) {
            uint64_t due = t0 + (uint64_t)((double)done / (mbps * 1.048576)), now;
            if (fflush(f) != 0)
                return 0;
            sync_fd(fileno(f));
            now = now_us();
            if (now < due)
                sleep_us(due - now);
        }
    }
    return 1;
}

static void file_store(const char *dir, const SJob *j, unsigned mbps)
{
    char name[200], path[1200], tmp[1300];
    struct tup_header hd;
    FILE *f;

    memset(&hd, 0, sizeof hd);
    hd.magic = MAGIC;
    hd.policy = NV2A_TEXUP_POLICY;
    hd.id = j->id;
    hd.hash = j->k.hash;
    hd.shaders = j->k.shaders;
    hd.w = j->k.w;
    hd.h = j->k.h;
    hd.format = j->k.format;
    hd.algo = j->k.algo;
    hd.scale = j->k.scale;
    hd.wrap = j->k.wrap;
    hd.bytes = (uint32_t)j->bytes;
    hd.checksum = checksum(j->texels, j->bytes / 4);
    nv2a_texup_key_name(&j->k, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    f = fopen(tmp, "wb");
    if (!f) {
        mkdirs(dir);
        f = fopen(tmp, "wb");
    }
    if (!f)
        return;
    if (fwrite(&hd, 1, sizeof hd, f) != sizeof hd
            || !paced_write(f, j->texels, j->bytes, mbps)) {
        fclose(f);
        remove(tmp);
        return;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return;
    }
    s_st.saved++;
    s_dir_bytes += j->bytes + sizeof hd;
    if (s_dir_bytes > s_limit)
        dir_sweep(dir, s_limit);
}

/* s_mx held. */
static void res_put(uint64_t id, int state, uint32_t *texels, size_t bytes)
{
    SRes *slot = NULL, *old = NULL;
    int i;
    for (i = 0; i < RES_MAX; i++) {
        SRes *r = &s_res[i];
        if (!r->used) {
            if (!slot)
                slot = r;
        } else if (!old || r->stamp < old->stamp) {
            old = r;
        }
    }
    if (!slot) {
        /* Nobody came for the oldest: its texture left the cache. */
        free(old->texels);
        memset(old, 0, sizeof *old);
        slot = old;
    }
    slot->id = id;
    slot->state = state;
    slot->texels = texels;
    slot->bytes = bytes;
    slot->stamp = ++s_stamp;
    slot->used = 1;
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_mx);
    for (;;) {
        SJob j;
        char dir[sizeof s_dir];
        int on, swept;
        unsigned i, mbps;
        while (!s_qn) {
            s_busy = 0;
            s_busy_id = 0;
            pthread_cond_broadcast(&s_idle);
            pthread_cond_wait(&s_cv, &s_mx);
        }
        /* Reads before writes: a read is a texture being drawn now, and the
         * writes are paced (the order of the writes does not matter). */
        for (i = 0; i < s_qn && s_q[(s_qh + i) % JOBS_MAX].save; i++)
            ;
        if (i && i < s_qn) {
            SJob t = s_q[s_qh];
            s_q[s_qh] = s_q[(s_qh + i) % JOBS_MAX];
            s_q[(s_qh + i) % JOBS_MAX] = t;
        }
        j = s_q[s_qh];
        s_qh = (s_qh + 1) % JOBS_MAX;
        s_qn--;
        if (j.save)
            s_q_save_bytes -= j.bytes < s_q_save_bytes ? j.bytes : s_q_save_bytes;
        mbps = s_mbps;
        s_busy = 1;
        s_busy_id = j.save ? 0 : j.id;
        on = s_dir_on;
        swept = s_swept;
        s_swept = 1;
        memcpy(dir, s_dir, sizeof dir);
        pthread_mutex_unlock(&s_mx);
        if (on && !swept)
            dir_sweep(dir, s_limit);
        if (j.save) {
            uint64_t t0 = now_us();
            if (j.save == 2) {
                size_t k, n = j.bytes / 4;
                for (k = 0; k < n; k++) {
                    uint32_t v = j.texels[k];
                    j.texels[k] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                }
            }
            if (on)
                file_store(dir, &j, mbps);
            s_st.save_ms += (double)(now_us() - t0) / 1000.0;
            free(j.texels);
            pthread_mutex_lock(&s_mx);
        } else {
            uint32_t *px = NULL;
            size_t bytes = 0;
            uint64_t t0 = now_us();
            int hit = on && file_load(dir, &j, &px, &bytes);
            s_st.load_ms += (double)(now_us() - t0) / 1000.0;
            pthread_mutex_lock(&s_mx);
            res_put(j.id, hit ? NV2A_TEXUP_READY : NV2A_TEXUP_ABSENT, px, bytes);
        }
    }
    return NULL;
}

/* s_mx held. 0: no room or no thread. */
static int enqueue(const SJob *j)
{
    if (s_qn == JOBS_MAX)
        return 0;
    if (!s_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, worker, NULL) != 0) {
            pthread_attr_destroy(&at);
            return 0;
        }
        pthread_attr_destroy(&at);
        s_started = 1;
    }
    s_q[(s_qh + s_qn) % JOBS_MAX] = *j;
    s_qn++;
    pthread_cond_signal(&s_cv);
    return 1;
}

int nv2a_texup_load(const struct nv2a_texup_key *k, uint32_t **out, size_t *bytes)
{
    uint64_t id = nv2a_texup_key_id(k);
    unsigned i;
    SJob j;

    pthread_mutex_lock(&s_mx);
    if (!s_dir_on) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXUP_ABSENT;
    }
    for (i = 0; i < RES_MAX; i++) {
        SRes *r = &s_res[i];
        if (r->used && r->id == id) {
            int st = r->state;
            *out = r->texels;
            *bytes = r->bytes;
            memset(r, 0, sizeof *r);
            pthread_mutex_unlock(&s_mx);
            return st;
        }
    }
    if (s_busy && s_busy_id == id) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXUP_PENDING;
    }
    for (i = 0; i < s_qn; i++) {
        SJob *q = &s_q[(s_qh + i) % JOBS_MAX];
        if (!q->save && q->id == id) {
            pthread_mutex_unlock(&s_mx);
            return NV2A_TEXUP_PENDING;
        }
    }
    memset(&j, 0, sizeof j);
    j.k = *k;
    j.id = id;
    j.bytes = (size_t)k->w * k->scale * k->h * k->scale * 4u;
    if (k->scale == 4)
        j.bytes += (size_t)k->w * 2 * k->h * 2 * 4u;
    i = enqueue(&j);
    pthread_mutex_unlock(&s_mx);
    return i ? NV2A_TEXUP_PENDING : NV2A_TEXUP_ABSENT;
}

static void save_as(const struct nv2a_texup_key *k, const uint32_t *texels, size_t bytes,
                    int how)
{
    SJob j;
    memset(&j, 0, sizeof j);
    pthread_mutex_lock(&s_mx);
    if (!s_dir_on || s_q_save_bytes + bytes > SAVE_QUEUE_MAX) {
        /* The writer is that far behind: this result is not kept (it is
         * made again the next time), rather than hold more memory. */
        if (s_dir_on && !s_unsaved++)
            fprintf(stderr, LOGP "disk cache writer behind (%.0f MB waiting): results not kept"
                    " until it catches up\n", (double)s_q_save_bytes / 1048576.0);
        pthread_mutex_unlock(&s_mx);
        return;
    }
    pthread_mutex_unlock(&s_mx);
    j.save = how;
    j.k = *k;
    j.id = nv2a_texup_key_id(k);
    j.bytes = bytes;
    j.texels = (uint32_t *)malloc(bytes);
    if (!j.texels)
        return;
    memcpy(j.texels, texels, bytes);
    pthread_mutex_lock(&s_mx);
    if (!enqueue(&j))
        free(j.texels);
    else
        s_q_save_bytes += bytes;
    pthread_mutex_unlock(&s_mx);
}

void nv2a_texup_save(const struct nv2a_texup_key *k, const uint32_t *texels, size_t bytes)
{
    save_as(k, texels, bytes, 1);
}

void nv2a_texup_save_rgba(const struct nv2a_texup_key *k, const uint32_t *texels, size_t bytes)
{
    save_as(k, texels, bytes, 2);
}

int nv2a_texup_store_flush(unsigned timeout_ms)
{
    struct timespec until;
    int done;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000u;
    until.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_mx);
    while (s_started && (s_qn || s_busy))
        if (pthread_cond_timedwait(&s_idle, &s_mx, &until) == ETIMEDOUT)
            break;
    done = !s_started || (!s_qn && !s_busy);
    pthread_mutex_unlock(&s_mx);
    return done;
}

#else  /* _MSC_VER: no worker thread; no disk cache */

void nv2a_texup_store_dir(const char *dir) { (void)dir; }
int nv2a_texup_load(const struct nv2a_texup_key *k, uint32_t **out, size_t *bytes)
{
    (void)k; (void)out; (void)bytes;
    return NV2A_TEXUP_ABSENT;
}
void nv2a_texup_save(const struct nv2a_texup_key *k, const uint32_t *t, size_t b)
{
    (void)k; (void)t; (void)b;
}
void nv2a_texup_save_rgba(const struct nv2a_texup_key *k, const uint32_t *t, size_t b)
{
    (void)k; (void)t; (void)b;
}
int nv2a_texup_store_flush(unsigned timeout_ms) { (void)timeout_ms; return 1; }

#endif

void nv2a_texup_stats(struct nv2a_texup_stats *s)
{
    *s = s_st;
}

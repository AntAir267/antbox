/**
 * The SDL window's Vulkan present: the zero-copy half of fb_present_sdl.c
 * when the Vulkan backend draws (RECOMP_PB_BACKEND=vulkan, not
 * RECOMP_DEBUG=vk_present=readback). The Vulkan sibling of
 * fb_present_metal.m, with the same split:
 *
 *   device         made here at window creation, before the guest starts
 *                  (nvk_core_init with the window's surface, made by
 *                  SDL_Vulkan_CreateSurface); the backend adopts it at its
 *                  lazy init, so a slot image takes the backend's copy and a
 *                  present on the same queue runs after the copy it shows
 *                  (submission order; the queue lock orders the threads).
 *   slots          three BGRA8 images, frame-sized (render.scale's host
 *                  size), kept in VK_IMAGE_LAYOUT_GENERAL. The backend copies
 *                  its present target into the back slot on the GPU; a
 *                  guest-bytes frame is uploaded into it.
 *   present        on the main thread: the swapchain's next image, cleared to
 *                  black, the slot blitted into nv2a_present_rect's rectangle,
 *                  nearest or linear as the SDL path filters. FIFO when vsync
 *                  is on (the default), so the display paces the main thread
 *                  and never the walker, as Metal's displaySyncEnabled.
 *   shot           RECOMP_WINDOW_SHOT: the swapchain image read back after
 *                  the blit.
 *   latency        the swapchain has the fewest images the driver allows
 *                  (minImageCount, at least 2): with FIFO, one image on
 *                  screen and at most one queued, so presenting adds at most
 *                  one frame (BO-74). The slot hand-off drops a stale frame
 *                  rather than queueing it, as on Metal.
 *   timing         every present's host time, and with VK_KHR_present_id +
 *                  VK_KHR_present_wait (RADV, MoltenVK) the time each frame
 *                  reached the display, from a waiter thread: present-to-
 *                  present intervals (p50/p95/p99, frames past 1.5 refresh
 *                  periods), every 5 s under RECOMP_TRACE=present_stats and
 *                  at exit; RECOMP_TRACE=vk_present=<file> writes one CSV line
 *                  per frame. The last displayed vblank is
 *                  fbv_display_vblank().
 *
 * Thread use: fbv_create/fbv_destroy/fbv_present on the main thread,
 * fbv_slot/fbv_upload on the NV2A ack thread (each with its own command
 * pool). Every submit and present holds nvk_queue_lock.
 */
#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)

#include <SDL.h>
#include <SDL_vulkan.h>
#if defined(__ANDROID__)
#include <SDL_syswm.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <SDL_syswm.h>
#define COBJMACROS
#include <dxgi1_6.h>
#include "dxgi_vblank_model.h"
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif
#include <sched.h>
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d/nv2a_vk_core.h"
#include "recomp_env.h"
#include "latency_trace.h"
#include "d3d/nv2a_vk_glsl.h"
#include "post_shaders.h"
#include "hdr_shaders.h"      /* BO-244: the HDR maths shared by both presenters */
#include "smaa.h"
#if defined(XBOXRECOMP_HAVE_OVERLAY)
#include "overlay.h"
#include "overlay_vulkan.h"
#endif

uint64_t xbox_HostNowNs(void);
void xbox_HostWakeListener(const char *what, void (*fn)(uint64_t gap_ns));
void xbox_PacingResetHook(void (*fn)(void));
void xbox_PacingPresent(uint64_t when_ns);
void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns);
void xbox_FlipHostShown(uint32_t flip, uint64_t shown_ns);
void xbox_VblankDisplayCycle(uint64_t period_ns);
int xbox_VblankDisplayCycleOk(void);
int xbox_VblankDisplayLockOn(void);
uint32_t xbox_HostFlipIntervalAsked(void);
#include "kernel/nv2a_backend_common.h"
#include "kernel/nv2a_pb_state.h"     /* nv2a_pb_present_state: the display size */
#include "kernel/xbox_pvideo.h"       /* the video overlay */
#include "kernel/xbox_gamma.h"        /* the display's gamma ramp */
#include "fb_present_vulkan.h"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#define LOGP "[PRESENT] "
#define SC_MAX 8

static const struct nvk_core *C;
static VkSwapchainKHR   s_sc;
static VkImage          s_sc_img[SC_MAX];
static VkImageView      s_sc_view[SC_MAX];   /* for the overlay pass */
static uint32_t         s_sc_n;
static VkFormat         s_sc_fmt;
static VkExtent2D       s_sc_ext;
static VkPresentModeKHR s_sc_mode;
static int              s_sc_shots;      /* TRANSFER_SRC usage: shots possible */
static int              s_sc_color;      /* COLOR_ATTACHMENT usage: the overlay can draw */
static int              s_recreate;
/* VK_SUBOPTIMAL_KHR asks for a new swapchain, except on Android: there it is
 * returned for as long as the swapchain's transform is not the display's,
 * which with the identity transform on a sideways-mounted panel (the Retroid
 * Pocket 5's is 1080x1920) is every frame, and rebuilding it every frame is
 * what that cost. A size change is caught by the extent check. */
#if defined(__ANDROID__)
#define FBV_SUBOPTIMAL_RECREATES 0
#else
#define FBV_SUBOPTIMAL_RECREATES 1
#endif
/* Android: the swapchain may be smaller than the window, and the system's
 * compositor scales it to the panel (RECOMP_ANDROID_SWAPCHAIN_HEIGHT=<rows>,
 * the width follows the window's shape): this presenter's own scaling pass
 * then writes that many pixels, not the panel's. The idea is Xerenge's
 * Android build's. The window size the swapchain was made for is kept apart,
 * for the "has the window changed" test. */
#if defined(__ANDROID__)
static uint32_t s_and_w, s_and_h;
#define FBV_SIZE_CHANGED(dw, dh) ((uint32_t)(dw) != s_and_w || (uint32_t)(dh) != s_and_h)
#else
#define FBV_SIZE_CHANGED(dw, dh) \
    ((uint32_t)(dw) != s_sc_ext.width || (uint32_t)(dh) != s_sc_ext.height)
#endif
static int              s_vsync, s_shots;

static VkCommandPool    s_pool;          /* main thread */
static VkCommandBuffer  s_cb[2];
static VkFence          s_fence[2];
static VkSemaphore      s_acq[2];
static VkSemaphore      s_done[SC_MAX];
static int              s_fi;

static struct nvk_image s_slot[3];       /* GENERAL once written */
static void post_destroy(void);          /* the post passes, below */

/* BO-110, HDR output: the swapchain in HDR10 (ST 2084 PQ, BT.2020) where the
 * surface offers it (gamescope's WSI layer with HDR on: SteamOS game mode,
 * the Deck OLED's panel or an HDR display on its dock) and graphics.hdr asks
 * for it. The backend writes the frame into the slot already as the display
 * takes it (PQ code values, BT.2020, 10-bit: nv2a_pb_vulkan.c maps it at the
 * flip), so the post passes, the scale and the present are as in SDR, on
 * 32-bit pixels of another format. Only the settings overlay needs a step
 * of its own: drawn into s_hcomp (8-bit, cleared transparent), then encoded
 * and blended over the swapchain image. */
#define HDR_SLOT_FMT VK_FORMAT_A2B10G10R10_UNORM_PACK32
static int              s_hdr_sc;        /* the swapchain is HDR10 now */
static int              s_hdr_offered;   /* the surface offers HDR10 */
static VkFormat         s_hdr_fmt;       /* its format for that */
static float            s_hdr_peak;      /* the display's peak, nits (its EDID or 1000) */
static struct nvk_image s_hcomp;         /* the overlay before its encode (HDR) */
static int  hdr_wanted(void) { return nv2a_host_hdr_active(); }
/* RECOMP_DEBUG=hdr with no HDR10 surface (a headless test): the HDR encode
 * path runs anyway, into the SDR swapchain (wrong colours, true cost). */
static int  hdr_test_encode(void)
{
    static int dbg = -1;
    if (dbg < 0) {
        const char *e = recomp_env(RENV_HDR);
        dbg = e && *e && strcmp(e, "0") != 0;
    }
    return dbg && !s_hdr_sc && hdr_wanted();
}

/* RECOMP_TRACE=post_gpu: the whole present's GPU time too (BO-110). */
static VkQueryPool s_tq;
static int         s_tq_on = -1, s_tq_set[2];
static double      s_tq_sum;
static unsigned    s_tq_n;

static VkCommandPool     s_up_pool;      /* ack thread */
static VkCommandBuffer   s_up_cb;
static VkFence           s_up_fence;
static int               s_up_pending;
static struct nvk_buffer s_up_buf;
static struct nvk_buffer s_shot_buf;
/* RECOMP_DEBUG=lat_sig: each new frame copied to the host, its 16x12
 * signature a G row of the latency trace once its fence is waited on. */
static struct nvk_buffer s_lat_buf[2];
static uint32_t s_lat_flip[2], s_lat_w[2], s_lat_h[2];
static int s_lat_pend[2], s_lat_sig = -1;

/* ---- present timing ----------------------------------------------------- */

#define IV_N 8192
typedef struct {
    float    iv[IV_N];      /* ms between consecutive frames */
    uint32_t n;             /* filled */
    uint32_t pos;
    uint64_t last_ns, count;
} Intervals;
static Intervals       s_host_iv, s_disp_iv;
static pthread_mutex_t s_tm_mu = PTHREAD_MUTEX_INITIALIZER;   /* the stats */
static pthread_mutex_t s_sc_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_sc vs the waiter */
static pthread_mutex_t s_id_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_present_id */
static pthread_cond_t  s_sc_cv = PTHREAD_COND_INITIALIZER;
static uint64_t        s_present_id;        /* last id presented */
static uint64_t        s_sc_first_id;       /* the first id of this swapchain */
static int             s_waiter_run;
static pthread_t       s_waiter;
static FILE           *s_csv;
#if defined(_WIN32)
/* The DXGI vblank thread feeds the vblank lock, not the waiter (BO-191). */
static volatile int    s_dxgi_lock;
/* What it knows of the window's display, for Smooth and HDR (BO-207). */
static volatile int    s_dxgi_vrr = -1;     /* 1 variable refresh, 0 fixed, -1 not known */
static volatile uint64_t s_dxgi_period;     /* the mode's stated refresh period, ns */
static volatile int    s_win_hdr = -1;      /* Windows HDR on for that display, -1 not known */
/* The DXGI thread saw Windows HDR change: the main thread folds it into
 * s_recreate (one writer each, so a request is never lost). */
static volatile int    s_hdr_recheck;
/* The DXGI thread's last refreshes, for the keeper's queue depth (BO-207). */
#define VB_RING 64
static uint64_t        s_vb_ring[VB_RING];
static volatile unsigned s_vb_n;
#endif
#include "smooth_keeper.h"
#define ID_RING 64
static uint32_t        s_id_flip[ID_RING];   /* present id -> the flip it showed */
static uint64_t        s_vbl_ns, s_vbl_period_ns;
static PFN_vkWaitForPresentKHR s_wait_present;   /* a device function */
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
/* BO-207: VK_KHR_present_wait2 where the driver has only that (AMD on
 * Windows): the swapchain is made for it when its surface takes it. */
static PFN_vkWaitForPresent2KHR s_wait_present2;
static int             s_pw2_ok;            /* this surface takes wait2 (asked per surface) */
#endif
/* display.frame_pacing = smooth, the queue keeper (below, keep_poll) */
static uint64_t        s_id_pns[ID_RING];   /* present id -> its present time */
static uint8_t         s_id_meas[ID_RING];  /* 1: a new frame at the title's 60 */
static uint8_t         s_id_rep[ID_RING];   /* 1: a refill's repeat */
static volatile int    s_keep_on = -1;      /* Smooth is running */
static volatile int    s_keep_want;         /* waiter -> presenter: +1 refill, -1 skip */
static volatile int    s_keep_depth = -1;   /* the last frame's queue depth */
static volatile unsigned s_keep_gen;        /* bumped at each correction or switch */
static uint64_t        s_keep_from_id;      /* measured from the id after this one */
static unsigned        s_keep_refills;
static int             s_keep_repeat;       /* the present being made is a repeat */
static uint32_t        s_keep_last_flip = 0xFFFFFFFFu;
static uint64_t        s_keep_poll_ns;
/* The pacing watchdog asked the keeper to start over (BO-211): the main
 * thread's keep_poll and the waiter each take their part. */
static volatile int    s_keep_reset_main, s_keep_reset_waiter;
static unsigned        s_keep_resets;
/* BO-210: a correction clears the waiter's period (learned again). */
static volatile int    s_keep_per_clear;
static volatile unsigned s_keep_deep, s_keep_held;   /* rows too deep / refills held back */
/* The window system's refresh cycle (VK_GOOGLE_display_timing; gamescope's
 * WSI layer has it), ns, 0 not known; the waiter asks it four times a second. */
static PFN_vkGetRefreshCycleDurationGOOGLE s_get_cycle;
static volatile uint64_t s_wsi_cycle_ns;
/* The counters line (fbv_counts, BO-210). */
static volatile uint64_t s_cnt_new, s_cnt_redraw, s_cnt_refill, s_cnt_shown, s_cnt_shown_rep;
static volatile uint64_t s_cnt_present_max_ns;
static uint32_t        s_cnt_last_flip = 0xFFFFFFFFu;
#if defined(_WIN32)
static int             s_keep_images;       /* Smooth asked: three swapchain images (BO-207) */
/* BO-207, Smooth on Windows (Fable's plan): the depth is known at one moment
 * and tracked from there. Switching Smooth on, or a new swapchain, starts
 * with nothing queued: the present-to-display time of the next KW_ANCHOR_N
 * measured frames is B, depth 0. Then the next frame at the title's 60 is
 * presented once more (s_kw_refill, the presenter), and the next
 * KW_ANCHOR_N give the anchor A, which must be B + one refresh (a hitch can
 * empty the queue again before it is taken: then once more; two deep: one
 * present skipped). From then on depth = 1 + round((p2d - A) / period). The
 * composed path's extra refresh and the driver's report lag are inside A
 * and B and cancel. After each correction the depth is 1 by construction:
 * A is taken again and checked against B. */
#define KW_ANCHOR_N 8
static uint64_t mono_ns(void);
static volatile int    s_kw_refill;         /* present the next 60 Hz frame once more */
static volatile int    s_kw_down;           /* the refill did not take twice: stand down */
static SkDown          s_kw_dn;             /* how long it stands down (BO-211) */
static volatile int    s_kw_tries;          /* refills since the last anchor that held */
static volatile int    s_kw_need_b;         /* B (depth 0) to be measured first */
static uint64_t        s_kw_B;              /* p2d with nothing queued, ns (waiter) */
static volatile uint64_t s_kw_on_ns;        /* when the refill was asked */
static uint64_t        s_kw_from_ns;        /* when this anchor was begun (waiter) */
static uint64_t        s_kw_a[KW_ANCHOR_N]; /* the waiter's anchor samples, ns */
static int             s_kw_an;
static uint64_t        s_kw_A;              /* the anchor, ns; 0 while taking it */
static unsigned        s_kw_dropped;        /* samples not just after a refresh */
static int             s_kw_said_none;

/* Switch-on or a new swapchain under Smooth: B, the refill, the anchor.
 * Frames presented from here on are measured. Main thread. */
static void kw_restart(void)
{
    s_kw_on_ns = mono_ns();
    s_kw_tries = 0;
    s_kw_down = 0;
    s_kw_refill = 0;
    s_kw_need_b = 1;
    pthread_mutex_lock(&s_id_mu);
    s_keep_from_id = s_present_id;
    pthread_mutex_unlock(&s_id_mu);
    __atomic_store_n(&s_keep_want, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_keep_gen, 1, __ATOMIC_RELEASE);
}
#endif


static uint64_t mono_ns(void)
{
#if defined(_WIN32)
    return xbox_HostNowNs();     /* the clock the pacing code compares against */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void iv_add(Intervals *v, uint64_t t)
{
    if (v->last_ns && t > v->last_ns) {
        v->iv[v->pos] = (float)((double)(t - v->last_ns) / 1e6);
        v->pos = (v->pos + 1) % IV_N;
        if (v->n < IV_N)
            v->n++;
    }
    v->last_ns = t;
    v->count++;
}

static int fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* p50/p95/p99/max and the share past 1.5 periods, of the last IV_N. */
static void iv_line(const char *what, Intervals *v, char *out, size_t n)
{
    static float tmp[IV_N];
    uint32_t i, miss = 0;
    double period = 1000.0 / 60.0;
    if (!v->n) {
        snprintf(out, n, "%s: none", what);
        return;
    }
    memcpy(tmp, v->iv, v->n * sizeof *tmp);
    qsort(tmp, v->n, sizeof *tmp, fcmp);
    for (i = 0; i < v->n; i++)
        miss += tmp[i] > 1.5 * period;
    snprintf(out, n, "%s: %u intervals p50 %.2f p95 %.2f p99 %.2f max %.2f ms, %.2f%% past"
             " 25 ms", what, v->n, tmp[v->n / 2], tmp[(uint32_t)(v->n * 0.95)],
             tmp[(uint32_t)(v->n * 0.99)], tmp[v->n - 1], 100.0 * miss / v->n);
}

void fbv_timing_report(const char *when)
{
    char a[256], b[256];
    pthread_mutex_lock(&s_tm_mu);
    iv_line("host presents", &s_host_iv, a, sizeof a);
    iv_line("displayed", &s_disp_iv, b, sizeof b);
    pthread_mutex_unlock(&s_tm_mu);
    fprintf(stderr, LOGP "Vulkan %s: %s; %s%s\n", when, a, b,
            C && C->present_wait ? "" : " (no VK_KHR_present_wait)");
    if (s_keep_on > 0)
        fprintf(stderr, LOGP "frame pacing smooth: %u refills (%u held back), %u rows too deep"
                " (left so), queue depth %d, %u watchdog resets\n", s_keep_refills,
                s_keep_held, s_keep_deep, s_keep_depth, s_keep_resets);
}

/* The counters line's Vulkan half (BO-210): running totals, and the
 * longest present call since the last call (then cleared). Main thread. */
void fbv_counts(FbvCounts *c)
{
    memset(c, 0, sizeof *c);
    c->presents_new = s_cnt_new;
    c->presents_redraw = s_cnt_redraw;
    c->refills = s_cnt_refill;
    c->shown = s_cnt_shown;
    c->shown_rep = s_cnt_shown_rep;
    c->present_max_ns = s_cnt_present_max_ns;
    s_cnt_present_max_ns = 0;
    c->wsi_cycle_ns = __atomic_load_n(&s_wsi_cycle_ns, __ATOMIC_RELAXED);
    c->period_ns = __atomic_load_n(&s_vbl_period_ns, __ATOMIC_RELAXED);
    c->deep = s_keep_deep;
    c->held = s_keep_held;
    c->smooth = s_keep_on > 0;
    c->depth = s_keep_depth;
    c->have_wait = s_waiter_run;
}

/* The last vblank a frame was shown at (CLOCK_MONOTONIC ns) and the
 * measured refresh period; 0 when not known (no present_wait). */
int fbv_display_vblank(uint64_t *t_ns, uint64_t *period_ns)
{
    uint64_t t = __atomic_load_n(&s_vbl_ns, __ATOMIC_ACQUIRE);
    *t_ns = t;
    *period_ns = __atomic_load_n(&s_vbl_period_ns, __ATOMIC_RELAXED);
    return t != 0;
}

/* display.frame_pacing (RECOMP_FRAME_PACING) = auto | low_latency | smooth
 * (BO-110). Under gamescope our FIFO queue is gamescope's: it releases one
 * commit per refresh, and its depth changes only at events (a late present
 * and its catch-up in one refresh: one deeper; a stretch where the title is
 * slower than the display, which includes every 30 fps crash camera: one
 * shallower per repeated refresh) and never comes back by itself. With
 * nothing queued, a present a few ms late misses its refresh and the screen
 * repeats a frame: most of the 33 ms ticks on the Steam Deck's frame-time
 * graph.
 *
 * Smooth keeps exactly one finished frame queued. Each frame's present-to-
 * display time (present_wait) says how many refreshes it waited behind
 * others: with the vblank locked to the display (kernel_pacing.c) a present
 * comes ~1 ms after a refresh, and shows 1 refresh later with nothing ahead,
 * 2 with one frame ahead. Three frames in a row at the title's 60 with an
 * empty queue: the frame just presented is presented once more (it shows for
 * two refreshes, once); three with two queued: one frame is not presented.
 * The title's 30 fps stretches and redraws are not measured. Costs one
 * refresh of latency (16.7 ms at 60 Hz) against an empty queue.
 *
 * Low latency is FIFO as before: no keeper (MAILBOX only with
 * display.present_mode = mailbox). Auto is Smooth under gamescope (Steam
 * Deck Game Mode, Bazzite), Low latency elsewhere. Needs present_wait and
 * FIFO; off the gamescope path also three swapchain images. */
static int under_gamescope(void)
{
#if defined(__linux__) && !defined(__ANDROID__)
    const char *g = getenv("GAMESCOPE_WAYLAND_DISPLAY"), *w = getenv("ENABLE_GAMESCOPE_WSI");
    return (g && *g) || (w && !strcmp(w, "1"));
#else
    return 0;
#endif
}

/* The refresh period Smooth works with: the window system's where it says
 * it and the reports agree, else the one present_wait's steps show (BO-210). */
static uint64_t keep_period(void)
{
    uint64_t w = __atomic_load_n(&s_wsi_cycle_ns, __ATOMIC_RELAXED);
    /* Not while the vblank lock finds the reports off its grid (a limiter's
     * target stated, not the panel's refresh). */
    if (w >= 2000000u && w <= 50000000u
            && (xbox_VblankDisplayCycleOk() || !xbox_VblankDisplayLockOn()))
        return w;
    return __atomic_load_n(&s_vbl_period_ns, __ATOMIC_RELAXED);
}

/* display.frame_pacing asks for Smooth (auto: under gamescope). */
static int pacing_smooth(void)
{
    const char *e = recomp_env(RENV_FRAME_PACING);
    int mode = e && !strcmp(e, "smooth") ? 2 : e && !strcmp(e, "low_latency") ? 1 : 0;
    return mode == 2 || (mode == 0 && under_gamescope());
}

/* Twice a second, on the main thread: the setting, and whether Smooth can
 * run with this swapchain. */
static void keep_poll(void)
{
    uint64_t now = mono_ns();
    const char *e, *off = "low latency (smooth needs FIFO, present_wait and, off gamescope,"
                          " three swapchain images)";
    int gs, smooth, on;

    if (s_keep_on >= 0 && now - s_keep_poll_ns < 500000000ull
            && !s_keep_reset_main)
        return;
    s_keep_poll_ns = now;
#if defined(_WIN32)
    /* A stand-down ends after 60 s (doubling on repeats): Smooth comes back
     * and measures B again (BO-211; it used to wait for a new swapchain). */
    if (__atomic_load_n(&s_kw_down, __ATOMIC_ACQUIRE) && sk_down_expired(&s_kw_dn, now)) {
        fprintf(stderr, "[PACING] smooth: trying again after %.0f s standing down\n",
                s_kw_dn.hold_ns / 1e9);
        s_kw_down = 0;
    }
#endif
    if (s_keep_reset_main) {
        s_keep_reset_main = 0;
        s_keep_resets++;
#if defined(_WIN32)
        /* B is the empty queue's: measured again only where the queue is
         * empty (standing down, or never measured). A reset mid-Smooth has
         * the kept frame queued, and a B taken then is a refresh long: the
         * session would sit a frame deeper for good (BO-211 review). The
         * gen bump below re-anchors A against the B kept. */
        if (s_kw_down || !s_kw_B) {
            s_kw_down = 0;
            if (s_keep_on > 0)
                kw_restart();
        } else {
            s_kw_tries = 0;
            s_kw_refill = 0;
        }
#endif
        if (s_keep_on > 0) {
            pthread_mutex_lock(&s_id_mu);
            s_keep_from_id = s_present_id;
            pthread_mutex_unlock(&s_id_mu);
            __atomic_store_n(&s_keep_want, 0, __ATOMIC_RELAXED);
            __atomic_add_fetch(&s_keep_gen, 1, __ATOMIC_RELEASE);
        }
    }
    e = recomp_env(RENV_FRAME_PACING);
    gs = under_gamescope();
    smooth = pacing_smooth();
    on = smooth && s_waiter_run && s_vsync && s_sc_mode == VK_PRESENT_MODE_FIFO_KHR
         && (gs || s_sc_n >= 3);
#if defined(_WIN32)
    {
        /* BO-207. Smooth wants a third image: the swapchain is made again
         * with it (keep_images). The keeper holds a frame queued only where
         * the display refreshes at the title's rate: under variable refresh
         * a frame shows when it is presented, so there is no queue to keep
         * and refilling it would chase our own presents (BO-191); at 120 Hz
         * and up a queued frame is shown at the next refresh, a few ms on,
         * and a late frame slips that much only. */
        uint64_t per = s_dxgi_period;
        if (smooth != s_keep_images && s_sc)
            s_recreate = 1;      /* make_swapchain sets s_keep_images */
#if defined(VK_KHR_present_wait2)
        if (on && C->present_wait2 && !s_pw2_ok) {
            on = 0;              /* the waiter has no display times on this surface */
            off = "smooth needs per-frame display times: this surface does not take"
                  " present_wait2";
        } else
#endif
        if (on && s_kw_down) {
            on = 0;
            off = "smooth: the frame presented once more did not stay queued, twice"
                  " (the anchor below 1.5 refreshes); low latency for a while, then it tries"
                  " again";
        } else if (on && s_dxgi_lock && s_dxgi_vrr < 0) {
            on = 0;
            off = "smooth, waiting for the display's refresh to be known";
        } else if (on && s_dxgi_vrr == 1) {
            on = 0;
            off = "smooth under variable refresh: each frame shows when it is presented,"
                  " no queue to keep (FIFO, three images)";
        }
        (void)per;
    }
#endif
    {
        /* Smooth only near 60 Hz, on every platform (BO-210; it was
         * Windows' only). At 90 Hz under gamescope a 60 Hz title's frames are
         * 1.5 refreshes apart: there is no queue of one to keep, and the
         * keeper's readings were wrong. At 120 Hz and up a queued frame shows
         * at the next refresh, a few ms on. Low latency until the refresh is
         * back. */
        uint64_t per = keep_period();
#if defined(_WIN32)
        if (s_dxgi_period)
            per = s_dxgi_period;
#endif
        if (on && !sk_refresh_ok(per)) {
            on = 0;
            off = "smooth at a refresh well away from 60 Hz: low latency until it is back"
                  " (a queued frame shows at the next refresh)";
        }
    }
    {
        /* A new reason Smooth stands down is said too (BO-207). */
        static const char *said_off;
        if (on == s_keep_on && !on && smooth && off != said_off && s_keep_on >= 0)
            fprintf(stderr, "[PACING] frame pacing %s: %s\n", e && *e ? e : "auto", off);
        said_off = on ? NULL : off;
    }
    if (on != s_keep_on) {
        fprintf(stderr, "[PACING] frame pacing %s: %s\n", e && *e ? e : "auto",
                on ? "smooth, one frame kept queued" : smooth ? off : "low latency");
        pthread_mutex_lock(&s_id_mu);
        s_keep_from_id = s_present_id;
        pthread_mutex_unlock(&s_id_mu);
        __atomic_store_n(&s_keep_want, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_keep_gen, 1, __ATOMIC_RELEASE);
        s_keep_on = on;
#if defined(_WIN32)
        if (on)
            kw_restart();
        else
            s_kw_refill = 0;
#endif
    }
}

/* The waiter, at each frame on the display: its queue depth, and a
 * correction when three in a row are off one. */
#if defined(_WIN32)
/* BO-207: the present came in the first 5 ms after a DXGI refresh (the
 * title's steady phase under the vblank lock), so its present-to-display
 * time is a whole number of refreshes plus a fixed lag. 0 under variable
 * refresh or with no DXGI thread. */
static int kw_phase_ok(uint64_t pns)
{
    unsigned n = __atomic_load_n(&s_vb_n, __ATOMIC_ACQUIRE), k;
    if (!s_dxgi_lock || s_dxgi_vrr != 0 || n < 2)
        return 0;
    for (k = 0; k < VB_RING - 1 && k < n; k++) {
        uint64_t v = s_vb_ring[(n - 1 - k) % VB_RING];
        if (v <= pns)
            return pns - v <= 5000000u;
    }
    return 0;
}

static int u64cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
#endif

static void keep_measure(uint64_t id, uint64_t t, uint64_t prev_t)
{
    static unsigned gen;
    static SkRun run = { -1, 0 };
    uint64_t p = keep_period(), pns, from;
    int meas, d;
    unsigned g = __atomic_load_n(&s_keep_gen, __ATOMIC_ACQUIRE);

    if (g != gen) {
        gen = g;
        sk_run_reset(&run);
#if defined(_WIN32)
        s_kw_an = 0;             /* a correction or a switch: anchor again */
        s_kw_A = 0;
        s_kw_from_ns = mono_ns();
        s_kw_said_none = 0;
#endif
    }
    pthread_mutex_lock(&s_id_mu);
    pns = s_id_pns[id % ID_RING];
    meas = s_id_meas[id % ID_RING];
    from = s_keep_from_id;
    pthread_mutex_unlock(&s_id_mu);
    if (id <= from || s_id_rep[id % ID_RING])
        return;
    if (!meas || !pns || t <= pns || p < 5000000u || p > 50000000u) {
        run.run_n = 0;           /* a 30 fps frame or a redraw breaks the row */
        return;
    }
    /* Refreshes it waited behind other frames (smooth_keeper.h). */
    d = sk_depth(t - pns, p);
#if defined(_WIN32)
    {
        uint64_t per = s_dxgi_period ? s_dxgi_period : p, p2d = t - pns;
        double x;
        int k;
        (void)prev_t;
        if (s_kw_refill || s_kw_down)
            return;              /* the refill not made yet */
        if (!kw_phase_ok(pns)) {
            s_kw_dropped++;
            static int said_n;
            if (!s_kw_A && !s_kw_said_none && mono_ns() - s_kw_from_ns > 10000000000ull
                    && said_n++ < 20) {
                s_kw_said_none = 1;
                fprintf(stderr, "[PACING] smooth: no anchor 10 s after the switch or the last"
                        " correction (%d of %d"
                        " samples; %u presents not just after a refresh): nothing measured,"
                        " nothing corrected\n", s_kw_an, KW_ANCHOR_N, s_kw_dropped);
            }
            run.run_n = 0;
            return;
        }
        if (s_kw_need_b || !s_kw_A) {
            uint64_t m;
            s_kw_a[s_kw_an++] = p2d;
            if (s_kw_an < KW_ANCHOR_N)
                return;
            qsort(s_kw_a, KW_ANCHOR_N, sizeof s_kw_a[0], u64cmp);
            s_kw_an = 0;
            m = s_kw_a[KW_ANCHOR_N / 2];
            if (s_kw_need_b) {
                s_kw_B = m;
                s_kw_need_b = 0;
                fprintf(stderr, "[PACING] smooth: nothing queued, a frame shows %.1f ms after its"
                        " present (%.2f refreshes); the next is presented once more\n",
                        m / 1e6, (double)m / (double)per);
                s_kw_refill = 1;
                return;
            }
            k = (int)floor(((double)m - (double)s_kw_B) / (double)per + 0.5);
            if (k < 1) {
                /* Nothing queued after all: the refill was not kept (a
                 * hitch emptied the queue before the anchor was taken). */
                fprintf(stderr, "[PACING] smooth: anchor %.1f ms is the empty queue's (%.1f): %s\n",
                        m / 1e6, s_kw_B / 1e6, s_kw_tries < 3 ? "presented once more again"
                        : "standing down");
                if (s_kw_tries < 3)
                    s_kw_refill = 1;
                else {
                    sk_down_set(&s_kw_dn, mono_ns());
                    __atomic_store_n(&s_kw_down, 1, __ATOMIC_RELEASE);
                }
                return;
            }
            if (k > 1) {
                /* Too deep: left so (a refresh of latency, not a frame;
                 * BO-210). The anchor is the one-deep one, from B. */
                fprintf(stderr, "[PACING] smooth: anchor %.1f ms is %d refreshes past the empty"
                        " queue's (%.1f): %d frames queued, left so\n", m / 1e6, k,
                        s_kw_B / 1e6, k);
                s_kw_A = s_kw_B + per;
                s_kw_tries = 0;
                s_keep_depth = k;
                return;
            }
            s_kw_A = m;
            s_kw_tries = 0;
            {
                static int said;
                if (said++ < 100)
                    fprintf(stderr, "[PACING] smooth: anchored, one frame queued shows %.1f ms"
                            " after its present (empty queue %.1f), %.0f ms after the switch\n",
                            s_kw_A / 1e6, s_kw_B / 1e6, (mono_ns() - s_kw_on_ns) / 1e6);
            }
            s_keep_depth = 1;
            return;
        }
        x = ((double)p2d - (double)s_kw_A) / (double)per;
        k = (int)floor(x + 0.5);
        if (fabs(x - k) > 0.25)
            return;              /* between the grid's steps: not counted */
        d = 1 + k;
    }
#else
    (void)prev_t;
#endif
    if (d < 0)
        d = 0;
    s_keep_depth = d;
    {
        int want = sk_run(&run, d, __atomic_load_n(&s_keep_want, __ATOMIC_RELAXED) != 0, t);
        if (want)
            __atomic_store_n(&s_keep_want, want, __ATOMIC_RELEASE);
        s_keep_deep = run.deep;
        s_keep_held = run.held;
    }
}

/* The presenter after a correction: frames presented from here on are the
 * new state's. */
static void keep_corrected(const char *what, uint32_t flip)
{
    static int said;
    pthread_mutex_lock(&s_id_mu);
    s_keep_from_id = s_present_id;
    pthread_mutex_unlock(&s_id_mu);
    __atomic_store_n(&s_keep_want, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_keep_gen, 1, __ATOMIC_RELEASE);
    /* The period is learned again from the next steps (BO-210: a wrong one
     * used to stay for the session). */
    __atomic_store_n(&s_keep_per_clear, 1, __ATOMIC_RELEASE);
    /* Printed for the first 400; all are counted in the counters line. */
    if (said++ < 400)
        fprintf(stderr, "[PACING] smooth: %s at flip %u, t_us %llu (refills %u, rows too deep"
                " %u)\n",
                what, flip, (unsigned long long)(mono_ns() / 1000u), s_keep_refills,
                s_keep_deep);
}

/* vkWaitForPresentKHR, or its second form (BO-207). s_sc_mu held. */
static VkResult wait_present(uint64_t id, uint64_t timeout_ns)
{
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    if (C->present_wait2) {
        VkPresentWait2InfoKHR wi;
        if (!s_pw2_ok || !s_wait_present2)
            return VK_NOT_READY;   /* no ids on this surface: the waiter idles */
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
        wi.presentId = id;
        wi.timeout = timeout_ns;
        return s_wait_present2(C->dev, s_sc, &wi);
    }
#endif
    return s_wait_present(C->dev, s_sc, id, timeout_ns);
}

/* Waits for each present id in turn to reach the display. s_id_mu guards
 * the id counter (held for instants); s_sc_mu is held across one wait (20 ms
 * at most), so the swapchain is not destroyed under it. */
static void *waiter_main(void *arg)
{
    uint64_t next = 1, prev_t = 0, cyc_t = 0;
    int prev_meas = 0;
    SkPeriod per = { 0, 0, 0, 0, 0 };
    (void)arg;
    for (;;) {
        VkResult r;
        pthread_mutex_lock(&s_id_mu);
        while (s_waiter_run && next > s_present_id)
            pthread_cond_wait(&s_sc_cv, &s_id_mu);
        pthread_mutex_unlock(&s_id_mu);
        if (!s_waiter_run)
            break;
        pthread_mutex_lock(&s_sc_mu);
        if (next < s_sc_first_id)
            next = s_sc_first_id;
        r = s_sc && next <= s_present_id ? wait_present(next, 20000000ull) : VK_NOT_READY;
        if (s_get_cycle && s_sc && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
            /* The window system's refresh, four times a second (gamescope's
             * layer answers from what the compositor last said). */
            uint64_t now = mono_ns();
            if (now - cyc_t >= 250000000ull) {
                VkRefreshCycleDurationGOOGLE rc;
                cyc_t = now;
                memset(&rc, 0, sizeof rc);
                if (s_get_cycle(C->dev, s_sc, &rc) == VK_SUCCESS) {
                    uint64_t c = rc.refreshDuration, o = s_wsi_cycle_ns;
                    if (c < 2000000u || c > 50000000u)
                        c = 0;
                    if (c != o && (!c || !o || (c > o ? c - o : o - c) > o / 200)) {
                        static int said;
                        if (said++ < 50)
                            XR_NOTICE("[PACING] window system refresh: %.3f ms (%.2f Hz)%s\n",
                                      c / 1e6, c ? 1e9 / (double)c : 0.0,
                                      o ? "" : ", for Smooth and the vblank lock");
                        __atomic_store_n(&s_wsi_cycle_ns, c, __ATOMIC_RELAXED);
                        xbox_VblankDisplayCycle(c);
                    }
                }
            }
        }
        pthread_mutex_unlock(&s_sc_mu);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            uint64_t t = mono_ns();
            pthread_mutex_lock(&s_tm_mu);
            iv_add(&s_disp_iv, t);
            pthread_mutex_unlock(&s_tm_mu);
            int meas_now;
            if (s_keep_reset_waiter) {
                s_keep_reset_waiter = 0;
                memset(&per, 0, sizeof per);
            }
            if (__atomic_exchange_n(&s_keep_per_clear, 0, __ATOMIC_ACQ_REL))
                sk_period_clear(&per);
            pthread_mutex_lock(&s_id_mu);
            meas_now = s_id_meas[next % ID_RING] && !s_id_rep[next % ID_RING];
            if (s_id_rep[next % ID_RING])
                s_cnt_shown_rep++;
            else
                s_cnt_shown++;
            pthread_mutex_unlock(&s_id_mu);
            if (prev_t && prev_meas && meas_now) {
                /* The refresh period from the steps between two new frames
                 * at the title's 60: a crash camera's 30 fps steps and a
                 * refill's repeat are not fed (smooth_keeper.h, BO-211).
                 * Told once confirmed: after a clear the last one stands
                 * until ~5 steps agree on the new one (BO-210). */
                sk_period_feed(&per, t - prev_t);
                if (sk_period_sure(&per))
                    __atomic_store_n(&s_vbl_period_ns, per.p, __ATOMIC_RELAXED);
            }
            prev_meas = meas_now;
            __atomic_store_n(&s_vbl_ns, t, __ATOMIC_RELEASE);
            /* The real display refresh, for RECOMP_VBLANK_SYNC=display (the
             * emulated vblank's edges snap to it) and the pacing trace. A
             * frame shown is a refresh; FIFO shows every frame. No period:
             * the step to the last one is a whole number of refreshes,
             * which the kernel resolves (s_vbl_period_ns is the frame
             * rate, not the display's, above 60 Hz). */
#if defined(_WIN32)
            /* On Windows the DXGI thread does this when it runs (BO-191). */
            if (!s_dxgi_lock)
#endif
            xbox_VblankDisplayRefresh(t, 0);
            xbox_PacingPresent(t);
            /* A new frame on the glass: when, for a title on the host grid. */
            if (!s_id_rep[next % ID_RING])
                xbox_FlipHostShown(s_id_flip[next % ID_RING], t);
            if (s_keep_on > 0)
                keep_measure(next, t, prev_t);
            if (xbox_LatOn() && !s_id_rep[next % ID_RING]) {
                char b[32];
                snprintf(b, sizeof b, "%llu", (unsigned long long)t);
                xbox_LatRow('D', s_id_flip[next % ID_RING], t, b);
            }
            if (s_csv)
#if defined(_WIN32)
                /* pending: presented and not yet shown at this report (BO-207):
                 * 0 Low latency / 1 Smooth on a flip path, one more composed. */
                fprintf(s_csv, "%llu,display,%llu,,,%lld\n", (unsigned long long)next,
                        (unsigned long long)t, (long long)(s_present_id - next));
#else
                fprintf(s_csv, "%llu,display,%llu,\n", (unsigned long long)next,
                        (unsigned long long)t);
#endif
            prev_t = t;
            next++;
        } else if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            next = s_present_id + 1;    /* out of date: the next swapchain */
        } else {
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
            if (C->present_wait2 && !s_pw2_ok) {
                Sleep(50);              /* a surface without wait2: nothing to wait for */
                next = s_present_id + 1;
                continue;
            }
#endif
            if (r == VK_NOT_READY) {
                /* No swapchain (minimised, a remake, the display asleep):
                 * nothing to wait for yet. A sleep, not a yield: the yield
                 * loop held a core for as long as it lasted (BO-210). */
#if defined(_WIN32)
                Sleep(2);
#else
                struct timespec ts = { 0, 2000000 };
                nanosleep(&ts, NULL);
#endif
            } else {
                sched_yield();
            }
        }
    }
    return NULL;
}

#if defined(_WIN32)
/* Windows: the emulated vblank's display lock (RECOMP_VBLANK_SYNC=display)
 * follows a thread that waits for each vertical blank of the output the
 * window is on (IDXGIOutput::WaitForVBlank, as the D3D11 presenter and
 * RetroArch/DuckStation-style "sync to host refresh" do), on every GPU.
 * Vulkan's present_wait (NVIDIA has it, AMD's RX 9060 XT driver did not)
 * reports only the refreshes our frames were shown at, and under variable
 * refresh those are our own presents: the lock chased them, and the vblank
 * swung 8.6-22.7 ms on AMD and ran at 61-66 Hz on NVIDIA with a slow
 * present (GitHub issue #4, BO-191). The DXGI returns tell the two kinds of
 * display apart (dxgi_vblank_model.h): every refresh of a fixed display,
 * our presents under variable refresh. Fixed: the filtered grid with the
 * mode's stated period (QueryDisplayConfig) is reported. Variable: nothing
 * is, so the lock stands down and the vblank keeps its own 60.000 Hz, which
 * the display then follows. present_wait still feeds the on-glass
 * statistics and the latency rows. DXGI is loaded at run time, so the exe
 * has no link dependency on it. */
typedef HRESULT (WINAPI *dxgi_create_fn)(REFIID, void **);
static volatile int  s_dxgi_run;
static HANDLE        s_dxgi_th;
static HWND          s_dxgi_hwnd;
static dxgi_create_fn s_dxgi_create;

/* A new DXGI factory (a reference), or NULL. */
static IDXGIFactory1 *dxgi_factory(void)
{
    static const GUID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba,
        { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    IDXGIFactory1 *fac = NULL;
    if (!s_dxgi_create || FAILED(s_dxgi_create(&iid_factory1, (void **)&fac)))
        return NULL;
    return fac;
}

/* The output of factory fac that shows monitor mon (a reference), or NULL. */
static IDXGIOutput *dxgi_output_in(IDXGIFactory1 *fac, HMONITOR mon)
{
    IDXGIOutput *found = NULL;
    UINT a, o;

    for (a = 0; fac && !found; a++) {
        IDXGIAdapter1 *ad = NULL;
        if (FAILED(IDXGIFactory1_EnumAdapters1(fac, a, &ad)) || !ad)
            break;
        for (o = 0; !found; o++) {
            IDXGIOutput *out = NULL;
            DXGI_OUTPUT_DESC od;
            if (FAILED(IDXGIAdapter1_EnumOutputs(ad, o, &out)) || !out)
                break;
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == mon)
                found = out;
            else
                IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    return found;
}

/* The DXGI output that shows monitor mon (a reference), or NULL. */
static IDXGIOutput *dxgi_output_for(HMONITOR mon)
{
    IDXGIFactory1 *fac = dxgi_factory();
    IDXGIOutput *found = dxgi_output_in(fac, mon);
    if (fac)
        IDXGIFactory1_Release(fac);
    return found;
}

/* DXGI, loaded at run time (no link dependency); 1 when there. */
static int dxgi_load(void)
{
    static int tried;
    if (!tried) {
        HMODULE dll = LoadLibraryA("dxgi.dll");
        tried = 1;
        s_dxgi_create = dll ? (dxgi_create_fn)(void (*)(void))GetProcAddress(dll, "CreateDXGIFactory1")
                            : NULL;
    }
    return s_dxgi_create != NULL;
}

/* BO-207: whether Windows shows monitor mon in HDR (its output's colour
 * space is HDR10: ST 2084, BT.2020), and its luminances (nits): lum[0] max,
 * lum[1] max full frame, lum[2] min; the EDID's for the monitor's current
 * mode, or a Windows HDR Calibration profile's where there is one. An
 * output keeps the colour space it had when its factory was made: fac is a
 * current factory (a new one, or one IsCurrent() still vouches for).
 * 1 on, 0 off, -1 not known (no DXGI 1.6 output for it). */
static int dxgi_monitor_hdr(IDXGIFactory1 *fac, HMONITOR mon, float lum[3])
{
    static const GUID iid_output6 = { 0x068346e8, 0xaaec, 0x4b84,
        { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } };
    IDXGIOutput *o;
    IDXGIOutput6 *o6 = NULL;
    DXGI_OUTPUT_DESC1 d;
    int hdr = -1;
    lum[0] = lum[1] = lum[2] = 0.0f;
    if (!mon || !fac || !(o = dxgi_output_in(fac, mon)))
        return -1;
    if (SUCCEEDED(IDXGIOutput_QueryInterface(o, &iid_output6, (void **)&o6)) && o6) {
        if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d))) {
            hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            lum[0] = d.MaxLuminance;
            lum[1] = d.MaxFullFrameLuminance;
            lum[2] = d.MinLuminance;
        }
        IDXGIOutput6_Release(o6);
    }
    IDXGIOutput_Release(o);
    return hdr;
}

static HWND window_hwnd(SDL_Window *win)
{
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!win || !SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS)
        return NULL;
    return wm.info.win.window;
}

/* The window's display, with a factory of its own. */
static int dxgi_window_hdr(SDL_Window *win, float lum[3])
{
    HWND h = window_hwnd(win);
    IDXGIFactory1 *fac;
    int r;
    lum[0] = lum[1] = lum[2] = 0.0f;
    if (!h || !dxgi_load() || !(fac = dxgi_factory()))
        return -1;
    r = dxgi_monitor_hdr(fac, MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), lum);
    IDXGIFactory1_Release(fac);
    return r;
}

/* The refresh period of monitor mon's current mode: the exact rational
 * from QueryDisplayConfig (60000/1001 for 59.94; SDL's and
 * EnumDisplaySettings' whole hertz would say 59), *exact 1. Failing that,
 * EnumDisplaySettings' whole hertz, *exact 0 (good enough to tell variable
 * refresh apart, not to lock to). 0 when neither says. */
static uint64_t dxgi_mode_period(HMONITOR mon, int *exact, unsigned *num, unsigned *den)
{
    MONITORINFOEXW mi;
    DEVMODEW dm;
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    uint64_t period = 0;
    int tries;

    *exact = 0;
    *num = *den = 0;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        return 0;
    for (tries = 0; tries < 3; tries++) {
        LONG rc;
        free(paths);
        free(modes);
        paths = NULL;
        modes = NULL;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
            break;
        paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np ? np : 1, sizeof *paths);
        modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *modes);
        if (!paths || !modes)
            break;
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
        if (rc == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (rc != ERROR_SUCCESS)
            break;
        for (i = 0; i < np && !period; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            DISPLAYCONFIG_RATIONAL r;
            UINT32 mi_idx;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS
                    || wcscmp(sn.viewGdiDeviceName, mi.szDevice))
                continue;
            r = paths[i].targetInfo.refreshRate;
            mi_idx = paths[i].targetInfo.modeInfoIdx;
            if (mi_idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mi_idx < nm
                    && modes[mi_idx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
                    && modes[mi_idx].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator
                    && modes[mi_idx].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator)
                r = modes[mi_idx].targetMode.targetVideoSignalInfo.vSyncFreq;
            if (r.Numerator && r.Denominator) {
                period = ((uint64_t)r.Denominator * 1000000000ull + r.Numerator / 2)
                         / r.Numerator;
                *num = r.Numerator;
                *den = r.Denominator;
                *exact = 1;
            }
        }
        break;
    }
    free(paths);
    free(modes);
    if (period < 2000000u || period > 50000000u) {
        period = 0;
        *exact = 0;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                && dm.dmDisplayFrequency > 1 && dm.dmDisplayFrequency < 500) {
            period = 1000000000ull / dm.dmDisplayFrequency;
            *num = dm.dmDisplayFrequency;
            *den = 1;
        }
    }
    return period;
}

static void dxgi_say_state(const dvm_model *m)
{
    double hz = m->period_ns ? 1e9 / (double)m->period_ns : 0;
    if (m->state == DVM_VRR)
        fprintf(stderr, "[PACING] display: variable refresh (%.2f Hz mode, its refreshes follow"
                " our presents: median step %.2f ms): the vblank runs on its own 60.000 Hz"
                " timer\n", hz, m->median_ns / 1e6);
    else if (m->state == DVM_FIXED && m->period_ns)
        fprintf(stderr, "[PACING] display: fixed refresh %.3f Hz (median step %.2f ms): %s\n",
                hz, m->median_ns / 1e6, !m->exact
                ? "the vblank follows its measured refreshes (no exact mode rate)"
                : "the vblank follows it where it is a multiple of 60 Hz, else its own"
                  " 60.000 Hz");
}

static DWORD WINAPI dxgi_vblank_main(LPVOID param)
{
    IDXGIOutput *out = (IDXGIOutput *)param;
    HMONITOR mon = NULL;
    dvm_model *m = (dvm_model *)calloc(1, sizeof *m);
    IDXGIFactory1 *fac = NULL;   /* for the HDR check: re-made only when no longer current */
    uint64_t last = 0, look = 0;
    unsigned fails = 0, fast = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (!m) {
        IDXGIOutput_Release(out);
        return 0;
    }
    dvm_reset(m, 0, 0);
    while (s_dxgi_run) {
        uint64_t t, when, per;
        int r;
        /* Once a second, and after a failed wait: the window's monitor and
         * its mode. A new output or a new rate starts the model over (it
         * says nothing for half a second, and the lock waits). */
        t = mono_ns();
        if (!look || t - look >= 1000000000ull || fails) {
            HMONITOR now_mon = MonitorFromWindow(s_dxgi_hwnd, MONITOR_DEFAULTTONEAREST);
            int exact, recheck = 0;
            unsigned num, den;
            uint64_t p;
            look = t;
            if (now_mon && now_mon != mon) {
                recheck = mon != NULL;     /* another display: its own HDR state */
                IDXGIOutput *o2 = dxgi_output_for(now_mon);
                if (o2) {
                    if (mon)
                        fprintf(stderr, "[PACING] display: the window moved to another"
                                " monitor; the vblank lock follows that one\n");
                    IDXGIOutput_Release(out);
                    out = o2;
                    mon = now_mon;
                    m->period_ns = ~0ull;      /* read below, and start over */
                }
            }
            p = dxgi_mode_period(mon, &exact, &num, &den);
            if (p != m->period_ns || exact != m->exact) {
                fprintf(stderr, "[PACING] display mode: %.3f Hz (%u/%u%s)%s\n",
                        p ? 1e9 / (double)p : 0.0, num, den,
                        exact ? ", QueryDisplayConfig" : p ? ", whole Hz only" : ", unknown",
                        m->period_ns != ~0ull && m->period_ns ? ", changed" : "");
                dvm_reset(m, p, exact);
                last = 0;
            }
            s_dxgi_period = m->period_ns;
            /* BO-207: Windows HDR switched on or off for this display, or
             * the window on another one: the swapchain is made again (and
             * make_swapchain asks DXGI itself). A factory stays current
             * until the outputs or their colour state change (Microsoft's
             * pattern): IsCurrent() is microseconds, the enumeration is not,
             * and this thread waits for every vblank. */
            if (!fac || !IDXGIFactory1_IsCurrent(fac)) {
                if (fac)
                    IDXGIFactory1_Release(fac);
                fac = dxgi_factory();
                recheck = 1;
            }
            if (recheck && fac && mon && s_win_hdr >= 0) {
                float lum[3];
                int h = dxgi_monitor_hdr(fac, mon, lum);
                if (h >= 0 && h != s_win_hdr) {
                    fprintf(stderr, "[HDR] Windows HDR is now %s for the window's display:"
                            " the swapchain is made again\n", h ? "on" : "off");
                    s_win_hdr = h;
                    s_hdr_recheck = 1;
                }
            }
        }
        if (FAILED(IDXGIOutput_WaitForVBlank(out))) {
            /* A lost output (mode change, display off): back off, never spin. */
            Sleep(fails < 8 ? 16 : 250);
            fails++;
            continue;
        }
        fails = 0;
        t = mono_ns();
        /* An output that is asleep answers at once, with success: this
         * thread then spun at time-critical priority, a core gone and, on a
         * loaded PC, the title's core with it (BO-75: the monitor had gone
         * to sleep before a run; under sixteen CPU-bound jobs the title
         * stood for up to 0.7 s with this thread on its core). No refresh
         * comes within half a millisecond of the last: three in a row is
         * not a display, so nothing is reported and the thread sleeps as
         * for a lost output, until a wait takes its time again. */
        if (last && t - last < 500000ull) {
            if (++fast >= 3) {
                Sleep(fast < 64 ? 16 : 250);
                last = mono_ns();
                continue;
            }
        } else {
            fast = 0;
        }
        /* With present_wait the waiter keeps the displayed-frame statistics. */
        if (!s_waiter_run && last && t - last < 60000000ull) {
            pthread_mutex_lock(&s_tm_mu);
            iv_add(&s_disp_iv, t);
            pthread_mutex_unlock(&s_tm_mu);
        }
        last = t;
        s_vb_ring[s_vb_n % VB_RING] = t;
        __atomic_add_fetch(&s_vb_n, 1, __ATOMIC_RELEASE);
        r = dvm_refresh(m, t, &when, &per);
        s_dxgi_vrr = m->state;     /* DVM_UNKNOWN -1, DVM_FIXED 0, DVM_VRR 1 */
        if (r & DVM_CHANGED)
            dxgi_say_state(m);
        if (r & DVM_REPORT)
            xbox_VblankDisplayRefresh(when, per);
        if (s_csv)
            fprintf(s_csv, "0,display,%llu,\n", (unsigned long long)t);
    }
    free(m);
    if (fac)
        IDXGIFactory1_Release(fac);
    IDXGIOutput_Release(out);
    return 0;
}

static void dxgi_vblank_start(SDL_Window *win)
{
    HWND hw;
    IDXGIOutput *found;

    if (!xbox_VblankDisplayLockOn())
        return;
    if (!(hw = window_hwnd(win)))
        return;
    if (!dxgi_load()) {
        fprintf(stderr, LOGP "vblank sync: no DXGI; the vblank keeps its own 60 Hz timer\n");
        return;
    }
    s_dxgi_hwnd = hw;
    found = dxgi_output_for(MonitorFromWindow(s_dxgi_hwnd, MONITOR_DEFAULTTONEAREST));
    if (!found) {
        fprintf(stderr, LOGP "vblank sync: the window's monitor has no DXGI output; the"
                " vblank keeps its own 60 Hz timer\n");
        return;
    }
    s_dxgi_run = 1;
    s_dxgi_lock = 1;     /* from here on the waiter does not feed the lock */
    s_dxgi_th = CreateThread(NULL, 0, dxgi_vblank_main, found, 0, NULL);
    if (!s_dxgi_th) {
        s_dxgi_run = 0;
        s_dxgi_lock = 0;
        IDXGIOutput_Release(found);
        return;
    }
    fprintf(stderr, LOGP "vblank sync: the emulated vblank follows the output's vertical"
            " blank on a fixed-rate display, and its own 60 Hz under variable refresh"
            " (DXGI WaitForVBlank, RECOMP_VBLANK_SYNC=display)\n");
}

static void dxgi_vblank_stop(void)
{
    if (!s_dxgi_th)
        return;
    s_dxgi_run = 0;
    /* The thread may be inside WaitForVBlank (one refresh at most). */
    WaitForSingleObject(s_dxgi_th, 500);
    CloseHandle(s_dxgi_th);
    s_dxgi_th = NULL;
    s_dxgi_lock = 0;
}
#endif

int fbv_load_library(char *err, size_t errn)
{
    const char *path = NULL;

    /* Our own loader first (vk_dyn.c): without one there is nothing for SDL
     * to load either, and the reason is the useful part. */
#if defined(XR_VK_DYNAMIC)
    if (!xr_vk_loader_ok()) {
        snprintf(err, errn, "%s", xr_vk_loader_error());
        return 0;
    }
#endif
    /* The loader the runtime calls: SDL_Vulkan_CreateSurface must use the
     * same instance functions as the instance made here. On Android that is
     * the one vk_dyn.c opened (a replacement driver's loader is not the
     * system's), which SDL finds through the vkGetInstanceProcAddr this
     * library exports. On Windows and Linux it is vulkan-1.dll or
     * libvulkan.so.1, which SDL's default load opens by the same name and
     * the system hands out as the copy already mapped. */
#if defined(__ANDROID__)
    Dl_info di;
    if (dladdr((void *)&vkGetInstanceProcAddr, &di) && di.dli_fname)
        path = di.dli_fname;
#endif
    if (SDL_Vulkan_LoadLibrary(path) != 0 && (!path || SDL_Vulkan_LoadLibrary(NULL) != 0)) {
        snprintf(err, errn, "SDL_Vulkan_LoadLibrary(%s): %s", path ? path : "default",
                 SDL_GetError());
        return 0;
    }
    return 1;
}

static VkSurfaceKHR make_surface(VkInstance inst, void *ctx)
{
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)ctx, inst, &s)) {
        XR_ERROR( LOGP "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return VK_NULL_HANDLE;
    }
#if defined(__ANDROID__)
    {
        /* 60 fps asked of the system for this surface (android_perf.c): the
         * first one and each one after the app comes back. */
        extern void xbox_AndroidSetFrameRate(void *window, const char *why);
        static int n;
        SDL_SysWMinfo wi;
        SDL_VERSION(&wi.version);
        if (SDL_GetWindowWMInfo((SDL_Window *)ctx, &wi) && wi.subsystem == SDL_SYSWM_ANDROID)
            xbox_AndroidSetFrameRate(wi.info.android.window, n++ ? "new" : "first");
    }
#endif
    return s;
}

static void destroy_swapchain(void)
{
    uint32_t i;
    for (i = 0; i < SC_MAX; i++) {
        if (s_done[i])
            vkDestroySemaphore(C->dev, s_done[i], NULL);
        s_done[i] = VK_NULL_HANDLE;
        if (s_sc_view[i])
            vkDestroyImageView(C->dev, s_sc_view[i], NULL);
        s_sc_view[i] = VK_NULL_HANDLE;
    }
    if (s_sc)
        vkDestroySwapchainKHR(C->dev, s_sc, NULL);
    s_sc = VK_NULL_HANDLE;
    s_sc_n = 0;
}

/* A window wait that did not prove the GPU done (nv2a_vk_core.h): the
 * window draws no more in this run, and unless the run is ending anyway the
 * device is reported lost (the player is told, the run ends). */
static int s_gpu_dead;

static void gpu_wait_failed(int outcome, VkResult r, const char *what)
{
    if (!s_gpu_dead)
        fprintf(stderr, LOGP "%s: the GPU wait failed (%s): the window draws no more\n", what,
                outcome == NVK_WAIT_TIMEOUT ? "timed out for good"
                : outcome == NVK_WAIT_CANCELLED ? "cancelled" : nvk_result_str(r));
    s_gpu_dead = 1;
    nvk_wait_fatal(outcome, r, what);
}

static int gpu_gone(void)
{
    return s_gpu_dead || nvk_core_lost();
}

static void queue_idle(void)
{
    nvk_queue_lock();
    vkQueueWaitIdle(C->queue);
    nvk_queue_unlock();
}

/* An Android app that leaves the screen (Home, recents, screen off) has its
 * window's surface destroyed, and gets another when it comes back. The
 * VkSurfaceKHR and the swapchain made on the old one are dead then: both are
 * dropped and made again on the window's new surface. Marked from the app's
 * life-cycle events (fbv_android_surface_gone, fb_present_sdl.c) and from
 * any VK_ERROR_SURFACE_LOST_KHR, on every platform (BO-118: a desktop whose
 * display went away and came back; until this the lost surface was asked
 * for an image every frame for the rest of the run). Off Android a surface
 * that cannot be made yet is tried twice a second, not every frame. */
static volatile int s_surface_lost;
/* A gap in the run (host_time.h): the swapchain is made again once. */
static volatile int s_wake_recreate;
static VkSurfaceKHR make_surface(VkInstance inst, void *ctx);
static void destroy_swapchain(void);

#if defined(__ANDROID__)
void fbv_android_surface_gone(void)
{
    s_surface_lost = 1;
}
#endif

static void fbv_woke(uint64_t gap_ns)
{
    (void)gap_ns;
    s_wake_recreate = 1;
}

/* The pacing watchdog's reset (BO-211), on the executor: the keeper's
 * refresh period, depth row and (Windows) anchor start over. */
static void fbv_pacing_reset(void)
{
    s_keep_reset_waiter = 1;
    s_keep_reset_main = 1;
}

/* RECOMP_DEBUG=vk_present_fail=at:n[:what] (test hook): from second `at`
 * after the first present, n acquires in a row do not happen and report
 * `what` instead. VK_SUCCESS: no failure this time. */
static VkResult acquire_hook(void)
{
    static int parsed, left;
    static double at = -1.0;
    static VkResult what = VK_ERROR_OUT_OF_DATE_KHR;
    static uint64_t t0;

    if (!parsed) {
        const char *e = recomp_env(RENV_VK_PRESENT_FAIL), *c;
        parsed = 1;
        t0 = xbox_HostNowNs();
        if (e && *e) {
            at = atof(e);
            c = strchr(e, ':');
            left = c ? atoi(c + 1) : 1;
            c = c ? strchr(c + 1, ':') : NULL;
            if (c && !strcmp(c + 1, "surface"))
                what = VK_ERROR_SURFACE_LOST_KHR;
            else if (c && !strcmp(c + 1, "timeout"))
                what = VK_TIMEOUT;
        }
    }
    if (at < 0.0 || left <= 0 || (double)(xbox_HostNowNs() - t0) / 1e9 < at)
        return VK_SUCCESS;
    left--;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "test hook: %d acquires fail with %s\n", left + 1,
                    what == VK_TIMEOUT ? "VK_TIMEOUT"
                    : what == VK_ERROR_SURFACE_LOST_KHR ? "VK_ERROR_SURFACE_LOST_KHR"
                    : "VK_ERROR_OUT_OF_DATE_KHR");
    }
    return what;
}

#if defined(_WIN32) && defined(VK_KHR_present_wait2)
static int surface_takes_wait2(void);
#endif
static int resurface(SDL_Window *win)
{
    static unsigned said;
    if (!C)
        return 0;
#if !defined(__ANDROID__)
    {
        static uint64_t tried_ns;
        uint64_t now = xbox_HostNowNs();
        if (tried_ns && now - tried_ns < 500000000ull)
            return 0;
        tried_ns = now;
    }
#endif
    queue_idle();
    pthread_mutex_lock(&s_sc_mu);
    destroy_swapchain();
    pthread_mutex_unlock(&s_sc_mu);
    if (!nvk_core_replace_surface(make_surface, win)) {
        if (said++ < 3)
            fprintf(stderr, LOGP "no surface for the window yet; trying again\n");
        return 0;
    }
    s_surface_lost = 0;
    s_recreate = 1;
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    if (C->present_wait2) {
        int was = s_pw2_ok;
        s_pw2_ok = surface_takes_wait2();      /* the new surface's own answer */
        if (was && !s_pw2_ok)
            fprintf(stderr, LOGP "present_wait2: the new surface does not take it; no per-frame"
                    " display times from here on\n");
    }
#endif
    fprintf(stderr, LOGP "the window's surface was replaced: a new VkSurfaceKHR and swapchain\n");
    return 1;
}

#if defined(__linux__) && !defined(__ANDROID__)
/* BO-110: the display's peak luminance from an EDID's CTA-861 HDR static
 * metadata block (max luminance code value v: 50 x 2^(v/32) nits), 0 when
 * it has none. */
static float edid_peak(const uint8_t *e, size_t n)
{
    size_t b, i;
    for (b = 128; b + 128 <= n; b += 128) {
        const uint8_t *x = e + b;
        size_t end;
        if (x[0] != 0x02)
            continue;
        end = x[2];
        if (end < 4 || end > 127)
            continue;
        for (i = 4; i < end;) {
            unsigned tag = x[i] >> 5, len = x[i] & 31u;
            if (tag == 7 && len >= 3 && x[i + 1] == 6 && len >= 4) {
                unsigned cv = x[i + 4];
                if (cv)
                    return 50.0f * powf(2.0f, (float)cv / 32.0f);
            }
            i += 1 + len;
        }
    }
    return 0.0f;
}

static float edid_file_peak(const char *path)
{
    uint8_t buf[1024];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0.0f;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n >= 256 ? edid_peak(buf, n) : 0.0f;
}
#endif

/* The peak of the display the game shows on: gamescope's EDID (what it
 * tells clients, GAMESCOPE_PATCHED_EDID_FILE), else a connected and enabled
 * DRM connector's (an external one first), else 1000 nits. */
static float hdr_display_peak(void)
{
    float pk = 0.0f;
#if defined(__linux__) && !defined(__ANDROID__)
    const char *g = getenv("GAMESCOPE_PATCHED_EDID_FILE");
    if (g && *g)
        pk = edid_file_peak(g);
    if (pk <= 0.0f) {
        static const char *const cand[] = {
            "/sys/class/drm/card0-DP-1", "/sys/class/drm/card0-DP-2", "/sys/class/drm/card0-HDMI-A-1",
            "/sys/class/drm/card1-DP-1", "/sys/class/drm/card1-DP-2", "/sys/class/drm/card1-HDMI-A-1",
            "/sys/class/drm/card0-eDP-1", "/sys/class/drm/card1-eDP-1",
        };
        unsigned i;
        for (i = 0; i < sizeof cand / sizeof cand[0] && pk <= 0.0f; i++) {
            char path[128], st[32] = "", en[32] = "";
            FILE *f;
            snprintf(path, sizeof path, "%s/status", cand[i]);
            if ((f = fopen(path, "r")) != NULL) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
            snprintf(path, sizeof path, "%s/enabled", cand[i]);
            if ((f = fopen(path, "r")) != NULL) { if (!fgets(en, sizeof en, f)) en[0] = 0; fclose(f); }
            if (strncmp(st, "connected", 9) || strncmp(en, "enabled", 7))
                continue;
            snprintf(path, sizeof path, "%s/edid", cand[i]);
            pk = edid_file_peak(path);
        }
    }
#endif
    return pk >= 100.0f ? pk : 1000.0f;
}

/* BO-110: the swapchain's HDR10 metadata: the content's peak as the
 * player's setting maps it, BT.2020 primaries, D65. */
static void hdr_metadata(void)
{
    static PFN_vkSetHdrMetadataEXT set;
    VkHdrMetadataEXT md;
    float pk, pw, hw;
    if (!s_hdr_sc || !C->hdr_metadata)
        return;
    if (!set)
        set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(C->dev, "vkSetHdrMetadataEXT");
    if (!set)
        return;
    nv2a_host_hdr_levels(&pk, &pw, &hw);
    {
        /* Sent again only when a level changed: each call is a message to
         * the compositor, and gamescope rebuilds its colour pipeline for
         * it, showing the frame on its way a refresh late (the Deck's
         * periodic hitch of the first HDR builds, which sent it every 120
         * flips, and a frame replaced in the slot before it was shown). */
        static float last[3] = { -1.0f, -1.0f, -1.0f };
        static VkSwapchainKHR last_sc;
        if (last_sc == s_sc && last[0] == pk && last[1] == pw && last[2] == hw)
            return;
        last_sc = s_sc;
        last[0] = pk;
        last[1] = pw;
        last[2] = hw;
    }
    memset(&md, 0, sizeof md);
    md.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
    md.displayPrimaryRed.x = 0.708f;   md.displayPrimaryRed.y = 0.292f;
    md.displayPrimaryGreen.x = 0.170f; md.displayPrimaryGreen.y = 0.797f;
    md.displayPrimaryBlue.x = 0.131f;  md.displayPrimaryBlue.y = 0.046f;
    md.whitePoint.x = 0.3127f;         md.whitePoint.y = 0.3290f;
    md.maxLuminance = pk;
    md.minLuminance = 0.0f;
    md.maxContentLightLevel = pk;
    md.maxFrameAverageLightLevel = pw > hw ? pw : hw;
    set(C->dev, 1, &s_sc, &md);
    {
        static int said;
        if (said++ < 20)
            fprintf(stderr, LOGP "HDR10 metadata set: BT.2020, D65, max %.0f nits, MaxCLL %.0f,"
                    " MaxFALL %.0f\n", md.maxLuminance, md.maxContentLightLevel,
                    md.maxFrameAverageLightLevel);
    }
}

/* display.present_mode with vsync on (RECOMP_PRESENT_MODE): auto or fifo
 * is FIFO, mailbox is MAILBOX where the surface offers it. MAILBOX is never
 * picked by itself: under gamescope it shortens the queue by a frame, but
 * every present later than its margin then misses its refresh and an early
 * one replaces the frame before it (BO-110 A/B, arm D: 4-10x the repeated
 * frames of FIFO, and discards). display.frame_pacing is the setting for the
 * queue under FIFO. */
static int present_mode_mailbox(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = recomp_env(RENV_PRESENT_MODE);
        v = e && !strcmp(e, "mailbox");
        if (e && *e && strcmp(e, "auto") && strcmp(e, "fifo") && strcmp(e, "mailbox"))
            fprintf(stderr, LOGP "display.present_mode=%s: auto, fifo or mailbox; using FIFO\n", e);
        fprintf(stderr, LOGP "present mode with vsync: %s (display.present_mode %s)\n",
                v ? "MAILBOX" : "FIFO", e && *e ? e : "auto");
    }
    return v;
}

static int s_sc_failed;      /* vkCreateSwapchainKHR failures in a row */

#if defined(_WIN32) && defined(VK_KHR_present_wait2)
/* BO-207: the surface takes present_id2 and present_wait2. */
static int surface_takes_wait2(void)
{
    static PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get;
    VkPhysicalDeviceSurfaceInfo2KHR si;
    VkSurfaceCapabilities2KHR c2;
    VkSurfaceCapabilitiesPresentId2KHR id2;
    VkSurfaceCapabilitiesPresentWait2KHR w2;
    if (!C->surface_caps2)
        return 0;
    if (!get)
        get = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(
            C->inst, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (!get)
        return 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    si.surface = C->surface;
    memset(&w2, 0, sizeof w2);
    w2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
    memset(&id2, 0, sizeof id2);
    id2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    id2.pNext = &w2;
    memset(&c2, 0, sizeof c2);
    c2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    c2.pNext = &id2;
    if (get(C->phys, &si, &c2) != VK_SUCCESS)
        return 0;
    return id2.presentId2Supported && w2.presentWait2Supported;
}
#endif

/* The window's drawable size. The presents run on the present thread
 * (fb_present_sdl.c, BO-210); on macOS (MoltenVK) SDL reads it from the
 * NSView, which belongs to the main thread, so there the main thread notes
 * it each turn (fbv_note_drawable) and that is used once noted. */
#if defined(__APPLE__)
static int s_note_w, s_note_h;
void fbv_note_drawable(int w, int h)
{
    __atomic_store_n(&s_note_w, w, __ATOMIC_RELAXED);
    __atomic_store_n(&s_note_h, h, __ATOMIC_RELAXED);
}
#endif
static void drawable_size(SDL_Window *win, int *w, int *h)
{
#if defined(__APPLE__)
    *w = __atomic_load_n(&s_note_w, __ATOMIC_RELAXED);
    *h = __atomic_load_n(&s_note_h, __ATOMIC_RELAXED);
    if (*w > 0 && *h > 0)
        return;
#endif
    SDL_Vulkan_GetDrawableSize(win, w, h);
}

static int make_swapchain(SDL_Window *win)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16];
    uint32_t nf = 64, nm = 16, i, n;
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = s_sc;
    VkSemaphoreCreateInfo si;
    int dw = 0, dh = 0;
    VkResult r;

    queue_idle();
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(C->phys, C->surface, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
    if (r != VK_SUCCESS)
        return 0;
    pthread_mutex_lock(&s_sc_mu);
    vkGetPhysicalDeviceSurfaceFormatsKHR(C->phys, C->surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(C->phys, C->surface, &nm, modes);
    /* UNORM: the frame's bytes go to the display as they are, as the SDL
     * renderer and the CAMetalLayer present show them. */
    s_sc_fmt = fmts[0].format;
    for (i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM
                || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
                && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            s_sc_fmt = fmts[i].format;
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
                break;
        }
    {   /* BO-110: HDR10 offered? (10-bit, ST 2084) */
        int was = s_hdr_offered;
        s_hdr_offered = 0;
        s_hdr_fmt = VK_FORMAT_UNDEFINED;
        for (i = 0; i < nf; i++)
            if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
                    && (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                        || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
                if (!s_hdr_offered || fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
                    s_hdr_fmt = fmts[i].format;
                s_hdr_offered = 1;
            }
#if defined(_WIN32)
        {
            /* BO-207: Windows lists HDR10 surface formats whether or not
             * the display is in HDR; the output's colour space says
             * (Settings > Display > Use HDR), and its peak with it. */
            /* The peak: DXGI's MaxLuminance (the EDID's for the monitor's
             * current mode, or the Windows HDR Calibration app's profile),
             * kept within 400-4000 nits (below, HDR is not worth the switch;
             * above, a broken EDID); 1000 where it says none. */
            float lum[3], pk;
            int h = dxgi_window_hdr(win, lum);
            pk = lum[0] <= 0.0f ? 1000.0f : lum[0] < 400.0f ? 400.0f : lum[0] > 4000.0f ? 4000.0f
               : lum[0];
            if (h != s_win_hdr || !s_sc)
                fprintf(stderr, LOGP "Windows HDR for the window's display: %s%s; DXGI max %.0f,"
                        " max full frame %.0f, min %.4f nits: peak %.0f%s\n",
                        h > 0 ? "on" : h == 0 ? "off" : "not known",
                        h > 0 && !s_hdr_offered ? " but the surface lists no HDR10 format" : "",
                        lum[0], lum[1], lum[2], pk,
                        lum[0] <= 0.0f ? " (not reported, 1000 assumed)"
                        : pk != lum[0] ? " (kept within 400-4000)" : "");
            s_win_hdr = h;
            if (h <= 0)
                s_hdr_offered = 0;
            else
                s_hdr_peak = pk;
        }
#endif
        if (s_hdr_offered && s_hdr_peak <= 0.0f)
            s_hdr_peak = hdr_display_peak();
        nv2a_host_hdr_display(s_hdr_offered ? s_hdr_peak / 100.0f : 1.0f,
                              s_hdr_offered ? s_hdr_peak / 100.0f : 1.0f);
        {   /* RECOMP_DEBUG=hdr without HDR10 (a headless test): the HDR rows
             * as on a 1000-nit display, for the overlay's own tests. */
            const char *e = recomp_env(RENV_HDR);
            if (!s_hdr_offered && e && *e && strcmp(e, "0") != 0)
                nv2a_host_hdr_display(10.0f, 10.0f);
        }
        if (s_hdr_offered != was || !s_sc)
            fprintf(stderr, LOGP "HDR10 output %s (%u surface formats), display peak %.0f nits\n",
                    s_hdr_offered ? "offered" : "not offered", nf,
                    s_hdr_offered ? s_hdr_peak : 0.0f);
    }
    s_sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (s_vsync && present_mode_mailbox())
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                s_sc_mode = modes[i];
    if (!s_vsync)
        for (i = 0; i < nm; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                s_sc_mode = modes[i];
                break;
            }
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                s_sc_mode = modes[i];
        }
    drawable_size(win, &dw, &dh);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        s_sc_ext = caps.currentExtent;
    } else {
        s_sc_ext.width = (uint32_t)(dw > 0 ? dw : 640);
        s_sc_ext.height = (uint32_t)(dh > 0 ? dh : 480);
    }
    if (s_sc_ext.width < caps.minImageExtent.width) s_sc_ext.width = caps.minImageExtent.width;
    if (s_sc_ext.height < caps.minImageExtent.height) s_sc_ext.height = caps.minImageExtent.height;
    if (s_sc_ext.width > caps.maxImageExtent.width) s_sc_ext.width = caps.maxImageExtent.width;
    if (s_sc_ext.height > caps.maxImageExtent.height) s_sc_ext.height = caps.maxImageExtent.height;
    if (!s_sc_ext.width || !s_sc_ext.height) {
        pthread_mutex_unlock(&s_sc_mu);
        return 0;     /* minimised */
    }
#if defined(__ANDROID__)
    s_and_w = (uint32_t)dw;
    s_and_h = (uint32_t)dh;
    {
        const char *e = getenv("RECOMP_ANDROID_SWAPCHAIN_HEIGHT");
        uint32_t rows = e ? (uint32_t)strtoul(e, NULL, 10) : 0;
        if (rows >= 240 && rows < s_sc_ext.height) {
            s_sc_ext.width = ((s_sc_ext.width * rows / s_sc_ext.height) + 1u) & ~1u;
            s_sc_ext.height = rows;
            if (s_sc_ext.width < caps.minImageExtent.width) s_sc_ext.width = caps.minImageExtent.width;
        }
    }
#endif
    /* The fewest images: one shown, at most one queued (BO-74 latency). */
    n = caps.minImageCount;
    if (n < 2) n = 2;
#if defined(_WIN32)
    /* BO-207: Smooth keeps one frame queued: one shown, one queued, one
     * being drawn. */
    s_keep_images = pacing_smooth();
    if (s_keep_images && n < 3) n = 3;
#endif
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    if (n > SC_MAX) n = SC_MAX;
    s_sc_shots = s_shots && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = C->surface;
    ci.minImageCount = n;
    s_hdr_sc = s_hdr_offered && (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
               && hdr_wanted();
    if (s_hdr_sc) {
        s_sc_fmt = s_hdr_fmt;
        s_sc_shots = 0;
    }
    ci.imageFormat = s_sc_fmt;
    ci.imageColorSpace = s_hdr_sc ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = s_sc_ext;
    ci.imageArrayLayers = 1;
    s_sc_color = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  | (s_sc_shots ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)
                  | (s_sc_color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
#if defined(__ANDROID__)
    /* A panel mounted sideways reports a rotation here, and a swapchain made
     * with it must be drawn rotated. This presenter does not: identity, and
     * the compositor turns the picture. */
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha
                                                     & -caps.supportedCompositeAlpha);
    ci.presentMode = s_sc_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    if (s_pw2_ok)
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
#endif
    r = vkCreateSwapchainKHR(C->dev, &ci, NULL, &s_sc);
    if (old) {
        VkSwapchainKHR keep = s_sc;
        s_sc = old;
        destroy_swapchain();
        s_sc = r == VK_SUCCESS ? keep : VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        s_sc = VK_NULL_HANDLE;
        pthread_mutex_unlock(&s_sc_mu);
        fprintf(stderr, LOGP "vkCreateSwapchainKHR: %s\n", nvk_result_str(r));
        /* The window is there (its surface answered above) and no swapchain
         * can be made for it: after a second of tries that is not a resize
         * in passing. */
        if (r == VK_ERROR_DEVICE_LOST || ++s_sc_failed >= 60)
            nvk_device_lost(r == VK_ERROR_DEVICE_LOST ? "vkCreateSwapchainKHR: VK_ERROR_DEVICE_LOST"
                                                      : "the window's swapchain cannot be made");
        return 0;
    }
    s_sc_failed = 0;
    s_sc_first_id = s_present_id + 1;
    pthread_mutex_unlock(&s_sc_mu);
    hdr_metadata();
    {
        static int last = -1;
        if (last != s_hdr_sc)
            fprintf(stderr, LOGP "swapchain %s\n", s_hdr_sc ? "HDR10 (ST 2084, BT.2020, 10-bit)"
                                                        : "SDR (sRGB)");
        last = s_hdr_sc;
    }
    s_sc_n = SC_MAX;
    vkGetSwapchainImagesKHR(C->dev, s_sc, &s_sc_n, s_sc_img);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < s_sc_n; i++) {
        vkCreateSemaphore(C->dev, &si, NULL, &s_done[i]);
        if (s_sc_color) {
            VkImageViewCreateInfo vi;
            memset(&vi, 0, sizeof vi);
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = s_sc_img[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = s_sc_fmt;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(C->dev, &vi, NULL, &s_sc_view[i]) != VK_SUCCESS)
                s_sc_view[i] = VK_NULL_HANDLE;
        }
    }
    s_recreate = 0;
#if defined(_WIN32)
    if (s_keep_on > 0)
        kw_restart();            /* a new swapchain starts with nothing queued */
    else
        s_kw_down = 0;
#endif
    return 1;
}

int fbv_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn)
{
    unsigned n = 0, i;
    const char **names = NULL;
    VkCommandPoolCreateInfo cp;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;

    s_vsync = vsync;
    s_shots = shots;
    if (!SDL_Vulkan_GetInstanceExtensions(win, &n, NULL)
            || !(names = (const char **)calloc(n ? n : 1, sizeof *names))
            || !SDL_Vulkan_GetInstanceExtensions(win, &n, names)) {
        snprintf(err, errn, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        free(names);
        return 0;
    }
    C = nvk_core_init(names, n, make_surface, win, err, errn);
    free(names);
    if (!C)
        return 0;
    if (!C->surface) {
        snprintf(err, errn, "the Vulkan device was made without the window");
        C = NULL;
        return 0;
    }
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = C->qfam;
    if (vkCreateCommandPool(C->dev, &cp, NULL, &s_pool) != VK_SUCCESS
            || vkCreateCommandPool(C->dev, &cp, NULL, &s_up_pool) != VK_SUCCESS) {
        snprintf(err, errn, "command pool failed");
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 2;
    vkAllocateCommandBuffers(C->dev, &ai, s_cb);
    ai.commandPool = s_up_pool;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(C->dev, &ai, &s_up_cb);
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < 2; i++) {
        vkCreateFence(C->dev, &fi, NULL, &s_fence[i]);
        vkCreateSemaphore(C->dev, &si, NULL, &s_acq[i]);
    }
    vkCreateFence(C->dev, &fi, NULL, &s_up_fence);
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    s_pw2_ok = C->present_wait2 && surface_takes_wait2();   /* per surface (BO-207) */
#endif
    if (!make_swapchain(win)) {
        snprintf(err, errn, "no swapchain for the window");
        return 0;
    }
    {
        const char *csv = recomp_env(RENV_VK_PRESENT_LOG);
        if (csv && strcmp(csv, "1") && (s_csv = fopen(csv, "w")) != NULL)
#if defined(_WIN32)
            fprintf(s_csv, "id,event,t_ns,flip,note,pending\n");   /* BO-207: pending */
#else
            fprintf(s_csv, "id,event,t_ns,flip\n");   /* t_ns: CLOCK_MONOTONIC */
#endif
    }
    if (C->present_wait
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
            && !C->present_wait2         /* VK_KHR_present_wait is not enabled then */
#endif
            )
        s_wait_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(C->dev,
                                                                      "vkWaitForPresentKHR");
    if (C->display_timing)
        s_get_cycle = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(C->dev,
                                                       "vkGetRefreshCycleDurationGOOGLE");
    {
        int can_wait = C->present_wait && s_wait_present;
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
        if (C->present_wait2) {
            s_wait_present2 = (PFN_vkWaitForPresent2KHR)vkGetDeviceProcAddr(C->dev,
                                                                            "vkWaitForPresent2KHR");
            can_wait = s_wait_present2 && s_pw2_ok;
            if (!can_wait)
                fprintf(stderr, LOGP "present_wait2: %s; no per-frame display times\n",
                        s_wait_present2 ? "the surface does not take it" : "no vkWaitForPresent2KHR");
        }
#endif
        if (can_wait) {
            s_waiter_run = 1;
            if (pthread_create(&s_waiter, NULL, waiter_main, NULL) != 0)
                s_waiter_run = 0;
        }
    }
#if defined(_WIN32)
    dxgi_vblank_start(win);     /* on every GPU, present_wait or not (BO-191) */
#endif
#if defined(__ANDROID__)
    /* No present_wait through Android's loader: the Choreographer's refresh
     * times instead (android_vsync.c). RECOMP_ANDROID_VSYNC=0: the old
     * free-running timer, for A/B. */
    if (!s_waiter_run && !(getenv("RECOMP_ANDROID_VSYNC") && getenv("RECOMP_ANDROID_VSYNC")[0] == '0')) {
        extern void xbox_AndroidVsyncStart(void);
        xbox_AndroidVsyncStart();
    }
#endif
    fprintf(stderr, LOGP "Vulkan swapchain: %ux%u, %u images, %s, %s%s\n", s_sc_ext.width,
            s_sc_ext.height, s_sc_n, s_hdr_sc ? "10-bit HDR10" : s_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8",
            s_sc_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)"
            : s_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "IMMEDIATE",
            s_waiter_run ? ", present_wait timing" : "");
    return 1;
}

void fbv_destroy(void)
{
    int k;
    if (!C)
        return;
#if defined(_WIN32)
    dxgi_vblank_stop();
#endif
#if defined(__ANDROID__)
    {
        extern void xbox_AndroidVsyncStop(void);
        xbox_AndroidVsyncStop();
    }
#endif
    if (s_waiter_run) {
        pthread_mutex_lock(&s_id_mu);
        s_waiter_run = 0;
        pthread_cond_broadcast(&s_sc_cv);
        pthread_mutex_unlock(&s_id_mu);
        pthread_join(s_waiter, NULL);
    }
    fbv_timing_report("presents at exit");
    if (s_csv)
        fclose(s_csv);
    s_csv = NULL;
    queue_idle();
    post_destroy();
    for (k = 0; k < 3; k++)
        nvk_image_free(&s_slot[k]);
    nvk_buffer_free(&s_up_buf);
    nvk_buffer_free(&s_shot_buf);
    nvk_buffer_free(&s_lat_buf[0]);
    nvk_buffer_free(&s_lat_buf[1]);
    s_lat_pend[0] = s_lat_pend[1] = 0;
    destroy_swapchain();
    /* The device, surface and pools stay: the backend holds the device and
     * the process is about to exit. */
}

void *fbv_slot(int k, uint32_t w, uint32_t h)
{
    struct nvk_image *s = &s_slot[k];
    if (!C)
        return NULL;
    VkFormat sf = hdr_wanted() ? HDR_SLOT_FMT : VK_FORMAT_B8G8R8A8_UNORM;   /* BO-110 */
    if (!s->img || s->w != w || s->h != h || s->format != sf) {
        if (s->img) {
            /* A present may still read it: rare (the frame size changed).
             * Freed only once the queue is proven idle. */
            static VkFence idle;
            VkResult r = VK_SUCCESS;
            int out;
            if (gpu_gone())
                return NULL;
            if (!idle) {
                VkFenceCreateInfo fi;
                memset(&fi, 0, sizeof fi);
                fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                if (vkCreateFence(C->dev, &fi, NULL, &idle) != VK_SUCCESS)
                    idle = VK_NULL_HANDLE;
            }
            if (!idle)
                return NULL;
            out = nvk_wait_queue(NVK_WAIT_SLOT, "window slot resize", idle, &r);
            if (out != NVK_WAIT_OK) {
                gpu_wait_failed(out, r, "window slot resize");
                return NULL;
            }
            nvk_image_free(s);
        }
        if (!nvk_image_new(s, sf, w, h, 1,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           /* The post passes (AA, sharpen) sample the slot: without
                            * SAMPLED the read is undefined (black on AMD's Windows
                            * driver; RADV happened to tolerate it). */
                           | VK_IMAGE_USAGE_SAMPLED_BIT
                           /* BO-110: the backend's HDR map draws into it. */
                           | (sf == HDR_SLOT_FMT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                           VK_IMAGE_ASPECT_COLOR_BIT))
            return NULL;
    }
    return s;
}

int fbv_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
               uint32_t bpp)
{
    struct nvk_image *s;
    VkCommandBufferBeginInfo bi;
    VkBufferImageCopy bc;
    VkSubmitInfo si;
    VkDeviceSize need = (VkDeviceSize)w * h * 4u;
    uint32_t x, y;
    VkResult r;

    if (!C || gpu_gone())
        return 0;
    if (s_up_pending) {
        /* The last upload still reads s_up_buf: not overwritten, and its
         * fence not reset, before it is proven done. */
        VkResult wr = VK_SUCCESS;
        int out = nvk_wait_fence(NVK_WAIT_UPLOAD, "window upload", s_up_fence, &wr);
        if (out != NVK_WAIT_OK) {
            gpu_wait_failed(out, wr, "window upload");
            return 0;
        }
        s_up_pending = 0;
    }
    if (s_up_buf.size < need) {
        nvk_buffer_free(&s_up_buf);
        if (!nvk_buffer_new(&s_up_buf, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
            return 0;
    }
    s = (struct nvk_image *)fbv_slot(k, w, h);
    if (!s)
        return 0;
    if (s->format == HDR_SLOT_FMT) {
        /* BO-110: a 10-bit HDR slot: the bytes as the backend's map leaves
         * a frame (the title's encoding at the HUD's white, gamma 2.2 or
         * sRGB, BT.709 to BT.2020, PQ). Read from src and written to the
         * mapping once, in order: s_up_buf is write-combined (uncached), and
         * converting there in place read every pixel back at ~100 ms a
         * 640x480 frame (15 fps on the guest-bytes path, BO-207). */
        static float lin8[256], lv[3];
        static uint16_t pq[4097];
        static int srgb_was = -1;
        float pk, pw, hw, sh, hl;
        int srgb;
        uint32_t i;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        nv2a_host_hdr_tone(&sh, &hl, &srgb);
        if (srgb != srgb_was || lv[0] != hw) {
            for (i = 0; i < 256; i++) {
                float c = (float)i / 255.0f;
                lin8[i] = srgb ? (c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f))
                               : powf(c, 2.2f);
            }
            /* PQ code of x^2 * hw nits, x in [0, 1] on 4096 steps (fine near black). */
            for (i = 0; i <= 4096; i++) {
                float x = (float)i / 4096.0f, l = x * x * hw / 10000.0f;
                float m = powf(l, 0.1593017578125f);
                float v = powf((0.8359375f + 18.8515625f * m) / (1.0f + 18.6875f * m), 78.84375f);
                pq[i] = (uint16_t)(v * 1023.0f + 0.5f);
            }
            srgb_was = srgb;
            lv[0] = hw;
        }
        for (y = 0; y < h; y++) {
            const uint8_t *row = src + (size_t)y * pitch;
            uint32_t *o = (uint32_t *)s_up_buf.map + (size_t)y * w;
            for (x = 0; x < w; x++) {
                uint32_t v, q[3], c3;
                float r, g, b, c[3];
                if (bpp == 4) {
                    v = ((const uint32_t *)row)[x];
                } else {
                    uint32_t p16 = ((const uint16_t *)row)[x];
                    v = (((p16 >> 11) & 0x1F) << 19) | (((p16 >> 5) & 0x3F) << 10)
                        | ((p16 & 0x1F) << 3);
                }
                r = lin8[(v >> 16) & 0xFF];
                g = lin8[(v >> 8) & 0xFF];
                b = lin8[v & 0xFF];
                c[0] = 0.6274f * r + 0.3293f * g + 0.0433f * b;
                c[1] = 0.0691f * r + 0.9195f * g + 0.0114f * b;
                c[2] = 0.0164f * r + 0.0880f * g + 0.8956f * b;
                for (c3 = 0; c3 < 3; c3++) {
                    float xv = c[c3] > 0.0f ? sqrtf(c[c3] > 1.0f ? 1.0f : c[c3]) : 0.0f;
                    q[c3] = pq[(int)(xv * 4096.0f + 0.5f)];
                }
                o[x] = 0xC0000000u | (q[2] << 20) | (q[1] << 10) | q[0];   /* A2B10G10R10 */
            }
        }
    } else {
        for (y = 0; y < h; y++) {
            uint32_t *o = (uint32_t *)s_up_buf.map + (size_t)y * w;
            if (bpp == 4) {
                memcpy(o, src + (size_t)y * pitch, (size_t)w * 4u);
                continue;
            }
            for (x = 0; x < w; x++) {
                uint32_t v = ((const uint16_t *)(src + (size_t)y * pitch))[x];
                uint32_t b = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2, rr = ((v >> 11) & 0x1F) << 3;
                o[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
            }
        }
    }
    vkResetCommandBuffer(s_up_cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_up_cb, &bi);
    nvk_image_barrier(s_up_cb, s->img, VK_IMAGE_ASPECT_COLOR_BIT,
                      s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED
                                                             : VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    s->layout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_up_cb, s_up_buf.buf, s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vkEndCommandBuffer(s_up_cb);
    vkResetFences(C->dev, 1, &s_up_fence);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_up_cb;
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, s_up_fence);
    if (r == VK_SUCCESS) {
        char what[96];
        snprintf(what, sizeof what, "window upload %ux%u", w, h);
        nvk_submit_note(what);
    }
    nvk_queue_unlock();
    s_up_pending = r == VK_SUCCESS;
    return r == VK_SUCCESS;
}

static void save_shot(const uint8_t *px, uint32_t w, uint32_t h, const char *path,
                      uint32_t flip)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 24,
                                                       SDL_PIXELFORMAT_BGR24);
    int rgba = s_sc_fmt == VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t x, y;

    if (!surf)
        return;
    for (y = 0; y < h; y++) {
        uint8_t *o = (uint8_t *)surf->pixels + (size_t)y * surf->pitch;
        const uint8_t *in = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            o[x * 3 + 0] = in[x * 4 + (rgba ? 2 : 0)];
            o[x * 3 + 1] = in[x * 4 + 1];
            o[x * 3 + 2] = in[x * 4 + (rgba ? 0 : 2)];
        }
    }
    if (SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, LOGP "window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, LOGP "window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

/* ---- the post passes ------------------------------------------------------
 *
 * FXAA or SMAA's three passes, then CAS (post_shaders.h, smaa.h), as
 * fullscreen-triangle passes with dynamic rendering at the frame's own size:
 * the slot (or the pass before) is the input, an image of the slot's size the
 * target, and the scale to the window then blits the last target instead of
 * the slot. GLSL compiled at the first use (the SPIR-V cache serves it
 * after), a pipeline each. A failure leaves that pass off, logged once; the
 * present is unaffected. A target exists only while its setting is on: it is
 * made at the first frame that needs it and freed (after the queue is idle)
 * when no setting does. One descriptor set per pass and frame in flight,
 * rewritten each frame: the fence wait at the top of fbv_present has retired
 * its last use.
 *
 * RECOMP_TRACE=post_gpu: each pass's GPU time, from timestamp queries around
 * it, read a frame later and averaged every 300 presents. */

static const char s_post_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 vUv;\n"
    "void main() {\n"
    "    vUv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define POST_GLSL_HEAD \
    "#version 450\n" \
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n" \
    "#define saturate(x) clamp((x), 0.0, 1.0)\n" \
    "layout(set = 0, binding = 0) uniform sampler2D tex0;\n" \
    "layout(push_constant) uniform PC { vec2 rcpv; float sharpv; } pc;\n" \
    "layout(location = 0) in vec2 vUv;\n" \
    "layout(location = 0) out vec4 outColor;\n" \
    "#define FETCH(p) texture(tex0, (p)).rgb\n" \
    "#define RETURN(c) { outColor = vec4((c), 1.0); return; }\n" \
    "void main() {\n" \
    "    vec2 uv = vUv; vec2 rcp = pc.rcpv; float sharp = pc.sharpv;\n"

static const char s_post_fxaa[] = POST_GLSL_HEAD POST_FXAA_BODY "}\n";
static const char s_post_cas[] = POST_GLSL_HEAD POST_CAS_BODY "}\n";

/* SMAA's three: the reference library after a header with the push constant
 * (1/w 1/h w h) and the passes' own bindings. */
static const char s_smaa_head[] =
    "#version 450\n"
    "#define SMAA_GLSL_4\n"
    "#define SMAA_PRESET_HIGH\n"
    "layout(push_constant) uniform PC { vec4 rtm; } pc;\n"
    "#define SMAA_RT_METRICS pc.rtm\n"
    "layout(location = 0) in vec2 vUv;\n"
    "layout(location = 0) out vec4 outColor;\n";
static const char *const s_smaa_bind[3] = {
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n",
    "layout(set = 0, binding = 0) uniform sampler2D edgesTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D areaTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D searchTex;\n",
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n",
};
static const char *const s_smaa_main[3] = {
    "void main() { vec4 offset[3]; SMAAEdgeDetectionVS(vUv, offset);\n"
    "    outColor = vec4(SMAALumaEdgeDetectionPS(vUv, offset, colorTex), 0.0, 0.0); }\n",
    "void main() { vec4 offset[3]; vec2 pixcoord; SMAABlendingWeightCalculationVS(vUv, pixcoord, offset);\n"
    "    outColor = SMAABlendingWeightCalculationPS(vUv, pixcoord, offset, edgesTex, areaTex,\n"
    "                                               searchTex, vec4(0.0)); }\n",
    "void main() { vec4 offset; SMAANeighborhoodBlendingVS(vUv, offset);\n"
    "    outColor = vec4(SMAANeighborhoodBlendingPS(vUv, offset, colorTex, blendTex).rgb, 1.0); }\n",
};

enum { PP_FXAA, PP_CAS, PP_SM_EDGES, PP_SM_WEIGHTS, PP_SM_BLEND, PP_PQ, PP_PAT, PP_PVIDEO,
       PP_GAMMA, PP_COUNT };
/* Targets: 0 FXAA's and SMAA's result, 1 CAS's, 2 edges, 3 weights, 4 the
 * frame with the video overlay, 5 the frame through the gamma ramp. */
enum { TG_AA, TG_CAS, TG_EDGES, TG_WEIGHTS, TG_OVL, TG_GAMMA, TG_COUNT };
static const VkFormat s_tg_fmt[TG_COUNT] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                             VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                             VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
static const char *const s_pp_name[PP_COUNT] = { "fxaa", "cas", "smaa edges", "smaa weights",
                                                 "smaa blend", "hdr pq", "hdr pattern", "pvideo",
                                                 "gamma" };

/* The display's gamma ramp (xbox_gamma.h) over the frame, as the DAC applied
 * it at scanout: tex1 is the ramp, 256x1, each channel through its own. */
static const char s_post_gamma[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D tex0;\n"
    "layout(set = 0, binding = 1) uniform sampler2D tex1;\n"
    "layout(location = 0) in vec2 vUv;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    vec3 c = texture(tex0, vUv).rgb;\n"
    "    vec3 i = (floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) + 0.5) / 256.0;\n"
    "    outColor = vec4(texture(tex1, vec2(i.r, 0.5)).r, texture(tex1, vec2(i.g, 0.5)).g,\n"
    "                    texture(tex1, vec2(i.b, 0.5)).b, 1.0);\n"
    "}\n";

/* The video overlay (xbox_pvideo.h) over the frame, as the display put it
 * at scanout: tex1, the overlay's source converted to RGB, in rect (x0, y0,
 * x1, y1 in the frame's uv); with the colour key on (key.a) only where the
 * frame shows the key. */
static const char s_post_pvideo[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D tex0;\n"
    "layout(set = 0, binding = 1) uniform sampler2D tex1;\n"
    "layout(push_constant) uniform PC { vec4 rect; vec4 key; } pc;\n"
    "layout(location = 0) in vec2 vUv;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    vec4 c = texture(tex0, vUv);\n"
    "    vec2 q = (vUv - pc.rect.xy) / (pc.rect.zw - pc.rect.xy);\n"
    "    if (all(greaterThanEqual(q, vec2(0.0))) && all(lessThan(q, vec2(1.0)))\n"
    "            && (pc.key.a < 0.5 || all(lessThan(abs(c.rgb - pc.key.rgb), vec3(0.6 / 255.0)))))\n"
    "        c = vec4(texture(tex1, q).rgb, 1.0);\n"
    "    outColor = c;\n"
    "}\n";

/* BO-110: the settings overlay over an HDR10 frame: s_hcomp holds it
 * premultiplied over transparent black; its colour is decoded (gamma 2.2),
 * x the HUD's white in units of 10,000 nits, BT.709 to BT.2020, PQ, and
 * blended over the swapchain image by its alpha. */
static const char s_post_pq[] = HDR_OVL_GLSL;     /* video/hdr_shaders.h (BO-244) */
/* BO-110: the overlay's calibration patterns over an HDR10 frame, past the
 * overlay's white (nv2a_host_hdr_pattern): p.xy the centre in pixels; p.z
 * the disc's radius (kind 0) or -1 (kind 1: the left half one level, the
 * right half the other); p.w both levels as 10-bit PQ codes, a * 1024 + b.
 * Drawn inside the pattern's rectangle (the scissor). */
static const char s_post_pat[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { vec4 p; } pc;\n"
    "layout(location = 0) in vec2 vUv;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    float a = floor(pc.p.w / 1024.0), b = pc.p.w - a * 1024.0;\n"
    "    vec2 d = gl_FragCoord.xy - pc.p.xy;\n"
    "    bool in1 = pc.p.z >= 0.0 ? length(d) < pc.p.z : d.x < 0.0;\n"
    "    outColor = vec4(vec3((in1 ? a : b) / 1023.0), 1.0);\n"
    "}\n";
static VkRect2D *s_pp_scissor;             /* post_pass: draw only here (the patterns) */
static uint32_t s_pp_pc_bytes = 16;        /* post_pass: push constant bytes (the overlay's 32) */
static int s_pp_blend;                     /* post_pipeline: alpha blending (the overlay's encode) */
static int s_pp_load;                      /* post_pass: keep the target's contents */
static int s_pp_half;                      /* this frame's passes run on the 10-bit HDR slot (BO-110) */
#define PP_TS 16                           /* timestamp slots a frame */

static struct {
    int tried, ok, smaa_tried, smaa_ok, warmed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkSampler smp;
    VkDescriptorPool pool;
    VkDescriptorSet ds[2][PP_COUNT];       /* [frame in flight][pass] */
    VkPipeline pso[PP_COUNT];
    VkPipeline hpso[PP_COUNT];             /* BO-110: half-float targets (FXAA, CAS, SMAA blend) */
    VkFormat   pq_fmt;                     /* the format P.pso[PP_PQ] writes */
    VkFormat   pat_fmt;                    /* and P.pso[PP_PAT] */
    struct nvk_image img[TG_COUNT];
    struct nvk_image lut[2];               /* SMAA's area and search tables */
    struct nvk_buffer stage[2];            /* their upload, until freed with them */
    int lut_done;
    struct nvk_image ovl;                  /* the video overlay's frame, RGB */
    struct nvk_image gam;                  /* the gamma ramp, 256x1 */
    struct nvk_buffer gam_stage[2];        /* its upload, per frame in flight */
    uint32_t gam_gen;                      /* the ramp's generation in gam */
    struct nvk_buffer ovl_stage[2];        /* its upload, per frame in flight */
    uint32_t ovl_seq;                      /* the overlay frame in ovl */
    /* post_gpu */
    VkQueryPool qp;
    int ts_n[2], ts_pass[2][PP_COUNT];     /* stamps written / which passes, per frame */
    double ts_sum[PP_COUNT];
    unsigned ts_cnt[PP_COUNT], ts_presents;
} P;

static VkShaderModule post_module(enum nvk_stage st, const char *src, const char *what)
{
    char err[2048];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    if (!nvk_glsl_spirv(st, src, &w, &n, &compiled, err, sizeof err)) {
        XR_ERROR( LOGP "post pass %s: GLSL compile failed: %s\n", what, err);
        return VK_NULL_HANDLE;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(C->dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static VkPipeline post_pipeline(VkShaderModule vs, VkShaderModule fs, VkFormat cf)
{
    VkPipelineShaderStageCreateInfo ss[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo pr;
    VkGraphicsPipelineCreateInfo gp;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipeline p = VK_NULL_HANDLE;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = vs;
    ss[0].pName = ss[1].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = fs;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (s_pp_blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    memset(&pr, 0, sizeof pr);
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &cf;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &pr;
    gp.stageCount = 2;
    gp.pStages = ss;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = P.pl;
    if (vkCreateGraphicsPipelines(C->dev, VK_NULL_HANDLE, 1, &gp, NULL, &p) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

/* The three SMAA fragment sources, made when it is first asked for. */
static void smaa_pipelines(VkShaderModule vs)
{
    static const VkFormat fmt[3] = { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_FORMAT_B8G8R8A8_UNORM };
    int k;

    P.smaa_ok = 1;
    for (k = 0; k < 3; k++) {
        const char *lib = smaa_glsl_library();
        size_t n = strlen(s_smaa_head) + strlen(s_smaa_bind[k]) + strlen(lib)
                 + strlen(s_smaa_main[k]) + 1;
        char *src = (char *)malloc(n);
        VkShaderModule fs;
        if (!src) {
            P.smaa_ok = 0;
            return;
        }
        snprintf(src, n, "%s%s%s%s", s_smaa_head, s_smaa_bind[k], lib, s_smaa_main[k]);
        fs = post_module(NVK_STAGE_FRAGMENT, src, s_pp_name[PP_SM_EDGES + k]);
        free(src);
        if (fs) {
            P.pso[PP_SM_EDGES + k] = post_pipeline(vs, fs, fmt[k]);
            if (k == 2)
                P.hpso[PP_SM_BLEND] = post_pipeline(vs, fs, HDR_SLOT_FMT);
            vkDestroyShaderModule(C->dev, fs, NULL);
        }
        if (!P.pso[PP_SM_EDGES + k])
            P.smaa_ok = 0;
    }
    fprintf(stderr, LOGP "SMAA %s (preset high)\n", P.smaa_ok ? "ready" : "unavailable");
}

/* SMAA's pipelines, when it is first asked for (the larger compile, once;
 * the SPIR-V cache serves it after). */
static int smaa_prepare(void)
{
    VkShaderModule vs;

    if (P.smaa_tried)
        return P.smaa_ok;
    P.smaa_tried = 1;
    vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
    if (vs) {
        smaa_pipelines(vs);
        vkDestroyShaderModule(C->dev, vs, NULL);
    }
    return P.smaa_ok;
}

/* Made at the first use; 1 when the passes can run. */
static int post_init(void)
{
    VkDescriptorSetLayoutBinding lb[3];
    VkDescriptorSetLayoutCreateInfo li;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetAllocateInfo dai;
    VkDescriptorSetLayout lay[2 * PP_COUNT];
    VkShaderModule vs, fs[2];
    int i;

    if (P.tried)
        return P.ok;
    P.tried = 1;
    memset(lb, 0, sizeof lb);
    for (i = 0; i < 3; i++) {
        lb[i].binding = (uint32_t)i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = lb;
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = 32;                      /* 16 for most passes; the overlay's 32 */
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    memset(&psz, 0, sizeof psz);
    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = 3 * 2 * PP_COUNT;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 2 * PP_COUNT;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &psz;
    if (vkCreateDescriptorSetLayout(C->dev, &li, NULL, &P.dsl) != VK_SUCCESS)
        goto fail;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(C->dev, &pli, NULL, &P.pl) != VK_SUCCESS
            || vkCreateSampler(C->dev, &sci, NULL, &P.smp) != VK_SUCCESS
            || vkCreateDescriptorPool(C->dev, &pci, NULL, &P.pool) != VK_SUCCESS)
        goto fail;
    for (i = 0; i < 2 * PP_COUNT; i++)
        lay[i] = P.dsl;
    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = P.pool;
    dai.descriptorSetCount = 2 * PP_COUNT;
    dai.pSetLayouts = lay;
    {
        VkDescriptorSet sets[2 * PP_COUNT];
        if (vkAllocateDescriptorSets(C->dev, &dai, sets) != VK_SUCCESS)
            goto fail;
        for (i = 0; i < 2 * PP_COUNT; i++)
            P.ds[i / PP_COUNT][i % PP_COUNT] = sets[i];
    }
    vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
    fs[PP_FXAA] = post_module(NVK_STAGE_FRAGMENT, s_post_fxaa, "FXAA");
    fs[PP_CAS] = post_module(NVK_STAGE_FRAGMENT, s_post_cas, "CAS");
    if (vs) {
        VkShaderModule ov = post_module(NVK_STAGE_FRAGMENT, s_post_pvideo, "video overlay");
        for (i = 0; i < 2; i++)
            if (fs[i]) {
                P.pso[i] = post_pipeline(vs, fs[i], VK_FORMAT_B8G8R8A8_UNORM);
                P.hpso[i] = post_pipeline(vs, fs[i], HDR_SLOT_FMT);
            }
        if (ov) {
            P.pso[PP_PVIDEO] = post_pipeline(vs, ov, VK_FORMAT_B8G8R8A8_UNORM);
            P.hpso[PP_PVIDEO] = post_pipeline(vs, ov, HDR_SLOT_FMT);
            vkDestroyShaderModule(C->dev, ov, NULL);
        }
        {
            VkShaderModule gm = post_module(NVK_STAGE_FRAGMENT, s_post_gamma, "gamma ramp");
            if (gm) {
                P.pso[PP_GAMMA] = post_pipeline(vs, gm, VK_FORMAT_B8G8R8A8_UNORM);
                vkDestroyShaderModule(C->dev, gm, NULL);
            }
        }
    }
    /* The modules are no longer needed once the pipelines exist. */
    for (i = 0; i < 2; i++)
        if (fs[i])
            vkDestroyShaderModule(C->dev, fs[i], NULL);
    if (vs)
        vkDestroyShaderModule(C->dev, vs, NULL);
    P.ok = P.pso[PP_FXAA] || P.pso[PP_CAS];
    fprintf(stderr, LOGP "post passes: FXAA %s, CAS %s\n", P.pso[PP_FXAA] ? "ready" : "unavailable",
            P.pso[PP_CAS] ? "ready" : "unavailable");
    if (recomp_env(RENV_POST_GPU) && C->props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * PP_TS;
        if (vkCreateQueryPool(C->dev, &qi, NULL, &P.qp) != VK_SUCCESS)
            P.qp = VK_NULL_HANDLE;
    }
    return P.ok;
fail:
    XR_ERROR( LOGP "post passes unavailable (Vulkan objects failed)\n");
    return 0;
}

static void post_free_targets(int luts_too)
{
    int i;
    queue_idle();
    for (i = 0; i < TG_COUNT; i++)
        nvk_image_free(&P.img[i]);
    if (luts_too) {
        for (i = 0; i < 2; i++) {
            nvk_image_free(&P.lut[i]);
            nvk_buffer_free(&P.stage[i]);
            nvk_buffer_free(&P.ovl_stage[i]);
            nvk_buffer_free(&P.gam_stage[i]);
        }
        nvk_image_free(&P.ovl);
        P.ovl_seq = 0;
        nvk_image_free(&P.gam);
        P.gam_gen = 0;
        P.lut_done = 0;
    }
}

static void post_destroy(void)
{
    int i;
    if (!P.tried)
        return;
    post_free_targets(1);
    for (i = 0; i < PP_COUNT; i++) {
        if (P.pso[i])
            vkDestroyPipeline(C->dev, P.pso[i], NULL);
        if (P.hpso[i])
            vkDestroyPipeline(C->dev, P.hpso[i], NULL);
    }
    if (P.qp)
        vkDestroyQueryPool(C->dev, P.qp, NULL);
    if (P.pool)
        vkDestroyDescriptorPool(C->dev, P.pool, NULL);
    if (P.smp)
        vkDestroySampler(C->dev, P.smp, NULL);
    if (P.pl)
        vkDestroyPipelineLayout(C->dev, P.pl, NULL);
    if (P.dsl)
        vkDestroyDescriptorSetLayout(C->dev, P.dsl, NULL);
    memset(&P, 0, sizeof P);
}

/* The target t at w x h, made if it is not (0 when it cannot be). */
static struct nvk_image *post_target(int t, uint32_t w, uint32_t h)
{
    struct nvk_image *im = &P.img[t];
    VkFormat f = s_pp_half && (t == TG_AA || t == TG_CAS || t == TG_OVL) ? HDR_SLOT_FMT : s_tg_fmt[t];
    if (im->img && (im->w != w || im->h != h || im->format != f)) {
        queue_idle();                   /* a present may still read it: rare */
        nvk_image_free(im);
    }
    if (!im->img && !nvk_image_new(im, f, w, h, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    return im;
}

/* SMAA's lookup tables, uploaded in cb the first time. */
static int post_luts(VkCommandBuffer cb)
{
    static const struct { VkFormat f; uint32_t w, h, bpp; } d[2] = {
        { VK_FORMAT_R8G8_UNORM, SMAA_AREA_W, SMAA_AREA_H, 2 },
        { VK_FORMAT_R8_UNORM, SMAA_SEARCH_W, SMAA_SEARCH_H, 1 },
    };
    const unsigned char *src[2];
    int i;

    if (P.lut_done)
        return 1;
    src[0] = smaa_area_bytes;
    src[1] = smaa_search_bytes;
    for (i = 0; i < 2; i++) {
        VkBufferImageCopy bc;
        VkDeviceSize n = (VkDeviceSize)d[i].w * d[i].h * d[i].bpp;
        if (!nvk_image_new(&P.lut[i], d[i].f, d[i].w, d[i].h, 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)
                || !nvk_buffer_new(&P.stage[i], n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)) {
            post_free_targets(1);
            return 0;
        }
        memcpy(P.stage[i].map, src[i], (size_t)n);
        nvk_image_to(cb, &P.lut[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = d[i].w;
        bc.imageExtent.height = d[i].h;
        bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, P.stage[i].buf, P.lut[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &bc);
        nvk_image_to(cb, &P.lut[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    P.lut_done = 1;
    return 1;
}

/* One pass in cb: `in` (up to three images, as laid out now) through pass p
 * into target t (cleared first when `clear`). pc is the 16 bytes of push
 * constants. */
static void post_pass(VkCommandBuffer cb, int p, struct nvk_image *dst, int clear,
                      struct nvk_image *const *in, const VkImageLayout *in_layout, int nin,
                      const float *pc)
{
    VkDescriptorImageInfo di[3];
    VkWriteDescriptorSet wr[3];
    VkRenderingAttachmentInfo att;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    int i, slot = -1;

    for (i = 0; i < nin; i++) {
        memset(&di[i], 0, sizeof di[i]);
        di[i].sampler = P.smp;
        di[i].imageView = in[i]->view;
        di[i].imageLayout = in_layout[i];
        memset(&wr[i], 0, sizeof wr[i]);
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = P.ds[s_fi][p];
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &di[i];
    }
    vkUpdateDescriptorSets(C->dev, (uint32_t)nin, wr, 0, NULL);

    nvk_image_to(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (P.qp && P.ts_n[s_fi] + 2 <= PP_TS) {
        slot = P.ts_n[s_fi];
        P.ts_pass[s_fi][slot / 2] = p;
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, P.qp, (uint32_t)(s_fi * PP_TS + slot));
    }
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = dst->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
               : s_pp_load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = dst->w;
    ri.renderArea.extent.height = dst->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)dst->w;
    vp.height = (float)dst->h;
    vp.maxDepth = 1.0f;
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = dst->w;
    sc.extent.height = dst->h;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, s_pp_scissor ? s_pp_scissor : &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      s_pp_half && P.hpso[p] ? P.hpso[p] : P.pso[p]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &P.ds[s_fi][p], 0, NULL);
    vkCmdPushConstants(cb, P.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, s_pp_pc_bytes, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    /* Readable by the next pass or the blit. */
    nvk_image_to(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (slot >= 0) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, P.qp,
                            (uint32_t)(s_fi * PP_TS + slot + 1));
        P.ts_n[s_fi] += 2;
    }
}

/* The last frame in this slot's passes' timestamps, read now that its fence
 * has signalled; a line every 300 presents. */
static void post_gpu_read(void)
{
    uint64_t t[PP_TS];
    int n = P.ts_n[s_fi], i;
    double period = (double)C->props.limits.timestampPeriod * 1e-6;     /* ms a tick */

    if (!P.qp || !n)
        return;
    P.ts_n[s_fi] = 0;
    if (vkGetQueryPoolResults(C->dev, P.qp, (uint32_t)(s_fi * PP_TS), (uint32_t)n,
                              sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (i = 0; i + 1 < n; i += 2) {
        int p = P.ts_pass[s_fi][i / 2];
        P.ts_sum[p] += (double)(t[i + 1] - t[i]) * period;
        P.ts_cnt[p]++;
    }
    if (++P.ts_presents % 300 == 0) {
        fprintf(stderr, LOGP "post GPU ms (avg of the last 300):");
        for (i = 0; i < PP_COUNT; i++) {
            if (P.ts_cnt[i])
                fprintf(stderr, " %s %.3f", s_pp_name[i], P.ts_sum[i] / (double)P.ts_cnt[i]);
            P.ts_sum[i] = 0;
            P.ts_cnt[i] = 0;
        }
        fprintf(stderr, "\n");
    }
}

/* The passes asked for (RECOMP_GFX_AA, RECOMP_GFX_SHARPEN), recorded into cb
 * over slot k. Returns the image and layout the scale to the window reads:
 * the last target, or the slot itself when no pass ran. */
/* The frame's size in the slot (fbv_present's fw x fh), for the overlay's
 * rectangle: the slot can be larger than the frame it holds. */
static uint32_t s_pp_frame_w, s_pp_frame_h;

/* The overlay's frame f into P.ovl in cb, through this frame's staging
 * buffer, when it is a new one: its source rectangle, YUY2 or UYVY, BT.601
 * video range to RGB (as the console's overlay converts it). 1 when P.ovl
 * holds it. */
static int pvideo_upload(VkCommandBuffer cb, const struct xbox_pvideo_frame *f)
{
    uint32_t sx = (f->s16 >> 4) & ~1u, sy = f->t16 >> 4, sw, sh, x, y;
    const uint8_t *src;
    uint8_t *dst;
    struct nvk_buffer *st = &P.ovl_stage[s_fi];
    VkDeviceSize need;
    VkBufferImageCopy bc;

    if (sx >= f->in_w || sy >= f->in_h || f->pitch < f->in_w * 2u)
        return 0;
    sw = (uint32_t)(((uint64_t)f->out_w * f->dsdx + (1u << 19)) >> 20);
    sh = (uint32_t)(((uint64_t)f->out_h * f->dtdy + (1u << 19)) >> 20);
    if (!sw || sx + sw > f->in_w)
        sw = f->in_w - sx;
    if (!sh || sy + sh > f->in_h)
        sh = f->in_h - sy;
    sw &= ~1u;
    if (!sw || !sh)
        return 0;
    if (P.ovl_seq == f->seq && P.ovl.img && P.ovl.w == sw && P.ovl.h == sh)
        return 1;
    src = f->host;
    if (!src)
        return 0;
    need = (VkDeviceSize)sw * sh * 4u;
    if (st->size < need) {
        nvk_buffer_free(st);
        if (!nvk_buffer_new(st, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
            return 0;
    }
    if (P.ovl.img && (P.ovl.w != sw || P.ovl.h != sh)) {
        queue_idle();
        nvk_image_free(&P.ovl);
    }
    if (!P.ovl.img && !nvk_image_new(&P.ovl, VK_FORMAT_B8G8R8A8_UNORM, sw, sh, 1,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    dst = (uint8_t *)st->map;
    for (y = 0; y < sh; y++) {
        const uint8_t *r = src + (size_t)(sy + y) * f->pitch + (size_t)sx * 2u;
        uint8_t *o = dst + (size_t)y * sw * 4u;
        for (x = 0; x < sw; x += 2, r += 4, o += 8) {
            int y0 = f->yuyv ? r[0] : r[1], u = f->yuyv ? r[1] : r[0];
            int y1 = f->yuyv ? r[2] : r[3], v = f->yuyv ? r[3] : r[2];
            int d = u - 128, e = v - 128, k, c;
            for (k = 0; k < 2; k++) {
                int R, G, B;
                c = (k ? y1 : y0) - 16;
                R = (298 * c + 409 * e + 128) >> 8;
                G = (298 * c - 100 * d - 208 * e + 128) >> 8;
                B = (298 * c + 516 * d + 128) >> 8;
                o[4 * k + 0] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
                o[4 * k + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
                o[4 * k + 2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
                o[4 * k + 3] = 255;
            }
        }
    }
    nvk_image_to(cb, &P.ovl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = sw;
    bc.imageExtent.height = sh;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, st->buf, P.ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    nvk_image_to(cb, &P.ovl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    P.ovl_seq = f->seq;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "video overlay: %ux%u %s at %u,%u -> %ux%u at %u,%u%s\n", sw, sh,
                    f->yuyv ? "YUY2" : "UYVY", sx, sy, f->out_w, f->out_h, f->out_x, f->out_y,
                    f->key_on ? ", colour keyed" : "");
    }
    return 1;
}

/* The ramp into P.gam in cb, through this frame's staging buffer, when it
 * changed. 1 when P.gam holds it. */
static int gamma_upload(VkCommandBuffer cb, const uint8_t *ramp, uint32_t gen)
{
    struct nvk_buffer *st = &P.gam_stage[s_fi];
    VkBufferImageCopy bc;
    uint8_t *dst;
    int i;

    if (P.gam.img && P.gam_gen == gen)
        return 1;
    if (st->size < 1024) {
        nvk_buffer_free(st);
        if (!nvk_buffer_new(st, 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
            return 0;
    }
    if (!P.gam.img && !nvk_image_new(&P.gam, VK_FORMAT_B8G8R8A8_UNORM, 256, 1, 1,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    dst = (uint8_t *)st->map;
    for (i = 0; i < 256; i++) {
        dst[4 * i + 0] = ramp[512 + i];     /* B */
        dst[4 * i + 1] = ramp[256 + i];     /* G */
        dst[4 * i + 2] = ramp[i];           /* R */
        dst[4 * i + 3] = 255;
    }
    nvk_image_to(cb, &P.gam, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = 256;
    bc.imageExtent.height = 1;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, st->buf, P.gam.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    nvk_image_to(cb, &P.gam, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    P.gam_gen = gen;
    return 1;
}

static void post_run(VkCommandBuffer cb, int k, VkImage *src, VkImageLayout *layout)
{
    int aa = nv2a_host_post_aa(), cas = nv2a_host_sharpen() > 0.001f;
    float sharp = nv2a_host_sharpen();
    struct nvk_image *slot = &s_slot[k], *cur = slot, *in[3], *t;
    VkImageLayout cur_layout = VK_IMAGE_LAYOUT_GENERAL, inl[3];
    float pc[4];
    int fx = aa == NV2A_AA_FXAA, sm = aa == NV2A_AA_SMAA, ran = 0;

    struct xbox_pvideo_frame pvf;
    int ov, gm;
    uint8_t ramp[768];
    uint32_t ramp_gen = 0;

    *src = slot->img;
    *layout = VK_IMAGE_LAYOUT_GENERAL;
    s_pp_half = slot->format == HDR_SLOT_FMT;
    /* The video overlay: SDR frames only (an HDR slot is encoded for the
     * display, the overlay's colours are not). */
    ov = !s_pp_half && xbox_PvideoFrame(&pvf);
    /* The title's gamma ramp: SDR frames only, as the overlay. */
    gm = !s_pp_half && xbox_GammaRamp(ramp, &ramp_gen);
    if (!P.warmed) {
        /* BO-208: every pass the player can switch on (FXAA, CAS, SMAA, in
         * both formats) is built at the first present, not at the moment of
         * the switch. */
        P.warmed = 1;
        if (post_init())
            (void)smaa_prepare();
    }
    if (!(fx || sm || cas || ov || gm) && !P.tried)
        return;
    if (!post_init())
        return;
    ov = ov && P.pso[PP_PVIDEO];
    gm = gm && P.pso[PP_GAMMA];
    fx = fx && (s_pp_half ? P.hpso[PP_FXAA] : P.pso[PP_FXAA]);
    sm = sm && smaa_prepare() && (!s_pp_half || P.hpso[PP_SM_BLEND]);
    cas = cas && (s_pp_half ? P.hpso[PP_CAS] : P.pso[PP_CAS]);
    {
        static int last_aa = -1;
        static float last_sharp = -1.0f;
        if (aa != last_aa || sharp != last_sharp) {
            last_aa = aa;
            last_sharp = sharp;
            fprintf(stderr, LOGP "post passes: AA %s, sharpen %.2f (%ux%u)\n",
                    aa == NV2A_AA_FXAA ? "FXAA" : aa == NV2A_AA_SMAA ? (sm ? "SMAA" : "SMAA (unavailable)")
                    : "off", sharp, slot->w, slot->h);
        }
    }
    post_gpu_read();
    /* Targets only while a setting needs them. */
    if (!fx && !sm && !cas && !ov && !gm && !P.img[TG_AA].img && !P.img[TG_CAS].img
            && !P.img[TG_EDGES].img && !P.img[TG_OVL].img && !P.img[TG_GAMMA].img
            && !P.lut[0].img)
        return;
    if (!sm && (P.img[TG_EDGES].img || P.img[TG_WEIGHTS].img || P.lut[0].img)) {
        /* SMAA went off: its targets and tables go (the queue is idle first). */
        queue_idle();
        nvk_image_free(&P.img[TG_EDGES]);
        nvk_image_free(&P.img[TG_WEIGHTS]);
        for (int i = 0; i < 2; i++) {
            nvk_image_free(&P.lut[i]);
            nvk_buffer_free(&P.stage[i]);
        }
        P.lut_done = 0;
    }
    if (!fx && !sm && P.img[TG_AA].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_AA]);
    }
    if (!cas && P.img[TG_CAS].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_CAS]);
    }
    if (!ov && P.img[TG_OVL].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_OVL]);
    }
    if (!gm && P.img[TG_GAMMA].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_GAMMA]);
    }
    if (!fx && !sm && !cas && !ov && !gm)
        return;
    /* After the backend's copy into the slot (earlier on the queue). */
    nvk_image_barrier(cb, slot->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    if (P.qp) {
        vkCmdResetQueryPool(cb, P.qp, (uint32_t)(s_fi * PP_TS), PP_TS);
        P.ts_n[s_fi] = 0;
    }
    memset(pc, 0, sizeof pc);
    pc[0] = 1.0f / (float)slot->w;
    pc[1] = 1.0f / (float)slot->h;
    pc[2] = sharp;
    if (gm && gamma_upload(cb, ramp, ramp_gen)
            && (t = post_target(TG_GAMMA, slot->w, slot->h)) != NULL) {
        in[0] = cur;
        inl[0] = cur_layout;
        in[1] = &P.gam;
        inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        post_pass(cb, PP_GAMMA, t, 0, in, inl, 2, pc);
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    }
    if (ov && pvideo_upload(cb, &pvf) && (t = post_target(TG_OVL, slot->w, slot->h)) != NULL) {
        /* The overlay's rectangle, display pixels, in the slot's uv: the
         * display is the presented surface's size (nv2a_pb_present_state),
         * held in s_pp_frame_w x s_pp_frame_h of the slot. */
        const struct nv2a_pb_present *pp = nv2a_pb_present_state();
        float dw = (float)(pp->w ? pp->w : slot->w), dh = (float)(pp->h ? pp->h : slot->h);
        float fx_ = (float)(s_pp_frame_w ? s_pp_frame_w : slot->w) / (float)slot->w / dw;
        float fy_ = (float)(s_pp_frame_h ? s_pp_frame_h : slot->h) / (float)slot->h / dh;
        float o[8];
        o[0] = (float)pvf.out_x * fx_;
        o[1] = (float)pvf.out_y * fy_;
        o[2] = (float)(pvf.out_x + pvf.out_w) * fx_;
        o[3] = (float)(pvf.out_y + pvf.out_h) * fy_;
        o[4] = (float)((pvf.key >> 16) & 0xFFu) / 255.0f;
        o[5] = (float)((pvf.key >> 8) & 0xFFu) / 255.0f;
        o[6] = (float)(pvf.key & 0xFFu) / 255.0f;
        o[7] = pvf.key_on ? 1.0f : 0.0f;
        in[0] = cur;
        inl[0] = cur_layout;
        in[1] = &P.ovl;
        inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        s_pp_pc_bytes = 32;
        post_pass(cb, PP_PVIDEO, t, 0, in, inl, 2, o);
        s_pp_pc_bytes = 16;
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    }
    if (fx && (t = post_target(TG_AA, slot->w, slot->h)) != NULL) {
        in[0] = cur;
        inl[0] = cur_layout;
        post_pass(cb, PP_FXAA, t, 0, in, inl, 1, pc);
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    } else if (sm && post_luts(cb)) {
        struct nvk_image *ed = post_target(TG_EDGES, slot->w, slot->h),
                         *wt = post_target(TG_WEIGHTS, slot->w, slot->h),
                         *out = post_target(TG_AA, slot->w, slot->h);
        if (ed && wt && out) {
            float m[4] = { pc[0], pc[1], (float)slot->w, (float)slot->h };
            in[0] = cur;
            inl[0] = cur_layout;
            post_pass(cb, PP_SM_EDGES, ed, 1, in, inl, 1, m);
            in[0] = ed;
            inl[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            in[1] = &P.lut[0];
            inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            in[2] = &P.lut[1];
            inl[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            post_pass(cb, PP_SM_WEIGHTS, wt, 1, in, inl, 3, m);
            in[0] = cur;
            inl[0] = cur_layout;
            in[1] = wt;
            inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            post_pass(cb, PP_SM_BLEND, out, 0, in, inl, 2, m);
            cur = out;
            cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ran = 1;
        }
    }
    if (cas && (t = post_target(TG_CAS, slot->w, slot->h)) != NULL) {
        in[0] = cur;
        inl[0] = cur_layout;
        post_pass(cb, PP_CAS, t, 0, in, inl, 1, pc);
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    }
    if (ran) {
        nvk_image_to(cb, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        *src = cur->img;
        *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
}

/* BO-110: the overlay's encode pipeline for the swapchain's format and
 * s_hcomp at the swapchain's size; 1 when the overlay can be drawn over an
 * HDR10 frame. */
static int hdr_overlay_ready(void)
{
    if (!post_init())
        return 0;
    if (!P.pso[PP_PQ] || P.pq_fmt != s_sc_fmt) {
        VkShaderModule vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
        VkShaderModule fs = post_module(NVK_STAGE_FRAGMENT, s_post_pq, "HDR10 overlay");
        if (P.pso[PP_PQ]) {
            queue_idle();
            vkDestroyPipeline(C->dev, P.pso[PP_PQ], NULL);
            P.pso[PP_PQ] = VK_NULL_HANDLE;
        }
        if (vs && fs) {
            s_pp_blend = 1;
            P.pso[PP_PQ] = post_pipeline(vs, fs, s_sc_fmt);
            s_pp_blend = 0;
        }
        if (vs)
            vkDestroyShaderModule(C->dev, vs, NULL);
        if (fs)
            vkDestroyShaderModule(C->dev, fs, NULL);
        P.pq_fmt = s_sc_fmt;
        if (!P.pso[PP_PQ]) {
            static int said;
            if (!said++)
                XR_ERROR(LOGP "HDR10 overlay pass unavailable\n");
            return 0;
        }
    }
    if (s_hcomp.img && (s_hcomp.w != s_sc_ext.width || s_hcomp.h != s_sc_ext.height)) {
        queue_idle();
        nvk_image_free(&s_hcomp);
    }
    if (!s_hcomp.img && !nvk_image_new(&s_hcomp, VK_FORMAT_B8G8R8A8_UNORM, s_sc_ext.width,
                                       s_sc_ext.height, 1,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    return 1;
}

/* BO-110: the overlay's pattern (nv2a_host_hdr_pattern), if it asked for one
 * this frame, drawn onto the HDR10 swapchain image (in TRANSFER_DST, left
 * so). */
static void hdr_pattern(VkCommandBuffer cb, struct nvk_image *sw)
{
    int kind;
    float r[4], a, b, pc[4];
    VkRect2D sc;
    if (!nv2a_host_hdr_pattern_take(&kind, r, &a, &b) || !post_init())
        return;
    if (!P.pso[PP_PAT] || P.pat_fmt != s_sc_fmt) {
        VkShaderModule vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
        VkShaderModule fs = post_module(NVK_STAGE_FRAGMENT, s_post_pat, "HDR10 pattern");
        if (P.pso[PP_PAT]) {
            queue_idle();
            vkDestroyPipeline(C->dev, P.pso[PP_PAT], NULL);
            P.pso[PP_PAT] = VK_NULL_HANDLE;
        }
        if (vs && fs)
            P.pso[PP_PAT] = post_pipeline(vs, fs, s_sc_fmt);
        if (vs)
            vkDestroyShaderModule(C->dev, vs, NULL);
        if (fs)
            vkDestroyShaderModule(C->dev, fs, NULL);
        P.pat_fmt = s_sc_fmt;
        if (!P.pso[PP_PAT])
            return;
    }
    if (r[0] < 0) r[0] = 0;
    if (r[1] < 0) r[1] = 0;
    if (r[2] > (float)sw->w) r[2] = (float)sw->w;
    if (r[3] > (float)sw->h) r[3] = (float)sw->h;
    if (r[2] <= r[0] + 1.0f || r[3] <= r[1] + 1.0f)
        return;
    sc.offset.x = (int32_t)r[0];
    sc.offset.y = (int32_t)r[1];
    sc.extent.width = (uint32_t)(r[2] - r[0]);
    sc.extent.height = (uint32_t)(r[3] - r[1]);
    pc[0] = (r[0] + r[2]) * 0.5f;
    pc[1] = (r[1] + r[3]) * 0.5f;
    pc[2] = kind == 0 ? (r[3] - r[1]) * 0.22f : -1.0f;
    {
        float c[2], n[2] = { a, b };
        int i;
        for (i = 0; i < 2; i++) {
            float m = powf(fminf(fmaxf(n[i], 0.0f), 10000.0f) / 10000.0f, 0.1593017578125f);
            c[i] = floorf(powf((0.8359375f + 18.8515625f * m) / (1.0f + 18.6875f * m), 78.84375f)
                          * 1023.0f + 0.5f);
        }
        pc[3] = c[0] * 1024.0f + c[1];
    }
    s_pp_half = 0;
    s_pp_load = 1;
    s_pp_scissor = &sc;
    post_pass(cb, PP_PAT, sw, 0, NULL, NULL, 0, pc);
    s_pp_scissor = NULL;
    s_pp_load = 0;
    nvk_image_barrier(cb, sw->img, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

int fbv_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                const char *shot_path, uint32_t flip)
{
    VkCommandBuffer cb;
    VkCommandBufferBeginInfo bi;
    VkClearColorValue black;
    VkImageSubresourceRange rr;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint32_t idx = 0;
    int dw = 0, dh = 0, shot;
    VkResult r, pr;
    int hdr;
    VkImage dimg;            /* where the frame is scaled to: the swapchain image, or s_hcomp */
    int new_frame;           /* a new game frame, not a redraw (frame pacing) */
    uint64_t t_call0;        /* the call's start, for the counters line */
    int fmt_odd;             /* the slot is not in the swapchain's format (an HDR switch) */

    if (!C || gpu_gone())
        return 0;
    if (!s_keep_repeat)
        keep_poll();
    new_frame = !s_keep_repeat && flip != s_keep_last_flip;
#if defined(XBOXRECOMP_HAVE_OVERLAY)
    if (xbox_OverlayOpen())
        new_frame = 0;           /* presents on every turn: not paced */
#endif
    /* No skip: a queue found too deep is left so (BO-210). */
    t_call0 = mono_ns();
    drawable_size(win, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;
    {
        static int listening;
        if (!listening) {
            listening = 1;
            xbox_HostWakeListener("the Vulkan swapchain", fbv_woke);
            xbox_PacingResetHook(fbv_pacing_reset);
        }
    }
    if (s_wake_recreate) {
        s_wake_recreate = 0;
        s_recreate = 1;
    }
    if (s_surface_lost && !resurface(win))
        return 0;
#if defined(_WIN32)
    if (s_hdr_recheck) {         /* Windows HDR changed (the DXGI thread, BO-207) */
        s_hdr_recheck = 0;
        s_recreate = 1;
    }
#endif
    if (s_sc && s_hdr_offered && hdr_wanted() != s_hdr_sc)
        s_recreate = 1;          /* BO-110: HDR switched in the settings */
    if (!s_sc || s_recreate || FBV_SIZE_CHANGED(dw, dh)) {
        if (!make_swapchain(win))
            return 0;
    }
    {
        /* The frame's last present: its command buffer, its acquire
         * semaphore and the fence itself are reused only once it is done. */
        VkResult wr = VK_SUCCESS;
        int out = nvk_wait_fence(NVK_WAIT_PRESENT, "window present", s_fence[s_fi], &wr);
        if (out != NVK_WAIT_OK) {
            gpu_wait_failed(out, wr, "window present");
            return 0;
        }
    }
    if (s_lat_pend[s_fi]) {
        char row[480];
        uint64_t t = xbox_HostNowNs();
        int n = snprintf(row, sizeof row, "%llu,", (unsigned long long)t);
        s_lat_pend[s_fi] = 0;
        xbox_LatSignature((const uint8_t *)s_lat_buf[s_fi].map, s_lat_w[s_fi], s_lat_h[s_fi],
                          s_lat_w[s_fi] * 4, 4, row + n, (int)sizeof row - n);
        xbox_LatRow('G', s_lat_flip[s_fi], t, row);
    }
    {
        uint64_t t_acq0 = xbox_LatOn() && !s_keep_repeat ? xbox_HostNowNs() : 0;
        r = acquire_hook();
        if (r == VK_SUCCESS)
            r = vkAcquireNextImageKHR(C->dev, s_sc, 1000000000ull, s_acq[s_fi], VK_NULL_HANDLE, &idx);
        if (t_acq0 && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
            char b[32];
            snprintf(b, sizeof b, "%llu", (unsigned long long)xbox_HostNowNs());
            xbox_LatRow('W', flip, t_acq0, b);
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        s_recreate = 1;
        return 0;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
    if (r == VK_ERROR_DEVICE_LOST) {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "the Vulkan device was lost (VK_ERROR_DEVICE_LOST): nothing"
                    " more can be shown in this run; the game has to be started again\n");
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;     /* timeout: try again at the next wake */
    if (r == VK_SUBOPTIMAL_KHR && FBV_SUBOPTIMAL_RECREATES)
        s_recreate = 1;
    vkResetFences(C->dev, 1, &s_fence[s_fi]);
    if (s_tq_on < 0) {
        s_tq_on = 0;
        if (recomp_env(RENV_POST_GPU) && C->props.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qi;
            memset(&qi, 0, sizeof qi);
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 4;
            s_tq_on = vkCreateQueryPool(C->dev, &qi, NULL, &s_tq) == VK_SUCCESS;
        }
    }
    if (s_tq_on && s_tq_set[s_fi]) {
        uint64_t t2[2];
        s_tq_set[s_fi] = 0;
        if (vkGetQueryPoolResults(C->dev, s_tq, (uint32_t)(s_fi * 2), 2, sizeof t2, t2,
                                  sizeof t2[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            s_tq_sum += (double)(t2[1] - t2[0]) * (double)C->props.limits.timestampPeriod * 1e-6;
            if (++s_tq_n == 300) {
                fprintf(stderr, LOGP "present GPU ms (avg of the last 300): %.3f%s\n",
                        s_tq_sum / 300.0, s_hdr_sc ? " (HDR10)" : hdr_test_encode() ? " (HDR test encode)" : "");
                s_tq_sum = 0;
                s_tq_n = 0;
            }
        }
    }
    shot = shot_path && s_sc_shots;
    if (shot && s_shot_buf.size < (VkDeviceSize)s_sc_ext.width * s_sc_ext.height * 4u) {
        nvk_buffer_free(&s_shot_buf);
        if (!nvk_buffer_new(&s_shot_buf, (VkDeviceSize)s_sc_ext.width * s_sc_ext.height * 4u,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            shot = 0;
    }

    /* BO-110: the slot holds an HDR10 frame (the backend's map). */
    hdr = (s_hdr_sc || hdr_test_encode()) && s_sc_view[idx] && s_slot[k].format == HDR_SLOT_FMT;
    /* Just after an HDR switch the slot still holds the frame in the other
     * format until the next upload: an 8-bit sRGB frame in an HDR10
     * swapchain is a full-white flash (1.0 is 10000 nits in PQ), a PQ frame
     * in an sRGB one washed out. That one frame stays black (BO-207). */
#if defined(_WIN32)
    fmt_odd = s_slot[k].img && (s_hdr_sc || hdr_test_encode()) != (s_slot[k].format == HDR_SLOT_FMT);
    if (fmt_odd) {
        static int said;
        if (said++ < 40)
            fprintf(stderr, LOGP "HDR switch: flip %u shown black (the frame is still %s, the"
                    " swapchain %s)\n", flip, s_slot[k].format == HDR_SLOT_FMT ? "HDR10" : "SDR",
                    s_hdr_sc ? "HDR10" : "SDR");
    }
#else
    fmt_odd = 0;             /* Linux/Deck as merged (BO-110) */
#endif
    if (hdr)
        hdr_metadata();         /* sent only when a level changed (a settings change) */
    cb = s_cb[s_fi];
    vkResetCommandBuffer(cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (s_tq_on) {
        vkCmdResetQueryPool(cb, s_tq, (uint32_t)(s_fi * 2), 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tq, (uint32_t)(s_fi * 2));
    }
    {
        /* From the acquire: chained to the semaphore wait (transfer stage),
         * the old contents discarded. */
        VkImageMemoryBarrier2 b;
        VkDependencyInfo di;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s_sc_img[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
    }
    memset(&black, 0, sizeof black);
    black.float32[3] = 1.0f;
    memset(&rr, 0, sizeof rr);
    rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rr.levelCount = 1;
    rr.layerCount = 1;
    vkCmdClearColorImage(cb, s_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    dimg = s_sc_img[idx];
    if (s_slot[k].img && s_slot[k].layout == VK_IMAGE_LAYOUT_GENERAL && fw && fh && !fmt_odd) {
        struct nv2a_rect r2, sr;
        uint32_t sw = fw < s_slot[k].w ? fw : s_slot[k].w, sh = fh < s_slot[k].h ? fh : s_slot[k].h;
        int whole;
        nv2a_present_frame(flip);
        whole = nv2a_present_layout(sw, sh, (uint32_t)s_sc_ext.width,
                                    (uint32_t)s_sc_ext.height, filter, &sr, &r2);
        if (r2.w > 0 && r2.h > 0) {
            int lin = filter == NV2A_PRESENT_LINEAR
                      || (filter == NV2A_PRESENT_INTEGER && !whole);
            VkImageBlit b;
            VkImage bsrc;
            VkImageLayout bsrc_layout;
            /* The post passes, if asked for, then the blit reads their last
             * target; else after the backend's copy into the slot (earlier on
             * the queue), and the clear above. */
            s_pp_frame_w = sw;
            s_pp_frame_h = sh;
            post_run(cb, k, &bsrc, &bsrc_layout);
            if (bsrc == s_slot[k].img)
                nvk_image_barrier(cb, s_slot[k].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            nvk_image_barrier(cb, dimg, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.layerCount = 1;
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = sr.x;
            b.srcOffsets[0].y = sr.y;
            b.srcOffsets[1].x = sr.x + sr.w;
            b.srcOffsets[1].y = sr.y + sr.h;
            b.srcOffsets[1].z = 1;
            b.dstOffsets[0].x = r2.x;
            b.dstOffsets[0].y = r2.y;
            b.dstOffsets[1].x = r2.x + r2.w;
            b.dstOffsets[1].y = r2.y + r2.h;
            b.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, bsrc, bsrc_layout, dimg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
    /* The settings overlay, over the game's image and under the present
     * (overlay.h): a dynamic-rendering pass on the swapchain image. */
    if (s_sc_view[idx] && xbox_OverlayWants() && !hdr && !fmt_odd) {
        nvk_image_barrier(cb, dimg, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        xbox_OverlayVulkanDraw(C, cb, s_sc_view[idx], s_sc_ext.width, s_sc_ext.height,
                               s_sc_fmt, s_sc_n);
        nvk_image_barrier(cb, dimg, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    } else if (s_sc_view[idx] && xbox_OverlayWants() && hdr_overlay_ready()) {
        /* BO-110: over an HDR10 frame: drawn apart, then encoded over it. */
        struct nvk_image sw, *in1 = &s_hcomp;
        VkImageLayout inl = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkClearColorValue clear0;
        float pk, pw, hw, pc[4];
        memset(&clear0, 0, sizeof clear0);
        nvk_image_to(cb, &s_hcomp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, s_hcomp.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear0, 1, &rr);
        nvk_image_to(cb, &s_hcomp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        xbox_OverlayVulkanDraw(C, cb, s_hcomp.view, s_sc_ext.width, s_sc_ext.height,
                               VK_FORMAT_B8G8R8A8_UNORM, s_sc_n);
        s_hcomp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        nvk_image_to(cb, &s_hcomp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        memset(&sw, 0, sizeof sw);
        sw.img = s_sc_img[idx];
        sw.view = s_sc_view[idx];
        sw.format = s_sc_fmt;
        sw.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        sw.w = s_sc_ext.width;
        sw.h = s_sc_ext.height;
        sw.levels = 1;
        sw.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        pc[0] = hw / 10000.0f;
        pc[1] = pc[2] = pc[3] = 0.0f;
        s_pp_half = 0;
        s_pp_load = 1;
        post_pass(cb, PP_PQ, &sw, 0, &in1, &inl, 1, pc);
        s_pp_load = 0;
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        sw.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hdr_pattern(cb, &sw);
    }
#endif
    if (s_lat_sig < 0) {
        const char *e = recomp_env(RENV_LAT_SIG);
        s_lat_sig = e && *e && strcmp(e, "0") && xbox_LatOn();
    }
    if (s_lat_sig && new_frame && s_slot[k].img && s_slot[k].layout == VK_IMAGE_LAYOUT_GENERAL
            && fw && fh) {
        uint32_t lw = fw < s_slot[k].w ? fw : s_slot[k].w, lh = fh < s_slot[k].h ? fh : s_slot[k].h;
        VkDeviceSize need = (VkDeviceSize)lw * lh * 4u;
        if (s_lat_buf[s_fi].size < need) {
            nvk_buffer_free(&s_lat_buf[s_fi]);
            nvk_buffer_new(&s_lat_buf[s_fi], need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (s_lat_buf[s_fi].size >= need && s_lat_buf[s_fi].map) {
            VkBufferImageCopy bc;
            VkMemoryBarrier mb;
            memset(&bc, 0, sizeof bc);
            bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount = 1;
            bc.imageExtent.width = lw;
            bc.imageExtent.height = lh;
            bc.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cb, s_slot[k].img, VK_IMAGE_LAYOUT_GENERAL,
                                   s_lat_buf[s_fi].buf, 1, &bc);
            memset(&mb, 0, sizeof mb);
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, NULL, 0, NULL);
            s_lat_flip[s_fi] = flip;
            s_lat_w[s_fi] = lw;
            s_lat_h[s_fi] = lh;
            s_lat_pend[s_fi] = 1;
        }
    }
    if (shot) {
        VkBufferImageCopy bc;
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = s_sc_ext.width;
        bc.imageExtent.height = s_sc_ext.height;
        bc.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, s_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_shot_buf.buf, 1, &bc);
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    if (s_tq_on) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tq, (uint32_t)(s_fi * 2 + 1));
        s_tq_set[s_fi] = 1;
    }
    vkEndCommandBuffer(cb);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &s_acq[s_fi];
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_done[idx];
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_sc;
    pi.pImageIndices = &idx;
    {
        static VkPresentIdKHR pid;
        static uint64_t idv;
        if (C->present_wait
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
                && !C->present_wait2     /* VK_KHR_present_id is not enabled then */
#endif
                ) {
            idv = s_present_id + 1;
            memset(&pid, 0, sizeof pid);
            pid.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
            pid.swapchainCount = 1;
            pid.pPresentIds = &idv;
            pi.pNext = &pid;
        }
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
        if (C->present_wait2) {      /* BO-207: the same id, the second form */
            static VkPresentId2KHR pid2;
            idv = s_present_id + 1;
            memset(&pid2, 0, sizeof pid2);
            pid2.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
            pid2.swapchainCount = 1;
            pid2.pPresentIds = &idv;
            pi.pNext = s_pw2_ok ? &pid2 : NULL;
        }
#endif
    }
    {   /* RECOMP_DEBUG=present_stall=ms[:every_s]: a late present on demand. */
        static int st_ms = -1;
        static uint64_t st_every, st_next;
        if (st_ms < 0) {
            const char *e = recomp_env(RENV_PRESENT_STALL), *c;
            st_ms = e && *e ? atoi(e) : 0;
            c = e ? strchr(e, ':') : NULL;
            st_every = (uint64_t)(c && atoi(c + 1) > 0 ? atoi(c + 1) : 10) * 1000000000ull;
        }
        if (st_ms > 0) {
            uint64_t now = mono_ns();
            if (!st_next)
                st_next = now + st_every;
            else if (now >= st_next) {
                st_next = now + st_every;
                fprintf(stderr, LOGP "present_stall: %d ms before the present of flip %u\n", st_ms, flip);
                SDL_Delay((Uint32)st_ms);
            }
        }
    }
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, s_fence[s_fi]);
    if (r == VK_SUCCESS) {
        char what[96];
        snprintf(what, sizeof what, "window present flip %u, swapchain %ux%u", flip,
                 s_sc_ext.width, s_sc_ext.height);
        nvk_submit_note(what);
    }
    pr = r == VK_SUCCESS ? vkQueuePresentKHR(C->queue, &pi) : r;
    nvk_queue_unlock();
    if (r == VK_SUCCESS) {
        uint64_t t = mono_ns();
        pthread_mutex_lock(&s_tm_mu);
        iv_add(&s_host_iv, t);
        pthread_mutex_unlock(&s_tm_mu);
        /* A new flip is counted new with the overlay open too (new_frame
         * is 0 then: not paced), so the counters line compares like with
         * like. */
        if (s_keep_repeat)
            s_cnt_refill++;
        else if (flip != s_cnt_last_flip) {
            s_cnt_new++;
            s_cnt_last_flip = flip;
        } else
            s_cnt_redraw++;
        if (t - t_call0 > s_cnt_present_max_ns)
            s_cnt_present_max_ns = t - t_call0;
        if (xbox_LatOn() && !s_keep_repeat)
            xbox_LatRow('P', flip, 0, NULL);
        if (!s_waiter_run)       /* else the waiter, at the display time */
            xbox_PacingPresent(t);
        pthread_mutex_lock(&s_id_mu);
        s_present_id++;
        s_id_flip[s_present_id % ID_RING] = flip;
        s_id_pns[s_present_id % ID_RING] = t;
        s_id_meas[s_present_id % ID_RING] = new_frame && xbox_HostFlipIntervalAsked() == 1;
        s_id_rep[s_present_id % ID_RING] = (uint8_t)s_keep_repeat;
        if (s_csv)
            fprintf(s_csv, "%llu,present,%llu,%u%s\n", (unsigned long long)s_present_id,
                    (unsigned long long)t, flip, s_keep_repeat ? ",repeat" : "");
        pthread_cond_broadcast(&s_sc_cv);
        pthread_mutex_unlock(&s_id_mu);
    }
    if (r != VK_SUCCESS) {
        static int said;
        if (said++ < 4)
            fprintf(stderr, LOGP "Vulkan present submit: %s\n", nvk_result_str(r));
        if (r == VK_ERROR_DEVICE_LOST)
            nvk_device_lost("the window's submit: VK_ERROR_DEVICE_LOST");
        /* The fence will not signal: make it signalled again. */
        {
            VkFenceCreateInfo fi;
            vkDestroyFence(C->dev, s_fence[s_fi], NULL);
            memset(&fi, 0, sizeof fi);
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(C->dev, &fi, NULL, &s_fence[s_fi]);
        }
        return 0;
    }
    if (pr == VK_ERROR_DEVICE_LOST)
        nvk_device_lost("vkQueuePresentKHR: VK_ERROR_DEVICE_LOST");
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || (pr == VK_SUBOPTIMAL_KHR && FBV_SUBOPTIMAL_RECREATES))
        s_recreate = 1;
    if (pr == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
    if (shot) {
        /* Read only once the copy is proven done. */
        VkResult wr = VK_SUCCESS;
        int out = nvk_wait_fence(NVK_WAIT_SHOT, "window shot", s_fence[s_fi], &wr);
        if (out == NVK_WAIT_OK)
            save_shot((const uint8_t *)s_shot_buf.map, s_sc_ext.width, s_sc_ext.height,
                      shot_path, flip);
        else
            gpu_wait_failed(out, wr, "window shot");
    } else if (shot_path) {
        fprintf(stderr, LOGP "window shot at flip %u: the swapchain cannot be read back\n",
                flip);
    }
    s_fi ^= 1;
    if (new_frame)
        s_keep_last_flip = flip;
#if defined(_WIN32)
    if (s_keep_on > 0 && new_frame && s_kw_refill && xbox_HostFlipIntervalAsked() == 1) {
        /* Smooth on Windows: the queue is empty at the switch (or a new
         * swapchain); this frame goes once more, so one stays queued. */
        char what[96];
        s_kw_refill = 0;
        s_kw_tries++;
        s_keep_repeat = 1;
        if (fbv_present(win, k, fw, fh, filter, NULL, flip))
            s_keep_refills++;
        s_keep_repeat = 0;
        snprintf(what, sizeof what, "switch-on refill %d, %.0f ms after the switch",
                 (int)s_kw_tries, (mono_ns() - s_kw_on_ns) / 1e6);
        keep_corrected(what, flip);
    } else
#endif
    if (s_keep_on > 0 && new_frame && __atomic_load_n(&s_keep_want, __ATOMIC_ACQUIRE) > 0) {
        /* Smooth: nothing was queued; the frame goes once more, so the next
         * one waits one refresh behind it. */
        s_keep_repeat = 1;
        if (fbv_present(win, k, fw, fh, filter, NULL, flip))
            s_keep_refills++;
        s_keep_repeat = 0;
        keep_corrected("queue empty, frame presented once more", flip);
    }
    return 1;
}

#endif /* !_WIN32 */

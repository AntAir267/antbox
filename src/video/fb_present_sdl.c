/**
 * Show the presented frames in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest used to own that thread
 * for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, input
 *   "present" pthread     the layer presenters' presents (Vulkan, Metal)
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * The present thread waits on the frame slots' condition variable for a new
 * frame or a request from the main thread (redraw, quit; frame_wake.h), so
 * no frame waits for SDL's event pump, which under gamescope's Xwayland can
 * block for a whole frame and more (BO-210). The main thread hands it what
 * it needs of the window (presentable or not; on macOS the layer's size and
 * the display, fbm_main_tick) and the overlay's input; AppKit and the
 * window's lifecycle stay on the main thread. The SDL renderer path (no
 * layer) still presents on the main thread.
 *
 * xbox_HostWindowMain() does the inversion: it opens the window, starts the
 * guest on a pthread with the main thread's stack size and QoS, starts the
 * present thread, and pumps events until the guest returns or the window is
 * closed.
 *
 * Frames come from the backend's flip (cpu_on_flip calls
 * xbox_FramebufferWindowPresent on the nv2a-ack thread, with the surface the
 * walker picked). The ack thread copies the finished surface into a free
 * slot of three and publishes it with a pointer swap under a mutex held for a
 * few instructions, so the walker never waits for the window and the window
 * never reads guest memory the rasteriser may be writing. The copy is the
 * same bytes RECOMP_FB_DUMP writes (bpp from pitch / width, 2 = R5G6B5,
 * 4 = X8R8G8B8), so the window shows what the dump shows.
 *
 * With RECOMP_PB_BACKEND=metal the window is a CAMetalLayer instead
 * (fb_present_metal.m; metal_present=readback keeps the SDL renderer): the
 * presenter owns the Metal device and queue and the backend adopts them
 * (xbox_FramebufferWindowMetal), so a frame drawn on the GPU reaches the
 * window by a blit into the back slot's texture
 * (xbox_FramebufferWindowBackTexture, then PublishTexture, inside the same
 * s_in_present handshake) and never through guest memory. Guest-bytes frames are uploaded into a slot. When the
 * layer cannot be set up, the window is re-created with the SDL renderer.
 *
 * With RECOMP_PB_BACKEND=vulkan the same shape on a Vulkan swapchain
 * (fb_present_vulkan.c; vk_present=readback keeps the SDL renderer): the
 * presenter makes the shared Vulkan device with the window's surface
 * (SDL_Vulkan_CreateSurface) and the backend adopts it, slots are images the
 * backend copies its frame into, and the main thread presents them FIFO.
 *
 * The settings overlay (src/overlay, overlay.h) rides on both layer presenters:
 * every event of the loop goes to it first (main thread), it is drawn into the
 * drawable after the game's image (present thread), and while it is open the
 * present thread presents on every turn, not only on a new game frame. The
 * SDL renderer path has none.
 *
 * RECOMP_HEADLESS=1 (or no display, or SDL video failing to start) skips all
 * of it: the guest runs on the main thread exactly as before.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_PRESENT_STATS=1 prints a [PRESENT] line every 5 s (one is always
 * printed at exit); RECOMP_WINDOW_SCALE=<n> sizes the window (default 2).
 *
 * RECOMP_PRESENT_TIMING=legacy|auto (display.present_timing): when a frame is
 * handed to the display, on the CAMetalLayer path (fb_present_metal.m and
 * src/kernel/present_cadence.h, BO-115). Each frame carries its emulated
 * vblank from the publish to its first present; the Metal presenter records
 * when frames reach the glass for the overlay's graph, and
 * RECOMP_TRACE=glass=<file> writes them out. The Vulkan presenter is as it
 * was: its graph says "not available".
 *
 * RECOMP_WINDOW_SHOT=<flip>[,<flip>...]: read back what the renderer drew for
 * the frame of that walker flip count (the number RECOMP_FB_DUMP_FLIPS and
 * RECOMP_FLIP_LOG print), at the window's pixel size, with
 * SDL_RenderReadPixels (the layer: a blit of the drawable), into
 * <prefix>win_flip<NNNNN>.bmp. <prefix> is RECOMP_WINDOW_SHOT_PREFIX, else
 * RECOMP_FB_DUMP, else "window_". A flip the window dropped is shot at the
 * next frame it shows, under that frame's own flip number. Only the
 * process's own pixels are read.
 *
 * RECOMP_WINDOW_QUIT_AFTER=<seconds>: post SDL_QUIT then, the event the
 * window's close button produces, so a script can test the close path.
 *
 * Enhancements (nv2a_host_opts, set by the opt-in layer's
 * xbox_enhance_init; all zero without it): present.filter = linear sets the
 * frame texture's scale mode on the same logical-size letterbox; integer
 * draws the frame at the largest whole multiple that fits, centred
 * (nv2a_present_rect; a frame larger than the window falls back to a
 * linear fit, never a crop, which is why SDL_RenderSetIntegerScale is not
 * used); present.fullscreen opens the window borderless fullscreen on the
 * desktop. The default (nearest, windowed) runs exactly the code it always
 * did.
 *
 * The window title is the title's to choose: call xbox_HostWindowSetTitle()
 * before xbox_HostWindowMain(). Unset, it is the XBE certificate's name, or
 * "Xbox Recomp" before the loader has read one. RECOMP_TRACE=title appends
 * FPS and draws, once a second (window_title.h).
 */
#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)

#include <SDL.h>
#include "kernel/xbox_pvideo.h"
#include "recomp_env.h"
#include "recomp_restart.h"
#include "recomp_exit_hook.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "kernel/nv2a_pb_state.h"
#include "kernel/nv2a_backend_common.h"
#include "window_title.h"
#include "frames_lost_rule.h"
#include "frame_wake.h"
#include <strings.h>
#if defined(__APPLE__)
#include "fb_present_metal.h"
#endif
#if defined(XBOXRECOMP_HAVE_VULKAN)
#include "fb_present_vulkan.h"
#include <SDL_vulkan.h>
#endif
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */
#include "mkb_input.h"        /* src/input: the mouse lock (RECOMP_MKB) */
#if defined(XBOXRECOMP_HAVE_OVERLAY)
#include "overlay.h"          /* src/overlay: the settings overlay */
#endif

extern uint64_t xbox_PacingFrameVblankNs(void);   /* src/kernel/kernel_pacing.c */
extern void xbox_PacingSetVisible(int visible);   /* src/kernel/kernel_pacing.c */
extern void xbox_DisplayRateSet(double hz);       /* src/kernel/kernel_pacing.c */

/* The refresh rate of the display the window is on, for a title that holds
 * its flips to it (graphics.high_refresh): read twice a second, so a window
 * moved to another display or a mode change is followed. An exclusive
 * fullscreen window's own mode, else the display's. SDL2 gives whole Hz
 * (175 for a 174.96 Hz panel). */
static void display_rate_poll(SDL_Window *win)
{
    static Uint32 last;
    Uint32 now = SDL_GetTicks();
    SDL_DisplayMode dm;
    int di;

    if (!win || (last && now - last < 500))
        return;
    last = now;
    di = SDL_GetWindowDisplayIndex(win);
    if ((SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN
            ? SDL_GetWindowDisplayMode(win, &dm) == 0
            : di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0)
        xbox_DisplayRateSet(dm.refresh_rate > 0 ? (double)dm.refresh_rate : 0.0);
}
extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_log_thread_role(const char *role, uint32_t routine);
extern void xbox_InputSetMainPump(int on);   /* src/input/xinput_device.c */
extern void xbox_InputInit(void);            /* src/input/input_core.c */
extern void xbox_InputPadEvent(const SDL_Event *ev); /* src/input/xinput_device.c */
extern void xbox_InputShutdown(void);                /* src/input/input_core.c */
extern int  xbox_InputAnyOpen(void);

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t w, h, bpp;
    uint32_t flip;                 /* the walker's flip count at this frame */
    uint64_t vbl_ns;               /* its emulated vblank (kernel_pacing.h) */
};

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
/* RECOMP_TRACE=pacing=all: when each frame was published (host ns, by
 * sequence, a small ring), so a dropped one can be told apart: two
 * publishes inside one main-loop turn, and how long that turn took. */
static int             s_trace_drops = -1;
static uint64_t        s_pub_ns[8];
static uint64_t        s_last_pickup_ns, s_last_present_ns;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
/* Signalled at each publish and each request, under s_lock: the present
 * thread waits on it (frame_wake.h, BO-210); so does the main loop on the SDL
 * renderer path. */
static pthread_cond_t  s_pub_cv = PTHREAD_COND_INITIALIZER;
static volatile uint64_t s_last_pub_ns;     /* the last publish, host ns */
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop (a lost device) */
/* The present thread: its requests (FW_CTL_*, under s_lock), and what the
 * main thread tells it of the window. */
static unsigned        s_pt_ctl;
static volatile int    s_win_ok = 1;         /* not minimised, hidden or (Android) away */
static volatile int    s_app_away;           /* Android: the app is in the background */
/* What reached the window, for the counters and the closing line: the
 * present thread's (the main thread's on the SDL renderer path). */
static uint64_t        s_shown_seq, s_presents, s_dropped, s_upload_ns;
static volatile int    s_in_present;         /* ack thread inside Present */
/* Layer mode (macOS, Metal backend): the window has a CAMetalLayer and no
 * SDL_Renderer, and a slot is a GPU texture in fb_present_metal.m (indexed by
 * slot number) rather than bytes. Set once before the guest starts. */
static int             s_layer;
/* Vulkan slot mode (RECOMP_PB_BACKEND=vulkan): s_layer is set too, and a
 * slot is an image in fb_present_vulkan.c. */
static int             s_vk;
/* The settings overlay is open: the present thread presents every turn
 * (the present thread's; read by the counters). */
static int             s_overlay_open;
static int             s_overlay_note;  /* the overlay shows a line without being open */

/* Ack-thread side counters, read by the stats line. */
static uint64_t s_flips, s_copy_ns;          /* relaxed atomics */

static uint64_t now_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* RECOMP_DEBUG=pump_stall=<ms>[,pump_stall_after=<s>] (BO-210 test): the
 * main loop sleeps before each event read, standing in for a blocking X11
 * pump. Zero cost when off: one cached flag. */
static int pump_stall_ms(void)
{
    static int ms = -1;
    static double after;
    static uint64_t t0;
    if (ms < 0) {
        const char *v = recomp_env(RENV_PUMP_STALL);
        const char *a = recomp_env(RENV_PUMP_STALL_AFTER);
        ms = v ? atoi(v) : 0;
        after = a ? atof(a) : 0.0;
        t0 = now_ns(CLOCK_MONOTONIC);
    }
    if (!ms)
        return 0;
    if ((double)(now_ns(CLOCK_MONOTONIC) - t0) < after * 1e9)
        return 0;
    return ms;
}

static void pump_stall_sleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}


void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va;
    (void)pitch;
}

void xbox_FramebufferWindowStart(void) {}

static void present_copy(uint32_t fb_va, uint32_t pitch);
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t pitch, uint32_t bpp, uint64_t t0);

/* Whether the window is up and takes frames: a GPU backend skips its
 * readback for the window when not (headless). */
int xbox_FramebufferWindowRunning(void)
{
    return __atomic_load_n(&s_running, __ATOMIC_SEQ_CST) != 0;
}

/* A GPU backend's frame, already in host memory (Metal: the presented
 * MTLTexture read back at the flip, BGRA8 = X8R8G8B8, bpp 4), handed to the
 * window instead of the guest surface. Same thread and handshake as
 * xbox_FramebufferWindowPresent. */
void xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                         uint32_t pitch, uint32_t bpp)
{
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !px || !w || !h
            || (bpp != 2 && bpp != 4) || pitch < w * bpp)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_pixels((const uint8_t *)px, w, h, pitch, bpp, now_ns(CLOCK_MONOTONIC));
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Called by the backend at each flip, on the nv2a-ack thread. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {   /* closing */
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return;
    }
    present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    const struct nv2a_pb_present *p;
    const uint8_t *src;
    uint32_t w = 640, h = 480, bpp;
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);

    /* The walker's pick has the surface's own size; before its first pick
     * (or for a surface it did not pick) the display's 640x480. */
    p = nv2a_pb_present_state();
    if (p && p->offset && p->w && p->h && p->pitch == pitch
            && nv2a_pb_dma_resolve(p->offset) == fb_va) {
        w = p->w;
        h = p->h;
    }
    bpp = pitch / w;
    if (bpp != 2 && bpp != 4)
        return;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    present_pixels(src, w, h, pitch, bpp, t0);
}

/* The copy and the publish, from any w x h image of `bpp` bytes a pixel,
 * `pitch` bytes a row: guest memory for present_copy, a GPU backend's own
 * readback for xbox_FramebufferWindowPresentPixels. */
/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t w, uint32_t h, uint32_t bpp, uint64_t t0)
{
    struct fb_slot *s = &s_slot[s_back];   /* only this thread touches s_back */

    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->flip = nv2a_pb_gpu_state()->flips;
    s->vbl_ns = xbox_PacingFrameVblankNs();
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
        s_pub_ns[s_ready_seq & 7] = now_ns(CLOCK_MONOTONIC);
        __atomic_store_n(&s_last_pub_ns, s_pub_ns[s_ready_seq & 7], __ATOMIC_RELEASE);
        pthread_cond_signal(&s_pub_cv);
    }
    pthread_mutex_unlock(&s_lock);
    /* No SDL event: the present thread waits on s_pub_cv (BO-210). */

    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_copy_ns, now_ns(CLOCK_MONOTONIC) - t0, __ATOMIC_RELAXED);
}

/* The copy and the publish, from any w x h image of `bpp` bytes a pixel,
 * `pitch` bytes a row: guest memory for present_copy, a GPU backend's own
 * readback for xbox_FramebufferWindowPresentPixels. In layer mode the image
 * is uploaded into the back slot's texture instead. */
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t pitch, uint32_t bpp, uint64_t t0)
{
    struct fb_slot *s;
    size_t need;
    uint32_t y;

#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk) {
        if (fbv_upload(s_back, src, w, h, pitch, bpp))
            publish(w, h, 4, t0);
        return;
    }
#endif
#if defined(__APPLE__)
    if (s_layer) {
        if (fbm_upload(s_back, src, w, h, pitch, bpp))
            publish(w, h, 4, t0);
        return;
    }
#endif
    s = &s_slot[s_back];               /* only this thread touches s_back */
    need = (size_t)w * h * bpp;
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    for (y = 0; y < h; y++)
        memcpy(s->px + (size_t)y * w * bpp, src + (size_t)y * pitch,
               (size_t)w * bpp);
    publish(w, h, bpp, t0);
}

/* ── Layer mode: the Metal backend's side (nv2a-ack thread) ─────────────── */

/* 1 with the layer's device and queue when the window presents GPU slot
 * textures; the Metal backend adopts them at its init. 0 otherwise
 * (headless, the renderer window, not macOS). */
int xbox_FramebufferWindowMetal(void **device, void **queue)
{
#if defined(__APPLE__)
    if (s_layer && !s_vk) {
        fbm_get(device, queue);
        return 1;
    }
#endif
    (void)device;
    (void)queue;
    return 0;
}

/* 1 when the window presents Vulkan slot images on the shared device
 * (nv2a_vk_core.h); the Vulkan backend then copies its frames into them. */
int xbox_FramebufferWindowVulkan(void)
{
    return s_vk;
}

/* The display's last vblank as the window saw a frame reach it
 * (CLOCK_MONOTONIC ns) and its measured refresh period, for pacing that
 * wants to lock the emulated vblank to the real one; 0 when unknown (no
 * Vulkan window, or no VK_KHR_present_wait). Any thread. */
int xbox_FramebufferWindowVblank(uint64_t *t_ns, uint64_t *period_ns)
{
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk)
        return fbv_display_vblank(t_ns, period_ns);
#endif
    *t_ns = 0;
    *period_ns = 0;
    return 0;
}

/* The back slot's id<MTLTexture> (Vulkan: struct nvk_image *) at w x h, for
 * the backend to blit its frame into, or NULL (no layer, window closing). A non-NULL return must be
 * followed by xbox_FramebufferWindowPublishTexture: the ack thread counts as
 * inside a present from here to there (the close path's handshake). */
static uint64_t s_tex_t0;
void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h)
{
#if defined(__APPLE__) || defined(XBOXRECOMP_HAVE_VULKAN)
    void *t = NULL;
    if (!s_layer || !__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !w || !h)
        return NULL;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    s_tex_t0 = now_ns(CLOCK_MONOTONIC);
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk)
        t = fbv_slot(s_back, w, h);
#endif
#if defined(__APPLE__)
    if (!s_vk)
        t = fbm_slot(s_back, w, h);
#endif
    if (!t) {
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    s_slot[s_back].w = w;
    s_slot[s_back].h = h;
    return t;
#else
    (void)w;
    (void)h;
    return NULL;
#endif
}

/* BO-110: the back slot's scene snapshot texture at w x h (Metal), between
 * BackTexture and PublishTexture; valid 0 says this frame has none (all of
 * it is UI). NULL when there is no such slot. */
void *xbox_FramebufferWindowBackScene(uint32_t w, uint32_t h, int valid)
{
#if defined(__APPLE__)
    if (!s_vk)
        return fbm_scene_slot(s_back, w, h, valid);
#endif
    (void)w;
    (void)h;
    (void)valid;
    return NULL;
}

/* The back slot's blit is committed: publish it. */
void xbox_FramebufferWindowPublishTexture(void)
{
    publish(s_slot[s_back].w, s_slot[s_back].h, 4, s_tex_t0);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    (void)arg;
    s_guest_rc = s_guest_main();
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_guest_done_event;
        SDL_PushEvent(&ev);
    }
    return NULL;
}

/* Title bar (window_title.h). A title's own xbox_HostWindowSetTitle wins;
 * otherwise the XBE certificate's name, which the loader hands
 * xbox_FramebufferWindowSetTitle. RECOMP_TRACE=title adds FPS and, once the
 * executor reports them, draws per frame, as the Win32 window does
 * (upstream 331d95b). Written from guest threads, read by the main thread:
 * a torn read shows a stale name until the next write, and s_name_gen,
 * bumped after each name change, makes sure there is one. */
static char s_title[128] = "Xbox Recomp";
static char s_xbe_title[128];
static int  s_host_title_set;
static int  s_have_draws;
static uint32_t s_frame_draws;
static unsigned s_name_gen;

void xbox_HostWindowSetTitle(const char *title)
{
    if (title && *title) {
        snprintf(s_title, sizeof s_title, "%s", title);
        __atomic_store_n(&s_host_title_set, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_name_gen, 1, __ATOMIC_RELEASE);
    }
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_xbe_title];

    if (xbox_title_utf16_to_utf8(buf, sizeof buf, name, max_chars)) {
        memcpy(s_xbe_title, buf, strlen(buf) + 1);
        __atomic_add_fetch(&s_name_gen, 1, __ATOMIC_RELEASE);
    }
}

/* Draws in the frame just flipped; the executor calls it at the flip. */
void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
    __atomic_store_n(&s_have_draws, 1, __ATOMIC_RELAXED);
}

static const char *window_name(void)
{
    if (__atomic_load_n(&s_host_title_set, __ATOMIC_ACQUIRE) || !s_xbe_title[0])
        return s_title;
    return s_xbe_title;
}

/* A new window is asked for scale x the frame size, centred on a display.
 * On a screen with less room than that (1024x768, 1366x768, a 1080p panel at
 * 150 % scaling, a taskbar or menu bar taking its strip) the title bar or the
 * bottom edge ended up off the screen, and on Windows a window is not
 * kept on screen by the system. Once the window exists (its border sizes are
 * known only then): if the window with its frame does not fit the display's
 * usable bounds, shrink it to fit (keeping the aspect), and put it in the
 * middle of the usable area. A window that already fits is left alone.
 * Skipped for fullscreen. The usable bounds and the border sizes are SDL's
 * (SDL_GetDisplayUsableBounds, SDL_GetWindowBordersSize; the same calls
 * other SDL ports use for this); platforms that cannot report the borders
 * report 0 for them. */
static void fit_window(SDL_Window *win)
{
    SDL_Rect db, ub;
    int di, w, h, x, y, bt = 0, bl = 0, bb = 0, br = 0, fits;
    float ddpi = 0.f;

    if (!win || (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)))
        return;
    di = SDL_GetWindowDisplayIndex(win);
    if (di < 0 || SDL_GetDisplayBounds(di, &db) != 0 || SDL_GetDisplayUsableBounds(di, &ub) != 0
            || ub.w <= 0 || ub.h <= 0)
        return;
    SDL_GetDisplayDPI(di, &ddpi, NULL, NULL);
    {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(di, &dm) == 0)
            XR_NOTICE( "[PRESENT] display %d mode %dx%d at %d Hz\n", di, dm.w, dm.h,
                    dm.refresh_rate);
    }
    SDL_GetWindowBordersSize(win, &bt, &bl, &bb, &br);
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetWindowPosition(win, &x, &y);
    XR_NOTICE( "[PRESENT] window: display %d \"%s\" %dx%d at %d,%d, usable %dx%d at %d,%d,"
            " %.0f dpi; window %dx%d at %d,%d, borders t%d l%d b%d r%d\n",
            di, SDL_GetDisplayName(di) ? SDL_GetDisplayName(di) : "", db.w, db.h, db.x, db.y,
            ub.w, ub.h, ub.x, ub.y, (double)ddpi, w, h, x, y, bt, bl, bb, br);
    fits = x - bl >= ub.x && y - bt >= ub.y
        && x + w + br <= ub.x + ub.w && y + h + bb <= ub.y + ub.h;
    if (fits)
        return;
    {
        int max_w = ub.w - bl - br, max_h = ub.h - bt - bb;
        if (max_w < 160)
            max_w = 160;
        if (max_h < 120)
            max_h = 120;
        if (w > max_w || h > max_h) {
            /* aspect kept: the smaller of the two ratios */
            if ((long long)max_w * h < (long long)max_h * w) {
                h = (int)((long long)h * max_w / w);
                w = max_w;
            } else {
                w = (int)((long long)w * max_h / h);
                h = max_h;
            }
            SDL_SetWindowSize(win, w, h);
            SDL_GetWindowSize(win, &w, &h);
        }
        x = ub.x + bl + (ub.w - bl - br - w) / 2;
        y = ub.y + bt + (ub.h - bt - bb - h) / 2;
        SDL_SetWindowPosition(win, x, y);
        fprintf(stderr, "[PRESENT] window did not fit the usable area: now %dx%d at %d,%d\n",
                w, h, x, y);
    }
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
#if !defined(_WIN32)
    struct rlimit rl;
#endif
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
#if !defined(_WIN32)
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;
#endif

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

#define SHOT_MAX 32
static uint32_t s_shot[SHOT_MAX];
static int      s_shot_n;

static void shot_parse(void)
{
    const char *e = recomp_env(RENV_WINDOW_SHOT);
    while (e && *e && s_shot_n < SHOT_MAX) {
        char *end;
        unsigned long v = strtoul(e, &end, 0);
        if (end == e)
            break;
        s_shot[s_shot_n++] = (uint32_t)v;
        e = *end == ',' ? end + 1 : end;
    }
}

/* A shot asked for by name (xbox_HostWindowShot): the next present is read
 * back into that file, whatever it shows (black before the first frame, an
 * open overlay over the paused title). Under s_lock; the present thread
 * takes it. */
static char s_shot_req[512];

/* A request for the present thread: its wait ends at once. Any thread. */
static void pt_request(unsigned bits)
{
    pthread_mutex_lock(&s_lock);
    s_pt_ctl |= bits;
    pthread_cond_signal(&s_pub_cv);
    pthread_mutex_unlock(&s_lock);
}

void xbox_HostWindowShot(const char *path)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_shot_req, sizeof s_shot_req, "%s", path ? path : "");
    s_pt_ctl |= FW_CTL_DIRTY;
    pthread_cond_signal(&s_pub_cv);
    pthread_mutex_unlock(&s_lock);
}

static char     s_burst_prefix[480];
static unsigned s_burst_n;

void xbox_HostWindowShotBurst(const char *prefix, unsigned n)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_burst_prefix, sizeof s_burst_prefix, "%s", prefix ? prefix : "");
    s_burst_n = n;
    pthread_mutex_unlock(&s_lock);
}

/* Shots may be asked for while running: the presenters keep their images
 * readable (RECOMP_WINDOW_SHOT, or a test driving the overlay). */
static int shots_wanted(void)
{
    return s_shot_n > 0 || recomp_env(RENV_OVERLAY_TEST) != NULL;
}

/* The first pending target this frame reaches, removed; -1 if none. */
static int shot_due(uint32_t flip)
{
    int i;
    for (i = 0; i < s_shot_n; i++)
        if (flip >= s_shot[i]) {
            s_shot[i] = s_shot[--s_shot_n];
            return 1;
        }
    return 0;
}

static void shot_path(char *path, size_t n, uint32_t flip)
{
    const char *prefix = recomp_env(RENV_WINDOW_SHOT_PREFIX);

    if (!prefix)
        prefix = recomp_env(RENV_FB_DUMP);
    if (!prefix)
        prefix = "window_";
    snprintf(path, n, "%swin_flip%05u.bmp", prefix, flip);
}

/* Between RenderCopy and RenderPresent: the back buffer is what is shown. */
static void shot_save(SDL_Renderer *ren, uint32_t flip)
{
    SDL_Surface *surf;
    char path[512];
    int w = 0, h = 0;

    if (SDL_GetRendererOutputSize(ren, &w, &h) != 0 || w <= 0 || h <= 0)
        return;
    surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_BGR24);
    if (!surf)
        return;
    shot_path(path, sizeof path, flip);
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_BGR24, surf->pixels,
                             surf->pitch) == 0 && SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, "[PRESENT] window shot: %s (%dx%d, flip %u)\n",
                path, w, h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n",
                flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

/* The renderer, or the layer's resources; the window goes after. */
static void close_present(SDL_Renderer *ren)
{
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk) {
        fbv_destroy();
        s_layer = s_vk = 0;
        return;
    }
#endif
#if defined(__APPLE__)
    if (s_layer) {
        fbm_destroy();
        s_layer = 0;
        return;
    }
#endif
    if (ren)
        SDL_DestroyRenderer(ren);
}

#if defined(__ANDROID__)
/* The app leaves the screen and comes back (Home, recents, screen off): SDL
 * delivers these from the activity's thread, at once, which is why this is an
 * event watch and not the event loop. While away the title is held as the
 * overlay holds it (kernel_pause.h: it stops at its next flip, its sound
 * stops with it), and the presenter is told its surface is gone; it makes a
 * new one at its first present after the return. A pause the overlay made
 * itself is left alone. */
static int s_bg_paused;
void xbox_HostPause(int on);
int  xbox_HostPaused(void);
void fbv_android_surface_gone(void);

#include <jni.h>
#include <SDL_system.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
/* recomp_restart.h on Android: the activity starts the app again (int
 * restartGame(): the same path as its driver reset, a page in another
 * process that starts the game once this process is gone). An activity
 * without the method: nothing happens, and the log says so. */
static void android_restart(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    jclass cls;
    jmethodID m;

    if (!act)
        return;
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, "restartGame", "()I");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    if (m) {
        (*env)->CallIntMethod(env, act, m);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
    } else {
        fprintf(stderr, "[RESTART] this app's activity has no restartGame(): not restarted\n");
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

/* recomp_quit_request (the overlay's Quit game) on Android: the activity is
 * finished and its task removed, so the app is closed and does not stay in
 * the recents. The window is closed the ordinary way right after (the
 * SDL_QUIT the overlay posts), which ends the process. Activity.
 * finishAndRemoveTask() is the platform's (API 21): no method of the app's
 * own is needed. Untested on a device. */
static void android_quit(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    jclass cls;
    jmethodID m;

    if (!act)
        return;
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, "finishAndRemoveTask", "()V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    if (m) {
        (*env)->CallVoidMethod(env, act, m);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

static void android_exit_now(const char *why);

/* The warning that no usable Vulkan driver was found, with the ways out on
 * it (BO-121, BO-153, BO-163): without the Vulkan window there is no
 * settings overlay, so a player whose driver cannot run the game could not
 * choose another from inside it. The port's activity says what there is to
 * offer and does the going (int resetGraphicsDriver(int): 0 asks, as bits:
 * 1 the driver in the app to go back to, 2 a driver package is worth
 * picking (an Adreno GPU, which Turnip drives), 4 this start was on the
 * driver in the app; 1 goes back to it and starts the app again), and opens
 * its driver page on the file picker (int pickDriverPackage(): the package
 * picked is chosen and the app starts again on it; that is also the only
 * way out when the driver in the app is the one that cannot run the game,
 * as Turnip on an Adreno A6xx "gen1 low" with Vulkan 1.0). The text is for
 * a player; the reason is in the log. An activity with neither method, or
 * a GPU with nothing to offer, gets the plain warning (msg). Returns 1 when
 * the window with the choices was shown. */
static int android_offer_driver_reset(const char *msg)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    jclass cls;
    jmethodID m, pick;
    int shown = 0, reset, ways = 2;

    if (!act)
        return 0;
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, "resetGraphicsDriver", "(I)I");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    pick = (*env)->GetMethodID(env, cls, "pickDriverPackage", "()I");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        pick = NULL;
    }
    if (m) {
        ways = (*env)->CallIntMethod(env, act, m, (jint)0);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            ways = 0;
        }
    }
    reset = (ways & 1) != 0;
    if (!(ways & 2))
        pick = NULL;
    if (reset || pick) {
        SDL_MessageBoxButtonData b[3];
        char text[1200];
        SDL_MessageBoxData d;
        int n = 0, hit = 0;

        memset(b, 0, sizeof b);
        if (reset) {
            b[n].buttonid = 1;
            b[n++].text = "Use built-in";
        }
        if (pick) {
            b[n].buttonid = 2;
            b[n++].text = "Driver file\xE2\x80\xA6";
        }
        b[0].flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        b[n].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        b[n].buttonid = 0;
        /* SDL's Android box puts the buttons in one row at the box's
         * width: three short labels, or the third one is a column of
         * letters (seen on a 1240x1080 Retroid Pocket Classic). */
        b[n].text = n == 2 ? "Continue" : "Continue (slow)";
        n++;
        if (reset)
            snprintf(text, sizeof text, "%s\n\n\"Use built-in\" restarts the game on the"
                     " Turnip driver that comes with Takedawn.%s \"Continue\" keeps going on"
                     " the processor.", msg,
                     pick ? " \"Driver file\xE2\x80\xA6\" lets you pick a Turnip driver"
                            " (.zip)." : "");
        else
            /* The driver in the app is the one that failed: what a driver
             * file is, and where players get one. */
            snprintf(text, sizeof text, "%s\n\nThe Turnip driver built into Takedawn can't"
                     " run the game on this GPU. A driver file is a Turnip build for Adreno"
                     " GPUs, packed as a .zip. Android emulator users share them for many"
                     " devices. \"Driver file\xE2\x80\xA6\" lets you pick one. The Android"
                     " section of the README has more.", msg);
        memset(&d, 0, sizeof d);
        d.flags = SDL_MESSAGEBOX_WARNING;
        d.title = "Graphics driver";
        d.message = text;
        d.numbuttons = n;
        d.buttons = b;
        shown = SDL_ShowMessageBox(&d, &hit) == 0;
        if (shown && hit == 1) {
            fprintf(stderr, "[PRESENT] the warning's \"Use built-in\": the app starts"
                    " again on the driver in the app\n");
            (*env)->CallIntMethod(env, act, m, (jint)1);
            if ((*env)->ExceptionCheck(env))
                (*env)->ExceptionClear(env);
            /* The restart page is started and waits for this process to
             * be gone; the game never drew on this driver (that is why
             * the box is up), so there is no GPU work to wait for. */
            android_exit_now("driver reset");
        } else if (shown && hit == 2) {
            /* The driver page comes up over the game, which goes on on
             * the CPU behind it: a package picked there restarts the app
             * (the activity's way, as the overlay's restart), Back or a
             * cancelled picker leaves the game as it is. */
            fprintf(stderr, "[PRESENT] the warning's \"Driver file\": the driver page"
                    " opens on the file picker\n");
            (*env)->CallIntMethod(env, act, pick);
            if ((*env)->ExceptionCheck(env))
                (*env)->ExceptionClear(env);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return shown;
}

/* ── the way out on Android: the GPU torn down before the process ends ──
 *
 * Every way the game ends on Android comes through the window loop's end
 * (Quit game, Restart game, a driver change, Back or the system finishing
 * the activity, the box after a lost device): the activity is finished, SDL
 * sends SDL_QUIT, the loop ends. Before exit(0) the Vulkan side goes in
 * order, as the emulators do at their own stop: the executor held (it
 * submits no more), the pre-warm thread stopped and the pipeline cache
 * written, the swapchain, then vkDeviceWaitIdle and the surface, the device
 * and the instance (nvk_core_shutdown). The kernel driver (kgsl) then has
 * nothing of this process's left when the next one opens the GPU: the app
 * restarts only once this process is gone (RestartActivity waits for it).
 * A step that cannot be done safely (the executor not held in time) leaves
 * the device to the process's end, and says so. A watchdog ends the process
 * if the teardown itself hangs (a wedged GPU). Each step is in the log with
 * the time since the teardown began. */
#if defined(XBOXRECOMP_HAVE_VULKAN)
int  xbox_HostHoldForExit(int timeout_ms);
int  nvk_backend_quiesce(int timeout_ms);
void nvk_core_shutdown(void);
#endif
#include <unistd.h>
void xbox_crash_log_flush(unsigned max_ms);   /* kernel/xbox_crash.h */

static struct timespec s_td_t0;
static volatile int s_td_done;

static double td_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - s_td_t0.tv_sec) * 1e3
         + (double)(t.tv_nsec - s_td_t0.tv_nsec) / 1e6;
}

static void *td_watchdog(void *arg)
{
    (void)arg;
    sleep(4);
    if (!s_td_done) {
        XR_NOTICE("[TEARDOWN] not done after %.0f ms: the process ends now (_exit)\n",
                td_ms());
        fflush(stderr);
        xbox_crash_log_flush(300);
        _exit(0);
    }
    return NULL;
}

/* The process's end on Android. Not exit(): its C++ static destructors
 * (the system's EGL and Adreno GLES libraries among them) run while the
 * activity's window still draws on HWUI's RenderThread, which then crashed
 * in eglQuerySurface (SIGSEGV at 0, Galaxy S20 FE, 2026-10-13: after a
 * clean teardown, and with diagnostic logging off too). So, as the Win32
 * windows do before ExitProcess: the runtime's exit hooks (a save still
 * being written reaches the disk, the pads' motors off, the missing-file
 * summary), the log passed on to its file, then _exit, which runs no
 * destructors. The pipeline cache was written by nvk_backend_quiesce. */
#include "../platform/recomp_exit_hook.h"

static void android_exit_now(const char *why)
{
    if (!s_td_t0.tv_sec && !s_td_t0.tv_nsec)
        clock_gettime(CLOCK_MONOTONIC, &s_td_t0);
    XR_NOTICE("[TEARDOWN] %s: exit hooks (%.1f ms)\n", why, td_ms());
    recomp_exit_hook_run("android");
    XR_NOTICE("[TEARDOWN] %s: _exit(0) (%.1f ms)\n", why, td_ms());
    s_td_done = 1;
    fflush(stdout);
    fflush(stderr);
    xbox_crash_log_flush(500);
    _exit(0);
}

/* Before the swapchain goes. Returns 1 when the device may be destroyed. */
static int android_teardown_begin(int vk)
{
    pthread_t th;
    int held = 0, quiet = 0;

    clock_gettime(CLOCK_MONOTONIC, &s_td_t0);
    XR_NOTICE("[TEARDOWN] begin (%s)\n", vk ? "Vulkan" : "no Vulkan device");
    if (pthread_create(&th, NULL, td_watchdog, NULL) == 0)
        pthread_detach(th);
    {
        /* The ADPF sessions (android_perf.c), from the thread that reports. */
        extern void xbox_AndroidPerfClose(void);
        xbox_AndroidPerfClose();
    }
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (vk) {
        held = xbox_HostHoldForExit(1500);
        XR_NOTICE("[TEARDOWN] the title's executor %s (%.1f ms)\n",
                held ? "is held" : "was not held in time", td_ms());
        /* The title is parked, so nothing waits on the GP doorbell any more:
         * now the APU frame thread can go (BO-219). A title that was not held
         * keeps its audio thread to the end of the process. */
        if (held)
            recomp_quit_hook_run(RECOMP_QUIT_HELPERS);
        quiet = nvk_backend_quiesce(1000);
        XR_NOTICE("[TEARDOWN] backend %s (%.1f ms)\n",
                quiet ? "quiet" : "not quiet", td_ms());
    }
#endif
    fflush(stderr);
    return held && quiet;
}

/* After the swapchain is gone (close_present). */
static void android_teardown_end(int vk, int destroy)
{
    XR_NOTICE("[TEARDOWN] swapchain destroyed (%.1f ms)\n", td_ms());
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (vk && destroy)
        nvk_core_shutdown();
    else if (vk)
        XR_NOTICE("[TEARDOWN] the device is left to the process's end (not safe to"
                " destroy now)\n");
#endif
    (void)destroy;
    XR_NOTICE("[TEARDOWN] GPU done (%.1f ms)\n", td_ms());
    fflush(stderr);
    s_td_done = 1;
}

#endif /* __ANDROID__ (reopened below) */

/* ── the graphics device is lost (nv2a_vk_core.h: nvk_device_lost) ───────
 *
 * A driver that gives up (VK_ERROR_DEVICE_LOST: on the Retroid Pocket 5 the
 * kernel's "GPU hang detected", BO-128) used to leave the last picture on
 * the screen with the title and its music running on behind it. Now the
 * window's loop is told, says so in a box, and ends the run the ordinary
 * way (the same as closing the window: the title's saves are what they
 * were at its last save, the settings are written). "Restart" where there
 * is one: on Android the activity's (restartApp), elsewhere the host's hook
 * when it has set one (xbox_FramebufferWindowSetRestart). */
static int s_gfx_lost;                 /* 1: to be said; 2: said */
static int s_gfx_restart;              /* the player chose Restart */
static char s_gfx_lost_why[160];
static void (*s_restart_fn)(void);

void xbox_FramebufferWindowSetRestart(void (*fn)(void))
{
    s_restart_fn = fn;
}

static void gfx_lost_handler(const char *why)
{
    snprintf(s_gfx_lost_why, sizeof s_gfx_lost_why, "%s", why ? why : "");
    __atomic_store_n(&s_gfx_lost, 1, __ATOMIC_RELEASE);
    {
        /* The loop may be asleep in its wait for an event. */
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        SDL_PushEvent(&ev);
    }
}

static int gfx_can_restart(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    /* The host's hook, or the program's own restart (recomp_restart.h). */
    return s_restart_fn != NULL || recomp_restart_available();
#endif
}

static void gfx_restart(void)
{
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    if (act) {
        jclass cls = (*env)->GetObjectClass(env, act);
        jmethodID m = (*env)->GetMethodID(env, cls, "restartApp", "()V");
        if (m)
            (*env)->CallVoidMethod(env, act, m);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        /* The restart page is started; the caller ends this process. */
    }
#else
    if (s_restart_fn)
        s_restart_fn();
    else
        recomp_restart_request("The graphics driver stopped working.");
#endif
}

/* A driver the player picked (a package, an Adreno's own driver) that
 * loses the device is dropped as a failed trial is (the activity's int
 * graphicsDriverLost(): the choice goes back to Automatic, 1 when it did),
 * so it does not come back at every start (BO-163). 0 elsewhere. */
static int gfx_lost_dropped(void)
{
    int r = 0;
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    if (act) {
        jclass cls = (*env)->GetObjectClass(env, act);
        jmethodID m = (*env)->GetMethodID(env, cls, "graphicsDriverLost", "()I");
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            m = NULL;
        }
        if (m) {
            r = (*env)->CallIntMethod(env, act, m) == 1;
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                r = 0;
            }
        }
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, act);
    }
    if (r)
        fprintf(stderr, "[PRESENT] the driver the player picked is dropped: the next start"
                " is on Automatic\n");
#endif
    return r;
}

/* The box; returns after the player's answer. The window's thread. */
static void gfx_lost_say(void)
{
    SDL_MessageBoxButtonData b[2];
    SDL_MessageBoxData d;
    char text[700];
    int n = 0, hit = 0;

    snprintf(text, sizeof text, "The graphics driver stopped working (%s), so the game cannot"
             " draw any more in this run.\n\nThe game is closed now. Your progress up to the"
             " game's last save is kept.%s", s_gfx_lost_why,
             gfx_lost_dropped()
                 ? "\n\nThis start used a driver you picked, so the next start goes back to"
                   " Automatic. You can pick it again under \"Takedawn: graphics driver\"."
                 : "");
    memset(b, 0, sizeof b);
    if (gfx_can_restart()) {
        b[n].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        b[n].buttonid = 1;
        b[n++].text = "Restart";
    }
    b[n].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (n ? 0 : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
    b[n].buttonid = 0;
    b[n++].text = "Quit";
    memset(&d, 0, sizeof d);
    d.flags = SDL_MESSAGEBOX_ERROR;
    d.title = "Graphics driver";
    d.message = text;
    d.numbuttons = n;
    d.buttons = b;
    fprintf(stderr, "[PRESENT] the graphics device is lost: telling the player and ending the run\n");
    if (SDL_ShowMessageBox(&d, &hit) == 0 && hit == 1)
        s_gfx_restart = 1;
    fprintf(stderr, "[PRESENT] the player chose %s\n", s_gfx_restart ? "Restart" : "Quit");
}

#if defined(__ANDROID__)
static int android_lifecycle(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND || ev->type == SDL_APP_DIDENTERBACKGROUND) {
        /* The present thread presents nothing while away (SDL's own thread
         * used to block in its pump then, and the presents with it). */
        __atomic_store_n(&s_app_away, 1, __ATOMIC_RELEASE);
        if (!s_bg_paused && !xbox_HostPaused()) {
            s_bg_paused = 1;
            xbox_HostPause(1);
        }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        /* An app in the background may be ended without a word (swiped out
         * of the recent apps): a setting changed in the overlay is written
         * now, not at the overlay's next frame or when it closes. */
        if (ev->type == SDL_APP_WILLENTERBACKGROUND) {
            extern void xbox_SettingsFlush(void);
            xbox_SettingsFlush();
        }
#endif
#if defined(XBOXRECOMP_HAVE_VULKAN)
        fbv_android_surface_gone();
#endif
        if (ev->type == SDL_APP_DIDENTERBACKGROUND)
            fprintf(stderr, "[PRESENT] the app is in the background: the title is held\n");
    } else if (ev->type == SDL_APP_DIDENTERFOREGROUND) {
        fprintf(stderr, "[PRESENT] the app is back in front\n");
        __atomic_store_n(&s_app_away, 0, __ATOMIC_RELEASE);
        pt_request(FW_CTL_DIRTY);
        if (s_bg_paused) {
            s_bg_paused = 0;
            xbox_HostPause(0);
        }
    }
    return 1;
}
#endif

/* The counters line (BO-210): what happens to the executor's frames on
 * their way to the glass, one line per 10 s. Printed every window with
 * diagnostic logging on; always (at most once a minute) when fewer frames
 * were presented or shown than flipped by more than 3 a second for a whole
 * window, with one 'back to normal' line after. The Deck's 45 fps at 60
 * executor flips (BO-210) had no line at all. The present thread only (the
 * main thread on the SDL renderer path). */
#define CNT_WINDOW_NS 10000000000ull
static struct {
    uint64_t t0, flips, pub, pick, drop, wf, wo, wt, presents;
    uint64_t picked, wake_frame, wake_other, wake_timeout, hand_max_ns;
    uint64_t low_since, last_alarm;
    int low, hidden;
#if defined(XBOXRECOMP_HAVE_VULKAN)
    FbvCounts v;
#endif
} s_cnt;

static void counters_tick(uint64_t now, uint64_t dropped, uint64_t presents, int visible)
{
    uint64_t flips = __atomic_load_n(&nv2a_pb_gpu_state()->flips, __ATOMIC_RELAXED);
    uint64_t pub = __atomic_load_n(&s_ready_seq, __ATOMIC_RELAXED);
    double dt, fr, got, nr = 0, sr = -1;
    char line[640], vk[320];
    int low, first = 0, limited = 0;
    uint64_t cyc = 0;
#if defined(XBOXRECOMP_HAVE_VULKAN)
    FbvCounts v;
    memset(&v, 0, sizeof v);
#endif
    /* Hidden, or the overlay open (the title paused or redrawn under it),
     * at any time in the window: no alarm for it. */
    if (!visible || s_overlay_open)
        s_cnt.hidden = 1;
    if (!s_cnt.t0 || now < s_cnt.t0) {
        s_cnt.t0 = now;
        first = 1;
    } else if (now - s_cnt.t0 < CNT_WINDOW_NS) {
        return;
    }
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk)
        fbv_counts(&v);   /* once a window: it clears the longest present */
#endif
    if (first)
        goto base;
    dt = (double)(now - s_cnt.t0) / 1e9;
    fr = (double)(uint32_t)((uint32_t)flips - (uint32_t)s_cnt.flips) / dt;
    vk[0] = 0;
    got = (double)(presents - s_cnt.presents) / dt;
#if defined(XBOXRECOMP_HAVE_VULKAN)
    if (s_vk) {
        nr = (double)(v.presents_new - s_cnt.v.presents_new) / dt;
        got = nr;
        if (v.have_wait) {
            sr = (double)(v.shown - s_cnt.v.shown) / dt;
            if (sr < got)
                got = sr;
        }
        snprintf(vk, sizeof vk, "; presents %llu new + %llu refills + %llu redraws (no skips),"
                 " shown %s%llu (+%llu refill repeats); longest present %.1f ms; refresh: window"
                 " system %.3f ms, present_wait steps %.3f ms; %s, depth %d, rows too deep %u,"
                 " refills held back %u",
                 (unsigned long long)(v.presents_new - s_cnt.v.presents_new),
                 (unsigned long long)(v.refills - s_cnt.v.refills),
                 (unsigned long long)(v.presents_redraw - s_cnt.v.presents_redraw),
                 v.have_wait ? "" : "(no present_wait) ",
                 (unsigned long long)(v.shown - s_cnt.v.shown),
                 (unsigned long long)(v.shown_rep - s_cnt.v.shown_rep), v.present_max_ns / 1e6,
                 v.wsi_cycle_ns / 1e6, v.period_ns / 1e6, v.smooth ? "smooth on" : "smooth off",
                 v.depth, v.deep, v.held);
    }
#endif
    (void)nr;
    snprintf(line, sizeof line, "flips %u (%.1f/s), handed over %llu, picked up %llu, replaced %llu"
             "%s; %s wakes: frame %llu, other %llu, timeout %llu; longest handover"
             " %.1f ms",
             (unsigned)((uint32_t)flips - (uint32_t)s_cnt.flips), fr,
             (unsigned long long)(pub - s_cnt.pub),
             (unsigned long long)(s_cnt.picked - s_cnt.pick),
             (unsigned long long)(dropped - s_cnt.drop), vk,
             s_layer ? "present-thread" : "main-thread",
             (unsigned long long)(s_cnt.wake_frame - s_cnt.wf),
             (unsigned long long)(s_cnt.wake_other - s_cnt.wo),
             (unsigned long long)(s_cnt.wake_timeout - s_cnt.wt), s_cnt.hand_max_ns / 1e6);
#if defined(XBOXRECOMP_HAVE_VULKAN)
    cyc = s_vk ? v.wsi_cycle_ns : 0;
#endif
    limited = flr_limited(fr, cyc);
    low = flr_lost(fr, got, cyc, s_cnt.hidden);
    if (low) {
        if (!s_cnt.low)
            s_cnt.low_since = s_cnt.t0;
        s_cnt.low = 1;
        if (!s_cnt.last_alarm || now - s_cnt.last_alarm >= 60000000000ull) {
            s_cnt.last_alarm = now;
            XR_NOTICE("[PACING] frames lost after the flip: %.1f a second reached the display of"
                      " %.1f flipped; counters %.0f s: %s\n", got, fr, dt, line);
        } else {
            fprintf(stderr, "[PACING] counters %.0f s (frames lost): %s\n", dt, line);
        }
    } else if (limited) {
        /* A frame limit (the window system's refresh slower than the
         * title's frame): fewer shown by design, said in the diagnostic
         * line only, and no 'back to normal' for it. */
        fprintf(stderr, "[PACING] counters %.0f s (frame limit: refresh %.3f ms): %s\n", dt,
                cyc / 1e6, line);
        s_cnt.low = 0;
    } else {
        if (s_cnt.low)
            XR_NOTICE("[PACING] frames after the flip back to normal: %.1f a second of %.1f"
                      " flipped (low for %.0f s); counters %.0f s: %s\n", got, fr,
                      (now - s_cnt.low_since) / 1e9 - dt, dt, line);
        else
            fprintf(stderr, "[PACING] counters %.0f s: %s\n", dt, line);
        s_cnt.low = 0;
    }
    s_cnt.t0 = now;
    s_cnt.hidden = !visible || s_overlay_open;
base:
    s_cnt.flips = flips;
    s_cnt.pub = pub;
    s_cnt.pick = s_cnt.picked;
    s_cnt.drop = dropped;
    s_cnt.wf = s_cnt.wake_frame;
    s_cnt.wo = s_cnt.wake_other;
    s_cnt.wt = s_cnt.wake_timeout;
    s_cnt.presents = presents;
    s_cnt.hand_max_ns = 0;
#if defined(XBOXRECOMP_HAVE_VULKAN)
    s_cnt.v = v;
#endif
}

/* RECOMP_PRESENT_STATS=1: a [PRESENT] line every 5 s, from the thread that
 * presents. */
static void stats_tick(const char *who)
{
    static uint64_t t_stat, cpu_stat, pres_stat, flips_stat;
    static const char *last_who;
    uint64_t t = now_ns(CLOCK_MONOTONIC);
    if (who != last_who) {               /* this thread's first call */
        last_who = who;
        t_stat = t;
        cpu_stat = now_ns(CLOCK_THREAD_CPUTIME_ID);
        pres_stat = s_presents;
        flips_stat = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
        return;
    }
    if (t - t_stat >= 5000000000ull) {
        uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
        double dt = (double)(t - t_stat) / 1e9;
        uint64_t fl = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
        fprintf(stderr, "[PRESENT] %.1f flips/s copied, %.1f presents/s,"
                " %s %.1f%% of a core, ack copy %.3f ms/flip\n",
                (double)(fl - flips_stat) / dt,
                (double)(s_presents - pres_stat) / dt, who,
                100.0 * (double)(cpu - cpu_stat) / (double)(t - t_stat),
                fl ? (double)__atomic_load_n(&s_copy_ns, __ATOMIC_RELAXED) / (double)fl / 1e6 : 0.0);
#if defined(XBOXRECOMP_HAVE_VULKAN)
        if (s_vk)
            fbv_timing_report("presents");
#endif
        t_stat = t;
        cpu_stat = cpu;
        pres_stat = s_presents;
        flips_stat = fl;
    }
}

/* ── The present thread (layer presenters: Vulkan, Metal) ─────────────────
 *
 * Waits for work on s_pub_cv (frame_wake.h): a new frame, or a request of
 * the main thread's (FW_CTL_DIRTY: expose, resize, a shot, the overlay
 * opening; FW_CTL_QUIT). While the overlay is open, or a notice is up, it
 * presents on every turn as the main loop did, paced by the present itself.
 * s_lock is held for the wait and the slot swap only, never across a present.
 * The window's flags come from the main thread (s_win_ok): no SDL window call
 * here but the presenters' own. */
static SDL_Window *s_pt_win;
static int         s_pt_vsync, s_pt_stats;
static pthread_t   s_pt_th;

static void *present_thread(void *arg)
{
    SDL_Window *win = s_pt_win;
    uint64_t shown_seq = 0;
    uint32_t shown_flip = 0;
    int dirty = 1;                       /* paint black before the first frame */
    int shoot = 0, burst = 0, new_frame = 0, retry_soon = 0, failed = 0;
    char req[sizeof s_shot_req] = "";
    char bprefix[sizeof s_burst_prefix] = "";
    unsigned bn = 0;

    (void)arg;
    (void)new_frame;
    xbox_log_thread_role("present", 0);
    for (;;) {
        uint64_t seq, timeout_ns;
        unsigned ctl;
        int w, ok = __atomic_load_n(&s_win_ok, __ATOMIC_ACQUIRE)
                    && !__atomic_load_n(&s_app_away, __ATOMIC_ACQUIRE);
        int filter = nv2a_host_opts()->present_filter;
        int pvideo_up = 0;
        {
            /* The video overlay (xbox_pvideo.h) has frames of its own: a
             * new one is shown over the last frame, as the display did at
             * scanout, whether the title flipped or not (an XMV movie on
             * the overlay flips once in its whole run); and once more when
             * it is taken off. */
            static uint32_t pvideo_shown;
            struct xbox_pvideo_frame pvf;
            if (xbox_PvideoFrame(&pvf)) {
                pvideo_up = 1;
                if (pvf.seq != pvideo_shown) {
                    pvideo_shown = pvf.seq;
                    dirty = 1;
                }
            } else if (pvideo_shown) {
                pvideo_shown = 0;
                dirty = 1;
            }
        }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        s_overlay_open = xbox_OverlayOpen();
        s_overlay_note = xbox_OverlayNotice();
        dirty |= s_overlay_open || s_overlay_note;
#endif
        /* How long to wait for work: not at all with a present to make;
         * an open overlay is redrawn every turn (the present paces it), a
         * notice at 60; a skipped Metal frame is tried again in 4 ms, a
         * failed present (no drawable, no surface yet) in 16. */
        if (!ok)
            timeout_ns = 100000000ull;   /* the main thread asks again when it is */
        else if (s_overlay_open)
            timeout_ns = s_pt_vsync ? 1000000ull : 8000000ull;
        else if (pvideo_up && !dirty)
            timeout_ns = 8000000ull;     /* the overlay's next frame */
        else if (retry_soon)
            timeout_ns = 4000000ull;
        else if (s_overlay_note || (dirty && failed))
            timeout_ns = 16000000ull;
        else if (dirty)
            timeout_ns = 0;
        else
            timeout_ns = 100000000ull;

        pthread_mutex_lock(&s_lock);
        w = fw_wait_work(&s_lock, &s_pub_cv, &s_ready_seq, shown_seq, &s_pt_ctl, timeout_ns);
        ctl = s_pt_ctl;
        s_pt_ctl = 0;
        if (s_shot_req[0]) {
            memcpy(req, s_shot_req, sizeof req);
            s_shot_req[0] = 0;
        }
        if (s_burst_n) {
            memcpy(bprefix, s_burst_prefix, sizeof bprefix);
            bn = s_burst_n;
            s_burst_n = 0;
        }
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            uint64_t h = now_ns(CLOCK_MONOTONIC) - s_pub_ns[seq & 7];
            s_front = s_ready;
            s_ready = t;
            s_cnt.picked++;
            if (h > s_cnt.hand_max_ns && h < 10000000000ull)
                s_cnt.hand_max_ns = h;
        }
        pthread_mutex_unlock(&s_lock);
        if (ctl & FW_CTL_QUIT)
            break;
        if (w == FW_WORK_FRAME)
            s_cnt.wake_frame++;
        else if (w == FW_WORK_CTL)
            s_cnt.wake_other++;
        else if (timeout_ns)
            s_cnt.wake_timeout++;
        if (ctl & FW_CTL_DIRTY)
            dirty = 1;
        if (w == FW_WORK_NONE)
            retry_soon = 0;              /* its time is up: try again below */

#if defined(XBOXRECOMP_HAVE_OVERLAY)
        /* The title is paused behind an open overlay, so no new frame comes
         * to take a pending RECOMP_WINDOW_SHOT: any pending one is taken now,
         * of what is shown (the overlay over the last frame). */
        if (s_overlay_open && shown_seq)
            shoot |= s_shot_n && shot_due(0xFFFFFFFFu);
#endif
        if (req[0]) {
            shoot = 1;                  /* of whatever the next present shows */
            dirty = 1;
        }
        if (s_trace_drops < 0) {
            const char *tr = recomp_env(RENV_PACING_TRACE);
            s_trace_drops = tr && !strcmp(tr, "all");
        }
        if (s_trace_drops && seq > shown_seq + 1 && shown_seq) {
            uint64_t now = now_ns(CLOCK_MONOTONIC);
            fprintf(stderr, "[PACING] window dropped %llu at t_us %llu: published"
                    " %.2f and %.2f ms ago, last pickup %.2f ms ago, last present"
                    " %.2f ms\n", (unsigned long long)(seq - shown_seq - 1),
                    (unsigned long long)(now / 1000u),
                    (now - s_pub_ns[(shown_seq + 1) & 7]) / 1e6,
                    (now - s_pub_ns[seq & 7]) / 1e6,
                    (now - s_last_pickup_ns) / 1e6, s_last_present_ns / 1e6);
        }
        if (seq != shown_seq) {
            /* The slot is already a GPU texture. */
            s_last_pickup_ns = now_ns(CLOCK_MONOTONIC);
            new_frame = 1;
            s_dropped += seq - shown_seq - 1;
            shown_flip = s_slot[s_front].flip;
            /* A shot still pending (no drawable yet) is taken of this frame,
             * under its own flip number, as for a dropped flip. */
            shoot |= s_shot_n && shot_due(shown_flip);
            burst = bn > 0 && !req[0];
            shoot |= burst;
            shown_seq = seq;
            __atomic_store_n(&s_shown_seq, seq, __ATOMIC_RELAXED);
            dirty = 1;
            retry_soon = 0;
        }

        if (dirty && ok && !retry_soon) {
            struct fb_slot *f = &s_slot[s_front];
            uint64_t t0 = now_ns(CLOCK_MONOTONIC);
            char path[512];
            int rc = 0;
            if (shoot && req[0])
                snprintf(path, sizeof path, "%s", req);
            else if (shoot && burst)
                snprintf(path, sizeof path, "%s%05u.bmp", bprefix, shown_flip);
            else if (shoot)
                shot_path(path, sizeof path, shown_flip);
#if defined(XBOXRECOMP_HAVE_VULKAN)
            if (s_vk)
                rc = fbv_present(win, s_front, shown_seq ? f->w : 0, shown_seq ? f->h : 0,
                                 filter, shoot ? path : NULL, shown_flip);
#endif
#if defined(__APPLE__)
            if (!s_vk) {
                /* A frame's own vblank goes with its first present only: a
                 * redraw (the overlay, an expose) shows it again. */
                uint64_t vbl = shown_seq && new_frame ? f->vbl_ns : 0;
                rc = fbm_present(win, s_front, shown_seq ? f->w : 0, shown_seq ? f->h : 0,
                                 filter, shoot ? path : NULL, shown_flip, vbl);
                if (rc == 2) {
                    /* Skipped (present timing: no drawable without a wait).
                     * The next frame replaces it; should none come (the
                     * title stopped on this picture), it is shown as a
                     * redraw. */
                    new_frame = 0;
                    retry_soon = 1;
                    rc = 0;
                } else if (rc) {
                    s_last_present_ns = now_ns(CLOCK_MONOTONIC) - t0;
                }
            }
#endif
            (void)filter;
            if (rc) {
                new_frame = 0;
                dirty = 0;
                failed = 0;
                if (shoot && burst && bn)
                    bn--;
                shoot = 0;
                burst = 0;
                req[0] = 0;
                s_presents++;
#if defined(__ANDROID__)
                {
                    /* The hot threads' work for this frame (android_perf.c). */
                    extern void xbox_AndroidPerfFrame(void);
                    xbox_AndroidPerfFrame();
                }
#endif
#if defined(XBOXRECOMP_HAVE_OVERLAY)
                xbox_OverlayFramePresented();
#endif
            } else if (!retry_soon) {
                /* No drawable (display asleep, window occluded), no surface
                 * yet, a lost device: tried again in a while. */
                failed = 1;
            }
            s_upload_ns += now_ns(CLOCK_MONOTONIC) - t0;
        }
        counters_tick(now_ns(CLOCK_MONOTONIC), s_dropped, s_presents,
                      __atomic_load_n(&s_win_ok, __ATOMIC_ACQUIRE));
        if (s_pt_stats)
            stats_tick("present thread");
    }
    return NULL;
}

static int present_thread_start(SDL_Window *win, int vsync, int stats)
{
    pthread_attr_t attr;
    int rc;
    s_pt_win = win;
    s_pt_vsync = vsync;
    s_pt_stats = stats;
    pthread_attr_init(&attr);
#if defined(__APPLE__)
    /* The main thread's class: the presents were made there. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&s_pt_th, &attr, present_thread, NULL);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

/* Before the presenter goes: the present thread returns (it is woken
 * whether or not a frame ever came). */
static void present_thread_stop(void)
{
    pt_request(FW_CTL_QUIT);
    pthread_join(s_pt_th, NULL);
}

/* ── the mouse cursor ─────────────────────────────────────────────────
 * Hidden in play, so that it does not sit over the picture; shown by a
 * mouse move or click, hidden again after CURSOR_IDLE_MS without one and at
 * once by a key or a pad input. The settings overlay is the mouse's: the
 * cursor is shown while it is open whatever else comes in, and hidden the
 * turn it closes. SDL_ShowCursor is the thread that pumps events', so all of
 * this runs in xbox_HostWindowMain's loop and never on the present thread;
 * the overlay's ImGui backend does not touch the cursor either
 * (ImGuiConfigFlags_NoMouseCursorChange, overlay_ui.cpp), or it would show
 * it on every frame of a notice or the perf panel. A touch is not a mouse:
 * SDL's synthetic touch-mouse events (Android, a Deck's screen) are
 * ignored, and the calls are harmless where there is no pointer. */
#define CURSOR_IDLE_MS 2500u

struct cursor_state {
    Uint32 last_move;       /* SDL_GetTicks() of the last mouse move or click */
    int shown;              /* what this code last asked SDL for */
    int want;               /* from the events of this turn: +1 show, -1 hide */
    int ov_was_open;
};

static void cursor_event(struct cursor_state *c, const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        if (ev->motion.which != SDL_TOUCH_MOUSEID && (ev->motion.xrel || ev->motion.yrel)) {
            c->last_move = SDL_GetTicks();
            c->want = 1;
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.which != SDL_TOUCH_MOUSEID) {
            c->last_move = SDL_GetTicks();
            c->want = 1;
        }
        break;
    case SDL_KEYDOWN:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_JOYBUTTONDOWN:
        c->want = -1;
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (ev->caxis.value > 8000 || ev->caxis.value < -8000)
            c->want = -1;
        break;
    default:
        break;
    }
}

static void cursor_set(struct cursor_state *c, int show, const char *why)
{
    if (show == c->shown)
        return;
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
    c->shown = show;
    XR_NOTICE("[PRESENT] cursor %s (%s)\n", show ? "shown" : "hidden", why);
}

/* Once per turn of the loop, after the events were read. */
static void cursor_tick(struct cursor_state *c, int ov_open)
{
    int want = c->want;

    c->want = 0;
    if (ov_open) {
        cursor_set(c, 1, "overlay open");
    } else if (c->ov_was_open) {
        cursor_set(c, 0, "overlay closed");
    } else if (want < 0) {
        cursor_set(c, 0, "key or pad input");
    } else if (want > 0) {
        cursor_set(c, 1, "mouse moved");
    } else if (c->shown && SDL_GetTicks() - c->last_move >= CURSOR_IDLE_MS) {
        cursor_set(c, 0, "mouse idle");
    }
    c->ov_was_open = ov_open;
    /* Whoever else asked SDL for the cursor (the window system on a focus
     * change, a driver): it stays as this code decided. */
    if (!c->shown && SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE)
        SDL_ShowCursor(SDL_DISABLE);
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    uint32_t tex_w = 0, tex_h = 0, tex_bpp = 0;
    uint64_t shown_seq = 0;      /* the SDL renderer path's (s_shown_seq) */
    uint64_t t_start, cpu_start;
    struct xbox_title_clock title_clock = {0};
    int title_stats = recomp_env_on(RENV_TITLE_STATS);
    int vsync = !recomp_env(RENV_PRESENT_VSYNC) || recomp_env_on(RENV_PRESENT_VSYNC);
    int stats = recomp_env_on(RENV_PRESENT_STATS);
    int scale = recomp_env(RENV_WINDOW_SCALE) ? atoi(recomp_env(RENV_WINDOW_SCALE)) : 2;
    int win_w, win_pos, win_pw, win_ph;
    int quit = 0, guest_done = 0, dirty = 0, shoot = 0, had_pads;
    double quit_after = recomp_env(RENV_WINDOW_QUIT_AFTER)
                      ? atof(recomp_env(RENV_WINDOW_QUIT_AFTER)) : 0.0;
    uint32_t shown_flip = 0;
    const struct nv2a_host_opts *eo = nv2a_host_opts();
    int filter = eo->present_filter;
    SDL_Rect dst;                /* integer: where the frame goes */
    int dst_ok = 0, out_w = 0, out_h = 0;

#if defined(__ANDROID__)
    /* A restart is the activity's to do (recomp_restart.h). */
    recomp_restart_set_hook(android_restart);
    recomp_quit_set_hook(android_quit);
#endif

    /* Ctrl-C and SIGTERM keep their default meaning (benches rely on it);
     * SDL would turn them into SDL_QUIT. Set before the headless return:
     * audio and input start SDL subsystems in headless runs too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(__ANDROID__) && defined(SDL_HINT_ANDROID_TRAP_BACK_BUTTON)
    /* The system Back must not finish the activity (the present loop would
     * then exit(0) mid-race): it arrives as SDLK_AC_BACK, which the
     * overlay takes as its toggle (GameActivity.onBackPressed sends it too). */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    if (recomp_env_on(RENV_HEADLESS)) {
#if defined(__APPLE__)
        /* No window, but the vblank can still follow the main display. */
        if (recomp_env(RENV_VBLANK_SYNC))
            fbm_vblank_link(NULL);
#endif
        return guest_main();
    }
    shot_parse();

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
#if defined(_WIN32) && defined(SDL_HINT_WINDOWS_DPI_AWARENESS)
    /* Per-monitor (v2) DPI awareness, so that Windows does not bitmap-stretch
     * the window by the desktop's scale factor (150 % on a 4K panel: a 1280x960
     * swapchain stretched to 1920x1440) and the swapchain is the real pixel
     * size. As SDL2's own documentation of the hint recommends it ("the
     * preferred DPI awareness level"); the default is left to the environment
     * (SDL_WINDOWS_DPI_AWARENESS=unaware restores the old behaviour).
     * SDL_HINT_WINDOWS_DPI_SCALING stays off: 1 SDL coordinate = 1 pixel, and
     * the starting size is scaled below. Must precede SDL_Init(VIDEO). */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
#if defined(__ANDROID__)
    SDL_AddEventWatch(android_lifecycle, NULL);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        XR_ERROR( "[PRESENT] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    if (scale < 1 || scale > 8)
        scale = 2;
    /* 640 wide, or 853 at present.aspect 16:9: the window starts the shape
     * the frame is shown at. */
    win_w = (int)nv2a_present_window_width(640, 480);
    /* RECOMP_DEBUG=window_display=<n>: open on display n (0 = main), e.g. a
     * 60 Hz screen next to a 120 Hz one for latency runs (BO-74). */
    {
        const char *e = recomp_env(RENV_WINDOW_DISPLAY);
        win_pos = e ? (int)SDL_WINDOWPOS_CENTERED_DISPLAY(atoi(e))
                    : (int)SDL_WINDOWPOS_CENTERED;
    }
    win_pw = win_w * scale;
    win_ph = 480 * scale;
#if defined(_WIN32) && defined(SDL_HINT_WINDOWS_DPI_AWARENESS)
    {
        /* DPI aware: SDL sizes are pixels, so a window meant to look as big
         * as it does on a 100 % desktop is the desktop's scale factor larger. */
        const char *aw = SDL_GetHint(SDL_HINT_WINDOWS_DPI_AWARENESS);
        const char *de = recomp_env(RENV_WINDOW_DISPLAY);
        float ddpi = 0.f;
        if (aw && (!strcmp(aw, "permonitorv2") || !strcmp(aw, "permonitor") || !strcmp(aw, "system"))
                && SDL_GetDisplayDPI(de ? atoi(de) : 0, &ddpi, NULL, NULL) == 0 && ddpi > 96.f) {
            win_pw = (int)((float)win_pw * ddpi / 96.f + 0.5f);
            win_ph = (int)((float)win_ph * ddpi / 96.f + 0.5f);
        }
    }
#endif
    {
        Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                      | (eo->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#if defined(__ANDROID__)
        /* An app's window is the screen; fullscreen is what makes SDL hide
         * the status and navigation bars (immersive mode). */
        wflags |= SDL_WINDOW_FULLSCREEN;
#endif
        const char *be = recomp_env(RENV_PB_BACKEND);
#if defined(XBOXRECOMP_HAVE_VULKAN)
        /* The Vulkan backend's frames stay on the GPU too: a Vulkan window
         * whose swapchain shows the slot images the backend copies into
         * (fb_present_vulkan.c), unless vk_present=readback. */
        const char *vp = recomp_env(RENV_VK_PRESENT);
        if (be && (!strcasecmp(be, "vulkan") || !strcasecmp(be, "vk") || !strcasecmp(be, "native"))
                && !(vp && !strcasecmp(vp, "readback"))) {
            char why[256] = "";
            win = NULL;
            if (fbv_load_library(why, sizeof why))
                win = SDL_CreateWindow(window_name(), win_pos, win_pos,
                                       win_pw, win_ph,
                                       wflags | SDL_WINDOW_VULKAN);
            if (!win && !why[0]) {
                snprintf(why, sizeof why, "%s", SDL_GetError());
            } else if (win) {
                fit_window(win);   /* before the swapchain takes the size */
                if (fbv_create(win, vsync, shots_wanted(), why, sizeof why))
                    s_layer = s_vk = 1;   /* the present thread paints black first */
            }
            if (!s_vk) {
                if (win) {
                    fbv_destroy();
                    SDL_DestroyWindow(win);
                }
                XR_ERROR( "[PRESENT] window up: Vulkan swapchain unavailable (%s);"
                        " SDL renderer, readback present\n", why);
#if defined(XBOXRECOMP_HAVE_OVERLAY)
                /* Said, not left to be found out: the overlay is drawn by
                 * the Vulkan (or Metal) presenter and by nothing else. */
                fprintf(stderr, "[OVERLAY] not available in this start: the window is not"
                        " presented through Vulkan, and the settings overlay is drawn there"
                        " only (Back+Start does nothing)\n");
#endif
                /* Not everybody has a terminal to read that on, and what
                 * follows is the CPU rasteriser at a few frames a second:
                 * say so on the screen (RECOMP_NO_GPU_NOTE=1 or, as before,
                 * RECOMP_ANDROID_NO_GPU_NOTE=1: not). On Android the box
                 * also offers the way back to the driver in the app and
                 * the driver page's file picker for a package. */
                if (!getenv("RECOMP_NO_GPU_NOTE") && !getenv("RECOMP_ANDROID_NO_GPU_NOTE")) {
                    char msg[640];
#if defined(__ANDROID__)
                    /* For a player: the reason is in the log (above). */
                    snprintf(msg, sizeof msg,
                             "This device's graphics driver can't run the game. The game"
                             " will run on the processor, which is far too slow to play.");
                    if (!android_offer_driver_reset(msg))
#else
                    /* BO-231 (GitHub #37): no loader, a loader older than
                     * Vulkan 1.3, or no device that has it. */
                    snprintf(msg, sizeof msg,
                             "Takedawn needs a Vulkan 1.3 graphics driver. Update the driver "
                             "from Intel, AMD or NVIDIA.\n\n%s\n\n"
                             "Until then the game draws on the CPU, which is far too slow "
                             "to play. The log says which device was tried.", why);
#endif
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING,
                                                 "Graphics driver", msg, NULL);
                }
            }
        }
#endif
#if defined(__APPLE__)
        /* The Metal backend's frames stay on the GPU: a CAMetalLayer window
         * the backend blits into (fb_present_metal.m), unless
         * metal_present=readback asks for the renderer and readback. */
        const char *mp = recomp_env(RENV_METAL_PRESENT);
        if (be && !strcasecmp(be, "metal") && !(mp && !strcasecmp(mp, "readback"))) {
            char why[256] = "";
            win = SDL_CreateWindow(window_name(), win_pos, win_pos, win_pw, win_ph,
                                   wflags | SDL_WINDOW_METAL);
            if (!win)
                snprintf(why, sizeof why, "%s", SDL_GetError());
            else {
                fit_window(win);
                if (fbm_create(win, vsync, shots_wanted(), why, sizeof why))
                    s_layer = 1;   /* the present thread paints black first */
            }
            if (!s_layer) {
                fbm_destroy();
                if (win)
                    SDL_DestroyWindow(win);
                XR_ERROR( "[PRESENT] window up: CAMetalLayer unavailable (%s);"
                        " SDL renderer, readback present\n", why);
#if defined(XBOXRECOMP_HAVE_OVERLAY)
                /* Said, not left to be found out: the overlay is drawn by
                 * the Vulkan (or Metal) presenter and by nothing else. */
                fprintf(stderr, "[OVERLAY] not available in this start: the window is not"
                        " presented through Vulkan, and the settings overlay is drawn there"
                        " only (Back+Start does nothing)\n");
#endif
            }
        }
#endif
        if (!s_layer) {
            win = SDL_CreateWindow(window_name(), win_pos, win_pos, win_pw, win_ph, wflags);
            fit_window(win);
            ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED
                                           | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0))
                      : NULL;
            if (!ren) {
                XR_ERROR( "[PRESENT] SDL window/renderer failed (%s); running headless\n",
                        SDL_GetError());
                if (win)
                    SDL_DestroyWindow(win);
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                return guest_main();
            }
        }
    }
    /* The output's shape, for whoever lays something out by the visible
     * picture before the first present (nv2a_present_visible_x). */
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        if (ww > 0 && wh > 0)
            nv2a_present_note_output((uint32_t)ww, (uint32_t)wh);
    }
    if (s_vk) {
        XR_NOTICE( "[PRESENT] window up: Vulkan swapchain (zero-copy), vsync %s;"
                " guest runs on a pthread\n", vsync ? "on" : "off");
    } else if (s_layer) {
        XR_NOTICE( "[PRESENT] window up: CAMetalLayer (zero-copy), vsync %s;"
                " guest runs on a pthread\n", vsync ? "on" : "off");
    } else {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            XR_NOTICE( "[PRESENT] window up: SDL %s renderer, vsync %s;"
                    " guest runs on a pthread\n", ri.name, vsync ? "on" : "off");
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }

#if defined(XBOXRECOMP_HAVE_OVERLAY)
    if (s_layer)
        xbox_OverlaySetWindow(win);
#endif
    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        XR_ERROR( "[PRESENT] SDL_RegisterEvents failed; running headless\n");
        close_present(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
    s_guest_main = guest_main;
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    /* The controllers come up here, on the thread that pumps events, before
     * any other thread runs: left to the first pad read, the OHCI thread
     * did SDL_InitSubSystem(GAMECONTROLLER) and SDL_NumJoysticks while this
     * thread pumped the joystick-added events that init posts. Under
     * sdl2-compat (Fedora/Bazzite: SDL2 is SDL3 underneath) both paths end
     * in its SDL_NumJoysticks, which frees and replaces one unlocked global
     * list (sdl2_compat.c, joystick_list; EventFilter3to2 calls it for
     * every joystick event), so the two threads freed it twice: "malloc():
     * unaligned tcache chunk detected" at startup, now and then (BO-76, six
     * crash reports on the Bazzite). A two-thread test of that shape there
     * aborted 9 times in 40; with the init first, 0 in 40. */
    xbox_InputInit();
    xbox_log_thread_role("sdl-main", 0);
    s_running = 1;
    if (s_layer) {
#if defined(__APPLE__)
        /* The layer's size, the display and the timing strategy before the
         * first present (then each turn of the loop). */
        if (!s_vk)
            fbm_main_tick(win);
#if defined(XBOXRECOMP_HAVE_VULKAN)
        if (s_vk) {
            int dw = 0, dh = 0;
            SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
            fbv_note_drawable(dw, dh);
        }
#endif
#endif
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        xbox_OverlayPlatformFrame(s_vk);   /* the ImGui context, on this thread */
#endif
        if (!present_thread_start(win, vsync, stats)) {
            XR_ERROR( "[PRESENT] cannot start the present thread; running headless\n");
            s_running = 0;
            xbox_InputSetMainPump(0);
            close_present(ren);
            SDL_DestroyWindow(win);
            return guest_main();
        }
    }
    if (!start_guest()) {
        XR_ERROR( "[PRESENT] cannot start the guest thread; running headless\n");
        if (s_layer)
            present_thread_stop();
        s_running = 0;
        xbox_InputSetMainPump(0);
        close_present(ren);
        SDL_DestroyWindow(win);
        return guest_main();
    }

    t_start = now_ns(CLOCK_MONOTONIC);
    cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);

#if defined(XBOXRECOMP_HAVE_VULKAN)
    {
        /* nv2a_vk_core.h, which this file does not include (Vulkan types). */
        extern void nvk_set_lost_handler(void (*fn)(const char *why));
        nvk_set_lost_handler(gfx_lost_handler);
    }
#endif
    /* The mouse cursor starts hidden (cursor_tick, above the main loop). */
    struct cursor_state cur = {0, 0, 0, 0};
    SDL_ShowCursor(SDL_DISABLE);
    while (!quit && !guest_done) {
        display_rate_poll(win);
        if (__atomic_load_n(&s_gfx_lost, __ATOMIC_ACQUIRE) == 1) {
            __atomic_store_n(&s_gfx_lost, 2, __ATOMIC_RELEASE);
            gfx_lost_say();
            quit = 1;           /* the ordinary way out, below */
            break;
        }
        SDL_Event ev;
        uint64_t seq;
        int have_ev, ov_open = 0, ov_note = 0;
        int stall_ms = pump_stall_ms();
        int flowing = fw_flowing(__atomic_load_n(&s_last_pub_ns, __ATOMIC_ACQUIRE),
                                 now_ns(CLOCK_MONOTONIC));
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        /* The overlay's Display page changes the filter while running. */
        filter = nv2a_host_opts()->present_filter;
        xbox_OverlayPoll();
        ov_open = s_layer && xbox_OverlayOpen();
        ov_note = s_layer && xbox_OverlayNotice();
#endif

        /* The mouse lock of the mouse + keyboard layer, on this thread as
         * SDL_SetRelativeMouseMode wants (no-op while the layer is off). */
        mkb_sdl_pump();

        /* One event loop for the window and the controllers: SDL_PumpEvents
         * (inside the wait) also updates the game controllers, which
         * xinput_device.c then only reads. No frame wakes this loop (the
         * present thread takes them, frame_wake.h): while frames come it
         * reads the events at least every FW_TICK_NS, as often as the loop
         * that took the frames did, and an open overlay's mouse as often as
         * it is redrawn. */
        if (stall_ms)
            pump_stall_sleep(stall_ms);
        if (s_layer) {
            have_ev = SDL_WaitEventTimeout(&ev, ov_open ? (vsync ? 1 : 8)
                                           : flowing ? (int)(FW_TICK_NS / 1000000ull)
                                           : ov_note ? 16 : 100);
        } else {
            /* The SDL renderer path takes and presents the frames here: the
             * wait is on the slot's condition variable, then SDL's events
             * are read without blocking. */
            pthread_mutex_lock(&s_lock);
            fw_wait_new(&s_lock, &s_pub_cv, &s_ready_seq, s_shown_seq,
                        flowing ? FW_TICK_NS : 10000000ull);
            pthread_mutex_unlock(&s_lock);
            have_ev = SDL_PollEvent(&ev);
        }
        if (have_ev) {
            do {
#if defined(XBOXRECOMP_HAVE_OVERLAY)
                if (s_layer)
                    xbox_OverlayEvent(&ev);
#endif
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. (An open overlay
                 * has the pad and keys neutral at the source,
                 * xbox_InputSetBlocked, so the key table keeps its state.) */
                xbox_InputSdlEvent(&ev);
                /* The mouse + keyboard layer's keys, buttons, motion and
                 * focus (RECOMP_MKB; off: nothing). */
                mkb_sdl_event(&ev);
                /* Controllers come and go, and take a port, here. */
                xbox_InputPadEvent(&ev);
                cursor_event(&cur, &ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event)
                    guest_done = 1;
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                    if (s_layer)
                        pt_request(FW_CTL_DIRTY);
                    else
                        dirty = 1;
                }
            } while (SDL_PollEvent(&ev));
        }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        ov_open = s_layer && xbox_OverlayOpen();    /* a key in this batch may have opened it */
#endif
        cursor_tick(&cur, ov_open);

        if (s_layer) {
#if defined(__APPLE__)
            /* The layer's size, the display, HDR headroom: AppKit's. */
            if (!s_vk)
                fbm_main_tick(win);
#if defined(XBOXRECOMP_HAVE_VULKAN)
            if (s_vk) {
                int dw = 0, dh = 0;
                SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
                fbv_note_drawable(dw, dh);
            }
#endif
#endif
#if defined(XBOXRECOMP_HAVE_OVERLAY)
            {
                /* ImGui's SDL half for the next overlay frame; the present
                 * thread is told when the overlay opens, closes or shows a
                 * notice, since no game frame may come to wake it (the title
                 * is paused behind it). */
                static int was_open, was_note;
                xbox_OverlayPlatformFrame(s_vk);
                ov_open = xbox_OverlayOpen();
                ov_note = xbox_OverlayNotice();
                if (ov_open != was_open || (ov_note && !was_note))
                    pt_request(FW_CTL_DIRTY);
                was_open = ov_open;
                was_note = ov_note;
            }
#endif
        } else {
            pthread_mutex_lock(&s_lock);
            seq = s_ready_seq;
            if (seq != s_shown_seq) {
                int t = s_front;
                uint64_t h = now_ns(CLOCK_MONOTONIC) - s_pub_ns[seq & 7];
                s_front = s_ready;
                s_ready = t;
                s_cnt.picked++;
                s_cnt.wake_frame++;
                if (h > s_cnt.hand_max_ns && h < 10000000000ull)
                    s_cnt.hand_max_ns = h;
            } else if (have_ev) {
                s_cnt.wake_other++;
            } else {
                s_cnt.wake_timeout++;
            }
            pthread_mutex_unlock(&s_lock);
            shown_seq = s_shown_seq;
            if (seq != shown_seq) {
                struct fb_slot *f = &s_slot[s_front];
                uint64_t t0 = now_ns(CLOCK_MONOTONIC);

                s_dropped += seq - shown_seq - 1;
                shown_flip = f->flip;
                shoot = s_shot_n && shot_due(shown_flip);
                shown_seq = seq;
                if (!tex || f->w != tex_w || f->h != tex_h || f->bpp != tex_bpp) {
                    if (tex)
                        SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, f->bpp == 4 ? SDL_PIXELFORMAT_RGB888
                                                             : SDL_PIXELFORMAT_RGB565,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            (int)f->w, (int)f->h);
                    tex_w = f->w;
                    tex_h = f->h;
                    tex_bpp = f->bpp;
                    if (filter == NV2A_PRESENT_INTEGER) {
                        dst_ok = 0;          /* new frame size: place it again */
                    } else {
                        /* At the present.aspect shape: the copy fills it. */
                        SDL_RenderSetLogicalSize(ren, (int)nv2a_present_width(f->w, f->h),
                                                 (int)f->h);
                        if (tex && filter == NV2A_PRESENT_LINEAR)
                            SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                    }
                }
                if (tex)
                    SDL_UpdateTexture(tex, NULL, f->px, (int)(f->w * f->bpp));
                s_upload_ns += now_ns(CLOCK_MONOTONIC) - t0;
                dirty = 1;
            }
            s_shown_seq = shown_seq;
            if (dirty && !s_layer) {
                dirty = 0;
                SDL_RenderClear(ren);
                if (tex && filter == NV2A_PRESENT_INTEGER) {
                    int ow = 0, oh = 0;
                    if (SDL_GetRendererOutputSize(ren, &ow, &oh) == 0
                            && (!dst_ok || ow != out_w || oh != out_h)) {
                        struct nv2a_rect r;
                        int whole = nv2a_present_rect(tex_w, tex_h, (uint32_t)ow, (uint32_t)oh,
                                                      NV2A_PRESENT_INTEGER, &r);
                        dst.x = r.x;
                        dst.y = r.y;
                        dst.w = r.w;
                        dst.h = r.h;
                        out_w = ow;
                        out_h = oh;
                        dst_ok = 1;
                        /* A frame that does not fit even at 1x is shrunk:
                         * linear there, nearest for whole multiples. Set on
                         * every placement: a new texture starts nearest. */
                        SDL_SetTextureScaleMode(tex, whole ? SDL_ScaleModeNearest
                                                           : SDL_ScaleModeLinear);
                    }
                    SDL_RenderCopy(ren, tex, NULL, &dst);
                } else if (tex)
                    SDL_RenderCopy(ren, tex, NULL, NULL);
                if (shoot) {
                    shoot = 0;
                    shot_save(ren, shown_flip);
                }
                SDL_RenderPresent(ren);
                s_presents++;
            }

        }
        {
            /* Said once per change: a hidden window presents nothing, and a
             * measured run that shows no on-glass times should say why. */
            static Uint32 last = 0xFFFFFFFFu;
            Uint32 fl = SDL_GetWindowFlags(win)
                        & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_INPUT_FOCUS);
#if defined(__APPLE__)
            if (s_layer && !s_vk && !fbm_visible(win))
                fl |= 0x80000000u;      /* covered by other windows, or on another Space */
#endif
            if (fl != last) {
                if (last != 0xFFFFFFFFu
                        || (fl & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | 0x80000000u)))
                    fprintf(stderr, "[PRESENT] window:%s%s%s%s\n",
                            fl & SDL_WINDOW_MINIMIZED ? " minimised" : "",
                            fl & SDL_WINDOW_HIDDEN ? " hidden" : "",
                            fl & 0x80000000u ? " not visible (covered, or on another Space)"
                                             : " visible",
                            fl & SDL_WINDOW_INPUT_FOCUS ? ", has the keyboard"
                                                        : ", without the keyboard");
                last = fl;
                /* The pacing watchdog decides nothing while it is not seen
                 * (BO-211). */
                xbox_PacingSetVisible(!(fl & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN
                                              | 0x80000000u)));
            }
            {
                /* The present thread's view of it: presents are made only
                 * while it is neither minimised nor hidden (nextDrawable
                 * then blocks for up to its 1 s timeout). Back: repainted. */
                int ok = !(fl & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
                if (ok != __atomic_load_n(&s_win_ok, __ATOMIC_ACQUIRE)) {
                    __atomic_store_n(&s_win_ok, ok, __ATOMIC_RELEASE);
                    if (ok && s_layer)
                        pt_request(FW_CTL_DIRTY);
                }
            }
        }
        if (win) {                     /* title bar: name, or 1 Hz stats */
            double fps;
            if (xbox_title_due(&title_clock, title_stats,
                               now_ns(CLOCK_MONOTONIC) / 1000000ull,
                               __atomic_load_n(&s_flips, __ATOMIC_RELAXED),
                               __atomic_load_n(&s_name_gen, __ATOMIC_ACQUIRE),
                               &fps)) {
                char tb[192];
                xbox_title_format(tb, sizeof tb, window_name(), title_stats, fps,
                                  __atomic_load_n(&s_have_draws, __ATOMIC_RELAXED),
                                  __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
                SDL_SetWindowTitle(win, tb);
            }
        }
        if (quit_after > 0 && (double)(now_ns(CLOCK_MONOTONIC) - t_start) / 1e9
                                  >= quit_after) {
            SDL_Event q;
            memset(&q, 0, sizeof q);
            q.type = SDL_QUIT;
            SDL_PushEvent(&q);
            quit_after = 0;
        }
        if (quit_after > 0 && (double)(now_ns(CLOCK_MONOTONIC) - t_start) / 1e9
                                  >= quit_after) {
            SDL_Event q;
            memset(&q, 0, sizeof q);
            q.type = SDL_QUIT;
            SDL_PushEvent(&q);
            quit_after = 0;
        }
        if (!s_layer) {
            counters_tick(now_ns(CLOCK_MONOTONIC), s_dropped, s_presents,
                          !(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)));
            if (stats)
                stats_tick("main thread");
        }
    }

    /* The present thread first: nothing presents once the loop has ended. */
    if (s_layer)
        present_thread_stop();
#if defined(XBOXRECOMP_HAVE_OVERLAY)
    /* Let the title go if the overlay had it paused, and write the settings. */
    if (s_layer)
        xbox_OverlayShutdown();
#endif
    /* No present may be mid-publish once the presenter goes. */
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    {
        uint64_t t = now_ns(CLOCK_MONOTONIC), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
        double dt = (double)(t - t_start) / 1e9;
        uint64_t fl = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
        fprintf(stderr, "[PRESENT] %s after %.1f s (%s): %llu flips copied (%.1f/s),"
                " %llu presents (%.1f/s), %llu dropped, main thread %.1f%% of a"
                " core, upload %.3f ms/frame, ack copy %.3f ms/flip\n",
                quit ? "window closed" : "guest returned", dt,
                s_vk ? "Vulkan swapchain" : s_layer ? "CAMetalLayer" : "SDL renderer",
                (unsigned long long)fl, dt > 0 ? (double)fl / dt : 0.0,
                (unsigned long long)s_presents, dt > 0 ? (double)s_presents / dt : 0.0,
                (unsigned long long)s_dropped,
                t > t_start ? 100.0 * (double)(cpu - cpu_start) / (double)(t - t_start) : 0.0,
                s_shown_seq ? (double)s_upload_ns / (double)s_shown_seq / 1e6 : 0.0,
                fl ? (double)__atomic_load_n(&s_copy_ns, __ATOMIC_RELAXED) / (double)fl / 1e6 : 0.0);
    }
    fflush(stdout);
    fflush(stderr);

    /* The pads first, while SDL is whole and on the thread that pumps it:
     * every motor off, the stop written out, the controllers closed
     * (input_core.c, xinput_device.c). atexit would do it too, but after
     * the video subsystem is gone and with the process a moment from its
     * end; from here on the title's threads cannot start a motor again. */
    had_pads = xbox_InputAnyOpen();
    xbox_InputShutdown();

    /* The title's threads are still running, and they must not be when the
     * GPU, the window and SDL go (BO-219, BO-206): a submit after the Vulkan
     * loader let go crashed the process (SIGSEGV in vkQueueSubmit, Steam
     * Deck), and the APU thread queued to an audio device SDL_Quit had
     * closed. In order: a save on its way finishes, the title's executor is
     * parked at its next frame (not when the guest has returned: nothing
     * flips), the helper threads stop. Each step is bounded and says so in
     * the log when its time is up. */
    recomp_quit_hook_run(RECOMP_QUIT_SETTLE);
    if (quit)
        recomp_quit_hook_run(RECOMP_QUIT_HOLD);
#if !defined(__ANDROID__)
    recomp_quit_hook_run(RECOMP_QUIT_HELPERS);
#else
    /* Android: the helpers (the APU frame thread) stop in
     * android_teardown_begin, after it has held the title. The GP doorbell
     * ack lives in that thread and the title spins on it (apu_core.c): with
     * the thread gone first, a DirectSound command in that window could keep
     * the title from reaching the hold. */
#endif

    /* The guest is still running when the window closes, and there is no way
     * to stop lifted code mid-flight. exit() ends every thread; atexit
     * handlers (ICALL feedback, profiles) still run.
     *
     * Window and renderer go first, on this thread as AppKit wants. SDL_Quit
     * is only safe once nothing else uses SDL: a run that had a controller
     * open keeps SDL up, as before, and exit() reclaims it. */
#if defined(__ANDROID__)
    {
        int vk = s_vk, destroy = android_teardown_begin(vk);
#endif
    if (tex)
        SDL_DestroyTexture(tex);
    close_present(ren);
#if defined(__ANDROID__)
        android_teardown_end(vk, destroy);
    }
#endif
    SDL_DestroyWindow(win);
    /* The main pump flag stays set: guest and OHCI threads keep running
     * until exit(), and must not start calling SDL_GameControllerUpdate
     * while SDL is torn down under them. */
    if (!had_pads)
        SDL_Quit();
    else
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (s_gfx_restart)
        gfx_restart();
#if defined(__ANDROID__)
    /* Every way out ends the process here (a guest that returned too): the
     * next start, or the restart page, waits for this process to be gone. */
    android_exit_now(s_gfx_restart ? "restart after a lost device" : "quit");
#endif
    if (quit)
        exit(0);
    return s_guest_rc;
}

#endif /* !_WIN32 */

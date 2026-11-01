/**
 * The SDL window's CAMetalLayer present (macOS): the zero-copy half of
 * fb_present_sdl.c, used when the Metal backend draws (RECOMP_PB_BACKEND=metal,
 * not RECOMP_DEBUG=metal_present=readback).
 *
 * The window has no SDL_Renderer then. fb_present_sdl.c keeps the window, the
 * event loop and the three-slot hand-off; this file owns what the slots point
 * at and how a slot reaches the screen:
 *
 *   device, queue  made here at window creation, before the guest starts; the
 *                  Metal backend adopts both at its lazy init
 *                  (xbox_FramebufferWindowMetal), so a slot texture takes the
 *                  backend's blit and a present pass on the same queue runs
 *                  after the blit it shows (commit order, tracked hazards).
 *   slots          three BGRA8 textures, frame-sized (render.scale's host
 *                  size). The backend blits its present target into the back
 *                  slot on the GPU; a guest-bytes frame is uploaded into it.
 *   present        on the main thread: the layer's next drawable, cleared to
 *                  black, the slot drawn into nv2a_present_rect's rectangle
 *                  with one full-screen triangle, nearest or linear as the
 *                  SDL path filters (and as D3D11's scaling blit does).
 *   shot           RECOMP_WINDOW_SHOT: the drawable read back after the pass,
 *                  framebufferOnly being off only when shots are asked for.
 *
 * Manual retain/release, like nv2a_pb_metal.m. Every entry point is wrapped in
 * an autorelease pool: the main loop and the ack thread have none of their own.
 */
#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAEDRMetadata.h>
#import <IOKit/IOKitLib.h>

#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend_common.h"
#include "platform/host_time.h"
#include "recomp_env.h"
#include "fb_present_metal.h"
#include "latency_trace.h"
#include "post_shaders.h"
#include "hdr_shaders.h"      /* BO-244: the HDR maths shared by both presenters */
#include "smaa.h"
#if defined(XBOXRECOMP_HAVE_OVERLAY)
#include "overlay.h"
#include "overlay_metal.h"
#endif
#include "host_time.h"
#include <mach/mach_time.h>

#include "kernel/kernel_pacing.h"
#include "kernel/present_cadence.h"
#include <os/lock.h>
#include <stdatomic.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#if __has_feature(objc_arc)
#error "fb_present_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

static id<MTLDevice>               s_dev;
/* BO-110 (RECOMP_DEBUG=hdr): an EDR layer, RGBA16Float in extended sRGB, so
 * the game's values pass 1.0 as they are (gamma-encoded, as the title blends
 * them); the slots and the post passes are half float too. Everything at or
 * below 1.0 is written as it is: SDR content keeps its look at the system's
 * paper white. fs_hdr_comp puts the scene's light (the frame where it
 * equals the backend's scene snapshot) through a shoulder towards the peak
 * and the HUD at its own white, live from the settings. */
static int                         s_hdr_win;    /* the layer is HDR now: HDRL_PQ or HDRL_EDR */
enum { HDRL_OFF, HDRL_EDR, HDRL_PQ };
static id<MTLRenderPipelineState>  s_hp_edr, s_hp_ovl, s_hp_pat;  /* BO-244 */
static id<MTLTexture>              s_ovl;        /* BO-244: the overlay before its encode (PQ) */
static float                       s_meta_peak;  /* BO-244: the peak the layer's HDR10 metadata says */
static float                       s_edid_peak;  /* BO-244: the display's EDID peak, nits (0 none) */
static float                       s_pat_r[4];   /* BO-244: where the last pattern went (drawable px) */
static int                         s_pat_kind = -1;
static float                       s_pat_ab[2];  /* BO-244: its two levels asked, nits */
static float                       s_pat_drawn[2]; /* and drawn (the disc's background at the display's level) */
static void hdr_comp_consts(float hp[12], float pk, float pw, float hw, float sh, float hl, int srgb,
                            float sf, float bl, int scene);
static float                       s_probe_now;  /* BO-227: the current headroom, for the probe */
static id<MTLRenderPipelineState>  s_hp_blit, s_hp_fxaa, s_hp_cas, s_hp_comp;   /* half-float variants */
static id<MTLTexture>              s_scene[3];   /* per slot: the scene snapshot (nv2a_pb_metal.m) */
static int                         s_scene_ok[3];
static id<MTLTexture>              s_hp_c;       /* the composite, frame size */
#define PP_FMT (s_hdr_win ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm)
static id<MTLCommandQueue>         s_queue;
static id<MTLRenderPipelineState>  s_pso;
static id<MTLRenderPipelineState>  s_pso_fxaa, s_pso_cas;   /* post passes */
static id<MTLTexture>              s_pp[2];                  /* their targets */
/* SMAA (smaa.h): made at the first use, its textures only while it is on. */
static id<MTLRenderPipelineState>  s_pso_sm[3];              /* edges, weights, blend */
static int                         s_smaa_state;             /* 0 untried, 1 ready, -1 failed */
static id<MTLTexture>              s_sm_tex[2];              /* edges RG8, weights RGBA8 */
static id<MTLTexture>              s_sm_lut[2];              /* area RG8, search R8 */
static id<MTLSamplerState>         s_smp[2];   /* nearest, linear */
static SDL_MetalView               s_view;
static CAMetalLayer               *s_layer;
static int                         s_unified;
static id<MTLTexture>              s_slot[3];
/* The last present pass that sampled each slot: an upload (a CPU write) into
 * a slot waits for it; a GPU blit needs no wait (tracked hazards). Written by
 * the main thread while the slot is front, read by the ack thread once the
 * hand-off's lock has made it back. */
static id<MTLCommandBuffer>        s_slot_cb[3];
static uint32_t                   *s_conv;     /* R5G6B5 -> BGRA8 rows */
static size_t                      s_conv_cap;

/* ---- the display's clock ------------------------------------------------
 *
 * RECOMP_VBLANK_SYNC=display: a CVDisplayLink on the window's display
 * reports every refresh to the vblank timer (kernel_pacing.c), which then
 * keeps the emulated vblank in phase with the display. RECOMP_TRACE=
 * pacing=all: each drawable's presentedTime, the time it reached the screen,
 * is logged as a host present. Both are mach_absolute_time stamps, moved
 * onto xbox_HostNowNs's clock by the offset between the two read now. */
static CVDisplayLinkRef s_link;
static int s_trace_presented;

static uint64_t mach_to_host_ns(uint64_t mach_ns)
{
    static mach_timebase_info_data_t tb;
    uint64_t host = xbox_HostNowNs(), mnow;
    if (!tb.denom)
        mach_timebase_info(&tb);
    mnow = mach_absolute_time() * tb.numer / tb.denom;
    return mnow >= mach_ns ? host - (mnow - mach_ns) : host + (mach_ns - mnow);
}

static uint64_t mach_ticks_ns(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *out, CVOptionFlags in_flags,
                        CVOptionFlags *out_flags, void *ctx)
{
    uint64_t period = 0;
    (void)link; (void)in_flags; (void)out_flags; (void)ctx;
    /* The output stamp: the refresh the next frame will be shown at, on
     * the display's grid (inNow is only when the callback ran). Its period
     * from the stamp's video time scale. */
    (void)now;
    if ((out->flags & kCVTimeStampVideoRefreshPeriodValid) && out->videoTimeScale)
        period = (uint64_t)out->videoRefreshPeriod * 1000000000ull
                 / (uint64_t)out->videoTimeScale;
    if (out->flags & kCVTimeStampHostTimeValid)
        xbox_VblankDisplayRefresh(mach_to_host_ns(mach_ticks_ns(out->hostTime)), period);
    return kCVReturnSuccess;
}

/* The display the window is on (its NSScreen's number), else the main one
 * (win NULL: a headless run, for measuring the lock without a window). */
void fbm_vblank_link(SDL_Window *win)
{
    CGDirectDisplayID id = CGMainDisplayID();
    SDL_SysWMinfo wm;
    if (s_link)
        return;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA) {
        NSScreen *scr = [wm.info.cocoa.window screen];
        NSNumber *n = scr ? [[scr deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
        if (n)
            id = (CGDirectDisplayID)[n unsignedIntValue];
    }
    if (CVDisplayLinkCreateWithCGDisplay(id, &s_link) != kCVReturnSuccess) {
        s_link = NULL;
        fprintf(stderr, "[PRESENT] vblank sync: no CVDisplayLink; the vblank keeps"
                " its own 60 Hz timer\n");
        return;
    }
    CVDisplayLinkSetOutputCallback(s_link, link_cb, NULL);
    CVDisplayLinkStart(s_link);
    fprintf(stderr, "[PRESENT] vblank sync: the emulated vblank follows display %u"
            " (CVDisplayLink, RECOMP_VBLANK_SYNC=display)\n", (unsigned)id);
}
#pragma clang diagnostic pop

/* ---- present timing (present_cadence.h, BO-115) ----------------------------
 *
 * display.present_timing = legacy: the layer has two drawables, a frame is
 * presented as soon as it is picked up (presentDrawable:), and nextDrawable
 * waits when both drawables are in use. Measured in a window on a 120 Hz
 * display that wait is a whole frame once it starts (a drawable is free
 * again only when the next one replaces it on the glass), and the frame is
 * on the glass about three frames after its vblank.
 *
 * auto: three drawables and never a wait: a frame is presented only while at
 * most one earlier present has not reached the glass yet, else it is skipped
 * (counted; the title's timing is not touched either way). Each frame is
 * aimed with presentDrawable:atTime: by the pacer: on a fixed-rate display
 * at one refresh of the display's grid, which the on-glass times define and
 * the emulated vblank is locked to; on a variable-refresh display at the
 * frame's vblank plus a fixed lead, the vblank on its own 60.000 Hz timer.
 * The display is looked at again twice a second (the window moved, the mode
 * changed, the setting changed).
 *
 * Every drawable's presentedTime is recorded in both modes: the overlay's
 * graph and RECOMP_TRACE=glass show what reached the glass. */
static pc_pacer        s_pacer;
void xbox_PacingResetHook(void (*fn)(void));
static pc_display      s_disp;
static int             s_strategy = -1;         /* PC_*; -1 before the first look */
static os_unfair_lock  s_pc_lock = OS_UNFAIR_LOCK_INIT;
static atomic_int      s_inflight;              /* presents not on the glass yet */
static atomic_uint     s_skips;
static char            s_disp_name[48];
static int             s_disp_fullscreen;
static uint64_t        s_last_commit_ns;        /* present thread */
static unsigned        s_no_drawable;
static FILE           *s_glass_f;
static int             s_glass_tried;

static double env_ms(recomp_env_id id, double def, double lo, double hi)
{
    const char *e = recomp_env(id);
    double v = e && *e ? atof(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

/* A time on xbox_HostNowNs's clock as presentDrawable:atTime: wants it:
 * seconds of mach_absolute_time. */
static CFTimeInterval host_ns_to_mach_s(uint64_t host_ns)
{
    uint64_t host = xbox_HostNowNs(), mnow = mach_ticks_ns(mach_absolute_time());
    return host_ns >= host ? (double)(mnow + (host_ns - host)) / 1e9
                           : (double)(mnow - (host - host_ns)) / 1e9;
}

/* The display the window is on: its rate, and whether it is variable. */
static void display_probe(SDL_Window *win, pc_display *d, char *name, size_t namen, int *fs)
{
    NSScreen *scr = nil;
    NSNumber *num;
    CGDirectDisplayID id;
    CGDisplayModeRef m;
    double mode_hz = 0, mn = 0, mx = 0, fps = 0;
    SDL_SysWMinfo wm;

    memset(d, 0, sizeof *d);
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
        scr = [wm.info.cocoa.window screen];
    if (!scr)
        scr = [NSScreen mainScreen];
    num = scr ? [[scr deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
    id = num ? (CGDirectDisplayID)[num unsignedIntValue] : CGMainDisplayID();
    m = CGDisplayCopyDisplayMode(id);
    if (m) {
        mode_hz = CGDisplayModeGetRefreshRate(m);
        CGDisplayModeRelease(m);
    }
    if (scr) {
        if (@available(macOS 12.0, *)) {
            mn = scr.minimumRefreshInterval;
            mx = scr.maximumRefreshInterval;
            fps = (double)scr.maximumFramesPerSecond;
        }
    }
    d->builtin = CGDisplayIsBuiltin(id) ? 1 : 0;
    d->refresh_hz = mn > 0 ? 1.0 / mn : mode_hz > 0 ? mode_hz : fps;
    d->min_hz = mx > 0 ? 1.0 / mx : d->refresh_hz;
    /* A variable display states a range of refresh intervals (an external
     * adaptive-sync display with the option on, a ProMotion panel). */
    d->vrr = mn > 0 && mx > mn * 1.02;
    if (name && namen) {
        const char *n = NULL;
        if (scr) {
            if (@available(macOS 10.15, *))
                n = [[scr localizedName] UTF8String];
        }
        snprintf(name, namen, "%s", n ? n : "display");
    }
    if (fs)
        *fs = win && (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

/* 1 while some part of the window can be seen (not covered by other
 * windows, not on another Space): only then does a drawable reach the glass
 * and report when. */
int fbm_visible(SDL_Window *win)
{
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
        return ([wm.info.cocoa.window occlusionState] & NSWindowOcclusionStateVisible) != 0;
    return 1;
}

static void timing_status(void)
{
    pc_status st;
    memset(&st, 0, sizeof st);
    os_unfair_lock_lock(&s_pc_lock);
    st.display = s_disp;
    st.strategy = s_strategy;
    st.depth = s_pacer.depth;
    st.phase_ms = s_pacer.phase_ns / 1e6;
    st.lead_ms = s_pacer.lead_ns / 1e6;
    st.slips = s_pacer.slips_all;
    st.lates = s_pacer.lates_all;
    os_unfair_lock_unlock(&s_pc_lock);
    st.measured = 1;
    st.skips = atomic_load(&s_skips);
    snprintf(st.source, sizeof st.source, "Metal presentedTime");
    xbox_PresentCadenceStatus(&st);
}

/* The pacing watchdog's reset (BO-211), on the executor: the depth and the
 * lead back to the setting's, the grid relearnt. */
static void timing_pacing_reset(void)
{
    os_unfair_lock_lock(&s_pc_lock);
    pc_pacer_relearn(&s_pacer);
    os_unfair_lock_unlock(&s_pc_lock);
}

/* Twice a second, on the main thread (fbm_main_tick): the display, the
 * setting, the strategy; what follows from a change. */
static void timing_poll(SDL_Window *win)
{
    static int hooked;
    if (!hooked) {
        hooked = 1;
        xbox_PacingResetHook(timing_pacing_reset);
    }
    pc_display d;
    char name[sizeof s_disp_name];
    int fs = 0, strategy, changed;

    {
        /* Twice a second (it was every 30th present). */
        uint64_t now = xbox_HostNowNs();
        static uint64_t next;
        if (s_strategy >= 0 && now < next)
            return;
        next = now + 500000000ull;
    }
    display_probe(win, &d, name, sizeof name, &fs);
    {
        /* auto unless the setting says otherwise: on macOS legacy's two
         * drawables showed every other frame on some displays (BO-239). */
        const char *t = recomp_env(RENV_PRESENT_TIMING);
        strategy = pc_pick(&d, t && *t ? pc_setting(t) : PC_SET_AUTO);
    }
    changed = strategy != s_strategy || d.vrr != s_disp.vrr
              || (int)(d.refresh_hz * 100) != (int)(s_disp.refresh_hz * 100)
              || strcmp(name, s_disp_name) || fs != s_disp_fullscreen;
    if (!changed) {
        timing_status();
        return;
    }
    os_unfair_lock_lock(&s_pc_lock);
    s_disp = d;
    s_strategy = strategy;
    pc_pacer_init(&s_pacer, strategy, &d,
                  (uint64_t)(env_ms(RENV_PRESENT_PHASE, 5.0, 0.0, 15.0) * 1e6),
                  (uint64_t)(env_ms(RENV_PRESENT_LEAD, 12.0, 0.0, 33.0) * 1e6),
                  (int)env_ms(RENV_PRESENT_DEPTH, 1.0, 0.0, PC_DEPTH_MAX));
    pc_pacer_set_handover(&s_pacer, (uint64_t)(env_ms(RENV_PRESENT_HANDOVER,
                                                      PC_HANDOVER_NS / 1e6, 0.5, 16.0) * 1e6));
    os_unfair_lock_unlock(&s_pc_lock);
    snprintf(s_disp_name, sizeof s_disp_name, "%s", name);
    s_disp_fullscreen = fs;
    {
        const char *e = recomp_env(RENV_PRESENT_DRAWABLES);
        int n = strategy != PC_LEGACY ? 3 : (e && atoi(e) == 3) ? 3 : 2;
        if ((int)s_layer.maximumDrawableCount != n)
            s_layer.maximumDrawableCount = n;
    }
    /* The vblank follows the display only where there is a grid to follow;
     * the on-glass times are its clock then, not a CVDisplayLink. */
    if (strategy == PC_LOCK && s_link) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
#pragma clang diagnostic pop
        s_link = NULL;
    }
    if (strategy != PC_LEGACY || !s_link)
        xbox_VblankDisplayLockAuto(strategy == PC_LOCK);
    fprintf(stderr, "[PRESENT] timing: %s on \"%s\" (%s%.2f Hz%s, %s): ",
            pc_strategy_name(strategy), s_disp_name, d.builtin ? "built-in, " : "",
            d.refresh_hz, d.vrr ? ", variable refresh" : ", fixed",
            fs ? "fullscreen" : "windowed");
    if (d.vrr)
        fprintf(stderr, "down to %.1f Hz; ", d.min_hz);
    switch (strategy) {
    case PC_LOCK:
        fprintf(stderr, "the vblank %.1f ms before a display refresh, each frame"
                " shown %d refresh(es) after it, handed over %.1f ms after the refresh"
                " before its own, 3 drawables\n",
                s_pacer.phase_ns / 1e6, s_pacer.depth,
                (s_pacer.handover_ns < s_pacer.period_ns / 2 ? s_pacer.handover_ns
                                                             : s_pacer.period_ns / 2) / 1e6);
        break;
    case PC_TIMED:
    case PC_TIMED_ODD:
        fprintf(stderr, "the vblank on its own 60 Hz timer, each frame presented"
                " %.1f ms after it, 3 drawables%s\n", s_pacer.lead_ns / 1e6,
                strategy == PC_TIMED_ODD ? "; this rate is not a multiple of 60:"
                " frames cannot all be shown for the same time" : "");
        break;
    default:
        fprintf(stderr, "present when ready, %d drawables\n",
                (int)s_layer.maximumDrawableCount);
        break;
    }
    timing_status();
}

/* RECOMP_DEBUG=glass_measure=0: no on-glass handler in legacy (an A/B of
 * what the measurement itself costs). */
static int glass_measure_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_GLASS_MEASURE);
        on = !(e && *e == '0');
    }
    return on;
}

/* RECOMP_TRACE=glass=<file>. */
static FILE *glass_file(void)
{
    if (!s_glass_tried) {
        const char *path = recomp_env(RENV_GLASS_TRACE);
        s_glass_tried = 1;
        if (path && *path && strcmp(path, "1")) {
            s_glass_f = fopen(path, "w");
            if (s_glass_f)
                fprintf(s_glass_f, "flip,vblank_ns,commit_ns,at_ns,target_ns,glass_ns,late,skipped,"
                        "steps,strategy,depth,refresh_hz,vrr\n");
            else
                fprintf(stderr, "[PRESENT] RECOMP_TRACE=glass: cannot write %s\n", path);
        }
    }
    return s_glass_f;
}

/* One presented (or skipped) game frame: the ring, the pacer, the vblank's
 * lock, the trace. Any thread. */
static void glass_note(uint32_t flip, uint64_t vblank_ns, uint64_t commit_ns,
                       const pc_aim *aim, uint64_t glass_ns, int skipped)
{
    pc_sample smp;
    uint64_t report = 0, period = 0;
    int strategy, depth, raised = 0;
    double hz;
    int vrr;
    FILE *f;

    memset(&smp, 0, sizeof smp);
    smp.flip = flip;
    smp.vblank_ns = vblank_ns;
    smp.glass_ns = glass_ns;
    smp.skipped = skipped ? 1 : 0;
    smp.steps = (uint8_t)xbox_PresentCadenceStepsOf(flip);
    xbox_PresentCadenceNote(&smp);
    os_unfair_lock_lock(&s_pc_lock);
    if (!skipped && glass_ns) {
        raised = pc_pacer_glass(&s_pacer, aim, glass_ns);
        report = pc_pacer_lock_report(&s_pacer, glass_ns);
        period = s_pacer.period_ns;
    }
    strategy = s_strategy;
    depth = s_pacer.depth;
    hz = s_disp.refresh_hz;
    vrr = s_disp.vrr;
    os_unfair_lock_unlock(&s_pc_lock);
    if (report)
        xbox_VblankDisplayRefresh(report, period);
    if (raised == 2)
        fprintf(stderr, "[PRESENT] timing: frames reached the glass a refresh early; now"
                " handed over half a refresh before their refresh\n");
    else if (raised > 0)
        fprintf(stderr, "[PRESENT] timing: frames reached the glass late; now %s\n",
                strategy == PC_LOCK ? "one refresh more between a frame's vblank and its refresh"
                                    : "2 ms more between a frame's vblank and its present");
    else if (raised < 0)
        fprintf(stderr, "[PRESENT] timing: %d s of frames on time; now %s\n",
                PC_DECAY_FRAMES / 60,
                strategy == PC_LOCK ? "one refresh less between a frame's vblank and its refresh"
                                    : "2 ms less between a frame's vblank and its present");
    if ((f = glass_file()) != NULL)
        fprintf(f, "%u,%llu,%llu,%llu,%llu,%llu,%d,%d,%u,%d,%d,%.3f,%d\n", flip,
                (unsigned long long)vblank_ns, (unsigned long long)commit_ns,
                (unsigned long long)aim->at_ns, (unsigned long long)aim->target_ns,
                (unsigned long long)glass_ns, aim->late, skipped, smp.steps, strategy, depth,
                hz, vrr);
}

static const char s_blit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VO { float4 p [[position]]; float2 t; };\n"
    "vertex VO vs_blit(uint i [[vertex_id]]) {\n"
    "    VO o;\n"
    "    o.t = float2((i << 1) & 2, i & 2);\n"
    "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    /* uv: the shown part of the slot (nv2a_present_layout), offset and size. */
    "fragment float4 fs_blit(VO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]], constant float4 &uv [[buffer(0)]],\n"
    "                        constant float &dither [[buffer(1)]]) {\n"
    "    float3 c = t.sample(s, uv.xy + i.t * uv.zw).rgb;\n"
    /* RECOMP_GFX_DITHER: interleaved gradient noise (Jimenez, 2014) at the
     * window pixel, scaled to just under +-0.5 LSB of the 8-bit target, so
     * a texel that is already an exact 8-bit value (1:1 text, flat colour)
     * rounds back to itself and only a filtered, in-between value moves. */
    "    if (dither > 0.0) {\n"
    "        float n = fract(52.9829189 * fract(dot(floor(i.p.xy), float2(0.06711056, 0.00583715))));\n"
    "        c += (n - 0.5) * (0.98 / 255.0);\n"
    "    }\n"
    "    return float4(c, 1);\n"
    "}\n"
    /* BO-110/BO-244: the HDR composite (PQ, or the extended-sRGB fallback),
     * the overlay's encode and the patterns: video/hdr_shaders.h, the maths
     * the Vulkan flip pass runs. */
    HDR_COMP_MSL
    /* The post passes (post_shaders.h): the frame at its own size, one pixel
     * to one texel, so a bilinear fetch at an offset of whole texels is that
     * texel. */
    "#define FETCH(p) t.sample(s, (p)).rgb\n"
    "#define RETURN(c) return float4((c), 1.0)\n"
    "fragment float4 fs_fxaa(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "    float2 uv = vo.t;\n"
    "    float2 rcp = 1.0 / float2(t.get_width(), t.get_height());\n"
    POST_FXAA_BODY
    "}\n"
    "fragment float4 fs_cas(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                       sampler s [[sampler(0)]], constant float &sharp [[buffer(0)]]) {\n"
    "    float2 uv = vo.t;\n"
    "    float2 rcp = 1.0 / float2(t.get_width(), t.get_height());\n"
    POST_CAS_BODY
    "}\n";

/* BO-244: the display's peak luminance from its EDID's CTA-861 HDR static
 * metadata block (max luminance code value v: 50 x 2^(v/32) nits), as the
 * Linux presenter reads it from DRM. On Apple silicon the EDID of a display
 * on a USB-C/DisplayPort/Thunderbolt port is the "EDID" property of its
 * IOPortTransportStateDisplayPort entry; the one whose vendor and product
 * match the screen's (CGDisplayVendorNumber / ModelNumber) is taken. 0 when
 * there is none (the built-in panel, HDMI, an EDID without the block). */
static float edid_peak(const uint8_t *e, size_t n)
{
    for (size_t b = 128; b + 128 <= n; b += 128) {
        const uint8_t *x = e + b;
        size_t end = x[2];
        if (x[0] != 0x02 || end < 4 || end > 127)
            continue;
        for (size_t i = 4; i < end;) {
            unsigned tag = x[i] >> 5, len = x[i] & 31u;
            if (tag == 7 && len >= 4 && x[i + 1] == 6 && x[i + 4])
                return 50.0f * powf(2.0f, (float)x[i + 4] / 32.0f);
            i += 1 + len;
        }
    }
    return 0.0f;
}
static float screen_edid_peak(NSScreen *scr)
{
    NSNumber *sn = [[scr deviceDescription] objectForKey:@"NSScreenNumber"];
    CGDirectDisplayID did = sn ? (CGDirectDisplayID)[sn unsignedIntValue] : CGMainDisplayID();
    uint32_t ven = CGDisplayVendorNumber(did), mod = CGDisplayModelNumber(did);
    io_iterator_t it = 0;
    io_service_t sv;
    float pk = 0.0f;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPortTransportStateDisplayPort"),
                                     &it) != KERN_SUCCESS)
        return 0.0f;
    while (pk <= 0.0f && (sv = IOIteratorNext(it)) != 0) {
        CFTypeRef d = IORegistryEntryCreateCFProperty(sv, CFSTR("EDID"), kCFAllocatorDefault, 0);
        if (d && CFGetTypeID(d) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)d) >= 256) {
            const uint8_t *e = CFDataGetBytePtr((CFDataRef)d);
            uint32_t ev = ((uint32_t)e[8] << 8) | e[9], em = e[10] | ((uint32_t)e[11] << 8);
            if (ev == ven && em == mod)
                pk = edid_peak(e, (size_t)CFDataGetLength((CFDataRef)d));
        }
        if (d)
            CFRelease(d);
        IOObjectRelease(sv);
    }
    IOObjectRelease(it);
    return pk;
}

/* BO-110: the window's display, for the HDR settings: how far past its SDR
 * white it can go at most and now (EDR headroom; 1.0 none). BO-244: and its
 * EDID's peak in nits, logged beside the headroom and used for the default
 * peak only while no headroom is known (nv2a_host_hdr_levels). HDR10 maps
 * 100 nits to the system's SDR white, so headroom x 100 is nits: the
 * PG32UCDP in its True Black 400 mode has potential 5.076 = its EDID's
 * 507.6 nits / 100; in Peak 1000 macOS says 13.166 while the EDID stays. */
static NSScreen *s_edid_scr;
static float     s_edid_pot;
static void hdr_display(SDL_Window *win)
{
    NSScreen *scr = [NSScreen mainScreen];
    SDL_SysWMinfo wm;
    float pot = 1.0f, now = 1.0f;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA
            && wm.info.cocoa.window.screen)
        scr = wm.info.cocoa.window.screen;
    if (@available(macOS 10.15, *)) {
        pot = (float)scr.maximumPotentialExtendedDynamicRangeColorComponentValue;
        now = (float)scr.maximumExtendedDynamicRangeColorComponentValue;
    }
    /* Again when the window moves to another screen or the screen's
     * potential changes (a display mode or the monitor's HDR mode): this
     * runs every main-loop tick, so the headroom is re-read all the time. */
    if (scr != s_edid_scr || pot != s_edid_pot) {
        s_edid_scr = scr;
        s_edid_pot = pot;
        s_edid_peak = screen_edid_peak(scr);
        fprintf(stderr, "[HDR] display %s: EDID peak %.1f nits, EDR headroom potential %.3fx"
                " (x 100 nits: %.1f)\n", [[scr localizedName] UTF8String], s_edid_peak, pot,
                pot * 100.0f);
        nv2a_host_hdr_display_peak(s_edid_peak);
    }
    s_probe_now = now;
    nv2a_host_hdr_display(pot, now);
}

/* BO-110/BO-244: the layer's HDR mode, live (the slots and the post targets
 * follow the format at their next use):
 *   HDRL_PQ   the Deck's output (BO-244): RGBA16Float holding ST 2084 PQ code
 *             values in BT.2020 (kCGColorSpaceITUR_2100_PQ), HDR10 metadata
 *             (CAEDRMetadata: the display's peak; HDR10 maps 100 nits to the
 *             system's SDR white), the same composite maths as Vulkan's flip
 *             pass, in nits. The default.
 *   HDRL_EDR  the fallback (BO-110/BO-227's): RGBA16Float in extended sRGB,
 *             1.0 = the system's SDR white = 100 nits. RECOMP_HDR_LAYER=edr.
 *   HDRL_OFF  BGRA8 sRGB, as before. */
static int hdr_layer_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_HDR_LAYER");
        m = e && !strcmp(e, "edr") ? HDRL_EDR : HDRL_PQ;
    }
    return m;
}
/* BO-244: macOS maps a PQ layer's nits relative to its SDR white, not to
 * the panel: the HDR10 metadata's opticalOutputScale says how many of the
 * content's nits make EDR 1.0 (the system's SDR white), 100 by default, and
 * the compositor puts that at the brightness slider's level. On the
 * PG32UCDP in Peak 1000 mode with the slider at the top (CoreBrightness:
 * SDR 265.05 nits, NitsPhysical 265.05, DisplayServicesBrightness 1.0,
 * MaxNitsEDR 1316.6, AvailableHeadroom 4.967 = 1316.6 / 265.05) a 100-nit
 * code reached the panel at 265 nits, the peak patch's 250-nit disc at 660
 * and its 497 background at 1309, everything past ~187 asked nits past the
 * metadata's max on the way to the panel, and the disc was gone at every
 * setting; the picture 2.65x brighter than the Deck's for the same levels.
 * Fix: opticalOutputScale = the SDR white's nits (potential / granted
 * headroom x 100: potential = the panel's peak / 100, granted = the panel's
 * peak / the SDR white), so a code of N nits is N nits on the panel as on
 * the Deck, whatever the slider; the metadata's max = the panel's peak
 * (potential x 100 = the granted headroom x the SDR white, the compositor's
 * clip in these absolute nits). The grant does not follow the metadata (the
 * PG32UCDP: 4.967x with max 500, 1000, 1317, 4000 or none). */
static float s_meta_scale;      /* the SDR white in nits the metadata says */
static float hdr_sdr_white(void)
{
    float g = nv2a_host_hdr_granted(), p = nv2a_host_hdr_potential();
    return g > 1.0f && p > g ? p / g * 100.0f : 100.0f;
}
static float hdr_meta_peak(void)
{
    float p = nv2a_host_hdr_potential();
    return p > 1.0f ? p * 100.0f : 1000.0f;
}
static void hdr_meta(void)
{
    float peak = hdr_meta_peak(), scale = hdr_sdr_white();
    if (s_hdr_win != HDRL_PQ || (peak == s_meta_peak && scale == s_meta_scale))
        return;
    s_meta_peak = peak;
    s_meta_scale = scale;
    s_layer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:0.0001f
                                                          maxLuminance:peak
                                                    opticalOutputScale:scale];
    fprintf(stderr, "[PRESENT] HDR10 metadata: max %.0f nits (the display's peak), SDR white %.1f nits"
            " (EDR 1.0; potential %.3f / granted %.3f): a code of N nits is N nits on the panel\n",
            peak, scale, nv2a_host_hdr_potential(), nv2a_host_hdr_granted());
}
static void hdr_layer(int on)
{
    CGColorSpaceRef cs;
    int mode = on ? hdr_layer_mode() : HDRL_OFF;
    if (mode == HDRL_PQ && (!s_hp_blit || !s_hp_comp || !s_hp_ovl || !s_hp_pat))
        mode = HDRL_EDR;
    if (mode == HDRL_EDR && (!s_hp_blit || !s_hp_edr))
        mode = HDRL_OFF;
    if (mode == s_hdr_win)
        return;
    s_hdr_win = mode;
    cs = CGColorSpaceCreateWithName(mode == HDRL_PQ ? kCGColorSpaceITUR_2100_PQ
                                    : mode == HDRL_EDR ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
    /* On the present thread, which has no run loop to commit an implicit
     * transaction: an explicit one. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    s_layer.pixelFormat = mode ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    s_layer.wantsExtendedDynamicRangeContent = mode ? YES : NO;
    s_layer.colorspace = cs;
    if (mode != HDRL_PQ)
        s_layer.EDRMetadata = nil;      /* macOS 10.15+; the build's floor is 13 */
    [CATransaction commit];
    CGColorSpaceRelease(cs);
    s_meta_peak = 0.0f;
    s_meta_scale = 0.0f;
    if (mode == HDRL_PQ)
        hdr_meta();
    /* BO-227: macOS 15+ tone maps an extended-range layer into the headroom
     * it grants (CAToneMapModeAutomatic). The shoulder here already fits the
     * light to the peak, and the peak patch needs values past the display's
     * peak clipped, not compressed: no system tone map (PQ as well). */
    if (@available(macOS 15.0, *)) {
        if ([s_layer respondsToSelector:@selector(setToneMapMode:)])
            s_layer.toneMapMode = mode ? CAToneMapModeNever : CAToneMapModeAutomatic;
    }
    /* SMAA's last pass writes the frame's format: made again. */
    for (int k = 0; k < 3; k++) {
        [s_pso_sm[k] release];
        s_pso_sm[k] = nil;
    }
    s_smaa_state = 0;
    fprintf(stderr, "[PRESENT] HDR %s: %s layer\n", mode ? "on" : "off",
            mode == HDRL_PQ ? "HDR10 (RGBA16Float, ST 2084 PQ, BT.2020)"
            : mode == HDRL_EDR ? "EDR (RGBA16Float, extended sRGB)" : "SDR (BGRA8, sRGB)");
}

/* BO-110: the peak calibration patch for the overlay (nv2a_host_hdr_patch):
 * a sun-like disc at symbol_nits on a background far past any display's
 * peak, so the disc disappears once it reaches what the display can show.
 * Extended sRGB values, as the overlay draws into the EDR layer. */
static id<MTLTexture> s_patch;
static float          s_patch_nits = -1.0f;
static void *hdr_patch(float nits)
{
    enum { PW = 128, PH = 64 };
    if (s_hdr_win != HDRL_EDR)
        return NULL;            /* BO-244: PQ draws the patterns itself, as Vulkan */
    if (!s_patch) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:PW height:PH mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_patch = [s_dev newTextureWithDescriptor:td];
        if (!s_patch)
            return NULL;
    }
    if (nits != s_patch_nits) {
        static __fp16 px[PH][PW][4];
        float bg = 1.055f * powf(100.0f, 1.0f / 2.4f) - 0.055f;          /* EDR 100: clipped by any display */
        float l = nits / 100.0f;
        float fg = l > 0.0031308f ? 1.055f * powf(l, 1.0f / 2.4f) - 0.055f : l * 12.92f;
        for (int y = 0; y < PH; y++)
            for (int x = 0; x < PW; x++) {
                float dx = x - PW * 0.5f + 0.5f, dy = y - PH * 0.5f + 0.5f;
                float d = sqrtf(dx * dx + dy * dy);
                float v = d < PH * 0.22f ? fg : bg;
                px[y][x][0] = px[y][x][1] = px[y][x][2] = (__fp16)v;
                px[y][x][3] = (__fp16)1.0f;
            }
        [s_patch replaceRegion:MTLRegionMake2D(0, 0, PW, PH) mipmapLevel:0 withBytes:px
                   bytesPerRow:PW * 8];
        s_patch_nits = nits;
    }
    return (void *)s_patch;
}

/* BO-227/BO-244 (RECOMP_HDR_PROBE=1, a test hook): once a second, after
 * the overlay, the values this presenter hands macOS: the headroom (now and
 * potential), the levels, the whole frame (the title's encoded values)
 * against the composite's output, decoded to nits, and what the Vulkan flip
 * pass's maths (video/hdr_shaders.h, transcribed below) gives for the same
 * values: the error, and the light past paper white (pixels, their nits:
 * median, 99th percentile, max; how many reach 90 % of the peak). Then the
 * drawable at the pattern's disc and background. The PQ layer in nits; the
 * extended-sRGB fallback in nits too (EDR x 100). */
static int hdr_probe_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HDR_PROBE");
        on = e && *e && strcmp(e, "0") ? 1 : 0;
    }
    return on;
}
static float probe_lin(float c)
{
    float a = fabsf(c), l = a <= 0.04045f ? a / 12.92f : powf((a + 0.055f) / 1.055f, 2.4f);
    return c < 0 ? -l : l;
}
static float probe_pq_nits(float e)
{
    float p = powf(fmaxf(e, 0.0f), 1.0f / 78.84375f);
    return 10000.0f * powf(fmaxf(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p),
                           1.0f / 0.1593017578125f);
}
/* hdr_tone + the scale + hdr_pq's black lift, in C: BT.709 nits, before the
 * BT.2020 matrix (the probe compares the brightest BT.709 channel's nits). */
static void probe_ref(const float c0[3], const float hp[12], float out[3])
{
    float l[3], y, mx, m;
    for (int i = 0; i < 3; i++) {
        float c = fmaxf(c0[i], 0.0f);
        l[i] = hp[3] < 0.5f ? powf(c, 2.2f)
             : (c > 0.04045f ? powf((c + 0.055f) / 1.055f, 2.4f) : c / 12.92f);
    }
    y = 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
    if (y > 0.0f && y < 0.18f && hp[4] != 1.0f)
        for (int i = 0; i < 3; i++) l[i] *= 0.18f * powf(y / 0.18f, hp[4]) / y;
    mx = fmaxf(l[0], fmaxf(l[1], l[2]));
    m = fmaxf(hp[0], 1.0f);
    if (mx > 1.0f) {
        float x = 1.0f + hp[2] * (mx - 1.0f), v = 1.0f;
        if (m > 1.0001f) {
            float k = m - 1.0f, e = 1.0f + k * (1.0f - expf(-(x - 1.0f) / k));
            float r = 1.0f + k * (x - 1.0f) / ((x - 1.0f) + k);
            float t = fminf(fmaxf((x - 2.5f) / 1.5f, 0.0f), 1.0f);
            float w = hp[5] * fminf(fmaxf(k - 0.5f, 0.0f), 1.0f) * t * t * (3.0f - 2.0f * t);
            v = e + (r - e) * w;
        }
        for (int i = 0; i < 3; i++) l[i] *= v / mx;
    }
    for (int i = 0; i < 3; i++)
        out[i] = 10000.0f * (hp[6] + (1.0f - hp[6] / hp[9]) * l[i] * hp[1]);
}
static int probe_fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static void hdr_probe(id<MTLCommandBuffer> cb, id<MTLTexture> frame, id<MTLTexture> comp,
                      id<MTLTexture> draw)
{
    static uint64_t last;
    static id<MTLBuffer> fbuf, cbuf, dbuf;
    static int busy;
    uint64_t now = xbox_HostNowNs();
    float pk, pw, hw, sh, hl, sf, bl, hp[12];
    int srgb;
    if (!hdr_probe_on() || !s_hdr_win || busy || now - last < 1000000000ull || !comp
            || [comp width] != [frame width] || [comp height] != [frame height])
        return;
    last = now;
    nv2a_host_hdr_levels(&pk, &pw, &hw);
    nv2a_host_hdr_tone(&sh, &hl, &srgb);
    nv2a_host_hdr_shape(&sf, &bl);
    hdr_comp_consts(hp, pk, pw, hw, sh, hl, srgb, sf, bl, 1);
    const NSUInteger W = [frame width], H = [frame height];
    const int fbpp = [frame pixelFormat] == MTLPixelFormatRGBA16Float ? 8 : 4;
    if (!fbuf || [fbuf length] < W * H * fbpp) {
        [fbuf release];
        fbuf = [s_dev newBufferWithLength:W * H * 8 options:MTLResourceStorageModeShared];
    }
    if (!cbuf || [cbuf length] < W * H * 8) {
        [cbuf release];
        cbuf = [s_dev newBufferWithLength:W * H * 8 options:MTLResourceStorageModeShared];
    }
    if (!dbuf)
        dbuf = [s_dev newBufferWithLength:4 * 8 options:MTLResourceStorageModeShared];
    if (!fbuf || !cbuf || !dbuf)
        return;
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:frame sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(W, H, 1) toBuffer:fbuf destinationOffset:0
   destinationBytesPerRow:W * fbpp destinationBytesPerImage:W * H * fbpp];
    [b copyFromTexture:comp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(W, H, 1) toBuffer:cbuf destinationOffset:0
   destinationBytesPerRow:W * 8 destinationBytesPerImage:W * H * 8];
    /* the drawable at the last pattern: its centre, inside near the left
     * edge (the disc's background, or the left half), near the right edge */
    int have_pat = 0;               /* BO-244: hdr_probe_patch, below, each second on its own */
    float pr[4];
    int pkind = s_pat_kind;
    const float pat_a = s_pat_ab[0], pat_b = s_pat_ab[1], meta = s_meta_peak;
    memcpy(pr, s_pat_r, sizeof pr);
    if (have_pat) {
        float px[3][2] = { { (pr[0] + pr[2]) * 0.5f, (pr[1] + pr[3]) * 0.5f },
                           { pr[0] + (pr[2] - pr[0]) * 0.06f, (pr[1] + pr[3]) * 0.5f },
                           { pr[2] - (pr[2] - pr[0]) * 0.06f, (pr[1] + pr[3]) * 0.5f } };
        for (int j = 0; j < 3; j++) {
            NSUInteger X = (NSUInteger)fmaxf(0.0f, fminf(px[j][0], [draw width] - 1.0f));
            NSUInteger Y = (NSUInteger)fmaxf(0.0f, fminf(px[j][1], [draw height] - 1.0f));
            [b copyFromTexture:draw sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(X, Y, 0)
                    sourceSize:MTLSizeMake(1, 1, 1) toBuffer:dbuf destinationOffset:(NSUInteger)j * 8
           destinationBytesPerRow:8 destinationBytesPerImage:8];
        }
    }
    [b endEncoding];
    const float pot = nv2a_host_hdr_potential(), cur = s_probe_now;
    const int mode = s_hdr_win;
    struct { float v[12]; } hq;      /* a block takes no arrays */
    memcpy(hq.v, hp, sizeof hq.v);
    busy = 1;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        const uint8_t *fp = (const uint8_t *)[fbuf contents];
        const __fp16 *cp = (const __fp16 *)[cbuf contents];
        size_t n = (size_t)W * H, nl = 0, nref = 0, nbad = 0, n90 = 0;
        float *lit = (float *)malloc(n * sizeof(float));
        float maxerr = 0.0f, omax = 0.0f, fmax_ = 0.0f, sum = 0.0f;
        for (size_t i = 0; i < n && lit; i++) {
            float f[3], o[3], r[3];
            for (int ch = 0; ch < 3; ch++) {
                f[ch] = fbpp == 8 ? (float)((const __fp16 *)fp)[i * 4 + ch] : fp[i * 4 + (2 - ch)] / 255.0f;
                float e = (float)cp[i * 4 + ch];
                o[ch] = mode == HDRL_PQ ? probe_pq_nits(e) : probe_lin(e) * 100.0f;
            }
            /* PQ: BT.2020 out; back to BT.709 for the comparison */
            if (mode == HDRL_PQ) {
                float r0 = 1.6605f * o[0] - 0.5876f * o[1] - 0.0728f * o[2];
                float g0 = -0.1246f * o[0] + 1.1329f * o[1] - 0.0083f * o[2];
                float b0 = -0.0182f * o[0] - 0.1006f * o[1] + 1.1187f * o[2];
                o[0] = r0; o[1] = g0; o[2] = b0;
            }
            float fm = fmaxf(f[0], fmaxf(f[1], f[2])), om = fmaxf(o[0], fmaxf(o[1], o[2]));
            fmax_ = fmaxf(fmax_, fm);
            omax = fmaxf(omax, om);
            sum += 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
            probe_ref(f, hq.v, r);
            float rm = fmaxf(r[0], fmaxf(r[1], r[2]));
            if (rm > 0.5f) {
                float d = fabsf(om - rm) / rm;
                nref++;
                if (d > 0.02f)
                    nbad++;         /* HUD over light (clamped there), or a real mismatch */
                else if (d > maxerr)
                    maxerr = d;
            }
            if (fm > 1.0f) {
                lit[nl++] = om;
                if (om >= 0.9f * pk)
                    n90++;
            }
        }
        float p50 = 0, p99 = 0, pmax = 0;
        if (nl && lit) {
            qsort(lit, nl, sizeof(float), probe_fcmp);
            p50 = lit[nl / 2];
            p99 = lit[(size_t)(nl * 0.99)];
            pmax = lit[nl - 1];
        }
        free(lit);
        fprintf(stderr, "[HDRPROBE] %s layer, headroom potential %.3f now %.3f; peak %.0f paper %.0f"
                " hud %.0f nits; frame %lux%lu max enc %.2f; out max %.0f nits, mean %.1f;"
                " vs the Vulkan maths: max rel. error %.4f over %zu px (%zu px > 2%%: HUD over light);"
                " past paper white %zu px (%.2f%%): median %.0f, p99 %.0f, max %.0f nits,"
                " %zu px >= 90%% of the peak\n",
                mode == HDRL_PQ ? "PQ" : "EDR", pot, cur, pk, pw, hw, (unsigned long)W,
                (unsigned long)H, fmax_, omax, sum / (float)n, maxerr, nref, nbad, nl,
                100.0f * (float)nl / (float)n, p50, p99, pmax, n90);
        if (have_pat) {
            const __fp16 *dp = (const __fp16 *)[dbuf contents];
            float v[3];
            for (int j = 0; j < 3; j++)
                v[j] = mode == HDRL_PQ ? probe_pq_nits((float)dp[j * 4 + 1]) : probe_lin((float)dp[j * 4 + 1]) * 100.0f;
            fprintf(stderr, "[HDRPROBE] pattern %d: asked %.0f (%s) and %.0f nits; drawable centre %.1f"
                    " left %.1f right %.1f nits; HDR10 metadata max %.0f (the display's peak), granted %.3fx:"
                    " %s\n", pkind, pat_a, pkind == 0 ? "disc" : "left", pat_b, v[0], v[1], v[2], meta,
                    cur,
                    pkind != 0 ? "paper white | HUD white"
                    : pat_a < meta ? "disc below the display's level: visible"
                    : "disc at or past the display's level: gone");
        }
        busy = 0;
    }];
}

/* BO-244 (RECOMP_HDR_PROBE=1): while the overlay asks for a pattern (the
 * peak or paper-white row selected), once a second and apart from the frame
 * statistics: the levels asked and drawn, the drawable's PQ code values at
 * the disc (centre) and the background (inside the left and right edges),
 * as nits, the HDR10 metadata's max, the granted headroom and the layer's
 * state. */
static void hdr_probe_patch(id<MTLCommandBuffer> cb, id<MTLTexture> draw)
{
    static uint64_t last;
    static id<MTLBuffer> buf;
    uint64_t now = xbox_HostNowNs();
    if (!hdr_probe_on() || s_hdr_win != HDRL_PQ || s_pat_kind < 0 || now - last < 1000000000ull
            || [draw pixelFormat] != MTLPixelFormatRGBA16Float)
        return;
    last = now;
    if (!buf)
        buf = [s_dev newBufferWithLength:3 * 8 options:MTLResourceStorageModeShared];
    if (!buf)
        return;
    float pr[4];
    memcpy(pr, s_pat_r, sizeof pr);
    const int kind = s_pat_kind;
    const float a = s_pat_ab[0], b = s_pat_ab[1], da = s_pat_drawn[0], db = s_pat_drawn[1];
    const float meta = s_meta_peak, scale = s_meta_scale, cur = s_probe_now, pot = nv2a_host_hdr_potential();
    s_pat_kind = -1;
    float px[3][2] = { { (pr[0] + pr[2]) * 0.5f, (pr[1] + pr[3]) * 0.5f },
                       { pr[0] + (pr[2] - pr[0]) * 0.06f, (pr[1] + pr[3]) * 0.5f },
                       { pr[2] - (pr[2] - pr[0]) * 0.06f, (pr[1] + pr[3]) * 0.5f } };
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    for (int j = 0; j < 3; j++) {
        NSUInteger X = (NSUInteger)fmaxf(0.0f, fminf(px[j][0], [draw width] - 1.0f));
        NSUInteger Y = (NSUInteger)fmaxf(0.0f, fminf(px[j][1], [draw height] - 1.0f));
        [be copyFromTexture:draw sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(X, Y, 0)
                 sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:(NSUInteger)j * 8
        destinationBytesPerRow:8 destinationBytesPerImage:8];
    }
    [be endEncoding];
    char layer[200];
    const char *tm = "n/a";
    if (@available(macOS 15.0, *))
        tm = [s_layer.toneMapMode UTF8String];
    snprintf(layer, sizeof layer, "layer RGBA16Float PQ, wantsEDR %d, metadata %s, toneMap %s",
             s_layer.wantsExtendedDynamicRangeContent ? 1 : 0, s_layer.EDRMetadata ? "HDR10" : "none", tm);
    NSString *ls = [NSString stringWithUTF8String:layer];
    [ls retain];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        const __fp16 *d = (const __fp16 *)[buf contents];
        float code[3], nits[3];
        for (int j = 0; j < 3; j++) {
            code[j] = (float)d[j * 4 + 1];
            nits[j] = probe_pq_nits(code[j]);
        }
        fprintf(stderr, "[HDRPROBE] patch %s: asked %.0f / %.0f nits, drawn %.0f / %.0f; drawable PQ"
                " codes centre %.4f (%.1f nits), left %.4f (%.1f), right %.4f (%.1f); metadata max %.0f"
                " (the display's peak), SDR white %.1f nits, headroom now %.3f of %.3f; %s; %s\n",
                kind == 0 ? "peak disc / background" : "paper | HUD white", a, b, da, db, code[0], nits[0],
                code[1], nits[1], code[2], nits[2], meta, scale, cur, pot, [ls UTF8String],
                kind != 0 ? "two halves"
                : fabsf(code[0] - code[1]) < 0.0005f ? "disc and background the same code: the disc is gone"
                : "disc and background differ: the disc shows");
        [ls release];
    }];
}

static int fail(char *err, size_t n, const char *what)
{
    snprintf(err, n, "%s", what);
    return 0;
}

int fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn)
{
    NSError *e = nil;
    id<MTLLibrary> lib;
    MTLCompileOptions *opts;
    MTLRenderPipelineDescriptor *pd;
    MTLSamplerDescriptor *sd;
    int w = 0, h = 0, k;

    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev)
            return fail(err, errn, "no Metal device");
        s_queue = [s_dev newCommandQueue];
        if (!s_queue)
            return fail(err, errn, "no command queue");
        s_unified = [s_dev hasUnifiedMemory] ? 1 : 0;
        opts = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_blit_msl]
                                  options:opts error:&e];
        [opts release];
        if (!lib)
            return fail(err, errn, "present shader failed to compile");
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_blit"] autorelease];
        /* BO-110: the half-float set first (an EDR layer, its passes). */
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        s_hp_blit = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_hdr_pq"] autorelease];
        s_hp_comp = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_hdr_edr"] autorelease];
        s_hp_edr = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_hdr_pat"] autorelease];
        s_hp_pat = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        /* BO-244: the overlay's encode, blended over the PQ drawable by its
         * alpha (the drawable's alpha kept), as fb_present_vulkan.c's. */
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_hdr_ovl"] autorelease];
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        s_hp_ovl = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.colorAttachments[0].blendingEnabled = NO;
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_fxaa"] autorelease];
        s_hp_fxaa = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_cas"] autorelease];
        s_hp_cas = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!s_hp_blit || !s_hp_comp || !s_hp_edr || !s_hp_ovl || !s_hp_pat)
            XR_ERROR( "[PRESENT] HDR passes unavailable: %s\n",
                    e ? [[e localizedDescription] UTF8String] : "?");
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (s_pso) {
            /* The post passes; a failure leaves them off (logged), the
             * present itself unaffected. */
            pd.fragmentFunction = [[lib newFunctionWithName:@"fs_fxaa"] autorelease];
            s_pso_fxaa = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!s_pso_fxaa)
                XR_ERROR( "[PRESENT] FXAA pass unavailable: %s\n",
                        e ? [[e localizedDescription] UTF8String] : "?");
            pd.fragmentFunction = [[lib newFunctionWithName:@"fs_cas"] autorelease];
            s_pso_cas = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!s_pso_cas)
                XR_ERROR( "[PRESENT] CAS pass unavailable: %s\n",
                        e ? [[e localizedDescription] UTF8String] : "?");
        }
        [pd release];
        [lib release];
        if (!s_pso)
            return fail(err, errn, "present pipeline failed");
        for (k = 0; k < 2; k++) {
            sd = [[MTLSamplerDescriptor alloc] init];
            sd.minFilter = sd.magFilter = k ? MTLSamplerMinMagFilterLinear
                                             : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            s_smp[k] = [s_dev newSamplerStateWithDescriptor:sd];
            [sd release];
        }

        s_view = SDL_Metal_CreateView(win);
        if (!s_view)
            return fail(err, errn, SDL_GetError());
        s_layer = (CAMetalLayer *)SDL_Metal_GetLayer(s_view);
        if (!s_layer)
            return fail(err, errn, "SDL_Metal_GetLayer returned NULL");
        [s_layer retain];
        s_layer.device = s_dev;
        s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* The SDL renderer this replaces draws SDR content as sRGB. */
        {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            s_layer.colorspace = cs;
            CGColorSpaceRelease(cs);
        }
        hdr_display(win);           /* BO-110: what the display can show */
        nv2a_host_set_hdr_patch_fn(hdr_patch);
        hdr_layer(nv2a_host_hdr_active());
        s_layer.framebufferOnly = shots || hdr_probe_on() ? NO : YES;
        s_layer.displaySyncEnabled = vsync ? YES : NO;
        /* Two drawables: at most one frame queued ahead of the display
         * (the BO-74 latency measurement's recommendation, with display
         * sync on). RECOMP_PRESENT_DRAWABLES=3 gives one more of slack. */
        {
            const char *e = recomp_env(RENV_PRESENT_DRAWABLES);
            s_layer.maximumDrawableCount = (e && atoi(e) == 3) ? 3 : 2;
            if (e)
                fprintf(stderr, "[PRESENT] CAMetalLayer: %d drawables, display sync %s\n",
                        (int)s_layer.maximumDrawableCount, vsync ? "on" : "off");
        }
        s_layer.allowsNextDrawableTimeout = YES;
        SDL_Metal_GetDrawableSize(win, &w, &h);
        if (w > 0 && h > 0)
            s_layer.drawableSize = CGSizeMake(w, h);
        if (recomp_env_on(RENV_WINDOW_FLOAT)) {
            SDL_SysWMinfo wm;
            SDL_VERSION(&wm.version);
            if (SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
                [wm.info.cocoa.window setLevel:NSFloatingWindowLevel];
        }
        if (xbox_VblankDisplayLockOn())
            fbm_vblank_link(win);
        {
            const char *t = recomp_env(RENV_PACING_TRACE);
            s_trace_presented = t && !strcmp(t, "all");
        }
    }
    return 1;
}

void fbm_destroy(void)
{
    int k;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (s_link) {
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
        s_link = NULL;
    }
#pragma clang diagnostic pop
    @autoreleasepool {
        for (k = 0; k < 3; k++) {
            if (s_slot_cb[k])
                [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
            [s_slot[k] release];
            s_slot[k] = nil;
        }
        [s_layer release];
        s_layer = nil;
        if (s_view)
            SDL_Metal_DestroyView(s_view);
        s_view = NULL;
        [s_pso release];
        s_pso = nil;
        [s_pso_fxaa release];
        [s_pso_cas release];
        s_pso_fxaa = s_pso_cas = nil;
        [s_pp[0] release];
        [s_pp[1] release];
        s_pp[0] = s_pp[1] = nil;
        for (k = 0; k < 3; k++) {
            [s_pso_sm[k] release];
            s_pso_sm[k] = nil;
        }
        for (k = 0; k < 2; k++) {
            [s_sm_tex[k] release];
            [s_sm_lut[k] release];
            s_sm_tex[k] = s_sm_lut[k] = nil;
        }
        [s_smp[0] release];
        [s_smp[1] release];
        s_smp[0] = s_smp[1] = nil;
        /* The device and queue stay: the backend holds its own references
         * and the process is about to exit. On the fallback path (setup
         * failed, the window goes back to the SDL renderer) that leaks the
         * device and queue made here for the run: the backend has not
         * started yet and creates its own. */
    }
}

void fbm_get(void **device, void **queue)
{
    *device = (void *)s_dev;
    *queue = (void *)s_queue;
}

/* Slot k at w x h, made or re-made at that size. Releasing the old texture
 * while a present pass reads it is fine: a committed command buffer retains
 * what it uses. */
void *fbm_slot(int k, uint32_t w, uint32_t h)
{
    @autoreleasepool {
        if (!s_slot[k] || [s_slot[k] width] != w || [s_slot[k] height] != h
                || [s_slot[k] pixelFormat] != PP_FMT) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:PP_FMT
                                             width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            /* Shared: an upload needs replaceRegion, which a private texture
             * refuses, and a blit destination gains nothing from private.
             * Managed on a discrete GPU, as the backend's targets. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
            [s_slot[k] release];
            s_slot[k] = [s_dev newTextureWithDescriptor:td];
        }
    }
    return (void *)s_slot[k];
}

/* BO-110: slot k's scene snapshot at w x h (half float), and whether this
 * frame has one. */
void *fbm_scene_slot(int k, uint32_t w, uint32_t h, int valid)
{
    s_scene_ok[k] = 0;
    if (!valid || !s_hdr_win)
        return NULL;
    @autoreleasepool {
        if (!s_scene[k] || [s_scene[k] width] != w || [s_scene[k] height] != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                             width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
            [s_scene[k] release];
            s_scene[k] = [s_dev newTextureWithDescriptor:td];
        }
    }
    s_scene_ok[k] = s_scene[k] != nil;
    return (void *)s_scene[k];
}

/* A guest-bytes frame (bpp 4 = X8R8G8B8, which is BGRA8's byte order;
 * bpp 2 = R5G6B5) into slot k. */
int fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
               uint32_t bpp)
{
    id<MTLTexture> t;
    uint32_t x, y;

    @autoreleasepool {
        if (s_slot_cb[k]) {
            [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
        }
        t = (id<MTLTexture>)fbm_slot(k, w, h);
        if (!t)
            return 0;
        s_scene_ok[k] = 0;
        if (s_hdr_win) {
            /* BO-110: a half-float slot; the guest's bytes converted. */
            uint16_t *row = (uint16_t *)malloc((size_t)w * 8u);
            for (y = 0; row && y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint32_t v;
                    __fp16 c[4];
                    if (bpp == 4) {
                        memcpy(&v, src + (size_t)y * pitch + x * 4u, 4);
                    } else {
                        uint16_t q;
                        memcpy(&q, src + (size_t)y * pitch + x * 2u, 2);
                        v = 0xFF000000u | ((uint32_t)((q >> 11) & 0x1F) << 19)
                          | ((uint32_t)((q >> 5) & 0x3F) << 10) | ((uint32_t)(q & 0x1F) << 3);
                    }
                    c[0] = (__fp16)(((v >> 16) & 0xFF) / 255.0f);
                    c[1] = (__fp16)(((v >> 8) & 0xFF) / 255.0f);
                    c[2] = (__fp16)((v & 0xFF) / 255.0f);
                    c[3] = (__fp16)1.0f;
                    memcpy(row + x * 4u, c, 8);
                }
                [t replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:row
                     bytesPerRow:w * 8u];
            }
            free(row);
            return row != NULL;
        }
        if (bpp == 4) {
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:src
                 bytesPerRow:pitch];
        } else {
            if (s_conv_cap < w) {
                uint32_t *n = (uint32_t *)realloc(s_conv, (size_t)w * 4);
                if (!n)
                    return 0;
                s_conv = n;
                s_conv_cap = w;
            }
            for (y = 0; y < h; y++) {
                const uint16_t *r = (const uint16_t *)(src + (size_t)y * pitch);
                for (x = 0; x < w; x++) {
                    uint32_t v = r[x];
                    uint32_t b = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2,
                             rr = ((v >> 11) & 0x1F) << 3;
                    s_conv[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
                }
                [t replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:s_conv
                     bytesPerRow:w * 4];
            }
        }
    }
    return 1;
}

static void save_shot(id<MTLBuffer> buf, uint32_t w, uint32_t h, const char *path,
                      uint32_t flip)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 24,
                                                       SDL_PIXELFORMAT_BGR24);
    const uint8_t *px = (const uint8_t *)[buf contents];
    uint32_t x, y;

    if (!surf)
        return;
    for (y = 0; y < h; y++) {
        uint8_t *o = (uint8_t *)surf->pixels + (size_t)y * surf->pitch;
        const uint8_t *in = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            /* BGRA in the drawable; BGR24 is the bytes B, G, R. */
            o[x * 3 + 0] = in[x * 4 + 0];
            o[x * 3 + 1] = in[x * 4 + 1];
            o[x * 3 + 2] = in[x * 4 + 2];
        }
    }
    if (SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, "[PRESENT] window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

/* A post pass's target at w x h, made or re-made at that size. */
static id<MTLTexture> target_texture(id<MTLTexture> *slot, MTLPixelFormat fmt, NSUInteger w,
                                     NSUInteger h)
{
    if (!*slot || [*slot width] != w || [*slot height] != h || [*slot pixelFormat] != fmt) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        [*slot release];
        *slot = [s_dev newTextureWithDescriptor:td];
    }
    return *slot;
}

/* RECOMP_TRACE=post_gpu: the GPU time of each pass, from its own command
 * buffer (the passes are committed one by one while it is on). */
enum { PT_FXAA, PT_CAS, PT_SM_EDGES, PT_SM_WEIGHTS, PT_SM_BLEND, PT_SCALE, PT_N };
static const char *const s_pt_name[PT_N] = { "fxaa", "cas", "smaa edges", "smaa weights",
                                             "smaa blend", "scale+overlay" };
static double s_pt_sum[PT_N];
static unsigned s_pt_n[PT_N], s_pt_presents;
static int s_pt_on = -1;

static int pt_on(void)
{
    if (s_pt_on < 0)
        s_pt_on = recomp_env(RENV_POST_GPU) != NULL;
    return s_pt_on;
}

/* The command buffer's GPU time under label `what`; every 300 scales a line. */
static void pt_watch(id<MTLCommandBuffer> cb, int what)
{
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        @synchronized ([NSNull null]) {
            int i;
            s_pt_sum[what] += c.GPUEndTime - c.GPUStartTime;
            s_pt_n[what]++;
            if (what == PT_SCALE && ++s_pt_presents % 300 == 0) {
                fprintf(stderr, "[PRESENT] post GPU ms (avg of the last 300):");
                for (i = 0; i < PT_N; i++) {
                    if (s_pt_n[i])
                        fprintf(stderr, " %s %.3f", s_pt_name[i],
                                s_pt_sum[i] * 1000.0 / (double)s_pt_n[i]);
                    s_pt_sum[i] = 0;
                    s_pt_n[i] = 0;
                }
                fprintf(stderr, "\n");
            }
        }
    }];
}

/* One full-frame pass: `src` (up to three textures, all sampled bilinear,
 * binding k with sampler k) through pso into dst, which is first cleared to
 * zero when `clear` (SMAA's edge pass discards where there is no edge).
 * `bytes` is the fragment function's constant at buffer 0. With the trace on
 * the pass is committed on its own and *cbp moves to a new buffer. */
static void post_pass(id<MTLCommandBuffer> *cbp, int what, id<MTLRenderPipelineState> pso,
                      id<MTLTexture> dst, int clear, id<MTLTexture> const *src, int nsrc,
                      const void *bytes, NSUInteger blen)
{
    MTLRenderPassDescriptor *pr = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> pe;
    int i;

    pr.colorAttachments[0].texture = dst;
    pr.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
    pr.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pr.colorAttachments[0].storeAction = MTLStoreActionStore;
    pe = [*cbp renderCommandEncoderWithDescriptor:pr];
    [pe setRenderPipelineState:pso];
    for (i = 0; i < nsrc; i++) {
        [pe setFragmentTexture:src[i] atIndex:(NSUInteger)i];
        [pe setFragmentSamplerState:s_smp[1] atIndex:(NSUInteger)i];
    }
    if (bytes)
        [pe setFragmentBytes:bytes length:blen atIndex:0];
    [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [pe endEncoding];
    if (pt_on()) {
        pt_watch(*cbp, what);
        [*cbp commit];
        *cbp = [s_queue commandBuffer];
    }
}

/* SMAA's pipelines and lookup textures, at the first use. 1 when it can run. */
static int smaa_init(void)
{
    const MTLPixelFormat fmt[3] = { MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm, PP_FMT };
    static const char *const fn[3] = { "fs_smaa_edges", "fs_smaa_weights", "fs_smaa_blend" };
    NSError *e = nil;
    char *src;
    id<MTLLibrary> lib;
    int k;

    if (s_smaa_state)
        return s_smaa_state > 0;
    s_smaa_state = -1;
    src = smaa_msl_source();
    if (!src) {
        XR_ERROR( "[PRESENT] SMAA unavailable: its shader source did not adapt\n");
        return 0;
    }
    lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:nil error:&e];
    free(src);
    if (!lib) {
        XR_ERROR( "[PRESENT] SMAA unavailable: %s\n",
                e ? [[e localizedDescription] UTF8String] : "shader compile failed");
        return 0;
    }
    for (k = 0; k < 3; k++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_smaa"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:[NSString stringWithUTF8String:fn[k]]]
                               autorelease];
        pd.colorAttachments[0].pixelFormat = fmt[k];
        s_pso_sm[k] = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        [pd release];
        if (!s_pso_sm[k]) {
            XR_ERROR( "[PRESENT] SMAA unavailable: pipeline %s: %s\n", fn[k],
                    e ? [[e localizedDescription] UTF8String] : "?");
            [lib release];
            return 0;
        }
    }
    [lib release];
    s_smaa_state = 1;
    fprintf(stderr, "[PRESENT] SMAA ready (preset high)\n");
    return 1;
}

/* SMAA's textures at w x h (the lookup tables once), or all released. */
static int smaa_textures(NSUInteger w, NSUInteger h)
{
    if (!target_texture(&s_sm_tex[0], MTLPixelFormatRG8Unorm, w, h)
            || !target_texture(&s_sm_tex[1], MTLPixelFormatRGBA8Unorm, w, h))
        return 0;
    if (!s_sm_lut[0]) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                         width:SMAA_AREA_W height:SMAA_AREA_H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_sm_lut[0] = [s_dev newTextureWithDescriptor:td];
        [s_sm_lut[0] replaceRegion:MTLRegionMake2D(0, 0, SMAA_AREA_W, SMAA_AREA_H) mipmapLevel:0
                         withBytes:smaa_area_bytes bytesPerRow:SMAA_AREA_W * 2];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                width:SMAA_SEARCH_W
                                                               height:SMAA_SEARCH_H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_sm_lut[1] = [s_dev newTextureWithDescriptor:td];
        [s_sm_lut[1] replaceRegion:MTLRegionMake2D(0, 0, SMAA_SEARCH_W, SMAA_SEARCH_H)
                       mipmapLevel:0 withBytes:smaa_search_bytes bytesPerRow:SMAA_SEARCH_W];
    }
    return s_sm_lut[0] && s_sm_lut[1];
}

static void smaa_release(void)
{
    int k;
    for (k = 0; k < 2; k++) {
        [s_sm_tex[k] release];
        [s_sm_lut[k] release];
        s_sm_tex[k] = s_sm_lut[k] = nil;
    }
}

/* The passes asked for (RECOMP_GFX_AA, RECOMP_GFX_SHARPEN) over `src`, the
 * slot: FXAA, or SMAA's three, then CAS. Returns what the scale to the
 * window samples. What a setting not asked for needs is released. */
static id<MTLTexture> post_chain(id<MTLCommandBuffer> *cbp, id<MTLTexture> src)
{
    int aa = nv2a_host_post_aa();
    float sharp = nv2a_host_sharpen();
    NSUInteger sw = [src width], sh = [src height];
    static int warmed;
    if (!warmed) {
        /* BO-208: SMAA's pipelines are built at the first present, not at
         * the moment the player picks SMAA (FXAA and CAS are made at start). */
        warmed = 1;
        (void)smaa_init();
    }
    id<MTLRenderPipelineState> pfx = s_hdr_win ? s_hp_fxaa : s_pso_fxaa;
    id<MTLRenderPipelineState> pcas = s_hdr_win ? s_hp_cas : s_pso_cas;
    int fx = aa == NV2A_AA_FXAA && pfx;
    int sm = aa == NV2A_AA_SMAA && smaa_init() && smaa_textures(sw, sh);
    int cas = sharp > 0.001f && pcas;
    float metrics[4] = { 1.0f / (float)sw, 1.0f / (float)sh, (float)sw, (float)sh };
    static int last_aa = -1;
    static float last_sharp = -1.0f;

    if (aa != last_aa || sharp != last_sharp) {
        last_aa = aa;
        last_sharp = sharp;
        fprintf(stderr, "[PRESENT] post passes: AA %s, sharpen %.2f (%lux%lu)\n",
                aa == NV2A_AA_FXAA ? "FXAA" : aa == NV2A_AA_SMAA ? (sm ? "SMAA" : "SMAA (unavailable)")
                : "off", sharp, (unsigned long)sw, (unsigned long)sh);
    }
    if (!sm)
        smaa_release();
    if (!fx && !sm && s_pp[0]) {
        [s_pp[0] release];
        s_pp[0] = nil;
    }
    if (!cas && s_pp[1]) {
        [s_pp[1] release];
        s_pp[1] = nil;
    }
    if (fx) {
        id<MTLTexture> dst = target_texture(&s_pp[0], PP_FMT, sw, sh);
        if (dst) {
            post_pass(cbp, PT_FXAA, pfx, dst, 0, &src, 1, NULL, 0);
            src = dst;
        }
    } else if (sm) {
        id<MTLTexture> dst = target_texture(&s_pp[0], PP_FMT, sw, sh);
        if (dst) {
            id<MTLTexture> in2[3];
            in2[0] = src;
            post_pass(cbp, PT_SM_EDGES, s_pso_sm[0], s_sm_tex[0], 1, in2, 1, metrics, sizeof metrics);
            in2[0] = s_sm_tex[0];
            in2[1] = s_sm_lut[0];
            in2[2] = s_sm_lut[1];
            post_pass(cbp, PT_SM_WEIGHTS, s_pso_sm[1], s_sm_tex[1], 1, in2, 3, metrics, sizeof metrics);
            in2[0] = src;
            in2[1] = s_sm_tex[1];
            post_pass(cbp, PT_SM_BLEND, s_pso_sm[2], dst, 0, in2, 2, metrics, sizeof metrics);
            src = dst;
        }
    }
    if (cas) {
        id<MTLTexture> dst = target_texture(&s_pp[1], PP_FMT, sw, sh);
        if (dst) {
            post_pass(cbp, PT_CAS, pcas, dst, 0, &src, 1, &sharp, sizeof sharp);
            src = dst;
        }
    }
    return src;
}

/* BO-244: the composite's constants. hq[0] = a, hq[1] = b as the Vulkan
 * flip pass's push constants (nv2a_pb_vulkan.c), hq[2] = (the HUD's white /
 * 10,000, the peak / 10,000, 1 = a scene snapshot). */
static void hdr_comp_consts(float hp[12], float pk, float pw, float hw, float sh, float hl, int srgb,
                            float sf, float bl, int scene)
{
    hp[0] = pk / pw;
    hp[1] = pw / 10000.0f;
    hp[2] = hl;
    hp[3] = srgb ? 1.0f : 0.0f;
    hp[4] = sh;
    hp[5] = sf;
    hp[6] = bl / 10000.0f;
    hp[7] = 0.0f;
    hp[8] = hw / 10000.0f;
    hp[9] = pk / 10000.0f;
    hp[10] = scene ? 1.0f : 0.0f;
    hp[11] = 0.0f;
}

static float pq_of_nits(float n)
{
    float m = powf(fminf(fmaxf(n, 0.0f), 10000.0f) / 10000.0f, 0.1593017578125f);
    return powf((0.8359375f + 18.8515625f * m) / (1.0f + 18.6875f * m), 78.84375f);
}

#if defined(XBOXRECOMP_HAVE_OVERLAY)
/* BO-244: the overlay over a PQ drawable, as fb_present_vulkan.c does it:
 * drawn into s_ovl (BGRA8, cleared to transparent), then encoded (gamma
 * 2.2, x the HUD's white, BT.2020, PQ) and blended over the drawable by its
 * alpha; then the calibration pattern it asked for, if any, in PQ. Ends
 * `enc` and returns the drawable's new encoder. */
static id<MTLRenderCommandEncoder> hdr_overlay(id<MTLCommandBuffer> cb, id<MTLRenderCommandEncoder> enc,
                                               id<MTLTexture> draw)
{
    NSUInteger w = [draw width], h = [draw height];
    MTLRenderPassDescriptor *op, *dp;
    id<MTLRenderCommandEncoder> oe;
    float pk, pw, hw, hp[4] = { 0 }, r[4], a, b;
    int kind;
    [enc endEncoding];
    if (!target_texture(&s_ovl, MTLPixelFormatBGRA8Unorm, w, h))
        s_ovl = nil;
    if (s_ovl) {
        op = [MTLRenderPassDescriptor renderPassDescriptor];
        op.colorAttachments[0].texture = s_ovl;
        op.colorAttachments[0].loadAction = MTLLoadActionClear;
        op.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        op.colorAttachments[0].storeAction = MTLStoreActionStore;
        oe = [cb renderCommandEncoderWithDescriptor:op];
        xbox_OverlayMetalDraw((void *)s_dev, (void *)cb, (void *)op, (void *)oe);
        [oe endEncoding];
    }
    dp = [MTLRenderPassDescriptor renderPassDescriptor];
    dp.colorAttachments[0].texture = draw;
    dp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    dp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cb renderCommandEncoderWithDescriptor:dp];
    if (s_ovl) {
        MTLViewport vp = { 0, 0, (double)w, (double)h, 0.0, 1.0 };
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        hp[0] = hw / 10000.0f;
        [enc setViewport:vp];
        [enc setRenderPipelineState:s_hp_ovl];
        [enc setFragmentTexture:s_ovl atIndex:0];
        [enc setFragmentBytes:hp length:sizeof hp atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (nv2a_host_hdr_pattern_take(&kind, r, &a, &b)) {
        s_pat_ab[0] = a;
        s_pat_ab[1] = b;
        if (r[0] < 0) r[0] = 0;
        if (r[1] < 0) r[1] = 0;
        if (r[2] > (float)w) r[2] = (float)w;
        if (r[3] > (float)h) r[3] = (float)h;
        if (r[2] > r[0] + 1.0f && r[3] > r[1] + 1.0f) {
            MTLScissorRect sc = { (NSUInteger)r[0], (NSUInteger)r[1], (NSUInteger)(r[2] - r[0]),
                                  (NSUInteger)(r[3] - r[1]) };
            MTLViewport vp = { 0, 0, (double)w, (double)h, 0.0, 1.0 };
            /* BO-244: the peak disc's background at the display's peak (the
             * HDR10 metadata's max, the compositor's clip in absolute nits:
             * hdr_meta), not 10000 nits: macOS treats content past that max
             * as it likes; at exactly the display's level the disc shows
             * below it and is gone at or past it, whatever macOS does past
             * the max. The levels as the Vulkan pattern takes them: 10-bit
             * PQ codes. */
            if (kind == 0 && s_meta_peak > 0.0f && b > s_meta_peak)
                b = s_meta_peak;
            if (kind == 0 && a > b)
                a = b;
            s_pat_drawn[0] = a;
            s_pat_drawn[1] = b;
            float pp[8] = { floorf(pq_of_nits(a) * 1023.0f + 0.5f) / 1023.0f,
                            floorf(pq_of_nits(b) * 1023.0f + 0.5f) / 1023.0f,
                            (r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f,
                            kind == 0 ? (r[3] - r[1]) * 0.22f : -1.0f, 0.0f, 0.0f, 0.0f };
            [enc setViewport:vp];
            [enc setScissorRect:sc];
            [enc setRenderPipelineState:s_hp_pat];
            [enc setFragmentBytes:pp length:sizeof pp atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            memcpy(s_pat_r, r, sizeof s_pat_r);

            s_pat_kind = kind;
        }
    }
    return enc;
}
#endif

/* Each turn of the main loop (the window's thread: AppKit and the layer's
 * geometry stay there, BO-210), and once before the first present: the
 * drawable's size, checked every turn and not only on resize events (a move
 * between displays of another scale need not arrive as SIZE_CHANGED); the
 * display's HDR headroom (macOS changes it with the brightness); the
 * display and the timing strategy, twice a second. At most every 8 ms (the
 * loop turns every 1-2 ms while frames come): it was once a present. */
void fbm_main_tick(SDL_Window *win)
{
    @autoreleasepool {
        static uint64_t next;
        uint64_t now = xbox_HostNowNs();
        int dw = 0, dh = 0;
        CGSize cur;
        if (!s_layer || now < next)
            return;
        next = now + 8000000ull;
        cur = s_layer.drawableSize;
        SDL_Metal_GetDrawableSize(win, &dw, &dh);
        if (dw > 0 && dh > 0 && ((int)cur.width != dw || (int)cur.height != dh))
            s_layer.drawableSize = CGSizeMake(dw, dh);
        hdr_display(win);
        timing_poll(win);
    }
}

/* Slot k (fw x fh) into the window. 0 when no drawable came (timeout, display
 * asleep, window hidden): the caller keeps the frame and tries again. On the
 * present thread (fb_present_sdl.c). */
int fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                const char *shot_path, uint32_t flip, uint64_t vblank_ns)
{
    int ok = 0;

    @autoreleasepool {
        id<CAMetalDrawable> d;
        id<MTLTexture> src;
        id<MTLCommandBuffer> cb;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor *rp;
        id<MTLBuffer> shot = nil;
        struct nv2a_rect r;
        int whole, dw = 0, dh = 0;
        uint64_t t_acq0 = xbox_LatOn() ? xbox_HostNowNs() : 0;
        pc_aim aim;
        int timed;

        (void)win;
        memset(&aim, 0, sizeof aim);
        /* The drawable's size, the display and the timing strategy are the
         * main thread's (fbm_main_tick): this runs on the present thread. */
        timed = s_strategy != PC_LEGACY;
        /* Never wait for a drawable: with two presents still on their way
         * to the glass the third drawable is the one on it. A new game
         * frame is skipped then (and said so); a redraw is tried again. */
        if (timed && !shot_path && atomic_load(&s_inflight) >= 2) {
            /* Presents the system has said nothing about for a tenth of a
             * second (the window went behind another, the display slept)
             * are not on their way any more. */
            if (xbox_HostNowNs() - s_last_commit_ns > 100000000ull) {
                atomic_store(&s_inflight, 0);
                goto take;
            }
            if (!vblank_ns)
                return 0;
            atomic_fetch_add(&s_skips, 1);
            glass_note(flip, vblank_ns, xbox_HostNowNs(), &aim, 0, 1);
            return 2;
        }
    take:
        d = [s_layer nextDrawable];
        if (!d) {
            if (!s_no_drawable++)
                fprintf(stderr, "[PRESENT] no drawable (the window is hidden, behind"
                        " another, or the display sleeps): presents wait\n");
            return 0;
        }
        if (s_no_drawable) {
            fprintf(stderr, "[PRESENT] drawables again after %u tries\n", s_no_drawable);
            s_no_drawable = 0;
        }
        if (t_acq0) {
            char b[32];
            snprintf(b, sizeof b, "%llu", (unsigned long long)xbox_HostNowNs());
            xbox_LatRow('W', flip, t_acq0, b);
        }
        dw = (int)[d.texture width];
        dh = (int)[d.texture height];
        cb = [s_queue commandBuffer];
        /* The post passes (post_shaders.h), at the frame's own size and
         * before the scale to the window; the overlay below is drawn after
         * them, so it is not filtered. */
        /* BO-110: HDR follows the setting live; the display's headroom is
         * read by the main thread (fbm_main_tick). */
        hdr_layer(nv2a_host_hdr_active());
        if (s_hdr_win && s_slot[k] && [s_slot[k] pixelFormat] != MTLPixelFormatRGBA16Float)
            fw = fh = 0;               /* a slot from before the switch: black once */
        src = s_slot[k];
        if (s_hdr_win && src && fw && fh) {
            /* The composite at the frame's size, then the post passes on it. */
            float pk, pw, hw;
            id<MTLTexture> in2[2];
            id<MTLTexture> dst = target_texture(&s_hp_c, MTLPixelFormatRGBA16Float,
                                                [src width], [src height]);
            nv2a_host_hdr_levels(&pk, &pw, &hw);
            float sf, bl, sh, hl;
            int srgb;
            nv2a_host_hdr_shape(&sf, &bl);
            nv2a_host_hdr_tone(&sh, &hl, &srgb);
            /* BO-244: the Vulkan flip pass's push constants (a, b), and the
             * HUD's white, the peak and whether there is a scene snapshot. */
            float hp[12];
            hdr_comp_consts(hp, pk, pw, hw, sh, hl, srgb, sf, bl, s_scene_ok[k] && s_scene[k]);
            hdr_meta();
            if (dst) {
                in2[0] = src;
                in2[1] = hp[10] > 0.5f ? s_scene[k] : src;
                post_pass(&cb, PT_SCALE, s_hdr_win == HDRL_PQ ? s_hp_comp : s_hp_edr, dst, 0, in2, 2,
                          hp, sizeof hp);
                src = dst;
            }
        }
        if (src && fw && fh)
            src = post_chain(&cb, src);
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (s_slot[k] && fw && fh) {
            struct nv2a_rect sr;
            nv2a_present_frame(flip);
            whole = nv2a_present_layout(fw, fh, (uint32_t)dw, (uint32_t)dh, filter, &sr, &r);
            if (r.w > 0 && r.h > 0) {
                float uv[4] = { (float)sr.x / (float)fw, (float)sr.y / (float)fh,
                                (float)sr.w / (float)fw, (float)sr.h / (float)fh };
                MTLViewport vp = { r.x, r.y, r.w, r.h, 0.0, 1.0 };
                int lin = filter == NV2A_PRESENT_LINEAR
                          || (filter == NV2A_PRESENT_INTEGER && !whole);
                [enc setRenderPipelineState:s_hdr_win ? s_hp_blit : s_pso];
                [enc setViewport:vp];
                [enc setFragmentTexture:src atIndex:0];
                [enc setFragmentSamplerState:s_smp[lin] atIndex:0];
                float dither = nv2a_host_dither() ? 1.0f : 0.0f;
                [enc setFragmentBytes:uv length:sizeof uv atIndex:0];
                if (s_hdr_win)
                    dither = 0.0f;      /* BO-110: the half-float layer needs none */
                [enc setFragmentBytes:&dither length:sizeof dither atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        /* The settings overlay, over the game's image and under the present
         * (overlay.h): more of the same encoder. BO-244: over a PQ drawable
         * drawn apart and encoded at the HUD's white, as on Vulkan. */
        if (xbox_OverlayWants()) {
            if (s_hdr_win == HDRL_PQ)
                enc = hdr_overlay(cb, enc, d.texture);
            else
                xbox_OverlayMetalDraw((void *)s_dev, (void *)cb, (void *)rp, (void *)enc);
        }
#endif
        [enc endEncoding];
        if (s_slot[k] && fw && fh)
            hdr_probe(cb, s_slot[k], s_hp_c, d.texture);   /* BO-227, RECOMP_HDR_PROBE=1 */
        hdr_probe_patch(cb, d.texture);                    /* BO-244 */
        if (shot_path && !s_hdr_win) {
            shot = [s_dev newBufferWithLength:(NSUInteger)dw * dh * 4
                                      options:MTLResourceStorageModeShared];
            if (shot) {
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake((NSUInteger)dw, (NSUInteger)dh, 1)
                          toBuffer:shot destinationOffset:0
                 destinationBytesPerRow:(NSUInteger)dw * 4
               destinationBytesPerImage:(NSUInteger)dw * dh * 4];
                [b endEncoding];
            }
        }
        if (s_trace_presented)
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                CFTimeInterval t = dr.presentedTime;   /* 0: never shown */
                if (t > 0)
                    xbox_PacingPresent(mach_to_host_ns((uint64_t)(t * 1e9)));
            }];
        /* Aimed here, with the pass encoded: what is left before the
         * commit is the same few microseconds every frame. */
        if (timed && vblank_ns) {
            uint64_t now = xbox_HostNowNs();
            os_unfair_lock_lock(&s_pc_lock);
            pc_pacer_aim(&s_pacer, vblank_ns, now, &aim);
            os_unfair_lock_unlock(&s_pc_lock);
            if (aim.at_ns > now + 100000000ull)   /* never park a frame */
                aim.at_ns = 0;
        }
        /* When it reached the glass: the pacer's feedback and the graph's
         * trace (a redraw of the same frame only gives its drawable back). */
        if (timed || glass_measure_on()) {
            uint64_t vb = vblank_ns, commit_ns = xbox_HostNowNs();
            uint32_t fl2 = flip;
            pc_aim aim2 = aim;
            atomic_fetch_add(&s_inflight, 1);
            s_last_commit_ns = commit_ns;
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                CFTimeInterval t = dr.presentedTime;   /* 0: never shown */
                if (atomic_fetch_sub(&s_inflight, 1) <= 0)
                    atomic_store(&s_inflight, 0);       /* reset while it was out */
                if (vb)
                    glass_note(fl2, vb, commit_ns, &aim2,
                               t > 0 ? mach_to_host_ns((uint64_t)(t * 1e9)) : 0, 0);
            }];
        }
        if (aim.at_ns)
            [cb presentDrawable:d atTime:host_ns_to_mach_s(aim.at_ns)];
        else
            [cb presentDrawable:d];
        if (t_acq0) {
            /* presentedTime: host seconds (mach_absolute_time) when the
             * drawable reached the glass, 0 when it never did. */
            uint32_t fl = flip;
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                static mach_timebase_info_data_t tb;
                double ps = dr.presentedTime;
                uint64_t now = xbox_HostNowNs(), mach_ns, on = 0;
                char b[32];
                if (!tb.denom)
                    mach_timebase_info(&tb);
                mach_ns = mach_absolute_time() * tb.numer / tb.denom;
                if (ps > 0)
                    on = now - mach_ns + (uint64_t)(ps * 1e9);
                snprintf(b, sizeof b, "%llu", (unsigned long long)on);
                xbox_LatRow('D', fl, now, b);
            }];
        }
        if (pt_on())
            pt_watch(cb, PT_SCALE);
        [cb commit];
        if (t_acq0)
            xbox_LatRow('P', flip, 0, NULL);
        [s_slot_cb[k] release];
        s_slot_cb[k] = [cb retain];
        if (shot) {
            [cb waitUntilCompleted];
            save_shot(shot, (uint32_t)dw, (uint32_t)dh, shot_path, flip);
            [shot release];
        }
        ok = 1;
    }
    return ok;
}

#endif /* __APPLE__ */

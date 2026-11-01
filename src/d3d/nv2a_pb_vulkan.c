/**
 * A Vulkan backend for the pushbuffer executor (struct nv2a_pb_backend): the
 * native-Linux sibling of nv2a_pb_metal.m, and a port of it. Where this file
 * does what the Metal backend does, it does it the same way and in the same
 * order, and the comments there apply; what differs is Vulkan's:
 *
 *   on_clear  -> a dynamic-rendering pass with loadOp CLEAR on the surface's
 *                render target (one per guest surface, grown to its largest
 *                clip); a clear of part of it (clip, clear rect) is
 *                vkCmdClearAttachments on the open pass
 *   on_draw   -> vertex-program batches through the program as GLSL
 *                (d3d8_vsh_glsl.c), pre-transformed ones through vs_pass;
 *                pixels through the register combiners as GLSL
 *                (d3d8_combiners_glsl.c) when programmed, else fs_basic.
 *                GLSL becomes SPIR-V with glslang (nv2a_vk_glsl.c, cached on
 *                disk); pipelines go through a VkPipelineCache saved to disk.
 *   on_flip   -> submit without waiting: two frames in flight, each with its
 *                own command buffers, rings and descriptor pools, reused once
 *                the timeline semaphore says the frame two flips back is
 *                done. With the SDL window's Vulkan presenter up
 *                (src/video/fb_present_vulkan.c, whose device this backend
 *                adopts), the surface the walker picked is copied into the
 *                window's back slot on the GPU and published: no CPU copy.
 *                xemu's Vulkan renderer waits for the frame at every flip
 *                (hw/xbox/nv2a/pgraph/vk/draw.c, pgraph_vk_finish); this does
 *                not.
 *   sync_guest -> write a surface back to guest memory when the executor
 *                dumps it, on demand (vk_writeback=lazy, the default) or at
 *                every flip (vk_writeback=always, vk_no_rtt), as Metal.
 *
 * Surfaces as the Metal backend: a render target per guest surface (RT_MAX
 * 32, toolkit 73ed271), every draw scissored to the surface clip (eedf538),
 * a dirty target written back before it is evicted (6407087), 16-bit
 * (R5G6B5) targets sampled directly through a view whose alpha reads 1, the
 * render-target ownership check (nv2a_rt_own) and render.scale. Visibility
 * tests through the shared tracker (nv2a_occ) with occlusion queries, one
 * per render pass a stretch spans, reset from the host.
 *
 * Coordinates: the emitters produce D3D-style clip space (y up, z 0..1), as
 * for Metal; the viewport has a negative height (Vulkan 1.1), so framebuffer
 * rows, scissors and winding are the Metal backend's as they are.
 *
 * Command buffers: the frame's main one, and an upload one submitted ahead
 * of it, which takes every copy into an image made for it (texture uploads,
 * seeds of new render targets): those can be recorded while a pass is open
 * on the main one, as Metal's replaceRegion can. Everything that touches an
 * image the main command buffer already uses goes there, outside a pass.
 *
 * Everything runs on the NV2A ack thread (or the smoke test's main thread),
 * except the window present, which only shares the queue (nvk_queue_lock).
 *
 * Opt-in: RECOMP_PB_BACKEND=vulkan (nv2a_pb_vulkan_register_from_env).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "nv2a_vk_core.h"
#include "nvr/nvr_capture.h"
#include "nv2a_vk_glsl.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "d3d8_glsl.h"
#include "../video/hdr_shaders.h"   /* BO-244: the HDR maths shared with Metal */
#include "d3d8_swizzle.h"
#include "nv2a_texrep.h"
#include "nv2a_texscale.h"
#include "nv2a_texmip.h"
#include "kernel_pause.h"
#include "kernel_pacing.h"
#include "nv2a_texup.h"
#include "recomp_exe_dir.h"
static void scale_prepare_loaded(void);
static void texup_cmd_begin(VkCommandBuffer cb);   /* BO-245 */
static void texup_cmd_end(VkCommandBuffer cb);
void xbox_FrametimeGpu(double gpu_ms, double tu_ms, double tu_pred);   /* kernel.h */
int  xbox_FrametimeOn(void);
static int ui_scale_log(void);
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "nv2a_pcache.h"
#include "recomp_env.h"
#include <pthread.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

ptrdiff_t xbox_GetMemoryOffset(void);


#define LOGP "[vulkan] "

extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);
extern int  xbox_FramebufferWindowRunning(void);
extern void xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                                uint32_t pitch, uint32_t bpp);
/* The Vulkan window (fb_present_vulkan.c through fb_present_sdl.c): 1 when
 * it presents slot images on the core's device; the back slot (a struct
 * nvk_image *) a frame is copied into, and its publish. */
extern int   xbox_FramebufferWindowVulkan(void);
extern void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h);
extern void  xbox_FramebufferWindowPublishTexture(void);

/* ---- shader ABI ------------------------------------------------------------ */

_Static_assert(sizeof(struct nv2a_vp_consts) == 80, "nv2a_vp_consts is not 5 x 16 bytes");
_Static_assert(sizeof(((struct nv2a_pb_vsh *)0)->c) == NV2A_VS_MAX_CONSTANTS * 16,
               "vertex constants are not 192 vec4");
_Static_assert(sizeof(struct nv2a_ps_consts) == 416, "nv2a_ps_consts size changed");

/* D3D8_GLSL_PSCONSTS: the shared constants, then tex_bias, std140. */
typedef struct {
    struct nv2a_ps_consts b;
    float tex_bias[4];
    float bump_mat[4][4];   /* per stage: SET_TEXTURE_SET_BUMP_ENV_MAT 00 01 10 11 */
    float bump_lum[4][4];   /* per stage: x bump-env luminance scale, y offset */
    float hdr[4];           /* BO-110: x the colour bound of the HDR combiners (unread by SDR ones) */
} VkPsConsts;
_Static_assert(offsetof(VkPsConsts, tex_bias) == 416 && offsetof(VkPsConsts, bump_mat) == 432
               && offsetof(VkPsConsts, bump_lum) == 496 && offsetof(VkPsConsts, hdr) == 560
               && sizeof(VkPsConsts) == 576,
               "VkPsConsts tail moved (D3D8_GLSL_PSCONSTS)");

#define VS_C_BYTES  (NV2A_VS_MAX_CONSTANTS * 16)
#define VP_C_BYTES  ((uint32_t)sizeof(struct nv2a_vp_consts))
#define PS_C_BYTES  ((uint32_t)sizeof(VkPsConsts))

/* One expanded vertex of a pass-through batch: attributes 0, 3, 4, 9..12,
 * as Metal's. */
#define VTX_F4 7
static const int s_vtx_attr[VTX_F4] = {0, 3, 4, 9, 10, 11, 12};

static const char s_vs_pass_glsl[] =
    "#version 450\n"
    "layout(set = 0, binding = 1, std140) uniform VpC { vec4 c[3]; uvec4 u[2]; } vp;\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_d0;\n"
    "layout(location = 2) in vec4 a_d1;\n"
    "layout(location = 3) in vec4 a_t0;\n"
    "layout(location = 4) in vec4 a_t1;\n"
    "layout(location = 5) in vec4 a_t2;\n"
    "layout(location = 6) in vec4 a_t3;\n"
    D3D8_GLSL_VS_OUT
    /* Attribute 0 is already in surface pixels (x, y, z, rhw): c[0] is
     * (2/w, -2/h, 1/zmax, 0) and c[1] (-1, 1, 0, 0), as Metal's vs_pass. */
    "void main() {\n"
    "    gl_Position = vec4(a_pos.xy * vp.c[0].xy + vp.c[1].xy,\n"
    "                       clamp(a_pos.z * vp.c[0].z + vp.c[0].w, 0.0, 1.0), 1.0);\n"
    "    o_d0 = a_d0; o_d1 = a_d1;\n"
    "    o_t0 = a_t0; o_t1 = a_t1; o_t2 = a_t2; o_t3 = a_t3;\n"
    "    o_fog = 1.0;\n"
    "    o_wz = gl_Position.z;\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";

/* Metal's fs_basic: MODULATE, then the alpha test. */
static const char s_fs_basic_glsl[] =
    "#version 450\n"
    D3D8_GLSL_PSCONSTS
    D3D8_GLSL_FS_IN
    "layout(set = 1, binding = 0) uniform sampler2D tex0;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 c = i_d0;\n"
    "    if (pc.tex_mode.x == 4u) {\n"
    "        c *= clamp(i_t0, 0.0, 1.0);\n"
    "    } else if (pc.tex_mode.x != 0u) {\n"
    "        vec2 uv = i_t0.xy;\n"
    "        if (pc.tex_mode.x == 1u && i_t0.w != 0.0 && i_t0.w != 1.0) uv /= i_t0.w;\n"
    "        c *= texture(tex0, uv * pc.tex_scale[0].xy, pc.tex_bias.x);\n"
    "    }\n"
    "    if (pc.atest.z != 0u) {\n"
    "        float ref = uintBitsToFloat(pc.atest.x);\n"
    "        uint a = uint(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5), r = uint(ref * 255.0 + 0.5);\n"
    "        bool ok = true;\n"
    "        uint f = pc.atest.y;\n"
    "        if      (f == 1u) ok = false;\n"
    "        else if (f == 2u) ok = a <  r;\n"
    "        else if (f == 3u) ok = a == r;\n"
    "        else if (f == 4u) ok = a <= r;\n"
    "        else if (f == 5u) ok = a >  r;\n"
    "        else if (f == 6u) ok = a != r;\n"
    "        else if (f == 7u) ok = a >= r;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    o_color = c;\n"
    "}\n";

/* ---- device ---------------------------------------------------------------- */

#define FRAMES_MAX 2
#define RT_FORMAT  VK_FORMAT_B8G8R8A8_UNORM   /* X8R8G8B8's bytes */

/* ---- HDR (BO-110), the Metal design (nv2a_pb_metal.m) ------------------------
 *
 * With HDR on, colour render targets are R16G16B16A16_SFLOAT: blending into
 * them does not clamp, so the title's additive light (lamps, sparks, brake
 * lights, streaks, the bloom's add) passes 1.0 by itself. The HDR variant of
 * the combiner shader (d3d8_combiners_glsl.c) takes its colour bound from
 * pc.hdr.x: s_hdr_hi between the title's scene marks (the 3D of a view,
 * nv2a_pb_stream_tag() == HDR_TAG_SCENE), 1.0 elsewhere (HUD, menus, front
 * end: paper white).
 *
 * The HUD apart, cheaper than Metal's snapshot + compare: when the first UI
 * draw follows a view's 3D (or the flip comes), a compute pass maps the
 * scene in place, only pixels past 1.0 written (or all when the HUD white
 * differs from paper white): a hue-kept shoulder on the brightest channel in
 * linear light, slope 1 at paper white, towards the peak, in units of the
 * HUD's white. The HUD and menus then draw over it at <= 1.0. The presenter
 * takes the frame as extended sRGB with 1.0 = the HUD's white and encodes it
 * for the display (HDR10 PQ). With HDR off nothing here runs: formats,
 * shaders, pipelines and usage flags are the SDR ones. */
#define RT_FORMAT_HDR VK_FORMAT_R16G16B16A16_SFLOAT
#define HDR_TAG_SCENE 1u
static int   s_hdr;               /* the colour targets are half float now */
static int   s_hdr_ab;            /* RECOMP_DEBUG=hdr_ab=n */
static float s_hdr_hi = 64.0f;
static uint64_t s_hdr_draws[2], s_hdr_maps;
#define RT_FMT (s_hdr ? RT_FORMAT_HDR : RT_FORMAT)
static uint16_t s_h8[256];        /* byte / 255 as a half float */

static const struct nvk_core *C;
static VkDevice               s_dev;
static int                    s_failed;
static int                    s_win;           /* the window takes slot images */
static uint32_t               s_max_dim;
static int                    s_frames = FRAMES_MAX;
static VkDeviceSize           s_ubo_align;

static VkSemaphore            s_timeline;
static uint64_t               s_next_serial = 1;   /* the next submit signals it */
static uint64_t               s_done_serial;       /* known complete */

static VkDescriptorSetLayout  s_dsl[2];
static VkPipelineLayout       s_playout;
static VkDescriptorPool       s_set0_pool;
static VkDescriptorSet        s_set0;
static VkPipelineCache        s_pcache;
static VkShaderModule         s_vs_pass, s_fs_basic;
static VkQueryPool            s_vkq;
static int                    s_qprecise;

/* Rings: one buffer each, FRAMES_MAX regions; a frame's region is reused
 * when its serial is done. Vertex and index as Metal's; uniform for the
 * three constant blocks of each draw; staging for uploads. */
enum { RING_VB, RING_IB, RING_UB, RING_UP, RING_N };
static struct nvk_buffer s_ring[RING_N];
static VkDeviceSize      s_ring_size[RING_N];      /* per frame */
static VkDeviceSize      s_ring_pos[RING_N];       /* within the frame's region */

typedef struct {
    VkCommandPool     pool;
    VkCommandBuffer  *cbs;
    uint32_t          ncbs, used;
    uint64_t          serial;          /* last submit while this frame was current */
    VkFence           fence;           /* vk_poll=0: signalled by the submit fence_serial */
    uint64_t          fence_serial;    /* 0: not armed */
    VkDescriptorPool *dps;
    uint32_t          ndps, dp_cur;
} Frame;
static Frame    s_fr[FRAMES_MAX];
static int      s_fi;                  /* the frame being recorded */

/* What the main command buffer has bound and set (vk_on_draw); nothing is
 * known of a new one. */
typedef struct {
    VkPipeline    pso;
    VkBool32      ztest, zwrite, stest;
    VkCompareOp   zcmp, scmp;
    VkStencilOp   sop[3];
    uint32_t      srmask, swmask, sref, vw, vh;
    float         slope, bf[4];
    VkRect2D      sr;
    VkFrontFace   front;
    VkCullModeFlags cull;
} DynState;
static DynState s_dyn;
static int      s_dyn_valid;

static VkCommandBuffer s_cmd;          /* main, begun lazily */
static VkCommandBuffer s_up;           /* uploads, submitted ahead of s_cmd */
static int             s_pass;         /* a render pass is open on s_cmd */

/* Resources an in-flight command buffer may still use, freed once the
 * serial they were last recorded under is done. */
typedef struct {
    struct nvk_image  im;
    VkImageView       view2;
    struct nvk_buffer buf;
    VkDescriptorPool  pool;           /* BO-245: an upscaler job's sets */
    uint64_t          serial;
} Garbage;
static Garbage *s_garbage;
static size_t   s_ngarbage, s_garbage_cap;

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_submit_errors,
                s_pso_created, s_pso_since_save, s_frame_waits, s_frame_wait_ns;

/* BO-128: what the command buffers of the next submit hold, for its line in
 * nvk_submit_note (printed when the device is lost). */
static struct {
    uint32_t draws, passes, zpasses, occ, d2, scaled, uploads, swaps, max_idx;
    uint32_t rt_w, rt_h;
} s_sn;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int vk_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_VK_TRACE) != NULL;
    return on;
}

/* ---- RECOMP_TRACE=vk_frame=<file> -------------------------------------------
 *
 * One CSV line per flip, to say what a frame cost the backend beyond its
 * draws: how many queue submits and why, how many render passes and how many
 * of them went back to a target the frame had already drawn (a tile-based
 * GPU stores and loads the whole target for each), and where the backend
 * called into the driver's waits. The frame's GPU time comes from a pair of
 * timestamp queries round each submit, read four flips later, so a line is
 * written that much after its frame. Off, each site is one load and a
 * branch (s_vf is NULL). */
enum { FL_FLIP, FL_READY, FL_REPORT, FL_POLL, FL_RING, FL_SAMPLER, FL_READBACK, FL_BATCH, FL_N };
#define VF_RING 8
#define VF_LAG  4
#define VF_TS   24                     /* submits a frame with a timestamp pair */
typedef struct {
    uint64_t present, t_ns;
    uint32_t flip, draws, passes, p_clear, p_load, p_resumed, p_empty, p_maxdraws,
             rt_switch, zload, zresumed, pass_draws,
             d_main, d_square, d_other;   /* draws by target: >= 640 wide, square, the rest */
    uint32_t sub[FL_N];
    uint32_t polls, waits, qres, nts, ready_done;
    uint64_t submit_ns, submit_max_ns, qlock_ns, poll_ns, poll_max_ns, wait_ns,
             qres_ns, endpass_ns, f_total, f_back, f_copy, f_submit, f_pub, f_adv;
} VkFrameRec;
static VkFrameRec   s_vfr[VF_RING];
static VkFrameRec  *s_vf;              /* the frame being recorded; NULL: off */
static FILE        *s_vf_out;
static VkQueryPool  s_vf_qp;
static int          s_vf_ts_open;      /* s_cmd has its start timestamp */
static void        *s_vf_last_rt;

/* ---- serials and garbage ---------------------------------------------------- */

/* How far the GPU is. With a timeline semaphore of the driver's own that is
 * one cheap question (vkGetSemaphoreCounterValue), asked wherever it helps.
 * Mesa's Turnip on Qualcomm's KGSL kernel interface (Android) has no such
 * semaphore: Mesa emulates it with a list of pending submits and answers the
 * question by "polling" each with IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID and
 * timeout 0 (vk_sync_timeline_gc_locked -> kgsl_syncobj_wait ->
 * wait_timestamp_safe, Mesa 26.2.4), and on the Retroid Pocket 5's 4.19
 * kernel that call does not return until the GPU has retired the submit:
 * measured 6 ms a call in a heavy frame at 2x, up to 11.9 ms (BO-85). Every
 * question was a wait for all the GPU work submitted so far, so the two
 * frames in flight never overlapped. A submit that signals such a semaphore
 * asks the same question of the oldest pending one
 * (vk_sync_timeline_alloc_point_locked), and waited as long. There
 * (vk_poll=0, the default on Android) the semaphore is not used and the
 * question is never asked: the last submit of a frame signals
 * the frame's fence, the fence is waited for when the frame's slot is taken
 * again (two flips on, long done), and what is known done is what such a
 * wait has shown. Garbage and occlusion slots are freed that much later;
 * occlusion counts are read from the query pool as soon as they are there. */
static int poll_ok(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_VK_POLL);
#if defined(__ANDROID__)
        on = e && *e == '1';
#else
        on = !(e && *e == '0');
#endif
    }
    return on;
}

static void serial_known(uint64_t v)
{
    if (v > s_done_serial)
        s_done_serial = v;
}

static uint64_t serial_done(void)
{
    uint64_t v = 0, t0;
    if (!poll_ok())
        return s_done_serial;
    t0 = s_vf ? now_ns() : 0;
    if (vkGetSemaphoreCounterValue(s_dev, s_timeline, &v) == VK_SUCCESS && v > s_done_serial)
        s_done_serial = v;
    if (s_vf) {
        uint64_t d = now_ns() - t0;
        s_vf->polls++;
        s_vf->poll_ns += d;
        if (d > s_vf->poll_max_ns)
            s_vf->poll_max_ns = d;
    }
    return s_done_serial;
}

/* A GPU wait that did not prove the work done: drawing stops for the run
 * (vk_on_* return early once s_failed is set) and, unless the run is
 * ending anyway (a cancelled wait), the device is reported lost, which tells
 * the player and ends the run (nvk_device_lost). Whatever the wait was for
 * is left as it is: not reset, reused, freed or read. */
static void wait_failed(int outcome, VkResult r, const char *what)
{
    if (!s_failed)
        fprintf(stderr, LOGP "%s: the GPU wait failed (%s): drawing stops\n", what,
                outcome == NVK_WAIT_TIMEOUT ? "timed out for good"
                : outcome == NVK_WAIT_CANCELLED ? "cancelled" : nvk_result_str(r));
    s_failed = 1;
    nvk_wait_fatal(outcome, r, what);
}

/* vk_poll=0: everything submitted so far is waited for through this fence,
 * an empty submit's (nvk_wait_queue): vkQueueWaitIdle, but bounded. */
static VkFence s_idle_fence;

/* Wait until the GPU has finished serial v. 1 when it is proven done; 0
 * when the wait failed (wait_failed has stopped drawing): the caller then
 * leaves alone whatever v's submits use. */
static int serial_wait(uint64_t v, int site)
{
    uint64_t t0;
    VkResult r = VK_SUCCESS;
    int out;
    if (!v || serial_done() >= v)
        return 1;
    if (s_failed)
        return 0;
    t0 = s_vf ? now_ns() : 0;
    if (!poll_ok()) {
        /* No semaphore to wait on (see poll_ok): everything submitted. A
         * readback or a full ring, not a frame's normal path. */
        uint64_t last = s_next_serial - 1;
        if (!s_idle_fence) {
            VkFenceCreateInfo fci;
            memset(&fci, 0, sizeof fci);
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(s_dev, &fci, NULL, &s_idle_fence) != VK_SUCCESS)
                s_idle_fence = VK_NULL_HANDLE;
        }
        if (s_idle_fence) {
            out = nvk_wait_queue(site, "backend queue", s_idle_fence, &r);
        } else {
            nvk_queue_lock();
            r = vkQueueWaitIdle(C->queue);
            nvk_queue_unlock();
            out = r == VK_SUCCESS ? NVK_WAIT_OK
                  : r == VK_ERROR_DEVICE_LOST ? NVK_WAIT_LOST : NVK_WAIT_ERROR;
        }
        if (out == NVK_WAIT_OK)
            serial_known(last);
    } else {
        out = nvk_wait_timeline(site, "backend serial", s_timeline, v, &r);
        if (out == NVK_WAIT_OK)
            serial_known(v);
    }
    if (s_vf) {
        s_vf->waits++;
        s_vf->wait_ns += now_ns() - t0;
    }
    if (out != NVK_WAIT_OK) {
        char what[96];
        snprintf(what, sizeof what, "backend wait for serial %llu", (unsigned long long)v);
        wait_failed(out, r, what);
        return 0;
    }
    serial_done();
    return 1;
}

static void garbage_push_(const struct nvk_image *im, VkImageView view2,
                          const struct nvk_buffer *buf, VkDescriptorPool pool)
{
    Garbage *g;
    if (s_ngarbage == s_garbage_cap) {
        size_t nc = s_garbage_cap ? s_garbage_cap * 2 : 64;
        Garbage *n = (Garbage *)realloc(s_garbage, nc * sizeof *n);
        if (!n) {
            /* Out of memory: wait for the GPU and free now; when the
             * wait fails, they are left (drawing has stopped). */
            if (!serial_wait(s_next_serial - 1, NVK_WAIT_SERIAL))
                return;
            if (im) { struct nvk_image t = *im; nvk_image_free(&t); }
            if (view2) vkDestroyImageView(s_dev, view2, NULL);
            if (buf) { struct nvk_buffer t = *buf; nvk_buffer_free(&t); }
            if (pool) vkDestroyDescriptorPool(s_dev, pool, NULL);
            return;
        }
        s_garbage = n;
        s_garbage_cap = nc;
    }
    g = &s_garbage[s_ngarbage++];
    memset(g, 0, sizeof *g);
    if (im)
        g->im = *im;
    g->view2 = view2;
    if (buf)
        g->buf = *buf;
    g->pool = pool;
    /* Whatever was recorded so far is in this or an earlier serial. */
    g->serial = s_next_serial;
}

static void garbage_push(const struct nvk_image *im, VkImageView view2,
                         const struct nvk_buffer *buf)
{
    garbage_push_(im, view2, buf, VK_NULL_HANDLE);
}

static void garbage_collect(void)
{
    uint64_t done = serial_done();
    size_t i = 0;
    while (i < s_ngarbage) {
        Garbage *g = &s_garbage[i];
        if (g->serial > done) {
            i++;
            continue;
        }
        if (g->view2)
            vkDestroyImageView(s_dev, g->view2, NULL);
        if (g->im.img || g->im.view)
            nvk_image_free(&g->im);
        if (g->buf.buf)
            nvk_buffer_free(&g->buf);
        if (g->pool)
            vkDestroyDescriptorPool(s_dev, g->pool, NULL);
        s_garbage[i] = s_garbage[--s_ngarbage];
    }
}

/* ---- command buffers ---------------------------------------------------------- */

static VkCommandBuffer cb_new(void)
{
    Frame *f = &s_fr[s_fi];
    VkCommandBufferBeginInfo bi;
    VkCommandBuffer cb;

    if (f->used == f->ncbs) {
        VkCommandBufferAllocateInfo ai;
        VkCommandBuffer *n = (VkCommandBuffer *)realloc(f->cbs, (f->ncbs + 8) * sizeof *n);
        if (!n)
            return VK_NULL_HANDLE;
        f->cbs = n;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 8;
        if (vkAllocateCommandBuffers(s_dev, &ai, f->cbs + f->ncbs) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        f->ncbs += 8;
    }
    cb = f->cbs[f->used++];
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void ensure_cmd(void)
{
    if (!s_cmd) {
        s_cmd = cb_new();
        s_dyn_valid = 0;
        if (s_cmd)
            texup_cmd_begin(s_cmd);   /* BO-245: the frame's GPU time */
        if (s_vf && s_vf_qp && s_cmd && s_vf->nts < VF_TS) {
            vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_vf_qp,
                                (uint32_t)((s_vf - s_vfr) * 2 * VF_TS + 2 * s_vf->nts));
            s_vf_ts_open = 1;
        }
    }
}

static void ensure_up(void)
{
    if (!s_up)
        s_up = cb_new();
}

/* Visibility tests: the slot counting on the open pass. */
static void occ_pass_begin(void);
static void occ_pass_end(void);
static void occ_after_wait(void);

typedef struct DepthTarget DepthTarget;
typedef struct RenderTarget RenderTarget;
static RenderTarget *s_enc_rt;   /* the target the open pass draws into */
static DepthTarget  *s_enc_dt;   /* and its depth attachment, or NULL */

static void end_encoder(void)
{
    if (s_pass) {
        occ_pass_end();
        if (s_vf) {
            uint64_t t0 = now_ns();
            vkCmdEndRendering(s_cmd);
            s_vf->endpass_ns += now_ns() - t0;
            if (!s_vf->pass_draws)
                s_vf->p_empty++;
            if (s_vf->pass_draws > s_vf->p_maxdraws)
                s_vf->p_maxdraws = s_vf->pass_draws;
        } else {
            vkCmdEndRendering(s_cmd);
        }
        s_pass = 0;
    }
    s_enc_rt = NULL;
    s_enc_dt = NULL;
}

/* Last uniform data placed, per block, so an unchanged block is not placed
 * again; reset with the ring. */
static struct {
    int          valid[3];
    VkDeviceSize off[3];
    uint8_t      vs[VS_C_BYTES];
    uint8_t      vp[sizeof(struct nv2a_vp_consts)];
    uint8_t      ps[sizeof(VkPsConsts)];
} s_umemo;
static VkDescriptorSet s_tset_last;      /* the last texture set, this frame */
static VkImageView     s_tset_views[4];
static VkSampler       s_tset_smp[4];

static void rings_reset(void)
{
    int k;
    for (k = 0; k < RING_N; k++)
        s_ring_pos[k] = 0;
    memset(s_umemo.valid, 0, sizeof s_umemo.valid);
}

/* End the open pass, submit what is recorded (uploads first) and, with
 * `wait`, block until the GPU is done with it. After a wait every ring is
 * free again. Returns 0 when drawing has stopped (a lost device, a failed
 * wait): then nothing the GPU may still use was reset, and nothing it
 * wrote is there to be read. */
static int s_flush_fence;    /* the next submit signals the frame's fence */

/* vk_batch: a frame's commands go to the GPU in parts, at the boundaries
 * between render passes, so it draws the first part while the rest is being
 * recorded. With one submit a frame the GPU began only when the CPU had
 * finished, and a frame heavy on both (dense traffic) had the two times one
 * after the other. Never in the middle of a pass: a pass cut in two is
 * stored and loaded again by a tile-based GPU. */
static uint64_t s_batch_draw0, s_batch_t0;   /* s_draws and the time at the last submit */
static int      s_report_defer;              /* a report's counts wait for the pass to end */
static uint64_t s_report_defer_t;

static unsigned batch_draws(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = recomp_env(RENV_VK_BATCH);
#if defined(__ANDROID__)
        n = e && *e ? atoi(e) : 300;
#else
        n = e && *e ? atoi(e) : 0;
#endif
        if (n < 0)
            n = 0;
    }
    return (unsigned)n;
}

static int flush(int wait, int why)
{
    VkCommandBuffer cbs[2];
    VkSubmitInfo si;
    VkTimelineSemaphoreSubmitInfo ti;
    uint64_t serial;
    uint32_t n = 0;
    uint64_t t0 = 0, t1 = 0;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r;

    end_encoder();
    if (!s_failed && nvk_core_lost()) {
        /* Lost where the window found it (a wait or a present of its own). */
        fprintf(stderr, LOGP "the device is reported lost: drawing stops\n");
        s_failed = 1;
    }
    if (s_failed) {
        /* The device is gone or a wait failed: nothing more is submitted. */
        s_cmd = VK_NULL_HANDLE;
        s_up = VK_NULL_HANDLE;
        s_flush_fence = 0;
        return 0;
    }
    if (s_flush_fence && (s_cmd || s_up) && s_fr[s_fi].fence && !s_fr[s_fi].fence_serial)
        fence = s_fr[s_fi].fence;
    s_flush_fence = 0;
    if (!s_cmd && !s_up) {
        if (wait) {
            if (!serial_wait(s_next_serial - 1, NVK_WAIT_SERIAL))
                return 0;
            rings_reset();
            s_flushes++;
            occ_after_wait();
        }
        return 1;
    }
    if (s_up) {
        vkEndCommandBuffer(s_up);
        cbs[n++] = s_up;
    }
    if (s_cmd) {
        if (s_vf_ts_open && s_vf && s_vf_qp) {
            vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_vf_qp,
                                (uint32_t)((s_vf - s_vfr) * 2 * VF_TS + 2 * s_vf->nts + 1));
            s_vf->nts++;
        }
        s_vf_ts_open = 0;
        texup_cmd_end(s_cmd);
        vkEndCommandBuffer(s_cmd);
        cbs[n++] = s_cmd;
    }
    serial = s_next_serial++;
    memset(&ti, 0, sizeof ti);
    ti.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &serial;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &ti;
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    if (poll_ok()) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_timeline;
    } else {
        si.pNext = NULL;
    }
    if (s_vf)
        t0 = now_ns();
    nvk_queue_lock();
    if (s_vf)
        t1 = now_ns();
    r = vkQueueSubmit(C->queue, 1, &si, fence);
    if (r == VK_SUCCESS) {
        static const char *const why_s[FL_N] = { "flip", "ready", "report", "poll", "ring",
                                                 "sampler", "readback", "batch" };
        char what[160];
        snprintf(what, sizeof what, "backend flip %llu %s%s: %u draws (%u 2D), %u scaled binds, %u passes"
                 " (%u with depth, last %ux%u), %u occlusion, %u uploads, %u swaps, max %u idx",
                 (unsigned long long)s_presents, why >= 0 && why < FL_N ? why_s[why] : "?",
                 fence ? " +fence" : "", s_sn.draws, s_sn.d2, s_sn.scaled, s_sn.passes,
                 s_sn.zpasses, s_sn.rt_w, s_sn.rt_h, s_sn.occ, s_sn.uploads, s_sn.swaps,
                 s_sn.max_idx);
        nvk_submit_note(what);
    }
    nvk_queue_unlock();
    memset(&s_sn, 0, sizeof s_sn);
    {
        /* RECOMP_DEBUG=vk_device_lost=<flip>: this submit counts as lost. */
        static long at = -2;
        if (at == -2) {
            const char *e = recomp_env(RENV_VK_DEVICE_LOST);
            at = e && *e ? atol(e) : -1;
        }
        if (at >= 0 && (long)s_presents >= at && !s_failed) {
            fprintf(stderr, LOGP "vk_device_lost=%ld: this submit is treated as lost\n", at);
            r = VK_ERROR_DEVICE_LOST;
        }
    }
    if (fence && r == VK_SUCCESS)
        s_fr[s_fi].fence_serial = serial;
    if (s_vf) {
        uint64_t d = now_ns() - t1;
        s_vf->sub[why]++;
        s_vf->qlock_ns += t1 - t0;
        s_vf->submit_ns += d;
        if (d > s_vf->submit_max_ns)
            s_vf->submit_max_ns = d;
    }
    if (r != VK_SUCCESS) {
        s_submit_errors++;
        if (s_submit_errors <= 4)
            XR_ERROR( LOGP "vkQueueSubmit failed: %s\n", nvk_result_str(r));
        if (r == VK_ERROR_DEVICE_LOST) {
            fprintf(stderr, LOGP "device lost: drawing stops\n");
            s_failed = 1;
            nvk_device_lost("vkQueueSubmit: VK_ERROR_DEVICE_LOST");
        } else {
            /* Out of memory: the batch did not go (the spec leaves its
             * semaphores and fence as they were), so its serial is the next
             * submit's; a wait for it would otherwise be on a value nothing
             * signals. The frame's fence was not taken (fence_serial 0). */
            s_next_serial = serial;
        }
    }
    if (r == VK_SUCCESS)
        s_fr[s_fi].serial = serial;
    s_cmd = VK_NULL_HANDLE;
    s_up = VK_NULL_HANDLE;
    s_flushes++;
    s_report_defer = 0;
    if (batch_draws()) {
        s_batch_draw0 = s_draws;
        s_batch_t0 = now_ns();
    }
    if (s_failed)
        return 0;
    if (wait) {
        /* After a submit that did not go: what went before it. */
        if (!serial_wait(r == VK_SUCCESS ? serial : serial - 1, NVK_WAIT_SERIAL))
            return 0;
        rings_reset();
        occ_after_wait();
    }
    return 1;
}

/* `bytes` from ring k, aligned; on overflow the frame so far is flushed
 * (and waited for) and the ring starts over. Returns the mapped pointer
 * and *off, the offset in the ring's buffer; NULL if it can never fit. */
static void *ring_alloc(int k, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize *off)
{
    VkDeviceSize at = (s_ring_pos[k] + align - 1) & ~(align - 1);
    if (bytes > s_ring_size[k])
        return NULL;
    if (at + bytes > s_ring_size[k]) {
        if (!flush(1, FL_RING))   /* resets every ring */
            return NULL;          /* or drawing has stopped */
        at = 0;
    }
    s_ring_pos[k] = at + bytes;
    *off = (VkDeviceSize)s_fi * s_ring_size[k] + at;
    return (uint8_t *)s_ring[k].map + *off;
}

/* ---- render targets: one per guest surface --------------------------------- */

/* Double-S.T.E.A.L 2 keeps five depth buffers and 59 colour surfaces in a
 * frame (its blur pyramid and shadow map); at 4 and 32 the least recently
 * used went every frame, and a depth buffer that goes loses its contents. */
#define DT_MAX 8
struct DepthTarget {
    uint32_t         zeta, w, h;
    uint32_t         hw, hh;     /* host size: w x h at render.scale */
    struct nvk_image im;
    VkImageView      sview;      /* its depth aspect, sampled by a shadow stage */
    uint32_t         zfmt;       /* SET_SURFACE_FORMAT's zeta format: 1 Z16, 2 Z24S8 */
    uint64_t         last_use;
    uint64_t         gen;        /* s_rt_seq at its last depth clear or depth draw */
    int              fresh;      /* contents undefined: the next pass clears */
    uint64_t         pass_flip;  /* vk_frame: s_presents + 1 at its last pass */
};

/* As Metal: 32 slots hold any frame Burnout 3 draws (14 surfaces in Crash
 * mode); xemu keeps an unbounded surface list. */
#define RT_MAX 64
struct RenderTarget {
    uint32_t         offset;     /* color_offset, as the title set it */
    uint32_t         addr;       /* its guest address (nv2a_pb_dma_resolve) */
    uint32_t         w, h;       /* guest size: the cache key, the mapping */
    uint32_t         hw, hh;     /* host size: the image (render.scale) */
    struct nvk_image im;
    VkImageView      opaque;     /* a view with alpha read as 1 (16-bit) */
    VkImageView      gb;         /* G8B8 read as the console stores it (rt_g8b8_view) */
    uint32_t         cfmt;       /* SET_SURFACE_FORMAT's colour field, last drawn as */
    uint64_t         last_use;
    DepthTarget     *dt;         /* the depth buffer its passes attach, or NULL */
    uint32_t         pitch;      /* the guest pitch it was made with */
    int              swz;        /* made as a swizzled surface (format type 2) */
    int              dirty;      /* drawn since its last write-back */
    uint64_t         gen;        /* s_rt_seq at its last clear or draw */
    int              presented;  /* ever picked by present_target */
    uint64_t         pass_flip;  /* vk_frame: s_presents + 1 at its last pass */
    struct nv2a_rt_own own;      /* is the guest memory under it still its own */
};

static RenderTarget  s_rt[RT_MAX];
static uint64_t      s_rt_seq;   /* bumped by every pass begun and draw recorded */
static DepthTarget   s_dt[DT_MAX];
/* Depth-only passes: their own colour images (surface_depth_only). */
#define NC_MAX 4
static RenderTarget  s_nocolor[NC_MAX];
static uint64_t      s_tick;
static uint64_t      s_zclears, s_draws_z, s_rtt_binds, s_self_copies, s_zcolor_binds;
static uint64_t      s_stale_drops, s_retired, s_decode_syncs;
static uint64_t      s_grows, s_partial_clears;

static int wb_always(void)
{
    const char *e = recomp_env(RENV_VK_WRITEBACK);
    return (e && !strcasecmp(e, "always")) || recomp_env(RENV_VK_NO_RTT) != NULL;
}

/* ---- render-target ownership: as Metal (nv2a_backend_common.h) ------------- */

static int alias_check_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_RT_ALIAS_CHECK);
        on = !(e && e[0] == '0');
    }
    return on;
}

static void rt_sync(RenderTarget *rt, uint32_t pitch);
static void rt_free(RenderTarget *rt);

static void rt_own_reset(RenderTarget *rt)
{
    if (!alias_check_on() || !rt->pitch)
        return;
    nv2a_rt_own_reset(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                      rt->pitch, rt->h, (uint32_t)s_presents);
}

static int rt_drop_if_stale(RenderTarget *rt, const char *why)
{
    static int said;
    if (!alias_check_on() || !rt->im.img || !rt->pitch)
        return 0;
    if (!nv2a_rt_own_stale(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                           rt->pitch, rt->h, (uint32_t)s_presents))
        return 0;
    s_stale_drops++;
    if (said < 20 || vk_trace()) {
        said++;
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target dropped (%s): the title"
                " rewrote its %u bytes; last drawn at flip %u, now %llu%s\n",
                rt->offset, rt->w, rt->h, why, rt->pitch * rt->h, rt->own.draw_flip,
                (unsigned long long)s_presents, said == 20 ? " (further drops: the"
                " present summary)" : "");
    }
    rt->dirty = 0;   /* the title's bytes win: no write-back */
    rt_free(rt);
    return 1;
}

static void rt_free(RenderTarget *rt)
{
    if (rt->dirty && rt->presented && rt_drop_if_stale(rt, "evict"))
        return;
    if (rt->dirty && rt->presented)
        rt_sync(rt, rt->pitch);
    if (s_enc_rt == rt)
        end_encoder();
    garbage_push(&rt->im, rt->opaque, NULL);
    if (rt->gb)
        garbage_push(NULL, rt->gb, NULL);
    memset(rt, 0, sizeof *rt);
}

static DepthTarget *depth_target(uint32_t w, uint32_t h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    int i;

    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (d->im.img && d->zeta == zeta && d->w == w && d->h == h) {
            d->last_use = s_tick;
            return d;
        }
        if (!d->im.img && !e)
            e = d;
        if (d->last_use < lru->last_use)
            lru = d;
    }
    if (!e) {
        e = lru;
        if (s_enc_dt == e)
            end_encoder();
        for (i = 0; i < RT_MAX; i++)
            if (s_rt[i].dt == e)
                s_rt[i].dt = NULL;
        for (i = 0; i < NC_MAX; i++)
            if (s_nocolor[i].dt == e)
                s_nocolor[i].dt = NULL;
        garbage_push(&e->im, e->sview, NULL);
        memset(e, 0, sizeof *e);
    }
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &e->hw, &e->hh);
    if (!nvk_image_new(&e->im, C->depth_format, e->hw, e->hh, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        return NULL;
    {
        /* The depth aspect alone, for a shadow stage (dt_for_texture). */
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = e->im.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = C->depth_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &e->sview) != VK_SUCCESS)
            e->sview = VK_NULL_HANDLE;
    }
    e->zeta = zeta;
    e->zfmt = (nv2a_pb_gpu_state()->format >> 4) & 0xF;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    e->fresh = 1;
    if (e->hw != w)
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer (%ux%u host)\n",
                zeta, w, h, e->hw, e->hh);
    else
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* A shadow stage's depth buffer: the depth target over the texture's bytes,
 * when this backend drew it (a depth-only pass, surface_depth_only), so the
 * stage samples the depth itself rather than guest memory nothing wrote
 * back. Not the depth buffer being drawn with now (a feedback loop): that
 * one, and any texture no depth target covers, is decoded from memory. */
static DepthTarget *dt_for_texture(const struct nv2a_stage *t)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int i;

    if (!t->offset || !nv2a_tex_depth_kind(t->color))
        return NULL;
    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (!d->im.img || !d->sview || ((d->zeta ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (((d->zeta ^ v->zeta_offset) & 0x07FFFFFFu) == 0
                && (v->depth_enable || v->stencil_enable))
            return NULL;
        return d;
    }
    return NULL;
}

/* A 32-bit linear colour texture over a Z24S8 depth buffer this backend
 * drew: the title reading depth as bytes. Double-S.T.E.A.L 2's depth of
 * field reads its 640x576 D24S8 buffer as LIN_A8R8G8B8 (alpha depth bits
 * 16-23, red 8-15, green 0-7) and looks the blur up from those bytes
 * (DPNDNT_AR). Depth is never written back, so guest memory has stale
 * bytes and the whole frame took the far plane's blur: the stage samples
 * the depth image and the shader packs it (tex_shadow 0x80). Not the depth
 * buffer being drawn with now, and not when a colour target at the address
 * was drawn since the depth was. */
static DepthTarget *dt_for_zcolor(const struct nv2a_stage *t)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t off = t->offset & 0x07FFFFFFu;
    int i, j;

    if (!off || (t->color != 0x12 && t->color != 0x1E))   /* LIN_A8R8G8B8, LIN_X8R8G8B8 */
        return NULL;
    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (!d->im.img || !d->sview || d->zfmt != 2 || (d->zeta & 0x07FFFFFFu) != off
                || (t->pitch && t->pitch != d->w * 4))
            continue;
        if (((d->zeta ^ v->zeta_offset) & 0x07FFFFFFu) == 0
                && (v->depth_enable || v->stencil_enable))
            return NULL;
        for (j = 0; j < RT_MAX; j++)
            if (s_rt[j].im.img && (s_rt[j].offset & 0x07FFFFFFu) == off && s_rt[j].gen > d->gen)
                return NULL;
        return d;
    }
    return NULL;
}

/* Open a pass on rt: with `clear`, loadOp CLEAR to c; else LOAD. As Metal's
 * begin_pass: the depth buffer rt->dt is attached when it has one, zmask
 * (bit 0 depth, bit 1 stencil) clears it to z / st, and a fresh one clears
 * to the far plane and stencil 0. */
static void begin_pass(RenderTarget *rt, int clear, const float c[4],
                       unsigned zmask, float z, uint32_t st)
{
    DepthTarget *dt = rt->dt;
    VkRenderingAttachmentInfo ca, da, sa;
    VkRenderingInfo ri;

    end_encoder();
    if (s_cmd && batch_draws()
            && (s_report_defer || s_draws - s_batch_draw0 >= batch_draws()
                || (s_draws != s_batch_draw0 && now_ns() - s_batch_t0 > 2000000ull)))
        flush(0, s_report_defer ? FL_REPORT : FL_BATCH);
    ensure_cmd();
    rt->dirty = 1;
    rt->gen = ++s_rt_seq;
    nv2a_rt_own_drawn(&rt->own, (uint32_t)s_presents);
    nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = rt->im.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear)
        memcpy(ca.clearValue.color.float32, c, 16);
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = rt->hw;
    ri.renderArea.extent.height = rt->hh;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (dt) {
        int zc = (zmask & 1u) || dt->fresh, sc = (zmask & 2u) || dt->fresh;
        nvk_image_to(s_cmd, &dt->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        memset(&da, 0, sizeof da);
        da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        da.imageView = dt->im.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sa = da;
        da.loadOp = zc ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.clearValue.depthStencil.depth = (zmask & 1u) ? z : 1.0f;
        sa.loadOp = sc ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.clearValue.depthStencil.stencil = (zmask & 2u) ? (st & 0xFFu) : 0u;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &sa;
        if (zc)
            dt->gen = rt->gen;
        dt->fresh = 0;
    }
    vkCmdBeginRendering(s_cmd, &ri);
    s_sn.passes++;
    s_sn.zpasses += dt != NULL;
    s_sn.rt_w = rt->hw;
    s_sn.rt_h = rt->hh;
    if (s_vf) {
        s_vf->passes++;
        s_vf->pass_draws = 0;
        if (clear)
            s_vf->p_clear++;
        else
            s_vf->p_load++;
        if (!clear && rt->pass_flip == s_presents + 1)
            s_vf->p_resumed++;
        if (s_vf_last_rt != (void *)rt)
            s_vf->rt_switch++;
        s_vf_last_rt = rt;
        rt->pass_flip = s_presents + 1;
        if (dt) {
            if (da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                s_vf->zload++;
                if (dt->pass_flip == s_presents + 1)
                    s_vf->zresumed++;
            }
            dt->pass_flip = s_presents + 1;
        }
    }
    s_pass = 1;
    s_enc_rt = rt;
    s_enc_dt = dt;
    occ_pass_begin();
}

static void begin_encoder(RenderTarget *rt, int clear, const float c[4])
{
    begin_pass(rt, clear, c, 0, 1.0f, 0);
}

/* Bytes into image `im` (level `level`, w x h, tightly packed BGRA8) through
 * the staging ring, on the upload command buffer: im must be new this
 * batch (nothing in s_cmd uses it yet). Leaves it in `final`. */
/* BO-110: BGRA8 bytes as RGBA half floats (n pixels). */
static void hdr_bgra_to_half(uint16_t *o, const uint8_t *p, size_t n)
{
    if (!s_h8[255]) {
        int i;
        for (i = 0; i < 256; i++) {
            /* byte / 255 in [0, 1] as an IEEE half, rounded to nearest. */
            float f = (float)i / 255.0f;
            uint32_t b;
            memcpy(&b, &f, 4);
            if (!i) { s_h8[i] = 0; continue; }
            {
                int e = (int)((b >> 23) & 0xFF) - 127 + 15;
                uint32_t m = b & 0x7FFFFFu;
                if (e <= 0) {           /* subnormal half */
                    m |= 0x800000u;
                    s_h8[i] = (uint16_t)((m >> (14 - e)) + ((m >> (13 - e)) & 1u));
                } else {
                    uint32_t h = ((uint32_t)e << 10) | (m >> 13);
                    if ((m >> 12) & 1u) h++;
                    s_h8[i] = (uint16_t)h;
                }
            }
        }
    }
    while (n--) {
        o[0] = s_h8[p[2]];
        o[1] = s_h8[p[1]];
        o[2] = s_h8[p[0]];
        o[3] = s_h8[p[3]];
        o += 4;
        p += 4;
    }
}

static float hdr_h2f(uint16_t h)
{
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FFu, b;
    float f;
    if (!e) return (float)m * (1.0f / 16777216.0f) * ((h & 0x8000u) ? -1.0f : 1.0f);
    if (e == 31) return (h & 0x8000u) ? -65504.0f : 65504.0f;
    b = ((uint32_t)(h & 0x8000u) << 16) | ((e - 15 + 127) << 23) | (m << 13);
    memcpy(&f, &b, 4);
    return f;
}

static int upload_layer(struct nvk_image *im, uint32_t layer, uint32_t level, uint32_t w,
                        uint32_t h, const void *px, VkImageLayout final, int last)
{
    VkDeviceSize off;
    VkBufferImageCopy bc;
    int half = im->format == RT_FORMAT_HDR;
    void *dst = ring_alloc(RING_UP, (VkDeviceSize)w * h * (half ? 8u : 4u), 16, &off);

    if (!dst)
        return 0;
    if (half)
        hdr_bgra_to_half((uint16_t *)dst, (const uint8_t *)px, (size_t)w * h);
    else
        memcpy(dst, px, (size_t)w * h * 4u);
    ensure_up();
    if (im->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.mipLevel = level;
    bc.imageSubresource.baseArrayLayer = layer;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_up, s_ring[RING_UP].buf, im->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    if (last)
        nvk_image_to(s_up, im, final);
    return 1;
}

static int upload_level(struct nvk_image *im, uint32_t level, uint32_t w, uint32_t h,
                        const void *px, VkImageLayout final, int last)
{
    return upload_layer(im, 0, level, w, h, px, final, last);
}

/* BO-110: the format of a colour target of w x h guest pixels with HDR on.
 * Half float where the scene's light is kept: a view (640 wide or more) and
 * the reflections' square faces; the title's small 2D targets (its bloom
 * levels, the radial blur's copy, HUD and front-end pieces) stay BGRA8: they
 * hold what they hold in SDR, at half the bytes, so the bloom adds back what
 * it adds in SDR instead of a low-resolution copy of the scene's light past
 * paper white (blocky round blobs around lamps and sign lights).
 * RECOMP_DEBUG=hdr_rt8=0: every target half float (as before). */
static VkFormat rt_format(uint32_t w, uint32_t h)
{
    static int small8 = -1;
    if (!s_hdr)
        return RT_FORMAT;
    if (small8 < 0) {
        const char *e = recomp_env(RENV_HDR_RT8);
        small8 = !(e && *e == '0');
    }
    {
        VkFormat f = !small8 || w >= 640 || w == h ? RT_FORMAT_HDR : RT_FORMAT;
        static int said;
        if (said++ < 12)
            fprintf(stderr, LOGP "HDR: colour target %ux%u %s\n", w, h,
                    f == RT_FORMAT_HDR ? "half float" : "BGRA8");
        return f;
    }
}

static int rt_image_new(struct nvk_image *im, uint32_t hw, uint32_t hh, uint32_t w, uint32_t h)
{
    return nvk_image_new(im, rt_format(w, h), hw, hh, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
}

/* im (w x h guest, hw x hh host, new) from the guest's bytes at addr when
 * the surface is 32-bit, as Metal's rt_seed; else cleared to transparent
 * black, as a new Metal target's margin is. On the upload command buffer;
 * im ends as a colour attachment. */
/* Swizzled render targets (BO-24, as Metal): a surface of format type 2 lies
 * in guest memory in the NV2A's Z order (d3d8_swizzle.h); the seed reads it
 * so and a write-back stores it so, through a linear staging copy. Only a
 * power-of-two size is a swizzled layout. */
static int rt_swz_ok(int swz, uint32_t w, uint32_t h)
{
    return swz && w && h && !(w & (w - 1)) && !(h & (h - 1));
}

static uint8_t *s_swz_lin;
static size_t   s_swz_cap;

static uint8_t *swz_staging(size_t bytes)
{
    if (s_swz_cap < bytes) {
        uint8_t *n = (uint8_t *)realloc(s_swz_lin, bytes);
        if (!n)
            return NULL;
        s_swz_lin = n;
        s_swz_cap = bytes;
    }
    return s_swz_lin;
}

static void rt_seed(struct nvk_image *im, uint32_t addr, uint32_t pitch, uint32_t w,
                    uint32_t h, uint32_t hw, uint32_t hh, int swz)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + addr;
    VkDeviceSize off;
    uint8_t *dst;

    if (pitch >= w * 4u && rt_swz_ok(swz, w, h)) {
        uint8_t *lin = swz_staging((size_t)w * h * 4u);
        if (lin) {
            xbox_unswizzle_rect(lin, src, w, h, 4);
            src = lin;
            pitch = w * 4u;
        }
    }

    if (pitch >= w * 4u && im->format == RT_FORMAT_HDR) {
        /* BO-110: a half-float target, seeded through a BGRA8 copy. */
        uint8_t *tmp = (uint8_t *)malloc((size_t)hw * hh * 4u);
        uint16_t *hp = tmp ? (uint16_t *)ring_alloc(RING_UP, (VkDeviceSize)hw * hh * 8u, 16, &off)
                           : NULL;
        if (hp) {
            VkBufferImageCopy bc;
            uint32_t y;
            if (hw == w) {
                for (y = 0; y < h; y++)
                    memcpy(tmp + (size_t)y * w * 4u, src + (size_t)y * pitch, (size_t)w * 4u);
            } else if (hw % w == 0 && hh == h * (hw / w)) {
                nv2a_upscale_nearest32(src, pitch, w, h, hw / w, tmp, hw * 4u);
            } else {
                nv2a_resample_nearest32(src, pitch, w, h, tmp, hw * 4u, hw, hh);
            }
            hdr_bgra_to_half(hp, tmp, (size_t)hw * hh);
            free(tmp);
            ensure_up();
            nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&bc, 0, sizeof bc);
            bc.bufferOffset = off;
            bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount = 1;
            bc.imageExtent.width = hw;
            bc.imageExtent.height = hh;
            bc.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s_up, s_ring[RING_UP].buf, im->img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
            nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            return;
        }
        free(tmp);
        pitch = 0;          /* cleared below */
    }
    if (pitch >= w * 4u
            && (dst = (uint8_t *)ring_alloc(RING_UP, (VkDeviceSize)hw * hh * 4u, 16, &off))) {
        VkBufferImageCopy bc;
        if (hw == w) {
            uint32_t y;
            for (y = 0; y < h; y++)
                memcpy(dst + (size_t)y * w * 4u, src + (size_t)y * pitch, (size_t)w * 4u);
        } else if (hw % w == 0 && hh == h * (hw / w)) {
            nv2a_upscale_nearest32(src, pitch, w, h, hw / w, dst, hw * 4u);
        } else {            /* 1.5x */
            nv2a_resample_nearest32(src, pitch, w, h, dst, hw * 4u, hw, hh);
        }
        ensure_up();
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.bufferOffset = off;
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = hw;
        bc.imageExtent.height = hh;
        bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_up, s_ring[RING_UP].buf, im->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    } else {
        VkClearColorValue z;
        VkImageSubresourceRange r;
        memset(&z, 0, sizeof z);
        memset(&r, 0, sizeof r);
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.levelCount = 1;
        r.layerCount = 1;
        ensure_up();
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(s_up, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
    }
    nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

/* rt made w x h (guest pixels), what it holds kept at its top left: Metal's
 * rt_grow. The new image is seeded on the upload command buffer, which runs
 * first, and the old contents are copied over it on the main one. */
static int rt_grow(RenderTarget *rt, uint32_t w, uint32_t h)
{
    static int said;
    struct nvk_image im;
    uint32_t hw, hh;
    int keep;

    /* The scale it has now against the scale it would get (in halves: 1.5x
     * is 3), by the sizes each gives its present guest size. */
    {
        uint32_t kw, kh;
        nv2a_host_size_x2(rt->w, rt->h, nv2a_host_render_scale_x2(), s_max_dim, &kw, &kh);
        keep = rt->hw == kw && rt->hh == kh;
    }
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &hw, &hh);
    if (!keep)
        rt_sync(rt, rt->pitch);
    end_encoder();
    if (!rt_image_new(&im, hw, hh, w, h))
        return 0;
    rt_seed(&im, rt->addr, rt->pitch, w, h, hw, hh, rt->swz);
    if (keep) {
        VkImageCopy ic;
        ensure_cmd();
        nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        nvk_image_to(s_cmd, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = rt->hw;
        ic.extent.height = rt->hh;
        ic.extent.depth = 1;
        vkCmdCopyImage(s_cmd, rt->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    }
    if (said++ < 20)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target grown to %ux%u\n",
                rt->offset, rt->w, rt->h, w, h);
    garbage_push(&rt->im, rt->opaque, NULL);
    rt->opaque = VK_NULL_HANDLE;
    if (rt->gb)
        garbage_push(NULL, rt->gb, NULL);
    rt->gb = VK_NULL_HANDLE;
    rt->im = im;
    rt->w = w;
    rt->h = h;
    rt->hw = hw;
    rt->hh = hh;
    rt->dt = NULL;
    rt_own_reset(rt);
    s_grows++;
    return 1;
}

/* rt (free) made as the target for the surface at offset, w x h guest,
 * seeded from the guest's bytes there. */
static int rt_make(RenderTarget *rt, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
                   int swz)
{
    uint32_t hw, hh;

    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &hw, &hh);
    if (!rt_image_new(&rt->im, hw, hh, w, h))
        return 0;
    rt->offset = offset;
    rt->addr = nv2a_pb_dma_resolve(offset);
    rt->w = w;
    rt->h = h;
    rt->hw = hw;
    rt->hh = hh;
    rt->pitch = pitch;
    rt->swz = swz;
    rt->last_use = s_tick;
    rt_own_reset(rt);
    rt_seed(&rt->im, rt->addr, pitch, w, h, hw, hh, rt->swz);
    if (hw != w)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target (%ux%u host)\n",
                offset, w, h, hw, hh);
    else
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n", offset, w, h);
    return 1;
}

/* The render target for the surface at color_offset: Metal's surface(),
 * rule for rule (see the comments there). */
static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    int i, swz = ((g->format >> 8) & 0xF) == 2;   /* the layout guest memory holds it in */

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            if (rt_drop_if_stale(rt, "reuse"))
                break;
            rt->last_use = s_tick;
            rt->swz = swz;   /* the guest layout for its next seed or write-back */
            rt->cfmt = g->format & 0xF;
            return rt;
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || rt->offset != g->color_offset || rt->pitch != g->pitch)
            continue;
        if (rt_drop_if_stale(rt, "reuse"))
            break;
        if ((rt->w >= w && rt->h >= h)
                || rt_grow(rt, rt->w > w ? rt->w : w, rt->h > h ? rt->h : h)) {
            rt->last_use = s_tick;
            rt->swz = swz;   /* the guest layout for its next seed or write-back */
            rt->cfmt = g->format & 0xF;
            return rt;
        }
        break;
    }
    if (alias_check_on()) {
        uint32_t addr = nv2a_pb_dma_resolve(g->color_offset);
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (!rt->im.img || !nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, addr, g->pitch * h))
                continue;
            if (rt_drop_if_stale(rt, "overlap"))
                continue;
            rt_sync(rt, rt->pitch);
            if (s_retired++ < 20)
                fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target retired:"
                        " 0x%08X %ux%u overlaps it\n", rt->offset, rt->w, rt->h,
                        g->color_offset, w, h);
            rt_free(rt);
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img && !free_rt)
            free_rt = rt;
        if (rt->last_use < lru->last_use)
            lru = rt;
    }
    if (!free_rt) {
        /* Out of slots: the least recently used target goes, written back
         * first, presented or not (6407087, BO-47). */
        free_rt = lru;
        if (free_rt->dirty && !rt_drop_if_stale(free_rt, "evict"))
            rt_sync(free_rt, free_rt->pitch);
        if (free_rt->im.img)
            rt_free(free_rt);
    }
    if (!rt_make(free_rt, g->color_offset, w, h, g->pitch, swz))
        return NULL;
    free_rt->cfmt = g->format & 0xF;
    return free_rt;
}

/* A depth-only pass (no colour surface: SET_SURFACE_COLOR_OFFSET 0, the
 * depth buffer written or tested) draws into a colour image of its own,
 * which no guest memory backs and nothing presents, samples or writes back;
 * its depth goes to the zeta surface's depth target as any pass's does.
 * surface() has nothing to give it, and every such draw was skipped:
 * Double-S.T.E.A.L 2 renders its shadow map so, into a 16-bit depth buffer
 * it then samples (dt_for_texture). */
static RenderTarget *surface_depth_only(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    uint32_t key = 0x80000000u | (v->zeta_offset & 0x07FFFFFFu);
    RenderTarget *rt = NULL, *lru = &s_nocolor[0];
    int i;

    if (g->color_offset || !v->zeta_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_tick++;
    for (i = 0; i < NC_MAX; i++) {
        RenderTarget *c = &s_nocolor[i];
        if (c->im.img && c->offset == key && c->w == w && c->h == h) {
            c->last_use = s_tick;
            return c;
        }
        if (!c->im.img && !rt)
            rt = c;
        if (c->last_use < lru->last_use)
            lru = c;
    }
    if (!rt) {
        rt = lru;
        if (s_enc_rt == rt)
            end_encoder();
        garbage_push(&rt->im, VK_NULL_HANDLE, NULL);
        memset(rt, 0, sizeof *rt);
    }
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &rt->hw, &rt->hh);
    if (!rt_image_new(&rt->im, rt->hw, rt->hh, w, h)) {
        memset(rt, 0, sizeof *rt);
        return NULL;
    }
    rt->offset = key;            /* a tag: no guest colour surface is there */
    rt->w = w;
    rt->h = h;
    rt->last_use = s_tick;
    fprintf(stderr, LOGP "zeta 0x%08X: depth-only pass, %ux%u colour scratch\n",
            v->zeta_offset, w, h);
    return rt;
}

/* ---- visibility tests (declared early: clears pause the count) --------------- */

#define VIS_SLOTS  8192
#define OCC_QSLOTS 16
#define OCC_POOL   (NV2A_OCC_PENDING * 2 + NV2A_OCC_SEG)
typedef struct OccQuery {
    int              slot[OCC_QSLOTS];
    uint64_t         serial[OCC_QSLOTS];   /* the submit it is recorded in */
    int              n, ovf;
    struct OccQuery *next;                 /* free list */
} OccQuery;
static OccQuery         *s_occ_q;          /* the query counting now, or NULL */
static int               s_occ_slot = -1;  /* its slot on the open pass */

/* ---- clears -------------------------------------------------------------------- */

/* A colour clear covers its clip and clear rect only (nv2a_clear_box), as on
 * Metal: a box that is the whole target is a clearing pass, a smaller one
 * vkCmdClearAttachments on a loading pass. A clear is not a zpass sample:
 * the query counting on the pass is ended around it and the count goes on
 * in a new slot. */
static void vk_on_clear(uint32_t param);
static int init(void);
static void occ_query_begin(void);

/* A CLEAR_SURFACE of depth and colour reaches the backend as on_zclear and
 * then on_clear. Each used to open its own pass: one that cleared the depth
 * buffer and loaded the colour, ended at once without a draw, then one that
 * cleared the colour and loaded the depth; on a tile-based GPU the second
 * loads a whole buffer that was just cleared. on_zclear leaves its clear
 * here when on_clear is to follow, and on_clear opens one pass that clears
 * both (or, when its box is not the whole target, the depth-clearing pass
 * as before). */
static struct {
    RenderTarget *rt;
    unsigned      zmask;
    float         z;
    uint32_t      st;
} s_zpend;

static void zpend_flush(void)
{
    if (s_zpend.rt) {
        RenderTarget *rt = s_zpend.rt;
        s_zpend.rt = NULL;
        begin_pass(rt, 0, NULL, s_zpend.zmask, s_zpend.z, s_zpend.st);
    }
}

static void vk_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    float c[4];
    uint32_t b[4];
    VkClearAttachment ca;
    VkClearRect cr;

    if (!(param & 0xF0) || !init())
        return;
    rt = surface();
    if (!rt || !nv2a_clear_box(g, rt->w, rt->h, b)) {
        zpend_flush();
        return;
    }
    c[0] = (float)((g->clear_color >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((g->clear_color >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(g->clear_color & 0xFF) / 255.0f;
    c[3] = (float)(g->clear_color >> 24) / 255.0f;
    s_clears++;
    if (b[0] == 0 && b[1] == 0 && b[2] == rt->w && b[3] == rt->h) {
        if (s_zpend.rt == rt) {
            s_zpend.rt = NULL;
            begin_pass(rt, 1, c, s_zpend.zmask, s_zpend.z, s_zpend.st);
        } else {
            zpend_flush();
            begin_encoder(rt, 1, c);
        }
        return;
    }
    zpend_flush();
    if (!s_pass || s_enc_rt != rt)
        begin_encoder(rt, 0, NULL);
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    memcpy(ca.clearValue.color.float32, c, 16);
    memset(&cr, 0, sizeof cr);
    cr.rect.offset.x = (int32_t)((uint64_t)b[0] * rt->hw / rt->w);
    cr.rect.offset.y = (int32_t)((uint64_t)b[1] * rt->hh / rt->h);
    cr.rect.extent.width = (uint32_t)((uint64_t)b[2] * rt->hw / rt->w) - (uint32_t)cr.rect.offset.x;
    cr.rect.extent.height = (uint32_t)((uint64_t)b[3] * rt->hh / rt->h) - (uint32_t)cr.rect.offset.y;
    cr.layerCount = 1;
    if (!cr.rect.extent.width || !cr.rect.extent.height)
        return;
    if (s_occ_slot >= 0) {
        occ_pass_end();
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
        occ_query_begin();
    } else {
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
    }
    s_partial_clears++;
}

static void vk_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;

    if (!(param & 3u) || !init())
        return;
    rt = surface();
    if (!rt)
        rt = surface_depth_only();
    if (!rt)
        return;
    dt = depth_target(rt->w, rt->h);
    if (!dt)
        return;
    if (param & 1u)
        s_zclears++;
    zpend_flush();
    if (param & 0xF0) {
        /* on_clear follows for the same CLEAR_SURFACE and opens the pass. */
        rt->dt = dt;
        s_zpend.rt = rt;
        s_zpend.zmask = param & 3u;
        s_zpend.z = nv2a_zclear_depth(g->format, v->zclear);
        s_zpend.st = v->zclear & 0xFFu;
        return;
    }
    if (s_pass && s_enc_rt == rt && s_enc_dt == dt && rt->dt == dt) {
        /* The pass on this target is open with this depth buffer: clear in
         * it. Ending it for a clearing pass would store and load the colour
         * for nothing. As a colour clear, not a zpass sample. */
        VkClearAttachment ca;
        VkClearRect cr;
        int counting = s_occ_slot >= 0;
        memset(&ca, 0, sizeof ca);
        ca.aspectMask = ((param & 1u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                      | ((param & 2u) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil.depth = nv2a_zclear_depth(g->format, v->zclear);
        ca.clearValue.depthStencil.stencil = v->zclear & 0xFFu;
        memset(&cr, 0, sizeof cr);
        cr.rect.extent.width = rt->hw < dt->hw ? rt->hw : dt->hw;
        cr.rect.extent.height = rt->hh < dt->hh ? rt->hh : dt->hh;
        cr.layerCount = 1;
        if (counting)
            occ_pass_end();
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
        if (counting)
            occ_query_begin();
        rt->gen = ++s_rt_seq;
        if (param & 1u)
            dt->gen = rt->gen;
        return;
    }
    rt->dt = dt;
    begin_pass(rt, 0, NULL, param & 3u, nv2a_zclear_depth(g->format, v->zclear),
               v->zclear & 0xFFu);
}

/* ---- shaders -------------------------------------------------------------------- */

static uint64_t s_compile_errors, s_vs_compiled, s_fs_compiled, s_glsl_compiles;
#define COUNT(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)

/* ---- the shader and pipeline tables, and who builds into them --------------------
 *
 * s_vs, s_fs and s_pso are filled by two threads: the ack thread, with what
 * a draw needs and the tables lack, and the pre-warm thread (pc_prewarm),
 * with the key list from disk, so that a start with a list builds nothing on
 * the ack thread (as the Metal backend). s_pc_mu guards the tables' slots; an
 * entry never moves or goes once it is in, so the ack thread keeps its memos
 * without the lock. Every build is counted by thread and kind
 * (RECOMP_TRACE=pcache logs each, =<file> adds one CSV line a flip). */
static pthread_mutex_t s_pc_mu = PTHREAD_MUTEX_INITIALIZER;
enum { PC_VS = 0, PC_FS = 1, PC_PSO = 2,
       PC_FS_HDR = 3 };   /* BO-110: a combiner state's HDR variant (a build without HDR skips the type) */
static const char *const s_pc_kind[3] = { "vs", "fs", "pso" };
static struct {
    uint64_t n[2][3], ns[2][3];          /* [ack thread, pre-warm][kind] */
    uint64_t rt_flip_n, rt_flip_ns;      /* ack thread, since the last flip */
    uint64_t rt_10s;                     /* ack thread, first 10 s of the process */
    double   rt_max_ms;
} s_cs;
static int      s_pc_trace;              /* RECOMP_TRACE=pcache */
static FILE    *s_pc_csv;                /* RECOMP_TRACE=pcache=<file> */
static uint64_t s_t0_ns;                 /* process start, for the trace */

static uint64_t now_ns(void);
__attribute__((constructor)) static void pc_t0(void)
{
    s_t0_ns = now_ns();
}

/* One build done: counted, and logged under RECOMP_TRACE=pcache. */
static void pc_note(int prewarm, int kind, uint64_t t_start, const char *what)
{
    uint64_t t = now_ns(), ns = t - t_start;
    double ms = (double)ns / 1e6, at = (double)(t - s_t0_ns) / 1e9;
    pthread_mutex_lock(&s_pc_mu);
    s_cs.n[prewarm][kind]++;
    s_cs.ns[prewarm][kind] += ns;
    if (!prewarm) {
        s_cs.rt_flip_n++;
        s_cs.rt_flip_ns += ns;
        if (at < 10.0)
            s_cs.rt_10s++;
        if (ms > s_cs.rt_max_ms)
            s_cs.rt_max_ms = ms;
    }
    nv2a_pcache_stat_built(prewarm, kind, ns);
    pthread_mutex_unlock(&s_pc_mu);
    if (s_pc_trace)
        fprintf(stderr, LOGP "pcache: compiled %s %s on the %s thread: %.2f ms at %.3f s\n",
                s_pc_kind[kind], what, prewarm ? "pre-warm" : "ack", ms, at);
}

/* The ack thread missed while the start-up pre-warm is still on the
 * pipelines of the key list and their shaders (the first frames of a start,
 * which race it): what it needs is most likely on its way, so it waits for
 * that key instead of compiling the same thing beside it (pc_wait_more:
 * look again every half millisecond until it is there, the pipelines' part
 * is done, or the time is up).
 *
 * Bounded so that it cannot reach gameplay: only the pre-warm the backend
 * starts with has this part (a later one, after a pre-build has added to the
 * list, is never waited for), and all waits together end 3 s after it
 * started, which is 3 s after the title's first draw. Past that the ack
 * thread compiles what it needs itself, as without a list. Each wait is
 * logged. */
static int      s_pc_warming;           /* a pre-warm thread is running (atomic) */
static int      s_pc_warm_urgent;       /* the first one is on the pipelines' part (atomic) */
static uint64_t s_pc_wait_until;        /* the ack thread's waits end here */
static uint64_t s_pc_waits, s_pc_wait_ns;
#define PC_WARMING() __atomic_load_n(&s_pc_warm_urgent, __ATOMIC_ACQUIRE)
static uint64_t s_pc_cap_ns = 180000000000ull;  /* BO-208: the most the title is held, in all */
static uint64_t s_pc_hold_t0;                    /* when the first pre-warm began */
static int pc_wait_more(void)
{
    uint64_t now;
    if (!PC_WARMING())
        return 0;
    now = now_ns();
    if (now >= __atomic_load_n(&s_pc_wait_until, __ATOMIC_ACQUIRE)) {
        /* BO-208: past the first 3 s, still holding while the pre-warm is doing
         * real compiling (a driver, OS or shader-generator change that the
         * lists could not tell beforehand): a build with the caches warm is
         * lookups and has spent far less than this by now. */
        uint64_t busy = s_cs.ns[1][0] + s_cs.ns[1][1] + s_cs.ns[1][2];
        if (now >= s_pc_hold_t0 + s_pc_cap_ns || busy < 400000000ull)
            return 0;
        __atomic_store_n(&s_pc_wait_until, now + 1000000000ull, __ATOMIC_RELEASE);
    }
    nv2a_pcache_stat_waiting(1);        /* the window shows a line meanwhile */
    usleep(500);
    return 1;
}
static void pc_waited(uint64_t t0, const char *what)
{
    uint64_t t = now_ns();
    nv2a_pcache_stat_waiting(0);
    s_pc_waits++;
    s_pc_wait_ns += t - t0;
    fprintf(stderr, LOGP "pcache: the ack thread waited %.1f ms for the pre-warm's %s"
            " at %.3f s\n", (double)(t - t0) / 1e6, what, (double)(t - s_t0_ns) / 1e9);
}

/* Any thread. */
static VkShaderModule module_from_glsl(enum nvk_stage stage, const char *src, const char *what)
{
    char err[2048];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    if (!nvk_glsl_spirv(stage, src, &w, &n, &compiled, err, sizeof err)) {
        if (COUNT(s_compile_errors) <= 4)
            XR_ERROR( LOGP "\x1b[1;31m%s: GLSL compile failed: %s\x1b[0m\n%s\n",
                    what, err, src);
        return VK_NULL_HANDLE;
    }
    if (compiled)
        COUNT(s_glsl_compiles);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(s_dev, &ci, NULL, &m) != VK_SUCCESS) {
        COUNT(s_compile_errors);
        XR_ERROR( LOGP "%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    if (vk_trace())
        fprintf(stderr, LOGP "%s: %s (%zu words)\n", what,
                compiled ? "compiled" : "from the SPIR-V cache", n);
    return m;
}

#define VS_MAX 2048
typedef struct {
    uint32_t       hash;
    uint16_t       inputs;
    int            used, bad;
    uint64_t       kid;       /* nv2a_fnv64 of the slots: the disk key's id */
    VkShaderModule mod;
} VsEntry;
static VsEntry s_vs[VS_MAX];

/* The key list on disk (nv2a_pcache.h, the Metal pipeline cache's format):
 * what was built, as keys (types PC_VS, PC_FS, PC_PSO), so the next start
 * builds it before the title draws with it (pc_prewarm). */
#define PC_VS_PASS  1ull      /* ids of the two built-in shaders */
#define PC_FS_BASIC 2ull
static nv2a_pcache *s_pcf;
static void pc_put(uint32_t type, const void *data, uint32_t len);
static void pc_warm_begin(int first);

/* The module for `n` slots of program (up to its FINAL), or VK_NULL_HANDLE;
 * its inputs in *inputs. Any thread: `parsed` and `glsl` (65536) are the
 * caller's. */
static VkShaderModule vs_build(const uint32_t *slots, int n, uint32_t hash, uint16_t *inputs,
                               NV2AVshProgram *parsed, char *glsl, int prewarm)
{
    VkShaderModule m = VK_NULL_HANDLE;
    uint64_t t = now_ns();
    char what[32];

    snprintf(what, sizeof what, "program %08X", hash);
    d3d8_vsh_parse(slots, n, parsed);
    if (d3d8_vsh_generate_glsl(parsed, D3D8_GLSL_SCREEN_SPACE, glsl, 65536) <= 0) {
        COUNT(s_compile_errors);
        fprintf(stderr, LOGP "%s: no GLSL\n", what);
    } else if ((m = module_from_glsl(NVK_STAGE_VERTEX, glsl, what)) != VK_NULL_HANDLE) {
        *inputs = parsed->inputs_read;
        COUNT(s_vs_compiled);
        pc_note(prewarm, PC_VS, t, what);
    }
    return m;
}

/* s_pc_mu held. */
static VsEntry *vs_find(uint32_t hash)
{
    int i;
    for (i = 0; i < VS_MAX; i++)
        if (s_vs[i].used && s_vs[i].hash == hash)
            return &s_vs[i];
    return NULL;
}

/* s_pc_mu held. A free entry taken for `hash`, or NULL when the table is full. */
static VsEntry *vs_take(uint32_t hash, uint64_t kid, VkShaderModule mod, uint16_t inputs)
{
    int i;
    for (i = 0; i < VS_MAX && s_vs[i].used; i++)
        ;
    if (i == VS_MAX)
        return NULL;
    s_vs[i].hash = hash;
    s_vs[i].kid = kid;
    s_vs[i].mod = mod;
    s_vs[i].inputs = inputs;
    s_vs[i].bad = mod ? 0 : 1;
    s_vs[i].used = 1;
    return &s_vs[i];
}

/* The id of a vertex program's key: nv2a_fnv64 of its slots. */
static uint64_t vs_kid(const void *slots, size_t bytes)
{
    uint64_t kid = nv2a_fnv64(slots, bytes, NV2A_FNV64_INIT);
    return kid <= PC_FS_BASIC ? kid + 3 : kid;
}

/* As Metal's vs_cache: the program from its start slot, parsed, emitted as
 * GLSL with the screen-space epilogue, keyed by nv2a_vsh_program_hash. */
static VsEntry *vs_cache(void)
{
    static NV2AVshProgram parsed;
    static char glsl[65536];
    static struct { int valid; uint32_t dwords, start; VsEntry *e; } memo;
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash, len = 0;
    VsEntry *e;

    if (memo.valid && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    hash = nv2a_vsh_program_hash(v, &len);
    if (!hash)
        hash = 1;
    pthread_mutex_lock(&s_pc_mu);
    e = vs_find(hash);
    pthread_mutex_unlock(&s_pc_mu);
    if (!e && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            e = vs_find(hash);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!e && pc_wait_more());
        pc_waited(t0, "vertex shader");
    }
    if (!e) {
        const uint32_t *slots = &v->prog[v->start][0];
        uint16_t inputs = 0;
        VkShaderModule mod;
        if (!len || len > NV2A_VSH_SLOTS - v->start)
            len = NV2A_VSH_SLOTS - v->start;
        mod = vs_build(slots, (int)len, hash, &inputs, &parsed, glsl, 0);
        pthread_mutex_lock(&s_pc_mu);
        if ((e = vs_find(hash)) != NULL) {
            if (mod)                    /* the pre-warm thread was first */
                vkDestroyShaderModule(s_dev, mod, NULL);
            mod = VK_NULL_HANDLE;
        } else {
            e = vs_take(hash, vs_kid(slots, (size_t)len * 16u), mod, inputs);
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (!e)
            return NULL;                /* the table is full */
        if (mod) {
            pc_put(PC_VS, slots, len * 16u);
            pc_warm_begin(0);           /* BO-208: the manifest's pipelines for a program only the title assembles */
        }
    }
    memo.valid = 1;
    memo.dwords = v->prog_dwords;
    memo.start = v->start;
    memo.e = e->bad ? NULL : e;
    return memo.e;
}

#define FS_MAX 2048
typedef struct {
    uint32_t          hash;
    int               bad;
    int               hdr;      /* BO-110: the HDR variant */
    uint64_t          kid;
    NV2ACombinerState st;
    VkShaderModule    mod;
} FsEntry;
static FsEntry *s_fs;         /* FS_MAX, allocated in init */
static int      s_fs_n;

/* The module for combiner state `st`, or VK_NULL_HANDLE. Any thread: `glsl`
 * (65536) is the caller's. */
static VkShaderModule fs_build(const NV2ACombinerState *st, uint32_t hash, char *glsl,
                               int prewarm, int hdr)
{
    VkShaderModule m = VK_NULL_HANDLE;
    uint64_t t = now_ns();
    char what[32];

    snprintf(what, sizeof what, "combiners %08X", hash);
    if ((hdr ? d3d8_combiners_generate_glsl_hdr(st, glsl, 65536)
             : d3d8_combiners_generate_glsl(st, glsl, 65536)) <= 0) {
        COUNT(s_compile_errors);
        fprintf(stderr, LOGP "%s: no GLSL\n", what);
    } else if ((m = module_from_glsl(NVK_STAGE_FRAGMENT, glsl, what)) != VK_NULL_HANDLE) {
        COUNT(s_fs_compiled);
        pc_note(prewarm, PC_FS, t, what);
    }
    return m;
}

/* s_pc_mu held. */
static FsEntry *fs_find(uint32_t hash, const NV2ACombinerState *st, int hdr)
{
    int n;
    for (n = 0; n < s_fs_n; n++)
        if (s_fs[n].hash == hash && s_fs[n].hdr == hdr && !memcmp(&s_fs[n].st, st, sizeof *st))
            return &s_fs[n];
    return NULL;
}

/* The id of a combiner state's key (its HDR variant's apart: the pipelines'
 * records name the one they use). */
static uint64_t fs_kid(const void *st, size_t bytes, int hdr)
{
    uint64_t kid = nv2a_fnv64(st, bytes, NV2A_FNV64_INIT);
    if (hdr)
        kid ^= 0x4844520000000000ull;
    return kid <= PC_FS_BASIC ? kid + 3 : kid;
}

/* The HDR variant's id from the SDR one's (fs_kid; its guard aside). */
static uint64_t fs_kid_hdr_of(uint64_t sdr_kid)
{
    uint64_t kid = sdr_kid ^ 0x4844520000000000ull;
    return kid <= PC_FS_BASIC ? kid + 3 : kid;
}

/* s_pc_mu held. The next entry taken for `st`, or NULL when the table is full. */
static FsEntry *fs_take(uint32_t hash, const NV2ACombinerState *st, VkShaderModule mod, int hdr)
{
    FsEntry *e;
    if (s_fs_n == FS_MAX)
        return NULL;
    e = &s_fs[s_fs_n];
    e->hash = hash;
    e->st = *st;
    e->mod = mod;
    e->hdr = hdr;
    e->bad = mod ? 0 : 1;
    e->kid = fs_kid(st, sizeof *st, hdr);
    s_fs_n++;               /* last: fs_find reads up to it */
    return e;
}

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *b++;
        h *= 0x01000193u;
    }
    return h;
}

typedef struct nv2a_stage Stage;
static Stage s_stage[4];

/* As Metal's fs_select: the combiners when programmed, else fs_basic; only
 * 2D textures are bound, so every sampling mode goes to the combiners as 2D. */
/* RECOMP_DEBUG=vk_fs_show=<stage>@<offset>: the draws into that target show
 * a stage's texture coordinate (combiner state debug_show). */
static uint8_t s_fs_show_now;

static uint8_t fs_show_for(const RenderTarget *rt)
{
    static int init;
    static uint32_t stage, off;
    if (!init) {
        const char *e = recomp_env(RENV_VK_FS_SHOW);
        init = 1;
        if (e && strchr(e, '@')) {
            stage = !strncmp(e, "lod", 3) ? (uint32_t)strtoul(e + 3, NULL, 0) + 5u
                  : (uint32_t)strtoul(e, NULL, 0) + 1u;
            off = (uint32_t)strtoul(strchr(e, '@') + 1, NULL, 16) & 0x07FFFFFFu;
        }
    }
    return (uint8_t)(stage && stage <= 8 && rt && (rt->offset & 0x07FFFFFFu) == off ? stage : 0);
}

static VkShaderModule fs_select(const struct nv2a_pb_vsh *v)
{
    static char glsl[65536];
    /* The registers a module was chosen for, 64 of them by a hash of the
     * key: a race frame switches between a few dozen combiner setups, and a
     * single remembered one sent most draws to the state's decode, a byte-wise
     * hash of it and the table's linear search. */
    static struct { int valid; uint32_t r[41]; VkShaderModule m; } memo[64];
    NV2ACombinerState st;
    uint32_t key[41], modes = 0, signs = 0, shadows = 0, hash, kh = 2166136261u;
    FsEntry *e;
    int n, wbuf;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        uint32_t m = (uint32_t)s_stage[n].mode;
        if (s_stage[n].raw_mode == 5)
            m = 5;
        else if (m == 3 && s_stage[n].cube)
            m = 3;          /* CUBEMAP: a samplerCube (cube_bind) */
        else if ((m == 6 || m == 7) && n > 0)
            ;               /* BUMPENVMAP(_LUM): offset by the previous stage */
        else if ((s_stage[n].raw_mode == 15 || s_stage[n].raw_mode == 16) && n > 0)
            m = (uint32_t)s_stage[n].raw_mode;   /* DPNDNT_AR / _GB */
        else if (s_stage[n].raw_mode == 17 && (n == 1 || n == 2))
            m = 17;         /* DOTPRODUCT: for the DOT_ST after it */
        else if (s_stage[n].raw_mode == 9 && n >= 2)
            m = 9;          /* DOT_ST */
        else if (m && m != 4) m = 1;
        modes |= m << (5 * n);
        if (m == 1 || m == 6 || m == 7 || m == 9 || m == 15 || m == 16)
            signs |= (uint32_t)nv2a_tex_signed_mask(s_stage[n].filter) << (4 * n);
        /* A depth-format texture on a 2D or 3D-projective stage: a shadow
         * buffer (d3d8_combiners_parse.h tex_shadow). */
        if (m == 1 && (s_stage[n].raw_mode == 1 || s_stage[n].raw_mode == 2)) {
            int kind = nv2a_tex_depth_kind(s_stage[n].color);
            if (kind)
                shadows |= (uint32_t)(kind | ((v->shadow_func & 7u) << 2)
                                      | (s_stage[n].raw_mode == 2 ? 0x20u : 0u)
                                      | (dt_for_texture(&s_stage[n]) ? 0x40u : 0u)) << (8 * n);
            else if (s_stage[n].raw_mode == 1 && dt_for_zcolor(&s_stage[n]))
                /* depth read as colour bytes: 0x04 the bytes filtered */
                shadows |= (0x80u | 0x40u | 2u
                            | (nv2a_tex_filter_linear(s_stage[n].filter, 1) ? 0x04u : 0u)) << (8 * n);
        }
    }
    memcpy(key, v->rc_cicw, 32);
    memcpy(key + 8, v->rc_aicw, 32);
    memcpy(key + 16, v->rc_cocw, 32);
    memcpy(key + 24, v->rc_aocw, 32);
    key[32] = v->rc_ctl;
    key[33] = v->rc_fcw0;
    key[34] = v->rc_fcw1;
    key[35] = modes;
    key[36] = signs;
    key[37] = v->shader_ctl & 0x00FF0FFFu;  /* stage 2 and 3 input stages, dot maps */
    key[38] = v->clip_plane_mode | (nv2a_alpha_kill_mask(s_stage, nv2a_pb_gpu_state()->flips) << 16);
    /* W-buffering, as nv2a_vp_consts_fill: the shader writes the vertex
     * stage's W depth per pixel (D3D8_GLSL_VS_OUT). fs_basic (no combiners
     * programmed, which D3D does for every draw) keeps the corners' depth. */
    wbuf = (v->control0 & (1u << 16)) && v->clip_max > 0.0f;
    key[39] = (uint32_t)s_hdr | ((uint32_t)s_fs_show_now << 8) | ((uint32_t)wbuf << 16);
    key[40] = shadows;
    for (n = 0; n < 41; n++)
        kh = (kh ^ key[n]) * 16777619u;
    kh = (kh ^ (kh >> 15)) & 63u;
    if (memo[kh].valid && !memcmp(memo[kh].r, key, sizeof key))
        return memo[kh].m;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;
    for (n = 0; n < 4; n++) {
        st.tex_signed[n] = (uint8_t)((signs >> (4 * n)) & 0xFu);
        st.clip_mode[n] = (uint8_t)((v->clip_plane_mode >> (4 * n)) & 0xFu);
        st.tex_shadow[n] = (uint8_t)(shadows >> (8 * n));
        st.dot_map[n] = n ? (uint8_t)((v->shader_ctl >> (4 * (n - 1))) & 0xFu) : 0;
    }
    st.alpha_kill = (uint8_t)nv2a_alpha_kill_mask(s_stage, nv2a_pb_gpu_state()->flips);
    st.debug_show = s_fs_show_now;
    st.wbuffer = (uint8_t)wbuf;
    st.input_tex[2] = (uint8_t)((v->shader_ctl >> 16) & 0xFu);
    st.input_tex[3] = (uint8_t)((v->shader_ctl >> 20) & 0xFu);
    hash = fnv(&st, sizeof st);
    if (s_hdr)
        hash ^= 0x48445200u;            /* BO-110: the HDR variant, its own entry */
    pthread_mutex_lock(&s_pc_mu);
    e = fs_find(hash, &st, s_hdr);
    pthread_mutex_unlock(&s_pc_mu);
    if (!e && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            e = fs_find(hash, &st, s_hdr);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!e && pc_wait_more());
        pc_waited(t0, "fragment shader");
    }
    if (!e) {
        VkShaderModule mod = fs_build(&st, hash, glsl, 0, s_hdr);
        pthread_mutex_lock(&s_pc_mu);
        if ((e = fs_find(hash, &st, s_hdr)) != NULL) {
            if (mod)                    /* the pre-warm thread was first */
                vkDestroyShaderModule(s_dev, mod, NULL);
            mod = VK_NULL_HANDLE;
        } else {
            e = fs_take(hash, &st, mod, s_hdr);
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (!e)
            return s_fs_basic;          /* the table is full */
        if (mod)
            if (!st.debug_show)     /* a debug view is not the title's */
                pc_put(s_hdr ? PC_FS_HDR : PC_FS, &st, sizeof st);
    }
    memcpy(memo[kh].r, key, sizeof key);
    memo[kh].valid = 1;
    memo[kh].m = e->bad ? s_fs_basic : e->mod;
    return memo[kh].m;
}

/* ---- pipelines -------------------------------------------------------------------- */

static VkBlendOp blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B:
    case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static VkBlendFactor blend_factor(uint32_t gl, int alpha)
{
    static const VkBlendFactor map[NV2A_BF_UNKNOWN + 1] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        /* One RGBA constant, which nv2a_blend_constant() fills for _COLOR
         * or _ALPHA use, as Metal and D3D11. */
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE,                      /* unknown */
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

/* A pipeline: its two modules, the vertex inputs, the blend state and
 * colour mask, the depth attachment and the topology. Everything else
 * (viewport, scissor, cull, winding, depth and stencil tests, blend
 * constant, stencil reference) is dynamic state, set per draw. */
typedef struct {
    uint64_t vs, fs;
    uint32_t nf4, topo, depth, wmask;
    uint32_t blend[4];       /* enable, src, dst, op */
} PsoKey;
#define PSO_CAP 8192         /* power of two */
static struct { PsoKey k; VkPipeline p; int used; } *s_pso;
static uint32_t s_pso_n;

/* Any thread. */
static VkPipeline pipeline_new(const PsoKey *k, int prewarm)
{
    uint64_t t0 = now_ns();
    VkPipelineShaderStageCreateInfo ss[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[NV2A_VS_MAX_INPUTS];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo pr;
    VkGraphicsPipelineCreateInfo gp;
    VkFormat cf = (k->depth & 2u) ? RT_FORMAT_HDR : RT_FORMAT;
    VkPipeline p = VK_NULL_HANDLE;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    };
    uint32_t i;
    VkResult r;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = (VkShaderModule)(uintptr_t)k->vs;
    ss[0].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = (VkShaderModule)(uintptr_t)k->fs;
    ss[1].pName = "main";
    memset(&vb, 0, sizeof vb);
    vb.binding = 0;
    vb.stride = k->nf4 * 16u;
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    for (i = 0; i < k->nf4; i++) {
        va[i].location = i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = i * 16u;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = k->nf4 ? 1 : 0;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = k->nf4;
    vi.pVertexAttributeDescriptions = va;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = k->topo == NV2A_TOPO_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == NV2A_TOPO_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = k->blend[0] ? VK_TRUE : VK_FALSE;
    if (k->blend[0]) {
        cba.srcColorBlendFactor = blend_factor(k->blend[1], 0);
        cba.dstColorBlendFactor = blend_factor(k->blend[2], 0);
        cba.srcAlphaBlendFactor = blend_factor(k->blend[1], 1);
        cba.dstAlphaBlendFactor = blend_factor(k->blend[2], 1);
        cba.colorBlendOp = (VkBlendOp)k->blend[3];
        cba.alphaBlendOp = (VkBlendOp)k->blend[3];
    }
    cba.colorWriteMask = ((k->wmask & 0x00FF0000u) ? 0 : VK_COLOR_COMPONENT_R_BIT)
                       | ((k->wmask & 0x0000FF00u) ? 0 : VK_COLOR_COMPONENT_G_BIT)
                       | ((k->wmask & 0x000000FFu) ? 0 : VK_COLOR_COMPONENT_B_BIT)
                       | ((k->wmask & 0xFF000000u) ? 0 : VK_COLOR_COMPONENT_A_BIT);
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    memset(&pr, 0, sizeof pr);
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &cf;
    pr.depthAttachmentFormat = (k->depth & 1u) ? C->depth_format : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = (k->depth & 1u) ? C->depth_format : VK_FORMAT_UNDEFINED;
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
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = s_playout;
    r = vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &gp, NULL, &p);
    if (r != VK_SUCCESS) {
        static int said;
        if (!said++)
            XR_ERROR( LOGP "pipeline failed: %s\n", nvk_result_str(r));
        return VK_NULL_HANDLE;
    }
    COUNT(s_pso_created);
    COUNT(s_pso_since_save);
    {
        char what[48];
        snprintf(what, sizeof what, "%016llX/%016llX", (unsigned long long)k->vs,
                 (unsigned long long)k->fs);
        pc_note(prewarm, PC_PSO, t0, what);
    }
    return p;
}

/* s_pc_mu held. The slot of `k`: its entry when *found, else the free slot
 * it would take. */
static uint32_t pso_slot(const PsoKey *k, int *found)
{
    uint32_t i = fnv(k, sizeof *k) & (PSO_CAP - 1);
    for (; s_pso[i].used; i = (i + 1) & (PSO_CAP - 1))
        if (!memcmp(&s_pso[i].k, k, sizeof *k)) {
            *found = 1;
            return i;
        }
    *found = 0;
    return i;
}

/* A pipeline key on disk: the shaders by id, not by module. */
typedef struct {
    uint64_t vs, fs;
    uint32_t nf4, topo, depth, wmask;
    uint32_t blend[4];
} PsoDisk;

static uint64_t module_kid(VkShaderModule m)
{
    int i;
    if (m == s_vs_pass)
        return PC_VS_PASS;
    if (m == s_fs_basic)
        return PC_FS_BASIC;
    for (i = 0; i < VS_MAX; i++)
        if (s_vs[i].used && s_vs[i].mod == m)
            return s_vs[i].kid;
    for (i = 0; i < s_fs_n; i++)
        if (s_fs[i].mod == m)
            return s_fs[i].kid;
    return 0;
}

static void pso_disk(const PsoKey *k, PsoDisk *d)
{
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&s_pc_mu);
    d->vs = module_kid((VkShaderModule)(uintptr_t)k->vs);
    d->fs = module_kid((VkShaderModule)(uintptr_t)k->fs);
    pthread_mutex_unlock(&s_pc_mu);
    d->nf4 = k->nf4;
    d->topo = k->topo;
    d->depth = k->depth;
    d->wmask = k->wmask;
    memcpy(d->blend, k->blend, sizeof d->blend);
}

static VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, uint32_t nf4,
                           enum nv2a_topology topo)
{
    /* The slots of the last keys, 32 of them by a hash of the key (as
     * fs_select's memo): the table's own lookup hashes the key byte by byte
     * under the lock. */
    static uint32_t last[32] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    PsoKey k;
    uint32_t i, mh = 2166136261u, w[sizeof(PsoKey) / 4];
    unsigned q;
    int found, full;

    memset(&k, 0, sizeof k);
    k.vs = (uint64_t)(uintptr_t)vs;
    k.fs = (uint64_t)(uintptr_t)fs;
    k.nf4 = nf4;
    k.topo = (uint32_t)topo;
    /* BO-110: bit 1, a half-float target */
    k.depth = (s_enc_dt ? 1u : 0u)
              | (s_enc_rt && s_enc_rt->im.format == RT_FORMAT_HDR ? 2u : 0u);
    k.wmask = g->color_keep;
    k.blend[0] = g->blend_enable ? 1 : 0;
    k.blend[1] = k.blend[0] ? g->blend_sfactor : 0;
    k.blend[2] = k.blend[0] ? g->blend_dfactor : 0;
    k.blend[3] = k.blend[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    memcpy(w, &k, sizeof w);
    for (q = 0; q < sizeof w / 4; q++)
        mh = (mh ^ w[q]) * 16777619u;
    mh = (mh ^ (mh >> 15)) & 31u;
    if (last[mh] < PSO_CAP && s_pso[last[mh]].used && !memcmp(&s_pso[last[mh]].k, &k, sizeof k))
        return s_pso[last[mh]].p;
    pthread_mutex_lock(&s_pc_mu);
    i = pso_slot(&k, &found);
    pthread_mutex_unlock(&s_pc_mu);
    if (!found && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            i = pso_slot(&k, &found);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!found && pc_wait_more());
        pc_waited(t0, "pipeline");
    }
    pthread_mutex_lock(&s_pc_mu);
    full = !found && s_pso_n + 1 >= PSO_CAP * 3 / 4;
    pthread_mutex_unlock(&s_pc_mu);
    if (!found) {
        VkPipeline p;
        if (full)
            return VK_NULL_HANDLE;   /* a title with this many wants eviction */
        p = pipeline_new(&k, 0);
        pthread_mutex_lock(&s_pc_mu);
        i = pso_slot(&k, &found);
        if (!found) {
            /* A failed one is kept too, so it is not tried at every draw. */
            s_pso[i].k = k;
            s_pso[i].p = p;
            s_pso[i].used = 1;
            s_pso_n++;
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (found) {                 /* the pre-warm thread was first */
            if (p)
                vkDestroyPipeline(s_dev, p, NULL);
        } else if (p) {
            PsoDisk dk;
            pso_disk(&k, &dk);
            pc_put(PC_PSO, &dk, sizeof dk);
            if (vk_trace())
                fprintf(stderr, LOGP "pipeline #%u: blend %u %04X/%04X op %u mask %08X"
                        " depth %u topo %u inputs %u\n", s_pso_n, k.blend[0], k.blend[1],
                        k.blend[2], k.blend[3], k.wmask, k.depth, k.topo, k.nf4);
        }
    }
    last[mh] = i;
    return s_pso[i].p;
}

/* ---- textures: Metal's cache, key and invalidation ------------------------- */

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 256
typedef struct {
    uint32_t         offset, color, width, height, pitch, palette, levels;
    uint32_t         sign;          /* nv2a_tex_signed_mask the bytes carry */
    uint32_t         gen, used;
    uint64_t         hash;
    uint32_t         rep;           /* nv2a_texrep generation it was uploaded at; 0: not a replaced texture */
    uint8_t          ui;            /* a 2D draw has bound it (nv2a_texscale.h); stays */
    uint8_t          scale;         /* the factor wanted when it was uploaded: 0, 2, 4 */
    uint8_t          salgo;         /* and the scaler */
    uint8_t          up_scale;      /* the factor the image has (0 where the scaler refused) */
    uint8_t          use;           /* RECOMP_DEBUG=ui_scale_log: the kinds of draw seen */
    uint8_t          pend;          /* its scaled picture is being made (nv2a_texscale_get) */
    uint32_t         cut;           /* the alpha test its levels were made for (nv2a_texmip_cut_ref); 0: the title's */
    float            zbias;         /* nv2a_texrep_zbias of its bytes (BO-176); 0: none */
    uint64_t         rhash;         /* nv2a_texrep_hash, with zbias_probe/ztint only (BO-204) */
    const struct nv2a_texrep_hdr_gain *hgain;   /* BO-110: a registered light texture's; NULL: none */
    uint8_t          solo;          /* BO-203: nv2a_texrep_hdr_stage0_only of its bytes */
    uint32_t         pal_len, pgen; /* P8 entries; the flip pend was last asked at */
    uint64_t         skey;          /* nv2a_texscale_key of the texels uploaded */
    /* graphics.texture_upscale (BO-245), as Metal's */
    uint8_t          tu, tu_slot, tu_add, tu_demote;
    uint32_t         tgen, tu_flip;
    uint64_t         tu_hash, tu_bytes;
    int              live;
    struct nvk_image im;
} TexEntry;

static TexEntry         s_tc[TEX_CACHE];

/* BO-45, as Metal: the channels stage t's filter reads as signed, honoured
 * for textures decoded from guest memory when the pixel stage is a combiner
 * shader; uploaded offset by 128 (nv2a_tex_signed_xor) for the shader to
 * take back (d3d8_combiners_glsl.c). xemu #587. */
static int s_signed_on;
static uint8_t stage_signed(const struct nv2a_stage *t)
{
    return s_signed_on ? nv2a_tex_signed_mask(t->filter) : 0;
}
static uint32_t         s_tc_clock;
static uint32_t        *s_texels;
static size_t           s_texels_cap;
static struct nvk_image s_black, s_magenta;
static struct nvk_image s_clear;   /* BO-203: transparent black, stages 1..3 of a stage0_only draw */

static int solid_texture(struct nvk_image *im, uint32_t argb)
{
    if (!nvk_image_new(im, RT_FORMAT, 1, 1, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    return upload_level(im, 0, 1, 1, &argb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
}

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = recomp_env(RENV_VK_NO_MIPS) != NULL;
    return off;
}

static void build_stages(void)
{
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int n;

    for (n = 0; n < 4; n++) {
        Stage *t = &s_stage[n];
        nv2a_stage_decode(n, &regs[(0x40u * n) / 4], &sets[(0x40u * n) / 4],
                          v->shader_set, v->shader_prog, nv2a_pb_dma_resolve,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR
                          | (no_mips() ? 0u : NV2A_STAGE_MIPS), t);
        if (t->mode == 5)
            t->mode = 0;
    }
}

static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = nv2a_tex_hash(mem + t->offset, bytes, NV2A_TEX_HASH_SEED);
    if (t->color == NV2A_TEX_P8)
        h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
    return h;
}

/* RECOMP_TRACE=pcache: this frame's new textures (tex_upload_log). */
static struct { uint32_t n; uint64_t texels, decode, image, copy; } s_fup;

/* Decoded on the CPU by the shared decode, uploaded as BGRA8 into a new
 * image: a draw already recorded this frame keeps the texels it was
 * recorded with, as Metal's new MTLTexture per change. */
static uint32_t s_tex_rep;    /* the last tex_upload's nv2a_texrep_apply */

/* graphics.ui_scale (nv2a_texscale.h), as Metal: the draw being recorded is
 * a 2D one, and the factor the texture tex_bind last returned has. */
static int       s_draw_2d;
static unsigned  s_bind_scale;
/* The depth offset the texture tex_bind returned asks for (nv2a_texrep_zbias,
 * BO-176), as Metal. */
static float     s_bind_zbias;
static uint64_t  s_bind_rhash, s_up_rhash;   /* BO-204 debug: texrep hash of the bind/upload */
static const struct nv2a_texrep_hdr_gain *s_bind_hgain;   /* BO-110, as the depth offset */
static int s_bind_solo;          /* BO-203: the bound texture is a stage0_only HDR picture */
static uint32_t *s_scaled;
static size_t    s_scaled_cap;

static unsigned scale_levels(unsigned f)
{
    return f == 4 ? 3u : f == 2 ? 2u : 1u;
}

static uint32_t  s_swap_flip = 0xFFFFFFFFu;   /* the flip of the last swap */
static int       s_up_pend;    /* the last tex_upload queued the scaled picture */
static uint64_t  s_up_key;

/* The disk cache of scaled textures: beside the shader cache. Once. */
static void scale_store_init(void)
{
    static int done;
    char dir[900], sub[1000];
    if (done)
        return;
    done = 1;
    if (nvk_cache_dir(dir, sizeof dir)) {
#if defined(__ANDROID__)
        /* BO-208: the key lists and driver caches are in the app's files
         * (they are the build, they must stay); the scaled art may be wiped
         * by the system and is made again, so it stays in the cache folder. */
        const char *xc = getenv("XDG_CACHE_HOME");
        if (xc && *xc)
            snprintf(sub, sizeof sub, "%s/%s/ui_scale", xc, nv2a_pcache_app());
        else
#endif
        snprintf(sub, sizeof sub, "%s/ui_scale", dir);
        nv2a_texscale_cache_dir(sub);
    }
}

static int scaled_room(const Stage *t, unsigned f)
{
    size_t n = nv2a_tex_texels(t->width * f, t->height * f, scale_levels(f));
    if (n > s_scaled_cap) {
        uint32_t *p = (uint32_t *)realloc(s_scaled, n * 4);
        if (!p)
            return 0;
        s_scaled = p;
        s_scaled_cap = n;
    }
    return 1;
}

/* s_scaled's level 0 (the texture f times as large) as an image with the
 * levels down to the title's size (as Metal). 0: no image (out untouched). */
static int scaled_image(const Stage *t, unsigned f, struct nvk_image *out)
{
    uint32_t w = t->width * f, h = t->height * f, levels = scale_levels(f), l;
    const uint32_t *src;

    nv2a_texscale_mips(s_scaled, w, h, levels);
    if (!nvk_image_new(out, RT_FORMAT, w, h, levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    for (l = 0, src = s_scaled; l < levels; l++) {
        uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
        if (!upload_level(out, l, lw, lh, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          l + 1 == levels)) {
            garbage_push(out, VK_NULL_HANDLE, NULL);
            memset(out, 0, sizeof *out);
            return 0;
        }
        src += (size_t)lw * lh;
    }
    return 1;
}

/* The scaled picture of s_texels, if the store has it ready; else it is
 * queued (s_up_pend) and the title's own texture is uploaded meanwhile.
 * Nothing is scaled on this thread. */
static int tex_upload_scaled(const Stage *t, unsigned f, int algo, struct nvk_image *out)
{
    uint64_t t0 = ui_scale_log() ? now_ns() : 0;
    int r;

    s_up_pend = 0;
    /* Every texture a 2D draw binds, opaque pictures too (the World Tour's
     * photographs, the loading pictures): the owner wants the whole 2D front
     * end scaled. Transparency only steers the switch-on pass below. */
    if (!scaled_room(t, f))
        return 0;
    scale_store_init();
    s_up_key = nv2a_texscale_key(s_texels, t->width, t->height);
    r = nv2a_texscale_get(s_up_key, s_texels, t->width, t->height, f,
                          (enum nv2a_texscale_algo)algo, 0, s_scaled);
    if (ui_scale_log())
        fprintf(stderr, "[UISCALE] ask %016llx %ux%u x%u: %s, %.3f ms on the render thread,"
                " flip %llu\n", (unsigned long long)s_up_key, t->width, t->height, f,
                r == NV2A_TEXSCALE_READY ? "ready" : r == NV2A_TEXSCALE_PENDING ? "queued"
                                                                               : "not taken",
                (double)(now_ns() - t0) / 1e6, (unsigned long long)s_presents);
    if (r == NV2A_TEXSCALE_READY)
        return scaled_image(t, f, out);
    s_up_pend = r == NV2A_TEXSCALE_PENDING;
    return 0;
}

static int tex_upload(const Stage *t, unsigned scale, int algo, uint32_t cut,
                      struct nvk_image *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL, *src;
    uint64_t t0, t1, t2;
    uint32_t l;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)(mem + t->palette);
    t0 = s_pc_trace ? now_ns() : 0;
    nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch,
                    t->levels, pal, pal ? t->pal_len : 0, s_texels);
    /* The title's button prompts as the pad in use prints them (input.glyphs,
     * nv2a_texrep.h): rectangles of a registered texture replaced here. */
    s_tex_rep = nv2a_texrep_apply(mem + t->offset, t->color, t->width, t->height,
                                  t->pitch, t->levels, s_texels);
    /* A registered picture that also comes larger (nv2a_texrep_hires): that
     * one, uploaded as an upscaler's picture is, so a prompt drawn at twice
     * the size stays sharp at a higher render scale. */
    if (s_tex_rep) {
        unsigned hf = 0;
        const uint32_t *hi = nv2a_texrep_hires(mem + t->offset, t->color, t->width,
                                               t->height, t->pitch, &hf);
        if (hi && hf >= 2 && hf <= 4 && scaled_room(t, hf)) {
            memcpy(s_scaled, hi, (size_t)t->width * hf * t->height * hf * 4);
            if (scaled_image(t, hf, out)) {
                static int said;
                if (said++ < 8)
                    fprintf(stderr, "[vulkan] texrep: %ux%u texture at 0x%08X uploaded"
                            " from its x%u picture\n", t->width, t->height, t->offset, hf);
                s_bind_scale = hf;
                s_up_pend = 0;
                s_tex_uploads++;
                s_sn.uploads++;
                return 1;
            }
        }
    }
    /* BO-204 debug: the texture's texrep hash; a ztint one goes magenta. */
    s_up_rhash = nv2a_texrep_ztint_apply(mem + t->offset, t->color, t->width, t->height,
                                         t->pitch, s_texels, n);
    /* An alpha-tested draw binds it: levels 1.. made again with level 0's
     * alpha coverage (nv2a_texmip.h). */
    (void)nv2a_texmip_apply(mem + t->offset, t->color, t->width, t->height, t->pitch,
                            t->levels, cut, s_texels);
    if (stage_signed(t)) {
        uint32_t x = nv2a_tex_signed_xor(stage_signed(t));
        size_t i;
        for (i = 0; i < n; i++)
            s_texels[i] ^= x;
    }
    s_bind_scale = 0;
    s_up_pend = 0;
    if (scale && tex_upload_scaled(t, scale, algo, out)) {
        s_bind_scale = scale;
        s_tex_uploads++;
        s_sn.uploads++;
        return 1;
    }
    t1 = s_pc_trace ? now_ns() : 0;
    /* TRANSFER_SRC: graphics.texture_upscale blits the title's levels out of
     * it (tu_blit_levels; BO-245). */
    if (!nvk_image_new(out, RT_FORMAT, t->width, t->height, t->levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    t2 = s_pc_trace ? now_ns() : 0;
    for (l = 0, src = s_texels; l < t->levels; l++) {
        uint32_t w = nv2a_tex_level_dim(t->width, l), h = nv2a_tex_level_dim(t->height, l);
        if (!upload_level(out, l, w, h, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          l + 1 == t->levels)) {
            garbage_push(out, VK_NULL_HANDLE, NULL);
            memset(out, 0, sizeof *out);
            return 0;
        }
        src += (size_t)w * h;
    }
    s_tex_uploads++;
    s_sn.uploads++;
    if (s_pc_trace) {
        uint64_t t3 = now_ns();
        s_fup.n++;
        s_fup.texels += n;
        s_fup.decode += t1 - t0;
        s_fup.image += t2 - t1;
        s_fup.copy += t3 - t2;
    }
    return 1;
}

/* RECOMP_TRACE=pcache: a frame that spent 2 ms or more on new textures says
 * so at its flip. Pipelines are not the only first-use cost: a scene's first
 * frame decodes and uploads every texture it draws with (a race's first
 * frame: some 150, 70 ms on the RX 9060 XT). */
static void tex_upload_log(void)
{
    uint64_t ns = s_fup.decode + s_fup.image + s_fup.copy;
    if (s_fup.n && ns >= 2000000u)
        fprintf(stderr, LOGP "textures: the frame before present #%llu uploaded %u"
                " (%.1f M texels) in %.2f ms: decode %.2f, image %.2f, copy %.2f\n",
                (unsigned long long)s_presents + 1, s_fup.n, s_fup.texels / 1e6,
                ns / 1e6, s_fup.decode / 1e6, s_fup.image / 1e6, s_fup.copy / 1e6);
    memset(&s_fup, 0, sizeof s_fup);
}



/* graphics.ui_scale switched on (or changed) in the overlay: while the title
 * is held for the pause, every loaded texture the rule could scale is decoded
 * and queued, to be made and written to the disk cache in the background
 * (nothing is uploaded here; a texture's own bind takes its picture from the
 * cache later). "Could scale": one level, swizzled or DXT, some transparency;
 * whether a 2D draw uses it is not known for a texture bound before the
 * setting was on, so some that never will be are made too. The executor's
 * thread (xbox_HostPauseIdleFn), which is the thread of every other use of
 * the cache. */
static void scale_prepare_loaded(void)
{
    static uint32_t done_gen;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    unsigned f = nv2a_texscale_factor(), queued = 0;
    int algo = (int)nv2a_texscale_algo(), i;

    if (done_gen == nv2a_texscale_generation())
        return;
    done_gen = nv2a_texscale_generation();
    if (!f)
        return;
    scale_store_init();
    if (!nv2a_texscale_cache_path(NULL, 0))
        return;
    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry *e = &s_tc[i];
        Stage st;
        const uint32_t *pal = NULL;
        uint32_t bytes;
        size_t n;

        if (!e->live || e->sign || e->up_scale == f || e->pend
                || !nv2a_texscale_tex_ok(nv2a_tex_size_from_format(e->color), e->width,
                                         e->height, e->levels))
            continue;
        /* Nothing says here whether a 2D draw uses a texture, and the larger
         * one-level textures with alpha are mostly the world's (car
         * liveries, tree lines, sky strips, 8 MB each at 4x): those are left
         * to their own bind, which knows. Fonts and HUD art are within this. */
        if ((uint64_t)e->width * e->height > 256u * 256u)
            continue;
        memset(&st, 0, sizeof st);
        st.offset = e->offset;
        st.color = e->color;
        st.width = e->width;
        st.height = e->height;
        st.pitch = e->pitch;
        st.palette = e->palette;
        st.pal_len = e->pal_len;
        st.levels = e->levels;
        bytes = nv2a_tex_extent(st.color, st.width, st.height, st.pitch, st.levels);
        /* Only what is still the texture that was uploaded. */
        if (!bytes || tex_hash_pal(mem, &st, bytes) != e->hash)
            continue;
        n = nv2a_tex_texels(st.width, st.height, st.levels);
        if (n > s_texels_cap) {
            uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
            if (!p)
                return;
            s_texels = p;
            s_texels_cap = n;
        }
        if (st.color == NV2A_TEX_P8)
            pal = (const uint32_t *)(mem + st.palette);
        nv2a_tex_decode(mem + st.offset, st.color, st.width, st.height, st.pitch, st.levels,
                        pal, pal ? st.pal_len : 0, s_texels);
        (void)nv2a_texrep_apply(mem + st.offset, st.color, st.width, st.height, st.pitch,
                                st.levels, s_texels);
        /* Not known to be 2D here: art cut out with alpha is the likely kind;
         * an opaque picture waits for its own 2D bind. */
        if (!nv2a_texscale_has_alpha(s_texels, (size_t)st.width * st.height))
            continue;
        if (nv2a_texscale_get(nv2a_texscale_key(s_texels, st.width, st.height), s_texels,
                              st.width, st.height, f, (enum nv2a_texscale_algo)algo,
                              NV2A_TEXSCALE_DISK_ONLY, NULL) == NV2A_TEXSCALE_PENDING)
            queued++;
    }
    fprintf(stderr, "[UISCALE] setting changed to %ux: %u loaded textures queued for the"
            " cache\n", f, queued);
}

/* RECOMP_DEBUG=ui_scale_log, as Metal. */
static int ui_scale_log(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_UI_SCALE_LOG) != NULL;
    return on;
}

static void ui_scale_use(TexEntry *e, const Stage *t, const uint8_t *mem)
{
    uint8_t bit = s_draw_2d ? 1 : 2;
    if (e->use & bit)
        return;
    e->use |= bit;
    fprintf(stderr, "[UISCALE] tex %016llx fmt 0x%02x %ux%u levels %u use %u scale %u flip %llu\n",
            (unsigned long long)nv2a_texrep_hash(mem + t->offset, t->color, t->width,
                                                 t->height, t->pitch),
            t->color, t->width, t->height, t->levels, e->use, e->up_scale,
            (unsigned long long)s_presents);
}

/* ---- graphics.texture_upscale (BO-245, nv2a_texup.h), as Metal ------------
 *
 * The same steps as nv2a_pb_metal.m's section: a slot per texture, the disk
 * cache first, else the one compute job in chunks under the governor, the
 * swap at the next bind once the GPU is done. Here the chunks are recorded
 * into the upload command buffer right after the frame's submit and
 * submitted at once (one queue: the frame's work first, then the chunks),
 * timed with two timestamps; scratch images stay in GENERAL for the job's
 * life; the result is RGBA8 (a storage format every desktop driver writes),
 * the title's levels blitted below the model's; the result's level 0 is
 * copied to a host buffer for the disk cache and read once its serial is
 * done. A disk result is uploaded into a BGRA8 image (the files are BGRA8)
 * through the upload ring under the byte budget. Old images go through
 * garbage_push as every texture does.
 *
 * Off: as Metal, texup_bind is not called for an entry that never had a
 * slot, nothing is timed, nothing is made. */

enum { TU_NONE, TU_SEEN, TU_DISK, TU_WAIT, TU_BUSY, TU_UP, TU_NO };

enum { TP_LUMA, TP_RECOMBINE, TP_NNEDI3, TP_FSR0, TP_N = TP_FSR0 + 13 };
static const char *const k_tp_file[TP_N] = {
    "luma", "recombine", "nnedi3",
    "fsrcnnx_00", "fsrcnnx_01", "fsrcnnx_02", "fsrcnnx_03", "fsrcnnx_04", "fsrcnnx_05",
    "fsrcnnx_06", "fsrcnnx_07", "fsrcnnx_08", "fsrcnnx_09", "fsrcnnx_10", "fsrcnnx_11",
    "fsrcnnx_12",
};
/* Layout A: binding 0 a sampled image, 1 a storage image, 4 the output (luma,
 * recombine). Layout B: 0..2 storage images, 4 the output (FSRCNNX, NNEDI3). */
static int tp_layout(int p) { return p == TP_LUMA || p == TP_RECOMBINE ? 0 : 1; }

static VkDescriptorSetLayout s_tu_dsl[2];
static VkPipelineLayout      s_tu_pl[2];
static VkPipeline            s_tu_pipe[TP_N];
static VkSampler             s_tu_point;
static int                   s_tu_state;      /* 0 not yet, 1 ready, -1 failed (atomic) */

struct tu_pc { int32_t origin[2], size[2], wrap[2], mode, pad0; };

static int tu_file(const char *dir, const char *name, char **out)
{
    char path[1200];
    FILE *f;
    long n;
    snprintf(path, sizeof path, "%s/%s.comp", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = (char *)malloc((size_t)n + 1);
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(*out);
        return 0;
    }
    (*out)[n] = 0;
    fclose(f);
    return 1;
}

static void *tu_load_main(void *arg)
{
    char *dir = (char *)arg;
    int i, ok = 1;
    for (i = 0; i < TP_N && ok; i++) {
        char *src = NULL, err[1024] = "";
        const uint32_t *w;
        size_t n;
        int compiled;
        VkShaderModuleCreateInfo mi;
        VkShaderModule mod = VK_NULL_HANDLE;
        VkComputePipelineCreateInfo ci;
        if (!tu_file(dir, k_tp_file[i], &src)) {
            fprintf(stderr, "[TEXUP] cannot read %s/%s.comp\n", dir, k_tp_file[i]);
            ok = 0;
            break;
        }
        if (!nvk_glsl_spirv(NVK_STAGE_COMPUTE, src, &w, &n, &compiled, err, sizeof err)) {
            fprintf(stderr, "[TEXUP] %s: %s\n", k_tp_file[i], err);
            free(src);
            ok = 0;
            break;
        }
        free(src);
        memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = n * 4;
        mi.pCode = w;
        if (vkCreateShaderModule(s_dev, &mi, NULL, &mod) != VK_SUCCESS) {
            ok = 0;
            break;
        }
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = s_tu_pl[tp_layout(i)];
        if (vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_tu_pipe[i])
                != VK_SUCCESS)
            ok = 0;
        vkDestroyShaderModule(s_dev, mod, NULL);
    }
    fprintf(stderr, "[TEXUP] shaders from %s: %s\n", dir, ok ? "ready" : "failed");
    __atomic_store_n(&s_tu_state, ok ? 1 : -1, __ATOMIC_RELEASE);
    free(dir);
    return NULL;
}

static int tu_state(void)
{
    return __atomic_load_n(&s_tu_state, __ATOMIC_ACQUIRE);
}

/* The layouts and the sampler here (the render thread), the pipelines on a
 * thread of their own. */
static void tu_pipes_async(const char *dir)
{
    static int started;
    VkDescriptorSetLayoutBinding b[5];
    VkDescriptorSetLayoutCreateInfo li;
    VkPipelineLayoutCreateInfo pi;
    VkPushConstantRange pr;
    VkSamplerCreateInfo sc;
    pthread_t th;
    char *d;
    int l, i;

    if (started)
        return;
    started = 1;
    for (l = 0; l < 2; l++) {
        uint32_t n = 0;
        memset(b, 0, sizeof b);
        for (i = 0; i < 5; i++) {
            if (i == 3 || (l == 0 && i == 2))
                continue;
            b[n].binding = (uint32_t)i;
            b[n].descriptorType = l == 0 && i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[n].descriptorCount = 1;
            b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            n++;
        }
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = n;
        li.pBindings = b;
        memset(&pr, 0, sizeof pr);
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.size = sizeof(struct tu_pc);
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &s_tu_dsl[l];
        pi.pushConstantRangeCount = 1;
        pi.pPushConstantRanges = &pr;
        if (vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_tu_dsl[l]) != VK_SUCCESS
                || vkCreatePipelineLayout(s_dev, &pi, NULL, &s_tu_pl[l]) != VK_SUCCESS) {
            s_tu_state = -1;
            return;
        }
    }
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(s_dev, &sc, NULL, &s_tu_point) != VK_SUCCESS) {
        s_tu_state = -1;
        return;
    }
    d = strdup(dir);
    if (!d || pthread_create(&th, NULL, tu_load_main, d) != 0) {
        free(d);
        s_tu_state = -1;
        return;
    }
    pthread_detach(th);
}

/* ── a job ─────────────────────────────────────────────────────────────── */

#define TU_STEPS 40
typedef struct {
    int      pipe, cls, mode;
    VkDescriptorSet set;
    VkImage  out_img;                 /* the image the step writes (for its barrier) */
    uint32_t dom_w, dom_h, size_w, size_h, group, row, col;
} TuStep;

typedef struct {
    struct nvk_image src;             /* the title's: the entry's until it lets go (src_owned) */
    struct nvk_image dst;             /* RGBA8, levels: the model's + the title's */
    VkImageView      lv[2];           /* dst's levels 0 and 1 */
    struct nvk_image scratch[16];
    int              nscratch;
    VkDescriptorPool pool;
    struct nvk_buffer rb;             /* level 0 (and 1) for the disk cache, then the title's
                                       * level 0 (the result check) */
    VkDeviceSize      rb_model;       /* the model's bytes at its start */
    struct nvk_buffer db;             /* RECOMP_DEBUG=texup_dump: the luma and SUBCONV1 */
    uint32_t w, h, levels, scale, wrap;
    int      algo, nstep, cur, started, finished;
    int      src_owned;               /* the entry let go of src: freed with the job */
    int      rb_idle;                 /* rb's copy is done and read: it may go to the pool */
    TuStep   step[TU_STEPS];
} TuJob;

/* BO-272: the jobs' scratch images and readback buffers are kept for the
 * next job instead of being made and freed for each (on Windows a job's
 * nine vkAllocateMemory calls cost 2.6-3.6 ms of the render thread). One
 * queue runs every job in order and each job's chunks begin with a full
 * compute/transfer barrier, so a later job may write what an earlier one
 * read; a buffer is handed back only when its job's copy is done and read.
 * At most TU_POOL_BYTES are kept; the oldest go through the garbage. */
#define TU_POOL      48
#define TU_POOL_BYTES (96ull << 20)
static struct { struct nvk_image im; uint64_t t; } s_tu_pim[TU_POOL];
static struct { struct nvk_buffer b; uint64_t t; } s_tu_pbuf[16];
static uint64_t s_tu_pool_bytes, s_tu_pool_t, s_tu_pool_hits, s_tu_pool_miss;

static void tu_pool_trim(void)
{
    while (s_tu_pool_bytes > TU_POOL_BYTES) {
        int i, old = -1, buf = 0;
        for (i = 0; i < TU_POOL; i++)
            if (s_tu_pim[i].im.img && (old < 0 || s_tu_pim[i].t < (buf ? s_tu_pbuf[old].t
                                                                      : s_tu_pim[old].t))) {
                old = i;
                buf = 0;
            }
        for (i = 0; i < 16; i++)
            if (s_tu_pbuf[i].b.buf && (old < 0 || s_tu_pbuf[i].t < (buf ? s_tu_pbuf[old].t
                                                                       : s_tu_pim[old].t))) {
                old = i;
                buf = 1;
            }
        if (old < 0)
            break;
        if (buf) {
            s_tu_pool_bytes -= s_tu_pbuf[old].b.size;
            garbage_push(NULL, VK_NULL_HANDLE, &s_tu_pbuf[old].b);
            memset(&s_tu_pbuf[old], 0, sizeof s_tu_pbuf[old]);
        } else {
            s_tu_pool_bytes -= s_tu_pim[old].im.bytes;
            garbage_push(&s_tu_pim[old].im, VK_NULL_HANDLE, NULL);
            memset(&s_tu_pim[old], 0, sizeof s_tu_pim[old]);
        }
    }
}

static void tu_pool_put_image(const struct nvk_image *im)
{
    int i, old = 0;
    if (!im->img)
        return;
    for (i = 0; i < TU_POOL && s_tu_pim[i].im.img; i++)
        if (s_tu_pim[i].t < s_tu_pim[old].t)
            old = i;
    if (i == TU_POOL) {
        i = old;
        s_tu_pool_bytes -= s_tu_pim[i].im.bytes;
        garbage_push(&s_tu_pim[i].im, VK_NULL_HANDLE, NULL);
    }
    s_tu_pim[i].im = *im;
    s_tu_pim[i].t = ++s_tu_pool_t;
    s_tu_pool_bytes += im->bytes;
    tu_pool_trim();
}

static int tu_pool_get_image(struct nvk_image *out, VkFormat fmt, uint32_t w, uint32_t h)
{
    int i;
    for (i = 0; i < TU_POOL; i++)
        if (s_tu_pim[i].im.img && s_tu_pim[i].im.format == fmt && s_tu_pim[i].im.w == w
                && s_tu_pim[i].im.h == h) {
            *out = s_tu_pim[i].im;
            s_tu_pool_bytes -= out->bytes;
            memset(&s_tu_pim[i], 0, sizeof s_tu_pim[i]);
            s_tu_pool_hits++;
            return 1;
        }
    s_tu_pool_miss++;
    return 0;
}

static void tu_pool_put_buffer(const struct nvk_buffer *b)
{
    int i, old = 0;
    if (!b->buf)
        return;
    for (i = 0; i < 16 && s_tu_pbuf[i].b.buf; i++)
        if (s_tu_pbuf[i].t < s_tu_pbuf[old].t)
            old = i;
    if (i == 16) {
        i = old;
        s_tu_pool_bytes -= s_tu_pbuf[i].b.size;
        garbage_push(NULL, VK_NULL_HANDLE, &s_tu_pbuf[i].b);
    }
    s_tu_pbuf[i].b = *b;
    s_tu_pbuf[i].t = ++s_tu_pool_t;
    s_tu_pool_bytes += b->size;
    tu_pool_trim();
}

/* A buffer of at least size bytes and at most twice that. */
static int tu_pool_get_buffer(struct nvk_buffer *out, VkDeviceSize size)
{
    int i, best = -1;
    for (i = 0; i < 16; i++)
        if (s_tu_pbuf[i].b.buf && s_tu_pbuf[i].b.size >= size && s_tu_pbuf[i].b.size <= size * 2
                && (best < 0 || s_tu_pbuf[i].b.size < s_tu_pbuf[best].b.size))
            best = i;
    if (best < 0) {
        s_tu_pool_miss++;
        return 0;
    }
    *out = s_tu_pbuf[best].b;
    s_tu_pool_bytes -= out->size;
    memset(&s_tu_pbuf[best], 0, sizeof s_tu_pbuf[best]);
    s_tu_pool_hits++;
    return 1;
}

static void tu_job_free(TuJob *j)
{
    int i;
    if (!j)
        return;
    for (i = 0; i < j->nscratch; i++)
        garbage_push(&j->scratch[i], VK_NULL_HANDLE, NULL);
    for (i = 0; i < 2; i++)
        if (j->lv[i])
            garbage_push(NULL, j->lv[i], NULL);
    if (j->dst.img)
        garbage_push(&j->dst, VK_NULL_HANDLE, NULL);
    if (j->rb.buf && j->rb_idle)
        tu_pool_put_buffer(&j->rb);   /* its copy is done and read (BO-272) */
    else if (j->rb.buf)
        garbage_push(NULL, VK_NULL_HANDLE, &j->rb);
    if (j->db.buf)
        garbage_push(NULL, VK_NULL_HANDLE, &j->db);
    if (j->src_owned)
        garbage_push(&j->src, VK_NULL_HANDLE, NULL);
    /* Its sets are in command buffers that may still be out: the pool goes
     * with the images, once the GPU is done with them. */
    if (j->pool)
        garbage_push_(NULL, VK_NULL_HANDLE, NULL, j->pool);
    free(j);
}

static VkImageView tu_level_view(VkImage img, VkFormat fmt, uint32_t level)
{
    VkImageViewCreateInfo vi;
    VkImageView v = VK_NULL_HANDLE;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = level;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_dev, &vi, NULL, &v) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return v;
}

static struct nvk_image *tu_scratch(TuJob *j, uint32_t w, uint32_t h)
{
    struct nvk_image *im;
    if (j->nscratch >= 16)
        return NULL;
    im = &j->scratch[j->nscratch];
    if (!tu_pool_get_image(im, VK_FORMAT_R16G16B16A16_SFLOAT, w, h)
            && !nvk_image_new(im, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1,
                              VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    j->nscratch++;
    return im;
}

/* A step: its descriptor set from the job's pool. in: views (0: sampled for
 * layout A), out: the storage view. */
static TuStep *tu_add(TuJob *j, int pipe, int cls, uint32_t dw, uint32_t dh, uint32_t sw,
                      uint32_t sh, uint32_t group, const VkImageView *in, int nin, VkImageView out,
                      VkImage out_img, int mode)
{
    VkDescriptorSetAllocateInfo ai;
    VkWriteDescriptorSet wr[5];
    VkDescriptorImageInfo ii[5];
    TuStep *s;
    int l = tp_layout(pipe), k, n = 0;
    if (j->nstep >= TU_STEPS)
        return NULL;
    s = &j->step[j->nstep];
    memset(s, 0, sizeof *s);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = j->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_tu_dsl[l];
    if (vkAllocateDescriptorSets(s_dev, &ai, &s->set) != VK_SUCCESS)
        return NULL;
    memset(wr, 0, sizeof wr);
    memset(ii, 0, sizeof ii);
    for (k = 0; k <= nin; k++) {
        int b = k == nin ? 4 : k;
        int sampled = l == 0 && b == 0;
        ii[n].sampler = sampled ? s_tu_point : VK_NULL_HANDLE;
        ii[n].imageView = k == nin ? out : in[k];
        ii[n].imageLayout = sampled && in[k] == j->src.view ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_GENERAL;
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = s->set;
        wr[n].dstBinding = (uint32_t)b;
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[n].pImageInfo = &ii[n];
        n++;
    }
    vkUpdateDescriptorSets(s_dev, (uint32_t)n, wr, 0, NULL);
    s->pipe = pipe;
    s->cls = cls;
    s->mode = mode;
    s->dom_w = dw;
    s->dom_h = dh;
    s->size_w = sw;
    s->size_h = sh;
    s->group = group;
    s->out_img = out_img;
    j->nstep++;
    return s;
}

/* One 2x round: in (sampled view, w x h) to out (a storage view of dst). */
static int tu_round(TuJob *j, VkImageView in, VkImageView out, uint32_t w, uint32_t h)
{
    struct nvk_image *y = tu_scratch(j, w, h);
    VkImageView v[3];
    if (!y)
        return 0;
    v[0] = in;
    if (!tu_add(j, TP_LUMA, NV2A_TEXUP_P_LUMA, w, h, w, h, 16, v, 1, y->view, y->img, 0))
        return 0;
    if (j->algo == NV2A_TEXUP_FSRCNNX) {
        struct nvk_image *f1 = tu_scratch(j, w, h), *f2 = tu_scratch(j, w, h);
        struct nvk_image *a1 = tu_scratch(j, w, h), *a2 = tu_scratch(j, w, h);
        struct nvk_image *b1 = tu_scratch(j, w, h), *b2 = tu_scratch(j, w, h);
        struct nvk_image *io[13][4];
        static const int cls[13] = {
            NV2A_TEXUP_P_FSR_FEATURE, NV2A_TEXUP_P_FSR_FEATURE,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_SUBCONV,
        };
        int i, k;
        if (!f1 || !f2 || !a1 || !a2 || !b1 || !b2)
            return 0;
        {
            struct nvk_image *t[13][4] = {
                { y, NULL, NULL, f1 }, { y, NULL, NULL, f2 },
                { f1, f2, NULL, a1 }, { f1, f2, NULL, a2 },
                { a1, a2, NULL, b1 }, { a1, a2, NULL, b2 },
                { b1, b2, NULL, a1 }, { b1, b2, NULL, a2 },
                { a1, a2, NULL, b1 }, { a1, a2, NULL, b2 },
                { b1, b2, f1, a1 }, { b1, b2, f2, a2 },
                { a1, a2, NULL, b1 },
            };
            memcpy(io, t, sizeof io);
        }
        for (i = 0; i < 13; i++) {
            for (k = 0; k < 3 && io[i][k]; k++)
                v[k] = io[i][k]->view;
            if (!tu_add(j, TP_FSR0 + i, cls[i], w, h, w, h, 16, v, k, io[i][3]->view,
                        io[i][3]->img, 0))
                return 0;
        }
        v[0] = in;
        v[1] = b1->view;
        return tu_add(j, TP_RECOMBINE, NV2A_TEXUP_P_RECOMBINE, w * 2, h * 2, w, h, 16, v, 2, out,
                      j->dst.img, 0) != NULL;
    } else {
        struct nvk_image *y2 = tu_scratch(j, w * 2, h * 2);
        if (!y2)
            return 0;
        v[0] = y->view;
        if (!tu_add(j, TP_NNEDI3, NV2A_TEXUP_P_NNEDI3, w, h, w, h, 16, v, 1, y2->view, y2->img, 0))
            return 0;
        v[0] = in;
        v[1] = y2->view;
        return tu_add(j, TP_RECOMBINE, NV2A_TEXUP_P_RECOMBINE, w * 2, h * 2, w, h, 16, v, 2, out,
                      j->dst.img, 1) != NULL;
    }
}

static TuJob *tu_job_new(const struct nvk_image *src, uint32_t w, uint32_t h, uint32_t levels,
                         int algo, unsigned scale, unsigned wrap)
{
    TuJob *j = (TuJob *)calloc(1, sizeof *j);
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pci;
    uint32_t add = scale == 4 ? 2u : 1u;
    VkDeviceSize rbytes = (VkDeviceSize)w * scale * h * scale * 4u
                        + (scale == 4 ? (VkDeviceSize)w * 2 * h * 2 * 4u : 0);
    VkDeviceSize rmodel = rbytes;
    rbytes += (VkDeviceSize)w * h * 4u;   /* the title's level 0, as dst holds it */
    if (!j)
        return NULL;
    j->src = *src;
    j->w = w;
    j->h = h;
    j->levels = levels;
    j->scale = scale;
    j->wrap = wrap;
    j->algo = algo;
    j->rb_model = rmodel;
    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[0].descriptorCount = 8;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps[1].descriptorCount = 4 * TU_STEPS;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = TU_STEPS;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(s_dev, &pci, NULL, &j->pool) != VK_SUCCESS)
        goto fail;
    if (!nvk_image_new(&j->dst, VK_FORMAT_R8G8B8A8_UNORM, w * scale, h * scale, levels + add,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        goto fail;
    j->lv[0] = tu_level_view(j->dst.img, VK_FORMAT_R8G8B8A8_UNORM, 0);
    if (!j->lv[0])
        goto fail;
    if (!tu_pool_get_buffer(&j->rb, rbytes)
            && !nvk_buffer_new(&j->rb, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                               | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        goto fail;
    j->rb_idle = 1;                   /* nothing recorded into it yet */
    {
        static int dumped;
        const char *dd = recomp_env(RENV_TEXUP_DUMP);
        if (dd && *dd && algo == NV2A_TEXUP_FSRCNNX && dumped < 3
                && nvk_buffer_new(&j->db, (VkDeviceSize)w * h * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                  | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
            dumped++;
    }
    if (scale == 2) {
        if (!tu_round(j, src->view, j->lv[0], w, h))
            goto fail;
    } else {
        j->lv[1] = tu_level_view(j->dst.img, VK_FORMAT_R8G8B8A8_UNORM, 1);
        if (!j->lv[1] || !tu_round(j, src->view, j->lv[1], w, h)
                || !tu_round(j, j->lv[1], j->lv[0], w * 2, h * 2))
            goto fail;
    }
    return j;
fail:
    tu_job_free(j);
    return NULL;
}

static double tu_job_cost(const TuJob *j)
{
    double c = 0;
    int i;
    for (i = 0; i < j->nstep; i++)
        c += nv2a_texup_pass_cost(j->step[i].cls, (double)j->step[i].dom_w * j->step[i].dom_h);
    return c;
}

static void tu_mem_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                     | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* The title's levels into dst's levels add.. (a blit: dst may be RGBA8, the
 * title's BGRA8). src stays in its read layout around it; dst ends in
 * dst_final. */
static void tu_blit_levels(VkCommandBuffer cb, struct nvk_image *src, struct nvk_image *dst,
                           uint32_t w, uint32_t h, uint32_t levels, uint32_t add,
                           VkImageLayout dst_final)
{
    VkImageLayout src_was = src->layout;
    uint32_t l;
    nvk_image_to(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (dst->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        nvk_image_to(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (l = 0; l < levels; l++) {
        VkImageBlit b;
        int32_t lw = (int32_t)nv2a_tex_level_dim(w, l), lh = (int32_t)nv2a_tex_level_dim(h, l);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = l;
        b.dstSubresource.mipLevel = l + add;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = b.dstOffsets[1].x = lw;
        b.srcOffsets[1].y = b.dstOffsets[1].y = lh;
        b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    nvk_image_to(cb, src, src_was);
    nvk_image_to(cb, dst, dst_final);
}

static void tu_dispatch(VkCommandBuffer cb, const TuJob *j, const TuStep *s, uint32_t col0,
                        uint32_t row0, uint32_t cols, uint32_t rows)
{
    struct tu_pc pc;
    int l = tp_layout(s->pipe);
    memset(&pc, 0, sizeof pc);
    pc.origin[0] = (int32_t)col0;
    pc.origin[1] = (int32_t)row0;
    pc.size[0] = (int32_t)s->size_w;
    pc.size[1] = (int32_t)s->size_h;
    pc.wrap[0] = (j->wrap & 1) != 0;
    pc.wrap[1] = (j->wrap & 2) != 0;
    pc.mode = s->mode;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_tu_pipe[s->pipe]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_tu_pl[l], 0, 1, &s->set, 0,
                            NULL);
    vkCmdPushConstants(cb, s_tu_pl[l], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (cols + s->group - 1) / s->group, (rows + s->group - 1) / s->group, 1);
}

/* As nv2a_texup_metal.m's tum_job_encode: whole rows while they fit, else
 * tiles of one group's rows. The first call sets the images up; the last
 * copies the result out for the disk cache and leaves it readable. */
static double tu_job_encode(TuJob *j, VkCommandBuffer cb, double budget, double factor)
{
    double used = 0;
    if (!j->started) {
        uint32_t i;
        for (i = 0; i < (uint32_t)j->nscratch; i++)
            nvk_image_to(cb, &j->scratch[i], VK_IMAGE_LAYOUT_GENERAL);
        tu_blit_levels(cb, &j->src, &j->dst, j->w, j->h, j->levels, j->scale == 4 ? 2u : 1u,
                       VK_IMAGE_LAYOUT_GENERAL);
        j->started = 1;
    }
    tu_mem_barrier(cb);   /* the chunks of earlier submits */
    while (j->cur < j->nstep) {
        TuStep *s = &j->step[j->cur];
        double per = nv2a_texup_pass_cost(s->cls, 1.0) * factor;
        uint32_t left = s->dom_h - s->row, rows = 0, cols, band;
        double room = budget - used;
        if (per <= 0)
            per = 1e-9;
        if (!s->col) {
            rows = room > 0 ? (uint32_t)(room / (per * s->dom_w)) : 0;
            rows = rows / s->group * s->group;
            if (rows > left)
                rows = left;
        }
        if (rows) {
            tu_dispatch(cb, j, s, 0, s->row, s->dom_w, rows);
            used += per * s->dom_w * rows;
            s->row += rows;
        } else {
            band = s->group < left ? s->group : left;
            cols = room > 0 ? (uint32_t)(room / (per * band)) : 0;
            cols = cols / s->group * s->group;
            if (!cols)
                break;
            if (cols > s->dom_w - s->col)
                cols = s->dom_w - s->col;
            tu_dispatch(cb, j, s, s->col, s->row, cols, band);
            used += per * cols * band;
            s->col += cols;
            if (s->col >= s->dom_w) {
                s->col = 0;
                s->row += band;
            }
        }
        if (s->row >= s->dom_h) {
            j->cur++;
            tu_mem_barrier(cb);
        }
    }
    if (j->cur >= j->nstep && !j->finished) {
        VkBufferImageCopy bc[3];
        uint32_t n = 1, W = j->w * j->scale, H = j->h * j->scale;
        memset(bc, 0, sizeof bc);
        bc[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc[0].imageSubresource.layerCount = 1;
        bc[0].imageExtent.width = W;
        bc[0].imageExtent.height = H;
        bc[0].imageExtent.depth = 1;
        if (j->scale == 4) {
            bc[1] = bc[0];
            bc[1].bufferOffset = (VkDeviceSize)W * H * 4u;
            bc[1].imageSubresource.mipLevel = 1;
            bc[1].imageExtent.width = W / 2;
            bc[1].imageExtent.height = H / 2;
            n = 2;
        }
        /* The title's level 0, below the model's: the result check. */
        bc[n] = bc[0];
        bc[n].bufferOffset = j->rb_model;
        bc[n].imageSubresource.mipLevel = j->scale == 4 ? 2u : 1u;
        bc[n].imageExtent.width = j->w;
        bc[n].imageExtent.height = j->h;
        n++;
        vkCmdCopyImageToBuffer(cb, j->dst.img, VK_IMAGE_LAYOUT_GENERAL, j->rb.buf, n, bc);
        if (j->db.buf && j->nscratch > 5) {
            /* RECOMP_DEBUG=texup_dump: the first round's luma and SUBCONV1
             * (FSRCNNX's last hook), RGBA16F, w x h each. */
            VkBufferImageCopy dc;
            memset(&dc, 0, sizeof dc);
            dc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            dc.imageSubresource.layerCount = 1;
            dc.imageExtent.width = j->w;
            dc.imageExtent.height = j->h;
            dc.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cb, j->scratch[0].img, VK_IMAGE_LAYOUT_GENERAL, j->db.buf, 1, &dc);
            dc.bufferOffset = (VkDeviceSize)j->w * j->h * 8u;
            vkCmdCopyImageToBuffer(cb, j->scratch[5].img, VK_IMAGE_LAYOUT_GENERAL, j->db.buf, 1, &dc);
        }
        nvk_image_to(cb, &j->dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        j->finished = 1;
        j->rb_idle = 0;               /* the copy above writes it; idle again once read */
        /* BO-272: its scratch for the next job (recorded after this one on
         * the same queue, behind the barrier its chunks begin with). */
        if (!j->db.buf) {
            int i;
            for (i = 0; i < j->nscratch; i++)
                tu_pool_put_image(&j->scratch[i]);
            j->nscratch = 0;
        }
    }
    return used;
}

/* ── slots, timing, the flip ───────────────────────────────────────────── */

#define TU_SLOTS 8
typedef struct {
    int       used;
    TexEntry *e;
    struct nv2a_texup_key k;
    unsigned  scale;
    TuJob    *job;
    struct nvk_image tex;             /* from the disk */
    uint32_t *px;
    size_t    bytes;
    uint64_t  serial;                 /* the submit that completes it; 0: not yet */
    uint64_t  done_flip;
    uint64_t  start_flip;             /* taken */
    int       saved;
    int       verdict;                /* the result check: 0 not yet, 1 kept, -1 refused */
} TuSlot;
static TuSlot   s_tu[TU_SLOTS];
static int      s_tu_job = -1;
static int      s_tu_fifo[TEX_CACHE];
static unsigned s_tu_fh, s_tu_fn;
static struct nv2a_texup_gov s_gov;
static size_t   s_tu_bytes_left = 32u << 20;
static unsigned s_tu_3d;
static int      s_tu_draw_3d, s_bind_tu, s_tu_inited, s_tu_timing;
static uint64_t s_tu_resident;
static double   s_tu_last_pred;
static int      s_tu_disk_pending[TU_SLOTS], s_tu_ndisk;   /* disk results recorded this flip */
/* RECOMP_DEBUG=texup_log: the upscaler's own render-thread time in a flip,
 * by part (ns), said when it passes 1 ms: binds (disk-result uploads,
 * swaps), the readback copy for the disk cache, new jobs, recording, the
 * submit. */
static uint64_t s_tu_ns_bind, s_tu_ns_save, s_tu_ns_new, s_tu_ns_enc, s_tu_ns_sub;

/* Timestamps: the frames' (a pair per submit, up to 8 a frame, 8 frames)
 * and the chunk buffers' (16 pairs). */
#define TQ_FRAME 8
#define TQ_SUB   8
#define TQ_CHUNK 16
static VkQueryPool s_tq;
static unsigned    s_tq_n[TQ_FRAME];             /* pairs used by frame f */
static int         s_tq_open = -1;               /* the pair the open s_cmd began */
static uint64_t    s_tq_chunk_serial[TQ_CHUNK];  /* 0: free */
static double      s_tq_chunk_pred[TQ_CHUNK];
static int         s_tq_chunk_algo[TQ_CHUNK];
static unsigned    s_tq_chunk_next;

static int tu_log(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_TEXUP_LOG) != NULL;
    return on;
}

static void tq_init(void)
{
    VkQueryPoolCreateInfo qi;
    if (s_tq || C->props.limits.timestampPeriod <= 0.0f)
        return;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = (TQ_FRAME * TQ_SUB + TQ_CHUNK) * 2;
    if (vkCreateQueryPool(s_dev, &qi, NULL, &s_tq) != VK_SUCCESS)
        s_tq = VK_NULL_HANDLE;
    else
        vkResetQueryPool(s_dev, s_tq, 0, qi.queryCount);
}

/* ensure_cmd: a new frame command buffer begins. */
static void texup_cmd_begin(VkCommandBuffer cb)
{
    unsigned f = (unsigned)(s_presents % TQ_FRAME);
    s_tq_open = -1;
    if (!s_tu_timing || !s_tq || s_tq_n[f] >= TQ_SUB)
        return;
    s_tq_open = (int)(f * TQ_SUB + s_tq_n[f]++);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tq, (uint32_t)s_tq_open * 2);
}

/* flush: the frame command buffer ends. */
static void texup_cmd_end(VkCommandBuffer cb)
{
    if (s_tq_open >= 0)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tq,
                            (uint32_t)s_tq_open * 2 + 1);
    s_tq_open = -1;
}

/* Frame f's GPU ms (the sum of its submits), -1 when not all are there. */
static double tq_frame(unsigned f)
{
    uint64_t ts[TQ_SUB * 2];
    double ms = 0;
    unsigned n = s_tq_n[f], i;
    if (!n)
        return -1;
    if (vkGetQueryPoolResults(s_dev, s_tq, f * TQ_SUB * 2, n * 2, sizeof ts, ts, 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return -1;
    for (i = 0; i < n; i++)
        if (ts[i * 2 + 1] > ts[i * 2])
            ms += (double)(ts[i * 2 + 1] - ts[i * 2]) * C->props.limits.timestampPeriod / 1e6;
    vkResetQueryPool(s_dev, s_tq, f * TQ_SUB * 2, n * 2);
    s_tq_n[f] = 0;
    return ms;
}

static unsigned tu_add_of(unsigned scale)
{
    return scale == 4 ? 2u : scale == 2 ? 1u : 0u;
}

static void texup_init_once(void);

/* Called at every texture bind while the setting is on, so only the flag
 * is read here. The work in texup_init_once (getenv, recomp_exe_dir, the
 * path buffers) used to run before that test at every bind; on Windows
 * ucrt's getenv takes a lock and walks the environment, which cost about
 * 2-3 ms a frame on the render thread (BO-273). */
static void texup_init(void)
{
    if (!s_tu_inited)
        texup_init_once();
}

static void texup_init_once(void)
{
    char sh[1024], dir[900], sub[1000], ver[64] = "";
    char c0[1024] = "", c1[1024] = "", c2[1024] = "";
    const char *cand[4];
    const char *exe = recomp_exe_dir(), *xd = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    FILE *f;
    s_tu_inited = 1;
    nv2a_texup_gov_init(&s_gov);
    tq_init();
    if (xd && *xd)
        snprintf(c0, sizeof c0, "%s/%s/shaders/upscale", xd, nv2a_pcache_app());
    else if (home && *home)
        snprintf(c0, sizeof c0, "%s/.local/share/%s/shaders/upscale", home, nv2a_pcache_app());
    if (exe && *exe) {
        snprintf(c1, sizeof c1, "%s/shaders/upscale", exe);
        snprintf(c2, sizeof c2, "%s/../share/takedawn/shaders/upscale", exe);
    }
    cand[0] = c0; cand[1] = c1; cand[2] = c2; cand[3] = NULL;
    if (!nv2a_texup_shader_dir(cand, sh, sizeof sh)) {
        fprintf(stderr, "[TEXUP] no shader files found: texture upscale stays off\n");
        s_tu_state = -1;
        return;
    }
    snprintf(sub, sizeof sub, "%s/VERSION", sh);
    if ((f = fopen(sub, "rb")) != NULL) {
        if (!fgets(ver, sizeof ver, f))
            ver[0] = 0;
        fclose(f);
    }
    nv2a_texup_set_shaders(sh, strtoull(ver, NULL, 16));
    tu_pipes_async(sh);
    if (nvk_cache_dir(dir, sizeof dir)) {
        snprintf(sub, sizeof sub, "%s/texup", dir);
        nv2a_texup_store_dir(sub);
    }
}

static void tu_slot_free(int i)
{
    TuSlot *s = &s_tu[i];
    if (s->e && s->e->tu_slot == i + 1)
        s->e->tu_slot = 0;
    if (s->job)
        tu_job_free(s->job);   /* everything it has through the garbage */
    if (s->tex.img)
        garbage_push(&s->tex, VK_NULL_HANDLE, NULL);
    free(s->px);
    if (s_tu_job == i)
        s_tu_job = -1;
    memset(s, 0, sizeof *s);
}

static TuSlot *tu_slot_of(const TexEntry *e)
{
    return e->tu_slot ? &s_tu[e->tu_slot - 1] : NULL;
}

static void tu_detach(TexEntry *e, int dynamic)
{
    TuSlot *s = tu_slot_of(e);
    if (s) {
        s->e = NULL;
        e->tu_slot = 0;
    }
    if (e->tu_add) {
        s_tu_resident -= e->tu_bytes;
        nv2a_texup_stats_rw()->upscaled--;
    }
    e->tu = (uint8_t)(dynamic ? TU_NO : TU_NONE);
    e->tu_add = 0;
    e->tu_demote = 0;
    e->tu_bytes = 0;
}

static void tu_fifo_push(TexEntry *e)
{
    if (s_tu_fn < TEX_CACHE) {
        s_tu_fifo[(s_tu_fh + s_tu_fn) % TEX_CACHE] = (int)(e - s_tc);
        s_tu_fn++;
    }
    nv2a_texup_stats_rw()->queued = s_tu_fn;
}

static int tu_room(uint64_t bytes, const TexEntry *keep)
{
    uint64_t cap = nv2a_texup_cap_bytes();
    while (s_tu_resident + bytes > cap) {
        TexEntry *lru = NULL;
        int i;
        for (i = 0; i < TEX_CACHE; i++) {
            TexEntry *c = &s_tc[i];
            if (c->live && c->tu_add && !c->tu_demote && c != keep
                    && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            return 0;
        lru->tu_demote = 1;
        s_tu_resident -= lru->tu_bytes;
        lru->tu_bytes = 0;
        nv2a_texup_stats_rw()->demoted++;
    }
    return 1;
}

/* An entry lets go of its image (a new upload, another texture in the
 * entry, a scaled picture, an upscaled one). A job made from that image
 * reads it in its first and last passes, chunk by chunk over many flips:
 * the job takes it over and frees it with its own images, when the GPU is
 * done with its last chunk. Before, the image went once the frames then out
 * were done, and the job's next chunks sampled freed memory (a GPU page
 * fault, then a ring reset or a lost device: the Deck at the car select,
 * NNEDI3). Otherwise it goes when the frames that may use it are done. */
static void tex_retire(struct nvk_image *im)
{
    int i;
    if (im->img)
        for (i = 0; i < TU_SLOTS; i++) {
            TuJob *j = s_tu[i].used ? s_tu[i].job : NULL;
            if (j && !j->src_owned && j->src.img == im->img) {
                j->src_owned = 1;
                memset(im, 0, sizeof *im);
                return;
            }
        }
    garbage_push(im, VK_NULL_HANDLE, NULL);
}

/* The slot's image becomes the entry's (the job's dst or the disk one). */
static void tu_save(TuSlot *s);
static int tu_check(TuSlot *s);

static void tu_swap(TexEntry *e, TuSlot *s)
{
    uint64_t bytes = (uint64_t)nv2a_tex_texels(e->width * s->scale, e->height * s->scale,
                                               e->levels + tu_add_of(s->scale)) * 4u;
    struct nv2a_texup_stats *st = nv2a_texup_stats_rw();
    struct nvk_image ni;
    if (!tu_room(bytes, e)) {
        st->dropped++;
        e->tu = TU_NO;
        tu_slot_free((int)(s - s_tu));
        return;
    }
    if (s->job && !tu_check(s)) {
        e->tu = TU_NO;   /* the title's texture stays */
        tu_slot_free((int)(s - s_tu));
        return;
    }
    if (s->job) {
        tu_save(s);   /* its result to the disk cache before the job goes */
        s->job->rb_idle = 1;   /* done and read: its buffer may serve the next job */
        ni = s->job->dst;
        memset(&s->job->dst, 0, sizeof s->job->dst);   /* the entry owns it now */
    } else {
        ni = s->tex;
        memset(&s->tex, 0, sizeof s->tex);
    }
    tex_retire(&e->im);
    e->im = ni;
    e->tu_add = (uint8_t)tu_add_of(s->scale);
    e->tu_bytes = bytes;
    e->tu = TU_UP;
    s_tu_resident += bytes;
    st->resident_bytes = s_tu_resident;
    st->upscaled++;
    st->swaps++;
    if (tu_log())
        fprintf(stderr, "[TEXUP] swap %016llx %ux%u x%u (%s), flip %llu\n",
                (unsigned long long)e->hash, e->width, e->height, s->scale,
                s->job ? "computed" : "from disk", (unsigned long long)s_presents);
    tu_slot_free((int)(s - s_tu));
}

/* A disk result into a BGRA8 image: its levels by the upload ring, the
 * title's by a blit, on the upload command buffer. 0: no room or memory. */
static int tu_from_file(TuSlot *s, TexEntry *e)
{
    uint32_t add = tu_add_of(s->scale), W = e->width * s->scale, H = e->height * s->scale;
    if (!nvk_image_new(&s->tex, RT_FORMAT, W, H, e->levels + add,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    if (!upload_level(&s->tex, 0, W, H, s->px, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0)
            || (add == 2 && !upload_level(&s->tex, 1, W / 2, H / 2, s->px + (size_t)W * H,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0))) {
        garbage_push(&s->tex, VK_NULL_HANDLE, NULL);
        memset(&s->tex, 0, sizeof s->tex);
        return 0;
    }
    ensure_up();
    tu_blit_levels(s_up, &e->im, &s->tex, e->width, e->height, e->levels, add,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

static void texup_bind_(TexEntry *e, const Stage *t, const uint8_t *mem);
static void tu_step(TexEntry *e, TuSlot *s);

static void texup_bind(TexEntry *e, const Stage *t, const uint8_t *mem)
{
    uint64_t t0 = tu_log() && e->tu >= TU_DISK && e->tu <= TU_BUSY ? now_ns() : 0;
    texup_bind_(e, t, mem);
    if (t0)
        s_tu_ns_bind += now_ns() - t0;
}

static void texup_bind_(TexEntry *e, const Stage *t, const uint8_t *mem)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t gen = nv2a_texup_generation();
    int algo = nv2a_texup_algo();
    TuSlot *s;

    if (e->tgen != gen) {
        tu_detach(e, 0);
        e->tgen = gen;
    }
    if (!algo)
        return;
    texup_init();
    if (tu_state() != 1)
        return;
    switch (e->tu) {
    case TU_NONE: {
        struct nv2a_texup_tex q;
        int why;
        memset(&q, 0, sizeof q);
        q.size_from_format = nv2a_tex_size_from_format(t->color);
        q.sign = e->sign != 0;
        q.cube = t->cube;
        q.replaced = e->rep != 0 || e->up_scale || e->scale;
        q.w = t->width;
        q.h = t->height;
        q.levels = t->levels;
        q.draw_3d = s_tu_draw_3d;
        q.ever_2d = e->ui;
        why = nv2a_texup_rule(&q);
        if (why == NV2A_TEXUP_OK) {
            e->tu = TU_SEEN;
            e->tu_flip = g->flips;
            e->tu_hash = e->hash;
            if (tu_log())    /* first sight: with the swap line, the wait for it */
                fprintf(stderr, "[TEXUP] seen %016llx %ux%u, flip %llu\n",
                        (unsigned long long)e->hash, t->width, t->height,
                        (unsigned long long)s_presents);
        } else if (why != NV2A_TEXUP_2D || e->ui) {
            e->tu = TU_NO;
        }
        break;
    }
    case TU_SEEN: {
        int i, r;
        uint32_t *px = NULL;
        size_t bytes = 0;
        if (e->hash != e->tu_hash || e->ui) {
            e->tu = TU_NO;
            break;
        }
        if (g->flips < e->tu_flip + 2)
            break;
        if (nv2a_texup_skip(nv2a_texrep_hash(mem + t->offset, t->color, t->width, t->height,
                                             t->pitch))) {
            e->tu = TU_NO;
            break;
        }
        for (i = 0; i < (int)nv2a_texup_slots() && s_tu[i].used; i++)
            ;
        if (i == (int)nv2a_texup_slots())
            break;
        s = &s_tu[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->e = e;
        s->start_flip = s_presents;
        s->scale = nv2a_texup_factor_for(t->width, t->height);
        s->k.hash = e->hash;
        s->k.w = t->width;
        s->k.h = t->height;
        s->k.format = t->color;
        s->k.algo = (uint32_t)algo;
        s->k.scale = s->scale;
        s->k.wrap = ((t->addr_u & 0xF) == 1 ? 1u : 0u) | ((t->addr_v & 0xF) == 1 ? 2u : 0u);
        s->k.shaders = nv2a_texup_shader_version();
        e->tu_slot = (uint8_t)(i + 1);
        e->tu = TU_DISK;
        nv2a_texup_stats_rw()->jobs++;
        r = nv2a_texup_load(&s->k, &px, &bytes);
        if (r == NV2A_TEXUP_ABSENT) {
            e->tu = TU_WAIT;
            tu_fifo_push(e);
        }
        break;
    }
    case TU_DISK:
    case TU_BUSY:
        s = tu_slot_of(e);
        if (!s) {
            e->tu = TU_NONE;
            break;
        }
        tu_step(e, s);
        break;
    default:
        break;
    }
}

/* A slot's next step, at a bind of its entry or at the flip (BO-272: a
 * result whose texture is not drawn again would otherwise hold its slot
 * until it is given back, 600 flips later, and the countdown's textures
 * waited behind the garage's). TU_DISK: the file's read, its upload under
 * the flip's byte slice, the swap once that upload is done; TU_BUSY: the
 * swap once the job's last submit is done. */
static void tu_step(TexEntry *e, TuSlot *s)
{
    if (e->tu == TU_DISK) {
        if (!s->px && !s->tex.img) {
            int r = nv2a_texup_load(&s->k, &s->px, &s->bytes);
            size_t want = (size_t)e->width * s->scale * e->height * s->scale * 4u
                        + (s->scale == 4 ? (size_t)e->width * 2 * e->height * 2 * 4u : 0);
            if (r == NV2A_TEXUP_ABSENT || (r == NV2A_TEXUP_READY && s->bytes != want)) {
                if (r == NV2A_TEXUP_ABSENT)
                    nv2a_texup_stats_rw()->absent++;
                free(s->px);
                s->px = NULL;
                e->tu = TU_WAIT;
                tu_fifo_push(e);
                return;
            }
        }
        if (s->px && !s->tex.img && s_tu_ndisk < TU_SLOTS
                && nv2a_texup_swap_fits(s->bytes, s_tu_bytes_left,
                                        nv2a_texup_gov_swap_bytes(&s_gov))) {
            if (!tu_from_file(s, e)) {
                e->tu = TU_NO;
                tu_slot_free((int)(s - s_tu));
                return;
            }
            s_tu_bytes_left -= s->bytes < s_tu_bytes_left ? s->bytes : s_tu_bytes_left;
            free(s->px);
            s->px = NULL;
            s_tu_disk_pending[s_tu_ndisk++] = (int)(s - s_tu);
            nv2a_texup_stats_rw()->from_disk++;
        }
        if (s->tex.img && s->serial && serial_done() >= s->serial)
            tu_swap(e, s);
    } else if (e->tu == TU_BUSY) {
        if (s->job && s->job->finished && s->serial && serial_done() >= s->serial)
            tu_swap(e, s);
    }
}

/* A finished job's level 0 (1) from its host buffer to the disk cache. */
static int tu_check(TuSlot *s)
{
    TuJob *j = s->job;
    if (!s->verdict) {
        const uint8_t *m = (const uint8_t *)j->rb.map;
        s->verdict = m && nv2a_texup_result_ok(m, m + j->rb_model, j->w, j->h, j->scale) ? 1 : -1;
        if (s->verdict < 0) {
            nv2a_texup_stats_rw()->refused++;
            fprintf(stderr, "[TEXUP] refused %016llx %ux%u x%u %s: the result does not match the"
                    " title's texture (alpha or brightness); the title's is kept, nothing saved\n",
                    (unsigned long long)s->k.hash, j->w, j->h, j->scale,
                    nv2a_texup_algo_name((int)s->k.algo));
        }
    }
    return s->verdict > 0;
}

static void tu_save(TuSlot *s)
{
    TuJob *j = s->job;
    size_t bytes = (size_t)j->rb_model;
    uint64_t t0;
    if (s->saved || !j->rb.map)
        return;
    s->saved = 1;
    if (!tu_check(s))
        return;
    t0 = tu_log() ? now_ns() : 0;
    if (j->db.map) {
        /* RECOMP_DEBUG=texup_dump: half floats out as floats. */
        const char *dd = recomp_env(RENV_TEXUP_DUMP);
        const uint16_t *hp = (const uint16_t *)j->db.map;
        int part;
        for (part = 0; part < 2 && dd; part++) {
            char path[1200];
            FILE *f;
            size_t i, n = (size_t)j->w * j->h * 4u;
            snprintf(path, sizeof path, "%s/%016llx_%ux%u_w%u_%s.f32", dd,
                     (unsigned long long)s->k.hash, j->w, j->h, j->wrap, part ? "model" : "luma");
            if ((f = fopen(path, "wb")) == NULL)
                continue;
            for (i = 0; i < n; i++) {
                float v = hdr_h2f(hp[part * n + i]);
                fwrite(&v, 4, 1, f);
            }
            fclose(f);
            fprintf(stderr, "[TEXUP] dump %s\n", path);
        }
    }
    nv2a_texup_save_rgba(&s->k, (const uint32_t *)j->rb.map, bytes);
    if (t0)
        s_tu_ns_save += now_ns() - t0;
}

static void texup_flip(void)
{
    static uint64_t last_ns, last_uploads;
    uint64_t now = now_ns();
    double frame_ms = last_ns ? (double)(now - last_ns) / 1e6 : 16.7, budget, gpu_ms = -1,
           tu_ms = 0;
    int algo = nv2a_texup_algo(), computed = s_tu_last_pred > 0, i;
    struct nv2a_texup_stats *st = nv2a_texup_stats_rw();
    uint64_t tl0 = 0, tl1;

    nv2a_texup_ctl(s_presents);
    s_tu_timing = algo != 0 || xbox_FrametimeOn();
    last_ns = now;
    if (s_tq) {
        uint64_t done = serial_done();
        if (s_presents >= 3)
            gpu_ms = tq_frame((unsigned)((s_presents - 3) % TQ_FRAME));
        for (i = 0; i < TQ_CHUNK; i++) {
            uint64_t ts[2];
            unsigned q = (TQ_FRAME * TQ_SUB + (unsigned)i) * 2;
            if (!s_tq_chunk_serial[i] || done < s_tq_chunk_serial[i])
                continue;
            if (vkGetQueryPoolResults(s_dev, s_tq, q, 2, sizeof ts, ts, 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
                double ms = (double)(ts[1] - ts[0]) * C->props.limits.timestampPeriod / 1e6;
                tu_ms += ms;
                nv2a_texup_gov_measured(&s_gov, s_tq_chunk_algo[i], s_tq_chunk_pred[i], ms);
                st->chunk_gpu_ms += ms;
                st->chunk_pred_ms += s_tq_chunk_pred[i];
                st->chunk_buffers++;
            }
            vkResetQueryPool(s_dev, s_tq, q, 2);
            s_tq_chunk_serial[i] = 0;
        }
    }
    xbox_FrametimeGpu(gpu_ms, tu_ms, s_tu_last_pred);
    if (!s_tu_inited) {
        last_uploads = s_tex_uploads;
        s_tu_3d = 0;
        if (s_presents % 300 == 0 && tu_log())
            fprintf(stderr, "[TEXUP] flip %llu: off, images %.1f MB\n",
                    (unsigned long long)s_presents, (double)nvk_image_bytes() / 1048576.0);
        return;
    }
    tl0 = tu_log() ? now_ns() : 0;
    {
        /* BO-272: where the title is (its loading screen, the countdown);
         * graphics.texture_upscale_preload off: always play, as before. */
        int ld = nv2a_texup_preload() ? nv2a_texup_loading() : NV2A_TEXUP_PLAY;
        if (ld != s_gov.loading && tu_log())
            fprintf(stderr, "[TEXUP] title: %s, flip %llu\n",
                    ld == NV2A_TEXUP_LOADING ? "loading" : ld == NV2A_TEXUP_PRESTART ? "prestart"
                                                                                  : "play",
                    (unsigned long long)s_presents);
        nv2a_texup_gov_loading(&s_gov, ld);
    }
    nv2a_texup_gov_frame(&s_gov, now, gpu_ms, frame_ms,
                         nv2a_texup_preload()
                             ? nv2a_texup_period_ms(xbox_FlipHostPeriodNs(),
                                                    xbox_HostFlipIntervalAsked())
                             : nv2a_texup_period_ms(0, xbox_HostFlipIntervalAsked()),
                         s_tu_3d, (unsigned)(s_tex_uploads - last_uploads), computed);
    last_uploads = s_tex_uploads;
    s_tu_3d = 0;
    s_tu_last_pred = 0;
    s_tu_bytes_left = nv2a_texup_gov_swap_bytes(&s_gov);
    /* A finished job: its result to the disk cache once its submit is done. */
    for (i = 0; i < TU_SLOTS; i++) {
        TuSlot *s = &s_tu[i];
        int done;
        if (!s->used)
            continue;
        done = s->serial && serial_done() >= s->serial
            && (!s->job || s->job->finished);
        if (done && s->job) {
            tu_save(s);
            s->job->rb_idle = 1;
        }
        if (done && !s->done_flip)
            s->done_flip = s_presents;
        /* BO-272: the next step without waiting for a bind (preload off:
         * at the bind only, as before). */
        if (nv2a_texup_preload() && s->e && s->e->live && s->e->tu_slot == i + 1
                && (s->e->tu == TU_DISK || s->e->tu == TU_BUSY)) {
            tu_step(s->e, s);
            if (!s->used)
                continue;
        }
        /* ... and a disk read whose texture was not bound again in 10 s
         * gives its slot back (read again at the next bind), so a few
         * such cannot hold all eight. */
        if ((!s->e && (done || !s->serial) && i != s_tu_job)
                || (s->done_flip && s_presents - s->done_flip > nv2a_texup_free_flips())
                || (s->e && s->e->tu == TU_DISK && !s->tex.img && !s->serial
                    && s_presents - s->start_flip > nv2a_texup_free_flips())) {
            if (s->e)
                s->e->tu = TU_NONE;
            tu_slot_free(i);
        }
    }
    /* RECOMP_DEBUG=texup_stress=n: every n flips the running job's texture
     * is taken as changed (its entry uploads it again at its next bind), the
     * way a title's own change goes (the car select's textures): a stress
     * test of the job's hold on the image it reads. */
    {
        static int every = -1;
        if (every < 0) {
            const char *x = recomp_env(RENV_TEXUP_STRESS);
            every = x && *x ? atoi(x) : 0;
        }
        if (every > 0 && s_tu_job >= 0 && s_tu[s_tu_job].e && s_tu[s_tu_job].job
                && s_tu[s_tu_job].job->started && !s_tu[s_tu_job].job->finished
                && s_presents % (unsigned)every == 0) {
            TexEntry *e = s_tu[s_tu_job].e;
            if (tu_log())
                fprintf(stderr, "[TEXUP] stress: %016llx taken as changed, flip %llu\n",
                        (unsigned long long)e->hash, (unsigned long long)s_presents);
            e->hash ^= 1;
            e->gen = 0;
        }
    }
    if (s_tu_job >= 0 && s_tu[s_tu_job].job && s_tu[s_tu_job].job->finished
            && s_tu[s_tu_job].serial)
        s_tu_job = -1;
    if (algo && tu_state() == 1) {
        /* The jobs: in play one new job a flip and its chunks under the
         * budget; before the GO and on the loading screen (BO-272) the
         * budget is larger and, as long as it and the render-thread
         * allowance last, a finished job is followed by the next in the
         * same flip. */
        unsigned started = 0, maxj = nv2a_texup_gov_jobs(&s_gov);
        unsigned q = s_tq_chunk_next % TQ_CHUNK;
        double left, pred_all = 0, rt_ms = nv2a_texup_gov_rt_ms(&s_gov);
        uint64_t rt0 = now_ns();
        int timed = 0, algo0 = 0;
        budget = nv2a_texup_gov_budget(&s_gov, now);
        left = budget;
        for (;;) {
            TuSlot *s;
            double pred;
            uint64_t tn;
            if (s_tu_job >= 0 && s_tu[s_tu_job].job && s_tu[s_tu_job].job->finished
                    && s_tu[s_tu_job].serial)
                s_tu_job = -1;
            while (s_tu_job < 0 && s_tu_fn && started < maxj
                   && (!started || (double)(now_ns() - rt0) / 1e6 < rt_ms)) {
                TexEntry *e = &s_tc[s_tu_fifo[s_tu_fh]];
                s = tu_slot_of(e);
                s_tu_fh = (s_tu_fh + 1) % TEX_CACHE;
                s_tu_fn--;
                if (!e->live || e->tu != TU_WAIT || !s || s->e != e)
                    continue;
                tn = tl0 ? now_ns() : 0;
                s->job = tu_job_new(&e->im, e->width, e->height, e->levels, (int)s->k.algo,
                                    s->scale, s->k.wrap);
                if (tn)
                    s_tu_ns_new += now_ns() - tn;
                if (!s->job) {
                    e->tu = TU_NO;
                    tu_slot_free((int)(s - s_tu));
                    continue;
                }
                e->tu = TU_BUSY;
                s_tu_job = (int)(s - s_tu);
                st->computed++;
                started++;
                if (tu_log())
                    fprintf(stderr, "[TEXUP] job %016llx %ux%u x%u %s: %.2f ms predicted%s\n",
                            (unsigned long long)e->hash, e->width, e->height, s->scale,
                            nv2a_texup_algo_name((int)s->k.algo), tu_job_cost(s->job),
                            s_gov.loading ? " (before the race)" : "");
            }
            if (s_tu_job < 0 || left <= 0 || s_tu[s_tu_job].job->finished)
                break;
            s = &s_tu[s_tu_job];
            tn = tl0 ? now_ns() : 0;
            ensure_up();
            if (!timed && s_tq && !s_tq_chunk_serial[q]) {
                vkCmdWriteTimestamp(s_up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tq,
                                    (TQ_FRAME * TQ_SUB + q) * 2);
                timed = 1;
                algo0 = (int)s->k.algo;
            }
            pred = tu_job_encode(s->job, s_up, left, s_gov.factor[s->k.algo]);
            left -= pred;
            pred_all += pred;
            if (tn)
                s_tu_ns_enc += now_ns() - tn;
            /* Submitted at once, behind the frame: its serial is the next. */
            if (!s->job->finished)
                break;                   /* the budget ended inside it */
            s->serial = s_next_serial;
        }
        st->queued = s_tu_fn;
        if (timed) {
            vkCmdWriteTimestamp(s_up, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tq,
                                (TQ_FRAME * TQ_SUB + q) * 2 + 1);
            s_tq_chunk_serial[q] = s_next_serial;
            s_tq_chunk_pred[q] = pred_all;
            s_tq_chunk_algo[q] = algo0;
            s_tq_chunk_next++;
        }
        s_tu_last_pred = pred_all;
    }
    if (s_tu_ndisk) {
        for (i = 0; i < s_tu_ndisk; i++)
            s_tu[s_tu_disk_pending[i]].serial = s_next_serial;
        s_tu_ndisk = 0;
    }
    if (s_up) {
        uint64_t tn = tl0 ? now_ns() : 0;
        flush(0, FL_FLIP);
        if (tn)
            s_tu_ns_sub += now_ns() - tn;
    }
    if (tl0) {
        tl1 = now_ns() - tl0 + s_tu_ns_bind;
        if (tl1 > 1000000u)
            fprintf(stderr, "[TEXUP] flip %llu: render thread %.2f ms (binds %.2f; readback"
                    " copies %.2f, at a bind or here; new jobs %.2f, recording %.2f, submit %.2f)\n",
                    (unsigned long long)s_presents, tl1 / 1e6, s_tu_ns_bind / 1e6,
                    s_tu_ns_save / 1e6, s_tu_ns_new / 1e6, s_tu_ns_enc / 1e6, s_tu_ns_sub / 1e6);
        s_tu_ns_bind = s_tu_ns_save = s_tu_ns_new = s_tu_ns_enc = s_tu_ns_sub = 0;
    }
    st->resident_bytes = s_tu_resident;
    if (s_presents % 300 == 0 && (algo || tu_log())) {
        struct nv2a_texup_stats x;
        nv2a_texup_stats(&x);
        fprintf(stderr, "[TEXUP] flip %llu: %s, images %.1f MB, %u upscaled (%.1f MB), %u waiting;"
                " jobs %llu, from disk %llu, computed %llu, saved %llu, dropped %llu, refused %llu,"
                " demoted %llu;"
                " chunks %.1f ms GPU (%.1f predicted) in %llu buffers; factor %.2f; pauses %llu\n",
                (unsigned long long)s_presents, nv2a_texup_algo_name(algo),
                (double)nvk_image_bytes() / 1048576.0, x.upscaled,
                (double)x.resident_bytes / 1048576.0, x.queued, (unsigned long long)x.jobs,
                (unsigned long long)x.from_disk, (unsigned long long)x.computed,
                (unsigned long long)x.saved, (unsigned long long)x.dropped,
                (unsigned long long)x.refused,
                (unsigned long long)x.demoted, x.chunk_gpu_ms, x.chunk_pred_ms,
                (unsigned long long)x.chunk_buffers, algo ? s_gov.factor[algo] : 0.0,
                (unsigned long long)s_gov.pauses);
    }
}

/* The texture cache's lookup (tex_bind): what makes an entry the stage's,
 * and a small table from a hash of that key to the entry last found. */
static int tc_match(const TexEntry *c, const Stage *t)
{
    return c->live && c->offset == t->offset && c->color == t->color
        && c->width == t->width && c->height == t->height
        && c->pitch == t->pitch && c->palette == t->palette
        && c->levels == t->levels && c->sign == stage_signed(t);
}

#define TC_HINT 1024
static uint16_t s_tc_hint[TC_HINT];   /* entry + 1, 0: none */

static unsigned tc_hint_key(const Stage *t)
{
    uint32_t h = t->offset * 0x9E3779B1u;
    h ^= ((uint32_t)t->color << 24) ^ (t->width << 12) ^ t->height ^ (t->palette * 0x85EBCA6Bu);
    h ^= h >> 15;
    return h & (TC_HINT - 1);
}

static VkImageView tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];
    unsigned hk;
    unsigned want;
    uint32_t cut;
    int algo;

    s_bind_tu = 0;
    s_bind_scale = 0;
    s_bind_zbias = 0.0f;
    s_bind_hgain = NULL;
    s_bind_solo = 0;

    if (t->valid && (uint64_t)t->width * t->height <= TEX_MAX_TEXELS)
        bytes = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    /* A render target over the bytes is written back first (Metal's decode
     * sync). */
    if (bytes) {
        uint32_t lo = t->offset & 0x07FFFFFFu, hi = lo + bytes;
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            uint32_t rlo = rt->addr & 0x07FFFFFFu;
            if (rt->im.img && rt->dirty && rt->pitch
                    && lo < rlo + rt->pitch * rt->h && rlo < hi) {
                if (rt_drop_if_stale(rt, "sync"))
                    continue;
                s_decode_syncs++;
                if (vk_trace())
                    fprintf(stderr, LOGP "decode sync: target 0x%08X %ux%u (%u bytes) for"
                            " texture 0x%08X fmt 0x%02X %ux%u (%u bytes), flip %llu\n",
                            rt->offset, rt->w, rt->h, rt->pitch * rt->h, t->offset,
                            t->color, t->width, t->height, bytes,
                            (unsigned long long)s_presents);
                rt_sync(rt, rt->pitch);
            }
        }
    }
    if (!bytes) {
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black.view;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta.view;
    }
    /* BO-277: the entry this stage's key found last, checked, before the
     * walk over all 256 (a texture bind is ~5 a draw, ~6000 a frame: the
     * walk was ~0.25 ms a frame of the render thread). A miss walks as
     * before, for the entry or the victim. */
    hk = tc_hint_key(t);
    if (s_tc_hint[hk] && tc_match(&s_tc[s_tc_hint[hk] - 1], t))
        e = &s_tc[s_tc_hint[hk] - 1];
    for (i = 0; !e && i < TEX_CACHE; i++) {
        TexEntry *c = &s_tc[i];
        if (tc_match(c, t)) {
            e = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    s_tc_hint[hk] = (uint16_t)((e ? e : victim) - s_tc + 1);
    /* graphics.ui_scale: a texture a 2D draw binds is scaled, from then on
     * and for every draw (off: want is 0 and nothing here differs). */
    want = nv2a_texscale_factor();
    algo = (int)nv2a_texscale_algo();
    if (want && !((s_draw_2d || (e && e->ui)) && !stage_signed(t)
                  && nv2a_texscale_tex_ok(nv2a_tex_size_from_format(t->color), t->width,
                                          t->height, t->levels)))
        want = 0;
    /* BO-171: a mipmapped texture an alpha-tested draw binds gets levels
     * made for that test, once (as Metal). */
    cut = 0;
    if (nv2a_texmip_mode() & NV2A_TEXMIP_LEVELS) {
        const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
        if (t->levels > 1 && nv2a_tex_filter_mip(t->filter) && !stage_signed(t))
            cut = nv2a_texmip_cut_ref(v->atest_enable, v->atest_func, v->atest_ref);
        if (e && e->cut)
            cut = e->cut;
    }
    if (e) {
        /* A replaced texture whose variant changed is uploaded again; so is
         * one whose scale is not the one wanted now. */
        int stale = (e->rep && e->rep != nv2a_texrep_generation())
                 || e->scale != want || (want && e->salgo != algo) || e->cut != cut
                 /* BO-245, as Metal */
                 || (e->tu_add && (e->tgen != nv2a_texup_generation() || e->tu_demote));
        if (s_draw_2d)
            e->ui = 1;
        if (e->gen != g->flips || stale) {
            uint64_t h = tex_hash_pal(mem, t, bytes);
            s_tex_rehash++;
            e->gen = g->flips;
            if (h != e->hash || stale) {
                struct nvk_image ni;
                if (e->tu || e->tu_slot || e->tu_add)
                    tu_detach(e, e->tu == TU_SEEN && h != e->hash);
                e->hash = h;
                s_tex_changed++;
                if (!tex_upload(t, want, algo, cut, &ni))
                    return s_black.view;
                tex_retire(&e->im);
                e->rep = s_tex_rep;
                e->cut = cut;
                e->zbias = nv2a_texrep_zbias(mem + t->offset, t->color, t->width,
                                             t->height, t->pitch);
                e->rhash = s_up_rhash;
                e->hgain = nv2a_texrep_hdr_gain(mem + t->offset, t->color, t->width,
                                                t->height, t->pitch);
                e->solo = (uint8_t)nv2a_texrep_hdr_stage0_only(mem + t->offset, t->color,
                                                               t->width, t->height, t->pitch);
                e->im = ni;
                e->scale = (uint8_t)want;
                e->salgo = (uint8_t)algo;
                e->up_scale = (uint8_t)s_bind_scale;
                e->pend = (uint8_t)s_up_pend;
                e->skey = s_up_key;
                e->pgen = g->flips;
                e->pal_len = t->pal_len;
            }
        }
    } else {
        struct nvk_image ni;
        e = victim;
        if (e->live)
            tex_retire(&e->im);
        if (e->tu || e->tu_slot || e->tu_add)
            tu_detach(e, 0);
        e->tgen = nv2a_texup_generation();
        memset(&e->im, 0, sizeof e->im);
        e->live = 0;
        if (!tex_upload(t, want, algo, cut, &ni))
            return s_black.view;
        e->ui = (uint8_t)(s_draw_2d != 0);
        e->scale = (uint8_t)want;
        e->salgo = (uint8_t)algo;
        e->up_scale = (uint8_t)s_bind_scale;
        e->pend = (uint8_t)s_up_pend;
        e->skey = s_up_key;
        e->pgen = g->flips;
        e->pal_len = t->pal_len;
        e->use = 0;
        e->cut = cut;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->sign = stage_signed(t);
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        e->rep = s_tex_rep;
        e->zbias = nv2a_texrep_zbias(mem + t->offset, t->color, t->width, t->height,
                                     t->pitch);
        e->rhash = s_up_rhash;
        e->hgain = nv2a_texrep_hdr_gain(mem + t->offset, t->color, t->width, t->height,
                                        t->pitch);
        e->solo = (uint8_t)nv2a_texrep_hdr_stage0_only(mem + t->offset, t->color, t->width,
                                                       t->height, t->pitch);
        e->im = ni;
        e->live = 1;
    }
    /* BO-245: graphics.texture_upscale's steps (Off: only an entry with a
     * slot still out). */
    if (nv2a_texup_algo() || e->tu_slot)
        texup_bind(e, t, mem);
    s_bind_tu = e->tu_add;
    /* Its scaled picture was being made: asked for once a flip, swapped in
     * when it is there (as Metal). The old image goes when the frames that
     * may use it are done. */
    if (e->pend && e->pgen != g->flips && s_swap_flip != g->flips) {
        e->pgen = g->flips;
        if (!scaled_room(t, e->scale)) {
            e->pend = 0;
        } else {
            uint64_t t0 = ui_scale_log() ? now_ns() : 0;
            int r = nv2a_texscale_get(e->skey, NULL, t->width, t->height, e->scale,
                                      (enum nv2a_texscale_algo)e->salgo, 0, s_scaled);
            if (r == NV2A_TEXSCALE_READY) {
                struct nvk_image ni;
                s_swap_flip = g->flips;   /* one a flip: its upload is the cost */
                e->pend = 0;
                if (scaled_image(t, e->scale, &ni)) {
                    tex_retire(&e->im);
                    e->im = ni;
                    e->up_scale = e->scale;
                    s_tex_uploads++;
                    s_sn.swaps++;
                }
            } else if (r != NV2A_TEXSCALE_PENDING) {
                e->pend = 0;         /* refused or lost: stays as the title's */
            }
            if (ui_scale_log() && r != NV2A_TEXSCALE_PENDING)
                fprintf(stderr, "[UISCALE] swap %016llx %ux%u x%u: %s, %.3f ms on the render"
                        " thread, flip %llu\n", (unsigned long long)e->skey, t->width,
                        t->height, (unsigned)e->scale,
                        r == NV2A_TEXSCALE_READY ? "scaled texture in" : "no result",
                        (double)(now_ns() - t0) / 1e6, (unsigned long long)s_presents);
        }
    }
    e->used = ++s_tc_clock;
    s_bind_scale = e->up_scale;
    s_bind_zbias = e->zbias;
    s_bind_rhash = e->rhash;
    s_bind_hgain = e->hgain;
    s_bind_solo = e->solo;
    s_sn.scaled += e->up_scale != 0;
    if (ui_scale_log())
        ui_scale_use(e, t, mem);
    return e->im.view;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp. */
static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

#define SMP_MAX 128
static struct { uint64_t key; VkSampler s; } s_smp[SMP_MAX];
static int s_smp_n;

static VkSampler sampler(const Stage *t, unsigned scale, int add)
{
    static int point = -1;
    VkSamplerCreateInfo sc;
    VkSampler st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i, aniso;
    uint64_t key;

    if (point < 0)
        point = recomp_env(RENV_VK_POINT) != NULL;
    mn = !point && nv2a_tex_filter_linear(t->filter, 0);
    mg = !point && nv2a_tex_filter_linear(t->filter, 1);
    mip = t->levels > 1 ? nv2a_tex_filter_mip(t->filter) : 0;
    /* A scaled 2D texture has the levels down to the title's size, blended
     * between (as Metal). */
    if (scale)
        mip = 2;
    /* BO-245: an upscaled texture has 'add' levels above the title's; one
     * the title gave one level is blended down to its own size. */
    if (add && !mip)
        mip = 2;
    /* Anisotropy only on a mipmapped sampler with a linear minification
     * filter, as asked (RECOMP_GFX_ANISO), within the device's limit. */
    {
        const struct nvk_core *core = nvk_core_get();
        aniso = (mip && mn && !scale && core && core->max_aniso > 1.0f) ? nv2a_host_aniso() : 1;
        if (aniso > 1 && (float)aniso > core->max_aniso)
            aniso = (int)core->max_aniso;
    }
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    key |= (uint64_t)aniso << 33;       /* the level is part of the sampler */
    key |= (uint64_t)scale << 40;
    key |= (uint64_t)add << 44;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key)
            return s_smp[i].s;
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE;
    sc.maxAnisotropy = aniso > 1 ? (float)aniso : 1.0f;
    sc.minFilter = mn ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.magFilter = mg ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = address_mode(u);
    sc.addressModeV = address_mode(v);
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.minLod = scale ? 0.0f : mip ? (float)t->lod_min : 0.0f;
    /* Not mipmapped: level 0, with the min/mag choice kept (the Vulkan
     * spec's maxLod 0.25 for Metal's MipFilterNotMipmapped). */
    sc.maxLod = scale ? (float)(scale_levels(scale) - 1u) : mip ? (float)t->lod_max : 0.25f;
    if (add) {
        /* The title's clamps move down by the levels added, except a
         * minimum of 0, which keeps the new detail (as Metal). */
        sc.minLod = t->levels > 1 && t->lod_min ? (float)(t->lod_min + (uint32_t)add) : 0.0f;
        sc.maxLod = t->levels > 1 ? (float)(t->lod_max + (uint32_t)add) : (float)add;
    }
    if (vkCreateSampler(s_dev, &sc, NULL, &st) != VK_SUCCESS)
        return s_smp_n ? s_smp[0].s : VK_NULL_HANDLE;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % SMP_MAX);
        /* A recorded draw may still use it. */
        if (!flush(1, FL_SAMPLER)) {
            /* Drawing has stopped: the old one stays, the new one is unused. */
            vkDestroySampler(s_dev, st, NULL);
            return s_smp[i].s;
        }
        vkDestroySampler(s_dev, s_smp[i].s, NULL);
    }
    s_smp[i].key = key;
    s_smp[i].s = st;
    return st;
}

/* ---- render-to-texture: Metal's rt_texture ----------------------------------
 *
 * A stage whose texture is a surface drawn here samples the render target
 * itself; 16-bit (R5G6B5) ones through a view whose alpha reads 1 (xemu's
 * check_surface_to_texture_compatiblity, hw/xbox/nv2a/pgraph/vk/texture.c).
 * The target being drawn into is sampled through a copy (s_self). The image
 * goes to SHADER_READ_ONLY, which ends an open pass when it is not there. */
static struct nvk_image s_self;       /* the copy */
static VkImageView      s_self_gb;    /* its G8B8 view (rt_g8b8_view's swizzle) */
static uint32_t         s_self_w, s_self_h, s_self_offset;

static VkImageView rt_opaque(RenderTarget *rt)
{
    if (!rt->opaque) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = rt->im.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = rt->im.format;
        vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &rt->opaque) != VK_SUCCESS)
            rt->opaque = VK_NULL_HANDLE;
    }
    return rt->opaque;
}

/* rt's image readable by the fragment shader: outside any pass. */
static void rt_to_sampled(struct nvk_image *im)
{
    if (im->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    end_encoder();
    ensure_cmd();
    nvk_image_to(s_cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* G8B8 surfaces (SET_SURFACE_FORMAT colour 0xA): the console keeps the
 * draw's green and blue, G in the high byte and B in the low one (the name
 * reads MSB first, as A8R8G8B8's does), and a LIN_G8B8 / G8B8 texture over
 * them reads the low byte as red and blue, the high one as green and alpha
 * (nv2a_tex_decode_texel). The target's image holds the whole BGRA colour,
 * so this view gives the texture's reading of it: R = B, G = G, B = B,
 * A = G. */
static int surf_is_g8b8(uint32_t cfmt) { return cfmt == 0xA; }   /* MA2's: 10 [traced] */

static VkImageView rt_g8b8_view(RenderTarget *rt)
{
    if (!rt->gb) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = rt->im.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = rt->im.format;
        vi.components.r = VK_COMPONENT_SWIZZLE_B;
        vi.components.g = VK_COMPONENT_SWIZZLE_G;
        vi.components.b = VK_COMPONENT_SWIZZLE_B;
        vi.components.a = VK_COMPONENT_SWIZZLE_G;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &rt->gb) != VK_SUCCESS)
            rt->gb = VK_NULL_HANDLE;
    }
    return rt->gb;
}

static RenderTarget *rt_texture(const Stage *t, RenderTarget *cur, VkImageView *view,
                                uint32_t *sw, uint32_t *sh)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i, r565, x8, g8b8;

    if (off < 0)
        off = recomp_env(RENV_VK_NO_RTT) != NULL;
    if (off || !t->offset || d3d8_format_dxt_block_bytes(t->color))
        return NULL;
    /* A shadow buffer reads the depth packed into texels (tex_bind's
     * decode), never a colour target over the same bytes. */
    if (nv2a_tex_depth_kind(t->color))
        return NULL;
    r565 = nv2a_tex_linear_twin(t->color) == 0x11;
    /* X8R8G8B8 (swizzled or linear) has no alpha: decoded from memory it
     * reads 1 (nv2a_tex_decode_texel), and sampled from the target's image it
     * must too, not what the draws left there. Double-S.T.E.A.L 2 blends its
     * sharp scene over its glow layers by that alpha; read as the image's,
     * the scene dropped out and only the blur was left. */
    x8 = nv2a_tex_linear_twin(t->color) == 0x1E;
    /* G8B8 over a G8B8 target of its size, as drawn (MechAssault 2's two
     * 256x256 linear ones, sampled every frame): read on the GPU. Through a
     * write-back each was a GPU wait and a re-upload, two a frame: cheap on
     * shared memory, a PCIe round trip and a stall on a discrete card (26 ms
     * of executor a frame on an RTX 4070, 8-11 on the 890M). */
    g8b8 = nv2a_tex_linear_twin(t->color) == 0x17;
    if (g8b8) {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            VkImageView gv;
            if (!rt->im.img || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
                continue;
            if (!surf_is_g8b8(rt->cfmt) || rt->pitch / rt->w != 2u
                    || rt->w != t->width || rt->h != t->height
                    || (d3d8_format_is_swizzled(t->color) != 0) != (rt->swz != 0)
                    || (!rt->swz && t->pitch != rt->pitch))
                return NULL;
            if (rt_drop_if_stale(rt, "bind"))
                return NULL;
            if (rt == cur) {            /* drawn into: through the copy, below */
                self = rt;
                break;
            }
            gv = rt_g8b8_view(rt);
            if (!gv)
                return NULL;
            *view = gv;
            rt_to_sampled(&rt->im);
            *sw = rt->w;
            *sh = rt->h;
            return rt;
        }
        if (!self)
            return NULL;
    }
    if (!g8b8 && !r565 && nv2a_tex_texel_bytes(nv2a_tex_linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX && !g8b8; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (r565 && (rt->pitch / rt->w != 2u || rt->w != t->width || rt->h != t->height
                     || (t->color == 0x11 ? rt->swz || t->pitch != rt->pitch : !rt->swz)))
            continue;
        if (rt_drop_if_stale(rt, "bind"))
            continue;
        /* A texture that reads the target's bytes in the other layout (a
         * swizzled format over a target drawn linear, or the reverse) sees
         * them reordered on the console, and an image sampled as drawn shows
         * none of that: through guest memory it is right. Double-S.T.E.A.L
         * 2 draws 0x00B30000 linear and samples it swizzled; sampled as
         * drawn, its glow layer covered the frame. */
        if (!r565 && (d3d8_format_is_swizzled(t->color) != 0) != (rt->swz != 0))
            return NULL;
        if (rt == cur) {
            if (!r565)
                self = rt;
            continue;
        }
        if (r565 || x8) {
            VkImageView ov = rt_opaque(rt);
            if (!ov)
                return NULL;
            *view = ov;
        } else {
            *view = rt->im.view;
        }
        rt_to_sampled(&rt->im);
        *sw = rt->w;
        *sh = rt->h;
        return rt;
    }
    if (!self)
        return NULL;
    if (!s_self.img || s_self_w != self->w || s_self_h != self->h
            || s_self.w != self->hw || s_self.h != self->hh || s_self.format != self->im.format) {
        if (s_self.img)
            garbage_push(&s_self, s_self_gb, NULL);
        s_self_gb = VK_NULL_HANDLE;
        if (!nvk_image_new(&s_self, self->im.format, self->hw, self->hh, 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
            memset(&s_self, 0, sizeof s_self);
            return NULL;
        }
        s_self_w = self->w;
        s_self_h = self->h;
    }
    {
        VkImageCopy ic;
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &self->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        nvk_image_to(s_cmd, &s_self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = self->hw;
        ic.extent.height = self->hh;
        ic.extent.depth = 1;
        vkCmdCopyImage(s_cmd, self->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_self.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        nvk_image_to(s_cmd, &s_self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    s_self_offset = self->offset;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", self->offset);
    }
    *view = s_self.view;
    if (g8b8) {
        if (!s_self_gb) {
            VkImageViewCreateInfo vi;
            memset(&vi, 0, sizeof vi);
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = s_self.img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = s_self.format;
            vi.components.r = VK_COMPONENT_SWIZZLE_B;
            vi.components.g = VK_COMPONENT_SWIZZLE_G;
            vi.components.b = VK_COMPONENT_SWIZZLE_B;
            vi.components.a = VK_COMPONENT_SWIZZLE_G;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(s_dev, &vi, NULL, &s_self_gb) != VK_SUCCESS)
                s_self_gb = VK_NULL_HANDLE;
        }
        if (!s_self_gb)
            return NULL;
        *view = s_self_gb;
    }
    *sw = self->w;
    *sh = self->h;
    return self;
}

/* ---- cube maps (BO-8): Metal's cube_bind ---------------------------------------
 *
 * A CUBEMAP stage samples a samplerCube (d3d8_combiners_glsl.c). The six
 * faces sit face_stride apart (nv2a_tex_face_stride, xemu texture.c); a face
 * this backend drew -- Burnout 3 renders its car reflections into six 128x128
 * targets every frame -- is copied from that target on the GPU whenever it
 * was drawn since the last copy (xemu renders a surface into the texture
 * that reads it, pgraph/gl/surface.c pgraph_gl_render_surface_to_texture);
 * the other faces decode from guest memory, re-checked once per flip; a
 * change makes a new cube, so a frame in flight keeps the one it used. */
#define CUBE_MAX 8
typedef struct {
    uint32_t         offset, color, width, levels, side;   /* side: host face size */
    uint32_t         gen, used;
    uint64_t         hash;
    uint64_t         face_gen[6];
    struct nvk_image im;
} CubeEntry;
static CubeEntry        s_cube[CUBE_MAX];
static uint32_t         s_cube_clock;
static struct nvk_image s_black_cube;
static uint64_t         s_cube_copies, s_cube_uploads;

static VkImageView black_cube(void)
{
    if (!s_black_cube.img) {
        static const uint32_t px = 0xFF000000u;
        int f;
        if (!nvk_image_new_cube(&s_black_cube, RT_FORMAT, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return s_black.view;   /* a 2D view: invalid there, but never NULL */
        for (f = 0; f < 6; f++)
            upload_layer(&s_black_cube, (uint32_t)f, 0, 1, 1, &px,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, f == 5);
    }
    return s_black_cube.view;
}

static RenderTarget *cube_face_rt(uint32_t addr, uint32_t w)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || ((rt->addr ^ addr) & 0x07FFFFFFu) || rt->w < w || rt->h < w)
            continue;
        if (rt_drop_if_stale(rt, "bind"))
            continue;
        return rt;
    }
    return NULL;
}

static VkImageView cube_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t *pal = t->color == NV2A_TEX_P8 ? (const uint32_t *)(mem + t->palette) : NULL;
    RenderTarget *frt[6];
    uint32_t w = t->width, side = t->width, levels, stride, bytes, f, l;
    CubeEntry *e = NULL, *victim = &s_cube[0];
    int any = 0, all = 1, fresh = 0, ncopy = 0, i;

    if (!t->cube || !w || t->width != t->height || (uint64_t)w * w * 6u > TEX_MAX_TEXELS)
        return black_cube();
    stride = nv2a_tex_face_stride(t->color, w, w, t->pitch, t->fmt_levels);
    bytes = nv2a_tex_extent(t->color, w, w, t->pitch, t->levels);
    if (!bytes)
        return black_cube();
    for (f = 0; f < 6; f++) {
        frt[f] = cube_face_rt(t->offset + f * stride, w);
        if (frt[f]) {
            any = 1;
            side = w * frt[f]->hw / frt[f]->w;
        } else {
            all = 0;
        }
    }
    levels = any ? 1u : t->levels;
    for (i = 0; i < CUBE_MAX; i++) {
        CubeEntry *c = &s_cube[i];
        if (c->im.img && c->offset == t->offset && c->color == t->color
                && c->width == w && c->levels == levels) {
            e = c;
            break;
        }
        if (!c->im.img || (victim->im.img && c->used < victim->used))
            victim = c;
    }
    if (!e) {
        e = victim;
        if (e->im.img)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(e, 0, sizeof *e);
        e->offset = t->offset;
        e->color = t->color;
        e->width = w;
        e->levels = levels;
        fresh = 1;
    }
    if (e->side != side || (e->im.img && e->im.format != (any ? RT_FMT : RT_FORMAT)))
        fresh = 1;
    if (!all && (fresh || e->gen != g->flips)) {
        uint64_t h = NV2A_TEX_HASH_SEED;
        for (f = 0; f < 6; f++)
            if (!frt[f])
                h = h * 31u + nv2a_tex_hash(mem + t->offset + f * stride, bytes,
                                            NV2A_TEX_HASH_SEED);
        if (pal)
            h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
        e->gen = g->flips;
        if (h != e->hash)
            fresh = 1;
        e->hash = h;
    }
    if (fresh) {
        size_t n = nv2a_tex_texels(w, w, levels);
        int uploaded = 0;
        if (e->im.img)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(&e->im, 0, sizeof e->im);
        e->side = side;
        memset(e->face_gen, 0, sizeof e->face_gen);
        if (!nvk_image_new_cube(&e->im, any ? RT_FMT : RT_FORMAT, side, levels,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return black_cube();
        if (n > s_texels_cap) {
            uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
            if (!p)
                return black_cube();
            s_texels = p;
            s_texels_cap = n;
        }
        for (f = 0; f < 6; f++) {
            const uint32_t *src = s_texels;
            if (frt[f])
                continue;
            nv2a_tex_decode(mem + t->offset + f * stride, t->color, w, w, t->pitch,
                            levels, pal, pal ? t->pal_len : 0, s_texels);
            if (side != w) {    /* beside drawn faces at render.scale: upscaled */
                uint8_t *up = (uint8_t *)malloc((size_t)side * side * 4u);
                if (up) {
                    nv2a_upscale_nearest32((const uint8_t *)s_texels, w * 4u, w, w,
                                           side / w, up, side * 4u);
                    upload_layer(&e->im, f, 0, side, side, up,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);
                    free(up);
                    uploaded = 1;
                }
                continue;
            }
            for (l = 0; l < levels; l++) {
                uint32_t lw = nv2a_tex_level_dim(w, l);
                upload_layer(&e->im, f, l, lw, lw, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);
                src += (size_t)lw * lw;
            }
            uploaded = 1;
        }
        if (uploaded && !any)
            nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else if (!uploaded) {
            /* All faces drawn: the copies below start from an undefined
             * image; a face never copied reads black (cleared). */
            VkClearColorValue z;
            VkImageSubresourceRange r;
            memset(&z, 0, sizeof z);
            z.float32[3] = 1.0f;
            memset(&r, 0, sizeof r);
            r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.levelCount = VK_REMAINING_MIP_LEVELS;
            r.layerCount = VK_REMAINING_ARRAY_LAYERS;
            ensure_up();
            nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_up, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
            if (!any)
                nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        s_cube_uploads++;
    }
    for (f = 0; f < 6; f++)
        if (frt[f] && frt[f]->gen != e->face_gen[f])
            ncopy++;
    if (ncopy) {
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (f = 0; f < 6; f++) {
            VkImageCopy ic;
            if (!frt[f] || frt[f]->gen == e->face_gen[f])
                continue;
            nvk_image_to(s_cmd, &frt[f]->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            memset(&ic, 0, sizeof ic);
            ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ic.srcSubresource.layerCount = 1;
            ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ic.dstSubresource.baseArrayLayer = f;
            ic.dstSubresource.layerCount = 1;
            ic.extent.width = side;
            ic.extent.height = side;
            ic.extent.depth = 1;
            vkCmdCopyImage(s_cmd, frt[f]->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
            e->face_gen[f] = frt[f]->gen;
            s_cube_copies++;
        }
    }
    if (e->im.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    e->used = ++s_cube_clock;
    return e->im.view;
}

/* ---- descriptor sets --------------------------------------------------------- */

static VkDescriptorSet texture_set(const VkImageView v[4], const VkSampler s[4])
{
    Frame *f = &s_fr[s_fi];
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int k;

    if (s_tset_last && !memcmp(v, s_tset_views, sizeof s_tset_views)
            && !memcmp(s, s_tset_smp, sizeof s_tset_smp))
        return s_tset_last;
    for (;;) {
        if (f->dp_cur < f->ndps) {
            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = f->dps[f->dp_cur];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &s_dsl[1];
            if (vkAllocateDescriptorSets(s_dev, &ai, &set) == VK_SUCCESS)
                break;
            f->dp_cur++;
            continue;
        }
        {
            VkDescriptorPoolSize ps;
            VkDescriptorPoolCreateInfo pi;
            VkDescriptorPool *n = (VkDescriptorPool *)realloc(f->dps, (f->ndps + 1) * sizeof *n);
            if (!n)
                return VK_NULL_HANDLE;
            f->dps = n;
            ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ps.descriptorCount = 4096 * 4;
            memset(&pi, 0, sizeof pi);
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = 4096;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(s_dev, &pi, NULL, &f->dps[f->ndps]) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            f->ndps++;
        }
    }
    for (k = 0; k < 4; k++) {
        ii[k].sampler = s[k];
        ii[k].imageView = v[k];
        ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    memcpy(s_tset_views, v, sizeof s_tset_views);
    memcpy(s_tset_smp, s, sizeof s_tset_smp);
    s_tset_last = set;
    return set;
}

/* One constant block into the uniform ring, unless it is the last placed. */
static int ubo_place(int which, const void *p, uint32_t n, VkDeviceSize *off)
{
    uint8_t *memo = which == 0 ? s_umemo.vs : which == 1 ? s_umemo.vp : s_umemo.ps;
    void *dst;

    if (s_umemo.valid[which] && !memcmp(memo, p, n)) {
        *off = s_umemo.off[which];
        return 1;
    }
    dst = ring_alloc(RING_UB, n, s_ubo_align, off);
    if (!dst)
        return 0;
    memcpy(dst, p, n);
    memcpy(memo, p, n);
    s_umemo.off[which] = *off;
    s_umemo.valid[which] = 1;
    return 1;
}

/* ---- draws ------------------------------------------------------------------- */

/* Metal's draw_scissor: the surface clip, clamped to rt, in host pixels
 * (xemu scissors every draw to it, pgraph/gl/draw.c). */
static int draw_scissor(const struct nv2a_pb_gpu *g, const RenderTarget *rt, VkRect2D *sr)
{
    uint32_t x0 = g->clip_x, y0 = g->clip_y;
    uint32_t x1 = g->clip_x + g->clip_w, y1 = g->clip_y + g->clip_h;

    if (x1 > rt->w) x1 = rt->w;
    if (y1 > rt->h) y1 = rt->h;
    if (x0 >= x1 || y0 >= y1)
        return 0;
    sr->offset.x = (int32_t)((uint64_t)x0 * rt->hw / rt->w);
    sr->offset.y = (int32_t)((uint64_t)y0 * rt->hh / rt->h);
    sr->extent.width = (uint32_t)((uint64_t)x1 * rt->hw / rt->w) - (uint32_t)sr->offset.x;
    sr->extent.height = (uint32_t)((uint64_t)y1 * rt->hh / rt->h) - (uint32_t)sr->offset.y;
    return 1;
}

/* ---- HDR (BO-110): the frame mapped for the display at the flip ------------
 *
 * The view's target holds the title's encoding past 1.0 where its light
 * stacked (the scene) and the HUD and menus at or below 1.0 (drawn after the
 * scene, x the HUD's white over paper white in the combiners). At the flip
 * one full-screen draw reads it and writes the window's slot, 10-bit, as the
 * display takes it: decoded (gamma 2.2 or sRGB), the shadows' power below
 * mid grey, the highlight gain and the hue-kept shoulder past paper white to
 * the peak (identity up to paper white, so the HUD passes as it is), x paper
 * white in nits, BT.709 to BT.2020, ST 2084. The presenter's AA, sharpening
 * and scale then work on 32-bit pixels and the scale writes the swapchain
 * as it is. */
static const char s_hf_vs_glsl[] =
    "#version 450\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
/* The maths is video/hdr_shaders.h's (BO-244), shared with the Metal
 * composite so both give the same nits: a = (the peak in paper whites,
 * paper white / 10,000 nits, the highlight gain, 1 = the sRGB curve),
 * b = (the shadows' power, the shoulder's softness, the black level /
 * 10,000 nits). */
static const char s_hf_fs_glsl[] = HDR_FLIP_GLSL;

#define HDR_SLOT_FORMAT VK_FORMAT_A2B10G10R10_UNORM_PACK32
static VkDescriptorSetLayout s_hf_dsl;
static VkPipelineLayout      s_hf_pl;
static VkPipeline            s_hf_pso;
static VkSampler             s_hf_smp;
static VkDescriptorPool      s_hf_pool[FRAMES_MAX];
static int                   s_hf_tried;

static VkShaderModule hf_module(enum nvk_stage stage, const char *src)
{
    VkShaderModuleCreateInfo mi;
    VkShaderModule m = VK_NULL_HANDLE;
    const uint32_t *w;
    size_t n;
    int compiled;
    char err[1024];
    if (!nvk_glsl_spirv(stage, src, &w, &n, &compiled, err, sizeof err)) {
        XR_ERROR(LOGP "HDR flip pass: GLSL compile failed: %s\n", err);
        return VK_NULL_HANDLE;
    }
    memset(&mi, 0, sizeof mi);
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = n * 4;
    mi.pCode = w;
    if (vkCreateShaderModule(s_dev, &mi, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int hf_init(void)
{
    VkDescriptorSetLayoutBinding lb;
    VkDescriptorSetLayoutCreateInfo li;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli;
    VkSamplerCreateInfo si;
    VkShaderModule vs, fs;
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
    VkFormat cf = HDR_SLOT_FORMAT;
    int k;

    if (s_hf_tried)
        return s_hf_pso != VK_NULL_HANDLE;
    s_hf_tried = 1;
    memset(&lb, 0, sizeof lb);
    lb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lb.descriptorCount = 1;
    lb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &lb;
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = 32;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s_hf_dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_hf_dsl) != VK_SUCCESS
            || vkCreatePipelineLayout(s_dev, &pli, NULL, &s_hf_pl) != VK_SUCCESS
            || vkCreateSampler(s_dev, &si, NULL, &s_hf_smp) != VK_SUCCESS)
        goto fail;
    for (k = 0; k < FRAMES_MAX; k++) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 };
        VkDescriptorPoolCreateInfo pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 8;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s_dev, &pi, NULL, &s_hf_pool[k]) != VK_SUCCESS)
            goto fail;
    }
    vs = hf_module(NVK_STAGE_VERTEX, s_hf_vs_glsl);
    fs = hf_module(NVK_STAGE_FRAGMENT, s_hf_fs_glsl);
    if (vs && fs) {
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
        gp.layout = s_hf_pl;
        if (vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_hf_pso) != VK_SUCCESS)
            s_hf_pso = VK_NULL_HANDLE;
    }
    if (vs)
        vkDestroyShaderModule(s_dev, vs, NULL);
    if (fs)
        vkDestroyShaderModule(s_dev, fs, NULL);
    if (!s_hf_pso)
        goto fail;
    return 1;
fail:
    XR_ERROR(LOGP "HDR flip pass unavailable: no HDR frame is shown\n");
    return 0;
}

/* rt (hcw x hch from its corner) mapped into the window's slot (see above),
 * on s_cmd; the slot is left in GENERAL, as the copy leaves it. 0: not done. */
static int hdr_flip_pass(RenderTarget *rt, struct nvk_image *slot, uint32_t hcw, uint32_t hch)
{
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkRenderingAttachmentInfo att;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    float pk, pw, hw, sh, hl, sf, bl;
    int srgb;
    float pc[8];

    if (!hf_init() || !slot->view)
        return 0;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s_hf_pool[s_fi];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_hf_dsl;
    if (vkAllocateDescriptorSets(s_dev, &ai, &set) != VK_SUCCESS)
        return 0;
    memset(&ii, 0, sizeof ii);
    ii.sampler = s_hf_smp;
    ii.imageView = rt->im.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    nv2a_host_hdr_levels(&pk, &pw, &hw);
    nv2a_host_hdr_tone(&sh, &hl, &srgb);
    nv2a_host_hdr_shape(&sf, &bl);
    pc[0] = pk / pw;
    pc[1] = pw / 10000.0f;
    pc[2] = hl;
    pc[3] = srgb ? 1.0f : 0.0f;
    pc[4] = sh;
    pc[5] = sf;
    pc[6] = bl / 10000.0f;
    pc[7] = 0.0f;

    end_encoder();
    ensure_cmd();
    nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /* GENERAL to GENERAL: after the last present that read it. */
    nvk_image_barrier(s_cmd, slot->img, VK_IMAGE_ASPECT_COLOR_BIT,
                      slot->layout == VK_IMAGE_LAYOUT_UNDEFINED
                      ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    slot->layout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = slot->view;
    att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = hcw;
    ri.renderArea.extent.height = hch;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(s_cmd, &ri);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)hcw;
    vp.height = (float)hch;
    vp.maxDepth = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.extent.width = hcw;
    sc.extent.height = hch;
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    vkCmdSetScissor(s_cmd, 0, 1, &sc);
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_hf_pso);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_hf_pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(s_cmd, s_hf_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    vkCmdEndRendering(s_cmd);
    /* For the presenter's read on the queue after this submit. */
    nvk_image_barrier(s_cmd, slot->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    s_dyn_valid = 0;
    s_tset_last = VK_NULL_HANDLE;
    s_hdr_maps++;
    return 1;
}

/* At a draw: its colour bound (x) and the factor on its colour (y), in the
 * title's encoding: scene draws keep light past 1.0; UI draws (the HUD,
 * menus, everything outside a view's 3D) are bound to 1.0, and those into
 * the target the last flip showed are scaled to the HUD's white over paper
 * white (a UI draw into another target is drawn into the frame later). */
static RenderTarget *s_hdr_prt[2];  /* the targets the last two flips showed (double buffering) */
static uint64_t      s_hdr_gain_draws;

/* RECOMP_DEBUG=hdr_gains=0: the title's per-texture gains are not applied;
 * =only:<hash>[+<hash>...] (BO-195): only those entries are. */
static uint64_t s_hg_only[16];
static int      s_hg_only_n;
static int hdr_gains_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_HDR_GAINS);
        on = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
        if (on && e && !strncmp(e, "only:", 5)) {
            e += 5;
            while (*e && s_hg_only_n < 16) {
                char *end;
                uint64_t v = strtoull(e, &end, 16);
                if (end == e)
                    break;
                s_hg_only[s_hg_only_n++] = v;
                e = *end ? end + 1 : end;
            }
            fprintf(stderr, LOGP "HDR gains: only %d of the title's entries\n", s_hg_only_n);
        }
    }
    return on;
}

/* BO-195, measuring one light class: RECOMP_DEBUG=hdr_gains_ab=n makes every
 * other block of n flips differ, so neighbouring flips of one run are about
 * the same moment with and without: with hdr_gains_probe those flips mark
 * one probe texture (its scene draws x4 in linear light: the one drawn in
 * the flip before with the fewest dumped marks so far), without it the gains are
 * off in them. s_hg_ab is the state of the frame being drawn (set at the
 * flip that starts it), s_hg_probe the probe it marks (-1 none); hdr_dump
 * writes both. Without hdr_gains_ab every probe is marked on every flip. */
static uint32_t s_hg_abn;
static int      s_hg_ab, s_hg_probe = -1;
static uint8_t  s_hg_seen[64];
static uint32_t s_hg_marks[64];

static float hdr_lodfade_span(void)
{
    static float span = -1.0f;
    if (span < 0.0f) {
        const char *e = recomp_env(RENV_HDR_LODFADE);
        span = e && *e ? (float)atof(e) : 2.0f;
        if (span < 0.0f || span > 8.0f)
            span = 2.0f;
    }
    return span;
}

static float hdr_gain_for(const struct nv2a_texrep_hdr_gain *e, int *mode)
{
    int p = nv2a_texrep_hdr_probes() ? nv2a_texrep_hdr_probe_index(e->hash) : -1, i;
    *mode = e->mode;
    if (p >= 0 && p < 64) {
        if (!s_hg_abn || (s_hg_ab && p == s_hg_probe)) {
            *mode = NV2A_HDR_GAIN_MUL;
            return 4.0f;
        }
        if (!s_hg_ab)
            s_hg_seen[p] = 1;
    }
    if (!hdr_gains_on() || (s_hg_abn && s_hg_ab && !nv2a_texrep_hdr_probes()))
        return 1.0f;
    if (s_hg_only_n) {
        for (i = 0; i < s_hg_only_n && s_hg_only[i] != e->hash; i++)
            ;
        if (i == s_hg_only_n)
            return 1.0f;
    }
    return e->gain > 0.0f ? e->gain : 1.0f;
}

/* At a flip: the state of the frame that starts (its draws see flips). */
static void hdr_gains_flip(uint32_t flip)
{
    static int read;
    int n = nv2a_texrep_hdr_probes(), i;
    if (!read) {
        const char *a = recomp_env(RENV_HDR_GAINS_AB);
        read = 1;
        s_hg_abn = a ? (uint32_t)strtoul(a, NULL, 0) : 0;
    }
    if (!s_hg_abn)
        return;
    s_hg_ab = (int)((flip / s_hg_abn) & 1u);
    s_hg_probe = -1;
    if (s_hg_ab && n) {
        for (i = 0; i < n && i < 64; i++)
            if (s_hg_seen[i] && (s_hg_probe < 0 || s_hg_marks[i] < s_hg_marks[s_hg_probe]))
                s_hg_probe = i;
        /* counted only on a frame hdr_dump writes (flip + 1 when shown),
         * so the dumped flips go round the probes in view */
        if (s_hg_probe >= 0 && nv2a_pb_dump_at_listed(flip + 1u))
            s_hg_marks[s_hg_probe]++;
    }
    if (!s_hg_ab)
        memset(s_hg_seen, 0, sizeof s_hg_seen);
}

/* RECOMP_DEBUG=hdr_texlog (as Metal): what the title draws with which blend,
 * by texrep_dump's hash of stage 0's texture, once per combination (id n);
 * every 1800 flips (BO-195) an [HDRTEXN] line per combination with its draws
 * and the number of flips it was drawn in so far. */
static int s_hdr_watch_hit;   /* hdr_watch: the watched texture drawn this frame (as Metal) */
static void hdr_texlog(const struct nv2a_pb_gpu *g)
{
    static int on = -1, nseen;
    static uint64_t watch, watch_from;
    static uint64_t seen[4096], last_sum;
    static uint32_t ndraw[4096], nflip[4096], lflip[4096];
    static FILE *out;
    const Stage *t = &s_stage[0];
    const uint8_t *mem;
    uint64_t h, k;
    int i;
    if (on < 0) {
        const char *e = recomp_env(RENV_HDR_TEXLOG);
        on = e != NULL;
        /* =<file>: appended there (a pre-build scene's log is deleted) */
        out = e && strchr(e, '/') ? fopen(e, "a") : NULL;
        if (!out)
            out = stderr;
        e = recomp_env(RENV_HDR_WATCH);
        watch = e ? strtoull(e, NULL, 16) : 0;
        /* hdr_watch=<hash>@<flip>: from that flip on */
        watch_from = e && strchr(e, '@') ? strtoull(strchr(e, '@') + 1, NULL, 10) : 0;
    }
    if ((!on && !watch) || !t->offset || !t->width || !t->valid)
        return;
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    h = nv2a_texrep_hash(mem + t->offset, t->color, t->width, t->height, t->pitch);
    if (watch && h == watch && s_presents >= watch_from && g->blend_enable
            && (g->blend_dfactor == 0x1 || nv2a_pb_vsh_state()->atest_enable)
            && nv2a_pb_stream_tag() == HDR_TAG_SCENE)
        s_hdr_watch_hit = 1;
    if (!on || nseen >= 4096)
        return;
    k = h ^ ((uint64_t)(g->blend_enable ? g->blend_sfactor : 0xFFFF) << 32)
          ^ ((uint64_t)(g->blend_enable ? g->blend_dfactor : 0xFFFF) << 16)
          ^ nv2a_pb_stream_tag();
    if (s_presents / 1800u != last_sum / 1800u) {
        last_sum = s_presents;
        for (i = 0; i < nseen; i++)
            fprintf(out, "[HDRTEXN] id %d draws %u flips %u at %llu\n", i, ndraw[i],
                    nflip[i], (unsigned long long)s_presents);
        if (out != stderr)
            fflush(out);
    }
    for (i = 0; i < nseen; i++)
        if (seen[i] == k)
            break;
    if (i < nseen) {
        ndraw[i]++;
        if (lflip[i] != (uint32_t)s_presents + 1u) {
            lflip[i] = (uint32_t)s_presents + 1u;
            nflip[i]++;
        }
        return;
    }
    ndraw[nseen] = nflip[nseen] = 1;
    lflip[nseen] = (uint32_t)s_presents + 1u;
    {
        const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
        fprintf(out, "[HDRTEX] tex %016llx fmt 0x%02x %ux%u blend %d sf 0x%x df 0x%x tag %u flip %llu"
                " id %d atest %d func 0x%x ref %u\n", (unsigned long long)h, t->color, t->width,
                t->height, g->blend_enable, g->blend_sfactor, g->blend_dfactor,
                nv2a_pb_stream_tag(), (unsigned long long)s_presents, nseen,
                v->atest_enable, v->atest_func, v->atest_ref);
        /* BO-203: the other stages and the combiner words of the same draw. */
        for (i = 1; i < 4; i++) {
            const Stage *s = &s_stage[i];
            if (!s->mode || !s->offset || !s->width)
                continue;
            fprintf(out, "[HDRTEX]   id %d stage %d mode %d tex %016llx fmt 0x%02x %ux%u off 0x%08x"
                    " pitch %u levels %u\n", nseen, i, s->mode,
                    (unsigned long long)nv2a_texrep_hash(mem + s->offset, s->color, s->width,
                                                         s->height, s->pitch),
                    s->color, s->width, s->height, s->offset, s->pitch, s->levels);
        }
        fprintf(out, "[HDRTEX]   id %d stage 0 mode %d off 0x%08x pitch %u rc_ctl 0x%08x fcw0 0x%08x"
                " fcw1 0x%08x aicw %08x %08x aocw %08x %08x cicw %08x %08x cocw %08x %08x"
                " mask 0x%08x\n", nseen, t->mode, t->offset, t->pitch, v->rc_ctl, v->rc_fcw0,
                v->rc_fcw1, v->rc_aicw[0], v->rc_aicw[1], v->rc_aocw[0], v->rc_aocw[1],
                v->rc_cicw[0], v->rc_cicw[1], v->rc_cocw[0], v->rc_cocw[1], g->color_keep);
    }
    seen[nseen++] = k;
    if (out != stderr)
        fflush(out);
}
static void hdr_draw(const RenderTarget *rt, float hdr[4])
{
    int scene;
    hdr[0] = hdr[1] = hdr[2] = 1.0f;
    hdr[3] = 0.0f;
    if (!s_hdr)
        return;
    scene = nv2a_pb_stream_tag() == HDR_TAG_SCENE;
    s_hdr_draws[scene]++;
    if (scene) {
        hdr[0] = s_hdr_hi;
    } else if (rt && (rt == s_hdr_prt[0] || rt == s_hdr_prt[1])) {
        static float last_hw = -1.0f, last_pw, k = 1.0f;
        float pk, pw, hw;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        if (hw != last_hw || pw != last_pw) {
            last_hw = hw;
            last_pw = pw;
            k = powf(hw / pw, 1.0f / 2.2f);
        }
        hdr[1] = k;
    }
}

/* nv2a_hud_squeeze: each element of a draw by the title's HUD program
 * narrowed by k about its own middle, in the program's x input (a fraction
 * of the target's width; the program maps it to the screen linearly, so
 * the middle stays where it was). An element: the triangles that share
 * corners (a glyph's quad, a picture's), then the small pieces near enough
 * to read as one (a word, a marker's tag and bar) -- each draw a few dozen
 * vertices. A piece over 30% of the width (a caption's box) is an element
 * of its own, never joined with what it overlaps; one nearly the target's
 * width (a bar, a fade) is left alone.
 * The vertices are gathered here first, as the upload ring is write-combined
 * and slow to read back. Returns 0 for any other draw (gathered as usual). */
#define HUD_MAXV 1024
static int draws_trace_on(void);
static int hud_squeeze(const struct nv2a_pb_draw *d, const int *slot, int nf4, uint32_t lo,
                       uint32_t count, const float (*inl)[4], const uint32_t *idx, uint32_t nidx,
                       int topo, float *vf)
{
    static float buf[HUD_MAXV * 16 * 4];
    static int par[HUD_MAXV];
    static float bx0[HUD_MAXV], bx1[HUD_MAXV], by0[HUD_MAXV], by1[HUD_MAXV];
    static unsigned char big[HUD_MAXV];
    const VsEntry *e;
    uint32_t hash, i, j;
    float k;
    int s0 = -1, merged;

    if (!d->program || !nv2a_hud_squeeze_get(&hash, &k))
        return 0;
    e = vs_cache();                     /* the draw's, memoised */
    if (!e || e->hash != hash)
        return 0;
    for (i = 0; i < (uint32_t)nf4; i++)
        if (slot[i] == 0)
            s0 = (int)i;
    if (s0 < 0 || count > HUD_MAXV || nf4 > 16)
        return 0;
    nv2a_vtx_gather(d, slot, nf4, lo, count, inl, (const uint8_t *)xbox_GetMemoryOffset(), buf);
#define X(v) buf[((v) * (uint32_t)nf4 + (uint32_t)s0) * 4]
#define Y(v) buf[((v) * (uint32_t)nf4 + (uint32_t)s0) * 4 + 1]
    for (i = 0; i < count; i++)
        par[i] = (int)i;
    {
        /* Union-find over the corners the triangles share. */
#define ROOT(x) ({ int r_ = (x); while (par[r_] != r_) r_ = par[r_] = par[par[r_]]; r_; })
        if (topo == NV2A_TOPO_TRIANGLES)
            for (i = 0; i + 2 < nidx; i += 3) {
                int a = ROOT((int)idx[i]), b = ROOT((int)idx[i + 1]), c = ROOT((int)idx[i + 2]);
                par[b] = a;
                par[c] = a;
            }
        for (i = 0; i < count; i++) {
            bx0[i] = by0[i] = 1e30f;
            bx1[i] = by1[i] = -1e30f;
        }
        for (i = 0; i < count; i++) {
            int r = ROOT((int)i);
            if (X(i) < bx0[r]) bx0[r] = X(i);
            if (X(i) > bx1[r]) bx1[r] = X(i);
            if (Y(i) < by0[r]) by0[r] = Y(i);
            if (Y(i) > by1[r]) by1[r] = Y(i);
        }
        /* Small pieces a little apart (a glyph's spacing, a tag over its
         * bar) are one element: 2% of the width across, 1% down. */
        for (i = 0; i < count; i++)
            big[i] = par[i] == (int)i && bx1[i] - bx0[i] > 0.3f;
        do {
            merged = 0;
            for (i = 0; i < count; i++) {
                if (par[i] != (int)i || big[i])
                    continue;
                for (j = i + 1; j < count; j++) {
                    if (par[j] != (int)j || big[j])
                        continue;
                    if (bx0[j] > bx1[i] + 0.02f || bx0[i] > bx1[j] + 0.02f
                            || by0[j] > by1[i] + 0.01f || by0[i] > by1[j] + 0.01f)
                        continue;
                    par[j] = (int)i;
                    if (bx0[j] < bx0[i]) bx0[i] = bx0[j];
                    if (bx1[j] > bx1[i]) bx1[i] = bx1[j];
                    if (by0[j] < by0[i]) by0[i] = by0[j];
                    if (by1[j] > by1[i]) by1[i] = by1[j];
                    merged = 1;
                }
            }
        } while (merged);
        if (draws_trace_on())
            for (i = 0; i < count; i++)
                if (par[i] == (int)i)
                    fprintf(stderr, "[VKDRAW]   hud element x %.3f..%.3f y %.3f..%.3f\n",
                            bx0[i], bx1[i], by0[i], by1[i]);
        for (i = 0; i < count; i++) {
            int r = ROOT((int)i);
            float m = (bx0[r] + bx1[r]) * 0.5f;
            if (bx1[r] - bx0[r] < 0.9f)
                X(i) = m + (X(i) - m) * k;
        }
#undef ROOT
    }
#undef X
#undef Y
    memcpy(vf, buf, (size_t)count * (size_t)nf4 * 16u);
    return 1;
}

/* RECOMP_TRACE=vk_draws=<flip>[,<flip>...]: every draw of those flips, to
 * stderr. A pass-by-pass account of a frame is what an effect chain (blur
 * pyramids, depth of field, glow) is debugged from: which target each draw
 * lands in, what it samples, and the combiner program that mixes it. */
static int draws_trace_on(void)
{
    static int n = -1;
    static uint64_t at[16];
    int i;

    if (n < 0) {
        const char *e = recomp_env(RENV_VK_DRAWS);
        n = 0;
        while (e && *e && n < 16) {
            char *x;
            at[n++] = strtoull(e, &x, 0);
            if (x == e)
                break;
            e = *x == ',' ? x + 1 : x;
        }
    }
    for (i = 0; i < n; i++)
        if (s_presents == at[i])
            return 1;
    return 0;
}

static void draws_trace(const struct nv2a_pb_draw *d, const RenderTarget *rt)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t vlen = 0, vh = d->program ? nv2a_vsh_program_hash(v, &vlen) : 0;
    int k, nst = (int)(v->rc_ctl & 0xFu);

    fprintf(stderr, "[VKDRAW] flip %llu rt %08X %ux%u fmt %X zeta %08X z %d/%d blend %u %X %X eq %X"
            " keep %08X prog %d vs %08X shader_ctl %08X rc %d ctl %08X fc %08X %08X fcc %08X %08X bc %08X\n",
            (unsigned long long)s_presents, rt->offset, rt->w, rt->h, g->format, v->zeta_offset,
            v->depth_enable, v->depth_mask, g->blend_enable, g->blend_sfactor, g->blend_dfactor,
            g->blend_equation, g->color_keep, d->program, vh, v->shader_ctl, nst, v->rc_ctl,
            v->rc_fcw0, v->rc_fcw1, v->rc_sf[0], v->rc_sf[1], g->blend_color);
    for (k = 0; k < 4; k++)
        if (s_stage[k].raw_mode)
            fprintf(stderr, "[VKDRAW]   t%d mode %d/%d %08X fmt %02X %ux%u pitch %u levels %u"
                    " (lod %u..%u of %u) filter %08X addr %u/%u ctl0 %08X\n", k, s_stage[k].mode,
                    s_stage[k].raw_mode, s_stage[k].offset, s_stage[k].color, s_stage[k].width,
                    s_stage[k].height, s_stage[k].pitch, s_stage[k].levels, s_stage[k].lod_min,
                    s_stage[k].lod_max, s_stage[k].fmt_levels, s_stage[k].filter,
                    s_stage[k].addr_u, s_stage[k].addr_v, nv2a_pb_tex_regs()[(0x40u * k + 0x0Cu) / 4]);
    fprintf(stderr, "[VKDRAW]   alpha test %d func %X ref %u, alpha kill %X; fog %d mode %X gen %X"
            " params %g %g %g colour %08X; stencil %d func %u ref %u masks %02X/%02X ops %X %X %X;"
            " depth func %X clip %g..%g cull %d face %X\n", v->atest_enable,
            v->atest_func, v->atest_ref, nv2a_alpha_kill_mask(s_stage, g->flips),
            v->fog_enable, v->fog_mode, v->fog_gen, v->fog_param[0], v->fog_param[1],
            v->fog_param[2], v->fog_color, v->stencil_enable, v->stencil_func, v->stencil_ref,
            v->stencil_rmask, v->stencil_wmask, v->stencil_op[0], v->stencil_op[1],
            v->stencil_op[2], v->depth_func, v->clip_min, v->clip_max, v->cull_enable,
            v->cull_face);
    fprintf(stderr, "[VKDRAW]   prim %u, %u indices%s; c0 %g %g %g %g c1 %g %g %g %g;"
            " c58 %g %g %g %g c59 %g %g %g %g\n", d->prim, d->idx_count,
            d->inline_data ? ", inline" : "", v->c[0][0], v->c[0][1], v->c[0][2], v->c[0][3],
            v->c[1][0], v->c[1][1], v->c[1][2], v->c[1][3], v->c[58][0], v->c[58][1],
            v->c[58][2], v->c[58][3], v->c[59][0], v->c[59][1], v->c[59][2], v->c[59][3]);
    for (k = 0; k < NV_VERTEX_ATTRS; k++) {
        const VertexAttr *a = &d->attr[k];
        float f[4] = { 0, 0, 0, 1 };
        uint32_t first = d->idx_count ? d->idx[0] : 0;
        const uint8_t *p = NULL;
        if (!a->size || !a->stride)
            continue;
        if (d->inline_data) {
            if ((size_t)a->offset + (size_t)first * a->stride + 16 <= d->inline_bytes)
                p = (const uint8_t *)d->inline_buf + a->offset + (size_t)first * a->stride;
        } else if (a->offset) {
            p = (const uint8_t *)xbox_GetMemoryOffset() + a->offset + (size_t)first * a->stride;
        }
        if (p)
            nv2a_vtx_decode(a->type, a->size, p, f);
        fprintf(stderr, "[VKDRAW]   v%d type %u size %u stride %u at %08X: %g %g %g %g", k,
                a->type, a->size, a->stride, a->offset, f[0], f[1], f[2], f[3]);
        if (k == 0 && d->idx_count <= 8) {   /* a quad's other corners */
            uint32_t j;
            for (j = 1; j < d->idx_count && p; j++) {
                const uint8_t *q = p + ((size_t)d->idx[j] - first) * a->stride;
                nv2a_vtx_decode(a->type, a->size, q, f);
                fprintf(stderr, " | %g %g %g %g", f[0], f[1], f[2], f[3]);
            }
        }
        fprintf(stderr, "\n");
    }
    for (k = 0; k < nst && k < 8; k++)
        fprintf(stderr, "[VKDRAW]   rc%d ci %08X co %08X ai %08X ao %08X c0 %08X c1 %08X\n", k,
                v->rc_cicw[k], v->rc_cocw[k], v->rc_aicw[k], v->rc_aocw[k], v->rc_f0[k], v->rc_f1[k]);
}

/* A draw not drawn: counted, and named in a vk_draws trace. */
/* The skips by reason, for the present report: the reasons are the literals
 * below, so a pointer names one. */
static const char *s_skip_why[16];
static uint64_t    s_skip_n[16];
static void skip_note(const char *why)
{
    int i;
    for (i = 0; i < 16 && s_skip_why[i] && s_skip_why[i] != why; i++)
        ;
    if (i < 16) {
        s_skip_why[i] = why;
        s_skip_n[i]++;
    }
}

#define SKIP_DRAW(why) do { \
        s_draws_skipped++; \
        skip_note(why); \
        if (draws_trace_on()) \
            fprintf(stderr, "[VKDRAW]   skipped: %s\n", why); \
    } while (0)

static void vk_on_draw(const struct nv2a_pb_draw *d)
{
    static uint32_t *idx;
    static uint32_t idx_cap;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    enum nv2a_topology topo;
    RenderTarget *rt;
    VkPipeline pso;
    VkShaderModule vmod, fmod;
    struct nv2a_vp_consts vc;
    VkPsConsts pc;
    VkDeviceSize vb_off = 0, ib_off = 0, uo[3] = {0, 0, 0}, vbytes;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k, nf4 = 0;
    uint32_t dyn_off[3];
    int slot[NV2A_VS_MAX_INPUTS];
    float *vf;
    void *ip;
    int cull = 0, cull_all = 0, tries;
    float zbias = 0.0f, zb_applied = 0.0f;
    int zb_split = 0;                 /* BO-253: offset on the front faces only */
    VkDeviceSize uo_plain = 0;        /* the vertex constants without the offset */
    uint64_t rhash = 0;               /* BO-204 debug */
    const Stage *rstage = NULL;
    const struct nv2a_texrep_hdr_gain *hgain = NULL;
    int solo = 0;                     /* BO-203: stage 0 is a stage0_only HDR picture */
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    VkImageView sview[4];
    VkSampler   ssmp[4];
    VkDescriptorSet tset;

    if (!init())
        return;
    if (g->color_keep == 0xFFFFFFFFu && !(v->depth_enable && v->depth_mask) && !use_s
            && !s_occ_q)
        return;
    if (!d->idx_count) {
        SKIP_DRAW("no indices");
        return;
    }
    if (d->program) {
        VsEntry *e = vs_cache();
        s_draws_prog++;
        if (!e) {
            SKIP_DRAW("vertex program not built");
            return;
        }
        vmod = e->mod;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
            if (e->inputs & (1u << i))
                slot[nf4++] = (int)i;
    } else {
        vmod = s_vs_pass;
        for (k = 0; k < VTX_F4; k++)
            slot[nf4++] = s_vtx_attr[k];
    }
    rt = surface();
    if (!rt)
        rt = surface_depth_only();
    if (!rt) {
        SKIP_DRAW("no render target");
        return;
    }
    build_stages();
    nv2a_ps_consts_fill(v, s_stage, &pc.b);
    float host_bias = nv2a_host_lod_bias();   /* RECOMP_GFX_LOD_BIAS; 0 by default */
    /* BO-171: a cut-out draw (alpha test) takes no negative host bias: a
     * finer level than the pixel's own makes its edge shimmer, and the
     * levels nv2a_texmip makes keep the leaves without it. */
    if (host_bias < 0.0f && (nv2a_texmip_mode() & NV2A_TEXMIP_BIAS)
            && nv2a_texmip_cut_ref(v->atest_enable, v->atest_func, v->atest_ref))
        host_bias = 0.0f;
    for (k = 0; k < 4; k++) {
        const Stage *t = &s_stage[k];
        float b = t->levels > 1 && nv2a_tex_filter_mip(t->filter)
                ? nv2a_tex_lod_bias(t->filter) + host_bias : 0.0f;
        pc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
    }
    {   /* Bump-env registers: methods 0x1B28..0x1B3C of each stage block. */
        const uint32_t *tr = nv2a_pb_tex_regs();
        for (k = 0; k < 4; k++) {
            const uint32_t *r = &tr[(0x40u * k + 0x28u) / 4];
            memcpy(pc.bump_mat[k], r, 16);
            memcpy(&pc.bump_lum[k][0], &r[4], 4);
            memcpy(&pc.bump_lum[k][1], &r[5], 4);
            pc.bump_lum[k][2] = pc.bump_lum[k][3] = 0.0f;
        }
    }
    /* BO-110: the colour bound of the HDR combiners (unread by SDR ones). */
    hdr_draw(rt, pc.hdr);
    s_fs_show_now = fs_show_for(rt);
    fmod = fs_select(v);
    s_signed_on = fmod != s_fs_basic;   /* fs_basic has no signed channels */
    nv2a_idx_range(d->idx, d->idx_count, &lo, &hi);
    count = hi - lo + 1;
    if (d->idx_count * 3u > idx_cap) {
        uint32_t *grown = (uint32_t *)realloc(idx, (size_t)d->idx_count * 12u);
        if (!grown) {
            SKIP_DRAW("out of memory");
            return;
        }
        idx = grown;
        idx_cap = d->idx_count * 3u;
    }
    nidx = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, lo, idx, &topo);
    if (!nidx) {
        /* Too few vertices for one primitive draws nothing on the console
         * either; an unknown primitive type would be ours to add. */
        if (topo == NV2A_TOPO_NONE)
            SKIP_DRAW("unknown primitive type");
        else
            SKIP_DRAW("too few vertices for a primitive");
        return;
    }
    if (v->cull_enable) {
        if (v->cull_face == 0x405)      cull = 1;   /* BACK */
        else if (v->cull_face == 0x404) cull = 2;   /* FRONT */
        else if (v->cull_face == 0x408) cull_all = 1;
    }
    if (cull_all && topo == NV2A_TOPO_TRIANGLES) {
        s_draws_culled++;
        return;
    }
    nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, &vc);
    /* Without a depth buffer no depth is stored, and what keeps or drops a
     * fragment is SET_CLIP_MIN/MAX alone, in depth units. The depth format
     * in SET_SURFACE_FORMAT follows the colour's size, not a buffer: XDK
     * D3D's pass-through program scales screen z by 16777215 whatever the
     * format says, and on a 16-bit target (Double-S.T.E.A.L 2's 720p front
     * buffers, X1R5G5B5 with Z16) z over the Z16 maximum went past the far
     * plane and its full-screen composite was clipped away. Mapped by the
     * clip range where that is wider, as the console keeps such a fragment.
     * Draws with a depth buffer keep the format's mapping (what they store
     * and what reads it back as a texture). */
    if (!(use_z && v->zeta_offset) && v->clip_max > nv2a_zmax(g->format)) {
        vc.c[2] = 1.0f / v->clip_max;
        vc.c[3] = v->poly_offset ? v->poly_bias / v->clip_max : 0.0f;
    }
    if (draws_trace_on())
        draws_trace(d, rt);

    /* Depth: the zeta surface's buffer, attached to this target's passes
     * from now on (a new pass if the open one lacks it). */
    if (use_z) {
        DepthTarget *dt = depth_target(rt->w, rt->h);
        if (dt && rt->dt != dt) {
            rt->dt = dt;
            if (s_enc_rt == rt)
                end_encoder();
        }
    }
    /* Textures before the pass: a self copy or a layout change is outside
     * one. */
    {
        int textured = 0;
        s_draw_2d = 0;
        s_tu_draw_3d = 0;
        if (nv2a_texup_algo()) {
            /* BO-245: the world's draws (a vertex program, the depth test). */
            s_tu_draw_3d = d->program && v->depth_enable;
            s_tu_3d += (unsigned)s_tu_draw_3d;
        }
        if (nv2a_texscale_factor() || ui_scale_log() || nv2a_texup_algo()) {
            const struct nv2a_pb_present *pr = nv2a_pb_present_state();
            struct nv2a_texscale_draw td2 = { d->program, v->depth_enable,
                                              v->depth_enable && v->depth_mask,
                                              rt->w, rt->h, pr->w, pr->h };
            s_draw_2d = nv2a_texscale_draw_is_2d(&td2);
        }
        for (k = 0; k < 4; k++) {
            const Stage *t = &s_stage[k];
            uint32_t sw, sh;
            sview[k] = s_black.view;
            ssmp[k] = sampler(t, 0, 0);
            if (!t->mode || t->mode == 4)
                continue;
            textured = 1;
            if (t->mode == 3 && t->cube && fmod != s_fs_basic) {   /* samplerCube */
                sview[k] = cube_bind(t);
            } else if (fmod != s_fs_basic && dt_for_texture(t) != NULL) {
                DepthTarget *sd = dt_for_texture(t);   /* a shadow buffer we drew */
                rt_to_sampled(&sd->im);
                sview[k] = sd->sview;
            } else if (fmod != s_fs_basic && t->raw_mode == 1 && dt_for_zcolor(t) != NULL) {
                DepthTarget *sd = dt_for_zcolor(t);    /* depth read as bytes */
                rt_to_sampled(&sd->im);
                sview[k] = sd->sview;
                pc.b.tex_scale[k][0] = 1.0f / (float)sd->w;   /* texel coordinates */
                pc.b.tex_scale[k][1] = 1.0f / (float)sd->h;
                s_zcolor_binds++;
            } else if ((t->mode == 3 || !stage_signed(t))
                       && rt_texture(t, rt, &sview[k], &sw, &sh) != NULL) {
                if (!nv2a_tex_size_from_format(t->color)) {
                    pc.b.tex_scale[k][0] = 1.0f / (float)sw;
                    pc.b.tex_scale[k][1] = 1.0f / (float)sh;
                }
                s_rtt_binds++;
            } else {
                sview[k] = tex_bind(t);
                if (s_bind_scale || s_bind_tu)
                    ssmp[k] = sampler(t, s_bind_scale, s_bind_tu);
                if (zbias == 0.0f)
                    zbias = s_bind_zbias;
                if (!rhash) {
                    rhash = s_bind_rhash;
                    rstage = t;
                }
                if (!hgain)
                    hgain = s_bind_hgain;
                if (k == 0)
                    solo = s_bind_solo;
            }
        }
        if (textured)
            s_draws_tex++;
        /* BO-203: an HDR picture that stands alone (the sun's disc): the
         * other stages read transparent black, whatever the title bound. */
        if (solo && s_clear.view && nv2a_texrep_hdr())
            for (k = 1; k < 4; k++) {
                const Stage *t = &s_stage[k];
                if (t->mode && t->mode != 4 && !(t->mode == 3 && t->cube))
                    sview[k] = s_clear.view;
            }
    }
    /* BO-110: a registered light texture's gain, on its scene draws. */
    if (hgain && s_hdr && pc.hdr[0] > 1.0f) {
        int mode;
        float gain = hdr_gain_for(hgain, &mode);
        if (gain != 1.0f) {
            float k = powf(gain, 1.0f / 2.2f);
            if (mode == NV2A_HDR_GAIN_HI)
                pc.hdr[2] = k;
            else
                pc.hdr[1] = k;
            /* BO-195 P3: a lift fades with the texture's mip (a cut never does; not for
             * measuring probes). The fade queries stage 0's LOD whichever stage supplied the
             * gain: fine while every entry is a stage-0 sprite, a trap for P1 entries. */
            if (gain > 1.0f && !nv2a_texrep_hdr_probes())
                pc.hdr[3] = hdr_lodfade_span();
            s_hdr_gain_draws++;
        }
    }
    if (s_hdr)
        hdr_texlog(g);
    /* BO-176: a registered decal's depth offset, added to the constant part
     * of the title's own polygon offset (vp_scale.w), as Metal. */
    struct nv2a_vp_consts vc_plain;
    memset(&vc_plain, 0, sizeof vc_plain);
    if (zbias != 0.0f && use_z && nv2a_texrep_zbias_on(g->flips)) {
        zb_applied = zbias / nv2a_zmax(g->format);
        /* BO-253: with culling off, the offset only on the face the title
         * treats as front: this draw with the back faces culled, a second
         * one without the offset with the front faces culled (as Metal). */
        zb_split = cull == 0 && nv2a_texrep_zbias_facing(g->flips);
        if (zb_split)
            vc_plain = vc;
        vc.c[3] += zb_applied;
    }
    /* BO-204 debug: zbias_probe pushes one texture's draws back a flip. */
    if (rhash && v->depth_enable)
        vc.c[3] += nv2a_texrep_zprobe(g->flips, rhash, rstage->color, rstage->width,
                                      rstage->height) / nv2a_zmax(g->format);

    /* Rings: an overflow flushes (ending the pass and resetting every
     * ring), so on a flush the placement starts over. */
    vbytes = (VkDeviceSize)count * nf4 * 16u;
    if (vbytes < 16)
        vbytes = 16;
    for (tries = 0; tries < 2; tries++) {
        uint64_t fl = s_flushes;
        vf = (float *)ring_alloc(RING_VB, vbytes, 16, &vb_off);
        ip = vf ? ring_alloc(RING_IB, (VkDeviceSize)nidx * 4u, 16, &ib_off) : NULL;
        if (vf && ip && d->program && !ubo_place(0, v->c, VS_C_BYTES, &uo[0]))
            ip = NULL;
        if (vf && ip && !ubo_place(1, &vc, VP_C_BYTES, &uo[1]))
            ip = NULL;
        if (vf && ip && !ubo_place(2, &pc, PS_C_BYTES, &uo[2]))
            ip = NULL;
        if (vf && ip && zb_split && !ubo_place(1, &vc_plain, VP_C_BYTES, &uo_plain))
            ip = NULL;
        if (fl == s_flushes)
            break;
    }
    if (!vf || !ip) {
        SKIP_DRAW("no ring space");
        return;
    }
    if (!d->program)
        uo[0] = s_umemo.valid[0] ? s_umemo.off[0] : 0;
    if (!hud_squeeze(d, slot, nf4, lo, count, v->inl, idx, nidx, topo, vf))
        nv2a_vtx_gather(d, slot, nf4, lo, count, v->inl,
                        (const uint8_t *)xbox_GetMemoryOffset(), vf);
    memcpy(ip, idx, (size_t)nidx * 4u);
    tset = texture_set(sview, ssmp);
    if (!tset) {
        SKIP_DRAW("no texture set");
        return;
    }

    if (!s_pass || s_enc_rt != rt)
        begin_encoder(rt, 0, NULL);
    pso = pipeline(vmod, fmod, nf4, topo);
    if (!pso) {
        SKIP_DRAW("no pipeline");
        return;
    }
    {
        /* The draw's dynamic state, and what of it differs from what this
         * command buffer already has (every pipeline here leaves the same
         * states dynamic, so they outlive a bind and a pass): a title draws
         * hundreds of batches in a row with the same depth, stencil, cull
         * and viewport state, and each vkCmdSet* is a call into the driver. */
        DynState n;
        int z = s_enc_dt && use_z && v->depth_enable;
        int st = s_enc_dt && use_s;
        VkRect2D sr;
        if (!draw_scissor(g, rt, &sr)) {
            SKIP_DRAW("empty scissor");
            return;
        }
        memset(&n, 0, sizeof n);
        n.pso = pso;
        /* Depth and stencil only with an attachment and when enabled; the
         * compare and op codes are in VkCompareOp / VkStencilOp order. */
        n.ztest = z ? VK_TRUE : VK_FALSE;
        n.zwrite = z && v->depth_mask ? VK_TRUE : VK_FALSE;
        n.zcmp = z ? (VkCompareOp)nv2a_cmp_from_gl(v->depth_func) : VK_COMPARE_OP_ALWAYS;
        n.stest = st ? VK_TRUE : VK_FALSE;
        n.sop[0] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[0]);
        n.sop[1] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[2]);
        n.sop[2] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[1]);
        n.scmp = (VkCompareOp)(v->stencil_func & 7u);
        n.srmask = v->stencil_rmask & 0xFFu;
        n.swmask = v->stencil_wmask & 0xFFu;
        n.sref = st ? (v->stencil_ref & 0xFFu) : 0u;
        /* BO-10 polygon offset, as Metal: the constant part in the vertex
         * stage (vp_scale.w); the slope part per host pixel, so times
         * render.scale (xemu PR #2240). */
        n.slope = (s_enc_dt && use_z && v->poly_offset)
                ? v->poly_factor * (float)nv2a_host_render_scale_x2() * 0.5f : 0.0f;
        /* A negative height: D3D's y-up clip space onto rows from the top,
         * as Metal's viewport. */
        n.vw = rt->hw;
        n.vh = rt->hh;
        n.sr = sr;
        n.front = v->front_face != 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                         : VK_FRONT_FACE_CLOCKWISE;
        n.cull = zb_split ? VK_CULL_MODE_BACK_BIT
               : cull == 1 ? VK_CULL_MODE_BACK_BIT
               : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                  g->blend_dfactor, g->blend_color, n.bf, &(int){0});
        if (s_enc_dt && use_z)
            s_draws_z++;
#define DYN(cond) (!s_dyn_valid || (cond))
        if (DYN(n.pso != s_dyn.pso))
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        if (DYN(n.ztest != s_dyn.ztest))
            vkCmdSetDepthTestEnable(s_cmd, n.ztest);
        if (DYN(n.zwrite != s_dyn.zwrite))
            vkCmdSetDepthWriteEnable(s_cmd, n.zwrite);
        if (DYN(n.zcmp != s_dyn.zcmp))
            vkCmdSetDepthCompareOp(s_cmd, n.zcmp);
        if (DYN(n.stest != s_dyn.stest))
            vkCmdSetStencilTestEnable(s_cmd, n.stest);
        if (DYN(memcmp(n.sop, s_dyn.sop, sizeof n.sop) || n.scmp != s_dyn.scmp))
            vkCmdSetStencilOp(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.sop[0], n.sop[1],
                              n.sop[2], n.scmp);
        if (DYN(n.srmask != s_dyn.srmask))
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.srmask);
        if (DYN(n.swmask != s_dyn.swmask))
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.swmask);
        if (DYN(n.sref != s_dyn.sref))
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.sref);
        if (DYN(memcmp(&n.slope, &s_dyn.slope, sizeof n.slope))) {
            vkCmdSetDepthBiasEnable(s_cmd, n.slope != 0.0f ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthBias(s_cmd, 0.0f, 0.0f, n.slope);
        }
        if (DYN(n.vw != s_dyn.vw || n.vh != s_dyn.vh)) {
            VkViewport vp;
            vp.x = 0.0f;
            vp.y = (float)rt->hh;
            vp.width = (float)rt->hw;
            vp.height = -(float)rt->hh;
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(s_cmd, 0, 1, &vp);
        }
        if (DYN(memcmp(&n.sr, &s_dyn.sr, sizeof n.sr)))
            vkCmdSetScissor(s_cmd, 0, 1, &n.sr);
        if (DYN(n.front != s_dyn.front))
            vkCmdSetFrontFace(s_cmd, n.front);
        if (DYN(n.cull != s_dyn.cull))
            vkCmdSetCullMode(s_cmd, n.cull);
        if (DYN(memcmp(n.bf, s_dyn.bf, sizeof n.bf)))
            vkCmdSetBlendConstants(s_cmd, n.bf);
#undef DYN
        s_dyn = n;
        s_dyn_valid = 1;
    }
    dyn_off[0] = (uint32_t)uo[0];
    dyn_off[1] = (uint32_t)uo[1];
    dyn_off[2] = (uint32_t)uo[2];
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1,
                            &s_set0, 3, dyn_off);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1,
                            &tset, 0, NULL);
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring[RING_VB].buf, &vb_off);
    vkCmdBindIndexBuffer(s_cmd, s_ring[RING_IB].buf, ib_off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(s_cmd, nidx, 1, 0, 0, 0);
    if (zb_split) {
        dyn_off[1] = (uint32_t)uo_plain;
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1,
                                &s_set0, 3, dyn_off);
        vkCmdSetCullMode(s_cmd, VK_CULL_MODE_FRONT_BIT);
        s_dyn.cull = VK_CULL_MODE_FRONT_BIT;
        vkCmdDrawIndexed(s_cmd, nidx, 1, 0, 0, 0);
    }
    s_draws++;
    s_sn.draws++;
    s_sn.d2 += s_draw_2d != 0;
    if (nidx > s_sn.max_idx)
        s_sn.max_idx = nidx;
    if (s_vf) {
        s_vf->draws++;
        s_vf->pass_draws++;
        if (rt->w >= 640)
            s_vf->d_main++;
        else if (rt->w == rt->h)
            s_vf->d_square++;
        else
            s_vf->d_other++;
    }
    rt->gen = ++s_rt_seq;
    if (rt->dt && use_z && (v->depth_mask || use_s))
        rt->dt->gen = rt->gen;
}

/* ---- flip ---------------------------------------------------------------------- */

static RenderTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && surface_offset && rt->offset == surface_offset
                && (!p->offset || (rt->w >= p->w && rt->h >= p->h))) {
            rt->presented = 1;
            return rt;
        }
    }
    /* The display's mode is larger than any target drawn there: a title
     * that clips every draw to the TV-safe area made this one from its
     * clip (Double-S.T.E.A.L 2's 720p front buffer, remade as 1216x684
     * after a loading screen reused its memory). Grown to the mode, so the
     * whole picture, border and all, is what is shown. */
    for (i = 0; p->offset && i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && surface_offset && rt->offset == surface_offset
                && rt->pitch == p->pitch && rt->w <= p->w && rt->h <= p->h
                && rt_grow(rt, p->w, p->h)) {
            rt->presented = 1;
            return rt;
        }
    }
    return NULL;
}

static void present_extent(const RenderTarget *rt, uint32_t *w, uint32_t *h)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    *w = rt->w;
    *h = rt->h;
    if (p->offset && p->offset == rt->offset && p->w && p->h) {
        if (p->w < *w) *w = p->w;
        if (p->h < *h) *h = p->h;
    }
}

static uint32_t s_shown_w, s_shown_h;

/* rt's host pixels read back: a copy into the readback buffer, a submit and
 * a wait. hw x hh BGRA8, tightly packed; valid until the next readback. The
 * ring positions survive the wait, since a draw may have placed its
 * vertices already (tex_bind runs inside on_draw). */
static struct nvk_buffer s_rb;

static uint8_t *s_rb_conv;     /* BO-110: a half-float target's pixels as BGRA8 */
static size_t   s_rb_conv_cap;

/* An image's level 0 (w x h, bpp bytes a texel) in s_rb.map, after a submit
 * and a wait; NULL when that failed. */
static const uint8_t *readback_raw(struct nvk_image *im, uint32_t w, uint32_t h, uint32_t bpp)
{
    VkDeviceSize need = (VkDeviceSize)w * h * bpp;
    VkDeviceSize pos[RING_N];
    VkBufferImageCopy bc;
    int k;

    if (s_rb.size < need) {
        if (!flush(1, FL_READBACK))
            return NULL;
        nvk_buffer_free(&s_rb);
        if (!nvk_buffer_new(&s_rb, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            return NULL;
    }
    end_encoder();
    ensure_cmd();
    nvk_image_to(s_cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_rb.buf, 1, &bc);
    {
        /* The copy's writes visible to the host. */
        VkMemoryBarrier2 mb;
        VkDependencyInfo di;
        memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(s_cmd, &di);
    }
    for (k = 0; k < RING_N; k++)
        pos[k] = s_ring_pos[k];
    if (!flush(1, FL_READBACK))
        return NULL;      /* the copy is not proven done: nothing to read */
    for (k = 0; k < RING_N; k++)
        s_ring_pos[k] = pos[k];
    return (const uint8_t *)s_rb.map;
}

/* BO-110: RECOMP_DEBUG=hdr_dump=<prefix>: at each fb_dump_at flip, the
 * presented target (w x h host pixels from its corner) as
 * <prefix>flip_NNNNN.rgbaf (as Metal: a 16-byte header "HDR1", w, h, 1 when
 * half float, then 32-bit float RGBA), and with HDR on the flip pass run
 * into an image of its own as <prefix>flip_NNNNN.pq10 ("PQ10", w, h, 0,
 * then A2B10G10R10 words: what the window's slot gets, before AA,
 * sharpening, the scale and the overlay) with <prefix>flip_NNNNN.json (the
 * levels the pass was given). Headless or not. */
static struct nvk_image s_hd_img;
static void hdr_dump(RenderTarget *rt, uint32_t w, uint32_t h)
{
    const char *prefix = recomp_env(RENV_HDR_DUMP);
    uint32_t flip = nv2a_pb_gpu_state()->flips, hdr[4], y, x;
    int half = rt->im.format == RT_FORMAT_HDR;
    const uint8_t *px;
    char path[1100];
    FILE *f;

    static uint32_t watch_last, watch_n;
    int watched = 0;
    /* hdr_watch (as Metal): a flip that drew the watched texture, and the one after it */
    if (watch_n && flip == watch_last + 1u)
        watched = 1;
    if (s_hdr_watch_hit) {
        s_hdr_watch_hit = 0;
        if (watch_n < 60 && flip - watch_last >= 20) {
            watched = 1;
            watch_last = flip;
            watch_n++;
        }
    }
    if (!prefix || !*prefix || !w || !h || w > rt->hw || h > rt->hh
            || !(watched || nv2a_pb_dump_at_listed(flip)))
        return;
    px = readback_raw(&rt->im, rt->hw, rt->hh, half ? 8u : 4u);
    if (!px)
        return;
    snprintf(path, sizeof path, "%sflip_%05u.rgbaf", prefix, flip);
    if ((f = fopen(path, "wb")) != NULL) {
        float *out = (float *)malloc((size_t)w * 16u);
        memcpy(&hdr[0], "HDR1", 4);
        hdr[1] = w;
        hdr[2] = h;
        hdr[3] = (uint32_t)half;
        fwrite(hdr, 4, 4, f);
        for (y = 0; out && y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t i = (size_t)y * rt->hw + x;
                if (half) {
                    const uint16_t *q = (const uint16_t *)px + i * 4u;
                    out[x * 4 + 0] = hdr_h2f(q[0]);
                    out[x * 4 + 1] = hdr_h2f(q[1]);
                    out[x * 4 + 2] = hdr_h2f(q[2]);
                    out[x * 4 + 3] = hdr_h2f(q[3]);
                } else {
                    const uint8_t *q = px + i * 4u;
                    out[x * 4 + 0] = q[2] / 255.0f;
                    out[x * 4 + 1] = q[1] / 255.0f;
                    out[x * 4 + 2] = q[0] / 255.0f;
                    out[x * 4 + 3] = q[3] / 255.0f;
                }
            }
            fwrite(out, 16, w, f);
        }
        free(out);
        fclose(f);
        fprintf(stderr, LOGP "hdr_dump: flip %u %ux%u %s -> %s\n", flip, w, h,
                half ? "half" : "bgra8", path);
    }
    if (!s_hdr)
        return;
    if (s_hd_img.img && (s_hd_img.w != w || s_hd_img.h != h)) {
        garbage_push(&s_hd_img, VK_NULL_HANDLE, NULL);
        memset(&s_hd_img, 0, sizeof s_hd_img);
    }
    if (!s_hd_img.img && !nvk_image_new(&s_hd_img, HDR_SLOT_FORMAT, w, h, 1,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        memset(&s_hd_img, 0, sizeof s_hd_img);
        return;
    }
    if (!hdr_flip_pass(rt, &s_hd_img, w, h)
            || !(px = readback_raw(&s_hd_img, w, h, 4u)))
        return;
    snprintf(path, sizeof path, "%sflip_%05u.pq10", prefix, flip);
    if ((f = fopen(path, "wb")) != NULL) {
        memcpy(&hdr[0], "PQ10", 4);
        hdr[1] = w;
        hdr[2] = h;
        hdr[3] = 0;
        fwrite(hdr, 4, 4, f);
        fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%sflip_%05u.json", prefix, flip);
    if ((f = fopen(path, "w")) != NULL) {
        float pk, pw, hw, sh, hl;
        int srgb;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        nv2a_host_hdr_tone(&sh, &hl, &srgb);
        fprintf(f, "{\"flip\": %u, \"w\": %u, \"h\": %u, \"peak\": %.1f, \"paper\": %.1f,"
                " \"hud\": %.1f, \"shadows\": %.3f, \"highlights\": %.3f, \"decode\": \"%s\","
                " \"gains\": %d", flip, w, h, pk, pw, hw, sh, hl, srgb ? "srgb" : "2.2",
                hdr_gains_on());
        /* BO-195: hdr_gains_ab's state of this frame and the probe it marks */
        if (s_hg_abn)
            fprintf(f, ", \"gains_ab\": %u, \"ab\": %d, \"probe\": \"%016llx\"", s_hg_abn,
                    s_hg_ab, (unsigned long long)nv2a_texrep_hdr_probe_hash(s_hg_probe));
        if (s_hg_only_n)
            fprintf(f, ", \"only\": %d", s_hg_only_n);
        fprintf(f, "}\n");
        fclose(f);
    }
}

static const uint8_t *host_readback(RenderTarget *rt)
{
    int half = rt->im.format == RT_FORMAT_HDR;
    if (!readback_raw(&rt->im, rt->hw, rt->hh, half ? 8u : 4u))
        return NULL;
    if (half) {
        /* As a BGRA8 target would hold it: clamped to [0, 1], rounded. */
        size_t n = (size_t)rt->hw * rt->hh, i;
        const uint16_t *q = (const uint16_t *)s_rb.map;
        if (s_rb_conv_cap < n * 4u) {
            uint8_t *nb = (uint8_t *)realloc(s_rb_conv, n * 4u);
            if (!nb)
                return NULL;
            s_rb_conv = nb;
            s_rb_conv_cap = n * 4u;
        }
        for (i = 0; i < n; i++, q += 4) {
            uint8_t *o = s_rb_conv + i * 4u;
            int c;
            for (c = 0; c < 4; c++) {
                float f = hdr_h2f(q[c]);
                uint8_t b = !(f > 0.0f) ? 0 : f >= 1.0f ? 255 : (uint8_t)(f * 255.0f + 0.5f);
                o[c == 3 ? 3 : 2 - c] = b;
            }
        }
        return s_rb_conv;
    }
    return (const uint8_t *)s_rb.map;
}

/* One host pixel (A8R8G8B8) as the 16-bit surface keeps it: G8B8 its green
 * and blue bytes (G high), anything else R5G6B5. G8B8 used to be packed as
 * R5G6B5 too, so a G8B8 texture read back from one saw other bits. */
static uint16_t px16(const RenderTarget *rt, uint32_t c)
{
    if (surf_is_g8b8(rt->cfmt))
        return (uint16_t)(c & 0xFFFFu);
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

/* The host pixels (hw x hh) into the guest surface at addr: 32-bit as they
 * are, 16-bit as R5G6B5 (G8B8: its two bytes); a scaled target box-filtered
 * to guest size. */
static void write_back_px(RenderTarget *rt, const uint8_t *px, uint32_t addr, uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, n = rt->hw / rt->w, y, x;
    static uint32_t *row;
    static uint32_t row_cap;
    static uint8_t *frac;
    static size_t frac_cap;

    /* A fractional scale (1.5x): to guest size first, nearest, then as an
     * unscaled target. */
    if (rt->hw % rt->w != 0 || rt->hh != rt->h * n) {
        size_t need = (size_t)w * rt->h * 4u;
        if (frac_cap < need) {
            uint8_t *nf = (uint8_t *)realloc(frac, need);
            if (!nf)
                return;
            frac = nf;
            frac_cap = need;
        }
        nv2a_resample_nearest32(px, rt->hw * 4u, rt->hw, rt->hh, frac, w * 4u, w, rt->h);
        px = frac;
        n = 1;
    }

    if (rt_swz_ok(rt->swz, rt->w, rt->h) && (bpp == 4 || bpp == 2)) {
        /* Swizzled: linear first (box-filtered when scaled), then Z order. */
        uint32_t *lin = (uint32_t *)swz_staging((size_t)w * rt->h * 4u);
        uint32_t i;
        if (!lin)
            return;
        if (n == 1)
            memcpy(lin, px, (size_t)w * rt->h * 4u);
        else
            nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, (uint8_t *)lin, w * 4u);
        if (bpp == 2) {
            uint16_t *o = (uint16_t *)lin;   /* in place: each texel shrinks */
            for (i = 0; i < w * rt->h; i++)
                o[i] = px16(rt, lin[i]);
        }
        xbox_swizzle_rect(dst, lin, w, rt->h, bpp);
        s_writebacks++;
        nvr_capture_gpu_wrote(addr, w * rt->h * bpp);
        return;
    }
    if (bpp >= 4) {
        if (n == 1)
            for (y = 0; y < rt->h; y++)
                memcpy(dst + (size_t)y * pitch, px + (size_t)y * w * 4u, (size_t)w * 4u);
        else
            nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, dst, pitch);
    } else if (bpp == 2) {
        if (row_cap < w) {
            uint32_t *nr = (uint32_t *)realloc(row, (size_t)w * 4);
            if (!nr)
                return;
            row = nr;
            row_cap = w;
        }
        for (y = 0; y < rt->h; y++) {
            uint16_t *o = (uint16_t *)(dst + (size_t)y * pitch);
            const uint32_t *in;
            if (n == 1) {
                in = (const uint32_t *)(px + (size_t)y * w * 4u);
            } else {
                nv2a_downscale_box32(px + (size_t)y * n * rt->hw * 4u, rt->hw * 4u, w, 1, n,
                                     (uint8_t *)row, w * 4u);
                in = row;
            }
            for (x = 0; x < w; x++)
                o[x] = px16(rt, in[x]);
        }
    } else {
        return;
    }
    s_writebacks++;
    /* RECOMP_NVR_CAPTURE: these bytes are the GPU's, which a replay writes
     * back itself; the capture must not hand them to it as the title's. */
    nvr_capture_gpu_wrote(addr, pitch * rt->h);
}

/* rt into guest memory now, if drawn since its last write-back (Metal's
 * rt_sync): a GPU wait. */
static void rt_sync(RenderTarget *rt, uint32_t pitch)
{
    const uint8_t *px;

    if (!rt->im.img || !rt->dirty || !pitch || pitch < rt->w * 2u)
        return;
    if ((px = host_readback(rt)) != NULL)
        write_back_px(rt, px, rt->addr, pitch);
    rt->dirty = 0;
    rt_own_reset(rt);
}

static void vk_sync_guest(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h)
{
    RenderTarget *rt = NULL;
    uint32_t addr = nv2a_pb_dma_resolve(offset) & 0x07FFFFFFu;
    int i;

    if (s_failed)
        return;
    for (i = 0; i < RT_MAX && !rt; i++)
        if (s_rt[i].im.img && s_rt[i].offset == offset && s_rt[i].w >= w && s_rt[i].h >= h)
            rt = &s_rt[i];
    if (!rt) {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *c = &s_rt[i];
            if (c->im.img && (c->addr & 0x07FFFFFFu) == addr
                    && (!rt || c->last_use > rt->last_use))
                rt = c;
        }
    }
    if (rt && !rt_drop_if_stale(rt, "sync"))
        rt_sync(rt, pitch);
}

/* For tests/nv2a_backend_smoke, the shapes of the Metal ones. */
uint64_t nv2a_pb_vulkan_writebacks(void) { return s_writebacks; }

int nv2a_pb_vulkan_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                         uint32_t *sw, uint32_t *sh)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].im.img && s_rt[i].offset == offset) {
            *tw = s_rt[i].w;
            *th = s_rt[i].h;
            *sw = s_shown_w;
            *sh = s_shown_h;
            return 1;
        }
    return 0;
}

const uint8_t *nv2a_pb_vulkan_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && rt->offset == offset) {
            *hw = rt->hw;
            *hh = rt->hh;
            return host_readback(rt);
        }
    }
    return NULL;
}

/* ---- visibility tests: occlusion queries -------------------------------------
 *
 * Metal's scheme (see "Visibility tests" in nv2a_pb_metal.m) with Vulkan
 * queries in place of visibility-result slots: a query is begun inside a
 * render pass and must end in it, so a stretch that spans passes takes a slot
 * per pass; slots are reset from the host when taken (hostQueryReset) and
 * read once the submit they were recorded in is done. GET_REPORT submits the
 * open command buffer when the report counts in it, so the count can land
 * within the frame. vk_occ=sync waits at GET_REPORT; =fixed leaves reports
 * to the walker (every test visible). */
static OccQuery          s_qpool[OCC_POOL];
static OccQuery         *s_qfree;
static int               s_qmade;
static unsigned char     s_vis_used[VIS_SLOTS];
static int               s_vis_next;
static int               s_retire[VIS_SLOTS];
static uint64_t          s_retire_serial[VIS_SLOTS];
static int               s_nretire;
static struct nv2a_occ   s_occ;
static int               s_occ_mode = -1;
static uint64_t          s_occ_ovf, s_report_commits;

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = recomp_env(RENV_VK_OCC);
        s_occ_mode = !e ? 0 : !strcasecmp(e, "sync") ? 1 : !strcasecmp(e, "fixed") ? 2 : 0;
    }
    return s_occ_mode;
}

static int vis_alloc(void)
{
    int i, k;
    for (k = 0; k < VIS_SLOTS; k++) {
        i = (s_vis_next + k) % VIS_SLOTS;
        if (!s_vis_used[i]) {
            s_vis_used[i] = 1;
            s_vis_next = (i + 1) % VIS_SLOTS;
            vkResetQueryPool(s_dev, s_vkq, (uint32_t)i, 1);
            return i;
        }
    }
    return -1;
}

static void occ_query_begin(void)
{
    OccQuery *q = s_occ_q;
    int i;
    if (!q || !s_pass || s_occ_slot >= 0)
        return;
    if (q->n >= OCC_QSLOTS || (i = vis_alloc()) < 0) {
        q->ovf = 1;
        s_occ_ovf++;
        return;
    }
    q->slot[q->n] = i;
    q->serial[q->n] = s_next_serial;
    q->n++;
    vkCmdBeginQuery(s_cmd, s_vkq, (uint32_t)i, s_qprecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_sn.occ++;
    s_occ_slot = i;
}

static void occ_pass_begin(void)
{
    occ_query_begin();
}

static void occ_pass_end(void)
{
    if (s_occ_slot >= 0 && s_pass)
        vkCmdEndQuery(s_cmd, s_vkq, (uint32_t)s_occ_slot);
    s_occ_slot = -1;
}

static uint64_t occ_now_us(void *ctx)
{
    (void)ctx;
    return now_ns() / 1000u;
}

static void *occ_get(void *ctx)
{
    OccQuery *q;
    (void)ctx;
    if (s_qfree) {
        q = s_qfree;
        s_qfree = q->next;
    } else if (s_qmade < OCC_POOL) {
        q = &s_qpool[s_qmade++];
    } else {
        return NULL;
    }
    q->n = 0;
    q->ovf = 0;
    return q;
}

static void occ_put(void *ctx, void *qv)
{
    OccQuery *q = (OccQuery *)qv;
    uint64_t done = serial_done();
    int i;
    (void)ctx;
    if (!q)
        return;
    if (q == s_occ_q) {
        occ_pass_end();
        s_occ_q = NULL;
    }
    for (i = 0; i < q->n; i++) {
        if (q->serial[i] <= done) {
            s_vis_used[q->slot[i]] = 0;
        } else {
            s_retire[s_nretire] = q->slot[i];
            s_retire_serial[s_nretire++] = q->serial[i];
        }
    }
    q->n = 0;
    q->next = s_qfree;
    s_qfree = q;
}

static void occ_begin(void *ctx, void *qv)
{
    (void)ctx;
    occ_pass_end();
    s_occ_q = (OccQuery *)qv;
    occ_query_begin();
}

static void occ_end(void *ctx, void *qv)
{
    (void)ctx;
    if (s_occ_q == (OccQuery *)qv) {
        occ_pass_end();
        s_occ_q = NULL;
    }
}

static int occ_result(void *ctx, void *qv, int flush_gpu, uint64_t *count)
{
    OccQuery *q = (OccQuery *)qv;
    uint64_t sum = 0, done;
    int i;
    (void)ctx;
    for (i = 0; i < q->n; i++) {
        if (q->serial[i] == s_next_serial) {     /* not submitted yet */
            if (!flush_gpu)
                return 0;
            flush(0, FL_POLL);
        }
    }
    /* Submitted. With a cheap poll the counts are read once their submits
     * are done; without one the query pool itself says which are there. */
    if (poll_ok()) {
        done = serial_done();
        for (i = 0; i < q->n; i++)
            if (q->serial[i] > done)
                return 0;
    }
    for (i = 0; i < q->n; i++) {
        uint64_t v = 0;
        uint64_t t0 = s_vf ? now_ns() : 0;
        VkResult qr = vkGetQueryPoolResults(s_dev, s_vkq, (uint32_t)q->slot[i], 1, sizeof v,
                                            &v, sizeof v, VK_QUERY_RESULT_64_BIT);
        if (s_vf) {
            s_vf->qres++;
            s_vf->qres_ns += now_ns() - t0;
        }
        if (qr == VK_SUCCESS)
            sum += v;
        else
            return 0;
    }
    *count = q->ovf ? NV2A_OCC_VISIBLE : sum;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static int occ_ready_on(void)
{
    if (occ_mode() == 2 || !init() || !s_vkq)
        return 0;
    if (!s_occ.ops) {
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
        nv2a_occ_set_scale_x2(&s_occ, nv2a_host_render_scale_x2());
    }
    return 1;
}

static void vk_on_zpass(int enable)
{
    if (occ_ready_on())
        nv2a_occ_zpass(&s_occ, enable);
}

static void vk_on_zpass_clear(void)
{
    if (occ_ready_on())
        nv2a_occ_zpass_clear(&s_occ);
}

static int report_in_cmd(uint32_t va)
{
    const struct nv2a_occ_report *p;
    int k, i;
    if (!s_occ.npend || s_occ.pend[s_occ.npend - 1].va != va)
        return 0;
    p = &s_occ.pend[s_occ.npend - 1];
    for (k = 0; k < p->n; k++) {
        const OccQuery *q = (const OccQuery *)p->q[k];
        for (i = 0; i < q->n; i++)
            if (q->serial[i] == s_next_serial)
                return 1;
    }
    return 0;
}

static int vk_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    nv2a_occ_report(&s_occ, va, occ_mode() == 1);
    if (occ_mode() == 0 && (s_cmd || s_up) && report_in_cmd(va)) {
        s_report_commits++;
        if (s_pass && batch_draws()) {
            /* vk_batch: the open pass is not ended for it; the counts go to
             * the GPU with the next submit, at the latest when the frame is
             * complete (or from on_poll, if the title seems to wait). */
            if (!s_report_defer) {
                s_report_defer = 1;
                s_report_defer_t = now_ns();
            }
        } else {
            flush(0, FL_REPORT);
        }
    }
    return 1;
}

static void vk_on_poll(void)
{
    if (!s_occ.npend)
        return;
    /* A deferred report's counts are not asked for by ending the pass, unless
     * half a frame has gone and the title may be waiting for them. */
    if (s_report_defer && now_ns() - s_report_defer_t > 8000000ull)
        flush(0, FL_REPORT);
    nv2a_occ_poll(&s_occ, !s_report_defer);
}

/* Retired slots whose submit is done are free again. */
static void occ_after_wait(void)
{
    uint64_t done = serial_done();
    int i = 0;
    while (i < s_nretire) {
        if (s_retire_serial[i] <= done) {
            s_vis_used[s_retire[i]] = 0;
            s_retire[i] = s_retire[--s_nretire];
            s_retire_serial[i] = s_retire_serial[s_nretire];
        } else {
            i++;
        }
    }
    if (s_occ.ops)
        nv2a_occ_poll(&s_occ, 0);
}

/* ---- frames ------------------------------------------------------------------- */

/* The flip's frame advance: the frame just recorded is submitted, and the
 * next one is reused once its last submit (s_frames flips back) is done. */
static void frame_advance(void)
{
    Frame *f;
    uint32_t k;
    uint64_t t0;

    if (s_failed)
        return;
    s_fi = (s_fi + 1) % s_frames;
    f = &s_fr[s_fi];
    if (f->fence_serial) {
        /* vk_poll=0: the frame's fence, signalled long ago as a rule; the
         * wait is the question and, when the GPU is behind, the answer.
         * Not done: the fence stays as it is (a pending fence must not be
         * reset) and so does everything of the frame's. */
        VkResult fr = VK_SUCCESS;
        int out;
        t0 = now_ns();
        out = nvk_wait_fence(NVK_WAIT_FRAME, "backend frame fence", f->fence, &fr);
        if (out != NVK_WAIT_OK) {
            char what[96];
            snprintf(what, sizeof what, "backend frame fence (serial %llu)",
                     (unsigned long long)f->fence_serial);
            wait_failed(out, fr, what);
            return;
        }
        serial_known(f->fence_serial);
        vkResetFences(s_dev, 1, &f->fence);
        f->fence_serial = 0;
        t0 = now_ns() - t0;
        if (t0 > 500000ull) {
            s_frame_waits++;
            s_frame_wait_ns += t0;
        }
        if (s_vf) {
            s_vf->waits++;
            s_vf->wait_ns += t0;
        }
    }
    if (f->serial && serial_done() < f->serial) {
        int ok;
        t0 = now_ns();
        ok = serial_wait(f->serial, NVK_WAIT_FRAME);
        s_frame_waits++;
        s_frame_wait_ns += now_ns() - t0;
        if (!ok)
            return;     /* its pools and rings may still be in use */
    }
    vkResetCommandPool(s_dev, f->pool, 0);
    f->used = 0;
    for (k = 0; k < f->ndps; k++)
        vkResetDescriptorPool(s_dev, f->dps[k], 0);
    if (s_hf_pool[s_fi])
        vkResetDescriptorPool(s_dev, s_hf_pool[s_fi], 0);
    f->dp_cur = 0;
    s_tset_last = VK_NULL_HANDLE;
    rings_reset();
    garbage_collect();
    occ_after_wait();
}

/* ---- caches on disk ------------------------------------------------------------- */

/* BO-208. The driver's VkPipelineCache on disk, one file per driver:
 * vk_pipeline_<vendor>_<device>_<first 4 bytes of pipelineCacheUUID>[_t<hash of
 * TU_DEBUG>].bin. The bundled Turnip, a system driver and a custom package of
 * the same GPU report one vendor and device id and differ in the UUID; and
 * TU_DEBUG flags (vk_dyn.c: sysmem, nobin, noubwc) change what Turnip
 * compiles. Each keeps its own file, so switching drivers back and forth
 * throws nothing away. The file is our 32-byte header and the driver's data:
 * the driver checks its own header, this one catches what the driver would
 * only silently ignore, a truncated or bit-flipped file (CRC), and a file
 * of another driver build under the same name. */
#define PCB_MAGIC "XRVKPC02"
struct pcb_hdr {
    char     magic[8];
    uint32_t vendor, device, driver, len, crc, uuid;
};

static uint32_t crc32_buf(const void *p, size_t n)
{
    static uint32_t tab[256];
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!tab[1]) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tab[k] = v;
        }
    }
    for (i = 0; i < n; i++)
        c = tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t pcb_uuid4(void)
{
    uint32_t u;
    memcpy(&u, C->props.pipelineCacheUUID, 4);
    return u;
}

static void pcache_path(char *p, size_t n, int *ok)
{
    char dir[1024], tu[24] = "";
    const char *t = getenv("TU_DEBUG");
    if (t && *t)
        snprintf(tu, sizeof tu, "_t%08x",
                 (unsigned)nv2a_fnv64(t, strlen(t), NV2A_FNV64_INIT));
    *ok = nvk_cache_dir(dir, sizeof dir)
          && snprintf(p, n, "%s/vk_pipeline_%04X_%04X_%08x%s.bin", dir, C->props.vendorID,
                      C->props.deviceID, pcb_uuid4(), tu) < (int)n;
}

/* Blobs of drivers not used for 90 days, and the unheadered one of before
 * BO-208 (vk_pipeline_<vendor>_<device>.bin), are removed. */
static void pcache_prune(const char *keep)
{
    char dir[1024], p[1200];
    DIR *d;
    struct dirent *e;
    time_t now = time(NULL);
    if (!nvk_cache_dir(dir, sizeof dir) || !(d = opendir(dir)))
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        size_t l = strlen(e->d_name);
        if (strncmp(e->d_name, "vk_pipeline_", 12) || l < 16 || strcmp(e->d_name + l - 4, ".bin")
                || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p
                || !strcmp(p, keep) || stat(p, &st) != 0)
            continue;
        if (l == 12 + 9 + 4 || now - st.st_mtime > 90 * 24 * 3600) {
            remove(p);
            fprintf(stderr, LOGP "pipeline cache: removed %s (%s)\n", e->d_name,
                    l == 12 + 9 + 4 ? "the format before BO-208" : "not used for 90 days");
        }
    }
    closedir(d);
}

static void pcache_save_locked(void)
{
    char path[1100], tmp[1200];
    size_t n = 0;
    void *data;
    FILE *f;
    int ok;

    if (!s_pcache || !s_pso_since_save)
        return;
    pcache_path(path, sizeof path, &ok);
    if (!ok || vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    data = malloc(n);
    if (!data)
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, data) == VK_SUCCESS) {
        struct pcb_hdr h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, PCB_MAGIC, 8);
        h.vendor = C->props.vendorID;
        h.device = C->props.deviceID;
        h.driver = C->props.driverVersion;
        h.len = (uint32_t)n;
        h.crc = crc32_buf(data, n);
        h.uuid = pcb_uuid4();
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if ((f = fopen(tmp, "wb")) != NULL) {
            int good = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(data, 1, n, f) == n;
            if (fclose(f) == 0 && good && rename(tmp, path) == 0) {
                s_pso_since_save = 0;
                if (vk_trace())
                    fprintf(stderr, LOGP "pipeline cache: %zu bytes to %s\n", n, path);
            } else {
                remove(tmp);
            }
        }
    }
    free(data);
}

/* One save at a time: the flip's periodic one runs on a thread of its own,
 * the way out's (quiesce, atexit) waits for it. vkGetPipelineCacheData is
 * safe beside pipeline creation in the same cache (not created externally
 * synchronised). */
static pthread_mutex_t s_pcs_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_pcs_running;                /* a periodic save's thread (atomic) */

static void pcache_save(void)
{
    pthread_mutex_lock(&s_pcs_mu);
    pcache_save_locked();
    pthread_mutex_unlock(&s_pcs_mu);
}

static void *pcache_save_main(void *arg)
{
    (void)arg;
    pcache_save();
    __atomic_store_n(&s_pcs_running, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* The flip's save (BO-245): the cache file is written off the render
 * thread. On the Deck's SD card that fopen/fwrite/rename could wait tens of
 * ms behind other writes; the render thread now only starts the thread. */
static int s_quiesced;                   /* nvk_backend_quiesce saved the cache (below) */

static void pcache_save_async(void)
{
    pthread_t th;
    if (s_quiesced)
        return;                          /* the way out has saved it */
    if (__atomic_exchange_n(&s_pcs_running, 1, __ATOMIC_ACQ_REL))
        return;                          /* one is still writing: next time */
    if (pthread_create(&th, NULL, pcache_save_main, NULL) != 0) {
        __atomic_store_n(&s_pcs_running, 0, __ATOMIC_RELEASE);
        pcache_save();
        return;
    }
    pthread_detach(th);
}

static int s_blob_cold;                  /* pcache_load: the driver kept nothing */
static int s_pc_save;                    /* the pre-warm thread asks for a save (atomic) */
static int s_pc_stop;                    /* the way out: pre-warm stops (atomic) */
static int s_quiesced;                   /* nvk_backend_quiesce saved the cache */

static void at_exit_save(void)
{
    if (C && !s_failed && !s_quiesced)
        pcache_save();
}

/* The way out (nv2a_vk_core.h): the pre-warm thread stops at its next
 * pipeline and the pipeline cache is written now, while the device is
 * whole; atexit then has nothing more to do with it. */
int nvk_backend_quiesce(int timeout_ms)
{
    int waited = 0;
    __atomic_store_n(&s_pc_stop, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_pc_warming, __ATOMIC_ACQUIRE) && waited < timeout_ms) {
        usleep(5000);
        waited += 5;
    }
    if (__atomic_load_n(&s_pc_warming, __ATOMIC_ACQUIRE)) {
        XR_NOTICE(LOGP "teardown: the pipeline pre-warm still runs after %d ms\n", waited);
        return 0;
    }
    if (C && !s_failed && !s_quiesced) {
        uint64_t t0 = now_ns();
        pcache_save();
        XR_NOTICE(LOGP "teardown: pipeline cache written (%.1f ms)\n",
                (double)(now_ns() - t0) / 1e6);
    }
    s_quiesced = 1;
    return 1;
}


static VkPipelineCache pcache_load(void)
{
    char path[1100];
    VkPipelineCacheCreateInfo ci;
    VkPipelineCache pc = VK_NULL_HANDLE;
    void *data = NULL, *payload = NULL;
    size_t n = 0, fsz = 0;
    const char *why = NULL;
    FILE *f;
    int ok;

    pcache_path(path, sizeof path, &ok);
    if (ok && (f = fopen(path, "rb")) != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)sz)) != NULL) {
                fsz = fread(data, 1, (size_t)sz, f);
                if (fsz != (size_t)sz) {
                    free(data);
                    data = NULL;
                    why = "unreadable";
                }
            }
        }
        fclose(f);
        if (data) {
            struct pcb_hdr h;
            if (fsz < sizeof h) {
                why = "truncated";
            } else {
                memcpy(&h, data, sizeof h);
                if (memcmp(h.magic, PCB_MAGIC, 8) != 0)
                    why = "not one of ours (the format before BO-208)";
                else if (h.len != fsz - sizeof h)
                    why = "truncated";
                else if (crc32_buf((char *)data + sizeof h, h.len) != h.crc)
                    why = "damaged (CRC)";
                else if (h.vendor != C->props.vendorID || h.device != C->props.deviceID
                         || h.driver != C->props.driverVersion || h.uuid != pcb_uuid4())
                    why = "of another driver build";
                else {
                    payload = (char *)data + sizeof h;
                    n = h.len;
                }
            }
        }
        if (why) {
            fprintf(stderr, LOGP "pipeline cache: %s is %s: discarded, built again\n", path, why);
            remove(path);
        }
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = n;
    ci.pInitialData = n ? payload : NULL;
    /* The driver validates its own header (vendor, device, cache UUID) and
     * starts empty when it does not match. */
    if (vkCreatePipelineCache(s_dev, &ci, NULL, &pc) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(s_dev, &ci, NULL, &pc);
        n = 0;
    }
    free(data);
    {
        /* What the driver kept of the file (it drops one of another driver). */
        size_t kept = 0;
        vkGetPipelineCacheData(s_dev, pc, &kept, NULL);
        s_blob_cold = kept < 64;
        fprintf(stderr, LOGP "pipeline cache: %s (%zu bytes, %zu kept by the driver)\n",
                n ? path : "empty", n, kept);
    }
    if (ok)
        pcache_prune(path);
    return pc;
}

/* ---- the key list and the pre-warm thread ---------------------------------------- */

static pthread_mutex_t s_pcf_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_pcf itself */
static int      s_pc_rescan;            /* asked for while one ran */
static uint64_t s_pc_warm[3], s_pc_warm_ns;
static char     s_pc_path[1100], s_pc_ver[256];
static int      s_pc_cold;              /* BO-208: a first build: the manifest brought keys or the driver's cache was empty */
static uint64_t s_pc_hold_ns = 3000000000ull;   /* how long the title may be held for the first pre-warm */

/* Appends a key the ack thread just built. */
static nv2a_pcache *s_gapf;             /* BO-208: gaps.bin: what the render thread had to build */
static void pc_put_(uint32_t type, const void *data, uint32_t len, int gap)
{
    pthread_mutex_lock(&s_pcf_mu);
    int added = s_pcf ? nv2a_pcache_put(s_pcf, type, data, len) : 0;
    if (s_pcf && added < 0) {
        static int said;
        if (!said++)
            XR_ERROR( LOGP "pcache: cannot append; the key list is not written\n");
    }
    if (gap && added > 0 && s_gapf) {
        /* A key the lists lacked when the title needed it: the field's gap in
         * the manifest. A vertex program as its hash only (the microcode is
         * the title's); pcache_keys.py manifest takes the file as it is. */
        if (type == PC_VS) {
            uint64_t h = nv2a_fnv64(data, len, NV2A_FNV64_INIT);
            nv2a_pcache_put(s_gapf, NV2A_PC_MANIFEST_VS, &h, 8);
        } else {
            nv2a_pcache_put(s_gapf, type, data, len);
        }
    }
    pthread_mutex_unlock(&s_pcf_mu);
}

/* The render thread's builds: a gap if the lists had it not. */
static void pc_put(uint32_t type, const void *data, uint32_t len)
{
    pc_put_(type, data, len, 1);
}

/* The seeds of a pre-build: not gaps. */
static void pc_seed(uint32_t type, const void *data, uint32_t len)
{
    pc_put_(type, data, len, 0);
}

/* A module of the tables by its key id, or VK_NULL_HANDLE. s_pc_mu held. */
static VkShaderModule module_by_kid(uint64_t kid, int fragment)
{
    int i;
    if (!fragment) {
        if (kid == PC_VS_PASS)
            return s_vs_pass;
        for (i = 0; i < VS_MAX; i++)
            if (s_vs[i].used && !s_vs[i].bad && s_vs[i].kid == kid)
                return s_vs[i].mod;
    } else {
        if (kid == PC_FS_BASIC)
            return s_fs_basic;
        for (i = 0; i < s_fs_n; i++)
            if (!s_fs[i].bad && s_fs[i].kid == kid)
                return s_fs[i].mod;
    }
    return VK_NULL_HANDLE;
}

/* BO-208. Builds every key of the list into the ack thread's own tables:
 * first the shaders the list's pipelines use, then the pipelines, then the
 * other shaders (keys seeded from the title's executable or the shipped
 * manifest that nothing drew yet: nobody waits for those), so the ack thread
 * finds them built. With the SPIR-V cache and the driver's pipeline cache
 * warm this is a few lookups; after a driver update, a changed shader
 * generator or a new manifest it is the compiling the ack thread would
 * otherwise do mid-game.
 *
 * Two rounds. Round 0 builds the variant the targets are in now (SDR, or
 * HDR with HDR on) on several threads, and is what the title is held for
 * ("Preparing graphics N of M"). Round 1 builds the other variant, when the
 * display or the system can do HDR at all (pc_hdr_reachable), on one
 * low-priority thread after the game has started, so that switching HDR in
 * the settings later waits for nothing. What the other settings change (the
 * AA modes, sharpening, render scale) is the presenter's and is built at its
 * first frame (fb_present_*.c); the title's own pipelines have one sample
 * and no resolution in their keys. */
typedef struct {
    nv2a_pcache *pc;
    size_t       n;
    uint64_t    *need;
    size_t       nneed;
    int          want_sdr, want_hdr, pass;
    size_t       next;                  /* the next record to take (atomic) */
    uint64_t     nb[3];                 /* built (atomic) */
    uint32_t     done;                  /* records handled (atomic) */
    uint32_t     total;
    int          report;                /* round 0: tell the window */
    int          cold;
} PcJob;

/* The other variant can be reached: the display or the OS can do HDR (the
 * toolkit's nv2a_host_hdr_potential: BO-207 sets it on Windows too) or HDR
 * is on, or a pre-build was told so. */
static int pc_hdr_reachable(void)
{
    const char *ph = recomp_env(RENV_PCACHE_HDR);
    return __atomic_load_n(&s_hdr, __ATOMIC_ACQUIRE) || nv2a_host_hdr_potential() > 1.0f
           || (ph && (*ph == '1' || *ph == '2'));     /* 2: a test of the background round */
}

/* One record of the list for the job's pass, as the single thread did.
 * `parsed` and `glsl` are the calling thread's. */
static void pc_warm_item(PcJob *J, size_t i, NV2AVshProgram *parsed, char *glsl)
{
    nv2a_pcache *pc = J->pc;
    uint64_t *need = J->need;
    size_t nneed = J->nneed, q;
    int pass = J->pass, want_sdr = J->want_sdr, want_hdr = J->want_hdr, v, handled = 0;
    const void *d;
    uint32_t type, len;

    if (!nv2a_pcache_get(pc, i, &type, &d, &len))
        return;
    if (pass == 0 && type == PC_VS) {
        uint64_t kid = vs_kid(d, len);
        for (q = 0; q < nneed && need[q] != kid; q++)
            ;
        if (q == nneed)
            return;                     /* pass 2 */
    }
    if (pass == 0 && (type == PC_FS || type == PC_FS_HDR)) {
        /* Needed when a pipeline names it in either variant. */
        uint64_t k0 = fs_kid(d, len, 0), k1 = fs_kid(d, len, 1);
        for (q = 0; q < nneed && need[q] != k0 && need[q] != k1; q++)
            ;
        if (q == nneed)
            return;                     /* pass 2 */
    }
    if (pass != 1 && type == PC_VS && len >= 16 && len % 16 == 0
            && len <= NV2A_VSH_SLOTS * 16u) {
        uint32_t hash = 2166136261u, k;
        uint16_t inputs = 0;
        VkShaderModule mod;
        VsEntry *e;
        handled = 1;
        for (k = 0; k < len / 4u; k++)      /* nv2a_vsh_program_hash */
            hash = (hash ^ ((const uint32_t *)d)[k]) * 16777619u;
        if (!hash)
            hash = 1;
        pthread_mutex_lock(&s_pc_mu);
        e = vs_find(hash);
        pthread_mutex_unlock(&s_pc_mu);
        if (e || !(mod = vs_build((const uint32_t *)d, (int)(len / 16u), hash, &inputs, parsed,
                                  glsl, 1)))
            goto out;
        pthread_mutex_lock(&s_pc_mu);
        if (vs_find(hash) || !vs_take(hash, vs_kid(d, len), mod, inputs))
            e = NULL;                   /* the ack thread was first, or the table is full */
        else
            e = s_vs, __atomic_fetch_add(&J->nb[PC_VS], 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&s_pc_mu);
        if (!e)
            vkDestroyShaderModule(s_dev, mod, NULL);
    } else if (pass != 1 && (type == PC_FS || type == PC_FS_HDR)
               && len == sizeof(NV2ACombinerState)
               && !((const uint8_t *)d)[offsetof(NV2ACombinerState, debug_show)]) {
        handled = 1;
        for (v = type == PC_FS_HDR; v < 2; v++) {
            NV2ACombinerState st;
            VkShaderModule mod;
            uint32_t hash;
            FsEntry *e;
            if (!(v ? want_hdr : want_sdr))
                continue;
            memcpy(&st, d, sizeof st);
            hash = fnv(&st, sizeof st);
            if (v)
                hash ^= 0x48445200u;    /* as fs_select */
            pthread_mutex_lock(&s_pc_mu);
            e = fs_find(hash, &st, v);
            pthread_mutex_unlock(&s_pc_mu);
            if (e || !(mod = fs_build(&st, hash, glsl, 1, v)))
                continue;
            pthread_mutex_lock(&s_pc_mu);
            if (fs_find(hash, &st, v) || !fs_take(hash, &st, mod, v))
                e = NULL;
            else
                e = s_fs, __atomic_fetch_add(&J->nb[PC_FS], 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&s_pc_mu);
            if (!e)
                vkDestroyShaderModule(s_dev, mod, NULL);
        }
    } else if (pass == 1 && type == PC_PSO && len == sizeof(PsoDisk)) {
        handled = 1;
        for (v = 0; v < 3; v++) {
            PsoDisk dk;
            PsoKey k;
            VkPipeline p;
            uint32_t at;
            int found, full;
            memcpy(&dk, d, sizeof dk);
            if (dk.depth & 2u) {
                /* A half-float target's, recorded so. */
                if (v || !want_hdr)
                    continue;
            } else if (!v) {
                /* SDR's, or (its fs an HDR variant) an 8-bit
                 * target's with HDR on, recorded so. */
                if (!want_sdr && !want_hdr)
                    continue;
            } else {
                /* BO-110: the same pipeline with the HDR variant of
                 * its combiners, on a half-float target (v 1) or an
                 * 8-bit one (v 2: the small 2D targets, rt_format). */
                if (!want_hdr)
                    continue;
                if (v == 1)
                    dk.depth |= 2u;
                if (dk.fs != PC_FS_BASIC)
                    dk.fs = fs_kid_hdr_of(dk.fs);
                else if (v == 2)
                    continue;           /* fs_basic on 8-bit: the SDR record itself */
            }
            memset(&k, 0, sizeof k);
            pthread_mutex_lock(&s_pc_mu);
            k.vs = (uint64_t)(uintptr_t)module_by_kid(dk.vs, 0);
            k.fs = (uint64_t)(uintptr_t)module_by_kid(dk.fs, 1);
            k.nf4 = dk.nf4;
            k.topo = dk.topo;
            k.depth = dk.depth;
            k.wmask = dk.wmask;
            memcpy(k.blend, dk.blend, sizeof k.blend);
            found = 1;
            full = 0;
            if (k.vs && k.fs && dk.nf4 <= NV2A_VS_MAX_INPUTS) {
                (void)pso_slot(&k, &found);
                full = s_pso_n + 1 >= PSO_CAP * 3 / 4;
            }
            pthread_mutex_unlock(&s_pc_mu);
            if (found || full || !(p = pipeline_new(&k, 1)))
                continue;
            pthread_mutex_lock(&s_pc_mu);
            at = pso_slot(&k, &found);
            if (!found) {
                s_pso[at].k = k;
                s_pso[at].p = p;
                s_pso[at].used = 1;
                s_pso_n++;
                __atomic_fetch_add(&J->nb[PC_PSO], 1, __ATOMIC_RELAXED);
            }
            pthread_mutex_unlock(&s_pc_mu);
            if (found)                  /* the ack thread was first */
                vkDestroyPipeline(s_dev, p, NULL);
        }
    }
out:
    if (handled && J->report) {
        uint32_t dn = __atomic_add_fetch(&J->done, 1, __ATOMIC_RELAXED);
        nv2a_pcache_stat_hold(dn > J->total ? J->total : dn, J->total, J->cold);
    }
}

/* One of the threads of a pass: takes records until none are left. */
static void *pc_worker(void *arg)
{
    PcJob *J = (PcJob *)arg;
    NV2AVshProgram *parsed = (NV2AVshProgram *)malloc(sizeof *parsed);
    char *glsl = (char *)malloc(65536);
    if (parsed && glsl) {
        size_t i;
        while ((i = __atomic_fetch_add(&J->next, 1, __ATOMIC_RELAXED)) < J->n
               && !__atomic_load_n(&s_pc_stop, __ATOMIC_ACQUIRE))
            pc_warm_item(J, i, parsed, glsl);
    }
    free(parsed);
    free(glsl);
    return NULL;
}

/* A pass over the list on `threads` threads (this one is one of them). */
static void pc_run_pass(PcJob *J, int threads)
{
    pthread_t th[8];
    int t, started = 0;
    J->next = 0;
    if (threads > 8)
        threads = 8;
    for (t = 1; t < threads; t++)
        if (pthread_create(&th[started], NULL, pc_worker, J) == 0)
            started++;
    pc_worker(J);
    for (t = 0; t < started; t++)
        pthread_join(th[t], NULL);
}

static int pc_thread_count(void)
{
    long e = recomp_env_int(RENV_PCACHE_THREADS, 0);
    int n = nv2a_pcache_cpu_count() - 2;
    if (e > 0)
        return e > 8 ? 8 : (int)e;
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

static void *pc_prewarm(void *arg)
{
    (void)arg;
    do {
        nv2a_pcache *pc;
        size_t n, i, nneed;
        uint64_t t0 = now_ns(), nb[3] = {0, 0, 0}, *need;
        uint32_t listed[3] = {0, 0, 0};
        int pass, hdr, round, rounds;
        PcJob J;

        __atomic_store_n(&s_pc_rescan, 0, __ATOMIC_RELEASE);
        /* The list as it is on disk now (a pre-build may have appended to it
         * since the last pass). The ack thread's puts wait for the reopen. */
        pthread_mutex_lock(&s_pcf_mu);
        if (s_pcf) {
            char msg[256];
            nv2a_pcache_close(s_pcf);
            s_pcf = nv2a_pcache_open(s_pc_path, s_pc_ver, msg, sizeof msg);
        }
        pc = s_pcf;
        pthread_mutex_unlock(&s_pcf_mu);
        nvk_glsl_reload();
        n = nv2a_pcache_count(pc);
        for (i = 0; i < n; i++) {
            const void *d;
            uint32_t type, len;
            if (nv2a_pcache_get(pc, i, &type, &d, &len) && type <= PC_FS_HDR)
                listed[type == PC_FS_HDR ? PC_FS : type]++;
        }
        nv2a_pcache_stat_open(s_pc_path, listed);
        /* The shader ids the list's pipelines name. */
        need = (uint64_t *)malloc((listed[PC_PSO] ? listed[PC_PSO] : 1) * 2 * sizeof *need);
        nneed = 0;
        for (i = 0; need && i < n; i++) {
            const void *d;
            uint32_t type, len;
            if (nv2a_pcache_get(pc, i, &type, &d, &len) && type == PC_PSO
                    && len == sizeof(PsoDisk) && nneed + 2 <= (size_t)listed[PC_PSO] * 2) {
                memcpy(&need[nneed], d, 16);            /* PsoDisk's vs, fs */
                nneed += 2;
            }
        }
        memset(&J, 0, sizeof J);
        J.pc = pc;
        J.n = n;
        J.need = need;
        J.nneed = nneed;
        J.total = listed[0] + listed[1] + listed[2];
        J.cold = s_pc_cold;
        /* BO-110, BO-208: the variants. Round 0: SDR while the targets are
         * SDR; HDR while they are half float (the HDR variant of every SDR
         * key is derived), and beside SDR in a pre-build that was told the
         * display can show HDR (RECOMP_PCACHE_HDR=1). Round 1, when HDR can
         * be reached at all: the other one. */
        hdr = __atomic_load_n(&s_hdr, __ATOMIC_ACQUIRE);
        {
            const char *ph = recomp_env(RENV_PCACHE_HDR);
            J.want_hdr = hdr || (ph && *ph == '1');
            J.want_sdr = !hdr;
        }
        /* Round 1 is decided after round 0: the presenter publishes the
         * display's HDR potential (Windows: BO-207) when its swapchain is
         * made, which can be after this thread started. */
        rounds = need && !(J.want_hdr && J.want_sdr) ? 2 : 1;
        for (round = 0; round < rounds && need; round++) {
            if (round) {
                if (!pc_hdr_reachable()) {
                    rounds = 1;
                    break;
                }
                nv2a_pcache_lower_priority();
                J.want_sdr = hdr;
                J.want_hdr = !hdr;
                J.report = 0;
                memset(J.nb, 0, sizeof J.nb);
                fprintf(stderr, LOGP "pcache: building the %s variants in the background"
                        " at %.2f s\n", hdr ? "SDR" : "HDR", (double)(now_ns() - s_t0_ns) / 1e9);
            } else {
                J.report = __atomic_load_n(&s_pc_warm_urgent, __ATOMIC_ACQUIRE);
            }
            for (pass = 0; pass < 3 && !__atomic_load_n(&s_pc_stop, __ATOMIC_ACQUIRE); pass++) {
                if (pass == 2 && !round)    /* the pipelines are in: nobody waits now */
                    __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
                J.pass = pass;
                pc_run_pass(&J, round ? 1 : pc_thread_count());
            }
            for (i = 0; i < 3; i++)
                nb[i] += J.nb[i];
            if (!round && J.report)
                nv2a_pcache_stat_hold(J.total, J.total, J.cold);
            if (!round) {
                nv2a_pcache_stat_warm_ms((uint32_t)((now_ns() - t0) / 1000000ull));
                fprintf(stderr, LOGP "pcache: pre-warmed %llu vs, %llu fs, %llu pipelines of %u/%u/%u"
                        " on disk in %.0f ms on %d threads (done at %.2f s)\n",
                        (unsigned long long)nb[PC_VS], (unsigned long long)nb[PC_FS],
                        (unsigned long long)nb[PC_PSO], listed[0], listed[1], listed[2],
                        (double)(now_ns() - t0) / 1e6, pc_thread_count(),
                        (double)(now_ns() - s_t0_ns) / 1e9);
                __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
                /* The title's variant is in: the pipeline cache goes to disk
                 * now, not after the background round. */
                __atomic_store_n(&s_pc_save, 1, __ATOMIC_RELEASE);
            }
        }
        free(need);
        for (i = 0; i < 3; i++)
            s_pc_warm[i] += nb[i];
        s_pc_warm_ns += now_ns() - t0;
        if (rounds > 1)
            fprintf(stderr, LOGP "pcache: both variants built: %llu vs, %llu fs, %llu pipelines"
                    " in all (done at %.2f s)\n", (unsigned long long)nb[PC_VS],
                    (unsigned long long)nb[PC_FS], (unsigned long long)nb[PC_PSO],
                    (double)(now_ns() - s_t0_ns) / 1e9);
    } while (__atomic_load_n(&s_pc_rescan, __ATOMIC_ACQUIRE)
             && !__atomic_load_n(&s_pc_stop, __ATOMIC_ACQUIRE));
    __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
    nv2a_pcache_stat_warming(0);
    __atomic_store_n(&s_pc_warming, 0, __ATOMIC_RELEASE);
    /* What a pre-build made is on disk for the next start. */
    __atomic_store_n(&s_pc_save, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Starts the pre-warm thread, or has the one running go round again.
 * `first`: the one the backend starts with, whose pipelines the ack thread
 * may wait for (pc_wait_more): 3 s, or on a cold start (the manifest brought
 * keys, or the driver's cache was empty) as long as the build takes, to a
 * cap. */
static void pc_warm_begin(int first)
{
    pthread_t th;
    __atomic_store_n(&s_pc_rescan, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&s_pc_warming, 1, __ATOMIC_ACQ_REL))
        return;
    nv2a_pcache_stat_warming(1);
    if (first) {
        s_pc_hold_t0 = now_ns();
        __atomic_store_n(&s_pc_wait_until, now_ns() + s_pc_hold_ns, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pc_warm_urgent, 1, __ATOMIC_RELEASE);
    }
    if (pthread_create(&th, NULL, pc_prewarm, NULL) == 0) {
        pthread_detach(th);
    } else {
        __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
        nv2a_pcache_stat_warming(0);
        __atomic_store_n(&s_pc_warming, 0, __ATOMIC_RELEASE);
    }
}

/* nv2a_pcache_rescan: the list gained keys (a pre-build in another process). */
static void pc_warm_start(void)
{
    pc_warm_begin(0);
}

static void pc_start(void)
{
    const char *tr = recomp_env(RENV_PCACHE_TRACE);
    char dir[1024], msg[256];

    if (tr) {
        s_pc_trace = 1;
        if (strcmp(tr, "1") != 0) {
            if ((s_pc_csv = fopen(tr, "w")) != NULL)
                fprintf(s_pc_csv, "flip,t_ms,interval_ms,ack_compiles,ack_compile_ms\n");
            else
                XR_ERROR( LOGP "pcache: cannot write %s\n", tr);
        }
    }
    if (!nvk_cache_dir(dir, sizeof dir)) {
        fprintf(stderr, LOGP "pcache: no disk cache (render.shader_cache)\n");
        return;
    }
    snprintf(s_pc_path, sizeof s_pc_path, "%s/vk_pipelines.bin", dir);
    /* What the keys mean, not the build (nv2a_pcache.h): a rebuild keeps
     * the list. Raise the number when a key changes meaning at its size. */
    snprintf(s_pc_ver, sizeof s_pc_ver, "vk pipelines 2; PsoDisk %zu; NV2ACombinerState %zu;"
             " vsh slots %d", sizeof(PsoDisk), sizeof(NV2ACombinerState),
             (int)NV2A_VSH_SLOTS);
    s_pcf = nv2a_pcache_open(s_pc_path, s_pc_ver, msg, sizeof msg);
    fprintf(stderr, LOGP "pcache: %s: %s\n", s_pc_path, msg);
    if (!s_pcf)
        return;
    {
        uint32_t none[3] = {0, 0, 0};
        nv2a_pcache_stat_open(s_pc_path, none);
    }
    {
        char gp[1100], gv[300], gm[128];
        snprintf(gp, sizeof gp, "%s/gaps_vulkan.bin", dir);
        snprintf(gv, sizeof gv, "%s; gaps", s_pc_ver);
        s_gapf = nv2a_pcache_open(gp, gv, gm, sizeof gm);
    }
    nv2a_pcache_set_rescan(pc_warm_start);
    nv2a_pcache_set_seed(pc_seed);
    {
        /* BO-208: the shipped manifest's keys join the list; a first build
         * (it brought keys, or the driver's cache is empty) holds the title
         * for its pipelines behind "Preparing graphics N/M". */
        nv2a_pcache_manifest_stat ms;
        uint32_t added = nv2a_pcache_manifest_apply(s_pcf, "vulkan", s_pc_ver, &ms);
        long hold = recomp_env_int(RENV_PCACHE_HOLD_S, 180);
        s_pc_cap_ns = (uint64_t)(hold < 3 ? 3 : hold) * 1000000000ull;
        s_pc_cold = (added > 0 || s_blob_cold) && (added > 64 || nv2a_pcache_count(s_pcf) > 64);
        s_pc_hold_ns = s_pc_cold ? (uint64_t)(hold < 3 ? 3 : hold) * 1000000000ull
                                 : 3000000000ull;
        fprintf(stderr, LOGP "pcache: %s start (%u keys new from the manifest, driver cache %s),"
                " the title may be held up to %llu s\n", s_pc_cold ? "cold" : "warm", added,
                s_blob_cold ? "empty" : "kept", (unsigned long long)(s_pc_hold_ns / 1000000000ull));
    }
    if (nv2a_pcache_count(s_pcf) || s_pc_cold)
        pc_warm_begin(1);
}

/* At every flip: the CSV line. */
static void pc_flip(uint64_t flip)
{
    static uint64_t last;
    uint64_t t, n, ns;
    if (!s_pc_csv)
        return;
    t = now_ns();
    pthread_mutex_lock(&s_pc_mu);
    n = s_cs.rt_flip_n;
    ns = s_cs.rt_flip_ns;
    s_cs.rt_flip_n = s_cs.rt_flip_ns = 0;
    pthread_mutex_unlock(&s_pc_mu);
    fprintf(s_pc_csv, "%llu,%.3f,%.3f,%llu,%.3f\n", (unsigned long long)flip,
            (double)(t - s_t0_ns) / 1e6, last ? (double)(t - last) / 1e6 : 0.0,
            (unsigned long long)n, (double)ns / 1e6);
    if ((flip % 60) == 0)
        fflush(s_pc_csv);
    last = t;
}

/* The present log's pipeline-cache line (the Metal backend's, field for field). */
static void pc_log(uint64_t presents)
{
    int i, nv = 0;
    pthread_mutex_lock(&s_pc_mu);
    for (i = 0; i < VS_MAX; i++)
        nv += s_vs[i].used;
    fprintf(stderr, LOGP "pcache at present #%llu: cached %d vs, %d fs, %u pipelines;"
            " ack-thread compiles %llu vs + %llu fs + %llu pipelines in %.1f ms (max %.1f,"
            " %llu in the first 10 s); pre-warm %llu + %llu + %llu in %.1f ms (%s);"
            " start-up waits %llu, %.1f ms\n",
            (unsigned long long)presents, nv, s_fs_n, s_pso_n,
            (unsigned long long)s_cs.n[0][PC_VS], (unsigned long long)s_cs.n[0][PC_FS],
            (unsigned long long)s_cs.n[0][PC_PSO],
            (double)(s_cs.ns[0][0] + s_cs.ns[0][1] + s_cs.ns[0][2]) / 1e6, s_cs.rt_max_ms,
            (unsigned long long)s_cs.rt_10s,
            (unsigned long long)s_cs.n[1][PC_VS], (unsigned long long)s_cs.n[1][PC_FS],
            (unsigned long long)s_cs.n[1][PC_PSO],
            (double)(s_cs.ns[1][0] + s_cs.ns[1][1] + s_cs.ns[1][2]) / 1e6,
            __atomic_load_n(&s_pc_warming, __ATOMIC_ACQUIRE) ? "running" : "done",
            (unsigned long long)s_pc_waits, (double)s_pc_wait_ns / 1e6);
    pthread_mutex_unlock(&s_pc_mu);
}

/* ---- init ---------------------------------------------------------------------- */

static int init(void)
{
    static int tried;
    char err[256] = "";
    VkSemaphoreTypeCreateInfo st;
    VkSemaphoreCreateInfo sci;
    VkDescriptorSetLayoutBinding b0[3], b1[4];
    VkDescriptorSetLayoutCreateInfo dl;
    VkPipelineLayoutCreateInfo pl;
    int i;

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    {   /* BO-110: HDR asked for (graphics.hdr where the display can, or
         * RECOMP_DEBUG=hdr), A/B and the scene bound for tests. */
        const char *e = recomp_env(RENV_HDR), *a = recomp_env(RENV_HDR_AB),
                   *h = recomp_env(RENV_HDR_HI);
        int dbg = e && *e && strcmp(e, "0") != 0;
        s_hdr_ab = dbg && a ? atoi(a) : 0;
        if (h && atof(h) >= 1.0)
            s_hdr_hi = (float)atof(h);
        s_hdr = dbg || nv2a_host_hdr_active();
        nv2a_texrep_set_hdr(s_hdr);     /* BO-203: the title's HDR pictures */
        if (s_hdr)
            fprintf(stderr, LOGP "HDR (BO-110): half-float targets, scene bound %.1f%s\n",
                    s_hdr_hi, s_hdr_ab ? ", A/B" : "");
    }
    s_win = xbox_FramebufferWindowVulkan();
    C = nvk_core_get();
    if (!C)
        C = nvk_core_init(NULL, 0, NULL, NULL, err, sizeof err);
    if (!C) {
        fprintf(stderr, LOGP "no Vulkan device: %s\n", err);
        return 0;
    }
    if (!s_win && C->surface) {
        /* The window made the device but takes no slots (readback). */
        s_win = 0;
    }
    s_dev = C->dev;
    s_max_dim = C->props.limits.maxImageDimension2D;
    if (s_max_dim > 16384u)
        s_max_dim = 16384u;
    s_ubo_align = C->props.limits.minUniformBufferOffsetAlignment;
    if (s_ubo_align < 16)
        s_ubo_align = 16;
    {
        const char *e = recomp_env(RENV_VK_FRAMES);
        s_frames = (e && atoi(e) == 1) ? 1 : FRAMES_MAX;
    }

    memset(&st, 0, sizeof st);
    st.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    st.initialValue = 0;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &st;
    if (vkCreateSemaphore(s_dev, &sci, NULL, &s_timeline) != VK_SUCCESS) {
        XR_ERROR( LOGP "timeline semaphore failed\n");
        return 0;
    }
    for (i = 0; i < s_frames; i++) {
        VkCommandPoolCreateInfo cp;
        memset(&cp, 0, sizeof cp);
        cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cp.queueFamilyIndex = C->qfam;
        if (!poll_ok()) {
            VkFenceCreateInfo fci;
            memset(&fci, 0, sizeof fci);
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(s_dev, &fci, NULL, &s_fr[i].fence) != VK_SUCCESS)
                s_fr[i].fence = VK_NULL_HANDLE;
        }
        if (vkCreateCommandPool(s_dev, &cp, NULL, &s_fr[i].pool) != VK_SUCCESS) {
            XR_ERROR( LOGP "command pool failed\n");
            return 0;
        }
    }

    /* Rings, per frame: vertices as Metal's (16 MB), indices for the
     * largest batch the walker accepts, uniforms for some thousands of
     * draws, staging for a frame's uploads. */
    s_ring_size[RING_VB] = 16u << 20;
    s_ring_size[RING_IB] = (VkDeviceSize)NV_MAX_INDICES * 3u * 4u;
    s_ring_size[RING_UB] = 16u << 20;
    s_ring_size[RING_UP] = 48u << 20;
    {
        static const VkBufferUsageFlags use[RING_N] = {
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        for (i = 0; i < RING_N; i++)
            if (!nvk_buffer_new(&s_ring[i], s_ring_size[i] * s_frames, use[i],
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                i == RING_UP ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                XR_ERROR( LOGP "ring buffer %d failed\n", i);
                return 0;
            }
    }

    /* Set 0: the three constant blocks, dynamic offsets into the uniform
     * ring. Set 1: four combined image samplers. */
    memset(b0, 0, sizeof b0);
    for (i = 0; i < 3; i++) {
        b0[i].binding = (uint32_t)i;
        b0[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = i == 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    }
    memset(b1, 0, sizeof b1);
    for (i = 0; i < 4; i++) {
        b1[i].binding = (uint32_t)i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = 1;
        b1[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dl, 0, sizeof dl);
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 3;
    dl.pBindings = b0;
    if (vkCreateDescriptorSetLayout(s_dev, &dl, NULL, &s_dsl[0]) != VK_SUCCESS)
        return 0;
    dl.bindingCount = 4;
    dl.pBindings = b1;
    if (vkCreateDescriptorSetLayout(s_dev, &dl, NULL, &s_dsl[1]) != VK_SUCCESS)
        return 0;
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = s_dsl;
    if (vkCreatePipelineLayout(s_dev, &pl, NULL, &s_playout) != VK_SUCCESS)
        return 0;
    {
        VkDescriptorPoolSize ps;
        VkDescriptorPoolCreateInfo pi;
        VkDescriptorSetAllocateInfo ai;
        VkDescriptorBufferInfo bi[3];
        VkWriteDescriptorSet w[3];
        static const uint32_t range[3] = { VS_C_BYTES, sizeof(struct nv2a_vp_consts),
                                           sizeof(VkPsConsts) };
        ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ps.descriptorCount = 3;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s_dev, &pi, NULL, &s_set0_pool) != VK_SUCCESS)
            return 0;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = s_set0_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s_dsl[0];
        if (vkAllocateDescriptorSets(s_dev, &ai, &s_set0) != VK_SUCCESS)
            return 0;
        for (i = 0; i < 3; i++) {
            bi[i].buffer = s_ring[RING_UB].buf;
            bi[i].offset = 0;
            bi[i].range = range[i];
            memset(&w[i], 0, sizeof w[i]);
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_set0;
            w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(s_dev, 3, w, 0, NULL);
    }
    {
        /* Occlusion queries, reset from the host (hostQueryReset). */
        VkPhysicalDeviceFeatures f;
        VkPhysicalDeviceVulkan12Features f12;
        VkPhysicalDeviceFeatures2 f2;
        memset(&f12, 0, sizeof f12);
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        memset(&f2, 0, sizeof f2);
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(C->phys, &f2);
        f = f2.features;
        s_qprecise = f.occlusionQueryPrecise ? 1 : 0;
        if (f12.hostQueryReset) {
            VkQueryPoolCreateInfo qi;
            memset(&qi, 0, sizeof qi);
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_OCCLUSION;
            qi.queryCount = VIS_SLOTS;
            if (vkCreateQueryPool(s_dev, &qi, NULL, &s_vkq) != VK_SUCCESS)
                s_vkq = VK_NULL_HANDLE;
        }
    }
    s_pso = calloc(PSO_CAP, sizeof *s_pso);
    s_fs = (FsEntry *)calloc(FS_MAX, sizeof *s_fs);
    if (!s_pso || !s_fs)
        return 0;
    s_pcache = pcache_load();
    s_vs_pass = module_from_glsl(NVK_STAGE_VERTEX, s_vs_pass_glsl, "vs_pass");
    s_fs_basic = module_from_glsl(NVK_STAGE_FRAGMENT, s_fs_basic_glsl, "fs_basic");
    if (!s_vs_pass || !s_fs_basic) {
        XR_ERROR( LOGP "base shaders failed\n");
        return 0;
    }
    pc_start();
    if (!solid_texture(&s_black, 0xFF000000u) || !solid_texture(&s_magenta, NV2A_TEX_MAGENTA)
            || !solid_texture(&s_clear, 0x00000000u)) {
        XR_ERROR( LOGP "base textures failed\n");
        return 0;
    }
    atexit(at_exit_save);
    fprintf(stderr, LOGP "backend up on %s: %d frames in flight, present %s, write-back %s,"
            " occlusion %s%s, GPU progress %s\n", C->name, s_frames,
            s_win ? "window slots" : C->surface ? "texture readback" : "headless",
            wb_always() ? "every flip" : "on demand",
            s_vkq ? "queries" : "fixed", s_qprecise ? " (precise)" : "",
            poll_ok() ? "polled (timeline semaphore)" : "from a fence per frame (vk_poll=0)");
    s_failed = 0;
#if !defined(__ANDROID__)
    {
        /* BO-245: graphics.texture_upscale where the device writes the
         * formats it needs from compute and blits between them (every
         * desktop driver; Android stays off, no measurement there). */
        VkFormatProperties f8, f16, fb;
        vkGetPhysicalDeviceFormatProperties(C->phys, VK_FORMAT_R8G8B8A8_UNORM, &f8);
        vkGetPhysicalDeviceFormatProperties(C->phys, VK_FORMAT_R16G16B16A16_SFLOAT, &f16);
        vkGetPhysicalDeviceFormatProperties(C->phys, RT_FORMAT, &fb);
        nv2a_texup_set_supported(
            (f8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            && (f8.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)
            && (f16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            && (fb.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
            && (fb.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT));
    }
#endif
    /* graphics.ui_scale switched on in the overlay: the loaded 2D textures
     * are prepared while the title is held (scale_prepare_loaded). */
    xbox_HostPauseIdleFn(scale_prepare_loaded);
    return 1;
}

/* ---- flip ----------------------------------------------------------------------- */

static void flip_log(uint32_t surface_offset, RenderTarget *rt)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_skip, p_cull, p_clears, p_tex, p_up,
                    p_chg, p_re, p_fl, p_occ, p_vis, p_late, p_super, p_sync, p_ovf;
    if (on < 0)
        on = recomp_env(RENV_FLIP_LOG) != NULL;
    if (!on)
        return;
    fprintf(stderr, "[VULKAN] flip %llu batches %llu prog %llu skipped %llu"
            " culled %llu clears %llu tex %llu uploads %llu changed %llu rehash %llu"
            " flushes %llu occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
            " sync %llu us, slot overflow %llu) present 0x%08X walker 0x%08X shown %ux%u\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_draws_culled - p_cull),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_flushes - p_fl),
            (unsigned long long)(s_occ.reports - p_occ),
            (unsigned long long)(s_occ.vis - p_vis), s_occ.npend,
            (unsigned long long)(s_occ.late - p_late),
            (unsigned long long)(s_occ.super - p_super),
            (unsigned long long)(s_occ.sync_us - p_sync),
            (unsigned long long)(s_occ_ovf - p_ovf),
            rt ? rt->offset : 0, surface_offset, rt ? s_shown_w : 0, rt ? s_shown_h : 0);
    p_occ = s_occ.reports; p_vis = s_occ.vis; p_late = s_occ.late;
    p_super = s_occ.super; p_sync = s_occ.sync_us; p_ovf = s_occ_ovf;
    p_draws = s_draws; p_prog = s_draws_prog; p_skip = s_draws_skipped;
    p_cull = s_draws_culled; p_clears = s_clears; p_tex = s_draws_tex;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_fl = s_flushes;
}

/* vk_frame: the file and the timestamp pool, at the first flip. */
static void vf_open(void)
{
    static int tried;
    const char *path;
    if (tried)
        return;
    tried = 1;
    path = recomp_env(RENV_VK_FRAME);
    if (!path || !*path || !strcmp(path, "0") || !strcmp(path, "1"))
        return;
    s_vf_out = fopen(path, "w");
    if (!s_vf_out) {
        fprintf(stderr, LOGP "vk_frame: cannot write %s\n", path);
        return;
    }
    if (C->props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = VF_RING * 2 * VF_TS;
        if (vkCreateQueryPool(s_dev, &qi, NULL, &s_vf_qp) != VK_SUCCESS)
            s_vf_qp = VK_NULL_HANDLE;
        else
            vkResetQueryPool(s_dev, s_vf_qp, 0, VF_RING * 2 * VF_TS);
    }
    fprintf(s_vf_out, "present,flip,t_ms,draws,passes,p_clear,p_load,p_resumed,p_empty,"
            "p_maxdraws,rt_switch,zload,zresumed,s_flip,s_ready,s_report,s_poll,s_ring,"
            "s_sampler,s_readback,s_batch,submit_ms,submit_max_ms,qlock_ms,polls,poll_ms,"
            "poll_max_ms,waits,wait_ms,qres,qres_ms,endpass_ms,f_total,f_back,f_copy,"
            "f_submit,f_pub,f_adv,ready_done,g_n,g_first_ms,g_last_ms,g_active_ms,"
            "d_main,d_square,d_other\n");
    memset(s_vfr, 0, sizeof s_vfr);
    s_vf = &s_vfr[0];
    fprintf(stderr, LOGP "vk_frame: one line per flip into %s (GPU timestamps %s)\n", path,
            s_vf_qp ? "on" : "not available");
}

/* The frame just presented is closed; the one VF_LAG flips back is written
 * with its GPU times, and the next slot is made ready. */
static void vf_flip(void)
{
    static uint64_t g0;
    VkFrameRec *done = s_vf, *w;
    int idx = (int)(s_vf - s_vfr), wi = (idx + VF_RING - VF_LAG) % VF_RING, k;
    int ni = (idx + 1) % VF_RING;

    done->present = s_presents;
    done->flip = nv2a_pb_gpu_state()->flips;
    done->t_ns = now_ns();
    w = &s_vfr[wi];
    if (w->t_ns) {
        uint64_t ts[2 * VF_TS][2];
        double per = (double)C->props.limits.timestampPeriod * 1e-6, first = 0, last = 0,
               act = 0;
        uint32_t n = 0;
        memset(ts, 0, sizeof ts);
        if (s_vf_qp && w->nts
                && vkGetQueryPoolResults(s_dev, s_vf_qp, (uint32_t)(wi * 2 * VF_TS), 2 * w->nts,
                                         sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT
                                         | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) >= 0) {
            for (k = 0; k < (int)w->nts; k++) {
                if (!ts[2 * k][1] || !ts[2 * k + 1][1])
                    continue;
                if (!g0)
                    g0 = ts[2 * k][0];
                if (!n)
                    first = (double)(int64_t)(ts[2 * k][0] - g0) * per;
                last = (double)(int64_t)(ts[2 * k + 1][0] - g0) * per;
                act += (double)(int64_t)(ts[2 * k + 1][0] - ts[2 * k][0]) * per;
                n++;
            }
        }
        fprintf(s_vf_out, "%llu,%u,%.3f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                "%.3f,%.3f,%.3f,%u,%.3f,%.3f,%u,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                "%.3f,%u,%u,%.3f,%.3f,%.3f,%u,%u,%u\n",
                (unsigned long long)w->present, w->flip, w->t_ns / 1e6, w->draws, w->passes,
                w->p_clear, w->p_load, w->p_resumed, w->p_empty, w->p_maxdraws, w->rt_switch,
                w->zload, w->zresumed, w->sub[FL_FLIP], w->sub[FL_READY], w->sub[FL_REPORT],
                w->sub[FL_POLL], w->sub[FL_RING], w->sub[FL_SAMPLER], w->sub[FL_READBACK],
                w->sub[FL_BATCH],
                w->submit_ns / 1e6, w->submit_max_ns / 1e6, w->qlock_ns / 1e6, w->polls,
                w->poll_ns / 1e6, w->poll_max_ns / 1e6, w->waits, w->wait_ns / 1e6, w->qres,
                w->qres_ns / 1e6, w->endpass_ns / 1e6, w->f_total / 1e6, w->f_back / 1e6,
                w->f_copy / 1e6, w->f_submit / 1e6, w->f_pub / 1e6, w->f_adv / 1e6,
                w->ready_done, n, first, last, act, w->d_main, w->d_square, w->d_other);
        if ((w->present & 63) == 0)
            fflush(s_vf_out);
    }
    s_vf = &s_vfr[ni];
    memset(s_vf, 0, sizeof *s_vf);
    if (s_vf_qp)
        vkResetQueryPool(s_dev, s_vf_qp, (uint32_t)(ni * 2 * VF_TS), 2 * VF_TS);
    s_vf_last_rt = NULL;
}

/* BO-110: at every flip the stream tag goes back to UI (a frame without the
 * title's scene marks -- menus, movies, loading -- is all at the HUD's
 * white); HDR follows the setting (or hdr_ab): every target is dropped
 * (written back first when the guest may read it) and made again in the
 * other format from the next draw on, as at a start. */
static int s_hdr_said_switch;   /* log the present after a switch */

static void hdr_flip(void)
{
    uint32_t flip = nv2a_pb_gpu_state()->flips;
    int want, i, nkeep = 0;
    static int dbg = -1;
    static RenderTarget keep[RT_MAX];

    nv2a_pb_stream_tag_set(0);
    if (dbg < 0) {
        const char *e = recomp_env(RENV_HDR);
        dbg = e && *e && strcmp(e, "0") != 0;
    }
    if ((flip % 600u) == 0 && s_hdr)
        fprintf(stderr, LOGP "HDR: flip %u; draws %llu UI-bound, %llu scene-bound (%llu with a"
                " texture's gain), %llu flip maps\n",
                flip, (unsigned long long)s_hdr_draws[0], (unsigned long long)s_hdr_draws[1],
                (unsigned long long)s_hdr_gain_draws, (unsigned long long)s_hdr_maps);
    want = s_hdr_ab ? ((flip / (uint32_t)s_hdr_ab) & 1u) == 0 : (dbg || nv2a_host_hdr_active());
    nv2a_texrep_flip(flip);             /* BO-203, hdr_image_ab */
    if (want == s_hdr)
        return;
    end_encoder();
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img)
            continue;
        if (rt->presented)
            keep[nkeep++] = *rt;
        rt_free(rt);
    }
    for (i = 0; i < CUBE_MAX; i++) {
        if (s_cube[i].im.img)
            garbage_push(&s_cube[i].im, VK_NULL_HANDLE, NULL);
        memset(&s_cube[i], 0, sizeof s_cube[i]);
    }
    if (s_self.img) {
        garbage_push(&s_self, s_self_gb, NULL);
        memset(&s_self, 0, sizeof s_self);
        s_self_gb = VK_NULL_HANDLE;
    }
    __atomic_store_n(&s_hdr, want, __ATOMIC_RELEASE);
    nv2a_texrep_set_hdr(want);          /* BO-203: textures with an HDR picture upload again */
    s_dyn_valid = 0;
    s_tset_last = VK_NULL_HANDLE;
    /* The targets a flip has shown come back at once, in the new format,
     * at their old size, from the bytes rt_free wrote back. Left to the next
     * draw, one is made at that draw's clip: Burnout 3's loading and menu
     * screens draw 640x448+16, so a 640x464 target that present_target never
     * takes for the 640x480 frame, and every flip went out as guest bytes
     * (the CPU path, stale pixels, 15 fps under HDR) until a full clear. */
    for (i = 0; i < nkeep; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt_make(rt, keep[i].offset, keep[i].w, keep[i].h, keep[i].pitch, keep[i].swz))
            rt->presented = 1;
    }
    s_hdr_said_switch = !s_hdr_ab || flip < 4000u;
    if (s_pcf) {
        /* The key list's shaders and pipelines in the other variant, in the
         * background; switching on, the draws wait for its pipelines as at
         * a start (at most 3 s, "Preparing shaders" on the screen; the
         * switch is made in the paused settings overlay). */
        nv2a_pcache_stat_hdr(want);
        pc_warm_begin(want);
    }
    if (!s_hdr_ab || flip < 4000u)
        fprintf(stderr, LOGP "HDR%s: flip %u, HDR %s\n", s_hdr_ab ? " A/B" : "", flip,
                want ? "on" : "off");
}

/* RECOMP_GPU_CPU_READS: render targets the title's CPU reads back. Nothing
 * else writes a target to guest memory before it is presented or evicted,
 * so such a read saw whatever was there. Double-S.T.E.A.L 2 reduces each
 * frame to a 16x16 target and reads its luminance for the exposure it puts
 * in the next frame's lighting constants, and its depth to a 64x16 one for
 * the depth of field's focus: unread, the exposure ran to its maximum (the
 * picture went white) and the focus stayed put. Pitch (unswizzled) targets
 * of the listed sizes, drawn since their last write-back, are written back
 * at every flip; the title reads them a frame or more later, after its
 * fence. Each is a GPU wait, so only the listed sizes. */
/* RECOMP_DEBUG=vk_rt_dump=<offset>[,...]@<flip>[,...]: the host pixels of
 * those render targets at those flips (each target at the flip it is
 * presented after), 32-bit BMP with alpha, under vk_rt_dump_dir. What an
 * effect chain left in each of its targets is the other half of a
 * vk_draws trace. */
static void rt_dump_tick(void)
{
    static int n = -1, nf;
    static uint32_t off[32];
    static uint64_t at[16];
    int i, j, hit = 0;

    if (n < 0) {
        const char *e = recomp_env(RENV_VK_RT_DUMP);
        char *x;
        n = nf = 0;
        while (e && *e && *e != '@' && n < 32) {
            off[n++] = (uint32_t)strtoul(e, &x, 16) & 0x07FFFFFFu;
            if (x == e)
                break;
            e = *x == ',' ? x + 1 : x;
        }
        if (e && *e == '@')
            for (e++; *e && nf < 16; ) {
                at[nf++] = strtoull(e, &x, 0);
                if (x == e)
                    break;
                e = *x == ',' ? x + 1 : x;
            }
        if (n)
            fprintf(stderr, LOGP "vk_rt_dump: %d target(s) at %d flip(s)\n", n, nf);
    }
    for (j = 0; j < nf; j++)
        hit |= s_presents == at[j];
    if (!n || !hit)
        return;
    end_encoder();
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        const uint8_t *px;
        for (j = 0; j < n; j++)
            if (rt->im.img && (rt->offset & 0x07FFFFFFu) == off[j])
                break;
        if (j == n || !(px = host_readback(rt)))
            continue;
        {
            const char *dir = recomp_env(RENV_VK_RT_DUMP_DIR);
            char path[512];
            FILE *f;
            uint32_t w = rt->hw, h = rt->hh, y, fs = 54 + w * h * 4;
            uint8_t hdr[54] = { 'B', 'M' };
            snprintf(path, sizeof path, "%s/rt_%08X_%05llu.bmp", dir ? dir : ".",
                     rt->offset & 0x07FFFFFFu, (unsigned long long)s_presents);
            if (!(f = fopen(path, "wb")))
                continue;
            memcpy(hdr + 2, &fs, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            for (y = h; y-- > 0; )
                fwrite(px + (size_t)y * w * 4, 4, w, f);
            fclose(f);
        }
    }
}

static void cpu_reads_sync(void)
{
    static int n = -1;
    static uint32_t size[8][2];
    int i, k;

    if (n < 0) {
        const char *e = recomp_env(RENV_GPU_CPU_READS);
        n = 0;
        while (e && *e && n < 8) {
            char *x;
            size[n][0] = (uint32_t)strtoul(e, &x, 10);
            if (*x != 'x' && *x != 'X')
                break;
            size[n][1] = (uint32_t)strtoul(x + 1, &x, 10);
            if (size[n][0] && size[n][1])
                n++;
            e = *x == ',' ? x + 1 : x;
            if (*x != ',')
                break;
        }
        if (n)
            fprintf(stderr, LOGP "%d render target size%s written back for the CPU at every flip"
                    " (RECOMP_GPU_CPU_READS)\n", n, n == 1 ? "" : "s");
    }
    for (i = 0; i < RT_MAX && n > 0; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || !rt->dirty || rt->swz)
            continue;
        for (k = 0; k < n; k++)
            if (rt->w == size[k][0] && rt->h == size[k][1]) {
                if (!rt_drop_if_stale(rt, "cpu read"))
                    rt_sync(rt, rt->pitch);
                break;
            }
    }
}

/* RECOMP_TRACE=vk_prof: on_flip's cost every 300 flips. */
static struct { uint64_t n, flip, wait, wb, blit, waits; } s_pa;

static void vk_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    static int prof = -1;
    RenderTarget *rt = NULL;
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;
    const uint8_t *hpx = NULL;
    uint64_t t_flip0 = 0, t;
    int always = wb_always(), win = 0, layer = 0;
    uint32_t cw = 0, ch = 0, hcw = 0, hch = 0;

    if (prof < 0)
        prof = recomp_env(RENV_VK_PROF) != NULL;
    if (prof)
        t_flip0 = now_ns();
    if (init()) {
        int wb;
        uint64_t vt0 = 0, vt1 = 0, vt2 = 0, vt3 = 0, vt4 = 0;
        vf_open();
        if (s_vf) {
            /* Is the GPU done with what the frame has submitted so far? */
            vt0 = now_ns();
            s_vf->ready_done = s_done_serial >= s_next_serial - 1;
        }
        cpu_reads_sync();
        rt_dump_tick();
        rt = present_target(surface_offset);
        if (s_hdr_said_switch) {
            /* The first present after an HDR switch: on the GPU path still. */
            s_hdr_said_switch = 0;
            fprintf(stderr, LOGP "HDR: present after the switch: surface 0x%08X (%s)\n",
                    surface_offset, rt ? "drawn here" : "guest bytes");
        }
        if (rt != s_hdr_prt[0]) {
            s_hdr_prt[1] = s_hdr_prt[0];
            s_hdr_prt[0] = rt;
        }
        if (rt) {
            const struct nv2a_pb_present *pp = nv2a_pb_present_state();
            present_extent(rt, &cw, &ch);
            /* What of it the title drew in (present.crop). */
            if (pp->offset == rt->offset && pp->draw_x1 && pp->draw_y1)
                nv2a_present_set_drawn(pp->draw_x0, pp->draw_y0, pp->draw_x1 < cw ? pp->draw_x1 : cw,
                                       pp->draw_y1 < ch ? pp->draw_y1 : ch, cw, ch);
            else
                nv2a_present_set_drawn(0, 0, cw, ch, cw, ch);
            hcw = (uint32_t)((uint64_t)cw * rt->hw / rt->w);
            hch = (uint32_t)((uint64_t)ch * rt->hh / rt->h);
            s_shown_w = cw;
            s_shown_h = ch;
        }
        win = rt && xbox_FramebufferWindowRunning();
        layer = win && s_win;
        wb = rt && addr && pitch && pitch >= rt->w * 2u && always;
        if (layer) {
            /* The frame into the window's back slot on the GPU, in this
             * frame's command buffer; published once submitted, so the
             * main thread's present that shows it comes after it on the
             * queue. */
            struct nvk_image *slot;
            t = prof ? now_ns() : 0;
            if (s_vf)
                vt1 = now_ns();
            slot = (struct nvk_image *)xbox_FramebufferWindowBackTexture(hcw, hch);
            if (s_vf)
                vt2 = now_ns();
            if (slot && slot->format != (s_hdr ? HDR_SLOT_FORMAT : rt->im.format)) {
                static int said;   /* BO-110: the slots follow at the next present */
                if (said++ < 4)
                    fprintf(stderr, LOGP "HDR: window slot format differs; frame not shown\n");
                slot = NULL;
            }
            if (slot && s_hdr) {
                /* BO-110: mapped for the display into the 10-bit slot. */
                if (hdr_flip_pass(rt, slot, hcw, hch)) {
                    if (s_vf)
                        vt3 = now_ns();
                    s_flush_fence = !(rt && wb);
                    flush(0, FL_FLIP);
                    if (s_vf)
                        vt4 = now_ns();
                    xbox_FramebufferWindowPublishTexture();
                    if (s_vf) {
                        s_vf->f_back = vt2 - vt1;
                        s_vf->f_copy = vt3 - vt2;
                        s_vf->f_submit = vt4 - vt3;
                        s_vf->f_pub = now_ns() - vt4;
                    }
                }
                slot = NULL;
            }
            if (slot) {
                VkImageCopy ic;
                end_encoder();
                ensure_cmd();
                nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                /* GENERAL to GENERAL: after the last present that read it. */
                nvk_image_barrier(s_cmd, slot->img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  slot->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                  ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_GENERAL);
                slot->layout = VK_IMAGE_LAYOUT_GENERAL;
                memset(&ic, 0, sizeof ic);
                ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.srcSubresource.layerCount = 1;
                ic.dstSubresource = ic.srcSubresource;
                ic.extent.width = hcw;
                ic.extent.height = hch;
                ic.extent.depth = 1;
                vkCmdCopyImage(s_cmd, rt->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                if (s_vf)
                    vt3 = now_ns();
                /* The frame's last submit, unless a readback follows. */
                s_flush_fence = !(rt && wb);
                flush(0, FL_FLIP);
                if (s_vf)
                    vt4 = now_ns();
                xbox_FramebufferWindowPublishTexture();
                if (s_vf) {
                    s_vf->f_back = vt2 - vt1;
                    s_vf->f_copy = vt3 - vt2;
                    s_vf->f_submit = vt4 - vt3;
                    s_vf->f_pub = now_ns() - vt4;
                }
            }
            if (prof)
                s_pa.blit += now_ns() - t;
        }
        if (rt && (wb || (win && !layer)))
            hpx = host_readback(rt);
        if (wb && hpx) {
            t = prof ? now_ns() : 0;
            write_back_px(rt, hpx, addr, pitch);
            rt->dirty = 0;
            if (addr == rt->addr && pitch == rt->pitch)
                rt_own_reset(rt);
            if (prof)
                s_pa.wb += now_ns() - t;
        }
        /* BO-110: RECOMP_DEBUG=hdr_dump, at fb_dump_at flips (after the
         * write-back: it reads back through the same buffer). */
        if (rt && hcw && hch)
            hdr_dump(rt, hcw, hch);
        /* Submit the frame and move on: the GPU runs it while the next
         * one is recorded. */
        s_flush_fence = 1;
        flush(0, FL_FLIP);
        t = prof ? now_ns() : 0;
        {
            uint64_t w0 = s_frame_waits;
            uint64_t va = s_vf ? now_ns() : 0;
            frame_advance();
            if (s_vf) {
                uint64_t vn = now_ns();
                s_vf->f_adv = vn - va;
                s_vf->f_total = vn - vt0;
                vf_flip();
            }
            texup_flip();   /* BO-245: chunks behind the frame, swaps budgeted */
            if (prof) {
                s_pa.wait += now_ns() - t;
                s_pa.waits += s_frame_waits - w0;
            }
        }
        if ((s_pso_since_save && (s_presents % 600) == 599)
                || __atomic_exchange_n(&s_pc_save, 0, __ATOMIC_ACQ_REL))
            pcache_save_async();
    }
    if (!s_failed) {
        hdr_flip();
        hdr_gains_flip(nv2a_pb_gpu_state()->flips);
    }
    if (s_pc_trace)
        tex_upload_log();
    s_presents++;
#if defined(__ANDROID__)
    if (s_presents == 120 && !s_failed) {
        /* A driver on trial (the app's GpuDriver) has drawn two seconds of
         * frames: it stays. */
        const char *trial = getenv("RECOMP_ANDROID_DRIVER_TRIAL");
        if (trial && *trial && remove(trial) == 0)
            fprintf(stderr, LOGP "the chosen driver has shown its first frames: kept\n");
    }
#endif
    pc_flip(s_presents);
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0)
        pc_log(s_presents);
    if (s_presents == 1 || (s_presents % 300) == 0) {
        uint64_t sh = 0, sc = 0;
        nvk_glsl_stats(&sh, &sc);
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (%s), %llu clears,"
                " %llu draws (%llu vertex-program), %llu skipped, %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs (%s),"
                " %llu decode syncs, %llu stale drops, %llu grows, %llu partial clears;"
                " shaders %llu vs + %llu fs (SPIR-V: %llu cached, %llu compiled),"
                " %llu pipelines, %llu compile errors;"
                " %llu z-clears, %llu depth draws, %llu rt binds (%llu self copies, %llu depth as colour);"
                " %llu submits, %llu at GET_REPORT, %llu frame waits (%.2f ms avg)\n",
                (unsigned long long)s_presents, surface_offset,
                rt ? "drawn here" : "guest bytes",
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_prog, (unsigned long long)s_draws_skipped,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads, (unsigned long long)s_tex_rehash,
                (unsigned long long)s_writebacks, always ? "always" : "lazy",
                (unsigned long long)s_decode_syncs, (unsigned long long)s_stale_drops,
                (unsigned long long)s_grows, (unsigned long long)s_partial_clears,
                (unsigned long long)s_vs_compiled, (unsigned long long)s_fs_compiled,
                (unsigned long long)sh, (unsigned long long)sc,
                (unsigned long long)s_pso_created, (unsigned long long)s_compile_errors,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies,
                (unsigned long long)s_zcolor_binds,
                (unsigned long long)s_flushes, (unsigned long long)s_report_commits,
                (unsigned long long)s_frame_waits,
                s_frame_waits ? (double)s_frame_wait_ns / (double)s_frame_waits / 1e6 : 0.0);
        if (s_draws_skipped) {
            char why[512];
            int i, o = 0;
            for (i = 0; i < 16 && s_skip_why[i] && o < (int)sizeof why - 64; i++)
                o += snprintf(why + o, sizeof why - (size_t)o, "%s%s %llu", i ? ", " : "",
                              s_skip_why[i], (unsigned long long)s_skip_n[i]);
            fprintf(stderr, LOGP "  skipped draws: %s\n", why);
        }
    }
    if (prof) {
        s_pa.flip += now_ns() - t_flip0;
        if (++s_pa.n == 300) {
            double n = (double)s_pa.n;
            fprintf(stderr, LOGP "prof %llu flips: on_flip %.3f ms = frame wait %.3f"
                    " (%llu waits) + write-back %.3f + slot copy %.3f (+rest);"
                    " %llu write-backs, %llu decode syncs total\n",
                    (unsigned long long)s_presents, s_pa.flip / n / 1e6, s_pa.wait / n / 1e6,
                    (unsigned long long)s_pa.waits, s_pa.wb / n / 1e6, s_pa.blit / n / 1e6,
                    (unsigned long long)s_writebacks, (unsigned long long)s_decode_syncs);
            memset(&s_pa, 0, sizeof s_pa);
        }
    }
    if (!pitch || !addr || layer)
        return;
    xbox_FramebufferWindowSet(addr, pitch);
    /* Readback mode (no Vulkan window): the frame's part of the host
     * pixels, read back above. A surface never drawn here goes as the CPU
     * path presents it. */
    if (rt && hpx && win) {
        xbox_FramebufferWindowPresentPixels(hpx, hcw, hch, rt->hw * 4u, 4u);
        return;
    }
    xbox_FramebufferWindowPresent(addr, pitch);
}

/* FLIP_STALL reached: submit what the frame has recorded so the GPU draws it
 * while the executor holds for the vblank (BO-74, as Metal's
 * metal_on_flip_ready); on_flip then adds only the slot copy. */
static void vk_on_flip_ready(void)
{
    if (!s_failed && (s_cmd || s_up))
        flush(0, FL_READY);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_vk_backend = {
    vk_on_clear,
    vk_on_zclear,
    vk_on_draw,
    vk_on_flip,
    vk_on_zpass,
    vk_on_zpass_clear,
    vk_on_report,
    vk_on_poll,
    1,                  /* writes_back: dumps read guest memory, after sync_guest */
    vk_sync_guest,
    vk_on_flip_ready,
};

const struct nv2a_pb_backend *nv2a_pb_vulkan_backend(void)
{
    (void)init();
    return &s_vk_backend;
}

/* RECOMP_PB_BACKEND=vulkan routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_vulkan_register_from_env(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    if (b && (!strcasecmp(b, "vulkan") || !strcasecmp(b, "vk"))) {
        const struct nv2a_pb_backend *vb = nv2a_pb_vulkan_backend();
        if (s_failed) {
            fprintf(stderr, LOGP "RECOMP_PB_BACKEND=vulkan: no device, staying on"
                    " the CPU rasteriser\n");
            return;
        }
        nv2a_pb_set_backend(vb);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=vulkan: executor draws through Vulkan\n");
    }
}

/*
 * nvr_backend.c - the native Vulkan renderer (ANTBOX.md, "Native Vulkan
 * renderer"), RECOMP_PB_BACKEND=native.
 *
 * Slice 2a (draws): vertex programs and pre-transformed vertices through the
 * shared GLSL generators (d3d8_vsh_glsl.c, d3d8_combiners_glsl.c, the
 * old backend's shader ABI: three dynamic uniform buffers in set 0, four
 * texture stages in set 1), pipelines for dynamic rendering keyed on what
 * they fix, vertices gathered into a per-frame ring (as the old backend
 * does, for now), textures decoded on the CPU by the shared decoders
 * (texture replacement and the mip cut-out included, as the old backend)
 * and cached by address and layout, checked once a flip by a hash of their
 * bytes. A texture over a target drawn since its last write-back has the
 * target written back first, unless it can be sampled in place: a texture
 * whose bytes and layout are a target's reads the target's image (a copy of
 * it when that is the target being drawn). Depth
 * and stencil: a buffer per zeta surface and size, attached to its target's
 * passes; a surface with no colour draws into a scratch image.
 *
 * Slice 1: render targets on the GPU, clears, flips and read-back. A target
 * is a BGRA8 image at render.scale, made the first time a surface is drawn
 * or cleared and seeded from the guest bytes under it; a clear covers the
 * box every path uses (nv2a_clear_box); a flip submits the frame's command
 * buffer (two in flight); guest memory and the replay's pictures are read
 * back from the image when asked (sync_guest, nvr_host_pixels). Draws are
 * counted and not drawn yet: they come next, through the shared shader
 * translators and guest memory imported as a Vulkan buffer.
 *
 * Its own files, so takedawn's changes to nv2a_pb_vulkan.c merge as they
 * are; the device is nv2a_vk_core's (one per process, the window's when
 * there is one).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "nv2a_vk_core.h"
#include "nv2a_vk_glsl.h"
#include "d3d8_glsl.h"
#include "d3d8_vsh_parse.h"
#include "d3d8_combiners_parse.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "nvr/nvr_capture.h"
#include "nv2a_texrep.h"
#include "d3d8_swizzle.h"
#include "nv2a_texmip.h"
#include "nv2a_texscale.h"
#include "nvr/nvr_backend.h"
#include "platform/recomp_env.h"

extern int   xbox_FramebufferWindowRunning(void);
extern int   xbox_FramebufferWindowVulkan(void);
extern void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h);
extern void  xbox_FramebufferWindowPublishTexture(void);
extern void  xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void  xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);
extern void  xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                                uint32_t pitch, uint32_t bpp);

#define LOGP "[nvr] "
#define NVR_TARGETS 64
#define NVR_FRAMES  2

ptrdiff_t xbox_GetMemoryOffset(void);

typedef struct {
    uint32_t offset;            /* SET_SURFACE_COLOR_OFFSET as the title set it */
    uint32_t addr;              /* where its bytes are (nv2a_pb_dma_resolve) */
    uint32_t w, h, pitch, bpp;  /* guest size and layout */
    uint32_t scale;             /* render.scale it was made at */
    struct nvk_image im;        /* BGRA8, w * scale x h * scale */
    int      dirty;             /* drawn since guest memory last had it */
    uint64_t last_use;
    struct NvrDepth *dt;        /* the depth buffer its passes attach, or NULL */
    uint64_t gen;               /* the draw that last wrote it */
    int      scratch;           /* a depth-only pass's colour: no guest bytes */
    int      swz;               /* guest memory holds it swizzled */
    uint32_t cfmt;              /* SET_SURFACE_FORMAT's colour field, last drawn as */
    struct nv2a_rt_own own;     /* is the guest memory under it still its own */
    VkImageView opaque, gb;     /* views: alpha read as 1; G8B8 as the console stores it */
} NvrTarget;

typedef struct NvrDepth {
    uint32_t zeta, w, h, zfmt;
    uint64_t gen;               /* the draw that last wrote it */
    VkImageView sview;          /* its depth aspect, for a shadow map's read */
    struct nvk_image im;
    int      fresh;             /* no pass has had it: its first one clears it */
    uint64_t last_use;
} NvrDepth;

typedef struct {
    VkCommandBuffer cb;
    VkFence         fence;
    int             busy;       /* submitted, its fence not yet waited on */
    struct nvk_buffer stage;    /* seeds recorded in it */
    VkDeviceSize    stage_used;
    struct nvk_buffer ring[3];  /* NVR_RING_*: this frame's vertices, indices, constants */
    VkDeviceSize    ring_used[3];
    VkDescriptorSet set0;       /* the constants ring's three dynamic views */
    uint64_t serial;            /* the submit last made from it */
    VkDescriptorPool tpool;     /* this frame's texture sets, reset with it */
    struct nvk_image *garbage;  /* images retired while this frame recorded */
    int ngarbage, garbage_cap;
    VkImageView gviews[64];     /* and views of targets */
    int ngviews;
} NvrFrame;

enum { NVR_RING_VB, NVR_RING_IB, NVR_RING_UB };
static const VkDeviceSize s_ring_size[3] = { 32u << 20, 8u << 20, 8u << 20 };

static const struct nvk_core *C;
static VkDevice      s_dev;
static VkCommandPool s_pool;
static NvrFrame      s_frame[NVR_FRAMES];
static NvrDepth      s_dt[8];
static int           s_slot, s_rec, s_ready, s_failed;
static uint64_t      s_frame_id, s_frame_id_set;   /* frames begun; texture_set's memo's */
static NvrTarget     s_rt[NVR_TARGETS];
static uint64_t      s_tick;
static struct nvk_buffer s_rb;           /* read-back */
static uint64_t      s_clears, s_flips, s_seeds, s_readbacks, s_gen;

static NvrTarget *s_pass_rt;     /* the target the open rendering pass draws into */
static NvrDepth  *s_pass_dt;     /* and the depth buffer it attaches */
static void nvr_sync_target(NvrTarget *t);
static void pass_end(void);
static int pass_begin(NvrTarget *t);
static void retire_image(struct nvk_image *im);
static void view_garbage(VkImageView v);
static void occ_query_begin(void);
static void occ_init(void);
static void occ_pass_end(void);
struct NvrOccQuery;
static struct NvrOccQuery *s_occ_q;  /* the query counting now, or NULL */
static int  s_occ_slot = -1;    /* the visibility query counting on the open pass */
static int  s_report_defer;     /* a report's counts are in this frame's commands */
static uint64_t s_next_serial = 1;   /* the submit being recorded */
static void target_own_reset(NvrTarget *t);
static int draw_init(void);

/* ---- the device and the frame's command buffer ------------------------- */

static int init(void)
{
    char err[256];
    int i;

    if (s_ready || s_failed)
        return s_ready;
    C = nvk_core_get();
    if (!C)
        C = nvk_core_init(NULL, 0, NULL, NULL, err, sizeof err);
    if (!C) {
        fprintf(stderr, LOGP "no Vulkan device: %s\n", err);
        s_failed = 1;
        return 0;
    }
    s_dev = C->dev;
    {
        VkCommandPoolCreateInfo pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = C->qfam;
        if (vkCreateCommandPool(s_dev, &pi, NULL, &s_pool) != VK_SUCCESS) {
            s_failed = 1;
            return 0;
        }
    }
    for (i = 0; i < NVR_FRAMES; i++) {
        VkCommandBufferAllocateInfo ai;
        VkFenceCreateInfo fi;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        {
            VkResult r1 = vkAllocateCommandBuffers(s_dev, &ai, &s_frame[i].cb);
            VkResult r2 = vkCreateFence(s_dev, &fi, NULL, &s_frame[i].fence);
            if (r1 != VK_SUCCESS || r2 != VK_SUCCESS) {
                fprintf(stderr, LOGP "command buffer %s, fence %s\n", nvk_result_str(r1),
                        nvk_result_str(r2));
                s_failed = 1;
                return 0;
            }
        }
    }
    if (!draw_init()) {
        s_failed = 1;
        return 0;
    }
    fprintf(stderr, LOGP "native renderer on %s\n", C->name);
    occ_init();
    s_ready = 1;
    return 1;
}

/* The frame's command buffer, open for recording; waits for the slot's last
 * submit first (two frames in flight). */
static VkCommandBuffer frame_cb(void)
{
    NvrFrame *fr = &s_frame[s_slot];
    VkCommandBufferBeginInfo bi;

    if (s_rec)
        return fr->cb;
    if (fr->busy) {
        int r = nvk_wait_fence(NVK_WAIT_FRAME, "nvr frame", fr->fence, NULL);
        if (r != NVK_WAIT_OK) {
            nvk_wait_fatal(r, VK_SUCCESS, "nvr frame");
            s_failed = 1;
            return VK_NULL_HANDLE;
        }
        fr->busy = 0;
    }
    vkResetFences(s_dev, 1, &fr->fence);
    vkResetCommandBuffer(fr->cb, 0);
    fr->stage_used = 0;
    memset(fr->ring_used, 0, sizeof fr->ring_used);
    /* What this slot retired two frames ago is done with now. */
    while (fr->ngviews > 0)
        vkDestroyImageView(s_dev, fr->gviews[--fr->ngviews], NULL);
    while (fr->ngarbage > 0)
        nvk_image_free(&fr->garbage[--fr->ngarbage]);
    if (fr->tpool)
        vkResetDescriptorPool(s_dev, fr->tpool, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    {
        VkResult r = vkBeginCommandBuffer(fr->cb, &bi);
        if (r != VK_SUCCESS) {
            fprintf(stderr, LOGP "vkBeginCommandBuffer: %s\n", nvk_result_str(r));
            s_failed = 1;
            return VK_NULL_HANDLE;
        }
    }
    s_rec = 1;
    s_frame_id++;
    return fr->cb;
}

/* The frame's commands to the queue; wait: until the GPU has run them. */
static int frame_submit(int wait, const char *what)
{
    NvrFrame *fr = &s_frame[s_slot];
    VkSubmitInfo si;
    VkResult r;

    if (!s_rec) {
        if (wait && s_frame[s_slot ^ 1].busy) {
            /* Nothing recorded: the last submit is what there is to wait for. */
            NvrFrame *pv = &s_frame[s_slot ^ 1];
            if (nvk_wait_fence(NVK_WAIT_SERIAL, what, pv->fence, NULL) != NVK_WAIT_OK)
                return 0;
            pv->busy = 0;
        }
        return 1;
    }
    pass_end();
    vkEndCommandBuffer(fr->cb);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr->cb;
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, fr->fence);
    if (r == VK_SUCCESS)
        nvk_submit_note(what);
    nvk_queue_unlock();
    s_rec = 0;
    s_report_defer = 0;
    fr->serial = s_next_serial++;
    if (r != VK_SUCCESS) {
        fprintf(stderr, LOGP "submit (%s): %s\n", what, nvk_result_str(r));
        s_failed = 1;
        return 0;
    }
    fr->busy = 1;
    if (wait) {
        int w = nvk_wait_fence(NVK_WAIT_SERIAL, what, fr->fence, NULL);
        if (w != NVK_WAIT_OK) {
            nvk_wait_fatal(w, VK_SUCCESS, what);
            s_failed = 1;
            return 0;
        }
        fr->busy = 0;
    }
    s_slot ^= 1;
    return 1;
}

/* n bytes of the frame's staging buffer, mapped; *off its offset. The
 * buffer only grows while no frame recorded in it is in flight. */
static uint8_t *stage(VkDeviceSize n, VkDeviceSize *off)
{
    NvrFrame *fr = &s_frame[s_slot];
    if (fr->stage_used + n > fr->stage.size) {
        VkDeviceSize want = fr->stage.size ? fr->stage.size : (8u << 20);
        if (fr->stage_used) {
            /* In use by commands already recorded: run them first. */
            if (!frame_submit(1, "nvr stage") || !frame_cb())
                return NULL;
            fr = &s_frame[s_slot];
        }
        while (want < n)
            want *= 2;
        if (fr->stage.size < want) {
            nvk_buffer_free(&fr->stage);
            if (!nvk_buffer_new(&fr->stage, want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
                return NULL;
        }
    }
    *off = fr->stage_used;
    fr->stage_used += (n + 15u) & ~(VkDeviceSize)15u;
    return (uint8_t *)fr->stage.map + *off;
}

/* ---- render targets ----------------------------------------------------- */

static uint32_t bpp_of(uint32_t cfmt)
{
    /* SET_SURFACE_FORMAT's colour field: R5G6B5 (3) and the 1555 ones are
     * 16-bit, A8R8G8B8/X8R8G8B8 32-bit. */
    switch (cfmt & 0xF) {
    case 0x1: case 0x2: case 0x3: return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: return 4;
    case 0xA: return 2;         /* G8B8 */
    case 0x9: return 1;         /* B8: no bytes go either way for now */
    default: return 0;
    }
}

/* Guest bytes (w x h, pitch, bpp) into BGRA8 at n x, rows of w*n*4. */
static void guest_to_bgra(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h,
                          uint32_t bpp, unsigned n, uint8_t *dst)
{
    uint32_t x, y;
    if (bpp == 4 && n == 1) {
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * w * 4u, src + (size_t)y * pitch, (size_t)w * 4u);
        return;
    }
    if (bpp == 4) {
        nv2a_upscale_nearest32(src, pitch, w, h, n, dst, w * n * 4u);
        return;
    }
    for (y = 0; y < h * n; y++) {
        const uint16_t *s = (const uint16_t *)(src + (size_t)(y / n) * pitch);
        uint8_t *d = dst + (size_t)y * w * n * 4u;
        for (x = 0; x < w * n; x++) {
            uint16_t c = s[x / n];
            d[x * 4 + 0] = (uint8_t)(((c & 0x1F) * 527u + 23u) >> 6);
            d[x * 4 + 1] = (uint8_t)((((c >> 5) & 0x3F) * 259u + 33u) >> 6);
            d[x * 4 + 2] = (uint8_t)((((c >> 11) & 0x1F) * 527u + 23u) >> 6);
            d[x * 4 + 3] = 0xFF;
        }
    }
}

/* Its guest bytes into the image (at its creation, and when the title wrote
 * them since). */
static void target_seed(NvrTarget *t)
{
    VkCommandBuffer cb;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    VkDeviceSize off;
    uint32_t hw = t->w * t->scale, hh = t->h * t->scale;
    uint8_t *p;
    VkBufferImageCopy r;

    pass_end();
    /* The space first: taking it can submit the frame and open the next. */
    if (!frame_cb() || !(p = stage((VkDeviceSize)hw * hh * 4u, &off)) || !(cb = frame_cb()))
        return;
    if (t->bpp < 2) {
        memset(p, 0, (size_t)hw * hh * 4u);
    } else if (t->swz && !(t->w & (t->w - 1)) && !(t->h & (t->h - 1))) {
        uint8_t *lin = (uint8_t *)malloc((size_t)t->w * t->h * t->bpp);
        if (!lin)
            return;
        xbox_unswizzle_rect(lin, mem + t->addr, t->w, t->h, t->bpp);
        guest_to_bgra(lin, t->w * t->bpp, t->w, t->h, t->bpp, t->scale, p);
        free(lin);
    } else {
        guest_to_bgra(mem + t->addr, t->pitch, t->w, t->h, t->bpp, t->scale, p);
    }
    nvk_image_to(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.bufferOffset = off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = hw;
    r.imageExtent.height = hh;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, s_frame[s_slot].stage.buf, t->im.img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    s_seeds++;
}

static void view_garbage(VkImageView v)
{
    NvrFrame *fr = &s_frame[s_slot];
    if (!v)
        return;
    if (fr->ngviews == (int)(sizeof fr->gviews / sizeof fr->gviews[0])) {
        frame_submit(1, "nvr retire");
        fr = &s_frame[s_slot];
        if (!frame_cb())
            return;
    }
    fr->gviews[fr->ngviews++] = v;
}

static void target_free(NvrTarget *t)
{
    if (s_pass_rt == t)
        pass_end();
    view_garbage(t->opaque);
    view_garbage(t->gb);
    retire_image(&t->im);
    memset(t, 0, sizeof *t);
}

static void target_own_reset(NvrTarget *t)
{
    if (!t->scratch)
        nv2a_rt_own_reset(&t->own, (const uint8_t *)xbox_GetMemoryOffset(), t->addr, t->pitch,
                          t->h, (uint32_t)s_flips);
}

/* The title rewrote the bytes under a target from the CPU: the target is
 * dropped, and what uses those bytes next sees the new ones. */
static int target_stale(NvrTarget *t)
{
    static int said;
    if (t->scratch || !t->im.img || !t->pitch)
        return 0;
    if (!nv2a_rt_own_stale(&t->own, (const uint8_t *)xbox_GetMemoryOffset(), t->addr, t->pitch,
                           t->h, (uint32_t)s_flips))
        return 0;
    if (said++ < 8)
        fprintf(stderr, LOGP "surface 0x%08X %ux%u dropped: the title rewrote its bytes\n",
                t->offset, t->w, t->h);
    target_free(t);
    return 1;
}

/* ---- ghosts ---------------------------------------------------------------
 *
 * A dirty target another target's bytes land on (MA2's glow chain reuses the
 * same memory several times a frame) stays on the GPU: its image is kept, the
 * new target is seeded from it there, and it goes to guest memory only when
 * something reads that range. Oldest first, by the draw that last wrote it. */
#define NVR_GHOSTS 8
static NvrTarget s_ghost[NVR_GHOSTS];
static uint64_t s_ghost_kept, s_ghost_copies, s_ghost_flushes, s_ghost_revived;

static int ranges_meet(const NvrTarget *a, const NvrTarget *b)
{
    return nv2a_rt_overlap(a->addr, a->pitch * a->h, b->addr, b->pitch * b->h);
}

/* The oldest ghost newer than `after`; NULL when none. */
static NvrTarget *ghost_next(uint64_t after)
{
    NvrTarget *best = NULL;
    int i;
    for (i = 0; i < NVR_GHOSTS; i++) {
        NvrTarget *g = &s_ghost[i];
        if (g->im.img && g->gen > after && (!best || g->gen < best->gen))
            best = g;
    }
    return best;
}

static void ghost_flush(NvrTarget *g)
{
    pass_end();
    nvr_sync_target(g);
    target_free(g);
    s_ghost_flushes++;
}

/* What reads guest memory in [lo, hi): the ghosts there, oldest first. */
static void ghost_flush_range(uint32_t lo, uint32_t hi)
{
    uint64_t after = 0;
    NvrTarget *g;
    while ((g = ghost_next(after)) != NULL) {
        uint32_t glo = g->addr & 0x07FFFFFFu;
        after = g->gen;
        if (lo < glo + g->pitch * g->h && glo < hi && !target_stale(g))
            ghost_flush(g);
    }
}

static void ghost_add(NvrTarget *t)
{
    NvrTarget *slot = NULL;
    int i;
    pass_end();
    for (i = 0; i < NVR_GHOSTS && !slot; i++)
        if (!s_ghost[i].im.img)
            slot = &s_ghost[i];
    if (!slot) {
        slot = ghost_next(0);
        ghost_flush(slot);
    }
    *slot = *t;
    memset(t, 0, sizeof *t);
    s_ghost_kept++;
}

/* g's bytes that t's layout also holds, image to image. Rows go one by one
 * where the pitches differ: the same bytes are other pixels then. */
static void ghost_copy(NvrTarget *g, NvrTarget *t, VkCommandBuffer cb)
{
    static VkImageCopy *r;
    static uint32_t cap;
    uint32_t n = 0, y, bpp = t->bpp, sc = t->scale;
    uint32_t glo = g->addr & 0x07FFFFFFu, tlo = t->addr & 0x07FFFFFFu;

    for (y = 0; y < t->h; y++) {
        uint32_t row = tlo + y * t->pitch, end = row + t->w * bpp, b = row;
        while (b < end) {
            uint32_t rel, gy, gx, len;
            if (b < glo) {
                b = glo;
                continue;
            }
            rel = b - glo;
            gy = rel / g->pitch;
            gx = rel % g->pitch;
            if (gy >= g->h)
                break;
            if (gx >= g->w * bpp) {     /* the ghost's gap at a row's end */
                b += g->pitch - gx;
                continue;
            }
            len = g->w * bpp - gx;
            if (len > end - b)
                len = end - b;
            if (n == cap) {
                uint32_t nc = cap ? cap * 2 : 256;
                VkImageCopy *q = (VkImageCopy *)realloc(r, nc * sizeof *r);
                if (!q)
                    return;
                r = q;
                cap = nc;
            }
            memset(&r[n], 0, sizeof r[n]);
            r[n].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r[n].srcSubresource.layerCount = 1;
            r[n].dstSubresource = r[n].srcSubresource;
            r[n].srcOffset.x = (int32_t)(gx / bpp * sc);
            r[n].srcOffset.y = (int32_t)(gy * sc);
            r[n].dstOffset.x = (int32_t)((b - row) / bpp * sc);
            r[n].dstOffset.y = (int32_t)(y * sc);
            r[n].extent.width = len / bpp * sc;
            r[n].extent.height = sc;
            r[n].extent.depth = 1;
            n++;
            b += len;
        }
    }
    if (!n)
        return;
    nvk_image_to(cb, &g->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    nvk_image_to(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cb, g->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->im.img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
    s_ghost_copies++;
}

/* Every row of t inside one ghost's pixels: guest memory has nothing to add. */
static int ghosts_cover(const NvrTarget *t)
{
    uint32_t y, tlo = t->addr & 0x07FFFFFFu;
    for (y = 0; y < t->h; y++) {
        uint32_t row = tlo + y * t->pitch, len = t->w * t->bpp;
        int i, found = 0;
        for (i = 0; i < NVR_GHOSTS && !found; i++) {
            const NvrTarget *g = &s_ghost[i];
            uint32_t glo = g->addr & 0x07FFFFFFu;
            if (g->im.img && row >= glo && (row - glo) / g->pitch < g->h
                    && (row - glo) % g->pitch + len <= g->w * g->bpp)
                found = 1;
        }
        if (!found)
            return 0;
    }
    return 1;
}

/* A new target's contents: guest memory, unless ghosts have every row, with
 * the ghosts over it. `after`: the gen a revived image already has. */
static void target_fill(NvrTarget *t, int revived, uint64_t after)
{
    uint64_t at = 0;
    NvrTarget *g;
    VkCommandBuffer cb;
    int i;

    /* Ghosts this target can't take go to guest memory for its seed. */
    while ((g = ghost_next(at)) != NULL) {
        at = g->gen;
        if (!ranges_meet(g, t) || target_stale(g))
            continue;
        if (g->bpp != t->bpp || g->scale != t->scale || g->swz || t->swz || g->cfmt != t->cfmt)
            ghost_flush(g);
    }
    if (!revived && !ghosts_cover(t))
        target_seed(t);
    at = after;
    while ((g = ghost_next(at)) != NULL) {
        at = g->gen;
        if (!ranges_meet(g, t))
            continue;
        pass_end();
        if (!(cb = frame_cb()))
            return;
        ghost_copy(g, t, cb);
        t->dirty = 1;
    }
    /* Ghosts wholly inside a gapless t: its image has their bytes now. */
    if (t->pitch == t->w * t->bpp)
        for (i = 0; i < NVR_GHOSTS; i++) {
            g = &s_ghost[i];
            if (g->im.img && ranges_meet(g, t) && (g->addr & 0x07FFFFFFu) >= (t->addr & 0x07FFFFFFu)
                    && (g->addr & 0x07FFFFFFu) + g->pitch * g->h
                       <= (t->addr & 0x07FFFFFFu) + t->pitch * t->h)
                target_free(g);
        }
}

/* The ghost that was this very target, back in slot t. */
static int ghost_revive(NvrTarget *t, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t bpp, int swz, uint32_t cfmt, uint32_t n)
{
    uint32_t addr = nv2a_pb_dma_resolve(offset);
    int i;
    for (i = 0; i < NVR_GHOSTS; i++) {
        NvrTarget *g = &s_ghost[i];
        uint64_t gen;
        if (!g->im.img || g->addr != addr || g->w != w || g->h != h || g->pitch != pitch
                || g->bpp != bpp || g->swz != swz || g->cfmt != cfmt || g->scale != n
                || target_stale(g))
            continue;
        pass_end();
        *t = *g;
        memset(g, 0, sizeof *g);
        gen = t->gen;
        t->offset = offset;
        t->last_use = s_tick;
        target_fill(t, 1, gen);
        s_ghost_revived++;
        return 1;
    }
    return 0;
}

/* A new image on a free or least-used slot, seeded from guest memory. */
static NvrTarget *target_make(uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
                              uint32_t bpp, int swz, uint32_t cfmt, uint32_t n)
{
    NvrTarget *t = NULL, *lru = &s_rt[0];
    int i;
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *c = &s_rt[i];
        if (!c->im.img && !t)
            t = c;
        if (c->im.img && lru->im.img && c->last_use < lru->last_use)
            lru = c;
    }
    if (!t) {
        t = lru;
        pass_end();
        if (t->dirty)
            nvr_sync_target(t);
        target_free(t);
    }
    if (ghost_revive(t, offset, w, h, pitch, bpp, swz, cfmt, n))
        return t;
    if (!nvk_image_new(&t->im, VK_FORMAT_B8G8R8A8_UNORM, w * n, h * n, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT)) {
        fprintf(stderr, LOGP "surface 0x%08X %ux%u: no image\n", offset, w, h);
        memset(t, 0, sizeof *t);
        return NULL;
    }
    t->offset = offset;
    t->addr = nv2a_pb_dma_resolve(offset);
    t->w = w;
    t->h = h;
    t->pitch = pitch;
    t->bpp = bpp;
    t->scale = n;
    t->swz = swz;
    t->cfmt = cfmt;
    t->last_use = s_tick;
    target_own_reset(t);
    target_fill(t, 0, 0);
    t->gen = ++s_gen;
    fprintf(stderr, LOGP "surface 0x%08X: %ux%u, %u-bit%s, scale %u\n", offset, w, h, bpp * 8u,
            swz ? ", swizzled" : "", n);
    return t;
}

/* The target the current surface draws into, as the old backend finds it:
 * its offset and size; else its offset and pitch at a larger size (grown when
 * the clip needs more); a new one after whatever overlaps its bytes has been
 * written back and retired. */
static NvrTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    uint32_t bpp = bpp_of(g->format), n = nv2a_host_render_scale(), pitch = g->pitch;
    uint32_t cfmt = g->format & 0xF, addr;
    int swz = ((g->format >> 8) & 0xF) == 2, i;
    NvrTarget *t;

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096 || !bpp)
        return NULL;
    if (n < 1)
        n = 1;
    if (pitch < w * bpp && swz)
        pitch = w * bpp;
    s_tick++;
    for (i = 0; i < NVR_TARGETS; i++) {
        t = &s_rt[i];
        if (t->im.img && !t->scratch && t->offset == g->color_offset && t->w == w && t->h == h
                && t->scale == n) {
            if (target_stale(t))
                break;
            t->last_use = s_tick;
            t->swz = swz;
            t->cfmt = cfmt;
            return t;
        }
    }
    for (i = 0; i < NVR_TARGETS; i++) {
        t = &s_rt[i];
        if (!t->im.img || t->scratch || t->offset != g->color_offset || t->pitch != pitch
                || t->scale != n)
            continue;
        if (target_stale(t))
            break;
        if (t->w >= w && t->h >= h) {
            t->last_use = s_tick;
            t->swz = swz;
            t->cfmt = cfmt;
            return t;
        }
        /* Grown: what it drew goes to guest memory, which seeds the new one. */
        {
            uint32_t nw = t->w > w ? t->w : w, nh = t->h > h ? t->h : h;
            pass_end();
            if (t->dirty)
                nvr_sync_target(t);
            target_free(t);
            return target_make(g->color_offset, nw, nh, pitch, bpp, swz, cfmt, n);
        }
    }
    addr = nv2a_pb_dma_resolve(g->color_offset);
    for (i = 0; i < NVR_TARGETS; i++) {
        t = &s_rt[i];
        if (!t->im.img || t->scratch
                || !nv2a_rt_overlap(t->addr, t->pitch * t->h, addr, pitch * h))
            continue;
        if (target_stale(t))
            continue;
        pass_end();
        if (t->dirty && !t->swz) {
            ghost_add(t);
            continue;
        }
        if (t->dirty)
            nvr_sync_target(t);
        target_free(t);
    }
    return target_make(g->color_offset, w, h, pitch, bpp, swz, cfmt, n);
}

/* The zeta surface's depth buffer for a w x h target: made on first use. */
static NvrDepth *depth_target(uint32_t w, uint32_t h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    uint32_t n = nv2a_host_render_scale();
    NvrDepth *e = NULL, *lru = &s_dt[0];
    int i;

    if (n < 1)
        n = 1;
    for (i = 0; i < (int)(sizeof s_dt / sizeof s_dt[0]); i++) {
        NvrDepth *d = &s_dt[i];
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
        pass_end();
        for (i = 0; i < NVR_TARGETS; i++)
            if (s_rt[i].dt == e)
                s_rt[i].dt = NULL;
        view_garbage(e->sview);
        retire_image(&e->im);
        memset(e, 0, sizeof *e);
    }
    if (!nvk_image_new(&e->im, C->depth_format, w * n, h * n, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        memset(e, 0, sizeof *e);
        return NULL;
    }
    {
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
    e->fresh = 1;
    e->last_use = s_tick;
    fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* A surface with no colour (a shadow map's depth pass): a scratch image to
 * draw into, keyed by the zeta surface. No guest bytes are under it. */
static NvrTarget *surface_depth_only(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    uint32_t key = 0x80000000u | (v->zeta_offset & 0x07FFFFFFu), n = nv2a_host_render_scale();
    NvrTarget *t, *lru = &s_rt[0];
    int i;

    if (g->color_offset || !v->zeta_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    if (n < 1)
        n = 1;
    s_tick++;
    for (i = 0; i < NVR_TARGETS; i++) {
        t = &s_rt[i];
        if (t->im.img && t->scratch && t->offset == key && t->w == w && t->h == h) {
            t->last_use = s_tick;
            return t;
        }
        if (!t->im.img || (lru->im.img && t->last_use < lru->last_use))
            lru = t;
    }
    t = lru;
    if (t->im.img) {
        pass_end();
        if (t->dirty)
            nvr_sync_target(t);
        retire_image(&t->im);
    }
    memset(t, 0, sizeof *t);
    if (!nvk_image_new(&t->im, VK_FORMAT_B8G8R8A8_UNORM, w * n, h * n, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    t->offset = key;
    t->addr = 0;
    t->w = w;
    t->h = h;
    t->pitch = w * 4u;
    t->bpp = 4;
    t->scale = n;
    t->scratch = 1;
    t->last_use = s_tick;
    return t;
}

/* The image's pixels into the read-back buffer (hw x hh BGRA8, rows packed). */
static const uint8_t *readback(NvrTarget *t)
{
    VkCommandBuffer cb;
    VkDeviceSize n = (VkDeviceSize)t->im.w * t->im.h * 4u;
    VkBufferImageCopy r;

    if (s_rb.size < n) {
        nvk_buffer_free(&s_rb);
        if (!nvk_buffer_new(&s_rb, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            return NULL;
    }
    if (!(cb = frame_cb()))
        return NULL;
    pass_end();
    nvk_image_to(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = t->im.w;
    r.imageExtent.height = t->im.h;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb.buf, 1, &r);
    if (!frame_submit(1, "nvr readback"))
        return NULL;
    s_readbacks++;
    return (const uint8_t *)s_rb.map;
}

/* Its pixels into guest memory, as the guest keeps them (box-filtered down
 * from render.scale; 16-bit as R5G6B5). */
static void nvr_sync_target(NvrTarget *t)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + t->addr;
    const uint8_t *px;
    uint32_t y;

    if (t->scratch) {
        t->dirty = 0;
        return;
    }
    if (!t->dirty || !(px = readback(t)))
        return;
    if (t->bpp < 2) {
        t->dirty = 0;
        return;
    }
    {
        /* To guest size in BGRA8, then to the guest's texel and layout. */
        static uint32_t *lin;
        static size_t lin_cap;
        size_t texels = (size_t)t->w * t->h;
        int swizzled = t->swz && !(t->w & (t->w - 1)) && !(t->h & (t->h - 1));
        if (lin_cap < texels) {
            uint32_t *nl = (uint32_t *)realloc(lin, texels * 4u);
            if (!nl)
                return;
            lin = nl;
            lin_cap = texels;
        }
        if (t->scale == 1)
            memcpy(lin, px, texels * 4u);
        else
            nv2a_downscale_box32(px, t->im.w * 4u, t->w, t->h, t->scale, (uint8_t *)lin,
                                 t->w * 4u);
        if (t->bpp == 2) {
            uint16_t *o = (uint16_t *)lin;   /* in place: each texel shrinks */
            size_t k;
            for (k = 0; k < texels; k++) {
                uint32_t c = lin[k];
                o[k] = t->cfmt == 0xA ? (uint16_t)(c & 0xFFFFu)   /* G high, B low */
                     : (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
        if (swizzled) {
            xbox_swizzle_rect(dst, lin, t->w, t->h, t->bpp);
        } else {
            for (y = 0; y < t->h; y++)
                memcpy(dst + (size_t)y * t->pitch, (const uint8_t *)lin + (size_t)y * t->w * t->bpp,
                       (size_t)t->w * t->bpp);
        }
    }
    t->dirty = 0;
    target_own_reset(t);
    nvr_capture_gpu_wrote(t->addr, t->pitch * t->h);
}

/* ---- the backend --------------------------------------------------------- */

static void nvr_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    NvrTarget *t;
    VkCommandBuffer cb;
    uint32_t b[4];
    VkClearAttachment ca;
    VkClearRect cr;
    static int said_mask;

    if (!(param & 0xF0) || !init() || !(t = surface()) || !nv2a_clear_box(g, t->w, t->h, b))
        return;
    if ((param & 0xF0) != 0xF0 && !said_mask++)
        fprintf(stderr, LOGP "clear of some colour channels (0x%02X): all cleared for now\n",
                param & 0xF0);
    if (!(cb = frame_cb()) || !pass_begin(t))
        return;
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.clearValue.color.float32[0] = (float)((g->clear_color >> 16) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[1] = (float)((g->clear_color >> 8) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[2] = (float)(g->clear_color & 0xFF) / 255.0f;
    ca.clearValue.color.float32[3] = (float)(g->clear_color >> 24) / 255.0f;
    memset(&cr, 0, sizeof cr);
    cr.rect.offset.x = (int32_t)(b[0] * t->scale);
    cr.rect.offset.y = (int32_t)(b[1] * t->scale);
    cr.rect.extent.width = (b[2] - b[0]) * t->scale;
    cr.rect.extent.height = (b[3] - b[1]) * t->scale;
    cr.layerCount = 1;
    {   /* A clear is not a zpass sample: the count goes on in a new slot. */
        int counting = s_occ_slot >= 0;
        if (counting)
            occ_pass_end();
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        if (counting)
            occ_query_begin();
    }
    t->dirty = 1;
    t->gen = ++s_gen;
    nv2a_rt_own_drawn(&t->own, (uint32_t)s_flips);
    s_clears++;
}

static void nvr_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    NvrTarget *t;
    NvrDepth *d;
    VkClearAttachment ca;
    VkClearRect cr;

    if (!(param & 3u) || !init())
        return;
    if (!(t = surface()) && !(t = surface_depth_only()))
        return;
    if (!(d = depth_target(t->w, t->h)))
        return;
    t->dt = d;
    if (!frame_cb() || !pass_begin(t))
        return;
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = ((param & 1u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                  | ((param & 2u) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    ca.clearValue.depthStencil.depth = nv2a_zclear_depth(g->format, v->zclear);
    ca.clearValue.depthStencil.stencil = v->zclear & 0xFFu;
    memset(&cr, 0, sizeof cr);
    cr.rect.extent.width = t->im.w < d->im.w ? t->im.w : d->im.w;
    cr.rect.extent.height = t->im.h < d->im.h ? t->im.h : d->im.h;
    cr.layerCount = 1;
    {
        int counting = s_occ_slot >= 0;
        if (counting)
            occ_pass_end();
        vkCmdClearAttachments(s_frame[s_slot].cb, 1, &ca, 1, &cr);
        if (counting)
            occ_query_begin();
    }
}

/* ---- passes --------------------------------------------------------------- */


static void pass_end(void)
{
    if (s_pass_rt) {
        occ_pass_end();
        vkCmdEndRendering(s_frame[s_slot].cb);
        s_pass_rt = NULL;
        s_pass_dt = NULL;
    }
}

static int s_dyn_valid;          /* the dynamic state below is the command buffer's */

/* A rendering pass on t, the open one when it is already t's. */
static int pass_begin(NvrTarget *t)
{
    VkCommandBuffer cb;
    VkRenderingAttachmentInfo att, datt;
    VkRenderingInfo ri;

    if (s_pass_rt == t && s_pass_dt == t->dt)
        return 1;
    pass_end();
    if (!(cb = frame_cb()))
        return 0;
    nvk_image_to(cb, &t->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = t->im.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = t->im.w;
    ri.renderArea.extent.height = t->im.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    if (t->dt) {
        NvrDepth *d = t->dt;
        nvk_image_to(cb, &d->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        memset(&datt, 0, sizeof datt);
        datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        datt.imageView = d->im.view;
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        datt.loadOp = d->fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        datt.clearValue.depthStencil.depth = 1.0f;
        datt.clearValue.depthStencil.stencil = 0;
        d->fresh = 0;
        ri.pDepthAttachment = &datt;
        ri.pStencilAttachment = &datt;
        if (d->im.w < ri.renderArea.extent.width)
            ri.renderArea.extent.width = d->im.w;
        if (d->im.h < ri.renderArea.extent.height)
            ri.renderArea.extent.height = d->im.h;
    }
    vkCmdBeginRendering(cb, &ri);
    s_pass_rt = t;
    s_pass_dt = t->dt;
    s_dyn_valid = 0;
    occ_query_begin();
    return 1;
}

/* ---- the draws' device objects ------------------------------------------- */

/* D3D8_GLSL_PSCONSTS: nv2a_ps_consts, then tex_bias, bump-env and the HDR
 * word, std140 (the old backend's VkPsConsts). */
typedef struct {
    struct nv2a_ps_consts b;
    float tex_bias[4];
    float bump_mat[4][4];
    float bump_lum[4][4];
    float hdr[4];
} NvrPsConsts;
_Static_assert(sizeof(NvrPsConsts) == 576, "D3D8_GLSL_PSCONSTS is 576 bytes");

#define NVR_VS_C_BYTES (NV2A_VS_MAX_CONSTANTS * 16)
#define NVR_VP_C_BYTES ((uint32_t)sizeof(struct nv2a_vp_consts))
#define NVR_PS_C_BYTES ((uint32_t)sizeof(NvrPsConsts))

/* A pass-through draw's vertex: attributes 0, 3, 4, 9..12. */
#define NVR_PASS_F4 7
static const int s_pass_attr[NVR_PASS_F4] = { 0, 3, 4, 9, 10, 11, 12 };

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
    "void main() {\n"
    "    gl_Position = vec4(a_pos.xy * vp.c[0].xy + vp.c[1].xy,\n"
    "                       clamp(a_pos.z * vp.c[0].z + vp.c[0].w, 0.0, 1.0), 1.0);\n"
    "    o_d0 = a_d0; o_d1 = a_d1;\n"
    "    o_t0 = a_t0; o_t1 = a_t1; o_t2 = a_t2; o_t3 = a_t3;\n"
    "    o_fog = 1.0;\n"
    "    o_wz = gl_Position.z;\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";

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

static VkDescriptorSetLayout s_dsl[2];
static VkPipelineLayout s_playout;
static VkDescriptorPool   s_dpool;
static VkDescriptorSet    s_set1_black;   /* four black stages (slice 2a) */
static struct nvk_image   s_black;
static VkSampler          s_smp_point;
static VkShaderModule     s_vs_pass, s_fs_basic;
static VkDeviceSize       s_ub_align = 256;
static uint64_t           s_draws, s_draw_skips[8];
enum { SKIP_NO_IDX, SKIP_NO_VS, SKIP_NO_RT, SKIP_NO_PRIM, SKIP_RING, SKIP_PSO, SKIP_SCISSOR,
       SKIP_MASKED };

static uint32_t fnv32(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static VkShaderModule module_from_glsl(enum nvk_stage stage, const char *src, const char *what)
{
    char err[2048];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;
    static int said;

    if (!nvk_glsl_spirv(stage, src, &w, &n, &compiled, err, sizeof err)) {
        if (said++ < 4)
            fprintf(stderr, LOGP "%s: GLSL compile failed: %s\n", what, err);
        return VK_NULL_HANDLE;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(s_dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int draw_init(void)
{
    VkDescriptorSetLayoutBinding b[4];
    VkDescriptorSetLayoutCreateInfo li;
    VkPipelineLayoutCreateInfo pl;
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi;
    VkDescriptorSetAllocateInfo ai;
    VkSamplerCreateInfo si;
    int i, f;

    s_ub_align = C->props.limits.minUniformBufferOffsetAlignment;
    if (s_ub_align < 16)
        s_ub_align = 16;
    /* Set 0: the vertex program's constants, nv2a_vp_consts, the fragment
     * constants, each a dynamic uniform buffer. Set 1: four stages. */
    memset(b, 0, sizeof b);
    for (i = 0; i < 3; i++) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[i].descriptorCount = 1;
        b[i].stageFlags = i < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_dsl[0]) != VK_SUCCESS)
        return 0;
    memset(b, 0, sizeof b);
    for (i = 0; i < 4; i++) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 4;
    if (vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_dsl[1]) != VK_SUCCESS)
        return 0;
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = s_dsl;
    if (vkCreatePipelineLayout(s_dev, &pl, NULL, &s_playout) != VK_SUCCESS)
        return 0;
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = 3 * NVR_FRAMES;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = 4;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = NVR_FRAMES + 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(s_dev, &pi, NULL, &s_dpool) != VK_SUCCESS)
        return 0;
    /* Each frame's rings, and its set 0 over its constants ring. */
    for (f = 0; f < NVR_FRAMES; f++) {
        VkDescriptorBufferInfo bi[3];
        VkWriteDescriptorSet w;
        static const VkBufferUsageFlags use[3] = {
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
        for (i = 0; i < 3; i++)
            if (!nvk_buffer_new(&s_frame[f].ring[i], s_ring_size[i], use[i],
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return 0;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = s_dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s_dsl[0];
        if (vkAllocateDescriptorSets(s_dev, &ai, &s_frame[f].set0) != VK_SUCCESS)
            return 0;
        {
            VkDescriptorPoolSize tp;
            VkDescriptorPoolCreateInfo tpi;
            tp.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tp.descriptorCount = 4 * 8192;
            memset(&tpi, 0, sizeof tpi);
            tpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            tpi.maxSets = 8192;
            tpi.poolSizeCount = 1;
            tpi.pPoolSizes = &tp;
            if (vkCreateDescriptorPool(s_dev, &tpi, NULL, &s_frame[f].tpool) != VK_SUCCESS)
                return 0;
        }
        for (i = 0; i < 3; i++) {
            bi[i].buffer = s_frame[f].ring[NVR_RING_UB].buf;
            bi[i].offset = 0;
            bi[i].range = i == 0 ? NVR_VS_C_BYTES : i == 1 ? NVR_VP_C_BYTES : NVR_PS_C_BYTES;
        }
        /* One write each: the bindings' stages differ. */
        for (i = 0; i < 3; i++) {
            memset(&w, 0, sizeof w);
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = s_frame[f].set0;
            w.dstBinding = (uint32_t)i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w.pBufferInfo = &bi[i];
            vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
        }
    }
    /* Slice 2a's textures: one black texel for every stage. */
    if (!nvk_image_new(&s_black, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(s_dev, &si, NULL, &s_smp_point) != VK_SUCCESS)
        return 0;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_dsl[1];
    if (vkAllocateDescriptorSets(s_dev, &ai, &s_set1_black) != VK_SUCCESS)
        return 0;
    {
        VkDescriptorImageInfo ii[4];
        VkWriteDescriptorSet w;
        for (i = 0; i < 4; i++) {
            ii[i].sampler = s_smp_point;
            ii[i].imageView = s_black.view;
            ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = s_set1_black;
        w.descriptorCount = 4;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = ii;
        vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    }
    {
        /* The black texel, made readable now (a frame's first command). */
        VkCommandBuffer cb = frame_cb();
        VkClearColorValue zero;
        VkImageSubresourceRange rr;
        if (!cb)
            return 0;
        memset(&zero, 0, sizeof zero);
        /* RECOMP_NVR_WHITE_TEX=1: the stand-in texel white, to see slice 2a's
         * geometry against the old backend's frames. */
        if (getenv("RECOMP_NVR_WHITE_TEX"))
            zero.float32[0] = zero.float32[1] = zero.float32[2] = zero.float32[3] = 1.0f;
        memset(&rr, 0, sizeof rr);
        rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rr.levelCount = 1;
        rr.layerCount = 1;
        nvk_image_to(cb, &s_black, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, s_black.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rr);
        nvk_image_to(cb, &s_black, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    s_vs_pass = module_from_glsl(NVK_STAGE_VERTEX, s_vs_pass_glsl, "vs_pass");
    s_fs_basic = module_from_glsl(NVK_STAGE_FRAGMENT, s_fs_basic_glsl, "fs_basic");
    return s_vs_pass && s_fs_basic;
}

/* n bytes of this frame's ring r, aligned; NULL when it is full (the caller
 * submits the frame and asks again). */
static void *ring_take(int r, VkDeviceSize n, VkDeviceSize align, VkDeviceSize *off)
{
    NvrFrame *fr = &s_frame[s_slot];
    VkDeviceSize o = (fr->ring_used[r] + align - 1) & ~(align - 1);
    if (o + n > fr->ring[r].size)
        return NULL;
    fr->ring_used[r] = o + n;
    *off = o;
    return (uint8_t *)fr->ring[r].map + o;
}

/* ---- shaders --------------------------------------------------------------- */

#define NVR_VS_CACHE 512
static struct { uint32_t hash, len; uint16_t inputs; VkShaderModule m; } s_vs[NVR_VS_CACHE];

/* The vertex program's module, made once per program (by its slots' hash). */
static VkShaderModule vs_program(const struct nv2a_pb_vsh *v, uint16_t *inputs)
{
    static NV2AVshProgram parsed;
    static char glsl[65536];
    uint32_t len = 0, hash = nv2a_vsh_program_hash(v, &len), i, slot;
    char what[32];

    if (!len)
        return VK_NULL_HANDLE;
    slot = hash % NVR_VS_CACHE;
    for (i = 0; i < NVR_VS_CACHE; i++, slot = (slot + 1) % NVR_VS_CACHE) {
        if (s_vs[slot].m && s_vs[slot].hash == hash && s_vs[slot].len == len) {
            *inputs = s_vs[slot].inputs;
            return s_vs[slot].m;
        }
        if (!s_vs[slot].m)
            break;
    }
    if (s_vs[slot].m)
        return VK_NULL_HANDLE;          /* full: not drawn (the old backend's
                                         * cache is the measure of how many) */
    d3d8_vsh_parse(&v->prog[v->start][0], (int)len, &parsed);
    snprintf(what, sizeof what, "program %08X", hash);
    if (d3d8_vsh_generate_glsl(&parsed, D3D8_GLSL_SCREEN_SPACE, glsl, sizeof glsl) <= 0)
        return VK_NULL_HANDLE;
    s_vs[slot].m = module_from_glsl(NVK_STAGE_VERTEX, glsl, what);
    s_vs[slot].hash = hash;
    s_vs[slot].len = len;
    s_vs[slot].inputs = parsed.inputs_read;
    *inputs = parsed.inputs_read;
    return s_vs[slot].m;
}

static struct nv2a_stage s_stage[4];

static void build_stages(void)
{
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int n;
    for (n = 0; n < 4; n++) {
        nv2a_stage_decode(n, &regs[(0x40u * n) / 4], &sets[(0x40u * n) / 4], v->shader_set,
                          v->shader_prog, nv2a_pb_dma_resolve,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR | NV2A_STAGE_MIPS,
                          &s_stage[n]);
        if (s_stage[n].mode == 5)
            s_stage[n].mode = 0;
    }
}

/* A shadow buffer we drew, when the stage reads its bytes as depth. */
static NvrDepth *dt_for_texture(const struct nv2a_stage *t)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int i;

    if (!t->offset || !nv2a_tex_depth_kind(t->color))
        return NULL;
    for (i = 0; i < (int)(sizeof s_dt / sizeof s_dt[0]); i++) {
        NvrDepth *d = &s_dt[i];
        if (!d->im.img || !d->sview || ((d->zeta ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (((d->zeta ^ v->zeta_offset) & 0x07FFFFFFu) == 0
                && (v->depth_enable || v->stencil_enable))
            return NULL;
        return d;
    }
    return NULL;
}

/* A depth buffer read as A8R8G8B8 bytes (Z24S8), unless a colour target over
 * the same bytes was drawn later. */
static NvrDepth *dt_for_zcolor(const struct nv2a_stage *t)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t off = t->offset & 0x07FFFFFFu;
    int i, j;

    if (!off || (t->color != 0x12 && t->color != 0x1E))
        return NULL;
    for (i = 0; i < (int)(sizeof s_dt / sizeof s_dt[0]); i++) {
        NvrDepth *d = &s_dt[i];
        if (!d->im.img || !d->sview || d->zfmt != 2 || (d->zeta & 0x07FFFFFFu) != off
                || (t->pitch && t->pitch != d->w * 4))
            continue;
        if (((d->zeta ^ v->zeta_offset) & 0x07FFFFFFu) == 0
                && (v->depth_enable || v->stencil_enable))
            return NULL;
        for (j = 0; j < NVR_TARGETS; j++)
            if (s_rt[j].im.img && (s_rt[j].offset & 0x07FFFFFFu) == off && s_rt[j].gen > d->gen)
                return NULL;
        return d;
    }
    return NULL;
}

#define NVR_FS_CACHE 512
static struct { uint32_t key[41]; VkShaderModule m; int valid; } s_fs[NVR_FS_CACHE];

/* The draw's fragment module: the combiners the registers program, or the
 * basic one. As the old backend's fs_select, less what slice 2a lacks
 * (cube maps, shadow maps, HDR, the debug views). */
static VkShaderModule fs_select(const struct nv2a_pb_vsh *v)
{
    static char glsl[65536];
    NV2ACombinerState st;
    uint32_t key[41], modes = 0, signs = 0, shadows = 0, h, i, slot;
    int n, wbuf;
    char what[32];

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        uint32_t m = (uint32_t)s_stage[n].mode;
        if (s_stage[n].raw_mode == 5)
            m = 5;
        else if (m == 3 && s_stage[n].cube)
            m = 3;
        else if ((m == 6 || m == 7) && n > 0)
            ;
        else if ((s_stage[n].raw_mode == 15 || s_stage[n].raw_mode == 16) && n > 0)
            m = (uint32_t)s_stage[n].raw_mode;
        else if (s_stage[n].raw_mode == 17 && (n == 1 || n == 2))
            m = 17;
        else if (s_stage[n].raw_mode == 9 && n >= 2)
            m = 9;
        else if (m && m != 4)
            m = 1;
        modes |= m << (5 * n);
        if (m == 1 || m == 6 || m == 7 || m == 9 || m == 15 || m == 16)
            signs |= (uint32_t)nv2a_tex_signed_mask(s_stage[n].filter) << (4 * n);
        if (m == 1 && (s_stage[n].raw_mode == 1 || s_stage[n].raw_mode == 2)) {
            int kind = nv2a_tex_depth_kind(s_stage[n].color);
            if (kind)
                shadows |= (uint32_t)(kind | ((v->shadow_func & 7u) << 2)
                                      | (s_stage[n].raw_mode == 2 ? 0x20u : 0u)
                                      | (dt_for_texture(&s_stage[n]) ? 0x40u : 0u)) << (8 * n);
            else if (s_stage[n].raw_mode == 1 && dt_for_zcolor(&s_stage[n]))
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
    key[37] = v->shader_ctl & 0x00FF0FFFu;
    key[38] = v->clip_plane_mode
            | (nv2a_alpha_kill_mask(s_stage, nv2a_pb_gpu_state()->flips) << 16);
    wbuf = (v->control0 & (1u << 16)) && v->clip_max > 0.0f;
    key[39] = (uint32_t)wbuf << 16;
    key[40] = shadows;
    h = fnv32(key, sizeof key);
    slot = h % NVR_FS_CACHE;
    for (i = 0; i < NVR_FS_CACHE; i++, slot = (slot + 1) % NVR_FS_CACHE) {
        if (s_fs[slot].valid && !memcmp(s_fs[slot].key, key, sizeof key))
            return s_fs[slot].m;
        if (!s_fs[slot].valid)
            break;
    }
    if (s_fs[slot].valid)
        return VK_NULL_HANDLE;
    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw, v->rc_ctl,
                             v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;
    for (n = 0; n < 4; n++) {
        st.tex_signed[n] = (uint8_t)((signs >> (4 * n)) & 0xFu);
        st.clip_mode[n] = (uint8_t)((v->clip_plane_mode >> (4 * n)) & 0xFu);
        st.tex_shadow[n] = (uint8_t)(shadows >> (8 * n));
        st.dot_map[n] = n ? (uint8_t)((v->shader_ctl >> (4 * (n - 1))) & 0xFu) : 0;
    }
    st.alpha_kill = (uint8_t)nv2a_alpha_kill_mask(s_stage, nv2a_pb_gpu_state()->flips);
    st.wbuffer = (uint8_t)wbuf;
    st.input_tex[2] = (uint8_t)((v->shader_ctl >> 16) & 0xFu);
    st.input_tex[3] = (uint8_t)((v->shader_ctl >> 20) & 0xFu);
    snprintf(what, sizeof what, "combiners %08X", h);
    if (d3d8_combiners_generate_glsl(&st, glsl, sizeof glsl) <= 0)
        return VK_NULL_HANDLE;
    memcpy(s_fs[slot].key, key, sizeof key);
    s_fs[slot].m = module_from_glsl(NVK_STAGE_FRAGMENT, glsl, what);
    s_fs[slot].valid = 1;
    return s_fs[slot].m;
}

/* ---- pipelines -------------------------------------------------------------- */

typedef struct {
    uint64_t vs, fs;
    uint32_t nf4, topo, wmask, depth;
    uint32_t blend[4];          /* enable, src, dst, VkBlendOp */
} NvrPsoKey;

#define NVR_PSO_CACHE 2048
static struct { NvrPsoKey k; VkPipeline p; } s_pso[NVR_PSO_CACHE];

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
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE,
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

static VkPipeline pipeline_new(const NvrPsoKey *k)
{
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
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipeline p = VK_NULL_HANDLE;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    };
    uint32_t i;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = (VkShaderModule)(uintptr_t)k->vs;
    ss[0].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = (VkShaderModule)(uintptr_t)k->fs;
    ss[1].pName = "main";
    memset(&vb, 0, sizeof vb);
    vb.stride = k->nf4 * 16u;
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
    pr.depthAttachmentFormat = k->depth ? C->depth_format : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = k->depth ? C->depth_format : VK_FORMAT_UNDEFINED;
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
    if (vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, NULL, &p) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

static VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, uint32_t nf4,
                           enum nv2a_topology topo)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    NvrPsoKey k;
    uint32_t i, slot;

    memset(&k, 0, sizeof k);
    k.vs = (uint64_t)(uintptr_t)vs;
    k.fs = (uint64_t)(uintptr_t)fs;
    k.nf4 = nf4;
    k.topo = (uint32_t)topo;
    k.wmask = g->color_keep;
    k.depth = s_pass_dt ? 1u : 0u;
    k.blend[0] = g->blend_enable ? 1u : 0u;
    k.blend[1] = k.blend[0] ? g->blend_sfactor : 0u;
    k.blend[2] = k.blend[0] ? g->blend_dfactor : 0u;
    k.blend[3] = k.blend[0] ? (uint32_t)blend_op(g->blend_equation) : 0u;
    slot = fnv32(&k, sizeof k) % NVR_PSO_CACHE;
    for (i = 0; i < NVR_PSO_CACHE; i++, slot = (slot + 1) % NVR_PSO_CACHE) {
        if (s_pso[slot].p && !memcmp(&s_pso[slot].k, &k, sizeof k))
            return s_pso[slot].p;
        if (!s_pso[slot].p)
            break;
    }
    if (s_pso[slot].p)
        return VK_NULL_HANDLE;
    s_pso[slot].k = k;
    s_pso[slot].p = pipeline_new(&k);
    return s_pso[slot].p;
}

/* ---- textures ------------------------------------------------------------- */

/* An image the GPU may still read: freed when this frame's slot comes round. */
static void retire_image(struct nvk_image *im)
{
    NvrFrame *fr = &s_frame[s_slot];
    if (!im->img)
        return;
    if (fr->ngarbage == fr->garbage_cap) {
        int cap = fr->garbage_cap ? fr->garbage_cap * 2 : 64;
        struct nvk_image *g = (struct nvk_image *)realloc(fr->garbage, (size_t)cap * sizeof *g);
        if (!g) {
            frame_submit(1, "nvr retire");
            nvk_image_free(im);
            memset(im, 0, sizeof *im);
            return;
        }
        fr->garbage = g;
        fr->garbage_cap = cap;
    }
    fr->garbage[fr->ngarbage++] = *im;
    memset(im, 0, sizeof *im);
}

#define NVR_TEX 1024
typedef struct {
    uint32_t offset, color, w, h, pitch, levels, palette, pal_len;
    uint8_t  sgn, cut;          /* the signed-channel XOR and mip cut-out applied */
    uint64_t hash;              /* of its bytes (and palette) at the upload */
    uint32_t checked;           /* the flip its hash was last checked at */
    uint32_t rep;               /* nv2a_texrep_apply's generation; 0: nothing replaced */
    uint32_t up_scale;          /* its image is this many times the title's size; 0: the same */
    uint64_t last_use;
    struct nvk_image im;
} NvrTex;
static NvrTex   s_tex[NVR_TEX];
static uint32_t *s_texels;
static size_t   s_texels_cap;
static uint64_t s_tex_uploads, s_tex_syncs, s_rtt_binds;
static uint32_t s_bind_scale;   /* the up_scale of the texture tex_bind last returned */

static uint64_t tex_hash(const uint8_t *mem, const struct nv2a_stage *t, uint32_t bytes)
{
    uint64_t h = nv2a_tex_hash(mem + t->offset, bytes, NV2A_TEX_HASH_SEED);
    if (t->color == NV2A_TEX_P8)
        h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
    return h;
}

/* Texels of every level into a new image (stage, copy, readable). */
static int tex_upload(const struct nv2a_stage *t, uint8_t sgn, uint32_t cut, NvrTex *e)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL;
    VkBufferImageCopy r[16];
    VkDeviceSize off;
    VkCommandBuffer cb;
    uint8_t *p;
    uint32_t l, iw = t->width, ih = t->height, levels = t->levels;
    size_t at = 0;

    if (n > s_texels_cap) {
        uint32_t *q = (uint32_t *)realloc(s_texels, n * 4);
        if (!q)
            return 0;
        s_texels = q;
        s_texels_cap = n;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)(mem + t->palette);
    nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch, t->levels,
                    pal, pal ? t->pal_len : 0, s_texels);
    e->rep = nv2a_texrep_apply(mem + t->offset, t->color, t->width, t->height, t->pitch,
                               t->levels, s_texels);
    /* A registered picture that also comes larger (nv2a_texrep_hires): that
     * one, with its levels down to the title's size, so a prompt drawn at
     * twice the size stays sharp at a higher render scale. */
    if (e->rep) {
        unsigned hf = 0;
        const uint32_t *hi = nv2a_texrep_hires(mem + t->offset, t->color, t->width, t->height,
                                               t->pitch, &hf);
        if (hi && hf >= 2 && hf <= 4) {
            uint32_t hl = hf == 4 ? 3u : 2u;
            size_t hn = nv2a_tex_texels(t->width * hf, t->height * hf, hl);
            uint32_t *q = hn > s_texels_cap ? (uint32_t *)realloc(s_texels, hn * 4) : s_texels;
            if (q) {
                static int said;
                if (hn > s_texels_cap) {
                    s_texels = q;
                    s_texels_cap = hn;
                }
                memcpy(s_texels, hi, (size_t)t->width * hf * t->height * hf * 4);
                iw = t->width * hf;
                ih = t->height * hf;
                levels = hl;
                n = hn;
                nv2a_texscale_mips(s_texels, iw, ih, levels);
                e->up_scale = hf;
                if (said++ < 8)
                    fprintf(stderr, LOGP "texrep: %ux%u texture at 0x%08X uploaded from its"
                            " x%u picture\n", t->width, t->height, t->offset, hf);
            }
        }
    }
    if (!e->up_scale) (void)nv2a_texmip_apply(mem + t->offset, t->color, t->width, t->height, t->pitch,
                            t->levels, cut, s_texels);
    if (sgn && !e->up_scale) {
        uint32_t x = nv2a_tex_signed_xor(sgn);
        size_t i;
        for (i = 0; i < n; i++)
            s_texels[i] ^= x;
    }
    pass_end();
    if (!frame_cb() || !(p = stage((VkDeviceSize)n * 4u, &off)) || !(cb = frame_cb()))
        return 0;
    memcpy(p, s_texels, n * 4u);
    if (!nvk_image_new(&e->im, VK_FORMAT_B8G8R8A8_UNORM, iw, ih, levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    memset(r, 0, sizeof r);
    for (l = 0; l < levels && l < 16; l++) {
        uint32_t w = nv2a_tex_level_dim(iw, l), h = nv2a_tex_level_dim(ih, l);
        r[l].bufferOffset = off + at * 4u;
        r[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r[l].imageSubresource.mipLevel = l;
        r[l].imageSubresource.layerCount = 1;
        r[l].imageExtent.width = w;
        r[l].imageExtent.height = h;
        r[l].imageExtent.depth = 1;
        at += (size_t)w * h;
    }
    nvk_image_to(cb, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, s_frame[s_slot].stage.buf, e->im.img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, l, r);
    nvk_image_to(cb, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_tex_uploads++;
    return 1;
}

/* The stage's texture as an image view: cached by where and how it is laid
 * out, its bytes hashed again once a flip. A target drawn over those bytes
 * since its last write-back is written back first. */
#define NVR_TEX_HINTS 4096
static uint16_t s_tex_hint[NVR_TEX_HINTS];
#define TEX_MATCH(c) ((c)->im.img && (c)->offset == t->offset && (c)->color == t->color \
                      && (c)->w == t->width && (c)->h == t->height && (c)->pitch == t->pitch \
                      && (c)->levels == t->levels && (c)->palette == t->palette \
                      && (c)->pal_len == t->pal_len && (c)->sgn == sgn)

static VkImageView tex_bind(const struct nv2a_stage *t, uint8_t sgn, uint32_t cut)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i, lo, hi, hk;
    NvrTex *e = NULL, *victim = &s_tex[0];
    uint64_t h;

    s_bind_scale = 0;
    if (t->valid && (uint64_t)t->width * t->height <= (1u << 24))
        bytes = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    if (!bytes)
        return s_black.view;
    lo = t->offset & 0x07FFFFFFu;
    hi = lo + bytes;
    ghost_flush_range(lo, hi);
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *rt = &s_rt[i];
        uint32_t rlo = rt->addr & 0x07FFFFFFu;
        if (rt->im.img && !rt->scratch && rt->dirty && lo < rlo + rt->pitch * rt->h && rlo < hi) {
            if (target_stale(rt))
                continue;
            nvr_sync_target(rt);
            s_tex_syncs++;
        }
    }
    /* The entry this key found last before the walk: a bind is several a
     * draw, a million draws a minute. */
    hk = ((t->offset >> 4) ^ (t->offset >> 12) ^ t->color ^ ((uint32_t)sgn << 7)) & (NVR_TEX_HINTS - 1);
    if (s_tex_hint[hk] && TEX_MATCH(&s_tex[s_tex_hint[hk] - 1]))
        e = &s_tex[s_tex_hint[hk] - 1];
    for (i = 0; !e && i < NVR_TEX; i++) {
        NvrTex *c = &s_tex[i];
        if (TEX_MATCH(c)) {
            e = c;
            break;
        }
        if (!c->im.img || (victim->im.img && c->last_use < victim->last_use))
            victim = c;
    }
    s_tex_hint[hk] = (uint16_t)((e ? e : victim) - s_tex + 1);
    if (e) {
        /* Levels made for an alpha test are made once, for the first test
         * that binds it; a title fading its reference would remake them
         * every step. */
        int stale;
        if (e->cut)
            cut = e->cut;
        stale = e->cut != cut || (e->rep && e->rep != nv2a_texrep_generation());
        e->last_use = s_tick;
        if (e->checked == g->flips && !stale) {
            s_bind_scale = e->up_scale;
            return e->im.view;
        }
        e->checked = g->flips;
        h = tex_hash(mem, t, bytes);
        if (h == e->hash && !stale) {
            s_bind_scale = e->up_scale;
            return e->im.view;
        }
        retire_image(&e->im);
    } else {
        e = victim;
        retire_image(&e->im);
        h = tex_hash(mem, t, bytes);
    }
    memset(e, 0, sizeof *e);
    if (!tex_upload(t, sgn, cut, e)) {
        memset(e, 0, sizeof *e);
        return s_black.view;
    }
    e->offset = t->offset;
    e->color = t->color;
    e->w = t->width;
    e->h = t->height;
    e->pitch = t->pitch;
    e->levels = t->levels;
    e->palette = t->palette;
    e->pal_len = t->pal_len;
    e->sgn = sgn;
    e->cut = (uint8_t)cut;
    e->hash = h;
    e->checked = g->flips;
    e->last_use = s_tick;
    s_bind_scale = e->up_scale;
    return e->im.view;
}

/* ---- targets sampled in place ---------------------------------------------- */

static struct nvk_image s_self;       /* a copy of the target being drawn */
static VkImageView      s_self_gb;

static VkImageView view_swizzled(VkImage img, VkFormat fmt, VkComponentSwizzle r, VkComponentSwizzle g,
                                 VkComponentSwizzle b, VkComponentSwizzle a)
{
    VkImageViewCreateInfo vi;
    VkImageView v = VK_NULL_HANDLE;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.components.r = r;
    vi.components.g = g;
    vi.components.b = b;
    vi.components.a = a;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_dev, &vi, NULL, &v) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return v;
}

/* Its image readable by a shader: outside any pass. */
static void to_sampled(struct nvk_image *im)
{
    VkCommandBuffer cb;
    if (im->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    pass_end();
    if ((cb = frame_cb()) != VK_NULL_HANDLE)
        nvk_image_to(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* ---- cube maps: cube_bind in the old backend -------------------------------
 *
 * The six faces sit face_stride apart. A face this renderer drew is copied
 * from its target whenever it was drawn since the last copy; the others decode
 * from guest memory, re-checked once a flip; a change makes a new cube, so a
 * frame in flight keeps the one it used. */
#define NVR_CUBES 8
typedef struct {
    uint32_t offset, color, width, levels, side;    /* side: host face size */
    uint32_t checked, used;
    uint64_t hash;
    uint64_t face_gen[6];
    struct nvk_image im;
} NvrCube;
static NvrCube s_cube[NVR_CUBES];
static uint32_t s_cube_clock;
static struct nvk_image s_black_cube;

static int cube_face_upload(struct nvk_image *im, uint32_t face, uint32_t level, uint32_t w,
                            const void *px)
{
    VkBufferImageCopy r;
    VkDeviceSize off;
    VkCommandBuffer cb;
    uint8_t *p;

    if (!(p = stage((VkDeviceSize)w * w * 4u, &off)) || !(cb = frame_cb()))
        return 0;
    memcpy(p, px, (size_t)w * w * 4u);
    if (im->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        nvk_image_to(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.bufferOffset = off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.mipLevel = level;
    r.imageSubresource.baseArrayLayer = face;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = w;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, s_frame[s_slot].stage.buf, im->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    return 1;
}

static VkImageView black_cube(void)
{
    if (!s_black_cube.img) {
        static const uint32_t px = 0xFF000000u;
        uint32_t f;
        pass_end();
        if (!nvk_image_new_cube(&s_black_cube, VK_FORMAT_B8G8R8A8_UNORM, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return s_black.view;        /* a 2D view: invalid there, but never NULL */
        for (f = 0; f < 6; f++)
            cube_face_upload(&s_black_cube, f, 0, 1, &px);
        if (frame_cb())
            nvk_image_to(frame_cb(), &s_black_cube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return s_black_cube.view;
}

static NvrTarget *cube_face_rt(uint32_t addr, uint32_t w)
{
    int i;
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *rt = &s_rt[i];
        if (!rt->im.img || rt->scratch || ((rt->addr ^ addr) & 0x07FFFFFFu)
                || rt->w < w || rt->h < w || target_stale(rt))
            continue;
        return rt;
    }
    return NULL;
}

static VkImageView cube_bind(const struct nv2a_stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t *pal = t->color == NV2A_TEX_P8 ? (const uint32_t *)(mem + t->palette) : NULL;
    NvrTarget *frt[6];
    uint32_t w = t->width, side = t->width, levels, stride, bytes, f, l;
    NvrCube *e = NULL, *victim = &s_cube[0];
    VkCommandBuffer cb;
    int any = 0, all = 1, fresh = 0, ncopy = 0, i;

    if (!w || t->width != t->height || (uint64_t)w * w * 6u > (1u << 24))
        return black_cube();
    stride = nv2a_tex_face_stride(t->color, w, w, t->pitch, t->fmt_levels);
    bytes = nv2a_tex_extent(t->color, w, w, t->pitch, t->levels);
    if (!bytes)
        return black_cube();
    for (f = 0; f < 6; f++) {
        frt[f] = cube_face_rt(t->offset + f * stride, w);
        if (frt[f]) {
            any = 1;
            side = w * frt[f]->scale;
        } else {
            all = 0;
        }
    }
    levels = any ? 1u : t->levels;
    for (i = 0; i < NVR_CUBES; i++) {
        NvrCube *c = &s_cube[i];
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
        retire_image(&e->im);
        memset(e, 0, sizeof *e);
        e->offset = t->offset;
        e->color = t->color;
        e->width = w;
        e->levels = levels;
        fresh = 1;
    }
    if (e->side != side)
        fresh = 1;
    if (!all && (fresh || e->checked != g->flips)) {
        uint64_t h = NV2A_TEX_HASH_SEED;
        for (f = 0; f < 6; f++)
            if (!frt[f])
                h = h * 31u + nv2a_tex_hash(mem + t->offset + f * stride, bytes,
                                            NV2A_TEX_HASH_SEED);
        if (pal)
            h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
        e->checked = g->flips;
        if (h != e->hash)
            fresh = 1;
        e->hash = h;
    }
    if (fresh) {
        size_t n = nv2a_tex_texels(w, w, levels);
        int uploaded = 0;

        pass_end();
        retire_image(&e->im);
        memset(&e->im, 0, sizeof e->im);
        e->side = side;
        memset(e->face_gen, 0, sizeof e->face_gen);
        if (!nvk_image_new_cube(&e->im, VK_FORMAT_B8G8R8A8_UNORM, side, levels,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return black_cube();
        if (n > s_texels_cap) {
            uint32_t *q = (uint32_t *)realloc(s_texels, n * 4);
            if (!q)
                return black_cube();
            s_texels = q;
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
                    uploaded |= cube_face_upload(&e->im, f, 0, side, up);
                    free(up);
                }
                continue;
            }
            for (l = 0; l < levels; l++) {
                uint32_t lw = nv2a_tex_level_dim(w, l);
                uploaded |= cube_face_upload(&e->im, f, l, lw, src);
                src += (size_t)lw * lw;
            }
        }
        if (!uploaded && (cb = frame_cb()) != VK_NULL_HANDLE) {
            /* All faces drawn: a face never copied reads black. */
            VkClearColorValue z;
            VkImageSubresourceRange r;
            memset(&z, 0, sizeof z);
            z.float32[3] = 1.0f;
            memset(&r, 0, sizeof r);
            r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.levelCount = VK_REMAINING_MIP_LEVELS;
            r.layerCount = VK_REMAINING_ARRAY_LAYERS;
            nvk_image_to(cb, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cb, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
        }
    }
    for (f = 0; f < 6; f++)
        if (frt[f] && frt[f]->gen != e->face_gen[f])
            ncopy++;
    if (ncopy) {
        pass_end();
        if ((cb = frame_cb()) != VK_NULL_HANDLE) {
            nvk_image_to(cb, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (f = 0; f < 6; f++) {
                VkImageCopy ic;
                if (!frt[f] || frt[f]->gen == e->face_gen[f])
                    continue;
                nvk_image_to(cb, &frt[f]->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                memset(&ic, 0, sizeof ic);
                ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.srcSubresource.layerCount = 1;
                ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.dstSubresource.baseArrayLayer = f;
                ic.dstSubresource.layerCount = 1;
                ic.extent.width = side;
                ic.extent.height = side;
                ic.extent.depth = 1;
                vkCmdCopyImage(cb, frt[f]->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                e->face_gen[f] = frt[f]->gen;
            }
        }
    }
    if (e->im.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        pass_end();
        if ((cb = frame_cb()) != VK_NULL_HANDLE)
            nvk_image_to(cb, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    e->used = ++s_cube_clock;
    return e->im.view;
}

/* The stage's texture when its bytes and layout are a target's: the target's
 * image (rt_texture in the old backend). 16-bit R5G6B5 and X8 through a view
 * with alpha 1, G8B8 through one that reads it as the console stores it, the
 * target being drawn through a copy. NULL: tex_bind's decode. */
static NvrTarget *rt_texture(const struct nv2a_stage *st, NvrTarget *cur, VkImageView *view,
                             uint32_t *sw, uint32_t *sh)
{
    NvrTarget *self = NULL;
    uint32_t twin = nv2a_tex_linear_twin(st->color);
    int i, r565 = twin == 0x11, x8 = twin == 0x1E, g8b8 = twin == 0x17;

    if (!st->offset || d3d8_format_dxt_block_bytes(st->color) || nv2a_tex_depth_kind(st->color))
        return NULL;
    if (!g8b8 && !r565 && nv2a_tex_texel_bytes(twin) != 4)
        return NULL;
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *t = &s_rt[i];
        if (!t->im.img || t->scratch || ((t->addr ^ st->offset) & 0x07FFFFFFu))
            continue;
        if (g8b8) {
            if (t->cfmt != 0xA || t->pitch / t->w != 2u || t->w != st->width || t->h != st->height
                    || (d3d8_format_is_swizzled(st->color) != 0) != (t->swz != 0)
                    || (!t->swz && st->pitch != t->pitch))
                return NULL;
        } else if (r565) {
            if (t->pitch / t->w != 2u || t->w != st->width || t->h != st->height
                    || (st->color == 0x11 ? t->swz || st->pitch != t->pitch : !t->swz))
                continue;
        }
        if (target_stale(t)) {
            if (g8b8)
                return NULL;
            continue;
        }
        if (!g8b8 && !r565 && (d3d8_format_is_swizzled(st->color) != 0) != (t->swz != 0))
            return NULL;
        if (t == cur) {
            if (r565)
                continue;
            self = t;
            break;
        }
        if (g8b8) {
            if (!t->gb)
                t->gb = view_swizzled(t->im.img, t->im.format, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_G);
            if (!t->gb)
                return NULL;
            *view = t->gb;
        } else if (r565 || x8) {
            if (!t->opaque)
                t->opaque = view_swizzled(t->im.img, t->im.format, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE);
            if (!t->opaque)
                return NULL;
            *view = t->opaque;
        } else {
            *view = t->im.view;
        }
        to_sampled(&t->im);
        *sw = t->w;
        *sh = t->h;
        return t;
    }
    if (!self)
        return NULL;
    /* Sampled while drawn into: a copy of it. */
    if (!s_self.img || s_self.w != self->im.w || s_self.h != self->im.h) {
        view_garbage(s_self_gb);
        s_self_gb = VK_NULL_HANDLE;
        retire_image(&s_self);
        if (!nvk_image_new(&s_self, self->im.format, self->im.w, self->im.h, 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
            memset(&s_self, 0, sizeof s_self);
            return NULL;
        }
    }
    {
        VkImageCopy ic;
        VkCommandBuffer cb;
        pass_end();
        if (!(cb = frame_cb()))
            return NULL;
        nvk_image_to(cb, &self->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        nvk_image_to(cb, &s_self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = self->im.w;
        ic.extent.height = self->im.h;
        ic.extent.depth = 1;
        vkCmdCopyImage(cb, self->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_self.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        nvk_image_to(cb, &s_self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    *view = s_self.view;
    if (g8b8) {
        if (!s_self_gb)
            s_self_gb = view_swizzled(s_self.img, s_self.format, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_G);
        if (!s_self_gb)
            return NULL;
        *view = s_self_gb;
    }
    *sw = self->w;
    *sh = self->h;
    return self;
}

#define NVR_SMP 128
static struct { uint64_t key; VkSampler s; } s_smp[NVR_SMP];
static int s_smp_n;

static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

/* The stage's sampler, as the old backend makes it (filters, wrap, mips and
 * their LOD clamps, the host's anisotropy). */
static VkSampler sampler(const struct nv2a_stage *t, uint32_t scale)
{
    VkSamplerCreateInfo sc;
    VkSampler st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn = nv2a_tex_filter_linear(t->filter, 0), mg = nv2a_tex_filter_linear(t->filter, 1);
    int mip = t->levels > 1 ? nv2a_tex_filter_mip(t->filter) : 0, aniso, i;
    uint64_t key;

    /* A larger picture has levels down to the title's size, blended between. */
    if (scale)
        mip = 2;
    aniso = (mip && mn && !scale && C->max_aniso > 1.0f) ? nv2a_host_aniso() : 1;
    if (aniso > 1 && (float)aniso > C->max_aniso)
        aniso = (int)C->max_aniso;
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    key |= (uint64_t)aniso << 33;
    key |= (uint64_t)scale << 40;
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
    sc.maxLod = scale ? (scale == 4 ? 2.0f : 1.0f) : mip ? (float)t->lod_max : 0.25f;
    if (vkCreateSampler(s_dev, &sc, NULL, &st) != VK_SUCCESS)
        return s_smp_point;
    if (s_smp_n == NVR_SMP)
        return st;                     /* past the cache: kept, not reused */
    s_smp[s_smp_n].key = key;
    s_smp[s_smp_n].s = st;
    s_smp_n++;
    return st;
}

/* Set 1 for these views and samplers, from this frame's pool; the last one
 * again when nothing changed. */
static VkDescriptorSet texture_set(const VkImageView view[4], const VkSampler smp[4])
{
    static VkImageView lv[4];
    static VkSampler ls[4];
    static VkDescriptorSet last;
    static int last_slot = -1;
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w;
    VkDescriptorSet set;
    int i;

    if (last && last_slot == s_slot && s_frame_id_set == s_frame_id
            && !memcmp(lv, view, sizeof lv) && !memcmp(ls, smp, sizeof ls))
        return last;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s_frame[s_slot].tpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_dsl[1];
    if (vkAllocateDescriptorSets(s_dev, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    for (i = 0; i < 4; i++) {
        ii[i].sampler = smp[i];
        ii[i].imageView = view[i];
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    memcpy(lv, view, sizeof lv);
    memcpy(ls, smp, sizeof ls);
    last = set;
    last_slot = s_slot;
    s_frame_id_set = s_frame_id;
    return set;
}

/* nv2a_hud_squeeze: as the old backend's (a title's HUD in a wider picture). */
#define HUD_MAXV 1024
static int hud_squeeze(const struct nv2a_pb_draw *d, const int *slot, int nf4, uint32_t lo,
                       uint32_t count, const float (*inl)[4], const uint32_t *idx, uint32_t nidx,
                       int topo, float *vf)
{
    static float buf[HUD_MAXV * 16 * 4];
    static int par[HUD_MAXV];
    static float bx0[HUD_MAXV], bx1[HUD_MAXV], by0[HUD_MAXV], by1[HUD_MAXV];
    static unsigned char big[HUD_MAXV];
    uint32_t hash, vhash, vlen = 0, i, j;
    float k;
    int s0 = -1, merged;

    if (!d->program || !nv2a_hud_squeeze_get(&hash, &k))
        return 0;
    vhash = nv2a_vsh_program_hash(nv2a_pb_vsh_state(), &vlen);
    if (vhash != hash)
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

/* ---- the draw --------------------------------------------------------------- */

static int draw_scissor(const struct nv2a_pb_gpu *g, const NvrTarget *t, VkRect2D *sr)
{
    uint32_t x0 = g->clip_x, y0 = g->clip_y;
    uint32_t x1 = g->clip_x + g->clip_w, y1 = g->clip_y + g->clip_h;
    uint32_t hw = t->im.w, hh = t->im.h;

    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (x0 >= x1 || y0 >= y1)
        return 0;
    sr->offset.x = (int32_t)((uint64_t)x0 * hw / t->w);
    sr->offset.y = (int32_t)((uint64_t)y0 * hh / t->h);
    sr->extent.width = (uint32_t)((uint64_t)x1 * hw / t->w) - (uint32_t)sr->offset.x;
    sr->extent.height = (uint32_t)((uint64_t)y1 * hh / t->h) - (uint32_t)sr->offset.y;
    return 1;
}

static void nvr_on_draw(const struct nv2a_pb_draw *d)
{
    float host_bias;
    static uint32_t *idx;
    static uint32_t idx_cap;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    enum nv2a_topology topo;
    NvrTarget *t;
    VkShaderModule vmod, fmod;
    VkPipeline pso;
    VkCommandBuffer cb;
    struct nv2a_vp_consts vc;
    NvrPsConsts pc;
    VkDeviceSize vb_off, ib_off, uo[3] = { 0, 0, 0 }, vbytes;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k, nf4 = 0, dyn_off[3];
    int slot[NV2A_VS_MAX_INPUTS], cull = 0, cull_all = 0, tries;
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    float *vf = NULL;
    uint32_t *ip = NULL;
    VkRect2D sr;
    VkDescriptorSet tset;

    if (!init())
        return;
    if (g->color_keep == 0xFFFFFFFFu && !(v->depth_enable && v->depth_mask) && !use_s
            && !s_occ_q) {
        s_draw_skips[SKIP_MASKED]++;
        return;
    }
    if (!d->idx_count) {
        s_draw_skips[SKIP_NO_IDX]++;
        return;
    }
    if (d->program) {
        uint16_t in = 0;
        vmod = vs_program(v, &in);
        if (!vmod) {
            s_draw_skips[SKIP_NO_VS]++;
            return;
        }
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
            if (in & (1u << i))
                slot[nf4++] = (int)i;
    } else {
        vmod = s_vs_pass;
        for (k = 0; k < NVR_PASS_F4; k++)
            slot[nf4++] = s_pass_attr[k];
    }
    if (!(t = surface()) && !(t = surface_depth_only())) {
        s_draw_skips[SKIP_NO_RT]++;
        return;
    }
    build_stages();
    memset(&pc, 0, sizeof pc);
    nv2a_ps_consts_fill(v, s_stage, &pc.b);
    /* A cut-out draw takes no negative host bias: a finer level than the
     * pixel's own makes its edge shimmer. */
    host_bias = nv2a_host_lod_bias();
    if (host_bias < 0.0f && (nv2a_texmip_mode() & NV2A_TEXMIP_BIAS)
            && nv2a_texmip_cut_ref(v->atest_enable, v->atest_func, v->atest_ref))
        host_bias = 0.0f;
    for (k = 0; k < 4; k++) {
        const struct nv2a_stage *st = &s_stage[k];
        float b = st->levels > 1 && nv2a_tex_filter_mip(st->filter)
                ? nv2a_tex_lod_bias(st->filter) + host_bias : 0.0f;
        pc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
    }
    {
        const uint32_t *tr = nv2a_pb_tex_regs();
        for (k = 0; k < 4; k++) {
            const uint32_t *r = &tr[(0x40u * k + 0x28u) / 4];
            memcpy(pc.bump_mat[k], r, 16);
            memcpy(&pc.bump_lum[k][0], &r[4], 4);
            memcpy(&pc.bump_lum[k][1], &r[5], 4);
        }
    }
    pc.hdr[0] = 1.0f;
    fmod = fs_select(v);
    if (!fmod) {
        s_draw_skips[SKIP_PSO]++;
        return;
    }
    nv2a_idx_range(d->idx, d->idx_count, &lo, &hi);
    count = hi - lo + 1;
    if (d->idx_count * 3u > idx_cap) {
        uint32_t *grown = (uint32_t *)realloc(idx, (size_t)d->idx_count * 12u);
        if (!grown)
            return;
        idx = grown;
        idx_cap = d->idx_count * 3u;
    }
    nidx = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, lo, idx, &topo);
    if (!nidx) {
        s_draw_skips[SKIP_NO_PRIM]++;
        return;
    }
    if (v->cull_enable) {
        if (v->cull_face == 0x405)      cull = 1;
        else if (v->cull_face == 0x404) cull = 2;
        else if (v->cull_face == 0x408) cull_all = 1;
    }
    if (cull_all && topo == NV2A_TOPO_TRIANGLES)
        return;
    nv2a_vp_consts_fill(v, g->format, t->w, t->h, &vc);
    if (!(use_z && v->zeta_offset) && v->clip_max > nv2a_zmax(g->format)) {
        vc.c[2] = 1.0f / v->clip_max;
        vc.c[3] = v->poly_offset ? v->poly_bias / v->clip_max : 0.0f;
    }
    if (!draw_scissor(g, t, &sr)) {
        s_draw_skips[SKIP_SCISSOR]++;
        return;
    }
    if (!frame_cb())
        return;
    if (use_z) {
        NvrDepth *dz = depth_target(t->w, t->h);
        if (dz)
            t->dt = dz;
    }
    {
        /* Textures first: an upload or a write-back is outside a pass, and a
         * write-back submits the frame, so nothing of the draw is placed in
         * this frame's rings yet. The basic fragment shader has no signed
         * channels. */
        VkImageView view[4];
        VkSampler smp[4];
        uint32_t cut = 0;
        for (k = 0; k < 4; k++) {
            const struct nv2a_stage *st = &s_stage[k];
            uint8_t sgn = fmod != s_fs_basic ? nv2a_tex_signed_mask(st->filter) : 0;
            view[k] = s_black.view;
            smp[k] = sampler(st, 0);
            if (!st->mode || st->mode == 4)
                continue;
            if (st->mode == 3 && st->cube) {
                if (fmod != s_fs_basic)
                    view[k] = cube_bind(st);
                continue;
            }
            cut = 0;
            if ((nv2a_texmip_mode() & NV2A_TEXMIP_LEVELS) && st->levels > 1
                    && nv2a_tex_filter_mip(st->filter) && !sgn)
                cut = nv2a_texmip_cut_ref(v->atest_enable, v->atest_func, v->atest_ref);
            if (fmod != s_fs_basic && dt_for_texture(st) != NULL) {
                NvrDepth *sd = dt_for_texture(st);
                to_sampled(&sd->im);
                view[k] = sd->sview;
                continue;
            }
            if (fmod != s_fs_basic && st->raw_mode == 1 && dt_for_zcolor(st) != NULL) {
                NvrDepth *sd = dt_for_zcolor(st);
                to_sampled(&sd->im);
                view[k] = sd->sview;
                pc.b.tex_scale[k][0] = 1.0f / (float)sd->w;
                pc.b.tex_scale[k][1] = 1.0f / (float)sd->h;
                continue;
            }
            {
                uint32_t sw, sh;
                if ((st->mode == 3 || !sgn) && rt_texture(st, t, &view[k], &sw, &sh) != NULL) {
                    if (!nv2a_tex_size_from_format(st->color)) {
                        pc.b.tex_scale[k][0] = 1.0f / (float)sw;
                        pc.b.tex_scale[k][1] = 1.0f / (float)sh;
                    }
                    s_rtt_binds++;
                    continue;
                }
            }
            view[k] = tex_bind(st, sgn, cut);
            if (s_bind_scale)
                smp[k] = sampler(st, s_bind_scale);
        }
        tset = texture_set(view, smp);
        if (!tset) {
            s_draw_skips[SKIP_PSO]++;
            return;
        }
    }
    vbytes = (VkDeviceSize)count * nf4 * 16u;
    if (vbytes < 16)
        vbytes = 16;
    for (tries = 0; tries < 2; tries++) {
        void *u0 = NULL, *u1, *u2;
        vf = (float *)ring_take(NVR_RING_VB, vbytes, 16, &vb_off);
        ip = vf ? (uint32_t *)ring_take(NVR_RING_IB, (VkDeviceSize)nidx * 4u, 16, &ib_off) : NULL;
        if (ip && d->program && !(u0 = ring_take(NVR_RING_UB, NVR_VS_C_BYTES, s_ub_align, &uo[0])))
            ip = NULL;
        u1 = ip ? ring_take(NVR_RING_UB, NVR_VP_C_BYTES, s_ub_align, &uo[1]) : NULL;
        u2 = u1 ? ring_take(NVR_RING_UB, NVR_PS_C_BYTES, s_ub_align, &uo[2]) : NULL;
        if (vf && ip && u1 && u2) {
            if (u0)
                memcpy(u0, v->c, NVR_VS_C_BYTES);
            memcpy(u1, &vc, NVR_VP_C_BYTES);
            memcpy(u2, &pc, NVR_PS_C_BYTES);
            break;
        }
        vf = NULL;
        /* A ring is full: this frame's commands go, the next frame's rings. */
        if (tries || !frame_submit(0, "nvr ring full") || !frame_cb())
            break;
    }
    if (!vf) {
        s_draw_skips[SKIP_RING]++;
        return;
    }
    if (!hud_squeeze(d, slot, (int)nf4, lo, count, v->inl, idx, nidx, (int)topo, vf))
        nv2a_vtx_gather(d, slot, (int)nf4, lo, count, v->inl,
                        (const uint8_t *)xbox_GetMemoryOffset(), vf);
    memcpy(ip, idx, (size_t)nidx * 4u);
    if (!pass_begin(t))
        return;
    cb = s_frame[s_slot].cb;
    if (!(pso = pipeline(vmod, fmod, nf4, topo))) {
        s_draw_skips[SKIP_PSO]++;
        return;
    }
    {
        VkViewport vp;
        float bf[4];
        int dummy = 0;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        vp.x = 0.0f;
        vp.y = (float)t->im.h;
        vp.width = (float)t->im.w;
        vp.height = -(float)t->im.h;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sr);
        vkCmdSetFrontFace(cb, v->front_face != 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                     : VK_FRONT_FACE_CLOCKWISE);
        vkCmdSetCullMode(cb, cull == 1 ? VK_CULL_MODE_BACK_BIT
                           : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
        (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor, g->blend_dfactor,
                                  g->blend_color, bf, &dummy);
        vkCmdSetBlendConstants(cb, bf);
        {
            /* Only with an attachment and when enabled; the compare and op
             * codes are in VkCompareOp / VkStencilOp order. Polygon offset: the
             * constant part is in the vertex stage, the slope part per host
             * pixel, so times render.scale. */
            int z = s_pass_dt && use_z && v->depth_enable;
            int st = s_pass_dt && use_s;
            float slope = (s_pass_dt && use_z && v->poly_offset)
                        ? v->poly_factor * (float)nv2a_host_render_scale_x2() * 0.5f : 0.0f;
            vkCmdSetDepthTestEnable(cb, z ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthWriteEnable(cb, z && v->depth_mask ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthCompareOp(cb, z ? (VkCompareOp)nv2a_cmp_from_gl(v->depth_func)
                                         : VK_COMPARE_OP_ALWAYS);
            vkCmdSetStencilTestEnable(cb, st ? VK_TRUE : VK_FALSE);
            vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK,
                              (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[0]),
                              (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[2]),
                              (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[1]),
                              (VkCompareOp)(v->stencil_func & 7u));
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, v->stencil_rmask & 0xFFu);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, v->stencil_wmask & 0xFFu);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK,
                                     st ? (v->stencil_ref & 0xFFu) : 0u);
            vkCmdSetDepthBiasEnable(cb, slope != 0.0f ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthBias(cb, 0.0f, 0.0f, slope);
        }
    }
    dyn_off[0] = (uint32_t)uo[0];
    dyn_off[1] = (uint32_t)uo[1];
    dyn_off[2] = (uint32_t)uo[2];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1,
                            &s_frame[s_slot].set0, 3, dyn_off);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1,
                            &tset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_frame[s_slot].ring[NVR_RING_VB].buf, &vb_off);
    vkCmdBindIndexBuffer(cb, s_frame[s_slot].ring[NVR_RING_IB].buf, ib_off,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, nidx, 1, 0, 0, 0);
    t->dirty = 1;
    nv2a_rt_own_drawn(&t->own, (uint32_t)s_flips);
    t->gen = ++s_gen;
    if (t->dt && use_z && (v->depth_mask || use_s))
        t->dt->gen = t->gen;
    s_draws++;
}

/* ---- visibility tests: the shared tracker (nv2a_occ) with occlusion queries --
 *
 * A query is one per render pass a stretch spans. Slots are reset from the host
 * and go back once the submit that used them is done. */
#define NVR_VIS_SLOTS  8192
#define NVR_OCC_QSLOTS 16
#define NVR_OCC_POOL   (NV2A_OCC_PENDING * 2 + NV2A_OCC_SEG)

typedef struct NvrOccQuery {
    int      slot[NVR_OCC_QSLOTS];
    uint64_t serial[NVR_OCC_QSLOTS];   /* the submit it is recorded in */
    int      n, ovf;
    struct NvrOccQuery *next;          /* free list */
} NvrOccQuery;

static VkQueryPool   s_vkq;
static int           s_qprecise;
static NvrOccQuery   s_qpool[NVR_OCC_POOL], *s_qfree;
static int           s_qmade;
static unsigned char s_vis_used[NVR_VIS_SLOTS];
static int           s_vis_next;
static int           s_retire[NVR_VIS_SLOTS];
static uint64_t      s_retire_serial[NVR_VIS_SLOTS];
static int           s_nretire;
static struct nv2a_occ s_occ;
static uint64_t      s_report_defer_ns, s_occ_ovf;

static uint64_t now_ns_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The newest submit known done: everything before one still running. */
static uint64_t serial_done(void)
{
    uint64_t done = s_next_serial - 1;
    int f;
    for (f = 0; f < NVR_FRAMES; f++) {
        NvrFrame *fr = &s_frame[f];
        if (fr->busy && fr->serial && fr->serial <= done
                && vkWaitForFences(s_dev, 1, &fr->fence, VK_TRUE, 0) != VK_SUCCESS)
            done = fr->serial - 1;
    }
    return done;
}

static int vis_alloc(void)
{
    int i, k;
    for (k = 0; k < NVR_VIS_SLOTS; k++) {
        i = (s_vis_next + k) % NVR_VIS_SLOTS;
        if (!s_vis_used[i]) {
            s_vis_used[i] = 1;
            s_vis_next = (i + 1) % NVR_VIS_SLOTS;
            vkResetQueryPool(s_dev, s_vkq, (uint32_t)i, 1);
            return i;
        }
    }
    return -1;
}

static void occ_query_begin(void)
{
    NvrOccQuery *q = s_occ_q;
    int i;
    if (!q || !s_pass_rt || s_occ_slot >= 0)
        return;
    if (q->n >= NVR_OCC_QSLOTS || (i = vis_alloc()) < 0) {
        q->ovf = 1;
        s_occ_ovf++;
        return;
    }
    q->slot[q->n] = i;
    q->serial[q->n] = s_next_serial;
    q->n++;
    vkCmdBeginQuery(s_frame[s_slot].cb, s_vkq, (uint32_t)i,
                    s_qprecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_occ_slot = i;
}

static void occ_pass_end(void)
{
    if (s_occ_slot >= 0 && s_pass_rt)
        vkCmdEndQuery(s_frame[s_slot].cb, s_vkq, (uint32_t)s_occ_slot);
    s_occ_slot = -1;
}

static uint64_t occ_now_us(void *ctx)
{
    (void)ctx;
    return now_ns_() / 1000u;
}

static void *occ_get(void *ctx)
{
    NvrOccQuery *q;
    (void)ctx;
    if (s_qfree) {
        q = s_qfree;
        s_qfree = q->next;
    } else if (s_qmade < NVR_OCC_POOL) {
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
    NvrOccQuery *q = (NvrOccQuery *)qv;
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
    s_occ_q = (NvrOccQuery *)qv;
    occ_query_begin();
}

static void occ_end(void *ctx, void *qv)
{
    (void)ctx;
    if (s_occ_q == (NvrOccQuery *)qv) {
        occ_pass_end();
        s_occ_q = NULL;
    }
}

static int occ_result(void *ctx, void *qv, int flush_gpu, uint64_t *count)
{
    NvrOccQuery *q = (NvrOccQuery *)qv;
    uint64_t sum = 0;
    int i;
    (void)ctx;
    for (i = 0; i < q->n; i++) {
        if (q->serial[i] == s_next_serial) {     /* not submitted yet */
            if (!flush_gpu)
                return 0;
            frame_submit(0, "nvr occlusion");
        }
    }
    /* Submitted: the query pool says which counts are there. */
    for (i = 0; i < q->n; i++) {
        uint64_t v = 0;
        if (vkGetQueryPoolResults(s_dev, s_vkq, (uint32_t)q->slot[i], 1, sizeof v, &v, sizeof v,
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return 0;
        sum += v;
    }
    *count = q->ovf ? NV2A_OCC_VISIBLE : sum;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static int occ_ready_on(void)
{
    if (!s_ready || !s_vkq)
        return 0;
    if (!s_occ.ops) {
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
        nv2a_occ_set_scale_x2(&s_occ, nv2a_host_render_scale_x2());
    }
    return 1;
}

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
}

static void nvr_on_zpass(int enable)
{
    if (occ_ready_on())
        nv2a_occ_zpass(&s_occ, enable);
}

static void nvr_on_zpass_clear(void)
{
    if (occ_ready_on())
        nv2a_occ_zpass_clear(&s_occ);
}

/* The open pass isn't ended for a report: its counts go with the next submit,
 * at the latest half a frame on (on_poll), if the title seems to wait. */
static int nvr_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    nv2a_occ_report(&s_occ, va, 0);
    if (s_rec && !s_report_defer) {
        s_report_defer = 1;
        s_report_defer_ns = now_ns_();
    }
    return 1;
}

static void nvr_on_poll(void)
{
    if (!s_occ.npend && !s_nretire)
        return;
    if (s_report_defer && now_ns_() - s_report_defer_ns > 8000000ull)
        frame_submit(0, "nvr report");
    occ_after_wait();
    if (s_occ.npend)
        nv2a_occ_poll(&s_occ, !s_report_defer);
}

static void occ_init(void)
{
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceFeatures2 f2;
    memset(&f12, 0, sizeof f12);
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    memset(&f2, 0, sizeof f2);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(C->phys, &f2);
    s_qprecise = f2.features.occlusionQueryPrecise ? 1 : 0;
    if (f12.hostQueryReset) {
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = NVR_VIS_SLOTS;
        if (vkCreateQueryPool(s_dev, &qi, NULL, &s_vkq) != VK_SUCCESS)
            s_vkq = VK_NULL_HANDLE;
    }
}

/* ---- flip ---------------------------------------------------------------- */

/* The target a flip shows: the one at its offset, grown when the display's
 * mode is larger (a title that clips every draw to the TV-safe area made it
 * from its clip). */
static NvrTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    if (!surface_offset)
        return NULL;
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *t = &s_rt[i];
        if (t->im.img && !t->scratch && t->offset == surface_offset
                && (!p->offset || (t->w >= p->w && t->h >= p->h)))
            return t;
    }
    for (i = 0; p->offset && i < NVR_TARGETS; i++) {
        NvrTarget *t = &s_rt[i];
        if (t->im.img && !t->scratch && t->offset == surface_offset && t->pitch == p->pitch
                && t->w <= p->w && t->h <= p->h) {
            uint32_t pitch = t->pitch, bpp = t->bpp, cfmt = t->cfmt, scale = t->scale;
            int swz = t->swz;
            pass_end();
            if (t->dirty)
                nvr_sync_target(t);
            target_free(t);
            return target_make(surface_offset, p->w, p->h, pitch, bpp, swz, cfmt, scale);
        }
    }
    return NULL;
}

/* Target sizes the title reads with the CPU (RECOMP_GPU_CPU_READS), written
 * back at every flip. */
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
    }
    for (i = 0; i < NVR_GHOSTS && n > 0; i++) {
        NvrTarget *g = &s_ghost[i];
        for (k = 0; g->im.img && k < n; k++)
            if (g->w == size[k][0] && g->h == size[k][1]) {
                ghost_flush(g);
                break;
            }
    }
    for (i = 0; i < NVR_TARGETS && n > 0; i++) {
        NvrTarget *t = &s_rt[i];
        if (!t->im.img || t->scratch || !t->dirty || t->swz)
            continue;
        for (k = 0; k < n; k++)
            if (t->w == size[k][0] && t->h == size[k][1]) {
                nvr_sync_target(t);
                break;
            }
    }
}

/* The frame into the window's back slot on the GPU, in this frame's command
 * buffer; published once submitted, so the present that shows it comes after
 * it on the queue. */
static int present_to_slot(NvrTarget *t, uint32_t hcw, uint32_t hch)
{
    struct nvk_image *slot = (struct nvk_image *)xbox_FramebufferWindowBackTexture(hcw, hch);
    VkCommandBuffer cb;
    VkImageCopy ic;

    if (!slot || slot->format != t->im.format)
        return 0;
    pass_end();
    if (!(cb = frame_cb()))
        return 0;
    nvk_image_to(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    /* GENERAL to GENERAL: after the last present that read it. */
    nvk_image_barrier(cb, slot->img, VK_IMAGE_ASPECT_COLOR_BIT,
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
    vkCmdCopyImage(cb, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   slot->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    frame_submit(0, "nvr flip");
    xbox_FramebufferWindowPublishTexture();
    return 1;
}

static void nvr_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;
    uint32_t hcw = 0, hch = 0;
    const uint8_t *hpx = NULL;
    NvrTarget *t = NULL;
    int win = 0, layer = 0;

    if (s_ready) {
        cpu_reads_sync();
        t = present_target(surface_offset);
        if (t) {
            const struct nv2a_pb_present *pp = nv2a_pb_present_state();
            uint32_t cw = t->w, ch = t->h;
            if (pp->offset && pp->offset == t->offset && pp->w && pp->h) {
                if (pp->w < cw) cw = pp->w;
                if (pp->h < ch) ch = pp->h;
            }
            /* What of it the title drew in (present.crop). */
            if (pp->offset == t->offset && pp->draw_x1 && pp->draw_y1)
                nv2a_present_set_drawn(pp->draw_x0, pp->draw_y0,
                                       pp->draw_x1 < cw ? pp->draw_x1 : cw,
                                       pp->draw_y1 < ch ? pp->draw_y1 : ch, cw, ch);
            else
                nv2a_present_set_drawn(0, 0, cw, ch, cw, ch);
            hcw = cw * t->scale;
            hch = ch * t->scale;
        }
        win = t && xbox_FramebufferWindowRunning();
        layer = win && xbox_FramebufferWindowVulkan() && present_to_slot(t, hcw, hch);
        if (win && !layer)
            hpx = readback(t);
        frame_submit(0, "nvr flip");
        if (++s_flips % 300 == 0)
            fprintf(stderr, LOGP "flip %llu: %llu clears, %llu draws (skipped: %llu masked,"
                    " %llu no target, %llu no program, %llu no pipeline, %llu ring), %llu seeds,"
                    " %llu read-backs\n", (unsigned long long)s_flips,
                    (unsigned long long)s_clears, (unsigned long long)s_draws,
                    (unsigned long long)s_draw_skips[SKIP_MASKED],
                    (unsigned long long)s_draw_skips[SKIP_NO_RT],
                    (unsigned long long)s_draw_skips[SKIP_NO_VS],
                    (unsigned long long)s_draw_skips[SKIP_PSO],
                    (unsigned long long)s_draw_skips[SKIP_RING],
                    (unsigned long long)s_seeds, (unsigned long long)s_readbacks);
    }
    if (!pitch || !addr || layer)
        return;
    xbox_FramebufferWindowSet(addr, pitch);
    /* No Vulkan window: the frame's part of the host pixels. A surface never
     * drawn here goes as the CPU path presents it. */
    if (t && hpx && win) {
        xbox_FramebufferWindowPresentPixels(hpx, hcw, hch, t->im.w * 4u, 4u);
        return;
    }
    xbox_FramebufferWindowPresent(addr, pitch);
}

/* FLIP_STALL reached: what the frame recorded goes to the GPU while the
 * executor holds for the vblank. */
static void nvr_on_flip_ready(void)
{
    if (s_ready)
        frame_submit(0, "nvr flip ready");
}

static NvrTarget *target_at(uint32_t offset, uint32_t w, uint32_t h)
{
    uint32_t addr = nv2a_pb_dma_resolve(offset);
    NvrTarget *best = NULL;
    int i;
    for (i = 0; i < NVR_TARGETS; i++) {
        NvrTarget *t = &s_rt[i];
        if (!t->im.img || t->scratch || (t->offset != offset && t->addr != addr)
                || t->w < w || t->h < h)
            continue;
        if (!best || t->last_use > best->last_use)
            best = t;
    }
    return best;
}

static void nvr_sync_guest(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h)
{
    NvrTarget *t;
    if (!s_ready)
        return;
    {
        uint32_t lo = nv2a_pb_dma_resolve(offset) & 0x07FFFFFFu;
        ghost_flush_range(lo, lo + (pitch ? pitch : w * 4u) * h);
    }
    if ((t = target_at(offset, w, h)) != NULL)
        nvr_sync_target(t);
}

static const struct nv2a_pb_backend s_nvr = {
    nvr_on_clear,
    nvr_on_zclear,
    nvr_on_draw,
    nvr_on_flip,
    nvr_on_zpass,
    nvr_on_zpass_clear,
    nvr_on_report,
    nvr_on_poll,
    1,                           /* writes back: dumps read guest memory */
    nvr_sync_guest,
    nvr_on_flip_ready,
};

const struct nv2a_pb_backend *nvr_backend(void)
{
    return init() ? &s_nvr : NULL;
}

void nvr_print_stats(void)
{
    fprintf(stderr, LOGP "%llu flips, %llu clears, %llu draws; skipped: %llu masked, %llu no"
            " indices, %llu no target, %llu no program, %llu no primitive, %llu no pipeline,"
            " %llu ring, %llu empty scissor; %llu seeds, %llu read-backs, %llu texture uploads,"
            " %llu targets sampled, %llu textures over targets written back\n",
            (unsigned long long)s_flips, (unsigned long long)s_clears,
            (unsigned long long)s_draws, (unsigned long long)s_draw_skips[SKIP_MASKED],
            (unsigned long long)s_draw_skips[SKIP_NO_IDX],
            (unsigned long long)s_draw_skips[SKIP_NO_RT],
            (unsigned long long)s_draw_skips[SKIP_NO_VS],
            (unsigned long long)s_draw_skips[SKIP_NO_PRIM],
            (unsigned long long)s_draw_skips[SKIP_PSO], (unsigned long long)s_draw_skips[SKIP_RING],
            (unsigned long long)s_draw_skips[SKIP_SCISSOR], (unsigned long long)s_seeds,
            (unsigned long long)s_readbacks, (unsigned long long)s_tex_uploads,
            (unsigned long long)s_rtt_binds, (unsigned long long)s_tex_syncs);
    fprintf(stderr, LOGP "ghosts: %llu kept, %llu revived, %llu copied from, %llu written back\n",
            (unsigned long long)s_ghost_kept, (unsigned long long)s_ghost_revived,
            (unsigned long long)s_ghost_copies, (unsigned long long)s_ghost_flushes);
}

const uint8_t *nvr_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh)
{
    NvrTarget *t;
    if (!s_ready || !(t = target_at(offset, 1, 1)))
        return NULL;
    *hw = t->im.w;
    *hh = t->im.h;
    return readback(t);
}

void nvr_register_from_env(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    if (b && !strcasecmp(b, "native")) {
        const struct nv2a_pb_backend *nb = nvr_backend();
        if (!nb) {
            fprintf(stderr, LOGP "RECOMP_PB_BACKEND=native: no device, staying on the"
                    " CPU rasteriser\n");
            return;
        }
        nv2a_pb_set_backend(nb);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=native: the native renderer draws\n");
    }
}

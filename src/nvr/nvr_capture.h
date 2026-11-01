/*
 * nvr_capture.h - frame capture for the native Vulkan renderer's checks
 * (ANTBOX.md, "Native Vulkan renderer"). RECOMP_NVR_CAPTURE=<file> records
 * every push-buffer segment the GPU walks and the guest memory the title
 * changed before it, so tools/nvr_replay can run the same frames through
 * any backend, headless, with no timing in the way.
 *
 * The file: "NVRCAP01", then records of a u32 type and its body:
 *   NVRCAP_SPACE  u32 space, u32 guest base, u64 size     (once per space)
 *   NVRCAP_PAGE   u32 space, u32 offset, 4096 bytes        (a page's content)
 *   NVRCAP_KICK   u32 from, u32 to                         (nv2a_pb_scan's)
 *   NVRCAP_STATE  u32 contiguous high-water mark, u32 image lo, u32 image hi,
 *                 u32 the executor's flips so far (a replay that counts
 *                 differently walked something else; 0x7FFFFFFF in captures
 *                 written before the count was kept: a 12-byte body)
 *                 (before a kick, when they changed: the executor reads them
 *                 to tell a physical offset in the contiguous window from
 *                 one in RAM, dma_resolve)
 *   NVRCAP_GAMMA  768 bytes: the gamma ramp (xbox_GammaRamp), before a kick,
 *                 when it changed: the title's or the port's, applied where
 *                 the frame is shown and dumped
 *   NVRCAP_SWM    u32 the method's parameter: a software method ran during
 *                 the kick before it, and the PAGE records just before this
 *                 one are what its dispatcher wrote (the walk reads them
 *                 next; a replay applies them in place of the dispatcher)
 *   NVRCAP_ENV    u32 length, "NAME=value" (no terminator): a CONFIG-tier
 *                 runtime setting the run had (recomp_env.h: render.scale,
 *                 graphics.aniso, GPU_CPU_READS, ...), all before the first
 *                 kick; DEBUG and TRACE ones are left out (dumps, captures,
 *                 pads)
 *   NVRCAP_HUD    u32 vertex program hash, f32 k: nv2a_hud_squeeze, before a
 *                 kick, when it changed (the port's call, not in the buffer)
 *   NVRCAP_END
 * Spaces are xbox_MemoryViews' (0 RAM, 1 contiguous, 2 NV2A registers).
 * The first kick writes every non-zero page; after that, the pages written
 * since the last kick (userfaultfd's async write protection, read and
 * re-armed by PAGEMAP_SCAN), each only when its bytes changed. Linux 6.7
 * or later only; elsewhere the calls do nothing.
 */
#ifndef NVR_CAPTURE_H
#define NVR_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NVRCAP_SPACE = 1,
    NVRCAP_PAGE  = 2,
    NVRCAP_KICK  = 3,
    NVRCAP_END   = 4,
    NVRCAP_STATE = 5,
    NVRCAP_GAMMA = 6,
    NVRCAP_STATE2 = 7,                  /* NVRCAP_STATE with the flip count */
    NVRCAP_SWM   = 8,
    NVRCAP_ENV   = 9,
    NVRCAP_HUD   = 10,
};
#define NVRCAP_MAGIC "NVRCAP01"
#define NVRCAP_PAGE_SIZE 4096u

/* On the GPU thread, before nv2a_pb_scan(from, to): records the memory the
 * title changed, then the kick. Cheap when capture is off. */
void nvr_capture_kick(uint32_t from, uint32_t to);
/* On the GPU thread, after the title's software-method dispatcher ran
 * (kernel_bridge.c): the pages it wrote, then an NVRCAP_SWM record. */
void nvr_capture_swm(uint32_t param);
/* The backend wrote guest memory itself (a render target's write-back):
 * guest VA addr, len bytes. A replay's backend writes the same, or none if
 * the live run's write-back had a reason the replay lacks; either way the
 * pages are taken as known, and reach the file only if the title changes
 * them afterwards. */
void nvr_capture_gpu_wrote(uint32_t addr, uint32_t len);
/* Ends the file (also at exit). */
void nvr_capture_close(void);
/* The same from a path that may not wait (the watchdog's _exit): does
 * nothing when the GPU thread holds the capture. */
void nvr_capture_close_nowait(void);

#ifdef __cplusplus
}
#endif

#endif

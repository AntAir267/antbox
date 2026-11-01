/**
 * The Vulkan device the pushbuffer executor's Vulkan backend (nv2a_pb_vulkan.c)
 * draws with and the SDL window's Vulkan presenter (src/video/
 * fb_present_vulkan.c) shows frames with: one instance, one physical device,
 * one logical device, one graphics queue.
 *
 * The window is made first, on the process main thread, before the guest
 * starts: its presenter calls nvk_core_init() with the instance extensions
 * SDL wants and a callback that makes the surface (SDL_Vulkan_CreateSurface),
 * so the device is picked for a queue that can present there. The backend
 * initialises lazily on the NV2A ack thread and adopts that device; headless,
 * it calls nvk_core_init() itself, without a surface.
 *
 * The queue is shared by two threads (the ack thread's submits, the main
 * thread's presents), so every vkQueueSubmit and vkQueuePresentKHR on it
 * holds nvk_queue_lock(). Submission order on the one queue is what orders
 * a backend blit into a window slot before the present that reads it.
 *
 * Requires Vulkan 1.3 (dynamic rendering, synchronization2, extended dynamic
 * state, timeline semaphores): RADV on the Steam Deck and MoltenVK 1.2.11+.
 */
#ifndef XBOXRECOMP_NV2A_VK_CORE_H
#define XBOXRECOMP_NV2A_VK_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvk_core {
    VkInstance                       inst;
    VkPhysicalDevice                 phys;
    VkDevice                         dev;
    VkQueue                          queue;
    uint32_t                         qfam;
    VkSurfaceKHR                     surface;   /* VK_NULL_HANDLE headless */
    VkPhysicalDeviceProperties       props;
    VkPhysicalDeviceMemoryProperties mem;
    VkFormat                         depth_format;   /* D32S8, else D24S8 */
    int                              portability;    /* MoltenVK */
    int                              validation;     /* layers on */
    int                              present_wait;   /* VK_KHR_present_id + _wait on, or the pair below */
    int                              present_wait2;  /* VK_KHR_present_id2 + _wait2 instead (Windows, BO-207) */
    int                              display_timing; /* VK_GOOGLE_display_timing on: the refresh cycle (gamescope, BO-210) */
    int                              surface_caps2;  /* VK_KHR_get_surface_capabilities2 on (BO-207) */
    int                              colorspace_ext; /* VK_EXT_swapchain_colorspace on (BO-110) */
    int                              hdr_metadata;   /* VK_EXT_hdr_metadata on (BO-110) */
    int                              demote;         /* shaderDemoteToHelperInvocation on: SPIR-V 1.6's discard */
    float                            max_aniso;      /* samplerAnisotropy on: the limit; else 0 */
    char                             name[256];
};

/* The device, made once. inst_exts/n: extensions the window system needs
 * (SDL_Vulkan_GetInstanceExtensions), NULL/0 headless. make_surface, when
 * given, is called with the new instance and must return the window's
 * surface; the device is then picked for a queue family that presents to it
 * and gets VK_KHR_swapchain. Returns the core, or NULL with a reason in err
 * (and on any later call, the same result: init runs once). Thread-safe. */
const struct nvk_core *nvk_core_init(const char *const *inst_exts, unsigned n,
                                     VkSurfaceKHR (*make_surface)(VkInstance, void *),
                                     void *ctx, char *err, size_t errn);
/* A window whose native surface was taken away and given back (an Android
 * app coming back from the background): the old VkSurfaceKHR is destroyed
 * and make_surface asked for a new one, which the core then holds. The
 * caller has destroyed the swapchain first. 0 when no surface could be made
 * (the core then has none; call again later). */
int nvk_core_replace_surface(VkSurfaceKHR (*make_surface)(VkInstance, void *), void *ctx);

/* The core if nvk_core_init has succeeded, else NULL. */
const struct nvk_core *nvk_core_get(void);
/* 1 when the device is a software renderer (llvmpipe, lavapipe, SwiftShader):
 * the machine's GPU could not be used. 0 before the device is up. */
int nvk_core_is_software(void);

/* The device is lost (VK_ERROR_DEVICE_LOST from a submit or a present, or a
 * swapchain that cannot be made any more): said once in the log, and passed
 * to whoever shows the picture (nvk_set_lost_handler: the window, which tells
 * the player and ends the run the ordinary way). Without a handler, a run
 * that has no window: the process ends with status 70 rather than go on
 * drawing nothing. Any thread. */
void nvk_device_lost(const char *why);
void nvk_set_lost_handler(void (*fn)(const char *why));

/* The way out (Android: every quit and restart, before the process ends;
 * fb_present_sdl.c). nvk_backend_quiesce first: the backend's pre-warm
 * thread stops and its pipeline cache is written; 0 when that thread did
 * not stop within timeout_ms (the device must then be left alone). Then,
 * with the executor held (xbox_HostHoldForExit) and the window's swapchain
 * gone: nvk_core_shutdown cancels the GPU waits still going
 * (nvk_wait_cancel), takes the queue lock for good, waits for the
 * device to be idle and destroys the surface, the device and the instance,
 * each step in the log. Objects the backend still holds go with the device
 * (the process ends next). */
int  nvk_backend_quiesce(int timeout_ms);
void nvk_core_shutdown(void);

void nvk_queue_lock(void);
void nvk_queue_unlock(void);

/* BO-128: the queue's submits that carried command buffers, counted and the
 * last 64 kept with a line from the submitter on what they held. Called
 * under nvk_queue_lock right after a vkQueueSubmit that succeeded with at
 * least one command buffer; returns the submit's ordinal (1 = the first).
 * Turnip on KGSL makes one kernel command batch of each such submit and none
 * of an empty one, so the ordinal is the context timestamp the kernel names
 * in 'gpu fault ctx N ... ts T' (both count from the device's creation).
 * nvk_device_lost prints the kept ones. */
uint64_t nvk_submit_note(const char *what);

/* Bounded GPU waits. A fence or timeline wait is made in slices; a slice
 * that times out (VK_TIMEOUT) is waited on again until a total limit, on
 * the monotonic clock (RECOMP_DEBUG=vk_wait_limit=<ms>, default 10 s), or
 * until the wait is cancelled (nvk_wait_cancel, a lost device reported from
 * any thread, the process's exit). The first slice is the whole cost of a
 * wait that succeeds: nothing else is waited for. Each kind of failure is
 * logged on its own: done after timeouts (with the time taken), timed out
 * for good, device lost, another VkResult, cancelled.
 *
 * Only NVK_WAIT_OK proves the GPU is done: on anything else the caller does
 * not reset, reuse, free or read what the wait was for, and passes the
 * outcome to nvk_wait_fatal, which ends drawing for the run through
 * nvk_device_lost (a cancelled wait is not reported: the run is ending).
 *
 * Sites (RECOMP_DEBUG=vk_wait_fault=<site>:<kind>[:<n>], test hook): the
 * waits at a site count; the one after n of them (default 300) is made to
 * fail as asked: retry (two slices time out, then the real wait), timeout
 * (every slice times out: the limit is reached), lost (VK_ERROR_DEVICE_LOST). */
enum nvk_wait_site {
    NVK_WAIT_SERIAL,      /* backend: a submit's serial (flush, ring, readback) */
    NVK_WAIT_FRAME,       /* backend: a frame's slot, reused two flips on */
    NVK_WAIT_UPLOAD,      /* window: the CPU frame's upload */
    NVK_WAIT_PRESENT,     /* window: the present's frame fence */
    NVK_WAIT_SHOT,        /* window: a window shot's readback */
    NVK_WAIT_SLOT,        /* window: a slot that changes size */
    NVK_WAIT_SITES
};
enum nvk_wait_outcome {
    NVK_WAIT_OK = 0,
    NVK_WAIT_TIMEOUT,     /* still not done at the limit */
    NVK_WAIT_LOST,        /* VK_ERROR_DEVICE_LOST */
    NVK_WAIT_ERROR,       /* another VkResult */
    NVK_WAIT_CANCELLED
};
/* Every fence in `fence`, as vkWaitForFences(waitAll). *res (may be NULL):
 * the last VkResult. */
int  nvk_wait_fence(int site, const char *what, VkFence fence, VkResult *res);
/* The timeline semaphore at `value` or past it. */
int  nvk_wait_timeline(int site, const char *what, VkSemaphore sem, uint64_t value,
                       VkResult *res);
/* Everything submitted to the queue so far, as vkQueueWaitIdle but bounded:
 * an empty submit that signals `fence` (the caller's, not pending; reset
 * here, under the queue lock), then nvk_wait_fence. A submit that fails is
 * NVK_WAIT_LOST or NVK_WAIT_ERROR and leaves the fence unsignalled with
 * nothing pending on it. */
int  nvk_wait_queue(int site, const char *what, VkFence fence, VkResult *res);
/* An outcome other than OK or CANCELLED: the device is reported lost
 * (nvk_device_lost) with what and why. Returns 1 for those, 0 otherwise. */
int  nvk_wait_fatal(int outcome, VkResult res, const char *what);
void nvk_wait_cancel(void);
/* 1 once nvk_device_lost has been called. */
int  nvk_core_lost(void);

/* A memory type index for `bits` with all of `want`, preferring one that
 * also has `prefer`; -1 if none. */
int nvk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer);

/* A buffer with its own memory, mapped when host visible. */
struct nvk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    void          *map;
};
int  nvk_buffer_new(struct nvk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer);
void nvk_buffer_free(struct nvk_buffer *b);

/* A 2D image with its own device-local memory and a view of all of it. */
struct nvk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFormat       format;
    VkImageAspectFlags aspect;
    uint32_t       w, h, levels;
    VkImageLayout  layout;      /* as the recorded commands leave it */
    VkDeviceSize   bytes;       /* its memory (nvk_image_bytes) */
};
/* BO-240, BO-245: the memory of every live nvk_image, bytes. */
uint64_t nvk_image_bytes(void);
int  nvk_image_new(struct nvk_image *im, VkFormat fmt, uint32_t w, uint32_t h,
                   uint32_t levels, VkImageUsageFlags usage, VkImageAspectFlags aspect);
void nvk_image_free(struct nvk_image *im);
/* A cube image (six layers) of side x side and its cube view. */
int  nvk_image_new_cube(struct nvk_image *im, VkFormat fmt, uint32_t side, uint32_t levels,
                        VkImageUsageFlags usage);

/* One image layout transition (synchronization2), with the stages and
 * accesses each layout is used with in this renderer. aspect 0: colour. */
void nvk_image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                       VkImageLayout from, VkImageLayout to);
/* nvk_image_barrier on im, from its tracked layout; updates it. A no-op when
 * it is already there, except for TRANSFER_DST and GENERAL, which take a
 * write-after-write barrier anyway. */
void nvk_image_to(VkCommandBuffer cb, struct nvk_image *im, VkImageLayout to);

const char *nvk_result_str(VkResult r);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_VK_CORE_H */

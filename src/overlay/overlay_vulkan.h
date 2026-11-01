/* overlay_vulkan.cpp: the overlay in the swapchain present
 * (fb_present_vulkan.c calls it). */
#ifndef XBOX_OVERLAY_VULKAN_H
#define XBOX_OVERLAY_VULKAN_H

#include <stdint.h>
#include "d3d/nv2a_vk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Records the overlay into cb, which has the swapchain image `view` (w x h,
 * format fmt, count images in the chain) in COLOR_ATTACHMENT_OPTIMAL, and
 * keeps what it draws over. The caller holds no nvk_queue_lock (the renderer
 * uploads its font through the shared queue under it). Draws nothing when
 * there is nothing to show. */
void xbox_OverlayVulkanDraw(const struct nvk_core *core, VkCommandBuffer cb,
                            VkImageView view, uint32_t w, uint32_t h, VkFormat fmt,
                            uint32_t image_count);

#ifdef __cplusplus
}
#endif

#endif

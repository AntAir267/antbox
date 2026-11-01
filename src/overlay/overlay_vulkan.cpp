/*
 * overlay_vulkan.cpp - the overlay on Vulkan: the official imgui_impl_vulkan
 * renderer, drawn with dynamic rendering into the swapchain image after the
 * game's blit (fb_present_vulkan.c). See overlay_internal.h for the order of
 * calls.
 *
 * The renderer uploads its font atlas with a submit of its own on the queue
 * it is given, and the queue is shared with the executor's thread, so that
 * whole call runs under nvk_queue_lock.
 */
#include "overlay_vulkan.h"
#include "overlay_internal.h"

#include "backends/imgui_impl_vulkan.h"

#include <stdio.h>
#include <string.h>

static bool s_init, s_failed;
static VkFormat s_fmt;
static uint32_t s_images;

static void check(VkResult r)
{
    if (r != VK_SUCCESS)
        fprintf(stderr, "[OVERLAY] Vulkan: %s\n", nvk_result_str(r));
}

extern "C" void xbox_OverlayVulkanDraw(const struct nvk_core *core, VkCommandBuffer cb,
                                       VkImageView view, uint32_t w, uint32_t h, VkFormat fmt,
                                       uint32_t image_count)
{
    if (s_failed || !core)
        return;
    /* The context is the main thread's to make (xbox_OverlayPlatformFrame). */
    if (!overlay_core_is_ready())
        return;
    overlay_lock();
    struct unlock_at_end { ~unlock_at_end() { overlay_unlock(); } } unlock_guard;
    if (!s_init) {
        ImGui_ImplVulkan_InitInfo ii{};
        s_fmt = fmt;
        s_images = image_count < 2 ? 2 : image_count;
        ii.ApiVersion = VK_API_VERSION_1_3;
        ii.Instance = core->inst;
        ii.PhysicalDevice = core->phys;
        ii.Device = core->dev;
        ii.QueueFamily = core->qfam;
        ii.Queue = core->queue;
        ii.DescriptorPoolSize = 32;
        ii.MinImageCount = 2;
        ii.ImageCount = s_images;
        ii.UseDynamicRendering = true;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_fmt;
        ii.CheckVkResultFn = check;
        if (!ImGui_ImplVulkan_Init(&ii)) {
            s_failed = true;
            overlay_fail();
            return;
        }
        s_init = true;
        fprintf(stderr, "[OVERLAY] Dear ImGui %s, Vulkan renderer\n", IMGUI_VERSION);
    }
    if (fmt != s_fmt) {
        /* BO-110: the target's format changed (HDR's half-float compose
         * image or the swapchain again): the pipeline is made again, once
         * nothing in flight uses the old one. */
        ImGui_ImplVulkan_PipelineInfo pi{};
        pi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        pi.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        pi.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        s_fmt = fmt;
        pi.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_fmt;
        nvk_queue_lock();
        vkQueueWaitIdle(core->queue);
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
        nvk_queue_unlock();
    }
    ImGui_ImplVulkan_NewFrame();
    overlay_set_target(w, h);
    if (!overlay_begin_frame())
        return;
    ImDrawData *dd = overlay_end_frame();
    if (!dd || dd->CmdListsCount == 0)
        return;

    VkRenderingAttachmentInfo att;
    VkRenderingInfo ri;
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    nvk_queue_lock();
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    vkCmdEndRendering(cb);
    nvk_queue_unlock();
}

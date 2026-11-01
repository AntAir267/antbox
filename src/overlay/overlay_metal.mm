/*
 * overlay_metal.mm - the overlay on Metal: the official imgui_impl_metal
 * renderer, drawn into the present pass's encoder (fb_present_metal.m). See
 * overlay_internal.h for the order of calls. Built with ARC; the presenter
 * is not, so everything crosses as void *.
 */
#include "overlay_metal.h"
#include "overlay_internal.h"

#import <Metal/Metal.h>

#include "backends/imgui_impl_metal.h"

#include <stdio.h>

static bool s_init, s_failed;

extern "C" void xbox_OverlayMetalDraw(void *device, void *cmdbuf, void *rpdesc, void *encoder)
{
    if (s_failed)
        return;
    /* The context is the main thread's to make (xbox_OverlayPlatformFrame). */
    if (!overlay_core_is_ready())
        return;
    overlay_lock();
    struct unlock_at_end { ~unlock_at_end() { overlay_unlock(); } } unlock_guard;
    if (!s_init) {
        ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device);
        s_init = true;
        fprintf(stderr, "[OVERLAY] Dear ImGui %s, Metal renderer\n", IMGUI_VERSION);
    }
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor *)rpdesc);
    if (!overlay_begin_frame())
        return;
    ImDrawData *dd = overlay_end_frame();
    if (dd && dd->CmdListsCount > 0)
        ImGui_ImplMetal_RenderDrawData(dd, (__bridge id<MTLCommandBuffer>)cmdbuf,
                                       (__bridge id<MTLRenderCommandEncoder>)encoder);
}

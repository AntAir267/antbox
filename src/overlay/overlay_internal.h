/*
 * overlay_internal.h - between overlay_ui.cpp (the pages, the input, the
 * ImGui frame) and a graphics backend (overlay_metal.mm, overlay_vulkan.cpp).
 *
 * A backend provides:
 *   - its init, with the objects the official imgui_impl_* renderer wants;
 *   - a draw that runs, in this order,
 *         the renderer's NewFrame, overlay_begin_frame(), overlay_end_frame(),
 *         the renderer's RenderDrawData
 *     with the draw data overlay_end_frame returns.
 * overlay_begin_frame does ImGui::NewFrame and builds the pages (ImGui's
 * SDL half runs on the main thread, xbox_OverlayPlatformFrame); it is false when there is nothing to draw (the backend
 * then skips the rest). overlay_end_frame is ImGui::Render.
 */
#ifndef XBOX_OVERLAY_INTERNAL_H
#define XBOX_OVERLAY_INTERNAL_H

#include <stdint.h>
#include "imgui.h"

/* The ImGui context and the SDL platform backend, made at the first draw.
 * `vulkan` selects ImGui_ImplSDL2_InitForVulkan. */
bool overlay_core_init(bool vulkan);
/* Made by the main thread (xbox_OverlayPlatformFrame): 1 once it is. */
bool overlay_core_is_ready();
/* A backend's draw holds this from its first ImGui call to its last: the
 * main thread hands events over under it (overlay_ui.cpp). */
void overlay_lock();
void overlay_unlock();
/* A backend could not start: the overlay turns itself off (an open one is
 * closed), so the title is not left paused behind nothing. */
void overlay_fail();
/* The size in pixels of the image the backend draws into (before
 * overlay_begin_frame; a backend that does not call it gets SDL's scale). */
void overlay_set_target(uint32_t w, uint32_t h);
bool overlay_begin_frame();
ImDrawData *overlay_end_frame();

#endif

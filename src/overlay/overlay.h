/*
 * overlay.h - the in-game settings overlay (Dear ImGui), as the window's
 * presenter sees it.
 *
 * The presenter (src/video/fb_present_sdl.c and its Metal and Vulkan halves)
 * owns the window and its one event loop on the process main thread, and
 * presents on a thread of its own (BO-210). The overlay's events, its poll
 * and ImGui's SDL half come from the main thread, its frame is built and
 * drawn on the present thread; one mutex in overlay_ui.cpp keeps ImGui to
 * one thread at a time.
 *
 *   events    every SDL event of the main loop goes to xbox_OverlayEvent
 *             first. It watches for the hotkeys (Back+Start on a pad, F1,
 *             Cmd+, on macOS); Esc or B closes. While it is open the title
 *             reads no pad or key (xbox_InputSetBlocked) and is paused
 *             (xbox_HostPause); see overlay_ui.cpp.
 *   drawing   after the game's image is in the drawable and before it is
 *             presented, the presenter calls its backend's draw
 *             (overlay_metal.h, overlay_vulkan.h), when xbox_OverlayWants.
 *   backends  one source file per graphics API, each a few calls around the
 *             official imgui_impl_* renderer; D3D11 adds one more
 *             (overlay_internal.h lists what a backend provides).
 *
 * RECOMP_OVERLAY=0 turns the whole thing off; =open starts it open (for
 * screenshots). The settings it edits are xbox_settings.h.
 */
#ifndef XBOX_OVERLAY_H
#define XBOX_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once, after the window exists. 0 when the overlay is off. */
int  xbox_OverlaySetWindow(SDL_Window *win);

/* Every event of the main loop, before anything else uses it. Returns 1 when
 * the overlay is open and took it. */
int  xbox_OverlayEvent(const SDL_Event *ev);

/* Once per turn of the main loop: held directions repeat, a saved change
 * reaches the file, the window follows the fullscreen setting. */
void xbox_OverlayPoll(void);

/* Once per turn of the main loop, after its events: ImGui's SDL half (the
 * window's size, the mouse, the cursor), which must run on the window's
 * thread, for the next frame the present thread draws; at the first call
 * also the ImGui context. `vulkan`: the Vulkan presenter (else Metal). */
void xbox_OverlayPlatformFrame(int vulkan);

/* Open, fading, or showing the frame-time graph: the presenter draws it, and
 * while it is open (xbox_OverlayOpen) it presents on every turn, not only on
 * a new game frame. */
int  xbox_OverlayWants(void);
int  xbox_OverlayOpen(void);
/* A line of text is up without the overlay being open ("Preparing shaders":
 * the render thread waits for the start-up pre-warm, nv2a_pcache.h, and no
 * game frame comes meanwhile): the presenter presents on every turn so that
 * it shows, over the last frame or over black. */
int  xbox_OverlayNotice(void);

/* The window showed a new frame (the graph counts presents). */
void xbox_OverlayFramePresented(void);

/* The loop is ending: the title is let go, the settings written. */
void xbox_OverlayShutdown(void);

/* Rows of the host's own on the Graphics page, under the settings: up to
 * four actions whose text and effect are the port's (Android: which Vulkan
 * driver the next start loads, a restart). fn(row, 0) gives the row's text
 * as it now stands (NULL or "": no such row at the moment; the pointer must
 * stay good until the next call for that row); fn(row, 1) is the row
 * activated, on the overlay's thread, and returns the text after it. */
#define XBOX_OVERLAY_HOST_ROWS 4
void xbox_OverlaySetHostRows(int n, const char *(*fn)(int row, int activate));

/* Tests (overlay_shot): the place on the open page of the row whose text
 * starts with `prefix` (0 is the first), -1 when the page has none. */
int xbox_OverlayTestRowIndex(const char *prefix);
/* Tests (overlay_shot): whether the open page has a row whose text starts
 * with `prefix`, and the cursor put on it (1 when there is one). */
int xbox_OverlayTestHasRow(const char *prefix);
int xbox_OverlayTestSelectRow(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_H */

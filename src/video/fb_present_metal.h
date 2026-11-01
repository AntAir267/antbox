/* fb_present_metal.m: the CAMetalLayer present of the SDL window (macOS).
 * Called by fb_present_sdl.c only; see that file and fb_present_metal.m. */
#ifndef FB_PRESENT_METAL_H
#define FB_PRESENT_METAL_H

#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

/* Device, queue, pipeline and the window's layer; 0 with a reason in err. */
int   fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn);
void  fbm_destroy(void);
/* RECOMP_VBLANK_SYNC=display: report win's display refreshes (NULL: the
 * main display) to the vblank timer. */
void  fbm_vblank_link(SDL_Window *win);
/* 0 while the window is wholly covered or on another Space. */
int   fbm_visible(SDL_Window *win);
/* The main thread's share of the layer, each turn and before the first
 * present (fb_present_metal.m). */
void  fbm_main_tick(SDL_Window *win);
void  fbm_get(void **device, void **queue);
/* Slot k (0..2) at w x h: its id<MTLTexture>, or NULL. */
void *fbm_slot(int k, uint32_t w, uint32_t h);
int   fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                 uint32_t bpp);
/* Slot k into the window. vblank_ns: the emulated vblank of the game frame
 * this present shows for the first time (present_cadence.h), 0 for a redraw.
 * 1 presented, 0 no drawable (try again), 2 the frame was skipped: two
 * earlier presents had not reached the glass yet (display.present_timing =
 * auto never waits for a drawable). */
/* BO-110: slot k's scene snapshot texture (half float) at w x h, and
 * whether the frame in slot k has one (valid 0: no texture, all UI). */
void *fbm_scene_slot(int k, uint32_t w, uint32_t h, int valid);
int   fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                  const char *shot_path, uint32_t flip, uint64_t vblank_ns);

#endif

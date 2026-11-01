/* fb_present_vulkan.c: the Vulkan swapchain present of the SDL window.
 * Called by fb_present_sdl.c only; the shape of fb_present_metal.h. */
#ifndef FB_PRESENT_VULKAN_H
#define FB_PRESENT_VULKAN_H

#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

/* Load the Vulkan library for SDL before a SDL_WINDOW_VULKAN window is made:
 * the loader this binary links, so SDL's instance functions and ours agree. */
int   fbv_load_library(char *err, size_t errn);
/* The shared device (nvk_core_init) with the window's surface, and the
 * swapchain; 0 with a reason in err. */
int   fbv_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn);
#if defined(__APPLE__)
/* The main thread's note of the drawable size (macOS: SDL reads it there). */
void  fbv_note_drawable(int w, int h);
#endif
void  fbv_destroy(void);
/* Slot k (0..2) at w x h: its struct nvk_image *, or NULL. Ack thread. */
void *fbv_slot(int k, uint32_t w, uint32_t h);
/* A guest-bytes frame into slot k (bpp 4 X8R8G8B8, 2 R5G6B5). Ack thread. */
int   fbv_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                 uint32_t bpp);
/* Slot k (fw x fh) into the window; 0 when no swapchain image came. Main
 * thread. */
int   fbv_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                  const char *shot_path, uint32_t flip);

/* The present timing (see fb_present_vulkan.c): a summary line now, and
 * the last displayed vblank (0 when unknown). */
void  fbv_timing_report(const char *when);
int   fbv_display_vblank(uint64_t *t_ns, uint64_t *period_ns);

/* The counters line's Vulkan half (BO-210): totals since the start. */
typedef struct {
    uint64_t presents_new, presents_redraw, refills;   /* presents made */
    uint64_t shown, shown_rep;      /* present_wait returns: new frames, refill repeats */
    uint64_t present_max_ns;        /* longest fbv_present call since the last read */
    uint64_t wsi_cycle_ns;          /* the window system's refresh cycle, 0 not known */
    uint64_t period_ns;             /* the refresh period present_wait's steps show */
    unsigned deep, held;            /* keeper rows too deep (left so), refills held back */
    int      smooth, depth, have_wait;
} FbvCounts;
void  fbv_counts(FbvCounts *c);

#endif

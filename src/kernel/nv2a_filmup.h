/**
 * nv2a_filmup.h - the title's films upscaled on the GPU while they play
 * (BO-136, graphics.film_upscale).
 *
 * A film frame is a linear (pitch) texture of a film's size (640x480 or
 * 640x352) whose bytes change from flip to flip: the title's video decoder
 * writes the next frame into it. Each time a backend uploads a new frame of
 * such a texture it runs a filter over it once, on the GPU, and binds the
 * result (twice the size, one level) in place of the frame; the draw's own
 * bilinear filter fits it to the screen. So the cost is one filter run per
 * decoded frame (about 30 a second), never one per draw, and nothing waits:
 * the filter goes into a command buffer submitted before the frame's.
 *
 *   Best   a light deblock (the WMV2 8x8 block edges), then FSRCNNX x2 on
 *          the luma (BO-245's LGPL shader files), chroma by ratio
 *   Cheap  the same deblock, then AMD FSR 1 (EASU 2x + RCAS, MIT) on the
 *          luma
 *   Off    nothing here is asked; the upload path is the one without it
 *
 * Nothing is written to disk. Races are untouched: the title stops its film
 * decoder while racing (BO-102), so no texture there passes the rule.
 *
 * This file is the OS-free part: the setting, the rule, the cost numbers.
 * The backends own every GPU object.
 */
#ifndef NV2A_FILMUP_H
#define NV2A_FILMUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nv2a_filmup_mode {
    NV2A_FILMUP_OFF   = 0,
    NV2A_FILMUP_CHEAP = 1,
    NV2A_FILMUP_BEST  = 2,
    NV2A_FILMUP_MODES
};
const char *nv2a_filmup_mode_name(int mode);   /* "off", "cheap", "best" */

/* graphics.film_upscale; any thread. Before the first set_ call the mode is
 * RECOMP_GFX_FILM_UPSCALE's, else nv2a_filmup_default(). */
void nv2a_filmup_set_mode(int mode);
int  nv2a_filmup_mode(void);
/* The built-in default on this machine: Best on desktops and on a Steam
 * Deck (BO-136: until the Deck's cost is measured there; Cheap is one
 * setting away), Cheap on Android and on ARM Linux boards. */
int  nv2a_filmup_default(void);

/* A backend that can run the filters says so once it is up; the overlay
 * shows the row only then. */
void nv2a_filmup_set_supported(int on);
int  nv2a_filmup_supported(void);

/* The rule, part one: a texture of a film's shape. linear: a linear (pitch)
 * colour format. render_target: bound from a render target (never a film). */
int nv2a_filmup_shape(int linear, int render_target, uint32_t w, uint32_t h);

/* The rule, part two, at each upload of new bytes of a texture that passed
 * nv2a_filmup_shape: the entry's two small counters (start them at 0) and
 * the flip. 1: it is a playing film, filter this frame. A texture becomes a
 * film on its second change within NV2A_FILMUP_GAP flips of the last; one
 * that then stays the same for longer starts over. */
#define NV2A_FILMUP_GAP 8u
int nv2a_filmup_changed(uint64_t flip, uint64_t *last_flip, uint8_t *changes);

/* ── the cost ────────────────────────────────────────────────────────── */

/* A filtered frame's GPU time (the film command buffer's own span), from
 * any thread (Metal reports it on its completion thread). */
void nv2a_filmup_frame_done(int mode, double gpu_ms);
/* Once a flip from the backend: the host interval from the last flip and
 * whether a film frame was filtered in it. Every 5 s while films are being
 * filtered a line goes to the diagnostic log:
 *   [FILMUP] best: 150 frames in 5.0 s, GPU 0.95 ms a frame (max 1.40);
 *            frame time 16.67 ms (max 17.20), flip 1234 */
void nv2a_filmup_flip(uint64_t flip, double frame_ms);

struct nv2a_filmup_stats {
    uint64_t frames;           /* filtered since the start */
    int      mode;             /* of the last one */
    double   gpu_ms;           /* mean GPU ms a filtered frame over the last window (5 s) */
    double   gpu_max_ms;       /* the window's largest */
    double   frame_ms;         /* mean host frame interval over the window, while filtering */
    double   frame_max_ms;
    int      recent;           /* a frame was filtered in the last 2 s */
};
void nv2a_filmup_stats(struct nv2a_filmup_stats *out);

/* Tests. */
void nv2a_filmup_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif

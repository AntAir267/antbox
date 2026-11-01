/*
 * xbox_pvideo.h - the NV2A video overlay (PVIDEO), as a display shows it.
 *
 * XDK D3D's overlay (D3DDevice_EnableOverlay / UpdateOverlay, which the XMV
 * movie player uses) points PVIDEO at a YUV surface and an output rectangle
 * and sets a buffer's in-use bit in NV_PVIDEO_BUFFER at every new frame. On
 * the console the display hardware composites it over the framebuffer at
 * scanout, where the framebuffer shows the colour key (or everywhere, with
 * the key off); nothing of it is in guest memory. The NV2A ack thread
 * (xbox_memory_layout.c) acknowledges the buffer bits and records the
 * registers of each frame here; a presenter composites the latest one
 * (fb_present_vulkan.c, post pass "pvideo").
 */
#ifndef XBOX_PVIDEO_H
#define XBOX_PVIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xbox_pvideo_frame {
    uint32_t seq;           /* bumped at every buffer flip */
    uint32_t addr;          /* the frame's physical address (BASE + OFFSET) */
    uint32_t in_w, in_h;    /* SIZE_IN, source pixels */
    uint32_t pitch;         /* FORMAT pitch, bytes */
    uint32_t yuyv;          /* FORMAT colour 1 (LE_CR8YB8CB8YA8): Y0 U Y1 V; else U Y0 V Y1 */
    uint32_t s16, t16;      /* POINT_IN, source origin in 1/16 pixels */
    uint32_t dsdx, dtdy;    /* source step per output pixel, 12.20 fixed */
    uint32_t out_x, out_y;  /* POINT_OUT, display pixels */
    uint32_t out_w, out_h;  /* SIZE_OUT */
    uint32_t key_on;        /* FORMAT display bit: shown only where the frame is the key */
    uint32_t key;           /* COLOR_KEY, 0xRRGGBB */
    const uint8_t *host;    /* the frame's bytes (pitch x in_h), resolved by
                             * xbox_PvideoFrame; NULL when out of range */
};

/* The overlay's latest frame into *f; 1 while it is up (a frame flipped in
 * the last 250 ms and PVIDEO_STOP not set), 0 otherwise. Any thread. */
int xbox_PvideoFrame(struct xbox_pvideo_frame *f);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PVIDEO_H */

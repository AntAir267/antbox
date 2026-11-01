/*
 * xbox_gamma.h - the display's gamma ramp, as the console applies it.
 *
 * XDK D3D's SetGammaRamp loads the NV2A DAC's 256-entry palette through the
 * VGA-style PRMDIO port (index at 0x6813C8, then red, green, blue bytes at
 * 0x6813C9), which the display applies to the framebuffer at scanout.
 * Nothing of it is in guest memory and the port is plain RAM here, so a
 * title names the moment it is written (Wreckless: a hook on its D3D's
 * writer, sub_000FF634). Games use it for fades, brightness settings and
 * overbright lighting. An identity ramp is no pass at all.
 *
 * A presenter applies it to the finished frame (fb_present_vulkan.c, post
 * pass "gamma"); the frame dumps apply it too (nv2a_pb_exec.c).
 */
#ifndef XBOX_GAMMA_H
#define XBOX_GAMMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ramp the title loaded: red[256], green[256], blue[256]. Any thread. */
void xbox_GammaSetRamp(const uint8_t rgb[768]);
/* The current ramp into out (768 bytes, same layout) and 1, or 0 when none
 * was loaded or it is the identity. *gen (optional) changes with it. */
int xbox_GammaRamp(uint8_t out[768], uint32_t *gen);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GAMMA_H */

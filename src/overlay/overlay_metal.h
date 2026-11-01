/* overlay_metal.mm: the overlay in the CAMetalLayer present
 * (fb_present_metal.m calls it; plain pointers, that file is manual
 * retain/release). */
#ifndef XBOX_OVERLAY_METAL_H
#define XBOX_OVERLAY_METAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Inside the present pass, after the game's image and before endEncoding:
 * id<MTLDevice>, id<MTLCommandBuffer>, MTLRenderPassDescriptor *,
 * id<MTLRenderCommandEncoder>. Draws the overlay if there is one to draw. */
void xbox_OverlayMetalDraw(void *device, void *cmdbuf, void *rpdesc, void *encoder);

#ifdef __cplusplus
}
#endif

#endif

/*
 * smaa.h - SMAA 1x (Jimenez et al., MIT; third_party/smaa) for the window
 * presenters: the reference shaders as source text for either shading
 * language, and the two lookup textures.
 *
 * Three passes at the frame's own size, in place of FXAA:
 *
 *   edges    the frame (BGRA8)                  -> edges   RG8
 *   weights  edges, area, search               -> weights RGBA8
 *   blend    the frame, weights                 -> the anti-aliased frame
 *
 * Both edges and weights start cleared (the edge pass discards where there is
 * no edge). The Metal presenter builds its source with smaa_msl_source(); the
 * Vulkan presenter pastes smaa_glsl_library() after its own header
 * (fb_present_vulkan.c). The preset is HIGH: threshold 0.1, 16 search steps,
 * 8 diagonal, corner rounding 25.
 */
#ifndef FB_SMAA_H
#define FB_SMAA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AreaTex: 160 x 560, RG8 (2 bytes a texel). SearchTex: 64 x 16, R8. */
#define SMAA_AREA_W 160
#define SMAA_AREA_H 560
#define SMAA_SEARCH_W 64
#define SMAA_SEARCH_H 16
extern const unsigned char *const smaa_area_bytes;
extern const unsigned char *const smaa_search_bytes;

/* The library for GLSL: SMAA.hlsl as it is, to follow `#define SMAA_GLSL_4`,
 * `#define SMAA_PRESET_HIGH` and a definition of SMAA_RT_METRICS (a float4,
 * 1/w 1/h w h). */
const char *smaa_glsl_library(void);

/* A Metal library source with the three fragment functions fs_smaa_edges,
 * fs_smaa_weights, fs_smaa_blend (and vs_smaa), in malloc'd memory the caller
 * frees. The reference source takes a few exact substitutions to be Metal
 * (references for inout and out, the offsets array, discard); a text that
 * no longer matches (the file was updated) returns NULL. */
char *smaa_msl_source(void);

#ifdef __cplusplus
}
#endif

#endif

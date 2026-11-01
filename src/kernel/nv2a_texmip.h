/**
 * nv2a_texmip.h - the lower levels of an alpha-tested texture, made again.
 *
 * Trees, bushes, fences and wires are cut-outs: a texture whose alpha says
 * where the leaves are, drawn with the alpha test on, so a texel is either
 * drawn or not. Its mip levels average the alpha, and an average of leaves
 * and gaps falls below the test's reference: the smaller the level, the
 * fewer texels pass, until a tree seen from afar is a few twigs or nothing
 * (the title's own levels do this; so does any box filter). The render scale
 * and a negative LOD bias pick finer levels, which keeps more of the tree but
 * makes the cut-out edge shimmer.
 *
 * nv2a_texmip_cutout() makes levels 1.. of such a texture from its level 0,
 * with Castano's coverage-preserving alpha ("Computing Alpha Mipmaps",
 * DirectXTex ScaleMipMapsAlphaForCoverage): each level's box-filtered alpha
 * is scaled so that as many of its texels pass the draw's alpha test as do
 * on level 0. Colour is averaged by alpha, so the black of transparent
 * texels does not darken the leaves' edges. No gamma: the averages are of
 * the stored values, as the Xbox's own levels are, so a distant tree keeps
 * its brightness. Level 0 is never changed.
 *
 * A backend calls it at upload for a mipmapped texture that an alpha-tested
 * draw binds (nv2a_texmip_cut_ref() says which draws), once; the texture
 * keeps those levels for every later draw.
 *
 * RECOMP_DEBUG=alpha_mips=0 keeps the title's levels and today's LOD bias
 * (the picture before BO-171); =mips or =bias switch on one half only.
 *
 * No OS or graphics headers: shared by the Metal and Vulkan backends.
 */
#ifndef NV2A_TEXMIP_H
#define NV2A_TEXMIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What alpha_mips switches on: bit 0 the levels made again, bit 1 no
 * negative host LOD bias on an alpha-tested draw. Both by default. */
enum { NV2A_TEXMIP_LEVELS = 1, NV2A_TEXMIP_BIAS = 2 };
unsigned nv2a_texmip_mode(void);

/* The reference of a draw's alpha test as nv2a_texmip_cutout() takes it,
 * 1 + ref (bit 8: the test is >=, not >), or 0 when the draw's test is not
 * a cut-out one (off, or a function other than > and >=, or a reference of 0
 * or 255 that every or no texel passes regardless of the levels). */
uint32_t nv2a_texmip_cut_ref(int atest_enable, uint32_t gl_func, uint32_t ref);

/* texels: nv2a_tex_texels(w, h, levels) entries, A8R8G8B8, level 0 first,
 * straight alpha. Levels 1.. are made again from level 0 for cut (a
 * nv2a_texmip_cut_ref value). Returns 1 if they were, 0 if the texture was
 * left as it was: one level, cut 0, every texel of level 0 passing or none,
 * a level 0 larger than 4096 x 4096, or no memory. The same input gives the
 * same output everywhere: plain float arithmetic, one thread. */
int nv2a_texmip_cutout(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels,
                       uint32_t cut);

/* A backend's call at upload, after nv2a_texrep_apply (its arguments, and
 * cut): nv2a_texmip_cutout() when the mode has the levels on, the
 * alpha_mips_log line, and the counters below. Returns what cutout did. */
int nv2a_texmip_apply(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                      uint32_t pitch, uint32_t levels, uint32_t cut, uint32_t *texels);
/* Textures made again so far and the time it took, in ns. */
void nv2a_texmip_stats(uint64_t *made, uint64_t *ns);

/* The share of a level's texels that pass the test cut stands for: what
 * the levels keep (tests, RECOMP_DEBUG=alpha_mips_log). */
float nv2a_texmip_coverage(const uint32_t *texels, uint32_t w, uint32_t h, uint32_t cut);

#ifdef __cplusplus
}
#endif

#endif

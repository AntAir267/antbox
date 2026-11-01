/*
 * nv2a_pcache_psdef.c - the combiner states a title's executable can ask
 * for, read out of the pixel-shader definitions compiled into it; see
 * nv2a_pcache_scan_psdef in nv2a_pcache.h.
 *
 * A D3DPIXELSHADERDEF is 60 words with no header, as the XDK's headers lay
 * it out (xgraphics.h: PSAlphaInputs[8], PSFinalCombinerInputsABCD, ...EFG,
 * PSConstant0[8], PSConstant1[8], PSAlphaOutputs[8], PSRGBInputs[8],
 * PSCompareMode, PSFinalCombinerConstant0/1, PSRGBOutputs[8],
 * PSCombinerCount, PSTextureModes, PSDotMapping, PSInputTexture,
 * PSC0Mapping, PSC1Mapping, PSFinalCombinerConstants). D3D writes the input,
 * output, count and final-combiner words to the NV2A as they are, so they
 * are the registers the backends key a fragment shader on. A block is taken
 * for one when every field that has reserved bits has them clear and the
 * stages past the count are zero.
 *
 * The texture modes are not the definition's alone: a stage with no texture
 * bound runs as NONE, and a PROJECT2D stage with a cube map bound samples a
 * cube (nv2a_stage_decode and the backends' fs_select). So each definition
 * yields one state per way its sampling stages can be bound: off, 2D, and
 * for the stages after the first, cube. Most of them the title never uses:
 * they cost a compile on the pre-warm thread and nothing after.
 */
#include "nv2a_pcache.h"

#include <string.h>

#include "d3d8_combiners_parse.h"

enum {
    W_ALPHA_IN = 0, W_FC_ABCD = 8, W_FC_EFG = 9, W_ALPHA_OUT = 26, W_RGB_IN = 34,
    W_COMPARE = 42, W_RGB_OUT = 45, W_COUNT = 53, W_MODES = 54, W_DOT = 55,
    W_INPUT_TEX = 56, W_FC_CONSTS = 59, W_WORDS = 60
};

static int is_psdef(const uint32_t *w)
{
    uint32_t n = w[W_COUNT] & 0xFFu, i;
    if (n < 1 || n > 8 || (w[W_COUNT] & 0xFFFEEE00u))
        return 0;
    if (w[W_MODES] >> 20 || w[W_DOT] >> 12 || (w[W_INPUT_TEX] & 0xFF00FFFFu)
            || w[W_COMPARE] >> 16 || w[W_FC_CONSTS] >> 12 || (w[W_FC_EFG] & 0x1Fu))
        return 0;
    for (i = 0; i < 4; i++)
        if (((w[W_MODES] >> (5 * i)) & 31u) > 0x12u)
            return 0;
    if ((w[W_MODES] & 31u) == 6 || (w[W_MODES] & 31u) == 7)
        return 0;                       /* stage 0 has no stage to bump from */
    for (i = 0; i < 8; i++) {
        if (w[W_RGB_OUT + i] >> 20 || w[W_ALPHA_OUT + i] >> 20)
            return 0;
        if (i >= n && (w[W_ALPHA_IN + i] | w[W_RGB_IN + i] | w[W_ALPHA_OUT + i]
                       | w[W_RGB_OUT + i]))
            return 0;
    }
    for (i = 0; i < n; i++)
        if (w[W_RGB_OUT + i] | w[W_ALPHA_OUT + i])
            return 1;
    return 0;                           /* no stage writes anything */
}

size_t nv2a_pcache_scan_psdef(const void *image, size_t size,
                              void (*fn)(void *ctx, const void *state, uint32_t len),
                              void *ctx)
{
    const unsigned char *b = (const unsigned char *)image;
    uint32_t w[W_WORDS];
    size_t o, found = 0;

    for (o = 0; o + sizeof w <= size; o += 4) {
        uint32_t modes[4], choices[4], pick[4], k, n;
        memcpy(w, b + o, sizeof w);
        if (!is_psdef(w))
            continue;
        found++;
        /* How each stage can run: fixed (NONE, PASSTHRU, CLIPPLANE), or a
         * sampling stage, which may be off, 2D, or after the first a cube;
         * a bump stage is off or itself. */
        for (k = 0; k < 4; k++) {
            modes[k] = (w[W_MODES] >> (5 * k)) & 31u;
            choices[k] = modes[k] == 0 || modes[k] == 4 || modes[k] == 5 ? 1
                       : modes[k] == 6 || modes[k] == 7 ? 2 : k == 0 ? 2 : 3;
            pick[k] = 0;
        }
        for (;;) {
            NV2ACombinerState st;
            uint32_t sm = 0;
            for (k = 0; k < 4; k++) {
                uint32_t m = modes[k];
                if (choices[k] > 1)
                    m = pick[k] == 0 ? 0 : (m == 6 || m == 7) ? m : pick[k] == 1 ? 1 : 3;
                sm |= m << (5 * k);
            }
            /* As the backends' fs_select builds it. */
            memset(&st, 0, sizeof st);
            d3d8_combiners_from_regs(&w[W_RGB_IN], &w[W_ALPHA_IN], &w[W_RGB_OUT],
                                     &w[W_ALPHA_OUT], w[W_COUNT], w[W_FC_ABCD], w[W_FC_EFG],
                                     sm, &st);
            st.fog_input = 1;
            for (n = 0; n < 4; n++)
                st.clip_mode[n] = (uint8_t)((w[W_COMPARE] >> (4 * n)) & 0xFu);
            st.input_tex[2] = (uint8_t)((w[W_INPUT_TEX] >> 16) & 0xFu);
            st.input_tex[3] = (uint8_t)((w[W_INPUT_TEX] >> 20) & 0xFu);
            if (fn)
                fn(ctx, &st, (uint32_t)sizeof st);
            for (k = 0; k < 4; k++) {
                if (++pick[k] < choices[k])
                    break;
                pick[k] = 0;
            }
            if (k == 4)
                break;
        }
        o += sizeof w - 4;
    }
    return found;
}

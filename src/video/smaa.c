/*
 * smaa.c - the SMAA shaders and lookup textures; see smaa.h.
 *
 * SMAA.hlsl is used as it is for GLSL. For Metal it is wrapped in a struct
 * (so its SMAA_RT_METRICS is a member, MSL having no globals that vary) with
 * the porting macros SMAA_CUSTOM_SL asks for, and these exact text
 * substitutions, which MSL needs and HLSL/GLSL do not:
 *
 *   inout float2/float4 x      -> thread float2/float4 &x          (references)
 *   out float2 e / float4 offset / float2 pixcoord, float4 offset[3]
 *                              -> thread references and pointers
 *   a swizzle passed as a reference (SMAAMovc's .xy/.zw, the corner
 *   patterns' weights.rg/.ba)  -> a temporary
 */
#include "smaa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smaa_hlsl.inc"
#include "AreaTex.h"
#include "SearchTex.h"

const unsigned char *const smaa_area_bytes = areaTexBytes;
const unsigned char *const smaa_search_bytes = searchTexBytes;

const char *smaa_glsl_library(void)
{
    return smaa_hlsl_source;
}

/* s with every `from` replaced by `to`, in a new buffer; *hits counts them. */
static char *replace_all(char *s, const char *from, const char *to, int *hits)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0, len = strlen(s);
    const char *p;
    char *out, *o;

    for (p = s; (p = strstr(p, from)) != NULL; p += fl)
        n++;
    if (!n) {
        *hits = 0;
        return s;
    }
    out = (char *)malloc(len + n * (tl > fl ? tl - fl : 0) + 1);
    if (!out) {
        *hits = 0;
        return s;
    }
    for (o = out, p = s; ; ) {
        const char *q = strstr(p, from);
        if (!q) {
            strcpy(o, p);
            break;
        }
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        memcpy(o, to, tl);
        o += tl;
        p = q + fl;
    }
    free(s);
    *hits = (int)n;
    return out;
}

char *smaa_msl_source(void)
{
    static const struct { const char *from, *to; } subs[] = {
        /* The swizzles first: a reference cannot bind to one. */
        { "SMAAMovc(cond.xy, variable.xy, value.xy);",
          "if (cond.x) variable.x = value.x; if (cond.y) variable.y = value.y;" },
        { "SMAAMovc(cond.zw, variable.zw, value.zw);",
          "if (cond.z) variable.z = value.z; if (cond.w) variable.w = value.w;" },
        { "SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), weights.rg, coords.xyzy, d);\n",
          "{ float2 w_ = weights.rg; SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), w_, coords.xyzy, d); weights.rg = w_; }\n" },
        { "SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), weights.ba, coords.xyxz, d);\n",
          "{ float2 w_ = weights.ba; SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), w_, coords.xyxz, d); weights.ba = w_; }\n" },
        { "out float4 offset[3]", "thread float4 *offset" },
        { "float4 offset[3]", "thread float4 *offset" },
        { "out float2 pixcoord", "thread float2 &pixcoord" },
        { "out float4 offset)", "thread float4 &offset)" },
        { "out float2 e)", "thread float2 &e)" },
        { "inout float2", "thread float2 &" },
        { "inout float4", "thread float4 &" },
    };
    static const char head[] =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VO { float4 p [[position]]; float2 t; };\n"
        "vertex VO vs_smaa(uint i [[vertex_id]]) {\n"
        "    VO o;\n"
        "    o.t = float2((i << 1) & 2, i & 2);\n"
        "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    return o;\n"
        "}\n"
        "#define SMAA_CUSTOM_SL\n"
        "#define SMAA_PRESET_HIGH\n"
        "#define SMAA_RT_METRICS rtm\n"
        /* Every sample is bilinear at a texel centre or between: the
         * reference's "point" ones are bilinear there too. A texture and its
         * sampler travel together, the sampler named after the texture. */
        "#define SMAATexture2D(tex) texture2d<float> tex, sampler tex##_s\n"
        "#define SMAATexturePass2D(tex) tex, tex##_s\n"
        "#define SMAASampleLevelZero(tex, coord) tex.sample(tex##_s, (coord), level(0))\n"
        "#define SMAASampleLevelZeroPoint(tex, coord) tex.sample(tex##_s, (coord), level(0))\n"
        "#define SMAASampleLevelZeroOffset(tex, coord, offset) "
            "tex.sample(tex##_s, (coord) + float2(offset) * SMAA_RT_METRICS.xy, level(0))\n"
        "#define SMAASample(tex, coord) tex.sample(tex##_s, (coord))\n"
        "#define SMAASamplePoint(tex, coord) tex.sample(tex##_s, (coord))\n"
        "#define SMAA_FLATTEN\n"
        "#define SMAA_BRANCH\n"
        "#define lerp(a, b, t) mix(a, b, t)\n"
        "#define mad(a, b, c) fma(a, b, c)\n"
        "#define discard discard_fragment()\n"
        "struct SmaaImpl {\n"
        "float4 rtm;\n";
    static const char tail[] =
        "};\n"
        "fragment float4 fs_smaa_edges(VO vo [[stage_in]], texture2d<float> colorTex [[texture(0)]],\n"
        "                              sampler colorTex_s [[sampler(0)]], constant float4 &m [[buffer(0)]]) {\n"
        "    SmaaImpl s; s.rtm = m;\n"
        "    float4 offset[3];\n"
        "    s.SMAAEdgeDetectionVS(vo.t, offset);\n"
        "    return float4(s.SMAALumaEdgeDetectionPS(vo.t, offset, colorTex, colorTex_s), 0.0, 0.0);\n"
        "}\n"
        "fragment float4 fs_smaa_weights(VO vo [[stage_in]], texture2d<float> edgesTex [[texture(0)]],\n"
        "                                sampler edgesTex_s [[sampler(0)]],\n"
        "                                texture2d<float> areaTex [[texture(1)]], sampler areaTex_s [[sampler(1)]],\n"
        "                                texture2d<float> searchTex [[texture(2)]], sampler searchTex_s [[sampler(2)]],\n"
        "                                constant float4 &m [[buffer(0)]]) {\n"
        "    SmaaImpl s; s.rtm = m;\n"
        "    float4 offset[3];\n"
        "    float2 pixcoord;\n"
        "    s.SMAABlendingWeightCalculationVS(vo.t, pixcoord, offset);\n"
        "    return s.SMAABlendingWeightCalculationPS(vo.t, pixcoord, offset, edgesTex, edgesTex_s,\n"
        "                                             areaTex, areaTex_s, searchTex, searchTex_s,\n"
        "                                             float4(0.0, 0.0, 0.0, 0.0));\n"
        "}\n"
        "fragment float4 fs_smaa_blend(VO vo [[stage_in]], texture2d<float> colorTex [[texture(0)]],\n"
        "                              sampler colorTex_s [[sampler(0)]],\n"
        "                              texture2d<float> blendTex [[texture(1)]], sampler blendTex_s [[sampler(1)]],\n"
        "                              constant float4 &m [[buffer(0)]]) {\n"
        "    SmaaImpl s; s.rtm = m;\n"
        "    float4 offset;\n"
        "    s.SMAANeighborhoodBlendingVS(vo.t, offset);\n"
        "    return float4(s.SMAANeighborhoodBlendingPS(vo.t, offset, colorTex, colorTex_s,\n"
        "                                               blendTex, blendTex_s).rgb, 1.0);\n"
        "}\n";
    char *lib = (char *)malloc(strlen(smaa_hlsl_source) + 1), *out;
    size_t i;

    if (!lib)
        return NULL;
    strcpy(lib, smaa_hlsl_source);
    for (i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        int hits = 0;
        lib = replace_all(lib, subs[i].from, subs[i].to, &hits);
        if (!hits) {
            fprintf(stderr, "[SMAA] the reference source has no \"%.40s...\": updated?\n",
                    subs[i].from);
            free(lib);
            return NULL;
        }
    }
    out = (char *)malloc(sizeof head + strlen(lib) + sizeof tail);
    if (out)
        sprintf(out, "%s%s%s", head, lib, tail);
    free(lib);
    return out;
}

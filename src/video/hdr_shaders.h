/*
 * hdr_shaders.h - the HDR output's maths (BO-110, BO-244), as one source
 * text that both shading languages take, so the Vulkan flip pass
 * (d3d/nv2a_pb_vulkan.c) and the Metal composite (video/fb_present_metal.m)
 * give the same nits for the same frame and settings.
 *
 * The functions are written once, inside HDR_TONE_FNS(X): X is a macro that
 * either stringifies them (HDR_STR, for the shaders) or pastes them as code
 * (a C++ test with small float3/float4 types: tests/hdr_equiv). The dialect
 * is neutral (float3/float4, mix, clamp, pow, exp, smoothstep, dot, no
 * swizzles past .x/.y/.z/.w and .r/.g/.b); each language's head maps it:
 *
 *   HDR_GLSL_HEAD   float3 -> vec3, HDR_SEL -> mix(x, y, bvec), HDR_GT -> greaterThan
 *   HDR_MSL_HEAD    HDR_SEL -> select, HDR_GT -> >
 *
 *   hdr_dec(c, srgb)   the title's encoded value to linear light: gamma 2.2,
 *                      or the sRGB curve when srgb >= 0.5 (graphics.hdr_decode)
 *   hdr_tone(c, a, b)  c >= 0 (encoded) to linear light in paper whites:
 *                      decoded, the shadows' power below mid grey (luma
 *                      < 0.18), the highlight gain and a hue-kept shoulder on
 *                      the brightest channel past paper white towards the
 *                      peak (exponential, blended to a slower rational curve
 *                      past 2.5x by the softness, BO-195)
 *                      a: x the peak in paper whites, y paper white / 10,000
 *                         nits, z the highlight gain, w 1 = the sRGB curve
 *                      b: x the shadows' power, y the shoulder's softness,
 *                         z the black level / 10,000 nits
 *   hdr_pq(l, pk, b)   linear BT.709 light in units of 10,000 nits to ST 2084
 *                      PQ code values in BT.2020: the black level lift
 *                      (nits' = black + (1 - black / peak) x nits, pk the
 *                      peak / 10,000), BT.709 to BT.2020, PQ
 *   hdr_ovl(s, hud)    the settings overlay (premultiplied, over transparent
 *                      black) to PQ at the HUD's white (hud = nits / 10,000)
 */
#ifndef XR_HDR_SHADERS_H
#define XR_HDR_SHADERS_H

#define HDR_STR(...) #__VA_ARGS__
#define HDR_CODE(...) __VA_ARGS__

#define HDR_TONE_FNS(X) X(                                                              \
float3 hdr_dec(float3 c, float srgb) {                                                  \
    return srgb < 0.5 ? pow(c, float3(2.2))                                             \
         : HDR_SEL(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)),                    \
                   HDR_GT(c, float3(0.04045)));                                          \
}                                                                                       \
float3 hdr_tone(float3 c, float4 a, float4 b) {                                         \
    float3 l = hdr_dec(c, a.w);                                                         \
    float y = dot(l, float3(0.2126, 0.7152, 0.0722));                                   \
    if (y > 0.0 && y < 0.18 && b.x != 1.0) l *= 0.18 * pow(y / 0.18, b.x) / y;         \
    float mx = max(l.r, max(l.g, l.b)), m = max(a.x, 1.0);                              \
    if (mx > 1.0) {                                                                     \
        float x = 1.0 + a.z * (mx - 1.0);                                               \
        float v = 1.0;                                                                  \
        if (m > 1.0001) {                                                               \
            float k = m - 1.0, e = 1.0 + k * (1.0 - exp(-(x - 1.0) / k));               \
            float r = 1.0 + k * (x - 1.0) / ((x - 1.0) + k);                            \
            v = mix(e, r, b.y * clamp(k - 0.5, 0.0, 1.0) * smoothstep(2.5, 4.0, x));    \
        }                                                                               \
        l *= v / mx;                                                                    \
    }                                                                                   \
    return l;                                                                           \
}                                                                                       \
float3 hdr_pq(float3 l, float pk, float4 b) {                                           \
    l = b.z + (1.0 - b.z / pk) * l;                                                     \
    l = float3(0.6274, 0.0691, 0.0164) * l.r + float3(0.3293, 0.9195, 0.0880) * l.g     \
      + float3(0.0433, 0.0114, 0.8956) * l.b;                                           \
    float3 q = pow(clamp(l, 0.0, 1.0), float3(0.1593017578125));                        \
    return pow((0.8359375 + 18.8515625 * q) / (1.0 + 18.6875 * q), float3(78.84375));   \
}                                                                                       \
float3 hdr_ovl(float4 s, float hud) {                                                   \
    float3 l = pow(clamp(float3(s.x, s.y, s.z) / s.w, 0.0, 1.0), float3(2.2)) * hud;    \
    return hdr_pq(l, 1.0, float4(0.0));                                                 \
}                                                                                       \
)

#define HDR_GLSL_HEAD                                                                   \
    "#define float3 vec3\n#define float4 vec4\n"                                        \
    "#define HDR_SEL(x, y, c) mix(x, y, c)\n#define HDR_GT(a, b) greaterThan(a, b)\n"

#define HDR_MSL_HEAD                                                                    \
    "#define HDR_SEL(x, y, c) select(x, y, c)\n#define HDR_GT(a, b) ((a) > (b))\n"

/* The Vulkan flip pass (nv2a_pb_vulkan.c): the view's target (the title's
 * encoding, the scene's light past 1.0, the HUD at or below it, x the HUD's
 * white over paper white in the combiners) to the slot in PQ, BT.2020.
 * Push constants as hdr_tone's a and b. */
#define HDR_FLIP_GLSL                                                                   \
    "#version 450\n"                                                                    \
    HDR_GLSL_HEAD                                                                       \
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"                             \
    "layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;\n"                        \
    "layout(location = 0) out vec4 o;\n"                                                \
    HDR_TONE_FNS(HDR_STR) "\n"                                                          \
    "void main() {\n"                                                                   \
    "    vec3 c = max(texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));\n"   \
    "    o = vec4(hdr_pq(hdr_tone(c, pc.a, pc.b) * pc.a.y, pc.a.x * pc.a.y, pc.b), 1.0);\n" \
    "}\n"

/* The Vulkan overlay encode (fb_present_vulkan.c): p.x the HUD's white / 10,000. */
#define HDR_OVL_GLSL                                                                    \
    "#version 450\n"                                                                    \
    HDR_GLSL_HEAD                                                                       \
    "layout(set = 0, binding = 0) uniform sampler2D tex0;\n"                            \
    "layout(push_constant) uniform PC { vec4 p; } pc;\n"                                \
    "layout(location = 0) in vec2 vUv;\n"                                               \
    "layout(location = 0) out vec4 outColor;\n"                                         \
    HDR_TONE_FNS(HDR_STR) "\n"                                                          \
    "void main() {\n"                                                                   \
    "    vec4 s = texelFetch(tex0, ivec2(gl_FragCoord.xy), 0);\n"                       \
    "    if (s.a <= 0.0) discard;\n"                                                    \
    "    outColor = vec4(hdr_ovl(s, pc.p.x), s.a);\n"                                   \
    "}\n"

/* The Metal side (fb_present_metal.m pastes it after its VO struct):
 *
 * fs_hdr_pq, the composite into a PQ (kCGColorSpaceITUR_2100_PQ) layer at
 * the frame's size: f the frame, g the backend's scene snapshot. A pixel
 * where the frame is the snapshot is scene: hdr_tone x paper white; one the
 * HUD or a menu drew over is clamped to 1.0, x the HUD's white (the Vulkan
 * backend scales its UI draws instead: the same with the HUD at paper
 * white, the default). hq[0] = a, hq[1] = b as hdr_tone; hq[2]: x the HUD's
 * white / 10,000, y the peak / 10,000, z 1 = a scene snapshot is there.
 *
 * fs_hdr_edr, the extended-sRGB EDR fallback (BO-110/BO-227's layer): the
 * same hdr_tone, its light in EDR units (1.0 = the system's SDR white =
 * 100 nits, as macOS maps HDR10), back to extended sRGB. Same hq.
 *
 * fs_hdr_ovl: the overlay (drawn apart into a BGRA8 target cleared to
 * transparent) encoded at the HUD's white, blended by its alpha.
 * fs_hdr_pat: a calibration pattern, two PQ levels (hp.x inside the disc or
 * the left half, hp.y elsewhere); hp.zw the centre, hr.x the disc's radius
 * (< 0: halves). */
#define HDR_COMP_MSL                                                                    \
    HDR_MSL_HEAD                                                                        \
    HDR_TONE_FNS(HDR_STR) "\n"                                                          \
    "float3 hdr_srgb_enc(float3 l) {\n"                                                 \
    "    return select(l * 12.92, 1.055 * pow(max(l, 0.0), 1.0 / 2.4) - 0.055, l > 0.0031308);\n" \
    "}\n"                                                                               \
    "bool hdr_is_scene(texture2d<float> t, texture2d<float> sc, sampler s, float2 uv, float has) {\n" \
    "    if (has < 0.5) return false;\n"                                                \
    "    float3 d = abs(t.sample(s, uv).rgb - sc.sample(s, uv).rgb);\n"                 \
    "    return max(d.r, max(d.g, d.b)) < 0.002;\n"                                     \
    "}\n"                                                                               \
    "fragment float4 fs_hdr_pq(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n" \
    "                          texture2d<float> sc [[texture(1)]], sampler s [[sampler(0)]],\n" \
    "                          constant float4 *hq [[buffer(0)]]) {\n"                  \
    "    float4 a = hq[0], b = hq[1], h = hq[2];\n"                                     \
    "    float3 f = t.sample(s, vo.t).rgb, l;\n"                                        \
    "    if (hdr_is_scene(t, sc, s, vo.t, h.z))\n"                                      \
    "        l = hdr_tone(max(f, 0.0), a, b) * a.y;\n"                                  \
    "    else\n"                                                                        \
    "        l = hdr_tone(saturate(f), a, b) * h.x;\n"                                  \
    "    return float4(hdr_pq(l, h.y, b), 1);\n"                                        \
    "}\n"                                                                               \
    "fragment float4 fs_hdr_edr(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n" \
    "                           texture2d<float> sc [[texture(1)]], sampler s [[sampler(0)]],\n" \
    "                           constant float4 *hq [[buffer(0)]]) {\n"                 \
    "    float4 a = hq[0], b = hq[1], h = hq[2];\n"                                     \
    "    float3 f = t.sample(s, vo.t).rgb, l;\n"                                        \
    "    if (hdr_is_scene(t, sc, s, vo.t, h.z))\n"                                      \
    "        l = hdr_tone(max(f, 0.0), a, b) * a.y;\n"                                  \
    "    else\n"                                                                        \
    "        l = hdr_tone(saturate(f), a, b) * h.x;\n"                                  \
    "    l = b.z + (1.0 - b.z / h.y) * l;\n"                                            \
    "    return float4(hdr_srgb_enc(l * 100.0), 1);\n"                                  \
    "}\n"                                                                               \
    "fragment float4 fs_hdr_ovl(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n" \
    "                           constant float4 &hp [[buffer(0)]]) {\n"                 \
    "    float4 s = t.read(uint2(vo.p.xy));\n"                                          \
    "    if (s.a <= 0.0) discard_fragment();\n"                                         \
    "    return float4(hdr_ovl(s, hp.x), s.a);\n"                                       \
    "}\n"                                                                               \
    "fragment float4 fs_hdr_pat(VO vo [[stage_in]], constant float4 *hp [[buffer(0)]]) {\n" \
    "    float2 d = vo.p.xy - hp[0].zw;\n"                                              \
    "    bool in1 = hp[1].x >= 0.0 ? length(d) < hp[1].x : d.x < 0.0;\n"               \
    "    return float4(float3(in1 ? hp[0].x : hp[0].y), 1);\n"                          \
    "}\n"

#endif /* XR_HDR_SHADERS_H */

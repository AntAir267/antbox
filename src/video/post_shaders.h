/*
 * post_shaders.h - the presenters' post-process passes, as source text both
 * shading languages take: FXAA (anti-aliasing) and CAS (sharpening). Each is a
 * function body in a neutral dialect (float2/float3, mix, saturate, rsqrt),
 * pasted into a fragment function by fb_present_metal.m (MSL) and
 * fb_present_vulkan.c (GLSL, compiled to SPIR-V at runtime), with a few macros
 * that map the dialect on:
 *
 *   FETCH(p)      the pass's input at uv p, a float3 (a bilinear fetch)
 *   RETURN(c)     leave with the colour c, a float3
 *   uv, rcp       the pixel's uv, and 1 / the input's size
 *   sharp         (CAS) the strength, 0..1
 *
 * The passes run on the frame at the game's internal resolution, after it is
 * final and before the scale to the window; the settings overlay is drawn
 * after them, so the interface is not filtered.
 *
 * FXAA is the algorithm of NVIDIA's FXAA 3.11 white paper (Timothy Lottes):
 * a luma edge test, the edge's direction, a search along it for its ends, and
 * a shift of the sample across the edge by where the pixel lies on it, with
 * the quality preset's step sizes (1 1 1 1 1 1.5 2 2 2 2 4 8) and
 * the subpixel and edge thresholds of its default (0.75, 0.125, 0.0312).
 * Written here from the algorithm, not from the header.
 *
 * SMAA is not here: it is the reference's own source (smaa.h, smaa.c).
 *
 * CAS is AMD FidelityFX Contrast Adaptive Sharpening (MIT): a cross filter
 * whose negative lobe shrinks where the neighbourhood is already contrasty.
 */
#ifndef FB_POST_SHADERS_H
#define FB_POST_SHADERS_H

#define POST_FXAA_BODY \
    "float3 rgbM = FETCH(uv);\n" \
    "float lumaM = dot(rgbM, float3(0.299, 0.587, 0.114));\n" \
    "float lumaN = dot(FETCH(uv + float2(0.0, -rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaS = dot(FETCH(uv + float2(0.0, rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaW = dot(FETCH(uv + float2(-rcp.x, 0.0)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaE = dot(FETCH(uv + float2(rcp.x, 0.0)), float3(0.299, 0.587, 0.114));\n" \
    "float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));\n" \
    "float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));\n" \
    "float range = rangeMax - rangeMin;\n" \
    "if (range < max(0.0312, rangeMax * 0.125)) { RETURN(rgbM); }\n" \
    "float lumaNW = dot(FETCH(uv + float2(-rcp.x, -rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaNE = dot(FETCH(uv + float2(rcp.x, -rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaSW = dot(FETCH(uv + float2(-rcp.x, rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaSE = dot(FETCH(uv + float2(rcp.x, rcp.y)), float3(0.299, 0.587, 0.114));\n" \
    "float lumaNS = lumaN + lumaS;\n" \
    "float lumaWE = lumaW + lumaE;\n" \
    "float subpixNSWE = lumaNS + lumaWE;\n" \
    "float edgeHorz1 = -2.0 * lumaM + lumaNS;\n" \
    "float edgeVert1 = -2.0 * lumaM + lumaWE;\n" \
    "float lumaNESE = lumaNE + lumaSE;\n" \
    "float lumaNWNE = lumaNW + lumaNE;\n" \
    "float edgeHorz2 = -2.0 * lumaE + lumaNESE;\n" \
    "float edgeVert2 = -2.0 * lumaN + lumaNWNE;\n" \
    "float lumaNWSW = lumaNW + lumaSW;\n" \
    "float lumaSWSE = lumaSW + lumaSE;\n" \
    "float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);\n" \
    "float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);\n" \
    "float edgeHorz3 = -2.0 * lumaW + lumaNWSW;\n" \
    "float edgeVert3 = -2.0 * lumaS + lumaSWSE;\n" \
    "float edgeHorz = abs(edgeHorz3) + edgeHorz4;\n" \
    "float edgeVert = abs(edgeVert3) + edgeVert4;\n" \
    "float subpixNWSWNESE = lumaNWSW + lumaNESE;\n" \
    "float lengthSign = rcp.x;\n" \
    "bool horzSpan = edgeHorz >= edgeVert;\n" \
    "float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;\n" \
    "if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }\n" \
    "if (horzSpan) { lengthSign = rcp.y; }\n" \
    "float subpixB = subpixA * (1.0 / 12.0) - lumaM;\n" \
    "float gradientN = lumaN - lumaM;\n" \
    "float gradientS = lumaS - lumaM;\n" \
    "float lumaNN = lumaN + lumaM;\n" \
    "float lumaSS = lumaS + lumaM;\n" \
    "bool pairN = abs(gradientN) >= abs(gradientS);\n" \
    "float gradient = max(abs(gradientN), abs(gradientS));\n" \
    "if (pairN) { lengthSign = -lengthSign; }\n" \
    "float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);\n" \
    "float2 posB = uv;\n" \
    "float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);\n" \
    "if (!horzSpan) { posB.x += lengthSign * 0.5; }\n" \
    "if (horzSpan) { posB.y += lengthSign * 0.5; }\n" \
    "float2 posN = posB - offNP;\n" \
    "float2 posP = posB + offNP;\n" \
    "float subpixD = -2.0 * subpixC + 3.0;\n" \
    "float lumaEndN = dot(FETCH(posN), float3(0.299, 0.587, 0.114));\n" \
    "float subpixE = subpixC * subpixC;\n" \
    "float lumaEndP = dot(FETCH(posP), float3(0.299, 0.587, 0.114));\n" \
    "if (!pairN) { lumaNN = lumaSS; }\n" \
    "float gradientScaled = gradient * 0.25;\n" \
    "float lumaMM = lumaM - lumaNN * 0.5;\n" \
    "float subpixF = subpixD * subpixE;\n" \
    "bool lumaMLTZero = lumaMM < 0.0;\n" \
    "lumaEndN -= lumaNN * 0.5;\n" \
    "lumaEndP -= lumaNN * 0.5;\n" \
    "bool doneN = abs(lumaEndN) >= gradientScaled;\n" \
    "bool doneP = abs(lumaEndP) >= gradientScaled;\n" \
    "if (!doneN) { posN -= offNP; }\n" \
    "if (!doneP) { posP += offNP; }\n" \
    "bool doneNP = !doneN || !doneP;\n" \
    "for (int iter = 2; iter < 12 && doneNP; iter++) {\n" \
    "    float q = iter < 5 ? 1.0 : iter == 5 ? 1.5 : iter < 10 ? 2.0 : iter == 10 ? 4.0 : 8.0;\n" \
    "    if (!doneN) { lumaEndN = dot(FETCH(posN), float3(0.299, 0.587, 0.114)) - lumaNN * 0.5; }\n" \
    "    if (!doneP) { lumaEndP = dot(FETCH(posP), float3(0.299, 0.587, 0.114)) - lumaNN * 0.5; }\n" \
    "    doneN = abs(lumaEndN) >= gradientScaled;\n" \
    "    doneP = abs(lumaEndP) >= gradientScaled;\n" \
    "    if (!doneN) { posN -= offNP * q; }\n" \
    "    if (!doneP) { posP += offNP * q; }\n" \
    "    doneNP = !doneN || !doneP;\n" \
    "}\n" \
    "float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;\n" \
    "float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;\n" \
    "bool directionN = dstN < dstP;\n" \
    "float lumaEndG = directionN ? lumaEndN : lumaEndP;\n" \
    "bool goodSpan = (lumaEndG < 0.0) != lumaMLTZero;\n" \
    "float spanLength = dstP + dstN;\n" \
    "float dst = directionN ? dstN : dstP;\n" \
    "float pixelOffset = 0.5 - dst / spanLength;\n" \
    "float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;\n" \
    "float pixelOffsetSubpix = max(pixelOffsetGood, subpixF * subpixF * 0.75);\n" \
    "float2 posM = uv;\n" \
    "if (!horzSpan) { posM.x += pixelOffsetSubpix * lengthSign; }\n" \
    "if (horzSpan) { posM.y += pixelOffsetSubpix * lengthSign; }\n" \
    "RETURN(FETCH(posM));\n"

#define POST_CAS_BODY \
    "float3 a = FETCH(uv + float2(-rcp.x, -rcp.y));\n" \
    "float3 b = FETCH(uv + float2(0.0, -rcp.y));\n" \
    "float3 c = FETCH(uv + float2(rcp.x, -rcp.y));\n" \
    "float3 d = FETCH(uv + float2(-rcp.x, 0.0));\n" \
    "float3 e = FETCH(uv);\n" \
    "float3 f = FETCH(uv + float2(rcp.x, 0.0));\n" \
    "float3 g = FETCH(uv + float2(-rcp.x, rcp.y));\n" \
    "float3 h = FETCH(uv + float2(0.0, rcp.y));\n" \
    "float3 i = FETCH(uv + float2(rcp.x, rcp.y));\n" \
    "float3 mn = min(min(min(d, e), min(f, b)), h);\n" \
    "float3 mn2 = min(min(min(mn, a), min(c, g)), i);\n" \
    "mn = mn + mn2;\n" \
    "float3 mx = max(max(max(d, e), max(f, b)), h);\n" \
    "float3 mx2 = max(max(max(mx, a), max(c, g)), i);\n" \
    "mx = mx + mx2;\n" \
    "float3 amp = saturate(min(mn, float3(2.0, 2.0, 2.0) - mx) / max(mx, float3(0.0001, 0.0001, 0.0001)));\n" \
    "amp = sqrt(amp);\n" \
    "float peak = -1.0 / mix(8.0, 5.0, sharp);\n" \
    "float3 w = amp * peak;\n" \
    "float3 rcpWeight = float3(1.0, 1.0, 1.0) / (float3(1.0, 1.0, 1.0) + 4.0 * w);\n" \
    "RETURN(saturate((b * w + d * w + f * w + h * w + e) * rcpWeight));\n"

#endif /* FB_POST_SHADERS_H */

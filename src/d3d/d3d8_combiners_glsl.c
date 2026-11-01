/**
 * NV2A register combiners -> GLSL fragment shader for Vulkan (see
 * d3d8_glsl.h). Line for line d3d8_combiners_msl.c; only the syntax differs
 * (textures are set-1 bindings, texture() with a bias, discard).
 */

#include "d3d8_glsl.h"
#include <stdio.h>
#include <string.h>

/* The shader mirrors nv2a_rc_eval_regs (src/kernel/nv2a_combiner.c), as the
 * MSL does; see the comment block at the top of d3d8_combiners_msl.c. */

#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/* An input as vec3 (RGB portion) or float (alpha portion), before mapping. */
static void input_src(const NV2ACombinerInput *in, int alpha_portion,
                      char *out, size_t n)
{
    int reg = (int)in->reg & 0xF;
    if (alpha_portion)
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "a" : "b");
    else
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "aaa" : "rgb");
}

static const char s_rc_helpers[] =
    "float rc_map(float x, int m) {\n"
    "    if (m == 0) return max(x, 0.0);\n"
    "    if (m == 1) return 1.0 - saturate(x);\n"
    "    if (m == 2) return 2.0 * max(x, 0.0) - 1.0;\n"
    "    if (m == 3) return 1.0 - 2.0 * max(x, 0.0);\n"
    "    if (m == 4) return max(x, 0.0) - 0.5;\n"
    "    if (m == 5) return 0.5 - max(x, 0.0);\n"
    "    if (m == 6) return x;\n"
    "    return -x;\n"
    "}\n"
    "vec3 rc_map3(vec3 x, int m) {\n"
    "    return vec3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
    "}\n"
    "float rc_op(float x, int op) {\n"
    "    if (op == 1) x -= 0.5;\n"
    "    else if (op == 2) x *= 2.0;\n"
    "    else if (op == 3) x = (x - 0.5) * 2.0;\n"
    "    else if (op == 4) x *= 4.0;\n"
    "    else if (op == 6) x *= 0.5;\n"
    "    return clamp(x, -1.0, 1.0);\n"
    "}\n"
    /* A UNORM byte read as two's complement, / 127: xemu psh.c sign3. */
    "float rc_sign8(float x) {\n"
    "    x *= 255.0;\n"
    "    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;\n"
    "}\n"
    "vec3 rc_op3(vec3 x, int op) {\n"
    "    return vec3(rc_op(x.r, op), rc_op(x.g, op), rc_op(x.b, op));\n"
    "}\n";

/* BO-110, HDR variant (as s_rc_helpers_hdr in d3d8_combiners_msl.c): the
 * upper bound of a value is a parameter (pc.hdr.x, per draw); the lower
 * bounds and every [0, 1] clamp that feeds a complement or a selector stay. */
static const char s_rc_helpers_hdr[] =
    "float rc_map(float x, int m) {\n"
    "    if (m == 0) return max(x, 0.0);\n"
    "    if (m == 1) return 1.0 - saturate(x);\n"
    "    if (m == 2) return 2.0 * saturate(x) - 1.0;\n"
    "    if (m == 3) return 1.0 - 2.0 * saturate(x);\n"
    "    if (m == 4) return saturate(x) - 0.5;\n"
    "    if (m == 5) return 0.5 - saturate(x);\n"
    "    if (m == 6) return x;\n"
    "    return -x;\n"
    "}\n"
    "vec3 rc_map3(vec3 x, int m) {\n"
    "    return vec3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
    "}\n"
    "float rc_op(float x, int op, float hi) {\n"
    "    if (op == 1) x -= 0.5;\n"
    "    else if (op == 2) x *= 2.0;\n"
    "    else if (op == 3) x = (x - 0.5) * 2.0;\n"
    "    else if (op == 4) x *= 4.0;\n"
    "    else if (op == 6) x *= 0.5;\n"
    "    return clamp(x, -1.0, hi);\n"
    "}\n"
    "float rc_sign8(float x) {\n"
    "    x *= 255.0;\n"
    "    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;\n"
    "}\n"
    "vec3 rc_op3(vec3 x, int op, float hi) {\n"
    "    return vec3(rc_op(x.r, op, hi), rc_op(x.g, op, hi), rc_op(x.b, op, hi));\n"
    "}\n";

/* The constants with the HDR member at the end (VkPsConsts). */
#define D3D8_GLSL_PSCONSTS_HDR \
    "layout(set = 0, binding = 2, std140) uniform PsConsts {\n" \
    "    vec4 c0[8]; vec4 c1[8]; vec4 fc0; vec4 fc1; vec4 fog_color;\n" \
    "    uvec4 atest; uvec4 alpha_only; vec4 tex_scale[4]; uvec4 tex_mode;\n" \
    "    vec4 tex_bias;\n" \
    "    vec4 bump_mat[4]; vec4 bump_lum[4];\n" \
    "    vec4 hdr;\n" \
    "} pc;\n"

static int generate(const NV2ACombinerState *state, char *buf, int bufsize, int hdr);

int d3d8_combiners_generate_glsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    return generate(state, buf, bufsize, 0);
}

int d3d8_combiners_generate_glsl_hdr(const NV2ACombinerState *state,
                                     char *buf, int bufsize)
{
    return generate(state, buf, bufsize, 1);
}

static int generate(const NV2ACombinerState *state, char *buf, int bufsize, int hdr)
{
    const char *OPHI = hdr ? ", hi" : "", *OPHA = hdr ? ", 1.0" : "";
    static const char *const in_name[4] = { "A", "B", "C", "D" };
    char src[32];
    int off = 0;
    int i, s, k;

    EMIT("#version 450\n"
         "/* Auto-generated NV2A register combiners (GLSL, Vulkan) */\n");
    EMIT("%s", D3D8_GLSL_SATURATE);
    EMIT("%s", hdr ? D3D8_GLSL_PSCONSTS_HDR : D3D8_GLSL_PSCONSTS);
    EMIT("%s", D3D8_GLSL_FS_IN);
    EMIT("layout(location = 0) out vec4 o_color;\n");
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE || m == NV2A_TEXMODE_PASSTHRU
                || m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        EMIT("layout(set = 1, binding = %d) uniform %s tex%d;\n", i,
             m == NV2A_TEXMODE_3D ? "sampler3D" :
             m == NV2A_TEXMODE_CUBEMAP ? "samplerCube" : "sampler2D", i);
    }
    EMIT("%s", hdr ? s_rc_helpers_hdr : s_rc_helpers);
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        if (state->tex_mode[i] == NV2A_TEXMODE_DOTPRODUCT || state->tex_mode[i] == NV2A_TEXMODE_DOT_ST)
            break;
    if (i < NV2A_MAX_TEXTURES)
        /* xemu psh.c's dot mappings: [0, 1] bytes to signed. */
        EMIT("float dm_s1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }\n"
             "float dm_s2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n"
             "float dm_s3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n"
             "vec3 dotmap(vec4 c, uint m) {\n"
             "    if (m == 1u) return vec3(dm_s1(c.r), dm_s1(c.g), dm_s1(c.b));\n"
             "    if (m == 2u) return vec3(dm_s2(c.r), dm_s2(c.g), dm_s2(c.b));\n"
             "    if (m == 3u) return vec3(dm_s3(c.r), dm_s3(c.g), dm_s3(c.b));\n"
             "    return c.rgb;\n"
             "}\n");

    EMIT("void main() {\n");
    if (state->wbuffer)
        EMIT("    gl_FragDepth = clamp(i_wz, 0.0, 1.0);\n");
    if (hdr)
        EMIT("    float hi = pc.hdr.x;\n"
             "    float hlod = 1.0;\n");   /* BO-195: how much of a draw's gain its texture's mip keeps */
    for (k = 0; k < 16; k++)
        EMIT("    vec4 x%d = vec4(0.0);\n", k);
    if (state->fog_input)
        EMIT("    x3 = vec4(pc.fog_color.rgb, saturate(i_fog));\n");
    else
        EMIT("    x3 = pc.fog_color;\n");
    EMIT("    x4 = i_d0;\n");
    EMIT("    x5 = i_d1;\n");

    /* ---- Texture stages ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE) {
            /* (0, 0, 0, 1), as xemu: see NV2ATextureMode */
            EMIT("    x%d = vec4(0.0, 0.0, 0.0, 1.0);\n", 8 + i);
            continue;
        }
        if (m == NV2A_TEXMODE_CLIPPLANE) {
            /* As the MSL (xemu psh.c PS_TEXTUREMODES_CLIPPLANE): each
             * coordinate component against 0 by the stage's compare mode;
             * the register reads 0. */
            for (k = 0; k < 4; k++)
                EMIT("    if (i_t%d.%c %s 0.0) discard;\n", i, "xyzw"[k],
                     (state->clip_mode[i] >> k) & 1u ? ">=" : "<");
            continue;
        }
        if (m == NV2A_TEXMODE_PASSTHRU) {
            EMIT("    x%d = saturate(i_t%d);\n", 8 + i, i);
            continue;
        }
        if (m == NV2A_TEXMODE_DOTPRODUCT || m == NV2A_TEXMODE_DOT_ST) {
            /* xemu psh.c: dot = this stage's coordinate . the input stage's
             * colour, mapped; DOTPRODUCT reads 0 and leaves its dot for the
             * DOT_ST after it, which samples at (that dot, its own). */
            int in = i == 1 ? 0 : state->input_tex[i] < i ? state->input_tex[i] : i - 1;
            EMIT("    float dot%d = dot(i_t%d.xyz, dotmap(x%d, %uu));\n", i, i, 8 + in,
                 (unsigned)(state->dot_map[i] & 3u));
            if (m == NV2A_TEXMODE_DOTPRODUCT)
                EMIT("    x%d = vec4(0.0);\n", 8 + i);
            else if (i > 0 && state->tex_mode[i - 1] == NV2A_TEXMODE_DOTPRODUCT)
                EMIT("    x%d = texture(tex%d, vec2(dot%d, dot%d) * pc.tex_scale[%d].xy);\n",
                     8 + i, i, i - 1, i, i);
            else
                EMIT("    x%d = texture(tex%d, vec2(0.0, dot%d) * pc.tex_scale[%d].xy);\n",
                     8 + i, i, i, i);
        } else if (m == NV2A_TEXMODE_DPNDNT_AR || m == NV2A_TEXMODE_DPNDNT_GB) {
            int in = i == 1 ? 0 : state->input_tex[i] < i ? state->input_tex[i] : i - 1;
            EMIT("    x%d = texture(tex%d, x%d.%s * pc.tex_scale[%d].xy);\n", 8 + i, i, 8 + in,
                 m == NV2A_TEXMODE_DPNDNT_AR ? "ar" : "gb", i);
        } else if (state->tex_shadow[i] & 0x80) {
            /* A depth buffer read as LIN_A8R8G8B8 (tex_shadow 0x80): the
             * depth image's value as the Z24S8 dword's bytes, alpha depth
             * bits 16-23, red 8-15, green 0-7, blue the stencil (read as 0);
             * filtered per byte as the texture unit would (0x04). */
            int lin = state->tex_shadow[i] & 0x04;
            EMIT("    {\n");
            EMIT("        vec2 uv = i_t%d.xy;\n", i);
            EMIT("        if (i_t%d.w != 0.0 && i_t%d.w != 1.0) uv /= i_t%d.w;\n", i, i, i);
            EMIT("        vec2 ts = vec2(textureSize(tex%d, 0));\n", i);
            EMIT("        vec2 tc = uv * pc.tex_scale[%d].xy * ts%s;\n", i, lin ? " - 0.5" : "");
            EMIT("        ivec2 i0 = ivec2(floor(tc));\n");
            EMIT("        vec2 f = %s;\n", lin ? "tc - floor(tc)" : "vec2(0.0)");
            EMIT("        x%d = vec4(0.0);\n", 8 + i);
            EMIT("        for (int k = 0; k < %d; k++) {\n", lin ? 4 : 1);
            EMIT("            ivec2 o = ivec2(k & 1, k >> 1);\n");
            EMIT("            uint d = uint(round(texelFetch(tex%d, clamp(i0 + o, ivec2(0), ivec2(ts) - 1), 0).r"
                 " * 16777215.0));\n", i);
            EMIT("            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n");
            EMIT("            x%d += vec4(float((d >> 8) & 255u), float(d & 255u), 0.0, float(d >> 16))"
                 " * (w / 255.0);\n", 8 + i);
            EMIT("        }\n");
            EMIT("    }\n");
        } else if (state->tex_shadow[i]) {
            static const char *const op[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
            int sh = state->tex_shadow[i], kind = sh & 3, func = (sh >> 2) & 7;
            if (func == 0 || func == 7) {
                EMIT("    x%d = vec4(%s);\n", 8 + i, func ? "1.0" : "0.0");
            } else {
                EMIT("    {\n");
                EMIT("        vec4 pt = i_t%d;\n", i);
                EMIT("        float q = pt.w != 0.0 ? pt.w : 1.0;\n");
                EMIT("        vec2 ts = vec2(textureSize(tex%d, 0));\n", i);
                EMIT("        vec2 tc = pt.xy / q * pc.tex_scale[%d].xy * ts - 0.5;\n", i);
                EMIT("        ivec2 i0 = ivec2(floor(tc));\n");
                EMIT("        vec2 f = tc - floor(tc);\n");
                EMIT("        float ref = %s;\n", (sh & 0x20)
                     ? (kind == 2 ? "clamp(pt.z / q, 0.0, 16777215.0)" : "clamp(pt.z / q, 0.0, 65535.0)")
                     : "0.0");
                EMIT("        float lit = 0.0;\n");
                EMIT("        for (int k = 0; k < 4; k++) {\n");
                EMIT("            ivec2 o = ivec2(k & 1, k >> 1);\n");
                EMIT("            vec4 c = texelFetch(tex%d, clamp(i0 + o, ivec2(0), ivec2(ts) - 1), 0);\n", i);
                if (sh & 0x40)
                    EMIT("            float d = c.r * %s;\n", kind == 2 ? "16777215.0" : "65535.0");
                else if (kind == 2)
                    EMIT("            float d = round(c.r * 255.0) * 65536.0 + round(c.g * 255.0) * 256.0"
                         " + round(c.b * 255.0);\n");
                else
                    EMIT("            float d = round(c.r * 255.0) * 256.0 + round(c.g * 255.0);\n");
                EMIT("            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n");
                EMIT("            lit += (d %s ref ? 1.0 : 0.0) * w;\n", op[func]);
                EMIT("        }\n");
                EMIT("        x%d = vec4(lit);\n", 8 + i);
                EMIT("    }\n");
            }
        } else if (m == NV2A_TEXMODE_3D || m == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    x%d = texture(tex%d, i_t%d.xyz, pc.tex_bias[%d]);\n", 8 + i, i, i, i);
        } else if ((m == NV2A_TEXMODE_BUMPENVMAP || m == NV2A_TEXMODE_BUMPENVMAP_LUM)
                   && i > 0) {
            /* As d3d8_combiners_msl.c (xemu psh.c PS_TEXTUREMODES_BUMPENVMAP):
             * the previous stage's (b, g) as signed, turned by this stage's
             * bump matrix and added to its coordinate; _LUM scales the texel
             * by scale * l + offset. */
            int p = i == 1 ? 0 : state->input_tex[i] < i ? state->input_tex[i] : i - 1;
            int sg = state->tex_signed[p];
            EMIT("    {\n");
            if ((sg & 3u) == 3u)
                EMIT("        vec2 dsdt = vec2(x%d.b, x%d.g);\n", 8 + p, 8 + p);
            else
                EMIT("        vec2 dsdt = vec2(rc_sign8(x%d.b), rc_sign8(x%d.g));\n",
                     8 + p, 8 + p);
            EMIT("        vec4 bm = pc.bump_mat[%d];\n", i);
            EMIT("        dsdt = vec2(bm.x * dsdt.x + bm.z * dsdt.y,"
                 " bm.y * dsdt.x + bm.w * dsdt.y);\n");
            EMIT("        x%d = texture(tex%d, (i_t%d.xy + dsdt) * pc.tex_scale[%d].xy,"
                 " pc.tex_bias[%d]);\n", 8 + i, i, i, i, i);
            if (m == NV2A_TEXMODE_BUMPENVMAP_LUM) {
                if (sg & 4u)
                    EMIT("        float l = x%d.r >= 0.0 ? x%d.r * 0.5 : 1.0 + x%d.r * 0.5;\n",
                         8 + p, 8 + p, 8 + p);
                else
                    EMIT("        float l = x%d.r;\n", 8 + p);
                EMIT("        x%d *= pc.bump_lum[%d].x * l + pc.bump_lum[%d].y;\n", 8 + i, i, i);
            }
            EMIT("    }\n");
        } else {
            EMIT("    {\n");
            EMIT("        vec2 uv = i_t%d.xy;\n", i);
            if (m == NV2A_TEXMODE_2D)
                /* As the MSL: only true 2D stages divide by q. */
                EMIT("        if (pc.tex_mode[%d] == 1u && i_t%d.w != 0.0 && i_t%d.w != 1.0)"
                     " uv /= i_t%d.w;\n", i, i, i, i);
            EMIT("        x%d = texture(tex%d, uv * pc.tex_scale[%d].xy, pc.tex_bias[%d]);\n",
                 8 + i, i, i, i);
            if (hdr && i == 0 && m == NV2A_TEXMODE_2D)
                /* BO-195 P3: pc.hdr.w is the span in mip levels over which a gained draw's
                 * gain fades out (0: no fade); the unclamped LOD, the texture may have one level */
                EMIT("        if (pc.hdr.w > 0.0)\n"
                     "            hlod = clamp(1.0 - max(textureQueryLod(tex0, uv * pc.tex_scale[0].xy).y, 0.0)"
                     " / pc.hdr.w, 0.0, 1.0);\n");
            EMIT("    }\n");
        }
        if (state->tex_signed[i]) {
            /* Signed channels (stored + 128, d3d8_combiners_parse.h): the
             * filtered byte back to two's complement, / 127, as the MSL. */
            static const char ch[4] = { 'b', 'g', 'r', 'a' };
            for (k = 0; k < 4; k++)
                if (state->tex_signed[i] & (1u << k))
                    EMIT("    x%d.%c = max((x%d.%c * 255.0 - 128.0) / 127.0, -1.0);\n",
                         8 + i, ch[k], 8 + i, ch[k]);
        }
        if (state->alpha_kill & (1u << i))
            EMIT("    if (x%d.a == 0.0) discard;\n", 8 + i);
        EMIT("    if (pc.alpha_only[%d] != 0u) x%d.rgb = vec3(1.0);\n", i, 8 + i);
    }
    EMIT("    x12.a = x8.a;\n");

    /* ---- General combiner stages ---- */
    for (s = 0; s < state->num_stages; s++) {
        const NV2ACombinerStage *st = &state->stages[s];
        const NV2ACombinerOutput *ro = &st->rgb_output, *ao = &st->alpha_output;

        EMIT("    {   /* stage %d */\n", s);
        EMIT("        x1 = pc.c0[%d]; x2 = pc.c1[%d];\n",
             state->uniq_c0 ? s : 0, state->uniq_c1 ? s : 0);
        if (ro->mux_sum || ao->mux_sum) {
            if (state->mux_msb)
                EMIT("        bool sel = x12.a >= 0.5;\n");
            else
                EMIT("        bool sel = ((uint(saturate(x12.a) * 255.0 + 0.5)) & 1u) != 0u;\n");
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->rgb_input[k], 0, src, sizeof src);
            EMIT("        vec3 %s = rc_map3(%s, %d);\n",
                 in_name[k], src, (int)st->rgb_input[k].mapping);
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->alpha_input[k], 1, src, sizeof src);
            EMIT("        float a%s = rc_map(%s, %d);\n",
                 in_name[k], src, (int)st->alpha_input[k].mapping);
        }
        EMIT("        vec3 ab = %s;\n", ro->ab_dot ? "vec3(dot(A, B))" : "A * B");
        EMIT("        vec3 cd = %s;\n", ro->cd_dot ? "vec3(dot(C, D))" : "C * D");
        EMIT("        vec3 sum = %s;\n", ro->mux_sum ? "sel ? cd : ab" : "ab + cd");
        EMIT("        ab = rc_op3(ab, %d%s); cd = rc_op3(cd, %d%s); sum = rc_op3(sum, %d%s);\n",
             (int)ro->output_map, OPHI, (int)ro->output_map, OPHI, (int)ro->output_map, OPHI);
        EMIT("        float aab = aA * aB, acd = aC * aD;\n");
        EMIT("        float asum = %s;\n", ao->mux_sum ? "sel ? acd : aab" : "aab + acd");
        if (ro->ab_dst)  EMIT("        x%d.rgb = ab;\n",  (int)ro->ab_dst);
        if (ro->cd_dst)  EMIT("        x%d.rgb = cd;\n",  (int)ro->cd_dst);
        if (ro->sum_dst) EMIT("        x%d.rgb = sum;\n", (int)ro->sum_dst);
        if (ao->ab_dst)  EMIT("        x%d.a = rc_op(aab, %d%s);\n",  (int)ao->ab_dst,  (int)ao->output_map, OPHA);
        if (ao->cd_dst)  EMIT("        x%d.a = rc_op(acd, %d%s);\n",  (int)ao->cd_dst,  (int)ao->output_map, OPHA);
        if (ao->sum_dst) EMIT("        x%d.a = rc_op(asum, %d%s);\n", (int)ao->sum_dst, (int)ao->output_map, OPHA);
        if (ro->ab_blue_to_alpha && ro->ab_dst)
            EMIT(hdr ? "        x%d.a = min(ab.b, 1.0);\n" : "        x%d.a = ab.b;\n", (int)ro->ab_dst);
        if (ro->cd_blue_to_alpha && ro->cd_dst)
            EMIT(hdr ? "        x%d.a = min(cd.b, 1.0);\n" : "        x%d.a = cd.b;\n", (int)ro->cd_dst);
        EMIT("    }\n");
    }

    /* ---- Final combiner ---- */
    EMIT("    vec4 res;\n");
    EMIT("    {   /* final combiner */\n");
    EMIT("        x1 = pc.fc0; x2 = pc.fc1;\n");
    {
        /* E and F first, then the special registers, then A..D and G,
         * which can read them. */
        static const char *const fname[7] = { "A", "B", "C", "D", "E", "F", "G" };
        static const int order[7] = { 4, 5, -1, 0, 1, 2, 3 };
        for (i = 0; i < 7; i++) {
            const NV2ACombinerInput *in;
            k = order[i];
            if (k < 0) {
                /* COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20 (xemu psh_regs.h). */
                if (!hdr) {
                EMIT("        vec3 fv1 = %ssaturate(x5.rgb);\n",
                     (state->final_flags & 0x40) ? "1.0 - " : "");
                EMIT("        vec3 fr0 = %ssaturate(x12.rgb);\n",
                     (state->final_flags & 0x20) ? "1.0 - " : "");
                EMIT("        x14 = vec4(%s(fv1 + fr0), 0.0);\n",
                     (state->final_flags & 0x80) ? "saturate" : "");
                } else {
                EMIT("        vec3 fv1 = %s;\n", (state->final_flags & 0x40)
                     ? "1.0 - saturate(x5.rgb)" : "clamp(x5.rgb, 0.0, hi)");
                EMIT("        vec3 fr0 = %s;\n", (state->final_flags & 0x20)
                     ? "1.0 - saturate(x12.rgb)" : "clamp(x12.rgb, 0.0, hi)");
                EMIT("        x14 = vec4(%s, 0.0);\n", (state->final_flags & 0x80)
                     ? "clamp(fv1 + fr0, 0.0, hi)" : "(fv1 + fr0)");
                }
                EMIT("        x15 = vec4(E * F, 0.0);\n");
                continue;
            }
            in = &state->final_input[k];
            input_src(in, 0, src, sizeof src);
            if (!hdr || ((int)in->mapping & 1))
                EMIT("        vec3 %s = %ssaturate(%s);\n", fname[k],
                     ((int)in->mapping & 1) ? "1.0 - " : "", src);
            else
                EMIT("        vec3 %s = clamp(%s, 0.0, hi);\n", fname[k], src);
        }
        input_src(&state->final_input[6], 1, src, sizeof src);
        EMIT("        float G = %ssaturate(%s);\n",
             ((int)state->final_input[6].mapping & 1) ? "1.0 - " : "", src);
    }
    if (!hdr)
        EMIT("        res = vec4(saturate(A * B + (1.0 - A) * C + D), G);\n");
    else
        EMIT("        res = vec4(clamp(A * B + (1.0 - saturate(A)) * C + D, 0.0, hi), G);\n");
    EMIT("    }\n");

    /* ---- Alpha test: D3DCMP numbering on 8-bit values ---- */
    EMIT("    if (pc.atest.z != 0u) {\n"
         "        float ref = uintBitsToFloat(pc.atest.x);\n"
         "        uint a = uint(saturate(res.a) * 255.0 + 0.5), r = uint(ref * 255.0 + 0.5);\n"
         "        bool ok = true;\n"
         "        uint f = pc.atest.y;\n"
         "        if      (f == 1u) ok = false;\n"
         "        else if (f == 2u) ok = a <  r;\n"
         "        else if (f == 3u) ok = a == r;\n"
         "        else if (f == 4u) ok = a <= r;\n"
         "        else if (f == 5u) ok = a >  r;\n"
         "        else if (f == 6u) ok = a != r;\n"
         "        else if (f == 7u) ok = a >= r;\n"
         "        if (!ok) discard;\n"
         "    }\n");
    if (hdr)    /* BO-110: the draw's factor (the HUD's white over paper white for UI draws) */
        EMIT(/* a gained draw's gain fades with the mip (BO-195 P3) */
             "    float hgy = pc.hdr.w > 0.0 ? 1.0 + (pc.hdr.y - 1.0) * hlod : pc.hdr.y;\n"
             "    float hgz = pc.hdr.w > 0.0 ? 1.0 + (pc.hdr.z - 1.0) * hlod : pc.hdr.z;\n"
             "    res.rgb *= hgy;\n"
             /* a light texture's highlight-only gain (nv2a_texrep_register_hdr_gain) */
             "    if (hgz != 1.0) {\n"
             "        float hs = smoothstep(0.6, 1.0, max(res.r, max(res.g, res.b)));\n"
             "        res.rgb *= 1.0 + (hgz - 1.0) * hs * hs;\n"
             "    }\n");
    if (state->debug_show > 4) {
        /* vk_fs_show=lod<n>: the mip level the stage samples, /10 in red;
         * green the level the derivatives alone give. */
        int n = state->debug_show - 5;
        EMIT("    {   /* vk_fs_show lod */\n");
        EMIT("        vec2 uv = i_t%d.xy;\n", n);
        EMIT("        if (pc.tex_mode[%d] == 1u && i_t%d.w != 0.0 && i_t%d.w != 1.0) uv /= i_t%d.w;\n",
             n, n, n, n);
        EMIT("        vec2 q = textureQueryLod(tex%d, uv * pc.tex_scale[%d].xy);\n", n, n);
        EMIT("        res = vec4(q.x / 10.0, q.y / 10.0, 0.0, 1.0);\n");
        EMIT("    }\n");
    } else if (state->debug_show) {
        int n = state->debug_show - 1;
        EMIT("    {   /* vk_fs_show */\n");
        EMIT("        vec2 uv = i_t%d.xy;\n", n);
        EMIT("        if (pc.tex_mode[%d] == 1u && i_t%d.w != 0.0 && i_t%d.w != 1.0) uv /= i_t%d.w;\n",
             n, n, n, n);
        EMIT("        uv *= pc.tex_scale[%d].xy;\n", n);
        EMIT("        res = vec4(clamp(uv, 0.0, 1.0), any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv,"
             " vec2(1.0))) ? 1.0 : 0.0, 1.0);\n");
        EMIT("    }\n");
    }
    EMIT("    o_color = res;\n");
    EMIT("}\n");
    return off;
}

#undef EMIT

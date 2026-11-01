/**
 * NV2A register combiners -> MSL fragment function (see d3d8_msl.h). Line
 * for line d3d8_combiners_hlsl.c; only the syntax differs (textures and
 * constants are function parameters, sample(), discard_fragment()).
 */

#include "d3d8_msl.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * MSL Code Generation
 *
 * The shader mirrors nv2a_rc_eval_regs (src/kernel/nv2a_combiner.c), so
 * the GPU and CPU paths agree:
 *   - registers x0..x15 by register code: 0 zero, 1 C0, 2 C1, 3 fog,
 *     4 V0, 5 V1, 8-11 T0-T3, 12 R0 (alpha starts as T0's), 13 R1,
 *     14 V1+R0 and 15 E*F (final combiner)
 *   - in each stage all inputs are read before any output is written;
 *     the RGB writes go AB, CD, sum, then alpha AB, CD, sum, then the
 *     blue-to-alpha copies
 *   - outputs are mapped, then clamped to [-1, 1]
 *   - the final combiner computes A*B + (1-A)*C + D with clamped,
 *     optionally inverted inputs, and alpha G
 *   - the alpha test compares 8-bit values
 * ================================================================ */

#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/* An input as float3 (RGB portion) or float (alpha portion), before
 * mapping. */
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
    "float3 rc_map3(float3 x, int m) {\n"
    "    return float3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
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
    "float3 rc_op3(float3 x, int op) {\n"
    "    return float3(rc_op(x.r, op), rc_op(x.g, op), rc_op(x.b, op));\n"
    "}\n";

/* BO-110, HDR variant: the same helpers with the upper bound of a value a
 * parameter (pc.hdr.x, per draw): 1.0 is the hardware's clamp, a larger one
 * keeps light that stacks past 1.0 in a half-float target. The lower bounds
 * and every [0, 1] clamp that feeds a complement or a selector stay. */
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
    "float3 rc_map3(float3 x, int m) {\n"
    "    return float3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
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
    "float3 rc_op3(float3 x, int op, float hi) {\n"
    "    return float3(rc_op(x.r, op, hi), rc_op(x.g, op, hi), rc_op(x.b, op, hi));\n"
    "}\n";

/* The constants with the HDR member after Metal's own tail (MsPsConsts). */
#define D3D8_MSL_PSCONSTS_HDR \
    "struct PsConsts {\n" \
    "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n" \
    "    uint4 atest; uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n" \
    "    float4 tex_bias;\n" \
    "    float4 bump_mat[4]; float4 bump_lum[4];\n" \
    "    float4 hdr;\n" \
    "};\n"

static int generate(const NV2ACombinerState *state, char *buf, int bufsize, int hdr);

int d3d8_combiners_generate_msl(const NV2ACombinerState *state,
                                char *buf, int bufsize)
{
    return generate(state, buf, bufsize, 0);
}

int d3d8_combiners_generate_msl_hdr(const NV2ACombinerState *state,
                                    char *buf, int bufsize)
{
    return generate(state, buf, bufsize, 1);
}

static int generate(const NV2ACombinerState *state, char *buf, int bufsize, int hdr)
{
    /* hdr: rc_op's bound, the per-draw one for colour and 1.0 for alpha
     * (an alpha past 1.0 would turn a blend's (1 - alpha) negative). */
    const char *OPHI = hdr ? ", hi" : "", *OPHA = hdr ? ", 1.0" : "";
    static const char *const in_name[4] = { "A", "B", "C", "D" };
    char src[32];
    int off = 0;
    int i, s, k;

    EMIT("/* Auto-generated NV2A register combiners (MSL) */\n"
         "#include <metal_stdlib>\n"
         "using namespace metal;\n");
    EMIT("%s", hdr ? D3D8_MSL_PSCONSTS_HDR : D3D8_MSL_PSCONSTS);
    EMIT("%s", D3D8_MSL_VOUT);
    EMIT("%s", hdr ? s_rc_helpers_hdr : s_rc_helpers);

    EMIT("fragment float4 fs_main(VOut i [[stage_in]],\n"
         "                        constant PsConsts &pc [[buffer(0)]]");
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE || m == NV2A_TEXMODE_PASSTHRU
                || m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        EMIT(",\n                        %s tex%d [[texture(%d)]], sampler samp%d [[sampler(%d)]]",
             m == NV2A_TEXMODE_3D ? "texture3d<float>" :
             m == NV2A_TEXMODE_CUBEMAP ? "texturecube<float>" : "texture2d<float>",
             i, i, i, i);
    }
    EMIT(") {\n");
    if (hdr)
        EMIT("    float hi = pc.hdr.x;\n");
    for (k = 0; k < 16; k++)
        EMIT("    float4 x%d = float4(0.0);\n", k);
    if (state->fog_input)
        EMIT("    x3 = float4(pc.fog_color.rgb, saturate(i.fog));\n");
    else
        EMIT("    x3 = pc.fog_color;\n");
    EMIT("    x4 = i.d0;\n");
    EMIT("    x5 = i.d1;\n");

    /* ---- Texture stages ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE) {
            /* (0, 0, 0, 1), as xemu: see NV2ATextureMode */
            EMIT("    x%d = float4(0.0, 0.0, 0.0, 1.0);\n", 8 + i);
            continue;
        }
        if (m == NV2A_TEXMODE_CLIPPLANE) {
            /* xemu psh.c PS_TEXTUREMODES_CLIPPLANE: each coordinate
             * component against 0, by the stage's compare mode; the
             * register reads 0 (left at its initial value). */
            for (k = 0; k < 4; k++)
                EMIT("    if (i.t%d.%c %s 0.0) discard_fragment();\n", i, "xyzw"[k],
                     (state->clip_mode[i] >> k) & 1u ? ">=" : "<");
            continue;
        }
        if (m == NV2A_TEXMODE_PASSTHRU) {
            EMIT("    x%d = saturate(i.t%d);\n", 8 + i, i);
            continue;
        }
        if (m == NV2A_TEXMODE_3D || m == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    x%d = tex%d.sample(samp%d, i.t%d.xyz, bias(pc.tex_bias[%d]));\n",
                 8 + i, i, i, i, i);
        } else if ((m == NV2A_TEXMODE_BUMPENVMAP || m == NV2A_TEXMODE_BUMPENVMAP_LUM)
                   && i > 0) {
            /* xemu psh.c PS_TEXTUREMODES_BUMPENVMAP(_LUM): (du, dv) is the
             * previous stage's (b, g) as signed bytes -- already in [-1, 1]
             * when its filter reads them signed (tex_signed), else taken as
             * two's complement here, as xemu's sign3 -- turned by the stage's
             * bump matrix (SET_TEXTURE_SET_BUMP_ENV_MAT, column-major as
             * xemu uploads it) and added to this stage's coordinate. _LUM
             * then scales the texel by scale * l + offset, l the previous
             * stage's r. */
            int p = i == 1 ? 0 : state->input_tex[i] < i ? state->input_tex[i] : i - 1;
            int sg = state->tex_signed[p];
            EMIT("    {\n");
            if ((sg & 3u) == 3u)
                EMIT("        float2 dsdt = float2(x%d.b, x%d.g);\n", 8 + p, 8 + p);
            else
                EMIT("        float2 dsdt = float2(rc_sign8(x%d.b), rc_sign8(x%d.g));\n",
                     8 + p, 8 + p);
            EMIT("        float4 bm = pc.bump_mat[%d];\n", i);
            EMIT("        dsdt = float2(bm.x * dsdt.x + bm.z * dsdt.y,"
                 " bm.y * dsdt.x + bm.w * dsdt.y);\n");
            EMIT("        x%d = tex%d.sample(samp%d, (i.t%d.xy + dsdt) * pc.tex_scale[%d].xy,"
                 " bias(pc.tex_bias[%d]));\n", 8 + i, i, i, i, i, i);
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
            EMIT("        float2 uv = i.t%d.xy;\n", i);
            if (m == NV2A_TEXMODE_2D)
                /* As the HLSL: only true 2D stages divide by q. */
                EMIT("        if (pc.tex_mode[%d] == 1u && i.t%d.w != 0.0 && i.t%d.w != 1.0)"
                     " uv /= i.t%d.w;\n", i, i, i, i);
            EMIT("        x%d = tex%d.sample(samp%d, uv * pc.tex_scale[%d].xy,"
                 " bias(pc.tex_bias[%d]));\n", 8 + i, i, i, i, i);
            EMIT("    }\n");
        }
        if (state->tex_signed[i]) {
            /* BO-45: signed channels (stored + 128, d3d8_combiners_parse.h):
             * the filtered byte back to two's complement, then / 127 with
             * -128 at -1, as a signed 8-bit texel reads on the NV2A (xemu
             * #587 has no support yet; D3D's signed formats map the same). */
            static const char ch[4] = { 'b', 'g', 'r', 'a' };
            for (k = 0; k < 4; k++)
                if (state->tex_signed[i] & (1u << k))
                    EMIT("    x%d.%c = max((x%d.%c * 255.0 - 128.0) / 127.0, -1.0);\n",
                         8 + i, ch[k], 8 + i, ch[k]);
        }
        if (state->alpha_kill & (1u << i))
            EMIT("    if (x%d.a == 0.0) discard_fragment();\n", 8 + i);
        EMIT("    if (pc.alpha_only[%d] != 0u) x%d.rgb = float3(1.0);\n", i, 8 + i);
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
                EMIT("        bool sel = (((uint)(saturate(x12.a) * 255.0 + 0.5)) & 1u) != 0u;\n");
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->rgb_input[k], 0, src, sizeof src);
            EMIT("        float3 %s = rc_map3(%s, %d);\n",
                 in_name[k], src, (int)st->rgb_input[k].mapping);
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->alpha_input[k], 1, src, sizeof src);
            EMIT("        float a%s = rc_map(%s, %d);\n",
                 in_name[k], src, (int)st->alpha_input[k].mapping);
        }
        EMIT("        float3 ab = %s;\n", ro->ab_dot ? "float3(dot(A, B))" : "A * B");
        EMIT("        float3 cd = %s;\n", ro->cd_dot ? "float3(dot(C, D))" : "C * D");
        EMIT("        float3 sum = %s;\n", ro->mux_sum ? "sel ? cd : ab" : "ab + cd");
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
    EMIT("    float4 res;\n");
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
                EMIT("        float3 fv1 = %ssaturate(x5.rgb);\n",
                     (state->final_flags & 0x40) ? "1.0 - " : "");
                EMIT("        float3 fr0 = %ssaturate(x12.rgb);\n",
                     (state->final_flags & 0x20) ? "1.0 - " : "");
                EMIT("        x14 = float4(%s(fv1 + fr0), 0.0);\n",
                     (state->final_flags & 0x80) ? "saturate" : "");
                } else {
                EMIT("        float3 fv1 = %s;\n", (state->final_flags & 0x40)
                     ? "1.0 - saturate(x5.rgb)" : "clamp(x5.rgb, 0.0, hi)");
                EMIT("        float3 fr0 = %s;\n", (state->final_flags & 0x20)
                     ? "1.0 - saturate(x12.rgb)" : "clamp(x12.rgb, 0.0, hi)");
                EMIT("        x14 = float4(%s, 0.0);\n", (state->final_flags & 0x80)
                     ? "clamp(fv1 + fr0, 0.0, hi)" : "(fv1 + fr0)");
                }
                EMIT("        x15 = float4(E * F, 0.0);\n");
                continue;
            }
            in = &state->final_input[k];
            input_src(in, 0, src, sizeof src);
            if (!hdr || ((int)in->mapping & 1))
                EMIT("        float3 %s = %ssaturate(%s);\n", fname[k],
                     ((int)in->mapping & 1) ? "1.0 - " : "", src);
            else
                EMIT("        float3 %s = clamp(%s, 0.0, hi);\n", fname[k], src);
        }
        input_src(&state->final_input[6], 1, src, sizeof src);
        EMIT("        float G = %ssaturate(%s);\n",
             ((int)state->final_input[6].mapping & 1) ? "1.0 - " : "", src);
    }
    if (!hdr)
        EMIT("        res = float4(saturate(A * B + (1.0 - A) * C + D), G);\n");
    else
        EMIT("        res = float4(clamp(A * B + (1.0 - saturate(A)) * C + D, 0.0, hi), G);\n");
    EMIT("    }\n");

    /* ---- Alpha test: D3DCMP numbering on 8-bit values ---- */
    EMIT("    if (pc.atest.z != 0u) {\n"
         "        float ref = as_type<float>(pc.atest.x);\n"
         "        uint a = (uint)(saturate(res.a) * 255.0 + 0.5), r = (uint)(ref * 255.0 + 0.5);\n"
         "        bool ok = true;\n"
         "        uint f = pc.atest.y;\n"
         "        if      (f == 1u) ok = false;\n"
         "        else if (f == 2u) ok = a <  r;\n"
         "        else if (f == 3u) ok = a == r;\n"
         "        else if (f == 4u) ok = a <= r;\n"
         "        else if (f == 5u) ok = a >  r;\n"
         "        else if (f == 6u) ok = a != r;\n"
         "        else if (f == 7u) ok = a >= r;\n"
         "        if (!ok) discard_fragment();\n"
         "    }\n");
    EMIT("    return res;\n");
    EMIT("}\n");
    return off;
}

#undef EMIT

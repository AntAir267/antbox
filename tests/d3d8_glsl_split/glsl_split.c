/*
 * d3d8_glsl_split: the GLSL emitters (d3d8_glsl.h) on d3d8_msl_split's
 * inputs -- NPROG pseudo-random vertex programs through d3d8_vsh_parse and
 * d3d8_vsh_generate_glsl (screen-space, as the backend uses it), NCOMB
 * pseudo-random combiner register sets through d3d8_combiners_from_regs and
 * d3d8_combiners_generate_glsl -- each compiled to SPIR-V with glslang
 * (nv2a_vk_glsl.c, the disk cache off). Any compile error fails the test.
 * GLSL_DUMP=<path> writes all the text.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_glsl.h"
#include "nv2a_vk_glsl.h"
#include "nv2a_backend_common.h"

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define NPROG 400
#define NCOMB 400

static uint32_t s_seed = 0x2f6b1d3u;   /* d3d8_msl_split's sequence */
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed ^ (s_seed >> 15);
}

static int s_bad, s_cases;
static FILE *s_dump;

static void check(const char *what, int idx, enum nvk_stage st, const char *text, int n)
{
    static char err[16384];
    const uint32_t *w;
    size_t nw;
    int compiled;
    s_cases++;
    if (s_dump && n > 0)
        fprintf(s_dump, "// %s %d\n%s\n", what, idx, text);
    if (n <= 0) {
        if (++s_bad <= 5)
            printf("FAIL: %s %d: no GLSL\n", what, idx);
        return;
    }
    if (!nvk_glsl_spirv(st, text, &w, &nw, &compiled, err, sizeof err) && ++s_bad <= 5)
        printf("FAIL: %s %d does not compile:\n%s\n%s\n", what, idx, err, text);
}

int main(void)
{
    static uint32_t prog[NV2A_VS_MAX_INSTRUCTIONS * 4];
    static NV2AVshProgram parsed;
    static char buf[256 * 1024];
    const char *dump = getenv("GLSL_DUMP");
    struct nv2a_host_opts o;
    int i, k, n;

    memset(&o, 0, sizeof o);
    o.shader_cache = "none";
    nv2a_host_opts_set(&o);
    if (dump && !(s_dump = fopen(dump, "wb"))) {
        fprintf(stderr, "cannot write %s\n", dump);
        return 2;
    }
    for (i = 0; i < NPROG; i++) {
        int len = 1 + (int)(rnd() % 40), fin = (int)(rnd() % (uint32_t)len);
        for (k = 0; k < len * 4; k++)
            prog[k] = rnd();
        for (k = 0; k < len; k++)
            prog[k * 4 + 3] &= ~1u;
        prog[fin * 4 + 3] |= 1u;
        d3d8_vsh_parse(prog, len, &parsed);
        n = d3d8_vsh_generate_glsl(&parsed, D3D8_GLSL_SCREEN_SPACE, buf, (int)sizeof buf);
        check("vsh-ss", i, NVK_STAGE_VERTEX, buf, n);
    }
    for (i = 0; i < NCOMB; i++) {
        uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
        uint32_t control, fcw0, fcw1, modes;
        NV2ACombinerState st;
        for (k = 0; k < 8; k++) {
            cicw[k] = rnd(); aicw[k] = rnd(); cocw[k] = rnd(); aocw[k] = rnd();
        }
        control = (rnd() & ~0xFFu) | (1 + rnd() % 8);
        fcw0 = rnd(); fcw1 = rnd(); modes = rnd();
        d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control, fcw0, fcw1, modes, &st);
        st.fog_input = (int)(rnd() & 1);
        n = d3d8_combiners_generate_glsl(&st, buf, (int)sizeof buf);
        check("rc", i, NVK_STAGE_FRAGMENT, buf, n);
    }
    if (s_dump)
        fclose(s_dump);
    printf("glsl_split: %d cases, %d failed\n", s_cases, s_bad);
    return s_bad ? 1 : 0;
}

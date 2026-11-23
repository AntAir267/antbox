/*
 * nv2a_wide_lines (BO-385): nv2a_lines_expand against hand-placed lines.
 *
 *   1. Pre-transformed vertices (no program): a horizontal, a vertical and
 *      a diagonal segment become quads one guest pixel wide about the line,
 *      the ends' colours carried to their corners, two triangles each.
 *   2. A vertex program (mov oPos, v0; mov oD0, v3), run on the CPU: the
 *      position comes out with 1/w, a segment with an end behind the eye
 *      (w <= 0) is left out.
 *   3. nv2a_wide_lines: only a line list, only into a scaled target, not
 *      with fog on.
 *
 * Exit 0 when every check holds.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_common.h"
#include "nv2a_vsh_cpu.h"

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

/* The vertex program encoder of tests/nv2a_vsh (xemu vsh.c's field table). */
static void put(uint32_t *w, int word, int shift, int bits, uint32_t v)
{
    w[word] &= ~(((1u << bits) - 1u) << shift);
    w[word] |= (v & ((1u << bits) - 1u)) << shift;
}
enum { V = 2 };
static void src0_v(uint32_t *w, int reg)
{
    put(w, 1, 6, 2, 0); put(w, 1, 4, 2, 1); put(w, 1, 2, 2, 2); put(w, 1, 0, 2, 3);
    put(w, 2, 26, 2, V);
    put(w, 1, 9, 4, (uint32_t)reg);
}
static void mov_o(uint32_t *w, int oaddr)
{
    put(w, 1, 21, 4, 1);                        /* mov */
    put(w, 3, 12, 4, 0xF); put(w, 3, 11, 1, 1);
    put(w, 3, 3, 8, (uint32_t)oaddr); put(w, 3, 2, 1, 0);
}

static struct nv2a_pb_vsh s_v;
static VertexAttr s_attr[NV_VERTEX_ATTRS];
static float s_mem[64][4];       /* "guest memory": attribute arrays */

/* Attribute a as a float4 array at s_mem[base..]. */
static void arr(int a, int base)
{
    s_attr[a].offset = (uint32_t)(base * 16);
    s_attr[a].type = 2;          /* float */
    s_attr[a].size = 4;
    s_attr[a].stride = 16;
}

static uint32_t expand(int program, const uint32_t *lidx, uint32_t n, uint32_t count,
                       float *out, uint32_t *oidx)
{
    struct nv2a_pb_draw d;
    memset(&d, 0, sizeof d);
    d.attr = s_attr;
    d.program = program;
    d.prim = NV_PRIM_LINES;
    return nv2a_lines_expand(&d, &s_v, lidx, n, 0, count, (const uint8_t *)s_mem, out, oidx);
}

/* Corner c of segment s: position x, y. */
static const float *corner(const float *out, int s, int c)
{
    return out + (size_t)(4 * s + c) * NV2A_WL_F4 * 4u;
}

static void check_pass(void)
{
    static const float pos[6][4] = {
        { 10, 10, 100, 1 }, { 30, 10, 100, 1 },      /* horizontal */
        { 50, 20, 200, 1 }, { 50, 60, 200, 1 },      /* vertical */
        { 0, 0, 0, 1 },     { 3, 4, 0, 1 },          /* diagonal, length 5 */
    };
    static const uint32_t lidx[6] = { 0, 1, 2, 3, 4, 5 };
    float out[12 * NV2A_WL_F4 * 4];
    uint32_t oidx[18], n, i;
    const float *p;

    memset(s_attr, 0, sizeof s_attr);
    memset(&s_v, 0, sizeof s_v);
    for (i = 0; i < 6; i++) {
        memcpy(s_mem[1 + i], pos[i], 16);
        s_mem[20 + i][0] = (float)i;             /* oD0.r: which vertex */
        s_mem[20 + i][3] = 1.0f;
    }
    arr(0, 1);
    arr(3, 20);
    n = expand(0, lidx, 6, 6, out, oidx);
    CHECK(n == 18, "pass: %u indices, want 18", n);
    /* Horizontal: corners at y 10 +- 0.5, x at the ends. */
    p = corner(out, 0, 0);
    CHECK(NEAR(p[0], 10) && NEAR(p[1], 10.5f) && NEAR(p[2], 100) && NEAR(p[4], 0),
          "pass: horizontal corner 0 (%g, %g, %g) d0 %g", p[0], p[1], p[2], p[4]);
    p = corner(out, 0, 1);
    CHECK(NEAR(p[0], 10) && NEAR(p[1], 9.5f), "pass: horizontal corner 1 (%g, %g)", p[0], p[1]);
    p = corner(out, 0, 3);
    CHECK(NEAR(p[0], 30) && NEAR(p[1], 9.5f) && NEAR(p[4], 1),
          "pass: horizontal corner 3 (%g, %g) d0 %g", p[0], p[1], p[4]);
    /* Vertical: corners at x 50 -+ 0.5. */
    p = corner(out, 1, 0);
    CHECK(NEAR(p[0], 49.5f) && NEAR(p[1], 20), "pass: vertical corner 0 (%g, %g)", p[0], p[1]);
    p = corner(out, 1, 3);
    CHECK(NEAR(p[0], 50.5f) && NEAR(p[1], 60), "pass: vertical corner 3 (%g, %g)", p[0], p[1]);
    /* Diagonal (3, 4), length 5: half a pixel along the normal (-4, 3) / 5. */
    p = corner(out, 2, 0);
    CHECK(NEAR(p[0], -0.4f) && NEAR(p[1], 0.3f), "pass: diagonal corner 0 (%g, %g)", p[0], p[1]);
    /* Two triangles a segment: 0 1 2, 2 1 3 from its first corner. */
    CHECK(oidx[6] == 4 && oidx[7] == 5 && oidx[8] == 6 && oidx[9] == 6 && oidx[10] == 5
          && oidx[11] == 7, "pass: segment 1's indices");
}

static void check_program(void)
{
    static const float pos[4][4] = {
        { 100, 50, 10, 2 }, { 100, 90, 10, 2 },      /* w 2 */
        { 0, 0, 0, 1 },     { 10, 0, 0, -1 },        /* an end behind the eye */
    };
    static const uint32_t lidx[4] = { 0, 1, 2, 3 };
    float out[8 * NV2A_WL_F4 * 4];
    uint32_t oidx[12], n, i;
    const float *p;

    memset(s_attr, 0, sizeof s_attr);
    memset(&s_v, 0, sizeof s_v);
    src0_v(s_v.prog[0], 0);
    mov_o(s_v.prog[0], 0);                        /* mov oPos, v0 */
    src0_v(s_v.prog[1], 3);
    mov_o(s_v.prog[1], 3);                        /* mov oD0, v3 */
    put(s_v.prog[1], 3, 0, 1, 1);                 /* final */
    for (i = 0; i < 4; i++) {
        memcpy(s_mem[1 + i], pos[i], 16);
        s_mem[20 + i][0] = 2.0f;                  /* clamped to 1 */
        s_mem[20 + i][1] = 0.25f;
    }
    arr(0, 1);
    arr(3, 20);
    n = expand(1, lidx, 4, 4, out, oidx);
    CHECK(n == 6, "program: %u indices, want 6 (the segment behind the eye left out)", n);
    p = corner(out, 0, 0);
    CHECK(NEAR(p[0], 99.5f) && NEAR(p[1], 50) && NEAR(p[2], 10) && NEAR(p[3], 0.5f),
          "program: corner 0 (%g, %g, %g, rhw %g)", p[0], p[1], p[2], p[3]);
    CHECK(NEAR(p[4], 1.0f) && NEAR(p[5], 0.25f), "program: oD0 (%g, %g)", p[4], p[5]);
    /* No specular: oD1 is (0, 0, 0, 1). */
    CHECK(NEAR(p[8], 0) && NEAR(p[11], 1), "program: oD1 (%g .. %g)", p[8], p[11]);
}

static void check_gate(void)
{
    memset(&s_v, 0, sizeof s_v);
    CHECK(nv2a_wide_lines(&s_v, NV2A_TOPO_LINES, 640, 1280), "gate: a 2x line list");
    CHECK(!nv2a_wide_lines(&s_v, NV2A_TOPO_LINES, 640, 640), "gate: a 1x line list");
    CHECK(!nv2a_wide_lines(&s_v, NV2A_TOPO_TRIANGLES, 640, 1280), "gate: triangles");
    s_v.fog_enable = 1;
    CHECK(!nv2a_wide_lines(&s_v, NV2A_TOPO_LINES, 640, 1280), "gate: fog on");
}

int main(void)
{
    check_pass();
    check_program();
    check_gate();
    if (!s_fail)
        printf("nv2a_wide_lines: ok\n");
    return s_fail ? 1 : 0;
}

/*
 * nv2a_blit2d: the 2D engine (nv2a_pb_exec.c, blit_2d).
 *
 *   classes   objects bound on subchannels 6 and 7 found by their class in
 *             instance memory (RAMHT), and a copy through them
 *   fallback  handles with no RAMHT entry on subchannels 3 and 2: XDK D3D's
 *             binding, surfaces-2D and image blit
 *   bytes     the rectangle lands at the destination point, rows at the
 *             destination pitch, nothing outside it touched; 16-bit too
 *   sync      the backend is asked to write the source back first, with the
 *             source offset, its pitch and the extent read
 *   overlap   a copy down inside one surface reads each row before writing
 *   refused   an operation other than SRCCOPY, an unknown format, a width
 *             past the pitch: nothing written
 *   gen       nv2a_pb_blit_generation moves on each copy, not on a refusal
 *
 * Exit 0 when all hold.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "recomp_env.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern ptrdiff_t xbox_GetMemoryOffset(void);

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int s_fails;
#define CHECK(c, ...) do { if (!(c)) { s_fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a backend that records sync_guest ---------------------------------- */
static uint32_t s_sync_n, s_sync_off, s_sync_pitch, s_sync_w, s_sync_h;
static void t_sync(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h)
{
    s_sync_n++;
    s_sync_off = offset; s_sync_pitch = pitch; s_sync_w = w; s_sync_h = h;
}
static const struct nv2a_pb_backend s_tb = { .sync_guest = t_sync };

static uint8_t *s_mem;

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

static void w32(uint32_t va, uint32_t v) { memcpy(s_mem + va, &v, 4); }

/* RAMHT at the start of PRAMIN (PFIFO_RAMHT 0: 4 KB at 0), one entry a
 * handle, its instance holding the class in the first word's low byte. */
static void bind_object(int slot, uint32_t handle, uint32_t cls)
{
    const uint32_t pramin = 0xFD700000u, inst = 0x1000u + 0x10u * (uint32_t)slot;
    w32(pramin + 8u * (uint32_t)slot, handle);
    w32(pramin + 8u * (uint32_t)slot + 4u, 0x80000000u | (inst >> 4));
    w32(pramin + inst, cls);
}

#define SRC 0x00200000u
#define DST 0x00300000u

static void surfaces(uint32_t sc, uint32_t fmt, uint32_t spitch, uint32_t dpitch,
                     uint32_t src, uint32_t dst)
{
    nv2a_pb_exec_method(sc, 0x0300, fmt);
    nv2a_pb_exec_method(sc, 0x0304, spitch | dpitch << 16);
    nv2a_pb_exec_method(sc, 0x0308, src);
    nv2a_pb_exec_method(sc, 0x030C, dst);
}

static void blit(uint32_t bc, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                 uint32_t w, uint32_t h)
{
    nv2a_pb_exec_method(bc, 0x0300, sx | sy << 16);
    nv2a_pb_exec_method(bc, 0x0304, dx | dy << 16);
    nv2a_pb_exec_method(bc, 0x0308, w | h << 16);
}

static void fill_src(uint32_t base, uint32_t pitch, uint32_t rows)
{
    uint32_t i;
    for (i = 0; i < pitch * rows; i++)
        s_mem[base + i] = (uint8_t)(i * 7u + 1u);
}

/* The w x h rectangle at (dx,dy) of DST equals (sx,sy) of SRC's pattern, and
 * the bytes around it in the row band are still `bg`. */
static int rect_ok(uint32_t bpp, uint32_t sp, uint32_t dp, uint32_t sx, uint32_t sy,
                   uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, uint8_t bg)
{
    uint32_t r, c;
    for (r = 0; r < h + 2; r++) {
        uint32_t row = dy + r - 1;
        for (c = 0; c < dp; c++) {
            uint8_t got = s_mem[DST + row * dp + c];
            int inside = r >= 1 && r <= h && c >= dx * bpp && c < (dx + w) * bpp;
            uint8_t want = inside
                ? s_mem[SRC + (sy + r - 1) * sp + sx * bpp + (c - dx * bpp)] : bg;
            if (got != want) {
                printf("  byte (%u,%u): got %02X want %02X\n", row, c, got, want);
                return 0;
            }
        }
    }
    return 1;
}

int main(void)
{
    static uint8_t xbe[0x1000];
    uint32_t g0, g1;

    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        printf("nv2a_blit2d: no memory layout\n");
        return 2;
    }
    s_mem = (uint8_t *)xbox_GetMemoryOffset();
    recomp_env_set(RENV_PB_EXEC, "1");
    nv2a_pb_set_backend(&s_tb);
    nv2a_pb_exec_method(0, 0x0100, 0);   /* the executor's first-call setup */
    w32(0xFD002210u, 0);                 /* PFIFO_RAMHT: 4 KB at PRAMIN 0 */

    /* classes: surfaces-2D on 6, image blit on 7, read from the objects. */
    bind_object(0, 0x11110062u, 0x62);
    bind_object(1, 0x2222009Fu, 0x9F);
    nv2a_pb_exec_method(6, 0x0000, 0x11110062u);
    nv2a_pb_exec_method(7, 0x0000, 0x2222009Fu);
    fill_src(SRC, 64, 32);
    memset(s_mem + DST, 0xEE, 64 * 32);
    surfaces(6, 0x0A, 64, 64, SRC, DST);
    g0 = nv2a_pb_blit_generation();
    s_sync_n = 0;
    blit(7, 2, 3, 3, 7, 8, 4);
    g1 = g0;
    CHECK(rect_ok(4, 64, 64, 2, 3, 3, 7, 8, 4, 0xEE), "classes/bytes: 8x4 at 32 bits");
    CHECK(nv2a_pb_blit_generation() == g1 + 1, "gen: did not move on a copy");
    CHECK(s_sync_n == 1 && s_sync_off == SRC && s_sync_pitch == 64 && s_sync_w == 10
          && s_sync_h == 7, "sync: n %u off %08X pitch %u extent %ux%u",
          s_sync_n, s_sync_off, s_sync_pitch, s_sync_w, s_sync_h);

    /* fallback: unlookable handles on 3 and 2; 16-bit, pitches differ. */
    nv2a_pb_exec_method(3, 0x0000, 0x0BADBAD3u);
    nv2a_pb_exec_method(2, 0x0000, 0x0BADBAD2u);
    memset(s_mem + DST, 0x55, 128 * 32);
    surfaces(3, 0x04, 64, 128, SRC, DST);
    blit(2, 1, 0, 10, 4, 6, 5);
    CHECK(rect_ok(2, 64, 128, 1, 0, 10, 4, 6, 5, 0x55), "fallback/bytes: 6x5 at 16 bits");

    /* overlap: within one surface, 4 rows down. */
    {
        uint8_t ref[64 * 16];
        uint32_t r;
        fill_src(SRC, 64, 16);
        memcpy(ref, s_mem + SRC, sizeof ref);
        surfaces(3, 0x0A, 64, 64, SRC, SRC);
        blit(2, 0, 0, 0, 4, 16, 8);
        for (r = 0; r < 8; r++)
            CHECK(!memcmp(s_mem + SRC + (r + 4) * 64, ref + r * 64, 64), "overlap: row %u", r + 4);
    }

    /* refused: an operation, a format, a width past the pitch. */
    memset(s_mem + DST, 0x77, 64 * 32);
    fill_src(SRC, 64, 32);
    surfaces(3, 0x0A, 64, 64, SRC, DST);
    g0 = nv2a_pb_blit_generation();
    nv2a_pb_exec_method(2, 0x02FC, 1);   /* not SRCCOPY */
    blit(2, 0, 0, 0, 0, 4, 4);
    nv2a_pb_exec_method(2, 0x02FC, 3);
    surfaces(3, 0x7F, 64, 64, SRC, DST); /* no such format */
    blit(2, 0, 0, 0, 0, 4, 4);
    surfaces(3, 0x0A, 64, 64, SRC, DST);
    blit(2, 0, 0, 0, 0, 17, 4);          /* 17*4 > 64 */
    {
        uint32_t i, untouched = 1;
        for (i = 0; i < 64 * 32; i++)
            untouched &= s_mem[DST + i] == 0x77;
        CHECK(untouched, "refused: something was written");
    }
    CHECK(nv2a_pb_blit_generation() == g0, "gen: moved on a refusal");

    printf("nv2a_blit2d: %s\n", s_fails ? "FAIL" : "PASS");
    return s_fails ? 1 : 0;
}

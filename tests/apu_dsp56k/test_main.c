/* apu_dsp56k: the small DSP56300 interpreter (apu_dsp56k.h, BO-119).
 *
 *   - arithmetic as the Family Manual has it: the fractional multiply,
 *     convergent rounding, limiting on a read of an accumulator, 56-bit
 *     shifts, short immediates (fractions into data registers, integers
 *     into the others)
 *   - flow: DOR loops (nested, a count from a register), long and short
 *     BSR / BRA, RTS, compare and branch, an operation executed on a
 *     condition (IFcc)
 *   - the DMA list: circular buffers in the memory behind it, the position
 *     written back, both directions, the handshake the programs wait on
 *   - an instruction it does not have stops the run and is named
 *   - every native routine against the same code interpreted, on random
 *     memory: X memory and all registers equal afterwards
 *
 * The programs are assembled here by hand; no title's code.
 */
#include "apu_dsp56k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static dsp56k d, e;
static uint32_t mem[0x4000];
static uint32_t mrd(void *c, uint32_t off) { (void)c; return off / 4 < 0x4000 ? mem[off / 4] : 0; }
static void mwr(void *c, uint32_t off, const uint32_t *w, int n)
{
    (void)c;
    for (int i = 0; i < n; i++) if (off / 4 + (uint32_t)i < 0x4000) mem[off / 4 + i] = w[i];
}

static int run(dsp56k *m, const uint32_t *p, unsigned n)
{
    m->p = p; m->pn = n; m->mem_read = mrd; m->mem_write = mwr;
    return dsp56k_run(m, 0, n - 1, 100000);
}

static uint32_t rng = 7;
static int32_t rnd24(void) { rng = rng * 1664525u + 1013904223u; return (int32_t)rng >> 8; }

#define NOP 0x000000u
#define RTS 0x00000Cu

static void test_arith(void)
{
    /* X0 = 0.5, Y0 = -0.25; A = X0 * Y0 (MPY Y0,X0,A); X:0x100 = A.
     * Then B = 0x7F (a fraction: 0x7F0000), B += B (ADD B,B is not a form:
     * use ASL B), X:0x101 = B limited. */
    static const uint32_t p1[] = {
        0x44F400, 0x400000,         /* MOVE #$400000,X0 */
        0x46F400, 0xE00000,         /* MOVE #$E00000,Y0 */
        0x2000D0,                   /* MPY Y0,X0,A */
        0x567000, 0x000100,         /* MOVE A,X:$100 */
        0x2F7F00,                   /* MOVE #$7F,B */
        0x20003A,                   /* ASL B */
        0x577000, 0x000101,         /* MOVE B,X:$101 */
        0x380500,                   /* MOVE #$05,N0 */
        0x2C1000,                   /* MOVE #$10,A1 */
        0x200013,                   /* CLR A */
        0x0140C0, 0x000003,         /* ADD #$000003,A */
        0x014485,                   /* CMP #$04,A */
        0x202913,                   /* CLR A IFLT: 3 < 4, taken */
        0x567000, 0x000102,         /* MOVE A,X:$102 */
        0x0140C0, 0x000009,         /* ADD #$000009,A */
        0x014485,                   /* CMP #$04,A */
        0x202913,                   /* CLR A IFLT: 9 >= 4, not taken */
        0x567000, 0x000103,         /* MOVE A,X:$103 */
        0x0C1D06,                   /* ASL #3,A,A */
        0x567000, 0x000104,         /* MOVE A,X:$104 */
        NOP,
    };
    memset(&d, 0, sizeof d);
    CHECK(run(&d, p1, sizeof p1 / 4) == 0, "p1 stopped at %X (%06X)", d.fault_pc, d.fault_word);
    CHECK(d.x[0x100] == -0x100000, "0.5 * -0.25: %06X", d.x[0x100] & 0xFFFFFF);
    CHECK(d.x[0x101] == 0x7FFFFF, "0.992 * 2 read from B is not limited: %06X", d.x[0x101] & 0xFFFFFF);
    CHECK(d.n[0] == 5, "a short immediate into N0: %X", d.n[0]);
    CHECK(d.x[0x102] == 0, "CLR A IFLT with LT true left %X", d.x[0x102]);
    CHECK(d.x[0x103] == 9, "CLR A IFLT with LT false cleared A (%X)", d.x[0x103]);
    CHECK(d.x[0x104] == 72, "ASL #3: %d", d.x[0x104]);

    /* Convergent rounding in MACR: 0.5 LSB ties go to even. X0 = 1 LSB,
     * Y0 = 0.25 -> product 0.25 LSB * ... build ties with A0 directly. */
    {
        static const uint32_t p2[] = {
            0x200013,               /* CLR A */
            0x54F400, 0x000001,     /* MOVE #$000001,A1 */
            0x50F400, 0x800000,     /* MOVE #$800000,A0   -> 1.5 LSB */
            0x200011,               /* RND A */
            0x567000, 0x000100,     /* MOVE A,X:$100 */
            0x200013,
            0x54F400, 0x000002,     /* 2.5 LSB */
            0x50F400, 0x800000,
            0x200011,
            0x567000, 0x000101,
            NOP,
        };
        memset(&d, 0, sizeof d);
        CHECK(run(&d, p2, sizeof p2 / 4) == 0, "p2 stopped at %X (%06X)", d.fault_pc, d.fault_word);
        CHECK(d.x[0x100] == 2 && d.x[0x101] == 2, "ties: 1.5 -> %d, 2.5 -> %d (want 2, 2)", d.x[0x100], d.x[0x101]);
    }
}

static void test_flow(void)
{
    /* Sum 1..5 with a DOR loop whose count comes from a register, call a
     * routine with a long and a short BSR, skip with BRA. */
    static const uint32_t p[] = {
        /* 00 */ 0x46F400, 0x000005,    /* MOVE #5,Y0 */
        /* 02 */ 0x200013,              /* CLR A */
        /* 03 */ 0x20001B,              /* CLR B */
        /* 04 */ 0x06C610, 0x000003,    /* DOR Y0,$07 */
        /* 06 */ 0x014188,              /* ADD #1,B */
        /* 07 */ 0x200010,              /* ADD B,A */
        /* 08 */ 0x567000, 0x000100,    /* MOVE A,X:$100 */
        /* 0A */ 0x0D1080, 0x00000A,    /* BSR $14 */
        /* 0C */ 0x050943,              /* BSR short $14: disp +8: 0000 0101 0000 1001 0100 1000? see below */
        /* 0D */ 0x0D10C0, 0x000003,    /* BRA $10 */
        /* 0F */ 0x200013,              /* CLR A (skipped) */
        /* 10 */ 0x567000, 0x000101,    /* MOVE A,X:$101 */
        /* 12 */ 0x0D10C0, 0x000005,    /* BRA $17 */
        /* 14 */ 0x014180,              /* ADD #1,A */
        /* 15 */ RTS,
        /* 16 */ NOP,
        /* 17 */ NOP,
    };
    static uint32_t q[sizeof p / 4];
    memcpy(q, p, sizeof p);
    /* BSR short at $0C to $14: displacement 8 = 0 0000 1000: bits 9..6 =
     * 0000, bits 4..0 = 01000. */
    q[0x0C] = 0x050800 | 0x08;
    memset(&d, 0, sizeof d);
    CHECK(run(&d, q, sizeof q / 4) == 0, "flow stopped at %X (%06X)", d.fault_pc, d.fault_word);
    CHECK(d.x[0x100] == 15, "sum 1..5 in a DOR loop: %d", d.x[0x100]);
    CHECK(d.x[0x101] == 17, "two calls and a skipped CLR: %d", d.x[0x101]);

    /* Nested loops: 3 x 4 increments. */
    {
        static const uint32_t n[] = {
            0x200013,
            0x060390, 0x000004,     /* DOR #3,$05 */
            0x060490, 0x000002,     /* DOR #4,$05?? inner ends at $05 */
            0x014180,               /* ADD #1,A */
            0x000000,               /* NOP: the outer loop's last word */
            0x567000, 0x000100,
            NOP,
        };
        static uint32_t m[sizeof n / 4];
        memcpy(m, n, sizeof n);
        m[2] = 0x000005;            /* outer: last word at $06 */
        m[4] = 0x000002;            /* inner: last word at $05 */
        memset(&d, 0, sizeof d);
        CHECK(run(&d, m, sizeof m / 4) == 0, "nested stopped at %X (%06X)", d.fault_pc, d.fault_word);
        CHECK(d.x[0x100] == 12, "3 x 4 nested: %d", d.x[0x100]);
    }

    /* Something it does not have: stops there. */
    {
        static const uint32_t u[] = { 0x200013, 0x0000B9, NOP };
        memset(&d, 0, sizeof d);
        CHECK(run(&d, u, 3) != 0 && d.fault_pc == 1 && d.fault_word == 0x0000B9,
              "an unknown instruction did not stop the run (%X %06X)", d.fault_pc, d.fault_word);
    }
}

static void test_dma(void)
{
    /* Two blocks at X:0x200 and X:0x207: 4 words from X:0x300 into a
     * circular buffer of 0x18 bytes at memory 0x1000, position 0x10 (so it
     * wraps after two words), then the same four read back to X:0x310
     * from position 0x10. The handshake as the effects do it. */
    static const uint32_t p[] = {
        0x08F496, 0x000002,         /* MOVEP #2,X:<<$FFFFD6 */
        0x0CD624, 0x000000,         /* BRSET #4,X:<<$FFFFD6,* */
        0x08F496, 0x000003,
        0x0CD603, 0x000000,         /* BRCLR #3,... */
        0x56F400, 0x000200,         /* MOVE #$200,A */
        0x08CE14,                   /* MOVEP A,X:<<$FFFFD4 */
        0x08F496, 0x000004,
        0x0CD623, 0x000000,
        0x08F496, 0x000001,
        0x0CD604, 0x000000,
        0x0CC507, 0x000000,         /* BRCLR #7,X:<<$FFFFC5,* */
        0x08F485, 0x000080,         /* MOVEP #$80,X:<<$FFFFC5 */
        NOP,
    };
    memset(&d, 0, sizeof d);
    memset(mem, 0, sizeof mem);
    d.x[0x200] = 0x207; d.x[0x201] = 0x4BD2; d.x[0x202] = 4; d.x[0x203] = 0x300;
    d.x[0x204] = 0x10; d.x[0x205] = 0x1000; d.x[0x206] = 0x17;
    d.x[0x207] = 0x4000; d.x[0x208] = 0x4BD0; d.x[0x209] = 4; d.x[0x20A] = 0x310;
    d.x[0x20B] = 0x10; d.x[0x20C] = 0x1000; d.x[0x20D] = 0x17;
    for (int i = 0; i < 4; i++) d.x[0x300 + i] = 0x111111 * (i + 1) - (i == 3 ? 0x800000 : 0);
    CHECK(run(&d, p, sizeof p / 4) == 0, "dma stopped at %X (%06X)", d.fault_pc, d.fault_word);
    CHECK(mem[0x1010 / 4] == 0x111111 && mem[0x1014 / 4] == 0x222222 && mem[0x1000 / 4] == 0x333333 &&
          mem[0x1004 / 4] == (0x444444u - 0x800000u & 0xFFFFFF),
          "circular write: %06X %06X | %06X %06X", mem[0x1010 / 4], mem[0x1014 / 4], mem[0x1000 / 4], mem[0x1004 / 4]);
    CHECK(d.x[0x204] == 0x08 && d.x[0x20B] == 0x08, "positions after: %X %X (want 8)", d.x[0x204], d.x[0x20B]);
    {
        int same = 1;
        for (int i = 0; i < 4; i++) same &= d.x[0x310 + i] == d.x[0x300 + i];
        CHECK(same, "read back differs (sign-extended 24-bit words)");
    }
    CHECK(!(d.irq & 0x80), "the end-of-list flag was not cleared by the program");
}

/* A native routine and the same words interpreted, from the same random
 * state. call: 1 = entered with BSR (the words end in RTS), 0 = inline. */
static void same_both_ways(const char *name, const uint32_t *w, unsigned n, int call)
{
    static uint32_t p[64];
    unsigned k = 0;
    if (call) { p[k++] = 0x0D1080; p[k++] = 0x000003; p[k++] = NOP; }   /* BSR $3; end at $2 */
    memcpy(p + k, w, n * 4); k += n;
    if (!call) p[k++] = NOP;
    for (int trial = 0; trial < 8; trial++) {
        memset(&d, 0, sizeof d);
        for (int i = 0x200; i < 0x1000; i++) d.x[i] = rnd24();
        if (trial == 0)             /* full scale everywhere: limiting and rounding at the edges */
            for (int i = 0x200; i < 0x1000; i++) d.x[i] = (i & 1) ? 0x7FFFFF : -0x800000;
        d.x0 = rnd24(); d.x1 = rnd24(); d.y0 = rnd24(); d.y1 = rnd24();
        d.a = (int64_t)rnd24() << 20; d.b = (int64_t)rnd24() << 22;
        d.r[0] = 0x300; d.r[1] = 0x400; d.r[2] = 0x500; d.r[3] = 0x600; d.r[4] = 0x700; d.r[5] = 0x800;
        d.x[0x824] = 0x900;         /* the matrix stretch finds its input here */
        for (int i = 0; i < 8; i++) d.m[i] = 0xFFFFFF;
        e = d;
        e.no_accel = 1;
        d.p = e.p = p; d.pn = e.pn = k;
        {
            int ra = dsp56k_run(&d, 0, call ? 2 : k - 1, 100000);
            int rb = dsp56k_run(&e, 0, call ? 2 : k - 1, 100000);
            CHECK(ra == 0 && rb == 0, "%s: stopped (%d at %X, %d at %X)", name, ra, d.fault_pc, rb, e.fault_pc);
            CHECK(d.steps < e.steps, "%s: the native version was not used (%lu steps, %lu)", name, d.steps, e.steps);
            CHECK(!memcmp(d.x, e.x, sizeof d.x), "%s, trial %d: X memory differs", name, trial);
            CHECK(d.a == e.a && d.b == e.b && d.x0 == e.x0 && d.x1 == e.x1 && d.y0 == e.y0 && d.y1 == e.y1,
                  "%s, trial %d: data registers differ", name, trial);
            CHECK(!memcmp(d.r, e.r, sizeof d.r) && !memcmp(d.n, e.n, sizeof d.n),
                  "%s, trial %d: address registers differ (R0 %X/%X R1 %X/%X R2 %X/%X R3 %X/%X R4 %X/%X)", name, trial,
                  d.r[0], e.r[0], d.r[1], e.r[1], d.r[2], e.r[2], d.r[3], e.r[3], d.r[4], e.r[4]);
        }
    }
}

static void test_native(void)
{
    static const uint32_t copy[] = { 0x061090, 0x000003, 0x44D800, 0x445900, RTS };
    static const uint32_t scale[] = { 0x061090, 0x000004, 0x45E000, 0x2000A0, 0x565800, RTS };
    static const uint32_t macr_into[] = { 0x061090, 0x000005, 0x45D800, 0x56E100, 0x2000A3, 0x565900, RTS };
    static const uint32_t add_into[] = { 0x061090, 0x000005, 0x44D800, 0x56E100, 0x200040, 0x565900, RTS };
    static const uint32_t add3[] = { 0x061090, 0x000005, 0x44D800, 0x56D900, 0x200040, 0x565A00, RTS };
    static const uint32_t onepole[] = { 0x0201D6, 0x020997, 0x061090, 0x000007, 0x020194, 0x2000D0, 0x45E000,
                                        0x2000F2, 0x02018E, 0x565800, RTS };
    static const uint32_t taps1[] = { 0x391000, 0x3A1000, 0x44D800, 0x061090, 0x00000F, 0x45E900, 0x44D000,
                                      0x2000A0, 0x45D900, 0x44D800, 0x2000A3, 0x45EA00, 0x2000E3, 0x45DA00,
                                      0x2000E3, 0x45DB00, 0x2000F3, 0x0C1D0E, 0x565C00, RTS };
    static const uint32_t taps2[] = { 0x3C1000, 0x3A1000, 0x391000, 0x44D800, 0x20EE00, 0x200022, 0x21C700,
                                      0x061090, 0x000012, 0x45E900, 0x44D000, 0x2000A8, 0x45D900, 0x44D800,
                                      0x2000A0, 0x45EA00, 0x2000EB, 0x45DA00, 0x2000E3, 0x45DB00, 0x2000FB,
                                      0x2000F3, 0x0C1D8F, 0x0C1D0E, 0x576C00, 0x565C00, RTS };
    static const uint32_t matrix[] = { 0x381000, 0x060490, 0x000010, 0x0A75D1, 0x000024, 0x061090, 0x00000B,
                                       0x223000, 0x227200, 0x200013, 0x060490, 0x000004, 0x44C800, 0x45DA00,
                                       0x2000A3, 0x565C00, 0x44D900, 0x44D300 };
    same_both_ways("copy", copy, sizeof copy / 4, 1);
    same_both_ways("scale", scale, sizeof scale / 4, 1);
    same_both_ways("macr into", macr_into, sizeof macr_into / 4, 1);
    same_both_ways("add into", add_into, sizeof add_into / 4, 1);
    same_both_ways("add to a third", add3, sizeof add3 / 4, 1);
    same_both_ways("one-pole", onepole, sizeof onepole / 4, 1);
    same_both_ways("taps, one output", taps1, sizeof taps1 / 4, 1);
    same_both_ways("taps, two outputs", taps2, sizeof taps2 / 4, 1);
    same_both_ways("mixing matrix", matrix, sizeof matrix / 4, 0);
}

int main(void)
{
    test_arith();
    test_flow();
    test_dma();
    test_native();
    if (failures) {
        printf("apu_dsp56k: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("apu_dsp56k: arithmetic, flow, DMA and the native routines ok (%d checks)\n", checks);
    return 0;
}

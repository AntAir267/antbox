/*
 * apu_dsp56k.h -- a small DSP56300 interpreter, for running one effect of a
 * title's GP effects image as the title wrote it (apu_gp.h).
 *
 * Not a DSP56300: the instructions, addressing modes and arithmetic that
 * the effects programs of DirectSound's images use, from Motorola's
 * DSP56300 Family Manual, and the GP's DMA engine as those programs drive
 * it. X memory only, linear addressing, no interrupts, no scaling modes.
 * An instruction outside that set stops the run (dsp56k_run returns
 * nonzero and says which word), so a program is either run exactly or not
 * at all.
 *
 * Arithmetic is the DSP's: 56-bit accumulators (8 bits of extension over a
 * 48-bit fraction), a multiply is (a * b) << 1, MACR / MPYR round
 * convergently, and reading an accumulator into a 24-bit destination
 * limits it to full scale when the extension is in use.
 */
#ifndef APU_DSP56K_H
#define APU_DSP56K_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP56K_X_WORDS 0x2000           /* X:0 .. X:0x1FFF */

typedef struct dsp56k {
    int32_t x[DSP56K_X_WORDS];          /* X memory, 24-bit words sign-extended */
    const uint32_t *p;                  /* the program, P-relative word 0 first */
    uint32_t pn;                        /* its length in words */

    int64_t a, b;                       /* 56 bits, sign-extended */
    int32_t x0, x1, y0, y1;
    uint32_t r[8], n[8], m[8];
    int lt, eq;                         /* of the last CMP */

    /* The GP's DMA engine: the list at X:FFFFD4 runs when the program
     * starts it. A block is seven X words: next block (0x4000: the last),
     * control (bit 1: DSP to memory), word count, X address, position in
     * the circular buffer, the buffer's base and its size - 1, the last
     * three in bytes of GP scratch memory. */
    uint32_t dma_next, dma_ctl, irq;
    uint32_t (*mem_read)(void *ctx, uint32_t byte_off);
    void (*mem_write)(void *ctx, uint32_t byte_off, const uint32_t *words, int n);
    void *ctx;

    /* Routines of the program that have a native version here (the
     * library loops DirectSound's effects share), found by their exact
     * code words: 0 none, else which, by the routine's first word. */
    const uint32_t *accel_p;
    uint8_t accel[2048];
    int no_accel;                       /* 1: interpret everything (tests) */

    unsigned long steps;                /* instructions run, in all */
    uint32_t fault_pc, fault_word;      /* where a run stopped */
} dsp56k;

/* Runs the program from word pc until it reaches word end (an effect's
 * code falls off its end into the next effect's). Returns 0, or nonzero
 * when it met an instruction it does not have, left X memory, or ran
 * longer than max_steps (fault_pc / fault_word say where). */
int dsp56k_run(dsp56k *d, uint32_t pc, uint32_t end, unsigned long max_steps);

#ifdef __cplusplus
}
#endif

#endif /* APU_DSP56K_H */

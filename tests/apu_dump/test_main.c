/*
 * The APU state dump in a hang report (BO-351).
 *
 * mcpx_apu_dump_state formats the APU's own view of itself: the front end, the
 * interrupt line, frames rendered, trap/delivery counters, and every voice
 * still in a list. It runs as a crash note, so it must not lock -- this test
 * builds a fake state and checks the text it produces, which is the one thing
 * a hang report adds that the rest of the runtime never exercises.
 */
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

/* Captured output of the dump, one buffer. */
static char g_buf[8192];
static size_t g_len;

static void capture(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_len += (size_t)vsnprintf(g_buf + g_len, sizeof(g_buf) - g_len, fmt, ap);
    va_end(ap);
}

static int failures;
static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* The voice array's base, in the fake guest RAM. */
#define VOICES 0x10000u

static void put32(uint8_t *ram, uint32_t off, uint32_t v)
{
    memcpy(ram + off, &v, 4);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    uint8_t *ram = (uint8_t *)calloc(1, 1u << 20);
    if (!d || !ram) return 2;
    qemu_mutex_init(&d->lock);
    g_apu_ram_ptr = ram;   /* the APU reads guest RAM through this */

    /* A trapped front end, an interrupt pending, and counters that say work
     * happened up to the freeze. */
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_TRAPPED
                           | NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
    d->regs[NV_PAPU_SECTL] = 0x3;    /* XCNTMODE on */
    d->regs[NV_PAPU_ISTS]  = NV_PAPU_ISTS_GINTSTS | NV_PAPU_ISTS_FETINTSTS;
    d->regs[NV_PAPU_IEN]   = 0xD9;
    d->regs[NV_PAPU_FECV]  = 5;
    d->regs[NV_PAPU_FEAV]  = 3;
    d->set_irq = true;
    d->frame_count = 123;
    d->ep_frame_div = 7;
    apu_irq_stats.traps = 10;
    apu_irq_stats.delivered = 8;
    apu_irq_stats.posted = 2;
    apu_irq_stats.holder_delivered = 1;
    apu_irq_stats.held_frames = 0;
    apu_irq_stats.forced = 0;
    apu_irq_stats.se_frames = 456;
    apu_irq_stats.fe_trapped_frames = 1;

    /* One voice in the 2D list: active, sustained, part-way through. The other
     * two lists are empty (top 0xFFFF). */
    d->regs[NV_PAPU_VPVADDR] = VOICES;
    d->regs[NV_PAPU_TVL2D] = 0;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    put32(ram, VOICES + NV_PAVS_VOICE_PAR_STATE,
          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE
          | (5u << 28) /* EACUR sustain */ | (5u << 24) /* EFCUR sustain */);
    put32(ram, VOICES + NV_PAVS_VOICE_PAR_OFFSET, 0x100u);   /* CBO */
    put32(ram, VOICES + NV_PAVS_VOICE_PAR_NEXT, 0x200u);     /* EBO */
    put32(ram, VOICES + NV_PAVS_VOICE_TAR_PITCH_LINK, 0xFFFFu); /* list ends */
    d->vp.voice_locked[0] = 1;   /* voice 0 is VOICE_LOCK'd */

    g_len = 0;
    mcpx_apu_dump_state(d, capture);
    g_buf[g_len] = '\0';

    check(strstr(g_buf, "front end") != NULL, "names the front end");
    check(strstr(g_buf, "trapped") != NULL, "says the front end is trapped");
    check(strstr(g_buf, "irq") != NULL, "names the interrupt line");
    check(strstr(g_buf, "line=up") != NULL, "says the line is up");
    check(strstr(g_buf, "frames") != NULL, "names the frame counters");
    check(strstr(g_buf, "traps/deliveries") != NULL, "names traps/deliveries");
    check(strstr(g_buf, "lock: free") != NULL, "says the APU lock is free");
    check(strstr(g_buf, "voice list 2D") != NULL, "names the 2D voice list");
    check(strstr(g_buf, "voice 0") != NULL, "names voice 0");
    check(strstr(g_buf, "active=1") != NULL, "voice 0 is active");
    check(strstr(g_buf, "cbo=256 ebo=512") != NULL, "voice 0 buffer position");
    check(strstr(g_buf, "locked voices: 0") != NULL, "names the locked voice");

    /* A corrupt list link (a handle past the 256 voices) must stop the walk
     * rather than read outside the voice array. */
    put32(ram, VOICES + NV_PAVS_VOICE_TAR_PITCH_LINK, 0x1234u);
    g_len = 0;
    mcpx_apu_dump_state(d, capture);
    g_buf[g_len] = '\0';
    check(strstr(g_buf, "bad voice handle 4660") != NULL,
          "stops on a corrupt list handle");

    /* A VPVADDR outside guest RAM must not fault the dump: voice_reg returns
     * 0xDEADBEEF instead of reading. */
    d->regs[NV_PAPU_VPVADDR] = 0xFFFFFF00u;
    g_len = 0;
    mcpx_apu_dump_state(d, capture);
    g_buf[g_len] = '\0';
    check(strstr(g_buf, "voice 0") != NULL,
          "a bad VPVADDR reads 0xDEADBEEF, does not fault");

    printf("---- dump ----\n%s", g_buf);

    free(ram);
    free(d);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

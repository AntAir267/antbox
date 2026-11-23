/* The APU raises its interrupt the way the title's driver expects.
 *
 * The driver arms FETFORCE1 so that an idle voice traps the front end, and
 * resumes it from its ISR. This checks the device half of that: the trap
 * records the method and latches FETINTSTS, which asserts GINTSTS when IEN
 * allows; the ISR's ack clears it even though the front end is still trapped
 * (it acks before it resumes, and a re-latched bit would replay the stale
 * method on the next delivery); nothing after the resume brings it back; a
 * masked trap keeps the line low and an unarmed one does not trap.
 *
 * A trap latches FETINTSTS in ISTS and sets set_irq; the frame thread turns
 * that into the line. An ISTS write runs the same update, so a write of 0
 * (acks nothing) stands in for the frame thread here. */
#include "apu_state.h"
#include "apu.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

#define VP(off) (0x20000u + (off))

static uint32_t reg(MCPXAPUState *d, uint32_t r) { return d->regs[r]; }
static int line(MCPXAPUState *d) { return !!(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_GINTSTS); }
static void settle(MCPXAPUState *d) { mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0, 4); }

/* 6. BO-351: the ISR's ack against a new trap on the frame thread.
 *
 * The ack's update_irq has found nothing pending and is about to lower the
 * line (mcpx_apu_irq_lower_hook). Right there the frame thread takes a trap
 * and runs its own update: a new idle voice traps the front end and the line
 * goes up. Without d->lock around the ack that all happens inside the ack's
 * window, the ack then lowers the line over the new trap, and nothing raises
 * it again: FETINTSTS set, GINTSTS clear, set_irq spent (the int044 Dee1
 * dump: ISTS=00000010 IEN=000000D9 line=down set_irq=0, front end trapped).
 * With it, the frame thread waits for the ack and raises the line after it.
 * The hook gives the frame thread 300 ms; it only ever finishes inside the
 * window when nothing keeps it out. */
static volatile LONG s_frame_done;
static QemuThread s_frame;

static void *frame_thread(void *arg)
{
    MCPXAPUState *d = (MCPXAPUState *)arg;
    qemu_mutex_lock(&d->lock);
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 73, 4);   /* the trap */
    mcpx_apu_frame_irq(d);                                 /* the line */
    qemu_mutex_unlock(&d->lock);
    InterlockedExchange(&s_frame_done, 1);
    return NULL;
}

static void lower_hook(MCPXAPUState *d)
{
    int i;
    mcpx_apu_irq_lower_hook = NULL;   /* once */
    qemu_thread_create(&s_frame, "frame", frame_thread, d, 0);
    for (i = 0; i < 300 && !InterlockedCompareExchange(&s_frame_done, 0, 0); i++)
        Sleep(1);
}

static void race_ack_vs_trap(MCPXAPUState *d)
{
    /* armed, resumed, nothing pending */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FETFORCE1, NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    d->set_irq = false;   /* the earlier steps' traps, as the frame thread spends it */
    CHECK("race: quiet before", reg(d, NV_PAPU_ISTS) == 0 && !d->set_irq);

    s_frame_done = 0;
    mcpx_apu_irq_lower_hook = lower_hook;
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, NV_PAPU_ISTS_FEVINTSTS, 4);   /* the ack */
    qemu_thread_join(&s_frame);
    mcpx_apu_irq_lower_hook = NULL;

    CHECK("race: the new trap is pending",
          (reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS)
              && (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FEMETHMODE)
                     == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    CHECK("race: the line is up for it", line(d));
}

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) return 2;
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    printf("apu_irq: running\n");

    /* The title's own sequence: IEN 0xD9, FETFORCE1 = SE2FE_IDLE_VOICE. */
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FETFORCE1,
                        NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE, 4);
    settle(d);
    CHECK("quiet before any trap", !line(d));

    /* 1. An idle voice traps the front end and raises the line. */
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 66, 4);
    CHECK("FECTL trapped",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    CHECK("trap reason requested",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FETRAPREASON)
              == NV_PAPU_FECTL_FETRAPREASON_REQUESTED);
    CHECK("FEDECMETH/PARAM name the voice",
          reg(d, NV_PAPU_FEDECMETH) == SE2FE_IDLE_VOICE
              && reg(d, NV_PAPU_FEDECPARAM) == 66);
    CHECK("trap asks for the line", d->set_irq);
    settle(d);
    CHECK("FETINTSTS latched", reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS);
    CHECK("line asserted", line(d));

    /* 2. The ISR acks before it resumes the front end. The trap was one
     *    event: the ack clears it although FECTL is still trapped. */
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    CHECK("ack while trapped drops the line", !line(d));
    CHECK("ack while trapped clears FETINTSTS",
          !(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS));

    /* 3. Resumed (FECTL free running, trap reason left at REQUESTED, as the
     *    driver leaves it): a later status update must not bring the old
     *    trap back, or the ISR handles FEDECPARAM's voice a second time. */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING
                            | NV_PAPU_FECTL_FETRAPREASON_REQUESTED, 4);
    settle(d);
    CHECK("resume: no stale FETINTSTS",
          !(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS));
    CHECK("resume: line low", !line(d));
    CHECK("ISTS clear", reg(d, NV_PAPU_ISTS) == 0);

    /* 3b. The driver halting the front end itself is not a trap. */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_HALTED
                            | NV_PAPU_FECTL_FETRAPREASON_REQUESTED, 4);
    settle(d);
    CHECK("halted: no FETINTSTS",
          !(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS));
    CHECK("halted: line low", !line(d));
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);

    /* 4. Masked: the trap still latches its status but the line stays low. */
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0x00, 4);
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 67, 4);
    settle(d);
    CHECK("masked: status latched", reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS);
    CHECK("masked: line low", !line(d));

    /* 5. Not armed: an idle voice is just a method, no trap. */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FETFORCE1, 0, 4);
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 68, 4);
    settle(d);
    CHECK("unarmed: no trap",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING);
    CHECK("unarmed: line low", !line(d));

    race_ack_vs_trap(d);

    if (failures) { printf("apu_irq: %d failure(s)\n", failures); return 1; }
    printf("apu_irq: all passed\n");
    return 0;
}

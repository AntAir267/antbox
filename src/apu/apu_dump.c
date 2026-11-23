/*
 * apu_dump.c - APU state in a hang report (BO-351).
 *
 * The int042 soak froze in the title's "stop a sound and wait until it
 * stopped" loop, polling a DirectSound voice that never went idle. The APU is
 * the only thing that can make that voice idle, so the hang report now gets
 * the APU's view of itself: the front end, the interrupt line, how many frames
 * ran, the trap/delivery counters, and every voice still in a list with its
 * envelope and buffer position.
 *
 * This runs as a crash-report note (xbox_crash_add_note), on the watchdog
 * thread while the rest of the process is stuck. It must not lock: every read
 * is a plain or atomic load, and a voice struct is read from guest RAM the way
 * the VP reads it, one dword at a time.
 */
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

/* The three voice lists' top/current/next registers, and a name for each. */
static const struct {
    uint32_t top, current, next;
    const char *name;
} voice_lists[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D, "2D" },
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D, "3D" },
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP, "MP" },
};

extern size_t g_xbox_total_ram;
extern size_t g_xbox_map_size;

/* The end of guest RAM, the same bound apu_core.c's apu_mapped_ram uses. */
static uint32_t apu_ram_end(void)
{
    return (uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram);
}

/* One raw dword of a voice's struct in guest RAM. The voice array lives at
 * NV_PAPU_VPVADDR, NV_PAVS_SIZE bytes per voice, read exactly as the VP does.
 * The dump runs while the game is broken, so a bad VPVADDR (or a handle past
 * the voice array) must not turn into a read outside guest RAM: return
 * 0xDEADBEEF instead. */
static uint32_t voice_reg(MCPXAPUState *d, uint16_t v, uint32_t off)
{
    uint64_t va = (uint64_t)d->regs[NV_PAPU_VPVADDR]
                + (uint64_t)v * NV_PAVS_SIZE + off;
    if (va + 4 > apu_ram_end())
        return 0xDEADBEEFu;
    return ldl_le_phys(address_space_memory, va);
}

static const char *fe_mode(uint32_t fectl)
{
    switch (fectl & NV_PAPU_FECTL_FEMETHMODE) {
    case NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING: return "run";
    case NV_PAPU_FECTL_FEMETHMODE_HALTED:       return "halted";
    case NV_PAPU_FECTL_FEMETHMODE_TRAPPED:      return "trapped";
    default:                                    return "?";
    }
}

void mcpx_apu_dump_state(MCPXAPUState *d, mcpx_apu_emit_fn emit)
{
    uint32_t fectl, sectl, ists, ien, fecv, feav;
    int list;

    if (!d) {
        emit("  [APU] no APU state\n");
        return;
    }

    fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
    sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
    ists  = qatomic_read(&d->regs[NV_PAPU_ISTS]);
    ien   = qatomic_read(&d->regs[NV_PAPU_IEN]);
    fecv  = qatomic_read(&d->regs[NV_PAPU_FECV]);
    feav  = qatomic_read(&d->regs[NV_PAPU_FEAV]);

    emit("  [APU] front end: FECTL=%08X mode=%s trap=%s FECV=%u FEAV=%u SECTL=%08X\n",
         fectl, fe_mode(fectl),
         (fectl & NV_PAPU_FECTL_FETRAPREASON) ? "requested" : "-",
         fecv, feav, sectl);

    emit("  [APU] irq: ISTS=%08X IEN=%08X line=%s set_irq=%d\n",
         ists, ien,
         (ists & NV_PAPU_ISTS_GINTSTS) ? "up" : "down",
         d->set_irq ? 1 : 0);

    emit("  [APU] frames: count=%d ep_div=%d se_frames=%u\n",
         d->frame_count, d->ep_frame_div, apu_irq_stats.se_frames);

    emit("  [APU] traps/deliveries: traps=%u delivered=%u posted=%u holder=%u"
         " held=%u forced=%u fe-trapped=%u\n",
         apu_irq_stats.traps, apu_irq_stats.delivered, apu_irq_stats.posted,
         apu_irq_stats.holder_delivered, apu_irq_stats.held_frames,
         apu_irq_stats.forced, apu_irq_stats.fe_trapped_frames);

    /* The APU lock. RecursionCount > 0 means held; OwningThread is the owner's
     * tid where the host keeps one (Windows), 0 elsewhere. */
    {
        LONG rec = d->lock.cs.RecursionCount;
        void *own = d->lock.cs.OwningThread;
        if (rec > 0)
            emit("  [APU] lock: held (recursion %ld, owner %p)\n",
                 (long)rec, own);
        else
            emit("  [APU] lock: free\n");
    }

    /* Every voice still in a list, with the fields that decide whether a
     * DirectSound buffer has gone idle: active/paused, the envelope state, and
     * the current/end buffer offsets (a voice at cbo == ebo has played out). */
    for (list = 0; list < 3; list++) {
        uint32_t top = qatomic_read(&d->regs[voice_lists[list].top]);
        uint32_t cur = qatomic_read(&d->regs[voice_lists[list].current]);
        uint32_t nxt = qatomic_read(&d->regs[voice_lists[list].next]);
        uint32_t h = top;
        int n;

        emit("  [APU] voice list %s: top=%u cur=%u next=%u\n",
             voice_lists[list].name, top, cur, nxt);
        for (n = 0; h != 0xFFFF && n < MCPX_HW_MAX_VOICES; n++) {
            uint16_t v = (uint16_t)h;
            uint32_t par, off, parn, link;

            /* The list links are read from guest RAM, which the freeze may
             * have left inconsistent; a handle past the 256 voices must not
             * turn into a read outside the voice array. */
            if (v >= MCPX_HW_MAX_VOICES) {
                emit("    (bad voice handle %u, list broken)\n", (unsigned)v);
                break;
            }
            par  = voice_reg(d, v, NV_PAVS_VOICE_PAR_STATE);
            off  = voice_reg(d, v, NV_PAVS_VOICE_PAR_OFFSET);
            parn = voice_reg(d, v, NV_PAVS_VOICE_PAR_NEXT);
            link = voice_reg(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK);

            emit("    voice %u: active=%d paused=%d eacur=%u efcur=%u"
                 " cbo=%u ebo=%u\n",
                 (unsigned)v,
                 (par & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) ? 1 : 0,
                 (par & NV_PAVS_VOICE_PAR_STATE_PAUSED) ? 1 : 0,
                 GET_MASK(par, NV_PAVS_VOICE_PAR_STATE_EACUR),
                 GET_MASK(par, NV_PAVS_VOICE_PAR_STATE_EFCUR),
                 GET_MASK(off, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                 GET_MASK(parn, NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = GET_MASK(link, NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
        if (n == MCPX_HW_MAX_VOICES)
            emit("    (list runs away: more than %d voices)\n",
                 MCPX_HW_MAX_VOICES);
    }

    /* Voices the title has VOICE_LOCK'd: one bit each, 256 bits in 4 words. */
    {
        uint64_t locked[4];
        int i, any = 0;

        for (i = 0; i < 4; i++) {
            locked[i] = qatomic_read(&d->vp.voice_locked[i]);
            any |= locked[i] != 0;
        }
        if (any) {
            emit("  [APU] locked voices:");
            for (i = 0; i < MCPX_HW_MAX_VOICES; i++)
                if (locked[i / 64] & (1ULL << (i % 64)))
                    emit(" %u", (unsigned)i);
            emit("\n");
        }
    }
}

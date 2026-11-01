/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * No DSP56300 runs here. The mixbins reach the output one of two ways:
 * - a title that wired a model of its GP effects image (apu_gp.h): the
 *   model's six speaker feeds, with its sends, in-place effects and the
 *   export its return voices play
 * - any other title: every bin folded to a speaker with the effects
 *   bypassed (apu_speakers.c in 5.1, even/odd bins in stereo)
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "apu_limit.h"
#include "apu_level.h"
#include "apu_speakers.h"
#include "recomp_env.h"
#include "fpconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto finds the doorbell from what the title programs.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (BLiNX 2
 * sub_003341BE spins on <block>+0x810; Burnout 3 sub_002F805E likewise). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved BLiNX 2's from 0x80A1C810 to
 * 0x80A20810); this does not. Wreckless fits too: its doorbell 0x014F8810 is
 * the first page of the scratch its GPSADDR table (0x01504000) maps, not
 * the table itself, which is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
/* A command to the title's GP image, as modelled (apu_gp.h; below). */
static void gp_command(MCPXAPUState *d, const uint32_t *doorbell);
/* RECOMP_ORACLE_CAP (apu_oracle_cap.c): plain dumps, off by default. */
int apu_ocap_on(void);
void apu_ocap_doorbell(MCPXAPUState *d, uint32_t value);
void apu_ocap_bins(MCPXAPUState *d, const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
void apu_ocap_out6(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                   const int8_t map[NUM_MIXBINS], const float *gain);

static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_sge;       /* the GPSADDR it was derived from */
static uint32_t *s_dsp_ack_auto_slot;     /* host address of the doorbell */

static void dsp_ack_init(void)
{
    const char *spec = recomp_env(RENV_APU_DSP_ACK);
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_APU_MIXDOWN_ALL);
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, resolved
 * once GPSADDR is programmed and the table's first entry names a page, and
 * again whenever the title reprograms GPSADDR. NULL until then. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge)
        return NULL;
    if (sge == s_dsp_ack_auto_sge)
        return s_dsp_ack_auto_slot;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    s_dsp_ack_auto_sge = sge;
    s_dsp_ack_auto_slot = (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
    fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps page"
                    " 0x%08X (%s); doorbell at physical 0x%08X\n",
            sge, page, mcpx_apu_phys_class(page), page + GP_DOORBELL_OFFSET);
    return s_dsp_ack_auto_slot;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (apu_ocap_on())
                apu_ocap_doorbell(d, *slot);
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            gp_command(d, slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            gp_command(d, slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* No DSP program runs here; each processor is only marked as set up
     * (ApuDspStandIn, apu_state.h). */
    d->gp.dsp = (ApuDspStandIn *)calloc(1, sizeof(ApuDspStandIn));
    d->ep.dsp = (ApuDspStandIn *)calloc(1, sizeof(ApuDspStandIn));

    if (d->gp.dsp) d->gp.dsp->unit = APU_DSP_UNIT_GLOBAL;
    if (d->ep.dsp) d->ep.dsp->unit = APU_DSP_UNIT_ENCODE;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* The bin -> speaker map of the 5.1 path (apu_speakers.c): DirectSound's
 * layout unless the title's wiring set its own. Read once a frame; set
 * before the APU starts. */
static apu_spk_map s_spk_map;
static int s_spk_map_set;

void mcpx_apu_set_speaker_map(const int8_t map[32])
{
    memcpy(s_spk_map, map, sizeof(s_spk_map));
    s_spk_map_set = 1;
}

/* RECOMP_AUDIO_FX_LEVEL (audio.fx_level): in 5.1, the level of the bins
 * the title's wiring names as effect sends (mcpx_apu_set_fx_bins), which
 * reach the speakers dry here since no effect runs. Live. */
static uint32_t s_fx_bins;
static float s_bin_gain[NUM_MIXBINS];
static volatile float s_fx_level = -1.0f;      /* < 0: not read yet */

static void fx_gains(void)
{
    if (s_fx_level < 0.0f) {
        const char *e = recomp_env(RENV_AUDIO_FX_LEVEL);
        float v = e && *e ? (float)atof(e) : 0.25f;
        s_fx_level = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
    for (int b = 0; b < NUM_MIXBINS; b++)
        s_bin_gain[b] = (s_fx_bins >> b) & 1u ? s_fx_level : 1.0f;
}

void mcpx_apu_set_fx_bins(uint32_t mask)
{
    s_fx_bins = mask;
}

void mcpx_apu_set_fx_level(float level)
{
    s_fx_level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
}

/* A dword of GP scratch, through the GPSADDR scatter-gather table (8 bytes
 * an entry, the page's physical address first, as xemu's scratch_rw walks
 * it). 0 with the table not set up yet. */
static uint32_t gp_scratch_word(MCPXAPUState *d, uint32_t off)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;
    if (!sge || !d->ram_ptr || (off >> 12) > d->regs[NV_PAPU_GPSMAXSGE])
        return 0;
    page = *(const uint32_t *)mcpx_apu_phys(sge + (off >> 12) * 8);
    if (!page || (page & 0xFFF))
        return 0;
    return *(const uint32_t *)(mcpx_apu_phys(page) + (off & 0xFFF));
}

/* ── the title's GP effects image, modelled (apu_gp.h) ───────────────────
 *
 * A title that wired its image (apu_gp_direct / _eq / _drive / _export)
 * gets its speaker feeds from that model instead of the fold below: its
 * commands load the model's effect states as they load the DSP's, and the
 * model's export lands in the same guest memory the GP program writes, so
 * the title's own voices can play it back. */
#include "apu_gp.h"

static uint32_t gp_mem_read(void *ctx, uint32_t off)
{
    return gp_scratch_word((MCPXAPUState *)ctx, off);
}

/* n dwords into GP scratch at off, a page at a time; pages the table does
 * not map yet are skipped. */
static void gp_mem_write(void *ctx, uint32_t off, const uint32_t *words, int n)
{
    MCPXAPUState *d = (MCPXAPUState *)ctx;
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]);

    if (!sge || !d->ram_ptr)
        return;
    while (n > 0) {
        uint32_t in_page = (0x1000u - (off & 0xFFF)) / 4u, page;
        int k = n < (int)in_page ? n : (int)in_page;
        if ((off >> 12) > d->regs[NV_PAPU_GPSMAXSGE])
            return;
        page = *(const uint32_t *)mcpx_apu_phys(sge + (off >> 12) * 8);
        if (page && !(page & 0xFFF))
            memcpy(mcpx_apu_phys(page) + (off & 0xFFF), words, (size_t)k * 4u);
        off += (uint32_t)k * 4u;
        words += k;
        n -= k;
    }
}

/* RECOMP_AUDIO_LFE_LOWPASS (audio.lfe_lowpass): the user's low-pass on
 * the model's LFE feed. < 0: not set, the title's wiring stands. Followed
 * every frame, since the model takes a change only between fades. */
static volatile int s_lfe_user = -2;    /* -2: the variable not read yet */

void mcpx_apu_set_custom_level(int mode)
{
    apu_level_set_mode(mode);
}

void mcpx_apu_set_lfe_lowpass(int hz)
{
    s_lfe_user = hz <= 0 ? 0 : hz < 40 ? 40 : hz > 250 ? 250 : hz;
}

static void gp_follow_lfe(void)
{
    int want = s_lfe_user;
    if (want == -2) {
        const char *e = recomp_env(RENV_AUDIO_LFE_LOWPASS);
        if (e && *e)
            mcpx_apu_set_lfe_lowpass(atoi(e));     /* "off" reads as 0 */
        else
            s_lfe_user = -1;
        want = s_lfe_user;
    }
    if (want >= 0 && apu_gp_lfe_lowpass_hz() != want)
        apu_gp_lfe_lowpass(want);
}

/* The guest's view of the GP's X memory (apu_gp_mem.xwin): the APU's
 * register window from 0x30000, plain RAM in this runtime, a dword an X
 * word. NULL before the memory layout is up. */
extern void *xbox_McpxWindow(void);

static uint32_t *gp_xwin(void)
{
    uint8_t *w = (uint8_t *)xbox_McpxWindow();
    return w ? (uint32_t *)(w + 0x30000) : NULL;
}

/* A command about to be acknowledged: its block is the five dwords before
 * the doorbell (GP scratch 0x800..0x80C). */
/* The doorbell whose command the model has already taken (at the start of
 * a frame, below), so the acknowledgement does not load it again. */
static const uint32_t *s_gp_taken;

static void gp_command(MCPXAPUState *d, const uint32_t *doorbell)
{
    apu_gp_mem m = { gp_mem_read, gp_mem_write, d, gp_xwin() };
    if (s_gp_taken == doorbell) {
        s_gp_taken = NULL;
        return;
    }
    if (apu_gp_wired())
        apu_gp_command(&m, doorbell[0], doorbell[-4], doorbell[-2], doorbell[-1]);
}

/* A command posted since the last acknowledgement poll: the GP program
 * looks at its doorbell when its frame starts, so a command the title
 * wrote just before this frame takes effect in it, not in the next. The
 * acknowledgement itself stays with the poll. */
static void gp_command_at_frame(MCPXAPUState *d)
{
    if (s_dsp_ack_count < 0 || !d->ram_ptr || s_gp_taken)
        return;
    if (s_dsp_ack_auto) {
        const uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            gp_command(d, slot);
            s_gp_taken = slot;
        }
        return;
    }
    for (int i = 0; i < s_dsp_ack_count; i++) {
        const uint32_t *slot = (const uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            gp_command(d, slot);
            s_gp_taken = slot;
            return;
        }
    }
}

/* pa, a voice buffer's physical address, in the block the GP model exports
 * (apu_vp.c: such a voice plays the model's output): 1 + the mixbin whose
 * slot it starts in, 0 when it is not in the block. */
int mcpx_apu_gp_export_buffer(MCPXAPUState *d, uint32_t pa)
{
    uint32_t off, bytes, sge, page;
    int first;

    if (!apu_gp_export_range(&off, &bytes, &first))
        return 0;
    sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]);
    if (!sge || !d->ram_ptr)
        return 0;
    for (uint32_t o = off & ~0xFFFu; o < off + bytes; o += 0x1000u) {
        uint32_t lo = o < off ? off : o, hi = o + 0x1000u;
        if (hi > off + bytes) hi = off + bytes;
        if ((o >> 12) > d->regs[NV_PAPU_GPSMAXSGE])
            break;
        page = *(const uint32_t *)mcpx_apu_phys(sge + (o >> 12) * 8);
        if (page && !(page & 0xFFF) && pa >= page + (lo & 0xFFF) &&
            pa < page + (hi - o))
            return 1 + first + (int)((o + (pa - page) - off) / (NUM_SAMPLES_PER_FRAME * 4u));
    }
    return 0;
}

#include "apu_bus.h"

/* The mix study (RECOMP_MIX_TRACE, apu_vp.c). */
int mcpx_apu_mix_trace_on(void);
void mcpx_apu_mix_trace_out(const float *acc, int nch, int n);

float mcpx_apu_bin_feed_gain(int bin)
{
    float g = 1.0f;
    if (bin < 0 || bin >= NUM_MIXBINS)
        return 0.0f;
    if (apu_gp_wired())
        return apu_gp_bin_gain(bin);
    if (s_fx_bins && s_fx_level >= 0.0f && ((s_fx_bins >> bin) & 1u))
        g = s_fx_level;
    return g;
}

/* Trace only (RECOMP_MIX_TRACE): the GP's scratch memory, where DirectSound
 * keeps the effect states of the title's image, as "G t=<s> off=<hex>
 * <24-bit words>" lines. */
void mcpx_apu_mix_trace_gp(MCPXAPUState *d, FILE *f, unsigned t, uint32_t off, uint32_t end)
{
    for (; off < end; off += 64) {
        fprintf(f, "G t=%u off=%04X", t, off);
        for (uint32_t k = 0; k < 64 && off + k < end; k += 4)
            fprintf(f, " %06X", gp_scratch_word(d, off + k) & 0xFFFFFF);
        fputc('\n', f);
    }
}

void mcpx_apu_speaker_map_get(int8_t map[32])
{
    if (apu_gp_wired()) {
        for (int b = 0; b < NUM_MIXBINS; b++)
            map[b] = (int8_t)apu_gp_bin_speaker(b);
        return;
    }
    if (!s_spk_map_set) {
        apu_spk_default_map(s_spk_map);
        s_spk_map_set = 1;
    }
    memcpy(map, s_spk_map, sizeof(s_spk_map));
}

/* RECOMP_TRACE=audio_channels: what reaches each mixbin and each output
 * channel, once a second (1500 frames of 32 samples):
 *
 *   [AUDIO-CH] t=12s layout=5.1 bins 0=-31.2 1=-30.8 4=-40.1 ... (dBFS RMS)
 *   [AUDIO-CH] t=12s out FL=-29.0 FR=-28.7 FC=-90 LFE=-90 SL=-38.2 SR=-37.9
 *
 * Bins that stayed silent are left out. Off: one branch a frame. */
static int s_ch_trace = -1;
static double s_ch_bin_sq[NUM_MIXBINS];
static double s_ch_out_sq[APU_SPK_CHANNELS];
static unsigned s_ch_frames, s_ch_secs;

static double ch_db(double sq, double n)
{
    return sq > 0 ? 10.0 * log10(sq / n) : -90.0;
}

static void ch_trace(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                     const int16_t *out, int nch)
{
    char line[1024];
    int len = 0;
    double n;

    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            s_ch_bin_sq[b] += (double)mixbins[b][i] * mixbins[b][i];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        for (int c = 0; c < nch; c++) {
            double x = out[i * nch + c] / 32768.0;
            s_ch_out_sq[c] += x * x;
        }
    if (++s_ch_frames < 1500)
        return;

    n = (double)s_ch_frames * NUM_SAMPLES_PER_FRAME;
    s_ch_secs++;
    for (int b = 0; b < NUM_MIXBINS; b++)
        if (s_ch_bin_sq[b] > 0 && len < (int)sizeof(line) - 16)
            len += snprintf(line + len, sizeof(line) - len, " %d=%.1f", b,
                            ch_db(s_ch_bin_sq[b], n));
    fprintf(stderr, "[AUDIO-CH] t=%us layout=%s bins%s\n", s_ch_secs,
            nch == APU_SPK_CHANNELS ? "5.1" : "stereo", len ? line : " (silent)");
    if (nch == APU_SPK_CHANNELS)
        fprintf(stderr, "[AUDIO-CH] t=%us out FL=%.1f FR=%.1f FC=%.1f LFE=%.1f"
                        " SL=%.1f SR=%.1f\n", s_ch_secs,
                ch_db(s_ch_out_sq[0], n), ch_db(s_ch_out_sq[1], n),
                ch_db(s_ch_out_sq[2], n), ch_db(s_ch_out_sq[3], n),
                ch_db(s_ch_out_sq[4], n), ch_db(s_ch_out_sq[5], n));
    else
        fprintf(stderr, "[AUDIO-CH] t=%us out L=%.1f R=%.1f\n", s_ch_secs,
                ch_db(s_ch_out_sq[0], n), ch_db(s_ch_out_sq[1], n));
    memset(s_ch_bin_sq, 0, sizeof(s_ch_bin_sq));
    memset(s_ch_out_sq, 0, sizeof(s_ch_out_sq));
    s_ch_frames = 0;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    {
        /* RECOMP_AUDIO_SAFETY_LIMITER=0: the clamp alone, for comparison. */
        static int s_lim_env;
        if (!s_lim_env) {
            const char *e = recomp_env(RENV_AUDIO_SAFETY_LIMITER);
            s_lim_env = 1;
            if (e && (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "false")))
                apu_limit_enable(0);
        }
    }

    if (apu_ocap_on())
        apu_ocap_bins(d, (const float (*)[NUM_SAMPLES_PER_FRAME])mixbins);

    if (s_ch_trace < 0)
        s_ch_trace = recomp_env_on(RENV_AUDIO_CHANNELS);

    /* A title with its GP image wired: the model's six speaker feeds, in
     * stereo folded as the console's analog output is taken to be (ITU-R
     * BS.775: centre and surrounds at -3 dB, no LFE). */
    if (apu_gp_wired() && d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        /* While a capture for the reference tool is taken
         * (RECOMP_ORACLE_CAP) the model writes nothing into GP scratch: the
         * capture is of what the title does, and the tool makes the export
         * and the delay lines itself. The return voices then play silence
         * and the reverb has no memory, as in the build the captures were
         * first made with. */
        apu_gp_mem m = { gp_mem_read, apu_ocap_on() ? NULL : gp_mem_write, d, gp_xwin() };
        float acc[APU_SPK_CHANNELS][NUM_SAMPLES_PER_FRAME];
        const int six = d->monitor.channels == APU_SPK_CHANNELS;
        const int nch = six ? APU_SPK_CHANNELS : 2;

        gp_follow_lfe();
        gp_command_at_frame(d);
        apu_gp_frame(&m, mixbins, acc);
        if (!six) {
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float c = 0.70710678f * acc[APU_SPK_FC][i];
                float l = acc[APU_SPK_FL][i] + c + 0.70710678f * acc[APU_SPK_SL][i];
                float r = acc[APU_SPK_FR][i] + c + 0.70710678f * acc[APU_SPK_SR][i];
                acc[0][i] = l;
                acc[1][i] = r;
            }
        }
        if (apu_bus_enabled())
            apu_bus_output(acc, nch, NUM_SAMPLES_PER_FRAME);
        if (mcpx_apu_mix_trace_on()) {
            float il[NUM_SAMPLES_PER_FRAME * APU_SPK_CHANNELS];
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                for (int c = 0; c < nch; c++)
                    il[i * nch + c] = acc[c][i];
            mcpx_apu_mix_trace_out(il, nch, NUM_SAMPLES_PER_FRAME);
        }
        apu_limit(acc, nch, NUM_SAMPLES_PER_FRAME);     /* the safety limiter (apu_limit.h) */
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            for (int c = 0; c < nch; c++) {
                float x = acc[c][i];
                if (x > 1.0f) x = 1.0f;
                if (x < -1.0f) x = -1.0f;
                if (six)
                    d->monitor.frame_buf6[off + i][c] = (int16_t)(x * 32767.0f);
                else
                    d->monitor.frame_buf[off + i][c] = (int16_t)(x * 32767.0f);
            }
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     six ? &d->monitor.frame_buf6[off][0] : &d->monitor.frame_buf[off][0],
                     nch);
        g_dbg.gp.cycles = 0;
        g_dbg.ep.cycles = 0;
        return;
    }

    /* 5.1: the six speaker feeds (apu_speakers.c), and the stereo fold
     * below is not run at all, so the stereo path is the same code it
     * always was. */
    if (d->monitor.channels == APU_SPK_CHANNELS &&
        d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        if (!s_spk_map_set) {
            apu_spk_default_map(s_spk_map);
            s_spk_map_set = 1;
        }
        fx_gains();
        if (apu_bus_enabled()) {
            /* The host mixer on (apu_bus.h): apu_spk_mix51's sums, bin by
             * bin in the same order, kept as float for the output stage
             * and the meters, then its conversion. */
            float acc[APU_SPK_CHANNELS][NUM_SAMPLES_PER_FRAME];
            const float *gain = s_fx_bins ? s_bin_gain : NULL;
            memset(acc, 0, sizeof(acc));
            for (int b = 0; b < NUM_MIXBINS; b++) {
                const float *src = mixbins[b];
                float *dst;
                if (s_spk_map[b] < 0 || s_spk_map[b] >= APU_SPK_CHANNELS)
                    continue;
                dst = acc[s_spk_map[b]];
                if (gain && gain[b] != 1.0f) {
                    const float g = gain[b];
                    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                        dst[i] += g * src[i];
                } else {
                    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                        dst[i] += src[i];
                }
            }
            apu_bus_output(acc, APU_SPK_CHANNELS, NUM_SAMPLES_PER_FRAME);
            apu_limit(acc, APU_SPK_CHANNELS, NUM_SAMPLES_PER_FRAME);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                for (int c = 0; c < APU_SPK_CHANNELS; c++) {
                    float x = acc[c][i];
                    if (x > 1.0f) x = 1.0f;
                    if (x < -1.0f) x = -1.0f;
                    d->monitor.frame_buf6[off + i][c] = (int16_t)(x * 32767.0f);
                }
        } else
        apu_spk_mix51(&mixbins[0][0], NUM_SAMPLES_PER_FRAME, s_spk_map,
                      s_fx_bins ? s_bin_gain : NULL,
                      &d->monitor.frame_buf6[off], NUM_SAMPLES_PER_FRAME);
        if (mcpx_apu_mix_trace_on()) {
            /* The same sums again, kept as float: the trace's copy. */
            float acc[NUM_SAMPLES_PER_FRAME * APU_SPK_CHANNELS];
            memset(acc, 0, sizeof(acc));
            for (int b = 0; b < NUM_MIXBINS; b++) {
                int sp = s_spk_map[b];
                float g = s_fx_bins ? s_bin_gain[b] : 1.0f;
                if (sp < 0 || sp >= APU_SPK_CHANNELS) continue;
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    acc[i * APU_SPK_CHANNELS + sp] += g * mixbins[b][i];
            }
            mcpx_apu_mix_trace_out(acc, APU_SPK_CHANNELS, NUM_SAMPLES_PER_FRAME);
        }
        if (apu_ocap_on())
            apu_ocap_out6((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins, s_spk_map,
                          s_fx_bins ? s_bin_gain : NULL);
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     &d->monitor.frame_buf6[off][0], APU_SPK_CHANNELS);
        g_dbg.gp.cycles = 0;
        g_dbg.ep.cycles = 0;
        return;
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP && apu_bus_enabled()) {
        /* The host mixer on (apu_bus.h): the fold below, sample by sample
         * in the same order, kept as float for the output stage and the
         * meters, then the same clamp and conversion. */
        float lr[2][NUM_SAMPLES_PER_FRAME];
        const int all = mcpx_apu_mixdown_all();
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left, right;
            if (all) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            lr[0][i] = left;
            lr[1][i] = right;
        }
        apu_bus_output(lr, 2, NUM_SAMPLES_PER_FRAME);
        apu_limit(lr, 2, NUM_SAMPLES_PER_FRAME);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left = lr[0][i], right = lr[1][i];
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     &d->monitor.frame_buf[off][0], 2);
    } else
    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        const int mix_trace = mcpx_apu_mix_trace_on();
        float fold[2][NUM_SAMPLES_PER_FRAME];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            if (mix_trace) {
                float lr[2] = { left, right };
                mcpx_apu_mix_trace_out(lr, 2, 1);
            }
            fold[0][i] = left;
            fold[1][i] = right;
        }
        apu_limit(fold, 2, NUM_SAMPLES_PER_FRAME);      /* the safety limiter (apu_limit.h) */
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left = fold[0][i], right = fold[1][i];
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     &d->monitor.frame_buf[off][0], 2);
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}

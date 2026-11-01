/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "recomp_env.h"
#include "fpconv.h"
#include "../kernel/kernel_prof.h"

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* The mix study (RECOMP_MIX_TRACE, further down). 0: off. */
static int s_mix_on = -1;
static void mix_trace_on_off(MCPXAPUState *d, unsigned v, int on);

/* The host mixer (apu_bus.h): the runtime block of this VP frame, NULL with
 * the mixer off; each voice's turn-on count and, with the mixer or the mix
 * study on, the guest return addresses of who turned it on. */
#include "apu_bus.h"
#include "apu_level.h"
#include "apu.h"
static struct apu_bus_rt *s_bus_rt;
static int s_bus_audition;              /* this frame: an audition clip loops */
static unsigned s_von_serial[MCPX_HW_MAX_VOICES];
/* A title VOICE_OFF still to be seen by the VP: 1 + the voice's VOICE_ON
 * serial at the off, 0 once checked or turned on again (voice_off_sticky). */
static unsigned s_off_pending[MCPX_HW_MAX_VOICES];
static unsigned long s_off_redone;
static uint32_t s_von_stack[MCPX_HW_MAX_VOICES][16];
static uint8_t s_von_nstack[MCPX_HW_MAX_VOICES];
static mcpx_apu_voice_tag_fn s_mix_tag_fn;

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].lowpass, 0, sizeof(d->vp.filters[v].lowpass));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    d->vp.filters[v].rs_have = 0;
    d->vp.filters[v].rs_pos = 0.0f;
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

/* RECOMP_ORACLE_CAP (apu_oracle_cap.c): plain dumps, off by default. */
int apu_ocap_on(void);
void apu_ocap_voice(const void *a, uint32_t an, const void *b, uint32_t bn);

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

/* A voice went off since the last idle scan (mcpx_apu_vp_idle_scan). */
static volatile int s_idle_pending;

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    s_idle_pending = 1;
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* The VP's first look at a voice since the title turned it off (the walk and
 * the idle scan, under d->lock). The hardware has no way from off back to on
 * but VOICE_ON, so a voice found on again with no VOICE_ON since had its off
 * undone behind the VP's back: soaks caught the ACTIVE bit back between the
 * title's VOICE_OFF and the next pass, with every FE method under d->lock, so
 * the store comes from outside the APU model (BO-138). The voice then played
 * to its own end (for good, if it loops) while DirectSound's StopEx-and-wait
 * (Burnout 3's 0x0020B590, Sleep(0) until the buffer stops) held guest-main:
 * mid-race freezes of 0.35-1.6 s. The off stands: the bit is cleared again and
 * the voice traps idle as it would have. The off already raised its
 * notification. */
static void voice_off_sticky(MCPXAPUState *d, uint16_t v)
{
    unsigned p;

    if (v >= MCPX_HW_MAX_VOICES || !(p = s_off_pending[v]))
        return;
    s_off_pending[v] = 0;
    if (p - 1 != s_von_serial[v])
        return;
    if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
        voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);
        s_idle_pending = 1;
        if (++s_off_redone <= 20 || s_off_redone % 100 == 0)
            fprintf(stderr, "[APU] voice %u: the title's VOICE_OFF was undone"
                            " before the next VP pass; cleared again (%lu)\n",
                    (unsigned)v, s_off_redone);
    }
}

/* VOICE_LOCK's bitmap. The caller holds d->lock: fe_method runs under it
 * (mcpx_apu_vp_write takes it for the title's methods, the frame thread holds
 * it for its own). */
static void voice_lock_held(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }
    qemu_cond_signal(&d->cond);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock_held(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        if (selected_handle < MCPX_HW_MAX_VOICES)
            s_off_pending[selected_handle] = 0;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock_held(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock_held(d, (uint16_t)selected_handle, false);
        if (selected_handle < MCPX_HW_MAX_VOICES) {
            s_von_serial[selected_handle]++;
            if ((s_mix_on || apu_bus_enabled() || apu_level_active()) && s_mix_tag_fn)
                s_von_nstack[selected_handle] =
                    (uint8_t)s_mix_tag_fn(s_von_stack[selected_handle], 16);
            else
                s_von_nstack[selected_handle] = 0;
        }
        if (s_mix_on) mix_trace_on_off(d, selected_handle, 1);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock_held(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock_held(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        {
            uint16_t h = (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE);
            if (h < MCPX_HW_MAX_VOICES)
                s_off_pending[h] = s_von_serial[h] + 1;
        }
        if (s_mix_on) mix_trace_on_off(d, argument & NV1BA0_PIO_VOICE_OFF_HANDLE, 0);
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        if (recomp_env_on(RENV_AUDIO_CHANNELS))
            fprintf(stderr, "[AUDIO-CH] HRTF submixes %u,%u,%u,%u (3D voices'"
                            " first four bins)\n",
                    d->vp.hrtf_submix[0], d->vp.hrtf_submix[1],
                    d->vp.hrtf_submix[2], d->vp.hrtf_submix[3]);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            /* Latched here, once per trap; update_irq no longer re-derives
             * it from FECTL (see there). */
            qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
            d->set_irq = true;
            apu_irq_stats.traps++;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
            if (recomp_env_on(RENV_AUDIO_CHANNELS))
                fprintf(stderr, "[AUDIO-CH] submix headroom: bin %u = %u (mixed at 1/%u)\n",
                        (unsigned)slot, (unsigned)d->vp.submix_headroom[slot],
                        1u << d->vp.submix_headroom[slot]);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* The title's methods run under d->lock, as the frame thread's own work
     * does. They write the same voice words the frame thread rewrites every
     * VP frame (PAR_STATE: VOICE_ON, VOICE_OFF, VOICE_PAUSE, VOICE_RELEASE;
     * PAR_OFFSET, the list links), with a plain load and store each. Unlocked,
     * a VOICE_OFF landing inside the frame thread's read-modify-write of
     * PAR_STATE was undone: the voice played on to its own end, and
     * DirectSound's StopEx-and-wait (Burnout 3's 0x0020B590, Sleep(0) until
     * the buffer stops) held guest-main that long, mid-race freezes of
     * 0.35-1.6 s (BO-138). Held, a method lands between frames. The frame
     * thread drops the lock in its pacing wait and once per frame, as
     * voice_lock's callers always needed. A notification raised or a voice
     * turned off wakes it out of that wait to report it (throttle,
     * apu_core.c); signalled under the lock, so it is not lost. */
    if (xbox_ProfOn()) {
        long long t0 = xbox_ProfNowUs();
        qemu_mutex_lock(&d->lock);
        xbox_ProfWait(XBOX_PROF_WAIT_APULOCK, xbox_ProfNowUs() - t0);
    } else {
        qemu_mutex_lock(&d->lock);
    }
    fe_method(d, (uint32_t)addr, (uint32_t)val);
    if (d->set_irq || s_idle_pending)
        qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Per-voice source statistics (RECOMP_APU_TRACE)
 *
 * What each voice fed the mixer over the last second: its format word, where
 * its data was read from and how that address resolved, the amplitude
 * envelope and the first two volumes/bins, plus the RMS and zero-crossing
 * rate of the fetched samples. The zero-crossing rate separates music
 * (~0.1-0.2) from white noise (~0.5) without a spectrum, so a voice that
 * reads garbage shows up by itself.
 *
 * RECOMP_APU_VOICE_DUMP=<voice> also writes that voice's fetched samples,
 * before gain, as raw s16 stereo 48 kHz to RECOMP_APU_VOICE_DUMP_FILE
 * (default voice_dump.raw in the working directory).
 * ============================================================ */

struct VpVoiceStat {
    uint32_t frames, zc;
    double   sq;
    float    last, ea, rate;
    uint32_t addr, fmt;
    uint16_t vol0, vol1;
    uint8_t  bin0, bin1;
    uint8_t  bins[8];                   /* all eight, 3D's first four HRTF */
    uint16_t vols[8];                   /* and their volumes (attenuation) */
    const char *cls;
};
static struct VpVoiceStat s_vstat[MCPX_HW_MAX_VOICES];
static int s_vstat_on = -1;
static int s_vdump_voice = -1;      /* -2: every voice, one file each */
static FILE *s_vdump;
static FILE *s_vdump_all[MCPX_HW_MAX_VOICES];
static char s_vdump_prefix[512];

static void vp_stat_init(void)
{
    const char *e;
    /* RECOMP_TRACE=audio_channels too: which bins each voice feeds is half
     * of what that trace is for. */
    s_vstat_on = recomp_env_on(RENV_APU_TRACE) || recomp_env_on(RENV_AUDIO_CHANNELS);
    e = recomp_env(RENV_APU_VOICE_DUMP);
    if (e && *e) {
        const char *path = recomp_env(RENV_APU_VOICE_DUMP_FILE);
        if (!strcmp(e, "all")) {
            /* RECOMP_APU_VOICE_DUMP=all: <file>.<voice>.raw for each voice
             * that renders anything (voice slots are not stable between
             * runs, so a sound cannot be picked by number in advance). */
            s_vdump_voice = -2;
            snprintf(s_vdump_prefix, sizeof(s_vdump_prefix), "%s",
                     path && *path ? path : "voice_dump");
            fprintf(stderr, "[APU] voice dump: every voice -> %s.<voice>.raw\n",
                    s_vdump_prefix);
            return;
        }
        s_vdump_voice = atoi(e);
        s_vdump = fopen(path && *path ? path : "voice_dump.raw", "wb");
        fprintf(stderr, "[APU] voice dump: voice %d -> %s\n", s_vdump_voice,
                s_vdump ? (path && *path ? path : "voice_dump.raw") : "(open failed)");
    }
}

static void vp_stat_samples(MCPXAPUState *d, uint32_t v, const float samples[][2],
                            int n, hwaddr first_addr, uint32_t fmt)
{
    struct VpVoiceStat *s = &s_vstat[v];
    if (s_vstat_on < 0) vp_stat_init();
    if (!s_vstat_on && (int)v != s_vdump_voice && s_vdump_voice != -2) return;
    if (n <= 0) return;
    if (s->frames == 0) {
        s->addr = (uint32_t)first_addr;
        s->cls = mcpx_apu_phys_class(first_addr);
        s->fmt = fmt;
    }
    for (int i = 0; i < n; i++) {
        float x = samples[i][0];
        s->sq += (double)x * x;
        if ((x < 0) != (s->last < 0)) s->zc++;
        s->last = x;
    }
    s->frames += (uint32_t)n;
    if (s_vdump_voice == -2 && !s_vdump_all[v]) {
        char name[600];
        snprintf(name, sizeof(name), "%s.%u.raw", s_vdump_prefix, (unsigned)v);
        s_vdump_all[v] = fopen(name, "wb");
    }
    {
        FILE *df = s_vdump_voice == -2 ? s_vdump_all[v]
                 : (int)v == s_vdump_voice ? s_vdump : NULL;
        int16_t out[NUM_SAMPLES_PER_FRAME * 2];
        for (int i = 0; df && i < n && i < NUM_SAMPLES_PER_FRAME; i++) {
            for (int c = 0; c < 2; c++) {
                float x = samples[i][c] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                out[2 * i + c] = (int16_t)x;
            }
        }
        if (df)
            fwrite(out, sizeof(int16_t) * 2, (size_t)(n < NUM_SAMPLES_PER_FRAME ? n : NUM_SAMPLES_PER_FRAME), df);
    }
    (void)d;
}

static void vp_stat_report(void)
{
    static unsigned frames, secs;
    if (s_vstat_on <= 0) return;
    if (++frames % 1500) return;
    secs++;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpVoiceStat *s = &s_vstat[v];
        if (!s->frames) continue;
        fprintf(stderr, "[APU-VSTAT] t=%us v=%u fmt=%08X stream=%u st=%u ss=%u cs=%u spb=%u"
                " rate=%.3f ea=%.2f vol=%03X/%03X bin=%u/%u addr=%08X %s"
                " frames=%u rms=%.0f zcr=%.2f bins=%u,%u,%u,%u,%u,%u,%u,%u"
                " vols=%03X,%03X,%03X,%03X,%03X,%03X,%03X,%03X%s\n",
                secs, v, s->fmt,
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK)),
                s->rate, s->ea, s->vol0, s->vol1, s->bin0, s->bin1, s->addr,
                s->cls ? s->cls : "?", s->frames,
                sqrt(s->sq / s->frames) * 32768.0, (double)s->zc / s->frames,
                s->bins[0], s->bins[1], s->bins[2], s->bins[3],
                s->bins[4], s->bins[5], s->bins[6], s->bins[7],
                s->vols[0], s->vols[1], s->vols[2], s->vols[3],
                s->vols[4], s->vols[5], s->vols[6], s->vols[7],
                v < MCPX_HW_MAX_3D_VOICES ? " 3d" : "");
        s->frames = 0; s->zc = 0; s->sq = 0;
    }
    if (s_vdump) fflush(s_vdump);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (s_vdump_all[v]) fflush(s_vdump_all[v]);
}

/* ============================================================
 * Ring monitor (RECOMP_TRACE=apu_ring)
 *
 * A title that streams compressed audio decodes it on the CPU into a looping
 * PCM buffer voice and keeps writing ahead of the play cursor. If the writer
 * falls behind, the VP plays the previous lap's data again: no digital zero,
 * nothing a check on the mix can see, and only a stutter to the ear.
 *
 * Each looping PCM buffer is split into 1 KB blocks, and the samples the VP
 * fetches are hashed per block as it plays them. When the cursor leaves a
 * block it played whole, that hash is compared with the one from the
 * block's previous play. Once some block of a buffer has played differently
 * on two laps, the buffer is a ring (one loaded once and looped -- a sound
 * effect -- never changes and is never judged). In a ring, a non-silent
 * block that plays exactly as it did one lap before is a stale-lap replay.
 *
 * "lead" is how far ahead of the cursor the writer is: the blocks after the
 * one being entered whose memory has changed since they were last played,
 * in source frames.
 *
 *   [APU-RING] stale v=<voice> blk=<b>/<n> lap=<laps> ms=<wall ms>
 *       first 200 stale-lap replays
 *   [APU-RING] t=<s> v=<voice> bytes=<ring size> entries=<blocks played>
 *       stale=<this second> stale_total=<run> lead_min=<frames> lead_avg=<frames>
 *       once a VP second for each ring that played
 * ============================================================ */

#define RING_BLK      1024u
#define RING_MAX_BLKS 1024u

struct VpRing {
    uint32_t ba, bytes, nblk;
    uint32_t cur;                 /* block being played, ~0: none */
    uint32_t cur_frames;          /* frames of it played so far */
    uint64_t acc;                 /* hash of those frames */
    bool     acc_sound;
    uint32_t laps;
    bool     dynamic;
    uint32_t entries, stale, stale_total;
    uint32_t lead_min, lead_n;
    uint64_t lead_sum;
    uint64_t played[RING_MAX_BLKS];   /* hash of its last whole play */
    uint64_t mem[RING_MAX_BLKS];      /* memory hash when it was last entered */
    uint8_t  seen[RING_MAX_BLKS];     /* bit 0: played, bit 1: mem valid */
};
static struct VpRing *s_ring[MCPX_HW_MAX_VOICES];
static int s_ring_on = -1;
static unsigned s_ring_logged;

static uint64_t ring_hash_block(MCPXAPUState *d, uint32_t ba, uint32_t off,
                                uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    while (len) {
        uint32_t lin = ba + off;
        uint32_t run = TARGET_PAGE_SIZE - lin % TARGET_PAGE_SIZE;
        if (run > len) run = len;
        const uint8_t *p = mcpx_apu_phys(
            get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, lin));
        uint32_t i = 0;
        for (; i + 8 <= run; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < run; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        off += run;
        len -= run;
    }
    return h;
}

/* The cursor left block r->cur. */
static void ring_close_block(struct VpRing *r, uint32_t v, uint32_t frames_per_blk)
{
    uint32_t b = r->cur;
    if (b == ~0u || r->cur_frames < frames_per_blk)
        return;                              /* not played whole */
    if (r->seen[b] & 1) {
        if (r->acc != r->played[b]) {
            r->dynamic = true;
        } else if (r->dynamic && r->acc_sound) {
            r->stale++;
            r->stale_total++;
            if (s_ring_logged < 200) {
                s_ring_logged++;
                fprintf(stderr, "[APU-RING] stale v=%u blk=%u/%u lap=%u ms=%llu\n",
                        v, b, r->nblk, r->laps,
                        (unsigned long long)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) / 1000));
            }
        }
    }
    r->played[b] = r->acc;
    r->seen[b] |= 1;
}

/* The VP fetched n frames of a looping PCM buffer voice, the first at
 * offset first; frames are frame_bytes apart in the buffer. */
static void vp_ring_check(MCPXAPUState *d, uint32_t v, uint32_t ba,
                          uint32_t ebo, uint32_t frame_bytes, uint32_t first,
                          const float samples[][2], int n)
{
    struct VpRing *r;
    uint32_t bytes = (ebo + 1) * frame_bytes;
    uint32_t fpb;

    if (s_ring_on < 0)
        s_ring_on = recomp_env_on(RENV_APU_RING);
    if (!s_ring_on || v >= MCPX_HW_MAX_VOICES || bytes < 2 * RING_BLK ||
        frame_bytes == 0 || RING_BLK % frame_bytes)
        return;
    fpb = RING_BLK / frame_bytes;
    r = s_ring[v];
    if (!r) {
        r = s_ring[v] = calloc(1, sizeof(*r));
        if (!r) return;
        r->ba = ~0u;
    }
    if (r->ba != ba || r->bytes != bytes) {   /* a new buffer on this voice */
        uint32_t keep_total = r->stale_total;
        memset(r, 0, sizeof(*r));
        r->ba = ba;
        r->bytes = bytes;
        r->nblk = bytes / RING_BLK;           /* a partial last block is skipped */
        if (r->nblk > RING_MAX_BLKS) r->nblk = RING_MAX_BLKS;
        r->cur = ~0u;
        r->lead_min = ~0u;
        r->stale_total = keep_total;
    }

    for (int i = 0; i < n; i++) {
        uint32_t b = (first + (uint32_t)i) / fpb;
        if (b != r->cur) {
            ring_close_block(r, v, fpb);
            if (b < r->cur && r->cur != ~0u)
                r->laps++;
            r->cur = b;
            r->cur_frames = 0;
            r->acc = 0x9E3779B97F4A7C15ull;
            r->acc_sound = false;
            if (b < r->nblk) {
                /* Entering b: the writer's lead, then b's memory hash. */
                uint64_t h = ring_hash_block(d, ba, b * RING_BLK, RING_BLK);
                r->entries++;
                if (r->dynamic) {
                    uint32_t k, lead = 0;
                    for (k = 1; k < r->nblk; k++) {
                        uint32_t bb = (b + k) % r->nblk;
                        if (!(r->seen[bb] & 2))
                            break;
                        if (ring_hash_block(d, ba, bb * RING_BLK, RING_BLK) == r->mem[bb])
                            break;
                        lead++;
                    }
                    lead *= fpb;
                    if (lead < r->lead_min) r->lead_min = lead;
                    r->lead_sum += lead;
                    r->lead_n++;
                }
                r->mem[b] = h;
                r->seen[b] |= 2;
            }
        }
        if (b >= r->nblk)
            continue;
        {
            uint32_t w[2];
            memcpy(w, samples[i], sizeof(w));
            r->acc = (r->acc ^ (((uint64_t)w[0] << 32) | w[1])) * 0x100000001B3ull;
            r->acc ^= r->acc >> 29;
            if (w[0] | w[1]) r->acc_sound = true;
            r->cur_frames++;
        }
    }
}

static void vp_ring_report(unsigned secs)
{
    if (s_ring_on <= 0) return;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpRing *r = s_ring[v];
        if (!r || !r->entries || !r->dynamic) {
            if (r) r->entries = 0;
            continue;
        }
        fprintf(stderr, "[APU-RING] t=%us v=%u bytes=%u entries=%u stale=%u"
                " stale_total=%u lead_min=%u lead_avg=%llu\n",
                secs, v, r->bytes, r->entries, r->stale, r->stale_total,
                r->lead_n ? r->lead_min : 0,
                (unsigned long long)(r->lead_n ? r->lead_sum / r->lead_n : 0));
        r->entries = 0;
        r->stale = 0;
        r->lead_min = ~0u;
        r->lead_sum = 0;
        r->lead_n = 0;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    hwaddr stat_addr = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (sample_count == 0) {
            uint32_t lin = adpcm ? (cbo / ADPCM_SAMPLES_PER_BLOCK) * (uint32_t)block_size
                                 : cbo * (uint32_t)block_size;
            stat_addr = stream ? segment_offset + lin
                               : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                              0xFFFFFFFF, ba + lin);
        }
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    vp_stat_samples(d, v, samples, sample_count, stat_addr,
                    voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                   0xFFFFFFFF));
    if (!stream && loop && !adpcm && sample_count > 0)
        vp_ring_check(d, v, ba, ebo, (uint32_t)block_size,
                      cbo - (uint32_t)sample_count, samples, sample_count);

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (linear interpolation in place of libsamplerate)
 *
 * libsamplerate is stubbed; voice_resample below interpolates linearly
 * between source frames at the pitch's ratio, keeping its position and
 * the last frame across calls in the voice's filter state.
 * ============================================================ */

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    /* Linear interpolation in place of libsamplerate. `rate` is the output
     * to source ratio the VP's pitch encodes (libsamplerate's src_ratio):
     * 1.088 for BLiNX 2's 44.1 kHz material at the 48 kHz frame rate, so
     * each output consumes 1/rate source samples. Fetching one source sample
     * per output, as the stub before did, played everything a semitone and a
     * half high, 8.8 % too fast -- and drained each stream buffer faster than
     * the title filled it, which is where the periodic dropouts came from.
     *
     * State per voice: the two source samples the output sits between
     * (rs_last at 0, rs_next at 1) and the output's position rs_pos in
     * [0,1). Source samples arrive from voice_get_samples in the voice's own
     * units; a shortfall (the voice ended or its stream ran dry) returns the
     * outputs that could be made, and a fetch failure with nothing made is
     * passed through as it was. */
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    float src[NUM_SAMPLES_PER_FRAME * 4 + 2][2];
    const int max_src = (int)(sizeof(src) / sizeof(src[0]));
    float step;
    int need, got, in, out;

    if (!(rate > 0.0f)) rate = 1.0f;
    step = 1.0f / rate;

    if (!f->rs_have) {
        got = voice_get_samples(d, v, src, 2);
        if (got < 2) return got < 0 ? got : 0;
        f->rs_last[0] = src[0][0]; f->rs_last[1] = src[0][1];
        f->rs_next[0] = src[1][0]; f->rs_next[1] = src[1][1];
        f->rs_pos = 0.0f;
        f->rs_have = 1;
    }

    /* Source samples the next requested_num outputs will step past, exactly:
     * output k sits at rs_pos + k * step, and every whole unit that position
     * reaches before the last output is a source sample consumed. The last
     * output's own step is left pending in rs_pos for the next call, so
     * nothing fetched here goes unused -- fetching floor(rs_pos + n * step)
     * instead over-read by up to one sample a frame, which was dropped: the
     * music came out 3 % fast. A rate far below 1 (a deep pitch bend) would
     * want more than the buffer holds; make fewer outputs rather than lose
     * source. */
    for (;;) {
        need = (int)floorf(f->rs_pos + (float)(requested_num - 1) * step);
        if (need <= max_src || requested_num <= 1) break;
        requested_num /= 2;
    }
    if (need > max_src) need = max_src;
    if (need < 0) need = 0;

    got = need > 0 ? voice_get_samples(d, v, src, need) : 0;
    if (got < 0) got = 0;

    for (in = 0, out = 0; out < requested_num; out++) {
        while (f->rs_pos >= 1.0f) {
            if (in >= got) goto done;
            f->rs_last[0] = f->rs_next[0]; f->rs_last[1] = f->rs_next[1];
            f->rs_next[0] = src[in][0];    f->rs_next[1] = src[in][1];
            in++;
            f->rs_pos -= 1.0f;
        }
        samples[out][0] = f->rs_last[0] + f->rs_pos * (f->rs_next[0] - f->rs_last[0]);
        samples[out][1] = f->rs_last[1] + f->rs_pos * (f->rs_next[1] - f->rs_last[1]);
        f->rs_pos += step;
    }
done:
    if (out == 0 && got == 0) {
        /* Nothing new and nothing left to interpolate: the voice has stopped
         * or its stream is empty. voice_process treats 0 as an underrun and
         * a negative as the end; the voice's active bit says which. */
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}

/* ============================================================
 * Mix study (RECOMP_MIX_TRACE=<file>, BO-114)
 *
 * What a host mixer could tell apart, measured: nothing here feeds back
 * into the mix, and with the variable unset each hook is one branch.
 *
 *   ON id=<n> v=<voice> f=<vp frame> stack=<a>,<b>,...
 *       a voice turned on (NV1BA0_PIO_VOICE_ON): the title's return
 *       addresses on the guest stack of the thread that did it, innermost
 *       first (mcpx_apu_set_voice_tag_hook)
 *   OFF id=<n> v=<voice> f=<vp frame>
 *   V t=<s> id=<n> v=<voice> 3d= st= stream= fmt= fmode= hrtf= rate= ea=
 *     ba=<buffer base> ebo=<end offset> frames=<vp frames mixed> src=<rms of
 *     the samples before gain, s16 units> bins=<8> vols=<8, attenuation>
 *     spk=<sum of squares the voice put into each speaker: FL FR C LFE SL SR
 *     in 5.1, L R in stereo> peak=<its largest speaker sample>
 *       once a second for every voice that mixed anything
 *   O t=<s> ch=<n> sq=<sum of squares per channel> peak=<per channel, before
 *     the clamp> over=<samples past full scale per channel> n=<samples>
 * ============================================================ */

#include <time.h>
#include "apu_speakers.h"

static FILE *s_mix_f;
static unsigned s_mix_serial, s_mix_frames, s_mix_secs;
static struct MixVoice {
    unsigned id, frames;
    double src_sq, spk_sq[APU_SPK_CHANNELS];
    float peak, rate, ea;
    uint32_t fmt, ba, ebo;
    uint8_t bins[8], fmode, hrtf, d3;
    uint16_t vols[8];
} s_mix_v[MCPX_HW_MAX_VOICES];
static double s_mix_out_sq[APU_SPK_CHANNELS];
static float s_mix_out_peak[APU_SPK_CHANNELS];
static unsigned s_mix_out_over[APU_SPK_CHANNELS], s_mix_out_n, s_mix_out_ch;

void mcpx_apu_set_voice_tag_hook(mcpx_apu_voice_tag_fn fn) { s_mix_tag_fn = fn; }

int mcpx_apu_mix_trace_on(void)
{
    if (s_mix_on < 0) {
        const char *e = recomp_env(RENV_MIX_TRACE);
        s_mix_f = e && *e && *e != '0' ? fopen(e, "w") : NULL;
        s_mix_on = s_mix_f != NULL;
        if (e && *e && *e != '0')
            fprintf(stderr, "[APU] mix trace -> %s%s\n", e, s_mix_f ? "" : " (open failed)");
    }
    return s_mix_on;
}

void mcpx_apu_mix_trace_out(const float *acc, int nch, int n)
{
    s_mix_out_ch = (unsigned)nch;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nch && c < APU_SPK_CHANNELS; c++) {
            float x = acc[i * nch + c], a = fabsf(x);
            s_mix_out_sq[c] += (double)x * x;
            if (a > s_mix_out_peak[c]) s_mix_out_peak[c] = a;
            if (a > 1.0f) s_mix_out_over[c]++;
        }
    s_mix_out_n += (unsigned)n;
}

static void mix_trace_on_off(MCPXAPUState *d, unsigned v, int on)
{
    (void)d;
    if (!mcpx_apu_mix_trace_on() || v >= MCPX_HW_MAX_VOICES) return;
    if (!on) {
        fprintf(s_mix_f, "OFF id=%u v=%u f=%u\n", s_mix_v[v].id, v, s_mix_frames);
        return;
    }
    const uint32_t *st = s_von_stack[v];
    int n = s_von_nstack[v];
    char line[16 * 9 + 1];
    int len = 0;
    line[0] = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%08X", i ? "," : "", st[i]);
    s_mix_v[v].id = ++s_mix_serial;
    fprintf(s_mix_f, "ON id=%u v=%u f=%u stack=%s\n", s_mix_v[v].id, v, s_mix_frames, line);
}

static void mix_trace_voice(MCPXAPUState *d, unsigned v,
                            const float samples[NUM_SAMPLES_PER_FRAME][2],
                            unsigned channels, const int bin[8],
                            const uint16_t vol[8], float ea, float rate,
                            int fmode)
{
    struct MixVoice *m = &s_mix_v[v];
    static apu_spk_map map;
    static int have_map;
    float spk[APU_SPK_CHANNELS][NUM_SAMPLES_PER_FRAME];
    int six = d->monitor.channels == APU_SPK_CHANNELS;
    int nsp = six ? APU_SPK_CHANNELS : 2;

    if (!have_map) { mcpx_apu_speaker_map_get(map); have_map = 1; }
    memset(spk, 0, sizeof(spk));
    for (int b = 0; b < 8; b++) {
        float g = ea, hr;
        int sp;
        if (v < MCPX_HW_MAX_3D_VOICES && b < 4)
            hr = (float)(1 << d->vp.hrtf_headroom);
        else
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        g *= attenuate(vol[b]) / hr;
        if (six) {
            sp = map[bin[b]];
            g *= mcpx_apu_bin_feed_gain(bin[b]);
        } else {
            sp = bin[b] & 1;   /* the stereo fold: even left, odd right */
        }
        if (sp < 0 || sp >= nsp || g == 0.0f) continue;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            spk[sp][i] += g * samples[i][(unsigned)b % channels];
    }
    for (int c = 0; c < nsp; c++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float a = fabsf(spk[c][i]);
            m->spk_sq[c] += (double)spk[c][i] * spk[c][i];
            if (a > m->peak) m->peak = a;
        }
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        m->src_sq += (double)samples[i][0] * samples[i][0];
    m->frames++;
    m->rate = rate; m->ea = ea; m->fmode = (uint8_t)fmode;
    m->d3 = v < MCPX_HW_MAX_3D_VOICES;
    m->hrtf = m->d3 && voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                  NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE) != HRTF_NULL_HANDLE;
    m->fmt = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    m->ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                           NV_PAVS_VOICE_CUR_PSL_START_BA);
    m->ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                            NV_PAVS_VOICE_PAR_NEXT_EBO);
    for (int b = 0; b < 8; b++) { m->bins[b] = (uint8_t)bin[b]; m->vols[b] = vol[b]; }
    if (!(m->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE)) {
        /* A buffer's bytes, once: which wave of the title's wave
         * dictionaries it is (matched against the disc's files offline).
         * ba is an offset into the VP's scatter-gather space; the pages
         * behind it need not be contiguous, so each chunk stays in a page. */
        static uint32_t seen[4096];
        uint32_t sge = d->regs[NV_PAPU_VPSGEADDR];
        uint32_t pa = (uint32_t)get_data_ptr(sge, 0xFFFFFFFF, m->ba);
        uint32_t h = (pa * 2654435761u) >> 20, k;
        for (k = 0; k < 4096 && seen[(h + k) & 4095] && seen[(h + k) & 4095] != pa; k++) {}
        if (pa && k < 4096 && !seen[(h + k) & 4095]) {
            uint32_t o2 = 0x400;
            seen[(h + k) & 4095] = pa;
            if (((m->ba + o2) & 0xFFF) > 0x1000 - 32) o2 += 0x40;
            fprintf(s_mix_f, "B ba=%06X pa=%08X ebo=%u fmt=%08X o2=%u hex=", m->ba, pa, m->ebo, m->fmt, o2);
            for (int c = 0; c < 2; c++) {
                uint32_t a = c ? (uint32_t)get_data_ptr(sge, 0xFFFFFFFF, m->ba + o2) : pa;
                const uint8_t *p8 = (const uint8_t *)mcpx_apu_phys(a);
                int lim = 0x1000 - (int)(a & 0xFFF); if (lim > 32) lim = 32;
                for (int i = 0; p8 && i < lim; i++) fprintf(s_mix_f, "%02X", p8[i]);
                fputc(c ? '\n' : ',', s_mix_f);
            }
        }
        m->ba = pa;     /* reported as the physical address: the wave's identity */
    }
}

/* RECOMP_MIX_TRACE_REC=<file> (with RECOMP_MIX_TRACE): the mix loop's
 * inputs and its result, raw, for tests/apu_bus's replay: per voice a 'V'
 * record (voice, channels, the eight bins and gains, the 32 samples), per
 * VP frame an 'F' record (the 32 mixbins after every voice). From
 * RECOMP_MIX_TRACE_REC_START seconds (default 0) for
 * RECOMP_MIX_TRACE_REC_SECONDS (default 10). RECOMP_MIX_TRACE_REC_START=@path:
 * from the moment the file path exists (looked for once a second), for a
 * driver that knows when the race is on. */
static FILE *s_rec_f;
static int s_rec_state = -1;            /* -1 unread, 0 off, 1 waiting, 2 recording, 3 done */
static unsigned s_rec_start, s_rec_frames_left, s_rec_multipass;
static char s_rec_trigger[1024];        /* RECOMP_MIX_TRACE_REC_START=@path */

static int mix_rec_on(void)
{
    if (s_rec_state < 0) {
        const char *e = getenv("RECOMP_MIX_TRACE_REC"), *t;
        s_rec_state = 0;
        if (e && *e && mcpx_apu_mix_trace_on() && (s_rec_f = fopen(e, "wb")) != NULL) {
            t = getenv("RECOMP_MIX_TRACE_REC_START");
            s_rec_start = t && *t != '@' ? (unsigned)atoi(t) * 1500u : 0;
            if (t && *t == '@')
                snprintf(s_rec_trigger, sizeof(s_rec_trigger), "%s", t + 1);
            t = getenv("RECOMP_MIX_TRACE_REC_SECONDS");
            s_rec_frames_left = (t ? (unsigned)atoi(t) : 10u) * 1500u;
            s_rec_state = 1;
        }
    }
    return s_rec_state == 2;
}

static void mix_rec_voice(MCPXAPUState *d, unsigned v,
                          const float samples[NUM_SAMPLES_PER_FRAME][2],
                          unsigned channels, const int bin[8],
                          const uint16_t vol[8], float ea, int multipass)
{
    struct { uint8_t tag, channels; uint16_t v; int32_t bin[8]; float g[8]; } h;
    memset(&h, 0, sizeof(h));
    h.tag = 'V'; h.channels = (uint8_t)channels; h.v = (uint16_t)v;
    for (int b = 0; b < 8; b++) {
        float g = ea;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        h.bin[b] = bin[b];
        h.g[b] = g;
    }
    if (multipass) s_rec_multipass++;
    fwrite(&h, sizeof(h), 1, s_rec_f);
    fwrite(samples, sizeof(float) * 2, NUM_SAMPLES_PER_FRAME, s_rec_f);
}

static void mix_rec_frame(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (s_rec_state == 1 && s_rec_trigger[0]) {
        FILE *t = s_mix_frames % 1500 == 0 ? fopen(s_rec_trigger, "rb") : NULL;
        if (t) { fclose(t); s_rec_trigger[0] = 0; s_rec_start = 0; }
    }
    if (s_rec_state == 1 && !s_rec_trigger[0] && s_mix_frames >= s_rec_start) {
        s_rec_state = 2;                /* from the next frame's first voice */
        return;
    }
    if (s_rec_state != 2) return;
    fputc('F', s_rec_f);
    fwrite(mixbins, sizeof(float) * NUM_SAMPLES_PER_FRAME, NUM_MIXBINS, s_rec_f);
    if (--s_rec_frames_left == 0) {
        fclose(s_rec_f);
        s_rec_f = NULL;
        s_rec_state = 3;
        fprintf(stderr, "[APU] mix trace: recording done (%u multipass voice frames)\n",
                s_rec_multipass);
    }
}

static void mix_trace_report(MCPXAPUState *d)
{
    if (!mcpx_apu_mix_trace_on()) return;
    if (++s_mix_frames % 1500) return;
    s_mix_secs++;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct MixVoice *m = &s_mix_v[v];
        if (!m->frames) continue;
        fprintf(s_mix_f, "V t=%u id=%u v=%u 3d=%u st=%u stream=%u fmt=%08X fmode=%u hrtf=%u"
                " rate=%.4f ea=%.3f ba=%08X ebo=%u frames=%u src=%.1f"
                " bins=%u,%u,%u,%u,%u,%u,%u,%u vols=%03X,%03X,%03X,%03X,%03X,%03X,%03X,%03X"
                " spk=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g peak=%.5f\n",
                s_mix_secs, m->id, v, m->d3,
                (unsigned)((m->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((m->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                m->fmt, m->fmode, m->hrtf, m->rate, m->ea, m->ba, m->ebo, m->frames,
                sqrt(m->src_sq / ((double)m->frames * NUM_SAMPLES_PER_FRAME)) * 32768.0,
                m->bins[0], m->bins[1], m->bins[2], m->bins[3],
                m->bins[4], m->bins[5], m->bins[6], m->bins[7],
                m->vols[0], m->vols[1], m->vols[2], m->vols[3],
                m->vols[4], m->vols[5], m->vols[6], m->vols[7],
                m->spk_sq[0], m->spk_sq[1], m->spk_sq[2], m->spk_sq[3],
                m->spk_sq[4], m->spk_sq[5], m->peak);
        m->frames = 0; m->src_sq = 0; m->peak = 0;
        memset(m->spk_sq, 0, sizeof(m->spk_sq));
    }
    fprintf(s_mix_f, "T t=%u wall=%ld\n", s_mix_secs, (long)time(NULL));
    if (s_bus_rt) {
        /* The mixer's own meters: each bus's level into the mix and its
         * sounding voices. */
        apu_bus_meters mm;
        apu_bus_get_meters(&mm);
        fprintf(s_mix_f, "M t=%u", s_mix_secs);
        for (int b = 0; b < apu_bus_count(); b++) {
            char key[32];
            apu_bus_key(b, key, sizeof(key));
            fprintf(s_mix_f, " %s=%.1f/%.1f", key, mm.bus_rms_db[b], mm.bus_voices[b]);
        }
        fprintf(s_mix_f, " duck=%.1f lim=%.1f\n", mm.duck_db, mm.limiter_db);
    }
    fprintf(s_mix_f, "O t=%u ch=%u sq=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g"
            " peak=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f over=%u,%u,%u,%u,%u,%u n=%u"
            " hrtf_sub=%u,%u,%u,%u hrtf_hr=%u\n", s_mix_secs, s_mix_out_ch,
            s_mix_out_sq[0], s_mix_out_sq[1], s_mix_out_sq[2], s_mix_out_sq[3],
            s_mix_out_sq[4], s_mix_out_sq[5],
            s_mix_out_peak[0], s_mix_out_peak[1], s_mix_out_peak[2],
            s_mix_out_peak[3], s_mix_out_peak[4], s_mix_out_peak[5],
            s_mix_out_over[0], s_mix_out_over[1], s_mix_out_over[2],
            s_mix_out_over[3], s_mix_out_over[4], s_mix_out_over[5], s_mix_out_n,
            d->vp.hrtf_submix[0], d->vp.hrtf_submix[1], d->vp.hrtf_submix[2],
            d->vp.hrtf_submix[3], d->vp.hrtf_headroom);
    {
        /* RECOMP_MIX_TRACE_GP=<from>-<to> (hex offsets into the effects
         * image): its states as the title has them now, every 10 s. */
        static int gp = -1;
        static unsigned long g0, g1;
        if (gp < 0) {
            const char *e = getenv("RECOMP_MIX_TRACE_GP");
            gp = e && sscanf(e, "%lx-%lx", &g0, &g1) == 2 && g1 > g0;
        }
        if (gp && s_mix_secs % 10 == 0) {
            extern void mcpx_apu_mix_trace_gp(MCPXAPUState *, FILE *, unsigned, uint32_t, uint32_t);
            mcpx_apu_mix_trace_gp(d, s_mix_f, s_mix_secs, (uint32_t)g0, (uint32_t)g1);
        }
    }
    memset(s_mix_out_sq, 0, sizeof(s_mix_out_sq));
    memset(s_mix_out_peak, 0, sizeof(s_mix_out_peak));
    memset(s_mix_out_over, 0, sizeof(s_mix_out_over));
    s_mix_out_n = 0;
    fflush(s_mix_f);
}

/* A voice buffer in the GP model's export block (apu_dsp.c): 1 + its
 * mixbin, else 0. */
int mcpx_apu_gp_export_buffer(MCPXAPUState *d, uint32_t pa);
int apu_gp_wired(void);

/* What the mixer's classifier is told about voice v (apu_bus.h). */
struct bus_fill_ctx { MCPXAPUState *d; unsigned v; const int *bin; uint32_t pa, ba; };

static int bus_fill(void *ctx_, apu_bus_voice *o)
{
    struct bus_fill_ctx *c = (struct bus_fill_ctx *)ctx_;
    MCPXAPUState *d = c->d;
    uint32_t fmt = voice_get_mask(d, (uint16_t)c->v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    uint32_t ebo = voice_get_mask(d, (uint16_t)c->v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t sge = d->regs[NV_PAPU_VPSGEADDR];
    o->stream = (fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    o->stereo = (fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    o->fmt = fmt;
    for (int b = 0; b < 8; b++) o->bins[b] = (uint8_t)c->bin[b];
    /* At least this many bytes: ADPCM is the densest, 36 bytes for 64. */
    o->bytes = (ebo + 1) / 16 * 9 * (o->stereo ? 2u : 1u);
    if (!o->stream && c->pa) {
        static const uint32_t offs[2] = { 0x400, 0x440 };
        const uint8_t *at[2] = { NULL, NULL };
        if ((c->ba & 0xFFF) <= 0x1000 - 16)
            o->head = mcpx_apu_phys(c->pa);
        for (int k = 0; k < 2; k++)
            if (o->bytes >= offs[k] + 16 && ((c->ba + offs[k]) & 0xFFF) <= 0x1000 - 16)
                at[k] = mcpx_apu_phys(get_data_ptr(sge, 0xFFFFFFFF, c->ba + offs[k]));
        o->at_400 = at[0];
        o->at_440 = at[1];
    }
    o->stack = s_von_stack[c->v];
    o->nstack = s_von_nstack[c->v];
    o->gp_return = !o->stream && c->pa ? mcpx_apu_gp_export_buffer(d, c->pa) : 0;
    return 1;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

/* A looping buffer voice at the frame rate whose buffer lies in the GP
 * model's export block (apu_dsp.c): a title's return voice. */

static bool voice_plays_gp_export(MCPXAPUState *d, uint16_t v, float rate)
{
    uint32_t fmt, ba;

    if (!apu_gp_wired() || rate != 1.0f)
        return false;
    fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    if ((fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) || !(fmt & NV_PAVS_VOICE_CFG_FMT_LOOP) ||
        ((fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> 30) !=
            NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B32)
        return false;
    ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
    return mcpx_apu_gp_export_buffer(
        d, (uint32_t)get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, ba)) != 0;
}

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else if (voice_plays_gp_export(d, v, rate)) {
        /* A voice whose buffer is the block the GP model exports each
         * frame (apu_gp.h), at the frame rate: the block as it is, one
         * frame after the bins it was made from, since the voices are
         * mixed before the GP stage runs. Not through the interpolator
         * below: that keeps one source sample in hand, so the first sample
         * of every frame would come from the block before. */
        MCPXAPUVoiceFilter *f = &d->vp.filters[v];
        if (f->rs_have) {
            /* Back over what the interpolator had fetched and not played. */
            uint32_t len = 1 + voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                              NV_PAVS_VOICE_PAR_NEXT_EBO);
            uint32_t cbo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                          NV_PAVS_VOICE_PAR_OFFSET_CBO);
            uint32_t held = f->rs_pos >= 1.0f ? 1 : 2;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, (cbo + len - held % len) % len);
            f->rs_have = 0;
        }
        for (int sample_count = 0, tries = 0;
             sample_count < NUM_SAMPLES_PER_FRAME && tries < 4; tries++) {
            int count = voice_get_samples(d, v, &samples[sample_count],
                                          NUM_SAMPLES_PER_FRAME - sample_count);
            if (count < 0) break;
            sample_count += count;
        }
    } else {
        int empty = 0;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    s_vstat[v].ea = ea_value;
    s_vstat[v].rate = rate;
    s_vstat[v].vol0 = vol[0]; s_vstat[v].vol1 = vol[1];
    s_vstat[v].bin0 = (uint8_t)bin[0]; s_vstat[v].bin1 = (uint8_t)bin[1];
    for (int i = 0; i < 8; i++) {
        s_vstat[v].bins[i] = (uint8_t)bin[i];
        s_vstat[v].vols[i] = vol[i];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            /* apu_lowpass.h: fc_f is its tuning coefficient and q_f its
             * damping, as they are. */
            ApuLowpass *lp = &d->vp.filters[v].lowpass[ch];
            ApuLowpassCoef lp_coef = apu_lowpass_coef(fc_f, q_f);
            apu_lowpass_tidy(lp);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = apu_lowpass_step(lp, &lp_coef, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices */
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    if (mcpx_apu_mix_trace_on()) {
        mix_trace_voice(d, v, (const float (*)[2])samples, channels, bin, vol,
                        ea_value, rate, fmode);
        if (mix_rec_on())
            mix_rec_voice(d, v, (const float (*)[2])samples, channels, bin, vol,
                          ea_value, multipass);
    }

    /* The custom soundtrack's leveller (apu_level.h): the song's samples
     * scaled before the title's sends and the mixer's buses see them. Not
     * a thing for any other voice, and not run at all when set to off. */
    if (apu_level_active() && voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                            NV_PAVS_VOICE_CFG_FMT_DATA_TYPE)) {
        struct bus_fill_ctx lc = { d, v, bin, 0, 0 };
        apu_level_voice(v, s_von_serial[v], bus_fill, &lc, samples, channels);
    }

    /* Mix into bins */
    if (s_bus_rt) {
        /* The host mixer on (apu_bus.h): the same gains, summed by the
         * voice's bus. */
        float gs[8];
        struct bus_fill_ctx fc = { d, v, bin, 0, 0 };
        bool is_stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
        int bus;
        for (int b = 0; b < 8; b++) {
            float g = ea_value;
            float hr;
            if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
                hr = (float)(1 << d->vp.hrtf_headroom);
            } else {
                hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            }
            g *= attenuate(vol[b]) / hr;
            gs[b] = g;
        }
        if (!is_stream) {
            fc.ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                                   NV_PAVS_VOICE_CUR_PSL_START_BA);
            fc.pa = (uint32_t)get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, fc.ba);
        }
        bus = apu_bus_voice_bus(v, s_von_serial[v], fc.pa, bus_fill, &fc);
        if (s_bus_audition) {
            /* An audition clip loops (apu_bus.h): the live voices run on,
             * unheard, and the clip's are mixed in their place. */
        } else if (bus == APU_BUS_BYPASS) {
            /* The input of an in-place GP effect: as with the mixer off. */
            for (int b = 0; b < 8; b++)
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    mixbins[bin[b]][i] += gs[b] * samples[i][(unsigned)b % channels];
        } else
        apu_bus_mix_voice(s_bus_rt, bus, mixbins, (const float (*)[2])samples,
                          channels, bin, gs);
    } else
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[bin[b]][i] += g * samples[i][b % channels];
        }
    }

    /* RECOMP_ORACLE_CAP (apu_oracle_cap.c): a voice playing 32-bit
     * containers (DirectSound's buffers over DSP memory): where its buffer
     * is and the gain it has into each of its bins this frame. */
    if (apu_ocap_on()) {
        uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
        if (((fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> 30) ==
                NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B32 &&
            !(fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE)) {
            uint32_t w[5 + 8];
            float gain[8];
            uint32_t ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                                         NV_PAVS_VOICE_CUR_PSL_START_BA);
            w[0] = v;
            w[1] = fmt;
            w[2] = (uint32_t)get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, ba);
            w[3] = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO);
            w[4] = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
            for (int b = 0; b < 8; b++) {
                float hr;
                if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4))
                    hr = (float)(1 << d->vp.hrtf_headroom);
                else
                    hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
                w[5 + b] = (uint32_t)bin[b] | ((uint32_t)vol[b] << 8) |
                           ((uint32_t)(lpf ? 1 : 0) << 24);
                gain[b] = ea_value * attenuate(vol[b]) / hr;
            }
            apu_ocap_voice(w, sizeof(w), gain, sizeof(gain));
        }
    }

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* RECOMP_APU_TRACE: every 60 VP frames, each active voice's play offset and
 * end offset, in samples (CBO and EBO as the VP holds them). This is the
 * audio clock Sofdec follows; the drift measurement compares how far these
 * advance with the flip count. */
static void vp_trace_positions(MCPXAPUState *d)
{
    static int trace = -1;
    static unsigned frames;

    if (trace < 0)
        trace = recomp_env(RENV_APU_TRACE) != NULL;
    if (!trace || ++frames % 60)
        return;

    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                fprintf(stderr, "[APU] voice %u pos=%u/%u\n", (unsigned)v,
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                       NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                       NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

/* The idle-voice trap of a VP frame, without the frame.
 *
 * The hardware VP walks its voice lists every 32-sample frame (0.67 ms) and
 * traps the front end on the first voice it finds off (SE2FE_IDLE_VOICE),
 * which is how DirectSound learns that a VOICE_OFF took effect. The frame
 * thread here renders a whole 256-sample EP period at once and then waits
 * for real time to catch up, so a voice turned off during that wait was only
 * reported at the next period, up to 5.33 ms later -- per voice, since one
 * trap is taken at a time. DirectSound's StopEx-and-wait spins on Sleep(0)
 * until then: Burnout 3 stopping two crash sounds held guest-main ~10 ms, a
 * late frame on the Deck (BO-78). The frame thread's pacing wait calls this
 * when a voice went off, so the trap comes as soon as on hardware; the voice
 * is already silent, so the audio is the same. Returns 1 when it trapped, 0
 * when nothing went off or no listed voice is idle, -1 when the front end
 * is already trapped (tried again on the next call). */
int mcpx_apu_vp_idle_scan(MCPXAPUState *d)
{
    if (!s_idle_pending)
        return 0;
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
            == NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
        return -1;
    /* Once per VOICE_OFF: a voice the service routine leaves in the list
     * until its DPC must not trap again and again here (se_frame keeps
     * finding it at the frame rate, as the hardware would). */
    s_idle_pending = 0;
    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            voice_off_sticky(d, v);
            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                fe_method(d, SE2FE_IDLE_VOICE, v);
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                        == NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    return 1;
                return 0;   /* the title does not trap idle voices */
            }
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
    return 0;
}

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    /* The host mixer: off (the default) this is one branch. */
    if (apu_bus_enabled()) {
        static int8_t map[32];
        static int have_map;
        if (!have_map) { mcpx_apu_speaker_map_get(map); have_map = 1; }
        s_bus_rt = apu_bus_frame_begin(d->monitor.channels, map);
        s_bus_audition = s_bus_rt && apu_bus_audition_playing();
    } else {
        s_bus_rt = NULL;
        s_bus_audition = 0;
    }

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            voice_off_sticky(d, v);
            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                /* One trap at a time: once the front end is trapped, a second
                 * idle voice in the same frame would overwrite FEDECPARAM
                 * before the ISR has read the first. The rest trap on later
                 * frames, after the ISR has resumed the front end. */
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                        != NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* Process voice directly (single-threaded) */
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            d->regs[current] = d->regs[next];
        }
    }

    if (s_bus_rt) {
        if (s_bus_audition)
            apu_bus_audition_mix(s_bus_rt, mixbins);
        apu_bus_frame_end(s_bus_rt, mixbins);
    }
    if (s_mix_on > 0 && s_rec_state > 0)
        mix_rec_frame((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins);

    vp_trace_positions(d);
    vp_stat_report();
    mix_trace_report(d);
    {
        static unsigned ring_frames, ring_secs;
        if (s_ring_on > 0 && ++ring_frames % 1500 == 0)
            vp_ring_report(++ring_secs);
    }

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}

/* Shared by test_main.c and replay.c: the mix loop of apu_vp.c as it is
 * with the mixer off (voice_process, "Mix into bins"), and the DSP stage's
 * output for a set of mixbins, stereo or 5.1, through the real
 * mcpx_apu_dsp_frame. */
#include "apu_state.h"
#include "apu.h"
#include "apu_bus.h"
#include "apu_limit.h"
#include "apu_speakers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

typedef struct {
    unsigned channels;
    int bin[8];
    float g[8];
    float samples[NUM_SAMPLES_PER_FRAME][2];
} tvoice;

static void old_mix(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], const tvoice *v)
{
    for (int b = 0; b < 8; b++) {
        float g = v->g[b];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[v->bin[b]][i] += g * v->samples[i][b % v->channels];
        }
    }
}

/* A title's GP image wired as Burnout 3's is (its gp_image.c), so the DSP
 * stage is the one a title really runs: the speaker bins, eleven IIR2 sends
 * (five of them inverted, the LFE one at a half), a distortion on bins 17
 * and 18, which reach no speaker, and the export of bins 11..30. The state
 * words are the title's in a race. k_map is the routing that gives, for
 * the checks that ask where a bin goes. */
#include "apu_gp.h"

static const int8_t k_map[32] = {
    0, 1, 2, 3, 4, 5, 0, 1, 4, 5, -1, -1, -1, 1, 4, 5, 2, -1, -1,
    0, 1, 4, 5, 2, 3, 0, -1, -1, -1, -1, -1, -1,
};

static MCPXAPUState *s_d;
static uint32_t s_gp_state[0x400];
static uint32_t gp_rd(void *c, uint32_t off) { (void)c; return off / 4 < 0x400 ? s_gp_state[off / 4] : 0; }

static void gp_wire(void)
{
    static const struct { int bin, spk; unsigned st; uint32_t b0; } eq[] = {
        { 25, 0, 0x083, 0x800000 }, { 13, 1, 0x0AA, 0x800000 }, { 14, 4, 0x0D1, 0x800000 },
        { 15, 5, 0x0F8, 0x800000 }, { 16, 2, 0x11F, 0x800000 }, { 19, 0, 0x146, 0x7FFFFF },
        { 20, 1, 0x16D, 0x7FFFFF }, { 21, 4, 0x194, 0x7FFFFF }, { 22, 5, 0x1BB, 0x7FFFFF },
        { 23, 2, 0x1E2, 0x7FFFFF }, { 24, 3, 0x209, 0x400000 },
    };
    static const uint32_t drive[9] = { 0x7FFFFF, 0x1FFFFF, 0x0147AE, 0x7FFFFF, 0x3FFFFF, 0, 0x1FFFFF, 0, 0x7FFFFF };
    apu_gp_mem m = { gp_rd, NULL, NULL };

    apu_gp_reset();
    for (int b = 0; b < 6; b++) apu_gp_direct(b, b, 1.0f);
    apu_gp_direct(6, 0, 1.0f); apu_gp_direct(7, 1, 1.0f);
    apu_gp_direct(8, 4, 1.0f); apu_gp_direct(9, 5, 1.0f);
    for (unsigned k = 0; k < sizeof eq / sizeof eq[0]; k++) {
        apu_gp_eq(eq[k].bin, eq[k].spk, eq[k].st);
        s_gp_state[eq[k].st + 7] = eq[k].b0;
    }
    apu_gp_drive(17, 0x02E);
    apu_gp_drive(18, 0x045);
    for (int k = 0; k < 9; k++) {
        s_gp_state[0x02E + 7 + k] = drive[k];
        s_gp_state[0x045 + 7 + k] = drive[k];
    }
    apu_gp_export(11, 20, 0xB000);
    apu_gp_lfe_lowpass(120);
    apu_gp_command(&m, 3, 0, 0, 0x400);
}

static void dsp_setup(void)
{
    s_d = (MCPXAPUState *)calloc(1, sizeof(*s_d));
    gp_wire();
}

/* One frame of mixbins to s16: out[32][6] (5.1) or out[32][2]. The DSP
 * stage may write into the bins, so it gets a copy. */
static void dsp_out_lim(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], int six, int16_t *out,
                        int limiter)
{
    static float copy[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memcpy(copy, mixbins, sizeof(copy));
    /* The GP model has running state (the distortion's filter, the LFE
     * low-pass): every frame starts from the same one, so two sets of bins
     * can be compared by their output. */
    gp_wire();
    /* The safety limiter (apu_limit.h) has memory too: every frame starts
     * from its idle state, or off (the clamp alone: today's output). */
    apu_limit_reset();
    apu_limit_enable(limiter);
    s_d->monitor.channels = six ? 6 : 2;
    s_d->ep_frame_div = 0;
    mcpx_apu_dsp_frame(s_d, copy);
    if (six) memcpy(out, s_d->monitor.frame_buf6, sizeof(int16_t) * 6 * NUM_SAMPLES_PER_FRAME);
    else     memcpy(out, s_d->monitor.frame_buf, sizeof(int16_t) * 2 * NUM_SAMPLES_PER_FRAME);
    apu_limit_enable(1);
}

static void dsp_out(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], int six, int16_t *out)
{
    dsp_out_lim(mixbins, six, out, 1);
}

/* The safety limiter's ceiling as a 16-bit value (-0.5 dBFS). */
#define LIM_CEIL16 30934


static const char *const k_names[13] = {
    "Music", "DJ", "Film", "Streams", "Player engine", "Player exhaust", "Other cars",
    "Traffic", "Tyres/road/wind", "Impacts/crash", "Boost", "UI/HUD", "Other",
};

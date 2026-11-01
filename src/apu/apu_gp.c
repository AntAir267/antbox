/*
 * apu_gp.c -- a title's GP effects image, modelled (apu_gp.h).
 *
 * Written from the title's own image as it sits in GP scratch memory
 * (plain DSP56300 code; Motorola's DSP56300 Family Manual for the
 * instruction set) and from measured input/output behaviour of the GP
 * stage. No emulator's code.
 */
#include "apu_gp.h"
#include "apu_dsp56k.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { GP_NONE = 0, GP_DIRECT, GP_EQ };

static struct {
    int wired;
    struct { uint8_t kind; int8_t spk; float gain; uint32_t st;
             float x1, x2, y1, y2; } route[APU_GP_BINS];
    struct { int bin; uint32_t st; } drive[4];
    int ndrive;
    struct { const char *what; uint32_t st; int bin, count; int32_t quiet, max; } un;
    int exp_first, exp_count;
    uint32_t exp_off;
    /* The LFE low-pass: the one running, and during a change the one it
     * is fading to. */
    struct gp_lfe { int hz; double c[2][5], z[2][2]; } lfe, lfe_to;
    int lfe_fade;                       /* frames of the fade left */
    int ran;                            /* a frame has run */
    /* A program of the image run as it is (apu_gp_program): its code in
     * GP scratch, copied when the image's state is first loaded. */
    struct { uint32_t off, words; int have, dead; uint32_t code[APU_GP_CODE_MAX]; } prog;
    const apu_gp_mem *mem;              /* of the frame or command running */
    /* The state words as the guest's X window last had them (xwin_in /
     * xwin_out), and how many there are (the title's first load). */
    int32_t xsh[APU_GP_STATE_MAX];
    uint32_t nstate;
    /* The GP's X memory. The effects' state words (signed 24-bit, loaded
     * from GP scratch by the title's commands) are X:0x80 and up; the
     * mixbins the programs read are at X:0x1400, the speaker sums they add
     * to at X:0xC00. */
    dsp56k cpu;
} g;

#define ST (&g.cpu.x[0x80])
#define X_MIXBUF 0x1400
#define X_SPEAKERS 0xC00

void apu_gp_reset(void)
{
    memset(&g, 0, sizeof(g));
    for (int b = 0; b < APU_GP_BINS; b++)
        g.route[b].spk = -1;
}

int apu_gp_wired(void) { return g.wired; }

static void wire(void)
{
    if (!g.wired) {
        apu_gp_reset();
        g.wired = 1;
    }
}

void apu_gp_direct(int bin, int speaker, float gain)
{
    wire();
    if (bin < 0 || bin >= APU_GP_BINS || speaker < 0 || speaker >= APU_GP_SPEAKERS)
        return;
    g.route[bin].kind = GP_DIRECT;
    g.route[bin].spk = (int8_t)speaker;
    g.route[bin].gain = gain;
}

void apu_gp_eq(int bin, int speaker, uint32_t state)
{
    wire();
    if (bin < 0 || bin >= APU_GP_BINS || speaker < 0 || speaker >= APU_GP_SPEAKERS ||
        state + 12 > APU_GP_STATE_MAX)
        return;
    g.route[bin].kind = GP_EQ;
    g.route[bin].spk = (int8_t)speaker;
    g.route[bin].st = state;
}

void apu_gp_drive(int bin, uint32_t state)
{
    wire();
    if (bin < 0 || bin >= APU_GP_BINS || state + 23 > APU_GP_STATE_MAX ||
        g.ndrive >= (int)(sizeof(g.drive) / sizeof(g.drive[0])))
        return;
    g.drive[g.ndrive].bin = bin;
    g.drive[g.ndrive].st = state;
    g.ndrive++;
}

void apu_gp_unmodelled(const char *what, int bin, uint32_t state, int count, int32_t quiet)
{
    wire();
    if (bin < 0 || bin >= APU_GP_BINS || count < 0 ||
        state + (uint32_t)count > APU_GP_STATE_MAX)
        return;
    g.un.what = what;
    g.un.bin = bin;
    g.un.st = state;
    g.un.count = count;
    g.un.quiet = quiet;
    g.un.max = 0;
}

int32_t apu_gp_unmodelled_max(void) { return g.un.max; }

void apu_gp_program(uint32_t code_scratch_off, uint32_t code_words)
{
    wire();
    if (code_words > APU_GP_CODE_MAX)
        return;
    g.prog.off = code_scratch_off;
    g.prog.words = code_words;
    g.prog.have = 0;
    g.prog.dead = 0;
}

unsigned long apu_gp_program_steps(void) { return g.cpu.steps; }

static uint32_t prog_mem_read(void *ctx, uint32_t off)
{
    (void)ctx;
    return g.mem && g.mem->read ? g.mem->read(g.mem->ctx, off) : 0;
}

static void prog_mem_write(void *ctx, uint32_t off, const uint32_t *words, int n)
{
    (void)ctx;
    if (g.mem && g.mem->write)
        g.mem->write(g.mem->ctx, off, words, n);
}

void apu_gp_export(int first, int count, uint32_t scratch_off)
{
    wire();
    if (first < 0 || count < 0 || first + count > APU_GP_BINS)
        return;
    g.exp_first = first;
    g.exp_count = count;
    g.exp_off = scratch_off;
}

int apu_gp_export_range(uint32_t *scratch_off, uint32_t *bytes, int *first_bin)
{
    if (!g.wired || !g.exp_count)
        return 0;
    if (scratch_off) *scratch_off = g.exp_off;
    if (bytes) *bytes = (uint32_t)g.exp_count * APU_GP_FRAME * 4u;
    if (first_bin) *first_bin = g.exp_first;
    return 1;
}

/* 4th order Butterworth low-pass as two biquads (Q 0.5412, 1.3066), the
 * bilinear transform with the cutoff pre-warped. */
static void lfe_design(struct gp_lfe *l, int hz)
{
    static const double q[2] = { 0.54119610014619698, 1.3065629648763766 };
    memset(l, 0, sizeof(*l));
    l->hz = hz > 0 ? hz : 0;
    for (int s = 0; s < 2 && l->hz; s++) {
        double w = 2.0 * 3.14159265358979323846 * l->hz / 48000.0;
        double al = sin(w) / (2.0 * q[s]), cs = cos(w), a0 = 1.0 + al;
        l->c[s][0] = (1.0 - cs) / 2.0 / a0;
        l->c[s][1] = (1.0 - cs) / a0;
        l->c[s][2] = (1.0 - cs) / 2.0 / a0;
        l->c[s][3] = -2.0 * cs / a0;
        l->c[s][4] = (1.0 - al) / a0;
    }
}

static void lfe_run(struct gp_lfe *l, const float *in, float *out)
{
    double x[APU_GP_FRAME];
    for (int i = 0; i < APU_GP_FRAME; i++) x[i] = in[i];
    for (int s = 0; s < 2 && l->hz; s++) {
        const double *c = l->c[s];
        double z1 = l->z[s][0], z2 = l->z[s][1];
        for (int i = 0; i < APU_GP_FRAME; i++) {
            double v = x[i], y = c[0] * v + z1;
            z1 = c[1] * v - c[3] * y + z2;
            z2 = c[2] * v - c[4] * y;
            x[i] = y;
        }
        if (fabs(z1) < 1e-30) z1 = 0.0;
        if (fabs(z2) < 1e-30) z2 = 0.0;
        l->z[s][0] = z1; l->z[s][1] = z2;
    }
    for (int i = 0; i < APU_GP_FRAME; i++) out[i] = (float)x[i];
}

#define LFE_FADE_FRAMES 32              /* 21 ms */

void apu_gp_lfe_lowpass(int hz)
{
    wire();
    if (hz < 0) hz = 0;
    if (!g.ran) {                       /* wiring: from the first frame */
        lfe_design(&g.lfe, hz);
        g.lfe_fade = 0;
        return;
    }
    /* While it plays: the new filter starts from rest beside the old one
     * and the feed fades across, so nothing steps. A change during a fade
     * waits for it (apu_gp_frame). */
    if (g.lfe_fade || hz == g.lfe.hz)
        return;
    lfe_design(&g.lfe_to, hz);
    g.lfe_fade = LFE_FADE_FRAMES;
}

int apu_gp_lfe_lowpass_hz(void)
{
    return g.lfe_fade ? g.lfe_to.hz : g.lfe.hz;
}

static float s24f(int32_t v) { return (float)v / 8388608.0f; }

int apu_gp_bin_speaker(int bin)
{
    if (!g.wired || bin < 0 || bin >= APU_GP_BINS || g.route[bin].kind == GP_NONE)
        return -1;
    return g.route[bin].spk;
}

float apu_gp_bin_gain(int bin)
{
    if (!g.wired || bin < 0 || bin >= APU_GP_BINS)
        return 0.0f;
    if (g.route[bin].kind == GP_DIRECT)
        return g.route[bin].gain;
    if (g.route[bin].kind == GP_EQ)
        return s24f(ST[g.route[bin].st + 7]);
    return 0.0f;
}

static int32_t sx24(uint32_t v)
{
    return (int32_t)(v << 8) >> 8;
}

/* The guest's X window and the model's state words (apu_gp_mem.xwin). */
static void xwin_in(const apu_gp_mem *m)
{
    if (!m || !m->xwin)
        return;
    for (uint32_t i = 0; i < g.nstate; i++) {
        int32_t v = sx24(m->xwin[0x80 + i]);
        if (v != g.xsh[i]) {
            ST[i] = v;
            g.xsh[i] = v;
        }
    }
}

static void xwin_out(const apu_gp_mem *m)
{
    if (!m || !m->xwin)
        return;
    for (uint32_t i = 0; i < g.nstate; i++)
        if (ST[i] != g.xsh[i]) {
            m->xwin[0x80 + i] = (uint32_t)ST[i] & 0xFFFFFFu;
            g.xsh[i] = ST[i];
        }
}

void apu_gp_command(const apu_gp_mem *m, uint32_t cmd, uint32_t dst,
                    uint32_t src, uint32_t count)
{
    if (!g.wired || !m || !m->read)
        return;
    if (cmd == 3 && g.prog.words && !g.prog.have) {
        /* The image is in place: take the program's code. */
        for (uint32_t k = 0; k < g.prog.words; k++)
            g.prog.code[k] = m->read(m->ctx, g.prog.off + 4 * k) & 0xFFFFFF;
        g.cpu.p = g.prog.code;
        g.cpu.pn = g.prog.words;
        g.cpu.mem_read = prog_mem_read;
        g.cpu.mem_write = prog_mem_write;
        g.prog.have = 1;
    }
    if (cmd == 3)
        dst = 0;
    else if (cmd != 2)
        return;
    if (dst >= APU_GP_STATE_MAX)
        return;
    if (count > APU_GP_STATE_MAX - dst)
        count = APU_GP_STATE_MAX - dst;
    xwin_in(m);
    for (uint32_t k = 0; k < count; k++)
        ST[dst + k] = sx24(m->read(m->ctx, src + 4 * k));
    if (dst + count > g.nstate)
        g.nstate = dst + count;
    xwin_out(m);
}

/* ── the distortion ──────────────────────────────────────────────────────
 *
 * The image's 133-word effect, instruction for instruction. State words
 * (s24 fractions): +7 f, +8 q, +9 wet, +10 dry, +11 drive, +13 low gain,
 * +15 out; running: +0 high, +17 low, +18 band. Per sample x:
 *
 *     low  = sat(low + f * band)
 *     v    = 8 * (8 * drive * x + x)              56-bit, no saturation
 *     w    = fold(v)                              see fold()
 *     high = sat(w - low - 4 * q * band)
 *     band = sat(band + f * high)
 *     y    = out * sat(wet * sat(8 * lowgain * low) + 8 * dry * x)
 *
 * a Chamberlin state variable filter after a wave folder, under the dry
 * signal amplified eight times and clipped. The accumulators are the
 * DSP's: 56 bits, 8 of extension over a 48-bit fraction; a multiply is
 * (a * b) << 1 with no rounding; reading an accumulator into a 24-bit
 * register or memory limits it to full scale when the extension is in
 * use and truncates otherwise. */
static inline int64_t w56(int64_t a)
{
    return (int64_t)((uint64_t)a << 8) >> 8;
}

static inline int32_t lim24(int64_t a)
{
    int64_t hi = a >> 47;
    if (hi > 0) return 0x7FFFFF;
    if (hi < -1) return -0x800000;
    return (int32_t)(a >> 24);
}

static inline int64_t mpy(int32_t a, int32_t b)
{
    return w56((int64_t)a * b * 2);
}

static inline int64_t ld(int32_t x)        /* a register into an accumulator */
{
    return (int64_t)x * 16777216;
}

static inline int64_t setext(int64_t a, int64_t e)
{
    return w56((int64_t)(((uint64_t)a & 0xFFFFFFFFFFFFull) | ((uint64_t)e << 48)));
}

/* The fold: the image looks at the sign and the two low bits of the
 * extension (the value in units of 2.0) and mirrors the signal back into
 * range: up to 1 it passes, 1..3 is held at full scale, 3..5 comes back
 * down through zero to minus full scale, and so on, period 8. The two
 * signs are mirror images except for the last quarter of the positive
 * side (6..8), where the image subtracts 1/128 instead of 2: kept as it
 * is. */
static int32_t fold(int64_t a)
{
    unsigned e = (unsigned)(a >> 48) & 0xFF;
    int64_t b;

    if (e & 0x80) {
        a = setext(a, 0xFF);
        if (e & 2) {
            if (e & 1)
                return lim24(a);
            b = w56(setext(0, 0xFF) - a);
            return lim24(setext(b, 0xFF));
        }
        if (e & 1)
            return lim24(w56(-a));
        return lim24(w56(a + setext(0, 1)));
    }
    a = setext(a, 0);
    if (e & 2) {
        if (e & 1)
            return lim24(w56(a - ld(0x010000)));
        return lim24(w56(-a));
    }
    if (e & 1) {
        b = w56(setext(0, 1) - a);
        return lim24(setext(b, 0));
    }
    return lim24(a);
}

void apu_gp_drive_run(int32_t *st, int32_t *x, int n)
{
    const int32_t f = st[7], q = st[8], wet = st[9], dry = st[10];
    const int32_t drv = st[11], lg = st[13], og = st[15];
    int32_t hi = st[0], low = st[17], band = st[18];

    for (int i = 0; i < n; i++) {
        const int32_t xi = x[i];
        int64_t a;
        int32_t w, y0;

        low = lim24(w56(mpy(f, band) + ld(low)));
        a = w56(mpy(xi, drv) * 8);
        a = w56(a + ld(xi));
        a = w56(a * 8);
        w = fold(a);
        hi = lim24(w56(ld(w) - ld(low) - w56(mpy(band, q) * 4)));
        band = lim24(w56(ld(band) + mpy(hi, f)));
        y0 = lim24(w56(mpy(low, lg) * 8));
        a = w56(mpy(y0, wet) + w56(mpy(dry, xi) * 8));
        x[i] = lim24(mpy(lim24(a), og));
    }
    st[0] = hi;
    st[17] = low;
    st[18] = band;
}

static inline int32_t f_to_s24(float v)
{
    double s = (double)v * 8388608.0;
    if (s >= 8388607.0) return 0x7FFFFF;
    if (s <= -8388608.0) return -0x800000;
    return (int32_t)lrint(s);
}

void apu_gp_frame(const apu_gp_mem *m, float bins[APU_GP_BINS][APU_GP_FRAME],
                  float out[APU_GP_SPEAKERS][APU_GP_FRAME])
{
    static const float k_max = 8388607.0f / 8388608.0f;

    memset(out, 0, sizeof(float) * APU_GP_SPEAKERS * APU_GP_FRAME);
    xwin_in(m);

    /* The GP's words are 24 bits: a bin past full scale arrives limited. */
    for (int b = 0; b < APU_GP_BINS; b++)
        for (int i = 0; i < APU_GP_FRAME; i++) {
            float x = bins[b][i];
            if (x > k_max) bins[b][i] = k_max;
            else if (x < -1.0f) bins[b][i] = -1.0f;
        }

    /* An effect that is not modelled, fed while the title has it open. */
    if (g.un.count) {
        const float *x = bins[g.un.bin];
        int fed = 0;
        for (int i = 0; i < APU_GP_FRAME && !fed; i++)
            fed = x[i] != 0.0f;
        if (fed) {
            int32_t mx = 0;
            for (int k = 0; k < g.un.count; k++) {
                int32_t v = ST[g.un.st + (uint32_t)k];
                if (v < 0) v = -v;
                if (v > mx) mx = v;
            }
            if (mx > g.un.max) {
                g.un.max = mx;
                if (mx > g.un.quiet)
                    fprintf(stderr, "[APU] GP image: %s is not modelled; the title feeds it"
                                    " with its level at 0x%06X (quiet up to 0x%06X)\n",
                            g.un.what ? g.un.what : "an effect", (unsigned)mx,
                            (unsigned)g.un.quiet);
            }
        }
    }

    /* The in-place effects. */
    for (int k = 0; k < g.ndrive; k++) {
        float *x = bins[g.drive[k].bin];
        int32_t w[APU_GP_FRAME];
        for (int i = 0; i < APU_GP_FRAME; i++)
            w[i] = f_to_s24(x[i]);
        apu_gp_drive_run(&ST[g.drive[k].st], w, APU_GP_FRAME);
        for (int i = 0; i < APU_GP_FRAME; i++)
            x[i] = s24f(w[i]);
    }

    /* The program run as the image has it: every bin where it reads its
     * input, the speaker sums cleared for what it adds, and afterwards the
     * bins it changed in place and its sums. */
    if (g.prog.have && !g.prog.dead) {
        int32_t *mb = &g.cpu.x[X_MIXBUF], *sp = &g.cpu.x[X_SPEAKERS];
        for (int b = 6; b < APU_GP_BINS; b++)
            for (int i = 0; i < APU_GP_FRAME; i++)
                mb[b * APU_GP_FRAME + i] = f_to_s24(bins[b][i]);
        memset(sp, 0, sizeof(int32_t) * APU_GP_SPEAKERS * APU_GP_FRAME);
        g.mem = m;
        if (dsp56k_run(&g.cpu, 0, g.prog.words - 1, 200000)) {
            g.prog.dead = 1;
            fprintf(stderr, "[APU] GP image: the program at scratch 0x%X stopped at word"
                            " 0x%X (%06X); it is left out from here on\n",
                    g.prog.off, g.cpu.fault_pc, g.cpu.fault_word);
        } else {
            for (int b = 6; b < APU_GP_BINS; b++)
                for (int i = 0; i < APU_GP_FRAME; i++) {
                    int32_t v = mb[b * APU_GP_FRAME + i];
                    if (v != f_to_s24(bins[b][i]))
                        bins[b][i] = s24f(v);
                }
            for (int c = 0; c < APU_GP_SPEAKERS; c++)
                for (int i = 0; i < APU_GP_FRAME; i++)
                    out[c][i] += s24f(sp[c * APU_GP_FRAME + i]);
        }
        g.mem = NULL;
    }

    /* The export, after them. */
    if (g.exp_count && m && m->write) {
        uint32_t w[APU_GP_FRAME];
        for (int k = 0; k < g.exp_count; k++) {
            const float *x = bins[g.exp_first + k];
            for (int i = 0; i < APU_GP_FRAME; i++)
                w[i] = (uint32_t)f_to_s24(x[i]) & 0xFFFFFFu;
            m->write(m->ctx, g.exp_off + (uint32_t)k * APU_GP_FRAME * 4u, w, APU_GP_FRAME);
        }
    }

    /* The speaker sums. */
    for (int b = 0; b < APU_GP_BINS; b++) {
        const float *x = bins[b];
        float *dst;
        if (g.route[b].kind == GP_NONE)
            continue;
        dst = out[g.route[b].spk];
        if (g.route[b].kind == GP_DIRECT) {
            const float gn = g.route[b].gain;
            if (gn == 1.0f)
                for (int i = 0; i < APU_GP_FRAME; i++) dst[i] += x[i];
            else
                for (int i = 0; i < APU_GP_FRAME; i++) dst[i] += gn * x[i];
        } else {
            /* The IIR2 effect:
             *     y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2]
             *            + 2 (a1 y[n-1] + a2 y[n-2])
             * b0 is a plain fraction: 0x7FFFFF passes the bin, 0x800000
             * inverts it, and the image's own default set (0x400000
             * 0xAD7746 0x23434C 0x5288B9 0xDCBCB3) is a flat one half,
             * its numerator being half its denominator. */
            const int32_t *c = &ST[g.route[b].st + 7];
            const float b0 = s24f(c[0]), b1 = s24f(c[1]), b2 = s24f(c[2]);
            const float a1 = 2.0f * s24f(c[3]), a2 = 2.0f * s24f(c[4]);
            if (c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0) {
                if (c[0] != 0)
                    for (int i = 0; i < APU_GP_FRAME; i++) dst[i] += b0 * x[i];
                g.route[b].x1 = x[APU_GP_FRAME - 1];
                g.route[b].x2 = x[APU_GP_FRAME - 2];
                g.route[b].y1 = b0 * g.route[b].x1;
                g.route[b].y2 = b0 * g.route[b].x2;
            } else if (a2 > -1.0f && a2 < 1.0f && fabsf(a1) < 1.0f - a2) {
                float x1 = g.route[b].x1, x2 = g.route[b].x2;
                float y1 = g.route[b].y1, y2 = g.route[b].y2;
                for (int i = 0; i < APU_GP_FRAME; i++) {
                    float y = b0 * x[i] + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;
                    x2 = x1; x1 = x[i];
                    y2 = y1; y1 = y;
                    dst[i] += y;
                }
                if (fabsf(y1) < 1e-20f) y1 = 0.0f;
                if (fabsf(y2) < 1e-20f) y2 = 0.0f;
                g.route[b].x1 = x1; g.route[b].x2 = x2;
                g.route[b].y1 = y1; g.route[b].y2 = y2;
            }
            /* else: poles outside the circle, nothing a fixed-point DSP
             * would survive either; the send is dropped. */
        }
    }

    /* Without a way to write GP scratch (a capture is being taken) the
     * model leaves the guest's memory alone during frames. */
    if (m && m->write)
        xwin_out(m);

    if (g.lfe_fade) {
        float a[APU_GP_FRAME], b[APU_GP_FRAME];
        const float t0 = (float)(LFE_FADE_FRAMES - g.lfe_fade) / LFE_FADE_FRAMES;
        lfe_run(&g.lfe, out[3], a);
        lfe_run(&g.lfe_to, out[3], b);
        for (int i = 0; i < APU_GP_FRAME; i++) {
            float t = t0 + (float)i / (APU_GP_FRAME * LFE_FADE_FRAMES);
            out[3][i] = a[i] + t * (b[i] - a[i]);
        }
        if (--g.lfe_fade == 0)
            g.lfe = g.lfe_to;
    } else if (g.lfe.hz) {
        lfe_run(&g.lfe, out[3], out[3]);
    }
    g.ran = 1;
}

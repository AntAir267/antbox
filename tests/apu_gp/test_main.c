/* apu_gp: the model of a title's GP effects image (apu_gp.h, BO-119).
 *
 *   - routing: a wired bin reaches its speaker with its gain and sign, an
 *     unwired one reaches nothing; opposite-sign sends cancel
 *   - the IIR2 send: b0 a plain fraction (+1, -1), the image's default
 *     coefficient set a flat one half, a real biquad when the title sets one
 *   - effect states move with the title's commands and not before
 *   - the export: the bins after the in-place effects, 24-bit words, at the
 *     wired scratch offset, in the frame they were made; a voice reading it
 *     in the next frame is one frame late
 *   - bins are limited to 24 bits on the way in
 *   - the distortion: x8 and a hard clip with the wet path shut, silence
 *     with its parameters at zero, the fold's shape, and impulse responses
 *     measured on the GP stage (synthetic signals, a title's parameters)
 *   - the LFE low-pass: -3 dB at its corner, 24 dB an octave, unity at DC,
 *     and only on the LFE
 *
 * The vectors are synthetic; no game data.
 */
#include "apu_gp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define N APU_GP_FRAME
enum { FL, FR, C, LFE, SL, SR };

/* GP scratch for the tests: 64 KB. */
static uint32_t scr[0x4000];
static uint32_t rd(void *c, uint32_t off) { (void)c; return off / 4 < 0x4000 ? scr[off / 4] : 0; }
static void wr(void *c, uint32_t off, const uint32_t *w, int n)
{
    (void)c;
    if (off / 4 + (uint32_t)n <= 0x4000) memcpy(&scr[off / 4], w, 4u * (size_t)n);
}
static const apu_gp_mem mem = { rd, wr, NULL };

#define STATE_OFF 0x3410u               /* where the test keeps the state area */
#define ST(word) scr[STATE_OFF / 4 + (word)]

static float bins[APU_GP_BINS][N], out[APU_GP_SPEAKERS][N];

static void clear(void) { memset(bins, 0, sizeof bins); }
static void frame(void) { apu_gp_frame(&mem, bins, out); }
static void load_all(void) { apu_gp_command(&mem, 3, 0x1234, STATE_OFF, 0x400); }

static double rms(const float *x) { double s = 0; for (int i = 0; i < N; i++) s += (double)x[i] * x[i]; return sqrt(s / N); }
/* Nothing but a recursive send's rounding tail. */
static int silent(int spk) { return rms(out[spk]) < 1e-6; }

static uint32_t rng = 12345;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (float)(int32_t)rng / 2147483648.0f; }

/* The wiring of the tests: the speaker bins, three sends, a distortion on
 * 17, the export of 11..30 at 0xB000. */
enum { EQ_A = 0x10, EQ_B = 0x40, EQ_L = 0x70, DRV = 0x100 };
static void wire(int lfe_hz)
{
    apu_gp_reset();
    memset(scr, 0, sizeof scr);
    for (int b = 0; b < 6; b++) apu_gp_direct(b, b, 1.0f);
    apu_gp_eq(19, FL, EQ_A);
    apu_gp_eq(25, FL, EQ_B);
    apu_gp_eq(24, LFE, EQ_L);
    apu_gp_drive(17, DRV);
    apu_gp_export(11, 20, 0xB000);
    apu_gp_lfe_lowpass(lfe_hz);
    ST(EQ_A + 7) = 0x7FFFFF;
    ST(EQ_B + 7) = 0x800000;
    /* The image's default set. */
    ST(EQ_L + 7) = 0x400000; ST(EQ_L + 8) = 0xAD7746; ST(EQ_L + 9) = 0x23434C;
    ST(EQ_L + 10) = 0x5288B9; ST(EQ_L + 11) = 0xDCBCB3;
}

static void drive_params(uint32_t f, uint32_t q, uint32_t wet, uint32_t dry,
                         uint32_t drv, uint32_t lg, uint32_t og)
{
    ST(DRV + 7) = f; ST(DRV + 8) = q; ST(DRV + 9) = wet; ST(DRV + 10) = dry;
    ST(DRV + 11) = drv; ST(DRV + 13) = lg; ST(DRV + 15) = og;
}

static void test_routing(void)
{
    wire(0);
    CHECK(apu_gp_wired(), "wired");
    load_all();

    for (int b = 0; b < APU_GP_BINS; b++) {
        int want = b < 6 ? b : b == 19 || b == 25 ? FL : b == 24 ? LFE : -1;
        float gain = b < 6 ? 1.0f : b == 19 ? 1.0f : b == 25 ? -1.0f : 0.5f;
        clear();
        for (int i = 0; i < N; i++) bins[b][i] = 0.25f * noise();
        float in[N];
        memcpy(in, bins[b], sizeof in);
        frame();
        CHECK(apu_gp_bin_speaker(b) == want, "bin %d: speaker %d, want %d", b, apu_gp_bin_speaker(b), want);
        for (int s = 0; s < APU_GP_SPEAKERS; s++) {
            if (s != want) { CHECK(silent(s), "bin %d reaches speaker %d", b, s); continue; }
            double err = 0;
            for (int i = 0; i < N; i++) err = fmax(err, fabs(out[s][i] - gain * in[i]));
            /* 0x7FFFFF is one step under 1; the default set's biquad
             * settles from rest within float rounding. */
            CHECK(err < 2e-6, "bin %d -> speaker %d: gain %g off by %g", b, s, gain, err);
        }
        if (want >= 0)
            CHECK(fabsf(apu_gp_bin_gain(b) - gain) < 1e-6f, "bin %d: reported gain %g", b, apu_gp_bin_gain(b));
    }

    /* Opposite polarity on the same speaker. */
    clear();
    for (int i = 0; i < N; i++) { bins[19][i] = 0.9f; bins[25][i] = 0.5f; }
    frame();
    CHECK(fabsf(out[FL][5] - 0.4f) < 1e-6f, "19 at 0.9 with 25 at 0.5 on FL: %g", out[FL][5]);

    /* A biquad the title might set: a one-pole low-pass, y = 0.25 x + 0.75
     * y1 (a1 is stored halved). Its step response rises to 1. */
    wire(0);
    ST(EQ_A + 7) = 0x200000; ST(EQ_A + 10) = 0x300000;
    load_all();
    clear();
    for (int i = 0; i < N; i++) bins[19][i] = 0.5f;
    frame();
    CHECK(fabsf(out[FL][0] - 0.125f) < 1e-6f && fabsf(out[FL][1] - 0.21875f) < 1e-6f,
          "one-pole send: first samples %g %g", out[FL][0], out[FL][1]);
    for (int k = 0; k < 4; k++) frame();
    CHECK(fabsf(out[FL][N - 1] - 0.5f) < 1e-4f, "one-pole send settles at the input: %g", out[FL][N - 1]);
}

/* The guest's X window: DirectSound reads effect states back from the
 * DSP's memory and writes some words there directly. */
static void test_xwindow(void)
{
    static uint32_t win[0x80 + 0x400 + 8];
    apu_gp_mem m = { rd, wr, NULL, win };

    wire(0);
    memset(win, 0, sizeof win);
    drive_params(0x7FFFFF, 0x1FFFFF, 0x0147AE, 0x7FFFFF, 0x3FFFFF, 0x1FFFFF, 0x7FFFFF);
    apu_gp_command(&m, 3, 0, STATE_OFF, 0x400);
    CHECK(win[0x80 + EQ_A + 7] == 0x7FFFFF && win[0x80 + EQ_B + 7] == 0x800000 && win[0x80 + DRV + 9] == 0x0147AE,
          "a loaded state is not in the guest's X window (%06X %06X)", win[0x80 + EQ_A + 7], win[0x80 + EQ_B + 7]);
    CHECK(win[0x7F] == 0 && win[0x80 + 0x400] == 0, "the window was written outside the state area");
    /* The guest writes a word there, no command: the model has it. */
    win[0x80 + EQ_A + 7] = 0x800000;
    clear();
    for (int i = 0; i < N; i++) { bins[19][i] = 0.5f; bins[17][i] = 0.01f * (float)(i % 5 - 2); }
    apu_gp_frame(&m, bins, out);
    CHECK(fabsf(out[FL][3] + 0.5f) < 1e-6f, "a word the guest wrote into the window did not reach the model: %g", out[FL][3]);
    /* The model's own running state appears in the window. */
    CHECK(win[0x80 + DRV + 17] != 0 || win[0x80 + DRV + 18] != 0 || win[0x80 + DRV + 0] != 0,
          "the distortion's filter state is not in the window");
    /* ... but a word the model did not change is not rewritten over the guest's. */
    win[0x80 + EQ_L + 7] = 0x200000;
    clear();
    for (int i = 0; i < N; i++) bins[24][i] = 0.5f;
    apu_gp_frame(&m, bins, out);
    CHECK(win[0x80 + EQ_L + 7] == 0x200000, "the guest's word was overwritten");
    /* With no way to write guest memory (a capture) frames leave the window alone. */
    {
        apu_gp_mem ro = { rd, NULL, NULL, win };
        uint32_t before = win[0x80 + DRV + 18];
        clear();
        for (int i = 0; i < N; i++) bins[17][i] = 0.02f * (float)(i % 7 - 3);
        apu_gp_frame(&ro, bins, out);
        CHECK(win[0x80 + DRV + 18] == before, "a frame wrote the window while a capture was taken");
    }
}

static void test_commands(void)
{
    wire(0);
    clear();
    for (int i = 0; i < N; i++) bins[19][i] = 0.5f;
    frame();
    CHECK(silent(FL), "a send passes before the title loaded its state");
    apu_gp_command(&mem, 7, 0, STATE_OFF, 0x400);
    frame();
    CHECK(silent(FL), "an unknown command loaded the state");
    load_all();
    frame();
    CHECK(fabsf(out[FL][3] - 0.5f) < 1e-6f, "after the load: %g", out[FL][3]);
    /* The title turns the send over: nothing changes until its command. */
    ST(EQ_A + 7) = 0x800000;
    frame();
    CHECK(out[FL][3] > 0.0f, "a scratch write moved the parameter without a command");
    apu_gp_command(&mem, 2, EQ_A + 7, STATE_OFF + 4 * (EQ_A + 7), 1);
    frame();
    CHECK(fabsf(out[FL][3] + 0.5f) < 1e-6f, "after command 2: %g", out[FL][3]);
    /* An effect that is not modelled: how far the title has it open
     * while its bin is fed. */
    apu_gp_unmodelled("the test's effect", 11, 0x200, 2, 0x104);
    ST(0x200) = 0x3B; ST(0x201) = 0x004000;
    apu_gp_command(&mem, 2, 0x200, STATE_OFF + 4 * 0x200, 2);
    clear();
    frame();
    CHECK(apu_gp_unmodelled_max() == 0, "an effect nobody feeds counts as open: %X", apu_gp_unmodelled_max());
    ST(0x201) = 0xFFFF00;                           /* -0x100 */
    apu_gp_command(&mem, 2, 0x200, STATE_OFF + 4 * 0x200, 2);
    bins[11][3] = 0.1f;
    frame();
    CHECK(apu_gp_unmodelled_max() == 0x100, "fed at a quiet level: %X", apu_gp_unmodelled_max());
    ST(0x201) = 0x004000;
    apu_gp_command(&mem, 2, 0x1F0, STATE_OFF + 4 * 0x1F0, 0x20);
    bins[11][3] = 0.1f;
    frame();
    CHECK(apu_gp_unmodelled_max() == 0x4000, "fed while the title has it open: %X", apu_gp_unmodelled_max());
    /* Out of range: ignored, no crash. */
    apu_gp_command(&mem, 2, APU_GP_STATE_MAX + 5, STATE_OFF, 10);
    apu_gp_command(&mem, 2, APU_GP_STATE_MAX - 2, STATE_OFF, 100);
}

static int32_t s24(uint32_t w) { return (int32_t)(w << 8) >> 8; }

static void test_export(void)
{
    uint32_t off, bytes;
    int first;

    wire(0);
    load_all();
    CHECK(apu_gp_export_range(&off, &bytes, &first) && off == 0xB000 && bytes == 20 * 32 * 4 && first == 11,
          "export range %X %u %d", off, bytes, first);
    clear();
    for (int b = 0; b < APU_GP_BINS; b++)
        for (int i = 0; i < N; i++)
            bins[b][i] = (float)(b * 64 + i) / 8388608.0f;
    bins[20][7] = 1.5f;                 /* past full scale */
    bins[21][7] = -1.5f;
    frame();
    for (int k = 0; k < 20; k++) {
        int b = 11 + k, bad = 0;
        if (b == 17) continue;          /* the distortion's bin: below */
        for (int i = 0; i < N; i++) {
            int32_t want = b * 64 + i;
            if (b == 20 && i == 7) want = 0x7FFFFF;
            if (b == 21 && i == 7) want = -0x800000;
            bad += s24(scr[(0xB000 + k * 0x80) / 4 + i]) != want || (scr[(0xB000 + k * 0x80) / 4 + i] >> 24);
        }
        CHECK(!bad, "export slot %d (bin %d): %d words wrong", k, b, bad);
    }
    CHECK(scr[0xB000 / 4 - 1] == 0 && scr[(0xB000 + 20 * 0x80) / 4] == 0, "export wrote outside its block");
    CHECK(bins[20][7] < 1.0f && bins[21][7] == -1.0f, "a bin past full scale was not limited");

    /* The in-place effect comes first: the slot of bin 17 holds its
     * output (x8 here), and so does the bin. */
    drive_params(0x7FFFFF, 0x1FFFFF, 0, 0x7FFFFF, 0x3FFFFF, 0x1FFFFF, 0x7FFFFF);
    load_all();
    clear();
    for (int i = 0; i < N; i++) bins[17][i] = (float)(1000 * (i - 16)) / 8388608.0f;
    frame();
    {
        int bad = 0;
        for (int i = 0; i < N; i++) {
            int32_t got = s24(scr[(0xB000 + 6 * 0x80) / 4 + i]);
            bad += abs(got - 8000 * (i - 16)) > 2 || fabsf(bins[17][i] * 8388608.0f - (float)got) > 0.5f;
        }
        CHECK(!bad, "bin 17's slot is not the distortion's output (%d samples)", bad);
        for (int s = 0; s < APU_GP_SPEAKERS; s++)
            CHECK(silent(s), "bin 17 reaches speaker %d directly", s);
    }

    /* Timing, as the VP drives it: the voices of frame n are mixed before
     * the GP stage of frame n runs, so a voice playing a slot hears frame
     * n - 1. A ramp that steps every frame on bin 18 (slot 7), played back
     * into bin 0 at unity. */
    {
        int late = 0, frames = 0;
        for (int n = 1; n <= 8; n++) {
            float ret[N];
            for (int i = 0; i < N; i++) ret[i] = (float)s24(scr[(0xB000 + 7 * 0x80) / 4 + i]) / 8388608.0f;
            clear();
            for (int i = 0; i < N; i++) { bins[18][i] = (float)n / 64.0f; bins[0][i] = ret[i]; }
            frame();
            if (n > 1) { frames++; late += fabsf(out[FL][0] - (float)(n - 1) / 64.0f) < 1e-6f; }
        }
        CHECK(late == frames, "return path: %d of %d frames one frame late", late, frames);
    }
}

/* The distortion on its own: n samples of a 23-word state. */
static void drive_state(int32_t *st, uint32_t f, uint32_t q, uint32_t wet, uint32_t dry,
                        uint32_t drv, uint32_t lg, uint32_t og)
{
    memset(st, 0, 23 * sizeof(int32_t));
    st[7] = s24(f); st[8] = s24(q); st[9] = s24(wet); st[10] = s24(dry);
    st[11] = s24(drv); st[13] = s24(lg); st[15] = s24(og);
}

static void test_drive(void)
{
    int32_t st[23], x[64];
    int bad;

    /* Parameters at zero (the title's state outside a race): silence. */
    drive_state(st, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < 64; i++) x[i] = 100000 * (i - 32);
    apu_gp_drive_run(st, x, 64);
    bad = 0;
    for (int i = 0; i < 64; i++) bad += x[i] != 0;
    CHECK(!bad, "distortion with zero parameters is not silent");

    /* Wet shut (the exhaust's setting): eight times the input, clipped
     * hard at full scale, no memory. The multiplies by 0x7FFFFF truncate:
     * up to two steps under the ideal. */
    drive_state(st, 0x7FFFFF, 0x1FFFFF, 0, 0x7FFFFF, 0x3FFFFF, 0x1FFFFF, 0x7FFFFF);
    bad = 0;
    for (int32_t v = -0x800000; v < 0x800000; v += 4099) {
        int32_t y = v, want = v * 8;
        if (want > 0x7FFFFF) want = 0x7FFFFF;
        if (want < -0x800000) want = -0x800000;
        apu_gp_drive_run(st, &y, 1);
        bad += abs(y - want) > 2 + (abs(want) >> 22);
    }
    CHECK(!bad, "wet shut: %d samples are not x8 with a hard clip", bad);
    {
        int32_t y = 0x100000;           /* 0.125: exactly at the knee */
        apu_gp_drive_run(st, &y, 1);
        CHECK(y >= 0x7FFFFC && y <= 0x7FFFFF, "0.125 in: %06X out", y);
        y = -0x400000;
        apu_gp_drive_run(st, &y, 1);
        CHECK(y <= -0x7FFFFC, "-0.5 in: %06X out", y);
    }

    /* The title's throttle set (wet 0.01, drive 0.5, low gain 0.25), an
     * impulse after silence. Measured on the GP stage, as multiples of the
     * impulse: 0.001 -> 8.00014, 0.79989, 0, then -0.00012; 0.02 -> 7.99998,
     * 0.49999, 0.49999, then -0.00001 (the wet part held at +-0.01). */
    {
        static const struct { double in, h[4]; } m[2] = {
            { 0.001, { 8.00014, 0.79989, 0.00000, -0.00012 } },
            { 0.020, { 7.99998, 0.49999, 0.49999, -0.00001 } },
        };
        for (int k = 0; k < 2; k++) {
            int32_t y[12];
            int32_t imp = (int32_t)lrint(m[k].in * 8388608.0);
            drive_state(st, 0x7FFFFF, 0x1FFFFF, 0x0147AE, 0x7FFFFF, 0x3FFFFF, 0x1FFFFF, 0x7FFFFF);
            memset(y, 0, sizeof y);
            y[0] = imp;
            apu_gp_drive_run(st, y, 12);
            for (int i = 0; i < 4; i++)
                CHECK(fabs((double)y[i] / (m[k].in * 8388608.0) - m[k].h[i]) < 1e-4,
                      "impulse %.3f, sample %d: %.5f, measured %.5f", m[k].in, i,
                      (double)y[i] / (m[k].in * 8388608.0), m[k].h[i]);
        }
    }

    /* Its filter state is in the state words and survives between calls;
     * a reload from the title clears it. */
    drive_state(st, 0x7FFFFF, 0x1FFFFF, 0x0147AE, 0x7FFFFF, 0x3FFFFF, 0x1FFFFF, 0x7FFFFF);
    for (int i = 0; i < 64; i++) x[i] = (i & 1) ? 300000 : -200000;
    apu_gp_drive_run(st, x, 64);
    CHECK(st[17] != 0 || st[18] != 0 || st[0] != 0, "the filter kept no state");
}

static void test_lfe(void)
{
    /* Sines through the LFE feed with the 120 Hz filter. */
    static const struct { double hz, db, tol; } pt[] = {
        { 20, 0.0, 0.05 }, { 60, -0.02, 0.1 }, { 120, -3.01, 0.05 }, { 240, -24.1, 0.3 }, { 480, -48.2, 0.5 },
    };
    for (unsigned k = 0; k < sizeof pt / sizeof pt[0]; k++) {
        double ph = 0, sq = 0, sq_in = 0;
        int n = 0;
        wire(120);
        load_all();
        for (int f = 0; f < 1500; f++) {
            clear();
            for (int i = 0; i < N; i++) {
                bins[3][i] = 0.5f * (float)sin(ph);
                bins[0][i] = bins[3][i];
                ph += 2.0 * 3.14159265358979323846 * pt[k].hz / 48000.0;
            }
            float in[N];
            memcpy(in, bins[3], sizeof in);
            frame();
            if (f >= 750) {
                for (int i = 0; i < N; i++) { sq += (double)out[LFE][i] * out[LFE][i]; sq_in += (double)in[i] * in[i]; n++; }
                if (f == 750) {
                    int same = 1;
                    for (int i = 0; i < N; i++) same &= out[FL][i] == in[i];
                    CHECK(same, "the LFE filter touched FL");
                }
            }
        }
        double db = 10.0 * log10(sq / sq_in);
        CHECK(fabs(db - pt[k].db) < pt[k].tol, "LFE low-pass at %g Hz: %.2f dB, want %.2f", pt[k].hz, db, pt[k].db);
    }
    /* Changed while it plays (the user's setting): the feed fades from one
     * to the other over 32 frames, nothing steps, and a 40 Hz tone that
     * passes both keeps its level throughout. */
    {
        double ph = 0, worst_step = 0, lo = 1e9, hi = 0;
        float last = 0;
        wire(120);
        load_all();
        for (int f = 0; f < 400; f++) {
            clear();
            for (int i = 0; i < N; i++) {
                bins[3][i] = 0.5f * (float)sin(ph);
                ph += 2.0 * 3.14159265358979323846 * 40.0 / 48000.0;
            }
            if (f == 200) {
                apu_gp_lfe_lowpass(0);
                CHECK(apu_gp_lfe_lowpass_hz() == 0, "the change is not reported");
                apu_gp_lfe_lowpass(80);         /* during the fade: ignored */
                CHECK(apu_gp_lfe_lowpass_hz() == 0, "a change during a fade was taken");
            }
            float in[N];
            memcpy(in, bins[3], sizeof in);
            frame();
            if (f >= 100) {
                for (int i = 0; i < N; i++) {
                    worst_step = fmax(worst_step, fabs(out[LFE][i] - last));
                    last = out[LFE][i];
                }
                /* The peak of each 40 frames (longer than a cycle). */
                static double pk;
                for (int i = 0; i < N; i++) pk = fmax(pk, fabs(out[LFE][i]));
                if (f % 40 == 39) { lo = fmin(lo, pk); hi = fmax(hi, pk); pk = 0; }
            } else {
                last = out[LFE][N - 1];
            }
            if (f == 240)
                CHECK(!memcmp(in, out[LFE], sizeof in), "after the fade to off the feed is not the input");
        }
        /* A 40 Hz sine at 0.5 moves 0.0026 a sample at most. */
        CHECK(worst_step < 0.004, "the LFE feed steps by %g at a change", worst_step);
        CHECK(hi / lo < 1.03, "the tone's peak moved %g..%g through the change", lo, hi);
    }

    /* Off: the feed as it is. */
    wire(0);
    load_all();
    clear();
    for (int i = 0; i < N; i++) bins[3][i] = 0.3f * noise();
    float in[N];
    memcpy(in, bins[3], sizeof in);
    frame();
    CHECK(!memcmp(in, out[LFE], sizeof in), "no filter wired, LFE changed");
}

int main(void)
{
    apu_gp_reset();
    CHECK(!apu_gp_wired(), "nothing wired yet");
    CHECK(apu_gp_bin_speaker(0) == -1 && !apu_gp_export_range(NULL, NULL, NULL), "unwired answers");
    test_routing();
    test_commands();
    test_xwindow();
    test_export();
    test_drive();
    test_lfe();
    if (failures) {
        printf("apu_gp: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("apu_gp: routing, polarity, commands, export and return timing, distortion, LFE filter ok (%d checks)\n", checks);
    return 0;
}

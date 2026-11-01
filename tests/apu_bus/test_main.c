/* The host mixer (src/apu/apu_bus.c, BO-114).
 *
 * 1. Neutral is today: on a deterministic set of voices the mixer on with
 *    the neutral configuration gives the mixbins of apu_vp.c's own loop and
 *    the s16 output of the DSP stage with the mixer off, byte for byte,
 *    stereo and 5.1.
 * 2. Each control does what it says, in dB, ms and Hz.
 * 3. The preset file parses, writes and reads back.
 *
 * Run with: ctest --test-dir <build> */
#include "common.h"
#include "apu_level.h"
#include <time.h>

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t s_seed = 12345;
static float frand(void)                /* -1..1 */
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(int32_t)s_seed / 2147483648.0f;
}

/* The bin sets the title really uses (the BO-114 study). */
static const int k_bins[5][8] = {
    { 0, 1, 4, 5, 2, 3, 6, 7 },             /* a stream on the speaker bins */
    { 0, 1, 4, 5, 2, 17, 3, 6 },            /* 2D: the player's engine */
    { 19, 20, 16, 24, 14, 15, 11, 0 },      /* positioned, + polarity */
    { 25, 13, 23, 24, 21, 22, 11, 0 },      /* positioned, - polarity */
    { 0, 1, 2, 3, 4, 5, 6, 7 },
};

static void make_voice(tvoice *v, int k)
{
    v->channels = (k % 3 == 0) ? 2 : 1;
    memcpy(v->bin, k_bins[k % 5], sizeof(v->bin));
    for (int b = 0; b < 8; b++) {
        float a = frand();
        v->g[b] = (k % 7 == 6 || a < -0.5f) ? 0.0f : 0.25f * (a + 1.0f);
    }
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        v->samples[i][0] = frand();
        v->samples[i][1] = v->channels == 2 ? frand() : v->samples[i][0];
    }
}

static double bin_rms(const float b[NUM_SAMPLES_PER_FRAME])
{
    double s = 0;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) s += (double)b[i] * b[i];
    return sqrt(s / NUM_SAMPLES_PER_FRAME);
}

static double db(double x) { return x > 1e-12 ? 20.0 * log10(x) : -240.0; }

/* A voice through the mixer with cfg, one frame, into bins. */
static void run_one(const apu_bus_config *cfg, int channels, int bus, const tvoice *v,
                    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    struct apu_bus_rt *rt;
    apu_bus_set_config(cfg);
    apu_bus_enable(1);
    rt = apu_bus_frame_begin(channels, k_map);
    apu_bus_mix_voice(rt, bus, bins, (const float (*)[2])v->samples, v->channels, v->bin, v->g);
    apu_bus_frame_end(rt, bins);
}

static void test_identity(void)
{
    static float A[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], B[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static tvoice vv[24];
    apu_bus_config cfg;
    unsigned long bad_bins = 0, bad_st = 0, bad_51 = 0, clipped = 0;

    apu_bus_config_neutral(&cfg);
    CHECK(apu_bus_config_is_neutral(&cfg), "the neutral configuration is not neutral");
    apu_bus_set_config(&cfg);
    for (int frame = 0; frame < 600; frame++) {
        int16_t oa[NUM_SAMPLES_PER_FRAME * 6], ob[NUM_SAMPLES_PER_FRAME * 6];
        for (int k = 0; k < 24; k++) make_voice(&vv[k], k + frame);
        memset(A, 0, sizeof(A));
        for (int k = 0; k < 24; k++) old_mix(A, &vv[k]);
        for (int six = 0; six < 2; six++) {
            struct apu_bus_rt *rt;
            apu_bus_enable(1);
            rt = apu_bus_frame_begin(six ? 6 : 2, k_map);
            memset(B, 0, sizeof(B));
            for (int k = 0; k < 24; k++)
                apu_bus_mix_voice(rt, k % 13, B, (const float (*)[2])vv[k].samples,
                                  vv[k].channels, vv[k].bin, vv[k].g);
            apu_bus_frame_end(rt, B);
            if (memcmp(A, B, sizeof(A))) bad_bins++;
            dsp_out(B, six, ob);
            apu_bus_enable(0);
            dsp_out(A, six, oa);
            if (memcmp(oa, ob, sizeof(int16_t) * (six ? 6 : 2) * NUM_SAMPLES_PER_FRAME)) {
                if (six) bad_51++; else bad_st++;
            }
            {
                /* What the clamp alone (today's output) cuts: the set must reach it. */
                int16_t o0[NUM_SAMPLES_PER_FRAME * 6];
                dsp_out_lim(A, six, o0, 0);
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME * (six ? 6 : 2); i++)
                    clipped += o0[i] == 32767 || o0[i] == -32767;
            }
        }
    }
    {
        /* The control: 0.1 dB on every bus must show, or the comparison
         * above compares nothing. */
        struct apu_bus_rt *rt;
        for (int k = 0; k < 13; k++) cfg.bus[k].gain_db = -0.1f;
        apu_bus_set_config(&cfg);
        apu_bus_enable(1);
        rt = apu_bus_frame_begin(6, k_map);
        memset(B, 0, sizeof(B));
        for (int k = 0; k < 24; k++)
            apu_bus_mix_voice(rt, k % 13, B, (const float (*)[2])vv[k].samples,
                              vv[k].channels, vv[k].bin, vv[k].g);
        apu_bus_frame_end(rt, B);
        CHECK(memcmp(A, B, sizeof(A)) != 0, "a 0.1 dB gain leaves the mixbins identical: the comparison is blind");
        apu_bus_config_neutral(&cfg);
        apu_bus_set_config(&cfg);
        apu_bus_enable(0);
    }
    CHECK(!bad_bins, "neutral mixer: mixbins differ from the loop's in %lu frames", bad_bins);
    CHECK(!bad_st, "neutral mixer: stereo s16 output differs in %lu frames", bad_st);
    CHECK(!bad_51, "neutral mixer: 5.1 s16 output differs in %lu frames", bad_51);
    /* The set must drive the clamp too, or the clamp is not covered. */
    CHECK(clipped > 100, "the test voices never reach full scale (%lu)", clipped);
    printf("identity: 600 frames x 24 voices, stereo and 5.1: %lu / %lu / %lu frames differ"
           " (mixbins / stereo / 5.1), %lu clamped samples covered\n",
           bad_bins, bad_st, bad_51, clipped);
}

/* The safety limiter (apu_limit.h) on the real DSP stage: against the
 * clamp alone, which is today's output. A frame whose clamp-alone output
 * stays under the ceiling must come out the same bytes; one that does not
 * must come out at or under it. Each frame starts from the idle limiter,
 * as every comparison here does. Scaled sets: 1 (clips), 0.08 and 0.04 (never
 * reaches the ceiling: the whole run must be identical). */
static void test_limiter_identity(void)
{
    static tvoice vv[24];
    static float A[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    unsigned long frames = 0, quiet = 0, quiet_bad = 0, loud = 0, loud_bad = 0, over_samples = 0;
    unsigned long fq = 0, fq_bad = 0, fl = 0, fl_bad = 0;

    for (int pass = 0; pass < 3; pass++) {
        const float scale = pass == 0 ? 1.0f : pass == 1 ? 0.08f : 0.04f;
        for (int frame = 0; frame < 300; frame++) {
            for (int k = 0; k < 24; k++) {
                make_voice(&vv[k], k + frame + 7 * pass);
                for (int b = 0; b < 8; b++) vv[k].g[b] *= scale;
            }
            memset(A, 0, sizeof(A));
            for (int k = 0; k < 24; k++) old_mix(A, &vv[k]);
            for (int six = 0; six < 2; six++) {
                int16_t on[NUM_SAMPLES_PER_FRAME * 6], off[NUM_SAMPLES_PER_FRAME * 6];
                const int n = NUM_SAMPLES_PER_FRAME * (six ? 6 : 2);
                int over = 0;
                dsp_out_lim(A, six, on, 1);
                dsp_out_lim(A, six, off, 0);
                if (six) {
                    /* The 5.1 feeds of today folded for a stereo device: saturating
                     * fold against the limited one (apu_limit_fold16). */
                    int16_t f0[NUM_SAMPLES_PER_FRAME][2], f1[NUM_SAMPLES_PER_FRAME][2];
                    int fover = 0, fhot = 0;
                    apu_limit_reset(); apu_limit_enable(0);
                    apu_spk_downmix((const int16_t (*)[6])off, f0, NUM_SAMPLES_PER_FRAME);
                    apu_limit_reset(); apu_limit_enable(1);
                    apu_spk_downmix((const int16_t (*)[6])off, f1, NUM_SAMPLES_PER_FRAME);
                    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                        for (int c = 0; c < 2; c++) {
                            if (abs(f0[i][c]) > LIM_CEIL16) fover = 1;
                            if (abs(f1[i][c]) > LIM_CEIL16) fhot = 1;
                        }
                    if (!fover) { fq++; if (memcmp(f0, f1, sizeof f0)) fq_bad++; }
                    else { fl++; fl_bad += fhot; }
                }
                for (int i = 0; i < n; i++)
                    if (abs(off[i]) > LIM_CEIL16) { over = 1; over_samples++; }
                frames++;
                if (!over) {
                    quiet++;
                    if (memcmp(on, off, sizeof(int16_t) * (size_t)n)) quiet_bad++;
                } else {
                    loud++;
                    for (int i = 0; i < n; i++)
                        if (abs(on[i]) > LIM_CEIL16) {
                            if (getenv("LIMDBG")) fprintf(stderr, "dbg six=%d i=%d ch=%d on=%d off=%d\n", six, i, six ? i % 6 : i % 2, on[i], off[i]);
                            loud_bad++; break; }
                }
            }
        }
    }
    CHECK(quiet > 1000, "the quiet sets never stay under the ceiling (%lu frames)", quiet);
    CHECK(loud > 100, "the loud set never reaches the ceiling (%lu frames)", loud);
    CHECK(!fq_bad && !fl_bad && fq > 100 && fl > 100, "fold limiter: %lu/%lu quiet differ, %lu/%lu loud above (need both kinds)", fq_bad, fq, fl_bad, fl);
    printf("limiter on the 5.1 fold (stereo device): %lu frames under the ceiling, %lu differ; %lu over it, %lu above -0.5 dBFS\n", fq, fq_bad, fl, fl_bad);
    CHECK(!quiet_bad, "safety limiter: %lu of %lu frames under the ceiling differ from the clamp alone", quiet_bad, quiet);
    CHECK(!loud_bad, "safety limiter: %lu of %lu frames over the ceiling come out above it", loud_bad, loud);
    printf("limiter on the DSP stage: %lu frames (stereo and 5.1); %lu stayed under the ceiling and differ from the"
           " clamp alone in %lu; %lu went over it (%lu samples) and %lu of those still show a sample above -0.5 dBFS\n",
           frames, quiet, quiet_bad, loud, over_samples, loud_bad);
}

/* ── the safety limiter's own behaviour ── */

static void test_limiter(void)
{
    static float f[2][APU_LIMIT_FRAME], ref[2][APU_LIMIT_FRAME];
    const double w = 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
    float peak = 0.0f;
    unsigned long moved = 0;

    /* A sine over full scale (1.5 = +3.5 dBFS) for one second comes out at
     * or under the ceiling, both channels the same gain. */
    apu_limit_reset();
    apu_limit_enable(1);
    for (int blk = 0; blk < 1500; blk++) {
        for (int i = 0; i < APU_LIMIT_FRAME; i++) {
            const float x = 1.5f * (float)sin(w * (blk * APU_LIMIT_FRAME + i));
            f[0][i] = x; f[1][i] = 0.5f * x;
        }
        apu_limit(f, 2, APU_LIMIT_FRAME);
        for (int i = 0; i < APU_LIMIT_FRAME; i++) {
            if (fabsf(f[0][i]) > peak) peak = fabsf(f[0][i]);
            if (fabsf(f[1][i]) > 0.5f * 0.94407f) { CHECK(0, "second channel above its share of the ceiling"); break; }
        }
    }
    CHECK(peak <= 0.94407f, "limited peak %.5f is over the ceiling 0.94406", peak);
    CHECK(peak >= 0.94f, "limited peak %.5f is far under the ceiling: the limiter pulls too hard", peak);
    printf("limiter: +3.5 dBFS sine in, peak out %.4f (ceiling %.4f = -0.5 dBFS), reduction now %.2f dB\n",
           peak, 0.94406, apu_limit_reduction_db());

    /* Instant attack: the first sample over the ceiling is itself on it. */
    apu_limit_reset();
    memset(f, 0, sizeof(f));
    f[0][10] = 2.0f; f[1][10] = -1.0f;
    apu_limit(f, 2, APU_LIMIT_FRAME);
    CHECK(fabsf(f[0][10]) <= 0.94407f && f[0][10] > 0.94f, "the first sample over is not limited at once: %f", f[0][10]);
    CHECK(f[1][10] < 0 && fabsf(f[1][10] - (-0.47203f)) < 0.001f, "the channels are not linked: %f", f[1][10]);

    /* Release: a burst, then quiet. The gain recovers by 1 - 1/e of the
     * way in about 50 ms and is exactly 1 again, the samples untouched. */
    apu_limit_reset();
    memset(f, 0, sizeof(f));
    f[0][0] = 1.5f;                                 /* gain 0.6294 from here */
    apu_limit(f, 2, APU_LIMIT_FRAME);
    {
        const float g0 = powf(10.0f, -apu_limit_reduction_db() / 20.0f);
        float g50 = 0.0f, g_end = 0.0f;
        int exact_at = -1;
        CHECK(fabsf(g0 - 0.6294f) < 0.007f, "gain after the peak %.4f, expected 0.6294 plus 31 samples of release", g0);
        for (int blk = 1; blk <= 1500 && exact_at < 0; blk++) {
            for (int i = 0; i < APU_LIMIT_FRAME; i++) { f[0][i] = 0.3f; f[1][i] = -0.3f; }
            apu_limit(f, 2, APU_LIMIT_FRAME);
            if (blk * APU_LIMIT_FRAME >= 2400 && g50 == 0.0f)       /* 50 ms later */
                g50 = powf(10.0f, -apu_limit_reduction_db() / 20.0f);
            if (apu_limit_reduction_db() == 0.0f) exact_at = blk * APU_LIMIT_FRAME;
            g_end = f[0][APU_LIMIT_FRAME - 1];
        }
        {
            const float want = 1.0f - (1.0f - g0) * expf(-1.0f);
            CHECK(fabsf(g50 - want) < 0.01f, "gain 50 ms after the peak %.4f, expected %.4f", g50, want);
            CHECK(exact_at > 0 && exact_at < 48000, "the gain never came back to exactly 1");
            CHECK(g_end == 0.3f, "samples touched after the release: %f", g_end);
            printf("limiter: peak at 1.5 -> gain %.3f; 50 ms later %.3f (1/e point %.3f); exactly 1 again after %d ms\n",
                   g0, g50, want, exact_at / 48);
        }
    }

    /* Under the ceiling: not a bit moves, nothing is counted as engaged. */
    apu_limit_reset();
    for (int blk = 0; blk < 1500; blk++) {
        for (int i = 0; i < APU_LIMIT_FRAME; i++) {
            const float x = 0.9f * (float)sin(w * (blk * APU_LIMIT_FRAME + i));
            ref[0][i] = f[0][i] = x; ref[1][i] = f[1][i] = -x;
        }
        apu_limit(f, 2, APU_LIMIT_FRAME);
        if (memcmp(f, ref, sizeof(f))) moved++;
    }
    CHECK(!moved && apu_limit_engaged() == 0, "a 0.9 sine (-0.9 dBFS) was touched in %lu blocks", moved);
    printf("limiter: 1 s of a -0.9 dBFS sine: %lu blocks changed, %lu samples engaged\n", moved, apu_limit_engaged());
    apu_limit_reset();
}

/* ── the custom-music leveller ── */

static float noise_amp(float e_db)         /* uniform noise per channel, E = 2 p */
{
    return sqrtf(3.0f * 0.5f * powf(10.0f, e_db / 10.0f));
}

/* seconds of noise at E dB through the leveller; returns the gain at the end */
static float lv_run(apu_level_state *st, float e_db, float secs, float target, float *at_s, float *gain_at, int n_at)
{
    static float blk[32][2];
    const float a = e_db <= -200.0f ? 0.0f : noise_amp(e_db);
    const int frames = (int)(secs * 1500.0f);
    float gl = 1.0f;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < 32; i++) { blk[i][0] = a * frand(); blk[i][1] = a * frand(); }
        gl = apu_level_frame_old(st, blk, 2, target);
        for (int k = 0; k < n_at; k++)
            if (f + 1 == (int)(at_s[k] * 1500.0f)) gain_at[k] = st->gain_db;
    }
    (void)gl;
    return st->gain_db;
}

static int lv_fill(void *ctx, apu_bus_voice *v) { v->stream = 1; v->nstack = *(int *)ctx; return 1; }
static int lv_class(const apu_bus_voice *v) { return v->nstack == 7; }

/* The first leveller (BO-132, RECOMP_AUDIO_CUSTOM_AGC=old): kept for comparison. */
static void test_leveller_old(void)
{
    const float T = APU_LEVEL_OLD_TARGET_DB;
    apu_level_state st;
    float g;

    /* A loud master (-7.5) is pulled down to the target: -8 dB. */
    apu_level_state_init(&st, -1000.0f);
    g = lv_run(&st, -7.5f, 20.0f, T, NULL, NULL, 0);
    CHECK(fabsf(g - (T + 7.5f)) < 0.5f, "loud song: gain %.2f dB, expected %.2f", g, T + 7.5f);
    printf("leveller (old): noise at E = -7.5 dB, 20 s: gain %+.2f dB (envelope %.2f dB, target %.1f)\n", g, st.env_db, T);

    /* A quiet one is brought up, never past +6 dB. */
    apu_level_state_init(&st, -1000.0f);
    g = lv_run(&st, -30.0f, 20.0f, T, NULL, NULL, 0);
    CHECK(g <= APU_LEVEL_OLD_MAX_DB + 1e-4f && g > APU_LEVEL_OLD_MAX_DB - 0.3f, "quiet song: gain %.2f dB, expected +6 at the limit", g);
    apu_level_state_init(&st, -1000.0f);
    g = lv_run(&st, T - 4.0f, 20.0f, T, NULL, NULL, 0);
    CHECK(fabsf(g - 4.0f) < 0.5f, "song 4 dB under: gain %.2f dB, expected +4", g);
    printf("leveller (old): -30 dB-like noise: gain %+.2f dB (limit +6); 4 dB under the target: %+.2f dB\n",
           lv_run(&(apu_level_state){0}, -30.0f, 1.0f, T, NULL, NULL, 0), g);

    /* A very loud one is limited to -15 dB. */
    apu_level_state_init(&st, -1000.0f);
    g = lv_run(&st, 3.0f, 10.0f, T, NULL, NULL, 0);
    CHECK(g >= APU_LEVEL_OLD_MIN_DB - 1e-4f && g < APU_LEVEL_OLD_MIN_DB + 0.3f, "very loud song: gain %.2f dB, expected -15", g);

    /* Silence, a very quiet intro and a fade out do not move the gain. */
    apu_level_state_init(&st, -1000.0f);
    {
        float g0 = lv_run(&st, -7.5f, 20.0f, T, NULL, NULL, 0), g1, g2;
        g1 = lv_run(&st, -200.0f, 15.0f, T, NULL, NULL, 0);     /* digital silence */
        CHECK(fabsf(g1 - g0) < 0.05f, "silence moved the gain: %.4f -> %.4f", g0, g1);
        g2 = lv_run(&st, -48.0f, 15.0f, T, NULL, NULL, 0);      /* a very quiet passage */
        CHECK(fabsf(g2 - g0) < 0.1f, "a quiet passage (-48) moved the gain: %.3f -> %.3f", g0, g2);
        printf("leveller (old): gain %+.3f dB; after 15 s of silence %+.3f; after 15 s at -48 dB %+.3f\n", g0, g1, g2);
    }
    /* A fade out over 8 s from -7.5 to -60: the gain stays. */
    apu_level_state_init(&st, -1000.0f);
    {
        static float blk[32][2];
        float g0 = lv_run(&st, -7.5f, 20.0f, T, NULL, NULL, 0), gmax = g0;
        for (int f = 0; f < 8 * 1500; f++) {
            const float e = -7.5f - 52.5f * (float)f / (8.0f * 1500.0f);
            const float a = noise_amp(e);
            for (int i = 0; i < 32; i++) { blk[i][0] = a * frand(); blk[i][1] = a * frand(); }
            apu_level_frame_old(&st, blk, 2, T);
            if (st.gain_db > gmax) gmax = st.gain_db;
        }
        CHECK(gmax - g0 < 0.5f, "a fade out wound the gain up by %.2f dB", gmax - g0);
        printf("leveller (old): 8 s fade out (-7.5 to -60): the gain's highest point %+.2f dB over where it was\n", gmax - g0);
    }

    /* A step from a quiet passage to a loud one is caught. */
    apu_level_state_init(&st, -1000.0f);
    {
        float at[4] = { 0.1f, 0.3f, 0.6f, 1.5f }, ga[4] = { 0 };
        float g0 = lv_run(&st, T - 7.0f, 30.0f, T, NULL, NULL, 0);        /* +7 -> +6 */
        float want = T + 7.5f, g10;
        lv_run(&st, -7.5f, 2.0f, T, at, ga, 4);
        g10 = lv_run(&st, -7.5f, 8.0f, T, NULL, NULL, 0);
        CHECK(fabsf(g10 - want) < 1.0f, "step: %.2f dB ten seconds after, settles at %.2f", g10, want);
        printf("leveller (old): quiet to loud step (+14.5 dB): gain %+.2f dB before; after 0.1 / 0.3 / 0.6 / 1.5 s: %+.2f / %+.2f / %+.2f / %+.2f (settles at %+.2f, reached %+.2f after 10 s)\n",
               g0, ga[0], ga[1], ga[2], ga[3], want, g10);
        /* The gain follows with a 0.3 s time constant: 63 % of the way is a
         * time constant after the envelope has seen the step. */
        CHECK(ga[1] < g0 - 0.30f * (g0 - want), "step: %.2f dB at 0.3 s is less than 30%% of the way from %.2f to %.2f", ga[1], g0, want);
        CHECK(ga[2] < g0 - 0.60f * (g0 - want), "step: %.2f dB at 0.6 s is less than 60%% of the way from %.2f to %.2f", ga[2], g0, want);
        CHECK(ga[3] < want + 4.0f, "step: still %.2f dB at 1.5 s (settles at %.2f)", ga[3], want);
    }
    /* Beat-like level changes inside a song (a break 8 dB down for 1 s)
     * do not pump: the gain moves under 1 dB. */
    apu_level_state_init(&st, -1000.0f);
    {
        float g0 = lv_run(&st, -7.5f, 20.0f, T, NULL, NULL, 0), lo = g0, hi = g0;
        for (int rep = 0; rep < 6; rep++) {
            lv_run(&st, -15.5f, 1.0f, T, NULL, NULL, 0);
            if (st.gain_db < lo) lo = st.gain_db;
            if (st.gain_db > hi) hi = st.gain_db;
            lv_run(&st, -7.5f, 3.0f, T, NULL, NULL, 0);
            if (st.gain_db < lo) lo = st.gain_db;
            if (st.gain_db > hi) hi = st.gain_db;
        }
        CHECK(hi - lo < 1.5f, "a 1 s break pumped the gain by %.2f dB", hi - lo);
        printf("leveller (old): 1 s breaks 8 dB down every 4 s: the gain moved within %.2f dB\n", hi - lo);
    }
    /* The offsets of the three levels. */
    CHECK(apu_level_mode_offset_db(APU_LEVEL_QUIETER) == -4.0f && apu_level_mode_offset_db(APU_LEVEL_MATCH) == 0.0f
          && apu_level_mode_offset_db(APU_LEVEL_LOUDER) == 4.0f, "mode offsets");
    apu_level_state_init(&st, -1000.0f);
    g = lv_run(&st, -7.5f, 20.0f, T - 4.0f, NULL, NULL, 0);
    CHECK(fabsf(g - (T - 4.0f + 7.5f)) < 0.5f, "quieter: gain %.2f", g);

    /* Voices: only the classifier's voices are touched, off touches none,
     * and a new song starts at the gain the last one settled on. */
    {
        static float a[32][2], b[32][2], keep[32][2];
        int custom = 7, other = 3;
        float gain1 = 0, first = 0;
        const float amp = noise_amp(-7.5f);
        apu_level_set_classifier(lv_class);
        apu_level_set_mode(APU_LEVEL_MATCH);
        apu_level_set_old(1);
        for (int i = 0; i < 32; i++) { a[i][0] = amp * frand(); a[i][1] = amp * frand(); }
        memcpy(keep, a, sizeof(a));
        for (int f = 0; f < 15 * 1500; f++) {
            for (int i = 0; i < 32; i++) { b[i][0] = amp * frand(); b[i][1] = amp * frand(); }
            memcpy(a, b, sizeof(a));
            apu_level_voice(5, 1, lv_fill, &custom, a, 2);                  /* song 1 */
            if (f == 15 * 1500 - 1) {
                double ra = 0, rb = 0;
                for (int i = 0; i < 32; i++) { ra += a[i][0] * a[i][0]; rb += b[i][0] * b[i][0]; }
                gain1 = 10.0f * log10f((float)(ra / rb));
            }
        }
        CHECK(fabsf(gain1 - (T + 7.5f)) < 0.7f, "voice path: gain %.2f dB", gain1);
        memcpy(a, b, sizeof(a));
        apu_level_voice(5, 2, lv_fill, &custom, a, 2);                      /* song 2 starts */
        {
            double ra = 0, rb = 0;
            for (int i = 0; i < 32; i++) { ra += a[i][0] * a[i][0]; rb += b[i][0] * b[i][0]; }
            first = 10.0f * log10f((float)(ra / rb));
        }
        CHECK(fabsf(first - gain1) < 0.3f, "a new song starts at %.2f dB, the last settled at %.2f", first, gain1);
        printf("leveller (old): voice path: song 1 settled at %+.2f dB, song 2 starts at %+.2f dB\n", gain1, first);
        /* another kind of voice: untouched */
        memcpy(a, b, sizeof(a));
        apu_level_voice(6, 1, lv_fill, &other, a, 2);
        CHECK(!memcmp(a, b, sizeof(a)), "a voice the classifier refuses was touched");
        /* off: nothing runs */
        apu_level_set_mode(APU_LEVEL_OFF);
        CHECK(!apu_level_active(), "off is active");
        memcpy(a, b, sizeof(a));
        apu_level_voice(5, 2, lv_fill, &custom, a, 2);
        CHECK(!memcmp(a, b, sizeof(a)), "off touched a voice");
        apu_level_set_mode(APU_LEVEL_MATCH);
        apu_level_set_classifier(NULL);
        apu_level_set_old(0);
        CHECK(!apu_level_active(), "no classifier but active");
        (void)keep;
    }
}


/* ── the loudness chain (BO-246) ── */

/* A 1 kHz sine of the same amplitude on both channels reads its peak level
 * in LUFS (the K weighting is +0.69 dB there, which the -0.691 of BS.1770
 * takes back): amplitude 10^(L/20). */
static double s_ph;
static void sine_frame(float blk[32][2], float lufs)
{
    const float a = lufs <= -200.0f ? 0.0f : powf(10.0f, lufs / 20.0f);
    for (int i = 0; i < 32; i++) {
        const float v = a * sinf((float)s_ph);
        s_ph += 2.0 * M_PI * 1000.0 / 48000.0;
        blk[i][0] = blk[i][1] = v;
    }
    if (s_ph > 2.0 * M_PI * 1000.0) s_ph = fmod(s_ph, 2.0 * M_PI);
}

/* secs of a sine at L LUFS through the chain; lo/hi: the gain's range */
static float ch_run(apu_level_state *st, float lufs, float secs, float target, float *lo, float *hi)
{
    static float blk[32][2];
    const int frames = (int)(secs * 1500.0f);
    for (int f = 0; f < frames; f++) {
        sine_frame(blk, lufs);
        apu_level_frame(st, blk, 2, target);
        if (lo && st->gain_db < *lo) *lo = st->gain_db;
        if (hi && st->gain_db > *hi) *hi = st->gain_db;
    }
    return st->gain_db;
}

/* the next song: as the voice path starts one */
static void ch_next(apu_level_state *st)
{
    const float g = st->gain_db, r = st->run_db;
    apu_level_state_init(st, g);
    st->run_db = st->prior_db = r;
}

static void test_leveller(void)
{
    const float T = APU_LEVEL_TARGET_DB;
    apu_level_state st;
    float g, lo, hi;

    /* The meter: a sine at -20 LUFS reads -20. */
    apu_level_state_init(&st, -1000.0f);
    ch_run(&st, -20.0f, 5.0f, T, NULL, NULL);
    CHECK(fabsf(st.st_db + 20.0f) < 0.2f, "K-weighted reading of a -20 LUFS sine: %.2f", st.st_db);

    /* A loud master is brought down to the target; the first song jumps
     * to its gain at its first reading. */
    apu_level_state_init(&st, -1000.0f);
    g = ch_run(&st, -7.0f, 30.0f, T, NULL, NULL);
    CHECK(fabsf(g - (T + 7.0f)) < 0.3f, "loud song: gain %.2f dB, expected %.2f", g, T + 7.0f);
    printf("leveller: a -7 LUFS song: gain %+.2f dB (target %.1f LUFS), short-term %.2f LUFS\n", g, T, st.st_db);

    /* A quiet one is brought up, up to +15 dB; a louder one than +3 LUFS
     * is cut by at most 20. */
    apu_level_state_init(&st, -1000.0f);
    g = ch_run(&st, T - 12.0f, 30.0f, T, NULL, NULL);
    CHECK(fabsf(g - 12.0f) < 0.3f, "song 12 LU under: gain %.2f dB, expected +12", g);
    apu_level_state_init(&st, -1000.0f);
    g = ch_run(&st, T - 25.0f, 30.0f, T, NULL, NULL);
    CHECK(g <= APU_LEVEL_MAX_DB + 1e-4f && g > APU_LEVEL_MAX_DB - 0.3f, "very quiet song: gain %.2f dB, expected +15", g);
    apu_level_state_init(&st, -1000.0f);
    g = ch_run(&st, 5.0f, 30.0f, T, NULL, NULL);
    CHECK(g >= APU_LEVEL_MIN_DB - 1e-4f && g < APU_LEVEL_MIN_DB + 0.3f, "very loud song: gain %.2f dB, expected -20", g);

    /* Silence and a passage far under the song hold the gain. */
    apu_level_state_init(&st, -1000.0f);
    {
        float g0 = ch_run(&st, -7.0f, 30.0f, T, NULL, NULL), g1, g2;
        g1 = ch_run(&st, -200.0f, 15.0f, T, NULL, NULL);
        g2 = ch_run(&st, -30.0f, 30.0f, T, NULL, NULL);
        /* (the 3 s window's tail of the song, read as it goes quiet, moves
         * the song's level by a few hundredths) */
        CHECK(fabsf(g1 - g0) < 0.1f && fabsf(g2 - g1) < 0.01f, "silence / a quiet passage moved the gain: %.3f -> %.3f -> %.3f", g0, g1, g2);
        printf("leveller: gain %+.3f dB; after 15 s of silence %+.3f; after 30 s at -30 LUFS %+.3f\n", g0, g1, g2);
    }

    /* A breakdown 6 LU down for 20 s, a minute into a song: the gain moves
     * under 1 dB (the song's level has settled). */
    apu_level_state_init(&st, -1000.0f);
    {
        float g0 = ch_run(&st, -9.0f, 60.0f, T, NULL, NULL);
        lo = hi = g0;
        ch_run(&st, -15.0f, 20.0f, T, &lo, &hi);
        ch_run(&st, -9.0f, 20.0f, T, &lo, &hi);
        CHECK(hi - lo < 1.0f, "a 20 s breakdown 6 LU down moved the gain by %.2f dB", hi - lo);
        printf("leveller: 20 s breakdown 6 LU down, a minute in: the gain moved within %.2f dB\n", hi - lo);
    }

    /* The next song starts at the last gain. A quiet intro (-28 LUFS,
     * under any song's level) holds it; the body then sets it. */
    apu_level_state_init(&st, -1000.0f);
    {
        float g0 = ch_run(&st, -10.0f, 60.0f, T, NULL, NULL), g20;
        ch_next(&st);
        lo = hi = g0;
        ch_run(&st, -28.0f, 20.0f, T, &lo, &hi);
        CHECK(hi - lo < 0.01f, "a 20 s quiet intro moved the gain by %.2f dB", hi - lo);
        g20 = ch_run(&st, -14.0f, 20.0f, T, NULL, NULL);
        CHECK(fabsf(g20 - (T + 14.0f)) < 1.0f, "after the intro: %.2f dB 20 s into the body, expected %.2f", g20, T + 14.0f);
        printf("leveller: next song, 20 s intro at -28 LUFS: gain held at %+.2f (moved %.3f); 20 s into its body at -14: %+.2f (wants %+.2f)\n",
               g0, hi - lo, g20, T + 14.0f);
    }

    /* A quiet song after a loud one: 13 LU under, gated at first, then
     * taken as the new level; within 30 s it is within 1 dB. */
    apu_level_state_init(&st, -1000.0f);
    {
        float g30;
        ch_run(&st, -7.0f, 60.0f, T, NULL, NULL);
        ch_next(&st);
        g30 = ch_run(&st, -20.0f, 30.0f, T, NULL, NULL);
        CHECK(fabsf(g30 - (T + 20.0f)) < 1.0f, "a quiet song after a loud one: %.2f dB after 30 s, expected %.2f", g30, T + 20.0f);
        g = ch_run(&st, -20.0f, 30.0f, T, NULL, NULL);
        printf("leveller: a -20 LUFS song after a -7 one: gain %+.2f after 30 s, %+.2f after 60 s (wants %+.2f)\n", g30, g, T + 20.0f);
    }

    /* The ceiling: a loud song after a quiet one is brought within 3 dB
     * of the target in about a second (its first reading, then 0.5 s). */
    apu_level_state_init(&st, -1000.0f);
    {
        float g15, g30;
        ch_run(&st, -20.0f, 60.0f, T, NULL, NULL);
        ch_next(&st);
        ch_run(&st, -7.0f, 1.0f, T, NULL, NULL);
        g15 = ch_run(&st, -7.0f, 1.0f, T, NULL, NULL);
        g30 = ch_run(&st, -7.0f, 28.0f, T, NULL, NULL);
        CHECK(g15 < T + APU_LEVEL_CEIL_DB + 7.0f + 1.5f, "a loud song after a quiet one: %.2f dB after 2 s, ceiling %.2f",
              g15, T + APU_LEVEL_CEIL_DB + 7.0f);
        CHECK(fabsf(g30 - (T + 7.0f)) < 0.3f, "a loud song after a quiet one: %.2f dB after 30 s, expected %.2f", g30, T + 7.0f);
        printf("leveller: a -7 LUFS song after a -20 one: gain %+.2f after 2 s (ceiling %+.2f), %+.2f after 30 s (wants %+.2f)\n",
               g15, T + APU_LEVEL_CEIL_DB + 7.0f, g30, T + 7.0f);
    }

    /* A song that goes up a level for good (4 LU over for 2 s) is followed
     * within seconds, not over its history. */
    apu_level_state_init(&st, -1000.0f);
    {
        float at[3];
        ch_run(&st, -20.0f, 60.0f, T, NULL, NULL);
        at[0] = ch_run(&st, -10.0f, 5.0f, T, NULL, NULL);
        at[1] = ch_run(&st, -10.0f, 5.0f, T, NULL, NULL);
        at[2] = ch_run(&st, -10.0f, 20.0f, T, NULL, NULL);
        CHECK(fabsf(at[1] - (T + 10.0f)) < 1.5f && fabsf(at[2] - (T + 10.0f)) < 0.3f,
              "step up by 10 LU: %.2f after 10 s, %.2f after 30 s, expected %.2f", at[1], at[2], T + 10.0f);
        printf("leveller: a minute at -20 LUFS, then -10: gain %+.2f / %+.2f / %+.2f after 5 / 10 / 30 s (wants %+.2f)\n",
               at[0], at[1], at[2], T + 10.0f);
    }

    /* The compressor: nothing at the target; a burst 12 dB over is turned
     * down within 30 ms, at 2:1 over the threshold. */
    apu_level_state_init(&st, -1000.0f);
    {
        static float blk[32][2];
        float c_steady, c_30ms = 0.0f, c_end;
        ch_run(&st, T, 30.0f, T, NULL, NULL);
        c_steady = -st.comp_db;
        for (int f = 0; f < 1500; f++) {
            sine_frame(blk, T + 12.0f);
            apu_level_frame(&st, blk, 2, T);
            if (f == 45) c_30ms = -st.comp_db;
        }
        c_end = -st.comp_db;
        CHECK(c_steady < 0.01f, "the compressor works at the target: %.2f dB", c_steady);
        CHECK(c_30ms > 1.0f, "a burst 12 dB over: %.2f dB of compression after 30 ms", c_30ms);
        /* After a second the ceiling has the gain down too: the burst comes
         * out no more than 3 dB over the target (the reading is 3 s long,
         * so 1 s of it is the burst averaged with the 2 s before). */
        CHECK(st.gain_db + st.comp_db < T + APU_LEVEL_CEIL_DB - (T + 12.0f) + 6.0f,
              "a burst 12 dB over: gain %.2f + compressor %.2f after 1 s", st.gain_db, st.comp_db);
        printf("leveller: compressor %.2f dB at the target; a burst 12 dB over: %.2f dB after 30 ms, %.2f dB after 1 s"
               " (gain then %+.2f dB)\n", c_steady, c_30ms, c_end, st.gain_db);
    }

    /* The offsets of the three levels. */
    CHECK(apu_level_mode_offset_db(APU_LEVEL_QUIETER) == -4.0f && apu_level_mode_offset_db(APU_LEVEL_MATCH) == 0.0f
          && apu_level_mode_offset_db(APU_LEVEL_LOUDER) == 4.0f, "mode offsets");
    apu_level_state_init(&st, -1000.0f);
    g = ch_run(&st, -7.0f, 30.0f, T + 4.0f, NULL, NULL);
    CHECK(fabsf(g - (T + 4.0f + 7.0f)) < 0.3f, "louder: gain %.2f", g);

    /* Voices: only the classifier's voices are touched, off touches none,
     * and a new song starts at the gain the last one settled on. */
    {
        static float a[32][2], b[32][2];
        int custom = 7, other = 3;
        float gain1 = 0, first = 0;
        apu_level_set_classifier(lv_class);
        apu_level_set_mode(APU_LEVEL_MATCH);
        apu_level_set_old(0);
        for (int f = 0; f < 30 * 1500; f++) {
            sine_frame(b, -7.0f);
            memcpy(a, b, sizeof(a));
            apu_level_voice(9, 1, lv_fill, &custom, a, 2);                  /* song 1 */
        }
        {
            double ra = 0, rb = 0;
            for (int i = 0; i < 32; i++) { ra += a[i][0] * a[i][0]; rb += b[i][0] * b[i][0]; }
            gain1 = 10.0f * log10f((float)(ra / rb));
        }
        CHECK(fabsf(gain1 - (T + 7.0f)) < 0.4f, "voice path: gain %.2f dB", gain1);
        sine_frame(b, -7.0f);
        memcpy(a, b, sizeof(a));
        apu_level_voice(9, 2, lv_fill, &custom, a, 2);                      /* song 2 starts */
        {
            double ra = 0, rb = 0;
            for (int i = 0; i < 32; i++) { ra += a[i][0] * a[i][0]; rb += b[i][0] * b[i][0]; }
            first = 10.0f * log10f((float)(ra / rb));
        }
        CHECK(fabsf(first - gain1) < 0.2f, "a new song starts at %.2f dB, the last settled at %.2f", first, gain1);
        printf("leveller: voice path: song 1 settled at %+.2f dB, song 2 starts at %+.2f dB\n", gain1, first);
        memcpy(a, b, sizeof(a));
        apu_level_voice(10, 1, lv_fill, &other, a, 2);
        CHECK(!memcmp(a, b, sizeof(a)), "a voice the classifier refuses was touched");
        apu_level_set_mode(APU_LEVEL_OFF);
        CHECK(!apu_level_active(), "off is active");
        memcpy(a, b, sizeof(a));
        apu_level_voice(9, 2, lv_fill, &custom, a, 2);
        CHECK(!memcmp(a, b, sizeof(a)), "off touched a voice");
        apu_level_set_mode(APU_LEVEL_MATCH);
        apu_level_set_classifier(NULL);
        CHECK(!apu_level_active(), "no classifier but active");
    }
}

/* What the two stages cost a sample. Printed, and bounded generously (a
 * Deck core does an order of magnitude less than 1 % on this). */
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void test_cost(void)
{
    static float f[6][APU_LIMIT_FRAME], blk[32][2];
    const int frames = 1500 * 20;           /* 20 s of audio */
    double t0, t_idle, t_hot, t_lv;
    volatile float sink = 0;
    apu_level_state st;

    apu_limit_reset();
    for (int c = 0; c < 6; c++) for (int i = 0; i < 32; i++) f[c][i] = 0.5f * frand();
    t0 = now_ns();
    for (int k = 0; k < frames; k++) { apu_limit(f, 6, 32); sink += f[0][0]; }
    t_idle = (now_ns() - t0) / ((double)frames * 32);
    for (int c = 0; c < 6; c++) for (int i = 0; i < 32; i++) f[c][i] = 1.6f * frand();
    t0 = now_ns();
    for (int k = 0; k < frames; k++) {
        for (int c = 0; c < 6; c++) for (int i = 0; i < 32; i++) f[c][i] = 1.6f * (float)((i * 7 + c + k) % 11 - 5) / 5.0f;
        apu_limit(f, 6, 32); sink += f[0][0];
    }
    t_hot = (now_ns() - t0) / ((double)frames * 32);
    apu_level_state_init(&st, -1000.0f);
    for (int i = 0; i < 32; i++) { blk[i][0] = 0.3f * frand(); blk[i][1] = 0.3f * frand(); }
    t0 = now_ns();
    for (int k = 0; k < frames; k++) { apu_level_frame(&st, blk, 2, APU_LEVEL_TARGET_DB); sink += blk[0][0]; }
    t_lv = (now_ns() - t0) / ((double)frames * 32);
    (void)sink;
    printf("cost: safety limiter %.2f ns a sample (6 channels, nothing over the ceiling; the loop is the whole cost),"
           " %.2f ns while limiting (incl. refilling the block); leveller %.2f ns a sample per custom voice (stereo)."
           " On a 48 kHz stream: limiter %.3f %%, leveller %.3f %% of a core\n",
           t_idle, t_hot, t_lv, t_idle * 48000e-7, t_lv * 48000e-7);
    CHECK(t_idle < 100.0 && t_lv < 200.0, "the stages cost too much a sample: %.1f / %.1f ns", t_idle, t_lv);
}

static void test_controls(void)
{
    static float ref[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], out[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    tvoice v;
    double r;

    memset(&v, 0, sizeof(v));
    v.channels = 1;
    memcpy(v.bin, k_bins[2], sizeof(v.bin));        /* FL FR C LFE SL SR + reverb */
    for (int b = 0; b < 6; b++) v.g[b] = 0.5f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        v.samples[i][0] = v.samples[i][1] = sinf((float)i * 0.7f);
    memset(ref, 0, sizeof(ref));
    old_mix(ref, &v);

    /* Gain: -6 dB on the voice's bus, nothing on another. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].gain_db = -6.0f;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    r = db(bin_rms(out[19]) / bin_rms(ref[19]));
    CHECK(fabs(r + 6.0) < 0.01, "gain -6 dB gave %.3f dB", r);
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 3, &v, out);
    CHECK(!memcmp(out, ref, sizeof(ref)), "a gain on one bus changed another");

    /* Mute, and solo of another bus. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].mute = 1;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(bin_rms(out[19]) == 0.0, "mute is not silent");
    apu_bus_config_neutral(&cfg);
    cfg.bus[2].solo = 1;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(bin_rms(out[19]) == 0.0, "a bus sounds while another is soloed");
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 2, &v, out);
    CHECK(!memcmp(out, ref, sizeof(ref)), "the soloed bus changed");

    /* Trims, route title: rear -3 dB, centre off, front and LFE as they were. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].trim_db[APU_BUS_TRIM_REAR] = -3.0f;
    cfg.bus[9].trim_db[APU_BUS_TRIM_CENTRE] = APU_BUS_OFF_DB;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(!memcmp(out[19], ref[19], sizeof(ref[0])) && !memcmp(out[24], ref[24], sizeof(ref[0])),
          "a rear trim changed the front or the LFE");
    r = db(bin_rms(out[14]) / bin_rms(ref[14]));
    CHECK(fabs(r + 3.0) < 0.01, "rear trim -3 dB gave %.3f dB", r);
    CHECK(bin_rms(out[16]) == 0.0, "centre trim off is not silent");

    /* Sends: the voice's level (its loudest send, 0.5) on the speakers
     * asked for and nowhere else. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[4].route = APU_BUS_ROUTE_SENDS;
    cfg.bus[4].send_db[0] = -6.0f;                  /* FL */
    cfg.bus[4].send_db[1] = APU_BUS_OFF_DB;
    cfg.bus[4].send_db[2] = 0.0f;                   /* C */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 4, &v, out);
    CHECK(fabs(db(bin_rms(out[2]) / bin_rms(ref[19]))) < 0.01, "send C 0 dB is not the voice's level");
    CHECK(fabs(db(bin_rms(out[0]) / bin_rms(ref[19])) + 6.0) < 0.01, "send FL -6 dB is off");
    for (int b = 0; b < NUM_MIXBINS; b++)
        if (b != 0 && b != 2)
            CHECK(bin_rms(out[b]) == 0.0, "route sends left signal in bin %d", b);
    /* The same sends on a stereo output: C at -3 dB into both sides. */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 2, 4, &v, out);
    CHECK(fabs(db(bin_rms(out[1]) / bin_rms(ref[19])) + 3.0) < 0.02, "stereo fold of the centre send is off");
    CHECK(bin_rms(out[0]) > bin_rms(out[1]), "stereo fold lost the FL send");

    /* LFE send with an 80 Hz crossover: 40 Hz passes, 1 kHz does not. */
    for (int tone = 0; tone < 2; tone++) {
        const double hz = tone ? 1000.0 : 40.0;
        double in_sq = 0, out_sq = 0;
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].lfe_send_db = 0.0f;
        cfg.bus[9].lfe_crossover_hz = 80;
        apu_bus_set_config(&cfg);
        apu_bus_enable(1);
        for (int frame = 0; frame < 1500; frame++) {
            struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                v.samples[i][0] = v.samples[i][1] =
                    (float)sin(2.0 * M_PI * hz * (frame * 32 + i) / 48000.0);
            memset(out, 0, sizeof(out));
            v.g[3] = 0.0f;                          /* the title's own LFE send shut */
            apu_bus_mix_voice(rt, 9, out, (const float (*)[2])v.samples, 1, v.bin, v.g);
            apu_bus_frame_end(rt, out);
            if (frame >= 750) {
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    in_sq += 0.25 * (double)v.samples[i][0] * v.samples[i][0];
                    out_sq += (double)out[3][i] * out[3][i];
                }
            }
        }
        r = 10.0 * log10(out_sq / in_sq + 1e-30);
        if (tone) CHECK(r < -80.0, "80 Hz crossover leaves 1 kHz at %.1f dB", r);
        else      CHECK(fabs(r) < 0.5, "80 Hz crossover has 40 Hz at %.2f dB", r);
        /* Let the filter and the send run out before the next tone. */
        for (int frame = 0; frame < 3000; frame++) {
            struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
            memset(out, 0, sizeof(out));
            apu_bus_frame_end(rt, out);
        }
    }
    v.g[3] = 0.5f;
}

static void test_output(void)
{
    static float feeds[6][APU_BUS_FRAME];
    apu_bus_config cfg;
    float mx;

    /* Neutral: the stage is not active and does not touch the feeds. */
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
    apu_bus_enable(1);
    CHECK(!apu_bus_output_active(), "the neutral output stage says it is active");

    /* Speaker trim -6 dB on FR, 1 ms delay on SL. */
    cfg.speaker_trim_db[1] = -6.0f;
    cfg.speaker_delay_ms[4] = 1.0f;
    apu_bus_set_config(&cfg);
    CHECK(apu_bus_output_active(), "trim and delay do not activate the stage");
    {
        int at = -1;
        for (int frame = 0; frame < 4 && at < 0; frame++) {
            memset(feeds, 0, sizeof(feeds));
            if (frame == 0) { feeds[1][0] = 1.0f; feeds[4][0] = 1.0f; }
            apu_bus_output(feeds, 6, APU_BUS_FRAME);
            if (frame == 0)
                CHECK(fabsf(feeds[1][0] - 0.5012f) < 0.001f, "speaker trim -6 dB gave %f", feeds[1][0]);
            for (int i = 0; i < APU_BUS_FRAME; i++)
                if (feeds[4][i] != 0.0f) at = frame * APU_BUS_FRAME + i;
        }
        CHECK(at == 48, "1 ms of delay is %d samples, not 48", at);
    }

    /* Soft clip and the peak limiter: a 6 dB over-full-scale sine stays
     * under the ceiling; a quiet one passes at its level. */
    for (int mode = APU_BUS_LIM_SOFT; mode <= APU_BUS_LIM_PEAK; mode++) {
        double sq_in = 0, sq_out = 0;
        apu_bus_config_neutral(&cfg);
        cfg.limiter = mode;
        cfg.ceiling_db = -1.0f;
        apu_bus_set_config(&cfg);
        mx = 0.0f;
        for (int frame = 0; frame < 600; frame++) {
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[1][i] =
                    2.0f * (float)sin(2.0 * M_PI * 440.0 * (frame * 32 + i) / 48000.0);
            apu_bus_output(feeds, 2, APU_BUS_FRAME);
            for (int i = 0; i < APU_BUS_FRAME; i++)
                if (fabsf(feeds[0][i]) > mx) mx = fabsf(feeds[0][i]);
        }
        CHECK(mx <= 0.8913f + 1e-4f, "limiter mode %d let %.4f through a -1 dB ceiling", mode, mx);
        CHECK(mx > 0.6f, "limiter mode %d output is too low (%.4f)", mode, mx);
        for (int frame = 0; frame < 3000; frame++) {    /* release, then measure */
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[1][i] =
                    0.1f * (float)sin(2.0 * M_PI * 440.0 * (frame * 32 + i) / 48000.0);
            if (frame >= 2000)
                for (int i = 0; i < APU_BUS_FRAME; i++) sq_in += (double)feeds[0][i] * feeds[0][i];
            apu_bus_output(feeds, 2, APU_BUS_FRAME);
            if (frame >= 2000)
                for (int i = 0; i < APU_BUS_FRAME; i++) sq_out += (double)feeds[0][i] * feeds[0][i];
        }
        CHECK(fabs(10.0 * log10(sq_out / sq_in)) < 0.05,
              "limiter mode %d changes a -20 dBFS signal by %.3f dB", mode, 10.0 * log10(sq_out / sq_in));
    }

    /* LFE low-pass 120 Hz: 1 kHz on the LFE feed is gone, FL untouched. */
    {
        double sq = 0, sq_fl = 0;
        apu_bus_config_neutral(&cfg);
        cfg.lfe_lowpass_hz = 120;
        apu_bus_set_config(&cfg);
        for (int frame = 0; frame < 1500; frame++) {
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[3][i] = (float)sin(2.0 * M_PI * 1000.0 * (frame * 32 + i) / 48000.0);
            apu_bus_output(feeds, 6, APU_BUS_FRAME);
            if (frame >= 750)
                for (int i = 0; i < APU_BUS_FRAME; i++) {
                    sq += (double)feeds[3][i] * feeds[3][i];
                    sq_fl += (double)feeds[0][i] * feeds[0][i];
                }
        }
        CHECK(10.0 * log10(sq / sq_fl) < -70.0, "LFE low-pass leaves 1 kHz at %.1f dB", 10.0 * log10(sq / sq_fl));
        CHECK(fabs(10.0 * log10(sq_fl / (750.0 * 32 * 0.5))) < 0.01, "LFE low-pass touched FL");
    }
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

static void test_duck(void)
{
    static float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    apu_bus_meters m;
    tvoice src, tgt;
    double level_at[4] = { 0, 0, 0, 0 };

    memset(&src, 0, sizeof(src));
    src.channels = 1;
    memcpy(src.bin, k_bins[2], sizeof(src.bin));
    src.g[0] = 0.5f;
    tgt = src;
    memcpy(tgt.bin, k_bins[0], sizeof(tgt.bin));
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        src.samples[i][0] = src.samples[i][1] = (i & 1) ? 0.5f : -0.5f;
        tgt.samples[i][0] = tgt.samples[i][1] = (i & 2) ? 0.5f : -0.5f;
    }
    apu_bus_config_neutral(&cfg);
    cfg.duck.enabled = 1;
    cfg.duck.source = 9;
    cfg.duck.target = 0;
    cfg.duck.threshold_db = -30.0f;
    cfg.duck.depth_db = 6.0f;
    cfg.duck.attack_ms = 20.0f;
    cfg.duck.release_ms = 200.0f;
    apu_bus_set_config(&cfg);
    apu_bus_enable(1);
    /* 0.2 s of both (the source at -18 dBFS: over the threshold), then
     * 2 s of the target alone. */
    for (int frame = 0; frame < 3300; frame++) {
        struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
        memset(bins, 0, sizeof(bins));
        apu_bus_mix_voice(rt, 0, bins, (const float (*)[2])tgt.samples, 1, tgt.bin, tgt.g);
        if (frame < 300)
            apu_bus_mix_voice(rt, 9, bins, (const float (*)[2])src.samples, 1, src.bin, src.g);
        apu_bus_frame_end(rt, bins);
        if (frame == 0)    level_at[0] = bin_rms(bins[0]);
        if (frame == 299)  level_at[1] = bin_rms(bins[0]);
        if (frame == 330)  level_at[2] = bin_rms(bins[0]);     /* 20 ms after: still down */
        if (frame == 3299) level_at[3] = bin_rms(bins[0]);
    }
    CHECK(fabs(db(level_at[1] / level_at[0]) + 6.0) < 0.1,
          "ducking: target at %.2f dB after 200 ms of the source, not -6", db(level_at[1] / level_at[0]));
    CHECK(db(level_at[2] / level_at[0]) < -4.5, "ducking released at once (%.2f dB)", db(level_at[2] / level_at[0]));
    CHECK(fabs(db(level_at[3] / level_at[0])) < 0.05, "ducking did not release (%.2f dB)", db(level_at[3] / level_at[0]));
    apu_bus_get_meters(&m);
    CHECK(m.duck_db < 0.1f, "the duck meter is stuck at %.2f dB", m.duck_db);
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

static void test_preset(void)
{
    static char text[32768], text2[32768];
    apu_bus_config a, b;
    char err[160];

    apu_bus_config_neutral(&a);
    a.bus[0].route = APU_BUS_ROUTE_SENDS;
    a.bus[0].send_db[4] = APU_BUS_OFF_DB;
    a.bus[0].send_db[5] = -12.5f;
    a.bus[8].gain_db = -3.5f;
    a.bus[8].trim_db[APU_BUS_TRIM_REAR] = 2.0f;
    a.bus[9].lfe_send_db = -6.0f;
    a.bus[9].lfe_crossover_hz = 100;
    a.bus[5].mute = 1;
    a.bus[4].route = APU_BUS_ROUTE_PAN;
    a.bus[4].pan_x = -0.25f; a.bus[4].pan_y = 0.5f; a.bus[4].pan_width = 0.75f; a.bus[4].pan_divergence = 0.5f;
    a.bus[7].field_width = 1.5f; a.bus[7].field_divergence = -0.25f;
    a.speaker_trim_db[2] = -1.5f;
    a.speaker_delay_ms[5] = 3.5f;
    a.limiter = APU_BUS_LIM_PEAK;
    a.ceiling_db = -1.0f;
    a.lfe_lowpass_hz = 120;
    a.duck.enabled = 1;
    a.duck.source = 9;
    a.duck.target = 0;
    a.duck.depth_db = 4.0f;
    apu_bus_config_write(&a, text, sizeof(text));
    apu_bus_config_neutral(&b);
    CHECK(apu_bus_config_parse(text, &b, err, sizeof(err)) == 0 && !*err, "written preset does not parse: %s", err);
    apu_bus_config_write(&b, text2, sizeof(text2));
    CHECK(!strcmp(text, text2), "a preset does not read back as written");
    CHECK(b.bus[0].send_db[4] <= APU_BUS_OFF_DB && b.bus[0].send_db[5] == -12.5f, "sends did not read back");
    CHECK(b.duck.source == 9 && b.duck.target == 0 && b.duck.enabled, "the duck slot did not read back");
    CHECK(!apu_bus_config_is_neutral(&b), "a changed preset reads as neutral");
    CHECK(b.bus[4].route == APU_BUS_ROUTE_PAN && b.bus[4].pan_x == -0.25f && b.bus[4].pan_width == 0.75f
          && b.bus[7].field_width == 1.5f && b.bus[7].field_divergence == -0.25f, "pan and field keys did not read back");

    /* By hand: comments, an unknown key (reported, not fatal), a bad line. */
    apu_bus_config_neutral(&b);
    CHECK(apu_bus_config_parse("# mine\n[bus.tyres_road_wind]\ngain_db = -2   # quieter\nwobble = 3\n"
                               "[speaker.LFE]\ntrim_db = 4\n", &b, err, sizeof(err)) == 0,
          "a hand-written preset does not parse");
    CHECK(b.bus[8].gain_db == -2.0f && b.speaker_trim_db[3] == 4.0f, "hand-written values not taken");
    CHECK(strstr(err, "wobble") != NULL, "an unknown key is not reported (%s)", err);
    apu_bus_config_neutral(&a);
    a.bus[1].gain_db = 1.0f;
    b = a;
    CHECK(apu_bus_config_parse("[master]\nlimiter soft\n", &b, err, sizeof(err)) < 0, "a line without = parses");
    CHECK(!memcmp(&a, &b, sizeof(a)), "a failed parse changed the configuration");
    CHECK(apu_bus_find("Tyres/road/wind") == 8 && apu_bus_find("impacts_crash") == 9 &&
          apu_bus_find("nothing") < 0, "bus keys");
}

/* The pan law, route "pan" and the field shaping. */
static void test_pan(void)
{
    static float out[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    tvoice v;
    float g[5], g2[2];
    double worst = 0.0;

    /* The law: the squares sum to 1 everywhere, for any divergence. */
    for (int ix = -10; ix <= 10; ix++)
        for (int iy = -10; iy <= 10; iy++)
            for (int id = 0; id <= 4; id++) {
                double p = 0;
                apu_bus_pan_gains(ix / 10.0f, iy / 10.0f, id / 4.0f, g);
                for (int k = 0; k < 5; k++) p += (double)g[k] * g[k];
                if (fabs(db(sqrt(p))) > worst) worst = fabs(db(sqrt(p)));
            }
    CHECK(worst < 0.001, "pan law: the power is off by %.4f dB somewhere on the grid", worst);
    /* The places that have a name. Order: FL FR C SL SR. */
    apu_bus_pan_gains(0, 1, 0, g);
    CHECK(fabsf(g[2] - 1) < 1e-5f && g[0] < 1e-5f && g[1] < 1e-5f && g[3] < 1e-5f, "front centre is not the centre speaker");
    apu_bus_pan_gains(-1, 1, 0, g);
    CHECK(fabsf(g[0] - 1) < 1e-5f && g[2] < 1e-4f && g[3] < 1e-4f, "front left corner is not FL");
    apu_bus_pan_gains(1, -1, 0, g);
    CHECK(fabsf(g[4] - 1) < 1e-5f && g[1] < 1e-4f && g[3] < 1e-4f, "rear right corner is not SR");
    apu_bus_pan_gains(0, -1, 0, g);
    CHECK(fabsf(g[3] - 0.70711f) < 1e-4f && fabsf(g[4] - 0.70711f) < 1e-4f && g[2] < 1e-5f, "rear centre is not SL + SR at -3 dB");
    apu_bus_pan_gains(1, 0, 0, g);
    CHECK(fabsf(g[1] - 0.70711f) < 1e-4f && fabsf(g[4] - 0.70711f) < 1e-4f, "the right wall's middle is not FR + SR at -3 dB");
    apu_bus_pan_gains(0, 0, 0, g);
    for (int k = 0; k < 5; k++) CHECK(fabsf(g[k] - 0.44721f) < 1e-4f, "the middle of the room is not all five at -7 dB");
    apu_bus_pan_gains(0, 1, 1, g);
    CHECK(g[2] < 1e-5f && fabsf(g[0] - 0.70711f) < 1e-4f && fabsf(g[1] - 0.70711f) < 1e-4f, "divergence 1 is not a phantom centre");
    apu_bus_pan_gains(0, 1, 0.5f, g);
    CHECK(fabsf(g[2] - 0.70711f) < 1e-4f && fabsf(g[0] - 0.5f) < 1e-4f, "divergence 0.5 does not halve the centre's power");
    apu_bus_pan_gains(0.5f, 1, 0, g);
    CHECK(g[2] > 0.3f && g[1] > 0.3f && g[0] < 1e-5f && g[4] < 1e-5f, "between C and FR other speakers play");
    for (int ix = -10; ix <= 10; ix++) {
        apu_bus_pan_gains_stereo(ix / 10.0f, g2);
        CHECK(fabs(db(sqrt((double)g2[0] * g2[0] + (double)g2[1] * g2[1]))) < 0.001, "stereo pan law power at %d", ix);
    }
    apu_bus_pan_gains_stereo(-1, g2);
    CHECK(fabsf(g2[0] - 1) < 1e-6f && fabsf(g2[1]) < 1e-6f, "stereo pan: hard left");

    /* Through the mixer. A stereo voice whose channels are orthogonal (a
     * sine and a cosine, whole periods in a frame), so powers add; its
     * loudest send is 0.5. */
    memset(&v, 0, sizeof(v));
    v.channels = 2;
    memcpy(v.bin, k_bins[0], sizeof(v.bin));
    v.g[0] = 0.5f; v.g[1] = 0.5f; v.g[2] = 0.3f; v.g[3] = 0.3f; v.g[5] = 0.1f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        v.samples[i][0] = (float)sin(2.0 * M_PI * 2.0 * i / 32.0);
        v.samples[i][1] = (float)cos(2.0 * M_PI * 2.0 * i / 32.0);
    }
    for (int mono = 0; mono < 2; mono++) {
        double want = 0.5 * sqrt(mono ? 0.5 : 1.0), worst_p = 0;      /* rms of both channels, or of one */
        tvoice t = v;
        if (mono) {
            t.channels = 1;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) t.samples[i][1] = t.samples[i][0];
        }
        for (int six = 0; six < 2; six++)
            for (int ix = -4; ix <= 4; ix++)
                for (int iy = -4; iy <= 4; iy++)
                    for (int iw = 0; iw <= 4; iw++) {
                        double p = 0, d;
                        apu_bus_config_neutral(&cfg);
                        cfg.bus[0].route = APU_BUS_ROUTE_PAN;
                        cfg.bus[0].pan_x = ix / 4.0f;
                        cfg.bus[0].pan_y = iy / 4.0f;
                        cfg.bus[0].pan_width = iw / 4.0f;
                        cfg.bus[0].pan_divergence = (float)((ix + iy + 8) % 3) / 2.0f;
                        memset(out, 0, sizeof(out));
                        run_one(&cfg, six ? 6 : 2, 0, &t, out);
                        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
                        d = fabs(db(sqrt(p) / want));
                        if (d > worst_p) worst_p = d;
                        if (six) CHECK(bin_rms(out[3]) == 0.0, "route pan feeds the LFE");
                    }
        CHECK(worst_p < 0.1, "route pan (%s source): total power off by %.3f dB somewhere", mono ? "mono" : "stereo", worst_p);
    }
    /* Front centre, full width: the left channel is FL and nothing else,
     * the right FR: the stereo picture as it is. Width 0: both in C. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[0].route = APU_BUS_ROUTE_PAN;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 0, &v, out);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        CHECK(fabsf(out[0][i] - 0.5f * v.samples[i][0]) < 1e-6f, "pan, width 1: FL is not the left channel");
        CHECK(fabsf(out[1][i] - 0.5f * v.samples[i][1]) < 1e-6f, "pan, width 1: FR is not the right channel");
    }
    CHECK(bin_rms(out[2]) < 1e-6 && bin_rms(out[4]) < 1e-6, "pan, width 1 at the front: centre or rears play");
    cfg.bus[0].pan_width = 0.0f;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 0, &v, out);
    CHECK(bin_rms(out[0]) < 1e-6 && fabs(db(bin_rms(out[2]) / 0.5)) < 0.01, "pan, width 0: not the two channels in the centre");
    /* Stereo output, x 0, width 1: left to L, right to R, as shipped; and
     * its mono sum is the source's mono sum (nothing cancels or doubles). */
    cfg.bus[0].pan_width = 1.0f;
    cfg.bus[0].pan_y = -0.5f;                           /* ignored on a stereo output */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 2, 0, &v, out);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        CHECK(fabsf(out[0][i] - 0.5f * v.samples[i][0]) < 1e-6f, "stereo output: L is not the left channel");
        CHECK(fabsf(out[0][i] + out[1][i] - 0.5f * (v.samples[i][0] + v.samples[i][1])) < 1e-6f,
              "stereo output: the mono sum is not the source's");
    }
    /* A mono voice with width: on the neighbours too, in phase. */
    {
        tvoice t = v;
        t.channels = 1;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) t.samples[i][1] = t.samples[i][0];
        apu_bus_config_neutral(&cfg);
        cfg.bus[4].route = APU_BUS_ROUTE_PAN;
        cfg.bus[4].pan_width = 0.5f;
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 4, &t, out);
        CHECK(bin_rms(out[0]) > 0.05 && bin_rms(out[1]) > 0.05 && bin_rms(out[2]) > 0.05,
              "a mono voice with width 0.5 at the front does not use FL, C and FR");
        CHECK(fabs(bin_rms(out[0]) - bin_rms(out[1])) < 1e-6, "its FL and FR differ");
    }

    /* Field shaping: the pair's width and the centre's divergence, power
     * kept, over a grid of pans. */
    {
        double worst_w = 0, worst_d = 0;
        for (int il = 0; il <= 8; il++)
            for (int ir = 0; ir <= 8; ir++)
                for (int iw = 0; iw <= 8; iw++) {
                    float l = il / 8.0f, r = ir / 8.0f, c = (float)((il + ir) % 5) / 4.0f;
                    float l2 = l, r2 = r, c2 = c;
                    const double p0 = (double)l * l + (double)r * r;
                    if (p0 == 0) continue;
                    apu_bus_field_width(iw / 4.0f, &l2, &r2);
                    if (fabs(db(sqrt(((double)l2 * l2 + (double)r2 * r2) / p0))) > worst_w)
                        worst_w = fabs(db(sqrt(((double)l2 * l2 + (double)r2 * r2) / p0)));
                    CHECK(l2 >= 0 && r2 >= 0, "field width made a negative gain");
                    if (iw == 4) CHECK(fabsf(l2 - l) < 1e-6f && fabsf(r2 - r) < 1e-6f, "field width 1 changes the pair");
                    if (iw == 0) CHECK(fabsf(l2 - r2) < 1e-6f, "field width 0 is not the centre line");
                    if (iw > 4 && l > r) CHECK(l2 >= l - 1e-6f && r2 <= r + 1e-6f, "field width > 1 does not widen");
                    l2 = l; r2 = r;
                    apu_bus_field_divergence((iw - 4) / 4.0f, &l2, &r2, &c2);
                    {
                        const double p1 = p0 + (double)c * c, p2 = (double)l2 * l2 + (double)r2 * r2 + (double)c2 * c2;
                        if (fabs(db(sqrt(p2 / p1))) > worst_d) worst_d = fabs(db(sqrt(p2 / p1)));
                    }
                    if (iw == 8) CHECK(c2 < 1e-6f, "field divergence 1 leaves a centre send");
                    if (iw == 0) CHECK(l2 < 1e-3f || r2 < 1e-3f, "field divergence -1 leaves a common part in FL and FR");
                }
        CHECK(worst_w < 0.01 && worst_d < 0.01, "field shaping: power off by %.4f / %.4f dB", worst_w, worst_d);
    }
    /* Through the mixer: a positioned voice, mostly front left. */
    memset(&v, 0, sizeof(v));
    v.channels = 1;
    memcpy(v.bin, k_bins[2], sizeof(v.bin));            /* FL FR C LFE SL SR, reverb */
    v.g[0] = 0.5f; v.g[1] = 0.1f; v.g[2] = 0.2f; v.g[3] = 0.05f; v.g[4] = 0.3f; v.g[5] = 0.1f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        v.samples[i][0] = v.samples[i][1] = sinf((float)i * 0.7f);
    {
        static float ref[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
        double p_ref = 0, p = 0;
        memset(ref, 0, sizeof(ref));
        old_mix(ref, &v);
        for (int b = 0; b < NUM_MIXBINS; b++) p_ref += bin_rms(ref[b]) * bin_rms(ref[b]);
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].field_width = 0.0f;
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 9, &v, out);
        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
        CHECK(fabs(bin_rms(out[19]) - bin_rms(out[20])) < 1e-6 && fabs(bin_rms(out[14]) - bin_rms(out[15])) < 1e-6,
              "field width 0: the pairs are not centred");
        CHECK(!memcmp(out[16], ref[16], sizeof(ref[0])) && !memcmp(out[24], ref[24], sizeof(ref[0])),
              "field width touched the centre or the LFE");
        CHECK(fabs(db(sqrt(p / p_ref))) < 0.01, "field width 0 changed the voice's power by %.3f dB", db(sqrt(p / p_ref)));
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].field_divergence = 1.0f;
        memset(out, 0, sizeof(out));
        p = 0;
        run_one(&cfg, 6, 9, &v, out);
        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
        CHECK(bin_rms(out[16]) < 1e-6 && bin_rms(out[19]) > bin_rms(ref[19]), "field divergence 1: the centre send is not in FL/FR");
        CHECK(fabs(db(sqrt(p / p_ref))) < 0.01, "field divergence changed the voice's power by %.3f dB", db(sqrt(p / p_ref)));
        /* Neutral values are not on the path at all: the same bytes as
         * the loop, whatever the pan settings of the unused route say. */
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].pan_x = -0.7f; cfg.bus[9].pan_y = 0.2f; cfg.bus[9].pan_width = 0.3f; cfg.bus[9].pan_divergence = 1.0f;
        CHECK(apu_bus_config_is_neutral(&cfg), "pan settings of a bus on the title's route make it non-neutral");
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 9, &v, out);
        CHECK(!memcmp(out, ref, sizeof(ref)), "a neutral bus with pan settings is not the loop's sum");
        cfg.bus[9].field_width = 1.01f;
        CHECK(!apu_bus_config_is_neutral(&cfg), "a field width of 1.01 reads as neutral");
    }
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

/* A voice's bus: what the classifier is told about a return voice (one
 * playing the GP model's export, apu_gp.h), and the answer for a voice that
 * is only the input of an in-place effect. */
static int s_seen_return;
static int classify_return(const apu_bus_voice *v)
{
    s_seen_return = v->gp_return;
    if (v->gp_return == 1 + 17) return 4;           /* Player engine */
    if (v->gp_return == 1 + 18) return 5;           /* Player exhaust */
    for (int b = 0; b < 8; b++)
        if (v->bins[b] == 17) return APU_BUS_BYPASS;
    return 99;                                      /* not a bus: "Other" */
}
static int fill_return(void *ctx, apu_bus_voice *v)
{
    const int *what = (const int *)ctx;
    v->gp_return = what[0];
    v->bins[5] = (uint8_t)what[1];
    return 1;
}

static void test_return_voices(void)
{
    int engine[2] = { 1 + 17, 19 }, exhaust[2] = { 1 + 18, 19 };
    int feeder[2] = { 0, 17 }, plain[2] = { 0, 19 };

    apu_bus_set_classifier(classify_return);
    apu_bus_reclassify();
    CHECK(apu_bus_voice_bus(68, 1, 0xBB8300, fill_return, engine) == 4 && s_seen_return == 18,
          "the return voice of bin 17 is not the engine bus");
    CHECK(apu_bus_voice_bus(69, 1, 0xBB8380, fill_return, exhaust) == 5,
          "the return voice of bin 18 is not the exhaust bus");
    CHECK(apu_bus_voice_bus(70, 1, 0x100000, fill_return, feeder) == APU_BUS_BYPASS,
          "an effect's input voice does not bypass the buses");
    /* Cached: not asked again for the same voice, serial and buffer. */
    s_seen_return = -1;
    CHECK(apu_bus_voice_bus(70, 1, 0x100000, fill_return, plain) == APU_BUS_BYPASS && s_seen_return == -1,
          "a bypassed voice was classified again");
    CHECK(apu_bus_voice_bus(71, 1, 0x200000, fill_return, plain) == 12,
          "an answer that is no bus is not Other");
    apu_bus_set_classifier(NULL);
    apu_bus_reclassify();
}

#include <unistd.h>
#include "audition.inc"

int main(void)
{
    dsp_setup();
    apu_bus_set_names(k_names, 13);
    test_identity();
    test_limiter_identity();
    test_limiter();
    test_leveller();
    test_leveller_old();
    test_cost();
    test_controls();
    test_output();
    test_duck();
    test_preset();
    test_pan();
    test_return_voices();
    test_audition();
    /* And once more after everything else ran, now with the audition
     * recording on: neutral is still today. */
    test_identity();
    if (s_fail) { fprintf(stderr, "%d check(s) failed\n", s_fail); return 1; }
    puts("PASS: the neutral mixer is bit-identical; controls, output stage, ducking and presets as specified.");
    return 0;
}

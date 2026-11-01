/*
 * apu_lowpass test -- the voice low-pass (src/apu/apu_lowpass.h) over the
 * whole range the voice processor can ask for: cutoff 2^(FC0/4096) clamped
 * to 1/256..1, resonance FC1/32768 clamped to 0.079407..1.
 *
 *   level      the response to a constant settles at sqrt(q/2 + 0.01)
 *   no delay   an impulse answers in the same sample, with level * f * f
 *   corner     with q = 1 the response at the corner Fs/pi * asin(f/2) is
 *              the passband level (a state variable low-pass at its corner
 *              has gain 1/q)
 *   stable     full-scale noise, then silence: the output stays finite and
 *              bounded and dies away, at every setting
 *   moving     a setting that changes every 32 samples (as the voice
 *              processor does it) across the whole range stays bounded
 *   tidy       a state that is not a number is cleared
 */
#include "apu_lowpass.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const double PI = 3.14159265358979323846;

static uint32_t g_seed = 0x105u;
static float noise(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 8388608.0f - 1.0f;    /* -1..1 */
}

static float cutoff_of(int fc0)     { float c = powf(2.0f, fc0 / 4096.0f); return c < 0.003906f ? 0.003906f : c > 1.0f ? 1.0f : c; }
static float resonance_of(int fc1)  { float r = fc1 / 32768.0f; return r < 0.079407f ? 0.079407f : r > 1.0f ? 1.0f : r; }

int main(void)
{
    /* level and no delay */
    for (int qi = 0; qi <= 8; qi++) {
        float q = 0.079407f + (1.0f - 0.079407f) * qi / 8.0f;
        float f = 0.25f;
        ApuLowpass s; memset(&s, 0, sizeof s);
        ApuLowpassCoef c = apu_lowpass_coef(f, q);
        float first = apu_lowpass_step(&s, &c, 1.0f);
        float want = sqrtf(0.5f * q + 0.01f);
        CHECK(fabsf(first - want * f * f) < 1e-6f, "q=%g: impulse answers %g, want %g", q, first, want * f * f);
        memset(&s, 0, sizeof s);
        float y = 0;
        for (int i = 0; i < 20000; i++) y = apu_lowpass_step(&s, &c, 1.0f);
        CHECK(fabsf(y - want) < 1e-4f, "q=%g: constant settles at %g, want %g", q, y, want);
    }

    /* corner: q = 1, a sine at Fs/pi * asin(f/2) comes out at the level */
    for (int k = 0; k < 5; k++) {
        static const float fs[5] = { 0.01f, 0.05f, 0.2f, 0.5f, 0.9f };
        float f = fs[k];
        double w = 2.0 * asin(f / 2.0);         /* radians a sample */
        ApuLowpass s; memset(&s, 0, sizeof s);
        ApuLowpassCoef c = apu_lowpass_coef(f, 1.0f);
        float peak = 0;
        for (int i = 0; i < 60000; i++) {
            float y = apu_lowpass_step(&s, &c, (float)(0.5 * sin(w * i)));
            if (i > 50000 && fabsf(y) > peak) peak = fabsf(y);
        }
        float want = 0.5f * sqrtf(0.51f);
        CHECK(fabsf(peak / want - 1.0f) < 0.02f, "f=%g: corner gain %g of the passband level", f, peak / want);
    }

    /* stable at every setting: FC0 from below the clamp to 0, FC1 over its range */
    float worst = 0;
    for (int fc0 = -34000; fc0 <= 0; fc0 += 500) {
        for (int fc1 = 0; fc1 <= 0x8000; fc1 += 0x400) {
            ApuLowpass s; memset(&s, 0, sizeof s);
            ApuLowpassCoef c = apu_lowpass_coef(cutoff_of(fc0), resonance_of(fc1));
            float peak = 0, y = 0;
            for (int i = 0; i < 4000; i++) {
                y = apu_lowpass_step(&s, &c, noise());
                if (!isfinite(y)) break;
                if (fabsf(y) > peak) peak = fabsf(y);
            }
            CHECK(isfinite(y), "fc0=%d fc1=%d: not finite", fc0, fc1);
            if (peak > worst) worst = peak;
            float e0 = fabsf(s.low) + fabsf(s.band), e1;
            for (int i = 0; i < 48000; i++) apu_lowpass_step(&s, &c, 0.0f);
            e1 = fabsf(s.low) + fabsf(s.band);
            CHECK(e1 <= e0 * 0.5f + 1e-12f, "fc0=%d fc1=%d: does not die away (%g -> %g)", fc0, fc1, e0, e1);
        }
    }
    /* White noise through a resonance of Q 12.6 at the passband level 0.22
     * cannot reach 4; a loop that was going unstable would. */
    CHECK(worst < 4.0f, "noise peak %g", worst);
    printf("noise peak over the range: %.3f\n", worst);

    /* moving setting */
    {
        ApuLowpass s; memset(&s, 0, sizeof s);
        float peak = 0;
        for (int frame = 0; frame < 20000; frame++) {
            g_seed = g_seed * 1664525u + 1013904223u;
            int fc0 = -(int)((g_seed >> 8) % 34000u);
            g_seed = g_seed * 1664525u + 1013904223u;
            int fc1 = (int)((g_seed >> 8) % 0x8001u);
            ApuLowpassCoef c = apu_lowpass_coef(cutoff_of(fc0), resonance_of(fc1));
            apu_lowpass_tidy(&s);
            for (int i = 0; i < 32; i++) {
                float y = apu_lowpass_step(&s, &c, noise());
                if (!(fabsf(y) <= peak)) peak = fabsf(y);
            }
        }
        CHECK(isfinite(peak) && peak < 8.0f, "moving setting: peak %g", peak);
        printf("moving setting peak: %.3f\n", peak);
    }

    /* a caller outside the voice processor's range is pulled inside the limit */
    {
        ApuLowpass s; memset(&s, 0, sizeof s);
        ApuLowpassCoef c = apu_lowpass_coef(5.0f, 0.01f);
        float y = 0;
        for (int i = 0; i < 4000; i++) y = apu_lowpass_step(&s, &c, noise());
        for (int i = 0; i < 400000; i++) y = apu_lowpass_step(&s, &c, 0.0f);
        CHECK(isfinite(y) && fabsf(y) < 1e-3f, "out-of-range setting: %g", y);
    }

    /* tidy */
    {
        ApuLowpass s; s.low = NAN; s.band = 1.0f;
        apu_lowpass_tidy(&s);
        CHECK(s.low == 0.0f && s.band == 0.0f, "tidy leaves a NaN");
        s.low = 0.25f; s.band = -0.5f;
        apu_lowpass_tidy(&s);
        CHECK(s.low == 0.25f && s.band == -0.5f, "tidy touches a live state");
    }

    printf(g_fail ? "apu_lowpass: %d FAILED\n" : "apu_lowpass: ok\n", g_fail);
    return g_fail ? 1 : 0;
}

/* The safety limiter on the final output (apu_limit.h). */
#include "apu_limit.h"

#include <math.h>

static int   s_on = 1;
static float s_gain = 1.0f;
static unsigned long s_engaged;

/* 10^(-0.5/20); the release is 1 - exp(-1 / (0.050 s * 48000)). */
#define CEIL_LIN  0.94406088f
#define REL_COEF  0.00041649f

void apu_limit_enable(int on) { s_on = on != 0; }
int  apu_limit_enabled(void) { return s_on; }

static float s_fold_gain = 1.0f;

void apu_limit_fold16(int32_t *l, int32_t *r)
{
    const float c16 = CEIL_LIN * 32768.0f;          /* 30934.4 */
    float g = s_fold_gain, pk;
    int32_t a, b;

    if (!s_on)
        return;
    a = *l < 0 ? -*l : *l;
    b = *r < 0 ? -*r : *r;
    pk = (float)(a > b ? a : b);
    if (g < 1.0f) {
        g += (1.0f - g) * REL_COEF;
        if (g > 0.9998f) g = 1.0f;
    }
    if (pk * g > c16)
        g = c16 / pk;
    s_fold_gain = g;
    if (g != 1.0f) {
        *l = (int32_t)((float)*l * g);
        *r = (int32_t)((float)*r * g);
    }
}

void apu_limit_reset(void)
{
    s_fold_gain = 1.0f;
    s_gain = 1.0f;
    s_engaged = 0;
}

float apu_limit_reduction_db(void)
{
    return s_gain >= 1.0f ? 0.0f : -20.0f * log10f(s_gain);
}

unsigned long apu_limit_engaged(void) { return s_engaged; }

void apu_limit(float (*feeds)[APU_LIMIT_FRAME], int nch, int n)
{
    float g = s_gain;

    if (!s_on)
        return;
    if (n > APU_LIMIT_FRAME)
        n = APU_LIMIT_FRAME;
    for (int i = 0; i < n; i++) {
        float pk = 0.0f;
        for (int c = 0; c < nch; c++) {
            const float a = fabsf(feeds[c][i]);
            if (a > pk) pk = a;
        }
        if (g < 1.0f) {
            g += (1.0f - g) * REL_COEF;
            if (g > 0.9998f) g = 1.0f;          /* back to the exact pass-through (-0.002 dB) */
        }
        if (pk * g > CEIL_LIN)
            g = CEIL_LIN / pk;                  /* instant attack: this sample lands on the ceiling */
        if (g != 1.0f) {
            for (int c = 0; c < nch; c++)
                feeds[c][i] *= g;
            s_engaged++;
        }
    }
    s_gain = g;
}

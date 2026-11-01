/*
 * apu_lowpass.h -- the resonant low-pass of a voice (the VP's per-voice
 * filter, when the title turns it on).
 *
 * This is xboxrecomp's own file, under the project's MIT licence (LICENSE).
 * It is not taken from xemu: it replaces the filter that came with xemu's
 * voice processor (vp/svf.h there, GPL-2.0), and was written from the
 * textbook description of the digital state variable filter, not from that
 * code. The reference is Hal Chamberlin, "Musical Applications of
 * Microprocessors" (2nd ed., 1985), the chapter on digital filters: two
 * integrators in a loop,
 *
 *     low  = low + f * band
 *     high = in - low - q * band
 *     band = band + f * high
 *
 * with the low-pass taken from `low`. Only the low-pass is here; it is the
 * only response the voice processor asks for.
 *
 * The order of the three steps: as listed, `low` is stepped from the band
 * value of the sample before, and the input reaches the output one sample
 * late. Here the band integrator is stepped first and `low` after it (high,
 * band, low), which is the same loop read one step on: the same poles, the
 * same magnitude response, no sample of delay. The filter this replaces
 * answered an impulse in the same sample (measured), and a voice that moved
 * by a sample against the others would not be the same mix.
 *
 *   f   the tuning coefficient. Chamberlin gives f = 2 sin(pi * Fc / Fs);
 *       the voice processor's cutoff argument (2^(FC0/4096), 1/256..1) is
 *       used as f itself, which puts the corner at
 *       Fc = Fs/pi * asin(f/2): 30 Hz at 1/256, 8 kHz at 1.
 *   q   the damping, 1/Q: 1 is no resonance to speak of, towards 0 the peak
 *       at the corner grows as 1/q. The voice processor's resonance argument
 *       (FC1/32768, 0.079..1) is used as q itself, so Q runs from 1 to 12.6.
 *
 * The level: a voice through this filter is scaled by sqrt(q/2 + 1/100), so
 * a resonant setting (low q) is quieter in the passband and its peak does not
 * run away: -2.9 dB at q = 1, -13.0 dB at q = 0.0794. That is the level the
 * filter it replaces produced, found by measuring its response (DC gain
 * squared against q on sixty settings is that line to seven digits), and
 * kept so that no voice changes loudness. The scale is applied to the input,
 * as measured there too: a change of resonance glides in rather than jumping.
 *
 * What is not the same as before: this filter is linear. The one it replaced
 * held a loud resonance back a little (a tone at full scale sitting on the
 * corner of the most resonant setting came out up to 3 dB lower than here);
 * in the passband, and on anything quiet or not resonant, the two measure the
 * same (0.000 dB, see BO-105 in the Burnout 3 port's backlog for the table).
 *
 * Stability: the loop is stable for f*f + 2*f*q < 4. The voice processor's
 * arguments (f <= 1, q <= 1) stay under 3, so nothing it can ask for is near
 * the limit; apu_lowpass_coef still pulls f in under it, for any other caller.
 */
#ifndef APU_LOWPASS_H
#define APU_LOWPASS_H

#include <math.h>

/* What the filter remembers between samples: the two integrators. All zero
 * is silence (memset is a reset). */
typedef struct ApuLowpass {
    float low;
    float band;
} ApuLowpass;

/* One setting, worked out once for a run of samples. */
typedef struct ApuLowpassCoef {
    float tune;     /* f */
    float damp;     /* q */
    float level;    /* the input scale */
} ApuLowpassCoef;

static inline ApuLowpassCoef apu_lowpass_coef(float cutoff, float resonance)
{
    ApuLowpassCoef c;
    float q = resonance, f = cutoff;

    if (!(q > 0.0f)) q = 0.0f;
    if (q > 2.0f) q = 2.0f;
    /* f*f + 2*f*q < 4, with a little room. */
    float f_max = 0.98f * (sqrtf(q * q + 4.0f) - q);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > f_max) f = f_max;

    c.tune = f;
    c.damp = q;
    c.level = sqrtf(0.5f * q + 0.01f);
    return c;
}

/* Called with a new setting, once per run of samples: a state that is no
 * longer a number is cleared (so one bad sample cannot hold the voice
 * silent for good), and one that has decayed below anything audible is set
 * to zero, so the integrators do not sit in denormals while a voice is
 * quiet. */
static inline void apu_lowpass_tidy(ApuLowpass *s)
{
    if (!isfinite(s->low) || !isfinite(s->band)) {
        s->low = 0.0f;
        s->band = 0.0f;
    } else if (fabsf(s->low) < 1e-18f && fabsf(s->band) < 1e-18f) {
        s->low = 0.0f;
        s->band = 0.0f;
    }
}

/* One sample in, one out. */
static inline float apu_lowpass_step(ApuLowpass *s, const ApuLowpassCoef *c,
                                     float in)
{
    float high;

    high = in * c->level - s->low - c->damp * s->band;
    s->band += c->tune * high;
    s->low += c->tune * s->band;
    return s->low;
}

#endif /* APU_LOWPASS_H */

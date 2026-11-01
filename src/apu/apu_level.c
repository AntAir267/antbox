/* The custom-music level (apu_level.h): the loudness chain (BO-246) and
 * the first leveller (BO-132, RECOMP_AUDIO_CUSTOM_AGC=old). */
#include "apu_level.h"
#include "apu_limit.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT          (32.0 / 48000.0)    /* one VP frame */
#define TAU_SLOW    3.0
#define TAU_FAST    0.3
#define TAU_DOWN    0.3
#define TAU_UP      4.0
#define FAST_DISCOUNT_DB 2.0f
#define FALLING_DB   3.0f

static const double A_SLOW = 0.99977780;    /* exp(-DT / TAU_SLOW) */
static const double A_FAST = 0.99777901;    /* exp(-DT / TAU_FAST) */
static const float  C_DOWN = 0.00222099f;   /* 1 - exp(-DT / TAU_DOWN) */
static const float  C_UP   = 0.00016664f;   /* 1 - exp(-DT / TAU_UP) */

float apu_level_mode_offset_db(int mode)
{
    return mode == APU_LEVEL_QUIETER ? -4.0f : mode == APU_LEVEL_LOUDER ? 4.0f : 0.0f;
}

static inline float db_lin(float db) { return powf(10.0f, db * 0.05f); }

/* The song's own level: the accepted readings in 0.1 dB bins, and their
 * power mean over the bins at most 10 LU under the mean of all of them (the
 * relative gate of BS.1770's integrated loudness: an intro or a breakdown
 * the body of the song outweighs drops out). */
static float bin_pow[APU_LEVEL_BINS];

void apu_level_state_init(apu_level_state *s, float gain_db)
{
    memset(s, 0, sizeof(*s));
    s->primed = gain_db > -100.0f;
    /* A first song has no history: until it has been heard for a moment
     * the gain is -6 dB (a modern master is far more likely than a quiet
     * one), then it jumps to the first reading's. */
    s->gain_db = s->primed ? gain_db : APU_LEVEL_FIRST_DB;
    s->st_db = -200.0f;
    s->run_db = -200.0f;
    s->prior_db = -200.0f;
    s->prior_w = APU_LEVEL_PRIOR_S;
    s->env_db = -200.0f;
    if (bin_pow[0] == 0.0f)
        for (int i = 0; i < APU_LEVEL_BINS; i++)
            bin_pow[i] = powf(10.0f, (APU_LEVEL_GATE_ABS_DB + (i + 0.5f) * 0.1f + 0.691f) * 0.1f);
}

/* ── the loudness chain ───────────────────────────────────────────── */

/* ITU-R BS.1770-4 K weighting at 48 kHz, the voice processor's rate: a
 * high shelf (+4 dB above about 1.5 kHz, the head) and a high pass (the
 * RLB curve, under about 60 Hz). Transposed direct form II. */
static const float KS_B0 = 1.53512485958697f, KS_B1 = -2.69169618940638f, KS_B2 = 1.19839281085285f;
static const float KS_A1 = -1.69065929318241f, KS_A2 = 0.73248077421585f;
static const float KH_A1 = -1.99004745483398f, KH_A2 = 0.99007225036621f;

static inline float kweight(float *z, float x)
{
    float y = KS_B0 * x + z[0];
    z[0] = KS_B1 * x - KS_A1 * y + z[1];
    z[1] = KS_B2 * x - KS_A2 * y;
    x = y;
    y = x + z[2];
    z[2] = -2.0f * x - KH_A1 * y + z[3];
    z[3] = x - KH_A2 * y;
    return y;
}

#define SUB_SAMPLES 4800u                   /* 100 ms at 48 kHz */
#ifndef READ_MIN
#define READ_MIN    3u                      /* a first reading after 0.3 s of the song (the ceiling) */
#endif
static inline float pow_db(double p) { return -0.691f + 10.0f * log10f((float)p + 1e-20f); }

static float song_level(const apu_level_state *s)
{
    double e = 0.0, n = 0.0, gate;
    int g;
    for (int i = 0; i < APU_LEVEL_BINS; i++) {
        e += (double)s->bins[i] * bin_pow[i];
        n += s->bins[i];
    }
    if (n <= 0.0)
        return -200.0f;
    gate = pow_db(e / n) + APU_LEVEL_GATE_REL_DB;
    g = (int)ceilf((gate - APU_LEVEL_GATE_ABS_DB) * 10.0f);
    if (g < 0) g = 0;
    e = n = 0.0;
    for (int i = g; i < APU_LEVEL_BINS; i++) {
        e += (double)s->bins[i] * bin_pow[i];
        n += s->bins[i];
    }
    return n > 0.0 ? pow_db(e / n) : -200.0f;
}

/* Every 100 ms: the short-term reading, the gate, the running level. */
static void level_read(apu_level_state *s)
{
    double sum = 0.0;
    float song;
    int bin;
    s->ring[s->ring_pos] = (float)(s->sub_acc / s->sub_n);
    s->ring_pos = (s->ring_pos + 1) % APU_LEVEL_RING;
    if (s->ring_n < APU_LEVEL_RING) s->ring_n++;
    s->sub_acc = 0.0;
    s->sub_n = 0;
    if (s->ring_n < READ_MIN)
        return;
    for (unsigned i = 0; i < s->ring_n; i++)
        sum += s->ring[i];
    sum /= s->ring_n;
    s->st_db = pow_db(sum);
    if (s->st_db < APU_LEVEL_GATE_ABS_DB) {         /* silence, a very quiet passage */
        s->hold = 1;
        return;
    }
    if (s->run_db > -100.0f && s->st_db < s->run_db + APU_LEVEL_GATE_REL_DB) {
        /* An intro, a breakdown, a fade: the gain holds. Held this long
         * while the song has little level of its own, and no more than
         * 20 LU under and over APU_LEVEL_SONG_FLOOR_DB (songs differ by
         * about 12 LU; a quiet intro is under any song's level), it is the
         * level the music is at
         * (a quiet song after a loud one): the count starts
         * again from here. Later in a song it is a breakdown or an outro,
         * and the gain holds for as long as it lasts. */
        if (s->st_db >= s->run_db + APU_LEVEL_GATE_NEW_DB && s->st_db >= APU_LEVEL_SONG_FLOOR_DB)
            s->gated_s += 0.1f;
        else
            s->gated_s = 0.0f;              /* far under any song: an intro, not a level */
        if (s->gated_s < APU_LEVEL_GATE_RESET_S || s->own_read >= APU_LEVEL_OWN_S * 10u) {
            s->hold = 1;
            return;
        }
        memset(s->bins, 0, sizeof(s->bins));
        s->n_read = 0;
        s->prior_db = s->st_db;
        s->prior_w = APU_LEVEL_PRIOR_S;
    }
    s->gated_s = 0.0f;
    s->hold = 0;
    /* Louder than the running level by 4 LU for 2 s: the song has gone up
     * a level for good (the body after a long quiet intro, a chorus after
     * minutes of verse), which its history would follow only slowly. The
     * count starts again from here. */
    if (s->run_db > -100.0f && s->st_db > s->run_db + APU_LEVEL_STEP_UP_DB) {
        s->up_s += 0.1f;
        if (s->up_s >= APU_LEVEL_STEP_UP_S) {
            memset(s->bins, 0, sizeof(s->bins));
            s->n_read = 0;
            s->prior_db = s->st_db;         /* with weight: a breakdown right after does not undo it */
            s->prior_w = APU_LEVEL_RESTART_PRIOR_S;
            s->up_s = 0.0f;
        }
    } else
        s->up_s = 0.0f;
    s->own_read++;
    bin = (int)((s->st_db - APU_LEVEL_GATE_ABS_DB) * 10.0f);
    if (bin >= APU_LEVEL_BINS) bin = APU_LEVEL_BINS - 1;
    if (s->bins[bin] < 0xFFFFFFFFu) s->bins[bin]++;
    s->n_read++;
    /* The running level: the song's own, weighed against the level the
     * last song ended on (worth APU_LEVEL_PRIOR_S of this one) while this
     * one has little of its own. */
    song = song_level(s);
    if (s->prior_db > -100.0f) {
        /* the prior fades as the song's own readings come in (time
         * constant APU_LEVEL_PRIOR_FADE_S): a song 6 LU quieter than the
         * last is at its own level within seconds, not over minutes */
        const float n = (float)s->n_read;
        const float w0 = s->prior_w * 10.0f * expf(-n / (10.0f * APU_LEVEL_PRIOR_FADE_S));
        s->run_db = (w0 * s->prior_db + n * song) / (w0 + n);
    } else
        s->run_db = song;
}

float apu_level_frame(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db)
{
    /* per-sample one-pole coefficients of the compressor's detector */
    static float c_att = 0.0f, c_rel = 0.0f, c_gain = 0.0f, c_fast = 0.0f;
    float g0, g1, comp, over;
    if (c_att == 0.0f) {
        c_att  = 1.0f - expf(-1.0f / (48.0f * APU_LEVEL_COMP_ATTACK_MS));
        c_rel  = 1.0f - expf(-1.0f / (48.0f * APU_LEVEL_COMP_RELEASE_MS));
        c_gain = 1.0f - expf(-32.0f / (48000.0f * APU_LEVEL_TAU_GAIN_S));
        c_fast = 1.0f - expf(-32.0f / (48000.0f * APU_LEVEL_TAU_CEIL_S));
    }

    {
        float cp = s->comp_pow;
        double acc = 0.0;
        for (int i = 0; i < 32; i++) {
            const float l = kweight(s->kz[0], samples[i][0]);
            const float r = channels > 1 ? kweight(s->kz[1], samples[i][1]) : l;
            const float p = l * l + r * r;
            acc += p;
            cp += (p > cp ? c_att : c_rel) * (p - cp);
        }
        s->comp_pow = cp;
        s->sub_acc += acc;
        s->sub_n += 32;
    }
    if (s->sub_n >= SUB_SAMPLES)
        level_read(s);
    s->env_db = s->run_db;
    s->frames++;

    g0 = db_lin(s->gain_db + s->comp_db);
    /* The ceiling: the short-term output no more than APU_LEVEL_CEIL_DB
     * over the target. A song that comes in louder than the last one (or
     * than its own quiet start) is brought down in half a second, not over
     * the slow gain's seconds; the slow gain never climbs past it. */
    float ceil_db = APU_LEVEL_MAX_DB;
    if (s->st_db >= APU_LEVEL_GATE_ABS_DB)
        ceil_db = target_db + APU_LEVEL_CEIL_DB - s->st_db;
    if (s->run_db > -100.0f && !s->hold) {
        float want = target_db - s->run_db;
        if (want > ceil_db) want = ceil_db;
        if (want < APU_LEVEL_MIN_DB) want = APU_LEVEL_MIN_DB;
        if (want > APU_LEVEL_MAX_DB) want = APU_LEVEL_MAX_DB;
        if (!s->primed) {
            s->gain_db = want;          /* the very first song: no history to slew from */
            s->primed = 1;
        } else
            s->gain_db += (want - s->gain_db) * c_gain;
    }
    if (s->primed && s->gain_db > ceil_db && ceil_db >= APU_LEVEL_MIN_DB)
        s->gain_db += (ceil_db - s->gain_db) * c_fast;
    /* The compressor: the detector's level after the gain, over the
     * threshold, 2:1 with a soft knee. */
    over = pow_db(s->comp_pow) + s->gain_db - (target_db + APU_LEVEL_COMP_ABOVE_DB);
    if (over <= -0.5f * APU_LEVEL_COMP_KNEE_DB)
        comp = 0.0f;
    else if (over < 0.5f * APU_LEVEL_COMP_KNEE_DB) {
        const float k = over + 0.5f * APU_LEVEL_COMP_KNEE_DB;
        comp = -(1.0f - 1.0f / APU_LEVEL_COMP_RATIO) * k * k / (2.0f * APU_LEVEL_COMP_KNEE_DB);
    } else
        comp = -(1.0f - 1.0f / APU_LEVEL_COMP_RATIO) * over;
    s->comp_db = comp;
    if (-comp > s->comp_max_db) s->comp_max_db = -comp;
    s->comp_sum_db -= comp;
    g1 = db_lin(s->gain_db + s->comp_db);
    /* Interpolated across the frame: the gain at the end of sample i is
     * g0 + (g1 - g0) (i + 1) / 32. */
    if (g0 == 1.0f && g1 == 1.0f)
        return 1.0f;
    for (int i = 0; i < 32; i++) {
        const float g = g0 + (g1 - g0) * (float)(i + 1) * (1.0f / 32.0f);
        samples[i][0] *= g;
        samples[i][1] *= g;
    }
    return g1;
}

/* ── the first leveller (BO-132) ──────────────────────────────────── */

float apu_level_frame_old(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db)
{
    double e = 0.0;
    float env_db, slow_db, fast_db, g0, g1;

    for (int i = 0; i < 32; i++) {
        const double l = samples[i][0];
        const double r = channels > 1 ? samples[i][1] : l;
        e += l * l + r * r;
    }
    e /= 32.0;
    /* Readings under -60 dBFS are silence: not averaged, so a pause does
     * not drag the envelope down and the song after it is not boosted. */
    if (e >= 1e-6) {
        s->slow_num = A_SLOW * s->slow_num + (1.0 - A_SLOW) * e;
        s->slow_wt  = A_SLOW * s->slow_wt  + (1.0 - A_SLOW);
        s->fast_num = A_FAST * s->fast_num + (1.0 - A_FAST) * e;
        s->fast_wt  = A_FAST * s->fast_wt  + (1.0 - A_FAST);
    }
    slow_db = s->slow_wt > 0.0 ? 10.0f * log10f((float)(s->slow_num / s->slow_wt) + 1e-20f) : -200.0f;
    fast_db = s->fast_wt > 0.0 ? 10.0f * log10f((float)(s->fast_num / s->fast_wt) + 1e-20f) : -200.0f;
    env_db = fast_db - FAST_DISCOUNT_DB > slow_db ? fast_db - FAST_DISCOUNT_DB : slow_db;
    s->env_db = env_db;
    s->frames++;

    g0 = db_lin(s->gain_db);
    if (env_db >= APU_LEVEL_FREEZE_DB) {
        float want = target_db - env_db;
        if (want < APU_LEVEL_OLD_MIN_DB) want = APU_LEVEL_OLD_MIN_DB;
        if (want > APU_LEVEL_OLD_MAX_DB) want = APU_LEVEL_OLD_MAX_DB;
        if (!s->primed) {
            /* (the first song's start, apu_level_state_init) */
            if (s->slow_wt >= 0.15) {   /* half a second of sound heard */
                s->gain_db = want;      /* the very first song: no history to slew from */
                s->primed = 1;
            }
        } else if (want > s->gain_db && fast_db < slow_db - FALLING_DB) {
            /* The level is dropping (a fade out, a break): that is not a
             * quiet song, the gain does not climb after it. */
        } else {
            s->gain_db += (want - s->gain_db) * (want < s->gain_db ? C_DOWN : C_UP);
        }
    }
    g1 = db_lin(s->gain_db);
    /* Interpolated across the frame: the gain at the end of sample i is
     * g0 + (g1 - g0) (i + 1) / 32. */
    if (g0 == 1.0f && g1 == 1.0f)
        return 1.0f;
    for (int i = 0; i < 32; i++) {
        const float g = g0 + (g1 - g0) * (float)(i + 1) * (1.0f / 32.0f);
        samples[i][0] *= g;
        samples[i][1] *= g;
    }
    return g1;
}

/* ── the voices ───────────────────────────────────────────────────── */

static volatile int s_mode = -1;                /* -1: the variable is not read yet */
static apu_level_class_fn s_classify;
static int s_log = -1;

void apu_level_set_mode(int mode)
{
    s_mode = mode < 0 || mode >= APU_LEVEL_MODES ? APU_LEVEL_MATCH : mode;
}

int apu_level_mode(void)
{
    if (s_mode < 0) {
        const char *e = recomp_env(RENV_AUDIO_CUSTOM_LEVEL);
        int m = APU_LEVEL_MATCH;
        if (e && *e) {
            if (!strcmp(e, "off") || !strcmp(e, "0")) m = APU_LEVEL_OFF;
            else if (!strcmp(e, "quieter")) m = APU_LEVEL_QUIETER;
            else if (!strcmp(e, "louder")) m = APU_LEVEL_LOUDER;
        }
        s_mode = m;
    }
    return s_mode;
}

void apu_level_set_classifier(apu_level_class_fn fn) { s_classify = fn; }

int apu_level_active(void)
{
    return s_classify && apu_level_mode() != APU_LEVEL_OFF;
}

static volatile int s_old = -1;                 /* RECOMP_AUDIO_CUSTOM_AGC=old; -1: not read yet */

int apu_level_use_old(void)
{
    if (s_old < 0) {
        const char *e = recomp_env(RENV_AUDIO_CUSTOM_AGC);
        s_old = e && !strcmp(e, "old");
    }
    return s_old;
}

void apu_level_set_old(int on) { s_old = on ? 1 : 0; }

static struct { unsigned key; int custom; apu_level_state st; unsigned log; } s_v[256];
static float  s_last_gain_db = -1000.0f;        /* what the last song settled on */
static float  s_last_run_db = -200.0f;          /* and the level it ended on */

void apu_level_voice(unsigned voice, unsigned serial,
                     int (*fill)(void *ctx, apu_bus_voice *v), void *ctx,
                     float (*samples)[2], unsigned channels)
{
    int mode = apu_level_mode();
    if (mode == APU_LEVEL_OFF || !s_classify || voice >= 256)
        return;
    if (s_v[voice].key != serial + 1u) {
        apu_bus_voice v;
        memset(&v, 0, sizeof(v));
        s_v[voice].key = serial + 1u;
        s_v[voice].custom = fill && fill(ctx, &v) && v.stream && s_classify(&v);
        if (s_v[voice].custom) {
            apu_level_state_init(&s_v[voice].st, s_last_gain_db);
            s_v[voice].st.run_db = s_v[voice].st.prior_db = s_last_run_db;
            s_v[voice].log = 0;
            if (s_log < 0) {
                const char *e = recomp_env(RENV_AUDIO_LEVEL_LOG);
                s_log = e && *e && *e != '0';
            }
        }
    }
    if (!s_v[voice].custom)
        return;
    if (apu_level_use_old())
        apu_level_frame_old(&s_v[voice].st, samples, channels,
                            APU_LEVEL_OLD_TARGET_DB + apu_level_mode_offset_db(mode));
    else
        apu_level_frame(&s_v[voice].st, samples, channels,
                        APU_LEVEL_TARGET_DB + apu_level_mode_offset_db(mode));
    if (s_v[voice].st.primed) {
        s_last_gain_db = s_v[voice].st.gain_db;
        s_last_run_db = s_v[voice].st.run_db;
    }
    if (s_log > 0 && ++s_v[voice].log % 1500 == 0) {
        const apu_level_state *st = &s_v[voice].st;
        if (apu_level_use_old())
            fprintf(stderr, "[LEVEL] voice %u: envelope %.1f dB, gain %+.2f dB (mode %d, old)\n",
                    voice, st->env_db, st->gain_db, mode);
        else
            fprintf(stderr, "[LEVEL] voice %u: short-term %.1f LUFS, running %.1f LUFS%s, gain %+.2f dB, "
                    "compressor %.2f dB (deepest %.2f, mean %.2f), limiter %lu samples (mode %d)\n",
                    voice, st->st_db, st->run_db, st->hold ? " (held)" : "", st->gain_db, -st->comp_db,
                    st->comp_max_db, st->frames ? st->comp_sum_db / st->frames : 0.0,
                    apu_limit_engaged(), mode);
    }
}

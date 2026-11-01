/*
 * apu_bus.c -- the host mixer: buses over the title's voices (apu_bus.h).
 *
 * No device, no APU state: the VP hands voices in, the DSP stage hands the
 * speaker feeds in. Everything the frame thread reads is in one runtime
 * block that a configuration change replaces whole.
 */
#include "apu_bus.h"
#include "recomp_env.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── small helpers ─────────────────────────────────────────────────── */

static float db_lin(float db)
{
    return db <= APU_BUS_OFF_DB ? 0.0f : powf(10.0f, db / 20.0f);
}

static float lin_db(double lin)
{
    return lin > 1e-10 ? (float)(20.0 * log10(lin)) : APU_BUS_OFF_DB;
}

/* A second-order section, direct form 1, double state. */
typedef struct { double b0, b1, b2, a1, a2; } bq_coef;
typedef struct { double x1, x2, y1, y2; } bq_state;

static inline double bq_step(const bq_coef *c, bq_state *s, double x)
{
    double y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = fabs(y) < 1e-30 ? 0.0 : y;
    return y;
}

/* Butterworth, 4th order, as two sections (Q 0.5412 and 1.3066), 48 kHz:
 * 24 dB an octave, -3 dB at hz. */
static void butter4(int hz, int highpass, bq_coef out[2])
{
    static const double q[2] = { 0.54119610, 1.30656296 };
    for (int k = 0; k < 2; k++) {
        double w = 2.0 * M_PI * (double)hz / 48000.0;
        double cw = cos(w), alpha = sin(w) / (2.0 * q[k]);
        double a0 = 1.0 + alpha;
        if (highpass) {
            out[k].b0 = (1.0 + cw) / 2.0 / a0;
            out[k].b1 = -(1.0 + cw) / a0;
            out[k].b2 = (1.0 + cw) / 2.0 / a0;
        } else {
            out[k].b0 = (1.0 - cw) / 2.0 / a0;
            out[k].b1 = (1.0 - cw) / a0;
            out[k].b2 = (1.0 - cw) / 2.0 / a0;
        }
        out[k].a1 = -2.0 * cw / a0;
        out[k].a2 = (1.0 - alpha) / a0;
    }
}

static int clamp_hz(int hz)
{
    if (hz <= 0) return 0;
    return hz < 40 ? 40 : hz > 250 ? 250 : hz;
}

/* ── the buses ─────────────────────────────────────────────────────── */

static char s_names[APU_BUS_MAX][32];
static int s_nbus;

void apu_bus_set_names(const char *const *names, int count)
{
    if (count > APU_BUS_MAX) count = APU_BUS_MAX;
    for (int i = 0; i < count; i++)
        snprintf(s_names[i], sizeof(s_names[i]), "%s", names[i] ? names[i] : "");
    s_nbus = count;
}

int apu_bus_count(void) { return s_nbus; }

const char *apu_bus_name(int bus)
{
    return bus >= 0 && bus < s_nbus ? s_names[bus] : "";
}

static void make_key(const char *name, char *out, size_t n)
{
    size_t k = 0;
    int us = 1;                         /* no leading or doubled '_' */
    for (; *name && k + 1 < n; name++) {
        if (isalnum((unsigned char)*name)) {
            out[k++] = (char)tolower((unsigned char)*name);
            us = 0;
        } else if (!us) {
            out[k++] = '_';
            us = 1;
        }
    }
    while (k && out[k - 1] == '_') k--;
    if (n) out[k] = 0;
}

void apu_bus_key(int bus, char *out, size_t n)
{
    make_key(apu_bus_name(bus), out, n);
}

int apu_bus_find(const char *s)
{
    char want[32], have[32];
    make_key(s ? s : "", want, sizeof(want));
    for (int i = 0; i < s_nbus; i++) {
        apu_bus_key(i, have, sizeof(have));
        if (!strcmp(want, have)) return i;
    }
    return -1;
}

/* ── configuration ─────────────────────────────────────────────────── */

static int s_duck_src, s_duck_tgt;

void apu_bus_set_duck_default(int source, int target)
{
    s_duck_src = source;
    s_duck_tgt = target;
}

void apu_bus_config_neutral(apu_bus_config *c)
{
    memset(c, 0, sizeof(*c));
    for (int b = 0; b < APU_BUS_MAX; b++) {
        c->bus[b].lfe_send_db = APU_BUS_OFF_DB;
        c->bus[b].lfe_crossover_hz = 80;
        c->bus[b].pan_y = 1.0f;             /* front centre, full width */
        c->bus[b].pan_width = 1.0f;
        c->bus[b].field_width = 1.0f;
        for (int s = 0; s < APU_BUS_SPEAKERS; s++)
            c->bus[b].send_db[s] = APU_BUS_OFF_DB;
        c->bus[b].send_db[0] = c->bus[b].send_db[1] = 0.0f;
    }
    c->ceiling_db = -0.3f;
    c->limiter_release_ms = 200.0f;
    c->duck.source = s_duck_src;
    c->duck.target = s_duck_tgt;
    c->duck.threshold_db = -30.0f;
    c->duck.depth_db = 6.0f;
    c->duck.attack_ms = 20.0f;
    c->duck.release_ms = 400.0f;
}

static int bus_is_neutral(const apu_bus_params *p)
{
    if (p->gain_db != 0.0f || p->mute || p->solo || p->route != APU_BUS_ROUTE_TITLE)
        return 0;
    for (int t = 0; t < APU_BUS_TRIMS; t++)
        if (p->trim_db[t] != 0.0f) return 0;
    if (p->field_width != 1.0f || p->field_divergence != 0.0f) return 0;
    return p->lfe_send_db <= APU_BUS_OFF_DB;
}

int apu_bus_config_is_neutral(const apu_bus_config *c)
{
    for (int b = 0; b < APU_BUS_MAX; b++)
        if (!bus_is_neutral(&c->bus[b])) return 0;
    for (int s = 0; s < APU_BUS_SPEAKERS; s++)
        if (c->speaker_trim_db[s] != 0.0f || c->speaker_delay_ms[s] != 0.0f) return 0;
    return !c->lfe_lowpass_hz && !c->mains_highpass_hz && !c->limiter && !c->duck.enabled;
}

/* ── the pan law (apu_bus.h) ───────────────────────────────────────── */

static float clampf1(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void apu_bus_pan_gains(float x, float y, float divergence, float out[5])
{
    /* The ring, clockwise from the centre speaker, by its angle in the room
     * (the square's corners at 45 and 135 degrees). The share of an arc is
     * the same whether the arc is measured in the room or between the ITU
     * angles the corners stand for, so only the room's are needed. */
    static const struct { int spk; float room; } ring[6] = {
        { 2, 0.0f }, { 1, 45.0f }, { 4, 135.0f }, { 3, 225.0f }, { 0, 315.0f }, { 2, 360.0f },
    };
    double pw[5] = { 0, 0, 0, 0, 0 };
    float r, ang;
    x = clampf1(x, -1.0f, 1.0f);
    y = clampf1(y, -1.0f, 1.0f);
    divergence = clampf1(divergence, 0.0f, 1.0f);
    r = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
    ang = r > 0.0f ? atan2f(x, y) * (float)(180.0 / M_PI) : 0.0f;
    if (ang < 0.0f) ang += 360.0f;
    for (int k = 0; k < 5; k++) {
        if (ang >= ring[k].room && ang <= ring[k + 1].room) {
            const float t = (ang - ring[k].room) / (ring[k + 1].room - ring[k].room);
            const double a = (double)t * (M_PI / 2.0);
            pw[ring[k].spk] += cos(a) * cos(a);
            pw[ring[k + 1].spk] += sin(a) * sin(a);
            break;
        }
    }
    for (int k = 0; k < 5; k++)
        pw[k] = (1.0 - r) / 5.0 + r * pw[k];
    {
        const double moved = divergence * pw[2];
        pw[2] -= moved;
        pw[0] += moved / 2.0;
        pw[1] += moved / 2.0;
    }
    for (int k = 0; k < 5; k++)
        out[k] = (float)sqrt(pw[k] > 0.0 ? pw[k] : 0.0);
}

void apu_bus_pan_gains_stereo(float x, float out[2])
{
    const double a = ((double)clampf1(x, -1.0f, 1.0f) + 1.0) * (M_PI / 4.0);
    out[0] = (float)cos(a);
    out[1] = (float)sin(a);
}

void apu_bus_field_width(float w, float *left, float *right)
{
    const float l = fabsf(*left), r = fabsf(*right);
    const float m = l + r, p = l * l + r * r;
    float s = (l - r) * clampf1(w, 0.0f, 2.0f), nl, nr, np;
    if (p <= 0.0f) return;
    if (s > m) s = m;
    if (s < -m) s = -m;
    nl = 0.5f * (m + s);
    nr = 0.5f * (m - s);
    np = nl * nl + nr * nr;
    if (np > 0.0f) {
        const float k = sqrtf(p / np);
        *left = nl * k;
        *right = nr * k;
    }
}

void apu_bus_field_divergence(float d, float *fl, float *fr, float *c)
{
    const float pl = *fl * *fl, pr = *fr * *fr, pc = *c * *c;
    d = clampf1(d, -1.0f, 1.0f);
    if (d > 0.0f) {
        const float moved = d * pc;
        *c = sqrtf(pc - moved);
        *fl = sqrtf(pl + 0.5f * moved);
        *fr = sqrtf(pr + 0.5f * moved);
    } else if (d < 0.0f) {
        const float common = (pl < pr ? pl : pr) * -d;
        *fl = sqrtf(pl - common);
        *fr = sqrtf(pr - common);
        *c = sqrtf(pc + 2.0f * common);
    }
}

/* ── the runtime block ─────────────────────────────────────────────── */

#define DELAY_MAX 4096                  /* samples: 50 ms is 2400 */
#define LOOKAHEAD 64                    /* samples: 1.33 ms */

struct apu_bus_rt {
    int   channels;                     /* this frame: 6 or 2 */
    uint8_t cls[APU_BUS_BINS];          /* trim group of each bin; APU_BUS_TRIMS: none */
    float eff[APU_BUS_MAX];             /* this frame: gain with mute, solo, duck */
    /* from the configuration: */
    float lin[APU_BUS_MAX];             /* gain with mute and solo */
    uint8_t route[APU_BUS_MAX];         /* APU_BUS_ROUTE_* */
    uint8_t plain[APU_BUS_MAX];         /* route title, trims 0 dB, no shaping, no LFE send */
    uint8_t shaped[APU_BUS_MAX];        /* route title with field width or divergence */
    float field_w[APU_BUS_MAX], field_d[APU_BUS_MAX];
    /* Route pan: the gains of the left channel, the right channel and a
     * mono voice; 5.1 as FL FR C SL SR, stereo as L R. */
    float pan6[APU_BUS_MAX][3][5], pan2[APU_BUS_MAX][3][2];
    int8_t spk[APU_BUS_BINS];           /* this frame: the speaker of each bin, -1 none */
    float trim[APU_BUS_MAX][APU_BUS_TRIMS + 1];
    float send[APU_BUS_MAX][APU_BUS_SPEAKERS];
    float lfe_send[APU_BUS_MAX];
    int   lfe_xover[APU_BUS_MAX];
    bq_coef lfe_coef[APU_BUS_MAX][2];
    int   any_lfe_send;
    float spk_trim[APU_BUS_SPEAKERS];
    int   spk_delay[APU_BUS_SPEAKERS];
    int   lfe_lp, mains_hp;
    bq_coef lfe_lp_coef[2], mains_hp_coef[2];
    int   limiter;
    float ceiling, lim_release;
    int   out_active;
    apu_bus_duck duck;
    float duck_thr_sq, duck_att, duck_rel;
};

static apu_bus_config s_cfg;
static int s_cfg_set;
static struct apu_bus_rt s_rt[2];
static struct apu_bus_rt *volatile s_cur;       /* what the frame thread reads */
static volatile int s_on = -1;

/* State that outlives a configuration change (frame thread only). */
static struct {
    bq_state lfe_send[APU_BUS_MAX][2];
    float lfe_acc[APU_BUS_MAX][APU_BUS_FRAME];
    uint8_t lfe_used[APU_BUS_MAX];
    float duck_acc[APU_BUS_FRAME];
    float duck_red_db;                  /* reduction now */
    bq_state lfe_lp[2], mains_hp[APU_BUS_SPEAKERS][2];
    float delay[APU_BUS_SPEAKERS][DELAY_MAX];
    unsigned delay_pos;
    float look[APU_BUS_SPEAKERS][LOOKAHEAD];
    float look_gain[LOOKAHEAD];
    unsigned look_pos;
    float lim_env;
    /* meters */
    double bus_sq[APU_BUS_MAX];
    unsigned bus_voices[APU_BUS_MAX], meter_frames;
    double spk_sq[APU_BUS_SPEAKERS];
    float spk_peak[APU_BUS_SPEAKERS], spk_hold[APU_BUS_SPEAKERS];
    unsigned spk_hold_age[APU_BUS_SPEAKERS], spk_n;
    float lim_min;
    unsigned file_frames;
} s_st;
static int s_st_init;

static apu_bus_meters s_meters;         /* published: plain stores, read racily */

static void build_rt(const apu_bus_config *c, struct apu_bus_rt *r)
{
    int any_solo = 0;
    memset(r, 0, sizeof(*r));
    for (int b = 0; b < APU_BUS_MAX; b++)
        any_solo |= c->bus[b].solo;
    for (int b = 0; b < APU_BUS_MAX; b++) {
        const apu_bus_params *p = &c->bus[b];
        int silent = p->mute || (any_solo && !p->solo);
        r->lin[b] = silent ? 0.0f : p->gain_db == 0.0f ? 1.0f : db_lin(p->gain_db);
        r->route[b] = (uint8_t)(p->route == APU_BUS_ROUTE_SENDS ? APU_BUS_ROUTE_SENDS
                              : p->route == APU_BUS_ROUTE_PAN ? APU_BUS_ROUTE_PAN : APU_BUS_ROUTE_TITLE);
        r->field_w[b] = clampf1(p->field_width, 0.0f, 2.0f);
        r->field_d[b] = clampf1(p->field_divergence, -1.0f, 1.0f);
        r->shaped[b] = r->route[b] == APU_BUS_ROUTE_TITLE
                    && (r->field_w[b] != 1.0f || r->field_d[b] != 0.0f);
        if (r->route[b] == APU_BUS_ROUTE_PAN) {
            /* The two channels a width apart around the position; a mono
             * voice is at both places with half its power each. */
            const float w = clampf1(p->pan_width, 0.0f, 1.0f);
            const float xl = clampf1(p->pan_x - w, -1.0f, 1.0f), xr = clampf1(p->pan_x + w, -1.0f, 1.0f);
            apu_bus_pan_gains(xl, p->pan_y, p->pan_divergence, r->pan6[b][0]);
            apu_bus_pan_gains(xr, p->pan_y, p->pan_divergence, r->pan6[b][1]);
            apu_bus_pan_gains_stereo(xl, r->pan2[b][0]);
            apu_bus_pan_gains_stereo(xr, r->pan2[b][1]);
            for (int k = 0; k < 5; k++)
                r->pan6[b][2][k] = sqrtf(0.5f * (r->pan6[b][0][k] * r->pan6[b][0][k]
                                               + r->pan6[b][1][k] * r->pan6[b][1][k]));
            for (int k = 0; k < 2; k++)
                r->pan2[b][2][k] = sqrtf(0.5f * (r->pan2[b][0][k] * r->pan2[b][0][k]
                                               + r->pan2[b][1][k] * r->pan2[b][1][k]));
        }
        for (int t = 0; t < APU_BUS_TRIMS; t++)
            r->trim[b][t] = p->trim_db[t] == 0.0f ? 1.0f : db_lin(p->trim_db[t]);
        r->trim[b][APU_BUS_TRIMS] = 1.0f;
        for (int s = 0; s < APU_BUS_SPEAKERS; s++)
            r->send[b][s] = db_lin(p->send_db[s]);
        r->lfe_send[b] = db_lin(p->lfe_send_db);
        r->lfe_xover[b] = clamp_hz(p->lfe_crossover_hz);
        if (r->lfe_xover[b])
            butter4(r->lfe_xover[b], 0, r->lfe_coef[b]);
        if (r->lfe_send[b] > 0.0f)
            r->any_lfe_send = 1;
        r->plain[b] = !r->route[b] && !r->shaped[b] && r->lfe_send[b] == 0.0f;
        for (int t = 0; t < APU_BUS_TRIMS; t++)
            if (r->trim[b][t] != 1.0f) r->plain[b] = 0;
    }
    for (int s = 0; s < APU_BUS_SPEAKERS; s++) {
        float ms = c->speaker_delay_ms[s];
        int n = (int)(ms * 48.0f + 0.5f);
        r->spk_trim[s] = c->speaker_trim_db[s] == 0.0f ? 1.0f : db_lin(c->speaker_trim_db[s]);
        r->spk_delay[s] = n < 0 ? 0 : n > DELAY_MAX - 1 ? DELAY_MAX - 1 : n;
        if (r->spk_trim[s] != 1.0f || r->spk_delay[s])
            r->out_active = 1;
    }
    r->lfe_lp = clamp_hz(c->lfe_lowpass_hz);
    r->mains_hp = clamp_hz(c->mains_highpass_hz);
    if (r->lfe_lp) butter4(r->lfe_lp, 0, r->lfe_lp_coef);
    if (r->mains_hp) butter4(r->mains_hp, 1, r->mains_hp_coef);
    r->limiter = c->limiter;
    r->ceiling = db_lin(c->ceiling_db > 0.0f ? 0.0f : c->ceiling_db);
    if (r->ceiling < 0.05f) r->ceiling = 0.05f;
    {
        float ms = c->limiter_release_ms < 5.0f ? 5.0f : c->limiter_release_ms;
        r->lim_release = 1.0f - expf(-1.0f / (ms * 48.0f));
    }
    if (r->lfe_lp || r->mains_hp || r->limiter)
        r->out_active = 1;
    r->duck = c->duck;
    if (r->duck.source < 0 || r->duck.source >= APU_BUS_MAX ||
        r->duck.target < 0 || r->duck.target >= APU_BUS_MAX ||
        r->duck.source == r->duck.target || r->duck.depth_db <= 0.0f)
        r->duck.enabled = 0;
    {
        float thr = db_lin(r->duck.threshold_db);
        float a = r->duck.attack_ms < 1.0f ? 1.0f : r->duck.attack_ms;
        float rl = r->duck.release_ms < 1.0f ? 1.0f : r->duck.release_ms;
        /* The detector runs once a VP frame: 32 / 48000 s. */
        r->duck_thr_sq = thr * thr;
        r->duck_att = 1.0f - expf(-(32.0f / 48.0f) / a);
        r->duck_rel = 1.0f - expf(-(32.0f / 48.0f) / rl);
    }
}

/* The overlay's thread and the frame thread (a changed file) both set the
 * configuration: one at a time. The frame thread's reads of the runtime
 * block take no lock; a block is complete before it is published. */
static volatile char s_cfg_lock;
#if defined(_MSC_VER)
#include <intrin.h>
static void cfg_lock(void) { while (_InterlockedExchange8(&s_cfg_lock, 1)) {} }
static void cfg_unlock(void) { _InterlockedExchange8(&s_cfg_lock, 0); }
#else
static void cfg_lock(void) { while (__sync_lock_test_and_set(&s_cfg_lock, 1)) {} }
static void cfg_unlock(void) { __sync_lock_release(&s_cfg_lock); }
#endif

void apu_bus_set_config(const apu_bus_config *c)
{
    struct apu_bus_rt *next;
    apu_bus_config in = *c;
    cfg_lock();
    s_cfg = in;
    s_cfg_set = 1;
    next = s_cur == &s_rt[0] ? &s_rt[1] : &s_rt[0];
    build_rt(&s_cfg, next);
    s_cur = next;
    cfg_unlock();
}

void apu_bus_get_config(apu_bus_config *c)
{
    cfg_lock();
    if (!s_cfg_set) {
        apu_bus_config_neutral(&s_cfg);
        s_cfg_set = 1;
    }
    *c = s_cfg;
    cfg_unlock();
}

static void file_open(void);
void apu_bus_enable(int on)
{
    int was = s_on > 0;
    if (on && !s_cur) {
        apu_bus_config c;
        apu_bus_get_config(&c);
        apu_bus_set_config(&c);
    }
    s_on = on != 0;
    if (on && !was)
        file_open();
}

int apu_bus_enabled(void)
{
    if (s_on < 0) {
        const char *e = recomp_env(RENV_AUDIO_MIXER);
        apu_bus_enable(e && *e && *e != '0' && strcmp(e, "off") && strcmp(e, "false"));
    }
    return s_on;
}

/* ── classification ────────────────────────────────────────────────── */

static apu_bus_classify_fn s_classify;
static volatile unsigned s_class_gen = 1;
static struct { unsigned serial, gen; uint32_t pa; int bus; } s_vbus[256];

void apu_bus_set_classifier(apu_bus_classify_fn fn) { s_classify = fn; }
void apu_bus_reclassify(void) { s_class_gen++; }

int apu_bus_voice_bus(unsigned voice, unsigned serial, uint32_t pa,
                      int (*fill)(void *ctx, apu_bus_voice *v), void *ctx)
{
    int other = s_nbus > 0 ? s_nbus - 1 : 0, bus;
    unsigned gen = s_class_gen;
    if (voice >= 256) return other;
    if (s_vbus[voice].gen == gen && s_vbus[voice].serial == serial && s_vbus[voice].pa == pa)
        return s_vbus[voice].bus;
    bus = other;
    if (s_classify && fill) {
        apu_bus_voice v;
        memset(&v, 0, sizeof(v));
        if (fill(ctx, &v)) {
            bus = s_classify(&v);
            if (bus != APU_BUS_BYPASS && (bus < 0 || bus >= s_nbus)) bus = other;
        }
    }
    s_vbus[voice].gen = gen;
    s_vbus[voice].serial = serial;
    s_vbus[voice].pa = pa;
    s_vbus[voice].bus = bus;
    return bus;
}

/* ── the preset file ───────────────────────────────────────────────── */

static const char *const k_spk[APU_BUS_SPEAKERS] = { "FL", "FR", "C", "LFE", "SL", "SR" };
static const char *const k_spk_lc[APU_BUS_SPEAKERS] = { "fl", "fr", "c", "lfe", "sl", "sr" };
static const char *const k_trim[APU_BUS_TRIMS] = { "front_db", "centre_db", "lfe_db", "rear_db" };
static const char *const k_lim[3] = { "off", "soft", "peak" };

static void set_err(char *err, size_t n, const char *fmt, int line, const char *what)
{
    if (err && n && !*err)
        snprintf(err, n, fmt, line, what);
}

/* A value: a number, "off" (silence), true/false, or a quoted string.
 * kind: 0 number, 1 string, 2 bool. */
static int parse_value(char *v, double *num, char **str)
{
    size_t n = strlen(v);
    while (n && isspace((unsigned char)v[n - 1])) v[--n] = 0;
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
        v[n - 1] = 0;
        *str = v + 1;
        if (!strcmp(*str, "off")) { *num = APU_BUS_OFF_DB; }
        return 1;
    }
    if (!strcmp(v, "true"))  { *num = 1; return 2; }
    if (!strcmp(v, "false")) { *num = 0; return 2; }
    if (!strcmp(v, "off"))   { *num = APU_BUS_OFF_DB; *str = v; return 1; }
    {
        char *end;
        *num = strtod(v, &end);
        if (end == v || *end) return -1;
        if (*num < APU_BUS_OFF_DB) *num = APU_BUS_OFF_DB;
    }
    return 0;
}

int apu_bus_config_parse(const char *text, apu_bus_config *out, char *err, size_t nerr)
{
    apu_bus_config c = *out;
    enum { S_NONE, S_MASTER, S_SPEAKER, S_DUCK, S_BUS, S_SKIP } sec = S_NONE;
    int idx = -1, line_no = 0;
    const char *p = text;

    if (err && nerr) *err = 0;
    while (*p) {
        char line[256], *s, *eq, *key, *val, *str = NULL;
        double num = 0;
        int kind;
        size_t n = strcspn(p, "\n");
        size_t take = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = 0;
        p += n + (p[n] == '\n');
        line_no++;
        /* A comment, outside a string. */
        for (s = line, kind = 0; *s; s++) {
            if (*s == '"') kind = !kind;
            if (*s == '#' && !kind) { *s = 0; break; }
        }
        for (s = line; isspace((unsigned char)*s); s++) {}
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) { set_err(err, nerr, "line %d: %s", line_no, "missing ]"); return -1; }
            *e = 0;
            s++;
            idx = -1;
            if (!strcmp(s, "master")) sec = S_MASTER;
            else if (!strcmp(s, "duck")) sec = S_DUCK;
            else if (!strncmp(s, "speaker.", 8)) {
                sec = S_SKIP;
                for (int k = 0; k < APU_BUS_SPEAKERS; k++)
                    if (!strcmp(s + 8, k_spk[k]) || !strcmp(s + 8, k_spk_lc[k])) { sec = S_SPEAKER; idx = k; }
                if (sec == S_SKIP) set_err(err, nerr, "line %d: no speaker %s", line_no, s + 8);
            } else if (!strncmp(s, "bus.", 4)) {
                idx = apu_bus_find(s + 4);
                sec = idx >= 0 ? S_BUS : S_SKIP;
                if (idx < 0) set_err(err, nerr, "line %d: no bus %s", line_no, s + 4);
            } else {
                sec = S_SKIP;
                set_err(err, nerr, "line %d: unknown section %s", line_no, s);
            }
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) { set_err(err, nerr, "line %d: %s", line_no, "expected key = value"); return -1; }
        *eq = 0;
        key = s;
        for (n = strlen(key); n && isspace((unsigned char)key[n - 1]); ) key[--n] = 0;
        for (val = eq + 1; isspace((unsigned char)*val); val++) {}
        kind = parse_value(val, &num, &str);
        if (kind < 0) { set_err(err, nerr, "line %d: bad value for %s", line_no, key); return -1; }
#define UNKNOWN() set_err(err, nerr, "line %d: unknown key %s", line_no, key)
        switch (sec) {
        case S_MASTER:
            if (!strcmp(key, "limiter")) {
                c.limiter = kind == 2 ? (num ? APU_BUS_LIM_PEAK : APU_BUS_LIM_OFF) : APU_BUS_LIM_OFF;
                for (int k = 0; str && k < 3; k++)
                    if (!strcmp(str, k_lim[k])) c.limiter = k;
            } else if (!strcmp(key, "ceiling_db")) c.ceiling_db = (float)num;
            else if (!strcmp(key, "limiter_release_ms")) c.limiter_release_ms = (float)num;
            else if (!strcmp(key, "lfe_lowpass_hz")) c.lfe_lowpass_hz = num <= 0 ? 0 : (int)num;
            else if (!strcmp(key, "mains_highpass_hz")) c.mains_highpass_hz = num <= 0 ? 0 : (int)num;
            else UNKNOWN();
            break;
        case S_SPEAKER:
            if (!strcmp(key, "trim_db")) c.speaker_trim_db[idx] = (float)num;
            else if (!strcmp(key, "delay_ms")) c.speaker_delay_ms[idx] = num < 0 ? 0 : num > 50 ? 50 : (float)num;
            else UNKNOWN();
            break;
        case S_DUCK:
            if (!strcmp(key, "enabled")) c.duck.enabled = num != 0;
            else if (!strcmp(key, "source") || !strcmp(key, "target")) {
                int b = str ? apu_bus_find(str) : -1;
                if (b < 0) set_err(err, nerr, "line %d: no bus for %s", line_no, key);
                else if (key[0] == 's') c.duck.source = b;
                else c.duck.target = b;
            } else if (!strcmp(key, "threshold_db")) c.duck.threshold_db = (float)num;
            else if (!strcmp(key, "depth_db")) c.duck.depth_db = (float)(num < 0 ? -num : num);
            else if (!strcmp(key, "attack_ms")) c.duck.attack_ms = (float)num;
            else if (!strcmp(key, "release_ms")) c.duck.release_ms = (float)num;
            else UNKNOWN();
            break;
        case S_BUS: {
            apu_bus_params *b = &c.bus[idx];
            int done = 0;
            if (!strcmp(key, "gain_db")) b->gain_db = (float)num, done = 1;
            else if (!strcmp(key, "mute")) b->mute = num != 0 && kind != 1, done = 1;
            else if (!strcmp(key, "solo")) b->solo = num != 0 && kind != 1, done = 1;
            else if (!strcmp(key, "route")) {
                b->route = str && !strcmp(str, "sends") ? APU_BUS_ROUTE_SENDS
                         : str && !strcmp(str, "pan") ? APU_BUS_ROUTE_PAN : APU_BUS_ROUTE_TITLE;
                done = 1;
            } else if (!strcmp(key, "lfe_send_db")) b->lfe_send_db = (float)num, done = 1;
            else if (!strcmp(key, "lfe_crossover_hz")) b->lfe_crossover_hz = num <= 0 ? 0 : (int)num, done = 1;
            else if (!strcmp(key, "pan_x")) b->pan_x = clampf1((float)num, -1.0f, 1.0f), done = 1;
            else if (!strcmp(key, "pan_y")) b->pan_y = clampf1((float)num, -1.0f, 1.0f), done = 1;
            else if (!strcmp(key, "pan_width")) b->pan_width = clampf1((float)num, 0.0f, 1.0f), done = 1;
            else if (!strcmp(key, "pan_divergence")) b->pan_divergence = clampf1((float)num, 0.0f, 1.0f), done = 1;
            else if (!strcmp(key, "field_width")) b->field_width = clampf1((float)num, 0.0f, 2.0f), done = 1;
            else if (!strcmp(key, "field_divergence")) b->field_divergence = clampf1((float)num, -1.0f, 1.0f), done = 1;
            for (int t = 0; !done && t < APU_BUS_TRIMS; t++)
                if (!strcmp(key, k_trim[t])) b->trim_db[t] = (float)num, done = 1;
            if (!done && !strncmp(key, "send_", 5)) {
                for (int k = 0; k < APU_BUS_SPEAKERS; k++) {
                    char want[16];
                    snprintf(want, sizeof(want), "send_%s_db", k_spk_lc[k]);
                    if (!strcmp(key, want)) b->send_db[k] = (float)num, done = 1;
                }
            }
            if (!done) UNKNOWN();
            break;
        }
        case S_NONE:
            UNKNOWN();
            break;
        default:
            break;
        }
#undef UNKNOWN
    }
    *out = c;
    return 0;
}

static size_t put(char *out, size_t n, size_t at, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
#include <stdarg.h>
static size_t put(char *out, size_t n, size_t at, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (at >= n) return at;
    va_start(ap, fmt);
    k = vsnprintf(out + at, n - at, fmt, ap);
    va_end(ap);
    return k < 0 ? at : at + (size_t)k < n ? at + (size_t)k : n - 1;
}

static size_t put_db(char *out, size_t n, size_t at, const char *key, float db)
{
    if (db <= APU_BUS_OFF_DB)
        return put(out, n, at, "%s = \"off\"\n", key);
    return put(out, n, at, "%s = %.1f\n", key, (double)db);
}

size_t apu_bus_config_write(const apu_bus_config *c, char *out, size_t n)
{
    size_t at = 0;
    char key[40];
    at = put(out, n, at,
        "# Mixer preset. Levels in dB (\"off\" = silence), times in ms, frequencies in Hz.\n"
        "# Edited while the game runs: the file is read again when it changes.\n\n"
        "[master]\nlimiter = \"%s\"            # off | soft | peak\n",
        k_lim[c->limiter >= 0 && c->limiter < 3 ? c->limiter : 0]);
    at = put(out, n, at, "ceiling_db = %.1f\nlimiter_release_ms = %.0f\n"
        "lfe_lowpass_hz = %d          # 0 = off; 120 is what the console's encoder did\n"
        "mains_highpass_hz = %d       # 0 = off\n",
        (double)c->ceiling_db, (double)c->limiter_release_ms, c->lfe_lowpass_hz, c->mains_highpass_hz);
    for (int s = 0; s < APU_BUS_SPEAKERS; s++)
        at = put(out, n, at, "\n[speaker.%s]\ntrim_db = %.1f\ndelay_ms = %.1f\n", k_spk[s],
                 (double)c->speaker_trim_db[s], (double)c->speaker_delay_ms[s]);
    at = put(out, n, at, "\n[duck]                     # the target bus goes down while the source is above the threshold\n"
                         "enabled = %s\n", c->duck.enabled ? "true" : "false");
    apu_bus_key(c->duck.source, key, sizeof(key));
    at = put(out, n, at, "source = \"%s\"\n", key);
    apu_bus_key(c->duck.target, key, sizeof(key));
    at = put(out, n, at, "target = \"%s\"\nthreshold_db = %.1f\ndepth_db = %.1f\nattack_ms = %.0f\nrelease_ms = %.0f\n",
             key, (double)c->duck.threshold_db, (double)c->duck.depth_db,
             (double)c->duck.attack_ms, (double)c->duck.release_ms);
    for (int b = 0; b < s_nbus; b++) {
        const apu_bus_params *p = &c->bus[b];
        apu_bus_key(b, key, sizeof(key));
        at = put(out, n, at, "\n[bus.%s]                # %s\n", key, apu_bus_name(b));
        at = put(out, n, at, "gain_db = %.1f\nmute = %s\nsolo = %s\n", (double)p->gain_db,
                 p->mute ? "true" : "false", p->solo ? "true" : "false");
        at = put(out, n, at, "route = \"%s\"            # title: the game's own panning (trims, field_*); sends: the six sends; pan: a place in the room (pan_*)\n",
                 p->route == APU_BUS_ROUTE_SENDS ? "sends" : p->route == APU_BUS_ROUTE_PAN ? "pan" : "title");
        for (int t = 0; t < APU_BUS_TRIMS; t++)
            at = put(out, n, at, "%s = %.1f\n", k_trim[t], (double)p->trim_db[t]);
        at = put(out, n, at, "field_width = %.2f\nfield_divergence = %.2f\n",
                 (double)p->field_width, (double)p->field_divergence);
        for (int s = 0; s < APU_BUS_SPEAKERS; s++) {
            char k2[24];
            snprintf(k2, sizeof(k2), "send_%s_db", k_spk_lc[s]);
            at = put_db(out, n, at, k2, p->send_db[s]);
        }
        at = put(out, n, at, "pan_x = %.2f\npan_y = %.2f\npan_width = %.2f\npan_divergence = %.2f\n",
                 (double)p->pan_x, (double)p->pan_y, (double)p->pan_width, (double)p->pan_divergence);
        at = put_db(out, n, at, "lfe_send_db", p->lfe_send_db);
        at = put(out, n, at, "lfe_crossover_hz = %d\n", p->lfe_crossover_hz);
    }
    return at;
}

static char s_file[1024], s_file_status[200];
static volatile unsigned s_file_gen;

unsigned apu_bus_file_gen(void) { return s_file_gen; }
static long long s_file_mtime = -1;

static long long file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st)) return -1;
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec + st.st_size;
#elif defined(_WIN32)
    return (long long)st.st_mtime * 1000 + st.st_size;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
#endif
}

int apu_bus_load_file(const char *path, apu_bus_config *c, char *err, size_t nerr)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    int rc;
    if (!f) {
        if (err && nerr) snprintf(err, nerr, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    rc = apu_bus_config_parse(buf, c, err, nerr);
    free(buf);
    return rc;
}

int apu_bus_save_file(const char *path, const apu_bus_config *c)
{
    char *buf = (char *)malloc(32768);
    FILE *f;
    size_t n;
    if (!buf) return -1;
    n = apu_bus_config_write(c, buf, 32768);
    f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, n, f);
    fclose(f);
    free(buf);
    if (!strcmp(path, s_file))
        s_file_mtime = file_mtime(path);    /* our own write is not a change */
    return 0;
}

static void file_check(int force)
{
    long long m;
    apu_bus_config c;
    char err[160];
    if (!s_file[0]) return;
    m = file_mtime(s_file);
    if (m < 0 || (!force && m == s_file_mtime)) return;
    s_file_mtime = m;
    apu_bus_config_neutral(&c);
    if (apu_bus_load_file(s_file, &c, err, sizeof(err)) == 0) {
        apu_bus_set_config(&c);
        s_file_gen++;
        snprintf(s_file_status, sizeof(s_file_status), "%s", err);
        fprintf(stderr, "[APU] mixer: %s loaded%s%s\n", s_file, *err ? ", " : "", err);
    } else {
        snprintf(s_file_status, sizeof(s_file_status), "%s", err);
        fprintf(stderr, "[APU] mixer: %s not loaded: %s\n", s_file, err);
    }
}

/* The mixer is on and a file is named: make it if it is not there, read it. */
static void file_open(void)
{
    FILE *f;
    if (!s_file[0]) return;
    f = fopen(s_file, "rb");
    if (f) {
        fclose(f);
    } else {
        apu_bus_config c;
        apu_bus_get_config(&c);
        apu_bus_save_file(s_file, &c);
    }
    s_file_mtime = -1;
    file_check(1);
}

void apu_bus_set_file(const char *path)
{
    snprintf(s_file, sizeof(s_file), "%s", path ? path : "");
    s_file_mtime = -1;
    s_file_status[0] = 0;
    if (apu_bus_enabled())
        file_open();
}

static struct { const char *name, *about, *toml; } s_presets[16];
static int s_npresets;

void apu_bus_add_preset(const char *name, const char *about, const char *toml)
{
    if (s_npresets < 16) {
        s_presets[s_npresets].name = name;
        s_presets[s_npresets].about = about;
        s_presets[s_npresets].toml = toml;
        s_npresets++;
    }
}

int apu_bus_preset_count(void) { return s_npresets; }
const char *apu_bus_preset_name(int i) { return i >= 0 && i < s_npresets ? s_presets[i].name : ""; }
const char *apu_bus_preset_about(int i) { return i >= 0 && i < s_npresets ? s_presets[i].about : ""; }

int apu_bus_preset_get(int i, apu_bus_config *c)
{
    if (i < 0 || i >= s_npresets) return -1;
    apu_bus_config_neutral(c);
    return apu_bus_config_parse(s_presets[i].toml, c, NULL, 0);
}

void apu_bus_commit(const apu_bus_config *c)
{
    apu_bus_set_config(c);
    if (s_file[0])
        apu_bus_save_file(s_file, c);
}

void apu_bus_preset_apply(int i)
{
    apu_bus_config c;
    if (apu_bus_preset_get(i, &c) == 0)
        apu_bus_commit(&c);
}

const char *apu_bus_file(void) { return s_file; }
const char *apu_bus_file_status(void) { return s_file_status; }

/* ── audition (apu_bus.h) ──────────────────────────────────────────── */

/* A frame in the store: u16 voices, then per voice
 *   u8 bus, u8 channels, u8 bin[8], float g[8], s16 samples[32 * channels].
 * The store is a byte ring that a frame never wraps in (it restarts at 0
 * when it would), and a ring of the frames' offsets, oldest first. */
#define AUD_BYTES_A_SECOND (3u << 20)       /* the store's size a second kept */
#define AUD_FRAME_MAX      (2 + 64 * (2 + 8 + 32 + 128))
#define AUD_XFADE_FRAMES   3000u            /* 2 s across the loop point */

static struct {
    volatile int capturing;                 /* frame thread captures into the ring */
    volatile int state;                     /* APU_BUS_AUD_* */
    volatile int request;                   /* 1 loop last, 2 loop the loaded clip, 3 stop */
    volatile int want_seconds;              /* asked for; the frame thread makes it so */
    int seconds;
    /* the ring */
    uint8_t *buf;
    size_t cap, wr;
    uint32_t *off;                          /* frame offsets, a ring of nmax */
    uint16_t *len;                          /* and their lengths */
    uint32_t nmax, first, count;
    size_t used;                            /* bytes of the frames held */
    uint8_t *cur;                           /* the frame being written */
    size_t cur_len;
    unsigned cur_voices;
    /* a loaded clip (apu_bus_audition_loop_file), linear */
    uint8_t *clip;
    uint32_t *clip_off;
    uint32_t clip_count;
    size_t clip_bytes;
    /* playing */
    uint32_t pos;
} s_aud;

static void aud_alloc(int seconds)
{
    free(s_aud.buf);
    free(s_aud.off);
    free(s_aud.len);
    s_aud.buf = NULL;
    s_aud.off = NULL;
    s_aud.len = NULL;
    s_aud.used = 0;
    s_aud.cap = s_aud.wr = 0;
    s_aud.nmax = s_aud.first = s_aud.count = 0;
    s_aud.seconds = seconds;
    if (seconds <= 0) return;
    s_aud.cap = (size_t)seconds * AUD_BYTES_A_SECOND;
    s_aud.nmax = (uint32_t)seconds * 1500u;
    s_aud.buf = (uint8_t *)malloc(s_aud.cap);
    s_aud.off = (uint32_t *)malloc(sizeof(uint32_t) * s_aud.nmax);
    s_aud.len = (uint16_t *)malloc(sizeof(uint16_t) * s_aud.nmax);
    if (!s_aud.buf || !s_aud.off || !s_aud.len) {
        free(s_aud.buf);
        free(s_aud.off);
        free(s_aud.len);
        s_aud.buf = NULL;
        s_aud.off = NULL;
        s_aud.len = NULL;
        s_aud.cap = 0;
        s_aud.nmax = 0;
    }
}

/* Frames of the ring that begin inside [a, b) are overwritten: gone. */
static void aud_drop(size_t a, size_t b)
{
    while (s_aud.count) {
        const uint32_t o = s_aud.off[s_aud.first];
        if (o < a || o >= b) break;
        s_aud.used -= s_aud.len[s_aud.first];
        s_aud.first = (s_aud.first + 1) % s_aud.nmax;
        s_aud.count--;
    }
}

static void aud_capture_voice(int bus, const float (*samples)[2], unsigned channels,
                              const int bin[8], const float g[8])
{
    uint8_t *p;
    int sounding = 0;
    for (int b = 0; b < 8; b++) sounding |= g[b] > 0.0f;
    if (!sounding || !s_aud.cur || s_aud.cur_voices >= 64) return;
    p = s_aud.cur + s_aud.cur_len;
    p[0] = (uint8_t)bus;
    p[1] = (uint8_t)(channels > 1 ? 2 : 1);
    for (int b = 0; b < 8; b++) p[2 + b] = (uint8_t)bin[b];
    memcpy(p + 10, g, sizeof(float) * 8);
    p += 42;
    for (int i = 0; i < APU_BUS_FRAME; i++)
        for (unsigned c = 0; c < (channels > 1 ? 2u : 1u); c++) {
            float x = samples[i][c] * 32767.0f;
            int16_t v = (int16_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f
                                  : x < 0 ? x - 0.5f : x + 0.5f);
            memcpy(p, &v, 2);
            p += 2;
        }
    s_aud.cur_len = (size_t)(p - s_aud.cur);
    s_aud.cur_voices++;
}

static void aud_frame_begin(void)
{
    /* Requests from the other thread, between frames. */
    const int req = s_aud.request;
    if (req) {
        s_aud.request = 0;
        if (req == 1 && s_aud.count > AUD_XFADE_FRAMES * 2) {
            s_aud.state = APU_BUS_AUD_LAST;
            s_aud.pos = 0;
            fprintf(stderr, "[APU] audition: looping the last %.1f s (%.1f MB, %.2f MB a second)\n",
                    s_aud.count / 1500.0, s_aud.used / 1048576.0,
                    s_aud.used / 1048576.0 / (s_aud.count / 1500.0));
        } else if (req == 2 && s_aud.clip && s_aud.clip_count > AUD_XFADE_FRAMES * 2) {
            s_aud.state = APU_BUS_AUD_FILE;
            s_aud.pos = 0;
        } else if (req == 3) {
            if (s_aud.state != APU_BUS_AUD_OFF)
                fprintf(stderr, "[APU] audition: stopped, the live sound again\n");
            s_aud.state = APU_BUS_AUD_OFF;
            s_aud.first = s_aud.count = 0;      /* a fresh capture: no jump in it */
            s_aud.wr = 0;
            s_aud.used = 0;
        }
    }
    s_aud.cur = NULL;
    s_aud.capturing = 0;
    /* The store's size, here so that no frame is ever in it meanwhile. */
    if (s_aud.want_seconds != s_aud.seconds && s_aud.state != APU_BUS_AUD_LAST)
        aud_alloc(s_aud.want_seconds);
    if (s_aud.state != APU_BUS_AUD_OFF || !s_aud.buf) return;
    if (s_aud.wr + AUD_FRAME_MAX > s_aud.cap) {
        aud_drop(s_aud.wr, s_aud.cap);
        s_aud.wr = 0;
    }
    aud_drop(s_aud.wr, s_aud.wr + AUD_FRAME_MAX);
    s_aud.cur = s_aud.buf + s_aud.wr;
    s_aud.cur_len = 2;
    s_aud.cur_voices = 0;
    s_aud.capturing = 1;
}

static void aud_frame_end(void)
{
    uint16_t n;
    if (!s_aud.capturing || !s_aud.cur) return;
    n = (uint16_t)s_aud.cur_voices;
    memcpy(s_aud.cur, &n, 2);
    if (s_aud.count == s_aud.nmax) {
        s_aud.used -= s_aud.len[s_aud.first];
        s_aud.first = (s_aud.first + 1) % s_aud.nmax;
        s_aud.count--;
    }
    s_aud.off[(s_aud.first + s_aud.count) % s_aud.nmax] = (uint32_t)s_aud.wr;
    s_aud.len[(s_aud.first + s_aud.count) % s_aud.nmax] = (uint16_t)s_aud.cur_len;
    s_aud.used += s_aud.cur_len;
    s_aud.count++;
    s_aud.wr += s_aud.cur_len;
    s_aud.capturing = 0;
    s_aud.cur = NULL;
}

void apu_bus_audition_keep(int seconds)
{
    s_aud.want_seconds = seconds < 0 ? 0 : seconds > 60 ? 60 : seconds;
}

int apu_bus_audition_kept_seconds(void) { return s_aud.want_seconds; }

int apu_bus_audition_playing(void) { return s_aud.state != APU_BUS_AUD_OFF; }
int apu_bus_audition_state(void) { return s_aud.state; }

static const uint8_t *aud_frame_at(uint32_t i, uint32_t *count)
{
    if (s_aud.state == APU_BUS_AUD_FILE) {
        *count = s_aud.clip_count;
        return i < s_aud.clip_count ? s_aud.clip + s_aud.clip_off[i] : NULL;
    }
    *count = s_aud.count;
    return i < s_aud.count ? s_aud.buf + s_aud.off[(s_aud.first + i) % s_aud.nmax] : NULL;
}

static void aud_mix_frame(struct apu_bus_rt *rt, float (*mixbins)[APU_BUS_FRAME],
                          const uint8_t *f, float fade)
{
    uint16_t n;
    float smp[APU_BUS_FRAME][2];
    if (!f || fade <= 0.0f) return;
    memcpy(&n, f, 2);
    f += 2;
    for (unsigned k = 0; k < n; k++) {
        const int bus = f[0];
        const unsigned ch = f[1];
        int bin[8];
        float g[8];
        for (int b = 0; b < 8; b++) bin[b] = f[2 + b] & (APU_BUS_BINS - 1);
        memcpy(g, f + 10, sizeof(g));
        f += 42;
        for (int i = 0; i < APU_BUS_FRAME; i++) {
            int16_t v;
            memcpy(&v, f, 2);
            f += 2;
            smp[i][0] = (float)v * (1.0f / 32767.0f);
            if (ch > 1) {
                memcpy(&v, f, 2);
                f += 2;
                smp[i][1] = (float)v * (1.0f / 32767.0f);
            } else {
                smp[i][1] = smp[i][0];
            }
        }
        if (fade != 1.0f)
            for (int b = 0; b < 8; b++) g[b] *= fade;
        apu_bus_mix_voice(rt, bus < s_nbus ? bus : (s_nbus ? s_nbus - 1 : 0), mixbins,
                          (const float (*)[2])smp, ch, bin, g);
    }
}

void apu_bus_audition_mix(struct apu_bus_rt *rt, float (*mixbins)[APU_BUS_FRAME])
{
    uint32_t count, len;
    const uint8_t *f = aud_frame_at(0, &count);
    (void)f;
    if (s_aud.state == APU_BUS_AUD_OFF || count <= AUD_XFADE_FRAMES * 2) return;
    /* The loop is the clip less the cross-fade: over its first two seconds
     * the clip's last two fade out under it, equal power. */
    len = count - AUD_XFADE_FRAMES;
    if (s_aud.pos >= len) s_aud.pos = 0;
    if (s_aud.pos < AUD_XFADE_FRAMES) {
        const float t = ((float)s_aud.pos + 0.5f) / (float)AUD_XFADE_FRAMES;
        aud_mix_frame(rt, mixbins, aud_frame_at(s_aud.pos, &count), sinf(t * (float)(M_PI / 2.0)));
        aud_mix_frame(rt, mixbins, aud_frame_at(len + s_aud.pos, &count), cosf(t * (float)(M_PI / 2.0)));
    } else {
        aud_mix_frame(rt, mixbins, aud_frame_at(s_aud.pos, &count), 1.0f);
    }
    s_aud.pos++;
}

/* The control side (the overlay's thread). A request is taken at the next
 * frame; with no frame thread running (tests) the caller's next
 * apu_bus_frame_begin takes it. */
int apu_bus_audition_loop_last(void)
{
    if (!s_aud.buf || s_aud.count <= AUD_XFADE_FRAMES * 2) return -1;
    s_aud.request = 1;
    return 0;
}

void apu_bus_audition_stop(void)
{
    s_aud.request = 3;
}

#define AUD_MAGIC "APUBUSAUD1"

int apu_bus_audition_save(const char *path)
{
    FILE *f;
    uint32_t count, i;
    if (s_aud.state == APU_BUS_AUD_OFF) return -1;      /* only a clip that stands still */
    aud_frame_at(0, &count);
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(AUD_MAGIC, 1, 10, f);
    fwrite(&count, 4, 1, f);
    for (i = 0; i < count; i++) {
        const uint8_t *fr = aud_frame_at(i, &count), *p = fr + 2;
        uint16_t n;
        uint32_t len;
        memcpy(&n, fr, 2);
        for (unsigned k = 0; k < n; k++)
            p += 42 + 2 * APU_BUS_FRAME * (p[1] > 1 ? 2 : 1);
        len = (uint32_t)(p - fr);
        fwrite(&len, 4, 1, f);
        fwrite(fr, 1, len, f);
    }
    return fclose(f) == 0 ? 0 : -1;
}

int apu_bus_audition_loop_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[10];
    uint32_t count, i;
    long size;
    uint8_t *clip;
    uint32_t *off;
    size_t at = 0;

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 14 || fread(magic, 1, 10, f) != 10 || memcmp(magic, AUD_MAGIC, 10) ||
        fread(&count, 4, 1, f) != 1 || count <= AUD_XFADE_FRAMES * 2 || count > 1500u * 600u) {
        fclose(f);
        return -1;
    }
    clip = (uint8_t *)malloc((size_t)size);
    off = (uint32_t *)malloc(sizeof(uint32_t) * count);
    if (!clip || !off) { free(clip); free(off); fclose(f); return -1; }
    for (i = 0; i < count; i++) {
        uint32_t len;
        if (fread(&len, 4, 1, f) != 1 || len < 2 || len > AUD_FRAME_MAX || at + len > (size_t)size ||
            fread(clip + at, 1, len, f) != len)
            break;
        off[i] = (uint32_t)at;
        at += len;
    }
    fclose(f);
    if (i != count) { free(clip); free(off); return -1; }
    /* Swap it in with the loop off, a frame's time apart from its use. */
    if (s_aud.state == APU_BUS_AUD_FILE) {
        s_aud.request = 3;
        for (int k = 0; k < 50 && s_aud.state == APU_BUS_AUD_FILE; k++) {
#if defined(_WIN32)
            _sleep(2);
#else
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
#endif
        }
        s_aud.state = APU_BUS_AUD_OFF;
    }
    free(s_aud.clip);
    free(s_aud.clip_off);
    s_aud.clip = clip;
    s_aud.clip_off = off;
    s_aud.clip_count = count;
    s_aud.clip_bytes = at;
    s_aud.request = 2;
    return 0;
}

void apu_bus_audition_get(apu_bus_audition_info *o)
{
    uint32_t count = 0;
    memset(o, 0, sizeof(*o));
    o->crossfade_s = (float)AUD_XFADE_FRAMES / 1500.0f;
    if (s_aud.state != APU_BUS_AUD_OFF) {
        aud_frame_at(0, &count);
        o->length_s = count > AUD_XFADE_FRAMES ? (float)(count - AUD_XFADE_FRAMES) / 1500.0f : 0.0f;
        o->position_s = (float)s_aud.pos / 1500.0f;
    }
    o->kept_s = (float)s_aud.count / 1500.0f;
    o->bytes = s_aud.cap + (size_t)s_aud.nmax * 4 + s_aud.clip_bytes;
}

/* ── the mix ───────────────────────────────────────────────────────── */

struct apu_bus_rt *apu_bus_frame_begin(int channels, const int8_t spk_map[APU_BUS_BINS])
{
    struct apu_bus_rt *r;
    if (s_on <= 0) {
        if (s_on == 0 || !apu_bus_enabled()) return NULL;
    }
    /* The preset file, once a second. */
    if (++s_st.file_frames >= 1500) {
        s_st.file_frames = 0;
        file_check(0);
    }
    r = s_cur;
    if (!r) return NULL;
    if (!s_st_init) {
        s_st.lim_env = s_st.lim_min = 1.0f;
        for (int k = 0; k < LOOKAHEAD; k++) s_st.look_gain[k] = 1.0f;
        s_st_init = 1;
    }
    aud_frame_begin();
    r->channels = channels == APU_BUS_SPEAKERS ? APU_BUS_SPEAKERS : 2;
    for (int b = 0; b < APU_BUS_BINS; b++) {
        int sp = r->channels == APU_BUS_SPEAKERS && spk_map ? spk_map[b] : 0;
        /* For the field shaping: the speaker a bin feeds; on a stereo
         * output only the left/right pair itself. */
        r->spk[b] = (int8_t)(r->channels == APU_BUS_SPEAKERS ? sp : b < 2 ? b : -1);
        r->cls[b] = sp < 0 ? APU_BUS_TRIMS
                  : sp <= 1 ? APU_BUS_TRIM_FRONT
                  : sp == 2 ? APU_BUS_TRIM_CENTRE
                  : sp == 3 ? APU_BUS_TRIM_LFE : APU_BUS_TRIM_REAR;
    }
    for (int b = 0; b < APU_BUS_MAX; b++)
        r->eff[b] = r->lin[b];
    if (r->duck.enabled) {
        if (s_st.duck_red_db > 0.01f)
            r->eff[r->duck.target] *= db_lin(-s_st.duck_red_db);
    } else {
        s_st.duck_red_db = 0.0f;
    }
    return r;
}

void apu_bus_mix_voice(struct apu_bus_rt *r, int bus,
                       float (*mixbins)[APU_BUS_FRAME],
                       const float (*samples)[2], unsigned channels,
                       const int bin[8], const float g[8])
{
    const float eff = r->eff[bus];
    float ss[2] = { 0.0f, 0.0f }, ref = 0.0f;
    double level = 0.0;

    if (s_aud.capturing)
        aud_capture_voice(bus, samples, channels, bin, g);

    /* The meters: the energy of the voice's channels, once. */
    for (int i = 0; i < APU_BUS_FRAME; i++) {
        ss[0] += samples[i][0] * samples[i][0];
        ss[1] += samples[i][1] * samples[i][1];
    }
    for (int b = 0; b < 8; b++)
        if (g[b] > ref) ref = g[b];

    if (r->route[bus] == APU_BUS_ROUTE_TITLE && r->shaped[bus]) {
        /* The title's own panning, reshaped: the width of its FL/FR and
         * SL/SR pairs around the centre line, and its centre send against
         * a phantom centre, each keeping the voice's power. The sends stay
         * on the bins (and polarities) the title chose. */
        const float *trim = r->trim[bus];
        float gg[8];
        int slot[APU_BUS_SPEAKERS] = { -1, -1, -1, -1, -1, -1 };
        for (int b = 0; b < 8; b++) {
            const int sp = r->spk[bin[b]];
            gg[b] = g[b];
            if (sp >= 0 && sp < APU_BUS_SPEAKERS && slot[sp] < 0)
                slot[sp] = b;       /* the first send to a speaker: a later one is an effect send */
        }
        if (r->field_w[bus] != 1.0f) {
            if (slot[0] >= 0 && slot[1] >= 0)
                apu_bus_field_width(r->field_w[bus], &gg[slot[0]], &gg[slot[1]]);
            if (slot[4] >= 0 && slot[5] >= 0)
                apu_bus_field_width(r->field_w[bus], &gg[slot[4]], &gg[slot[5]]);
        }
        if (r->field_d[bus] != 0.0f && slot[0] >= 0 && slot[1] >= 0 && slot[2] >= 0)
            apu_bus_field_divergence(r->field_d[bus], &gg[slot[0]], &gg[slot[1]], &gg[slot[2]]);
        for (int b = 0; b < 8; b++) {
            const float t = eff * trim[r->cls[bin[b]]] * gg[b];
            for (int i = 0; i < APU_BUS_FRAME; i++)
                mixbins[bin[b]][i] += t * samples[i][(unsigned)b % channels];
            level += (double)t * t * ss[(unsigned)b % channels];
        }
    } else if (r->route[bus] == APU_BUS_ROUTE_PAN) {
        /* A place in the room at the voice's own level (its loudest send):
         * each channel through its own constant-power gains. */
        const float base = ref * eff;
        if (r->channels == APU_BUS_SPEAKERS) {
            static const uint8_t pbin[5] = { 0, 1, 2, 4, 5 };   /* FL FR C SL SR */
            if (channels > 1) {
                const float *gl = r->pan6[bus][0], *gr = r->pan6[bus][1];
                for (int k = 0; k < 5; k++) {
                    const float a = base * gl[k], c = base * gr[k];
                    if (a == 0.0f && c == 0.0f) continue;
                    for (int i = 0; i < APU_BUS_FRAME; i++)
                        mixbins[pbin[k]][i] += a * samples[i][0] + c * samples[i][1];
                    level += (double)a * a * ss[0] + (double)c * c * ss[1];
                }
            } else {
                const float *gm = r->pan6[bus][2];
                for (int k = 0; k < 5; k++) {
                    const float a = base * gm[k];
                    if (a == 0.0f) continue;
                    for (int i = 0; i < APU_BUS_FRAME; i++)
                        mixbins[pbin[k]][i] += a * samples[i][0];
                    level += (double)a * a * ss[0];
                }
            }
        } else {
            const float (*p2)[2] = r->pan2[bus];
            if (channels > 1) {
                for (int k = 0; k < 2; k++) {
                    const float a = base * p2[0][k], c = base * p2[1][k];
                    for (int i = 0; i < APU_BUS_FRAME; i++)
                        mixbins[k][i] += a * samples[i][0] + c * samples[i][1];
                    level += (double)a * a * ss[0] + (double)c * c * ss[1];
                }
            } else {
                for (int k = 0; k < 2; k++) {
                    const float a = base * p2[2][k];
                    for (int i = 0; i < APU_BUS_FRAME; i++)
                        mixbins[k][i] += a * samples[i][0];
                    level += (double)a * a * ss[0];
                }
            }
        }
    } else if (!r->route[bus]) {
        /* The title's own bins and volumes. With eff 1 and the trims at
         * 0 dB this is the loop of apu_vp.c, operation for operation. */
        const float *trim = r->trim[bus];
        const int plain = r->plain[bus];
        for (int b = 0; b < 8; b++) {
            float gg = g[b];
            if (eff != 1.0f) gg *= eff;
            if (!plain) {
                const float t = trim[r->cls[bin[b]]];
                if (t != 1.0f) gg *= t;
            }
            for (int i = 0; i < APU_BUS_FRAME; i++)
                mixbins[bin[b]][i] += gg * samples[i][(unsigned)b % channels];
            level += (double)gg * gg * ss[(unsigned)b % channels];
        }
    } else {
        /* Six sends at the voice's own level (its loudest send): the left
         * channel to the left speakers, the right to the right ones, the
         * mono sum to the centre and the LFE. Stereo output: the ITU fold
         * of the same sends, no LFE. */
        const float *send = r->send[bus];
        const float base = ref * eff;
        const unsigned rc = channels > 1 ? 1u : 0u;
        float gl[APU_BUS_SPEAKERS];
        for (int s = 0; s < APU_BUS_SPEAKERS; s++)
            gl[s] = base * send[s];
        if (r->channels == APU_BUS_SPEAKERS) {
            /* Bins 0..5 are the speaker feeds themselves: FL FR C LFE SL SR. */
            static const uint8_t side[APU_BUS_SPEAKERS] = { 0, 1, 2, 2, 0, 1 };
            for (int s = 0; s < APU_BUS_SPEAKERS; s++) {
                const float gs = gl[s];
                if (gs == 0.0f) continue;
                if (side[s] == 0) {
                    for (int i = 0; i < APU_BUS_FRAME; i++) mixbins[s][i] += gs * samples[i][0];
                    level += (double)gs * gs * ss[0];
                } else if (side[s] == 1) {
                    for (int i = 0; i < APU_BUS_FRAME; i++) mixbins[s][i] += gs * samples[i][rc];
                    level += (double)gs * gs * ss[rc];
                } else {
                    const float h = 0.5f * gs;
                    for (int i = 0; i < APU_BUS_FRAME; i++)
                        mixbins[s][i] += h * (samples[i][0] + samples[i][rc]);
                    level += (double)h * h * (ss[0] + ss[rc]);
                }
            }
        } else {
            const float k = 0.70710678f;
            const float l = gl[0] + k * gl[4], rr = gl[1] + k * gl[5], c = 0.5f * k * gl[2];
            for (int i = 0; i < APU_BUS_FRAME; i++) {
                const float m = c * (samples[i][0] + samples[i][rc]);
                mixbins[0][i] += l * samples[i][0] + m;
                mixbins[1][i] += rr * samples[i][rc] + m;
            }
            level += (double)l * l * ss[0] + (double)rr * rr * ss[rc];
        }
    }

    if (r->lfe_send[bus] > 0.0f && r->channels == APU_BUS_SPEAKERS) {
        const float gs = 0.5f * ref * eff * r->lfe_send[bus];
        const unsigned rc = channels > 1 ? 1u : 0u;
        float *acc = s_st.lfe_acc[bus];
        for (int i = 0; i < APU_BUS_FRAME; i++)
            acc[i] += gs * (samples[i][0] + samples[i][rc]);
        s_st.lfe_used[bus] = 1;
    }
    if (r->duck.enabled && bus == r->duck.source) {
        const float gs = 0.5f * ref * r->lin[bus];
        const unsigned rc = channels > 1 ? 1u : 0u;
        for (int i = 0; i < APU_BUS_FRAME; i++)
            s_st.duck_acc[i] += gs * (samples[i][0] + samples[i][rc]);
    }
    s_st.bus_sq[bus] += level;
    if (level > 1e-9)
        s_st.bus_voices[bus]++;
}

void apu_bus_frame_end(struct apu_bus_rt *r, float (*mixbins)[APU_BUS_FRAME])
{    aud_frame_end();

    if (r->any_lfe_send) {
        for (int b = 0; b < APU_BUS_MAX; b++) {
            float *acc = s_st.lfe_acc[b];
            if (r->lfe_send[b] <= 0.0f) continue;
            /* The low-pass keeps running through silence, so a send that
             * comes back does not click. */
            if (r->lfe_xover[b]) {
                for (int i = 0; i < APU_BUS_FRAME; i++) {
                    double y = bq_step(&r->lfe_coef[b][0], &s_st.lfe_send[b][0], acc[i]);
                    y = bq_step(&r->lfe_coef[b][1], &s_st.lfe_send[b][1], y);
                    mixbins[3][i] += (float)y;
                }
            } else if (s_st.lfe_used[b]) {
                for (int i = 0; i < APU_BUS_FRAME; i++)
                    mixbins[3][i] += acc[i];
            }
            if (s_st.lfe_used[b]) {
                memset(acc, 0, sizeof(s_st.lfe_acc[b]));
                s_st.lfe_used[b] = 0;
            }
        }
    }
    if (r->duck.enabled) {
        float sq = 0.0f, want;
        for (int i = 0; i < APU_BUS_FRAME; i++)
            sq += s_st.duck_acc[i] * s_st.duck_acc[i];
        memset(s_st.duck_acc, 0, sizeof(s_st.duck_acc));
        want = sq / APU_BUS_FRAME > r->duck_thr_sq ? r->duck.depth_db : 0.0f;
        s_st.duck_red_db += (want - s_st.duck_red_db)
                          * (want > s_st.duck_red_db ? r->duck_att : r->duck_rel);
    }
    /* The bus meters: about ten a second. */
    if (++s_st.meter_frames >= 150) {
        const double n = (double)s_st.meter_frames * APU_BUS_FRAME;
        for (int b = 0; b < APU_BUS_MAX; b++) {
            s_meters.bus_rms_db[b] = lin_db(sqrt(s_st.bus_sq[b] / n));
            s_meters.bus_voices[b] = (float)s_st.bus_voices[b] / (float)s_st.meter_frames;
            s_st.bus_sq[b] = 0.0;
            s_st.bus_voices[b] = 0;
        }
        s_meters.duck_db = s_st.duck_red_db;
        s_st.meter_frames = 0;
    }
}

int apu_bus_output_active(void)
{
    const struct apu_bus_rt *r = s_cur;
    return s_on > 0 && r && r->out_active;
}

static void out_meters(float (*feeds)[APU_BUS_FRAME], int nch, int n)
{
    for (int c = 0; c < nch; c++) {
        float pk = s_st.spk_peak[c];
        double sq = 0.0;
        unsigned over = 0;
        for (int i = 0; i < n; i++) {
            const float a = fabsf(feeds[c][i]);
            sq += (double)a * a;
            if (a > pk) pk = a;
            if (a > 1.0f) over++;
        }
        s_st.spk_sq[c] += sq;
        s_st.spk_peak[c] = pk;
        if (over) s_meters.spk_over[c] += over;
    }
    s_st.spk_n += (unsigned)n;
    if (s_st.spk_n >= 4800) {           /* 100 ms */
        for (int c = 0; c < APU_BUS_SPEAKERS; c++) {
            if (s_st.spk_peak[c] >= s_st.spk_hold[c] || ++s_st.spk_hold_age[c] > 15) {
                s_st.spk_hold[c] = s_st.spk_peak[c];
                s_st.spk_hold_age[c] = 0;
            }
            s_meters.spk_rms_db[c] = lin_db(sqrt(s_st.spk_sq[c] / s_st.spk_n));
            s_meters.spk_peak_db[c] = lin_db(s_st.spk_hold[c]);
            s_st.spk_sq[c] = 0.0;
            s_st.spk_peak[c] = 0.0f;
        }
        s_meters.limiter_db = -lin_db(s_st.lim_min);
        s_st.lim_min = 1.0f;
        s_meters.channels = nch;
        s_st.spk_n = 0;
    }
}

void apu_bus_output(float (*feeds)[APU_BUS_FRAME], int nch, int n)
{
    const struct apu_bus_rt *r = s_cur;
    if (!r || n > APU_BUS_FRAME) return;
    if (r->out_active) {
        /* Stereo: channel 0 and 1 take FL's and FR's trim and delay. */
        if (r->lfe_lp && nch == APU_BUS_SPEAKERS)
            for (int i = 0; i < n; i++) {
                double y = bq_step(&r->lfe_lp_coef[0], &s_st.lfe_lp[0], feeds[3][i]);
                feeds[3][i] = (float)bq_step(&r->lfe_lp_coef[1], &s_st.lfe_lp[1], y);
            }
        if (r->mains_hp)
            for (int c = 0; c < nch; c++) {
                if (c == 3 && nch == APU_BUS_SPEAKERS) continue;
                for (int i = 0; i < n; i++) {
                    double y = bq_step(&r->mains_hp_coef[0], &s_st.mains_hp[c][0], feeds[c][i]);
                    feeds[c][i] = (float)bq_step(&r->mains_hp_coef[1], &s_st.mains_hp[c][1], y);
                }
            }
        for (int c = 0; c < nch; c++) {
            const float t = r->spk_trim[c];
            const unsigned d = (unsigned)r->spk_delay[c];
            float *ring = s_st.delay[c];
            if (t != 1.0f)
                for (int i = 0; i < n; i++) feeds[c][i] *= t;
            /* The ring is always written, so a delay switched on later has
             * the past to read. */
            for (int i = 0; i < n; i++) {
                const unsigned w = (s_st.delay_pos + (unsigned)i) & (DELAY_MAX - 1);
                ring[w] = feeds[c][i];
                if (d) feeds[c][i] = ring[(w - d) & (DELAY_MAX - 1)];
            }
        }
        s_st.delay_pos = (s_st.delay_pos + (unsigned)n) & (DELAY_MAX - 1);

        if (r->limiter == APU_BUS_LIM_SOFT) {
            /* Linear up to 3 dB under the ceiling, then a tanh knee that
             * reaches the ceiling only at infinity. No delay. */
            const float ceil_ = r->ceiling, knee = 0.7071f * ceil_, span = ceil_ - knee;
            for (int c = 0; c < nch; c++)
                for (int i = 0; i < n; i++) {
                    const float x = feeds[c][i], a = fabsf(x);
                    if (a > knee) {
                        const float y = knee + span * tanhf((a - knee) / span);
                        if (y / a < s_st.lim_min) s_st.lim_min = y / a;
                        feeds[c][i] = x < 0 ? -y : y;
                    }
                }
        } else if (r->limiter == APU_BUS_LIM_PEAK) {
            /* A look-ahead peak limiter, all channels linked: the gain
             * that keeps the loudest of the next 64 samples (1.33 ms, the
             * delay it adds) under the ceiling, eased in, released slowly. */
            const float ceil_ = r->ceiling;
            for (int i = 0; i < n; i++) {
                float pk = 0.0f, need, target, env = s_st.lim_env;
                const unsigned w = s_st.look_pos;
                for (int c = 0; c < nch; c++) {
                    const float a = fabsf(feeds[c][i]);
                    if (a > pk) pk = a;
                }
                need = pk > ceil_ ? ceil_ / pk : 1.0f;
                /* The ring still holds the sample leaving now (slot w) and
                 * the 63 after it; the one arriving is the 64th ahead. */
                target = need;
                for (int k = 0; k < LOOKAHEAD; k++)
                    if (s_st.look_gain[k] < target) target = s_st.look_gain[k];
                if (target < env) env += (target - env) * 0.12f;
                else              env += (target - env) * r->lim_release;
                s_st.lim_env = env;
                if (env < s_st.lim_min) s_st.lim_min = env;
                for (int c = 0; c < nch; c++) {
                    const float in = feeds[c][i];
                    float y = s_st.look[c][w] * env;
                    /* What the easing let through stops at the ceiling. */
                    if (y > ceil_) y = ceil_;
                    if (y < -ceil_) y = -ceil_;
                    feeds[c][i] = y;
                    s_st.look[c][w] = in;
                }
                s_st.look_gain[w] = need;
                s_st.look_pos = (w + 1) & (LOOKAHEAD - 1);
            }
        }
    }
    out_meters(feeds, nch, n);
}

void apu_bus_get_meters(apu_bus_meters *m) { *m = s_meters; }

void apu_bus_reset_overs(void)
{
    memset(s_meters.spk_over, 0, sizeof(s_meters.spk_over));
}

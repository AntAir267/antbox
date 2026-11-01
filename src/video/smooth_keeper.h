/*
 * smooth_keeper.h - the arithmetic of the Vulkan Smooth keeper
 * (fb_present_vulkan.c, BO-110/BO-207) that keeps state between frames,
 * pulled out so tests/vblank_schedule can drive it (BO-211). Header-only,
 * pure: no clock, lock or platform call.
 *
 * The refresh period from present_wait's steps (sk_period_feed). The old
 * filter smoothed in every step shorter than 1.5 x the current period. That
 * ratcheted: at 90 Hz (11.1 ms) the 60 Hz steps (16.67) sit right on 1.5 x,
 * and a burst of back-to-back present_wait returns (steps near 0) pulled the
 * period down; once under 11.1 ms the 16.67 steps were never accepted again,
 * and the keeper misread the queue depth (skips) for the rest of the session
 * (BO-210: the period seen at 4.8 ms on the Deck). Now:
 *   - a step under 2 ms is two returns of one refresh, not a refresh: not fed;
 *   - the first step is taken, and confirmed by SK_CONFIRM_N in a row near
 *     it (their mean); before that a shorter step replaces it and a longer
 *     one (a missed refresh) is passed over (BO-210: learning again after
 *     a correction in ~5 frames, and a title-bound stretch's 33 ms steps
 *     never become it);
 *   - a step within 7/8 .. 9/8 of the period is smoothed in (1/16), on both
 *     sides alike (a report 4 ms late is not a refresh 4 ms long);
 *   - any other step is a candidate for a new period; eight in a row that
 *     agree within 5 % become it (the display changed its rate);
 *   - the caller feeds only steps between two new frames at the title's 60,
 *     so a 30 fps crash camera's 33 ms steps never become the period.
 *
 * The depth reading (sk_depth) and the three-in-a-row rule (sk_run) are the
 * gamescope formula. Since BO-210 the keeper only refills: a queue found too
 * deep costs one refresh of latency, never a frame. Its skip was a closed
 * loop on the Deck: a wrong period read every frame as two deep, a skip
 * emptied the queue, the next three readings were wrong again, and every
 * fourth frame went (45 fps); the 33 ms step each skip made broke the run
 * of eight that would have taken the right period. A refill is at most
 * once a second (SK_FIX_MIN_NS), and the caller clears the period with
 * each one (sk_period_clear), so a wrong period is learned again.
 *
 * Smooth only at a refresh near 60 Hz (sk_refresh_ok, every platform since
 * BO-210): at 90 Hz under gamescope a 60 Hz title's frames are 1.5
 * refreshes apart, there is no queue of one to keep, and the keeper's
 * readings mean nothing.
 *
 * Windows' stand-down (sk_down_*): the keeper stands down when a refill does
 * not stay queued twice. It used to stay down until the swapchain was made
 * again; now it clears after 60 s (doubling to 10 min on repeats) and the
 * empty-queue base B is measured again.
 */
#ifndef XBOX_SMOOTH_KEEPER_H
#define XBOX_SMOOTH_KEEPER_H

#include <math.h>
#include <stdint.h>

#define SK_STEP_MIN_NS   2000000ull
#define SK_STEP_MAX_NS   60000000ull
#define SK_RESEED_N      8
#define SK_KEEP_SAME     3
#define SK_CONFIRM_N     4               /* steps that agree before the period is told */
#define SK_FIX_MIN_NS    1000000000ull   /* a refill at most once a second */
#define SK_REFRESH_MIN_NS 14000000ull    /* Smooth only at 14-19 ms a refresh */
#define SK_REFRESH_MAX_NS 19000000ull

typedef struct {
    uint64_t p;          /* the period, ns; 0 not known */
    uint64_t cand;       /* a new period the steps show */
    int      cand_n;     /* steps in a row that showed it */
    unsigned reseeds;    /* times the period was taken anew */
    int      agree;      /* steps in a row on p since it was taken */
} SkPeriod;

/* Learn the period again from the next steps (after a correction). */
static inline void sk_period_clear(SkPeriod *s)
{
    unsigned r = s->reseeds;
    s->p = s->cand = 0;
    s->cand_n = s->agree = 0;
    s->reseeds = r;
}

/* 1 when p is confirmed: SK_CONFIRM_N steps agreed on it, or it was taken
 * from SK_RESEED_N that agreed. Until then a caller keeps the period it
 * had (one stray first step is not a refresh rate). */
static inline int sk_period_sure(const SkPeriod *s)
{
    return s->p && s->agree >= SK_CONFIRM_N;
}

/* Smooth can run at this refresh period (0: not known yet, allowed). */
static inline int sk_refresh_ok(uint64_t p)
{
    return !p || (p >= SK_REFRESH_MIN_NS && p <= SK_REFRESH_MAX_NS);
}

/* One step between two frames shown (both new frames at the title's 60).
 * Returns the period. */
static inline uint64_t sk_period_feed(SkPeriod *s, uint64_t d)
{
    if (d < SK_STEP_MIN_NS || d > SK_STEP_MAX_NS)
        return s->p;
    if (!s->p) {
        s->p = d;
        s->agree = 0;
        return d;
    }
    if (d * 8 >= s->p * 7 && d * 8 <= s->p * 9) {
        /* The mean of the steps at first (the first was one sample), then
         * 1/16. */
        s->p = s->agree < SK_CONFIRM_N
             ? (s->p * (uint64_t)(s->agree + 1) + d) / (uint64_t)(s->agree + 2)
             : (s->p * 15 + d) / 16;
        s->cand_n = 0;
        if (s->agree < 1000000)
            s->agree++;
        return s->p;
    }
    if (s->agree < SK_CONFIRM_N) {
        /* Not confirmed. A shorter step: the first was a missed refresh (or
         * two), start from this one. A longer one is a missed refresh: not
         * counted, unless SK_RESEED_N come in a row (the start was a burst
         * of early returns). */
        if (d < s->p || ++s->cand_n >= SK_RESEED_N) {
            s->p = d;
            s->agree = 0;
            s->cand_n = 0;
        }
        return s->p;
    }
    if (s->cand_n && d * 20 >= s->cand * 19 && d * 20 <= s->cand * 21) {
        s->cand = (s->cand * (uint64_t)s->cand_n + d) / (uint64_t)(s->cand_n + 1);
        s->cand_n++;
    } else {
        s->cand = d;
        s->cand_n = 1;
    }
    if (s->cand_n >= SK_RESEED_N) {
        s->p = s->cand;
        s->cand_n = 0;
        s->reseeds++;
        s->agree = SK_CONFIRM_N;
    }
    return s->p;
}

/* Refreshes a frame waited behind others, from its present-to-display time.
 * The report comes up to ~1 ms after the refresh and the present ~1 ms after
 * one: 1.5 ms of slack keeps 2 refreshes plus that at one frame queued. */
static inline int sk_depth(uint64_t p2d, uint64_t p)
{
    int d = (int)ceil(((double)p2d - 1.5e6) / (double)p) - 1;
    return d < 0 ? 0 : d;
}

typedef struct {
    int run_d, run_n;
    uint64_t last_fix_ns;   /* the last refill asked; kept across a reset */
    unsigned deep;          /* rows that read two or more deep (left alone) */
    unsigned held;          /* refills not asked: under a second since the last */
} SkRun;

static inline void sk_run_reset(SkRun *r)
{
    r->run_d = -1;
    r->run_n = 0;
}

/* One depth reading at now_ns. Returns 1 (refill: present once more) when
 * SK_KEEP_SAME readings in a row found the queue empty, no correction is
 * pending and the last refill is a second old; else 0. Too deep is counted
 * and left: it costs a refresh of latency, a skip cost a frame (BO-210). */
static inline int sk_run(SkRun *r, int d, int pending, uint64_t now_ns)
{
    if (d == r->run_d)
        r->run_n++;
    else {
        r->run_d = d;
        r->run_n = 1;
    }
    if (r->run_n >= SK_KEEP_SAME && d != 1 && !pending) {
        r->run_n = 0;
        if (d > 1) {
            r->deep++;
            return 0;
        }
        if (r->last_fix_ns && now_ns - r->last_fix_ns < SK_FIX_MIN_NS) {
            r->held++;
            return 0;
        }
        r->last_fix_ns = now_ns;
        return 1;
    }
    return 0;
}

/* Windows: the stand-down's clock. */
#define SK_DOWN_FIRST_NS  60000000000ull     /* 60 s */
#define SK_DOWN_MAX_NS    600000000000ull    /* 10 min */
#define SK_DOWN_FORGET_NS 1800000000000ull   /* 30 min without one: back to 60 s */

typedef struct {
    int      down;
    uint64_t since_ns, hold_ns;
    unsigned downs;
} SkDown;

static inline void sk_down_set(SkDown *s, uint64_t now)
{
    if (s->downs && now - s->since_ns > SK_DOWN_FORGET_NS)
        s->downs = 0;
    s->hold_ns = !s->downs ? SK_DOWN_FIRST_NS
               : s->hold_ns * 2 > SK_DOWN_MAX_NS ? SK_DOWN_MAX_NS : s->hold_ns * 2;
    s->downs++;
    s->down = 1;
    s->since_ns = now;
}

/* 1 when a stand-down just ran out (the caller measures B again). */
static inline int sk_down_expired(SkDown *s, uint64_t now)
{
    if (s->down && now - s->since_ns >= s->hold_ns) {
        s->down = 0;
        return 1;
    }
    return 0;
}

#endif /* XBOX_SMOOTH_KEEPER_H */

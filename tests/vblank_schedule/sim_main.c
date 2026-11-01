/*
 * The pacing simulator (BO-211): the real vblank schedule and display lock
 * (kernel_pacing.c), the real watchdog (pacing_watch.h), the Vulkan keeper's
 * arithmetic (smooth_keeper.h) and the Metal pacer (present_cadence.c),
 * between a simulated title, timer thread, display and compositor, on a
 * virtual clock: no sleeps, minutes of play in milliseconds.
 *
 * Actors:
 *   timer      raises vblank n when xbox_VblankSchedule says so, 0-0.2 ms
 *              late (1 in 100 at 2 ms); can stall.
 *   executor   per frame: work (busy, from a normal distribution) and the
 *              wait for the title (idle); then the XDK swap rule (a swap
 *              whose vblank has passed goes to the next one), the hold to
 *              that vblank's raise, the present.
 *   display    a fixed refresh grid (with a mode switch) and a FIFO queue
 *              that shows one frame per refresh; or variable refresh.
 *   reports    present_wait (the refreshes a frame was shown at, 0-1.2 ms
 *              late, 1 in 200 at 5 ms), a display link (every refresh with
 *              its period, optionally irregular), or none.
 *   keeper     Smooth: depth from present-to-display, three in a row, a
 *              refill or a skip; the refresh period from the steps.
 *   host       pauses, loads, a suspend, 30 Hz crash-camera stretches.
 *
 * Each named scenario checks: recovery after every disturbance within 5 s
 * (every 1 s window from then on healthy: at most 5 % late flips at the
 * title's 60, at most 4 % repeated refreshes), zero watchdog resets where
 * the state is legitimate, and its own numbers.
 */
#include "kernel_pacing.h"
#include "platform/recomp_env.h"
#include "pacing_watch.h"
#include "present_cadence.h"
#include "video/smooth_keeper.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define MS   1000000ull
#define US   1000ull
#define SEC  1000000000ull
#define P60  16666667ull
#define WIN_NS (1 * SEC)
#define MAXWIN 800
#define NSPAN 6

static int failures;
static int verbose;

static void check(int ok, const char *what)
{
    printf("  %-74s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

typedef struct { double at, dur; } span;      /* seconds from the start */
enum { R_NONE, R_PW, R_LINK };
enum { INJ_NONE, INJ_CLEARABLE, INJ_STICKY };

typedef struct {
    const char *name;
    double secs;
    double hz, hz2, sw_s;          /* display, and a mode switch at sw_s */
    int    vrr;
    int    report;
    double jitter_ms;              /* R_LINK: stamps up to this either side */
    int    smooth;                 /* the keeper */
    double busy, sd, idle;         /* executor work, ms */
    span   iv2[NSPAN], loads[NSPAN], pauses[NSPAN], stalls[NSPAN], bursts[NSPAN];
    span   suspend;
    double inject_s;               /* every other vblank 9 ms late from here */
    int    inject;
    int    exec_bound;             /* the cliff: no recovery to check */
    double wake_ms;                /* the hold lets go 0..wake_ms after the raise (else 50 us) */
    int    wake_arg;               /* what the watchdog is told of it: 0 the real one,
                                    * 1 nothing (0: the test before the BO-211 review),
                                    * 2 not known (-1) */
    int    old_keeper;             /* the keeper before BO-210: skips, no refill limit,
                                    * no 14-19 ms guard off Windows, no relearn */
    double seed_kp_ms;             /* at sw_s the keeper's period is found this wrong */
    int    wsi;                    /* the window system says the refresh (gamescope) */
} scen;

typedef struct { uint64_t pt, vbl; uint32_t flip; int meas, rep; } qframe;
typedef struct { uint64_t t, period; qframe f; int shown; } report;

typedef struct {
    int flips1, late1, refr, rep;
    int shown;                     /* new frames on the glass (a refill's repeat not) */
    int nlat;
    float lat[260];
} hwin;

typedef struct {
    int resets, forced, lines, exec_lines, fault_lines, futile;
    double recover_max;            /* s; -1 none checked, 1e9 never */
    double fps;
    uint64_t kp_min, kp_max;       /* keeper period while used, after warm-up */
    uint64_t kp_final, old_kp_final;
    int refills, skips, guarded;
    double shown_last;             /* new frames shown a second, the last 20 s */
    double shown_after;            /* the same, 5-25 s after the mode switch */
    double lat_first, lat_last;    /* ms, medians */
    unsigned watch_windows, watch_voids;
} result;

static struct {
    const scen *c;
    uint64_t t0, end;
    unsigned rng;
    /* timer */
    XboxVblankSchedule sch;
    uint64_t next_raise, last_edge;
    uint32_t vc;
    int inject_on;
    /* display */
    double rp;
    uint64_t grid0;                /* the grid's origin */
    uint64_t gk;                   /* refreshes since it */
    uint64_t next_ref, vrr_last;
    int switched;
    qframe q[16];
    int qn;
    /* reports */
    report rep[64];
    int nrep;
    /* keeper */
    SkPeriod per;
    SkRun run;
    uint64_t pub;                  /* the period told (s_vbl_period_ns) */
    int seeded;
    int want;
    uint32_t from_flip;
    uint64_t prev_rt, old_p;
    int prev_meas;
    /* executor */
    int ex;                        /* 0 working, 1 holding, 2 releasing */
    uint64_t f_start, f_arrive, f_release, busy, idle;
    uint32_t target, last_target, iv, flips;
    int late;
    int pending_force, paused_frame;
    uint64_t next_due, edge_due, rel_edge;
    int64_t prev_wake;
    XboxPacingWatch w;
    hwin win[MAXWIN];
    result r;
} S;

/* The keeper's row rule before BO-210, for the before-and-after test:
 * three in a row two deep skipped a present. */
static int sk_run_old(SkRun *r, int d, int pending)
{
    if (d == r->run_d)
        r->run_n++;
    else {
        r->run_d = d;
        r->run_n = 1;
    }
    if (r->run_n >= SK_KEEP_SAME && d != 1 && !pending) {
        r->run_n = 0;
        return d < 1 ? 1 : -1;
    }
    return 0;
}

static unsigned rnd(void)
{
    S.rng = S.rng * 1103515245u + 12345u;
    return S.rng >> 8;
}

static double urand(void)
{
    return (rnd() & 0xFFFFFF) / 16777216.0;
}

static double gauss(double m, double sd)
{
    double u1 = urand() + 1e-9, u2 = urand();
    return m + sd * sqrt(-2 * log(u1)) * cos(6.283185307 * u2);
}

static uint64_t at_ns(double s)
{
    return S.t0 + (uint64_t)(s * 1e9);
}

static int in_spans(const span *sp, uint64_t t, uint64_t *end)
{
    int i;
    for (i = 0; i < NSPAN; i++) {
        if (sp[i].dur <= 0)
            continue;
        if (t >= at_ns(sp[i].at) && t < at_ns(sp[i].at + sp[i].dur)) {
            if (end)
                *end = at_ns(sp[i].at + sp[i].dur);
            return 1;
        }
    }
    return 0;
}

static hwin *win_at(uint64_t t)
{
    uint64_t i = (t - S.t0) / WIN_NS;
    return i < MAXWIN ? &S.win[i] : NULL;
}

/* The watchdog's reset, as the kernel performs it: the display lock and the
 * keeper's hook; the injected fault stands for a sticky state a reset
 * clears. */
static void sim_reset(void)
{
    xbox_VblankDisplayRelearn();
    memset(&S.per, 0, sizeof S.per);
    sk_run_reset(&S.run);
    S.want = 0;
    S.from_flip = S.flips;
    if (S.c->inject == INJ_CLEARABLE)
        S.inject_on = 0;
}

static void raise_late(uint64_t due)
{
    S.next_due = due;
    uint64_t l = rnd() % 100 == 0 ? 2 * MS : (uint64_t)(urand() * 200000.0);
    /* The injected sticky state moves every other edge itself 9 ms (a grid
     * snapped to the wrong place), not only its raise: a late raise is the
     * timer's lateness, which the diagnosis counts with the hold's wake. */
    if (S.inject_on && ((S.vc + 1) & 1) == 0)
        S.next_due = due += 9 * MS;
    S.next_raise = due + l;
}

static void start_frame(uint64_t t)
{
    uint64_t e;
    double b;
    int guard = 0;
    S.paused_frame = 0;
    while (guard++ < 8 && (in_spans(S.c->loads, t, &e) || in_spans(S.c->pauses, t, &e))) {
        S.paused_frame |= in_spans(S.c->pauses, t, NULL);
        t = e;
    }
    S.iv = in_spans(S.c->iv2, t, NULL) ? 2 : 1;
    b = gauss(S.c->busy, S.c->sd);
    if (b < 1)
        b = 1;
    S.busy = (uint64_t)(b * 1e6);
    S.idle = (uint64_t)(S.c->idle * 1e6);
    S.f_start = t;
    S.f_arrive = t + S.busy + S.idle;
    S.ex = 0;
}

static void push_report(uint64_t t, uint64_t period, const qframe *f, int shown)
{
    int i = S.nrep;
    if (S.nrep >= 64)
        return;
    while (i > 0 && S.rep[i - 1].t > t) {
        S.rep[i] = S.rep[i - 1];
        i--;
    }
    S.rep[i].t = t;
    S.rep[i].period = period;
    if (f)
        S.rep[i].f = *f;
    S.rep[i].shown = shown;
    S.nrep++;
}

static int active(uint64_t t)
{
    return !in_spans(S.c->loads, t, NULL) && !in_spans(S.c->pauses, t, NULL)
           && !in_spans(S.c->iv2, t, NULL) && !in_spans(S.c->stalls, t, NULL)
           && !(S.c->suspend.dur > 0 && t >= at_ns(S.c->suspend.at)
                && t < at_ns(S.c->suspend.at + S.c->suspend.dur) + SEC);
}

static void ev_timer(uint64_t now)
{
    uint64_t next, late, e;
    if (in_spans(S.c->stalls, now, &e)) {
        S.next_raise = e;
        return;
    }
    if (S.c->inject && !S.inject_on && S.c->inject_s > 0 && now >= at_ns(S.c->inject_s)
            && now < at_ns(S.c->inject_s) + 20 * MS)
        S.inject_on = 1;
    if (xbox_VblankSchedule(&S.sch, now, &next, &late)) {
        S.vc++;
        S.last_edge = now;
        S.edge_due = S.next_due ? S.next_due : now;
        if (S.ex == 1 && (int32_t)(S.vc - S.target) >= 0) {
            S.f_release = now + (S.c->wake_ms > 0 ? (uint64_t)(urand() * S.c->wake_ms * 1e6)
                                                  : 50 * US);
            S.rel_edge = S.edge_due;
            S.ex = 2;
        }
        if (next > now)
            raise_late(next);
        else
            S.next_raise = now + MS;           /* catching up: one a millisecond */
    } else {
        raise_late(next);
    }
}

static int display_60(void)
{
    return fabs(S.rp - 1e9 / 60.0) < 0.02 * 1e9 / 60.0;
}

static void shown_frame(uint64_t glass, const qframe *f)
{
    hwin *w = win_at(glass);
    if (w && !f->rep && w->nlat < 260 && f->vbl && glass > f->vbl)
        w->lat[w->nlat++] = (float)((glass - f->vbl) / 1e6);
}

static void ev_refresh(uint64_t r)
{
    hwin *w = win_at(r);
    int latched = 0;
    qframe f;

    if (!S.switched && S.c->sw_s > 0 && r >= at_ns(S.c->sw_s)) {
        S.switched = 1;
        S.rp = 1e9 / S.c->hz2;
        S.grid0 = r;
        S.gk = 0;
    }
    if (S.qn && S.q[0].pt + 300 * US <= r) {
        int i;
        f = S.q[0];
        for (i = 1; i < S.qn; i++)
            S.q[i - 1] = S.q[i];
        S.qn--;
        latched = 1;
        shown_frame(r, &f);
        if (w && !f.rep)
            w->shown++;
    }
    if (w && display_60() && active(r) && S.flips > 10) {
        w->refr++;
        w->rep += !latched;
    }
    if (S.c->report == R_PW && latched) {
        uint64_t rt = r + (rnd() % 200 == 0 ? 5 * MS : (uint64_t)(urand() * 1.2e6));
        uint64_t e;
        /* A stalled waiter: its returns come back to back afterwards. */
        if (in_spans(S.c->bursts, r, &e) && (r - S.t0) % SEC < 40 * MS) {
            uint64_t st = r - (r - S.t0) % SEC + 40 * MS;
            rt = st + (uint64_t)S.nrep * 30 * US;
        }
        push_report(rt, 0, &f, 1);
    } else if (S.c->report == R_LINK) {
        int64_t j = 0;
        if (S.c->jitter_ms > 0)
            j = (int64_t)((urand() * 2 - 1) * S.c->jitter_ms * 1e6);
        push_report((uint64_t)((int64_t)r + j), (uint64_t)S.rp, NULL, 0);
    }
    S.gk++;
    S.next_ref = S.grid0 + (uint64_t)((double)S.gk * S.rp);
}

static void ev_report(void)
{
    report rp = S.rep[0];
    int i;
    for (i = 1; i < S.nrep; i++)
        S.rep[i - 1] = S.rep[i];
    S.nrep--;
    xbox_VblankDisplayRefresh(rp.t, rp.period);
    if (S.c->report != R_PW || !rp.shown)
        return;
    {
        const qframe *f = &rp.f;
        int meas = f->meas && !f->rep;
        if (S.prev_rt && rp.t > S.prev_rt) {
            uint64_t d = rp.t - S.prev_rt;
            /* The old filter, for the record: every step under 60 ms that is
             * shorter than 1.5 x the period. */
            if (d < 60 * MS) {
                if (!S.old_p)
                    S.old_p = d;
                else if (d < S.old_p * 3 / 2)
                    S.old_p = (S.old_p * 15 + d) / 16;
            }
            if (S.prev_meas && meas) {
                sk_period_feed(&S.per, d);
                /* What the waiter tells (s_vbl_period_ns): before BO-210
                 * every step's result, now only a confirmed one. */
                if (S.c->old_keeper || sk_period_sure(&S.per))
                    S.pub = S.per.p;
            }
        }
        if (S.c->seed_kp_ms > 0 && !S.seeded && rp.t >= at_ns(S.c->sw_s)) {
            S.seeded = 1;
            S.per.p = S.pub = (uint64_t)(S.c->seed_kp_ms * 1e6);
            S.per.agree = SK_CONFIRM_N;
            S.per.cand_n = 0;
        }
        S.prev_rt = rp.t;
        S.prev_meas = meas;
        if (!S.c->smooth || f->rep || f->flip <= S.from_flip)
            return;
        {
            /* The keeper's period: the window system's refresh where it
             * says it, else the one told. Off near 60 Hz Smooth is Low
             * latency (BO-210; before, only on Windows). */
            uint64_t kp = S.c->wsi ? (uint64_t)S.rp : S.pub;
            if (!S.c->old_keeper && !sk_refresh_ok(kp)) {
                S.run.run_n = 0;
                S.r.guarded++;
                return;
            }
            if (!meas || kp < 5 * MS || kp > 50 * MS) {
                S.run.run_n = 0;
                return;
            }
            if (rp.t > S.t0 + 10 * SEC && rp.t > at_ns(S.c->sw_s + 5)) {
                if (!S.r.kp_min || kp < S.r.kp_min) S.r.kp_min = kp;
                if (kp > S.r.kp_max) S.r.kp_max = kp;
            }
            {
                int d = sk_depth(rp.t - f->pt, kp);
                int want = S.c->old_keeper ? sk_run_old(&S.run, d, S.want != 0)
                                           : sk_run(&S.run, d, S.want != 0, rp.t);
                if (want)
                    S.want = want;
            }
        }
    }
}

static void enqueue(const qframe *f)
{
    if (S.c->vrr) {
        uint64_t g = f->pt > S.vrr_last + 4166667u ? f->pt : S.vrr_last + 4166667u;
        S.vrr_last = g;
        shown_frame(g, f);
        return;
    }
    if (S.qn >= 16) {
        int i;
        for (i = 1; i < S.qn; i++)
            S.q[i - 1] = S.q[i];
        S.qn--;
    }
    S.q[S.qn++] = *f;
}

static void ev_executor(uint64_t now)
{
    char line[400];
    if (S.ex == 0) {
        /* The swap, at the frame's end: the XDK rule. */
        uint32_t count = S.vc, target = S.last_target + S.iv;
        S.late = (int32_t)(target - count) <= 0;
        if (S.late)
            target = count + 1;
        S.last_target = S.target = target;
        S.ex = 1;
        return;
    }
    /* S.ex == 2: released at the vblank: the flip. */
    {
        hwin *w = win_at(now);
        int k;
        qframe f;
        S.flips++;
        if (w && S.iv == 1 && active(now)) {
            w->flips1++;
            w->late1 += S.late;
        }
        if (S.pending_force) {
            S.pending_force = 0;
            pwatch_force(&S.w, now);
            sim_reset();
            S.r.forced++;
            if (verbose)
                printf("    [PACING] after a gap: the frame-queue keeper and the display lock start over\n");
        }
        if (xbox_VblankDisplayRateChanged())
            pwatch_void(&S.w, PWATCH_V_RATE);
        if (S.paused_frame)
            pwatch_void(&S.w, PWATCH_V_PAUSED);
        {
            /* This frame began at the previous hold's release. */
            int64_t wake = S.c->wake_arg == 1 ? 0 : S.c->wake_arg == 2 ? -1 : S.prev_wake;
            S.prev_wake = now >= S.rel_edge ? (int64_t)(now - S.rel_edge) : 0;
            k = pwatch_flip(&S.w, now, S.iv, S.late, S.busy, S.idle,
                            now > S.f_arrive ? now - S.f_arrive : 0, wake, 0, 0, line, sizeof line);
        }
        if (k & PWATCH_LINE) {
            S.r.lines++;
            S.r.exec_lines += S.w.d.state == PDIAG_EXEC;
            S.r.fault_lines += S.w.d.state == PDIAG_FAULT;
            if (verbose)
                printf("    %6.1f s [PACING] %s\n", (now - S.t0) / 1e9, line);
        }
        if (k & PWATCH_RESET) {
            S.r.resets++;
            S.r.futile += strstr(S.w.why, "did not help") != NULL;
            sim_reset();
            if (verbose)
                printf("    %6.1f s [PACING] %s\n", (now - S.t0) / 1e9, S.w.why);
        }
        /* The present. */
        f.pt = now + 300 * US;
        f.vbl = S.last_edge;
        f.flip = S.flips;
        f.meas = S.iv == 1;
        f.rep = 0;
        if (S.c->smooth && S.want < 0) {
            /* Only the old keeper skips. */
            S.want = 0;
            S.r.skips++;
            sk_run_reset(&S.run);
            S.from_flip = S.flips;
        } else {
            enqueue(&f);
            if (S.c->smooth && S.want > 0 && f.meas) {
                f.rep = 1;
                enqueue(&f);
                S.want = 0;
                S.r.refills++;
                if (!S.c->old_keeper)
                    sk_period_clear(&S.per);   /* learned again (BO-210) */
                if (verbose > 1)
                    printf("    %6.2f s refill (keeper p %.3f, queue %d)\n", (now - S.t0) / 1e9,
                           S.per.p / 1e6, S.qn);
                sk_run_reset(&S.run);
                S.from_flip = S.flips;
            }
        }
        start_frame(now);
    }
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static double win_lat(hwin *w)
{
    if (!w->nlat)
        return 0;
    qsort(w->lat, (size_t)w->nlat, sizeof w->lat[0], cmpf);
    return w->lat[w->nlat / 2];
}

static int win_ok(const hwin *w)
{
    if (w->flips1 < 30)
        return 1;                          /* not judged: under half a second at 60 */
    if (w->late1 * 20 > w->flips1)
        return 0;
    if (w->refr && w->rep * 25 > w->refr)
        return 0;
    return 1;
}

/* Disturbances: [start, end) in ns. */
typedef struct { uint64_t a, b; } dist;

static int add_spans(dist *d, int n, const span *sp)
{
    int i;
    for (i = 0; i < NSPAN; i++)
        if (sp[i].dur > 0) {
            d[n].a = at_ns(sp[i].at);
            d[n].b = at_ns(sp[i].at + sp[i].dur);
            n++;
        }
    return n;
}

static int dcmp(const void *x, const void *y)
{
    const dist *a = x, *b = y;
    return a->a < b->a ? -1 : a->a > b->a;
}

static double recovery(void)
{
    dist d[64];
    int n = 0, i, nwin = (int)((S.end - S.t0) / WIN_NS);
    double worst = -1;
    const scen *c = S.c;

    n = add_spans(d, n, c->iv2);
    n = add_spans(d, n, c->loads);
    n = add_spans(d, n, c->pauses);
    n = add_spans(d, n, c->stalls);
    n = add_spans(d, n, c->bursts);
    if (c->suspend.dur > 0) {
        d[n].a = at_ns(c->suspend.at);
        d[n].b = at_ns(c->suspend.at + c->suspend.dur);
        n++;
    }
    if (c->sw_s > 0) {
        d[n].a = d[n].b = at_ns(c->sw_s);
        n++;
    }
    if (c->inject) {
        d[n].a = d[n].b = at_ns(c->inject_s);
        n++;
    }
    /* The start is a disturbance too: the lock and keeper learning. */
    d[n].a = d[n].b = S.t0;
    n++;
    qsort(d, (size_t)n, sizeof d[0], dcmp);
    for (i = 0; i < n; i++) {
        uint64_t e = d[i].b, stop = i + 1 < n ? d[i + 1].a : S.end;
        int w0, wi, ok_from = -1;
        if (c->inject == INJ_STICKY && d[i].a == at_ns(c->inject_s))
            continue;                      /* it never ends */
        w0 = (int)((e - S.t0 + WIN_NS - 1) / WIN_NS);
        for (wi = w0; wi < nwin && S.t0 + (uint64_t)(wi + 1) * WIN_NS <= stop; wi++) {
            if (!win_ok(&S.win[wi]))
                ok_from = -1;
            else if (ok_from < 0)
                ok_from = wi;
        }
        if (wi == w0)
            continue;                      /* no whole window before the next one */
        {
            double rec = ok_from < 0 ? 1e9
                       : ((double)(S.t0 + (uint64_t)ok_from * WIN_NS) - (double)e) / 1e9;
            if (rec < 0)
                rec = 0;
            if (verbose || rec > 5)
                printf("    disturbance %.1f-%.1f s: healthy from %.1f s after\n",
                       (d[i].a - S.t0) / 1e9, (e - S.t0) / 1e9, rec);
            if (rec > worst)
                worst = rec;
        }
    }
    return worst;
}

static void run(const scen *c, unsigned seed, result *out)
{
    static uint64_t base = 100000 * SEC;
    uint64_t now;
    int i;

    memset(&S, 0, sizeof S);
    S.c = c;
    S.rng = seed;
    S.t0 = (base += 20000 * SEC);
    S.end = S.t0 + (uint64_t)(c->secs * 1e9);
    S.rp = 1e9 / c->hz;
    S.grid0 = S.t0 + 3 * MS;
    S.next_ref = S.grid0;
    S.next_raise = S.t0;
    S.r.recover_max = -1;
    sk_run_reset(&S.run);
    pwatch_init(&S.w);
    xbox_VblankDisplayRelearn();
    xbox_VblankDisplayRateChanged();
    start_frame(S.t0 + 1 * MS);

    for (;;) {
        uint64_t tA = S.next_raise, tB = c->vrr ? ~0ull : S.next_ref;
        uint64_t tC = S.nrep ? S.rep[0].t : ~0ull;
        uint64_t tD = S.ex == 0 ? S.f_arrive : S.ex == 2 ? S.f_release : ~0ull;
        now = tA;
        if (tB < now) now = tB;
        if (tC < now) now = tC;
        if (tD < now) now = tD;
        if (now >= S.end)
            break;
        /* A suspend: nothing runs; the display's grid goes on. */
        if (c->suspend.dur > 0 && now >= at_ns(c->suspend.at)
                && now < at_ns(c->suspend.at + c->suspend.dur)) {
            uint64_t e = at_ns(c->suspend.at + c->suspend.dur), sh = e - now;
            if (S.next_raise < e) S.next_raise = e;
            while (S.next_ref < e) {
                S.gk++;
                S.next_ref = S.grid0 + (uint64_t)((double)S.gk * S.rp);
            }
            S.nrep = 0;
            S.qn = 0;
            if (S.ex == 0) S.f_arrive += sh;
            if (S.ex == 2) S.f_release += sh;
            S.f_start += sh;
            S.pending_force = 1;          /* the wake listener */
            continue;
        }
        if (now == tD)
            ev_executor(now);
        else if (now == tA)
            ev_timer(now);
        else if (now == tC)
            ev_report();
        else
            ev_refresh(now);
    }
    S.r.fps = S.flips / c->secs;
    S.r.kp_final = S.c->wsi ? (uint64_t)S.rp : S.pub;
    S.r.old_kp_final = S.old_p;
    S.r.watch_windows = S.w.windows;
    S.r.watch_voids = S.w.voids;
    {
        /* Latency: the first clean minute after warm-up, and the last 30 s. */
        double a[70], b[40];
        int na = 0, nb = 0, nwin = (int)((S.end - S.t0) / WIN_NS);
        for (i = 10; i < nwin && na < 60; i++)
            if (S.win[i].nlat > 30 && win_ok(&S.win[i]))
                a[na++] = win_lat(&S.win[i]);
        for (i = nwin - 30; i < nwin; i++)
            if (i >= 0 && S.win[i].nlat > 30)
                b[nb++] = win_lat(&S.win[i]);
        S.r.lat_first = 0;
        for (i = 0; i < na; i++) S.r.lat_first += a[i] / na;
        S.r.lat_last = 0;
        for (i = 0; i < nb; i++) S.r.lat_last += b[i] / nb;
        S.r.shown_last = 0;
        for (i = nwin - 20; i < nwin; i++)
            if (i >= 0)
                S.r.shown_last += S.win[i].shown / 20.0;
        S.r.shown_after = 0;
        for (i = (int)c->sw_s + 5; c->sw_s > 0 && i < (int)c->sw_s + 25 && i < nwin; i++)
            S.r.shown_after += S.win[i].shown / 20.0;
    }
    if (!c->exec_bound)
        S.r.recover_max = recovery();
    *out = S.r;
}

/* ── the scenarios ─────────────────────────────────────────── */

static void report_line(const scen *c, const result *r)
{
    printf("  %s: %.1f fps, resets %d (forced %d), lines %d, recovery %.1f s, keeper p %.3f ms"
           " (old filter %.3f), refills %d skips %d, shown %.1f/s at the end, latency %.1f -> %.1f ms, windows %u (void %u)\n",
           c->name, r->fps, r->resets, r->forced, r->lines, r->recover_max, r->kp_final / 1e6,
           r->old_kp_final / 1e6, r->refills, r->skips, r->shown_last, r->lat_first, r->lat_last,
           r->watch_windows, r->watch_voids);
}

static const scen base60 = {
    .name = "", .secs = 120, .hz = 60, .report = R_PW, .smooth = 0,
    .busy = 8, .sd = 1.5, .idle = 0.5,
};

static void scenario(scen c, const char *name, unsigned seed, result *r)
{
    c.name = name;
    run(&c, seed, r);
    report_line(&c, r);
}

static void all(void)
{
    result r;
    char what[160];
    scen c;

    printf("pacing simulator (BO-211):\n");

    /* 1. The Steam Deck: 90 Hz at the start, 60 once the window is up;
     * Smooth (gamescope). */
    c = base60;
    c.hz = 90; c.hz2 = 60; c.sw_s = 3; c.smooth = 1;
    scenario(c, "deck_90_to_60", 11, &r);
    snprintf(what, sizeof what, "1 deck 90 -> 60 Hz, Smooth: recovers in %.1f s, %d resets, keeper %.2f ms",
             r.recover_max, r.resets, r.kp_final / 1e6);
    check(r.recover_max >= 0 && r.recover_max <= 5 && r.resets == 0
          && fabs(r.kp_final / 1e6 - 16.667) < 0.33, what);

    /* 2. BO-191: a variable-refresh 240 Hz display (DXGI: nothing reported,
     * the lock stands down). */
    c = base60;
    c.hz = 240; c.vrr = 1; c.report = R_NONE;
    scenario(c, "vrr_240", 12, &r);
    snprintf(what, sizeof what, "2 VRR 240 Hz, no reports: %.1f fps, %d resets, recovery %.1f s",
             r.fps, r.resets, r.recover_max);
    check(r.resets == 0 && r.fps > 59.5 && r.recover_max <= 5, what);

    /* 3. BO-77: an external 120 Hz display whose link reports +-4 ms off. */
    c = base60;
    c.hz = 120; c.report = R_LINK; c.jitter_ms = 4;
    scenario(c, "link_120_irregular", 13, &r);
    snprintf(what, sizeof what, "3 link 120 Hz, irregular stamps: %.1f fps, %d resets", r.fps,
             r.resets);
    check(r.resets == 0 && r.fps > 59.5 && r.recover_max <= 5, what);

    /* 4. BO-210: the cliff, an executor at 16.1 +- 0.8 ms: executor-bound,
     * said, never reset. */
    c = base60;
    c.busy = 16.1; c.sd = 0.8; c.idle = 0.5; c.exec_bound = 1; c.smooth = 1;
    scenario(c, "bo210_cliff", 14, &r);
    snprintf(what, sizeof what, "4 cliff 16.1 ms: %.1f fps, executor-bound said %d times, %d resets",
             r.fps, r.exec_lines, r.resets);
    check(r.fps < 50 && r.exec_lines >= 1 && r.exec_lines <= 4 && r.resets == 0, what);

    /* 5. Crash cameras: 30 Hz stretches of 3-16 s under Smooth. */
    c = base60;
    c.smooth = 1; c.secs = 180;
    c.iv2[0] = (span){ 20, 3 };
    c.iv2[1] = (span){ 40, 16 };
    c.iv2[2] = (span){ 80, 8 };
    c.iv2[3] = (span){ 110, 5 };
    c.iv2[4] = (span){ 140, 12 };
    scenario(c, "crash_cams", 15, &r);
    snprintf(what, sizeof what, "5 crash cams 3-16 s: %d resets, keeper %.2f-%.2f ms, %d refills,"
             " recovery %.1f s", r.resets, r.kp_min / 1e6, r.kp_max / 1e6, r.refills, r.recover_max);
    check(r.resets == 0 && r.kp_min > 16.33 * MS && r.kp_max < 17.0 * MS && r.refills <= 12
          && r.recover_max <= 5, what);

    /* 6. BO-210's 4.8 ms keeper period: 90 -> 60 and a waiter that stalls
     * and then returns back to back. */
    c = base60;
    c.hz = 90; c.hz2 = 60; c.sw_s = 20; c.smooth = 1; c.secs = 120;
    c.bursts[0] = (span){ 2, 38 };
    scenario(c, "keeper_period_bursts", 16, &r);
    snprintf(what, sizeof what, "6 waiter bursts: keeper %.2f ms (old filter %.2f), %d skips,"
             " recovery %.1f s", r.kp_final / 1e6, r.old_kp_final / 1e6, r.skips, r.recover_max);
    check(fabs(r.kp_final / 1e6 - 16.667) < 0.33 && r.kp_min > 16.33 * MS
          && r.old_kp_final < 12 * MS && r.resets == 0
          && r.recover_max <= 5, what);

    /* 7. Suspend and resume: 30 s with nothing running. */
    c = base60;
    c.smooth = 1;
    c.suspend = (span){ 50, 30 };
    scenario(c, "suspend_resume", 17, &r);
    snprintf(what, sizeof what, "7 suspend 30 s: %d forced reset, %d watchdog resets, recovery %.1f s",
             r.forced, r.resets, r.recover_max);
    check(r.forced == 1 && r.resets == 0 && r.recover_max <= 5, what);

    /* 14. BO-210 live on the Deck (2026-10-18): gamescope ran the panel at
     * 90 Hz under the game with Smooth on, then 60 again, and the game sat
     * at 45 fps until Frame pacing went to Low latency. The keeper's period
     * is found wrong when the panel comes back (5.55 ms here, a third of a
     * refresh; BO-210's F1 run showed 4.84 on the Deck): every frame reads
     * too deep, a skip, three readings, a skip. The old keeper: 45 shown a second for good. Now: no skip, the
     * 14-19 ms guard holds the keeper off until the period is learned again;
     * and with the window system's refresh (gamescope) never misread. */
    {
        double old_shown, old_rec;
        int old_skips;
        c = base60;
        c.hz = 90; c.hz2 = 60; c.sw_s = 60; c.smooth = 1; c.secs = 150;
        c.seed_kp_ms = 5.55;
        c.old_keeper = 1;
        scenario(c, "deck_90_then_60_old_keeper", 23, &r);
        old_shown = r.shown_after;
        old_skips = r.skips;
        old_rec = r.recover_max;
        c.old_keeper = 0;
        scenario(c, "deck_90_then_60", 23, &r);
        snprintf(what, sizeof what, "14 90 Hz then 60, keeper period wrong: %.1f shown/s 5-25 s on"
                 " (old keeper %.1f, %d skips, %.0f s to recover), %d refills", r.shown_after,
                 old_shown, old_skips, old_rec, r.refills);
        check(old_shown < 47 && old_shown > 43 && old_rec > 20 && r.shown_after > 59.0
              && r.shown_last > 59.0 && r.skips == 0 && r.refills <= 150 && r.recover_max >= 0
              && r.recover_max <= 5, what);
        c.wsi = 1;
        scenario(c, "deck_90_then_60_wsi", 23, &r);
        snprintf(what, sizeof what, "14b the same with gamescope's refresh cycle: %.1f shown/s,"
                 " %d refills, recovery %.1f s", r.shown_after, r.refills, r.recover_max);
        check(r.shown_after > 59.0 && r.shown_last > 59.0 && r.skips == 0 && r.recover_max >= 0 && r.recover_max <= 5,
              what);
    }

    /* 8 and 9 are below (the Windows stand-down, the Metal pacer). */

    /* 13. BO-211 review, F1: the hold lets go 0-2 ms after the edge (Auto
     * level 0 sleeping on the condition variable, Windows' timers, a loaded
     * Deck) and the title needs 15.0 +- 0.5 ms: its late frames had their
     * own time but not the hold's lateness. Counted without the wake (the
     * test before the review) they read as a pacing fault and were reset;
     * with it, and with the wake not known (85 % budget), title-bound and
     * never reset. */
    {
        int old_resets;
        c = base60;
        c.busy = 15.0; c.sd = 0.5; c.idle = 0.3; c.wake_ms = 2; c.exec_bound = 1;
        c.wake_arg = 1;
        scenario(c, "wake_late_old_budget", 22, &r);
        old_resets = r.resets;
        c.wake_arg = 0;
        scenario(c, "wake_late", 22, &r);
        snprintf(what, sizeof what, "13 hold wakes 0-2 ms late, 15 ms title: %.1f fps, %d resets"
                 " (%d before the fix)", r.fps, r.resets, old_resets);
        check(r.resets == 0 && r.fault_lines == 0 && old_resets >= 1, what);
        c.wake_arg = 2;
        scenario(c, "wake_unknown", 22, &r);
        snprintf(what, sizeof what, "13b the same, the wake not known (85 %% budget): %d resets",
                 r.resets);
        check(r.resets == 0 && r.fault_lines == 0, what);
    }

    /* 10. The timer thread stalls 300 ms, three times. */
    c = base60;
    c.stalls[0] = (span){ 30, 0.3 };
    c.stalls[1] = (span){ 31.3, 0.3 };
    c.stalls[2] = (span){ 32.6, 0.3 };
    scenario(c, "timer_stalls", 18, &r);
    snprintf(what, sizeof what, "10 timer stalls 300 ms x3: %d resets, recovery %.1f s", r.resets,
             r.recover_max);
    check(r.resets == 0 && r.recover_max <= 5, what);

    /* 11. Pauses and loads (overlay open 1-60 s, loads 1-5 s), Smooth. */
    c = base60;
    c.smooth = 1; c.secs = 240;
    c.pauses[0] = (span){ 20, 1 };
    c.pauses[1] = (span){ 40, 60 };
    c.loads[0] = (span){ 120, 5 };
    c.loads[1] = (span){ 150, 1 };
    c.loads[2] = (span){ 180, 3 };
    scenario(c, "pauses_loads", 19, &r);
    snprintf(what, sizeof what, "11 pauses 1-60 s, loads 1-5 s: %d resets, recovery %.1f s", r.resets,
             r.recover_max);
    check(r.resets == 0 && r.recover_max <= 5, what);

    /* 12. A sticky pacing state (every other vblank edge 9 ms late, cleared by a
     * reset): the watchdog resets once and it is gone. Then the same that
     * no reset clears: the resets back off, and say so. */
    c = base60;
    c.smooth = 1; c.inject = INJ_CLEARABLE; c.inject_s = 30.5;
    scenario(c, "sticky_fault", 20, &r);
    snprintf(what, sizeof what, "12a sticky fault, cleared by a reset: %d reset, recovery %.1f s",
             r.resets, r.recover_max);
    check(r.resets == 1 && r.recover_max <= 5, what);
    c = base60;
    c.secs = 240; c.inject = INJ_STICKY; c.inject_s = 30.5;
    scenario(c, "unclearable_fault", 21, &r);
    snprintf(what, sizeof what, "12b a fault no reset clears, 210 s: %d resets (backing off), %d"
             " 'did not help' line", r.resets, r.futile);
    check(r.resets >= 3 && r.resets <= 6 && r.futile == 1, what);
}

/* 8. Windows: the keeper's stand-down ends after 60 s, then 120, 240 ...
 * up to 10 minutes; half an hour without one starts it over at 60. */
static void windows_stand_down(void)
{
    SkDown d;
    uint64_t t = 1000 * SEC, cleared[6];
    int i, ok = 1;
    char what[160];

    memset(&d, 0, sizeof d);
    for (i = 0; i < 6; i++) {
        uint64_t t1 = t;
        sk_down_set(&d, t);
        while (!sk_down_expired(&d, t))
            t += SEC / 2;
        cleared[i] = (t - t1) / SEC;
        t += 5 * SEC;                       /* tries again, stands down soon */
    }
    ok = cleared[0] == 60 && cleared[1] == 120 && cleared[2] == 240 && cleared[3] == 480
         && cleared[4] == 600 && cleared[5] == 600;
    t += 31 * 60 * SEC;
    sk_down_set(&d, t);
    snprintf(what, sizeof what, "8 Windows stand-down: %llu, %llu, %llu, %llu, %llu, %llu s; after"
             " 31 min %.0f s", (unsigned long long)cleared[0], (unsigned long long)cleared[1],
             (unsigned long long)cleared[2], (unsigned long long)cleared[3],
             (unsigned long long)cleared[4], (unsigned long long)cleared[5], d.hold_ns / 1e9);
    check(ok && d.hold_ns == 60 * SEC, what);
}

/* 9. The Metal pacer on a 120 Hz display after a cold shader cache: four
 * seconds of frames a refresh late raise the depth to its most; then the
 * frames are on time and the depth (and the vblank-to-glass time) come back. */
static void metal_cold_cache(void)
{
    pc_display d;
    pc_pacer p;
    pc_aim a;
    uint64_t v = 7000 * SEC, R;
    int i, raised = 0, back_at = -1, max_depth = 0;
    double lat0 = 0, lat1 = 0;
    char what[180];

    memset(&d, 0, sizeof d);
    d.refresh_hz = 120;
    d.min_hz = 120;
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 12 * MS, 1);
    R = p.period_ns;
    p.grid_ns = v;
    for (i = 1; i <= 60 * 120; i++) {
        int slip = i > PC_GRACE_FRAMES && i <= PC_GRACE_FRAMES + 240 && i % 5 == 0;
        int r;
        v += 2 * R;
        pc_pacer_aim(&p, v - 5 * MS, v - 5 * MS + 300000, &a);
        r = pc_pacer_glass(&p, &a, a.target_ns + (slip ? R : 0));
        raised += r > 0;
        if (p.depth > max_depth)
            max_depth = p.depth;
        if (i == PC_GRACE_FRAMES)
            lat0 = (a.target_ns - (v - 5 * MS)) / 1e6;
        if (back_at < 0 && raised && p.depth == 1 && i > PC_GRACE_FRAMES + 240)
            back_at = i - (PC_GRACE_FRAMES + 240);
        lat1 = (a.target_ns - (v - 5 * MS)) / 1e6;
    }
    snprintf(what, sizeof what, "9 Metal 120 Hz cold cache: depth up to %d, back to 1 after %.1f s"
             " of frames on time; vblank-to-glass %.1f -> %.1f ms", max_depth, back_at / 60.0,
             lat0, lat1);
    check(max_depth == 3 && back_at > 0 && back_at <= 2 * PC_DECAY_FRAMES + 1
          && fabs(lat1 - lat0) < 0.5, what);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v") ? 1 : argc > 1 && !strcmp(argv[1], "-vv") ? 2 : 0;
    recomp_env_set(RENV_VBLANK_SYNC, "display");
    all();
    windows_stand_down();
    metal_cold_cache();
    printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

/*
 * The vblank schedule (xbox_VblankSchedule, kernel_pacing.c).
 *
 * The timer thread raises vblank N at t0 + N * 10^9 / 60 ns. Driven here with
 * made-up clocks: a steady grid, a pass 50 ms late (caught up one vblank per
 * call, no restart), a 150 ms stall (the grid restarts at now instead of
 * firing a burst), 10^6 vblanks fired at jittered times with no drift, and an
 * epoch near 2^62 ns, a host that has been up for a very long time.
 *
 * Then RECOMP_VBLANK_SYNC=display against a simulated FIFO display at 60,
 * 59.94, 90, 100, 119.88, 120 and 144 Hz, reported as Vulkan's present_wait
 * does: only the refreshes a frame was shown at, a little late, with no
 * period. A frame now and then misses its refresh, so the steps between
 * reports are uneven whole numbers of refreshes (BO-77).
 *
 * Then the Windows DXGI vblank thread's model (src/video/dxgi_vblank_model.h,
 * BO-191) between a simulated display and the same lock: a 60 fps title on
 * fixed 240, 239.76, 59.94, 144 and 165 Hz displays and on variable-refresh
 * 240 and 144 Hz ones (the display refreshes at each present, its floor is
 * 48 Hz), with the thread's wake-up up to 1 ms late, and a mode change and a
 * VRR toggle mid-run.
 */
#include "kernel_pacing.h"
#include "platform/recomp_env.h"
#include "video/dxgi_vblank_model.h"
#include "pacing_diag.h"
#include "video/frames_lost_rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define NS_PER_S 1000000000ull
#define MS       1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Edge n of a grid that starts at t0. */
static uint64_t edge(uint64_t t0, uint64_t n)
{
    return t0 + n * NS_PER_S / 60u;
}

static void steady_grid(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 5 * NS_PER_S, next, late, n;
    int ok = 1;

    /* The first call starts the grid and fires vblank 0. */
    ok &= xbox_VblankSchedule(&s, t0, &next, &late) == 1;
    ok &= next == edge(t0, 1) && late == 0;
    for (n = 1; n < 600 && ok; n++) {
        /* A hair early: nothing yet, and told the same edge. */
        ok &= xbox_VblankSchedule(&s, edge(t0, n) - 1, &next, &late) == 0;
        ok &= next == edge(t0, n);
        ok &= xbox_VblankSchedule(&s, edge(t0, n), &next, &late) == 1;
        ok &= next == edge(t0, n + 1) && late == 0;
    }
    check(ok, "steady grid: one vblank per edge, none early");
    check(s.count == 600 && edge(t0, 600) - t0 == 10 * NS_PER_S,
          "600 vblanks span exactly 10 s");
}

static void late_tick(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 1 * NS_PER_S, now, next, late;
    int fired = 0, restarted = 0;

    xbox_VblankSchedule(&s, t0, &next, &late);
    /* The next pass comes 50 ms late: edges 1, 2 and 3 (16.7, 33.3 and
     * 50 ms) are all due. Each call fires one and reports the next edge,
     * in the past while behind. */
    now = t0 + 50 * MS;
    while (xbox_VblankSchedule(&s, now, &next, &late)) {
        fired++;
        restarted |= late != 0;
        if (fired > 10)
            break;
    }
    check(fired == 3, "50 ms late: the three missed edges fire one per call");
    check(!restarted && s.t0_ns == t0, "50 ms late: the grid keeps its epoch");
    check(next == edge(t0, 4), "50 ms late: then waits for the next edge");
}

static void stall_restart(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 2 * NS_PER_S, now, next, late;
    int r;

    xbox_VblankSchedule(&s, t0, &next, &late);
    /* 150 ms with no pass: edge 1 is 133 ms overdue, past the 100 ms rule. */
    now = t0 + 150 * MS;
    r = xbox_VblankSchedule(&s, now, &next, &late);
    check(r == 1, "150 ms stall: one vblank fires");
    check(late == now - edge(t0, 1), "150 ms stall: reports how late it was");
    check(s.t0_ns == now && next == edge(now, 1),
          "150 ms stall: the grid restarts at now, no burst");
    check(xbox_VblankSchedule(&s, now + 1, &next, &late) == 0,
          "150 ms stall: nothing more until the new edge");
}

static void no_drift(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 3 * NS_PER_S, now = t0, next = 0, late, fired = 0;
    uint32_t rng = 12345;
    const uint64_t N = 1000000;
    int ok = 1;

    /* The caller wakes up to 900 us after each edge, as a host sleep does. */
    while (fired < N) {
        if (xbox_VblankSchedule(&s, now, &next, &late)) {
            fired++;
            ok &= late == 0;
        }
        rng = rng * 1103515245u + 12345u;
        now = next > now ? next + (rng >> 8) % (900 * 1000) : now;
    }
    check(ok, "10^6 jittered vblanks: never restarted");
    check(s.t0_ns == t0 && next == edge(t0, N),
          "10^6 jittered vblanks: the next edge is still t0 + N/60 s");
    /* 10^6 vblanks are 16666.67 s: the rate is exact to the nanosecond. */
    check(next - t0 == 16666666666666ull, "no drift over 10^6 vblanks");
}

static void large_epoch(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 1ull << 62, next, late, prev, n;
    int ok = 1;

    xbox_VblankSchedule(&s, t0, &next, &late);
    prev = t0;
    for (n = 1; n <= 6000; n++) {
        uint64_t due = next, gap;
        ok &= xbox_VblankSchedule(&s, due, &next, &late) == 1 && late == 0;
        gap = due - prev;
        ok &= gap == 16666666u || gap == 16666667u;
        prev = due;
    }
    check(ok, "epoch near 2^62 ns: every gap is 16.666667 ms");
    check(next == t0 + 6001ull * NS_PER_S / 60u,
          "epoch near 2^62 ns: no wrap, 100 s on the grid");
}

/* A title flipping at every emulated vblank onto a FIFO display of the
 * given refresh: each frame is ready `work` after its vblank (one in 29
 * takes longer than a frame and misses), shown at the first free refresh,
 * and reported up to 300 us after it. Returns the vblank rate over the last
 * 20 of 40 simulated seconds, edge to edge. */
static double display_run(double hz, int fps30)
{
    XboxVblankSchedule s = {0};
    const uint64_t t0 = 3 * NS_PER_S, end = t0 + 40 * NS_PER_S;
    const double rp = 1e9 / hz;
    uint64_t now = t0, next = t0, late, shown = 0, report = 0, n_mid = 0, n = 0;
    uint64_t t_mid = 0, t_last = 0;
    uint64_t pend[8];
    int npend = 0;
    unsigned rng = 12345;

    while (now < end) {
        /* The next event: a vblank edge, or the oldest pending report. */
        if (npend && pend[0] <= next) {
            int i;
            now = pend[0];
            xbox_VblankDisplayRefresh(now, 0);
            for (i = 1; i < npend; i++)
                pend[i - 1] = pend[i];
            npend--;
            continue;
        }
        now = next;
        if (!xbox_VblankSchedule(&s, now, &next, &late))
            continue;
        n++;
        t_last = now;
        if (now >= t0 + 20 * NS_PER_S && !n_mid) {
            n_mid = n;
            t_mid = now;
        }
        if (fps30 && (n & 1))
            continue;
        {
            uint64_t ready = now + (n % 29 == 0 ? 20 : 4) * MS;
            uint64_t k = (uint64_t)((double)(ready - t0) / rp) + 1, at;
            at = t0 + (uint64_t)((double)k * rp);
            if (at <= shown)
                at = shown + (uint64_t)rp;
            shown = at;
            rng = rng * 1103515245u + 12345u;
            report = at + (rng >> 8) % 300000u;
            if (npend < 8)
                pend[npend++] = report;
        }
    }
    return (double)(n - n_mid) * 1e9 / (double)(t_last - t_mid);
}

/* A CVDisplayLink's reports: every refresh, with its period, each up to
 * `jitter` either side of the refresh (an external display's link that
 * extrapolates its timeline steps 3.7-13.7 ms on 8.333). Returns the vblank
 * rate over the last 20 of 40 simulated seconds, edge to edge. */
static double link_run(double hz, uint64_t jitter)
{
    XboxVblankSchedule s = {0};
    const uint64_t t0 = 3 * NS_PER_S, end = t0 + 40 * NS_PER_S;
    const double rp = 1e9 / hz;
    uint64_t now = t0, next = t0, late, k = 1, n = 0, n_mid = 0, t_mid = 0, t_last = 0;
    unsigned rng = 777;

    while (now < end) {
        uint64_t r = t0 + (uint64_t)((double)k * rp);
        if (r <= next) {
            rng = rng * 1103515245u + 12345u;
            now = r;
            xbox_VblankDisplayRefresh(jitter ? r - jitter + (rng >> 8) % (2 * jitter) : r,
                                      (uint64_t)rp);
            k++;
            continue;
        }
        now = next;
        if (!xbox_VblankSchedule(&s, now, &next, &late))
            continue;
        n++;
        t_last = now;
        if (now >= t0 + 20 * NS_PER_S && !n_mid) {
            n_mid = n;
            t_mid = now;
        }
    }
    return (double)(n - n_mid) * 1e9 / (double)(t_last - t_mid);
}

static void display_lock(void)
{
    static const double hz[] = { 60.0, 59.94, 90.0, 100.0, 119.88, 120.0, 144.0 };
    unsigned i;
    char what[96];

    for (i = 0; i < sizeof hz / sizeof hz[0]; i++) {
        /* A multiple of 60 within 1%: the display's own rate over two or
         * one refreshes; otherwise the free-running 60.000. */
        double m = hz[i] / 60.0, want = 60.0;
        double r60, r30;
        if (m > 0.99 && m < 1.01)
            want = hz[i];
        else if (m > 1.98 && m < 2.02)
            want = hz[i] / 2;
        r60 = display_run(hz[i], 0);
        r30 = display_run(hz[i], 1);
        snprintf(what, sizeof what, "display %.2f Hz: vblank %.3f Hz (30 fps title %.3f)",
                 hz[i], r60, r30);
        check(r60 > want - 0.005 && r60 < want + 0.005
              && r30 > want - 0.005 && r30 < want + 0.005, what);
    }
}

static void display_link(void)
{
    double clean = link_run(119.88, 0), jit = link_run(120.0, 4 * MS);
    char what[96];

    snprintf(what, sizeof what, "link 119.88 Hz, clean: vblank %.3f Hz", clean);
    check(clean > 59.935 && clean < 59.945, what);
    snprintf(what, sizeof what, "link 120 Hz, +-4 ms stamps: vblank %.3f Hz", jit);
    check(jit > 59.995 && jit < 60.005, what);
}

/* The Steam Deck: the game starts at 90 Hz and Steam switches the display
 * to the player's 60 once the game has a window. Reported as present_wait
 * does (the refreshes a frame was shown at, a little late). Returns the
 * largest distance, in the last 10 of 40 s, from a vblank edge to the 60 Hz
 * refresh grid: with the lock on that grid it is the reports' lateness; on
 * the 5.55 ms period the 90-then-60 steps fold into, any third of a refresh. */
static double switch_run(double hz1, double hz2, double at_s, uint64_t spike, uint64_t *gap_dev)
{
    static uint64_t base = 500 * NS_PER_S;   /* each run later than the last: the lock keeps state */
    XboxVblankSchedule s = {0};
    const uint64_t t0 = (base += 100 * NS_PER_S), sw = t0 + (uint64_t)(at_s * 1e9), end = t0 + 40 * NS_PER_S;
    uint64_t now = t0, next = t0, late, shown = 0, worst = 0, prev_edge = 0, nrep = 0;
    uint64_t pend[8];
    int npend = 0;
    unsigned rng = 4242;
    const double p2 = 1e9 / hz2;

    while (now < end) {
        if (npend && pend[0] <= next) {
            int i;
            now = pend[0];
            xbox_VblankDisplayRefresh(now, 0);
            for (i = 1; i < npend; i++)
                pend[i - 1] = pend[i];
            npend--;
            continue;
        }
        now = next;
        if (!xbox_VblankSchedule(&s, now, &next, &late))
            continue;
        if (now >= end - 10 * NS_PER_S && prev_edge && gap_dev) {
            uint64_t g = now - prev_edge, nom = (uint64_t)p2;
            uint64_t dv = g > nom ? g - nom : nom - g;
            if (dv > *gap_dev)
                *gap_dev = dv;
        }
        prev_edge = now;
        if (now >= end - 10 * NS_PER_S) {
            uint64_t ph = (uint64_t)fmod((double)(now - sw), p2);
            uint64_t d = ph < p2 / 2 ? ph : (uint64_t)p2 - ph;
            if (d > worst)
                worst = d;
        }
        {   /* ready 4 ms after its vblank, shown at the next refresh */
            uint64_t ready = now + 4 * MS, at;
            if (ready < sw) {
                double rp = 1e9 / hz1;
                at = t0 + (uint64_t)(ceil((double)(ready - t0) / rp) * rp);
                if (at > sw)
                    at = sw;
            } else
                at = sw + (uint64_t)(ceil((double)(ready - sw) / p2) * p2);
            if (at <= shown)
                continue;
            shown = at;
            rng = rng * 1103515245u + 12345u;
            if (npend < 8)
                pend[npend++] = at + (rng >> 8) % 300000u + (spike && ++nrep % 10 == 0 ? spike : 0);
        }
    }
    return (double)worst / 1e6;
}

static void display_switch(void)
{
    /* Where the 60 Hz grid falls against the 90 Hz one decides which third
     * of a refresh the old lock kept: three switch times, the worst. */
    uint64_t dev = 0;
    double w = switch_run(90.0, 60.0, 5.0, 0, NULL), w2 = switch_run(90.0, 60.0, 5.0037, 0, NULL),
           w3 = switch_run(90.0, 60.0, 5.0093, 0, NULL);
    if (w2 > w) w = w2;
    if (w3 > w) w = w3;
    char what[96];

    snprintf(what, sizeof what, "display 90 -> 60 Hz: vblank %.2f ms off the refresh at most", w);
    check(w < 1.0, what);
    /* A reporting thread that wakes a millisecond late now and then (the
     * Deck's present_wait waiter): the vblank's gaps stay even. */
    switch_run(60.0, 60.0, 1.0, 1200000u, &dev);
    snprintf(what, sizeof what, "reports 1.2 ms late 1 in 10: gaps within %.2f ms of 16.67", dev / 1e6);
    check(dev < 600000u, what);
}

/* ── BO-210: a long session ──────────────────────────────── */

/* As switch_run, and the title misses its refresh once every `miss` frames
 * (a heavy frame: shown a refresh late, a 33 ms step in the reports), and
 * at relearn_s the lock is told to relearn (the cadence diagnosis on a
 * pacing fault). Returns the worst distance of a vblank edge from the 60 Hz
 * refresh grid over [from_s, to_s). */
static int s_fold_hint;   /* fold_run: the window system states the refresh (BO-210) */
static uint64_t s_fold_hint_fixed;   /* ... or states this, whatever the display does */
static int s_fold_hint_ok;           /* xbox_VblankDisplayCycleOk() at the run's end */
static double fold_run(double at_s, int miss, double relearn_s, double from_s, double to_s)
{
    static uint64_t base = 9000 * NS_PER_S;
    XboxVblankSchedule s = {0};
    const uint64_t t0 = (base += 200 * NS_PER_S), sw = t0 + (uint64_t)(at_s * 1e9);
    const uint64_t end = t0 + (uint64_t)(to_s * 1e9);
    const uint64_t from = t0 + (uint64_t)(from_s * 1e9);
    const uint64_t rl = relearn_s > 0 ? t0 + (uint64_t)(relearn_s * 1e9) : ~0ull;
    uint64_t now = t0, next = t0, late, shown = 0, worst = 0, nfr = 0;
    uint64_t pend[8];
    int npend = 0, relearned = 0;
    unsigned rng = 777;
    const double p2 = 1e9 / 60.0, p1 = 1e9 / 90.0;

    xbox_VblankDisplayCycle(s_fold_hint_fixed ? s_fold_hint_fixed : s_fold_hint ? (uint64_t)p1 : 0);
    while (now < end) {
        if (npend && pend[0] <= next) {
            int i;
            now = pend[0];
            if (s_fold_hint && !s_fold_hint_fixed)
                xbox_VblankDisplayCycle(now >= sw ? (uint64_t)p2 : (uint64_t)p1);
            xbox_VblankDisplayRefresh(now, 0);
            for (i = 1; i < npend; i++)
                pend[i - 1] = pend[i];
            npend--;
            continue;
        }
        now = next;
        if (!relearned && now >= rl) {
            xbox_VblankDisplayRelearn();
            relearned = 1;
        }
        if (!xbox_VblankSchedule(&s, now, &next, &late))
            continue;
        if (now >= from) {
            uint64_t ph = (uint64_t)fmod((double)(now - sw), p2);
            uint64_t d = ph < p2 / 2 ? ph : (uint64_t)p2 - ph;
            if (d > worst)
                worst = d;
        }
        {
            uint64_t ready = now + (miss && ++nfr % (uint64_t)miss == 0 ? 19 : 4) * MS, at;
            if (ready < sw) {
                at = t0 + (uint64_t)(ceil((double)(ready - t0) / p1) * p1);
                if (at > sw)
                    at = sw;
            } else
                at = sw + (uint64_t)(ceil((double)(ready - sw) / p2) * p2);
            if (at <= shown)
                continue;
            shown = at;
            rng = rng * 1103515245u + 12345u;
            if (npend < 8)
                pend[npend++] = at + (rng >> 8) % 300000u;
        }
    }
    s_fold_hint_ok = xbox_VblankDisplayCycleOk();
    xbox_VblankDisplayCycle(0);
    return (double)worst / 1e6;
}

static void long_session(void)
{
    /* The Deck's 90 -> 60 switch, then a title that misses a refresh now
     * and then: the period folded from the two rates (a third of a refresh)
     * was unfolded only after two seconds with no missed frame, which a
     * heavy session may never have. Several switch phases, the worst. */
    static const double at[] = { 5.0, 5.0037, 5.0093, 5.0061, 5.0012, 5.0081 };
    double w = 0, w2 = 0;
    unsigned i;
    char what[120];

    for (i = 0; i < sizeof at / sizeof at[0]; i++) {
        double a = fold_run(at[i], 9, 0, 30, 60);
        if (a > w) w = a;
    }
    snprintf(what, sizeof what, "90 -> 60 Hz, a miss every 9 frames: vblank %.2f ms off the"
             " refresh at most", w);
    check(w < 1.0, what);
    /* A lock told to relearn mid-run (the cadence diagnosis's action) is
     * back on the refresh within a second, and never leaves the 60 Hz
     * grid by more than a refresh's quarter meanwhile. */
    for (i = 0; i < sizeof at / sizeof at[0]; i++) {
        double a = fold_run(at[i], 9, 20, 21, 40);
        if (a > w2) w2 = a;
    }
    snprintf(what, sizeof what, "relearn at 20 s: vblank %.2f ms off the refresh at most"
             " from 21 s", w2);
    check(w2 < 1.0, what);
    /* BO-210: with the window system's refresh (gamescope's refresh cycle)
     * the lock takes the new rate at the first report after the switch,
     * and a relearn at the next report: on the 60 Hz grid 0.2 s after. */
    s_fold_hint = 1;
    w = w2 = 0;
    for (i = 0; i < sizeof at / sizeof at[0]; i++) {
        double a = fold_run(at[i], 9, 0, at[i] + 0.2, at[i] + 3), b = fold_run(at[i], 9, 20, 20.2, 23);
        if (a > w) w = a;
        if (b > w2) w2 = b;
    }
    snprintf(what, sizeof what, "refresh stated, 90 -> 60 Hz: %.2f ms off 0.2-3 s after;"
             " relearn: %.2f", w, w2);
    check(w < 1.0 && w2 < 1.0, what);
    {
        /* A stated refresh that is not the display's (16.67 ms while the
         * panel runs 90 Hz: a limiter's target): eight reports off its grid
         * and the lock learns from the reports; once the panel is at 60 the
         * reports agree and it is taken again. */
        int ok90, ok60;
        s_fold_hint_fixed = 16666667u;
        fold_run(1000, 0, 0, 1, 5);
        ok90 = s_fold_hint_ok;
        w = fold_run(5.0, 9, 0, 7, 20);
        ok60 = s_fold_hint_ok;
        s_fold_hint_fixed = 0;
        snprintf(what, sizeof what, "refresh stated 16.67 at 90 Hz: dropped (%s); at 60 taken"
                 " again (%s), %.2f ms off", ok90 ? "no" : "yes", ok60 ? "yes" : "no", w);
        check(!ok90 && ok60 && w < 1.0, what);
    }
    s_fold_hint = 0;
}

/* The cadence diagnosis (pacing_diag.h) against the XDK swap rule: the
 * executor's work per frame from a distribution, the hold to the asked
 * vblank, and a swap whose vblank has passed waits for the next one (the
 * title never sets "immediate when late"). Returns the flips per second
 * over 60 s and the last kind the diagnosis said; *faults counts the
 * pacing-fault lines, *lines all lines. */
typedef struct {
    double busy_ms, busy_sd;       /* executor work */
    double idle_ms;                /* waiting for the title */
    int    edge_jitter_every;      /* every n-th vblank edge comes 9 ms late */
} cadence_case;

static double cadence_run(const cadence_case *c, int *last_kind, int *faults, int *lines)
{
    XboxPacingDiag d;
    const uint64_t P = NS_PER_S / 60u, t0 = 50000 * NS_PER_S;
    uint64_t flip_t = t0, count = 0, last_target = 0, nflips = 0, t_end = t0 + 60 * NS_PER_S;
    unsigned rng = 99;
    char line[400];

    pdiag_init(&d);
    *last_kind = PDIAG_CLEAN;
    *faults = *lines = 0;
    while (flip_t < t_end) {
        double u1, u2, w;
        uint64_t arrive, target, edge, busy, idle;
        int late, k;
        rng = rng * 1103515245u + 12345u;
        u1 = ((rng >> 8) & 0xFFFF) / 65536.0 + 1e-9;
        rng = rng * 1103515245u + 12345u;
        u2 = ((rng >> 8) & 0xFFFF) / 65536.0;
        w = c->busy_ms + c->busy_sd * sqrt(-2 * log(u1)) * cos(6.2831853 * u2);
        if (w < 1) w = 1;
        busy = (uint64_t)(w * 1e6);
        idle = (uint64_t)(c->idle_ms * 1e6);
        arrive = flip_t + busy + idle;
        /* The vblank count when the swap is scheduled (at arrival). */
        count = (arrive - t0) / P;
        target = last_target + 1;
        late = (int64_t)(target - count) <= 0;
        if (late)
            target = count + 1;
        edge = t0 + target * P;
        if (c->edge_jitter_every && target % (uint64_t)c->edge_jitter_every == 0)
            edge += 9 * MS;       /* a pacing fault: the vblank's edge moves */
        if (c->edge_jitter_every && !late && arrive > t0 + target * P - 2 * MS
                && target % (uint64_t)c->edge_jitter_every == 1)
            late = 1;
        last_target = target;
        nflips++;
        k = pdiag_flip(&d, edge, 1, late, busy, idle, edge - arrive, 0, 0, 0, line, sizeof line);
        if (k != PDIAG_NONE) {
            printf("    [PACING] %s\n", line);
            (*lines)++;
            *faults += k == PDIAG_FAULT;
            *last_kind = k;
        }
        flip_t = edge;
    }
    return (double)nflips / 60.0;
}

static void cadence_diag(void)
{
    static const cadence_case cliff = { 16.1, 0.8, 0.5, 0 };
    static const cadence_case heavy = { 14.0, 0.8, 0.5, 0 };
    static const cadence_case title = { 6.0, 0.5, 11.0, 0 };
    cadence_case c;
    int kind, faults, lines;
    double fps;
    char what[120];

    /* BO-210: an executor that needs 16.1 +- 0.8 ms a frame (16.6 with the wait for the title) (the Deck at
     * 2.9 GHz in a heavy scene) gives 40 fps, not 55: every late swap costs
     * a whole extra vblank. Said as executor-bound; nothing reset. */
    fps = cadence_run(&cliff, &kind, &faults, &lines);
    snprintf(what, sizeof what, "16.1 ms executor: %.1f fps, said executor-bound, no reset", fps);
    check(fps > 36 && fps < 46 && kind == PDIAG_EXEC && faults == 0 && lines >= 1 && lines <= 3,
          what);
    /* 14 ms: 60 fps, not a word. */
    fps = cadence_run(&heavy, &kind, &faults, &lines);
    snprintf(what, sizeof what, "14 ms executor: %.1f fps, no line", fps);
    check(fps > 59.5 && lines == 0, what);
    /* The title's own work over the frame: title-bound. */
    c = title;
    fps = cadence_run(&c, &kind, &faults, &lines);
    snprintf(what, sizeof what, "6 ms executor + 11 ms title: %.1f fps, said title-bound", fps);
    check(fps < 50 && kind == PDIAG_TITLE && faults == 0, what);
    /* Frames with 6 ms to spare that still miss every other vblank (a
     * vblank grid out of step): a pacing fault, said within one window;
     * then 10 s clean: said back to normal. */
    {
        XboxPacingDiag d;
        uint64_t t = 70000 * NS_PER_S, first_fault = 0, t_start = t;
        int i, back = 0;
        char line[400];
        pdiag_init(&d);
        for (i = 0; i < 2400; i++) {
            int bad = i < 1200, late = bad && (i & 1);
            int k;
            t += (late ? 2 : 1) * (NS_PER_S / 60u);
            k = pdiag_flip(&d, t, 1, late, 10 * MS, 500000u, 6 * MS, 0, 0, 0, line, sizeof line);
            if (k == PDIAG_FAULT && !first_fault)
                first_fault = t;
            if (k != PDIAG_NONE)
                printf("    [PACING] %s\n", line);
            back |= k == PDIAG_CLEAN;
        }
        snprintf(what, sizeof what, "10 ms frames late every other flip: pacing fault after"
                 " %.1f s, then back", first_fault ? (first_fault - t_start) / 1e9 : -1.0);
        check(first_fault && first_fault - t_start < 3 * NS_PER_S && back, what);
    }
}

/* ── the DXGI vblank thread's model (BO-191) ─────────────── */

typedef struct {
    const char *name;
    double   hz;          /* the display's real rate (VRR: its top rate) */
    uint64_t stated;      /* the mode's period the thread reads; 0 none */
    int      vrr;
    /* 0.1.1's lock input, no model, period 0: 1 every DXGI return (AMD),
     * 2 Vulkan present_wait, only the refreshes our frames were shown at
     * (NVIDIA) */
    int      raw;
    unsigned wake_us;     /* the reporting thread's wake-up: 0..wake_us late */
    /* a change at change_s: the display's new rate, stated period, VRR */
    double   change_s, hz2;
    uint64_t stated2;
    int      vrr2;
    unsigned work_us;     /* vblank to the frame ready to scan out; 0: 1.3 ms */
} dxgi_case;

typedef struct {
    double rate, p1, p99, max_dev;   /* vblank rate and intervals (ms) */
    double all_min, all_max;         /* every interval of the run, ms */
    int    hitches;                  /* intervals more than 1 ms off 16.667 */
    int    state, changes;
    double switched_s;               /* the last state change, s after the change */
} dxgi_res;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void pend_add(uint64_t *pend, int *npend, uint64_t t)
{
    int i = *npend;
    if (i >= 32)
        return;
    while (i > 0 && pend[i - 1] > t) {
        pend[i] = pend[i - 1];
        i--;
    }
    pend[i] = t;
    (*npend)++;
}

/* 40 simulated seconds; measured from 10 s, or 3 s after the change. The
 * title flips at each vblank, its frame ready work_us (0: 1.3 ms) to half a
 * millisecond more later. */
static void dxgi_run(const dxgi_case *c, dxgi_res *o)
{
    static uint64_t base = 2000 * NS_PER_S;   /* each run on its own clock stretch */
    static uint64_t edges[40 * 64], iv[40 * 64];
    XboxVblankSchedule s = {0};
    dvm_model m;
    const uint64_t t0 = base, end = t0 + 40 * NS_PER_S, floor_ns = NS_PER_S / 48;
    uint64_t t_change = c->change_s > 0 ? t0 + (uint64_t)(c->change_s * 1e9) : ~0ull;
    uint64_t t_reread = ~0ull, t_switch = 0, from;
    uint64_t now = t0, next = t0, late, grid0 = t0, gk = 1, last_refresh = 0, prev_edge = 0;
    uint64_t pend[32];
    double hz = c->hz, rp = 1e9 / c->hz;
    int vrr = c->vrr, npend = 0, ne = 0, i;
    unsigned rng = 4242;

    base += 1000 * NS_PER_S;
    from = t0 + (c->change_s > 0 ? (uint64_t)((c->change_s + 3) * 1e9) : 10 * NS_PER_S);
    dvm_reset(&m, c->stated, 1);
    memset(o, 0, sizeof *o);
    o->all_min = 1e9;
    while (now < end) {
        uint64_t ev_rep = npend ? pend[0] : ~0ull, ev_ref = ~0ull, ev;
        if (!vrr)
            ev_ref = grid0 + (uint64_t)((double)gk * rp);
        else if (last_refresh)
            ev_ref = last_refresh + floor_ns;          /* the panel's self-refresh */
        ev = next;
        if (ev_rep < ev) ev = ev_rep;
        if (ev_ref < ev) ev = ev_ref;
        if (t_change < ev) ev = t_change;
        if (t_reread < ev) ev = t_reread;
        now = ev;
        if (ev == t_change) {
            /* The mode or the driver's VRR setting changes; the thread reads
             * the stated period again at its next once-a-second look. */
            hz = c->hz2;
            rp = 1e9 / hz;
            vrr = c->vrr2;
            grid0 = now;
            gk = 1;
            last_refresh = now;
            t_change = ~0ull;
            if (c->stated2 != c->stated)
                t_reread = t0 + ((now - t0) / NS_PER_S + 1) * NS_PER_S;
            continue;
        }
        if (ev == t_reread) {
            dvm_reset(&m, c->stated2, 1);
            t_reread = ~0ull;
            continue;
        }
        if (ev == ev_rep) {
            uint64_t w, p;
            int r;
            for (i = 1; i < npend; i++)
                pend[i - 1] = pend[i];
            npend--;
            if (c->raw) {
                xbox_VblankDisplayRefresh(now, 0);
                continue;
            }
            r = dvm_refresh(&m, now, &w, &p);
            if (r & DVM_CHANGED) {
                o->changes++;
                t_switch = now;
            }
            if (r & DVM_REPORT)
                xbox_VblankDisplayRefresh(w, p);
            continue;
        }
        if (ev == ev_ref) {
            if (!vrr)
                gk++;
            last_refresh = now;
            rng = rng * 1103515245u + 12345u;
            /* present_wait reports only our frames, not a self-refresh */
            if (c->raw != 2)
                pend_add(pend, &npend, now + (uint64_t)((rng >> 8) % (c->wake_us + 1)) * 1000u);
            continue;
        }
        if (!xbox_VblankSchedule(&s, now, &next, &late))
            continue;
        if (prev_edge) {
            double d = (double)(now - prev_edge) / 1e6;
            if (d < o->all_min) o->all_min = d;
            if (d > o->all_max) o->all_max = d;
        }
        prev_edge = now;
        if (now >= from && ne < (int)(sizeof edges / sizeof edges[0]))
            edges[ne++] = now;
        if (vrr) {
            /* The flip: the frame is ready 1-1.5 ms after its vblank and the
             * panel refreshes as its scanout starts, 0.3 ms on, no sooner
             * than one top-rate period after the last refresh. */
            uint64_t at;
            rng = rng * 1103515245u + 12345u;
            at = now + (c->work_us ? c->work_us : 1000u) * 1000ull + 300000u
                 + (uint64_t)((rng >> 8) % 500u) * 1000u;
            if (last_refresh && at < last_refresh + (uint64_t)rp)
                at = last_refresh + (uint64_t)rp;
            last_refresh = at;
            rng = rng * 1103515245u + 12345u;
            pend_add(pend, &npend, at + (uint64_t)((rng >> 8) % (c->wake_us + 1)) * 1000u);
        }
    }
    for (i = 1; i < ne; i++) {
        double dev = ((double)(edges[i] - edges[i - 1]) - 1e9 / 60.0) / 1e6;
        iv[i - 1] = edges[i] - edges[i - 1];
        if (dev < 0) dev = -dev;
        if (dev > 1.0) o->hitches++;
    }
    qsort(iv, (size_t)(ne - 1), sizeof iv[0], cmp_u64);
    o->rate = (double)(ne - 1) * 1e9 / (double)(edges[ne - 1] - edges[0]);
    o->p1 = iv[(ne - 1) / 100] / 1e6;
    o->p99 = iv[(ne - 1) * 99 / 100] / 1e6;
    for (i = 0; i < ne - 1; i++) {
        double dev = (double)iv[i] / 1e6 - 1000.0 / o->rate;
        if (dev < 0) dev = -dev;
        if (dev > o->max_dev) o->max_dev = dev;
    }
    o->state = m.state;
    o->switched_s = t_switch && c->change_s > 0
        ? (double)(t_switch - t0) / 1e9 - c->change_s : 0;
}

static void dxgi_report(const dxgi_case *c, const dxgi_res *r)
{
    printf("    %-34s rate %.3f, iv p1 %.2f p99 %.2f (max dev %.2f, all %.2f-%.2f) ms,"
           " %d hitches, %s%s\n", c->name, r->rate, r->p1, r->p99, r->max_dev,
           r->all_min, r->all_max, r->hitches,
           r->state == DVM_VRR ? "VRR" : r->state == DVM_FIXED ? "fixed" : "unknown",
           c->raw == 1 ? " (raw DXGI reports)" : c->raw == 2 ? " (present_wait reports)" : "");
}

#define P_HZ(h) ((uint64_t)(1e9 / (h) + 0.5))

static void dxgi_model(void)
{
    /* 240000/1001 and 60000/1001 Hz: periods of 1001/240 and 1001/60 ms. */
    static const dxgi_case cases[] = {
        { "0.1.1: VRR 240, raw reports", 240, P_HZ(240), 1, 1, 300 },
        { "fixed 240, wake 1 ms", 240, P_HZ(240), 0, 0, 1000 },
        { "fixed 239.76 (240000/1001)", 240000.0 / 1001, 4170833, 0, 0, 1000 },
        { "fixed 59.94 (60000/1001)", 60000.0 / 1001, 16683333, 0, 0, 1000 },
        { "fixed 144", 144, P_HZ(144), 0, 0, 1000 },
        { "fixed 165", 165, P_HZ(165), 0, 0, 1000 },
        { "VRR 240", 240, P_HZ(240), 1, 0, 300 },
        { "VRR 240, wake 1 ms", 240, P_HZ(240), 1, 0, 1000 },
        { "VRR 144", 144, P_HZ(144), 1, 0, 300 },
        { "mode 240 -> 59.94 at 15 s", 240, P_HZ(240), 0, 0, 500,
          15, 60000.0 / 1001, 16683333, 0 },
        { "VRR off -> on at 15 s (240)", 240, P_HZ(240), 0, 0, 500, 15, 240, P_HZ(240), 1 },
        { "VRR on -> off at 15 s (239.76)", 240000.0 / 1001, 4170833, 1, 0, 500,
          15, 240000.0 / 1001, 4170833, 0 },
        /* GitHub #4's own report: RTX 4090, 5120x1440 at 120 Hz with G-Sync,
         * render scale 4, so a frame reaches the glass ~9 ms after its
         * vblank. 0.1.1 locked to present_wait there. */
        { "0.1.1 NVIDIA: VRR 120, 9 ms", 120, 0, 1, 2, 300, 0, 0, 0, 0, 8700 },
        { "VRR 120, 9 ms", 120, P_HZ(120), 1, 0, 300, 0, 0, 0, 0, 8700 },
        { "VRR 120, 1.3 ms", 120, P_HZ(120), 1, 0, 300 },
        { "fixed 120 (or a driver's virtual vblank)", 120, P_HZ(120), 0, 0, 1000 },
    };
    dxgi_res r[sizeof cases / sizeof cases[0]];
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dxgi_run(&cases[i], &r[i]);
        dxgi_report(&cases[i], &r[i]);
    }
    /* Case 0 models 0.1.1 (no VRR detector). Since the BO-110 lock stands
     * down on irregular reports it no longer chases its presents here, so
     * the spread is printed, not checked. */
    printf("  0.1.1 model on VRR 240 with the BO-110 lock: vblank spread %.2f ms\n",
           r[0].all_max - r[0].all_min);
    check(r[1].state == DVM_FIXED && r[1].rate > 59.995 && r[1].rate < 60.005
          && r[1].max_dev < 0.3 && !r[1].hitches,
          "fixed 240: locked, intervals within 0.3 ms with 1 ms wake-ups");
    check(r[2].state == DVM_FIXED && r[2].rate > 59.935 && r[2].rate < 59.945
          && r[2].max_dev < 0.3, "fixed 239.76: the vblank follows it, 59.94");
    check(r[3].state == DVM_FIXED && r[3].rate > 59.935 && r[3].rate < 59.945
          && r[3].max_dev < 0.3, "fixed 59.94: locked at 59.94 (stated 60000/1001)");
    check(r[4].state == DVM_FIXED && r[4].rate > 59.995 && r[4].rate < 60.005
          && r[4].max_dev < 0.01, "fixed 144: no lock (not a multiple), own 60.000");
    check(r[5].state == DVM_FIXED && r[5].rate > 59.995 && r[5].rate < 60.005
          && r[5].max_dev < 0.01, "fixed 165: no lock (not a multiple), own 60.000");
    for (i = 6; i <= 8; i++) {
        char what[96];
        snprintf(what, sizeof what, "%s: VRR seen once, vblank 16.667 +-0.01 ms", cases[i].name);
        check(r[i].state == DVM_VRR && r[i].changes == 1 && r[i].max_dev < 0.01
              && r[i].all_max - r[i].all_min < 0.02 && !r[i].hitches, what);
    }
    check(r[9].state == DVM_FIXED && r[9].rate > 59.935 && r[9].rate < 59.945
          && r[9].max_dev < 0.3 && r[9].all_max < 25.1,
          "mode change 240 -> 59.94: relocked at 59.94, no gap past 25 ms");
    check(r[10].state == DVM_VRR && r[10].switched_s < 2.0 && r[10].max_dev < 0.01
          && !r[10].hitches, "VRR switched on mid-run: seen within 2 s, then clean");
    check(r[11].state == DVM_FIXED && r[11].switched_s < 2.0 && r[11].rate > 59.935
          && r[11].rate < 59.945, "VRR switched off mid-run: relocked within 2 s, 59.94");
    check(r[12].rate > 60.5 && r[12].all_min < 12.0,
          "0.1.1 NVIDIA VRR 120, 9 ms to glass: the vblank runs fast (#4: 60.9)");
    for (i = 13; i <= 14; i++) {
        char what[96];
        snprintf(what, sizeof what, "%s: VRR seen once, vblank 16.667 +-0.01 ms", cases[i].name);
        check(r[i].state == DVM_VRR && r[i].changes == 1 && r[i].max_dev < 0.01
              && r[i].all_max - r[i].all_min < 0.02 && !r[i].hitches, what);
    }
    check(r[15].state == DVM_FIXED && r[15].rate > 59.995 && r[15].rate < 60.005
          && r[15].max_dev < 0.3 && !r[15].hitches, "fixed 120: locked, m = 2");
}

/* BO-210: the counters line's "frames lost" rule. */
static void frames_lost_rule(void)
{
    char what[160];
    int a = flr_lost(60, 45, 16666667u, 0);        /* the Deck's 45 at 60 Hz: said */
    int b = flr_lost(60, 40, 24999000u, 0);        /* a 40 fps limit: not said */
    int c = flr_lost(60, 45, 22221000u, 0);        /* a 45 fps limit: not said */
    int d = flr_lost(60, 59.5, 16666667u, 0);      /* normal */
    int e = flr_lost(30, 30, 16666667u, 0);        /* a crash camera's 30 */
    int f = flr_lost(60, 45, 0, 0);                /* no stated refresh: said */
    int g = flr_lost(60, 45, 16666667u, 1);        /* hidden window */
    int h = flr_lost(60, 50, 11111111u, 0);        /* 90 Hz panel, frames lost: said */
    snprintf(what, sizeof what, "frames lost rule: 45@60 %d, limit 40 %d, limit 45 %d, normal %d,"
             " 30 fps %d, no cycle %d, hidden %d, 90 Hz %d", a, b, c, d, e, f, g, h);
    check(a && !b && !c && !d && !e && f && !g && h, what);
}

int main(void)
{
    printf("vblank_schedule:\n");
    /* Read once, at the first schedule: on from the start. With no display
     * report the schedule is the free-running one. */
    recomp_env_set(RENV_VBLANK_SYNC, "display");
    steady_grid();
    late_tick();
    stall_restart();
    no_drift();
    large_epoch();
    display_lock();
    display_switch();      /* before display_link: its irregular stated periods stand the lock down */
    long_session();
    frames_lost_rule();
    cadence_diag();
    /* Before display_link: its irregular stamps leave the kernel's
     * stand-down counter up for a while. */
    dxgi_model();
    display_link();
    printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

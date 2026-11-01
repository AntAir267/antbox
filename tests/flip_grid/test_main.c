/*
 * The host flip grid (flip_grid.h, kernel_pacing.c's xbox_DisplayGrid and
 * xbox_FlipHostGrid*), BO-241.
 *
 * A title asks for its flips at the display's rate from the display mode's
 * whole Hz (SDL: 175 for a 174.96 Hz panel, 90 for an 89.97 Hz one). A
 * simulated FIFO display reports the refreshes frames were shown at, as
 * Vulkan's present_wait does (a little late, no period), with the vblank
 * lock off. Each frame's swap arrives after 1 to 4 ms of work, is held to
 * the grid's edge, presented 1 ms later and shown at the first free refresh
 * at or after that. Over 60 s every refresh must get exactly one new frame
 * with the display's own grid; with the mode's whole Hz the grid walks off
 * the panel and some refreshes get none or two. Then the grid state the
 * title reads (swaps, last edge, period) and the edge it predicts for the
 * frame it is building.
 */
#include "kernel_pacing.h"
#include "flip_grid.h"
#include "platform/recomp_env.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

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
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t rnd_state = 12345;
static uint64_t rnd(uint64_t n)
{
    rnd_state = rnd_state * 6364136223846793005ull + 1442695040888963407ull;
    return (rnd_state >> 33) % n;
}

typedef struct {
    int frames, refreshes, empty, doubled;
    double max_queue_ms;      /* refreshes a frame waited behind another (FIFO) */
    double period_ms;         /* xbox_DisplayGrid's, at the end */
    double rate_hz;           /* xbox_DisplayRate's, at the end */
    int pred_ok, pred_n;      /* the title's predicted edge within 0.5 ms of the real one */
    double pred_max_ms;
} run_t;

/* One run: real panel `hz`, the mode's whole Hz `mode_hz`; `use_grid` 0
 * makes the edge ignore xbox_DisplayGrid (the PR's first version). */
static run_t sim(double hz, double mode_hz, int use_grid, double secs)
{
    run_t r = {0};
    const double P = 1e9 / hz;
    /* On the host's clock: xbox_DisplayRate reads it for staleness. */
    const uint64_t t0 = xbox_HostNowNs() + NS_PER_S, end = t0 + (uint64_t)(secs * 1e9);
    uint64_t now = t0, due = 0, per = 0, last_shown = 0;
    uint64_t warm = t0 + NS_PER_S;
    uint32_t swaps = 0, issued = 0;
    long last_k = -1;
    int *hits;
    long nref = (long)(secs * hz) + 16;

    hits = calloc((size_t)nref, sizeof *hits);
    xbox_DisplayRateSet(mode_hz);
    xbox_FlipHostRate(mode_hz);
    while (now < end) {
        uint64_t ref = 0, pd = 0, release, present, shown;
        int g, late;
        long k;

        /* The title builds the frame: its predicted edge (hirefresh.c). */
        {
            uint32_t sw;
            uint64_t d, p;
            uint64_t pred = 0;
            if (xbox_FlipHostGridState(&sw, &d, &p)) {
                int32_t n = (int32_t)(issued + 1 - sw);
                if (n < 1) n = 1;
                if (n > 4) n = 4;
                pred = d + (uint64_t)n * p;
            }
            now += (1 + rnd(4)) * MS;            /* the frame's work */
            issued++;
            g = use_grid ? xbox_DisplayGrid(now, &ref, &pd) : 0;
            due = fg_next_edge(due, (uint64_t)(1e9 / mode_hz + 0.5), g, ref, pd, now, &per, &late);
            swaps++;
            xbox_FlipHostGridNote(swaps, due, per);
            if (pred && now > warm) {
                r.pred_n++;
                uint64_t e = pred > due ? pred - due : due - pred;
                if (e < 500000u)
                    r.pred_ok++;
                if ((double)e / 1e6 > r.pred_max_ms)
                    r.pred_max_ms = (double)e / 1e6;
            }
        }
        release = due > now ? due : now;
        present = release + MS;
        /* FIFO: the first refresh at or after the present, after the last;
         * a frame that has to wait behind the one before is queued. */
        {
            long k0 = (long)ceil(((double)(present - t0)) / P);
            k = k0 <= last_k ? last_k + 1 : k0;
            shown = t0 + (uint64_t)llround((double)k * P);
            if (now > warm && k < nref) {
                hits[k]++;
                r.frames++;
                if ((double)(k - k0) > r.max_queue_ms)
                    r.max_queue_ms = (double)(k - k0);
            }
        }
        last_k = k;
        last_shown = shown;
        /* present_wait's report: the shown time, up to 0.3 ms late. */
        xbox_VblankDisplayRefresh(shown + rnd(300000), 0);
        now = release;   /* the next frame's work starts at the release */
    }
    {
        long a = (long)ceil((double)(warm - t0) / P) + 2, b = last_k - 2, k;
        for (k = a; k < b && k < nref; k++) {
            r.refreshes++;
            if (hits[k] == 0) r.empty++;
            if (hits[k] > 1) r.doubled++;
        }
    }
    {
        uint64_t ref, pd;
        if (xbox_DisplayGrid(last_shown, &ref, &pd))
            r.period_ms = pd / 1e6;
        r.rate_hz = xbox_DisplayRate();
    }
    xbox_FlipHostRate(0);
    free(hits);
    return r;
}

static void one(double hz, double mode_hz)
{
    char what[128];
    run_t a, b;

    printf("panel %.3f Hz, display mode %.0f Hz, 60 s:\n", hz, mode_hz);
    b = sim(hz, mode_hz, 0, 60.0);
    printf("    mode's whole Hz: %d frames, %d refreshes, %d empty, queued up to %.0f refreshes\n",
           b.frames, b.refreshes, b.empty, b.max_queue_ms);
    a = sim(hz, mode_hz, 1, 60.0);
    printf("    display's grid:  %d frames, %d refreshes, %d empty, queued up to %.0f refreshes;"
           " period %.4f ms, rate %.3f Hz, predicted %d/%d (max off %.3f ms)\n",
           a.frames, a.refreshes, a.empty, a.max_queue_ms, a.period_ms, a.rate_hz,
           a.pred_ok, a.pred_n, a.pred_max_ms);
    snprintf(what, sizeof what, "%.2f Hz: every refresh one new frame (grid)", hz);
    check(a.empty == 0 && a.doubled == 0 && a.max_queue_ms == 0, what);
    snprintf(what, sizeof what, "%.2f Hz: the measured period within 1 %% of the panel's", hz);
    check(fabs(a.period_ms - 1e3 / hz) < 1e3 / hz / 100.0, what);
    snprintf(what, sizeof what, "%.2f Hz: xbox_DisplayRate within 1 %% of the panel's", hz);
    check(fabs(a.rate_hz - hz) < hz / 100.0, what);
    snprintf(what, sizeof what, "%.2f Hz: the title's predicted edge within 0.5 ms (99 %%)", hz);
    check(a.pred_n > 0 && a.pred_ok >= a.pred_n * 99 / 100, what);
    if (fabs(mode_hz - hz) > 0.001) {
        snprintf(what, sizeof what, "%.2f Hz: the mode's whole Hz does not hold it", hz);
        check(b.empty + b.doubled > 0 || b.max_queue_ms > 0, what);
    }
}

int main(void)
{
    printf("flip_grid\n");
    one(174.96, 175.0);
    one(89.97, 90.0);
    one(143.856, 144.0);
    one(119.88, 120.0);
    one(120.0, 120.0);
    /* With nothing reported, the display mode's rate. */
    {
        uint64_t ref, pd;
        xbox_VblankDisplayRelearn();
        xbox_DisplayRateSet(144.0);
        check(xbox_DisplayGrid(xbox_HostNowNs(), &ref, &pd) == 0
              && fabs(xbox_DisplayRate() - 144.0) < 1e-9,
              "no reports: the display mode's rate");
    }
    /* When each frame was shown (xbox_FlipHostHanded/Shown/ShownAt): the
     * hand-over stands in until a display time comes. */
    {
        const uint64_t P = 11111111ull, e0 = xbox_HostNowNs() + 5 * NS_PER_S;
        uint64_t sh = 0, ed = 0;
        int real = -1;
        xbox_FlipHostHanded(100, 500, e0, P, e0 + 100000u);          /* on time */
        check(xbox_FlipHostShownAt(500, &sh, &ed, &real) && sh == e0 && ed == e0 && real == 2,
              "shown: a frame handed over on time is shown at its edge");
        xbox_FlipHostHanded(101, 501, e0 + P, P, e0 + P + 3000000u); /* 3 ms late */
        check(xbox_FlipHostShownAt(501, &sh, &ed, &real) && sh == e0 + 2 * P && real == 0,
              "shown: 3 ms late, at the next refresh");
        xbox_FlipHostHanded(102, 502, e0 + 2 * P, P, e0 + 4 * P + 1000000u); /* 2+ late */
        check(xbox_FlipHostShownAt(502, &sh, &ed, &real) && sh == e0 + 5 * P,
              "shown: two periods late, at the first refresh after it");
        check(!xbox_FlipHostShownAt(499, &sh, &ed, &real), "shown: an unknown swap is not");
        xbox_FlipHostShown(102, e0 + 5 * P + 9000000u);
        check(xbox_FlipHostShownAt(502, &sh, &ed, &real) && sh == e0 + 5 * P + 9000000u
              && real == 1, "shown: a display time replaces the stand-in");
        xbox_FlipHostShown(102, e0 + 7 * P);
        check(xbox_FlipHostShownAt(502, &sh, &ed, &real) && sh == e0 + 5 * P + 9000000u,
              "shown: a frame's first display time stands (a repeat is no new one)");
        /* Display times flowing: a frame waits for its own. */
        xbox_FlipHostHanded(103, 503, e0 + 6 * P, P, e0 + 6 * P);
        check(!xbox_FlipHostShownAt(503, &sh, &ed, &real), "shown: with display times, none until it comes");
        xbox_FlipHostShown(103, e0 + 7 * P);
        check(xbox_FlipHostShownAt(503, &sh, &ed, &real) && sh == e0 + 7 * P && real == 3,
              "shown: on time and its display time");
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}

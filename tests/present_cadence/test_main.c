/*
 * present_cadence.c (BO-115): the strategy per display, where a present is
 * aimed, and the on-glass statistics, against made-up displays.
 *
 *   strategy   60/120/240 Hz fixed lock; 59.94 and 119.88 count as multiples;
 *              75/100/144 Hz are "timed, odd"; a variable display is timed;
 *              legacy stays legacy; nothing known is timed.
 *   lock       on a 120 Hz and a 60 Hz grid every frame of a locked vblank is
 *              aimed at the refresh `depth` after its own, the targets one
 *              guest frame apart, the time handed to the system 3 ms after
 *              the refresh before it (at most half a refresh); a frame picked up after its refresh is
 *              late, aimed at the next it can make, and not counted; four
 *              slips raise the depth at 120 Hz and never at 60 Hz, where one
 *              more refresh would put a frame two frames behind its vblank.
 *   handover   BO-239: the time handed over names the frame's own refresh
 *              with the grid estimate 1.2 ms off either way, at 60, 120
 *              and 240 Hz, picked up on time and late; it is 13.7 ms before
 *              the refresh at 60 Hz; RECOMP_PRESENT_HANDOVER past half a
 *              refresh is half a refresh; frames shown a refresh early put
 *              it back at half a refresh; slips still raise the depth.
 *   forced     BO-239 (#42): at 60 Hz a frame shown a refresh late takes
 *              the next frame's refresh; that frame is forced late, not a
 *              slip; a skip breaks the chain; a whole stream a refresh late
 *              is one slip.
 *   timed     the vblank plus the lead; six late frames add 2 ms.
 *   statistics even 120 Hz (2-2-2), the 1-3-1-3 repeat, 144 Hz, a variable
 *              display with small and with large jitter, 30 fps content, a
 *              frame the presenter skipped, a window that shows nothing, the
 *              vblank-to-glass latency, the simulation steps.
 */
#include "present_cadence.h"

#include <stdio.h>
#include <string.h>

#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static pc_display disp(double hz, int vrr, double min_hz)
{
    pc_display d;
    memset(&d, 0, sizeof d);
    d.refresh_hz = hz;
    d.min_hz = vrr ? min_hz : hz;
    d.vrr = vrr;
    return d;
}

static void strategy(void)
{
    pc_display d;
    int m = 0;

    printf("strategy\n");
    check(pc_multiple_of_60(60.0, &m) && m == 1, "60 Hz is 1 x 60");
    check(pc_multiple_of_60(59.94, &m) && m == 1, "59.94 Hz is 1 x 60");
    check(pc_multiple_of_60(119.88, &m) && m == 2, "119.88 Hz is 2 x 60");
    check(pc_multiple_of_60(240.0, &m) && m == 4, "240 Hz is 4 x 60");
    check(!pc_multiple_of_60(144.0, &m) && m == 2, "144 Hz is not a multiple");
    check(!pc_multiple_of_60(75.0, NULL) && !pc_multiple_of_60(100.0, NULL)
          && !pc_multiple_of_60(50.0, NULL) && !pc_multiple_of_60(0.0, NULL),
          "75, 100, 50 and unknown are not");
    d = disp(120, 0, 0);
    check(pc_pick(&d, PC_SET_LEGACY) == PC_LEGACY, "legacy stays legacy");
    check(pc_pick(&d, PC_SET_AUTO) == PC_LOCK, "auto, 120 Hz fixed: lock");
    d = disp(60, 0, 0);
    check(pc_pick(&d, PC_SET_AUTO) == PC_LOCK, "auto, 60 Hz fixed: lock");
    d = disp(240, 0, 0);
    check(pc_pick(&d, PC_SET_AUTO) == PC_LOCK, "auto, 240 Hz fixed: lock");
    d = disp(144, 0, 0);
    check(pc_pick(&d, PC_SET_AUTO) == PC_TIMED_ODD, "auto, 144 Hz fixed: timed, odd");
    d = disp(120, 1, 48);
    check(pc_pick(&d, PC_SET_AUTO) == PC_TIMED, "auto, 48-120 Hz variable: timed");
    check(pc_pick(&d, PC_SET_LOCK) == PC_LOCK, "lock forced on a variable display");
    d = disp(0, 0, 0);
    check(pc_pick(&d, PC_SET_AUTO) == PC_TIMED, "auto, rate unknown: timed");
    check(pc_pick(&d, PC_SET_LOCK) == PC_TIMED, "lock forced without a rate: timed");
    check(pc_pick(NULL, PC_SET_AUTO) == PC_TIMED, "auto, no display: timed");
    check(pc_setting("auto") == PC_SET_LINK && pc_setting("link") == PC_SET_LINK
          && pc_setting("aimed") == PC_SET_AUTO && pc_pick(&d, PC_SET_LINK) == PC_LINK,
          "auto and link ask for the display link, aimed is 0.2.x's pick (BO-236)");
    check(pc_setting("legacy") == PC_SET_LEGACY
          && pc_setting(NULL) == PC_SET_LEGACY && pc_setting("x") == PC_SET_LEGACY
          && pc_setting("timed") == PC_SET_TIMED && pc_setting("lock") == PC_SET_LOCK,
          "the setting's text");
}

/* A locked run on a display of `hz`: the vblank `phase` before every
 * mult-th refresh, picked up 0.3 ms after it. Returns the uneven targets. */
static int lock_run(double hz, int mult, int frames, pc_pacer *p, uint64_t *first_gap)
{
    pc_display d = disp(hz, 0, 0);
    uint64_t R, g0 = 1000 * MS, phase = 5 * MS, prev = 0;
    int i, bad = 0;

    pc_pacer_init(p, PC_LOCK, &d, phase, 12 * MS, 1);
    R = p->period_ns;
    p->grid_ns = g0;                      /* a first on-glass time */
    *first_gap = 0;
    for (i = 1; i <= frames; i++) {
        uint64_t vsync = g0 + (uint64_t)i * mult * R, v = vsync - phase;
        pc_aim a;
        pc_pacer_aim(p, v, v + 300000, &a);
        uint64_t want = vsync + (uint64_t)p->depth * R;
        /* The grid is the last on-glass stamp, which is up to 40 us off. */
        if (a.late || a.target_ns < want || a.target_ns > want + 40000
                || a.at_ns != a.target_ns - R + (PC_HANDOVER_NS < R / 2 ? PC_HANDOVER_NS : R / 2))
            bad++;
        if (prev && (a.target_ns - prev > (uint64_t)mult * R + 40000
                     || a.target_ns - prev + 40000 < (uint64_t)mult * R))
            bad++;
        if (!*first_gap)
            *first_gap = a.target_ns - v;
        prev = a.target_ns;
        pc_pacer_glass(p, &a, want + (uint64_t)(i % 5) * 10000);   /* shown there, stamped a little off */
    }
    return bad;
}

static void lock(void)
{
    pc_pacer p;
    pc_display d;
    pc_aim a;
    uint64_t gap, R, v;
    int i, raised;

    printf("lock\n");
    check(lock_run(120, 2, 600, &p, &gap) == 0, "120 Hz: every frame at its refresh, 2 refreshes apart");
    check(gap == 5 * MS + p.period_ns && p.depth == 1 && p.slips_all == 0,
          "120 Hz: on the glass 5 ms + one refresh after the vblank (13.3 ms)");
    check(lock_run(60, 1, 600, &p, &gap) == 0, "60 Hz: every frame at its refresh, 1 refresh apart");
    check(gap == 5 * MS + p.period_ns && gap < 22 * MS, "60 Hz: 21.7 ms after the vblank");
    check(lock_run(240, 4, 600, &p, &gap) == 0 && gap < 10 * MS, "240 Hz: 4 refreshes apart, 9.2 ms");

    /* A frame picked up after its own refresh. */
    d = disp(120, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    v = 1000 * MS + 20 * R - 5 * MS;
    pc_pacer_aim(&p, v, v + 6 * MS, &a);
    check(a.late && a.target_ns == 1000 * MS + 22 * R, "picked up late: the next refresh it can make, flagged");
    /* A vblank that is not on the grid (the lock not engaged yet). */
    pc_pacer_aim(&p, v + 3 * MS, v + 3 * MS + 100000, &a);
    check(a.late && a.target_ns, "a vblank off the grid: aimed, not counted");
    /* No grid yet: at once. */
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    pc_pacer_aim(&p, v, v, &a);
    check(!a.at_ns && !a.target_ns, "no on-glass time yet: presented at once");

    /* Slips: frames that reach the glass one refresh after their target. */
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    p.grid_ns = 1000 * MS;
    raised = 0;
    for (i = 1; i <= PC_GRACE_FRAMES + 8; i++) {
        v = 1000 * MS + (uint64_t)i * 2 * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        raised += pc_pacer_glass(&p, &a, a.target_ns + R);
    }
    check(raised == 2 && p.depth == 3 && p.depth_max == 3,
          "120 Hz: four slips raise the depth (not during the first 120 frames)");
    /* BO-211: the depth comes back, one refresh per 600 frames on time. */
    {
        int lowered = 0, k;
        uint64_t v0 = v;
        for (k = 1; k <= 2 * PC_DECAY_FRAMES + 10; k++) {
            int r;
            v = v0 + (uint64_t)k * 2 * R;
            pc_pacer_aim(&p, v, v + 300000, &a);
            r = pc_pacer_glass(&p, &a, a.target_ns);
            lowered += r < 0;
            if (k == PC_DECAY_FRAMES + 1 && p.depth != 2)
                lowered = -100;
        }
        check(lowered == 2 && p.depth == 1, "120 Hz: 1200 frames on time take the depth back to 1");
        pc_pacer_relearn(&p);
        check(p.depth == 1 && p.grid_ns && p.grace == PC_GRACE_FRAMES,
              "relearn: the setting's depth, the grid kept, the grace anew");
    }
    /* BO-211 review: slips that come back right after each step back. The
     * calm a step back needs doubles each time (600, 1200, 2400 ... 4800
     * frames): the depth no longer swings every 12 s. */
    {
        int k, lowered = 0, raised2 = 0, last_low = 0, gap_max = 0;
        uint64_t v0;
        d = disp(120, 0, 0);
        pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
        R = p.period_ns;
        p.grid_ns = 1000 * MS;
        v0 = 1000 * MS;
        for (k = 1; k <= 60 * 60 * 6; k++) {
            int r, slip;
            /* At depth 1 a slip every 10th frame; at 2 or more none. */
            slip = k > PC_GRACE_FRAMES && p.depth == 1 && k % 10 == 0;
            v = v0 + (uint64_t)k * 2 * R - 5 * MS;
            pc_pacer_aim(&p, v, v + 300000, &a);
            r = pc_pacer_glass(&p, &a, a.target_ns + (slip ? R : 0));
            raised2 += r > 0;
            if (r < 0) {
                if (last_low && k - last_low > gap_max)
                    gap_max = k - last_low;
                last_low = k;
                lowered++;
            }
        }
        check(p.decay_frames == PC_DECAY_MAX && lowered <= 9 && gap_max >= PC_DECAY_MAX,
              "slips back after every step back: the step back waits up to 80 s");
    }
    d = disp(60, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    raised = 0;
    for (i = 1; i <= PC_GRACE_FRAMES + 40; i++) {
        v = 1000 * MS + (uint64_t)i * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        raised += pc_pacer_glass(&p, &a, a.target_ns + R);
    }
    /* BO-239: at 60 Hz a frame shown a refresh late takes the next frame's
     * refresh, so every frame after the first is forced late, not a slip;
     * here the first was in the grace, so none is a slip. */
    check(raised == 0 && p.depth == 1 && p.depth_max == 1 && p.slips_all == 0
          && p.forced_all == 40,
          "60 Hz: the depth stays 1 (2 would be two frames of latency); all 40 forced");
    check(pc_pacer_lock_report(&p, 2000 * MS) == 2000 * MS - 5 * MS,
          "the lock is told the on-glass time less the phase");
    /* A slip here and there does not raise it. */
    d = disp(120, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    raised = 0;
    for (i = 1; i <= 2000; i++) {
        v = 1000 * MS + (uint64_t)i * 2 * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        raised += pc_pacer_glass(&p, &a, a.target_ns + (i % 50 == 0 ? R : 0));
    }
    check(raised == 0 && p.depth == 1, "one slip in fifty frames: the depth stays");
}

/* The first refresh of the true grid (g + k * R) at or after t. */
static uint64_t first_at_or_after(uint64_t g, uint64_t R, uint64_t t)
{
    return t <= g ? g - (g - t) / R * R : g + (t - g + R - 1) / R * R;
}

/* BO-239: hand-over after the refresh before the frame's own. On a true grid
 * g + k * R the pacer's estimate is off by err (the last on-glass stamp); the
 * system shows a frame at the first true refresh at or after at_ns. Returns
 * the frames whose at_ns named a refresh other than their own. */
static int handover_run(double hz, int mult, int64_t err, uint64_t pick_ns, int depth,
                        uint64_t *lead_min)
{
    pc_display d = disp(hz, 0, 0);
    pc_pacer p;
    uint64_t R, g = 2000 * MS, phase = 5 * MS;
    int i, bad = 0;

    pc_pacer_init(&p, PC_LOCK, &d, phase, 0, depth);
    R = p.period_ns;
    *lead_min = ~0ull;
    for (i = 1; i <= 300; i++) {
        uint64_t vsync = g + (uint64_t)i * mult * R, v = vsync - phase, shown;
        pc_aim a;
        p.grid_ns = (uint64_t)((int64_t)(vsync - (uint64_t)mult * R) + err);
        pc_pacer_aim(&p, v, v + pick_ns, &a);
        /* The refresh the target means (the estimate's point nearest it). */
        uint64_t own = first_at_or_after(g, R, a.target_ns - R / 2);
        shown = first_at_or_after(g, R, a.at_ns > v + pick_ns ? a.at_ns : v + pick_ns);
        if (shown != own)
            bad++;
        if (own - a.at_ns < *lead_min)
            *lead_min = own - a.at_ns;
    }
    return bad;
}

static void handover(void)
{
    static const int64_t errs[] = { -1200000, -600000, 0, 600000, 1200000 };
    pc_pacer p;
    pc_display d;
    pc_aim a;
    uint64_t R, v, lead, lead60 = ~0ull;
    int i, k, bad = 0, raised;

    printf("handover\n");
    for (k = 0; k < 5; k++) {
        bad += handover_run(60, 1, errs[k], 300000, 1, &lead);
        if (lead < lead60)
            lead60 = lead;
        bad += handover_run(60, 1, errs[k], 6 * MS, 1, &lead);    /* picked up late */
        bad += handover_run(120, 2, errs[k], 300000, 1, &lead);
        bad += handover_run(120, 2, errs[k], 300000, 2, &lead);
        bad += handover_run(240, 4, errs[k], 300000, 1, &lead);
    }
    check(bad == 0, "the grid 1.2 ms off: every frame shown at its own refresh");
    check(lead60 >= 16666667 - 3 * MS - 1200000 && lead60 > 12 * MS,
          "60 Hz: handed over 13.7 ms (12.5 at the worst grid) before its refresh (was 8.3)");

    d = disp(60, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    v = 1000 * MS + 10 * R - 5 * MS;
    pc_pacer_aim(&p, v, v + 300000, &a);
    check(a.at_ns == a.target_ns - R + 3 * MS && a.target_ns == 1000 * MS + 11 * R,
          "60 Hz: 3 ms after the refresh before its own");
    pc_pacer_set_handover(&p, 12 * MS);
    pc_pacer_aim(&p, v, v + 300000, &a);
    check(a.at_ns == a.target_ns - R + R / 2, "a hand-over past half a refresh: half a refresh");
    d = disp(120, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    p.grid_ns = 1000 * MS;
    pc_pacer_aim(&p, v, v + 300000, &a);
    check(a.at_ns == a.target_ns - p.period_ns + 3 * MS, "120 Hz: 3 ms after the refresh before");
    d = disp(240, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    p.grid_ns = 1000 * MS;
    pc_pacer_aim(&p, v, v + 300000, &a);
    check(a.at_ns == a.target_ns - p.period_ns + p.period_ns / 2, "240 Hz: half a refresh (3 ms is more)");

    /* Shown a refresh early (the system took the nearest refresh): back to
     * half a refresh after PC_EARLIES_BACK, once. */
    d = disp(60, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    raised = 0;
    for (i = 1; i <= PC_GRACE_FRAMES + 10; i++) {
        int r;
        v = 1000 * MS + (uint64_t)i * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        r = pc_pacer_glass(&p, &a, a.target_ns - (p.handover_half ? 0 : R));
        if (r == 2)
            raised++;
    }
    pc_pacer_aim(&p, v + R, v + R + 300000, &a);
    check(raised == 1 && p.handover_half && p.earlies_all == PC_EARLIES_BACK
          && a.at_ns == a.target_ns - R + R / 2 && p.slips_all == 0 && p.depth == 1,
          "frames shown a refresh early: half a refresh again, no slip counted");
    pc_pacer_relearn(&p);
    check(p.handover_half, "relearn keeps it");

    /* Slips still raise the depth at 120 Hz with the new hand-over. */
    d = disp(120, 0, 0);
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    raised = 0;
    for (i = 1; i <= PC_GRACE_FRAMES + 4; i++) {
        v = 1000 * MS + (uint64_t)i * 2 * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        raised += pc_pacer_glass(&p, &a, a.target_ns + R);
    }
    check(raised == 1 && p.depth == 2 && p.slips_all == 4 && p.earlies_all == 0
          && p.forced_all == 0,
          "120 Hz: four slips still raise the depth, none taken as early or forced");
}

/* BO-239 (#42): the reporter's 60 Hz pattern. A frame misses its refresh
 * and is shown one later; the next frame's refresh is that one, so it is
 * shown a refresh late too (forced); the presenter skips the one after
 * (two presents on their way); the frame after that is on time. Only the
 * first is a slip: the presenter's own recovery costs no second one, and
 * a chain of frames all one refresh late is one slip, not one per frame. */
static void forced(void)
{
    pc_display d = disp(60, 0, 0);
    pc_pacer p;
    pc_aim a;
    uint64_t R;
    int i, raised = 0, misses = 0;

    printf("forced\n");
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    R = p.period_ns;
    p.grid_ns = 1000 * MS;
    for (i = 1; i <= 2 * PC_GRACE_FRAMES + 600; i++) {
        uint64_t v = 1000 * MS + (uint64_t)i * R - 5 * MS, shown;
        int k = i % 6;                 /* 0: miss, 1: forced, 2: skipped, 3-5: on time */
        pc_pacer_aim(&p, v, v + 300000, &a);
        if (k == 2)
            continue;                  /* skipped: never shown, nothing to feed */
        shown = a.target_ns + (k <= 1 ? R : 0);
        if (k == 0 && !p.grace)
            misses++;
        raised += pc_pacer_glass(&p, &a, shown);
    }
    check(misses > 100 && p.slips_all == (uint32_t)misses && p.forced_all == (uint32_t)misses,
          "60 Hz miss, forced, skip: one slip and one forced per miss");
    check(raised == 0 && p.depth == 1, "the depth cannot rise at 60 Hz: no raise");

    /* A stream shifted by a refresh (the chain never broken): at most one
     * slip, here none, as the chain began in the grace. */
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    p.grid_ns = 1000 * MS;
    for (i = 1; i <= PC_GRACE_FRAMES + 200; i++) {
        uint64_t v = 1000 * MS + (uint64_t)i * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        pc_pacer_glass(&p, &a, a.target_ns + R);
    }
    check(p.slips_all == 0 && p.forced_all == 200, "every frame a refresh late: no slip, all forced");

    /* Two frames late on their own, a frame on time between: two slips. */
    pc_pacer_init(&p, PC_LOCK, &d, 5 * MS, 0, 1);
    p.grid_ns = 1000 * MS;
    for (i = 1; i <= PC_GRACE_FRAMES + 3; i++) {
        uint64_t v = 1000 * MS + (uint64_t)i * R - 5 * MS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        if (i == PC_GRACE_FRAMES + 2) {
            /* this frame skipped by the presenter, the next one on time */
            continue;
        }
        pc_pacer_glass(&p, &a, a.target_ns + (i > PC_GRACE_FRAMES && i != PC_GRACE_FRAMES + 3 ? R : 0));
    }
    check(p.slips_all == 1 && p.forced_all == 0 && p.grid_ns == a.target_ns,
          "a skip breaks the chain: the frame after it on time, no forced");
}

static void timed(void)
{
    pc_display d = disp(120, 1, 48);
    pc_pacer p;
    pc_aim a;
    uint64_t v = 5000 * MS;
    int i, raised = 0;

    printf("timed\n");
    pc_pacer_init(&p, PC_TIMED, &d, 5 * MS, 12 * MS, 1);
    pc_pacer_aim(&p, v, v + 300000, &a);
    check(a.target_ns == v + 12 * MS && a.at_ns == a.target_ns && !a.late,
          "variable: presented at the vblank plus the lead");
    check(pc_pacer_lock_report(&p, v) == 0, "variable: the vblank is not locked to the glass");
    pc_pacer_aim(&p, v, v + 13 * MS, &a);
    check(a.late, "picked up after its time: late");
    for (i = 0; i < PC_GRACE_FRAMES + PC_LATES_RAISE; i++) {
        v += PC_FRAME_NS;
        pc_pacer_aim(&p, v, v + 300000, &a);
        raised += pc_pacer_glass(&p, &a, a.target_ns + 3 * MS);
    }
    check(raised == 1 && p.lead_ns == 14 * MS, "six frames 3 ms late: the lead grows 2 ms");
    {
        int lowered = 0, at599 = 0;
        for (i = 0; i < PC_DECAY_FRAMES; i++) {
            int r;
            v += PC_FRAME_NS;
            pc_pacer_aim(&p, v, v + 300000, &a);
            r = pc_pacer_glass(&p, &a, a.target_ns + 400000);
            raised += r > 0;
            lowered += r < 0;
            if (i == PC_DECAY_FRAMES - 2)
                at599 = (int)(p.lead_ns / MS);
        }
        check(raised == 1, "0.4 ms after the target is on time");
        /* BO-211: ten seconds on time take the raise back. */
        check(lowered == 1 && at599 == 14 && p.lead_ns == 12 * MS,
              "600 frames on time: the lead back to 12 ms");
    }
    /* BO-236: frames every 8.3 ms (High refresh at 120 Hz): two presents on
     * their way hold at most 16.7 ms, so the lead stops at 13.7 ms; it was
     * raised up to 32 and the presenter skipped a quarter of the frames. */
    {
        uint64_t f120 = 8333333;
        int r120 = 0;
        d = disp(120, 1, 24);
        pc_pacer_init(&p, PC_TIMED, &d, 5 * MS, 12 * MS, 1);
        pc_pacer_set_frame(&p, f120);
        check(pc_pacer_lead_max(&p) == 2 * f120 - 3 * MS, "120 fps: the lead's ceiling is two frames less 3 ms");
        for (i = 0; i < PC_GRACE_FRAMES + 20 * PC_LATES_RAISE; i++) {
            v += f120;
            pc_pacer_aim(&p, v, v + 300000, &a);
            r120 += pc_pacer_glass(&p, &a, a.target_ns + 3 * MS) > 0;
        }
        check(r120 == 0 && p.lead_ns == 12 * MS, "120 fps, every frame late: the lead stays under its ceiling");
        d = disp(120, 1, 24);
        pc_pacer_init(&p, PC_TIMED, &d, 5 * MS, 12 * MS, 1);
        p.lead_ns = 32 * MS;
        pc_pacer_set_frame(&p, f120);
        check(p.lead_ns == 2 * f120 - 3 * MS, "a lead raised at 60 fps comes down when frames come at 120");
        pc_pacer_set_frame(&p, PC_FRAME_NS);
        check(pc_pacer_lead_max(&p) == 2 * PC_FRAME_NS - 3 * MS, "60 fps: the ceiling is 30.3 ms");
        pc_pacer_set_frame(&p, 0);
        check(pc_pacer_lead_max(&p) == PC_LEAD_MAX_NS, "frames not known: the old ceiling");
    }
    d = disp(144, 0, 0);
    pc_pacer_init(&p, PC_TIMED_ODD, &d, 5 * MS, 12 * MS, 1);
    pc_pacer_aim(&p, v, v, &a);
    check(a.at_ns == v + 12 * MS && a.late, "144 Hz: timed, nothing counted");
    pc_pacer_init(&p, PC_LEGACY, &d, 0, 0, 1);
    pc_pacer_aim(&p, v, v, &a);
    check(!a.at_ns, "legacy: no time given");
}

/* n frames 1/60 s apart; on-glass times from pattern[i % np] (ns after the
 * vblank; 0: never shown). */
static int fill(pc_sample *s, int n, const uint64_t *pattern, int np, uint64_t step)
{
    int i;
    memset(s, 0, sizeof *s * (size_t)n);
    for (i = 0; i < n; i++) {
        s[i].flip = 100u + (uint32_t)i;
        s[i].vblank_ns = 9000 * MS + (uint64_t)i * step;
        s[i].glass_ns = pattern[i % np] ? s[i].vblank_ns + pattern[i % np] : 0;
        s[i].steps = 1;
    }
    return n;
}

static void statistics(void)
{
    static pc_sample s[PC_RING];
    pc_report r;
    pc_display d = disp(120, 0, 0);
    const uint64_t R = 8333333;
    uint64_t even[1] = { 13333333 };
    uint64_t saw[2] = { 13333333, 13333333 - 8333333 };   /* 1-3-1-3 */
    uint64_t none[1] = { 0 };
    uint64_t now;
    int n;

    printf("statistics\n");
    n = fill(s, 200, even, 1, PC_FRAME_NS);
    now = s[n - 1].glass_ns + 5 * MS;
    pc_analyze(s, n, &d, now, &r);
    check(r.available && r.expected == 2 && r.uneven == 0 && !r.warn_uneven && !r.warn_rate
          && r.n_iv == 199, "120 Hz, every frame 2 refreshes: even");
    check(r.iv_refresh[10] > 1.99f && r.iv_refresh[10] < 2.01f && r.iv_p50_ms > 16.6 && r.iv_p50_ms < 16.7,
          "the interval is 16.67 ms = 2.0 refreshes");
    check(r.lat_p50_ms > 13.3 && r.lat_p50_ms < 13.4 && r.lat_p95_ms < 13.4, "vblank to glass 13.3 ms");
    check(r.steps_max == 1 && r.steps_multi == 0, "one simulation step a frame");

    n = fill(s, 200, saw, 2, PC_FRAME_NS);
    pc_analyze(s, n, &d, s[n - 1].glass_ns, &r);
    check(r.uneven == r.n_iv && r.warn_uneven && r.iv_uneven[5], "the 1-3-1-3 repeat: every interval uneven, warned");
    check(r.iv_p50_ms > 8.0 && r.iv_min_ms < 8.4 && r.iv_max_ms > 24.9, "its intervals are 8.3 and 25 ms");

    /* One frame a refresh late among two hundred: counted, not warned. */
    n = fill(s, 200, even, 1, PC_FRAME_NS);
    s[100].glass_ns += R;
    pc_analyze(s, n, &d, s[n - 1].glass_ns, &r);
    check(r.uneven == 2 && !r.warn_uneven, "one late frame: two uneven intervals, no warning");

    /* 144 Hz: frames last 2 or 3 refreshes of 6.94 ms. */
    {
        pc_display d144 = disp(144, 0, 0);
        int i;
        n = fill(s, 200, even, 1, PC_FRAME_NS);
        for (i = 0; i < n; i++) {
            uint64_t t = s[i].vblank_ns + 10 * MS, p = 6944444;
            s[i].glass_ns = (t + p - 1) / p * p;        /* the first refresh at or after */
        }
        pc_analyze(s, n, &d144, s[n - 1].glass_ns, &r);
        check(r.warn_rate && r.expected == 0 && r.uneven > r.n_iv / 2, "144 Hz: warned, most intervals uneven");
    }
    /* A variable display: 0.3 ms of jitter is even, 3 ms is not. */
    {
        pc_display dv = disp(120, 1, 48);
        uint64_t small[2] = { 12 * MS, 12 * MS + 300000 }, big[2] = { 12 * MS, 15 * MS };
        n = fill(s, 200, small, 2, PC_FRAME_NS);
        pc_analyze(s, n, &dv, s[n - 1].glass_ns, &r);
        check(r.available && r.uneven == 0 && r.expected == 0 && !r.warn_rate, "variable, 0.3 ms jitter: even");
        n = fill(s, 200, big, 2, PC_FRAME_NS);
        pc_analyze(s, n, &dv, s[n - 1].glass_ns, &r);
        check(r.uneven == r.n_iv && r.warn_uneven, "variable, 3 ms jitter: uneven, warned");
    }
    /* 30 fps content (a swap every second vblank) shown 4 refreshes each. */
    n = fill(s, 100, even, 1, 2 * PC_FRAME_NS);
    pc_analyze(s, n, &d, s[n - 1].glass_ns, &r);
    check(r.uneven == 0 && r.iv_refresh[3] > 3.99f, "30 fps frames 4 refreshes each: even");

    /* A frame the presenter skipped: the frame before it lasts twice. */
    n = fill(s, 200, even, 1, PC_FRAME_NS);
    s[50].glass_ns = 0;
    s[50].skipped = 1;
    pc_analyze(s, n, &d, s[n - 1].glass_ns, &r);
    check(r.skipped == 1 && r.uneven == 1 && r.shown == 199, "a skipped frame: one uneven");

    /* Two steps in a frame. */
    s[20].steps = 2;
    pc_analyze(s, n, &d, s[n - 1].glass_ns, &r);
    check(r.steps_max == 2 && r.steps_multi == 1, "a frame of two simulation steps is seen");

    /* Nothing shown (the window is covered), and data gone stale. */
    n = fill(s, 200, none, 1, PC_FRAME_NS);
    pc_analyze(s, n, &d, s[n - 1].vblank_ns, &r);
    check(!r.available && r.shown == 0 && r.n_iv == 0 && !r.warn_uneven, "no on-glass times: not available");
    n = fill(s, 200, even, 1, PC_FRAME_NS);
    pc_analyze(s, n, &d, s[n - 1].glass_ns + 2000 * MS, &r);
    check(!r.available, "on-glass times two seconds old: not available");
    pc_analyze(s, 0, &d, 0, &r);
    check(!r.available && r.samples == 0, "an empty ring");
    /* More than the ring holds: the newest are used. */
    pc_analyze(s, 200, NULL, 0, &r);
    check(r.samples == 200 && r.expected == 0, "no display: intervals in ms only");
}

/* The display link (BO-236): callbacks every period, each naming a commit
 * deadline tt and a refresh on the glass at tt + 8 ms. */
static void link(void)
{
    pc_link l;
    uint64_t per = 8333333, tt = 3000 * MS, rep, pd = 0, r_dummy = 0;
    int i, miss, moved = 0, misses = 0;

    printf("link\n");
    pc_link_init(&l, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
    rep = pc_link_tick(&l, tt, tt + 8 * MS, 0, &pd);
    check(rep == tt - 4 * MS && pd == 0, "first callback: the vblank 4 ms before the deadline, no period yet");
    /* A start-up step of 6 ms first (seen on the Mac): the median ignores it. */
    tt += 6 * MS;
    pc_link_tick(&l, tt, tt + 8 * MS, 0, &pd);
    for (i = 0; i < 50; i++) {
        tt += per;
        rep = pc_link_tick(&l, tt, tt + 8 * MS, 0, &pd);
    }
    check(pd == per, "120 Hz callbacks after a 6 ms start-up step: the period is 8.33 ms");
    tt += 2 * per;
    rep = pc_link_tick(&l, tt, tt + 8 * MS, 0, &pd);
    check(pd == per, "a callback the system left out: still 8.33 ms");
    check(pc_link_take_until(&l, tt) == tt - PC_LINK_GUARD_NS, "the newest frame is taken until 1.5 ms before the deadline");
    /* 60 fps frames on 120 Hz callbacks (High refresh on, a 60 fps menu):
     * every other refresh is empty, and that is no miss. */
    for (i = 0; i < PC_GRACE_FRAMES + 400; i++) {
        tt += per;
        rep = pc_link_tick(&l, tt, tt + 8 * MS, i & 1, &pd);
        if (!(i & 1)) {
            moved += pc_link_frame(&l, rep + 200000, 0, &miss) != 0;
            misses += miss;
        }
    }
    check(!moved && !misses && l.phase_ns == 4 * MS, "60 fps on a 120 Hz link: no miss, the phase stays");
    /* A frame whose vblank was in time for the refresh before, which went
     * empty: it missed. The vblank moves 1 ms earlier. */
    for (i = 0; i < 20 * PC_SLIP_WINDOW; i++) {
        tt += per;
        rep = pc_link_tick(&l, tt, tt + 8 * MS, 1, &pd);
        moved += pc_link_frame(&l, rep - per, 0, &miss) > 0;
    }
    check(moved >= 1 && l.phase_ns <= per - PC_LINK_GUARD_NS - MS && l.phase_ns > 4 * MS,
          "missed frames move the vblank earlier, inside the refresh");
    {
        int lowered = 0;
        for (i = 0; i < 40 * PC_DECAY_FRAMES && l.phase_ns > 4 * MS; i++) {
            tt += per;
            rep = pc_link_tick(&l, tt, tt + 8 * MS, 0, &pd);
            lowered += pc_link_frame(&l, rep, 0, &miss) < 0;
        }
        check(lowered >= 1 && l.phase_ns == 4 * MS, "calm frames take the raise back");
    }
    /* 60 Hz callbacks: a frame after an empty refresh whose vblank came
     * after that refresh's take-until is not a miss (the title was slow). */
    pc_link_init(&l, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
    per = PC_FRAME_NS;
    for (i = 0; i < 10; i++) {
        tt += per;
        rep = pc_link_tick(&l, tt, tt + 16 * MS, 0, &pd);
    }
    tt += per;
    rep = pc_link_tick(&l, tt, tt + 16 * MS, 1, &pd);
    pc_link_frame(&l, tt - per - 1 * MS, 0, &miss);
    check(!miss, "60 Hz: a frame after the refresh before's take-until is not a miss");
    pc_link_frame(&l, tt - per - 3 * MS, 0, &miss);
    check(miss, "60 Hz: a frame in time for an empty refresh before is a miss");
    /* BO-236: a slack moves the deadline, the vblank and the take-until
     * later, never closer than 2 ms to the refresh on the glass. */
    {
        pc_link k;
        uint64_t t = 7000 * MS, r, p;
        pc_link_init(&k, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
        pc_link_set_slack(&k, 12 * MS);
        r = pc_link_tick(&k, t, t + 33 * MS, 0, &p);
        check(pc_link_deadline(&k) == t + 12 * MS && r == t + 8 * MS
              && pc_link_take_until(&k, pc_link_deadline(&k)) == t + 12 * MS - PC_LINK_GUARD_NS,
              "slack 12 ms: deadline, vblank and take-until 12 ms later");
        pc_link_set_slack(&k, 30 * MS);
        t += PC_FRAME_NS;
        pc_link_tick(&k, t, t + 16 * MS, 0, &p);
        check(pc_link_deadline(&k) == t + 14 * MS, "a slack is held 2 ms short of the refresh");
    }
    /* The slack by default: the link's lead less PC_LINK_COMMIT_MIN_NS,
     * keyed to the room, not to the link's rate (the Fable review). */
    {
        pc_link k;
        uint64_t t = 9000 * MS, p;
        const uint64_t lead120 = 16666667, lead60 = 33366667;
        pc_link_init(&k, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
        check(k.slack_ns == PC_LINK_SLACK_AUTO, "the slack is automatic by default");
        /* A 120 Hz display, the link at 60 (High refresh off, the default):
         * the lead is two 120 Hz refreshes; no slack. */
        for (i = 0; i < 20; i++) {
            t += PC_FRAME_NS;
            pc_link_tick(&k, t, t + lead120, 0, &p);
        }
        check(k.slack_eff_ns == 0 && pc_link_deadline(&k) == t,
              "120 Hz display, link at 60: no slack (the deadline is the link's)");
        check(t + lead120 - pc_link_take_until(&k, pc_link_deadline(&k)) >= 18 * MS,
              "120 Hz display, link at 60: frames taken until 18 ms or more before the glass");
        /* The same display, the link at 120 (High refresh on): no slack. */
        for (i = 0; i < 20; i++) {
            t += 8333333;
            pc_link_tick(&k, t, t + lead120, 0, &p);
        }
        check(k.slack_eff_ns == 0, "120 Hz display, link at 120: no slack");
        /* A 60 Hz display: the lead is two 60 Hz refreshes; the slack is it
         * less 17 ms, the vblank 4 ms before that. */
        pc_link_init(&k, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
        for (i = 0; i < 20; i++) {
            t += PC_FRAME_NS;
            r_dummy = pc_link_tick(&k, t, t + lead60, 0, &p);
        }
        check(k.slack_eff_ns == lead60 - PC_LINK_COMMIT_MIN_NS
              && pc_link_deadline(&k) == t + lead60 - PC_LINK_COMMIT_MIN_NS
              && r_dummy == t + lead60 - PC_LINK_COMMIT_MIN_NS - PC_LINK_PHASE_NS,
              "60 Hz display: the deadline its lead less 17 ms before the glass");
        check(t + lead60 - r_dummy == PC_LINK_COMMIT_MIN_NS + PC_LINK_PHASE_NS,
              "60 Hz display: 21 ms from vblank to glass (37 with the link's deadline)");
        /* One odd lead does not move the slack (the reported grid). */
        t += PC_FRAME_NS;
        pc_link_tick(&k, t, t + lead60 + 3 * MS, 0, &p);
        check(k.slack_eff_ns == lead60 - PC_LINK_COMMIT_MIN_NS, "one odd lead: the slack holds (median)");
        /* RECOMP_PRESENT_LINK_SLACK overrides it. */
        pc_link_set_slack(&k, 5 * MS);
        t += PC_FRAME_NS;
        pc_link_tick(&k, t, t + lead60, 0, &p);
        check(k.slack_eff_ns == 5 * MS, "a fixed slack overrides the automatic one");
        pc_link_set_slack(&k, PC_LINK_SLACK_AUTO);
        t += PC_FRAME_NS;
        pc_link_tick(&k, t, t + lead60, 0, &p);
        check(k.slack_eff_ns == lead60 - PC_LINK_COMMIT_MIN_NS, "back to automatic");
    }
    /* The steady "one refresh behind" state (the Fable review): every frame
     * a little after its take-until, so each is shown a refresh late and no
     * refresh goes empty. With the vblank following the link it counts as
     * misses and the phase moves; while the lock settles it does not. */
    {
        pc_link k;
        uint64_t t = 20000 * MS, r = 0, p, prev_r = 0;
        int raised = 0, m, all = 0;
        pc_link_init(&k, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
        for (i = 0; i < PC_GRACE_FRAMES + 20; i++) {
            t += PC_FRAME_NS;
            prev_r = r;
            r = pc_link_tick(&k, t, t + 16666667, 0, &p);
            if (prev_r) {
                pc_link_frame(&k, prev_r, 0, &m);
                all += m;
            }
        }
        check(all == 0, "one behind while the lock settles: no miss");
        for (i = 0; i < 40; i++) {
            t += PC_FRAME_NS;
            prev_r = r;
            r = pc_link_tick(&k, t, t + 16666667, 0, &p);
            raised += pc_link_frame(&k, prev_r, 1, &m) > 0;
            all += m;
        }
        check(all >= 4 && raised >= 1 && k.phase_ns > PC_LINK_PHASE_NS,
              "one behind with the vblank locked: misses, the vblank moves earlier");
        /* A frame of this refresh is no miss, locked or not. */
        t += PC_FRAME_NS;
        r = pc_link_tick(&k, t, t + 16666667, 0, &p);
        pc_link_frame(&k, r + 2 * MS, 1, &m);
        check(!m, "a frame on time with the vblank locked: no miss");
    }
    /* A rate change (60 -> 120): the old steps go, no period until five new
     * ones, then the new one; the grace starts anew. */
    {
        pc_link k;
        uint64_t t = 30000 * MS, p = 1;
        pc_link_init(&k, PC_LINK_PHASE_NS, PC_LINK_GUARD_NS);
        for (i = 0; i < 20; i++) {
            t += PC_FRAME_NS;
            pc_link_tick(&k, t, t + 16666667, 0, &p);
        }
        check(p == PC_FRAME_NS, "link at 60: 16.67 ms");
        pc_link_reset(&k);
        check(k.period_ns == 0 && k.grace == PC_GRACE_FRAMES, "reset: no period, the grace anew");
        /* The first callback after the reset starts the steps anew. */
        for (i = 0; i < 6; i++) {
            t += 8333333;
            pc_link_tick(&k, t, t + 16666667, 0, &p);
            if (i == 4)
                check(p == 0, "reset: no period reported before five new steps");
        }
        check(p == 8333333, "reset: the new period after five steps (not the old median for nine)");
    }
    /* BO-236, the shift seen at 120 Hz: a frame meant for the refresh
     * before is stale for this one; a frame of this refresh is not. */
    check(pc_link_stale(&l, tt - per - 3 * MS) && !pc_link_stale(&l, tt - 4 * MS)
          && !pc_link_stale(&l, tt - per - 1 * MS) && !pc_link_stale(&l, 0),
          "a frame in time for the refresh before is stale, this refresh's is not");
}

int main(void)
{
    link();
    strategy();
    lock();
    handover();
    forced();
    timed();
    statistics();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

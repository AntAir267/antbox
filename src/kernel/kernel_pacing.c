/*
 * kernel_pacing.c - see kernel_pacing.h.
 *
 * The wait is a generation counter under a lock with a condition variable.
 * A waker takes the lock, bumps the generation, drops the lock and
 * broadcasts. A waiter takes the lock and sleeps only while the generation
 * still equals the one it last saw; the condition variable releases the lock
 * atomically. The generation is never read outside the lock, so:
 *  - a wake between the lifted loop's memory read and the wait has already
 *    moved the generation, and the wait returns without sleeping (no lost
 *    wake-up);
 *  - a guest store that precedes a wake in program order (the vblank DPC's
 *    bump of the frame counter, then xbox_IrqlLeaveInterrupt) is visible to
 *    the loop's next MEM32 read after the wait returns, on x86-64 and arm64,
 *    because the lock pairs the waker's release with the waiter's acquire.
 * Only a site whose writer the kernel never signals times out, and then by
 * at most SPIN_TIMEOUT_MS.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "power_auto.h"
#include "pacing_watch.h"
#include "kernel_pause.h"
#include "recomp_env.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <time.h>
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

/* ── The vblank schedule ─────────────────────────────────── */

static uint64_t vblank_due(const XboxVblankSchedule *s, uint64_t n)
{
    /* n * 1e9 overflows 64 bits only after ~9.7 years of vblanks; the
     * 100 ms restart rule resets n long before a host runs that long. */
    return s->t0_ns + n * 1000000000ull / XBOX_VBLANK_HZ;
}

/* ── Locking the vblank to the display ─────────────────────
 *
 * RECOMP_VBLANK_SYNC=display. The emulated vblank is a host timer at
 * exactly 60 Hz; the display refreshes at its own 59.9x-60.0x Hz, from its
 * own clock. Free-running, the two drift through each other: a guest flip
 * that lands just before a display latch one second lands just after it the
 * next, and the screen repeats one frame and skips another -- the periodic
 * double/skip a FIFO swap chain only postpones. Emulators that care pace to
 * the host refresh instead (DuckStation's and PCSX2's "sync to host refresh
 * rate": when the guest rate is within a few percent of the display's, the
 * display is the clock).
 *
 * Here the vblank timer stays the clock -- a stalled or missing display
 * report never stops the title -- but its grid is moved onto the display's:
 * a present path reports each display refresh (xbox_VblankDisplayRefresh:
 * Metal's CVDisplayLink, D3D11's IDXGIOutput::WaitForVBlank, Vulkan's
 * present_wait timestamps), and each vblank's next edge is the display
 * refresh nearest one guest period on. Its period is the display's, or a
 * whole multiple of it (120 Hz: every second refresh), when that is within
 * 1% of 60 Hz; otherwise, or with no report for 250 ms, the timer runs on
 * its own 60.000 Hz grid as before. In phase with the display, a guest
 * flip's present always lands the same distance before the next latch:
 * each frame is shown exactly once.
 *
 * A path that reports only the refreshes a frame was shown at (Vulkan's
 * present_wait) gives no period, and the step between two reports is a
 * whole number of refreshes, not one: at 120 Hz a 60 Hz title's frames
 * are 16.7 ms apart, and 25 ms when one misses its refresh. Averaged, the
 * steps were the title's own frame rate -- 17.5 ms at 57 fps -- which the
 * lock then took for the display's and held the title to (BO-77). So the
 * period is the longest one every step is a whole multiple of: a step off
 * the current period's grid (25 ms on a 16.7 ms period) shows a shorter
 * one (8.3 ms), taken once three steps agree on it. */
#define DISPLAY_STALE_NS 250000000ull
#define DISPLAY_LOCK_TOL 100                /* 1 / 100 of 60 Hz */
static volatile LONGLONG s_disp_ns;        /* last display refresh, host ns */
static volatile LONGLONG s_disp_period;    /* filtered refresh period, ns */
static int s_disp_lock = -1;               /* RECOMP_VBLANK_SYNC=display */
static uint64_t s_disp_cand;               /* a shorter period the steps show */
static int s_disp_cand_n;                  /* steps that showed it */
static int s_disp_off;                     /* steps in a row on no period */
/* Steps off the period's grid in a row, each one guest vblank long: three
 * of them and the display now refreshes once per guest vblank, which is the
 * new period. (Other steps off the grid -- a frame that missed a refresh now
 * and then -- fold into a shorter period with the current one, below.) The
 * Steam Deck starts the game at 90 Hz and switches to the player's 60 once
 * it has a window: the steps went 11.1 then 16.7, folded into the one period
 * that fits both, 5.55 ms, and that stayed: the vblank then snapped to any
 * third of a refresh, a phase picked by chance, and a frame was shown 16 or
 * 31 ms after its present depending on the run. */
static int s_disp_offrun, s_disp_onrun;
static uint64_t s_disp_offsum, s_disp_onsum;
/* Steps off the period's grid: 240 each (two seconds of steps at 120 Hz),
 * less one per step on it, up to a minute's worth. */
static int s_disp_jitter;
/* Set by the cadence diagnosis on a pacing fault (BO-210): the reporting
 * thread drops what the lock learned at its next report, and the vblank
 * keeps its own grid until fresh reports lock it again. */
static volatile int s_disp_relearn;
/* The lock saw the display change its rate: the watchdog's window then
 * decides nothing (pacing_watch.h). */
static volatile int s_disp_rate_changed;
/* The window system's refresh period (xbox_VblankDisplayCycle), and the one
 * the lock last took from it. */
static volatile LONGLONG s_disp_hint;
static uint64_t s_disp_hint_used;
/* The stated refresh is checked against the reports: eight steps
 * in a row off its grid and it is not the display's (a limiter's target, say:
 * 16.67 ms stated while the frames come 11.1 ms apart): the lock learns from
 * the steps instead, until eight in a row are on its grid again. */
#define DISPLAY_HINT_RUN 8
static uint64_t s_disp_hint_seen;
static int s_disp_hint_bad, s_disp_hint_offn, s_disp_hint_onn;
/* The host flip grid a title asks for (xbox_FlipHostRate, below): while it
 * runs the reports are learned from even with the lock off, for the grid's
 * period and phase (xbox_DisplayGrid). */
static volatile LONGLONG s_flip_host_ns;     /* 0: flips on the vblank */

static int display_lock_on(void)
{
    if (s_disp_lock < 0) {
        const char *e = recomp_env(RENV_VBLANK_SYNC);
        s_disp_lock = e && !strcmp(e, "display");
    }
    return s_disp_lock;
}

int xbox_VblankDisplayLockOn(void)
{
    return display_lock_on();
}

/* display.present_timing = auto: a presenter turns the lock on for a display
 * it can follow (and off again), while RECOMP_VBLANK_SYNC says nothing. A
 * given RECOMP_VBLANK_SYNC stays what it says. */
void xbox_VblankDisplayLockAuto(int on)
{
    display_lock_on();
    if (!recomp_env(RENV_VBLANK_SYNC))
        s_disp_lock = on ? 1 : 0;
}

/* The longest period both steps are whole multiples of, within an eighth
 * of it (Euclid's algorithm with a tolerance); 0 when it would be under
 * 2 ms, so the steps disagree by more than a reporting thread's jitter. */
static uint64_t steps_gcd(uint64_t a, uint64_t b)
{
    if (a < b) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    while (b >= 2000000u) {
        uint64_t r = a % b;
        if (r <= b / 8 || b - r <= b / 8)
            return b;
        a = b;
        b = r;
    }
    return 0;
}

void xbox_VblankDisplayCycle(uint64_t period_ns)
{
    if (period_ns && (period_ns < 2000000u || period_ns > 50000000u))
        period_ns = 0;
    s_disp_hint = (LONGLONG)period_ns;
}

int xbox_VblankDisplayCycleOk(void)
{
    return s_disp_hint && !s_disp_hint_bad && (uint64_t)s_disp_hint == s_disp_hint_seen;
}

void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns)
{
    uint64_t prev = (uint64_t)s_disp_ns, cur = (uint64_t)s_disp_period;
    uint64_t hint = (uint64_t)s_disp_hint;

    if ((!display_lock_on() && !s_flip_host_ns) || !when_ns)
        return;
    if (hint != s_disp_hint_seen) {
        s_disp_hint_seen = hint;
        s_disp_hint_bad = s_disp_hint_offn = s_disp_hint_onn = 0;
    }
    if (hint && !period_ns && prev && when_ns > prev && when_ns - prev <= 100000000u) {
        /* Is this step on the stated grid (n refreshes, within an eighth)? */
        uint64_t d = when_ns - prev, n = (d + hint / 2) / hint;
        int on = n && (d > n * hint ? d - n * hint : n * hint - d) <= hint / 8;
        static int said;
        if (!s_disp_hint_bad) {
            if (on)
                s_disp_hint_offn = 0;
            else if (++s_disp_hint_offn >= DISPLAY_HINT_RUN) {
                s_disp_hint_bad = 1;
                s_disp_hint_onn = 0;
                if (said++ < 20)
                    fprintf(stderr, "[PACING] display refresh: %d reports in a row off the window"
                            " system's %.3f ms grid (the last step %.3f ms): learned from the"
                            " reports instead\n", DISPLAY_HINT_RUN, hint / 1e6, d / 1e6);
            }
        } else {
            if (!on)
                s_disp_hint_onn = 0;
            else if (++s_disp_hint_onn >= DISPLAY_HINT_RUN) {
                s_disp_hint_bad = 0;
                s_disp_hint_offn = 0;
                if (said++ < 20)
                    fprintf(stderr, "[PACING] display refresh: the reports are on the window"
                            " system's %.3f ms grid again: taken\n", hint / 1e6);
            }
        }
    }
    if (s_disp_hint_bad)
        hint = 0;
    if (s_disp_relearn || hint != s_disp_hint_used) {
        /* A relearn, or the window system's refresh changed: with the
         * refresh stated, the period is it from this report on. */
        if (!s_disp_relearn && hint && cur) {
            fprintf(stderr, "[PACING] display refresh: the window system says %.3f ms (was"
                    " %.3f): the display changed its rate\n", hint / 1e6, cur / 1e6);
            s_disp_rate_changed = 1;
        }
        s_disp_relearn = 1;
        s_disp_hint_used = hint;
    }
    if (s_disp_relearn) {
        s_disp_relearn = 0;
        s_disp_ns = 0;
        s_disp_period = 0;
        s_disp_cand = 0;
        s_disp_cand_n = s_disp_off = 0;
        s_disp_offrun = s_disp_onrun = 0;
        s_disp_offsum = s_disp_onsum = 0;
        s_disp_jitter = 0;
        prev = cur = 0;
        if (hint) {
            s_disp_period = (LONGLONG)hint;
            cur = hint;
        }
    }
    /* A report may be of a refresh still to come (CVDisplayLink's output
     * time): any point on the display's grid will do. */
    if (period_ns) {
        /* A reported period is exact. */
        if (period_ns >= 2000000u && period_ns <= 50000000u)
            s_disp_period = (LONGLONG)period_ns;
    } else if (hint && prev && when_ns > prev && when_ns - prev <= 100000000u) {
        /* The window system states the refresh: a step on its grid is
         * smoothed in (held within 1 % of the stated one); any other is a
         * missed frame or a late report, never a new period. */
        uint64_t d = when_ns - prev, n = (d + hint / 2) / hint;
        if (n && (d > n * hint ? d - n * hint : n * hint - d) <= hint / 8) {
            uint64_t np = (cur * 15u + d / n) / 16u;
            if (np > hint + hint / 100) np = hint + hint / 100;
            if (np < hint - hint / 100) np = hint - hint / 100;
            s_disp_period = (LONGLONG)np;
        }
    } else if (prev && when_ns > prev && when_ns - prev <= 100000000u) {
        /* Measured: a step of one or more refreshes, jittered by the
         * reporting thread's wake-up. */
        const uint64_t nominal = 1000000000ull / XBOX_VBLANK_HZ;
        uint64_t d = when_ns - prev, g;
        if (!cur) {
            if (d >= 2000000u && d <= 50000000u)
                s_disp_period = (LONGLONG)d;
        } else if ((g = steps_gcd(cur, d)) == 0) {
            /* No period fits both: one stray step, or a period that was
             * wrong from the start. Eight in a row start it over. */
            if (++s_disp_off >= 8 && d >= 2000000u && d <= 50000000u) {
                s_disp_period = (LONGLONG)d;
                s_disp_off = 0;
            }
        } else {
            s_disp_off = 0;
            if (g + cur / 8 >= cur) {
                /* On the period's grid: n periods, smoothed in. */
                uint64_t n = (d + cur / 2) / cur;
                s_disp_period = (LONGLONG)((cur * 15u + d / n) / 16u);
                s_disp_offrun = 0;
                /* Two seconds of steps of one guest vblank each, on a period
                 * of a half or a third of it: the period was folded from two
                 * rates (as above, when the fold came first); the display
                 * refreshes once per guest vblank now. */
                if (n >= 2 && d + nominal / 8 >= nominal && d <= nominal + nominal / 8) {
                    s_disp_onsum += d;
                    if (++s_disp_onrun >= 120) {
                        uint64_t np = s_disp_onsum / (uint64_t)s_disp_onrun;
                        fprintf(stderr, "[PACING] display refresh: steps of %.3f ms for two"
                                " seconds on a %.3f ms period: the display changed its"
                                " rate\n", np / 1e6, cur / 1e6);
                        s_disp_rate_changed = 1;
                        s_disp_period = (LONGLONG)np;
                        s_disp_onrun = 0;
                        s_disp_onsum = 0;
                    }
                } else if (n >= 2 && (d + nominal / 8) / nominal >= 2
                           && (d + nominal / 8) / nominal <= 3
                           && (d + nominal / 8) % nominal <= nominal / 4) {
                    /* Two or three guest vblanks: a frame that missed its
                     * refresh, or the title's 30 fps stretch. Neither for
                     * nor against (BO-210: a session that missed now and
                     * then never had two clean seconds, and the vblank
                     * stayed on a third of a refresh). */
                } else {
                    s_disp_onrun = 0;
                    s_disp_onsum = 0;
                }
            } else if (d + nominal / 8 >= nominal && d <= nominal + nominal / 8
                       && (s_disp_offsum = (s_disp_offrun ? s_disp_offsum : 0) + d,
                           ++s_disp_offrun >= 3)) {
                /* Three guest vblanks in a row off the grid: a new rate. */
                uint64_t np = s_disp_offsum / (uint64_t)s_disp_offrun;
                fprintf(stderr, "[PACING] display refresh: steps of %.3f ms in a row, off the"
                        " %.3f ms period: the display changed its rate\n", np / 1e6, cur / 1e6);
                s_disp_rate_changed = 1;
                s_disp_period = (LONGLONG)np;
                s_disp_offrun = 0;
                s_disp_cand = 0;
                s_disp_cand_n = 0;
            } else if (!(d + nominal / 8 >= nominal && d <= nominal + nominal / 8)) {
                /* Any other step off the grid: a shorter period that fits it
                 * and the current one, taken once three agree. */
                s_disp_offrun = 0;
                if (s_disp_cand && g + s_disp_cand / 8 >= s_disp_cand
                        && g <= s_disp_cand + s_disp_cand / 8)
                    s_disp_cand_n++;
                else {
                    s_disp_cand = g;
                    s_disp_cand_n = 1;
                }
                if (s_disp_cand_n >= 3) {
                    s_disp_period = (LONGLONG)s_disp_cand;
                    s_disp_cand = 0;
                    s_disp_cand_n = 0;
                }
            }
        }
    }
    /* Reports whose steps are not whole periods are not a display's
     * refreshes: a CVDisplayLink on an external 120 Hz display (BO-77)
     * steps 3.7-13.7 ms on its stated 8.333, as it extrapolates its own
     * timeline. Snapped to those, the vblank ran at 63 Hz with 12-21 ms
     * gaps. While any such step is on the books the lock stands down and
     * the timer keeps its own 60 Hz. (A measured period is fitted to the
     * steps above; this holds a stated one to them.) */
    cur = (uint64_t)s_disp_period;
    if (period_ns && cur && prev && when_ns > prev && when_ns - prev <= 100000000u) {
        uint64_t r = (when_ns - prev) % cur;
        if (r > cur / 8 && cur - r > cur / 8) {
            static int said;
            if (!said++)
                fprintf(stderr, "[PACING] display refresh reports are irregular (a"
                        " %.2f ms step on a %.3f ms period): the vblank keeps its"
                        " own 60 Hz timer while they are\n",
                        (when_ns - prev) / 1e6, cur / 1e6);
            if (s_disp_jitter < 7200)
                s_disp_jitter += 240;
        } else if (s_disp_jitter) {
            s_disp_jitter--;
        }
    }
    /* The grid's anchor. A measured report is the refresh plus the reporting
     * thread's wake-up, never before it: on the Steam Deck present_wait's
     * waiter woke up to a millisecond late now and then, the vblank's next
     * edge followed it, and the title's frame took 17.7 ms and the next 15.7
     * (a yellow bar). So a report later than the grid predicts moves the
     * anchor an eighth of the way (the display's own drift is far slower),
     * an earlier one sets it. A stated period (a display link's own times)
     * is taken as it comes. */
    cur = (uint64_t)s_disp_period;
    if (!period_ns && cur && prev && when_ns > prev && when_ns - prev <= 100000000u) {
        uint64_t k = (when_ns - prev + cur / 2) / cur, pred = prev + k * cur;
        if (k && when_ns > pred && when_ns - pred < cur / 4) {
            s_disp_ns = (LONGLONG)(pred + (when_ns - pred) / 8);
            return;
        }
    }
    s_disp_ns = (LONGLONG)when_ns;
}

void xbox_VblankDisplayRelearn(void)
{
    s_disp_relearn = 1;
}

int xbox_VblankDisplayRateChanged(void)
{
    if (!s_disp_rate_changed)
        return 0;
    s_disp_rate_changed = 0;
    return 1;
}

/* The display grid the vblank follows now: a refresh time and the guest
 * period on it. 0 when not locked. */
static int display_grid(uint64_t now_ns, uint64_t *ref_ns, uint64_t *period_ns)
{
    const uint64_t nominal = 1000000000ull / XBOX_VBLANK_HZ;
    uint64_t ref, pd, m, p;

    if (!display_lock_on())
        return 0;
    ref = (uint64_t)s_disp_ns;
    pd = (uint64_t)s_disp_period;
    if (!ref || !pd || (now_ns > ref && now_ns - ref > DISPLAY_STALE_NS)
            || s_disp_jitter || s_disp_relearn)
        return 0;
    m = (nominal + pd / 2) / pd;           /* refreshes per guest vblank */
    if (!m)
        return 0;
    p = m * pd;
    if (p < nominal - nominal / DISPLAY_LOCK_TOL
            || p > nominal + nominal / DISPLAY_LOCK_TOL)
        return 0;                          /* not within 1% of 60 Hz */
    *ref_ns = ref;
    *period_ns = p;
    return 1;
}

int xbox_VblankSchedule(XboxVblankSchedule *s, uint64_t now_ns,
                        uint64_t *next_due_ns, uint64_t *restarted_late_ns)
{
    uint64_t due;

    *restarted_late_ns = 0;
    if (!s->started) {
        s->started = 1;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    due = vblank_due(s, s->count);
    if (now_ns < due) {
        *next_due_ns = due;
        return 0;
    }
    if (now_ns - due > XBOX_VBLANK_RESTART_NS) {
        *restarted_late_ns = now_ns - due;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    s->count++;
    *next_due_ns = vblank_due(s, s->count);
    {
        /* Locked: the next edge is one guest period (on the display's
         * clock) after the edge just raised, moved to the display refresh
         * nearest it -- from the edge, not from now, so at 120 Hz the same
         * refresh of each pair stays the guest's. The grid is re-anchored
         * there, so the free-running schedule takes over if the reports
         * stop. */
        uint64_t ref, p, pd, target, next;
        if (display_grid(now_ns, &ref, &p)) {
            pd = (uint64_t)s_disp_period;
            target = due + p;
            if (target >= ref)
                next = ref + (target - ref + pd / 2) / pd * pd;
            else
                next = ref - (ref - target + pd / 2) / pd * pd;
            while (next <= now_ns)
                next += p;
            s->t0_ns = next - s->count * 1000000000ull / XBOX_VBLANK_HZ;
            *next_due_ns = next;
        }
    }
    return 1;
}

/* ── Mode and the wait ───────────────────────────────────── */

#define SPIN_TIMEOUT_MS 1
/* A thread that comes back to a wait sooner than this after its last one is
 * still in the same loop (a pass is a few guest instructions; the game's
 * frame work between two loops takes milliseconds). Longer, and its last
 * wait was the loop's final pass: the outcome that let the loop exit. */
#define SPIN_EXIT_GAP_NS 500000ull
/* The waiting thread's CPU clock is read on one call in this many. */
#define SPIN_CPU_SAMPLE 64

volatile int g_xbox_spin_sleep;

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static uint32_t s_gen;                       /* under s_lock only */
static XBOX_THREAD_LOCAL uint32_t t_seen_gen;

/* Vblank waits (xbox_VblankWaitUntil): their own lock and condition
 * variable, so the vblank's broadcast wakes only the threads waiting for
 * one. */
static CRITICAL_SECTION s_vbl_lock;
static CONDITION_VARIABLE s_vbl_cv;

static BOOL CALLBACK pacing_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    InitializeCriticalSection(&s_vbl_lock);
    InitializeConditionVariable(&s_vbl_cv);
    return TRUE;
}

static void pacing_once(void)
{
    InitOnceExecuteOnce(&s_once, pacing_init, NULL, NULL);
}

void xbox_PacingSetMode(int mode)
{
    pacing_once();
    g_xbox_spin_sleep = (mode == XBOX_PACING_SLEEP);
}

int xbox_PacingMode(void)
{
    return g_xbox_spin_sleep ? XBOX_PACING_SLEEP : XBOX_PACING_SPIN;
}

/* Per-site counters: an open-addressed table keyed by the loop's VA. A
 * title has a handful of lowered sites; past the table's size they share
 * the last slot, reported as "other". */
#define SITE_SLOTS 31
typedef struct {
    volatile LONG va;          /* 0 = free */
    volatile LONG waits, immediate, wakes, timeouts, dispatch;
    /* How loops left: their last pass returned on a signal (or at once),
     * or on the 1 ms timeout. Timeouts mid-loop are normal (a vblank wait
     * spans up to 16 of them); a loop that keeps leaving on a timeout has a
     * writer the kernel never signals. */
    volatile LONG exit_wake, exit_timeout;
    volatile LONG inwait_us;   /* sampled CPU inside the wait, x SAMPLE */
    volatile LONG calls;       /* for the sampling stride */
    /* The waiting thread's CPU clock against wall time, first and last
     * sample in the window: its whole CPU share, game work included. */
    volatile LONGLONG cpu0_ns, wall0_ns, cpu1_ns, wall1_ns;
} SpinSite;
static SpinSite s_sites[SITE_SLOTS + 1];

/* The calling thread's last wait, for the exit count (mode sleep only). */
enum { LAST_NONE, LAST_WAKE, LAST_TIMEOUT };
static XBOX_THREAD_LOCAL SpinSite *t_last_site;
static XBOX_THREAD_LOCAL int t_last_outcome;
static XBOX_THREAD_LOCAL uint64_t t_last_ret_ns;

static SpinSite *site_for(uint32_t va)
{
    unsigned h = (va * 2654435761u) % SITE_SLOTS, i;

    for (i = 0; i < SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[(h + i) % SITE_SLOTS];
        LONG cur = s->va;
        if (cur == (LONG)va)
            return s;
        if (!cur && InterlockedCompareExchange(&s->va, (LONG)va, 0) == 0)
            return s;
        if (s->va == (LONG)va)
            return s;
    }
    return &s_sites[SITE_SLOTS];
}

void xbox_SpinWait(uint32_t va)
{
    SpinSite *site;
    int sample, woke = 0, immediate = 0;
    uint64_t c0 = 0, now;

    /* A lowered loop's pass is between two guest instructions: a holder
     * spinning here at DISPATCH takes a posted interrupt, as a CPU would.
     * At "spin" pacing the macro calls only for that. */
    xbox_IrqSafePoint();
#if !defined(_WIN32)
    /* And a hand-over point for guest_cpus=token (kernel_thread.c): a loop
     * still waiting lets a thread that wants the guest CPU run now rather
     * than at the holder's next preemption. */
    guest_turn_yield();
#endif
    if (!g_xbox_spin_sleep)
        return;
    site = site_for(va);
    now = xbox_HostNowNs();

    /* The previous wait on this thread ended its loop if the thread was
     * away long enough: count how that loop left. */
    if (t_last_site && t_last_outcome != LAST_NONE
        && now - t_last_ret_ns > SPIN_EXIT_GAP_NS)
        InterlockedIncrement(t_last_outcome == LAST_WAKE
                             ? &t_last_site->exit_wake
                             : &t_last_site->exit_timeout);
    t_last_site = site;
    t_last_outcome = LAST_NONE;
    t_last_ret_ns = now;

    InterlockedIncrement(&site->waits);
    /* At DISPATCH or with the gate held the DPC that ends this loop cannot
     * run until the thread leaves; sleeping would only add latency. */
    if (xbox_IrqlThisThreadBlocksDpcs()) {
        InterlockedIncrement(&site->dispatch);
        return;
    }
    sample = (InterlockedIncrement(&site->calls) % SPIN_CPU_SAMPLE) == 0;
    if (sample)
        c0 = xbox_ThreadCpuNs();

    pacing_once();
    EnterCriticalSection(&s_lock);
    if (s_gen != t_seen_gen) {
        immediate = 1;
    } else {
        SleepConditionVariableCS(&s_cv, &s_lock, SPIN_TIMEOUT_MS);
        woke = (s_gen != t_seen_gen);
    }
    t_seen_gen = s_gen;
    LeaveCriticalSection(&s_lock);

    if (immediate)
        InterlockedIncrement(&site->immediate);
    else if (woke)
        InterlockedIncrement(&site->wakes);
    else
        InterlockedIncrement(&site->timeouts);
    t_last_outcome = immediate || woke ? LAST_WAKE : LAST_TIMEOUT;
    t_last_ret_ns = xbox_HostNowNs();

    if (sample) {
        uint64_t c1 = xbox_ThreadCpuNs(), w = xbox_HostNowNs();
        InterlockedExchangeAdd(&site->inwait_us,
                               (LONG)((c1 - c0) / 1000u * SPIN_CPU_SAMPLE));
        if (!site->wall0_ns) {
            site->cpu0_ns = (LONGLONG)c1;
            site->wall0_ns = (LONGLONG)w;
        }
        site->cpu1_ns = (LONGLONG)c1;
        site->wall1_ns = (LONGLONG)w;
    }
}

/* ── Waiting for a vblank ────────────────────────────────── */

/* present.spin_lead: the last stretch before the vblank that a held flip
 * spins through instead of sleeping (mode sleep only).
 *
 * The Steam Deck's APU picks its CPU clock itself (amd-pstate-epp: CPPC
 * autonomous, no governor; a utilization clamp asks nothing of it), from
 * how busy its cores are. With every wait asleep the process keeps all
 * eight threads near 2.3 GHz; one core kept busy lifts them all to
 * 3.2-3.4. The executor's frame is the same cycles and instructions either
 * way (perf stat, BO-78), so at sleep pacing it takes a third longer and
 * the heavy frames miss their vblank. Mode spin kept the clock up by
 * accident: the title's fence wait spun beside the executor. The spin here
 * is on the executor's own thread, right before the frame it scans next,
 * and its length is the trade between clock and watts. */
#define SPIN_LATE_NS 2000000ull   /* past a vblank due, give up the spin */
static uint64_t s_spin_lead_ns;
/* When the vblank after the last one raised is due (the timer thread). */
static volatile LONGLONG s_vbl_next_ns;
/* The edge of the last vblank raised, and the vblank of the frame the
 * executor is handing to the host (xbox_PacingFrameVblank). */
static volatile LONGLONG s_vbl_last_ns, s_frame_vbl_ns;

/* Power mode Auto (power_auto.h): the lead picked from the holds. The
 * controller runs on the executor's thread, in xbox_PacingHoldSample, the
 * only writer of s_spin_lead_ns while it is on; the overlay reads the
 * level and the reason. */
static volatile int s_auto_on, s_auto_reset;
static XboxPowerAuto s_auto;
static volatile int s_auto_level = XBOX_POWER_AUTO_START;
static char s_auto_why[96];
/* When the timer thread raised the last vblank (host clock), and what the
 * last vblank wait measured of its own lateness (-1: not measured). */
static volatile LONGLONG s_vbl_raised_ns;
static int64_t s_wait_wake_ns = -1;
/* The executor's last hold: how long after the edge of the vblank it waited
 * for it was let go (-1: not measured), for the cadence diagnosis's budget
 * (pacing_diag.h, BO-211). */
static int64_t s_hold_wake_ns = -1;
/* The executor's last hold, for the cadence diagnosis (BO-210): it found
 * the published edge already more than SPIN_LATE_NS past (the timer thread
 * is behind), or it spun longer than the lead allows. */
static int s_hold_stale, s_hold_overspin;

void xbox_PacingSetSpinLead(uint64_t ns)
{
    s_auto_on = 0;
    s_spin_lead_ns = ns;
}

void xbox_PacingSetSpinLeadAuto(void)
{
    if (!s_auto_on) {
        s_auto_reset = 1;      /* the executor starts it afresh */
        s_spin_lead_ns = xbox_PowerAutoLeadNs(XBOX_POWER_AUTO_START);
        s_auto_level = XBOX_POWER_AUTO_START;
        snprintf(s_auto_why, sizeof s_auto_why, "start (as Balanced)");
        s_auto_on = 1;
    }
}

int xbox_PacingAutoState(double *lead_ms, char *why, size_t n)
{
    if (lead_ms)
        *lead_ms = (double)xbox_PowerAutoLeadNs(s_auto_level) / 1e6;
    if (why && n)
        snprintf(why, n, "%s", s_auto_why);
    return s_auto_on;
}

void xbox_PacingHoldSample(uint32_t interval, uint64_t arrive_ns, uint64_t edge_ns)
{
    XboxPowerAutoSample smp;

    if (!s_auto_on || !g_xbox_spin_sleep || xbox_TimeWarpOn())
        return;
    if (s_auto_reset) {
        s_auto_reset = 0;
        xbox_PowerAutoInit(&s_auto);
    }
    smp.now_ns = xbox_HostNowNs();
    smp.interval = edge_ns ? interval : 0;
    smp.slack_ns = (int64_t)edge_ns - (int64_t)arrive_ns;
    smp.wake_ns = s_wait_wake_ns;
    s_wait_wake_ns = -1;
    if (xbox_PowerAutoFeed(&s_auto, &smp)) {
        int lv = xbox_PowerAutoLevel(&s_auto);
        s_spin_lead_ns = xbox_PowerAutoLeadNs(lv);
        snprintf(s_auto_why, sizeof s_auto_why, "%s", s_auto.why);
        s_auto_level = lv;
        fprintf(stderr, "[PACING] power auto: spin lead %llu ms (%s)\n",
                (unsigned long long)(s_spin_lead_ns / 1000000u), s_auto.why);
    }
}

/* The cadence diagnosis (pacing_diag.h, BO-210) and the watchdog on it
 * (pacing_watch.h, BO-211), on the executor at each flip, in every pacing
 * mode: running totals of the executor's hold and scan time (ns, any one
 * clock), and whether the swap's vblank had passed. Says in one line,
 * always, when the frame rate drops and why; two pacing-fault windows in a
 * row reset the frame-queue keeper and the display lock together (with a
 * holdoff), as do a settings change and a wake gap. */
static XboxPacingWatch s_watch;
static int s_diag_off = -1;
#define RESET_HOOKS 4
static void (*s_reset_hook[RESET_HOOKS])(void);
static volatile LONG s_reset_hooks;
/* Asked from another thread, done at the executor's next flip. */
static volatile LONG s_reset_req;
static char s_reset_why[96];
static volatile LONGLONG s_wake_gap_ns;
static volatile int s_visible = 1;

void xbox_PacingResetHook(void (*fn)(void))
{
    int n = s_reset_hooks;
    if (!fn || n >= RESET_HOOKS)
        return;
    s_reset_hook[n] = fn;
    /* A full barrier: the pointer is there before the count says so. */
    InterlockedExchange(&s_reset_hooks, n + 1);
}

void xbox_PacingWatchReset(const char *why)
{
    snprintf(s_reset_why, sizeof s_reset_why, "%s", why ? why : "asked");
    InterlockedExchange(&s_reset_req, 1);
}

void xbox_PacingSetVisible(int visible)
{
    s_visible = visible != 0;
}

/* The timer thread, after a gap of a second or more (host_time.h). */
static void pacing_woke(uint64_t gap_ns)
{
    s_wake_gap_ns = (LONGLONG)(gap_ns ? gap_ns : 1);
}

static void pacing_reset_all(void)
{
    int i, n = s_reset_hooks;
    if (display_lock_on())
        xbox_VblankDisplayRelearn();
    for (i = 0; i < n; i++)
        if (s_reset_hook[i])
            s_reset_hook[i]();
}

void xbox_PacingFlipCadence(uint32_t interval, int late, uint64_t hold_total,
                            uint64_t scan_total)
{
    static uint64_t p_hold, p_scan, p_now;
    static int64_t p_wake = -1;
    uint64_t now = xbox_HostNowNs(), hold, scan, frame, gap;
    char line[400];
    int k, stale = s_hold_stale, over = s_hold_overspin;
    /* This frame began at the previous hold's release: its lateness. */
    int64_t wake = p_wake;

    p_wake = s_hold_wake_ns;
    s_hold_wake_ns = -1;
    s_hold_stale = s_hold_overspin = 0;
    if (s_diag_off < 0) {
        const char *e = recomp_env(RENV_CADENCE_DIAG);
        s_diag_off = e && *e == '0';
        pwatch_init(&s_watch);
        xbox_HostWakeListener("the pacing state", pacing_woke);
    }
    /* Forced resets first: a wake gap, a settings change. */
    if ((gap = (uint64_t)s_wake_gap_ns) != 0) {
        s_wake_gap_ns = 0;
        InterlockedExchange(&s_reset_req, 0);
        pwatch_force(&s_watch, now);
        pacing_reset_all();
        XR_NOTICE("[PACING] after a %.0f s gap: the frame-queue keeper and the display lock"
                  " start over\n", gap / 1e9);
    } else if (s_reset_req && InterlockedExchange(&s_reset_req, 0)) {
        pwatch_force(&s_watch, now);
        pacing_reset_all();
        XR_NOTICE("[PACING] %s: the frame-queue keeper and the display lock start over\n",
                  s_reset_why);
    }
    if (s_diag_off || xbox_TimeWarpOn())
        return;
    if (xbox_HostPaused())
        pwatch_void(&s_watch, PWATCH_V_PAUSED);
    if (!s_visible)
        pwatch_void(&s_watch, PWATCH_V_HIDDEN);
    if (xbox_VblankDisplayRateChanged())
        pwatch_void(&s_watch, PWATCH_V_RATE);
    /* Flips on a host grid at the display's rate (xbox_FlipHostRate): not
     * the vblank's cadence the watch knows; it decides nothing. */
    if (s_flip_host_ns)
        pwatch_void(&s_watch, PWATCH_V_RATE);
    if (!p_now) {
        p_now = now;
        p_hold = hold_total;
        p_scan = scan_total;
        return;
    }
    hold = hold_total - p_hold;
    scan = scan_total - p_scan;
    frame = now - p_now;
    p_now = now;
    p_hold = hold_total;
    p_scan = scan_total;
    if (hold > scan)
        hold = scan;
    k = pwatch_flip(&s_watch, now, interval, late, scan - hold,
                    frame > scan ? frame - scan : 0, hold, wake, stale, over, line, sizeof line);
    if (k & PWATCH_LINE)
        XR_NOTICE("[PACING] %s\n", line);
    if (k & PWATCH_RESET) {
        pacing_reset_all();
        XR_NOTICE("[PACING] %s\n", s_watch.why);
    }
}

void xbox_VblankNextDue(uint64_t ns)
{
    /* The edge raised now is the one that was due next. */
    s_vbl_last_ns = s_vbl_next_ns;
    s_vbl_next_ns = (LONGLONG)ns;
    s_vbl_raised_ns = (LONGLONG)xbox_HostNowNs();
}

uint64_t xbox_VblankLastDue(void)
{
    return (uint64_t)s_vbl_last_ns;
}

uint64_t xbox_VblankEdgeNs(uint32_t count)
{
    uint64_t last = (uint64_t)s_vbl_last_ns, next = (uint64_t)s_vbl_next_ns;
    uint64_t p = next > last && last ? next - last : 1000000000ull / XBOX_VBLANK_HZ;
    int32_t ahead = (int32_t)(count - xbox_VblankCount());

    if (!next)
        return 0;
    if (ahead >= 1)
        return next + (uint64_t)(ahead - 1) * p;
    if (!last)
        return 0;
    return last - (uint64_t)(-ahead) * p;
}

void xbox_PacingFrameVblank(uint64_t ns)
{
    s_frame_vbl_ns = (LONGLONG)ns;
}

uint64_t xbox_PacingFrameVblankNs(void)
{
    return (uint64_t)s_frame_vbl_ns;
}

void xbox_CpuRelax(void)
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/* The timer thread, right after it counted a vblank. Taking the lock orders
 * the broadcast after a waiter's check of the count: a waiter either saw the
 * new count or is already asleep on the variable (no lost wake-up). */
void xbox_VblankSignal(void)
{
    pacing_once();
    EnterCriticalSection(&s_vbl_lock);
    LeaveCriticalSection(&s_vbl_lock);
    WakeAllConditionVariable(&s_vbl_cv);
}

#if defined(__ANDROID__) && defined(__aarch64__)
/* The spin's clock on Android. clock_gettime goes into the kernel on the
 * Retroid Pocket 5 (Snapdragon 865, Linux 4.19: the vDSO falls back to the
 * system call), and the spin to the vblank's edge was 44 % of the executor
 * thread's cycles, most of it that call (BO-85). The spin needs no time of
 * day, only "has the edge's deadline passed": it reads the CPU's virtual
 * counter (cntvct_el0, ticking at cntfrq_el0) against a limit worked out
 * once from the host clock. Not used when the counter's frequency reads as
 * nonsense or a read of it costs as much as the call it replaces (a kernel
 * that traps and emulates it). */
static inline uint64_t cnt_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static uint64_t cnt_freq(void)
{
    static uint64_t freq;          /* 0: not looked at; 1: not usable */
    if (!freq) {
        uint64_t f, t0, t1, c0, c1 = 0;
        int i;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        t0 = xbox_HostNowNs();
        c0 = cnt_read();
        for (i = 0; i < 256; i++)
            c1 = cnt_read();
        t1 = xbox_HostNowNs();
        freq = (f >= 1000000u && f <= 10000000000ull && c1 > c0 && (t1 - t0) / 256u < 200u)
             ? f : 1;
        fprintf(stderr, "[PACING] vblank spin clock: %s (cntfrq %llu Hz, %llu ns a read)\n",
                freq > 1 ? "the CPU's counter" : "clock_gettime", (unsigned long long)f,
                (unsigned long long)((t1 - t0) / 256u));
    }
    return freq > 1 ? freq : 0;
}
#endif

int xbox_VblankWaitUntil(uint32_t target, uint32_t timeout_ms)
{
    uint64_t enter = xbox_HostNowNs();
    uint64_t deadline = enter + (uint64_t)timeout_ms * 1000000u;
    int ok = 1, spun = 0;

    pacing_once();
    EnterCriticalSection(&s_vbl_lock);
    while ((int32_t)(xbox_VblankCount() - target) < 0) {
        uint64_t now = xbox_HostNowNs();
        DWORD ms;
        if (now >= deadline) {
            ok = 0;
            break;
        }
        /* A run that does not wait for the clock (host_time.h): move the
         * guest clock to the next vblank's edge, which wakes the timer
         * thread to raise it, and wait for the count. One vblank at a time.
         * (One raised while the title's handler for the last is still
         * queued behind the one-CPU gate is declined by its ISR, as a
         * vblank that comes too soon is in any run: 2 to 20 in a hundred at
         * these rates, none in real time. Burnout 3 steps by its flips and
         * plays the same; waiting for the handler here changed nothing.) */
        if (xbox_TimeWarpOn()) {
            uint64_t due = (uint64_t)s_vbl_next_ns;
            LeaveCriticalSection(&s_vbl_lock);
            xbox_TimeWarpTo(due);
            EnterCriticalSection(&s_vbl_lock);
            if ((int32_t)(xbox_VblankCount() - target) < 0) {
                ms = (DWORD)((deadline - now + 999999u) / 1000000u);
                SleepConditionVariableCS(&s_vbl_cv, &s_vbl_lock, ms);
            }
            continue;
        }
        /* One vblank to go, and its edge is known: sleep to the lead, then
         * spin to the edge. An edge already well past (the timer thread is
         * behind, or has not published one) falls back to the condition
         * variable. */
        if (s_spin_lead_ns && xbox_VblankCount() + 1 == target) {
            uint64_t due = (uint64_t)s_vbl_next_ns, lead = s_spin_lead_ns;
            if (due + SPIN_LATE_NS > now) {
                LeaveCriticalSection(&s_vbl_lock);
                if (due > now + lead) {
                    uint64_t d = due - lead - now;
                    xbox_HostSleepNs(d < deadline - now ? d : deadline - now);
                } else if (!spun) {
                    /* One spin per wait, at most the lead plus SPIN_LATE_NS
                     * from where it starts, whatever `due` says; the rest of
                     * the wait sleeps on the condition variable (BO-210). */
                    uint64_t lim = due + SPIN_LATE_NS, s0 = now, cap = now + lead + SPIN_LATE_NS;
                    if (lim > cap)
                        lim = cap;
                    spun = 1;
#if defined(__ANDROID__) && defined(__aarch64__)
                    uint64_t f = cnt_freq();
                    if (f) {
                        /* The same deadline, in ticks. */
                        uint64_t clim = cnt_read() + (lim - now) * f / 1000000000ull;
                        while ((int32_t)(xbox_VblankCount() - target) < 0 && cnt_read() < clim)
                            xbox_CpuRelax();
                    } else
#endif
                    while ((int32_t)(xbox_VblankCount() - target) < 0
                           && xbox_HostNowNs() < lim)
                        xbox_CpuRelax();
                    if (xbox_HostNowNs() - s0 > lead + SPIN_LATE_NS + 1000000u)
                        s_hold_overspin = 1;
                }
                EnterCriticalSection(&s_vbl_lock);
                if (spun && (int32_t)(xbox_VblankCount() - target) < 0
                        && xbox_HostNowNs() < deadline) {
                    now = xbox_HostNowNs();
                    ms = (DWORD)((deadline - now + 999999u) / 1000000u);
                    SleepConditionVariableCS(&s_vbl_cv, &s_vbl_lock, ms);
                }
                continue;
            }
            s_hold_stale = 1;    /* the edge it waits for is long past */
        }
        ms = (DWORD)((deadline - now + 999999u) / 1000000u);
        SleepConditionVariableCS(&s_vbl_cv, &s_vbl_lock, ms);
    }
    LeaveCriticalSection(&s_vbl_lock);
    /* The wait's own lateness, for power mode Auto: from the raise of the
     * vblank it waited for to the return, when it was there before the
     * raise and no later vblank came meanwhile. */
    s_wait_wake_ns = -1;
    s_hold_wake_ns = -1;
    if (ok && !xbox_TimeWarpOn()) {
        uint64_t back = xbox_HostNowNs(), edge = xbox_VblankEdgeNs(target);
        if (s_auto_on && xbox_VblankCount() == target) {
            uint64_t r = (uint64_t)s_vbl_raised_ns;
            if (r > enter && back >= r)
                s_wait_wake_ns = (int64_t)(back - r);
        }
        /* From the edge (the grid's time, not the raise): the timer
         * thread's lateness counts too. */
        if (edge && back >= edge && back - edge < 50000000u)
            s_hold_wake_ns = (int64_t)(back - edge);
    }
    return ok;
}

void xbox_SpinWake(void)
{
    if (!g_xbox_spin_sleep)
        return;
    pacing_once();
    EnterCriticalSection(&s_lock);
    s_gen++;
    LeaveCriticalSection(&s_lock);
    WakeAllConditionVariable(&s_cv);
}

int xbox_SpinSiteStats(uint32_t va, XboxSpinSiteStats *out)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[i];
        if (s->va != (LONG)va || !s->waits)
            continue;
        out->va = va;
        out->waits = s->waits;
        out->immediate = s->immediate;
        out->wakes = s->wakes;
        out->timeouts = s->timeouts;
        out->dispatch = s->dispatch;
        out->exit_wake = s->exit_wake;
        out->exit_timeout = s->exit_timeout;
        return 1;
    }
    return 0;
}

static void site_reset(SpinSite *s)
{
    InterlockedExchange(&s->waits, 0);
    InterlockedExchange(&s->immediate, 0);
    InterlockedExchange(&s->wakes, 0);
    InterlockedExchange(&s->timeouts, 0);
    InterlockedExchange(&s->dispatch, 0);
    InterlockedExchange(&s->exit_wake, 0);
    InterlockedExchange(&s->exit_timeout, 0);
    InterlockedExchange(&s->inwait_us, 0);
    /* Not atomic with a waiter's sample: a sample landing between these
     * stores and the next window's first one can pair an old cpu0 with a
     * new wall0, or count a wait in the window it did not finish in. Only
     * the trace's thread CPU share for one window can be off; nothing
     * reads these counters for pacing itself. */
    s->wall0_ns = 0;
}

void xbox_SpinStatsReset(void)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++)
        site_reset(&s_sites[i]);
}

/* ── CPU clocks ──────────────────────────────────────────── */

uint64_t xbox_ThreadCpuNs(void)
{
#ifdef _WIN32
    /* Wine advances these in system-tick steps (15.6 ms): unbiased over a
     * trace window, coarse within one. */
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t xbox_ProcessCpuNs(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000ull
           + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000ull;
#endif
}

/* ── RECOMP_TRACE=pacing ─────────────────────────────────── */

#define TRACE_WINDOW 600

static int s_trace = -1;          /* 0 off, 1 summaries, 2 also every flip */
/* The vblank gap range since the last summary, in µs (the timer thread
 * writes, the walker reads and resets). */
static volatile LONG s_gap_min_us = 0x7FFFFFFF, s_gap_max_us;
static uint64_t s_last_vblank_ns;   /* timer thread only */
/* RECOMP_VBLANK_SYNC=display: each vblank's distance after the display
 * refresh before it, in us, over the window. */
static volatile LONG s_ph_min_us = 0x7FFFFFFF, s_ph_max_us = -1;

static int trace_mode(void)
{
    if (s_trace < 0) {
        const char *v = recomp_env(RENV_PACING_TRACE);
        s_trace = !v || !*v || *v == '0' ? 0 : (strcmp(v, "all") == 0 ? 2 : 1);
    }
    return s_trace;
}

void xbox_PacingVblank(uint64_t now_ns)
{
    if (!trace_mode())
        return;
    if (s_last_vblank_ns) {
        LONG g = (LONG)((now_ns - s_last_vblank_ns) / 1000u), cur;
        while ((cur = s_gap_min_us) > g &&
               InterlockedCompareExchange(&s_gap_min_us, g, cur) != cur)
            ;
        while ((cur = s_gap_max_us) < g &&
               InterlockedCompareExchange(&s_gap_max_us, g, cur) != cur)
            ;
    }
    s_last_vblank_ns = now_ns;
    {
        uint64_t ref, p;
        if (display_grid(now_ns, &ref, &p)) {
            uint64_t pd = (uint64_t)s_disp_period;
            LONG ph = (LONG)((now_ns >= ref ? (now_ns - ref) % pd
                                           : pd - (ref - now_ns) % pd) / 1000u);
            if (ph < s_ph_min_us) s_ph_min_us = ph;
            if (ph > s_ph_max_us) s_ph_max_us = ph;
        }
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static double pct(const uint32_t *sorted, int n, double p)
{
    int i = (int)(p * (n - 1) + 0.5);
    return sorted[i] / 1000.0;
}

static void site_line(const SpinSite *s, double window_s, int other)
{
    double inwait = window_s > 0 ? s->inwait_us / 1e6 / window_s * 100.0 : 0;
    double thread = 0;
    if (s->wall1_ns > s->wall0_ns)
        thread = (double)(s->cpu1_ns - s->cpu0_ns)
                 / (double)(s->wall1_ns - s->wall0_ns) * 100.0;
    if (other)
        fprintf(stderr, "[PACING]   site other:");
    else
        fprintf(stderr, "[PACING]   site 0x%08X:", (uint32_t)s->va);
    fprintf(stderr, " waits %ld immediate %ld wakes %ld timeouts %ld"
            " dispatch %ld; exits wake %ld timeout %ld;"
            " cpu in-wait %.1f%% thread %.0f%%\n",
            (long)s->waits, (long)s->immediate, (long)s->wakes,
            (long)s->timeouts, (long)s->dispatch, (long)s->exit_wake,
            (long)s->exit_timeout, inwait, thread);
}

/* A host present (the backend's swap chain Present returned, or Metal's
 * presented handler with the time the drawable reached the display): under
 * RECOMP_TRACE=pacing=all one line each, so the present-to-present intervals
 * -- what the display was handed -- can be read beside the guest's flips
 * (tools/deck/stats.py). when_ns 0: now. */
void xbox_PacingPresent(uint64_t when_ns)
{
    static volatile LONG n;
    if (trace_mode() != 2)
        return;
    fprintf(stderr, "[PACING] present %ld t_us %llu\n", (long)InterlockedIncrement(&n),
            (unsigned long long)((when_ns ? when_ns : xbox_HostNowNs()) / 1000u));
}

void xbox_PacingFlip(uint32_t flip, uint32_t batches)
{
    /* Walker thread only. */
    static uint64_t last_ns, win_ns, win_cpu;
    static uint32_t iv_us[TRACE_WINDOW];
    static int n;
    uint64_t now;
    int mode = trace_mode();

    if (!mode)
        return;
    now = xbox_HostNowNs();
    if (mode == 2)
        fprintf(stderr, "[PACING] flip %u t_us %llu batches %u\n", flip,
                (unsigned long long)(now / 1000u), batches);
    if (!win_ns) {
        win_ns = now;
        win_cpu = xbox_ProcessCpuNs();
    }
    if (last_ns)
        iv_us[n++] = (uint32_t)((now - last_ns) / 1000u);
    last_ns = now;
    if (n < TRACE_WINDOW)
        return;

    {
        uint32_t s[TRACE_WINDOW];
        uint64_t cpu = xbox_ProcessCpuNs();
        double window_s = (double)(now - win_ns) / 1e9;
        LONG gmin = InterlockedExchange(&s_gap_min_us, 0x7FFFFFFF);
        LONG gmax = InterlockedExchange(&s_gap_max_us, 0);
        unsigned i;

        memcpy(s, iv_us, sizeof s);
        qsort(s, TRACE_WINDOW, sizeof s[0], cmp_u32);
        fprintf(stderr, "[PACING] flips %d: interval p5 %.1f p50 %.1f p95 %.1f"
                " max %.1f ms; ", TRACE_WINDOW, pct(s, TRACE_WINDOW, 0.05),
                pct(s, TRACE_WINDOW, 0.50), pct(s, TRACE_WINDOW, 0.95),
                s[TRACE_WINDOW - 1] / 1000.0);
        if (gmax)
            fprintf(stderr, "vblank gap %.1f-%.1f ms; ", gmin / 1000.0,
                    gmax / 1000.0);
        else
            fprintf(stderr, "vblank gap -; ");
        {
            LONG pmin = InterlockedExchange(&s_ph_min_us, 0x7FFFFFFF);
            LONG pmax = InterlockedExchange(&s_ph_max_us, -1);
            if (pmax >= 0)
                fprintf(stderr, "display %.3f ms, vblank %.2f-%.2f ms after its"
                        " refresh; ", (double)s_disp_period / 1e6, pmin / 1000.0,
                        pmax / 1000.0);
        }
        fprintf(stderr, "cpu process %.0f%%; mode %s",
                window_s > 0 ? (double)(cpu - win_cpu) / 1e9 / window_s * 100.0 : 0,
                g_xbox_spin_sleep ? "sleep" : "spin");
        if (s_auto_on)
            fprintf(stderr, ", auto lead %.0f ms (holds %ld, near misses %ld, hitches %ld,"
                    " late wakes %ld of %ld; up %ld down %ld back %ld restore %ld)",
                    (double)s_spin_lead_ns / 1e6, s_auto.tot.holds, s_auto.tot.near_misses,
                    s_auto.tot.hitches, s_auto.tot.late_wakes, s_auto.tot.waits,
                    s_auto.tot.ups, s_auto.tot.downs, s_auto.tot.reverts, s_auto.tot.restores);
        else
            fprintf(stderr, ", lead %.0f ms", (double)s_spin_lead_ns / 1e6);
        /* The watchdog (BO-211): its windows so far, how many were void,
         * and its resets (decided, forced). */
        fprintf(stderr, "; watchdog windows %u (void %u, fault %u), resets %u + %u forced",
                s_watch.windows, s_watch.voids, s_watch.faults, s_watch.resets, s_watch.forced);
        fputc('\n', stderr);
        for (i = 0; i <= SITE_SLOTS; i++) {
            SpinSite *st = &s_sites[i];
            if ((st->va || i == SITE_SLOTS) && st->waits)
                site_line(st, window_s, i == SITE_SLOTS);
            site_reset(st);
        }
        fflush(stderr);
        n = 0;
        win_ns = now;
        win_cpu = cpu;
    }
}

/* ── The display's rate and the host flip grid ───────────── */

/* Plain volatile 64-bit words, as s_disp_ns: one writer each. */
static volatile LONGLONG s_display_mhz;      /* millihertz, 0 unknown */

void xbox_DisplayRateSet(double hz)
{
    LONGLONG v = hz > 0.0 && hz < 2000.0 ? (LONGLONG)(hz * 1000.0 + 0.5) : 0;
    if (s_display_mhz == v)
        return;
    s_display_mhz = v;
    if (v)
        XR_NOTICE("[PACING] the display refreshes at %.3f Hz\n", hz);
}

int xbox_DisplayGrid(uint64_t now_ns, uint64_t *ref_ns, uint64_t *period_ns)
{
    uint64_t ref = (uint64_t)s_disp_ns, pd = (uint64_t)s_disp_period;
    uint64_t hint = s_disp_hint_bad ? 0 : (uint64_t)s_disp_hint;
    uint64_t sdl = (uint64_t)s_display_mhz;
    int fresh = (display_lock_on() || s_flip_host_ns) && ref && pd && !s_disp_relearn
                && !(now_ns > ref && now_ns - ref > DISPLAY_STALE_NS);

    /* Measured faster than the mode SDL reads by more than 5 %: a period
     * folded from two rates (kernel_pacing above), not the display's. */
    if (fresh && sdl && 1e12 / (double)pd > (double)sdl * 1.05)
        fresh = 0;
    /* Measured off the window system's stated refresh: the stated one. */
    if (fresh && hint && (pd > hint ? pd - hint : hint - pd) > hint / 100)
        fresh = 0;
    if (fresh) {
        *period_ns = pd;
        /* Irregular reports (a display link extrapolating its timeline,
         * BO-77): the period holds, the phase does not. */
        *ref_ns = s_disp_jitter ? 0 : ref;
        return *ref_ns ? 2 : 1;
    }
    if (hint) {
        *ref_ns = 0;
        *period_ns = hint;
        return 1;
    }
    return 0;
}

double xbox_DisplayRate(void)
{
    uint64_t ref, pd;

    if (xbox_DisplayGrid(xbox_HostNowNs(), &ref, &pd))
        return 1e9 / (double)pd;
    return (double)s_display_mhz / 1000.0;
}

void xbox_FlipHostRate(double hz)
{
    LONGLONG ns = hz >= 24.0 && hz <= 1000.0 ? (LONGLONG)(1e9 / hz + 0.5) : 0;
    LONGLONG cur = s_flip_host_ns;
    /* The measured rate moves by a little between reads: a change of 1 %
     * or less is the same rate (the grid takes its period from the display
     * anyway, nv2a_pb_exec.c). */
    if (ns && cur && (ns > cur ? ns - cur : cur - ns) <= cur / 100)
        return;
    if (cur == ns)
        return;
    /* The lock off: what it learned before (another grid, another display)
     * is dropped; the grid follows fresh reports. */
    if (ns && !cur && !display_lock_on())
        s_disp_relearn = 1;
    s_flip_host_ns = ns;
    if (ns)
        XR_NOTICE("[PACING] flips held to a %.3f Hz host grid\n", hz);
    else
        XR_NOTICE("[PACING] flips held to the vblank again\n");
}

uint64_t xbox_FlipHostPeriodNs(void)
{
    return (uint64_t)s_flip_host_ns;
}

/* The grid's state for the title, a sequence lock: the executor writes, the
 * title's thread reads. */
static uint32_t s_grid_seq, s_grid_swaps;
static uint64_t s_grid_due, s_grid_per;

void xbox_FlipHostGridNote(uint32_t swaps, uint64_t due_ns, uint64_t period_ns)
{
    uint32_t q = __atomic_load_n(&s_grid_seq, __ATOMIC_RELAXED);

    __atomic_store_n(&s_grid_seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s_grid_swaps, swaps, __ATOMIC_RELAXED);
    __atomic_store_n(&s_grid_due, due_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&s_grid_per, period_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&s_grid_seq, q + 2, __ATOMIC_RELEASE);
}

int xbox_FlipHostGridState(uint32_t *swaps, uint64_t *due_ns, uint64_t *period_ns)
{
    int n;

    for (n = 0; n < 16; n++) {
        uint32_t a = __atomic_load_n(&s_grid_seq, __ATOMIC_ACQUIRE), b;
        if (a & 1)
            continue;
        *swaps = __atomic_load_n(&s_grid_swaps, __ATOMIC_RELAXED);
        *due_ns = __atomic_load_n(&s_grid_due, __ATOMIC_RELAXED);
        *period_ns = __atomic_load_n(&s_grid_per, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&s_grid_seq, __ATOMIC_RELAXED);
        if (a == b)
            return a != 0 && *due_ns && *period_ns;
    }
    return 0;
}

/* xbox_FlipHostHanded/Shown/ShownAt: a ring by flip number. An entry is
 * written by the executor (flip, swap, edge; shown 0 or the stand-in) and
 * then by the presenter's waiter (shown); its seq is odd while it changes. */
#define SHOWN_RING 64u
static struct {
    uint32_t seq, flip, swap, real;
    uint64_t edge, shown;
} s_shown[SHOWN_RING];
static volatile LONGLONG s_shown_real_ns;   /* the last display time reported */

static void shown_write(uint32_t i, uint32_t flip, uint32_t swap, uint64_t edge,
                        uint64_t shown, uint32_t real)
{
    uint32_t q = __atomic_load_n(&s_shown[i].seq, __ATOMIC_RELAXED);

    __atomic_store_n(&s_shown[i].seq, q | 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s_shown[i].flip, flip, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shown[i].swap, swap, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shown[i].edge, edge, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shown[i].shown, shown, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shown[i].real, real, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shown[i].seq, (q | 1u) + 1u, __ATOMIC_RELEASE);
}

void xbox_FlipHostHanded(uint32_t flip, uint32_t swap, uint64_t edge_ns, uint64_t period_ns,
                         uint64_t handed_ns)
{
    static uint64_t anchor;           /* the last on-time frame's edge: a refresh */
    uint64_t shown = 0, last = (uint64_t)s_shown_real_ns;

    /* No display times for a second: the hand-over stands in, on a display
     * whose refreshes are where the frames handed over on time are (within
     * half a millisecond of their edge). A frame handed over later shows at
     * the first refresh after it. */
    if (!last || handed_ns > last + 1000000000ull) {
        if (handed_ns <= edge_ns + 500000u || !anchor || !period_ns
                || anchor + 500000u >= handed_ns) {
            anchor = edge_ns;
            shown = edge_ns;
        } else {
            shown = anchor + (handed_ns - 500000u - anchor + period_ns - 1) / period_ns * period_ns;
        }
    }
    /* real's bit 1: handed over on time (by half a ms after its edge). */
    shown_write(flip % SHOWN_RING, flip, swap, edge_ns, shown,
                handed_ns <= edge_ns + 500000u ? 2u : 0u);
}

void xbox_FlipHostShown(uint32_t flip, uint64_t shown_ns)
{
    uint32_t i = flip % SHOWN_RING;
    uint32_t rl = __atomic_load_n(&s_shown[i].real, __ATOMIC_RELAXED);

    s_shown_real_ns = (LONGLONG)shown_ns;
    if (__atomic_load_n(&s_shown[i].flip, __ATOMIC_RELAXED) != flip || (rl & 1u))
        return;                       /* not a grid frame, or shown already */
    shown_write(i, flip, __atomic_load_n(&s_shown[i].swap, __ATOMIC_RELAXED),
                __atomic_load_n(&s_shown[i].edge, __ATOMIC_RELAXED), shown_ns, rl | 1u);
}

int xbox_FlipHostShownAt(uint32_t swap, uint64_t *shown_ns, uint64_t *edge_ns, int *real)
{
    uint32_t i;

    for (i = 0; i < SHOWN_RING; i++) {
        uint32_t a = __atomic_load_n(&s_shown[i].seq, __ATOMIC_ACQUIRE), b, sw, rl;
        uint64_t sh, ed;
        if (!a || (a & 1u))
            continue;
        sw = __atomic_load_n(&s_shown[i].swap, __ATOMIC_RELAXED);
        sh = __atomic_load_n(&s_shown[i].shown, __ATOMIC_RELAXED);
        ed = __atomic_load_n(&s_shown[i].edge, __ATOMIC_RELAXED);
        rl = __atomic_load_n(&s_shown[i].real, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&s_shown[i].seq, __ATOMIC_RELAXED);
        if (a != b || sw != swap || !sh)
            continue;
        *shown_ns = sh;
        *edge_ns = ed;
        *real = (int)rl;
        return 1;
    }
    return 0;
}

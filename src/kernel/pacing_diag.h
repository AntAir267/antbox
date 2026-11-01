/*
 * pacing_diag.h - why the title's frame rate dropped, in one line (BO-210).
 *
 * Pure arithmetic, header-only (tests/vblank_schedule drives it);
 * kernel_pacing.c feeds it one sample per flip from the executor and prints
 * what it returns, always (no diagnostic logging needed): the drop is rare
 * and a bug report needs to say which kind it was.
 *
 * Per flip: the interval the title asked for, whether its vblank had already
 * passed when the swap was scheduled ("late": the XDK's rule then waits for
 * the next one, so a frame that needs 16.8 ms costs 33.3), and how the time
 * since the previous flip was spent by the executor:
 *   busy   walking the pushbuffer (the executor's own work: decode, draws)
 *   idle   waiting for the title to submit (the title's CPU work)
 *   hold   held for the vblank
 * plus two counters from the hold itself: a hold that found the published
 * vblank edge already more than 2 ms past (the timer thread is behind), and
 * a hold that spun longer than the spin lead allows (should never happen).
 *
 * In windows of at least 2 s and 30 flips, a window with no gap over 250 ms
 * and no frame over 100 ms (a load, a pause, a shader compile: void), the
 * share of late flips decides whether the cadence dropped (>= 15 %: one
 * miss in seven, below ~52 fps at interval 1; normal racing is ~0.1 %), and
 * the busy and idle shares say why:
 *   EXEC   half the late frames or more had executor work over 95 % of the
 *          asked interval: CPU-bound in the executor (a heavy scene, a low
 *          CPU clock). Pacing is not the cause; nothing is reset.
 *   TITLE  the same for executor work plus the wait for the title: the
 *          title's own CPU work is the limit.
 *   FAULT  under 30 % of the late frames were over budget either way (the
 *          frames had the time and still missed), or the timer thread was
 *          behind on a tenth of the holds, or one hold in twenty spun past
 *          its cap (preemption aside, a bug): a pacing state. The caller relearns the display lock.
 *   MIXED  in between: said, nothing done.
 * One line when a drop starts, one a minute while it lasts (a move between
 * executor-bound, title-bound and mixed is the same drop), one when it ends.
 */
#ifndef XBOX_PACING_DIAG_H
#define XBOX_PACING_DIAG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PDIAG_PERIOD_NS   16666667ull
#define PDIAG_WINDOW_NS   2000000000ull
#define PDIAG_MIN_FLIPS   30
#define PDIAG_GAP_NS      250000000ull
#define PDIAG_HITCH_NS    100000000ull
#define PDIAG_REPEAT_NS   60000000000ull

enum { PDIAG_NONE = 0, PDIAG_CLEAN, PDIAG_EXEC, PDIAG_TITLE, PDIAG_FAULT, PDIAG_MIXED };

typedef struct {
    /* the window */
    uint64_t win_ns, last_ns;
    unsigned n, late, late_exec, late_title, stale, overspin, void_;
    double   asked;                 /* sum of 60/iv: the asked flips/s x seconds */
    uint64_t busy_sum, idle_sum, hold_sum, frame_sum;
    /* the episode */
    int      state;                 /* PDIAG_CLEAN or the drop's kind */
    uint64_t said_ns;
    /* the last window's numbers, for the line */
    double   fps, want_fps, late_share, busy_ms, idle_ms, exec_share, title_share;
    double   stale_share;
    unsigned overspins;
    /* every window's verdict, for the watchdog (pacing_watch.h): bumped at
     * each window's end; the kind is PDIAG_NONE for a void window */
    unsigned win_seq;
    int      win_kind;
    /* the window's fault came only from the timer thread being behind or a
     * hold over-spinning: nothing the keeper or the lock can cure */
    int      win_timer_only;
} XboxPacingDiag;

static inline void pdiag_init(XboxPacingDiag *d)
{
    memset(d, 0, sizeof *d);
    d->state = PDIAG_CLEAN;
}

static inline void pdiag_reset_window(XboxPacingDiag *d, uint64_t now)
{
    d->win_ns = now;
    d->n = d->late = d->late_exec = d->late_title = d->stale = d->overspin = d->void_ = 0;
    d->asked = 0;
    d->busy_sum = d->idle_sum = d->hold_sum = d->frame_sum = 0;
}

static inline const char *pdiag_kind(int k)
{
    switch (k) {
    case PDIAG_EXEC:  return "executor-bound";
    case PDIAG_TITLE: return "title-bound";
    case PDIAG_FAULT: return "pacing fault";
    case PDIAG_MIXED: return "mixed";
    default:          return "clean";
    }
}

/* One flip. Returns PDIAG_NONE, or at a window's end what it was when that
 * is worth a line (a drop starting, a minute of it, its end); *line gets the
 * line (without the "[PACING] " prefix).
 *
 * wake: how late the previous hold let the executor go, after the edge it
 * waited for (ns; -1 not known). The frame's busy and idle time are counted
 * from that release, so a frame whose own time fits the interval is still
 * late when the release came late enough: the hold's wake-up, not a pacing
 * state (BO-211 review: at Auto level 0 the hold sleeps on the condition
 * variable and wakes 1-3 ms late under load or on Windows' timers; a title
 * at 15-16 ms read as "had the time and missed" and was reset for nothing).
 * The wake counts toward the frame; not known, the budget is 85 % of the
 * interval instead of 95 %, which hides a wake of up to 2.5 ms. */
static inline int pdiag_flip(XboxPacingDiag *d, uint64_t now, uint32_t iv, int late,
                             uint64_t busy, uint64_t idle, uint64_t hold, int64_t wake,
                             int stale, int overspin, char *line, size_t n)
{
    uint64_t frame = busy + idle + hold, budget, w = wake > 0 ? (uint64_t)wake : 0;
    int kind, out = PDIAG_NONE;

    if (!d->win_ns) {
        d->win_ns = d->last_ns = now;
        if (!d->state)
            d->state = PDIAG_CLEAN;
        return PDIAG_NONE;
    }
    if (now - d->last_ns > PDIAG_GAP_NS || frame > PDIAG_HITCH_NS || iv == 0 || iv > 4)
        d->void_++;
    d->last_ns = now;
    if (iv == 0 || iv > 4)
        iv = 1;
    budget = (uint64_t)iv * PDIAG_PERIOD_NS * (wake < 0 ? 85u : 95u) / 100u;
    d->n++;
    d->asked += 60.0 / iv;
    d->busy_sum += busy;
    d->idle_sum += idle;
    d->hold_sum += hold;
    d->frame_sum += frame;
    d->stale += stale != 0;
    d->overspin += overspin != 0;
    if (late) {
        d->late++;
        d->late_exec += busy + w > budget;
        d->late_title += busy + idle + w > budget;
    }
    if (now - d->win_ns < PDIAG_WINDOW_NS || d->n < PDIAG_MIN_FLIPS)
        return PDIAG_NONE;

    d->win_seq++;
    d->win_kind = PDIAG_NONE;
    if (d->void_) {
        pdiag_reset_window(d, now);
        return PDIAG_NONE;
    }
    {
        double secs = (double)(now - d->win_ns) / 1e9;
        double ls = (double)d->late / d->n;
        double es = d->late ? (double)d->late_exec / d->late : 0;
        double ts = d->late ? (double)d->late_title / d->late : 0;
        double ss = (double)d->stale / d->n;
        d->fps = d->n / secs;
        d->want_fps = d->asked / d->n;
        d->late_share = ls;
        d->busy_ms = (double)d->busy_sum / d->n / 1e6;
        d->idle_ms = (double)d->idle_sum / d->n / 1e6;
        d->exec_share = es;
        d->title_share = ts;
        d->stale_share = ss;
        d->overspins = d->overspin;
        int timer = d->overspin * 20u >= d->n || (ss >= 0.10 && ls >= 0.05);
        d->win_timer_only = timer && !(ls >= 0.15 && es < 0.5 && ts < 0.3);
        if (timer)
            kind = PDIAG_FAULT;
        else if (ls < 0.15)
            kind = PDIAG_CLEAN;
        else if (es >= 0.5)
            kind = PDIAG_EXEC;
        else if (ts >= 0.5)
            kind = PDIAG_TITLE;
        else if (ts < 0.3)
            kind = PDIAG_FAULT;
        else
            kind = PDIAG_MIXED;
    }
    d->win_kind = kind;
    if (kind == PDIAG_CLEAN) {
        if (d->state != PDIAG_CLEAN) {
            snprintf(line, n, "frame rate back to %.1f fps (was %s)", d->fps,
                     pdiag_kind(d->state));
            out = PDIAG_CLEAN;
        }
        d->state = PDIAG_CLEAN;
    } else if (kind != PDIAG_FAULT && d->state != PDIAG_CLEAN && d->state != PDIAG_FAULT
               && now - d->said_ns < PDIAG_REPEAT_NS) {
        /* Executor-bound, title-bound, mixed: one slow episode, said once a
         * minute however its kind moves between them (BO-211: on a slow
         * machine it moved every window, a line each time). */
        d->state = kind;
    } else if (kind != d->state || now - d->said_ns >= PDIAG_REPEAT_NS) {
        const char *why =
            kind == PDIAG_EXEC  ? "the executor needs more than the frame (CPU-bound); pacing is not the cause" :
            kind == PDIAG_TITLE ? "the title's own CPU work needs more than the frame; pacing is not the cause" :
            kind == PDIAG_FAULT ? "frames had the time and still missed their vblank: a pacing fault" :
                                  "some frames over budget, some not";
        snprintf(line, n, "%.1f fps of %.0f asked, %.0f%% of flips late: %s"
                 " (executor %.1f ms busy + %.1f ms waiting for the title a frame;"
                 " late frames over budget: executor %.0f%%, with the title %.0f%%;"
                 " timer behind on %.0f%% of holds, %u over-spins)",
                 d->fps, d->want_fps, d->late_share * 100.0, why, d->busy_ms, d->idle_ms,
                 d->exec_share * 100.0, d->title_share * 100.0, d->stale_share * 100.0,
                 d->overspins);
        d->state = kind;
        d->said_ns = now;
        out = kind;
    }
    pdiag_reset_window(d, now);
    return out;
}

#endif /* XBOX_PACING_DIAG_H */

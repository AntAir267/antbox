/*
 * pacing_watch.h - the pacing watchdog (BO-211): a stuck pacing state does
 * not outlive a few seconds.
 *
 * Pure arithmetic, header-only (tests/vblank_schedule drives it, with the
 * simulator there); kernel_pacing.c feeds it from the executor at each flip
 * and performs what it asks.
 *
 * It sits on the cadence diagnosis (pacing_diag.h, BO-210), which already
 * sorts each 2 s window into clean, executor-bound, title-bound, mixed or a
 * pacing fault (frames that had the time and still missed their vblank, or
 * a timer thread behind on a tenth of the holds). The diagnosis only says
 * so; the watchdog acts on the one kind that pacing state can cause:
 *
 *   a pacing-fault window (2 s)  ->  one reset: the frame-queue
 *   keeper (Vulkan Smooth, Windows anchor, Metal pacer: their hooks) and the
 *   display lock start over together. The vblank keeps its own grid until
 *   fresh display reports lock it again, so the title sees nothing.
 *
 * A fault that is only the timer thread behind on a tenth of the holds, or
 * holds over-spinning, is said and not reset: neither the keeper nor the
 * lock is its cause (a starved machine). The diagnosis counts the hold's own
 * wake-up lateness into each frame (pacing_diag.h), so a late hold is never
 * read as a pacing state either.
 *
 * After a reset the next one waits a holdoff: 10 s, doubled at each further
 * reset up to 5 minutes; 60 s of clean windows bring it back to 10 s. A
 * third reset in a row without a clean minute between says once that resets
 * do not help (the cause is elsewhere: see RECOMP_TRACE=frametime). Every
 * reset is one line, always printed.
 *
 * A window is void for the watchdog (never counts toward a reset, and breaks
 * a run of faults) when inside it: the title was paused (the overlay), the
 * window was hidden or minimised, the machine slept or the process stopped
 * (a wake gap), the display changed its rate, or the title asked for a swap
 * interval of 2 or more (its 30 Hz crash cameras: their misses are the
 * title's rate, not ours). Loads and hitches are void in the diagnosis
 * already (a gap over 250 ms, a frame over 100 ms). Executor-bound,
 * title-bound and mixed windows never reset anything.
 *
 * A settings change (Frame pacing, Power mode) and a wake gap reset at once
 * (pwatch_force), and start the holdoff over.
 *
 * Cost per flip: the diagnosis's dozen adds and a few compares here.
 */
#ifndef XBOX_PACING_WATCH_H
#define XBOX_PACING_WATCH_H

#include "pacing_diag.h"

#define PWATCH_FAULTS_TO_RESET 1
#define PWATCH_HOLDOFF_NS      10000000000ull     /* 10 s */
#define PWATCH_HOLDOFF_MAX_NS  300000000000ull    /* 5 min */
#define PWATCH_CLEAN_NS        60000000000ull     /* a clean minute forgives */
#define PWATCH_FUTILE          3                  /* resets in a row before saying they do not help */

/* Why a window is void (bits, pwatch_void). */
enum {
    PWATCH_V_PAUSED  = 1,
    PWATCH_V_HIDDEN  = 2,
    PWATCH_V_WAKE    = 4,
    PWATCH_V_RATE    = 8,
    PWATCH_V_IV2     = 16,
};

/* What the caller does after a flip (bits). */
enum { PWATCH_NONE = 0, PWATCH_LINE = 1, PWATCH_RESET = 2 };

typedef struct {
    XboxPacingDiag d;
    unsigned seen_seq;
    unsigned void_bits;        /* this window's */
    int      fault_run;        /* pacing-fault windows in a row */
    uint64_t next_ok_ns;       /* no reset before this */
    uint64_t holdoff_ns;
    uint64_t clean_from_ns;    /* start of the current run of clean windows; 0 none */
    int      in_row;           /* resets with no clean minute between */
    int      futile_said;
    /* totals, for the trace and the tests */
    unsigned resets, forced, windows, voids, faults;
    char     why[320];         /* the reset's line */
} XboxPacingWatch;

static inline void pwatch_init(XboxPacingWatch *w)
{
    memset(w, 0, sizeof *w);
    pdiag_init(&w->d);
    w->holdoff_ns = PWATCH_HOLDOFF_NS;
}

/* Something that voids the current window (any of PWATCH_V_*). */
static inline void pwatch_void(XboxPacingWatch *w, unsigned bits)
{
    w->void_bits |= bits;
}

/* A reset not decided by the windows: a settings change, a wake gap. The
 * caller performs it; the ladder starts over. */
static inline void pwatch_force(XboxPacingWatch *w, uint64_t now)
{
    w->forced++;
    w->fault_run = 0;
    w->in_row = 0;
    w->futile_said = 0;
    w->holdoff_ns = PWATCH_HOLDOFF_NS;
    w->next_ok_ns = now;
    w->clean_from_ns = 0;
    w->void_bits |= PWATCH_V_WAKE;    /* the window it falls in decides nothing */
}

/* One flip (the arguments are pdiag_flip's). Returns PWATCH_NONE, or bits:
 * PWATCH_LINE (print *line: the diagnosis said something) and PWATCH_RESET
 * (perform the reset and print w->why). */
static inline int pwatch_flip(XboxPacingWatch *w, uint64_t now, uint32_t iv, int late,
                              uint64_t busy, uint64_t idle, uint64_t hold, int64_t wake,
                              int stale, int overspin, char *line, size_t n)
{
    int k, kind, out;

    if (iv >= 2)
        w->void_bits |= PWATCH_V_IV2;
    k = pdiag_flip(&w->d, now, iv, late, busy, idle, hold, wake, stale, overspin, line, n);
    out = k != PDIAG_NONE ? PWATCH_LINE : PWATCH_NONE;
    if (w->d.win_seq == w->seen_seq)
        return out;
    /* A window just ended. */
    w->seen_seq = w->d.win_seq;
    w->windows++;
    kind = w->d.win_kind;
    if (kind == PDIAG_NONE || w->void_bits) {
        w->voids++;
        w->void_bits = 0;
        w->fault_run = 0;
        return out;
    }
    if (kind == PDIAG_CLEAN) {
        w->fault_run = 0;
        if (!w->clean_from_ns)
            w->clean_from_ns = now;
        else if (now - w->clean_from_ns >= PWATCH_CLEAN_NS) {
            w->holdoff_ns = PWATCH_HOLDOFF_NS;
            w->in_row = 0;
            w->futile_said = 0;
        }
        return out;
    }
    w->clean_from_ns = 0;
    if (kind != PDIAG_FAULT) {
        w->fault_run = 0;
        return out;
    }
    w->faults++;
    /* A fault that is only the timer thread behind (a starved machine) or a
     * hold over-spinning: said by the diagnosis, not reset, since neither
     * the keeper nor the lock is its cause (BO-211 review). */
    if (w->d.win_timer_only) {
        w->fault_run = 0;
        return out;
    }
    if (++w->fault_run < PWATCH_FAULTS_TO_RESET || now < w->next_ok_ns)
        return out;
    /* Reset. */
    w->fault_run = 0;
    w->resets++;
    w->in_row++;
    w->next_ok_ns = now + w->holdoff_ns;
    {
        uint64_t held = w->holdoff_ns;
        w->holdoff_ns = w->holdoff_ns * 2 > PWATCH_HOLDOFF_MAX_NS ? PWATCH_HOLDOFF_MAX_NS
                                                                 : w->holdoff_ns * 2;
        if (w->in_row >= PWATCH_FUTILE && !w->futile_said) {
            w->futile_said = 1;
            snprintf(w->why, sizeof w->why, "watchdog: %d resets in a row did not help (%.1f fps of %.0f"
                     " asked, %.0f%% of flips late): this is not a pacing state; next reset"
                     " in %.0f s at the earliest (RECOMP_TRACE=frametime says more)",
                     w->in_row, w->d.fps, w->d.want_fps, w->d.late_share * 100.0, held / 1e9);
        } else {
            snprintf(w->why, sizeof w->why, "watchdog: %.1f fps of %.0f asked, %.0f%% of flips late for"
                     " %.0f s with the frames on time (executor %.1f ms busy, timer behind"
                     " on %.0f%% of holds): the frame-queue keeper and the display lock start"
                     " over (next reset in %.0f s at the earliest)",
                     w->d.fps, w->d.want_fps, w->d.late_share * 100.0,
                     PWATCH_FAULTS_TO_RESET * PDIAG_WINDOW_NS / 1e9, w->d.busy_ms, w->d.stale_share * 100.0, held / 1e9);
        }
    }
    return out | PWATCH_RESET;
}

#endif /* XBOX_PACING_WATCH_H */

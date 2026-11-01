/*
 * power_auto.h - Power mode "Auto": the vblank hold's spin lead picked at
 * run time (BO-175). Pure arithmetic; tests/power_auto drives it with
 * synthetic traces, kernel_pacing.c feeds it one sample per held flip.
 *
 * What it changes: only how the executor waits for the vblank its flip is
 * held for (sleep, then spin the last `lead` ms; kernel_pacing.c). The flip
 * still leaves at the same vblank, so the title's frames, its simulation
 * steps and its own 30 fps phases are what they are in any other mode.
 *
 * What it measures, per held flip, against the vblank the title itself asked
 * for (its swap interval: 1 in a race, 2 in the crash camera or split
 * screen), never against a fixed 60:
 *   slack  the target vblank's edge minus the frame's arrival at the hold.
 *          Negative: the frame missed the vblank it asked for. Missed by
 *          less than a vblank period is a near miss (what a low CPU clock
 *          does to a heavy frame, and what spinning cures); missed by more,
 *          or a gap of over 250 ms between holds, is a hitch (loading, file
 *          IO, a shader compile) that the wait method cannot cure.
 *   wake   the hold's return minus the moment the timer thread raised that
 *          vblank: the wait's own lateness (a thread woken late from its
 *          sleep). The timer thread's lateness is not in it: no spin on
 *          this thread can make it raise sooner.
 *
 * Decisions, in windows of 2 s of holds:
 *   up      the first window with >= 2 near misses, or late wakes (> 1 ms)
 *           in >= 5 % of the waits: one level more spin, two when near
 *           misses were >= 2 % of the holds (a bend on the Deck: help
 *           within 2 s, enough to lift the CPU clock).
 *   check   the window after a step up: no longer bad ends the climb
 *           there; otherwise one or two more steps. No help even at the top: spinning is not the cure (the
 *           GPU is the limit, or the timer): back to where the climb
 *           started, which is the ceiling for 60 s (doubling on each
 *           repeat, up to 10 min).
 *   play    a window whose interval-1 frames have half their interval or
 *           less to spare in a quarter of them is play (a race on a
 *           handheld; a menu has nearly all of it to spare). In play the
 *           level never goes below 2 (4 ms: the clock stays up for the next
 *           bend) and a step down needs 30 s of clean windows; elsewhere
 *           10 s, and 0 (sleep only) is allowed.
 *   down    after that run of clean windows with headroom (90 % of the
 *           on-time frames arrived with at least a quarter of their
 *           interval to spare): one level less. A step up within 30 s of a
 *           step down doubles the run needed (up to 5 min).
 *   hitch   a frame one to six vblanks late (a shader compile, a file read)
 *           skips that window's decision, nothing more.
 *   stall   a frame more than 100 ms late, or 250 ms without a hold (a
 *           load): the 3 s after it decide nothing; a climb that began
 *           less than 20 s before it is undone with the level it taught
 *           (the near misses of a loading screen were its run-up); and the
 *           level returns to the one last proven needed (held, decaying one
 *           step per 10 min since it was last proven), so a race after a
 *           light menu starts where the last race ended.
 *
 * Levels (ms of spin before the vblank): 0 (Battery saver), 2, 4, 8
 * (Balanced; the start), 12, 17 (the whole frame: the busy-wait).
 */
#ifndef XBOX_POWER_AUTO_H
#define XBOX_POWER_AUTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_POWER_AUTO_LEVELS 6
#define XBOX_POWER_AUTO_START  3            /* 8 ms: Balanced */

typedef struct {
    uint64_t now_ns;      /* the hold's return */
    uint32_t interval;    /* vblanks the title asked for (swap interval); 0: none (not counted) */
    int64_t  slack_ns;    /* target edge minus arrival; < 0 the frame missed its vblank */
    int64_t  wake_ns;     /* return minus the vblank's raise; < 0: the hold did not sleep through a raise */
} XboxPowerAutoSample;

/* Totals since init, for the overlay, the trace and the tests. */
typedef struct {
    long holds, near_misses, hitches, late_wakes, waits;
    long ups, downs, reverts, restores;
    long bad_windows;
} XboxPowerAutoTotals;

typedef struct {
    int      level, ceiling, held;
    uint64_t ceiling_until_ns, held_at_ns;
    /* the window being filled */
    uint64_t win_start_ns, last_ns;
    int      n, ontime, tight, waited, late_wakes, nears, hitch;
    int      ontime1, busy;         /* on-time holds at interval 1, those with < half to spare */
    int      play;                  /* the last window looked like play (the overlay, tests) */
    /* the run of windows */
    int      clean_run, down_need;
    uint64_t quiet_until_ns, last_down_ns, last_up_ns, down_ok_since_ns;
    /* after a step up: did it help? */
    int      probe_from, probe_base;
    /* the climb under way: where it began, what was held then */
    int      climb_from, climb_held;
    uint64_t climb_ns;
    uint64_t ceiling_wait_ns;
    char     why[96];
    XboxPowerAutoTotals tot;
} XboxPowerAuto;

void xbox_PowerAutoInit(XboxPowerAuto *a);
/* One held flip. Returns 1 when the level changed (a->why says why). */
int  xbox_PowerAutoFeed(XboxPowerAuto *a, const XboxPowerAutoSample *s);
int  xbox_PowerAutoLevel(const XboxPowerAuto *a);
/* The spin lead of a level, in ns. */
uint64_t xbox_PowerAutoLeadNs(int level);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_POWER_AUTO_H */

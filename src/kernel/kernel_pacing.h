/*
 * kernel_pacing.h - frame pacing: the vblank schedule's arithmetic, the
 * sleeping wait that lowered guest spin loops call, and RECOMP_TRACE=pacing.
 *
 * A title that waits for its next frame in a busy loop on a counter the
 * vblank DPC moves (BLiNX 2's main loop at 0x00060475) holds a host core for
 * nothing. With `tools.recomp --spin-waits`, the generator lowers such a
 * loop's back edge to RECOMP_SPIN_WAIT(va) (templates/runtime/recomp_types.h),
 * which calls xbox_SpinWait while the pacing mode is "sleep". The kernel
 * signals xbox_SpinWake whenever guest-visible state may have changed (a
 * vblank pass, any host-run ISR or DPC, a fence write), and every wait is
 * bounded at 1 ms, so a wake the kernel never sends costs a millisecond and
 * never hangs the title. Mode "spin" (the default) returns at once: the loop
 * busy-waits exactly as the console does.
 */
#ifndef XBOX_KERNEL_PACING_H
#define XBOX_KERNEL_PACING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── The vblank schedule (pure; tests/vblank_schedule drives it) ──
 *
 * Vblank N is due at t0 + N * 10^9 / 60 ns, so the rate is exact whatever
 * the caller's sleep granularity. A caller more than 100 ms behind (a
 * debugger stop, a stalled timer thread) restarts the grid at now instead of
 * firing the missed vblanks in a burst. */
typedef struct {
    int      started;
    uint64_t t0_ns;      /* the grid's epoch */
    uint64_t count;      /* vblanks fired since t0 */
} XboxVblankSchedule;

#define XBOX_VBLANK_HZ          60u
#define XBOX_VBLANK_RESTART_NS  100000000ull   /* 100 ms behind: restart */

/* Advance the schedule to now_ns. Returns 1 when a vblank is due now (and
 * counts it), 0 when not. *next_due_ns is the next edge on the grid (in the
 * past while catching up); *restarted_late_ns is how far behind a restart
 * was, 0 when none. */
int xbox_VblankSchedule(XboxVblankSchedule *s, uint64_t now_ns,
                        uint64_t *next_due_ns, uint64_t *restarted_late_ns);

/* ── The pacing mode ── */
enum { XBOX_PACING_SPIN = 0, XBOX_PACING_SLEEP = 1 };

/* Set before the guest starts (the enhancements layer's present.pacing).
 * Without a call the mode is spin. */
void xbox_PacingSetMode(int mode);
int  xbox_PacingMode(void);

/* Read by RECOMP_SPIN_WAIT on every pass of a lowered loop, with
 * g_xbox_irq_pending (kernel_hal.c): a pass with an interrupt posted calls
 * the wait at any pacing, as the gate holder's safe point. */
extern volatile int g_xbox_spin_sleep;
extern volatile int g_xbox_irq_pending;

/* One pass of a lowered loop whose condition was still true. First a safe
 * point (xbox_IrqSafePoint): the gate holder runs what was posted. Mode spin
 * returns there. Mode sleep:
 * return at once if the wake generation moved since this thread last looked,
 * else block until it moves or 1 ms passes. Never sleeps at DISPATCH level
 * or above, or with the one-CPU gate held: the DPC the loop waits for could
 * not run. */
void xbox_SpinWait(uint32_t va);

/* Guest-visible state may have changed: wake every xbox_SpinWait. One load
 * when the mode is spin. */
void xbox_SpinWake(void);

/* Block until the vblank count reaches target, or timeout_ms passes; 1 when
 * it did, 0 on the timeout. Woken by xbox_VblankSignal, which the timer
 * thread calls after counting each vblank: a condition variable, as plume
 * waits for its vsync worker (Xerenge rexglue/src/plume_renderer/
 * plume_graphics_system.cpp, PaceSwapToVblank), rather than a sleep-poll. */
int  xbox_VblankWaitUntil(uint32_t target, uint32_t timeout_ms);
/* One pass of a busy-wait: the CPU's pause hint (x86 PAUSE, arm64 YIELD;
 * nothing elsewhere). For the vblank spins here and the timer thread's. */
void xbox_CpuRelax(void);
void xbox_VblankSignal(void);
/* Mode sleep: spin through the last ns before the target vblank instead of
 * sleeping (present.spin_lead; 0, the default, sleeps to it). The timer
 * thread tells it when the next vblank is due, before counting the one it
 * raises. */
void xbox_PacingSetSpinLead(uint64_t ns);
/* Power mode Auto (BO-175, power_auto.h): the lead is picked at run time
 * from the holds; xbox_PacingSetSpinLead turns it off again. Only how the
 * hold waits changes, never which vblank the flip leaves at. */
void xbox_PacingSetSpinLeadAuto(void);
/* 1 while Auto is on; the lead it holds now and why it moved last. */
int  xbox_PacingAutoState(double *lead_ms, char *why, size_t n);
/* The executor, after a flip's hold: the swap interval the title asked for
 * (vblanks), when the frame reached the hold and the edge of the vblank it
 * asked for (0: unknown). Feeds Auto; nothing when it is off. */
void xbox_PacingHoldSample(uint32_t interval, uint64_t arrive_ns, uint64_t edge_ns);
/* The executor at each flip, in every mode: the interval asked, whether its
 * vblank had passed, and running totals of its hold and scan time (ns). An
 * always-printed [PACING] line when the frame rate drops says whether the
 * executor, the title or the pacing is the cause (pacing_diag.h, BO-210). */
void xbox_PacingFlipCadence(uint32_t interval, int late, uint64_t hold_total,
                            uint64_t scan_total);
/* The pacing watchdog (pacing_watch.h, BO-211). A presenter's hook that
 * makes its frame-queue keeper start over: called on the executor's thread
 * at a reset, it sets flags and returns. At most 4. */
void xbox_PacingResetHook(void (*fn)(void));

/* The display the window is on refreshes at this rate (Hz, 0 unknown): the
 * presenter's event loop sets it from the window's display mode, again when
 * the window moves to another display or the mode changes. */
void   xbox_DisplayRateSet(double hz);
double xbox_DisplayRate(void);

/* A title that keeps its simulation's own clock between flips asks for its
 * flips to be held to a host grid at this rate (Hz) instead of to the swap's
 * vblank (graphics.high_refresh); 0 goes back to the vblank. Any thread. */
void     xbox_FlipHostRate(double hz);
uint64_t xbox_FlipHostPeriodNs(void);   /* the period asked for, 0 none */

/* The display's refresh grid as the presenter's reports show it (the vblank
 * lock's period and anchor: a refresh time, less the presenter's phase on
 * Metal), learned while the lock is on or a title asks for the host grid;
 * else the window system's stated refresh (VK_GOOGLE_display_timing) as a
 * period alone. 2: period and phase (*ref_ns a refresh), 1: the period
 * only (*ref_ns 0), 0: neither. xbox_DisplayRate is this rate when there
 * is one, else the SDL display mode's (whole Hz). Any thread. */
int xbox_DisplayGrid(uint64_t now_ns, uint64_t *ref_ns, uint64_t *period_ns);

/* The executor, at every swap (nv2a_pb_exec.c): the swaps seen so far, and
 * for a swap held to the host grid its edge and the grid's period (0, 0
 * for one that is not). The title, at the start of a frame: the same, read
 * consistently; 1 when the last swap was on the grid. With its own count
 * of swaps issued, the title knows how many frames are on their way and
 * so the edge the frame it is building will flip at. */
void xbox_FlipHostGridNote(uint32_t swaps, uint64_t due_ns, uint64_t period_ns);
int  xbox_FlipHostGridState(uint32_t *swaps, uint64_t *due_ns, uint64_t *period_ns);
/* When each frame held to the host grid reached the display. The executor,
 * as it hands one over (nv2a_pb_exec.c): its flip number, its swap number
 * (the count xbox_FlipHostGridNote gives), its edge and the hand-over time.
 * A presenter that learns when a frame is on the glass (Vulkan's
 * present_wait) says so by flip number; without one (headless, a backend
 * with no display times) the hand-over stands in: the edge, or the first
 * refresh after it when the frame came later than its edge (by half a ms). The
 * title, by swap number: 1 and *shown_ns (host ns) and *edge_ns once known,
 * *real bit 0 when the time is the display's, bit 1 when the frame was
 * handed over on time (within half a ms of its edge). Any thread; a ring of the last
 * 64 frames. */
void xbox_FlipHostHanded(uint32_t flip, uint32_t swap, uint64_t edge_ns, uint64_t period_ns,
                         uint64_t handed_ns);
void xbox_FlipHostShown(uint32_t flip, uint64_t shown_ns);
int  xbox_FlipHostShownAt(uint32_t swap, uint64_t *shown_ns, uint64_t *edge_ns, int *real);
/* Reset the pacing state now (a settings change: Frame pacing, Power mode):
 * the keeper hooks and the display lock, at the executor's next flip. Any
 * thread. One always-printed line, "<why>: ... start over". */
void xbox_PacingWatchReset(const char *why);
/* The window can be seen (not minimised or hidden): while it cannot, the
 * watchdog decides nothing. Any thread. */
void xbox_PacingSetVisible(int visible);
void xbox_VblankNextDue(uint64_t ns);
/* The grid time of the last vblank raised, and of vblank number `count`
 * (one already raised, or one still to come); 0 before the first. */
uint64_t xbox_VblankLastDue(void);
uint64_t xbox_VblankEdgeNs(uint32_t count);
/* The emulated vblank of the frame the executor hands to the host: set
 * before the backend's on_flip, read by the window's presenter when the
 * frame is published (the same thread), so a present can be aimed at a
 * time on the vblank's grid (present_cadence.h). */
void     xbox_PacingFrameVblank(uint64_t ns);
uint64_t xbox_PacingFrameVblankNs(void);
uint32_t xbox_VblankCount(void);

/* Per-site counters, for the trace and the tests. */
typedef struct {
    uint32_t va;
    long waits, immediate, wakes, timeouts, dispatch;
    /* Loops that left on a signal (or at once) or on the timeout; a loop's
     * exit is counted when its thread next waits, more than 0.5 ms later. */
    long exit_wake, exit_timeout;
} XboxSpinSiteStats;
/* Copy the site's counters since the last reset; 0 if the site never ran. */
int  xbox_SpinSiteStats(uint32_t va, XboxSpinSiteStats *out);
void xbox_SpinStatsReset(void);

/* ── RECOMP_TRACE=pacing ── */

/* The executor's flip, on every backend (nv2a_pb_exec.c). */
void xbox_PacingFlip(uint32_t flip, uint32_t batches);

/* RECOMP_VBLANK_SYNC=display: a present path reports each display refresh
 * (host time, and its period when the platform knows it, else 0) and the
 * vblank timer's grid follows the display's (xbox_VblankSchedule). */
void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns);
int  xbox_VblankDisplayLockOn(void);
/* Drop what the lock learned (period, anchor, candidates); the vblank keeps
 * its own grid until fresh reports lock it again. Any thread; taken at the
 * next report. */
void xbox_VblankDisplayRelearn(void);
/* The refresh period the window system states (Vulkan's
 * VK_GOOGLE_display_timing refresh cycle: gamescope's own), 0 none. While it
 * is known the lock's period is it: taken at once when it changes and at a
 * relearn, and steps off its grid do not fold or change it, unless eight
 * in a row are off it (xbox_VblankDisplayCycleOk) (BO-210: a
 * 90 <-> 60 change or a Frame pacing switch learned in under a report, not
 * over seconds of steps). Any thread. */
void xbox_VblankDisplayCycle(uint64_t period_ns);
/* 1 while the stated refresh is the one in force: eight reports in a row
 * off its grid and it is not (until eight are on it again). */
int  xbox_VblankDisplayCycleOk(void);
/* 1 once after the lock saw the display change its rate (then 0 until it
 * sees another): the watchdog's window then decides nothing. */
int  xbox_VblankDisplayRateChanged(void);
/* A presenter turns the lock on or off itself (display.present_timing =
 * auto); a RECOMP_VBLANK_SYNC that is set keeps its meaning. */
void xbox_VblankDisplayLockAuto(int on);
/* A host present reached the swap chain (when_ns 0: now) or the display. */
void xbox_PacingPresent(uint64_t when_ns);
/* A vblank raised at now_ns (the timer thread), for the gap range. */
void xbox_PacingVblank(uint64_t now_ns);

/* CPU time of the calling thread, and of the process, in ns. */
uint64_t xbox_ThreadCpuNs(void);
uint64_t xbox_ProcessCpuNs(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_PACING_H */

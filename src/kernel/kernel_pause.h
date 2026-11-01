/*
 * kernel_pause.h - the host pause: the in-game settings overlay stops the
 * title while it is open.
 *
 * The title is held where the console's GPU would hold it. The executor (the
 * nv2a-ack thread, which stands in for the GPU) stops at FLIP_STALL, after
 * the frame is presented (xbox_HostPauseHold); the title's CPU then runs into
 * its own fence and pushbuffer-space waits within a frame or two and stops
 * there, which is what a stalled GPU does on the console. Nothing in the
 * title is suspended from outside, so no lock can be held across the pause
 * and no guest thread is stopped mid-instruction.
 *
 * Listeners are told of each change on the calling thread: the APU stops its
 * frame thread, so the title's sound does not play on (and its streaming,
 * which follows the play cursor, waits with it).
 *
 * The game time is not frozen: the vblank keeps counting and the host
 * clocks run. A title that steps its simulation by elapsed time is bounded
 * by its own catch-up limit (Burnout 3's sub_001B58E0: at most four steps a
 * frame), so it comes back with one frame of catch-up at most.
 *
 * A pause is taken at the next flip, so it waits out a load or a movie that
 * does not flip; the overlay opens at once either way.
 */
#ifndef XBOX_KERNEL_PAUSE_H
#define XBOX_KERNEL_PAUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pause (1) or resume (0) the title. Any thread; repeated calls are fine. */
void xbox_HostPause(int on);
int  xbox_HostPaused(void);

/* The way out (a quit or a restart that tears the GPU device down first):
 * the title is paused for good (a later xbox_HostPause(0) is ignored, the
 * hold runs no hold or idle work and never returns) and this waits up to
 * timeout_ms for the executor to be in the hold. 1: it is, so it uses the
 * GPU no more; 0: it did not get there in time (a load that does not flip,
 * a wait on a lost device). Any thread but the executor's; once. */
int  xbox_HostHoldForExit(int timeout_ms);

/* The executor, at the end of FLIP_STALL: notes the flip's time
 * (xbox_HostFlipTimes), then returns at once unless paused, otherwise blocks
 * until resumed. Counts as a flip for the hang watchdog while it waits (a
 * pause is not a hang). Holds no lock. */
void xbox_HostPauseHold(void);

/* The executor's recent flips, for the overlay's frame-time graph: the host
 * times (xbox_HostNowNs) of the last flips, oldest first, up to max; a 0
 * entry is a gap (the title was paused there). Returns how many. Any
 * thread; a read racing the executor may see one stale entry. */
int  xbox_HostFlipTimes(uint64_t *ns, int max);
/* The same, with the swap interval (vblanks) each of those flips asked for
 * beside its time: 1 at 60 fps, 2 where the title runs at 30 (Burnout 3's
 * crash camera), 0 at a gap. */
int  xbox_HostFlipTimesIv(uint64_t *ns, uint8_t *iv, int max);
/* The executor, before xbox_HostPauseHold: the interval the flip it is
 * about to note asked for (0 counts as 1). */
void xbox_HostFlipInterval(uint32_t iv);
/* The interval the last noted flip asked for (1 at 60 fps, 2 at 30): the
 * presenter's frame pacing leaves the title's own 30 fps stretches alone. */
uint32_t xbox_HostFlipIntervalAsked(void);

/* The session's frame times, flip to flip, counted at every flip (a pause is
 * a gap, not a frame). Cheap enough to be always on: one histogram slot a
 * flip. For the bug report (xbox_report.h). */
typedef struct xbox_flip_stats {
    uint64_t flips;           /* flips since start */
    uint64_t intervals;       /* frame times counted */
    uint32_t pauses;
    double   span_s;          /* first flip to last */
    double   mean_ms, p50_ms, p90_ms, p95_ms, p99_ms, max_ms;
    uint32_t late;            /* longer than 1.5 x the median */
    uint32_t over[5];         /* longer than 20, 25, 34, 50, 100 ms */
    uint32_t recent;          /* of the last frames (the flip ring): */
    double   recent_p50_ms, recent_p95_ms, recent_max_ms;
} xbox_flip_stats;
void xbox_HostFlipStats(xbox_flip_stats *out);

/* fn() is called by the executor while it holds for a pause, every tick and
 * outside the lock: work that must run on the executor's thread while the
 * title is stopped (a frame capture for the bug report). One; NULL: none. */
void xbox_HostPauseHoldFn(void (*fn)(void));
/* A second such call, for the GPU backend (work on its own tables while the
 * title is stopped: graphics.ui_scale prepares the loaded 2D textures when
 * the setting is switched on in the overlay). One; NULL: none. */
void xbox_HostPauseIdleFn(void (*fn)(void));

/* fn(paused) is called, after the flag changes, on every xbox_HostPause that
 * changes it. At most 4; call before the guest starts. */
void xbox_HostPauseListener(void (*fn)(int paused));

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_PAUSE_H */

/*
 * host_time.h - a monotonic nanosecond clock and a sleep that honours it.
 *
 * The kernel's vblank schedule, the frame-pacing trace and a title's own
 * pacers (BLiNX 2's BlockUntilVerticalBlank) all need sub-millisecond time.
 * Sleep() is whole milliseconds and, on native Windows without
 * timeBeginPeriod, rounds to the 15.6 ms system tick; a schedule run on it
 * fires vblanks milliseconds off their edges.
 *
 * xbox_HostNowNs reads the same performance counter on Windows that the
 * kernel's millisecond clock reads, so ms and ns deadlines compare on one
 * clock. xbox_HostSleepNs may return early or late (signals, Wine's timer
 * server): callers re-read the clock against their absolute deadline rather
 * than trusting it.
 */
#ifndef XBOX_HOST_TIME_H
#define XBOX_HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds on a monotonic clock with an arbitrary epoch. */
uint64_t xbox_HostNowNs(void);

/* Sleep about `ns` nanoseconds; 0 returns at once. */
void xbox_HostSleepNs(uint64_t ns);

/* xbox_HostSleepNs for the one thread that sleeps here (the kernel's timer
 * thread), ended early by xbox_HostSleepWake from any thread. A wake that
 * comes while it is not asleep ends its next sleep at once. */
void xbox_HostSleepWakeableNs(uint64_t ns);
void xbox_HostSleepWake(void);
/* 1 when a xbox_HostSleepWake came that no wakeable sleep has taken yet:
 * for the sleeper's busy-waits (take it with xbox_HostSleepWakeableNs(0)).
 * One load; may be a moment behind the wake. */
int  xbox_HostSleepWakePending(void);

/* For a thread that keeps a clock with xbox_HostSleepNs (the kernel's timer
 * thread): ask the host to wake it on time. macOS wakes an ordinary thread's
 * nanosleep 3.7 ms late on average (8 ms at worst) even on an idle machine,
 * at any QoS class, which put the 600-vblank windows at 13.4-20.0 ms; a
 * time-constraint (real-time) thread wakes within ~20 us. Nothing elsewhere.
 * Call on the thread itself. */
void xbox_HostTimerThreadInit(void);

/* ── A gap in the run: the machine slept, or the process was stopped ──
 *
 * xbox_HostWakeTick is called by a thread that wakes every few milliseconds
 * (the kernel's timer thread, each pass). When a second or more went by
 * between two calls -- on the host clock (a stopped process; a sleep on
 * macOS and Windows, whose clocks count it) or on the clock that counts the
 * machine's sleep where the host clock does not (Linux: CLOCK_BOOTTIME) --
 * it says so in the log once and tells the listeners, on that thread: they
 * set a flag and return (the audio sink closes its device and opens the
 * default output anew, the Vulkan presenter makes a new swapchain).
 *
 * The guest's clocks are not held back over the gap, as they are not over
 * the overlay's pause (kernel_pause.h): the vblank schedule restarts from
 * now when it finds itself more than 100 ms behind, periodic timers are due
 * one period from now, the APU's frame clock restarts, and a title that
 * steps by elapsed time is bounded by its own catch-up limit. On Linux the
 * host clock does not count a sleep at all. */
void xbox_HostWakeTick(void);
/* fn(gap_ns) at each gap; `what` is the listener's part of the log line
 * ("the audio queue"). At most 4. Any thread, before the gap. */
void xbox_HostWakeListener(const char *what, void (*fn)(uint64_t gap_ns));

/* ── The guest's clock, and a run that does not wait for it ──
 *
 * Everything the title can read time from (the vblank schedule, rdtsc and
 * the performance counter, KeTickCount, the system and interrupt time, the
 * kernel's timers and delays, the APU's frame clock, a scripted pad's
 * presses) is the host's monotonic clock plus one process-wide skew. The
 * skew is 0 and stays 0 unless the host program calls xbox_TimeWarpEnable:
 * then a flip that would sit out the time to its vblank moves the clock to
 * that vblank instead (xbox_TimeWarpTo, from xbox_VblankWaitUntil), and the
 * title goes on at once. Time the title spends working passes as it does
 * anyway; only the waiting is skipped, on every clock at the same moment,
 * so to the title each frame still took its 16.7 or 33.3 ms.
 *
 * It is for a title that plays by itself with nobody watching (a shader
 * pre-build's scenes, xbox_autoplay.h). No setting or environment variable
 * turns it on.
 *
 * What it does not move: work the host does for the title on its own
 * threads (file reads, decoding), which takes the wall time it takes and so
 * looks slower to a title whose clock runs ahead. max_speed bounds that: a
 * vblank is never given sooner than 1/max_speed of its period after the
 * last one, so the guest clock runs at most max_speed times the host's
 * (0: no bound). */
void     xbox_TimeWarpEnable(unsigned max_speed);
int      xbox_TimeWarpOn(void);
/* What has been skipped so far, in ns (0 without a warp). */
uint64_t xbox_GuestSkewNs(void);
/* xbox_HostNowNs() + xbox_GuestSkewNs(). */
uint64_t xbox_GuestNowNs(void);
/* Move the guest clock to guest_ns if it is not there yet, first sleeping
 * out what max_speed asks for, and wake the threads asleep in xbox_GuestSleepUntilNs. Does
 * nothing without a warp. */
void     xbox_TimeWarpTo(uint64_t guest_ns);
/* Sleep until the guest clock reads guest_ns, however it gets there. For a
 * thread that keeps a schedule on the guest clock (the kernel's timer
 * thread, the APU's frame thread). Without a warp it is one xbox_HostSleepNs
 * of the difference, which may return early. */
void     xbox_GuestSleepUntilNs(uint64_t guest_ns);
/* Sleep until `ns` have passed on the guest clock: a title's own delay. */
void     xbox_GuestSleepNs(uint64_t ns);
/* Since the warp was enabled: guest time skipped and the times it was, for
 * a log line. */
void     xbox_TimeWarpStats(uint64_t *skipped_ns, uint64_t *jumps);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_HOST_TIME_H */

/*
 * kernel_wake.h - a woken thread of higher priority runs first (BO-221).
 *
 * The Xbox has one CPU and a priority scheduler: a thread that signals an
 * event on which a thread of higher priority waits is preempted at once, and
 * gets the CPU back only when that thread blocks again. Titles count on it.
 * Burnout 3's RenderWare file worker (sub_001B3CA0, created one priority
 * step above the main thread) takes each command the main thread posts
 * before the main thread runs on; the main thread's cancel of a custom
 * soundtrack song therefore never finds the song's open command still
 * queued. Here the guest threads run side by side (or share one core under
 * the host's own scheduler), the worker could take the cancel first, free
 * the file slot, the main thread gave that slot to the next file it opened,
 * and the song player then closed it: a stream kept reading through the
 * freed slot, which the front-end bank fe.awd got next (BO-221 hang/crash).
 *
 * So the signaller waits, after the signal, until the woken thread makes
 * its next call that can block (a wait, a delay or yield, file I/O, a
 * critical section), bounded by a few milliseconds. Only between guest
 * threads, only towards a higher guest priority (KeSetBasePriorityThread),
 * only for single-object waits. RECOMP_WAKE_HANDOFF=0 turns it off.
 */
#ifndef XBOX_KERNEL_WAKE_H
#define XBOX_KERNEL_WAKE_H

#include <stdint.h>

/* The calling thread is a guest thread with this thread object. */
void xbox_WakeGuestThread(uint32_t thobj);
/* A thread object that is (re)used for a new thread: priority 0. */
void xbox_WakeNewThreadObject(uint32_t thobj);
/* KeSetBasePriorityThread on a thread object. */
void xbox_WakeSetPriority(uint32_t thobj, int32_t increment);
/* The calling thread makes a call that can block (kernel dispatch). */
void xbox_WakeBlockingCall(void);
/* The calling thread waits on one object / is back from that wait. */
void xbox_WakeWaitBegin(const void *object, uint32_t guest_ret);
void xbox_WakeWaitEnd(void);
/* The calling thread has just signalled the object. */
void xbox_WakeSignalled(const void *object, uint32_t guest_ret);
/* Is the kernel ordinal one that can block? */
int  xbox_WakeOrdinalBlocks(uint32_t ordinal);
/* Hand-offs done, and those that ran into the time bound. */
void xbox_WakeCounts(uint64_t *handoffs, uint64_t *timeouts);
/* Tests: the bound, in nanoseconds (default 3 ms); 0/1 turns it off/on. */
void xbox_WakeConfigure(int on, uint64_t bound_ns);

#endif

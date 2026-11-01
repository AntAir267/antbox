/*
 * frame_wake.h - how a published frame reaches the thread that presents it,
 * without SDL's cross-thread wake (BO-210).
 *
 * The ack thread publishes a frame into the latest-frame slot. It used to
 * push one SDL user event to wake the main loop, which slept in
 * SDL_WaitEventTimeout and presented. Under gamescope that wake goes through
 * Xwayland (SDL's X11 driver sends a ClientMessage to its own window and
 * waits on the X socket). On the Deck, after a long session, it stopped
 * arriving per frame: the executor flipped 60 a second, the main loop
 * picked up ~35 (the rest were replaced in the slot unseen), every pickup
 * came on a frame wake, the present calls took 2.4 ms at most and every
 * present was shown. The main thread slept in XCheckIfEvent. A first fix
 * (the main loop waited on the slot's condition variable, then read SDL's
 * events) did not survive a pump that itself blocks ~29 ms a turn: the
 * pickup still came after the pump.
 *
 * Now the window's layer presenters (Vulkan, Metal) present on a thread of
 * their own (fb_present_sdl.c, "present"). It waits on the slot's condition
 * variable for work: a new frame (the slot's sequence moved) or a request
 * from the main thread (FW_CTL_*: quit, redraw). Both are state read under
 * the slot's mutex, not a count of signals, so a signal before the wait, a
 * spurious wake or two publishes in one wait lose nothing. The main thread
 * keeps SDL's events, input and the window; nothing it does sits between a
 * publish and its present.
 *
 * fw_wait_new and fw_flowing are the main loop's wait on the SDL renderer
 * path (no layer), which still presents on the main thread.
 *
 * Header-only, for fb_present_sdl.c and tests/frame_wake.
 */
#ifndef XBOX_FRAME_WAKE_H
#define XBOX_FRAME_WAKE_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define FW_TICK_NS     2000000ull     /* longest wait for a frame while they come */
#define FW_FLOWING_NS  100000000ull   /* frames come: one published this recently */

/* An absolute CLOCK_REALTIME time ns from now (pthread_cond_timedwait's
 * clock on every platform we build for). */
static inline void fw_deadline(struct timespec *ts, uint64_t ns)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(ns / 1000000000ull);
    ts->tv_nsec += (long)(ns % 1000000000ull);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* mu held. Waits until *seq differs from seen (a frame was published) or
 * timeout_ns passed. 1: a new frame. */
static inline int fw_wait_new(pthread_mutex_t *mu, pthread_cond_t *cv,
                              const uint64_t *seq, uint64_t seen, uint64_t timeout_ns)
{
    struct timespec ts;

    if (*seq != seen)
        return 1;
    fw_deadline(&ts, timeout_ns);
    while (*seq == seen)
        if (pthread_cond_timedwait(cv, mu, &ts) == ETIMEDOUT)
            break;
    return *seq != seen;
}

/* Frames come: the last was published under FW_FLOWING_NS ago. */
static inline int fw_flowing(uint64_t last_pub_ns, uint64_t now_ns)
{
    return last_pub_ns && now_ns >= last_pub_ns && now_ns - last_pub_ns < FW_FLOWING_NS;
}

/* Requests for the present thread, in a word read under the slot's mutex. */
#define FW_CTL_QUIT   1u      /* the window is closing: return */
#define FW_CTL_DIRTY  2u      /* present again without a new frame (expose,
                               * resize, a shot, the overlay) */

#define FW_WORK_NONE  0       /* the timeout passed */
#define FW_WORK_FRAME 1       /* a new frame (and maybe a request too) */
#define FW_WORK_CTL   2       /* a request only */

/* mu held. Waits until there is work: *seq differs from seen, or *ctl has a
 * bit set, or timeout_ns passed (0: only looks). The predicate is checked
 * before the first wait and after every wake, so a publish before the wait,
 * a spurious wake or several signals in one wait change nothing. The caller
 * takes the requests (clears *ctl) under the same mutex. */
static inline int fw_wait_work(pthread_mutex_t *mu, pthread_cond_t *cv,
                               const uint64_t *seq, uint64_t seen,
                               const unsigned *ctl, uint64_t timeout_ns)
{
    struct timespec ts;
    int timed_out = 0;

    if (*seq == seen && !*ctl && timeout_ns) {
        fw_deadline(&ts, timeout_ns);
        while (*seq == seen && !*ctl && !timed_out)
            timed_out = pthread_cond_timedwait(cv, mu, &ts) == ETIMEDOUT;
    }
    return *seq != seen ? FW_WORK_FRAME : *ctl ? FW_WORK_CTL : FW_WORK_NONE;
}

#endif /* XBOX_FRAME_WAKE_H */

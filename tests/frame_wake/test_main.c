/*
 * frame_wake test (BO-210): a publisher at 60 a second into a latest-frame
 * slot, as the ack thread does (fb_present_sdl.c publish), and a main loop
 * that takes frames. The SDL wake the publisher sends is lost every time,
 * as on the Deck after a long session (the loop then woke only by its
 * 100 ms timeout or other events). With the condition-variable wait of
 * frame_wake.h the loop still takes every frame; the old wait, which
 * depends on that wake, takes ~1 in 6.
 */
#include "frame_wake.h"

#include <stdio.h>
#include <string.h>

#define FRAMES   120
#define FRAME_NS 16666667ull

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static uint64_t        seq;              /* frames published (s_ready_seq) */
static volatile uint64_t last_pub;
static volatile int    done;

static uint64_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
    uint64_t now;
    while ((now = mono()) < t) {
        struct timespec ts = { 0, (long)(t - now) };
        nanosleep(&ts, NULL);
    }
}

static void *publisher(void *arg)
{
    uint64_t t0 = mono() + 20000000ull;
    int i;
    (void)arg;
    for (i = 0; i < FRAMES; i++) {
        sleep_until(t0 + (uint64_t)i * FRAME_NS);
        pthread_mutex_lock(&mu);
        seq++;
        __atomic_store_n(&last_pub, mono(), __ATOMIC_RELEASE);
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&mu);
        /* The SDL wake (SDL_PushEvent) would go here: lost. */
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* One run. new_wait: frame_wake.h; else the old SDL wait with the wake lost
 * (only its 100 ms timeout ends it). Returns frames taken. */
static int run(int new_wait, uint64_t *max_late_ns)
{
    pthread_t th;
    uint64_t shown = 0, picks = 0, wait_ns = 100000000ull;
    seq = 0;
    last_pub = 0;
    done = 0;
    *max_late_ns = 0;
    pthread_create(&th, NULL, publisher, NULL);
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE) || seq != shown) {
        uint64_t s, now = mono();
        if (new_wait && fw_flowing(__atomic_load_n(&last_pub, __ATOMIC_ACQUIRE), now)) {
            pthread_mutex_lock(&mu);
            fw_wait_new(&mu, &cv, &seq, shown, FW_TICK_NS);
            pthread_mutex_unlock(&mu);
        } else if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
            /* SDL_WaitEventTimeout(100) with no event ever delivered; in the
             * new loop this is the idle wait before the first frame, which
             * the first frame's event would end: shortened to 20 ms so the
             * test does not start late. */
            struct timespec ts = { 0, (long)(new_wait ? 20000000ull : wait_ns) };
            nanosleep(&ts, NULL);
        }
        pthread_mutex_lock(&mu);
        s = seq;
        pthread_mutex_unlock(&mu);
        if (s != shown) {
            uint64_t late = mono() - __atomic_load_n(&last_pub, __ATOMIC_ACQUIRE);
            if (shown && late > *max_late_ns)
                *max_late_ns = late;
            shown = s;
            picks++;
        }
    }
    pthread_join(th, NULL);
    return (int)picks;
}

/* ── the present thread's wait (fw_wait_work) ── */

static unsigned ctl;                     /* FW_CTL_* (s_pt_ctl) */

enum { POKE_SPURIOUS, POKE_PUBLISH, POKE_QUIT };
struct poke { int what; uint64_t after_ns; };

/* After `after_ns`: signal with nothing changed (many times), publish one
 * frame, or ask the waiter to quit. */
static void *poker(void *arg)
{
    struct poke *p = (struct poke *)arg;
    uint64_t t0 = mono();
    sleep_until(t0 + p->after_ns);
    if (p->what == POKE_SPURIOUS) {
        int i;
        for (i = 0; i < 20; i++) {
            pthread_mutex_lock(&mu);
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            sleep_until(mono() + 1000000ull);
        }
        return NULL;
    }
    pthread_mutex_lock(&mu);
    if (p->what == POKE_PUBLISH)
        seq++;
    else
        ctl |= FW_CTL_QUIT;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* One wait of `timeout_ns` with a poker started first. */
static int one_wait(int what, uint64_t after_ns, uint64_t timeout_ns, uint64_t *took)
{
    pthread_t th;
    struct poke p = { what, after_ns };
    uint64_t t0;
    int r;
    seq = 0;
    ctl = 0;
    pthread_create(&th, NULL, poker, &p);
    pthread_mutex_lock(&mu);
    t0 = mono();
    r = fw_wait_work(&mu, &cv, &seq, 0, &ctl, timeout_ns);
    *took = mono() - t0;
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
    return r;
}

/* A present thread takes frames at 60 a second while the main thread's
 * event pump stalls 29 ms every turn (RECOMP_DEBUG=pump_stall=29): the
 * stall is on another thread, so every frame is taken. Returns frames
 * taken; the present thread quits on FW_CTL_QUIT once the frames are done. */
static volatile int pump_turns;
static void *stalled_pump(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        sleep_until(mono() + 29000000ull);
        pump_turns++;
    }
    pthread_mutex_lock(&mu);
    ctl |= FW_CTL_QUIT;                  /* the window closes */
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static int run_present_thread(uint64_t *max_late_ns)
{
    pthread_t pub, pump;
    uint64_t shown = 0, picks = 0;
    seq = 0;
    ctl = 0;
    last_pub = 0;
    done = 0;
    pump_turns = 0;
    *max_late_ns = 0;
    pthread_create(&pub, NULL, publisher, NULL);
    pthread_create(&pump, NULL, stalled_pump, NULL);
    for (;;) {
        int w;
        uint64_t s;
        pthread_mutex_lock(&mu);
        w = fw_wait_work(&mu, &cv, &seq, shown, &ctl, 100000000ull);
        s = seq;
        if (ctl & FW_CTL_QUIT) {
            pthread_mutex_unlock(&mu);
            break;
        }
        pthread_mutex_unlock(&mu);
        if (w == FW_WORK_FRAME) {
            uint64_t late = mono() - __atomic_load_n(&last_pub, __ATOMIC_ACQUIRE);
            if (late > *max_late_ns)
                *max_late_ns = late;
            shown = s;
            picks++;
            /* The present itself: a couple of ms. */
            sleep_until(mono() + 2000000ull);
        }
    }
    pthread_join(pub, NULL);
    pthread_join(pump, NULL);
    return (int)picks;
}

int main(void)
{
    int fails = 0, picks;
    uint64_t late;
    pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c2 = PTHREAD_COND_INITIALIZER;
    uint64_t q = 5, t0, dt;

    /* A frame already there: no wait. */
    pthread_mutex_lock(&m2);
    t0 = mono();
    if (!fw_wait_new(&m2, &c2, &q, 4, FW_TICK_NS) || mono() - t0 > 1000000ull) {
        printf("FAIL: a frame already published was waited for\n");
        fails++;
    }
    /* None comes: back after about the tick. */
    t0 = mono();
    if (fw_wait_new(&m2, &c2, &q, 5, FW_TICK_NS)) {
        printf("FAIL: a frame seen with none published\n");
        fails++;
    }
    dt = mono() - t0;
    pthread_mutex_unlock(&m2);
    if (dt < 1500000ull || dt > 50000000ull) {
        printf("FAIL: the tick took %.2f ms (want ~%.0f)\n", dt / 1e6, FW_TICK_NS / 1e6);
        fails++;
    }
    if (fw_flowing(0, 1) || !fw_flowing(1000, 1000 + FW_FLOWING_NS - 1)
            || fw_flowing(1000, 1000 + FW_FLOWING_NS)) {
        printf("FAIL: fw_flowing\n");
        fails++;
    }

    picks = run(1, &late);
    printf("condition-variable wait, SDL wake lost: %d of %d frames taken, latest %.2f ms"
           " after its publish at most\n", picks, FRAMES, late / 1e6);
    if (picks < FRAMES - 2) {
        printf("FAIL: frames replaced unseen with the new wait\n");
        fails++;
    }
    picks = run(0, &late);
    printf("old SDL wait, SDL wake lost: %d of %d frames taken\n", picks, FRAMES);
    if (picks > FRAMES / 3) {
        printf("FAIL: the old wait did not show the loss (the test models nothing)\n");
        fails++;
    }
    {
        uint64_t took;
        int r;
        /* Work already there: no wait. */
        seq = 3;
        ctl = 0;
        pthread_mutex_lock(&mu);
        t0 = mono();
        r = fw_wait_work(&mu, &cv, &seq, 2, &ctl, 50000000ull);
        took = mono() - t0;
        pthread_mutex_unlock(&mu);
        if (r != FW_WORK_FRAME || took > 1000000ull) {
            printf("FAIL: present wait: a frame already there was waited for\n");
            fails++;
        }
        /* Spurious wakes (signals, nothing changed) do not end the wait. */
        r = one_wait(POKE_SPURIOUS, 2000000ull, 60000000ull, &took);
        printf("present wait: 20 spurious wakes, back after %.1f ms (timeout 60): %d\n",
               took / 1e6, r);
        if (r != FW_WORK_NONE || took < 55000000ull) {
            printf("FAIL: a spurious wake ended the present thread's wait\n");
            fails++;
        }
        /* A publish during the wait ends it at once. */
        r = one_wait(POKE_PUBLISH, 10000000ull, 1000000000ull, &took);
        printf("present wait: publish at 10 ms, back after %.2f ms: %d\n", took / 1e6, r);
        if (r != FW_WORK_FRAME || took > 30000000ull) {
            printf("FAIL: a publish during the wait was not taken at once\n");
            fails++;
        }
        /* Shutdown with no frame ever published. */
        r = one_wait(POKE_QUIT, 10000000ull, 1000000000ull, &took);
        printf("present wait: quit at 10 ms, no frames, back after %.2f ms: %d\n",
               took / 1e6, r);
        if (r != FW_WORK_CTL || took > 30000000ull) {
            printf("FAIL: quit did not wake the present thread without frames\n");
            fails++;
        }
        picks = run_present_thread(&late);
        printf("present thread, the pump stalling 29 ms a turn (%d turns): %d of %d frames"
               " taken, latest %.2f ms after its publish at most\n", pump_turns, picks,
               FRAMES, late / 1e6);
        if (picks != FRAMES) {
            printf("FAIL: the present thread lost frames to a stalled pump\n");
            fails++;
        }
    }
    printf(fails ? "frame_wake: %d FAILED\n" : "frame_wake: all passed\n", fails);
    return fails != 0;
}

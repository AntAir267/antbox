/*
 * host_time.c - see host_time.h.
 *
 * Windows (and Wine): QueryPerformanceCounter, and a high-resolution
 * waitable timer per thread (Windows 10 1803+; Wine has it too), which waits
 * to the 100 ns unit where Sleep rounds to the system tick. Without one,
 * Sleep in whole milliseconds, rounded up.
 * POSIX: CLOCK_MONOTONIC and nanosleep.
 */
#include "host_time.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
/* Not xbox_winnt.h's XBOX_THREAD_LOCAL: that header is the POSIX build's
 * Win32 vocabulary and is not included on Windows here. */
#if defined(_MSC_VER)
#define HOST_THREAD_LOCAL __declspec(thread)
#else
#define HOST_THREAD_LOCAL _Thread_local
#endif
#else
#include <time.h>
#include <pthread.h>
#ifdef __APPLE__
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#endif

uint64_t xbox_HostNowNs(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    /* Split so the product never overflows: QPC runs at 10 MHz under Wine
     * and on most hosts, and t * 1e9 would wrap after 29 minutes. */
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000ull
           + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000ull
             / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void xbox_HostSleepNs(uint64_t ns)
{
    if (!ns)
        return;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    {
        static HOST_THREAD_LOCAL HANDLE timer;
        static HOST_THREAD_LOCAL int timer_failed;

        if (!timer && !timer_failed) {
            timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
            timer_failed = !timer;
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
                return;
        }
        Sleep((DWORD)((ns + 999999u) / 1000000u));
    }
#else
    {
        struct timespec ts = { (time_t)(ns / 1000000000u),
                               (long)(ns % 1000000000u) };
        nanosleep(&ts, NULL);
    }
#endif
}

/* ── The wakeable sleep (one sleeper) ────────────────────── */

/* A wake not yet taken, for xbox_HostSleepWakePending: set before the wake
 * is posted, cleared by the sleep that takes it. */
static volatile int s_wake_pending;

int xbox_HostSleepWakePending(void)
{
    return s_wake_pending;
}

#ifdef _WIN32
static HANDLE s_wake_ev;
static INIT_ONCE s_wake_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wake_ev_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    s_wake_ev = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    return TRUE;
}

void xbox_HostSleepWakeableNs(uint64_t ns)
{
    static HOST_THREAD_LOCAL HANDLE timer;
    static HOST_THREAD_LOCAL int timer_failed;

    InitOnceExecuteOnce(&s_wake_once, wake_ev_init, NULL, NULL);
    if (!s_wake_ev) {
        xbox_HostSleepNs(ns);
        return;
    }
    s_wake_pending = 0;
    if (!ns) {
        WaitForSingleObject(s_wake_ev, 0);      /* take a pending wake */
        return;
    }
    if (!timer && !timer_failed) {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        timer_failed = !timer;
    }
    if (timer) {
        LARGE_INTEGER due;
        HANDLE h[2];
        due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);
        h[0] = s_wake_ev;
        h[1] = timer;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0)
                CancelWaitableTimer(timer);
            return;
        }
    }
    WaitForSingleObject(s_wake_ev, (DWORD)((ns + 999999u) / 1000000u));
}

void xbox_HostSleepWake(void)
{
    InitOnceExecuteOnce(&s_wake_once, wake_ev_init, NULL, NULL);
    s_wake_pending = 1;
    if (s_wake_ev)
        SetEvent(s_wake_ev);
}
#else
static pthread_mutex_t s_wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake_cv;
static pthread_once_t s_wake_once = PTHREAD_ONCE_INIT;
static int s_wake_set;

static void wake_cv_init(void)
{
    pthread_condattr_t a;

    pthread_condattr_init(&a);
#ifndef __APPLE__
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&s_wake_cv, &a);
    pthread_condattr_destroy(&a);
}

void xbox_HostSleepWakeableNs(uint64_t ns)
{
    pthread_once(&s_wake_once, wake_cv_init);
    pthread_mutex_lock(&s_wake_mu);
    if (!s_wake_set && ns) {
#ifdef __APPLE__
        struct timespec rel = { (time_t)(ns / 1000000000u),
                                (long)(ns % 1000000000u) };
        pthread_cond_timedwait_relative_np(&s_wake_cv, &s_wake_mu, &rel);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(ns / 1000000000u);
        ts.tv_nsec += (long)(ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s_wake_cv, &s_wake_mu, &ts);
#endif
    }
    s_wake_set = 0;
    s_wake_pending = 0;
    pthread_mutex_unlock(&s_wake_mu);
}

void xbox_HostSleepWake(void)
{
    pthread_once(&s_wake_once, wake_cv_init);
    pthread_mutex_lock(&s_wake_mu);
    s_wake_set = 1;
    s_wake_pending = 1;
    pthread_cond_signal(&s_wake_cv);
    pthread_mutex_unlock(&s_wake_mu);
}
#endif

void xbox_HostTimerThreadInit(void)
{
#ifdef __APPLE__
    /* A 60 Hz period. The computation budget is generous because the timer
     * thread runs guest ISRs and DPCs between sleeps, and it is preemptible.
     * XNU demotes a real-time thread only after seconds of continuous work
     * past its budget, so a guest routine that spins on this thread can hold
     * a core for that long before the scheduler steps in. Refused (a
     * sandbox, a future macOS), the thread stays a normal one: the vblank
     * grid keeps its rate and only the wake-up jitter is the old one. */
    mach_timebase_info_data_t tb;
    thread_time_constraint_policy_data_t p;
    double per_ms;

    if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.numer)
        return;
    per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    p.period = (uint32_t)(per_ms * 1000.0 / 60.0);
    p.computation = (uint32_t)(per_ms * 2.0);
    p.constraint = (uint32_t)(per_ms * 8.0);
    p.preemptible = 1;
    {
        kern_return_t kr = thread_policy_set(
            pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (kr != KERN_SUCCESS)
            fprintf(stderr, "[KERNEL] timer thread: time-constraint policy refused"
                    " (%d); vblank wake-ups may run late by a few ms\n", (int)kr);
    }
#endif
}

/* ── A gap in the run ─────────────────────────────────────── */

#define WAKE_GAP_NS 1000000000ull
#define WAKE_LISTENERS 4
static struct { const char *what; void (*fn)(uint64_t); } s_wake_l[WAKE_LISTENERS];
static volatile int s_wake_n;

/* Time the machine has spent asleep, where the host clock does not count
 * it; 0 where it does (macOS's CLOCK_MONOTONIC, Windows's performance
 * counter) or where there is no such clock. */
static uint64_t slept_ns(void)
{
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
    struct timespec b, m;
    uint64_t bn, mn;

    if (clock_gettime(CLOCK_BOOTTIME, &b) || clock_gettime(CLOCK_MONOTONIC, &m))
        return 0;
    bn = (uint64_t)b.tv_sec * 1000000000ull + (uint64_t)b.tv_nsec;
    mn = (uint64_t)m.tv_sec * 1000000000ull + (uint64_t)m.tv_nsec;
    return bn > mn ? bn - mn : 0;
#else
    return 0;
#endif
}

void xbox_HostWakeListener(const char *what, void (*fn)(uint64_t gap_ns))
{
    int n = s_wake_n;

    if (n >= WAKE_LISTENERS)
        return;
    s_wake_l[n].what = what;
    s_wake_l[n].fn = fn;
    s_wake_n = n + 1;
}

void xbox_HostWakeTick(void)
{
    static uint64_t last_ns, last_slept;
    uint64_t now = xbox_HostNowNs(), slept = slept_ns();
    uint64_t run_gap = last_ns ? now - last_ns : 0;
    uint64_t sleep_gap = last_ns && slept > last_slept ? slept - last_slept : 0;
    int n = s_wake_n, i;

    last_ns = now;
    last_slept = slept;
    if (run_gap < WAKE_GAP_NS && sleep_gap < WAKE_GAP_NS)
        return;
    {
        char did[256];
        size_t len = 0;

        did[0] = 0;
        for (i = 0; i < n; i++) {
            int w = snprintf(did + len, sizeof did - len, "%s%s", i ? ", " : "",
                             s_wake_l[i].what);
            if (w < 0 || (size_t)w >= sizeof did - len)
                break;
            len += (size_t)w;
        }
        fprintf(stderr, "[HOST] a gap of %.1f s in the run (%s): the vblank"
                " schedule and the timers go on from now%s%s\n",
                (double)(run_gap > sleep_gap ? run_gap : sleep_gap) / 1e9,
                sleep_gap >= WAKE_GAP_NS ? "the machine slept"
                    : "the machine slept or the process was stopped",
                n ? "; started again: " : "", did);
        fflush(stderr);
    }
    for (i = 0; i < n; i++)
        s_wake_l[i].fn(run_gap > sleep_gap ? run_gap : sleep_gap);
}

/* ── The guest's clock ────────────────────────────────────── */

/* The skew is read on every guest clock read, from any thread, without the
 * lock: a 64-bit load, atomic on the 64-bit hosts this runs on. It only
 * grows, and only under the lock. */
static volatile uint64_t s_skew_ns;
static volatile int s_warp;
static unsigned s_warp_max;             /* 0: no bound */
static uint64_t s_warp_grant_ns;        /* host time of the last jump */
static uint64_t s_warp_jumps;

#ifdef _WIN32
static CRITICAL_SECTION s_warp_mu;
static CONDITION_VARIABLE s_warp_cv;
#define warp_lock()    EnterCriticalSection(&s_warp_mu)
#define warp_unlock()  LeaveCriticalSection(&s_warp_mu)
#define warp_wake()    WakeAllConditionVariable(&s_warp_cv)
#else
static pthread_mutex_t s_warp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_warp_cv = PTHREAD_COND_INITIALIZER;
#define warp_lock()    pthread_mutex_lock(&s_warp_mu)
#define warp_unlock()  pthread_mutex_unlock(&s_warp_mu)
#define warp_wake()    pthread_cond_broadcast(&s_warp_cv)
#endif

void xbox_TimeWarpEnable(unsigned max_speed)
{
#ifdef _WIN32
    if (!s_warp) {
        InitializeCriticalSection(&s_warp_mu);
        InitializeConditionVariable(&s_warp_cv);
    }
#endif
    s_warp_max = max_speed;
    s_warp = 1;
}

int xbox_TimeWarpOn(void)
{
    return s_warp;
}

uint64_t xbox_GuestSkewNs(void)
{
    return s_skew_ns;
}

uint64_t xbox_GuestNowNs(void)
{
    return xbox_HostNowNs() + s_skew_ns;
}

void xbox_TimeWarpTo(uint64_t guest_ns)
{
    uint64_t host, now;

    if (!s_warp)
        return;
    warp_lock();
    host = xbox_HostNowNs();
    now = host + s_skew_ns;
    if (guest_ns > now) {
        uint64_t d = guest_ns - now;
        if (s_warp_max) {
            /* No sooner than 1/max of what is skipped after the last jump:
             * the guest clock then runs at most max times the host's. */
            uint64_t earliest = s_warp_grant_ns + d / s_warp_max;
            if (earliest > host && earliest - host < d) {
                warp_unlock();
                xbox_HostSleepNs(earliest - host);
                warp_lock();
                host = xbox_HostNowNs();
                now = host + s_skew_ns;
            }
        }
        if (guest_ns > now) {
            s_skew_ns += guest_ns - now;
            s_warp_jumps++;
        }
        s_warp_grant_ns = host;
    }
    warp_unlock();
    warp_wake();
}

void xbox_GuestSleepUntilNs(uint64_t guest_ns)
{
    uint64_t now;

    if (!s_warp) {
        now = xbox_HostNowNs();
        if (guest_ns > now)
            xbox_HostSleepNs(guest_ns - now);
        return;
    }
    /* The clock is only moved under the lock, so a jump cannot slip in
     * between the look at it and the wait. */
    warp_lock();
    while ((now = xbox_HostNowNs() + s_skew_ns) < guest_ns) {
        uint64_t d = guest_ns - now;
#ifdef _WIN32
        /* Whole milliseconds, rounded up: a sleeper is woken by the jump it
         * waits for, and is otherwise late by under a millisecond. */
        SleepConditionVariableCS(&s_warp_cv, &s_warp_mu, (DWORD)((d + 999999u) / 1000000u));
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(d / 1000000000u);
        ts.tv_nsec += (long)(d % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s_warp_cv, &s_warp_mu, &ts);
#endif
    }
    warp_unlock();
}

void xbox_GuestSleepNs(uint64_t ns)
{
    if (!s_warp)
        xbox_HostSleepNs(ns);
    else
        xbox_GuestSleepUntilNs(xbox_GuestNowNs() + ns);
}

void xbox_TimeWarpStats(uint64_t *skipped_ns, uint64_t *jumps)
{
    *skipped_ns = s_skew_ns;
    *jumps = s_warp_jumps;
}

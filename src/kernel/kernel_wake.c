/*
 * kernel_wake.c - a woken thread of higher priority runs first (BO-221).
 * See kernel_wake.h for why.
 */
#include "kernel_wake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "platform/host_time.h"

#ifndef RECOMP_TLS
#  if defined(_MSC_VER)
#    define RECOMP_TLS __declspec(thread)
#  else
#    define RECOMP_TLS _Thread_local
#  endif
#endif

#define WAKE_MAX 128

typedef struct {
    uint32_t thobj;              /* 0 = free entry */
    int32_t prio;                /* the guest base priority increment */
    uint32_t epoch;              /* + 1 at every call that can block */
    const void *waiting_on;      /* the object of a single-object wait */
    uint32_t watched;            /* signallers waiting for its next block */
    uint32_t waits;              /* single-object waits begun */
    uint32_t wait_ret;           /* the guest caller of the current wait */
    DWORD tid;                   /* its host thread id (GetCurrentThreadId) */
    uint32_t wakes;              /* ... and ended */
    HANDLE ev;                   /* set at that block while watched */
} wake_rec;

static wake_rec s_rec[WAKE_MAX];
static uint32_t s_nrec;
static volatile long s_lock;
static RECOMP_TLS wake_rec *t_self;
static int s_on = -1;
static uint64_t s_bound_ns = 3000000ull;
static uint64_t s_handoffs, s_timeouts;

static void lock(void)
{
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE))
        SwitchToThread();
}

static void unlock(void) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); }

static int on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_WAKE_HANDOFF");
        s_on = !(e && e[0] == '0');
        if (!s_on) {
            fprintf(stderr, "  [KERNEL] RECOMP_WAKE_HANDOFF=0: a signalled thread"
                            " of higher priority does not run first\n");
            fflush(stderr);
        }
    }
    return s_on;
}

/* The entry of a thread object, made when asked to. */
static wake_rec *rec_of(uint32_t thobj, int make)
{
    uint32_t i, n;
    wake_rec *r = NULL;

    if (!thobj)
        return NULL;
    n = __atomic_load_n(&s_nrec, __ATOMIC_ACQUIRE);
    for (i = 0; i < n; i++)
        if (__atomic_load_n(&s_rec[i].thobj, __ATOMIC_RELAXED) == thobj)
            return &s_rec[i];
    if (!make)
        return NULL;
    lock();
    n = s_nrec;
    for (i = 0; i < n; i++)
        if (s_rec[i].thobj == thobj) {
            r = &s_rec[i];
            break;
        }
    if (!r && n < WAKE_MAX) {
        r = &s_rec[n];
        memset(r, 0, sizeof *r);
        r->thobj = thobj;
        __atomic_store_n(&s_nrec, n + 1, __ATOMIC_RELEASE);
    }
    unlock();
    return r;
}

void xbox_WakeConfigure(int on_, uint64_t bound_ns)
{
    s_on = on_;
    if (bound_ns)
        s_bound_ns = bound_ns;
}

void xbox_WakeGuestThread(uint32_t thobj)
{
    static int said;

    t_self = rec_of(thobj, 1);
    if (t_self)
        t_self->tid = GetCurrentThreadId();
    if (said < 16) {
        said++;
        fprintf(stderr, "  [KERNEL] wake: guest thread %lu has thread object 0x%08X\n",
                (unsigned long)GetCurrentThreadId(), (unsigned)thobj);
        fflush(stderr);
    }
}

void xbox_WakeNewThreadObject(uint32_t thobj)
{
    wake_rec *r = rec_of(thobj, 0);

    if (r) {
        __atomic_store_n(&r->prio, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&r->waiting_on, NULL, __ATOMIC_RELAXED);
    }
}

void xbox_WakeSetPriority(uint32_t thobj, int32_t increment)
{
    wake_rec *r = rec_of(thobj, 1);
    static int said;

    if (r)
        __atomic_store_n(&r->prio, increment, __ATOMIC_RELAXED);
    if (said < 8) {
        said++;
        fprintf(stderr, "  [KERNEL] guest thread object 0x%08X: base priority %+d\n",
                (unsigned)thobj, (int)increment);
        fflush(stderr);
    }
}

void xbox_WakeBlockingCall(void)
{
    wake_rec *r = t_self;

    if (r) {
        __atomic_add_fetch(&r->epoch, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&r->watched, __ATOMIC_SEQ_CST) && r->ev)
            SetEvent(r->ev);
    }
}

void xbox_WakeWaitBegin(const void *object, uint32_t guest_ret)
{
    wake_rec *r = t_self;

    if (r) {
        r->wait_ret = guest_ret;
        __atomic_store_n(&r->waiting_on, object, __ATOMIC_RELEASE);
        __atomic_add_fetch(&r->waits, 1, __ATOMIC_RELAXED);
    }
}

void xbox_WakeWaitEnd(void)
{
    wake_rec *r = t_self;

    if (r) {
        __atomic_store_n(&r->waiting_on, NULL, __ATOMIC_RELEASE);
        __atomic_add_fetch(&r->wakes, 1, __ATOMIC_RELAXED);
    }
}

int xbox_WakeOrdinalBlocks(uint32_t ordinal)
{
    switch (ordinal) {
    case 99:    /* KeDelayExecutionThread */
    case 158:   /* KeWaitForMultipleObjects */
    case 159:   /* KeWaitForSingleObject */
    case 190:   /* NtCreateFile */
    case 202:   /* NtOpenFile */
    case 219:   /* NtReadFile */
    case 233:   /* NtWaitForSingleObject */
    case 234:   /* NtWaitForSingleObjectEx */
    case 235:   /* NtWaitForMultipleObjectsEx */
    case 236:   /* NtWriteFile */
    case 238:   /* NtYieldExecution */
    case 277:   /* RtlEnterCriticalSection: may find it held */
        return 1;
    default:
        return 0;
    }
}

void xbox_WakeSignalled(const void *object, uint32_t guest_ret)
{
#if defined(_WIN32)
    /* A Win32 event does not say which wait took it, so the hand-off could
     * not tell a waiter that woke from one that another thread beat to it. */
    (void)object;
    (void)guest_ret;
#else
    wake_rec *self = t_self, *best = NULL;
    uint32_t i, n, e0;
    uint64_t t0;

    /* Host threads (timers, DPCs, audio) never wait here. */
    if (!self || !object || !on())
        return;
    n = __atomic_load_n(&s_nrec, __ATOMIC_ACQUIRE);
    for (i = 0; i < n; i++) {
        wake_rec *r = &s_rec[i];

        if (r == self || __atomic_load_n(&r->waiting_on, __ATOMIC_ACQUIRE) != object)
            continue;
        if (__atomic_load_n(&r->prio, __ATOMIC_RELAXED) <= __atomic_load_n(&self->prio, __ATOMIC_RELAXED))
            continue;
        if (!best || r->prio > best->prio)
            best = r;
    }
    if (!best)
        return;
    if (!best->ev) {
        lock();
        if (!best->ev)
            best->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        unlock();
        if (!best->ev)
            return;
    }
    /* It entered the wait with a blocking call; the next one is after it
     * ran. Sleep on its event, not a yield: a host scheduler that ranks
     * this thread above it would never let it run. */
    __atomic_add_fetch(&best->watched, 1, __ATOMIC_SEQ_CST);
    e0 = __atomic_load_n(&best->epoch, __ATOMIC_SEQ_CST);
    t0 = xbox_HostNowNs();
    for (;;) {
        DWORD taker;
        int set;

        if (__atomic_load_n(&best->epoch, __ATOMIC_SEQ_CST) != e0) {
            uint64_t k = __atomic_add_fetch(&s_handoffs, 1, __ATOMIC_RELAXED);

            if (k == 1 || k == 1000 || k == 100000) {
                fprintf(stderr, "  [KERNEL] wake hand-off %llu: a signalled thread of priority %d"
                                " ran first (%.3f ms); %llu over the bound so far\n",
                        (unsigned long long)k, (int)best->prio,
                        (double)(xbox_HostNowNs() - t0) / 1e6,
                        (unsigned long long)__atomic_load_n(&s_timeouts, __ATOMIC_RELAXED));
                fflush(stderr);
            }
            break;
        }
        /* Another thread's wait took the event: this one stays asleep. */
        set = w32_event_peek((HANDLE)object, &taker);
        if (set < 0 || (!set && taker && taker != best->tid))
            break;
        if (xbox_HostNowNs() - t0 >= s_bound_ns) {
            if (__atomic_add_fetch(&s_timeouts, 1, __ATOMIC_RELAXED) <= 8) {
                fprintf(stderr, "  [KERNEL] wake hand-off: thread 0x%08X (priority %d) did not"
                                " block within %.1f ms of the signal from 0x%08X (its wait from"
                                " 0x%08X, event %s); going on\n", (unsigned)best->thobj,
                        (int)best->prio, (double)s_bound_ns / 1e6, (unsigned)guest_ret,
                        (unsigned)best->wait_ret, set ? "still set" : "taken by it");
                fflush(stderr);
            }
            break;
        }
        if (guest_turn_joined())
            guest_turn_yield();
        WaitForSingleObject(best->ev, 1);
    }
    __atomic_sub_fetch(&best->watched, 1, __ATOMIC_SEQ_CST);
#endif
}

void xbox_WakeCounts(uint64_t *handoffs, uint64_t *timeouts)
{
    if (handoffs) *handoffs = __atomic_load_n(&s_handoffs, __ATOMIC_RELAXED);
    if (timeouts) *timeouts = __atomic_load_n(&s_timeouts, __ATOMIC_RELAXED);
}

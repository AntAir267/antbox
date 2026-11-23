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

#if defined(_WIN32)
/* An event's state without taking it: 1 set, 0 not, -1 unknown (not an
 * event, or no NtQueryEvent). EVENT_QUERY_STATE is in the access a guest
 * event's handle was created with. */
typedef LONG (NTAPI *nt_query_event_fn)(HANDLE, int, PVOID, ULONG, PULONG);

static int win_event_state(HANDLE h)
{
    static nt_query_event_fn query;
    static int resolved;
    struct { int type; LONG state; } info;
    ULONG len = 0;

    if (!resolved) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll)
            query = (nt_query_event_fn)(void *)GetProcAddress(ntdll, "NtQueryEvent");
        resolved = 1;
    }
    if (!query || query(h, 0 /* EventBasicInformation */, &info, sizeof info, &len) < 0)
        return -1;
    return info.state != 0;
}
#endif

/* Has a thread other than `best` taken the signal, so that `best` stays
 * asleep and waiting for it to run on is pointless?
 *
 * POSIX: the compat layer's events record the thread whose wait took them
 * (w32_event_peek). A Win32 event does not, which is why this hand-off was
 * left out on Windows and BO-221's ordering was not kept there. What every
 * guest wait does say, on every host, is when it ends (xbox_WakeWaitEnd
 * counts `wakes`). So on Windows: the event still set means nobody has
 * taken it yet; `best` counted a wake means it took it; another guest
 * thread that was in a wait on the same object when it was signalled and
 * has counted a wake since, while `best` has not, took it instead (an
 * auto-reset event wakes one waiter). Otherwise keep waiting, up to the
 * bound: Windows hands an auto-reset event to a waiter inside SetEvent, so
 * "not set and nobody has counted yet" is the normal state between the
 * signal and the woken thread being scheduled. */
typedef struct { wake_rec *r; uint32_t wakes; } wake_seen;

static int wake_taken_elsewhere(const void *object, wake_rec *best,
                                uint32_t best_wakes0, const wake_seen *seen,
                                uint32_t nseen, int *state)
{
#if defined(_WIN32)
    uint32_t i;

    *state = win_event_state((HANDLE)object);
    if (*state < 0)
        return 1;
    if (*state)
        return 0;
    if (__atomic_load_n(&best->wakes, __ATOMIC_ACQUIRE) != best_wakes0)
        return 0;
    for (i = 0; i < nseen; i++)
        if (__atomic_load_n(&seen[i].r->wakes, __ATOMIC_ACQUIRE) != seen[i].wakes)
            return 1;
    return 0;
#else
    DWORD taker;
    int set = w32_event_peek((HANDLE)object, &taker);

    (void)best_wakes0; (void)seen; (void)nseen;
    *state = set;
    return set < 0 || (!set && taker && taker != best->tid);
#endif
}

/* Who waited on the object the calling thread is about to signal, and at
 * which point of its run (epoch) and which wait (waits). Without it the
 * waiters are read after the signal, and a woken thread that already ran
 * and began its next wait on the same object looked like the one still to
 * run: the signaller then slept the whole bound for a block that had
 * already happened (4 in 2000 posts on Windows and on a loaded Mac). */
typedef struct { wake_rec *r; uint32_t epoch, waits, wakes; } wake_snap;
static RECOMP_TLS const void *t_snap_object;
static RECOMP_TLS uint32_t t_snap_n;
static RECOMP_TLS wake_snap t_snap[WAKE_MAX];

void xbox_WakeSignalBegin(const void *object)
{
    uint32_t i, n;

    t_snap_object = NULL;
    t_snap_n = 0;
    if (!t_self || !object || !on())
        return;
    n = __atomic_load_n(&s_nrec, __ATOMIC_ACQUIRE);
    for (i = 0; i < n; i++) {
        wake_rec *r = &s_rec[i];
        uint32_t w = __atomic_load_n(&r->waits, __ATOMIC_ACQUIRE);
        uint32_t e = __atomic_load_n(&r->epoch, __ATOMIC_SEQ_CST);

        /* In that wait, before and after the epoch was read: its epoch is
         * the one of that wait. */
        if (r == t_self || __atomic_load_n(&r->waiting_on, __ATOMIC_ACQUIRE) != object
                || __atomic_load_n(&r->waits, __ATOMIC_ACQUIRE) != w)
            continue;
        t_snap[t_snap_n].r = r;
        t_snap[t_snap_n].epoch = e;
        t_snap[t_snap_n].waits = w;
        t_snap[t_snap_n].wakes = __atomic_load_n(&r->wakes, __ATOMIC_ACQUIRE);
        t_snap_n++;
    }
    t_snap_object = object;
}

/* Wait until `best` makes its next call that can block after epoch e0,
 * another thread took the signal, or the bound runs out. */
static void wake_wait_for(const void *object, wake_rec *best, uint32_t e0,
                          uint32_t best_wakes0, const wake_seen *seen,
                          uint32_t nseen, uint32_t guest_ret)
{
    uint64_t t0;

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
    t0 = xbox_HostNowNs();
    for (;;) {
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
        if (wake_taken_elsewhere(object, best, best_wakes0, seen, nseen, &set))
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
#if !defined(_WIN32)
        if (guest_turn_joined())
            guest_turn_yield();
#endif
        WaitForSingleObject(best->ev, 1);
    }
    __atomic_sub_fetch(&best->watched, 1, __ATOMIC_SEQ_CST);
}

/* A signal from a thread at DISPATCH_LEVEL or holding the one-CPU gate.
 *
 * On the console nothing preempts a thread at DISPATCH_LEVEL: the woken
 * thread gets the CPU when the signaller lowers IRQL. Waiting for it here,
 * while raised, was a stall at every such signal (finding 10 of the audit
 * of 2026-11-03): a woken thread whose next step raises IRQL, or that a
 * guest_cpus token holds back, cannot get past the gate this thread holds,
 * so the full bound always ran out. The hand-off is noted instead and run
 * at the lower that lets the gate go (xbox_WakeIrqlLowered). A few per
 * raise; one past that is not waited for. */
#define WAKE_DEFER_MAX 8
#define WAKE_DEFER_SEEN 8
typedef struct {
    const void *object;
    wake_rec *best;
    uint32_t e0, best_wakes0, nseen, guest_ret;
    wake_seen seen[WAKE_DEFER_SEEN];
} wake_deferred;
static RECOMP_TLS wake_deferred t_defer[WAKE_DEFER_MAX];
static RECOMP_TLS uint32_t t_ndefer;
static uint64_t s_deferred, s_defer_dropped;

static void wake_defer(const void *object, wake_rec *best, uint32_t e0,
                       uint32_t best_wakes0, const wake_seen *seen,
                       uint32_t nseen, uint32_t guest_ret)
{
    wake_deferred *d;
    uint64_t k;

    if (t_ndefer >= WAKE_DEFER_MAX) {
        __atomic_add_fetch(&s_defer_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    d = &t_defer[t_ndefer++];
    d->object = object;
    d->best = best;
    d->e0 = e0;
    d->best_wakes0 = best_wakes0;
    d->guest_ret = guest_ret;
    /* Only for the Windows "taken by another" test: fewer seen means that
     * test answers "no" more often, and the bound decides. */
    d->nseen = nseen < WAKE_DEFER_SEEN ? nseen : WAKE_DEFER_SEEN;
    memcpy(d->seen, seen, d->nseen * sizeof seen[0]);
    k = __atomic_add_fetch(&s_deferred, 1, __ATOMIC_RELAXED);
    if (k == 1) {
        fprintf(stderr, "  [KERNEL] wake hand-off: signal at DISPATCH_LEVEL from 0x%08X;"
                        " the woken thread runs first at the lower\n", (unsigned)guest_ret);
        fflush(stderr);
    }
}

void xbox_WakeSignalled(const void *object, uint32_t guest_ret)
{
    wake_rec *self = t_self, *best = NULL;
    uint32_t i, n, e0 = 0, best_wakes0 = 0, nseen = 0;
    wake_seen seen[WAKE_MAX];

    int snapped = 0;

    /* Host threads (timers, DPCs, audio) never wait here. */
    if (!self || !object || !on())
        return;
    if (t_snap_object == object) {
        /* The waiters as they were at the signal (xbox_WakeSignalBegin). */
        snapped = 1;
        t_snap_object = NULL;
        for (i = 0; i < t_snap_n; i++) {
            wake_rec *r = t_snap[i].r;

            seen[nseen].r = r;
            seen[nseen].wakes = t_snap[i].wakes;
            nseen++;
            if (__atomic_load_n(&r->prio, __ATOMIC_RELAXED) <= __atomic_load_n(&self->prio, __ATOMIC_RELAXED))
                continue;
            if (!best || r->prio > best->prio) {
                best = r;
                e0 = t_snap[i].epoch;
            }
        }
        n = 0;
    } else {
        n = __atomic_load_n(&s_nrec, __ATOMIC_ACQUIRE);
    }
    for (i = 0; i < n; i++) {
        wake_rec *r = &s_rec[i];

        if (r == self || __atomic_load_n(&r->waiting_on, __ATOMIC_ACQUIRE) != object)
            continue;
        /* Every other waiter on it, with its wake count now (Windows). */
        seen[nseen].r = r;
        seen[nseen].wakes = __atomic_load_n(&r->wakes, __ATOMIC_ACQUIRE);
        nseen++;
        if (__atomic_load_n(&r->prio, __ATOMIC_RELAXED) <= __atomic_load_n(&self->prio, __ATOMIC_RELAXED))
            continue;
        if (!best || r->prio > best->prio)
            best = r;
    }
    if (!best)
        return;
    /* `seen` is the others: drop best from it. */
    for (i = 0; i < nseen; i++)
        if (seen[i].r == best) {
            best_wakes0 = seen[i].wakes;
            seen[i] = seen[--nseen];
            break;
        }
    if (!snapped)
        e0 = __atomic_load_n(&best->epoch, __ATOMIC_SEQ_CST);
    if (xbox_IrqlThisThreadBlocksDpcs()) {
        wake_defer(object, best, e0, best_wakes0, seen, nseen, guest_ret);
        return;
    }
    wake_wait_for(object, best, e0, best_wakes0, seen, nseen, guest_ret);
}

/* Lowered below DISPATCH_LEVEL, the one-CPU gate let go (kernel_hal.c):
 * the hand-offs signalled while raised run now. */
void xbox_WakeIrqlLowered(void)
{
    wake_deferred d[WAKE_DEFER_MAX];
    uint32_t i, n = t_ndefer;

    if (!n)
        return;
    memcpy(d, t_defer, n * sizeof d[0]);
    t_ndefer = 0;
    for (i = 0; i < n; i++)
        wake_wait_for(d[i].object, d[i].best, d[i].e0, d[i].best_wakes0,
                      d[i].seen, d[i].nseen, d[i].guest_ret);
}

void xbox_WakeCounts(uint64_t *handoffs, uint64_t *timeouts)
{
    if (handoffs) *handoffs = __atomic_load_n(&s_handoffs, __ATOMIC_RELAXED);
    if (timeouts) *timeouts = __atomic_load_n(&s_timeouts, __ATOMIC_RELAXED);
}

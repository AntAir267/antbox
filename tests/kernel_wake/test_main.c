/*
 * kernel_wake: a signalled thread of higher priority runs first (BO-221,
 * src/kernel/kernel_wake.h).
 *
 * The shape of Burnout 3's RenderWare file worker: the main thread (priority
 * 0) posts a command and sets the worker's event; the worker (priority +1)
 * waits on the event, takes the command, and only then makes a call that can
 * block. On the console the main thread runs on only after that, so it never
 * sees its command still untaken. Each round here posts one command and
 * looks at once: "untaken" is the window in which the title's cancel freed a
 * file slot that was still in use.
 *
 * Without the hand-off (xbox_WakeConfigure(0)) most rounds see the command
 * untaken; with it none may. A third case: a waiter of the same priority
 * gets no hand-off (the poster goes straight on). A fourth: the worker has
 * woken, run and begun its next wait before the poster looks (the order a
 * wake boost gives on Windows); the poster must see the hand-off done, not
 * sleep the bound waiting for a block that already happened.
 */
#include "kernel.h"
#include "kernel_wake.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>

#define ROUNDS 2000

/* The IRQL path links the kernel, which looks up guest code; none here. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(unsigned int va);
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return NULL; }

static HANDLE g_ev;
static volatile int g_cmd, g_taken, g_ready, g_stop;
static int32_t g_worker_prio;
static int g_late;      /* look at the waiters only after the worker went round */
static int g_raised;    /* the poster signals at DISPATCH_LEVEL; the worker
                           raises IRQL itself before its next block */
static uint8_t g_guest[0x10000];  /* fs:[0x24] is published here */
extern ptrdiff_t g_xbox_mem_offset;

static void signal_begin(HANDLE ev)
{
#ifdef XBOX_HAVE_WAKE_SIGNAL_BEGIN
    xbox_WakeSignalBegin(ev);
#else
    (void)ev;
#endif
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    xbox_WakeGuestThread(0x1000);
    xbox_WakeSetPriority(0x1000, g_worker_prio);
    while (!g_stop) {
        xbox_WakeBlockingCall();            /* the wait is a blocking call */
        xbox_WakeWaitBegin(g_ev, 0);
        __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
        WaitForSingleObject(g_ev, INFINITE);
        xbox_WakeWaitEnd();
        if (__atomic_load_n(&g_cmd, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_cmd, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_taken, 1, __ATOMIC_RELEASE);
        }
        /* Some guest code, then the next call that can block. */
        for (volatile int i = 0; i < 2000; i++)
            ;
        if (g_raised) {
            /* Needs the one-CPU gate the poster holds while raised. */
            KIRQL old = xbox_KeRaiseIrqlToDpcLevel();
            xbox_KfLowerIrql(old);
        }
    }
    return 0;
}

/* Rounds in which the posted command was still untaken right after the post. */
static int run(int handoff, int32_t worker_prio, int rounds)
{
    HANDLE t;
    int i, untaken = 0;

    xbox_WakeConfigure(handoff, 20000000ull);
    g_worker_prio = worker_prio;
    g_stop = 0;
    g_ready = 0;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    xbox_WakeGuestThread(0x2000);
    xbox_WakeSetPriority(0x2000, 0);
    for (i = 0; i < rounds; i++) {
        while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
            SwitchToThread();
        __atomic_store_n(&g_ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_taken, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_cmd, 1, __ATOMIC_RELEASE);
        KIRQL was = 0;

        if (g_raised)
            was = xbox_KeRaiseIrqlToDpcLevel();
        signal_begin(g_ev);
        SetEvent(g_ev);
        if (g_late) {
            /* The worker woke, took the command and is in its next wait
             * before the signaller looks: the hand-off has happened, and
             * the signaller must not wait the bound for another block. */
            while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
                SwitchToThread();
        }
        xbox_WakeSignalled(g_ev, 0);
        if (g_raised)
            xbox_KfLowerIrql(was);      /* the worker runs first here */
        if (!__atomic_load_n(&g_taken, __ATOMIC_ACQUIRE))
            untaken++;
        /* The title's frame goes on; the command is taken by now. */
        while (__atomic_load_n(&g_cmd, __ATOMIC_ACQUIRE))
            SwitchToThread();
    }
    g_stop = 1;
    do
        SetEvent(g_ev);
    while (WaitForSingleObject(t, 10) != WAIT_OBJECT_0);
    CloseHandle(t);
    CloseHandle(g_ev);
    return untaken;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest;  /* fs:[0x24] lands in g_guest */
    int off = run(0, 1, ROUNDS), on = run(1, 1, ROUNDS), same = run(1, 0, ROUNDS), fails = 0;
    uint64_t handoffs, timeouts, h1, t1, t2;
    uint64_t late_ms;

    xbox_WakeCounts(&handoffs, &timeouts);
    /* The worker runs its whole round between the signal and the look: 50
     * posts, each must end at once rather than after the 20 ms bound. */
    g_late = 1;
    t2 = xbox_HostNowNs();
    run(1, 1, 50);
    late_ms = (xbox_HostNowNs() - t2) / 1000000ull;
    g_late = 0;
    xbox_WakeCounts(&h1, &t1);
    /* Signalled at DISPATCH_LEVEL, the worker's next step needs the gate:
     * it can only run once the poster lowers, which is where it must run
     * first, without the poster sitting out the bound while raised. */
    {
        uint64_t h2, t3, t4 = xbox_HostNowNs();
        int raised_untaken;

        g_raised = 1;
        raised_untaken = run(1, 1, 50);
        g_raised = 0;
        xbox_WakeCounts(&h2, &t3);
        printf("raised: untaken after the lower %d/50, %llu hand-offs, %llu over the"
               " bound, %llu ms for 50 posts\n", raised_untaken,
               (unsigned long long)(h2 - h1), (unsigned long long)(t3 - t1),
               (unsigned long long)((xbox_HostNowNs() - t4) / 1000000ull));
        if (raised_untaken != 0 || t3 != t1) {
            printf("FAIL: a signal at DISPATCH_LEVEL stalled the poster or let it run on first\n");
            fails++;
        }
    }
    printf("late look: %llu hand-offs, %llu over the bound, %llu ms for 50 posts\n",
           (unsigned long long)(h1 - handoffs), (unsigned long long)(t1 - timeouts),
           (unsigned long long)late_ms);
    if (t1 != timeouts) {
        printf("FAIL: a worker that had already run and blocked again cost the signaller the bound\n");
        fails++;
    }
    printf("untaken after the post: %d/%d without the hand-off, %d/%d with it"
           " (%llu hand-offs, %llu over the bound); same priority: %d/%d\n",
           off, ROUNDS, on, ROUNDS, (unsigned long long)handoffs,
           (unsigned long long)timeouts, same, ROUNDS);
    if (on != 0) {
        printf("FAIL: a higher-priority worker did not take the command first\n");
        fails++;
    }
    if (timeouts != 0) {
        printf("FAIL: hand-offs ran into the bound\n");
        fails++;
    }
    if (handoffs < ROUNDS) {
        printf("FAIL: %llu hand-offs for %d posts\n", (unsigned long long)handoffs, ROUNDS);
        fails++;
    }
    printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

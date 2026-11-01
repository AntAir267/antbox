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
 * gets no hand-off (the poster goes straight on).
 */
#include "kernel.h"
#include "kernel_wake.h"

#include <stdio.h>
#include <stdlib.h>

#define ROUNDS 2000

static HANDLE g_ev;
static volatile int g_cmd, g_taken, g_ready, g_stop;
static int32_t g_worker_prio;

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
    }
    return 0;
}

/* Rounds in which the posted command was still untaken right after the post. */
static int run(int handoff, int32_t worker_prio)
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
    for (i = 0; i < ROUNDS; i++) {
        while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
            SwitchToThread();
        __atomic_store_n(&g_ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_taken, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_cmd, 1, __ATOMIC_RELEASE);
        SetEvent(g_ev);
        xbox_WakeSignalled(g_ev, 0);
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
    int off = run(0, 1), on = run(1, 1), same = run(1, 0), fails = 0;
    uint64_t handoffs, timeouts;

    xbox_WakeCounts(&handoffs, &timeouts);
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

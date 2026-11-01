/*
 * The one-guest-CPU token: see win32_compat.c. Parts 1-4 are the fixture of
 * vyanhursky/xboxrecomp dc00f70 (tests/guest_cpu_posix, guest_turn_* names
 * from e5e59f6); part 5 covers the locks this runtime added to the list of
 * primitives that give the CPU up.
 *
 *   1. Two joined threads never run at once. Each counts on its own, and a
 *      third thread looks at both counters twice, 50 microseconds apart.
 *      Side by side, both have moved in every such window; taking turns,
 *      only in a window that happened to contain a change-over, so in no
 *      more windows than there were change-overs.
 *   2. A joined thread spinning on a flag is preempted for the joined thread
 *      that sets it. Without preemption this never ends.
 *   3. A joined thread blocked in a wait does not hold the CPU: the thread
 *      that signals it gets to run.
 *   4. A thread interrupted outside the range given as guest code is not
 *      switched there; it hands over at its next checkpoint.
 *   5. A joined thread that finds a critical section or an SRW lock held
 *      gives the CPU up while it waits: the holder, which blocked while it
 *      held the lock, needs the CPU back to get as far as releasing it.
 *      Without that both wait for ever.
 */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile int g_flag, g_go;
static volatile long g_count[2];
static volatile int g_done[2];
#define ROUNDS 400000000L
static HANDLE g_event;

/* Everything a "guest" thread runs is in this file, so the whole address
 * space stands in for the title's code, except in part 4. */
static void whole_space_is_guest(void) { guest_turn_set_code((void *)0, SIZE_MAX); }

static DWORD WINAPI exclusive(LPVOID arg)
{
    int me = (int)(intptr_t)arg;
    long i;

    guest_turn_join();
    for (i = 0; i < ROUNDS; i++)
        g_count[me] = i;
    g_done[me] = 1;
    return 0;
}

static DWORD WINAPI spinner(LPVOID arg)
{
    (void)arg;
    guest_turn_join();
    g_go = 1;
    while (!g_flag) { }      /* no kernel call, no wait: only preemption ends it */
    return 0;
}

static DWORD WINAPI setter(LPVOID arg)
{
    (void)arg;
    while (!g_go)
        Sleep(1);
    guest_turn_join();
    g_flag = 1;
    return 0;
}

static DWORD WINAPI waiter(LPVOID arg)
{
    (void)arg;
    guest_turn_join();
    g_go = 1;
    return WaitForSingleObject(g_event, 10000) == WAIT_OBJECT_0 ? 0 : 1;
}

static DWORD WINAPI signaller(LPVOID arg)
{
    (void)arg;
    while (!g_go)
        Sleep(1);
    guest_turn_join();
    SetEvent(g_event);
    return 0;
}

/* Spins in "host" code, then reaches a checkpoint. */
static DWORD WINAPI host_spinner(LPVOID arg)
{
    volatile long n = 0;
    uint64_t before, after;

    (void)arg;
    guest_turn_join();
    g_go = 1;
    guest_turn_counts(&before, NULL);
    while (n < 400000000 && !g_flag)
        n++;
    guest_turn_counts(&after, NULL);
    if (g_flag || after != before)
        return 1;                       /* it was switched in host code */
    guest_turn_checkpoint();             /* the setter runs here */
    return g_flag ? 0 : 2;
}

static CRITICAL_SECTION g_cs;
static SRWLOCK g_srw = SRWLOCK_INIT;
static int g_use_srw;

static void lock_take(void)
{
    if (g_use_srw) AcquireSRWLockExclusive(&g_srw);
    else           EnterCriticalSection(&g_cs);
}

static void lock_drop(void)
{
    if (g_use_srw) ReleaseSRWLockExclusive(&g_srw);
    else           LeaveCriticalSection(&g_cs);
}

/* Holds the lock across a sleep, which gives the CPU up. */
static DWORD WINAPI locker(LPVOID arg)
{
    (void)arg;
    guest_turn_join();
    lock_take();
    g_go = 1;
    Sleep(20);
    lock_drop();
    return 0;
}

/* Runs while the locker sleeps, and finds the lock held. */
static DWORD WINAPI contender(LPVOID arg)
{
    (void)arg;
    while (!g_go)
        Sleep(1);
    guest_turn_join();
    lock_take();
    lock_drop();
    return 0;
}

static int run_pair(LPTHREAD_START_ROUTINE a, LPTHREAD_START_ROUTINE b, DWORD *ra, DWORD *rb)
{
    HANDLE t[2];

    g_go = g_flag = 0;
    t[0] = CreateThread(NULL, 0, a, (LPVOID)(intptr_t)0, 0, NULL);
    t[1] = CreateThread(NULL, 0, b, (LPVOID)(intptr_t)1, 0, NULL);
    if (WaitForSingleObject(t[0], 60000) != WAIT_OBJECT_0 ||
            WaitForSingleObject(t[1], 60000) != WAIT_OBJECT_0)
        return 0;
    GetExitCodeThread(t[0], ra);
    GetExitCodeThread(t[1], rb);
    return 1;
}

int main(void)
{
    DWORD ra = 0, rb = 0;
    uint64_t handoffs = 0, kicks = 0;

    whole_space_is_guest();

    {
        HANDLE t[2];
        long windows = 0, both = 0;
        LARGE_INTEGER f, a, b;

        QueryPerformanceFrequency(&f);
        t[0] = CreateThread(NULL, 0, exclusive, (LPVOID)(intptr_t)0, 0, NULL);
        t[1] = CreateThread(NULL, 0, exclusive, (LPVOID)(intptr_t)1, 0, NULL);
        while (!g_done[0] && !g_done[1]) {
            long c0 = g_count[0], c1 = g_count[1];
            QueryPerformanceCounter(&a);
            do
                QueryPerformanceCounter(&b);
            while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < 50);
            windows++;
            both += g_count[0] != c0 && g_count[1] != c1;
        }
        WaitForSingleObject(t[0], INFINITE);
        WaitForSingleObject(t[1], INFINITE);
        guest_turn_counts(&handoffs, &kicks);
        printf("exclusion: both threads moved in %ld of %ld windows, %llu change-overs\n",
               both, windows, (unsigned long long)handoffs);
        /* One change-over can straddle two windows; a join and an exit are
         * change-overs the count does not include. */
        if (windows < 100 || both > 2 * (long)handoffs + 4)
            return 1;
    }

    if (!run_pair(spinner, setter, &ra, &rb)) {
        fprintf(stderr, "preemption: a spinning thread kept the guest CPU\n");
        return 1;
    }
    printf("preemption: the spinning thread was interrupted\n");

    g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!run_pair(waiter, signaller, &ra, &rb) || ra != 0) {
        fprintf(stderr, "wait: a blocked thread kept the guest CPU (%lu)\n", (unsigned long)ra);
        return 1;
    }
    printf("wait: a blocked thread gave the guest CPU up\n");

    guest_turn_set_code((void *)0, 0);       /* nothing is guest code */
    if (!run_pair(host_spinner, setter, &ra, &rb) || ra != 0) {
        fprintf(stderr, "checkpoint: host code was switched, or never handed over (%lu)\n",
                (unsigned long)ra);
        return 1;
    }
    printf("checkpoint: host code handed over only at its checkpoint\n");

    InitializeCriticalSection(&g_cs);
    for (g_use_srw = 0; g_use_srw < 2; g_use_srw++) {
        const char *what = g_use_srw ? "SRW lock" : "critical section";
        if (!run_pair(locker, contender, &ra, &rb)) {
            fprintf(stderr, "locks: a thread waiting on a %s kept the guest CPU\n", what);
            return 1;
        }
        printf("locks: a thread waiting on a %s gave the guest CPU up\n", what);
    }
    return 0;
}

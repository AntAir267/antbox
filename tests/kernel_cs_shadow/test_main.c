/*
 * The critical-section shadow table (audit 2026-11-03, kernel-sync finding 6).
 *
 * The Xbox kernel has no RtlDeleteCriticalSection, so a guest lock inside a
 * heap object is a new key each time the object lands somewhere new. The
 * table kept every key ever seen; at 4096 RtlEnterCriticalSection returned
 * without locking. Checks:
 *  - after 10000 distinct locks were used once each, a new lock still
 *    excludes a second thread;
 *  - a lock held across that churn is never freed: a second thread still
 *    blocks on it, and recursion on it still balances;
 *  - the table frees slots (count of freed > 0, slots in use <= 4096).
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(unsigned int va);
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* 20-byte guest RTL_CRITICAL_SECTIONs; only their addresses matter. */
#define CHURN 10000
static uint8_t s_locks[CHURN + 4][32];
#define LOCK(i) ((PRTL_CRITICAL_SECTION)(void *)s_locks[i])

static volatile LONG s_got;
static DWORD WINAPI take(LPVOID p)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)p);
    InterlockedExchange(&s_got, 1);
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)p);
    return 0;
}

/* Does a second thread block on `cs` while this one holds it? */
static int excludes(PRTL_CRITICAL_SECTION cs)
{
    HANDLE h;
    int blocked;

    xbox_RtlEnterCriticalSection(cs);
    InterlockedExchange(&s_got, 0);
    h = CreateThread(NULL, 0, take, cs, 0, NULL);
    Sleep(200);
    blocked = InterlockedCompareExchange(&s_got, 0, 0) == 0;
    xbox_RtlLeaveCriticalSection(cs);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    return blocked && s_got == 1;
}

int main(void)
{
    PRTL_CRITICAL_SECTION held = LOCK(CHURN), fresh = LOCK(CHURN + 1);
    long freed = 0;
    int i, used;

    setvbuf(stdout, NULL, _IONBF, 0);
    check(excludes(LOCK(CHURN + 2)), "a lock excludes a second thread (baseline)");

    /* Held across the churn, recursively. */
    xbox_RtlEnterCriticalSection(held);
    xbox_RtlEnterCriticalSection(held);
    for (i = 0; i < CHURN; i++) {
        xbox_RtlInitializeCriticalSection(LOCK(i));
        xbox_RtlEnterCriticalSection(LOCK(i));
        xbox_RtlLeaveCriticalSection(LOCK(i));
    }
    InterlockedExchange(&s_got, 0);
    {
        HANDLE h = CreateThread(NULL, 0, take, held, 0, NULL);
        Sleep(200);
        check(s_got == 0, "a lock held across the churn still blocks another thread");
        xbox_RtlLeaveCriticalSection(held);
        Sleep(100);
        check(s_got == 0, "one leave of two recursive enters keeps it held");
        xbox_RtlLeaveCriticalSection(held);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
        check(s_got == 1, "the last leave lets the other thread in");
    }

    check(excludes(fresh), "after 10000 distinct locks, a new lock still excludes");
    check(excludes(LOCK(7)), "an early, freed lock still excludes when used again");

#ifdef XBOX_HAVE_CS_TABLE_STATS
    used = xbox_cs_table_stats(&freed);
#else
    used = -1;                        /* the table could not say */
#endif
    printf("  slots in use %d, freed so far %ld\n", used, freed);
    check(freed > 0 && used >= 0 && used <= 4096, "idle shadow slots are freed");

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}

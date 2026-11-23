/*
 * The kernel-call log gate (BO-351).
 *
 * KERNEL_LOG_ON() throttles the per-call kernel trace once the call counter
 * passes the log budget. The counter used to be a signed 32-bit int, so a
 * title that spun in a kernel-call loop (Burnout 3's stop-a-sound wait, on the
 * int042 soak) wrapped it past 2^31: the count went negative, `count <= budget`
 * became true again, and full tracing switched back on for millions of lines.
 *
 * The counter is now 64-bit, and the gate is a callable so this project can
 * drive it past the old wrap point without making two billion calls. The
 * checks that matter are the ones just above 2^31: with the signed type the
 * gate re-opens there; with the unsigned 64-bit type it stays shut.
 */
#include "kernel.h"

#include <stdio.h>

/* Test seam in kernel_bridge.c: the gate, a getter and a setter for the
 * counter. xbox_kernel_log_on() is the exact predicate behind KERNEL_LOG_ON(),
 * so "the gate is shut" is "tracing stays off"; the getter exposes the stored
 * count so the 64-bit type is asserted, not just assumed. */
int  xbox_kernel_log_on(void);
int  xbox_kernel_log_on_half(void);
unsigned long long xbox_kernel_call_count(void);
void xbox_kernel_set_call_count(unsigned long long count);

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Reaching the bridge's log
 * gate pulls in kernel_bridge.o, which references these. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Count 0 is within any budget (the budget is clamped to >= 0), so the
     * trace starts on. */
    xbox_kernel_set_call_count(0);
    check(xbox_kernel_log_on() == 1, "trace is on at call 0");

    /* The regression: the gate must stay shut past 2^31, where the old signed
     * 32-bit counter wrapped negative and re-opened it. The getter proves the
     * counter really is 64-bit: 2^31 is stored as 2^31, not as a negative. */
    xbox_kernel_set_call_count(0x80000000ull);   /* 2^31 */
    check(xbox_kernel_call_count() == 0x80000000ull,
          "counter holds 2^31 (64-bit, no wrap)");
    check(xbox_kernel_log_on() == 0, "trace stays off at 2^31");
    check(xbox_kernel_log_on_half() == 0, "half gate stays off at 2^31");

    xbox_kernel_set_call_count(0x80000001ull);   /* 2^31 + 1 */
    check(xbox_kernel_log_on() == 0, "trace stays off just past 2^31");

    xbox_kernel_set_call_count(~0ull);           /* 2^64 - 1 */
    check(xbox_kernel_log_on() == 0, "trace stays off at 2^64-1");

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

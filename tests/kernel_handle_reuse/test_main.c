/*
 * A closed handle's token does not name the next object opened (BO-221).
 *
 * The bridge hands the guest 32-bit tokens for host handles. It used to give
 * each new handle the lowest free slot, so the handle opened right after a
 * close got the closed handle's token back. A guest thread still holding the
 * old value -- another thread closed it between two calls that name it --
 * then reached the new object: on Burnout 3 a seek meant for the race music's
 * wave bank could move the next sound bank's file position past its end, and
 * the bank was parsed from a sector that was never read (GitHub #17, a
 * SIGSEGV in the title's bank loader on the Steam Deck).
 *
 * Checked through the bridge with events, which take tokens from the same
 * table as files and need no paths: an event is created and closed, a second
 * one is created, and the first token is used again (NtSetEvent, as the
 * stale call). The second event must not be set by it, and the stale call
 * must fail as a closed handle does. Before the fix the second event got the
 * first one's token and the stale NtSetEvent set it.
 *
 * Cross-platform: Win32 handles on Windows, the POSIX shim elsewhere.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define THUNK_VA  0x10000u
#define STACK_VA  0x20000u
#define OUT_VA    0x30000u          /* the PHANDLE NtCreateEvent writes */
#define TIME_VA   0x30010u          /* a zero timeout */

static uint8_t *mem;
/* The thunk slots (their order in the table at THUNK_VA). The dispatcher
 * recomp_lookup_kernel returns serves the slot it was last asked for, so
 * each call looks its slot up right before it runs. */
enum { K_CLOSE, K_CREATE, K_SET, K_WAIT };
static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

/* A stdcall into the bridge: the guest pushed a return address, then args. */
static uint32_t call(int slot, int n, const uint32_t *args)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4 * slot));
    int i;
    if (!fn) { puts("FAIL: an ordinal has no bridge"); exit(1); }
    sp[0] = 0xBEEF0001u;
    for (i = 0; i < n; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    fn();
    return g_eax;
}

static uint32_t create_event(void)
{
    const uint32_t a[4] = { OUT_VA, 0, 0 /* NotificationEvent */, 0 /* not set */ };
    *(uint32_t *)(mem + OUT_VA) = 0;
    if (call(K_CREATE, 4, a) != 0)
        return 0;
    return *(uint32_t *)(mem + OUT_VA);
}

static uint32_t is_set(uint32_t token)
{
    const uint32_t a[4] = { token, 0, 0, TIME_VA };
    return call(K_WAIT, 4, a) == 0;     /* STATUS_SUCCESS; 0x102 is a timeout */
}

int main(void)
{
    static const uint32_t ordinals[4] = { 187, 189, 225, 234 };
    uint32_t first, second, st;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { puts("FAIL: no guest memory stand-in"); return 1; }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    for (i = 0; i < 4; i++)
        *(uint32_t *)(mem + THUNK_VA + 4 * i) = 0x80000000u | ordinals[i];
    xbox_kernel_set_thunk_address(THUNK_VA, 4);
    xbox_kernel_bridge_init();
    memset(mem + TIME_VA, 0, 8);

    first = create_event();
    check(first != 0, "NtCreateEvent gives a token", first);
    {
        const uint32_t a[1] = { first };
        check(call(K_CLOSE, 1, a) == 0, "NtClose of it succeeds", g_eax);
    }
    second = create_event();
    check(second != 0, "a second NtCreateEvent gives a token", second);
    check(second != first, "the second event does not get the closed one's token", second);

    /* The stale call: NtSetEvent with the closed event's token. */
    {
        const uint32_t a[2] = { first, 0 };
        st = call(K_SET, 2, a);
    }
    check(st == 0xC0000008u, "NtSetEvent on the closed token fails (INVALID_HANDLE)", st);
    check(!is_set(second), "the second event is not set by the stale call", second);

    /* Many open/close rounds: no token comes back while its old holder
     * could still have it (a few thousand handles later is fine). */
    {
        uint32_t seen = second, t;
        int again = 0, k;
        for (k = 0; k < 1000; k++) {
            const uint32_t a[1] = { seen };
            call(K_CLOSE, 1, a);
            t = create_event();
            if (t == seen || t == first) again++;
            seen = t;
        }
        check(again == 0, "1000 close/create rounds reuse no just-closed token", (unsigned)again);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

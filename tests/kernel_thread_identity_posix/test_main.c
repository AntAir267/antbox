/*
 * Thread identity of host threads that run guest code (audit 2026-11-03,
 * kernel-sync finding 1).
 *
 * The timer/DPC thread, the APU frame thread and the OHCI thread each build
 * their TIB with xbox_AllocThreadTib. That used to copy the main thread's
 * fs:[0x28] -- its KTHREAD stand-in -- so a DPC or ISR read the main thread's
 * id at +0x12C (XAPI's GetCurrentThreadId) and its TlsData at +0x28.
 *
 * Checks, on a real timer DPC (KeSetTimer -> timer thread -> routine) and on
 * two plain host threads that take a TIB the way the APU and OHCI threads do:
 *  - fs:[0x28] is not the main thread's object;
 *  - [fs:[0x28]+0x12C] is non-zero, not the main thread's, unique per thread;
 *  - [fs:[0x28]+0x28] (TlsData) is this thread's own block, the same dword
 *    [fs:[4] + index*4] reaches;
 *  - fs:[0x58], KPRCB.DpcRoutineActive, is 1 inside a DPC and 0 elsewhere
 *    (finding 4: the TIB was 0x40 bytes, so it read TLS template bytes);
 *  - fs:[0x24] (Irql) starts at 0 and fs:[0x18] (Self) is the thread's own
 *    TIB, not copies of the main thread's (finding 9);
 *  - and the dispatcher state guest code can read inline: a timer is
 *    signalled before its DPC runs and unsignalled by the next KeSetTimer,
 *    and a KEVENT's SignalState follows set, reset and satisfied waits
 *    (findings 7 and 8).
 *
 * The XBE is tools/conformance/test.xbe with a TLS directory patched into its
 * header page, because xbox_AllocThreadTib needs an image with TLS.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
extern RECOMP_TLS uint32_t g_esp, g_eax;
extern RECOMP_TLS uint32_t g_fs_base;
extern ptrdiff_t g_xbox_mem_offset;
extern recomp_func_t recomp_lookup_kernel(uint32_t);

#define BASE       0x00010000u
#define TLS_DIR    (BASE + 0x800)
#define TLS_DATA   (BASE + 0x900)      /* 32 bytes of 0xA5 */
#define TLS_INDEX  (BASE + 0x940)
#define THUNKS     (BASE + 0xA00)
#define DPC_FN_VA  0x00011000u         /* the "guest" DPC routine */
#define TIMER_VA   (BASE + 0xB00)
#define DPC_VA     (BASE + 0xB40)
#define EV_SYNC    (BASE + 0xB80)
#define EV_NOTIF   (BASE + 0xBA0)
#define ZERO_TIME  (BASE + 0xBC0)      /* LARGE_INTEGER 0: do not wait */
#define EV_OTHER   (BASE + 0xBE0)
#define TIME_1S    (BASE + 0xC00)      /* LARGE_INTEGER -1 s: relative */
#define MAIN_ID    0x40000000u

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* What one thread saw of itself. */
typedef struct {
    uint32_t fs, kthread, id, tls_data, tls_via_fs4, dpc_active, irql, self;
    uint32_t timer_state;           /* the DPC's timer's SignalState */
} Seen;

static uint32_t s_tls_total;

static void see(Seen *s)
{
    uint32_t fs = g_fs_base;
    s->fs = fs;
    s->kthread = G32(fs + 0x28);
    s->id = s->kthread ? G32(s->kthread + 0x12C) : 0;
    s->tls_data = s->kthread ? G32(s->kthread + 0x28) : 0;
    s->tls_via_fs4 = G32(fs + 0x04) - s_tls_total;   /* index = -(total/4) */
    s->dpc_active = G8(fs + 0x58);
    s->irql = G8(fs + 0x24);
    s->self = G32(fs + 0x18);
}

/* The DPC routine: stdcall (Dpc, Context, Arg1, Arg2) after a return slot. */
static Seen s_dpc;
static HANDLE s_dpc_done;
static volatile LONG s_dpc_runs;

static void dpc_routine(void)
{
    see(&s_dpc);
    s_dpc.timer_state = G32(TIMER_VA + 4);
    InterlockedIncrement(&s_dpc_runs);
    g_esp += 20;
    SetEvent(s_dpc_done);
}

recomp_func_t recomp_lookup(unsigned int va);
recomp_func_t recomp_lookup(unsigned int va)
{
    return va == DPC_FN_VA ? dpc_routine : NULL;
}
recomp_func_t recomp_lookup_manual(unsigned int va);
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return NULL; }

/* Call import `slot` with stdcall arguments on a scratch guest stack. */
static uint32_t s_stack;
static uint32_t kcall_on(uint32_t stack, int slot, int n, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup_kernel(G32(THUNKS + 4u * (uint32_t)slot));
    int i;

    if (!fn) { printf("  import %d not resolved\n", slot); exit(3); }
    g_esp = stack;
    G32(g_esp) = 0xBEEF0001u;                 /* return address */
    for (i = 0; i < n; i++)
        G32(g_esp + 4u + 4u * (uint32_t)i) = args[i];
    fn();
    return g_eax;
}

static uint32_t kcall(int slot, int n, const uint32_t *args)
{
    return kcall_on(s_stack, slot, n, args);
}


enum { K_INIT_DPC, K_INIT_TIMER, K_SET_TIMER, K_CANCEL_TIMER,
       K_INIT_EVENT, K_SET_EVENT, K_RESET_EVENT, K_WAIT, K_COUNT };

/* A thread waiting on EV_SYNC for a second, on a stack of its own. */
static volatile LONG s_waiter_done;
static volatile uint32_t s_waiter_status;
static DWORD WINAPI waiter_thread(LPVOID p)
{
    uint32_t args[5] = { EV_SYNC, 0, 0, 0, TIME_1S };

    (void)p;
    s_waiter_status = kcall_on(s_stack - 0x400, K_WAIT, 5, args);
    InterlockedExchange(&s_waiter_done, 1);
    return 0;
}

static Seen s_host[2];
static DWORD WINAPI host_thread(LPVOID p)
{
    Seen *s = (Seen *)p;
    uint32_t tib = xbox_AllocThreadTib();     /* as apu_core.c / ohci.c do */
    if (tib)
        g_fs_base = tib;
    see(s);
    return tib ? 0 : 1;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void put32(unsigned char *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }

int main(int argc, char **argv)
{
    size_t n = 0;
    unsigned char *xbe;
    uint32_t main_kthread, main_tls, args[5];
    Seen me;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_env_set(RENV_AC97_READY, NULL);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || n < 0x1000) { fprintf(stderr, "cannot read the test XBE\n"); return 2; }

    /* TLS directory: start, end, index address, callbacks, zero fill. */
    put32(xbe, 0x12C, TLS_DIR);
    put32(xbe, TLS_DIR - BASE + 0x00, TLS_DATA);
    put32(xbe, TLS_DIR - BASE + 0x04, TLS_DATA + 32);
    put32(xbe, TLS_DIR - BASE + 0x08, TLS_INDEX);
    put32(xbe, TLS_DIR - BASE + 0x0C, 0);
    put32(xbe, TLS_DIR - BASE + 0x10, 16);
    memset(xbe + (TLS_DATA - BASE), 0xA5, 32);
    /* Imports: KeInitializeDpc, KeInitializeTimerEx, KeSetTimer, KeCancelTimer. */
    put32(xbe, THUNKS - BASE + 0x00, 0x80000000u | 107);
    put32(xbe, THUNKS - BASE + 0x04, 0x80000000u | 113);
    put32(xbe, THUNKS - BASE + 0x08, 0x80000000u | 149);
    put32(xbe, THUNKS - BASE + 0x0C, 0x80000000u | 97);
    put32(xbe, THUNKS - BASE + 0x10, 0x80000000u | 108);
    put32(xbe, THUNKS - BASE + 0x14, 0x80000000u | 145);
    put32(xbe, THUNKS - BASE + 0x18, 0x80000000u | 138);
    put32(xbe, THUNKS - BASE + 0x1C, 0x80000000u | 159);
    put32(xbe, THUNKS - BASE + 0x20, 0);

    if (!xbox_MemoryLayoutInit(xbe, n)) { fprintf(stderr, "memory layout failed\n"); return 2; }
    xbox_kernel_set_thunk_address(THUNKS, K_COUNT);
    xbox_kernel_bridge_init();
    s_stack = XBOX_STACK_TOP - 0x1000;
    s_tls_total = ((32 + 16 + 0xF) & ~0xFu) + 4;

    /* The main thread as the title leaves it: its fs:[0x28] object carries
     * an id once the first PsCreateSystemThreadEx ran (0f95accd). A value
     * the id counter does not reach in this test. */
    main_kthread = G32(XBOX_TIB_MAIN + 0x28);
    G32(main_kthread + 0x12C) = MAIN_ID;
    main_tls = G32(main_kthread + 0x28);
    see(&me);
    printf("main: fs=0x%08X kthread=0x%08X id=%u TlsData=0x%08X\n",
           me.fs, me.kthread, me.id, me.tls_data);

    /* A timer DPC, on the timer thread. */
    s_dpc_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    args[0] = DPC_VA; args[1] = DPC_FN_VA; args[2] = 0x1234;
    kcall(K_INIT_DPC, 3, args);
    args[0] = TIMER_VA; args[1] = 0;
    kcall(K_INIT_TIMER, 2, args);
    args[0] = TIMER_VA; args[1] = (uint32_t)-10000; args[2] = 0xFFFFFFFFu; args[3] = DPC_VA;
    kcall(K_SET_TIMER, 4, args);
    check(WaitForSingleObject(s_dpc_done, 5000) == WAIT_OBJECT_0, "timer DPC ran");
    printf("dpc:  fs=0x%08X kthread=0x%08X id=%u TlsData=0x%08X fs4-block=0x%08X\n",
           s_dpc.fs, s_dpc.kthread, s_dpc.id, s_dpc.tls_data, s_dpc.tls_via_fs4);
    check(s_dpc.fs != XBOX_TIB_MAIN, "DPC runs on its own TIB");
    check(s_dpc.kthread && s_dpc.kthread != main_kthread,
          "DPC: fs:[0x28] is not the main thread's KTHREAD");
    check(s_dpc.id && s_dpc.id != MAIN_ID, "DPC: [fs:[0x28]+0x12C] is its own, non-zero id");
    check(s_dpc.tls_data && s_dpc.tls_data != main_tls,
          "DPC: TlsData is not the main thread's");
    check(s_dpc.tls_data == s_dpc.tls_via_fs4,
          "DPC: TlsData is the block [fs:[4] + index*4] reaches");

    /* KPRCB.DpcRoutineActive (finding 4): 1 inside the DPC, 0 after it on
     * the timer thread, 0 on the main thread. */
    check(s_dpc.dpc_active == 1, "DPC: fs:[0x58] DpcRoutineActive is 1 inside the DPC");
    {
        int k;
        for (k = 0; k < 1000 && G8(s_dpc.fs + 0x58); k++)
            Sleep(1);
        check(s_dpc.fs && G8(s_dpc.fs + 0x58) == 0,
              "timer thread: fs:[0x58] back to 0 after the DPC");
    }
    check(me.dpc_active == 0, "main thread: fs:[0x58] is 0");

    /* The timer is signalled before its DPC runs, and a new KeSetTimer
     * makes it unsignalled again (findings 7 and 8). */
    check(s_dpc.timer_state == 1, "timer: SignalState is 1 when its DPC runs");
    args[0] = TIMER_VA; args[1] = (uint32_t)-100000000; args[2] = 0xFFFFFFFFu; args[3] = 0;
    kcall(K_SET_TIMER, 4, args);                     /* 10 s: stays armed */
    check(G32(TIMER_VA + 4) == 0, "timer: KeSetTimer clears SignalState");
    args[0] = TIMER_VA;
    check(kcall(K_CANCEL_TIMER, 1, args) == 1, "timer: KeCancelTimer of an armed timer is TRUE");
    /* Not armed now. The kernel's own list links (TimerListEntry, +24)
     * hold whatever the title or a previous insert left there. */
    G32(TIMER_VA + 24) = 0x00010C08u;
    args[0] = TIMER_VA;
    check(kcall(K_CANCEL_TIMER, 1, args) == 0,
          "timer: KeCancelTimer of a timer that is not armed is FALSE");

    /* KEVENT.SignalState follows the host event (finding 8). */
    args[0] = EV_SYNC; args[1] = 1; args[2] = 0;
    kcall(K_INIT_EVENT, 3, args);
    args[0] = EV_SYNC; args[1] = 0; args[2] = 0;
    kcall(K_SET_EVENT, 3, args);
    check(G32(EV_SYNC + 4) == 1, "event: KeSetEvent sets SignalState");
    args[0] = EV_SYNC; args[1] = 0; args[2] = 0; args[3] = 0; args[4] = ZERO_TIME;
    check(kcall(K_WAIT, 5, args) == 0, "event: the wait on a set event is satisfied");
    check(G32(EV_SYNC + 4) == 0, "event: a satisfied wait takes a synchronization event");
    args[0] = EV_NOTIF; args[1] = 0; args[2] = 0;
    kcall(K_INIT_EVENT, 3, args);
    args[0] = EV_NOTIF; args[1] = 0; args[2] = 0;
    kcall(K_SET_EVENT, 3, args);
    args[0] = EV_NOTIF; args[1] = 0; args[2] = 0; args[3] = 0; args[4] = ZERO_TIME;
    kcall(K_WAIT, 5, args);
    check(G32(EV_NOTIF + 4) == 1, "event: a notification event stays set after a wait");
    args[0] = EV_NOTIF;
    kcall(K_RESET_EVENT, 1, args);
    check(G32(EV_NOTIF + 4) == 0, "event: KeResetEvent clears SignalState");

    /* KeInitializeEvent again while a thread waits on the event (finding
     * 11): the waiter is orphaned, as on the console, not left on a closed
     * host object. */
    {
        HANDLE w;

        G32(TIME_1S) = (uint32_t)-10000000; G32(TIME_1S + 4) = 0xFFFFFFFFu;
        w = CreateThread(NULL, 0, waiter_thread, NULL, 0, NULL);
        Sleep(100);
        args[0] = EV_SYNC; args[1] = 1; args[2] = 0;
        kcall(K_INIT_EVENT, 3, args);
        /* Another event made and set: a host object freed under the waiter
         * is memory the next one may be built in. */
        args[0] = EV_OTHER; args[1] = 1; args[2] = 0;
        kcall(K_INIT_EVENT, 3, args);
        args[0] = EV_OTHER; args[1] = 0; args[2] = 0;
        kcall(K_SET_EVENT, 3, args);
        Sleep(300);
        check(!s_waiter_done, "re-init: a waiter on the old object is not failed or woken");
        WaitForSingleObject(w, 5000);
        CloseHandle(w);
        printf("  re-init: the waiter's wait ended with 0x%08X\n", s_waiter_status);
        check(s_waiter_done && s_waiter_status == 0x102,
              "re-init: the orphaned wait times out normally (STATUS_TIMEOUT)");
        args[0] = EV_SYNC; args[1] = 0; args[2] = 0;
        kcall(K_SET_EVENT, 3, args);
        args[0] = EV_SYNC; args[1] = 0; args[2] = 0; args[3] = 0; args[4] = ZERO_TIME;
        check(kcall(K_WAIT, 5, args) == 0, "re-init: the new object works");
    }

    /* Two host threads taking a TIB as the APU and OHCI threads do, made
     * while the main thread's IRQL byte says DISPATCH_LEVEL. */
    G8(XBOX_TIB_MAIN + 0x24) = 2;
    for (i = 0; i < 2; i++) {
        HANDLE h = CreateThread(NULL, 0, host_thread, &s_host[i], 0, NULL);
        DWORD rc = 1;
        WaitForSingleObject(h, 5000);
        GetExitCodeThread(h, &rc);
        CloseHandle(h);
        check(rc == 0, "host thread got a TIB");
    }
    check(s_host[0].kthread != main_kthread && s_host[1].kthread != main_kthread,
          "host threads: fs:[0x28] is not the main thread's KTHREAD");
    check(s_host[0].kthread != s_host[1].kthread && s_host[0].kthread != s_dpc.kthread,
          "host threads: each has its own KTHREAD");
    check(s_host[0].id && s_host[1].id && s_host[0].id != s_host[1].id
          && s_host[0].id != MAIN_ID && s_host[1].id != MAIN_ID && s_host[0].id != s_dpc.id,
          "host threads: ids are unique and non-zero");
    check(s_host[0].tls_data == s_host[0].tls_via_fs4
          && s_host[1].tls_data == s_host[1].tls_via_fs4
          && s_host[0].tls_data != s_host[1].tls_data,
          "host threads: own TlsData, consistent with fs:[4]");

    G8(XBOX_TIB_MAIN + 0x24) = 0;
    check(s_host[0].irql == 0 && s_host[1].irql == 0,
          "host threads: fs:[0x24] Irql starts at PASSIVE, not the main's");
    check(s_host[0].self == s_host[0].fs && s_host[1].self == s_host[1].fs,
          "host threads: fs:[0x18] NtTib.Self is the thread's own TIB");
    check(s_host[0].dpc_active == 0 && s_host[1].dpc_active == 0,
          "host threads: fs:[0x58] is 0 outside a DPC");

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}

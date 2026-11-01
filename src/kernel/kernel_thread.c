/*
 * kernel_thread.c - Xbox Threading Subsystem
 *
 * Implements Xbox thread creation, termination, delays, and priority
 * management using Win32 threading APIs.
 *
 * Xbox threading model:
 *   - PsCreateSystemThreadEx creates kernel-mode threads (→ CreateThread)
 *   - Thread start routines are __stdcall with a single PVOID context
 *   - Time intervals use NT 100-nanosecond units (negative = relative)
 *   - Thread priorities use NT KPRIORITY increments
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* XBOX_WORKER_STACK_* + worker-stack decls */
#include "recomp_env.h"           /* RECOMP_GUEST_CPUS */
#include "platform/android_host.h"  /* the fast core (Android) */
#include "platform/host_time.h"     /* the guest's clock */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

/* ============================================================================
 * Thread Start Wrapper
 *
 * Xbox start routines are __stdcall void(*)(PVOID), but Win32 CreateThread
 * expects DWORD WINAPI (*)(LPVOID). We wrap the Xbox routine to bridge
 * the calling convention and return type.
 * ============================================================================ */

typedef struct _XBOX_THREAD_START_INFO {
    PXBOX_SYSTEM_ROUTINE StartRoutine;
    PVOID                StartContext;
} XBOX_THREAD_START_INFO;

static DWORD WINAPI xbox_thread_wrapper(LPVOID lpParameter)
{
    XBOX_THREAD_START_INFO info = *(XBOX_THREAD_START_INFO*)lpParameter;

    xbox_log_thread_role("xbox-thread", 0);

    /* Free the start info before calling the routine - the routine may
     * never return (calling PsTerminateSystemThread instead) */
    HeapFree(GetProcessHeap(), 0, lpParameter);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u starting at %p",
        GetCurrentThreadId(), info.StartRoutine);

    info.StartRoutine(info.StartContext);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u returned normally",
        GetCurrentThreadId());

    return 0;
}

/* ============================================================================
 * PsCreateSystemThreadEx
 *
 * Xbox signature:
 *   PsCreateSystemThreadEx(
 *     OUT PHANDLE ThreadHandle,
 *     IN ULONG ThreadExtraSize,      // extra bytes in thread object (ignored)
 *     IN ULONG KernelStackSize,      // stack size (0 = default)
 *     IN ULONG TlsDataSize,          // TLS data size (Xbox-specific, ignored)
 *     OUT PULONG ThreadId,           // optional thread ID
 *     IN PVOID StartContext1,        // context passed to StartRoutine
 *     IN PVOID StartContext2,        // alternate context (unused by game code)
 *     IN BOOLEAN CreateSuspended,
 *     IN BOOLEAN DebugStack,         // debug stack (ignored)
 *     IN PXBOX_SYSTEM_ROUTINE StartRoutine
 *   )
 *
 * Maps to: CreateThread with a wrapper for calling convention adaptation.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle,
    ULONG ThreadExtraSize,
    ULONG KernelStackSize,
    ULONG TlsDataSize,
    PULONG ThreadId,
    PVOID StartContext1,
    PVOID StartContext2,
    BOOLEAN CreateSuspended,
    BOOLEAN DebugStack,
    PXBOX_SYSTEM_ROUTINE StartRoutine)
{
    XBOX_THREAD_START_INFO* info;
    HANDLE hThread;
    DWORD dwThreadId;
    DWORD dwCreationFlags;

    (void)ThreadExtraSize;
    (void)TlsDataSize;
    (void)StartContext2;
    (void)DebugStack;

    if (!ThreadHandle || !StartRoutine)
        return STATUS_INVALID_PARAMETER;

    /* Allocate start info - freed by the wrapper thread */
    info = (XBOX_THREAD_START_INFO*)HeapAlloc(GetProcessHeap(), 0, sizeof(XBOX_THREAD_START_INFO));
    if (!info)
        return STATUS_NO_MEMORY;

    info->StartRoutine = StartRoutine;
    info->StartContext = StartContext1;

    dwCreationFlags = CreateSuspended ? CREATE_SUSPENDED : 0;

    /* Use default stack size if 0 (Xbox default is 64KB) */
    if (KernelStackSize == 0)
        KernelStackSize = 65536;

    /* Not pinned to the guest core (xbox_GuestThreadPin): this thunk-table
     * path is for native-call titles, and the bridge titles that showed the
     * need go through bridge_PsCreateSystemThreadEx. */
    hThread = CreateThread(NULL, KernelStackSize, xbox_thread_wrapper, info,
                           dwCreationFlags, &dwThreadId);
    if (!hThread) {
        HeapFree(GetProcessHeap(), 0, info);
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "PsCreateSystemThreadEx: CreateThread failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *ThreadHandle = hThread;
    if (ThreadId)
        *ThreadId = dwThreadId;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THREAD,
        "PsCreateSystemThreadEx: created thread %u (handle=%p, routine=%p, suspended=%d)",
        dwThreadId, hThread, StartRoutine, CreateSuspended);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * PsTerminateSystemThread
 *
 * Terminates the calling thread. On Xbox this is the standard way for
 * system threads to exit. Maps directly to ExitThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "PsTerminateSystemThread: thread %u exiting with status 0x%08X",
        GetCurrentThreadId(), ExitStatus);

    ExitThread((DWORD)ExitStatus);

    /* ExitThread never returns, but the compiler needs this */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * KeDelayExecutionThread
 *
 * Delays the current thread. The interval uses NT 100-nanosecond units:
 *   - Negative values = relative delay (most common)
 *   - Positive values = absolute time (rare)
 *   - Zero = yield
 *
 * Maps to: SleepEx (for alertable waits) or Sleep
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeDelayExecutionThread(
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Interval)
{
    DWORD ms;

    (void)WaitMode;

    if (!Interval)
        return STATUS_INVALID_PARAMETER;

    if (Interval->QuadPart == 0) {
        /* Zero interval = yield the thread's time slice */
        SwitchToThread();
        return STATUS_SUCCESS;
    }

    if (Interval->QuadPart < 0) {
        /* Negative = relative time in 100ns units. Convert to milliseconds. */
        LONGLONG relative_100ns = -Interval->QuadPart;
        ms = (DWORD)(relative_100ns / 10000);
        /* Ensure at least 1ms for very short intervals */
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
    } else {
        /* Positive = absolute time. Calculate relative delay from now. */
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        now.QuadPart += (LONGLONG)(xbox_GuestSkewNs() / 100u);  /* the guest's clock */
        LONGLONG diff = Interval->QuadPart - now.QuadPart;
        if (diff <= 0)
            return STATUS_SUCCESS; /* Already past */
        ms = (DWORD)(diff / 10000);
    }

    /* A run that does not wait for the clock (host_time.h): the delay is on
     * the guest's clock and ends when that has been moved past it. */
    if (xbox_TimeWarpOn()) {
        xbox_GuestSleepNs((uint64_t)ms * 1000000u);
        if (Alertable && SleepEx(0, TRUE) == WAIT_IO_COMPLETION)
            return STATUS_ALERTED;
        return STATUS_SUCCESS;
    }

    if (Alertable) {
        DWORD result = SleepEx(ms, TRUE);
        if (result == WAIT_IO_COMPLETION)
            return STATUS_ALERTED;
    } else {
        Sleep(ms);
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Thread Priority
 *
 * Xbox uses NT KPRIORITY base priority increments relative to the process.
 * We map these to Win32 thread priority levels.
 * ============================================================================ */

/*
 * Map Xbox priority increment to Win32 priority level.
 * Xbox base priorities typically range from -2 to +2 for game threads.
 */
static int xbox_priority_to_win32(LONG increment)
{
    if (increment <= -15)       return THREAD_PRIORITY_IDLE;
    else if (increment <= -2)   return THREAD_PRIORITY_LOWEST;
    else if (increment == -1)   return THREAD_PRIORITY_BELOW_NORMAL;
    else if (increment == 0)    return THREAD_PRIORITY_NORMAL;
    else if (increment == 1)    return THREAD_PRIORITY_ABOVE_NORMAL;
    else if (increment <= 2)    return THREAD_PRIORITY_HIGHEST;
    else                        return THREAD_PRIORITY_TIME_CRITICAL;
}

static LONG win32_priority_to_xbox(int priority)
{
    switch (priority) {
        case THREAD_PRIORITY_IDLE:          return -15;
        case THREAD_PRIORITY_LOWEST:        return -2;
        case THREAD_PRIORITY_BELOW_NORMAL:  return -1;
        case THREAD_PRIORITY_NORMAL:        return 0;
        case THREAD_PRIORITY_ABOVE_NORMAL:  return 1;
        case THREAD_PRIORITY_HIGHEST:       return 2;
        case THREAD_PRIORITY_TIME_CRITICAL: return 15;
        default:                            return 0;
    }
}

LONG __stdcall xbox_KeSetBasePriorityThread(PVOID Thread, LONG Increment)
{
    HANDLE hThread = (HANDLE)Thread;
    LONG previous;

    /* Get previous priority before setting new one */
    int prev_win32 = GetThreadPriority(hThread);
    previous = win32_priority_to_xbox(prev_win32);

    int new_win32 = xbox_priority_to_win32(Increment);
    SetThreadPriority(hThread, new_win32);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "KeSetBasePriorityThread: thread=%p, increment=%d (win32=%d), prev=%d",
        Thread, Increment, new_win32, previous);

    return previous;
}

LONG __stdcall xbox_KeQueryBasePriorityThread(PVOID Thread)
{
    HANDLE hThread = (HANDLE)Thread;
    int win32_priority = GetThreadPriority(hThread);
    return win32_priority_to_xbox(win32_priority);
}

/* ============================================================================
 * KeAlertThread
 *
 * Sends an alert to a thread, which can wake it from an alertable wait.
 * On Xbox, this sets the alerted flag on the thread object.
 * We approximate this with QueueUserAPC using a no-op APC routine.
 * ============================================================================ */

static VOID CALLBACK xbox_alert_apc(ULONG_PTR dwParam)
{
    (void)dwParam;
    /* No-op - the purpose is just to wake the thread from alertable wait */
}

NTSTATUS __stdcall xbox_KeAlertThread(PVOID Thread, KPROCESSOR_MODE AlertMode)
{
    HANDLE hThread = (HANDLE)Thread;

    (void)AlertMode;

    if (!QueueUserAPC(xbox_alert_apc, hThread, 0)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THREAD,
            "KeAlertThread: QueueUserAPC failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtYieldExecution
 *
 * Yields the current thread's remaining time slice.
 * Maps directly to SwitchToThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtDuplicateObject
 *
 * Duplicates a kernel handle. On Xbox this is simpler than Win32 since
 * there's only one process. Maps to DuplicateHandle within the same process.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtDuplicateObject(
    HANDLE SourceHandle,
    PHANDLE TargetHandle,
    ULONG Options)
{
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwOptions = 0;

    if (!TargetHandle)
        return STATUS_INVALID_PARAMETER;

    /* Xbox DUPLICATE_CLOSE_SOURCE = 0x1, same as Win32 */
    if (Options & 0x1)
        dwOptions |= DUPLICATE_CLOSE_SOURCE;
    /* Xbox DUPLICATE_SAME_ACCESS = 0x2, same as Win32 */
    if (Options & 0x2)
        dwOptions |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(hProcess, SourceHandle, hProcess, TargetHandle,
                         0, FALSE, dwOptions)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtDuplicateObject: DuplicateHandle failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtDuplicateObject: source=%p → target=%p (options=0x%X)",
        SourceHandle, *TargetHandle, Options);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSuspendThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /*
     * SuspendThread returns the previous suspend count, or (DWORD)-1 on
     * failure. The Xbox call reports that count through an out-parameter, so
     * the two are not interchangeable: -1 must become an error status rather
     * than a suspend count of 0xFFFFFFFF.
     */
    prev = SuspendThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtSuspendThread: SuspendThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtSuspendThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtResumeThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /* Mirror of NtSuspendThread: (DWORD)-1 is failure, not a count. */
    prev = ResumeThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtResumeThread: ResumeThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtResumeThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}


/* ================================================================
 * Worker stack slices + game-thread tracking (host-tick-driven titles)
 * ================================================================
 *
 * Ported from the Burnout 3 fork as the runtimes reunite. A host-driven title
 * returns from its entry after spawning an init thread and expects the host's
 * own thread to drive the per-frame tick; to call recompiled code from there it
 * needs a guest stack, which a worker slice provides. Additive: a default-model
 * title never calls any of this, so it is inert for Halo, Crimson Skies, etc.
 * See docs/technical/burnout3-reunification.md and XBOX_WORKER_STACK_* in
 * xbox_memory_layout.h.
 */

/* The game's own thread, kept so a wedged boot can be inspected from the host
 * watchdog. Set when a title's game thread is spawned; NULL under the default
 * inline model, where there is no separate thread to sample. */
static HANDLE g_game_thread = NULL;

void  xbox_set_game_thread(void *h) { g_game_thread = (HANDLE)h; }
void *xbox_thread_debug_handle(void) { return (void *)g_game_thread; }

/* One bit per worker stack slice. Interlocked because a host-driven title can
 * allocate a slice from the host thread while recompiled code allocates one for
 * a spawned worker, so the allocator itself must be thread-safe. */
static volatile LONG g_worker_stack_used[XBOX_WORKER_STACK_COUNT];

int xbox_worker_stack_alloc(void)
{
    int i;
    for (i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
        /* Slices carve the main stack region, which shrinks for large images
         * (xbox_memory_layout.h): hand out only the ones that fit in it. */
        if ((uint32_t)(i + 1) * XBOX_WORKER_STACK_SIZE > XBOX_STACK_SIZE)
            break;
        if (InterlockedCompareExchange(&g_worker_stack_used[i], 1, 0) == 0)
            return i;
    }
    return -1;  /* all slices in use */
}

void xbox_worker_stack_free(int slot)
{
    if (slot >= 0 && slot < XBOX_WORKER_STACK_COUNT)
        InterlockedExchange(&g_worker_stack_used[slot], 0);
}

/* ============================================================================
 * One host core for the title's threads
 *
 * The console has one CPU, and titles lean on it: two threads of equal
 * priority only interleave at a quantum boundary or a wait, so a short
 * read-modify-write on a shared global never sees the other thread's half.
 * BLiNX 2's stage load spawns a loader thread, and both it and the main
 * thread add lights to one global pool with "slot = count; fill; count =
 * slot + 1". On two host cores the two loops ran at the same time and lost
 * each other's entries, and every terrain piece baked from the mangled pool
 * came out black (the dark water of 2026-10-07). Pinning every guest thread
 * to one core gives the console's interleaving back: the host still
 * preempts at its tick, but inside a window of a few instructions that is
 * as rare as it was on the hardware.
 *
 * Pinned: the main guest thread and every PsCreateSystemThreadEx worker,
 * the threads the title schedules. Not pinned: the kernel timer thread,
 * which runs the title's DPCs and timer routines, and the device models'
 * threads (APU, OHCI, nv2a), which deliver ISRs while the one-CPU gate is
 * free. They do run lifted code, but they stand in for interrupts, which
 * on the console ran between any two instructions of the thread they
 * interrupted; the gate already orders them against guest code at
 * DISPATCH_LEVEL, and putting them on the guest core would queue a DPC
 * behind a main thread spinning in its frame wait, the latency the m1hang
 * work removed. The GPU and the audio keep their cores for the same
 * reason. RECOMP_GUEST_CPUS=all is the A/B, =<n> picks the core (cpu 0
 * takes most of the IRQ work on Linux).
 *
 * A title may opt out of the pin, before its first guest thread starts,
 * with xbox_GuestCpusDefaultUnpinned(), once it has evidence that its own
 * race class is empty; guest_cpus=<n> (or =all) still overrides. Burnout 3
 * does on Windows (BO-282.10): the Mac has run its guest threads on every
 * core since the start with no such bug; a lockstep race (BO3_LOCKSTEP,
 * HR off) gave the same step hashes over all 24705 steps pinned and
 * unpinned; and on a Ryzen 7 5800X the pin cost it pictures, the title's
 * thread waiting for its idle core to wake after each hand-off sleep
 * (3 x 3 interleaved, 120 Hz drift corners: frames over 9 ms 560/609/550
 * pinned, 316/321/295 unpinned; late flips 56/72/47 against 5/2/2). The
 * toolkit's default stays the pin: BLiNX 2's light pool above.
 *
 * =token keeps the same rule where there is no affinity to set: Darwin on
 * Apple Silicon, where SetThreadAffinityMask does nothing and the pin above
 * left every guest thread on every core. The threads that would be pinned
 * join a token instead (win32_compat.c, ported from vyanhursky/xboxrecomp
 * dc00f70): one of them runs at a time, every blocking primitive gives the
 * token up, and a thread that has waited a millisecond preempts the holder
 * -- at once while the holder is in the title's lifted code, at its next
 * kernel call's return (kernel_bridge.c) while it is in host code. The
 * lifted code is the __TEXT,__guest section, where a title puts it by
 * force-including templates/runtime/recomp_guest_section.h; without one a
 * guest thread spinning with no kernel call keeps the token, the deadlock
 * upstream's cooperative RECOMP_GUEST_LOCK (#160) documents. The threads
 * left off the core are left off the token too, so an interrupt still runs
 * beside the holder, ordered by the one-CPU gate as it is with affinity.
 * Off by default: no thread joins, and nothing changes for any title.
 */
static int guest_cpus_token(void)
{
    const char *mode = recomp_env(RENV_GUEST_CPUS);
    return mode && !strcmp(mode, "token");
}

static int s_default_unpinned;

void xbox_GuestCpusDefaultUnpinned(void)
{
    s_default_unpinned = 1;
}

DWORD_PTR xbox_GuestCpuMask(void)
{
    const char *mode = recomp_env(RENV_GUEST_CPUS);
    DWORD_PTR proc = 0, sys = 0;

    if (mode && !strcmp(mode, "all"))
        return 0;
    /* A title that opted in (xbox_GuestCpusDefaultUnpinned) runs unpinned
     * unless guest_cpus asks for a core. */
    if (s_default_unpinned && (!mode || !*mode))
        return 0;
#if !defined(_WIN32)
    if (guest_cpus_token())
        return 0;   /* the token, not a core, holds them to one CPU */
#endif
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc)
        return 0;
    /* =<n>: that core, when the process may use it. */
    if (mode && *mode >= '0' && *mode <= '9') {
        long n = strtol(mode, NULL, 10);
        if (n >= 0 && n < (long)(8 * sizeof proc) && (proc & ((DWORD_PTR)1 << n)))
            return (DWORD_PTR)1 << n;
        fprintf(stderr, "  [KERNEL] guest_cpus=%s: not a core the process may"
                        " use (mask %#lx); taking the lowest\n", mode,
                (unsigned long)proc);
    }
#if defined(_WIN32)
    /* Not core 0 or its SMT sibling: Windows steers most device interrupts
     * and DPCs (GPU, audio, USB, NIC) and a good deal of other processes'
     * work at the first core, and a title thread pinned there stalled for
     * 20-90 ms at a time (the guest ran 1.6 % of 60 Hz race frames late on
     * a Ryzen 7 5800X + RX 9060 XT, none in 20 000 on another core; BO-75).
     * The first even-numbered logical processor from 2 on, the first thread
     * of the second physical core on an SMT part; a one- or two-thread host
     * has no choice. */
    {
        DWORD_PTR rest = proc & ~(DWORD_PTR)3;
        DWORD_PTR even = rest & (DWORD_PTR)0x5555555555555555ull;
        if (even)
            return even & (DWORD_PTR)(-(intptr_t)even);
        if (rest)
            return rest & (DWORD_PTR)(-(intptr_t)rest);
    }
#endif
#if defined(__ANDROID__)
    /* Not the lowest: on a phone's little/big/prime layout that is a little
     * core, a fraction of the speed. The fastest core is left to the
     * executor, the heavier thread (kernel_bridge.c pins it there; measured
     * on a Snapdragon 865: 60 % of a core against the guest's 29 %); the
     * guest's threads take a core of the next kind, or the fastest when
     * there is only one kind (android_host.h). */
    {
        int fast = recomp_android_cpu_rank(1);
        if (fast < 0)
            fast = recomp_android_fast_cpu();
        if (fast >= 0 && fast < (int)(8 * sizeof proc) && (proc & ((DWORD_PTR)1 << fast)))
            return (DWORD_PTR)1 << fast;
    }
#endif
    /* The lowest core the process may use: fixed, so every thread lands
     * on the same one whatever core it was created from. */
    return proc & (DWORD_PTR)(-(intptr_t)proc);
}

#if !defined(_WIN32)
static BOOL CALLBACK guest_turn_setup(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
#if defined(__APPLE__)
    if (!guest_turn_has_code()) {
        unsigned long size = 0;
        const void *code = getsectiondata(&_mh_execute_header, "__TEXT",
                                          "__guest", &size);
        if (code && size)
            guest_turn_set_code(code, (size_t)size);
    }
#endif
    if (guest_turn_has_code())
        fprintf(stderr, "  [KERNEL] guest threads take turns on one guest CPU"
                        " (guest_cpus=token); preempted in the lifted code\n");
    else
        fprintf(stderr, "  [KERNEL] guest threads take turns on one guest CPU"
                        " (guest_cpus=token); no __TEXT,__guest section, so"
                        " they change over only at kernel calls and waits\n");
    fflush(stderr);
    return TRUE;
}
#endif

#if defined(_WIN32)
/* ============================================================================
 * Windows: the priority class of a process with pinned threads
 *
 * A pinned thread cannot move. Windows gives each processor to the
 * highest-priority thread ready for it, and a thread that may use one core
 * waits in that core's queue while a thread of equal or higher priority
 * runs there, however many other cores are idle. The title's threads sleep
 * most of each frame (the guest core was idle 85 % of a race on a Ryzen 7
 * 5800X), so Windows keeps placing other threads on that core, and a title
 * thread that then wakes gets it at once only from a thread it outranks.
 *
 * Its rank was whatever the process was started with. From Explorer that is
 * the normal class: title threads at 8, 10 after a wait while the window
 * has the foreground. From Task Scheduler, which is how a PC is driven
 * over SSH, it is the below-normal class (a task's default priority is 7):
 * 6, and 8 in the foreground. Ordinary work runs at 8 and up. Races on that
 * PC, with a CPU-bound normal-priority loop pinned to the guest core for
 * 3-6 s, and with sixteen CPU-bound jobs at once anywhere for 30 s (what a
 * parallel build on the same PC does), by kernel context-switch traces and
 * flip times (BO-75):
 *
 *   class, window        the loop on the core       sixteen jobs
 *   below normal, front  flips 150-420 ms apart     10 flips/s, 139 over 100 ms
 *   below normal, back   one gap of 5.9 s           3 flips/s, gaps to 4.7 s
 *   normal, front        60 flips/s, none late      60 flips/s, none over 42 ms
 *   normal, back         2 over 100 ms (156 ms)     39 flips/s, 23 over 100 ms
 *   above normal, front  60 flips/s, none late      56 flips/s, none over 66 ms
 *   above normal, back   60 flips/s, none late      55 flips/s, none over 68 ms
 *
 * (Flips per second with the title's own 30 Hz crash cameras in them: an
 * undisturbed race reads 55-60.) In the trace of the first row the loop
 * had the core 99 % of the time and
 * the title's main thread got it for under a millisecond at each of the
 * loop's quantum ends. The 0.3-5 s stalls first seen in some races fell in
 * an hour when another job built on that PC, with the game below normal.
 *
 * So while its threads are pinned the process is in the above-normal class
 * at least, whatever it was started in, window or headless, test or not:
 * what is measured is what a player gets. DOSBox runs the same way by
 * default while it has the focus ("priority = higher,normal"). It is the
 * whole process that moves, so the title's own relative priorities and the
 * boosts that order its threads among themselves are as they were; the
 * compositor, the audio engine and input (13 and up) stay above it. For
 * everything else on the PC it means what the class means: where one of
 * this process's threads and a normal-priority thread want the same core,
 * this one runs first. The title's threads use about an eighth of their
 * core and the executor most of another; the other cores are untouched.
 * RECOMP_DEBUG=host_priority=keep leaves the class as started, =normal and
 * =high pick another; a class is only ever raised.
 * ============================================================================ */
static const char *priority_class_name(DWORD cls)
{
    switch (cls) {
    case IDLE_PRIORITY_CLASS:         return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below normal";
    case NORMAL_PRIORITY_CLASS:       return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above normal";
    case HIGH_PRIORITY_CLASS:         return "high";
    case REALTIME_PRIORITY_CLASS:     return "realtime";
    default:                          return "unknown";
    }
}

static int priority_class_rank(DWORD cls)
{
    switch (cls) {
    case IDLE_PRIORITY_CLASS:         return 0;
    case BELOW_NORMAL_PRIORITY_CLASS: return 1;
    case ABOVE_NORMAL_PRIORITY_CLASS: return 3;
    case HIGH_PRIORITY_CLASS:         return 4;
    case REALTIME_PRIORITY_CLASS:     return 5;
    default:                          return 2;     /* normal */
    }
}

static BOOL CALLBACK host_priority_apply(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const char *want = recomp_env(RENV_HOST_PRIORITY);
    DWORD cur = GetPriorityClass(GetCurrentProcess());
    DWORD cls = ABOVE_NORMAL_PRIORITY_CLASS;

    (void)once; (void)param; (void)ctx;
    if (want && !strcmp(want, "keep"))
        cls = 0;
    else if (want && !strcmp(want, "normal"))
        cls = NORMAL_PRIORITY_CLASS;
    else if (want && !strcmp(want, "high"))
        cls = HIGH_PRIORITY_CLASS;
    else if (want && *want && strcmp(want, "above"))
        fprintf(stderr, "  [KERNEL] host_priority=%s: not keep, normal, above"
                        " or high; taking above\n", want);
    if (!cur)
        fprintf(stderr, "  [KERNEL] process priority class: not readable"
                        " (error %lu)\n", (unsigned long)GetLastError());
    else if (!cls || priority_class_rank(cur) >= priority_class_rank(cls))
        fprintf(stderr, "  [KERNEL] process priority class: %s, as started%s\n",
                priority_class_name(cur),
                priority_class_rank(cur) < priority_class_rank(NORMAL_PRIORITY_CLASS)
                ? ": any normal-priority thread on the title's core holds"
                  " the title up" : "");
    else if (SetPriorityClass(GetCurrentProcess(), cls))
        fprintf(stderr, "  [KERNEL] process priority class: %s, started %s"
                        " (RECOMP_DEBUG=host_priority=keep: as started)\n",
                priority_class_name(cls), priority_class_name(cur));
    else
        fprintf(stderr, "  [KERNEL] process priority class: %s; raising it to"
                        " %s failed (error %lu)\n", priority_class_name(cur),
                priority_class_name(cls), (unsigned long)GetLastError());
    fflush(stderr);
    return TRUE;
}
#endif /* _WIN32 */

void xbox_GuestThreadPin(const char *role)
{
    static int announced, failed;
    DWORD_PTR mask;
    int core = 0;

    xbox_FrametimeThread(role);   /* RECOMP_TRACE=frametime samples it */

#if !defined(_WIN32)
    if (guest_cpus_token()) {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&once, guest_turn_setup, NULL, NULL);
        guest_turn_join();
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "guest thread %s joins the"
                 " guest CPU token", role ? role : "?");
        return;
    }
#endif
    mask = xbox_GuestCpuMask();
    if (!mask)
        return;
    while (!((mask >> core) & 1))
        core++;
#if defined(_WIN32)
    {
        /* Before the first pin: a pinned thread needs the class. */
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&once, host_priority_apply, NULL, NULL);
    }
#endif
    if (!SetThreadAffinityMask(GetCurrentThread(), mask)) {
        if (!failed) {
            failed = 1;
            fprintf(stderr, "  [KERNEL] guest threads: this host sets no thread"
                            " affinity; the title's threads stay on every core\n");
            fflush(stderr);
        }
        return;
    }
    if (!announced) {
        announced = 1;
        fprintf(stderr, "  [KERNEL] guest threads on one host core (cpu %d),"
                        " as on the console (RECOMP_GUEST_CPUS=all: every core)\n",
                core);
        fflush(stderr);
    }
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "guest thread %s on cpu %d",
             role ? role : "?", core);
}

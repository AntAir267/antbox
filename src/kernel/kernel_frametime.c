/*
 * kernel_frametime.c - RECOMP_TRACE=frametime=<file>: one CSV line per flip,
 * saying where that frame's time went.
 *
 * Two sources:
 *
 *  - The executor (nv2a_pb_exec.c, the nv2a-ack thread) hands in, at every
 *    FLIP_STALL, its running totals: the time held for the swap's vblank,
 *    time walking the pushbuffer (and inside that the backend's draws and
 *    present), so a frame's host NV2A emulation is the difference.
 *
 *  - A sampler thread looks at every guest thread (the ones
 *    xbox_GuestThreadPin registers) every RECOMP_TRACE=frametime_us (500)
 *    microseconds: suspended for the read, its program counter and whether it
 *    is running. A running thread whose PC is in a lifted function is
 *    charged to the XBE section that function came from: .text and the
 *    other title sections are "game", the XDK's D3D and XGRPH sections
 *    "d3d", any other XDK section (DSOUND, XMV, WMADEC, XPP, XNET...) "xdk".
 *    Functions listed in RECOMP_TRACE=frametime_spin=<va>[+<va>...] are
 *    "spin" instead (a busy-wait for the GPU, say). A running thread
 *    anywhere else is in host code reached from the title ("host": kernel
 *    HLE, MMIO traps, spin-wait lowering); a blocked one is "wait".
 *    A sample is worth the time since the sampler's previous pass, not the
 *    period it asked to sleep: a pass takes its own time (every thread is
 *    stopped and read) and a sleep overshoots, so passes were 0.66 ms apart
 *    on an M5 and each counted as 0.5 ms, and every column read 24% low
 *    (BO-102: the menu film's decode 1.70 ms a frame where `sample` had
 *    2.14). A thread's columns now add up to the frame.
 *
 * The lifted function under a PC is found by a sorted table of every
 * function recomp_lookup knows, built once from the XBE's own section
 * table; on macOS the __TEXT,__guest section (templates/runtime/
 * recomp_guest_section.h) bounds it, elsewhere the first and last lifted
 * function do. This is the sampling Instruments or `sample` does, only
 * bucketed per frame and by guest address range, so a run's CSV can be
 * compared with another's (tools/perf/framestats.py).
 *
 * Columns (milliseconds unless named otherwise):
 *   flip, t_ms         the flip and its time since the first
 *   frame_ms           since the previous flip
 *   iv, late           the interval the swap asked for, and whether its
 *                      vblank had already passed (flip pacing)
 *   ex_hold            executor held for the vblank (FLIP_STALL)
 *   ex_busy            executor walking the pushbuffer, minus the hold
 *   ex_draw, ex_present  the backend's draws and present, inside ex_busy
 *   ex_idle            executor waiting for the title to submit
 *   g_game, g_d3d, g_xdk, g_spin, g_host, g_wait
 *                      guest threads' sampled time, summed over threads
 *   g_threads          guest threads sampled
 *   cpu_proc           process CPU time in the frame (all threads)
 *   g_io               guest threads blocked in uninterruptible sleep (disk
 *                      I/O: Linux D state, Mach TH_STATE_UNINTERRUPTIBLE);
 *                      sampled like g_wait, and not counted in it
 *   g_runq             time guest threads were runnable but not running
 *                      (preempted, or queued behind another thread on the
 *                      guest core), exact from /proc/<tid>/schedstat; Linux
 *                      only, 0 elsewhere
 *   m_game ... m_runq  the same buckets for the guest-main thread alone
 *                      (the one the executor waits for)
 *   gpu_ms, tu_ms, tu_pred  the GPU backend's (xbox_FrametimeGpu, BO-245):
 *                      GPU time of the last frame known complete (-1: not
 *                      known; Metal: three flips back), the texture
 *                      upscaler's chunks completed since the last flip, and
 *                      the chunk ms it predicted for this flip
 *   ex_cpu             CPU time of the executor's own thread in the frame:
 *                      beside ex_busy it tells a frame the executor worked
 *                      through from one it sat out, blocked or descheduled
 *                      (Windows: QueryThreadCycleTime over the TSC's rate,
 *                      GetThreadTimes there moves in 15.6 ms steps)
 *
 * RECOMP_TRACE=frametime_us=0 starts no sampler: the executor's columns
 * only (ex_*, cpu_proc), with no thread stopped 2000 times a second, for a
 * run whose pacing is itself the measurement.
 *
 * RECOMP_TRACE=frametime_host=<file> says where guest-main's host time
 * goes: each of its samples in host code also records the host stack (the
 * frame-record walk on Mach, glibc backtrace() in the Linux handler, as
 * xbox_crash.c unwinds), and every frame whose m_host reaches
 * frametime_host_ms (4) is written out with its stacks, identical ones
 * counted together, each address as symbol+offset where dladdr knows one,
 * else module+offset (addr2line -e <binary> takes that offset), and a
 * frame in lifted code as the guest function it came from.
 *
 * Linux has no thread_suspend/GetThreadContext, so the sampler reads each
 * thread's run state from /proc/self/task/<tid>/stat (R, S, D) and sends a
 * running one SIGPROF; the handler, on that thread, buckets its own
 * interrupted PC. This is how perf-less profilers (gperftools' SIGPROF
 * timer, Go's runtime/pprof) sample a thread's PC on Linux.
 *
 * Off, the executor's call is one load and a branch.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#elif defined(_WIN32)
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#define FT_HAVE_TSC 1
#endif
#elif defined(__linux__)
#include <pthread.h>
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#endif

typedef void (*ft_func_t)(void);
extern ft_func_t recomp_lookup(uint32_t xbox_va);

enum { B_GAME, B_D3D, B_XDK, B_SPIN, B_HOST, B_WAIT, B_IO, B_COUNT };

#define FT_MAX_THREADS 32
#define FT_MAX_SECTIONS 32
#define FT_MAX_SPIN 16

typedef struct {
    uintptr_t host;      /* the lifted function's address */
    uint32_t  va;        /* its guest address */
    uint8_t   bucket;    /* B_GAME / B_D3D / B_XDK / B_SPIN */
} FtFunc;

static int s_state = -1;            /* -1 unread, 0 off, 1 on */
static FILE *s_out;

/* The rows reach the file from a thread of their own (BO-245): the executor
 * only formats a row into this ring. Its own fflush every 64 flips was a
 * write() on the executor, and on the Deck's SD card, busy writing back the
 * texture upscaler's cache files, that write waited 70-240 ms: the cold-run
 * hitches at flips 5696 and 7040 (both multiples of 64) were the trace's
 * own. A full ring drops the row (counted, said once) rather than wait.
 * Windows keeps the direct write. */
#if defined(__APPLE__) || defined(__linux__)
#define FT_RING_BYTES (1u << 20)    /* some 3800 rows, a minute at 60 fps */
static char           *s_ring;
static size_t          s_ring_head, s_ring_len;   /* the writer's start, bytes waiting */
static int             s_ring_busy;               /* the writer is in fwrite */
static unsigned long   s_ring_lost;
static pthread_mutex_t s_ring_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_ring_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  s_ring_idle = PTHREAD_COND_INITIALIZER;

static void *ring_writer(void *p)
{
    (void)p;
    pthread_mutex_lock(&s_ring_mx);
    for (;;) {
        size_t n;
        while (!s_ring_len)
            pthread_cond_wait(&s_ring_cv, &s_ring_mx);
        n = s_ring_len < FT_RING_BYTES - s_ring_head ? s_ring_len : FT_RING_BYTES - s_ring_head;
        s_ring_busy = 1;
        pthread_mutex_unlock(&s_ring_mx);
        /* The bytes [head, head + n) are the writer's until it moves head. */
        fwrite(s_ring + s_ring_head, 1, n, s_out);
        pthread_mutex_lock(&s_ring_mx);
        s_ring_head = (s_ring_head + n) % FT_RING_BYTES;
        s_ring_len -= n;
        if (!s_ring_len) {
            pthread_mutex_unlock(&s_ring_mx);
            fflush(s_out);
            pthread_mutex_lock(&s_ring_mx);
        }
        s_ring_busy = 0;
        pthread_cond_broadcast(&s_ring_idle);
    }
    return NULL;
}

/* At exit: what is still in the ring, written (at most 2 s). */
static void ring_drain(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&s_ring_mx);
    pthread_cond_signal(&s_ring_cv);
    while (s_ring_len || s_ring_busy)
        if (pthread_cond_timedwait(&s_ring_idle, &s_ring_mx, &until) != 0)
            break;
    pthread_mutex_unlock(&s_ring_mx);
}

static void ring_start(void)
{
    pthread_t th;
    s_ring = (char *)malloc(FT_RING_BYTES);
    if (!s_ring)
        return;
    if (pthread_create(&th, NULL, ring_writer, NULL) != 0) {
        free(s_ring);
        s_ring = NULL;
        return;
    }
    pthread_detach(th);
    atexit(ring_drain);
}

/* 1: the row is the writer's now; 0: no ring, write it here. */
static int ring_put(const char *row, size_t n, int wake)
{
    size_t at, first;
    if (!s_ring)
        return 0;
    pthread_mutex_lock(&s_ring_mx);
    if (s_ring_len + n > FT_RING_BYTES) {
        if (!s_ring_lost++)
            fprintf(stderr, "  [FRAMETIME] the file writer is behind: rows dropped\n");
        pthread_mutex_unlock(&s_ring_mx);
        return 1;
    }
    at = (s_ring_head + s_ring_len) % FT_RING_BYTES;
    first = n < FT_RING_BYTES - at ? n : FT_RING_BYTES - at;
    memcpy(s_ring + at, row, first);
    memcpy(s_ring, row + first, n - first);
    s_ring_len += n;
    if (wake)
        pthread_cond_signal(&s_ring_cv);
    pthread_mutex_unlock(&s_ring_mx);
    return 1;
}
#else
static void ring_start(void) { }
static int ring_put(const char *row, size_t n, int wake)
{
    (void)row; (void)n; (void)wake;
    return 0;
}
#endif

static FtFunc *s_funcs;
static size_t s_nfuncs;
static uintptr_t s_code_lo, s_code_hi;
static uint32_t s_spin[FT_MAX_SPIN];
static int s_nspin;
static unsigned s_period_us = 500;
static int s_nosample;              /* frametime_us=0: no sampler thread */

/* Per-frame sampled time in microseconds, by bucket: the sampler adds, the
 * flip takes. s_mcount: the guest-main thread's alone. s_pass_us: what one
 * sample of the current pass is worth, the time since the pass before it
 * (the Linux handler reads it on the sampled thread). s_runq_ns / s_mrunq_ns: time
 * runnable but not running (Linux schedstat), summed / main only. */
static volatile LONG s_count[B_COUNT];
static volatile LONG s_mcount[B_COUNT];
static volatile LONG s_nthreads;
static volatile LONG s_main_slot = -1;
static volatile LONG s_pass_us = 500;
#if defined(__linux__)
static volatile int64_t s_runq_ns, s_mrunq_ns;
#endif

/* frametime_host: guest-main's host-code samples this frame, with stacks. */
#define HS_MAX    512
#define HS_FRAMES 12
typedef struct { int n; uintptr_t pc[HS_FRAMES]; } HostStack;
static FILE *s_hout;
static unsigned s_host_min = 4000; /* microseconds: frametime_host_ms */
static HostStack s_hs[HS_MAX];
static volatile LONG s_hs_n;

static void count_sample(LONG slot, int b)
{
    LONG us = s_pass_us;
    InterlockedExchangeAdd(&s_count[b], us);
    if (slot >= 0 && slot == s_main_slot)
        InterlockedExchangeAdd(&s_mcount[b], us);
}

#if defined(__APPLE__)
static mach_port_t s_threads[FT_MAX_THREADS];
#elif defined(_WIN32)
static HANDLE s_threads[FT_MAX_THREADS];
static ULONG64 s_cycles[FT_MAX_THREADS];
static uintptr_t s_lastpc[FT_MAX_THREADS];
#elif defined(__linux__)
static pthread_t s_threads[FT_MAX_THREADS];
static int s_statfd[FT_MAX_THREADS], s_schedfd[FT_MAX_THREADS];
static uint64_t s_runq_last[FT_MAX_THREADS];
static volatile int s_ready[FT_MAX_THREADS];
static __thread LONG t_slot = -1;   /* the sampled thread's own slot */
#endif

static int cmp_func(const void *a, const void *b)
{
    uintptr_t x = ((const FtFunc *)a)->host, y = ((const FtFunc *)b)->host;
    return x < y ? -1 : x > y;
}

/* The XBE section a guest address lies in: its name, from the image's own
 * section headers (XBE base 0x10000: count at +0x11C, headers at +0x120,
 * 0x38 bytes each, virtual address +4, size +8, name pointer +0x14). */
static int section_bucket(const char *name)
{
    static const char *const xdk[] = {
        "DSOUND", "XMV", "WMADEC", "XONLINE", "XNET", "XNETS", "XNETN", "XPP",
        "XACTENG", "XAPI", "DOLBY", "XON_RD", "D3DX", NULL };
    int i;
    if (!strcmp(name, "D3D") || !strcmp(name, "XGRPH"))
        return B_D3D;
    /* The XDK's other libraries keep sections of their own. */
    for (i = 0; xdk[i]; i++)
        if (!strcmp(name, xdk[i]))
            return B_XDK;
    return B_GAME;   /* .text and whatever else the title linked */
}

static void build_table(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t n = *(const uint32_t *)(mem + 0x1011C);
    uint32_t hdr = *(const uint32_t *)(mem + 0x10120);
    size_t cap = 32768;
    uint32_t i, va;

    s_funcs = (FtFunc *)malloc(cap * sizeof *s_funcs);
    if (!s_funcs || n > FT_MAX_SECTIONS || hdr < 0x10000 || hdr > 0x1000000)
        return;
    for (i = 0; i < n; i++) {
        const uint8_t *h = mem + hdr + i * 0x38u;
        uint32_t flags = *(const uint32_t *)h;
        uint32_t lo = *(const uint32_t *)(h + 4), sz = *(const uint32_t *)(h + 8);
        uint32_t name_va = *(const uint32_t *)(h + 0x14);
        char name[16] = "";
        int bucket;
        if (!(flags & 4))            /* XBE section flag 4: executable */
            continue;
        if (name_va >= 0x10000 && name_va < 0x1000000)
            snprintf(name, sizeof name, "%s", (const char *)(mem + name_va));
        bucket = section_bucket(name);
        fprintf(stderr, "  [FRAMETIME] section %-8s 0x%08X-0x%08X -> %s\n", name, lo,
                lo + sz, bucket == B_D3D ? "d3d" : bucket == B_GAME ? "game" : "xdk");
        for (va = lo; va < lo + sz; va++) {
            ft_func_t f = recomp_lookup(va);
            int k;
            if (!f)
                continue;
            if (s_nfuncs == cap) {
                FtFunc *g = (FtFunc *)realloc(s_funcs, cap * 2 * sizeof *s_funcs);
                if (!g)
                    break;
                s_funcs = g;
                cap *= 2;
            }
            s_funcs[s_nfuncs].host = (uintptr_t)f;
            s_funcs[s_nfuncs].va = va;
            s_funcs[s_nfuncs].bucket = (uint8_t)bucket;
            for (k = 0; k < s_nspin; k++)
                if (s_spin[k] == va)
                    s_funcs[s_nfuncs].bucket = B_SPIN;
            s_nfuncs++;
        }
    }
    qsort(s_funcs, s_nfuncs, sizeof *s_funcs, cmp_func);
#if defined(__APPLE__)
    {
        unsigned long size = 0;
        const uint8_t *code = (const uint8_t *)getsectiondata(&_mh_execute_header,
                                                              "__TEXT", "__guest", &size);
        if (code && size) {
            s_code_lo = (uintptr_t)code;
            s_code_hi = (uintptr_t)code + size;
        }
    }
#endif
    if (!s_code_hi && s_nfuncs) {
        /* No section to bound the lifted code: from the first lifted
         * function to the last, plus room for the last one's body. */
        s_code_lo = s_funcs[0].host;
        s_code_hi = s_funcs[s_nfuncs - 1].host + 0x10000;
    }
    fprintf(stderr, "  [FRAMETIME] %zu lifted functions mapped, code 0x%llx-0x%llx,"
            " %d spin function(s), sampling every %u us\n", s_nfuncs,
            (unsigned long long)s_code_lo, (unsigned long long)s_code_hi, s_nspin,
            s_period_us);
}

static int bucket_of_pc(uintptr_t pc)
{
    size_t lo = 0, hi = s_nfuncs;
    if (pc < s_code_lo || pc >= s_code_hi || !s_nfuncs || pc < s_funcs[0].host)
        return B_HOST;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (s_funcs[mid].host <= pc)
            lo = mid;
        else
            hi = mid;
    }
    return s_funcs[lo].bucket;
}

static void sample_once(void)
{
    LONG n = s_nthreads, i;
    for (i = 0; i < n && i < FT_MAX_THREADS; i++) {
        int b;
#if defined(__APPLE__)
        mach_port_t t = s_threads[i];
        struct thread_basic_info bi;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        uintptr_t pc = 0;
        if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS)
            continue;
        if (bi.run_state == TH_STATE_UNINTERRUPTIBLE) {
            b = B_IO;
        } else if (bi.run_state != TH_STATE_RUNNING) {
            b = B_WAIT;
        } else if (thread_suspend(t) == KERN_SUCCESS) {
#if defined(__arm64__) || defined(__aarch64__)
            arm_thread_state64_t st;
            mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS)
                pc = (uintptr_t)arm_thread_state64_get_pc(st);
#else
            x86_thread_state64_t st;
            mach_msg_type_number_t sc = x86_THREAD_STATE64_COUNT;
            if (thread_get_state(t, x86_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS)
                pc = (uintptr_t)st.__rip;
#endif
            b = bucket_of_pc(pc);
            if (b == B_HOST && s_hout && i == s_main_slot) {
                LONG k = InterlockedIncrement(&s_hs_n) - 1;
                if (k < HS_MAX) {
                    HostStack *h = &s_hs[k];
                    uintptr_t fp, lo;
                    h->n = 0;
                    h->pc[h->n++] = pc;
#if defined(__arm64__) || defined(__aarch64__)
                    h->pc[h->n++] = (uintptr_t)arm_thread_state64_get_lr(st);
                    fp = (uintptr_t)arm_thread_state64_get_fp(st);
#else
                    fp = (uintptr_t)st.__rbp;
#endif
                    /* The frame records, as xbox_crash.c's host_frames
                     * walks them: the thread is stopped, so they hold. */
                    lo = fp;
                    while (h->n < HS_FRAMES && fp >= lo && fp - lo < (8u << 20)
                           && !(fp & 7) && fp) {
                        uintptr_t next = ((const uintptr_t *)fp)[0];
                        uintptr_t ret = ((const uintptr_t *)fp)[1];
                        if (!ret || next <= fp)
                            break;
                        h->pc[h->n++] = ret;
                        fp = next;
                    }
                }
            }
            thread_resume(t);
        } else {
            continue;
        }
#elif defined(_WIN32)
        HANDLE t = s_threads[i];
        CONTEXT c;
        ULONG64 cyc = 0;
        uintptr_t pc;
        if (SuspendThread(t) == (DWORD)-1)
            continue;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        pc = GetThreadContext(t, &c) ? (uintptr_t)c.Rip : 0;
        ResumeThread(t);
        QueryThreadCycleTime(t, &cyc);
        /* No run state to ask for: a thread whose cycle count barely moved
         * since the last look, at the same PC, was blocked. */
        if (pc == s_lastpc[i] && cyc - s_cycles[i] < 20000)
            b = B_WAIT;
        else
            b = bucket_of_pc(pc);
        s_lastpc[i] = pc;
        s_cycles[i] = cyc;
#elif defined(__linux__)
        char buf[512];
        ssize_t n;
        const char *p;
        if (!s_ready[i])
            continue;
        /* schedstat: "<ns on cpu> <ns runnable on a runqueue> <slices>" */
        n = s_schedfd[i] >= 0 ? pread(s_schedfd[i], buf, sizeof buf - 1, 0) : -1;
        if (n > 0) {
            unsigned long long run = 0, rq = 0;
            buf[n] = 0;
            if (sscanf(buf, "%llu %llu", &run, &rq) == 2) {
                if (s_runq_last[i] && rq > s_runq_last[i]) {
                    int64_t d = (int64_t)(rq - s_runq_last[i]);
                    __atomic_add_fetch(&s_runq_ns, d, __ATOMIC_RELAXED);
                    if (i == s_main_slot)
                        __atomic_add_fetch(&s_mrunq_ns, d, __ATOMIC_RELAXED);
                }
                s_runq_last[i] = rq;
            }
        }
        /* stat: "<tid> (<comm>) <state> ...": comm may hold spaces. */
        n = s_statfd[i] >= 0 ? pread(s_statfd[i], buf, sizeof buf - 1, 0) : -1;
        if (n <= 0)
            continue;
        buf[n] = 0;
        p = strrchr(buf, ')');
        if (!p || !p[1] || !p[2])
            continue;
        if (p[2] == 'R') {
            pthread_kill(s_threads[i], SIGPROF);   /* it buckets its own PC */
            continue;
        }
        b = p[2] == 'D' ? B_IO : B_WAIT;
#else
        (void)i;
        return;
#endif
        count_sample(i, b);
    }
}

#if defined(__linux__)
static void prof_handler(int sig, siginfo_t *si, void *ucv)
{
    const ucontext_t *uc = (const ucontext_t *)ucv;
    uintptr_t pc;
    int e = errno;
    (void)sig; (void)si;
#if defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    pc = 0;
#endif
    if (t_slot >= 0) {
        int b = bucket_of_pc(pc);
        count_sample(t_slot, b);
        if (b == B_HOST && s_hout && t_slot == s_main_slot) {
            LONG k = InterlockedIncrement(&s_hs_n) - 1;
            if (k < HS_MAX) {
                void *f[HS_FRAMES + 4];
                HostStack *h = &s_hs[k];
                int got = backtrace(f, HS_FRAMES + 4), i, skip = 0;
                /* From the interrupted pc: the frames before it are this
                 * handler's and the signal trampoline's. */
                for (i = 0; i < got; i++)
                    if ((uintptr_t)f[i] == pc) { skip = i; break; }
                h->n = 0;
                if (skip == 0)
                    h->pc[h->n++] = pc;
                for (i = skip; i < got && h->n < HS_FRAMES; i++)
                    h->pc[h->n++] = (uintptr_t)f[i];
            }
        }
    }
    errno = e;
}
#endif

#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
static DWORD WINAPI sampler_main(LPVOID p)
#else
static void *sampler_main(void *p)
#endif
{
    uint64_t prev = 0;
    (void)p;
    for (;;) {
        /* What this pass's samples stand for: the time since the last one.
         * A pass that came very late (the process was stopped) is not a
         * tenth of a second of anything. */
        uint64_t now = xbox_HostNowNs();
        uint64_t us = prev ? (now - prev + 500u) / 1000u : s_period_us;
        prev = now;
        s_pass_us = (LONG)(us > 100000u ? 100000u : us);
        sample_once();
        xbox_HostSleepNs((uint64_t)s_period_us * 1000u);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
#endif

static int frametime_on(void)
{
    if (s_state < 0) {
        const char *path = recomp_env(RENV_FRAMETIME);
        const char *spin = recomp_env(RENV_FRAMETIME_SPIN);
        const char *us = recomp_env(RENV_FRAMETIME_US);
        s_state = 0;
        if (!path || !*path || !strcmp(path, "1") || !strcmp(path, "0"))
            return 0;
        s_out = fopen(path, "w");
        if (!s_out) {
            fprintf(stderr, "  [FRAMETIME] cannot write %s\n", path);
            return 0;
        }
        while (spin && *spin && s_nspin < FT_MAX_SPIN) {
            char *end;
            unsigned long v = strtoul(spin, &end, 16);
            if (end == spin)
                break;
            s_spin[s_nspin++] = (uint32_t)v;
            spin = *end ? end + 1 : end;
        }
        if (us && atoi(us) >= 100)
            s_period_us = (unsigned)atoi(us);
        else if (us && !strcmp(us, "0"))
            s_nosample = 1;     /* the executor's columns only */
        fprintf(s_out, "flip,t_ms,frame_ms,iv,late,ex_hold,ex_busy,ex_draw,ex_present,"
                "ex_idle,g_game,g_d3d,g_xdk,g_spin,g_host,g_wait,g_threads,cpu_proc,"
                "g_io,g_runq,m_game,m_d3d,m_xdk,m_spin,m_host,m_wait,m_io,m_runq,ex_cpu,"
                "gpu_ms,tu_ms,tu_pred\n");
        fflush(s_out);
        ring_start();
        build_table();
        {
            const char *hp = recomp_env(RENV_FRAMETIME_HOST);
            const char *hm = recomp_env(RENV_FRAMETIME_HOST_MS);
            double ms = hm ? atof(hm) : 4.0;
            if (hp && *hp && strcmp(hp, "0") && strcmp(hp, "1")) {
                s_hout = fopen(hp, "w");
                if (!s_hout)
                    fprintf(stderr, "  [FRAMETIME] cannot write %s\n", hp);
            }
            if (ms > 0)
                s_host_min = (unsigned)(ms * 1000.0 + 0.5);
            if (!s_host_min)
                s_host_min = 1;
        }
        if (s_nosample) {
            fprintf(stderr, "  [FRAMETIME] frametime_us=0: no guest thread sampling\n");
            s_state = 1;
            return s_state;
        }
#if defined(__APPLE__)
        {
            pthread_t th;
            pthread_create(&th, NULL, sampler_main, NULL);
            pthread_detach(th);
        }
#elif defined(_WIN32)
        CloseHandle(CreateThread(NULL, 0, sampler_main, NULL, 0, NULL));
#elif defined(__linux__)
        {
            struct sigaction sa;
            pthread_t th;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = prof_handler;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPROF, &sa, NULL);
            pthread_create(&th, NULL, sampler_main, NULL);
            pthread_detach(th);
        }
#else
        fprintf(stderr, "  [FRAMETIME] no thread sampler on this host: the guest"
                " columns stay 0\n");
#endif
        s_state = 1;
    }
    return s_state;
}

void xbox_FrametimeThread(const char *role)
{
    LONG i;
    if (!frametime_on() || s_nosample)
        return;
    i = InterlockedIncrement(&s_nthreads) - 1;
    if (i >= FT_MAX_THREADS)
        return;
#if defined(__APPLE__)
    s_threads[i] = mach_thread_self();
#elif defined(_WIN32)
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_threads[i], THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                    | THREAD_QUERY_INFORMATION, FALSE, 0);
#elif defined(__linux__)
    {
        char path[64];
        long tid = (long)syscall(SYS_gettid);
        s_threads[i] = pthread_self();
        snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
        s_statfd[i] = open(path, O_RDONLY | O_CLOEXEC);
        snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
        s_schedfd[i] = open(path, O_RDONLY | O_CLOEXEC);
        t_slot = i;
        __atomic_store_n(&s_ready[i], 1, __ATOMIC_RELEASE);
    }
#endif
    if (role && !strcmp(role, "guest-main") && s_main_slot < 0)
        s_main_slot = i;
    fprintf(stderr, "  [FRAMETIME] sampling guest thread %s\n", role ? role : "?");
}

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

static void host_addr(FILE *f, uintptr_t pc)
{
    int b = bucket_of_pc(pc);
#if !defined(_WIN32)
    Dl_info info;
#endif
    if (b != B_HOST) {
        /* Lifted code: the guest function it was lifted from. */
        size_t lo = 0, hi = s_nfuncs;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (s_funcs[mid].host <= pc) lo = mid; else hi = mid;
        }
        fprintf(f, " guest:0x%08X", s_funcs[lo].va);
        return;
    }
#if !defined(_WIN32)
    memset(&info, 0, sizeof info);
    if (dladdr((void *)pc, &info) && info.dli_sname && info.dli_saddr) {
        fprintf(f, " %s+0x%lx", info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
        return;
    }
    if (info.dli_fname && info.dli_fbase) {
        const char *base = strrchr(info.dli_fname, '/');
        fprintf(f, " %s+0x%lx", base ? base + 1 : info.dli_fname,
                (unsigned long)(pc - (uintptr_t)info.dli_fbase));
        return;
    }
#endif
    fprintf(f, " 0x%llx", (unsigned long long)pc);
}

/* frametime_host: this frame's stacks, identical ones counted together. */
static void host_dump(uint32_t flip, double frame_ms, LONG mhost)
{
    LONG n = InterlockedExchange(&s_hs_n, 0), i, j;
    static int seen[HS_MAX];
    if (!s_hout)
        return;
    if (n > HS_MAX)
        n = HS_MAX;
    if (mhost < (LONG)s_host_min || n <= 0)
        return;
    fprintf(s_hout, "flip %u frame_ms %.2f m_host %.2f stacks %ld\n", flip, frame_ms,
            mhost / 1000.0, (long)n);
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        int count = 1, k;
        if (seen[i])
            continue;
        for (j = i + 1; j < n; j++)
            if (!seen[j] && s_hs[j].n == s_hs[i].n
                && !memcmp(s_hs[j].pc, s_hs[i].pc, sizeof(uintptr_t) * (size_t)s_hs[i].n)) {
                seen[j] = 1;
                count++;
            }
        fprintf(s_hout, "  %3d", count);
        for (k = 0; k < s_hs[i].n; k++) {
            if (k)
                fprintf(s_hout, " <-");
            host_addr(s_hout, s_hs[i].pc[k]);
        }
        fprintf(s_hout, "\n");
    }
    fflush(s_hout);
}

/* The calling thread's CPU time, ns. Windows: its cycle count over the
 * TSC's rate, measured against the host clock since the first call (the
 * rate settles within the first second; GetThreadTimes is in clock ticks). */
static uint64_t self_cpu_ns(uint64_t now)
{
#if defined(_WIN32) && defined(FT_HAVE_TSC)
    static uint64_t tsc0, ns0;
    ULONG64 cyc = 0;
    uint64_t tsc = __rdtsc();
    if (!ns0) {
        tsc0 = tsc;
        ns0 = now;
    }
    if (!QueryThreadCycleTime(GetCurrentThread(), &cyc) || now - ns0 < 1000000u)
        return 0;
    return (uint64_t)((double)cyc * (double)(now - ns0) / (double)(tsc - tsc0));
#else
    (void)now;
    return xbox_ThreadCpuNs();
#endif
}

/* At FLIP_STALL, from the executor: its running totals in ns. */
/* The backend's columns (xbox_FrametimeGpu): the latest values, written
 * with the next line. Doubles as bits, so any thread may set them. */
static volatile uint64_t s_gpu_bits, s_tu_bits, s_tup_bits;

static uint64_t d2b(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static double b2d(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

void xbox_FrametimeGpu(double gpu_ms, double tu_ms, double tu_pred)
{
    s_gpu_bits = d2b(gpu_ms);
    s_tu_bits = d2b(tu_ms);
    s_tup_bits = d2b(tu_pred);
}

int xbox_FrametimeOn(void)
{
    return s_state > 0 || (s_state < 0 && frametime_on());
}

void xbox_FrametimeFlip(uint32_t flip, uint32_t iv, int late, uint64_t hold,
                        uint64_t scan, uint64_t draw, uint64_t present)
{
    static uint64_t t0, last, p_hold, p_scan, p_draw, p_pres, p_cpu, p_self;
    uint64_t now, cpu, self, frame, d_hold, d_scan;
    LONG c[B_COUNT], m[B_COUNT];
    int64_t rq, mrq;
    const double per = 1.0 / 1000.0;   /* the buckets hold microseconds */
    char row[768];
    int b, n;

    if (s_state == 0 || (s_state < 0 && !frametime_on()))
        return;
    now = xbox_HostNowNs();
    cpu = xbox_ProcessCpuNs();
    self = self_cpu_ns(now);
    for (b = 0; b < B_COUNT; b++) {
        c[b] = InterlockedExchange(&s_count[b], 0);
        m[b] = InterlockedExchange(&s_mcount[b], 0);
    }
#if defined(__linux__)
    rq = __atomic_exchange_n(&s_runq_ns, 0, __ATOMIC_RELAXED);
    mrq = __atomic_exchange_n(&s_mrunq_ns, 0, __ATOMIC_RELAXED);
#else
    rq = mrq = 0;
#endif
    if (s_hout)
        host_dump(flip, last ? (now - last) / 1e6 : 0, m[B_HOST]);
    if (!last) {
        t0 = last = now;
        p_hold = hold; p_scan = scan; p_draw = draw; p_pres = present; p_cpu = cpu;
        p_self = self;
        return;
    }
    frame = now - last;
    d_hold = hold - p_hold;
    d_scan = scan - p_scan;
    n = snprintf(row, sizeof row, "%u,%.3f,%.3f,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%ld,%.3f,"
            "%.2f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            flip, (now - t0) / 1e6, frame / 1e6, iv, late,
            d_hold / 1e6, (double)(int64_t)(d_scan - d_hold) / 1e6,
            (draw - p_draw) / 1e6, (present - p_pres) / 1e6,
            (double)(int64_t)(frame - d_scan) / 1e6,
            c[B_GAME] * per, c[B_D3D] * per, c[B_XDK] * per, c[B_SPIN] * per,
            c[B_HOST] * per, c[B_WAIT] * per, (long)s_nthreads,
            (cpu - p_cpu) / 1e6,
            c[B_IO] * per, rq / 1e6,
            m[B_GAME] * per, m[B_D3D] * per, m[B_XDK] * per, m[B_SPIN] * per,
            m[B_HOST] * per, m[B_WAIT] * per, m[B_IO] * per, mrq / 1e6,
            self > p_self && p_self ? (self - p_self) / 1e6 : 0.0,
            b2d(s_gpu_bits), b2d(s_tu_bits), b2d(s_tup_bits));
    if (n > 0 && (size_t)n < sizeof row && !ring_put(row, (size_t)n, (flip & 63) == 0)) {
        fputs(row, s_out);
        if ((flip & 63) == 0)
            fflush(s_out);
    }
    last = now;
    p_hold = hold; p_scan = scan; p_draw = draw; p_pres = present; p_cpu = cpu;
    p_self = self;
}

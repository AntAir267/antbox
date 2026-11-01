/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 *
 * The walk goes from GET to PUT the way the DMA engine does: a jump moves
 * GET to its target (new style w & ~3, old style w & 0x1FFFFFFC), a call
 * does the same and keeps one return address, and a return goes back to it.
 * So a ring wrap is the title's own jump back to the start, and a segment
 * with a jump in it goes on after it to PUT.
 *
 * Anything else is a word the walk is out of step on, and it stops there
 * rather than guess: an unknown word, a call inside a call, or a word or
 * target outside the contiguous window (a parameter misread as a jump can
 * point anywhere in 256 MB, and only 64 MB is mapped).
 */
#include <stdio.h>
#include "recomp_env.h"
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "nv2a_pb_state.h"
#include "platform/host_time.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Walks that stopped short of PUT. Counted because after eight warning lines
 * a stop is otherwise silent. */
static uint32_t s_tot_stops, s_tot_salvaged, s_catchups;
uint32_t nv2a_pb_scan_stops(void) { return s_tot_stops; }

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern uint32_t nv2a_pb_exec_semaphore_last_value(void);
extern uint32_t nv2a_pb_exec_semaphore_releases(void);
extern void nv2a_pb_perf_scan_begin(void);
extern void nv2a_pb_perf_scan_end(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;   /* RECOMP_PB_SCAN, read once */

/* s_seen's slot for each (subchannel, method), plus one (0: not seen yet).
 * The walker calls note() for every method in the stream, ~100,000 a frame in
 * a busy scene, and a linear search of a few hundred pairs there was
 * half the walker's time. s_seen keeps first-seen order for the report. */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note(uint32_t subch, uint32_t method)
{
    static uint16_t none;
    uint16_t *slot = &none;

    /* An incrementing run can carry the method past 0x1FFC; those (rare)
     * keep the linear search. */
    if (method < 0x2000 && !(method & 3)) {
        slot = &s_seen_slot[subch & 7][method >> 2];
        if (*slot) {
            s_seen[*slot - 1].count++;
            return;
        }
    } else {
        for (int i = 0; i < s_seen_count; i++) {
            if (s_seen[i].method == method && s_seen[i].subch == subch) {
                s_seen[i].count++;
                return;
            }
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        if (slot != &none)
            *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0110, "WAIT_FOR_IDLE" },
    { 0x0120, "SET_FLIP_READ" },
    { 0x0124, "SET_FLIP_WRITE" },
    { 0x0128, "SET_FLIP_MODULO" },
    { 0x012C, "FLIP_INCREMENT_WRITE" },
    { 0x0130, "FLIP_STALL" },
    { 0x01A4, "SET_CONTEXT_DMA_SEMAPHORE" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_ZSTENCIL_CLEAR_VALUE" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_SEMAPHORE_OFFSET" },
    { 0x1D70, "BACK_END_WRITE_SEMAPHORE_RELEASE" },
    { 0x1D98, "SET_CLEAR_RECT_HORIZONTAL" },
    { 0x1D9C, "SET_CLEAR_RECT_VERTICAL" },
    { 0x0B00, "SET_TRANSFORM_PROGRAM" },
    { 0x0B80, "SET_TRANSFORM_CONSTANT" },
    { 0x1EA4, "SET_TRANSFORM_CONSTANT_LOAD" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B10, "SET_TEXTURE_CONTROL1" },
    { 0x1B14, "SET_TEXTURE_FILTER" },
    { 0x1B1C, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !recomp_env(RENV_PB_SCAN))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised,"
                    " %u stopped (%u with a fence release salvaged, %u catch-ups)"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_tot_stops, s_tot_salvaged, s_catchups, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Once per pass of the ack thread's loop: the backend's on_poll, so a
 * deferred visibility-test report can complete while no pushbuffer comes
 * (a title spinning on GetVisibilityTestResult submits nothing). */
void nv2a_pb_poll(void)
{
    const struct nv2a_pb_backend *b;
    if (s_exec_enabled <= 0)
        return;
    b = nv2a_pb_get_backend();
    if (b && b->on_poll)
        b->on_poll();
}

static int pb_in_window(uint32_t va)
{
    return (va & 3u) == 0 && va - XBOX_CONTIG_BASE <= XBOX_CONTIG_SIZE - 4u;
}

/* The words from a stop to PUT are never executed, and they may hold the
 * semaphore release the title's next fence wait spins on. stop_salvage
 * runs that release itself. When it cannot search the span, or finds
 * releases there but none it trusts, it asks for a catch-up instead: the
 * fence mirror then writes the title's submitted count once
 * (fence_mirrors_tick). */
uint32_t nv2a_pb_scan_catchups(void) { return s_catchups; }

/* The XDK writes a fence as the pair (BACK_END_WRITE_SEMAPHORE_RELEASE,
 * count 1, subchannel 0) and its value. Run the last such release found in
 * [from, to): releasing that one marks all the earlier ones done too, and
 * none beyond PUT. Mirroring the submitted count would also pass fences
 * the title has not yet handed over, and a resource it is still using
 * could then be freed.
 *
 * The pair can also be data: ARRAY_ELEMENT16 indices 7536, 4 read as
 * 0x00041D70. A value taken from data could jump the fence far ahead,
 * which would turn fences off for the rest of the run. So a candidate
 * counts only if it looks like the next fence: odd (the XDK's fence values
 * are), after the last release, and at most 128 past it. Returns how the
 * fence was handled, for the log. */
static const char *stop_salvage(const uint8_t *mem, uint32_t from, uint32_t to,
                                uint32_t *value)
{
    uint32_t va, last, rel;
    int found = 0, seen = 0;

    if (!s_exec_enabled)
        return "not executing";
    if (from >= to || !pb_in_window(from) || !pb_in_window(to - 4)) {
        s_catchups++;
        return "span wraps or is unreadable; fence catch-up requested";
    }
    last = nv2a_pb_exec_semaphore_last_value();
    for (va = from; va + 4 < to; va += 4) {
        if (*(const uint32_t *)(mem + va) == 0x00041D70u) {
            uint32_t v = *(const uint32_t *)(mem + va + 4);
            seen++;
            if ((v & 1u) && (int32_t)(v - last) > 0 && v - last <= 128u) {
                last = v;
                found = 1;
            }
        }
    }
    if (!found && seen) {
        /* Data, or a real release the check turned down (an even value,
         * or one far past a stale last value): not running it could leave
         * the title waiting for ever, so fall back to the mirror. */
        s_catchups++;
        return "release(s) in the rest but none plausible; catch-up requested";
    }
    if (!found)
        return "no fence release in the rest";
    rel = nv2a_pb_exec_semaphore_releases();
    nv2a_pb_exec_method(0, 0x1D70u, last);
    *value = last;
    if (nv2a_pb_exec_semaphore_releases() == rel)
        return "fence release found but refused by the executor";
    s_tot_salvaged++;
    return "fence release salvaged";
}

/* A method header's parameters can go past PUT: the title may submit
 * the header and some of its data now and the rest with the next PUT. So
 * the method in progress outlives one walk. */
static uint32_t s_left, s_subch, s_method;
static int s_noninc;

/* RECOMP_TRACE=pb_slow[=ms]: the method the executor stood in. A frame that
 * is long with little of it in draws (RECOMP_TRACE=frametime: ex_busy far
 * above ex_draw) spent the time in one command or another: a report that
 * waits for the GPU, the early submit at FLIP_STALL, a semaphore release.
 * This names it. */
static uint64_t s_slow_ns = UINT64_MAX;      /* unread; 0 off */

/* The executor's own parts of FLIP_STALL (the method holds for the vblank,
 * so it is not timed whole): 0 when the trace is off, else the time now. */
uint64_t nv2a_pb_slow_begin(void)
{
    return s_slow_ns && s_slow_ns != UINT64_MAX ? xbox_HostNowNs() : 0;
}

void nv2a_pb_slow_end(const char *what, uint64_t t0)
{
    uint64_t d;
    if (!t0)
        return;
    d = xbox_HostNowNs() - t0;
    if (d >= s_slow_ns)
        fprintf(stderr, "[PB] slow: %s: %.2f ms at %llu us\n", what, d / 1e6,
                (unsigned long long)(t0 / 1000u));
}

/* One timing a method header, over all its parameters, not one a word: a
 * frame's inline vertex data is some 100 000 parameter words, and two clock
 * reads for each took 4 ms a frame (decode 1.6 -> 5.7 ms: the trace made
 * frames late). The run is closed when its last parameter is done; a run
 * still open when a walk ends is dropped (the title had not sent the rest). */
static uint64_t s_run_t0;
static uint32_t s_run_method, s_run_first;
static int s_run_have;

static void slow_run_end(uint32_t subch)
{
    uint64_t d;
    if (!s_run_t0)
        return;
    d = xbox_HostNowNs() - s_run_t0;
    if (d >= s_slow_ns && s_run_method != 0x0130u)   /* NV097_FLIP_STALL: see above */
        fprintf(stderr, "[PB] slow method 0x%04X (subchannel %u, first param 0x%08X): %.2f ms"
                " at %llu us\n", s_run_method, subch, s_run_first, d / 1e6,
                (unsigned long long)(s_run_t0 / 1000u));
    s_run_t0 = 0;
}


/* Stream marks (nv2a_pb_state.h): a ring the guest's thread writes and the
 * walker reads. Addresses compare within the 64 MB window, so the cached and
 * the write-combined views of the same word are one place. */
#define SM_N 256u
#define SM_MASK 0x03FFFFFCu
static struct { uint32_t va, tag; } s_sm[SM_N];
static volatile uint32_t s_sm_w, s_sm_r;
static volatile uint32_t s_sm_tag;
static uint64_t s_sm_hit, s_sm_lost;

void nv2a_pb_stream_mark(uint32_t va, uint32_t tag)
{
    uint32_t w = __atomic_load_n(&s_sm_w, __ATOMIC_RELAXED);
    if (w - __atomic_load_n(&s_sm_r, __ATOMIC_ACQUIRE) >= SM_N) {
        s_sm_lost++;
        return;
    }
    s_sm[w % SM_N].va = va & SM_MASK;
    s_sm[w % SM_N].tag = tag;
    __atomic_store_n(&s_sm_w, w + 1, __ATOMIC_RELEASE);
}
uint32_t nv2a_pb_stream_tag(void) { return s_sm_tag; }
void nv2a_pb_stream_tag_set(uint32_t tag) { s_sm_tag = tag; }

/* The marks queued at `va`, applied; called with every word's address. */
static inline void sm_reach(uint32_t va)
{
    uint32_t r = s_sm_r;
    while (r != __atomic_load_n(&s_sm_w, __ATOMIC_ACQUIRE)
           && s_sm[r % SM_N].va == (va & SM_MASK)) {
        s_sm_tag = s_sm[r % SM_N].tag;
        r++;
        s_sm_hit++;
    }
    __atomic_store_n(&s_sm_r, r, __ATOMIC_RELEASE);
}

/* A walk that stopped: what is queued can no longer be placed. */
static void sm_drop(void)
{
    uint32_t w = __atomic_load_n(&s_sm_w, __ATOMIC_ACQUIRE);
    if (w != s_sm_r) {
        s_sm_lost += w - s_sm_r;
        __atomic_store_n(&s_sm_r, w, __ATOMIC_RELEASE);
    }
}

/* The last jumps, calls and returns a walk took (from, to), for its stop
 * report: a walk that loops or lands in data says where it turned. */
#define PB_HOPS 12
static uint32_t s_hop_from[PB_HOPS], s_hop_to[PB_HOPS], s_hop_w[PB_HOPS], s_hop_n;
static void hop(uint32_t from, uint32_t to)
{
    s_hop_w[s_hop_n % PB_HOPS] = *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + from);
    s_hop_from[s_hop_n % PB_HOPS] = from;
    s_hop_to[s_hop_n % PB_HOPS] = to;
    s_hop_n++;
}

/* NV_PFIFO_CACHE1_DMA_SUBROUTINE: where a pushbuffer subroutine returns to
 * (bits 31:2) and whether one is running (bit 0). The engine's own state,
 * so it lasts from one walk to the next, and software reads and writes it:
 * XDK 4134 D3D runs a pushbuffer resource with a CALL whose block leaves by
 * a JUMP back (its exit patched each time), so the subroutine never
 * RETURNs; its software method 3 (the run's fixups, sub_001015B0 ->
 * sub_00101530 in Wreckless) writes this register with bit 0 clear before
 * the next CALL. A second CALL with it set is a parse error the console
 * never reaches (D3D's interrupt breaks into the debugger on one). So it
 * is published at every call and return, and read back after a software
 * method in case the title changed it. */
#define PB_DMA_SUBROUTINE 0xFD00324Cu
static int s_in_call;
static uint32_t s_ret_va;

static void subr_publish(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    *(volatile uint32_t *)(mem + PB_DMA_SUBROUTINE) =
        (s_ret_va & 0x0FFFFFFCu) | (s_in_call ? 1u : 0u);
}

static void subr_adopt(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t v = *(const volatile uint32_t *)(mem + PB_DMA_SUBROUTINE);
    s_in_call = (int)(v & 1u);
    s_ret_va = XBOX_CONTIG_BASE | (v & 0x0FFFFFFCu);
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va, at = start_va, w = 0;
    uint32_t words = 0, jumps = 0, unknown = 0;
    static int inject = -1;
    const char *why = NULL;

    if (s_exec_enabled < 0)
        s_exec_enabled = recomp_env(RENV_PB_EXEC) != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = recomp_env(RENV_PB_SCAN) != NULL;
    if (!(s_scan_enabled || s_exec_enabled) || end_va == start_va)
        return;
    if (s_exec_enabled)
        nv2a_pb_perf_scan_begin();
    if (s_slow_ns == UINT64_MAX) {
        const char *e = recomp_env(RENV_PB_SLOW);
        double ms = e && *e ? atof(e) : 0.0;
        s_slow_ns = !e ? 0 : (uint64_t)((ms > 0.0 ? ms : 5.0) * 1e6);
    }
    /* Test hook: RECOMP_PB_INJECT_STOP=N stops every Nth walk at its first
     * word, so the recovery from a stop can be seen without a real desync.
     * Never the first walk: it sets up the semaphore, and without that the
     * executor never takes the fence over and there is nothing to recover. */
    if (inject < 0)
        inject = recomp_env(RENV_PB_INJECT_STOP)
               ? atoi(recomp_env(RENV_PB_INJECT_STOP)) : 0;
    if (inject > 0 && s_tot_segments && s_tot_segments % (uint32_t)inject == 0)
        why = "injected (RECOMP_DEBUG=pb_inject_stop)";

    /* Bounded at 4 MB of words, a sane single-frame amount: GET that never
     * reaches PUT is a stream this walk is out of step with. */
    while (!why && va != end_va) {
        uint32_t tgt;

        at = va;
        w = 0;
        if (s_sm_r != s_sm_w)
            sm_reach(va);
        if (words >= 0x100000u) { why = "word limit"; break; }
        if (!pb_in_window(va)) { why = "outside the contiguous window"; break; }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if (s_left) {                         /* a parameter */
            /* The inventory is only reported with RECOMP_PB_SCAN: counting
             * every word without it cost ~0.34 ms a frame of the render
             * thread (BO-277, WPR on Windows). */
            if (s_scan_enabled)
                note(s_subch, s_method);
            /* Same walk, two consumers: the survey counts, the executor
             * acts. Keeping them on one decode means they can never
             * disagree about what the stream said. */
            if (s_run_t0 && !s_run_have) {
                s_run_first = w;
                s_run_have = 1;
            }
            if (s_exec_enabled) {
                if (s_method == 0x0100u && w) {
                    /* A software method: the title's interrupt code runs
                     * and may move the subroutine state. */
                    subr_publish();
                    nv2a_pb_exec_method(s_subch, s_method, w);
                    subr_adopt();
                } else {
                    nv2a_pb_exec_method(s_subch, s_method, w);
                }
            }
            if (!s_noninc)
                s_method += 4;
            if (--s_left == 0 && s_run_t0)
                slow_run_end(s_subch);
            continue;
        }
        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t to;
            jumps++;
            tgt = (w & 3u) == 1u ? w & ~3u : w & 0x1FFFFFFCu;
            if (tgt >= XBOX_CONTIG_SIZE) { why = "jump outside the window"; break; }
            to = XBOX_CONTIG_BASE | tgt;
            hop(at, to);
            va = to;
            continue;
        }
        if ((w & 3u) == 2u) {                 /* call */
            jumps++;
            tgt = w & ~3u;
            if (s_in_call) { why = "call inside a call"; break; }
            if (tgt >= XBOX_CONTIG_SIZE) { why = "call outside the window"; break; }
            s_ret_va = va;
            s_in_call = 1;
            subr_publish();
            hop(at, XBOX_CONTIG_BASE | tgt);
            va = XBOX_CONTIG_BASE | tgt;
            continue;
        }
        if (w == 0x00020000u) {               /* return */
            if (!s_in_call) { why = "return outside a call"; break; }
            hop(at, s_ret_va);
            va = s_ret_va;
            s_in_call = 0;
            subr_publish();
            continue;
        }
        if ((w & 0xE0030003u) == 0u || (w & 0xE0030003u) == 0x40000000u) {
            s_left   = (w >> 18) & 0x7FFu;
            s_subch  = (w >> 13) & 7u;
            s_method =  w & 0x1FFCu;
            s_noninc = (w & 0xE0000000u) == 0x40000000u;
            if (s_slow_ns && s_exec_enabled && s_left) {
                s_run_t0 = xbox_HostNowNs();
                s_run_method = s_method;
                s_run_have = 0;
            }
            continue;
        }
        unknown++;
        why = "unknown word";
        break;
    }
    if (why) {
        static unsigned said;
        s_left = 0;                 /* out of step: drop the method in progress */
        s_in_call = 0;              /* and whatever call it thought it was in */
        subr_publish();
        sm_drop();
        uint32_t v = 0;
        const char *fence = stop_salvage(mem, at, end_va, &v);
        s_tot_stops++;
        if (said++ < 8) {
            uint32_t k, n = s_hop_n < PB_HOPS ? s_hop_n : PB_HOPS;
            fprintf(stderr, "[PB] walk 0x%08X -> 0x%08X stopped at 0x%08X"
                    " (word 0x%08X) after %u words without reaching PUT (%s);"
                    " %s 0x%08X\n",
                    start_va, end_va, at, w, words, why, fence, v);
            fprintf(stderr, "[PB]   last hops (from -> to, oldest first):");
            for (k = 0; k < n; k++) {
                uint32_t j = (s_hop_n - n + k) % PB_HOPS;
                fprintf(stderr, " %08X[%08X]->%08X", s_hop_from[j], s_hop_w[j], s_hop_to[j]);
            }
            fprintf(stderr, "\n");
        }
        {
            /* DS2_PB_STOP_DUMP=<file>: guest RAM (64 MB, physical) at the
             * first stop, to decode the span offline. Bring-up only. */
            static int dumped;
            const char *f = getenv("DS2_PB_STOP_DUMP");
            if (f && !dumped++) {
                FILE *o = fopen(f, "wb");
                if (o) {
                    fwrite(mem + XBOX_CONTIG_BASE, 1, XBOX_CONTIG_SIZE, o);
                    fclose(o);
                    fprintf(stderr, "[PB]   RAM written to %s\n", f);
                }
            }
        }
    }

    s_run_t0 = 0;                   /* a run the walk left open is not timed */
    if (s_exec_enabled)
        nv2a_pb_perf_scan_end();
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

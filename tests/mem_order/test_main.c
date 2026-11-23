/*
 * Message passing through guest memory, two host threads (mem_order.py).
 *
 * The writer stores data then a flag, the way a lifted `mov [data], i; mov
 * [flag], i` does; the reader reads the flag then the data, the way a lifted
 * spin loop and the code after it do. On x86 the data is never older than
 * the flag. On arm64 it can be: a plain `str` may become visible before the
 * store that preceded it, and the data load may be satisfied before the
 * flag load. Two runs:
 *   - plain: MEM32 both sides, as the lifter emits every ordinary access.
 *     The count of stale reads is reported; on arm64 it is usually > 0 (on
 *     Apple M-series it was 1 in 20M rounds with a handshake and far more
 *     without one), on x86-64 it is 0 by the architecture. Not asserted:
 *     the hardware is not obliged to show it in a bounded run.
 *   - ordered: RECOMP_STORE_REL32 for the flag and RECOMP_ACQUIRE_FENCE()
 *     after it was seen. Asserted 0: that is what --mem-order emits at a
 *     listed lock release and after a spin-wait candidate.
 */
#include "recomp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ptrdiff_t g_xbox_mem_offset;
static uint8_t guest[1 << 16];
#define DATA 0x20040u        /* two different cache lines */
#define FLAG 0x21000u

static volatile int stop;
static int ordered;

static void *writer(void *arg)
{
    (void)arg;
    for (uint32_t i = 1; !stop; i++) {
        MEM32(DATA) = i;
        if (ordered)
            RECOMP_STORE_REL32(FLAG, i);
        else
            MEM32(FLAG) = i;
    }
    return NULL;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long run(int use_order, double seconds, long *rounds)
{
    ordered = use_order;
    stop = 0;
    memset(guest, 0, sizeof guest);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest - (ptrdiff_t)0x20000u;
    pthread_t t;
    if (pthread_create(&t, NULL, writer, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        exit(2);
    }
    long stale = 0, n = 0;
    double end = now() + seconds;
    uint32_t last = 0;
    while (now() < end) {
        for (int k = 0; k < 100000; k++) {
            uint32_t f = MEM32(FLAG);
            if (f == last)
                continue;
            if (use_order)
                RECOMP_ACQUIRE_FENCE();
            uint32_t d = MEM32(DATA);
            if (d < f)
                stale++;
            last = f;
            n++;
        }
    }
    stop = 1;
    pthread_join(t, NULL);
    *rounds = n;
    return stale;
}

int main(void)
{
    long rounds;
    long plain = run(0, 2.0, &rounds);
    printf("  plain volatile stores and loads: %ld stale reads in %ld rounds"
           " (%s)\n", plain, rounds,
           plain ? "the host reorders them: this is the arm64 gap"
                 : "not observed in this run");
    long fixed = run(1, 2.0, &rounds);
    printf("  release store + acquire fence:   %ld stale reads in %ld rounds"
           " %s\n", fixed, rounds, fixed ? "FAIL" : "PASS");
    return fixed ? 1 : 0;
}

/*
 * present_cadence_ring.c - the process's ring of presented frames and the
 * presenter's status, for the overlay's graph (present_cadence.h). Written
 * by a presenter (its on-glass callback, any thread), read by the overlay.
 */
#include "kernel.h"
#include "present_cadence.h"

#include <string.h>

static CRITICAL_SECTION s_lock;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static pc_sample s_ring[PC_RING];
static uint32_t s_n;
static pc_status s_status;
/* The title's steps by flip (the port's hook writes, a presenter reads):
 * the flip's low bits and the count in one word, so a stale slot is seen. */
#define STEP_RING 256
static volatile LONG s_steps[STEP_RING];

static BOOL CALLBACK ring_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    return TRUE;
}

static void ring_lock(void)
{
    InitOnceExecuteOnce(&s_once, ring_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
}

void xbox_PresentCadenceNote(const pc_sample *s)
{
    ring_lock();
    s_ring[s_n++ % PC_RING] = *s;
    LeaveCriticalSection(&s_lock);
}

void xbox_PresentCadenceStatus(const pc_status *st)
{
    ring_lock();
    s_status = *st;
    LeaveCriticalSection(&s_lock);
}

int xbox_PresentCadenceSnapshot(pc_sample *out, int max, pc_status *st)
{
    uint32_t have, from, i;

    ring_lock();
    have = s_n < PC_RING ? s_n : PC_RING;
    if (max < 0)
        max = 0;
    if (have > (uint32_t)max)
        have = (uint32_t)max;
    from = s_n - have;
    for (i = 0; i < have; i++)
        out[i] = s_ring[(from + i) % PC_RING];
    if (st)
        *st = s_status;
    LeaveCriticalSection(&s_lock);
    return (int)have;
}

void xbox_PresentCadenceSteps(uint32_t flip, int steps)
{
    if (steps < 0)
        steps = 0;
    if (steps > 255)
        steps = 255;
    s_steps[flip % STEP_RING] = (LONG)(((flip & 0xFFFFFFu) << 8) | (uint32_t)steps);
}

int xbox_PresentCadenceStepsOf(uint32_t flip)
{
    uint32_t v = (uint32_t)s_steps[flip % STEP_RING];
    return (v >> 8) == (flip & 0xFFFFFFu) ? (int)(v & 0xFFu) : 0;
}

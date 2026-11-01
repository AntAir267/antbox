/*
 * flip_grid.h - the next edge of the host flip grid (nv2a_pb_exec.c's
 * flip_pacing_swap; tests/flip_grid). Arithmetic on host nanoseconds only.
 *
 * A title that keeps its simulation's clock between flips asks for its
 * flips at the display's rate (xbox_FlipHostRate). Each swap is held to the
 * grid's next edge: one period after the last. The period and the phase are
 * the display's as its refreshes are reported (xbox_DisplayGrid), when that
 * is the rate asked for (within 5 %): a rate from the display mode alone
 * (SDL's whole Hz, 175 for a 174.96 Hz panel) walks off the panel's
 * refreshes, a frame then lands in the same refresh as the one before (the
 * swap chain queues it, a refresh more of delay, or it replaces it) every
 * 25 s. So each edge is moved to the refresh nearest it. A swap later than
 * a whole period, or an edge further off than two periods (the grid was
 * idle), starts it again from now: from the last refresh at or before now
 * when the refreshes are known, so the edges stay on them (BO-241: an edge
 * between two refreshes, and the ones after it, said a time the frames
 * were not shown at).
 */
#ifndef XBOX_FLIP_GRID_H
#define XBOX_FLIP_GRID_H

#include <stdint.h>

/* prev_due: the last edge (0 none). asked: the period asked for. g, ref, pd:
 * xbox_DisplayGrid's answer (g 2: ref is a refresh, 1: pd alone, 0: none).
 * Returns the edge; *per_out the period in force, *late 1 when it started
 * again from now. */
static inline uint64_t fg_next_edge(uint64_t prev_due, uint64_t asked, int g,
                                    uint64_t ref, uint64_t pd, uint64_t now,
                                    uint64_t *per_out, int *late)
{
    uint64_t per = asked, due;

    if (g && pd && pd * 100u >= asked * 95u && pd * 100u <= asked * 105u)
        per = pd;
    else
        g = 0;
    due = prev_due + per;
    if (g == 2 && ref) {
        if (due >= ref)
            due = ref + (due - ref + pd / 2) / pd * pd;
        else
            due = ref - (ref - due + pd / 2) / pd * pd;
    }
    *late = 0;
    if (!prev_due || due + per < now || due > now + 2 * per) {
        *late = prev_due != 0;
        due = now;
        /* With the refreshes known, from the last one at or before now: the
         * frame goes at once as before, and the edges stay on them. */
        if (g == 2 && ref) {
            if (now >= ref)
                due = ref + (now - ref) / pd * pd;
            else
                due = ref - (ref - now + pd - 1) / pd * pd;
        }
    }
    *per_out = per;
    return due;
}

#endif

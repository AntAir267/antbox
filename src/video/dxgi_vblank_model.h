/*
 * dxgi_vblank_model.h - Windows: what the DXGI vblank thread
 * (fb_present_vulkan.c) makes of IDXGIOutput::WaitForVBlank (BO-191).
 *
 * RECOMP_VBLANK_SYNC=display moves the emulated 60 Hz vblank onto the
 * display's refresh grid (kernel_pacing.c). The grid comes from a thread
 * that waits for each vertical blank of the window's output. Two things
 * about those returns:
 *
 *  - On a fixed-rate display the thread returns at every refresh, whether
 *    the title presents or not: 4.167 ms steps at 240 Hz. Under variable
 *    refresh (G-Sync, FreeSync) the display refreshes when a frame arrives,
 *    so the thread returns at our own presents (16.7 ms at 60 fps), or at
 *    the panel's self-refresh floor when none comes. Locked to those, the
 *    vblank chases its own presents: each report is a present plus its
 *    latency, the lock re-anchors on it, and the vblank intervals swing
 *    8.6-22.7 ms (GitHub issue #4). So: the median step over the last half
 *    second longer than 1.5 stated periods is variable refresh; held for a
 *    second it changes the state. Under variable refresh nothing is
 *    reported: the lock stands down (no report for 250 ms) and the vblank
 *    runs on its own 60.000 Hz timer, which the display then follows.
 *
 *  - A return is the thread's wake-up after the refresh, up to a
 *    millisecond late on a busy PC. Snapped to each raw return, the grid
 *    carried that jitter into every vblank edge (p99 17.5 ms at a fixed
 *    240 Hz). With the mode's stated period (QueryDisplayConfig, an exact
 *    rational: 60000/1001 for 59.94) the grid is a filtered phase instead:
 *    each return moves it 1/32 of the way, by at most an eighth of a
 *    period, and the reports are points on that grid, carrying the stated
 *    period. Their steps are whole periods, so the kernel's irregular-step
 *    guard never stands the lock down on wake-up jitter.
 *
 * Pure arithmetic on host nanoseconds, no platform call: the thread feeds
 * it, tests/vblank_schedule drives it. Only the Windows build uses it.
 */
#ifndef DXGI_VBLANK_MODEL_H
#define DXGI_VBLANK_MODEL_H

#include <stdint.h>
#include <string.h>

#define DVM_STEPS     256                 /* half a second at up to 500 Hz */
#define DVM_WINDOW_NS 500000000ull        /* the steps the median is over */
#define DVM_HOLD_NS   1000000000ull       /* how long a new state must hold */
#define DVM_MIN_STEPS 8
#define DVM_BREAK_NS  100000000ull        /* a longer step is a break, not a refresh */
#define DVM_GAIN      32                  /* the phase filter: 1/32 of each error */

enum { DVM_UNKNOWN = -1, DVM_FIXED = 0, DVM_VRR = 1 };

/* dvm_refresh's result bits. */
#define DVM_REPORT  1                     /* report *when to the vblank lock */
#define DVM_CHANGED 2                     /* the state changed (log it) */

typedef struct {
    uint64_t period_ns;   /* the mode's refresh period; 0 unknown */
    int      exact;       /* period_ns is the mode's rational, not a whole-Hz rate */
    int      state;       /* DVM_UNKNOWN until half a second of steps */
    uint64_t first_ns;    /* the first return since the reset */
    uint64_t last_ns;     /* the last return */
    uint64_t grid_ns;     /* the filtered refresh; 0 none */
    int      off;         /* returns in a row far off the grid */
    uint64_t step_at[DVM_STEPS];
    uint32_t step_ns[DVM_STEPS];
    unsigned pos, n;
    int      pending;     /* the other state the steps show */
    uint64_t pending_ns;  /* since when */
    uint32_t median_ns;   /* the last window's median step */
} dvm_model;

/* A new output or mode: forget everything. */
static inline void dvm_reset(dvm_model *m, uint64_t period_ns, int exact)
{
    memset(m, 0, sizeof *m);
    m->period_ns = period_ns;
    m->exact = exact && period_ns;
    m->state = DVM_UNKNOWN;
    m->pending = DVM_UNKNOWN;
}

/* The median of the steps that ended in the last DVM_WINDOW_NS; 0 when
 * fewer than DVM_MIN_STEPS. */
static inline uint32_t dvm_median(const dvm_model *m, uint64_t now_ns)
{
    uint32_t v[DVM_STEPS];
    unsigned i, k = 0, j;

    for (i = 0; i < m->n; i++) {
        unsigned at = (m->pos + DVM_STEPS - 1 - i) % DVM_STEPS;
        if (now_ns - m->step_at[at] > DVM_WINDOW_NS)
            break;
        /* insertion sort: at most a few hundred, once per return */
        for (j = k; j > 0 && v[j - 1] > m->step_ns[at]; j--)
            v[j] = v[j - 1];
        v[j] = m->step_ns[at];
        k++;
    }
    return k < DVM_MIN_STEPS ? 0 : v[k / 2];
}

/* One WaitForVBlank return at t_ns. Returns DVM_* bits; with DVM_REPORT,
 * *when_ns is the refresh to report and *period_ns its period (0 when the
 * period is not exact: the kernel measures it from the steps, as before). */
static inline int dvm_refresh(dvm_model *m, uint64_t t_ns, uint64_t *when_ns,
                              uint64_t *period_ns)
{
    const uint64_t p = m->period_ns;
    int res = 0, meas;

    if (!m->first_ns)
        m->first_ns = t_ns;
    if (m->last_ns && t_ns > m->last_ns && t_ns - m->last_ns <= DVM_BREAK_NS) {
        m->step_at[m->pos] = t_ns;
        m->step_ns[m->pos] = (uint32_t)(t_ns - m->last_ns);
        m->pos = (m->pos + 1) % DVM_STEPS;
        if (m->n < DVM_STEPS)
            m->n++;
    }
    m->last_ns = t_ns;

    /* The detector. Unknown period: the old behaviour, a fixed display
     * whose period the kernel measures. */
    if (!p) {
        meas = DVM_FIXED;
    } else {
        m->median_ns = dvm_median(m, t_ns);
        if (!m->median_ns || t_ns - m->first_ns < DVM_WINDOW_NS * 4 / 5)
            meas = m->state;            /* not enough seen yet */
        else
            meas = (uint64_t)m->median_ns * 2 > p * 3 ? DVM_VRR : DVM_FIXED;
    }
    if (meas != DVM_UNKNOWN && meas != m->state) {
        if (m->state == DVM_UNKNOWN) {
            m->state = meas;
            res |= DVM_CHANGED;
        } else if (m->pending != meas) {
            m->pending = meas;
            m->pending_ns = t_ns;
        } else if (t_ns - m->pending_ns >= DVM_HOLD_NS) {
            m->state = meas;
            m->pending = DVM_UNKNOWN;
            res |= DVM_CHANGED;
        }
    } else {
        m->pending = DVM_UNKNOWN;
    }
    if (res & DVM_CHANGED)
        m->grid_ns = 0;                 /* a fresh phase on the new state */

    if (m->state != DVM_FIXED)
        return res;
    if (!m->exact) {
        *when_ns = t_ns;
        *period_ns = 0;
        return res | DVM_REPORT;
    }

    /* The phase filter. */
    if (!m->grid_ns || t_ns <= m->grid_ns || t_ns - m->grid_ns > DVM_BREAK_NS) {
        m->grid_ns = t_ns;
        m->off = 0;
    } else {
        uint64_t k = (t_ns - m->grid_ns + p / 2) / p;
        uint64_t pred = m->grid_ns + k * p;
        int64_t r = (int64_t)(t_ns - pred), lim = (int64_t)(p / 8);
        if (r > (int64_t)(p / 4) || r < -(int64_t)(p / 4)) {
            /* Far off: a late wake once in a while, but a run of them is a
             * grid that moved (a stated period that no longer holds). */
            if (++m->off >= 8) {
                m->grid_ns = t_ns;
                m->off = 0;
                goto report;
            }
        } else {
            m->off = 0;
        }
        if (r > lim)
            r = lim;
        else if (r < -lim)
            r = -lim;
        m->grid_ns = pred + r / DVM_GAIN;
    }
report:
    *when_ns = m->grid_ns;
    *period_ns = p;
    return res | DVM_REPORT;
}

#endif

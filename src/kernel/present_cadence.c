/*
 * present_cadence.c - see present_cadence.h. Pure arithmetic: no clock, no
 * lock, no platform call (the process's ring is present_cadence_ring.c).
 */
#include "present_cadence.h"

#include <stdlib.h>
#include <string.h>

/* ── the display ─────────────────────────────────────────── */

int pc_multiple_of_60(double hz, int *mult)
{
    int m = (int)(hz / 60.0 + 0.5);
    double d;

    if (m < 1)
        m = 1;
    if (mult)
        *mult = m;
    if (hz <= 0.0)
        return 0;
    d = hz - 60.0 * m;
    if (d < 0)
        d = -d;
    return d <= 0.01 * 60.0 * m;
}

/* ── the strategy ────────────────────────────────────────── */

int pc_setting(const char *text)
{
    if (!text)
        return PC_SET_LEGACY;
    if (!strcmp(text, "auto") || !strcmp(text, "link"))
        return PC_SET_LINK;
    if (!strcmp(text, "aimed"))
        return PC_SET_AUTO;
    if (!strcmp(text, "lock"))
        return PC_SET_LOCK;
    if (!strcmp(text, "timed"))
        return PC_SET_TIMED;
    return PC_SET_LEGACY;
}

int pc_pick(const pc_display *d, int setting)
{
    int fixed_known = d && d->refresh_hz > 0.0 && !d->vrr;

    switch (setting) {
    case PC_SET_LEGACY:
        return PC_LEGACY;
    case PC_SET_TIMED:
        return PC_TIMED;
    case PC_SET_LINK:
        return PC_LINK;
    case PC_SET_LOCK:
        /* A lock needs a grid: without a rate there is none. */
        return d && d->refresh_hz > 0.0 ? PC_LOCK : PC_TIMED;
    default:
        break;
    }
    if (!fixed_known)
        return PC_TIMED;              /* variable, or nothing known: no grid to lock to */
    return pc_multiple_of_60(d->refresh_hz, NULL) ? PC_LOCK : PC_TIMED_ODD;
}

const char *pc_strategy_name(int strategy)
{
    switch (strategy) {
    case PC_LOCK:      return "lock";
    case PC_TIMED:     return "timed";
    case PC_TIMED_ODD: return "timed (not a multiple of 60)";
    case PC_LINK:      return "display link";
    default:           return "legacy";
    }
}

/* ── the pacer ───────────────────────────────────────────── */

void pc_pacer_init(pc_pacer *p, int strategy, const pc_display *d,
                   uint64_t phase_ns, uint64_t lead_ns, int depth)
{
    memset(p, 0, sizeof *p);
    p->strategy = strategy;
    if (d && d->refresh_hz > 0.0)
        p->period_ns = (uint64_t)(1e9 / d->refresh_hz + 0.5);
    p->phase_ns = phase_ns;
    p->lead_ns = lead_ns;
    p->depth = depth < 0 ? 0 : depth > PC_DEPTH_MAX ? PC_DEPTH_MAX : depth;
    p->depth_max = p->depth;
    while (p->period_ns && p->depth_max < PC_DEPTH_MAX
           && phase_ns + (uint64_t)(p->depth_max + 1) * p->period_ns <= PC_LATENCY_MAX_NS)
        p->depth_max++;
    p->grace = PC_GRACE_FRAMES;
    p->depth0 = p->depth;
    p->lead0_ns = p->lead_ns;
    p->decay_frames = PC_DECAY_FRAMES;
    p->since_back = 1 << 30;
    p->handover_ns = PC_HANDOVER_NS;
}

void pc_pacer_set_handover(pc_pacer *p, uint64_t handover_ns)
{
    p->handover_ns = handover_ns;
}

uint64_t pc_pacer_lead_max(const pc_pacer *p)
{
    uint64_t m = PC_LEAD_MAX_NS;
    if (p->frame_ns && p->frame_ns * PC_INFLIGHT > PC_INFLIGHT_MARGIN_NS) {
        uint64_t f = p->frame_ns * PC_INFLIGHT - PC_INFLIGHT_MARGIN_NS;
        if (f < m)
            m = f;
    }
    return m;
}

void pc_pacer_set_frame(pc_pacer *p, uint64_t frame_ns)
{
    uint64_t m;
    p->frame_ns = frame_ns;
    m = pc_pacer_lead_max(p);
    if (p->lead_ns > m)
        p->lead_ns = m;
}

void pc_pacer_relearn(pc_pacer *p)
{
    p->depth = p->depth0;
    p->lead_ns = p->lead0_ns;
    if (p->lead_ns > pc_pacer_lead_max(p))
        p->lead_ns = pc_pacer_lead_max(p);
    p->grace = PC_GRACE_FRAMES;
    p->decay_frames = PC_DECAY_FRAMES;
    p->since_back = 1 << 30;
    p->frames = p->slips = p->lates = 0;
    p->calm = 0;
}

/* The point of the grid (g + k * period, any whole k) nearest t. */
static uint64_t grid_nearest(uint64_t g, uint64_t period, uint64_t t)
{
    if (t >= g)
        return g + (t - g + period / 2) / period * period;
    {
        uint64_t back = (g - t + period / 2) / period * period;
        return back > g ? 0 : g - back;
    }
}

/* The first point of the grid at or after t. */
static uint64_t grid_at_or_after(uint64_t g, uint64_t period, uint64_t t)
{
    if (t >= g)
        return g + (t - g + period - 1) / period * period;
    return g - (g - t) / period * period;
}

void pc_pacer_aim(const pc_pacer *p, uint64_t vblank_ns, uint64_t now_ns, pc_aim *out)
{
    memset(out, 0, sizeof *out);
    if (!vblank_ns)
        return;
    switch (p->strategy) {
    case PC_LOCK: {
        uint64_t want, aligned, off;
        if (!p->grid_ns || !p->period_ns)
            return;                    /* no grid yet: at once; its on-glass time starts one */
        want = vblank_ns + p->phase_ns;
        aligned = grid_nearest(p->grid_ns, p->period_ns, want);
        off = aligned > want ? aligned - want : want - aligned;
        if (off > p->period_ns / 4)
            out->late = 1;             /* the vblank is not on the grid (yet): no accounting */
        if (now_ns + PC_LATE_GUARD_NS > aligned) {
            /* Picked up after its own refresh: the next one it can make. */
            aligned = grid_at_or_after(p->grid_ns, p->period_ns, now_ns + PC_LATE_GUARD_NS);
            out->late = 1;
        }
        out->target_ns = aligned + (uint64_t)p->depth * p->period_ns;
        /* The system shows a frame at the first refresh at or after the
         * time it is given: a little after the refresh before this one
         * names this one while the grid estimate is off by less than the
         * hand-over, and leaves the compositor most of a refresh before
         * the frame's own (BO-239; it was half a refresh before it). */
        {
            uint64_t h = p->handover_ns, half = p->period_ns / 2;
            if (p->handover_half || h > half)
                h = half;
            out->at_ns = out->target_ns - p->period_ns + h;
        }
        break;
    }
    case PC_TIMED:
        out->target_ns = vblank_ns + p->lead_ns;
        out->at_ns = out->target_ns;
        if (now_ns >= out->target_ns)
            out->late = 1;
        break;
    case PC_TIMED_ODD:
        /* Shown at the first refresh at or after the vblank plus the lead:
         * somewhere in the refresh after it. */
        out->at_ns = vblank_ns + p->lead_ns;
        out->target_ns = out->at_ns + p->period_ns / 2;
        out->late = 1;                 /* no grid of its own to slip from */
        break;
    default:
        break;
    }
}

int pc_pacer_glass(pc_pacer *p, const pc_aim *aim, uint64_t glass_ns)
{
    int raised = 0;

    uint64_t prev_glass = p->grid_ns;    /* the frame shown before this one */

    if (!glass_ns)
        return 0;
    if (p->strategy == PC_LOCK || p->strategy == PC_TIMED_ODD)
        p->grid_ns = glass_ns;
    if (p->grace > 0)
        p->grace--;
    else if (aim && aim->target_ns && !aim->late) {
        if (p->strategy == PC_LOCK && glass_ns > aim->target_ns + p->period_ns / 2) {
            /* Late. Its refresh taken by the frame before it (shown at or
             * after this one's target): forced, not a slip of its own. */
            if (prev_glass && prev_glass + p->period_ns / 4 >= aim->target_ns) {
                p->forced_all++;
            } else {
                p->slips++;
                p->slips_all++;
                p->calm = 0;
            }
        } else if (p->strategy == PC_LOCK && glass_ns + p->period_ns / 2 < aim->target_ns) {
            /* Shown before its refresh: the hand-over named an earlier one. */
            p->earlies_all++;
            if (!p->handover_half && p->earlies_all >= PC_EARLIES_BACK
                    && p->handover_ns < p->period_ns / 2) {
                p->handover_half = 1;
                raised = 2;
            }
        } else if (p->strategy == PC_TIMED && glass_ns > aim->target_ns + PC_LATE_NS) {
            p->lates++;
            p->lates_all++;
            p->calm = 0;
        }
    }
    p->frames++;
    if (p->since_back < (1 << 30))
        p->since_back++;
    /* Calm long enough: one step back toward the setting's (BO-211). */
    if (++p->calm >= p->decay_frames) {
        p->calm = 0;
        if (p->strategy == PC_LOCK && p->depth > p->depth0) {
            p->depth--;
            p->lowered++;
            p->since_back = 0;
            raised = -1;
        } else if (p->strategy == PC_TIMED && p->lead_ns > p->lead0_ns) {
            p->lead_ns = p->lead_ns - p->lead0_ns >= PC_LEAD_STEP_NS
                       ? p->lead_ns - PC_LEAD_STEP_NS : p->lead0_ns;
            p->lowered++;
            p->since_back = 0;
            raised = -1;
        }
    }
    if (p->strategy == PC_LOCK && p->slips >= PC_SLIPS_RAISE) {
        if (p->depth < p->depth_max) {
            p->depth++;
            p->raised++;
            raised = 1;
            if (p->since_back <= 2 * PC_SLIP_WINDOW && p->decay_frames < PC_DECAY_MAX)
                p->decay_frames *= 2;
        }
        p->calm = 0;
        p->frames = p->slips = 0;
    } else if (p->strategy == PC_TIMED && p->lates >= PC_LATES_RAISE) {
        if (p->lead_ns + PC_LEAD_STEP_NS <= pc_pacer_lead_max(p)) {
            p->lead_ns += PC_LEAD_STEP_NS;
            p->raised++;
            raised = 1;
            if (p->since_back <= 2 * PC_SLIP_WINDOW && p->decay_frames < PC_DECAY_MAX)
                p->decay_frames *= 2;
        }
        p->calm = 0;
        p->frames = p->lates = 0;
    }
    if (p->frames >= PC_SLIP_WINDOW)
        p->frames = p->slips = p->lates = 0;
    return raised;
}

uint64_t pc_pacer_lock_report(const pc_pacer *p, uint64_t glass_ns)
{
    if (p->strategy != PC_LOCK || !glass_ns || glass_ns <= p->phase_ns)
        return 0;
    return glass_ns - p->phase_ns;
}

/* ── the display link ────────────────────────────────────── */

void pc_link_init(pc_link *l, uint64_t phase_ns, uint64_t guard_ns)
{
    memset(l, 0, sizeof *l);
    l->phase_ns = l->phase0_ns = phase_ns;
    l->guard_ns = guard_ns;
    l->grace = PC_GRACE_FRAMES;
    l->decay_frames = PC_DECAY_FRAMES;
    l->since_back = 1 << 30;
    l->slack_ns = PC_LINK_SLACK_AUTO;
}

void pc_link_reset(pc_link *l)
{
    l->n_iv = l->i_iv = 0;
    l->n_ld = l->i_ld = 0;
    l->period_ns = 0;
    l->lead_ns = 0;
    l->last_tpt_ns = 0;
    l->grace = PC_GRACE_FRAMES;
    l->frames = l->misses = 0;
    l->calm = 0;
}

uint64_t pc_link_phase_max(const pc_link *l)
{
    /* At most the period less the guard and 1 ms: the vblank stays after
     * the deadline of the refresh before. Never under the phase asked. */
    uint64_t m = l->period_ns > l->guard_ns + 1000000ull
               ? l->period_ns - l->guard_ns - 1000000ull : 0;
    return m > l->phase0_ns ? m : l->phase0_ns;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t pc_link_tick(pc_link *l, uint64_t tt_ns, uint64_t tpt_ns, int prev_empty,
                      uint64_t *period_ns)
{
    if (l->last_tpt_ns && tpt_ns > l->last_tpt_ns) {
        uint64_t d = tpt_ns - l->last_tpt_ns;
        if (d >= 2000000ull && d <= 50000000ull) {
            l->iv[l->i_iv] = d;
            l->i_iv = (l->i_iv + 1) % PC_LINK_IV;
            if (l->n_iv < PC_LINK_IV)
                l->n_iv++;
            if (l->n_iv >= 5) {
                /* The median: a callback the system left out (two
                 * refreshes) or a late one does not move it. */
                uint64_t s[PC_LINK_IV];
                memcpy(s, l->iv, sizeof s[0] * (size_t)l->n_iv);
                qsort(s, (size_t)l->n_iv, sizeof s[0], cmp_u64);
                l->period_ns = s[l->n_iv / 2];
            }
        }
    }
    l->last_tpt_ns = tpt_ns;
    if (l->phase_ns > pc_link_phase_max(l))
        l->phase_ns = pc_link_phase_max(l);
    l->prev_empty = prev_empty;
    *period_ns = l->period_ns;
    /* The link's lead (deadline to glass): the median of its last ones, so
     * one odd callback does not move the slack, and with it the grid the
     * vblank is reported on. */
    if (tpt_ns > tt_ns && tpt_ns - tt_ns <= 100000000ull) {
        l->ld[l->i_ld] = tpt_ns - tt_ns;
        l->i_ld = (l->i_ld + 1) % PC_LINK_IV;
        if (l->n_ld < PC_LINK_IV)
            l->n_ld++;
        if (l->n_ld >= 5) {
            uint64_t s[PC_LINK_IV];
            memcpy(s, l->ld, sizeof s[0] * (size_t)l->n_ld);
            qsort(s, (size_t)l->n_ld, sizeof s[0], cmp_u64);
            l->lead_ns = s[l->n_ld / 2];
        } else {
            l->lead_ns = tpt_ns - tt_ns;
        }
    }
    /* The slack this refresh leaves room for: the deadline stays at least
     * PC_LINK_SLACK_ROOM_NS before its time on the glass. */
    {
        uint64_t room = tpt_ns > tt_ns + PC_LINK_SLACK_ROOM_NS
                      ? tpt_ns - tt_ns - PC_LINK_SLACK_ROOM_NS : 0;
        uint64_t want = l->slack_ns;
        if (want == PC_LINK_SLACK_AUTO)
            want = l->lead_ns > PC_LINK_COMMIT_MIN_NS ? l->lead_ns - PC_LINK_COMMIT_MIN_NS : 0;
        l->slack_eff_ns = want < room ? want : room;
    }
    tt_ns += l->slack_eff_ns;
    /* For pc_link_frame: the refresh before this one. */
    l->prev_tt_ns = l->cur_tt_ns;
    l->cur_tt_ns = tt_ns;
    return tt_ns > l->phase_ns ? tt_ns - l->phase_ns : 0;
}

void pc_link_set_slack(pc_link *l, uint64_t slack_ns)
{
    l->slack_ns = slack_ns;
}

uint64_t pc_link_deadline(const pc_link *l)
{
    return l->cur_tt_ns;
}

uint64_t pc_link_take_until(const pc_link *l, uint64_t tt_ns)
{
    return tt_ns > l->guard_ns ? tt_ns - l->guard_ns : tt_ns;
}

int pc_link_stale(const pc_link *l, uint64_t vblank_ns)
{
    return vblank_ns && l->prev_tt_ns && vblank_ns < pc_link_take_until(l, l->prev_tt_ns);
}

int pc_link_frame(pc_link *l, uint64_t vblank_ns, int locked, int *missed)
{
    int moved = 0, miss = 0;
    l->frames_all++;
    if ((l->prev_empty || locked) && pc_link_stale(l, vblank_ns))
        miss = 1;
    if (missed)
        *missed = miss;
    if (l->grace > 0) {
        l->grace--;
        return 0;
    }
    if (miss) {
        l->misses++;
        l->misses_all++;
        l->calm = 0;
    }
    l->frames++;
    if (l->since_back < (1 << 30))
        l->since_back++;
    if (++l->calm >= l->decay_frames) {
        l->calm = 0;
        if (l->phase_ns > l->phase0_ns) {
            l->phase_ns = l->phase_ns - l->phase0_ns >= PC_LINK_STEP_NS
                        ? l->phase_ns - PC_LINK_STEP_NS : l->phase0_ns;
            l->lowered++;
            l->since_back = 0;
            moved = -1;
        }
    }
    if (l->misses >= PC_LINK_MISSES_RAISE) {
        if (l->phase_ns + PC_LINK_STEP_NS <= pc_link_phase_max(l)) {
            l->phase_ns += PC_LINK_STEP_NS;
            l->raised++;
            moved = 1;
            if (l->since_back <= 2 * PC_SLIP_WINDOW && l->decay_frames < PC_DECAY_MAX)
                l->decay_frames *= 2;
        }
        l->calm = 0;
        l->frames = l->misses = 0;
    }
    if (l->frames >= PC_SLIP_WINDOW)
        l->frames = l->misses = 0;
    return moved;
}

/* ── the statistics ──────────────────────────────────────── */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

void pc_analyze(const pc_sample *s, int n, const pc_display *d, uint64_t now_ns,
                pc_report *out)
{
    static const double frame_ms = 1000.0 / 60.0;
    double lat[PC_RING], ivs[PC_RING];
    double refresh_ms = 0.0, tol_ms = 1.5;
    int i, prev = -1, n_lat = 0, gap = 0, mult = 0, recent = 0, fixed;
    uint64_t newest = 0;

    memset(out, 0, sizeof *out);
    if (n > PC_RING) {
        s += n - PC_RING;
        n = PC_RING;
    }
    fixed = d && d->refresh_hz > 0.0 && !d->vrr;
    if (fixed) {
        refresh_ms = 1000.0 / d->refresh_hz;
        tol_ms = refresh_ms / 4.0;
        if (pc_multiple_of_60(d->refresh_hz, &mult))
            out->expected = mult;
        else
            out->warn_rate = 1;
    }
    out->samples = n;
    for (i = 0; i < n; i++) {
        if (s[i].steps > out->steps_max)
            out->steps_max = s[i].steps;
        if (s[i].steps > 1)
            out->steps_multi++;
        if (s[i].skipped)
            out->skipped++;
        if (!s[i].glass_ns) {
            if (prev >= 0)
                gap++;
            continue;
        }
        out->shown++;
        if (i >= n - 16)
            recent++;
        if (s[i].glass_ns > newest)
            newest = s[i].glass_ns;
        if (s[i].vblank_ns && s[i].glass_ns > s[i].vblank_ns)
            lat[n_lat++] = (double)(s[i].glass_ns - s[i].vblank_ns) / 1e6;
        if (prev >= 0 && s[i].glass_ns > s[prev].glass_ns && out->n_iv < PC_RING) {
            double iv = (double)(s[i].glass_ns - s[prev].glass_ns) / 1e6, want, diff;
            int k = out->n_iv++;
            /* The title's own spacing of the two frames: 16.7 ms at 60 fps,
             * 33.3 where it swaps every second vblank or missed one. */
            if (s[i].vblank_ns > s[prev].vblank_ns && s[prev].vblank_ns)
                want = (double)(s[i].vblank_ns - s[prev].vblank_ns) / 1e6;
            else
                want = frame_ms * (double)(uint32_t)(s[i].flip - s[prev].flip);
            diff = iv > want ? iv - want : want - iv;
            out->iv_ms[k] = (float)iv;
            out->iv_refresh[k] = (float)(iv / (fixed ? refresh_ms : frame_ms));
            out->iv_uneven[k] = diff > tol_ms;
            /* Frames the display never showed between the two. */
            if (out->iv_uneven[k] || gap)
                out->uneven += out->iv_uneven[k] > gap ? out->iv_uneven[k] : gap;
            ivs[k] = iv;
        }
        prev = i;
        gap = 0;
    }
    if (out->n_iv) {
        qsort(ivs, (size_t)out->n_iv, sizeof ivs[0], cmp_double);
        out->iv_p50_ms = ivs[out->n_iv / 2];
        out->iv_min_ms = ivs[0];
        out->iv_max_ms = ivs[out->n_iv - 1];
    }
    if (n_lat) {
        qsort(lat, (size_t)n_lat, sizeof lat[0], cmp_double);
        out->lat_p50_ms = lat[n_lat / 2];
        out->lat_p95_ms = lat[(int)((n_lat - 1) * 0.95)];
    }
    out->available = recent >= 4
                     && (!now_ns || (newest && (now_ns <= newest || now_ns - newest < 500000000ull)));
    out->warn_uneven = out->available && out->n_iv >= 30 && out->uneven >= 3
                       && out->uneven * 50 >= out->n_iv;
}

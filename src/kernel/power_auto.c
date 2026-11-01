/*
 * power_auto.c - Power mode "Auto": the vblank hold's spin lead picked at
 * run time (BO-175). The rules are in power_auto.h.
 */
#include "power_auto.h"

#include <stdio.h>
#include <string.h>

#define MS                 1000000ull
#define PERIOD_NS          16666667ll           /* one emulated vblank */
#define WINDOW_NS          (2000 * MS)
#define HITCH_GAP_NS       (250 * MS)           /* no hold for this long: a stall */
#define QUIET_NS           (3000 * MS)          /* after a stall, decide nothing */
#define STALL_NS           (100 * MS)           /* a frame this late: a stall (a load), not a hitch */
#define LATE_WAKE_NS       (1 * MS)
#define DOWN_RUN           5                    /* clean windows before a step down (menus) */
#define DOWN_RUN_PLAY      15                   /* the same in play: 30 s */
#define PLAY_FLOOR         2                    /* in play never below this level (4 ms) */
#define DOWN_RUN_MAX       150                  /* 5 min */
#define OSC_NS             (30000 * MS)         /* up this soon after a down: oscillation */
#define DOWN_RESET_NS      (600000 * MS)        /* no step up for this long: back to DOWN_RUN */
#define CEIL_WAIT_NS       (60000 * MS)
#define CEIL_WAIT_MAX_NS   (600000 * MS)
#define HELD_DECAY_NS      (600000 * MS)
#define RUNUP_NS           (20000 * MS)         /* a climb this soon before a stall: its run-up */
#define MAX_LEVEL          (XBOX_POWER_AUTO_LEVELS - 1)

static const uint32_t k_lead_us[XBOX_POWER_AUTO_LEVELS] = { 0, 2000, 4000, 8000, 12000, 17000 };

uint64_t xbox_PowerAutoLeadNs(int level)
{
    if (level < 0)
        level = 0;
    if (level > MAX_LEVEL)
        level = MAX_LEVEL;
    return (uint64_t)k_lead_us[level] * 1000u;
}

int xbox_PowerAutoLevel(const XboxPowerAuto *a)
{
    return a->level;
}

void xbox_PowerAutoInit(XboxPowerAuto *a)
{
    memset(a, 0, sizeof *a);
    a->level = XBOX_POWER_AUTO_START;
    a->held = XBOX_POWER_AUTO_START;
    a->ceiling = MAX_LEVEL;
    a->down_need = DOWN_RUN;
    a->ceiling_wait_ns = CEIL_WAIT_NS;
    a->probe_from = -1;
    snprintf(a->why, sizeof a->why, "start (as Balanced)");
}

static void window_reset(XboxPowerAuto *a, uint64_t now)
{
    a->win_start_ns = now;
    a->n = a->ontime = a->tight = a->waited = a->late_wakes = a->nears = a->hitch = 0;
    a->ontime1 = a->busy = 0;
}

/* The end of a 2 s window: the decisions. Returns 1 when the level moved. */
static int window_end(XboxPowerAuto *a, uint64_t now)
{
    int score = a->nears + a->late_wakes;
    /* A hitch (a frame one to six vblanks late: a shader compile, a file
     * read) skips this window's decision and nothing else; a stall (a
     * load) also quiets the next 3 s. */
    int disturbed = a->hitch || now < a->quiet_until_ns;
    int bad = !disturbed && (a->nears >= 2
                             || (a->waited >= 20 && a->late_wakes * 20 >= a->waited));
    /* In play: frames that take a good part of their interval (the race on
     * a handheld: half the frame or less to spare in a quarter of them; a
     * menu has nearly all of it). The CPU clock matters there: a floor of
     * spin, and slower steps down. */
    int play = a->ontime1 >= 30 && a->busy * 4 >= a->ontime1;
    int need = play && a->down_need < DOWN_RUN_PLAY ? DOWN_RUN_PLAY : a->down_need;
    int before = a->level;

    a->tot.bad_windows += bad;
    /* What has run out. */
    if (a->ceiling < MAX_LEVEL && now >= a->ceiling_until_ns)
        a->ceiling = MAX_LEVEL;
    if (a->held > 0 && now - a->held_at_ns > HELD_DECAY_NS) {
        a->held--;
        a->held_at_ns = now;
    }
    if (a->down_need > DOWN_RUN && now - a->last_up_ns > DOWN_RESET_NS)
        a->down_need = DOWN_RUN;
    a->play = play;

    if (disturbed) {
        /* nothing decided */
    } else if (a->probe_from >= 0) {
        /* The window after a step up: is the trouble gone? Yes: this level
         * is the one needed. Not yet: one or two more steps, up to the
         * top; still there at the top: spinning is not the cure (the GPU
         * or the timer is the limit): back to where the climb started.
         * (Less trouble is not enough: misses that spinning does not cure
         * come and go at random, and one quiet window would keep the
         * busy-wait.) */
        if (!bad) {
            if (a->level > a->held) {
                a->held = a->level;
                a->held_at_ns = now;
            }
            a->probe_from = -1;
        } else if (a->level < a->ceiling) {
            int step = a->nears * 50 >= a->n ? 2 : 1;
            a->level = a->level + step > a->ceiling ? a->ceiling : a->level + step;
            a->last_up_ns = now;
            a->tot.ups++;
            snprintf(a->why, sizeof a->why, "trouble %d -> %d after the last step: more",
                     a->probe_base, score);
        } else {
            a->level = a->probe_from;
            a->ceiling = a->probe_from;
            a->ceiling_until_ns = now + a->ceiling_wait_ns;
            a->ceiling_wait_ns = a->ceiling_wait_ns * 2 > CEIL_WAIT_MAX_NS
                               ? CEIL_WAIT_MAX_NS : a->ceiling_wait_ns * 2;
            a->tot.reverts++;
            snprintf(a->why, sizeof a->why, "more spin did not help (%d -> %d): back",
                     a->probe_base, score);
            a->probe_from = -1;
        }
        a->clean_run = 0;
    } else if (bad) {
        /* At once, on the first bad window: a bend that misses for 4 s has
         * help within 2. Two levels when 2 % of the holds missed. */
        int step = a->nears * 50 >= a->n ? 2 : 1;
        int to = a->level + step > a->ceiling ? a->ceiling : a->level + step;
        a->clean_run = 0;
        if (to > a->level) {
            if (a->last_down_ns && now - a->last_down_ns < OSC_NS)
                a->down_need = a->down_need * 2 > DOWN_RUN_MAX ? DOWN_RUN_MAX : a->down_need * 2;
            /* The start of a climb (not a step within one): where it began
             * and what was held, for a stall that may follow. */
            if (!a->climb_ns || now - a->climb_ns > RUNUP_NS) {
                a->climb_from = a->level;
                a->climb_held = a->held;
                a->climb_ns = now;
            }
            a->probe_from = a->level;
            a->probe_base = score;
            a->level = to;
            a->last_up_ns = now;
            a->tot.ups++;
            snprintf(a->why, sizeof a->why, "%d near misses, %d late wakes of %d waits",
                     a->nears, a->late_wakes, a->waited);
        }
    } else {
        if (++a->clean_run >= need && a->level > (play ? PLAY_FLOOR : 0)
                && a->ontime >= 30 && a->tight * 10 <= a->ontime) {
            a->level--;
            a->last_down_ns = now;
            a->clean_run = 0;
            a->tot.downs++;
            snprintf(a->why, sizeof a->why, "steady with headroom for %d s",
                     need * (int)(WINDOW_NS / (1000 * MS)));
        }
    }
    /* The floor in play, whatever the window decided. */
    if (play && a->level < PLAY_FLOOR) {
        a->level = PLAY_FLOOR;
        snprintf(a->why, sizeof a->why, "in play: at least %llu ms",
                 (unsigned long long)(xbox_PowerAutoLeadNs(PLAY_FLOOR) / 1000000u));
    }
    window_reset(a, now);
    return a->level != before;
}

int xbox_PowerAutoFeed(XboxPowerAuto *a, const XboxPowerAutoSample *s)
{
    uint64_t now = s->now_ns;
    int gap = a->last_ns && now - a->last_ns > HITCH_GAP_NS;
    int changed = 0;

    a->last_ns = now;
    if (!a->win_start_ns)
        a->win_start_ns = a->held_at_ns = now;
    if (!s->interval || s->interval > 4)
        return 0;
    a->n++;
    a->tot.holds++;
    if (gap || s->slack_ns < -PERIOD_NS) {
        int stall = gap || s->slack_ns < -(int64_t)STALL_NS;
        a->hitch++;
        a->tot.hitches++;
        /* A stall (a load): the first of a cluster undoes a climb that was
         * its run-up, and takes the level back to the one last proven
         * needed (a scene change). A shorter hitch (a shader compile in a
         * bend) only skips the window's decision. */
        if (stall && now >= a->quiet_until_ns) {
            int to;
            if (a->climb_ns && now - a->climb_ns < RUNUP_NS && a->level > a->climb_from) {
                a->level = a->climb_from;
                a->held = a->climb_held;
                a->tot.reverts++;
                snprintf(a->why, sizeof a->why, "a stall followed: the climb was the run-up to a load");
                changed = 1;
            }
            a->probe_from = -1;
            a->climb_ns = 0;
            to = a->held < a->ceiling ? a->held : a->ceiling;
            if (to > a->level) {
                a->level = to;
                a->tot.restores++;
                snprintf(a->why, sizeof a->why, "a stall (scene change?): back to the level last needed");
                changed = 1;
            }
        }
        if (stall)
            a->quiet_until_ns = now + QUIET_NS;
    } else if (s->slack_ns < 0) {
        a->nears++;
        a->tot.near_misses++;
    } else {
        a->ontime++;
        if (s->slack_ns < (int64_t)s->interval * PERIOD_NS / 4)
            a->tight++;
        if (s->interval == 1) {
            a->ontime1++;
            if (s->slack_ns < PERIOD_NS / 2)
                a->busy++;
        }
    }
    if (s->wake_ns >= 0) {
        a->waited++;
        a->tot.waits++;
        if (s->wake_ns > (int64_t)LATE_WAKE_NS) {
            a->late_wakes++;
            a->tot.late_wakes++;
        }
    }
    if (now - a->win_start_ns >= WINDOW_NS)
        changed |= window_end(a, now);
    return changed;
}

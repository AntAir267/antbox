/*
 * present_cadence.h - when frames reach the glass: the strategy per display,
 * the target time of each present, and the statistics the overlay's graph
 * and RECOMP_TRACE=glass show (BO-115).
 *
 * A 60 fps title is smooth only when every frame stays on the screen for the
 * same time. The flip-to-flip interval says when the title finished a frame;
 * it does not say when the display showed it. On a 120 Hz panel a present
 * that lands a little early or late is shown one refresh early or late: the
 * frames last 1, 3, 2, 2 ... refreshes while the flip graph is flat. Under
 * variable refresh there is no grid at all, and every bit of present-time
 * jitter is on the glass.
 *
 * Everything here is arithmetic on host nanoseconds with no clock, lock or
 * platform call of its own (tests/present_cadence drives it). The Metal
 * presenter feeds it (fb_present_metal.m: CAMetalLayer, presentedTime); the
 * other presenters are as they were, and the overlay says "not available"
 * for them.
 *
 * Strategies (pc_pick), with display.present_timing = auto:
 *
 *   PC_LOCK       a fixed-rate display whose rate is a multiple of 60
 *                 (60/120/240 Hz). The emulated vblank is moved onto the
 *                 display's grid, `phase` before a refresh (the presenter
 *                 reports each on-glass time less the phase to
 *                 xbox_VblankDisplayRefresh), and each frame is presented
 *                 for one named refresh: the one `depth` refreshes after
 *                 the refresh its vblank sits before. Every frame is shown
 *                 for the same number of refreshes; a frame that misses its
 *                 refresh (a slip) is counted, and four in two seconds
 *                 raise the depth by one, while that still leaves the frame
 *                 on the glass inside two frames of its vblank.
 *   PC_TIMED      a variable-refresh display (VRR, ProMotion). The vblank
 *                 keeps its own 60.000 Hz timer and each frame is presented
 *                 at its vblank plus a fixed lead: the display refreshes
 *                 when the frame arrives, so equal present times are equal
 *                 on-glass times. Frames that arrive late raise the lead.
 *   PC_TIMED_ODD  a fixed-rate display that is not a multiple of 60 (75,
 *                 100, 144 Hz): as PC_TIMED; the cadence cannot be even
 *                 there and the overlay says so.
 *   PC_LEGACY     display.present_timing = legacy: present when ready, as
 *                 before BO-115. Measured all the same.
 *   PC_LINK       macOS 14 and later (BO-236): a CAMetalDisplayLink names
 *                 each refresh the presenter may fill (its callback, a
 *                 drawable, the commit deadline and the time on the
 *                 glass). The emulated vblank, and the High refresh grid,
 *                 sit `phase` before each commit deadline; a frame finished
 *                 by the deadline goes into that refresh's drawable with a
 *                 plain present (no time, no skip rule), a refresh without
 *                 a new frame presents nothing. One path for 60 Hz, 120 Hz,
 *                 variable refresh and ProMotion: the link's rate is the
 *                 title's (60) or, under High refresh, the display's.
 *                 pc_link_* below is its arithmetic.
 */
#ifndef XBOX_PRESENT_CADENCE_H
#define XBOX_PRESENT_CADENCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_FRAME_NS 16666667ull            /* one emulated vblank, 1/60 s */

/* ── the display ── */

typedef struct {
    double refresh_hz;   /* the mode's rate (the highest of a variable display); 0 unknown */
    double min_hz;       /* the lowest rate of a variable display; refresh_hz when fixed */
    int    vrr;          /* the display refreshes when a frame arrives */
    int    builtin;      /* the machine's own panel */
} pc_display;

/* 1 when hz is a whole multiple of 60 within 1% (59.94, 119.88, 240): *mult
 * is the multiple. 0 otherwise, *mult the nearest (at least 1). */
int pc_multiple_of_60(double hz, int *mult);

/* ── the strategy ── */

enum { PC_LEGACY = 0, PC_LOCK, PC_TIMED, PC_TIMED_ODD, PC_LINK };
/* PC_SET_AUTO is the per-display pick of 0.2.x ("aimed"; "auto" before
 * BO-236); PC_SET_LINK asks for PC_LINK, which the presenter grants where
 * it has the display link ("auto" on macOS 14 and later). */
enum { PC_SET_LEGACY = 0, PC_SET_AUTO, PC_SET_LOCK, PC_SET_TIMED, PC_SET_LINK };

/* The setting's value from its text: legacy; auto and link ask for the
 * display link (PC_SET_LINK; the presenter falls back to aimed where there
 * is none); aimed is the per-display pick of 0.2.x (PC_SET_AUTO); lock and
 * timed force one strategy, for experiments. PC_SET_LEGACY for NULL or
 * anything else. */
int pc_setting(const char *text);
int pc_pick(const pc_display *d, int setting);
const char *pc_strategy_name(int strategy);

/* ── the pacer: where each present is aimed ── */

#define PC_DEPTH_MAX      3
#define PC_SLIP_WINDOW    120            /* frames a slip count is over */
#define PC_SLIPS_RAISE    4              /* slips in the window that raise the depth */
#define PC_GRACE_FRAMES   120            /* frames after a (re)start that are not counted */
#define PC_LATE_GUARD_NS  500000ull      /* PC_LOCK: committed this close to its refresh is late */
/* Two presents may be on their way at once (three drawables, one on the
 * glass), so a frame must be on the glass well inside two frames of its
 * vblank: the depth is never raised past that. */
#define PC_LATENCY_MAX_NS 30000000ull
/* PC_LOCK: a frame is handed to the system (presentDrawable:atTime:) this
 * long after the refresh before its own, not half a refresh before its own
 * (BO-239): on a display that takes its frames by direct scanout the half
 * refresh sat on the flip deadline (about 8.3 ms before the refresh at
 * 60 Hz), and a quarter of the frames missed it. The grid estimate is off by
 * at most ~1.2 ms (measured), so 3 ms still names the frame's own refresh;
 * never more than half a refresh (RECOMP_PRESENT_HANDOVER). */
#define PC_HANDOVER_NS    3000000ull
/* Frames shown a refresh or more before their target (the system took the
 * time as "the nearest refresh", not "at or after") that put the hand-over
 * back at half a refresh for the session. */
#define PC_EARLIES_BACK   3
#define PC_LATE_NS        1500000ull     /* PC_TIMED: later than this after the target is late */
#define PC_LATES_RAISE    6              /* lates in the window that raise the lead */
#define PC_LEAD_STEP_NS   2000000ull
#define PC_LEAD_MAX_NS    33000000ull
/* PC_TIMED*: a present holds its drawable from the commit until the system
 * says it is on the glass, about the lead after its vblank, and the
 * presenter lets PC_INFLIGHT of them be on their way at once (three
 * drawables, one on the glass). Frames come every frame_ns (16.7 ms, 8.3
 * under High refresh at 120 Hz), so a lead over PC_INFLIGHT frames less a
 * margin starves the presenter: it then skips frames by its own rule (the
 * #42 ProMotion report, 120 flips a second and about 96 on the glass). The
 * lead is never raised past that, and comes down to it when frames come
 * faster (BO-236). */
#define PC_INFLIGHT       2
#define PC_INFLIGHT_MARGIN_NS 3000000ull
/* A raise is taken back, one step at a time, after this many frames on the
 * glass with no slip (no late frame) since the last raise or step back: a
 * shader-compile burst or a load early in a session no longer leaves the Mac
 * a refresh or two later for the rest of it (BO-211). */
#define PC_DECAY_FRAMES   600
/* A raise right after a step back (within two slip windows): the step back
 * was too soon, and the next waits twice as long, up to 8 x (80 s), so a
 * Mac whose slips come back at the lower depth does not drop and repeat a
 * frame every 12 s for the session. */
#define PC_DECAY_MAX      (8 * PC_DECAY_FRAMES)
typedef struct {
    int      strategy;
    uint64_t period_ns;    /* the display's refresh period (PC_LOCK, PC_TIMED_ODD) */
    uint64_t grid_ns;      /* a refresh on the display's grid: the last on-glass time; 0 none yet */
    uint64_t phase_ns;     /* PC_LOCK: the vblank sits this long before a refresh */
    int      depth;        /* PC_LOCK: refreshes from that refresh to the frame's */
    int      depth_max;    /* the most that keeps a frame inside PC_LATENCY_MAX_NS */
    int      grace;        /* frames still not counted */
    uint64_t lead_ns;      /* PC_TIMED*: present at the vblank plus this */
    uint64_t frame_ns;     /* the time between the title's frames; 0 unknown (BO-236) */
    int      depth0;       /* what the setting asked: a raise decays back to these */
    uint64_t lead0_ns;
    int      calm;         /* frames since the last slip (late), raise or step back */
    int      decay_frames; /* calm frames a step back needs now */
    int      since_back;   /* frames since the last step back */
    uint64_t handover_ns;  /* PC_LOCK: hand a frame over this long after the refresh before its own */
    int      handover_half; /* frames came early: half a refresh before the frame's own instead */
    /* the window's counts */
    int      frames, slips, lates;
    /* since pc_pacer_init. forced_all (PC_LOCK): frames shown a refresh
     * late because the frame before them was shown late and took their
     * refresh (one frame per refresh: a slip costs the next frame its
     * refresh too, and the presenter a skip after that, BO-239). They are
     * not slips of their own and do not count toward a raise. */
    uint32_t slips_all, lates_all, raised, lowered, earlies_all, forced_all;
} pc_pacer;

/* What one present is aimed at. */
typedef struct {
    uint64_t at_ns;        /* the time to hand the system (present at or after); 0: at once */
    uint64_t target_ns;    /* when the frame should be on the glass; 0 unknown */
    int      late;         /* the frame came too late for its own refresh: not a slip */
} pc_aim;

void pc_pacer_init(pc_pacer *p, int strategy, const pc_display *d,
                   uint64_t phase_ns, uint64_t lead_ns, int depth);
/* PC_LOCK: the hand-over after the refresh before a frame's own
 * (PC_HANDOVER_NS by default; at most half a refresh is used). */
void pc_pacer_set_handover(pc_pacer *p, uint64_t handover_ns);
/* PC_TIMED*: frames come every frame_ns (0 unknown): the lead's ceiling
 * (pc_pacer_lead_max) follows, and a lead above it comes down at once. */
void pc_pacer_set_frame(pc_pacer *p, uint64_t frame_ns);
uint64_t pc_pacer_lead_max(const pc_pacer *p);
/* The frame whose emulated vblank was vblank_ns, picked up at now_ns. */
void pc_pacer_aim(const pc_pacer *p, uint64_t vblank_ns, uint64_t now_ns, pc_aim *out);
/* The frame aimed with *aim reached the glass at glass_ns (0: never shown).
 * Returns 1 when the depth or the lead was raised, -1 when a raise was taken
 * back (PC_DECAY_FRAMES calm frames), 2 when frames came early and the
 * hand-over went back to half a refresh. */
int  pc_pacer_glass(pc_pacer *p, const pc_aim *aim, uint64_t glass_ns);
/* The pacing watchdog's reset (BO-211): the depth and lead back to the
 * setting's, the grace anew. The grid (the display's) is kept: a reset
 * drops no frame when the depth was already the setting's. */
void pc_pacer_relearn(pc_pacer *p);
/* PC_LOCK: the time to report to the vblank's display lock for an on-glass
 * time (a point `phase` before the display's grid); 0 for other strategies. */
uint64_t pc_pacer_lock_report(const pc_pacer *p, uint64_t glass_ns);

/* ── the display link (PC_LINK, BO-236) ── */

#define PC_LINK_PHASE_NS     4000000ull  /* the vblank this long before a commit deadline */
#define PC_LINK_GUARD_NS     1500000ull  /* a frame is taken until this long before it */
#define PC_LINK_STEP_NS      1000000ull
#define PC_LINK_MISSES_RAISE 4           /* misses in PC_SLIP_WINDOW frames that move the vblank earlier */

#define PC_LINK_IV 16

typedef struct {
    uint64_t phase_ns, phase0_ns;  /* the vblank this long before a commit deadline */
    uint64_t guard_ns;             /* the newest frame is taken until deadline - guard */
    uint64_t slack_ns;             /* the deadline taken this much after the link's own (BO-236) */
    uint64_t slack_eff_ns;         /* of it what the refresh named last left room for */
    uint64_t period_ns;            /* the link's refresh period: the median of its last steps */
    uint64_t last_tpt_ns;          /* the last refresh named */
    uint64_t cur_tt_ns, prev_tt_ns; /* the deadline of the refresh named last, and of the one before */
    int      prev_empty;           /* and whether it went without a new frame */
    uint64_t iv[PC_LINK_IV];       /* the last steps between named refreshes */
    int      n_iv, i_iv;
    uint64_t ld[PC_LINK_IV];       /* the last leads of the link: deadline to glass (tpt - tt) */
    int      n_ld, i_ld;
    uint64_t lead_ns;              /* their median (the newest one before there are five) */
    int      grace, frames, misses, calm, decay_frames, since_back;
    uint32_t misses_all, raised, lowered, frames_all;
} pc_link;

void pc_link_init(pc_link *l, uint64_t phase_ns, uint64_t guard_ns);
/* The phase's ceiling for the period: a frame must still have most of a
 * refresh between its vblank and the one before. */
uint64_t pc_link_phase_max(const pc_link *l);
/* A callback: the refresh it names is on the glass at tpt_ns; its work
 * must be committed by tt_ns. prev_empty: the refresh before went without a
 * new frame. Returns the point on the display's grid to report to the
 * vblank's lock (tt - phase), *period_ns the link's period (0 not known
 * yet: the median of at least five steps). */
uint64_t pc_link_tick(pc_link *l, uint64_t tt_ns, uint64_t tpt_ns, int prev_empty,
                      uint64_t *period_ns);
/* The link's rate changed (60 <-> 120: High refresh switched, the window on
 * another display): the steps and leads learned are of the old rate, so they
 * go, and no period is reported until five new steps agree (the kernel's
 * lock is told to relearn by the presenter, xbox_VblankDisplayRelearn). The
 * grace starts anew: frames are off the new grid until the lock has it. */
void pc_link_reset(pc_link *l);
/* The link's own commit deadline (targetTimestamp) is two refreshes of the
 * display before the refresh it names, whatever the link's rate (measured,
 * latency 1): 16.67 ms on a 120 Hz display at a link of 60 and of 120, 33.4
 * ms on a 60 Hz display, where that is 37 ms from vblank to glass against
 * 21.7 for presents aimed with a time. A slack takes the deadline later than
 * the link's: the vblank, the take-until and the stale rule all move with it
 * (pc_link_deadline), never closer than PC_LINK_SLACK_ROOM_NS to the refresh
 * named. */
#define PC_LINK_SLACK_ROOM_NS 2000000ull
/* The slack by default (PC_LINK_SLACK_AUTO): the link's lead (deadline to
 * glass, the median of its last callbacks) less PC_LINK_COMMIT_MIN_NS, none
 * when the lead is no more than that. It is keyed to the room the link
 * leaves, not to its rate (the Fable review of BO-236): on a 120 Hz display
 * the lead is 16.67 ms at a link of 60 as at 120, so there is no slack and
 * a frame is committed about 18 ms before the glass (take-until: deadline
 * less the guard), as measured good (20.7 ms vblank to glass, 0.00 %
 * uneven); on a 60 Hz display the lead is 33.4 ms and the slack about 16.4,
 * the same 18 ms commit-to-glass and about 21 ms vblank to glass (37 with
 * the link's own deadline). About one 60 Hz refresh, a little over 16.67 so
 * a 120 Hz display's lead never yields a sliver of slack. */
#define PC_LINK_COMMIT_MIN_NS 17000000ull
#define PC_LINK_SLACK_AUTO    UINT64_MAX
/* slack_ns: a fixed slack (RECOMP_PRESENT_LINK_SLACK), or PC_LINK_SLACK_AUTO
 * (the default after pc_link_init). */
void pc_link_set_slack(pc_link *l, uint64_t slack_ns);
/* The deadline used for the refresh named last: tt + the slack it allowed. */
uint64_t pc_link_deadline(const pc_link *l);
/* The newest frame may be taken until this time for the refresh of tt_ns. */
uint64_t pc_link_take_until(const pc_link *l, uint64_t tt_ns);
/* A frame of emulated vblank vblank_ns is stale for the refresh named last:
 * it was in time for the one before (its vblank before that refresh's
 * take-until). Presented now it would sit a refresh behind its vblank, and
 * so would every frame after it, each one taking the refresh the next was
 * meant for (seen at 120 Hz: 29 ms vblank to glass for minutes instead of
 * 20.7). The presenter waits for the newer frame instead, until the
 * take-until, and drops the stale one: one frame for the refresh of latency. */
int pc_link_stale(const pc_link *l, uint64_t vblank_ns);
/* A new frame of emulated vblank vblank_ns goes into the refresh named last
 * (pc_link_tick), or was dropped as stale for it (a newer one came in time).
 * A miss: the frame is stale (its vblank was in time for the refresh before,
 * pc_link_stale), and either the refresh before went without a new frame or
 * the vblank follows the link (locked: the kernel's lock is on its grid).
 * The second counts the steady "one refresh behind" state (every frame a
 * little after its take-until, each shown a refresh late while every
 * refresh still gets one), which moving the vblank earlier is for. While
 * the lock is still settling a stale frame says nothing about the phase and
 * is not counted unless its refresh went empty. Returns 1 when the phase was
 * raised, -1 when a raise was taken back. *missed: this frame missed. */
int pc_link_frame(pc_link *l, uint64_t vblank_ns, int locked, int *missed);

/* ── the statistics ── */

#define PC_RING 256

typedef struct {
    uint32_t flip;         /* the walker's flip count of the frame */
    uint64_t vblank_ns;    /* its emulated vblank */
    uint64_t glass_ns;     /* on the glass; 0: the system never showed it (or skipped) */
    uint8_t  steps;        /* simulation steps the title ran for it; 0 unknown */
    uint8_t  skipped;      /* the presenter did not present it (no free drawable) */
} pc_sample;

typedef struct {
    int    samples, shown, skipped;
    int    available;      /* on-glass times are coming: the numbers below mean something */
    /* On-glass intervals between frames shown one after the other, oldest
     * first: in ms, and in display refreshes (ms on a variable display, where
     * iv_refresh is the interval in 1/60 s). */
    int    n_iv;
    float  iv_ms[PC_RING];
    float  iv_refresh[PC_RING];
    uint8_t iv_uneven[PC_RING];
    double iv_p50_ms, iv_min_ms, iv_max_ms;
    int    expected;       /* refreshes a 60 fps frame should last; 0 on a variable or unknown display */
    int    uneven;         /* intervals that differ from the title's own spacing, and frames never shown between two shown */
    double lat_p50_ms, lat_p95_ms;   /* emulated vblank to glass */
    int    warn_rate;      /* a fixed display that is not a multiple of 60 */
    int    warn_uneven;    /* uneven enough to be seen */
    int    steps_max;      /* the most steps one of the samples ran; 0 unknown */
    int    steps_multi;    /* samples that ran more than one */
} pc_report;

/* samples[0..n): oldest first. now_ns: the newest shown sample must be within
 * half a second of it for the report to be available. */
void pc_analyze(const pc_sample *samples, int n, const pc_display *d, uint64_t now_ns,
                pc_report *out);

/* ── the process's ring (fb_present_sdl.c; any thread) ── */

/* A presented frame's times. */
void xbox_PresentCadenceNote(const pc_sample *s);
/* The display the window is on and the strategy in use, with the pacer's
 * numbers for the overlay's detail lines. */
typedef struct {
    pc_display display;
    int        strategy;
    int        measured;       /* the backend can tell when a frame is on the glass */
    int        depth;
    double     phase_ms, lead_ms;
    uint32_t   slips, lates, skips;
    char       source[48];     /* where the on-glass times come from */
} pc_status;
void xbox_PresentCadenceStatus(const pc_status *st);
/* Copy the ring (oldest first, at most max) and the status; returns the count. */
int  xbox_PresentCadenceSnapshot(pc_sample *out, int max, pc_status *st);
/* Simulation steps the title ran for the frame of this flip (the title's
 * port calls it; the presenter looks it up when the frame is presented). */
void xbox_PresentCadenceSteps(uint32_t flip, int steps);
int  xbox_PresentCadenceStepsOf(uint32_t flip);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PRESENT_CADENCE_H */

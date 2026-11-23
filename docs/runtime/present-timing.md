# Present timing: when frames reach the glass

`display.present_timing` (`RECOMP_PRESENT_TIMING`): `legacy`, `auto` or
`aimed`.
Code: `src/kernel/present_cadence.{h,c}` (the arithmetic, tested by
`tests/present_cadence`), `src/video/fb_present_metal.m` (the Metal
presenter).

The default is `auto` on macOS (BO-239: with `legacy`'s two drawables a
fullscreen 60 Hz external display showed every other frame for one user) and
`legacy`, what the presenters did before, elsewhere; only the Metal presenter
reads the setting. On macOS 14 and later `auto` is the display-link presenter
(BO-236, below); before macOS 14 it is `aimed`, which was `auto` until 0.2.x
and is described next. A `legacy` stored by 0.2.x moves to `auto` once on
macOS 14 and later (it was mostly the #42 workaround; the settings file then
carries `display.present_timing_rev = "2"`, and `legacy` chosen again stays). Measured numbers for
a title are in its own docs (Burnout 3: `docs/present-timing.md`); nothing on
this page has been verified on a variable-refresh display, a ProMotion panel,
or a host other than macOS.

## The problem

A 60 fps title looks smooth only when every frame stays on the screen for the
same time. The frame-time graph used to show the title's flip-to-flip
interval, which says when a frame was finished, not when the display showed
it. Two things went wrong between the two, with a flat graph:

- **Uneven repeats.** On a 120 Hz display a present that lands a little early
  or late is shown one refresh early or late, so frames last 1, 3, 2, 2
  refreshes. The emulated vblank was a free-running 60.000 Hz timer on macOS,
  so its phase against the display drifted, and for seconds at a time the
  presents sat on a refresh boundary. Under variable refresh there is no grid
  at all: every bit of present-time jitter is on the glass.
- **Latency.** The layer had two drawables and a blocking `nextDrawable`. In a
  window a drawable is free again only when the next one replaces it on the
  glass; once one present was late, every later `nextDrawable` waited a whole
  frame, and the picture stayed about three frames behind its vblank (48.7 ms
  at p50 on a 60 Hz panel, wandering between 32 and 65 ms).

## The display link (`auto` on macOS 14 and later, BO-236)

A `CAMetalDisplayLink` on the window's layer runs on the present thread's run
loop. Each callback names the next refresh the window can fill: a drawable,
the time its work must be committed by (`targetTimestamp`) and the time it
will be on the glass (`targetPresentationTimestamp`). The thread takes the
newest finished frame, waiting for one until 1.5 ms before the deadline
(`RECOMP_PRESENT_LINK_GUARD`), draws it into that drawable and presents it
with a plain `presentDrawable:` (the link asserts on a timed present). A
refresh with no new frame presents nothing. There is no skip rule and no
in-flight count: the system hands out a drawable only for a refresh it can
make, and says which.

The title's clock follows the link. Each callback reports a point 4 ms before
its commit deadline (`RECOMP_PRESENT_LINK_PHASE`) to the vblank's display lock
with the link's period (the median of its steps), so the emulated vblank, and
under High refresh the flip grid, which reads the same reports, falls just
before the refresh it is for. A frame that was in time for the refresh before
and was not shown at it is a miss: its refresh went empty, or (while the
vblank follows the link) it was shown a refresh late or dropped for the newer
one. Four in two seconds move the vblank 1 ms earlier, ten calm seconds take it
back; while the vblank's lock is still settling only the first kind counts.
The link's rate is 60 for the title's own 60 fps (where the display's rate is
a multiple of 60, or variable), and the display's top rate with High refresh
on. When that rate changes while running (High refresh switched, the window
moved to another display) the link's learned steps go and the vblank's lock
learns the new grid at once, instead of standing down for about 18 s on
reports off it. `RECOMP_PRESENT_LINK_LATENCY` is the link's
`preferredFrameLatency` (1, the default, or 2).

The link's own commit deadline is two refreshes of the display before the
refresh it names, whatever its rate: 16.67 ms on a 120 Hz display at a link
of 60 and of 120, 33.4 ms on a 60 Hz display (37 ms from vblank to glass at
59.94 Hz, measured, against 21.7 for `aimed`). So the presenter takes the
deadline later than the link's by the link's lead (deadline to glass, the
median of its last callbacks) less 17 ms, none when the lead is shorter: a
frame is taken until about 18.5 ms before the glass on every display. That is
no slack on a 120 Hz display at either rate (the measured state below) and
about 16.4 ms on a 60 Hz display, which puts the vblank about 21 ms before
the glass there. Not yet measured on a 60 Hz display.
`RECOMP_PRESENT_LINK_SLACK` (ms, 0 to 30) sets a fixed slack instead; a slack
is never taken closer than 2 ms to the refresh.

If the link names no refresh for 3 s while the window can be seen (hidden or
minimised time does not count), the link goes and the window's layer is made
again, as for a live switch, and a new link is made on it; if that one names
nothing for 3 s either, `aimed` takes over on a new layer for the session
(both logged). With diagnostic logging a `[PRESENT] display link` line every
10 s gives the callbacks, presents, empty refreshes, misses, the
callback-to-deadline and deadline-to-glass times, the slack in use and the
phase.

A frame that was in time for the refresh before (its vblank before that
refresh's take-until) is not put into this one: presented, it would sit a
refresh behind its vblank, and so would every frame after it, each taking the
refresh the next was meant for (seen at 120 Hz: 29 ms from vblank to glass for
minutes). It waits for the newer frame instead, until the take-until, and is
dropped when that comes, shown when it does not. A redraw (an expose, a
window resize, the overlay) waits for the take-until the same way, so it no
longer takes the refresh's drawable from the title's frame due a few ms later.

Measured (MacBook Air M5, macOS 26.7.1, PG32UCDP 4K "looks like 1920x1080",
120 Hz fixed, the release's out-of-box settings: 16:9, render scale 3, SMAA;
2026-10-30, Burnout 3):

| Case | On-glass intervals | Vblank to glass p50 |
|---|---|---|
| menus, link at 60, fullscreen | 1515 of 1515 at 2 refreshes, 0.00 % uneven | 20.7 ms, flat |
| menus, link at 60, window | 0.81 % uneven | 20.7 ms |
| race, High refresh at 120, fullscreen | 4602 of 4702 at 1 refresh (the rest the title's own 60 fps), 0.04 % uneven, none skipped | 20.7 ms, flat |

The callback came 8.3 ms before its commit deadline and the frame was on the
glass 16.67 ms after the deadline, at 60 and at 120 Hz alike (latency 1).
Press to glass in the pause menu at 120 Hz: p50 41.2 ms, p95 49.1 (the title's
own share to its vblank 22 ms, the host's 18.8). `aimed` at 60 Hz: 21.7 ms
vblank to glass.

`legacy` keeps two drawables only in a window on a fixed 60 Hz display; in
fullscreen, above 60 Hz and on a variable display it gets a third
(`RECOMP_PRESENT_DRAWABLES` decides when set): there a drawable came back
only once the next frame was on the glass, which held two drawables to every
other refresh (30 fps on a 60 Hz external display, about 96 of 120 on a
ProMotion panel, #42). `aimed`'s lead on a variable display is never raised
past two frames in flight less 3 ms (13.7 ms at 120 fps): beyond it its own
skip rule starved the presenter.

## What `aimed` does

The display the window is on is looked at twice a second (the window moved,
the mode changed, the setting changed): its rate from `NSScreen`
(`minimumRefreshInterval`, else the `CGDisplayMode`), and whether it is
variable (`maximumRefreshInterval` longer than the minimum: an external
adaptive-sync display with the option on, a ProMotion panel).

| Display | Strategy | The emulated vblank | A frame is presented |
|---|---|---|---|
| fixed, a multiple of 60 (60, 120, 240 Hz; 59.94 and 119.88 count) | `lock` | moved onto the display's grid, 5 ms before a refresh (`RECOMP_PRESENT_PHASE`) | for one named refresh: the one after the refresh its vblank sits before (`presentDrawable:atTime:`, handed over 3 ms after the refresh before it) |
| variable (VRR, ProMotion) | `timed` | its own 60.000 Hz timer | at its vblank + 12 ms (`RECOMP_PRESENT_LEAD`) |
| fixed, not a multiple of 60 (75, 100, 144 Hz) | `timed` | its own timer | at the first refresh at or after vblank + 12 ms; cannot be even, and the overlay says so |
| any, `legacy` | `legacy` | its own timer (or `RECOMP_VBLANK_SYNC=display`) | when ready, two drawables |

Common to the three new ones: three drawables and never a wait for one. A new
frame is presented only while at most one earlier present has not reached the
glass yet; otherwise it is skipped (counted, shown in the overlay). That is
the rule that brings the picture back in step after the compositor missed a
refresh, where `legacy` kept the extra frame of latency for good. On a locked
display a missed refresh costs three frames: the frame shown a refresh late
holds the refresh the next frame was aimed at, so that one is shown a refresh
late as well ("forced": counted apart, not a slip of its own, since one
present per refresh leaves it no other refresh), and the frame after that is
skipped. Half the late frames in a trace are therefore the presenter's own
recovery; the slips the pacer counts (and raises the depth on) are the misses.
`RECOMP_PRESENT_ATTIME=0` hands a locked display's frames over at once
instead of at their time (the target is still accounted and traced): a test
of whether the time itself delays frames on a system. The title's
timing is not touched by any of it: the hold at FLIP_STALL and the swap
counters are as they were, and `power.mode` works as before.

**The lock's clock is the glass.** Each drawable's `presentedTime` is a point
on the display's grid. The presenter reports it, less the phase, to
`xbox_VblankDisplayRefresh` with the display's stated period, and the vblank
timer snaps its edges to that grid (the mechanism the Vulkan build uses on the
Steam Deck). No CVDisplayLink: on an external 120 Hz display its timestamps
were irregular (BO-77). If the on-glass times stop (the window is covered) the
timer runs free again after 250 ms; if they are not on a grid after all (a
variable display that macOS reports as fixed) the kernel's irregular-step
check stands the lock down.

**Why one refresh and not zero.** In a window the compositor needs a whole
refresh: a frame committed 10 ms before a refresh is not shown at it
(measured, 60 Hz). So the floor in a window is the phase plus one refresh:
21.7 ms at 60 Hz, 13.3 ms at 120 Hz. When frames still miss their refresh
(four in two seconds), the pacer puts one more refresh between vblank and
glass, but only while that keeps a frame inside 30 ms: at 60 Hz it never does,
because a second refresh would be two frames of latency and the skip rule
would then drop every third frame.

**When a frame is handed over (BO-239).** `presentDrawable:atTime:` shows
a frame at the first refresh at or after the time it is given. Until 0.2.1
that time was half a refresh before the frame's refresh (8.3 ms at 60 Hz),
the point farthest from both refreshes. On a fullscreen external 60 Hz
display (MacBook Pro M5 Max, macOS 27, LG HDR 4K) a quarter of the frames
then missed their refresh with the commit timing identical for the frames on
time and the late ones, which points at a compositor (direct scanout) flip
deadline near that point. The time is now 3 ms after the refresh before the
frame's own (`RECOMP_PRESENT_HANDOVER`, ms, 0.5 to 16, at most half a refresh
is used): 13.7 ms ahead at 60 Hz, 5.3 ms at 120 Hz, half a refresh (2.1 ms)
at 240 Hz. The pacer's grid (the last on-glass time) was off by at most
1.2 ms in the traces, so the time still names the frame's own refresh
(`tests/present_cadence`, "handover"). Nothing moves on the glass: the target
refresh is the same, only the hand-over is earlier. Should a system take the
time as "the nearest refresh" and show frames a refresh early, three such
frames put the hand-over back at half a refresh for the session
(`[PRESENT] timing: frames reached the glass a refresh early`).

**Rejected before BO-236:** `CAMetalDisplayLink` pinned to 60 (macOS 14+),
measured with a stand-alone probe on a 120 Hz display: 25 ms from callback
to glass at `preferredFrameLatency` 1 (41.6 ms at 2) and no steadier (4.5 to
9% of the intervals off) than a plain present in a good phase. The probe
presented whatever frame it had at the callback; the presenter above moves
the title's vblank onto the link's deadlines instead, so the frame is
finished just before the callback that takes it (callback to glass is still
about 25 ms at 60, but the vblank sits 4 ms before the deadline, not a
callback's worth).

## What the overlay shows

The performance overlay's graph has the flip intervals as bars, as before,
and over them a line: the on-glass interval of the same frames on the same
scale. Even frames draw it flat on the 60 Hz mark; a 1-3-1-3 repeat is a saw,
with a red dot on every uneven interval. Text:

    60.0 fps   16.6 ms
    p50 16.7  p99 16.9  max 17.2 ms   window 60/s
    on glass 16.7 ms = 2.0 refr   120 Hz fixed   uneven 0   lag 13 ms
    ! uneven on the glass: 9 of 250 frames          (only when there is something to warn of)

"uneven" counts, over the ring (about four seconds), the on-glass intervals
that differ from the title's own spacing of the two frames by more than a
quarter of a refresh (1.5 ms on a variable display), and the frames that were
never shown between two shown ones. Content at 30 fps shown four refreshes
each is even. "lag" is the median time from a frame's emulated vblank to the
glass. The warning line appears when 2% or more are uneven, or when the
display's rate is not a multiple of 60. When the system gives no on-glass
times (the window is covered or minimised, or the backend cannot tell) the
line says "not available" and no trace is drawn: a flat line there would
claim "even". The System page has the same in words, with the counts since
the start, and "Graph: simulation steps" (`advanced.perf_steps`) marks the
frames in which the title ran more than one simulation step, when the port
reports them (`xbox_PresentCadenceSteps`).

## Traces

`RECOMP_TRACE=glass=<file>`: one CSV line per presented frame:

    flip,vblank_ns,commit_ns,at_ns,target_ns,glass_ns,late,skipped,steps,strategy,depth,refresh_hz,vrr,acq_ns,sched_ns,gpu_start_ns,gpu_end_ns

(`glass_ns` 0: the system never showed it; `strategy` 0 legacy, 1 lock,
2 timed, 3 timed on an odd rate. The last four, since 0.2.1: how long
`nextDrawable` took, and the present command buffer's scheduled, GPU start
and GPU end times on the same clock as the others; 0 where not known. They
tell a GPU that finished late from a drawable that came back late and from a
system that showed a finished frame a refresh late, which `glass_ns` alone
cannot: #42.) The port's `tools/perf/glass.py` prints the
on-glass interval histogram in refreshes, the uneven count, vblank-to-glass
and the vblank interval from it. `RECOMP_TRACE=latency` has the same on-glass
time in its `D` rows, so `tools/perf/latency.py` gives press-to-glass.

A measured run needs the window visible: a covered window reports no times.
The log says so (`[PRESENT] window: not visible ...`), and
`RECOMP_DEBUG=window_float=1` keeps the window above the others.

## Other backends

Only the Metal presenter is changed. On Vulkan (Linux / Steam Deck, Windows,
Android, MoltenVK) presentation is as it was and the overlay's on-glass line
says "not available". A measurement from `VK_KHR_present_wait` (the time the
wait for a present id returns) is written but has run nowhere yet; it is kept
on the branch `bo3/glass-vulkan` until it has been built and run on those
hosts. Android's loader swapchain has no present wait: `VK_GOOGLE_display_timing`
is the candidate there.

Can "present when ready" judder happen there? On a fixed-rate display the
Vulkan build already locks the vblank to the display (`RECOMP_VBLANK_SYNC=
display`, from the present-wait times) and FIFO shows one frame per refresh at
60 Hz, so the phase is constant; at 120 Hz and above, FIFO shows a frame at
the first refresh after the present, and a present that straddles a refresh
gives the same 1-3 repeat (the lock keeps the phase constant, but nothing
chooses it). Under variable refresh (G-Sync/FreeSync desktop, gamescope with
an adaptive-sync display) the display follows the present, so present-time
jitter is on the glass exactly as on the Mac, and the lock's input (the
present-wait times) then follows the title's own presents instead of a grid.
The Deck's own panel is fixed-rate, so the tuned Deck path is not affected.
This is reasoning from the code, not a measurement. Proposed follow-up: the
same pacer on Vulkan, with the present call itself made at the vblank plus a
lead on a variable display (FIFO shows it at once there),
`VK_EXT_present_timing` for a target time and a real on-glass stamp where the
driver has it, `VK_GOOGLE_display_timing` on Android, and the
variable-refresh state from the swapchain or the compositor.

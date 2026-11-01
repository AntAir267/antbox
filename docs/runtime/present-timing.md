# Present timing: when frames reach the glass

`display.present_timing` (`RECOMP_PRESENT_TIMING`): `legacy` or `auto`.
Code: `src/kernel/present_cadence.{h,c}` (the arithmetic, tested by
`tests/present_cadence`), `src/video/fb_present_metal.m` (the Metal
presenter).

The default is `auto` on macOS (BO-239: with `legacy`'s two drawables a
fullscreen 60 Hz external display showed every other frame for one user) and
`legacy`, what the presenters did before, elsewhere; only the Metal presenter
reads the setting. Measured numbers for
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

## What `auto` does

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
refresh, where `legacy` kept the extra frame of latency for good. The title's
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

**Rejected:** `CAMetalDisplayLink` pinned to 60 (macOS 14+). Measured with a
stand-alone probe on a 120 Hz display: 25 ms from callback to glass at
`preferredFrameLatency` 1 (41.6 ms at 2) and no steadier (4.5 to 9% of the
intervals off) than a plain present in a good phase.

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
claim "even". The Advanced page has the same in words, with the counts since
the start, and "Graph: simulation steps" (`advanced.perf_steps`) marks the
frames in which the title ran more than one simulation step, when the port
reports them (`xbox_PresentCadenceSteps`).

## Traces

`RECOMP_TRACE=glass=<file>`: one CSV line per presented frame:

    flip,vblank_ns,commit_ns,at_ns,target_ns,glass_ns,late,skipped,steps,strategy,depth,refresh_hz,vrr

(`glass_ns` 0: the system never showed it; `strategy` 0 legacy, 1 lock,
2 timed, 3 timed on an odd rate.) The port's `tools/perf/glass.py` prints the
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

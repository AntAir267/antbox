/*
 * frames_lost_rule.h - when the counters line (fb_present_sdl.c, BO-210)
 * says "frames lost after the flip". Header-only, pure, for the tests.
 *
 * Lost: over a whole visible window the frames that reached the display
 * (the fewer of new presents and present_wait's shown) are more than 3 a
 * second below the title's flips, at 20 flips a second or more. Not while
 * the window system's refresh is slower than the title's frame: a frame
 * limit the player set (gamescope's 40 or 45 under a 60 Hz title) shows
 * fewer frames than are flipped by design, and the line would come every
 * minute for as long as the limit is on. Such a window is still counted in
 * the 10 s diagnostic line.
 */
#ifndef XBOX_FRAMES_LOST_RULE_H
#define XBOX_FRAMES_LOST_RULE_H

#include <stdint.h>

/* 1 while the stated refresh is more than 5 % longer than a title frame. */
static inline int flr_limited(double flips_per_s, uint64_t wsi_cycle_ns)
{
    return wsi_cycle_ns && flips_per_s > 0
           && (double)wsi_cycle_ns > 1.05e9 / flips_per_s;
}

/* 1: say "frames lost" for this window. */
static inline int flr_lost(double flips_per_s, double reached_per_s, uint64_t wsi_cycle_ns,
                           int hidden)
{
    return !hidden && flips_per_s >= 20 && reached_per_s < flips_per_s - 3
           && !flr_limited(flips_per_s, wsi_cycle_ns);
}

#endif /* XBOX_FRAMES_LOST_RULE_H */

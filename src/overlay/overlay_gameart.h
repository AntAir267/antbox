/*
 * overlay_gameart.h - the title's own menu pictures for the overlay (the
 * arrows and the cursor, the rounded corners of the value boxes, the tick
 * bar, the d-pad, the grid, the big arc, the button prompts), read at run
 * time from the player's copy of the title's data (xbox_settings.h,
 * xbox_OverlaySetGameArt), never from this repository.
 *
 * The sprites are small DXT images. A thread reads the file, checks its
 * SHA-256, decodes them, makes them 4x larger with Super-xBR and packs them
 * into two RGBA textures (one for the sprites, one for the prompt sheets of
 * every glyph set); nothing is written anywhere. Between two frames the main
 * thread hands the textures to Dear ImGui (gameart_attach), whose backends
 * upload them like the font atlas. Until then, and for good when the file is
 * missing or is not the expected one, nothing is drawn from here and the
 * overlay keeps the look it has without them.
 */
#ifndef OVERLAY_GAMEART_H
#define OVERLAY_GAMEART_H

#include "imgui.h"

/* Sprites, in the texture of the sprites. */
enum {
    AS_ARROW_UP,        /* the cursor: an up triangle (the title draws it before a menu item) */
    AS_ARROW_LEFT,      /* a triangle pointing left; right is its mirror */
    AS_TICK,            /* one tick of the bar (4 x 8 texels in a 6 x 10 cell) */
    AS_DPAD_DOWN,       /* the d-pad: a triangle pointing down ... */
    AS_DPAD_RIGHT,      /* ... and one pointing right; up and left are their mirrors */
    AS_FILL16,          /* a box's corner as a filled shape, white (tint it), small */
    AS_EDGE16,          /* its outline alone, white */
    AS_FILL32,          /* the same, larger */
    AS_EDGE32,
    AS_GRID,            /* a line along the top and the left, white; tiles */
    AS_BIGCURVE,        /* the arc, in its own colours */
    /* From xbox_OverlaySetGameArtMore; each may be missing on its own. */
    AS_VOL_FULL,        /* a full cell of the volume bar (5 x 15 and its shadow, 6 x 16), white */
    AS_VOL_EMPTY,       /* an empty one: a blue outline, the same box */
    AS_CHECK,           /* the tick in its blue disc, in its own colours */
    AS_RING,            /* a white ring */
    AS_PADLOCK,         /* the padlock, in its own colours */
    AS_COUNT
};

/* Where a picture is in its texture (fractions) and how large it is in the
 * title's own texels (for its proportions). */
struct ArtRect {
    float u0, v0, u1, v1;
    float w, h;
};

/* The prompt cells of a sheet. LB and RB are the pad's shoulder buttons
 * (the title's white and black ones, or L1 / R1 / LB; the original Xbox's
 * own sheet has the L and R triggers where they are drawn). */
enum { AP_B, AP_A, AP_X, AP_Y, AP_LB, AP_RB, AP_COUNT };
#define AP_SETS 15             /* the glyph sets of pad_glyphs.h (XBOX_GLYPH_SET_COUNT) */

/* 1 the pictures are ready (or in use), -1 there are none (nothing named,
 * another file, off), 0 still being made. */
int gameart_state();

/* Main thread, between frames: hands the finished textures to Dear ImGui,
 * once. True when the pictures can be drawn. */
bool gameart_attach();

const ArtRect *gameart_sprite(int sprite);                    /* NULL until attached, or when missing */
ImTextureRef gameart_tex();
/* The cell of `set` (xbox_glyph_set), false when there is no such picture. */
bool gameart_pad_cell(int set, int cell, ArtRect *out);
ImTextureRef gameart_pad_tex();

#endif

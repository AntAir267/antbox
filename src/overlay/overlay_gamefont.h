/*
 * overlay_gamefont.h - the title's own bitmap fonts for the overlay, read at
 * run time from the player's copy of the executable (overlay.h,
 * xbox_OverlaySetGameFonts), never from this repository.
 *
 * A font is a DXT5 atlas with a glyph table (where each letter is in it, its
 * offset and advance). It is decoded and made 4x larger with Super-xBR
 * (nv2a_texscale.h) on a thread of its own when the port registers it; the
 * overlay then draws it at any size through a Dear ImGui font loader that
 * samples that picture, so a 4K window gets letters as sharp as the scaler
 * makes them. Until then, and for good when the file is missing or is not
 * the expected one (its SHA-256), the overlay draws with its own font only.
 */
#ifndef OVERLAY_GAMEFONT_H
#define OVERLAY_GAMEFONT_H

#include <stdint.h>
#include <unordered_map>
#include <vector>

struct ImFontLoader;

enum { GF_BODY = 0, GF_TITLE = 1, GF_COUNT };

struct GfGlyph {
    float u, v, w, h;        /* in the atlas, fractions of its size */
    float xo, yo, adv;       /* offset from the cell's top left, advance: fractions of the atlas */
};

struct GameFont {
    int tw, th;                          /* the atlas as stored */
    int k;                               /* the factor of the picture below */
    std::vector<uint8_t> a;              /* coverage, (tw * k) x (th * k) */
    std::unordered_map<uint32_t, GfGlyph> g;
    float ref;                           /* the cell: the tallest glyph, in texels */
    float base;                          /* the baseline ('H''s foot), a fraction of the cell */
    float caps;                          /* the capitals' height, a fraction of the cell */
};

/* 1 both fonts are ready, -1 there are none (no file, another file, off),
 * 0 still being made. */
int gamefont_state();
const GameFont *gamefont(int which);

/* Shared with the game's pictures (overlay_gameart.cpp): the file whole when
 * it is `size` bytes with this SHA-256 (lower-case hex), else NULL and why;
 * a DXT5 (BC3) image to A8R8G8B8 with straight alpha. */
uint8_t *gf_read_verified(const char *path, uint64_t size, const char *sha_hex, const char **why);
void gf_dxt5_decode(const uint8_t *src, int w, int h, uint32_t *dst);

/* The loader: ImFontConfig.FontData is the GameFont, FontDataSize 1,
 * FontDataOwnedByAtlas false. Its size is the cell's height; a merged one
 * (ExtraSizeScale) sits on the first one's baseline. */
const ImFontLoader *gamefont_loader();

#endif

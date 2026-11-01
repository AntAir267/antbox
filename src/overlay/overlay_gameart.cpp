/*
 * overlay_gameart.cpp - the title's own menu pictures, from the player's
 * install (overlay_gameart.h, xbox_settings.h).
 *
 * Nothing decoded here is written anywhere: the file is read, checked by its
 * SHA-256 against the one the port names, the sprites are decoded and scaled
 * in memory, and that is all. Any failure (no file, another size or hash, a
 * sprite that is not where or what the port said) leaves the overlay on the
 * look it has without them, with a line in the log and nothing on screen.
 */
#include "xbox_settings.h"
#include "overlay_gameart.h"
#include "pad_glyphs.h"
#include "overlay_gamefont.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "nv2a_texscale.h"
#include "nv2a_texrep.h"

#include <SDL.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define SCALE 4        /* Super-xBR's factor */
#define GUTTER 4       /* texels of an image's edge repeated around it in the atlas */

static_assert(AP_SETS == XBOX_GLYPH_SET_COUNT, "one sheet per glyph set");

namespace {

enum { AI_ARROW, AI_DPAD, AI_GRID, AI_BIG, AI_E32, AI_F32, AI_E16, AI_F16,
       AI_VOLF, AI_VOLE, AI_CHECK, AI_RING, AI_LOCK, AI_COUNT };

/* A picture of xbox_OverlaySetGameArtMore, its file named with it. */
struct More {
    bool on;
    char path[1024];
    uint64_t size;
    char sha[65];
    uint32_t off, w, h, fmt;
};

struct Job {
    char path[1024];
    uint64_t size;
    char sha[65];
    xbox_overlay_sprite spr[XBOX_OVERLAY_ART_COUNT];
    struct nv2a_texrep_variant pads[AP_SETS];
    unsigned n_pads;
    More more[XBOX_OVERLAY_ART2_COUNT];
};

struct Atlas {
    int w = 0, h = 0;
    std::vector<uint32_t> abgr;          /* RGBA bytes in memory */
};

struct Sub { int image, x, y, w, h; };
/* Where each sprite is in the title's own texels, in the image it comes from. */
const Sub k_sub[AS_COUNT] = {
    /* AS_ARROW_UP    */ { AI_ARROW, 0, 0, 21, 12 },
    /* AS_ARROW_LEFT  */ { AI_ARROW, 21, 0, 12, 20 },
    /* AS_TICK        */ { AI_ARROW, 1, 22, 6, 10 },
    /* AS_DPAD_DOWN   */ { AI_DPAD, 0, 0, 16, 13 },
    /* AS_DPAD_RIGHT  */ { AI_DPAD, 18, 0, 12, 16 },
    /* AS_FILL16      */ { AI_F16, 0, 0, 16, 16 },
    /* AS_EDGE16      */ { AI_E16, 0, 0, 16, 16 },
    /* AS_FILL32      */ { AI_F32, 0, 0, 32, 32 },
    /* AS_EDGE32      */ { AI_E32, 0, 0, 32, 32 },
    /* AS_GRID        */ { AI_GRID, 0, 0, 16, 16 },
    /* AS_BIGCURVE    */ { AI_BIG, 0, 0, 128, 128 },
    /* AS_VOL_FULL    */ { AI_VOLF, 0, 0, 6, 16 },
    /* AS_VOL_EMPTY   */ { AI_VOLE, 0, 0, 6, 16 },
    /* AS_CHECK       */ { AI_CHECK, 0, 0, 64, 64 },
    /* AS_RING        */ { AI_RING, 0, 0, 32, 32 },
    /* AS_PADLOCK     */ { AI_LOCK, 0, 0, 32, 32 },
};

/* The prompt sheet (128 x 64): the rectangles of its cells. The discs sit
 * 28 wide from (1, 1) with a transparent texel around them. */
struct Cell { int x, y, w, h; };
const Cell k_cell[AP_COUNT] = {
    { 0, 0, 30, 30 },       /* B */
    { 30, 0, 30, 30 },      /* A */
    { 60, 0, 30, 30 },      /* X */
    { 90, 0, 30, 30 },      /* Y */
    { 1, 37, 28, 20 },      /* LB: the white button's place (a key 26 x 17 for the pads that print L1 or LB) */
    { 31, 37, 28, 20 },     /* RB: the black button's */
};
const Cell k_cell_trigger_l = { 62, 29, 18, 18 };    /* the original pad's L and R triggers */
const Cell k_cell_trigger_r = { 62, 46, 18, 18 };

const int k_expect[XBOX_OVERLAY_ART_COUNT][2] = {
    { 64, 32 }, { 128, 64 }, { 32, 16 }, { 16, 16 }, { 128, 128 }, { 32, 32 }, { 32, 32 }, { 16, 16 }, { 16, 16 },
};
const int k_expect_more[XBOX_OVERLAY_ART2_COUNT][2] = { { 128, 32 }, { 64, 64 }, { 32, 32 }, { 32, 32 } };

Job s_job;
More s_more[XBOX_OVERLAY_ART2_COUNT];      /* named before xbox_OverlaySetGameArt */
bool s_have[AS_COUNT];
std::atomic<int> s_state{-1};              /* -1 none, 0 loading, 1 made */
SDL_Thread *s_thread;
Atlas s_sprites, s_pads;
ArtRect s_rect[AS_COUNT];
ArtRect s_cell[AP_SETS][AP_COUNT];
bool s_attached;
ImTextureData *s_tex_sprites, *s_tex_pads;
struct nv2a_texrep_variant s_pad_variants[AP_SETS];
unsigned s_n_pad_variants;

/* ── decoding ──────────────────────────────────────────────────────── */

void c565(uint16_t c, uint8_t out[3])
{
    out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* DXT1 (BC1), blocks in rows, to A8R8G8B8: a block with c0 <= c1 has one
 * transparent colour. */
void dxt1_decode(const uint8_t *src, int w, int h, uint32_t *dst)
{
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, src += 8) {
            const uint16_t c0 = (uint16_t)(src[0] | src[1] << 8), c1 = (uint16_t)(src[2] | src[3] << 8);
            const uint32_t bits = (uint32_t)src[4] | (uint32_t)src[5] << 8 | (uint32_t)src[6] << 16
                                | (uint32_t)src[7] << 24;
            uint8_t c[4][3];
            uint8_t al[4] = { 255, 255, 255, 255 };
            c565(c0, c[0]);
            c565(c1, c[1]);
            for (int k = 0; k < 3; k++) {
                if (c0 > c1) {
                    c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
                    c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
                } else {
                    c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
                    c[3][k] = 0;
                }
            }
            if (c0 <= c1)
                al[3] = 0;
            for (int i = 0; i < 16; i++) {
                const int x = bx + (i & 3), y = by + (i >> 2), k = (bits >> (2 * i)) & 3;
                dst[y * w + x] = (uint32_t)al[k] << 24 | (uint32_t)c[k][0] << 16 | (uint32_t)c[k][1] << 8 | c[k][2];
            }
        }
}

typedef std::vector<uint32_t> Img;       /* A8R8G8B8 */

/* An 8-bit palette image, the Xbox's swizzle (the bits of x and y
 * interleaved from the lowest while each still has some), to A8R8G8B8. The
 * palette's resource header (0x40 bytes) and its 256 B, G, R, A colours
 * follow the pixels. */
bool p8_decode(const uint8_t *file, uint64_t size, uint32_t off, int w, int h, uint32_t *dst)
{
    const uint64_t head = (uint64_t)off + (uint64_t)w * h, pal = head + 0x40;
    if (pal + 1024 > size)
        return false;
    const uint32_t ph = (uint32_t)file[head] | (uint32_t)file[head + 1] << 8 | (uint32_t)file[head + 2] << 16;
    if (ph != 0x030001u)                     /* a palette resource, of 256 colours */
        return false;
    uint32_t xs[256], ys[256], xbit[8], ybit[8];
    int nx = 0, ny = 0;
    uint32_t bit = 1;
    for (int mx = 1, my = 1; mx < w || my < h;) {
        if (mx < w) { xbit[nx++] = bit; bit <<= 1; mx <<= 1; }
        if (my < h) { ybit[ny++] = bit; bit <<= 1; my <<= 1; }
    }
    for (int x = 0; x < w; x++) {
        xs[x] = 0;
        for (int i = 0; i < nx; i++)
            if (x >> i & 1) xs[x] |= xbit[i];
    }
    for (int y = 0; y < h; y++) {
        ys[y] = 0;
        for (int i = 0; i < ny; i++)
            if (y >> i & 1) ys[y] |= ybit[i];
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *c = file + pal + 4 * file[off + xs[x] + ys[y]];
            dst[y * w + x] = (uint32_t)c[3] << 24 | (uint32_t)c[2] << 16 | (uint32_t)c[1] << 8 | c[0];
        }
    return true;
}

/* The `w` x `h` texels of `src` (sw wide) from (x, y). */
Img crop(const Img &src, int sw, int x, int y, int w, int h)
{
    Img out((size_t)w * h);
    for (int j = 0; j < h; j++)
        memcpy(&out[(size_t)j * w], &src[(size_t)(y + j) * sw + x], w * sizeof(uint32_t));
    return out;
}

/* The pictures of xbox_OverlaySetGameArtMore: each file read and checked
 * once; one that fails costs its own pictures only. */
void more_load(Img raw[XBOX_OVERLAY_ART2_COUNT])
{
    for (int i = 0; i < XBOX_OVERLAY_ART2_COUNT; i++) {
        const More &m = s_job.more[i];
        if (!m.on || !raw[i].empty())
            continue;
        const char *why = NULL;
        uint8_t *file = gf_read_verified(m.path, m.size, m.sha, &why);
        /* Every picture of this file, from the one read. */
        for (int j = i; j < XBOX_OVERLAY_ART2_COUNT; j++) {
            const More &d = s_job.more[j];
            if (!d.on || strcmp(d.path, m.path) || d.size != m.size || strcmp(d.sha, m.sha))
                continue;
            const char *bad = why;
            if (!bad && ((int)d.w != k_expect_more[j][0] || (int)d.h != k_expect_more[j][1]))
                bad = "another size of picture";
            Img img((size_t)d.w * d.h);
            if (!bad) {
                const uint64_t bytes = (uint64_t)d.w * d.h / (d.fmt == 1 ? 2 : 1);
                if ((d.fmt != 1 && d.fmt != 5 && d.fmt != 8) || (uint64_t)d.off + bytes > m.size)
                    bad = "a picture is not where it was said to be";
                else if (d.fmt == 8) {
                    if (!p8_decode(file, m.size, d.off, (int)d.w, (int)d.h, img.data()))
                        bad = "a picture is not where it was said to be";
                } else if (d.fmt == 5)
                    gf_dxt5_decode(file + d.off, (int)d.w, (int)d.h, img.data());
                else
                    dxt1_decode(file + d.off, (int)d.w, (int)d.h, img.data());
            }
            if (bad) {
                fprintf(stderr, "[overlay] a picture of the game's is not used (%d): %s: %s\n", j, d.path, bad);
                raw[j].clear();
            } else
                raw[j] = img;
            if (j != i && raw[j].empty())
                s_job.more[j].on = false;      /* not again */
        }
        if (raw[i].empty())
            s_job.more[i].on = false;
        free(file);
    }
}

uint32_t abgr_of(uint32_t p)
{
    return (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
}

/* `img` (w x h) made SCALE times larger, straight alpha; empty when the
 * scaler refuses. */
Img scaled(const Img &img, int w, int h)
{
    Img big((size_t)w * h * SCALE * SCALE);
    if (!nv2a_texscale(img.data(), (uint32_t)w, (uint32_t)h, SCALE, NV2A_TEXSCALE_SUPERXBR, big.data()))
        big.clear();
    return big;
}

/* Puts `src` (w x h, A8R8G8B8) into the atlas at (x, y), with GUTTER texels
 * of its edge around it (wrapped for a tile, else repeated). */
void blit(Atlas &a, int x, int y, const Img &src, int w, int h, bool wrap)
{
    for (int dy = -GUTTER; dy < h + GUTTER; dy++)
        for (int dx = -GUTTER; dx < w + GUTTER; dx++) {
            int sx = dx, sy = dy;
            if (wrap) {
                sx = ((sx % w) + w) % w;
                sy = ((sy % h) + h) % h;
            } else {
                sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
                sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
            }
            a.abgr[(size_t)(y + dy) * a.w + (x + dx)] = abgr_of(src[(size_t)sy * w + sx]);
        }
}

/* The sheet of one glyph set: the title's own, with the port's drawings over
 * the rectangles the variant names. */
bool pad_sheet(const Img &base, int set, Img &out)
{
    out = base;
    if (set < 1 || (unsigned)(set - 1) >= s_job.n_pads)
        return true;
    const struct nv2a_texrep_variant &v = s_job.pads[set - 1];
    if (!v.image || !v.rects)
        return true;
    for (unsigned i = 0; i < v.n_rects; i++) {
        const struct nv2a_texrep_rect &r = v.rects[i];
        if ((int)r.x + r.w > 128 || (int)r.y + r.h > 64)
            return false;
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(&out[(size_t)y * 128 + r.x], &v.image[(size_t)y * 128 + r.x], r.w * sizeof(uint32_t));
    }
    return true;
}

bool make(const uint8_t *file, uint64_t size)
{
    Img raw[XBOX_OVERLAY_ART_COUNT];
    for (int i = 0; i < XBOX_OVERLAY_ART_COUNT; i++) {
        const xbox_overlay_sprite &d = s_job.spr[i];
        if ((int)d.w != k_expect[i][0] || (int)d.h != k_expect[i][1] || (d.dxt != 1 && d.dxt != 5))
            return false;
        const uint64_t bytes = (uint64_t)d.w * d.h / (d.dxt == 1 ? 2 : 1);
        if ((uint64_t)d.off + bytes > size)
            return false;
        raw[i].resize((size_t)d.w * d.h);
        if (d.dxt == 5)
            gf_dxt5_decode(file + d.off, (int)d.w, (int)d.h, raw[i].data());
        else
            dxt1_decode(file + d.off, (int)d.w, (int)d.h, raw[i].data());
    }

    /* The atlas's images: the title's own pictures, or masks made from them
     * (a box's outline is the white of its corner, its fill the shape). */
    Img im[AI_COUNT];
    int iw[AI_COUNT], ih[AI_COUNT];
    auto take = [&](int ai, int art) {
        im[ai] = raw[art];
        iw[ai] = k_expect[art][0];
        ih[ai] = k_expect[art][1];
    };
    take(AI_ARROW, XBOX_OVERLAY_ART_ARROW);
    take(AI_DPAD, XBOX_OVERLAY_ART_DPAD);
    take(AI_GRID, XBOX_OVERLAY_ART_GRID);
    take(AI_BIG, XBOX_OVERLAY_ART_BIG_CURVE);
    take(AI_E32, XBOX_OVERLAY_ART_BOX_CURVE);
    take(AI_F32, XBOX_OVERLAY_ART_BOX_CURVE_B);
    take(AI_E16, XBOX_OVERLAY_ART_CURVE16);
    take(AI_F16, XBOX_OVERLAY_ART_CURVE16_B);
    {
        /* The volume bar's first cells, full and empty, cut apart: the
         * scaler blends neighbours, and the two halves touch. */
        Img more[XBOX_OVERLAY_ART2_COUNT];
        more_load(more);
        auto take_more = [&](int ai, int a2, int x, int y, int w, int h) {
            if (more[a2].empty())
                return;
            im[ai] = crop(more[a2], k_expect_more[a2][0], x, y, w, h);
            iw[ai] = w;
            ih[ai] = h;
        };
        for (int ai = AI_VOLF; ai < AI_COUNT; ai++)
            iw[ai] = ih[ai] = 0;
        take_more(AI_VOLF, XBOX_OVERLAY_ART2_VOLUMEBAR, 0, 0, 6, 16);
        take_more(AI_VOLE, XBOX_OVERLAY_ART2_VOLUMEBAR, 0, 16, 6, 16);
        take_more(AI_CHECK, XBOX_OVERLAY_ART2_TICK, 0, 0, 64, 64);
        take_more(AI_RING, XBOX_OVERLAY_ART2_CIRCLE, 0, 0, 32, 32);
        take_more(AI_LOCK, XBOX_OVERLAY_ART2_PADLOCK, 0, 0, 32, 32);
    }
    for (int ai : { AI_E32, AI_E16 })
        for (uint32_t &p : im[ai]) {
            /* White outline, and a shade of 50% black inside: the outline is
             * what is both light and opaque. */
            const uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            const uint32_t lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
            p = (a * lum / 255) << 24 | 0x00ffffffu;
        }
    /* The arc keeps its blue and its soft black shadow; the dark navy wash the
     * picture lays over its whole area (it ends in a visible edge on a
     * screen the arc does not span) is dropped. */
    for (uint32_t &p : im[AI_BIG]) {
        const uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        const uint32_t lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (lum >= 4 && lum < 0x30)
            p &= 0x00ffffffu;
    }
    for (int ai : { AI_F32, AI_F16, AI_GRID })
        for (uint32_t &p : im[ai])
            p |= 0x00ffffffu;                    /* the shape's alpha is all there is */

    /* Pack the scaled images in rows of at most 1024 texels. */
    int px[AI_COUNT], py[AI_COUNT];
    Img big[AI_COUNT];
    int x = 0, y = 0, rowh = 0, width = 0;
    for (int ai = 0; ai < AI_COUNT; ai++) {
        if (!iw[ai])
            continue;                            /* a picture of the more that is missing */
        big[ai] = scaled(im[ai], iw[ai], ih[ai]);
        if (big[ai].empty())
            return false;
        const int w = iw[ai] * SCALE + 2 * GUTTER, h = ih[ai] * SCALE + 2 * GUTTER;
        if (x + w > 1024) {
            x = 0;
            y += rowh;
            rowh = 0;
        }
        px[ai] = x + GUTTER;
        py[ai] = y + GUTTER;
        x += w;
        if (x > width) width = x;
        if (h > rowh) rowh = h;
    }
    s_sprites.w = width;
    s_sprites.h = y + rowh;
    s_sprites.abgr.assign((size_t)s_sprites.w * s_sprites.h, 0);
    for (int ai = 0; ai < AI_COUNT; ai++)
        if (iw[ai])
            blit(s_sprites, px[ai], py[ai], big[ai], iw[ai] * SCALE, ih[ai] * SCALE, ai == AI_GRID);
    for (int s = 0; s < AS_COUNT; s++) {
        const Sub &u = k_sub[s];
        ArtRect &r = s_rect[s];
        s_have[s] = iw[u.image] != 0;
        if (!s_have[s])
            continue;
        r.u0 = (float)(px[u.image] + u.x * SCALE) / s_sprites.w;
        r.v0 = (float)(py[u.image] + u.y * SCALE) / s_sprites.h;
        r.u1 = (float)(px[u.image] + (u.x + u.w) * SCALE) / s_sprites.w;
        r.v1 = (float)(py[u.image] + (u.y + u.h) * SCALE) / s_sprites.h;
        r.w = (float)u.w;
        r.h = (float)u.h;
    }

    /* The prompt sheets, two to a row. */
    const int cw = 128 * SCALE + 2 * GUTTER, ch = 64 * SCALE + 2 * GUTTER;
    s_pads.w = 2 * cw;
    s_pads.h = ((AP_SETS + 1) / 2) * ch;
    s_pads.abgr.assign((size_t)s_pads.w * s_pads.h, 0);
    for (int set = 0; set < AP_SETS; set++) {
        Img sheet;
        if (!pad_sheet(raw[XBOX_OVERLAY_ART_BUTTONS], set, sheet))
            return false;
        Img b = scaled(sheet, 128, 64);
        if (b.empty())
            return false;
        const int ox = (set % 2) * cw + GUTTER, oy = (set / 2) * ch + GUTTER;
        blit(s_pads, ox, oy, b, 128 * SCALE, 64 * SCALE, false);
        for (int c = 0; c < AP_COUNT; c++) {
            Cell k = k_cell[c];
            if (set == 0 && c == AP_LB) k = k_cell_trigger_l;     /* the title's own sheet: triggers */
            if (set == 0 && c == AP_RB) k = k_cell_trigger_r;
            ArtRect &r = s_cell[set][c];
            r.u0 = (float)(ox + k.x * SCALE) / s_pads.w;
            r.v0 = (float)(oy + k.y * SCALE) / s_pads.h;
            r.u1 = (float)(ox + (k.x + k.w) * SCALE) / s_pads.w;
            r.v1 = (float)(oy + (k.y + k.h) * SCALE) / s_pads.h;
            r.w = (float)k.w;
            r.h = (float)k.h;
        }
    }
    return true;
}

int load_thread(void *)
{
    const uint32_t t0 = SDL_GetTicks();
    const char *why = NULL;
    uint8_t *file = gf_read_verified(s_job.path, s_job.size, s_job.sha, &why);
    if (file && !make(file, s_job.size))
        why = "a picture is not where it was said to be";
    free(file);
    if (why) {
        s_sprites = Atlas();
        s_pads = Atlas();
        fprintf(stderr, "[overlay] the game's pictures are not used: %s: %s\n", s_job.path, why);
        s_state.store(-1);
    } else {
        fprintf(stderr, "[overlay] the game's pictures: %u ms (%dx%d + %dx%d, x%d)\n",
                (unsigned)(SDL_GetTicks() - t0), s_sprites.w, s_sprites.h, s_pads.w, s_pads.h, SCALE);
        s_state.store(1);
    }
    return 0;
}

ImTextureData *make_texture(Atlas &a)
{
    ImTextureData *t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, a.w, a.h);
    memcpy(t->GetPixels(), a.abgr.data(), a.abgr.size() * sizeof(uint32_t));
    t->UseColors = true;
    ImGui::RegisterUserTexture(t);
    a.abgr.clear();
    a.abgr.shrink_to_fit();
    return t;
}

} // namespace

extern "C" void xbox_OverlaySetPadSheets(const struct nv2a_texrep_variant *variants, unsigned n)
{
    s_n_pad_variants = n < AP_SETS - 1 ? n : AP_SETS - 1;
    for (unsigned i = 0; i < s_n_pad_variants; i++)
        s_pad_variants[i] = variants[i];
}

extern "C" void xbox_OverlaySetGameArtMore(const xbox_overlay_sprite_file *sprites)
{
    if (s_thread || !sprites)
        return;
    for (int i = 0; i < XBOX_OVERLAY_ART2_COUNT; i++) {
        const xbox_overlay_sprite_file &f = sprites[i];
        More &m = s_more[i];
        m.on = f.path && f.path[0] && f.sha256_hex && strlen(f.sha256_hex) == 64;
        if (!m.on)
            continue;
        snprintf(m.path, sizeof m.path, "%s", f.path);
        m.size = f.size;
        for (int k = 0; k < 65; k++)
            m.sha[k] = (char)((f.sha256_hex[k] >= 'A' && f.sha256_hex[k] <= 'F') ? f.sha256_hex[k] + 32 : f.sha256_hex[k]);
        m.off = f.off;
        m.w = f.w;
        m.h = f.h;
        m.fmt = f.fmt;
    }
}

extern "C" void xbox_OverlaySetGameArt(const char *path, uint64_t size, const char *sha256_hex,
                                       const xbox_overlay_sprite *sprites)
{
    const char *e = getenv("RECOMP_OVERLAY_GAME_ART");
    if (s_thread || s_state.load() == 1 || !path || !path[0] || !sha256_hex || strlen(sha256_hex) != 64 || !sprites
        || (e && e[0] == '0'))
        return;
    snprintf(s_job.path, sizeof s_job.path, "%s", path);
    s_job.size = size;
    for (int i = 0; i < 65; i++)
        s_job.sha[i] = (char)((sha256_hex[i] >= 'A' && sha256_hex[i] <= 'F') ? sha256_hex[i] + 32 : sha256_hex[i]);
    memcpy(s_job.spr, sprites, sizeof s_job.spr);
    memcpy(s_job.more, s_more, sizeof s_job.more);
    s_job.n_pads = s_n_pad_variants;
    memcpy(s_job.pads, s_pad_variants, sizeof s_job.pads);
    s_state.store(0);
    s_thread = SDL_CreateThread(load_thread, "gameart", NULL);
    if (s_thread)
        SDL_DetachThread(s_thread);
    else
        load_thread(NULL);
}

extern "C" int xbox_OverlayGameArtState(unsigned wait_ms)
{
    const uint32_t until = SDL_GetTicks() + wait_ms;
    while (s_state.load() == 0 && (int32_t)(until - SDL_GetTicks()) > 0)
        SDL_Delay(5);
    return s_state.load();
}

int gameart_state() { return s_state.load(); }

bool gameart_attach()
{
    if (s_attached)
        return true;
    if (s_state.load() != 1)
        return false;
    s_tex_sprites = make_texture(s_sprites);
    s_tex_pads = make_texture(s_pads);
    s_attached = true;
    return true;
}

const ArtRect *gameart_sprite(int sprite)
{
    return s_attached && sprite >= 0 && sprite < AS_COUNT && s_have[sprite] ? &s_rect[sprite] : nullptr;
}

ImTextureRef gameart_tex() { return s_tex_sprites->GetTexRef(); }
ImTextureRef gameart_pad_tex() { return s_tex_pads->GetTexRef(); }

bool gameart_pad_cell(int set, int cell, ArtRect *out)
{
    if (!s_attached || set < 0 || set >= AP_SETS || cell < 0 || cell >= AP_COUNT)
        return false;
    *out = s_cell[set][cell];
    return true;
}

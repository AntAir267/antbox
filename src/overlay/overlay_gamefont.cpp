/*
 * overlay_gamefont.cpp - the title's own bitmap fonts, from the player's
 * executable (overlay_gamefont.h, overlay.h).
 *
 * Nothing decoded here is written anywhere: the file is read, checked by its
 * SHA-256 against the one the port names, the atlases are decoded and
 * scaled in memory, and that is all. Any failure (no file, another size or
 * hash, a table that does not read as glyphs) leaves the overlay on its own
 * font, with a line in the log and no message on screen.
 */
#include "xbox_settings.h"
#include "overlay_gamefont.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "nv2a_texscale.h"
#include "xr_sha256.h"

#include <SDL.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GameFont s_gf[GF_COUNT];
static std::atomic<int> s_state{-1};     /* -1 none, 0 loading, 1 ready */
static SDL_Thread *s_thread;

struct Job {
    char path[1024];
    uint64_t size;
    char sha[65];
    xbox_overlay_bitmap_font f[GF_COUNT];
};
static Job s_job;

int gamefont_state() { return s_state.load(); }
const GameFont *gamefont(int which) { return s_state.load() == 1 ? &s_gf[which] : nullptr; }

/* ── decoding ──────────────────────────────────────────────────────── */

static void rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* DXT5 (BC3), blocks in rows, to A8R8G8B8 with straight alpha. */
void gf_dxt5_decode(const uint8_t *src, int w, int h, uint32_t *dst)
{
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, src += 16) {
            uint8_t al[8];
            al[0] = src[0];
            al[1] = src[1];
            if (al[0] > al[1])
                for (int i = 0; i < 6; i++) al[2 + i] = (uint8_t)(((6 - i) * al[0] + (i + 1) * al[1]) / 7);
            else {
                for (int i = 0; i < 4; i++) al[2 + i] = (uint8_t)(((4 - i) * al[0] + (i + 1) * al[1]) / 5);
                al[6] = 0;
                al[7] = 255;
            }
            uint64_t abits = 0;
            for (int i = 0; i < 6; i++) abits |= (uint64_t)src[2 + i] << (8 * i);
            const uint16_t c0 = (uint16_t)(src[8] | src[9] << 8), c1 = (uint16_t)(src[10] | src[11] << 8);
            const uint32_t cbits = (uint32_t)src[12] | (uint32_t)src[13] << 8 | (uint32_t)src[14] << 16
                                 | (uint32_t)src[15] << 24;
            uint8_t c[4][3];
            rgb565(c0, c[0]);
            rgb565(c1, c[1]);
            for (int k = 0; k < 3; k++) {
                c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
                c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
            }
            for (int i = 0; i < 16; i++) {
                const int x = bx + (i & 3), y = by + (i >> 2);
                const uint8_t *cc = c[(cbits >> (2 * i)) & 3];
                const uint32_t a = al[(abits >> (3 * i)) & 7];
                dst[y * w + x] = a << 24 | (uint32_t)cc[0] << 16 | (uint32_t)cc[1] << 8 | cc[2];
            }
        }
}

static float rd_f32(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool font_make(const uint8_t *file, uint64_t size, const xbox_overlay_bitmap_font &d, GameFont &out)
{
    const uint64_t tex_bytes = (uint64_t)d.w * d.h;          /* DXT5: a byte a texel */
    if (d.w < 4 || d.h < 4 || d.w > 1024 || d.h > 1024 || (d.w & 3) || (d.h & 3) || d.count == 0
        || d.count > 4096 || (uint64_t)d.table_off + (uint64_t)d.count * 32 > size
        || (uint64_t)d.tex_off + tex_bytes > size)
        return false;
    out.tw = (int)d.w;
    out.th = (int)d.h;
    out.g.clear();
    float tall = 0.0f;
    for (uint32_t i = 0; i < d.count; i++) {
        const uint8_t *r = file + d.table_off + (uint64_t)i * 32;
        GfGlyph g = { rd_f32(r), rd_f32(r + 4), rd_f32(r + 8), rd_f32(r + 12),
                      rd_f32(r + 16), rd_f32(r + 20), rd_f32(r + 24) };
        const uint32_t code = rd_u32(r + 28);
        const float *f = &g.u;
        bool ok = code < 0x10000;
        for (int k = 0; k < 7; k++)
            ok = ok && isfinite(f[k]) && f[k] >= -2.0f && f[k] <= 2.0f;
        if (!ok)
            return false;                         /* not a glyph table: not the file it was said to be */
        if (code == 0 || g.w <= 0.0f || g.u < 0.0f || g.v < 0.0f || g.u + g.w > 1.001f || g.v + g.h > 1.001f)
            continue;                             /* an empty slot */
        out.g[code] = g;
        if (g.h * d.h > tall)
            tall = g.h * d.h;
    }
    auto H = out.g.find('H');
    if (out.g.size() < 26 || H == out.g.end() || tall <= 0.0f)
        return false;
    out.ref = tall;
    out.caps = H->second.h * d.h / tall;
    out.base = (H->second.yo + H->second.h) * d.h / tall;

    std::vector<uint32_t> rgba((size_t)d.w * d.h);
    gf_dxt5_decode(file + d.tex_off, (int)d.w, (int)d.h, rgba.data());
    /* The letters are white on clear: their alpha is all there is. White
     * under every texel so the scaler sees one colour. */
    for (uint32_t &p : rgba)
        p |= 0x00ffffffu;
    out.k = 4;
    std::vector<uint32_t> big((size_t)d.w * d.h * 16);
    if (!nv2a_texscale(rgba.data(), d.w, d.h, 4, NV2A_TEXSCALE_SUPERXBR, big.data())) {
        out.k = 1;
        big = rgba;
    }
    out.a.resize(big.size());
    for (size_t i = 0; i < big.size(); i++)
        out.a[i] = (uint8_t)(big[i] >> 24);
    return true;
}

/* The file whole, when it is `size` bytes with this SHA-256 (lower-case hex);
 * else NULL and the reason. Free it with free(). */
uint8_t *gf_read_verified(const char *path, uint64_t size, const char *sha_hex, const char **why_out)
{
    const char *why = NULL;
    uint8_t *file = NULL;
#if defined(__ANDROID__)
    /* SDL on Android opens a relative path in the app's internal storage (or
     * the APK's assets), not the current folder the host chdir()s to, where
     * the title's install and the port's paths are. */
    FILE *fp = fopen(path, "rb");
    SDL_RWops *rw = fp ? SDL_RWFromFP(fp, SDL_TRUE) : NULL;
    if (fp && !rw)
        fclose(fp);
#else
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
#endif
    Sint64 n = rw ? SDL_RWsize(rw) : -1;
    if (!rw)
        why = "not readable";
    else if (n <= 0 || (uint64_t)n != size)
        why = "another size";
    else if (!(file = (uint8_t *)malloc((size_t)n)) || SDL_RWread(rw, file, 1, (size_t)n) != (size_t)n)
        why = "not readable";
    if (rw)
        SDL_RWclose(rw);
    if (!why) {
        xr_sha256 h;
        uint8_t dg[32];
        char hex[65];
        xr_sha256_init(&h);
        xr_sha256_update(&h, file, (size_t)n);
        xr_sha256_final(&h, dg);
        xr_sha256_hex(dg, hex);
        if (strcmp(hex, sha_hex))
            why = "another version (SHA-256)";
    }
    if (why) {
        free(file);
        file = NULL;
    }
    *why_out = why;
    return file;
}

static int load_thread(void *)
{
    const uint32_t t0 = SDL_GetTicks();
    const char *why = NULL;
    uint8_t *file = gf_read_verified(s_job.path, s_job.size, s_job.sha, &why);
    const Sint64 n = (Sint64)s_job.size;
    for (int i = 0; !why && i < GF_COUNT; i++)
        if (!font_make(file, (uint64_t)n, s_job.f[i], s_gf[i]))
            why = "no font where expected";
    free(file);
    if (why) {
        for (GameFont &g : s_gf) { g.a.clear(); g.g.clear(); }
        fprintf(stderr, "[overlay] the game's fonts are not used: %s: %s\n", s_job.path, why);
        s_state.store(-1);
    } else {
        fprintf(stderr, "[overlay] the game's fonts: %u ms (%zu + %zu glyphs, x%d)\n",
                (unsigned)(SDL_GetTicks() - t0), s_gf[0].g.size(), s_gf[1].g.size(), s_gf[0].k);
        s_state.store(1);
    }
    return 0;
}

extern "C" void xbox_OverlaySetGameFonts(const char *path, uint64_t size, const char *sha256_hex,
                                         const xbox_overlay_bitmap_font *body,
                                         const xbox_overlay_bitmap_font *title)
{
    const char *e = getenv("RECOMP_OVERLAY_GAME_FONT");
    if (s_thread || s_state.load() == 1 || !path || !path[0] || !sha256_hex || strlen(sha256_hex) != 64
        || !body || !title || (e && e[0] == '0'))
        return;
    snprintf(s_job.path, sizeof s_job.path, "%s", path);
    s_job.size = size;
    for (int i = 0; i < 65; i++)
        s_job.sha[i] = (char)((sha256_hex[i] >= 'A' && sha256_hex[i] <= 'F') ? sha256_hex[i] + 32 : sha256_hex[i]);
    s_job.f[GF_BODY] = *body;
    s_job.f[GF_TITLE] = *title;
    s_state.store(0);
    s_thread = SDL_CreateThread(load_thread, "gamefont", NULL);
    if (s_thread)
        SDL_DetachThread(s_thread);
    else
        load_thread(NULL);
}

/* ── a font the port made (xbox_OverlaySetGameFontAtlas) ───────────── */

struct AtlasJob {
    std::vector<uint8_t> cov;
    uint32_t w, h, cell;
    std::vector<xbox_overlay_atlas_glyph> g;
};
static AtlasJob s_ajob;

/* As font_make, from the port's coverage and boxes. A glyph with no box (the
 * space) is kept for its advance. */
static bool atlas_make(const AtlasJob &j, GameFont &out)
{
    out.tw = (int)j.w;
    out.th = (int)j.h;
    out.g.clear();
    const float W = (float)j.w, H = (float)j.h;
    for (const xbox_overlay_atlas_glyph &a : j.g) {
        if (!a.code || a.code >= 0x10000 || (uint32_t)a.x + a.w > j.w || (uint32_t)a.y + a.h > j.h)
            continue;
        GfGlyph g = { a.x / W, a.y / H, a.w / W, a.h / H, a.xo / W, a.yo / H, a.adv / W };
        out.g[a.code] = g;
    }
    auto Hg = out.g.find('H');
    if (out.g.size() < 26 || Hg == out.g.end() || Hg->second.h <= 0.0f)
        return false;
    out.ref = (float)j.cell;
    out.caps = Hg->second.h * H / out.ref;
    out.base = (Hg->second.yo + Hg->second.h) * H / out.ref;
    std::vector<uint32_t> rgba((size_t)j.w * j.h);
    for (size_t i = 0; i < rgba.size(); i++)
        rgba[i] = (uint32_t)j.cov[i] << 24 | 0x00ffffffu;
    out.k = 4;
    std::vector<uint32_t> big(rgba.size() * 16);
    if (!nv2a_texscale(rgba.data(), (int)j.w, (int)j.h, 4, NV2A_TEXSCALE_SUPERXBR, big.data())) {
        out.k = 1;
        big = rgba;
    }
    out.a.resize(big.size());
    for (size_t i = 0; i < big.size(); i++)
        out.a[i] = (uint8_t)(big[i] >> 24);
    return true;
}

static int atlas_thread(void *)
{
    const uint32_t t0 = SDL_GetTicks();
    if (!atlas_make(s_ajob, s_gf[GF_BODY])) {
        s_gf[GF_BODY] = GameFont();
        fprintf(stderr, "[overlay] the game's font is not used: its atlas has no capitals\n");
        s_state.store(-1);
    } else {
        s_gf[GF_TITLE] = s_gf[GF_BODY];
        fprintf(stderr, "[overlay] the game's font: %u ms (%zu glyphs, x%d)\n", (unsigned)(SDL_GetTicks() - t0),
                s_gf[GF_BODY].g.size(), s_gf[GF_BODY].k);
        s_state.store(1);
    }
    s_ajob = AtlasJob();
    return 0;
}

extern "C" void xbox_OverlaySetGameFontAtlas(const uint8_t *coverage, uint32_t w, uint32_t h, uint32_t cell,
                                             const xbox_overlay_atlas_glyph *glyphs, uint32_t count)
{
    const char *e = getenv("RECOMP_OVERLAY_GAME_FONT");
    if (s_thread || s_state.load() == 1 || !coverage || !glyphs || !count || !cell || w < 4 || h < 4
        || w > 2048 || h > 2048 || (e && e[0] == '0'))
        return;
    s_ajob.cov.assign(coverage, coverage + (size_t)w * h);
    s_ajob.w = w;
    s_ajob.h = h;
    s_ajob.cell = cell;
    s_ajob.g.assign(glyphs, glyphs + count);
    s_state.store(0);
    s_thread = SDL_CreateThread(atlas_thread, "gamefont", NULL);
    if (s_thread)
        SDL_DetachThread(s_thread);
    else
        atlas_thread(NULL);
}

extern "C" int xbox_OverlayGameFontsState(unsigned wait_ms)
{
    const uint32_t until = SDL_GetTicks() + wait_ms;
    while (s_state.load() == 0 && (int32_t)(until - SDL_GetTicks()) > 0)
        SDL_Delay(5);
    return s_state.load();
}

/* ── the Dear ImGui loader ─────────────────────────────────────────── */

static bool gl_src_init(ImFontAtlas *, ImFontConfig *src)
{
    src->FontLoaderData = src->FontData;
    return src->FontData != nullptr;
}

static bool gl_contains(ImFontAtlas *, ImFontConfig *src, ImWchar c)
{
    const GameFont *f = (const GameFont *)src->FontLoaderData;
    return f->g.count((uint32_t)c) != 0;
}

static bool gl_baked_init(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *)
{
    const GameFont *f = (const GameFont *)src->FontLoaderData;
    if (!src->MergeMode) {
        baked->Ascent = ImCeil(f->base * baked->Size);
        baked->Descent = -ImCeil((1.0f - f->base) * baked->Size);
    }
    return true;
}

/* The coverage at (x, y) of the large picture, bilinear, kept inside the
 * glyph's own box [x0, x1) x [y0, y1) so no neighbour bleeds in. */
static float sample(const GameFont *f, float x, float y, float x0, float y0, float x1, float y1)
{
    const int W = f->tw * f->k;
    x = x < x0 ? x0 : x > x1 - 1.0f ? x1 - 1.0f : x;
    y = y < y0 ? y0 : y > y1 - 1.0f ? y1 - 1.0f : y;
    const int ix = (int)x, iy = (int)y;
    const float fx = x - ix, fy = y - iy;
    const int ix1 = ix + 1 < (int)x1 ? ix + 1 : ix, iy1 = iy + 1 < (int)y1 ? iy + 1 : iy;
    const uint8_t *a = f->a.data();
    const float top = a[iy * W + ix] * (1 - fx) + a[iy * W + ix1] * fx;
    const float bot = a[iy1 * W + ix] * (1 - fx) + a[iy1 * W + ix1] * fx;
    return top * (1 - fy) + bot * fy;
}

static bool gl_load_glyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *, ImWchar c,
                          ImFontGlyph *out, float *out_adv)
{
    const GameFont *f = (const GameFont *)src->FontLoaderData;
    auto it = f->g.find((uint32_t)c);
    if (it == f->g.end())
        return false;
    const GfGlyph &g = it->second;
    const float S = baked->Size * src->ExtraSizeScale;
    const float sc = S / f->ref;                          /* pixels a texel */
    const float adv = g.adv * f->tw * sc;
    if (out_adv) {
        *out_adv = adv;
        return true;
    }
    out->Codepoint = c;
    out->AdvanceX = adv;
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float scr = sc * density;
    const int w = (int)ceilf(g.w * f->tw * scr), h = (int)ceilf(g.h * f->th * scr);
    if (w <= 0 || h <= 0)
        return true;
    ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
    if (id == ImFontAtlasRectId_Invalid)
        return false;
    ImTextureRect *r = ImFontAtlasPackGetRect(atlas, id);
    ImFontAtlasBuilder *b = atlas->Builder;
    b->TempBuffer.resize(w * h);
    unsigned char *px = b->TempBuffer.Data;
    /* A pixel's footprint in the large picture; several taps when it covers
     * more than a texel there (the usual case: the picture is 4x). */
    const float k = (float)f->k;
    const float gx0 = g.u * f->tw * k, gy0 = g.v * f->th * k;
    const float gx1 = gx0 + g.w * f->tw * k, gy1 = gy0 + g.h * f->th * k;
    const float step = k / scr;
    int taps = (int)ceilf(step);
    taps = taps < 1 ? 1 : taps > 6 ? 6 : taps;
    const float inv = 1.0f / (float)(taps * taps);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int ty = 0; ty < taps; ty++)
                for (int tx = 0; tx < taps; tx++)
                    acc += sample(f, gx0 + (x + (tx + 0.5f) / taps) * step - 0.5f,
                                  gy0 + (y + (ty + 0.5f) / taps) * step - 0.5f, gx0, gy0, gx1, gy1);
            const float v = acc * inv;
            px[y * w + x] = (unsigned char)(v > 255.0f ? 255 : v);
        }
    /* Placed on whole pixels, on the first source's baseline. */
    const float x0 = ImFloor(g.xo * f->tw * sc + 0.5f);
    const float y0 = ImFloor(g.yo * f->th * sc - f->base * S + baked->Ascent + 0.5f);
    out->X0 = x0;
    out->Y0 = y0;
    out->X1 = x0 + w / density;
    out->Y1 = y0 + h / density;
    out->Visible = true;
    out->PackId = id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, px, ImTextureFormat_Alpha8, w);
    return true;
}

const ImFontLoader *gamefont_loader()
{
    static ImFontLoader l;
    if (!l.Name) {
        l.Name = "game bitmap font";
        l.FontSrcInit = gl_src_init;
        l.FontSrcContainsGlyph = gl_contains;
        l.FontBakedInit = gl_baked_init;
        l.FontBakedLoadGlyph = gl_load_glyph;
    }
    return &l;
}

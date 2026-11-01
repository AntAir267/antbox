/*
 * overlay_shot - the overlay on Metal, off screen: no window is shown and no
 * game runs. SDL's dummy video driver gives ImGui a window to read its size
 * from; the frames are drawn into a texture the tool makes, with synthetic
 * key and controller events, and written as BMPs. Checks the pause and input
 * routing on the way (the open overlay pauses and blocks, closing undoes it).
 *
 *   overlay_shot <out prefix> [width height]
 */
#include "overlay.h"
#include "overlay_metal.h"
#include "xbox_settings.h"
#include "apu_bus.h"
#include "nv2a_texrep.h"
#include "pad_glyphs.h"
#include "recomp_env.h"

#import <Metal/Metal.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Not the headers: their Windows type names clash with Objective-C's BOOL. */
extern "C" int xbox_InputBlocked(void);
extern "C" int xbox_HostPaused(void);
extern "C" void xbox_HostPauseHold(void);

/* The kernel library wants the title's function table; there is no title. */
extern "C" void *recomp_lookup(unsigned va) { (void)va; return NULL; }
extern "C" void *recomp_lookup_manual(unsigned va) { (void)va; return NULL; }

/* The bug report's maker (xbox_report.h), stood in for: the overlay's two
 * actions call it and show where the archive went, the home folder as ~. */
extern "C" void xbox_BugReportSetMaker(int (*fn)(int screenshot, char *out_path, unsigned n));
static int s_reports, s_report_shot;
static int fake_report(int screenshot, char *out, unsigned n)
{
    const char *home = getenv("HOME");
    s_reports++;
    s_report_shot = screenshot;
    snprintf(out, n, "%s/Library/Application Support/Test/reports/test-report-20260101-000000.zip",
             home ? home : "");
    return 1;
}

extern "C" void xbox_InputOfferControlScheme(int on);    /* input_map.h */

static int s_fail;
#define CHECK(c) do { if (!(c)) { s_fail++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static id<MTLTexture> tex;
static int W = 1280, H = 720;

static void key(SDL_Keycode k, int mod = 0, bool down = true)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.sym = k;
    e.key.keysym.mod = (Uint16)mod;
    xbox_OverlayEvent(&e);
}

static void tap(SDL_Keycode k)
{
    key(k);
    key(k, 0, false);
}

static void pad(int button, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = 0;
    e.cbutton.button = (Uint8)button;
    xbox_OverlayEvent(&e);
}

static void frame(const char *save)
{
    @autoreleasepool {
        xbox_OverlayPoll();
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        if (!tex)
            tex = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.4, 0.55, 1);   /* a stand-in for the game */
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (xbox_OverlayWants())
            xbox_OverlayMetalDraw((__bridge void *)dev, (__bridge void *)cb, (__bridge void *)rp, (__bridge void *)enc);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (save) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
            [tex getBytes:s->pixels bytesPerRow:(NSUInteger)s->pitch fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            SDL_SaveBMP(s, save);
            SDL_FreeSurface(s);
            fprintf(stderr, "shot %s\n", save);
        }
    }
}

static void run(int n)
{
    for (int i = 0; i < n; i++) {
        frame(NULL);
        SDL_Delay(20);
    }
}

/* OVERLAY_SHOT_MIXER=<preset file>: the Mixer page (BO-114) instead of the
 * tour of the pages: thirteen buses as a title registers them, a little
 * signal for the meters, and the page driven with keys. */
static void frame(const char *save);
static void run(int n);

static void mixer_signal(void)
{
    static const int8_t map[32] = { 0, 1, 2, 3, 4, 5, 0, 1, 4, 5, 0, 1, 0, 1, 4, 5, 2, 0, 1,
                                    0, 1, 4, 5, 2, 3, 0, -1, 1, 0, 1, 0, 1 };
    static float bins[32][32], feeds[6][32], smp[32][2];
    static const int bin[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };
    static unsigned t;
    for (int f = 0; f < 400; f++) {
        struct apu_bus_rt *rt = apu_bus_frame_begin(6, map);
        if (!rt) return;
        memset(bins, 0, sizeof bins);
        for (int bus = 0; bus < 12; bus++) {
            float g[8] = { 0.05f * (float)(bus + 1), 0.04f * (float)(bus + 1), 0.02f, 0.02f, 0, 0.01f, 0, 0 };
            for (int i = 0; i < 32; i++)
                smp[i][0] = smp[i][1] = sinf((float)(t + (unsigned)i) * 0.05f * (float)(bus + 1));
            apu_bus_mix_voice(rt, bus, bins, (const float (*)[2])smp, 1, bin, g);
        }
        apu_bus_frame_end(rt, bins);
        for (int c = 0; c < 6; c++)
            memcpy(feeds[c], bins[c], sizeof feeds[c]);
        apu_bus_output(feeds, 6, 32);
        t += 32;
    }
}

/* ── OVERLAY_SHOT_MIXER_E2E=<preset file>: every control of the Mixer page,
 * worked with pad buttons as a player does, and after each: the mixer's
 * active configuration changed AND what it renders changed. Three voices
 * as the game makes them: music (a stereo stream on the speaker bins), a
 * positioned impact (six sends), the player's engine (mixbin 17). */
extern "C" int xbox_OverlayMixerSelect(const char *label);
extern "C" int xbox_OverlayMixerView(void);

enum { V_MUSIC = 1, V_IMPACT = 2, V_ENGINE = 4 };
static double e_spk[6], e_peak;                 /* speaker feeds: RMS, and the peak */

static void e2e_render(int voices)
{
    static const int8_t map[32] = { 0, 1, 2, 3, 4, 5, 0, 1, 4, 5, 0, 1, 0, 1, 4, 5, 2, 0, 1,
                                    0, 1, 4, 5, 2, 3, 0, -1, 1, 0, 1, 0, 1 };
    static const int b_music[8] = { 0, 1, 4, 5, 2, 3, 6, 7 }, b_pos[8] = { 19, 20, 16, 24, 14, 15, 11, 0 },
                     b_eng[8] = { 0, 1, 4, 5, 2, 17, 3, 6 };
    static const float g_music[8] = { 0.5f, 0.5f, 0.4f, 0.4f, 0, 0.07f, 0, 0 },
                       g_pos[8] = { 0.5f, 0.1f, 0.2f, 0.05f, 0.3f, 0.1f, 0, 0 },
                       g_eng[8] = { 0, 0, 0, 0, 0, 0.5f, 0, 0 };
    static float bins[32][32], feeds[6][32], st[32][2], mono[32][2], low[32][2];
    static unsigned long t_low;
    double sq[6] = { 0, 0, 0, 0, 0, 0 };
    int n = 0;
    e_peak = 0;
    for (int f = 0; f < 1200; f++) {
        struct apu_bus_rt *rt = apu_bus_frame_begin(6, map);
        if (!rt) { memset(e_spk, 0, sizeof e_spk); return; }
        memset(bins, 0, sizeof bins);
        for (int i = 0; i < 32; i++) {
            /* 6 kHz: whole periods in a frame, left and right orthogonal.
             * The engine: 100 Hz, running on from frame to frame. */
            low[i][0] = low[i][1] = sinf(2.0f * 3.14159265f * 100.0f * (float)(t_low++ % 480) / 48000.0f);
            st[i][0] = sinf(2.0f * 3.14159265f * 4.0f * (float)i / 32.0f);
            st[i][1] = cosf(2.0f * 3.14159265f * 4.0f * (float)i / 32.0f);
            mono[i][0] = mono[i][1] = sinf(2.0f * 3.14159265f * 3.0f * (float)i / 32.0f);
        }
        if (apu_bus_audition_playing()) {
            /* A clip loops: its voices in place of the live ones (apu_vp.c). */
            apu_bus_audition_mix(rt, bins);
        } else {
        if (voices & V_MUSIC)  apu_bus_mix_voice(rt, 0, bins, (const float (*)[2])st, 2, b_music, g_music);
        if (voices & V_IMPACT) apu_bus_mix_voice(rt, 9, bins, (const float (*)[2])mono, 1, b_pos, g_pos);
        if (voices & V_ENGINE) apu_bus_mix_voice(rt, 4, bins, (const float (*)[2])low, 1, b_eng, g_eng);
        }
        apu_bus_frame_end(rt, bins);
        memset(feeds, 0, sizeof feeds);
        for (int b = 0; b < 32; b++)
            if (map[b] >= 0)
                for (int i = 0; i < 32; i++) feeds[map[b]][i] += bins[b][i];
        apu_bus_output(feeds, 6, 32);
        if (f < 900) continue;                  /* filters, ducking and the limiter settled */
        for (int c = 0; c < 6; c++)
            for (int i = 0; i < 32; i++) {
                sq[c] += (double)feeds[c][i] * feeds[c][i];
                if (fabs(feeds[c][i]) > e_peak) e_peak = fabs(feeds[c][i]);
            }
        n += 32;
    }
    for (int c = 0; c < 6; c++) e_spk[c] = sqrt(sq[c] / n);
}

static double e_db(double a, double b) { return a > 1e-9 && b > 1e-9 ? 20.0 * log10(a / b) : a > 1e-9 ? 200.0 : -200.0; }
#define SILENT(c) (e_spk[c] < 1e-6)

static void ptap(int button, int times = 1)
{
    for (int i = 0; i < times; i++) {
        pad(button, true); run(1);
        pad(button, false); run(1);
    }
}
#define P_A     SDL_CONTROLLER_BUTTON_A
#define P_B     SDL_CONTROLLER_BUTTON_B
#define P_Y     SDL_CONTROLLER_BUTTON_Y
#define P_UP    SDL_CONTROLLER_BUTTON_DPAD_UP
#define P_DOWN  SDL_CONTROLLER_BUTTON_DPAD_DOWN
#define P_LEFT  SDL_CONTROLLER_BUTTON_DPAD_LEFT
#define P_RIGHT SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#define P_LB    SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#define P_RB    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#define SEL(label) do { CHECK(xbox_OverlayMixerSelect(label)); run(1); } while (0)

static int mixer_e2e(const char *file)
{
    static const char *const names[13] = {
        "Music", "DJ", "Film", "Streams", "Player engine", "Player exhaust", "Other cars",
        "Traffic", "Tyres/road/wind", "Impacts/crash", "Boost", "UI/HUD", "Other" };
    apu_bus_config c;
    double base[6], v;

    remove(file);
    apu_bus_set_names(names, 13);
    apu_bus_set_duck_default(9, 0);
    apu_bus_add_preset("Factory", "", "");
    apu_bus_add_preset("Music front", "", "[bus.music]\nroute = \"pan\"\npan_x = 0\npan_y = 1\npan_width = 1\n");
    apu_bus_set_file(file);

    /* Open, Audio, the Mixer row; on. The other pages pause the title, the
     * Mixer page does not. */
    pad(SDL_CONTROLLER_BUTTON_BACK, true); pad(SDL_CONTROLLER_BUTTON_START, true);
    pad(SDL_CONTROLLER_BUTTON_START, false); pad(SDL_CONTROLLER_BUTTON_BACK, false);
    run(12);
    CHECK(xbox_OverlayOpen() && xbox_HostPaused() && xbox_InputBlocked());
    ptap(P_RB, 2); run(3);
    CHECK(xbox_OverlayTestRowIndex("Custom music folder") == 0);
    CHECK(xbox_OverlayTestSelectRow("Mixer")); run(2);   /* past the music folder's rows and the loudness */
    ptap(P_A); run(3);
    CHECK(xbox_OverlayMixerView() == 0);
    CHECK(!xbox_HostPaused() && xbox_InputBlocked());               /* live: runs, input still ours */
    SEL("Mixer"); ptap(P_A); run(2);
    CHECK(apu_bus_enabled());
    e2e_render(V_MUSIC);
    memcpy(base, e_spk, sizeof base);
    CHECK(base[0] > 0.1 && base[4] > 0.1 && SILENT(2));             /* the game's music: four corners */

    /* Bus gain, mute (LB), solo (RB), reset (Y), from the list. */
    SEL("Music"); ptap(P_RIGHT, 4);
    apu_bus_get_config(&c); CHECK(c.bus[0].gain_db == 2.0f);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[0], base[0]) - 2.0) < 0.01);
    ptap(P_LB);
    apu_bus_get_config(&c); CHECK(c.bus[0].mute);
    e2e_render(V_MUSIC); CHECK(SILENT(0) && SILENT(4));
    ptap(P_LB);
    SEL("Film"); ptap(P_RB);
    apu_bus_get_config(&c); CHECK(c.bus[2].solo && !c.bus[0].mute);
    e2e_render(V_MUSIC); CHECK(SILENT(0));
    ptap(P_RB);
    SEL("Music"); ptap(P_Y);
    apu_bus_get_config(&c); CHECK(c.bus[0].gain_db == 0.0f);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[0], base[0])) < 0.001);

    /* The Music bus. Its own Gain / Mute / Solo rows. */
    SEL("Music"); ptap(P_A); run(2);
    CHECK(xbox_OverlayMixerView() == 1 && !xbox_HostPaused());
    SEL("Gain"); ptap(P_LEFT, 6);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[1], base[1]) + 3.0) < 0.01);
    ptap(P_Y);
    SEL("Mute"); ptap(P_A);
    e2e_render(V_MUSIC); CHECK(SILENT(0));
    ptap(P_A);
    SEL("Solo"); ptap(P_A);
    e2e_render(V_MUSIC | V_IMPACT); v = e_spk[2];                   /* the impact's centre is gone */
    CHECK(v < 1e-6 && e_spk[0] > 0.1);
    ptap(P_A);

    /* Route "the game's panning": the trims. */
    SEL("Front trim"); ptap(P_A);                                    /* A: off */
    apu_bus_get_config(&c); CHECK(c.bus[0].trim_db[APU_BUS_TRIM_FRONT] <= APU_BUS_OFF_DB);
    e2e_render(V_MUSIC); CHECK(SILENT(0) && SILENT(1) && fabs(e_db(e_spk[4], base[4])) < 0.001);
    ptap(P_A);                                                       /* A: back at 0 dB */
    SEL("Rear trim"); ptap(P_LEFT, 12);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[4], base[4]) + 6.0) < 0.01 && fabs(e_db(e_spk[0], base[0])) < 0.001);
    ptap(P_Y);
    SEL("LFE trim"); ptap(P_A);
    e2e_render(V_MUSIC); CHECK(SILENT(3));
    ptap(P_A);
    SEL("Centre trim"); ptap(P_RIGHT, 2);
    apu_bus_get_config(&c); CHECK(c.bus[0].trim_db[APU_BUS_TRIM_CENTRE] == 1.0f);
    ptap(P_Y);

    /* Route "sends". */
    SEL("Route"); ptap(P_RIGHT);
    apu_bus_get_config(&c); CHECK(c.bus[0].route == APU_BUS_ROUTE_SENDS);
    e2e_render(V_MUSIC); CHECK(e_spk[0] > 0.1 && SILENT(4) && SILENT(5) && SILENT(3));
    SEL("Send to surround left"); ptap(P_A);                         /* off -> 0 dB */
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[4], e_spk[0])) < 0.01 && SILENT(5));
    ptap(P_LEFT, 12);                                                /* -6 dB */
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[4], e_spk[0]) + 6.0) < 0.01);
    SEL("Send to front left"); ptap(P_A);                            /* off */
    SEL("Send to front right"); ptap(P_A);
    SEL("Send to surround right"); ptap(P_RIGHT);                    /* from off: in at -20 dB */
    apu_bus_get_config(&c); CHECK(c.bus[0].send_db[5] == -20.0f);
    ptap(P_A); ptap(P_A);                                            /* off, then 0 dB */
    e2e_render(V_MUSIC); CHECK(SILENT(0) && SILENT(1) && SILENT(2) && e_spk[4] > 0.05 && e_spk[5] > 0.1);  /* rear only */
    SEL("Send to centre"); ptap(P_A);
    SEL("Send to LFE (full range)"); ptap(P_A);
    e2e_render(V_MUSIC); CHECK(e_spk[2] > 0.1 && e_spk[3] > 0.1);

    /* Route "a place in the room": front, full width; then by hand to the
     * rear wall (rear only), then narrow and to the left wall (SL only). */
    SEL("Route"); ptap(P_RIGHT);
    apu_bus_get_config(&c); CHECK(c.bus[0].route == APU_BUS_ROUTE_PAN);
    e2e_render(V_MUSIC); CHECK(e_spk[0] > 0.1 && e_spk[1] > 0.1 && SILENT(2) && SILENT(4) && SILENT(5) && SILENT(3));
    SEL("Position"); ptap(P_A);                                      /* move */
    ptap(P_DOWN, 40);
    apu_bus_get_config(&c); CHECK(fabsf(c.bus[0].pan_y + 1.0f) < 1e-4f);
    e2e_render(V_MUSIC); CHECK(SILENT(0) && SILENT(1) && SILENT(2) && e_spk[4] > 0.1 && e_spk[5] > 0.1);   /* rear only */
    ptap(P_A);                                                       /* done */
    CHECK(xbox_OverlayMixerView() == 1);
    SEL("Width"); ptap(P_LEFT, 20);
    apu_bus_get_config(&c); CHECK(fabsf(c.bus[0].pan_width) < 1e-4f);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[4], e_spk[5])) < 0.01);                                       /* rear centre */
    SEL("Position X (left - right)"); ptap(P_LEFT, 20);
    apu_bus_get_config(&c); CHECK(fabsf(c.bus[0].pan_x + 1.0f) < 1e-4f);
    e2e_render(V_MUSIC); CHECK(e_spk[4] > 0.1 && SILENT(5) && SILENT(0) && SILENT(1));                       /* SL only */
    SEL("Position Y (rear - front)"); ptap(P_RIGHT, 40);
    e2e_render(V_MUSIC); CHECK(e_spk[0] > 0.1 && SILENT(4) && SILENT(1));                                    /* FL only */
    SEL("Position X (left - right)"); ptap(P_RIGHT, 20);
    e2e_render(V_MUSIC); CHECK(e_spk[2] > 0.1 && SILENT(0) && SILENT(1));                                    /* C only */
    SEL("Centre divergence"); ptap(P_RIGHT, 20);
    apu_bus_get_config(&c); CHECK(fabsf(c.bus[0].pan_divergence - 1.0f) < 1e-4f);
    e2e_render(V_MUSIC); CHECK(SILENT(2) && e_spk[0] > 0.1 && fabs(e_db(e_spk[0], e_spk[1])) < 0.01);        /* phantom centre */
    /* The position with the stick held: it speeds up, and gets there. */
    SEL("Position"); ptap(P_A);
    pad(P_DOWN, true);
    for (int i = 0; i < 60; i++) { SDL_Delay(20); run(1); }
    pad(P_DOWN, false); run(1);
    apu_bus_get_config(&c); CHECK(c.bus[0].pan_y == -1.0f);
    ptap(P_B);                                                       /* B ends the move, not the view */
    CHECK(xbox_OverlayMixerView() == 1);

    /* The LFE send and its crossover (the music tone is 6 kHz: through an
     * 80 Hz crossover nothing of it arrives, full range all of it). */
    SEL("LFE send (through the crossover)"); ptap(P_A);
    apu_bus_get_config(&c); CHECK(c.bus[0].lfe_send_db == -10.0f);
    e2e_render(V_MUSIC); CHECK(e_spk[3] < 1e-4);
    SEL("LFE send crossover"); ptap(P_LEFT);                         /* 80 Hz -> 60 Hz */
    apu_bus_get_config(&c); CHECK(c.bus[0].lfe_crossover_hz == 60);
    ptap(P_LEFT);                                                    /* full range */
    apu_bus_get_config(&c); CHECK(c.bus[0].lfe_crossover_hz == 0);
    e2e_render(V_MUSIC); CHECK(e_spk[3] > 0.05);
    SEL("Reset this bus"); ptap(P_A);
    apu_bus_get_config(&c); CHECK(c.bus[0].route == APU_BUS_ROUTE_TITLE && c.bus[0].lfe_send_db <= APU_BUS_OFF_DB);
    e2e_render(V_MUSIC);
    for (int k = 0; k < 6; k++) CHECK(fabs(e_spk[k] - base[k]) < 1e-9);
    ptap(P_B); run(2);
    CHECK(xbox_OverlayMixerView() == 0 && !xbox_HostPaused());

    /* A positioned bus: the field shaping. */
    e2e_render(V_IMPACT);
    memcpy(base, e_spk, sizeof base);
    CHECK(base[0] > base[1] * 3 && base[2] > 0.05);
    SEL("Impacts/crash"); ptap(P_A); run(2);
    SEL("Field width"); ptap(P_LEFT, 20);
    apu_bus_get_config(&c); CHECK(fabsf(c.bus[9].field_width) < 1e-4f);
    e2e_render(V_IMPACT); CHECK(fabs(e_db(e_spk[0], e_spk[1])) < 0.01 && fabs(e_db(e_spk[4], e_spk[5])) < 0.01);
    ptap(P_Y);
    apu_bus_get_config(&c); CHECK(c.bus[9].field_width == 1.0f);
    SEL("Centre divergence"); ptap(P_RIGHT, 20);
    e2e_render(V_IMPACT); CHECK(SILENT(2) && e_spk[0] > base[0]);
    ptap(P_Y);
    e2e_render(V_IMPACT);
    for (int k = 0; k < 6; k++) CHECK(fabs(e_spk[k] - base[k]) < 1e-9);
    ptap(P_B); run(2);

    /* The master rows. */
    SEL("Limiter"); ptap(P_RIGHT);                                   /* soft clip */
    apu_bus_get_config(&c); CHECK(c.limiter == APU_BUS_LIM_SOFT);
    SEL("Ceiling"); ptap(P_LEFT, 12);                                /* -0.3 -> -6.5 (the row's grid) */
    apu_bus_get_config(&c); CHECK(c.ceiling_db < -6.0f && c.ceiling_db > -7.0f);
    SEL("Music"); ptap(P_RIGHT, 24);                                 /* +12 dB: over the ceiling */
    e2e_render(V_MUSIC); CHECK(e_peak <= pow(10.0, c.ceiling_db / 20.0) + 1e-4 && e_peak > 0.3);
    SEL("Limiter"); ptap(P_RIGHT);                                   /* peak limiter */
    apu_bus_get_config(&c); CHECK(c.limiter == APU_BUS_LIM_PEAK);
    e2e_render(V_MUSIC); CHECK(e_peak <= pow(10.0, c.ceiling_db / 20.0) + 1e-4 && e_peak > 0.3);
    ptap(P_RIGHT);                                                   /* off */
    e2e_render(V_MUSIC); CHECK(e_peak > 1.0);
    SEL("Music"); ptap(P_Y);
    SEL("LFE low-pass"); ptap(P_RIGHT, 1);                           /* 60 Hz */
    apu_bus_get_config(&c); CHECK(c.lfe_lowpass_hz == 60);
    e2e_render(V_MUSIC); v = e_spk[3];
    ptap(P_LEFT);
    e2e_render(V_MUSIC); CHECK(v < e_spk[3] * 0.1);
    SEL("Mains high-pass"); ptap(P_LEFT);                            /* 150 Hz (wraps) */
    apu_bus_get_config(&c); CHECK(c.mains_highpass_hz == 150);
    e2e_render(V_ENGINE); v = e_spk[0];                              /* the engine's 100 Hz, on FL */
    ptap(P_RIGHT);
    e2e_render(V_ENGINE); e2e_render(V_ENGINE);
    CHECK(e_spk[0] > 0.3 && e_db(v, e_spk[0]) < -10.0);

    /* Speakers. */
    e2e_render(V_MUSIC); memcpy(base, e_spk, sizeof base);
    SEL("Speakers: trim and delay"); ptap(P_A); run(2);
    CHECK(xbox_OverlayMixerView() == 2 && !xbox_HostPaused());
    SEL("Front right trim"); ptap(P_LEFT, 12);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[1], base[1]) + 6.0) < 0.01 && fabs(e_db(e_spk[0], base[0])) < 0.001);
    ptap(P_Y);
    SEL("Surround left delay"); ptap(P_RIGHT, 2);
    apu_bus_get_config(&c); CHECK(c.speaker_delay_ms[4] == 1.0f);
    e2e_render(V_MUSIC); CHECK(fabs(e_db(e_spk[4], base[4])) < 0.01);       /* delayed, not changed */
    ptap(P_Y);
    ptap(P_B); run(2);

    /* Ducking: music under impacts. */
    SEL("Ducking"); ptap(P_A); run(2);
    CHECK(xbox_OverlayMixerView() == 3);
    SEL("Ducking"); ptap(P_A);
    apu_bus_get_config(&c); CHECK(c.duck.enabled && c.duck.source == 9 && c.duck.target == 0);
    SEL("Depth"); ptap(P_RIGHT, 4);                                  /* 6 -> 8 dB */
    apu_bus_get_config(&c); CHECK(c.duck.depth_db == 8.0f);
    e2e_render(V_MUSIC); v = e_spk[5];                               /* SR: music only (the impact's is small there) */
    CHECK(fabs(e_db(v, base[5])) < 0.01);                            /* nothing to duck under */
    e2e_render(V_MUSIC | V_IMPACT);
    CHECK(e_db(e_spk[1], base[1]) < -5.0);                           /* music's FR well down */
    SEL("Threshold"); ptap(P_RIGHT, 60);                             /* 0 dBFS: never reached */
    e2e_render(V_MUSIC | V_IMPACT); e2e_render(V_MUSIC | V_IMPACT);  /* the release, 400 ms, run out */
    CHECK(e_db(e_spk[1], base[1]) > -1.0);
    SEL("Ducking"); ptap(P_A);
    ptap(P_B); run(2);

    /* Audition: record, loop, the mixer acts on the loop, save, stop, the
     * saved clip. */
    {
        apu_bus_audition_info ai;
        char clip[600];
        snprintf(clip, sizeof clip, "%s", file);
        char *sl = strrchr(clip, '/');
        snprintf(sl ? sl + 1 : clip, 40, "mixer-audition.clip");
        remove(clip);
        SEL("Audition"); ptap(P_A); run(1);                          /* nothing recorded: stays live */
        e2e_render(V_MUSIC);
        CHECK(apu_bus_audition_state() == APU_BUS_AUD_OFF);
        SEL("Audition recording"); ptap(P_RIGHT); run(1);            /* the last 30 s */
        CHECK(apu_bus_audition_kept_seconds() == 30);
        for (int i = 0; i < 8; i++) e2e_render(V_MUSIC | V_IMPACT);  /* 6.4 s of "play" */
        memcpy(base, e_spk, sizeof base);
        apu_bus_audition_get(&ai);
        CHECK(ai.kept_s > 6.0f && ai.bytes > 1000000);
        SEL("Audition"); ptap(P_A); run(1);
        /* No live voice: the loop sounds. Past its first 2 s, where the
         * clip's end fades out under its start (the test's tones are the
         * same there, so they add; real sound does not). */
        for (int i = 0; i < 3; i++) e2e_render(0);
        CHECK(apu_bus_audition_state() == APU_BUS_AUD_LAST);
        CHECK(fabs(e_db(e_spk[0], base[0])) < 0.2 && fabs(e_db(e_spk[2], base[2])) < 0.2);
        SEL("Music"); ptap(P_LB);                                    /* mute acts on the loop */
        e2e_render(0);
        CHECK(e_db(e_spk[5], base[5]) < -6.0 && fabs(e_db(e_spk[2], base[2])) < 0.2);
        ptap(P_LB);
        SEL("Save this clip"); ptap(P_A); run(1);
        { FILE *fp = fopen(clip, "rb"); CHECK(fp != NULL); if (fp) fclose(fp); }
        SEL("Audition"); ptap(P_A); run(1);                          /* back to live */
        e2e_render(V_MUSIC);
        CHECK(apu_bus_audition_state() == APU_BUS_AUD_OFF && SILENT(2) && e_spk[0] > 0.1);
        SEL("Audition"); ptap(P_RIGHT); run(1);                      /* the saved clip */
        for (int i = 0; i < 3; i++) e2e_render(0);
        CHECK(apu_bus_audition_state() == APU_BUS_AUD_FILE && fabs(e_db(e_spk[2], base[2])) < 0.2);
        ptap(P_A); run(1);
        e2e_render(V_MUSIC);
        CHECK(apu_bus_audition_state() == APU_BUS_AUD_OFF);
        SEL("Audition recording"); ptap(P_LEFT); run(1);
        e2e_render(V_MUSIC);
        CHECK(apu_bus_audition_kept_seconds() == 0);
        remove(clip);
    }

    /* Presets and A/B. */
    SEL("Load a preset into A"); ptap(P_RIGHT); ptap(P_A);
    apu_bus_get_config(&c); CHECK(c.bus[0].route == APU_BUS_ROUTE_PAN);
    e2e_render(V_MUSIC); CHECK(SILENT(4) && e_spk[0] > 0.1);
    SEL("Listening to"); ptap(P_A); run(1);                          /* B = Factory */
    apu_bus_get_config(&c); CHECK(apu_bus_config_is_neutral(&c));
    e2e_render(V_MUSIC); CHECK(e_spk[4] > 0.1);
    pad(SDL_CONTROLLER_BUTTON_BACK, true); pad(P_Y, true); pad(P_Y, false); pad(SDL_CONTROLLER_BUTTON_BACK, false);
    run(2);                                                          /* Back + Y: A again */
    apu_bus_get_config(&c); CHECK(c.bus[0].route == APU_BUS_ROUTE_PAN);
    e2e_render(V_MUSIC); CHECK(SILENT(4));

    /* The file has what is heard; an edit of the file is heard too. */
    SDL_Delay(500); run(3);
    {
        apu_bus_config f;
        char err[160];
        apu_bus_config_neutral(&f);
        CHECK(apu_bus_load_file(file, &f, err, sizeof err) == 0 && f.bus[0].route == APU_BUS_ROUTE_PAN);
        FILE *fp = fopen(file, "ab");
        fputs("\n[bus.music]\nroute = \"pan\"\npan_y = -1\npan_width = 0\n", fp);
        fclose(fp);
        for (int i = 0; i < 4; i++) e2e_render(V_MUSIC);            /* 3.2 s of frames: the once-a-second look */
        apu_bus_get_config(&c); CHECK(c.bus[0].pan_y == -1.0f);
        CHECK(SILENT(0) && e_spk[4] > 0.1);
        run(3);
        SEL("Music"); ptap(P_A); run(2);
        SEL("Position Y (rear - front)"); ptap(P_RIGHT, 2);          /* the page works on from the file's values */
        apu_bus_get_config(&c); CHECK(fabsf(c.bus[0].pan_y + 0.9f) < 1e-4f && c.bus[0].pan_width == 0.0f);
        ptap(P_B); run(2);
    }

    /* Out: the Audio page pauses again; closed, nothing is paused. */
    ptap(P_B); run(3);
    CHECK(xbox_OverlayOpen() && xbox_OverlayMixerView() == -1 && xbox_HostPaused());
    ptap(P_B); run(12);
    CHECK(!xbox_OverlayOpen() && !xbox_HostPaused() && !xbox_InputBlocked());
    fprintf(stderr, s_fail ? "overlay_shot mixer e2e: %d failed\n" : "overlay_shot mixer e2e: ok\n", s_fail);
    return s_fail ? 1 : 0;
}

static int mixer_tour(const char *pre, const char *file)
{
    static const char *const names[13] = {
        "Music", "DJ", "Film", "Streams", "Player engine", "Player exhaust", "Other cars",
        "Traffic", "Tyres/road/wind", "Impacts/crash", "Boost", "UI/HUD", "Other" };
    char path[512];
    apu_bus_config c;

    remove(file);
    apu_bus_set_names(names, 13);
    apu_bus_add_preset("Factory", "The game's own mix.", "");
    apu_bus_add_preset("Music front", "Music on the front pair only.",
                       "[bus.music]\nroute = \"sends\"\nsend_fl_db = 0\nsend_fr_db = 0\n");
    apu_bus_set_file(file);

    tap(SDLK_F1);
    run(12);
    tap(SDLK_PAGEDOWN); run(2); tap(SDLK_PAGEDOWN); run(3);       /* Audio */
    CHECK(xbox_OverlayTestSelectRow("Mixer")); run(1);            /* the Mixer row, under the music folder's */
    snprintf(path, sizeof path, "%smixer_0_audio.bmp", pre); frame(path);
    tap(SDLK_RETURN); run(3);
    CHECK(!apu_bus_enabled());
    snprintf(path, sizeof path, "%smixer_1_off.bmp", pre); frame(path);
    tap(SDLK_RETURN); run(3);                                      /* Mixer: On */
    CHECK(apu_bus_enabled() && xbox_SettingValue(XS_AUDIO_MIXER) != 0);
    mixer_signal();
    for (int i = 0; i < 7; i++) { tap(SDLK_DOWN); run(1); }        /* Music */
    for (int i = 0; i < 4; i++) { tap(SDLK_RIGHT); run(1); }       /* +2 dB */
    apu_bus_get_config(&c);
    CHECK(c.bus[0].gain_db == 2.0f);
    tap(SDLK_DOWN); run(1); tap(SDLK_PAGEUP); run(1);              /* DJ: mute */
    tap(SDLK_DOWN); run(1); tap(SDLK_PAGEDOWN); run(1);            /* Film: solo */
    apu_bus_get_config(&c);
    CHECK(c.bus[1].mute && c.bus[2].solo);
    tap(SDLK_PAGEDOWN); run(1); tap(SDLK_UP); run(1); tap(SDLK_PAGEUP); run(1);
    apu_bus_get_config(&c);
    CHECK(!c.bus[1].mute && !c.bus[2].solo);
    mixer_signal();
    snprintf(path, sizeof path, "%smixer_2_main.bmp", pre); frame(path);
    tap(SDLK_UP); run(1);                                          /* Music */
    tap(SDLK_RETURN); run(3);                                      /* its page */
    for (int i = 0; i < 3; i++) { tap(SDLK_DOWN); run(1); }        /* Route */
    snprintf(path, sizeof path, "%smixer_3_bus_title.bmp", pre); frame(path);
    tap(SDLK_RIGHT); run(2);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].route == APU_BUS_ROUTE_SENDS);
    for (int i = 0; i < 3; i++) { tap(SDLK_DOWN); run(1); }        /* send to centre: off -> -60 -> up */
    tap(SDLK_RIGHT); run(1);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].send_db[2] == -20.0f);            /* from off: in at -20 dB */
    tap(SDLK_RETURN); run(1);                                      /* Enter on a level: off */
    apu_bus_get_config(&c);
    CHECK(c.bus[0].send_db[2] <= APU_BUS_OFF_DB);
    snprintf(path, sizeof path, "%smixer_4_bus_sends.bmp", pre); frame(path);
    /* Route: a place in the room. The position row, moved with the keys;
     * the width; then back to sends for the checks below. */
    for (int i = 0; i < 3; i++) { tap(SDLK_UP); run(1); }          /* Route */
    tap(SDLK_RIGHT); run(2);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].route == APU_BUS_ROUTE_PAN && c.bus[0].pan_x == 0.0f && c.bus[0].pan_y == 1.0f);
    snprintf(path, sizeof path, "%smixer_4b_pan_front.bmp", pre); frame(path);
    tap(SDLK_DOWN); run(1); tap(SDLK_RETURN); run(1);              /* Position: move */
    for (int i = 0; i < 6; i++) { tap(SDLK_DOWN); run(1); }        /* 0.3 towards the rear */
    for (int i = 0; i < 4; i++) { tap(SDLK_LEFT); run(1); }        /* 0.2 to the left */
    apu_bus_get_config(&c);
    CHECK(fabsf(c.bus[0].pan_y - 0.7f) < 1e-4f && fabsf(c.bus[0].pan_x + 0.2f) < 1e-4f);
    snprintf(path, sizeof path, "%smixer_4c_pan_moving.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(2);                                      /* done moving: still on the bus */
    CHECK(xbox_OverlayOpen());
    for (int i = 0; i < 3; i++) { tap(SDLK_DOWN); run(1); }        /* Width */
    for (int i = 0; i < 10; i++) { tap(SDLK_LEFT); run(1); }       /* 1.0 -> 0.5 */
    apu_bus_get_config(&c);
    CHECK(fabsf(c.bus[0].pan_width - 0.5f) < 1e-4f);
    tap(SDLK_DOWN); run(1); tap(SDLK_RIGHT); run(1);               /* divergence 0.05 */
    apu_bus_get_config(&c);
    CHECK(fabsf(c.bus[0].pan_divergence - 0.05f) < 1e-4f);
    snprintf(path, sizeof path, "%smixer_4d_pan_width.bmp", pre); frame(path);
    for (int i = 0; i < 5; i++) { tap(SDLK_UP); run(1); }          /* Route */
    tap(SDLK_LEFT); run(2);                                        /* sends again */
    apu_bus_get_config(&c);
    CHECK(c.bus[0].route == APU_BUS_ROUTE_SENDS);
    tap(SDLK_ESCAPE); run(3);                                      /* back to the list */
    CHECK(xbox_OverlayOpen());
    /* A/B: B is Factory. */
    tap(SDLK_F7); run(2);
    apu_bus_get_config(&c);
    CHECK(apu_bus_config_is_neutral(&c));
    snprintf(path, sizeof path, "%smixer_5_b.bmp", pre); frame(path);
    tap(SDLK_F7); run(2);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].gain_db == 2.0f && c.bus[0].route == APU_BUS_ROUTE_SENDS);
    /* The file follows, a moment after the last change. */
    SDL_Delay(500); run(3);
    {
        apu_bus_config f;
        char err[160];
        apu_bus_config_neutral(&f);
        CHECK(apu_bus_load_file(file, &f, err, sizeof err) == 0);
        CHECK(f.bus[0].gain_db == 2.0f && f.bus[0].route == APU_BUS_ROUTE_SENDS);
    }
    /* Master rows, the speakers and the duck views. */
    for (int i = 0; i < 13; i++) { tap(SDLK_DOWN); run(1); }       /* Limiter */
    tap(SDLK_RIGHT); tap(SDLK_RIGHT); run(2);
    apu_bus_get_config(&c);
    CHECK(c.limiter == APU_BUS_LIM_PEAK);
    snprintf(path, sizeof path, "%smixer_6_master.bmp", pre); frame(path);
    for (int i = 0; i < 4; i++) { tap(SDLK_DOWN); run(1); }        /* Speakers */
    tap(SDLK_RETURN); run(3);
    snprintf(path, sizeof path, "%smixer_7_speakers.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(2);
    tap(SDLK_DOWN); run(1); tap(SDLK_RETURN); run(3);              /* Ducking */
    tap(SDLK_RETURN); run(2);
    apu_bus_get_config(&c);
    CHECK(c.duck.enabled);
    snprintf(path, sizeof path, "%smixer_8_duck.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(2);
    tap(SDLK_ESCAPE); run(2);                                      /* the Audio page */
    CHECK(xbox_OverlayOpen());
    tap(SDLK_ESCAPE); run(12);
    CHECK(!xbox_OverlayOpen());
    fprintf(stderr, s_fail ? "overlay_shot mixer: %d failed\n" : "overlay_shot mixer: ok\n", s_fail);
    return s_fail ? 1 : 0;
}

/* OVERLAY_SHOT_SAVEIO=<work dir>: the save rows (BO-123) and the Restart
 * row (BO-124), over a made save tree in the work dir: no real profile, no
 * home folder. Nothing is restarted: the row is found and selected, not
 * picked (the request would start this tool again). */
extern "C" {
#include "recomp_save_dir.h"
#include "recomp_restart.h"
#include "xbox_soundtrack.h"
}
#include <sys/stat.h>

static void put_file(const char *root, const char *rel, const char *text)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    FILE *f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void press_a()
{
    pad(SDL_CONTROLLER_BUTTON_A, true);
    pad(SDL_CONTROLLER_BUTTON_A, false);
    run(3);
}

static void press(int button)
{
    pad(button, true);
    pad(button, false);
    run(2);
}

static int saveio_tour(const char *pre, const char *work, int argc, char **argv)
{
    char save[1024], dest[1024], path[1400], found[1024];
    xbox_save_io io;

    snprintf(path, sizeof path, "rm -rf '%s'", work);
    if (system(path) != 0) return 2;
    mkdir(work, 0755);
    snprintf(save, sizeof save, "%s/profile", work);
    snprintf(dest, sizeof dest, "%s/home/Takedawn-saves", work);
    put_file(save, "UDATA/4541005b/TitleMeta.xbx", "meta");
    put_file(save, "UDATA/4541005b/57BD267AFF58/Profile 1", "the profile");
    put_file(save, "TDATA/4541005b/x.dat", "title data");
    io.save_dir = save; io.title_id = "4541005b"; io.export_dir = dest; io.note = "from the overlay_shot tour";
    xbox_OverlaySetSaveIO(&io);
    recomp_restart_init(argc, argv);

    tap(SDLK_F1);
    CHECK(xbox_OverlayOpen());
    press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);                 /* the last page: Advanced */
    run(10);
    CHECK(xbox_OverlayTestHasRow("Export save to home folder"));
    CHECK(xbox_OverlayTestHasRow("Import save from home folder (none found"));
    CHECK(xbox_OverlayTestHasRow("Import save from another folder"));
    /* Nothing waits for a restart: Quit game is the page's last row and there
     * is no Restart row at all (none of its own in the page either). */
    CHECK(!xbox_OverlayTestHasRow("Restart"));
    CHECK(xbox_OverlayTestRowIndex("Quit game") > xbox_OverlayTestRowIndex("Reset all settings"));
    CHECK(!xbox_OverlayTestHasRow("Cancel the import"));

    /* Import with nothing exported: a message, nothing staged. */
    CHECK(xbox_OverlayTestSelectRow("Import save from home folder"));
    press_a();
    CHECK(!recomp_save_import_staged(save, found, sizeof found));
    CHECK(xbox_OverlayTestHasRow("No exported save found"));
    snprintf(path, sizeof path, "%ssave_none.bmp", pre);
    frame(path);

    /* Export. */
    CHECK(xbox_OverlayTestSelectRow("Export save to home folder"));
    press_a();
    CHECK(recomp_save_export_newest(dest, "4541005b", found, sizeof found));
    CHECK(xbox_OverlayTestHasRow("Exported: "));
    CHECK(xbox_OverlayTestHasRow("Import save from home folder (newest: 20"));
    snprintf(path, sizeof path, "%ssave_exported.bmp", pre);
    frame(path);

    /* Import the newest: staged, the rows change, a restart is offered. */
    CHECK(xbox_OverlayTestSelectRow("Import save from home folder"));
    press_a();
    CHECK(recomp_save_import_staged(save, path, sizeof path) && !strcmp(path, found));
    CHECK(xbox_OverlayTestHasRow("Cancel the import"));
    CHECK(xbox_OverlayTestHasRow("Restart now to apply 1 change"));
    CHECK(xbox_OverlayTestHasRow("Import waiting for the next start"));
    CHECK(!xbox_OverlayTestHasRow("Import save from another folder"));
    CHECK(xbox_OverlayTestSelectRow("Restart now"));
    run(3);
    snprintf(path, sizeof path, "%ssave_staged.bmp", pre);
    frame(path);
    /* And on another page too, while it waits. */
    press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    CHECK(xbox_OverlayTestHasRow("Restart now to apply 1 change"));
    press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    /* Cancel. */
    CHECK(xbox_OverlayTestSelectRow("Cancel the import"));
    press_a();
    CHECK(!recomp_save_import_staged(save, path, sizeof path));
    CHECK(!xbox_OverlayTestHasRow("Cancel the import") && xbox_OverlayTestHasRow("Import cancelled"));
    CHECK(!xbox_OverlayTestHasRow("Restart now"));

    /* The cursor keeps its index when the rows change under it. With the
     * cursor on "Import Xbox save..." (the fourth save row) an import gets
     * staged, as the Xbox flow does after its folder browser: the save rows
     * collapse to "Cancel the import" and the index now falls on "Reset all
     * settings to the defaults". One more Enter there must not wipe the
     * settings (settings.toml was rewritten to defaults on a test machine);
     * a reset asks once more first. */
    CHECK(xbox_OverlayTestSelectRow("Import Xbox save"));
    run(2);
    {
        char err[1400] = "";
        CHECK(recomp_save_import_stage(save, found, "4541005b", err, sizeof err));
    }
    run(3);
    CHECK(xbox_OverlayTestRowIndex("Reset all settings") == xbox_OverlayTestRowIndex("Cancel the import") + 2);
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 2);
    run(2);
    press_a();
    CHECK(xbox_SettingGet(XS_GRAPHICS_RENDER_SCALE)->user && xbox_SettingValue(XS_GRAPHICS_RENDER_SCALE) == 2);
    press_a();                                                  /* asked once more: now it does */
    CHECK(!xbox_SettingGet(XS_GRAPHICS_RENDER_SCALE)->user);
    recomp_save_import_cancel(save);
    run(3);
    CHECK(!xbox_OverlayTestHasRow("Cancel the import"));

    /* Another folder: the browser, on the exported saves. */
    CHECK(xbox_OverlayTestSelectRow("Import save from another folder"));
    press_a();
    CHECK(xsb_is_open());
    {
        xsb_row r;
        CHECK(xsb_row_at(0, &r) && !strcmp(r.label, "Exported saves"));
    }
    run(10);
    snprintf(path, sizeof path, "%ssave_browser_places.bmp", pre);
    frame(path);
    press_a();                                                  /* into the exported saves */
    run(15);                                                    /* the marks arrive */
    {
        xsb_row r;
        CHECK(xsb_row_at(0, &r) && r.kind == XSB_USE && !strcmp(r.label, "Import the save in this folder"));
        CHECK(xsb_row_at(2, &r) && r.kind == XSB_DIR && r.songs == 1);
    }
    snprintf(path, sizeof path, "%ssave_browser_list.bmp", pre);
    frame(path);
    /* The folder of saves itself is not a save: refused, with a message. */
    press(SDL_CONTROLLER_BUTTON_DPAD_UP); press(SDL_CONTROLLER_BUTTON_DPAD_UP);
    press_a();
    CHECK(!xsb_is_open() && !recomp_save_import_staged(save, path, sizeof path));
    CHECK(xbox_OverlayTestHasRow("Not imported"));
    /* Into the dated folder, and that one is taken. */
    CHECK(xbox_OverlayTestSelectRow("Import save from another folder"));
    press_a();
    press_a();                                                  /* exported saves */
    press_a();                                                  /* the cursor is on the first folder */
    CHECK(xsb_is_open());
    press(SDL_CONTROLLER_BUTTON_DPAD_UP); press(SDL_CONTROLLER_BUTTON_DPAD_UP);
    press_a();                                                  /* "Import the save in this folder" */
    CHECK(!xsb_is_open() && recomp_save_import_staged(save, path, sizeof path) && !strcmp(path, found));
    /* B in the browser goes back to the page without choosing. */
    CHECK(xbox_OverlayTestSelectRow("Cancel the import"));
    press_a();
    CHECK(xbox_OverlayTestSelectRow("Import save from another folder"));
    press_a();
    press(SDL_CONTROLLER_BUTTON_B);
    xbox_OverlayPoll();
    CHECK(!xsb_is_open() && xbox_OverlayOpen());

    /* The save history (BO-129): the restore row, and the browser opened
     * in the history, newest first. */
    CHECK(!xbox_OverlayTestHasRow("Restore an earlier save"));
    {
        char made[3][1024];
        for (int k = 0; k < 3; k++)
            CHECK(recomp_save_history_take(save, "4541005b", made[k], sizeof made[k]));
        SDL_Delay(2100);                                        /* the count is read every 2 s */
        run(3);
        CHECK(xbox_OverlayTestHasRow("Restore an earlier save (3 kept)..."));
        CHECK(xbox_OverlayTestSelectRow("Restore an earlier save"));
        run(3);
        snprintf(path, sizeof path, "%ssave_restore_row.bmp", pre);
        frame(path);
        press_a();
        run(15);                                                /* the marks arrive */
        CHECK(xsb_is_open());
        {
            xsb_row r;
            const char *newest = strrchr(made[2], '/') + 1;
            CHECK(xsb_row_at(0, &r) && r.kind == XSB_USE && !strcmp(r.label, "Restore the save in this folder"));
            CHECK(xsb_row_at(2, &r) && r.kind == XSB_DIR && !strcmp(r.label, newest) && r.songs == 1);
        }
        snprintf(path, sizeof path, "%ssave_restore_list.bmp", pre);
        frame(path);
        press_a();                                              /* the cursor is on the newest: into it */
        press(SDL_CONTROLLER_BUTTON_DPAD_UP); press(SDL_CONTROLLER_BUTTON_DPAD_UP);
        press_a();                                              /* "Restore the save in this folder" */
        CHECK(!xsb_is_open() && recomp_save_import_staged(save, path, sizeof path)
              && !strcmp(path, made[2]));
        CHECK(xbox_OverlayTestHasRow("Restart now to apply 1 change"));
        CHECK(xbox_OverlayTestSelectRow("Cancel the import"));
        press_a();
    }

    /* A setting the table flags: the one Restart row (at the foot of the
     * page, over Quit game) comes and goes with it, and no page has a Restart
     * row of its own. */
    for (int p = 0; p < 12 && !xbox_OverlayTestHasRow("Aspect ratio"); p++)
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    CHECK(xbox_OverlayTestHasRow("Aspect ratio"));
    CHECK(!xbox_OverlayTestHasRow("Restart"));
    xbox_SettingSet(XS_DISPLAY_ASPECT, 1);
    run(2);
    CHECK(xbox_SettingsNeedRestart() && xbox_OverlayTestHasRow("Restart now to apply 1 change"));
    CHECK(!xbox_OverlayTestHasRow("Restart game"));
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 2);
    run(2);
    CHECK(xbox_OverlayTestHasRow("Restart now to apply 2 changes"));
    CHECK(xbox_OverlayTestSelectRow("Restart now"));
    run(3);
    snprintf(path, sizeof path, "%srestart_row.bmp", pre);
    frame(path);
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 1);
    xbox_SettingSet(XS_DISPLAY_ASPECT, 0);
    run(2);
    CHECK(!xbox_OverlayTestHasRow("Restart now"));
    CHECK(!recomp_restart_requested());

    /* "Reset this page" asks once more as well. */
    xbox_SettingSet(XS_DISPLAY_ASPECT, 1);
    CHECK(xbox_OverlayTestSelectRow("Reset this page"));
    run(2);
    press_a();
    CHECK(xbox_SettingGet(XS_DISPLAY_ASPECT)->user && xbox_SettingValue(XS_DISPLAY_ASPECT) == 1);
    press_a();
    CHECK(!xbox_SettingGet(XS_DISPLAY_ASPECT)->user);
    CHECK(!recomp_restart_requested());

    /* Restart now and Quit game: on every page while a change waits, each
     * asks once more; with nothing waiting only Quit game is there. */
    for (int p = 0; p < 6; p++) {
        CHECK(!xbox_OverlayTestHasRow("Restart") && xbox_OverlayTestHasRow("Quit game"));
        CHECK(xbox_OverlayTestRowIndex("Quit game") > xbox_OverlayTestRowIndex("Reset "));
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    }
    xbox_SettingSet(XS_DISPLAY_ASPECT, 1);
    run(2);
    for (int p = 0; p < 6; p++) {
        CHECK(xbox_OverlayTestHasRow("Restart now to apply 1 change") && xbox_OverlayTestHasRow("Quit game"));
        CHECK(xbox_OverlayTestRowIndex("Quit game") == xbox_OverlayTestRowIndex("Restart now") + 1);
        CHECK(xbox_OverlayTestRowIndex("Restart now") > xbox_OverlayTestRowIndex("Reset "));
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    }
    press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);                  /* the Advanced page */
    CHECK(xbox_OverlayTestSelectRow("Restart now"));
    run(3);
    snprintf(path, sizeof path, "%squitrow.bmp", pre);
    frame(path);
    press_a();                                                  /* the question: nothing yet */
    CHECK(xbox_OverlayOpen() && !recomp_restart_requested() && !SDL_HasEvent(SDL_QUIT));
    snprintf(path, sizeof path, "%squitrow_confirm.bmp", pre);
    frame(path);
    press(SDL_CONTROLLER_BUTTON_B);                             /* B takes it back and stays */
    xbox_OverlayPoll();
    CHECK(xbox_OverlayOpen() && !recomp_restart_requested());
    press_a();                                                  /* asked again: it does not remember */
    CHECK(xbox_OverlayOpen() && !recomp_restart_requested());
    press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);                     /* off the row: cancelled */
    press(SDL_CONTROLLER_BUTTON_DPAD_UP);
    press_a();
    CHECK(xbox_OverlayOpen() && !recomp_restart_requested());
    press_a();                                                  /* and the same row again does it */
    CHECK(!xbox_OverlayOpen() && recomp_restart_requested() && SDL_HasEvent(SDL_QUIT));
    recomp_restart_cancel();                                    /* or this tool would start again */
    SDL_FlushEvent(SDL_QUIT);
    xbox_SettingSet(XS_DISPLAY_ASPECT, 0);

    tap(SDLK_F1);
    CHECK(xbox_OverlayOpen());
    CHECK(xbox_OverlayTestSelectRow("Quit game"));
    run(3);
    press_a();
    CHECK(xbox_OverlayOpen() && !SDL_HasEvent(SDL_QUIT));
    press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);                 /* another page: cancelled */
    press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    press_a();
    CHECK(xbox_OverlayOpen() && !SDL_HasEvent(SDL_QUIT));
    press_a();
    CHECK(!xbox_OverlayOpen() && SDL_HasEvent(SDL_QUIT) && !recomp_restart_requested());
    SDL_FlushEvent(SDL_QUIT);

    fprintf(stderr, "overlay_shot save and restart rows: %s\n", s_fail ? "FAILED" : "ok");
    xbox_OverlayShutdown();
    return s_fail ? 1 : 0;
}

/* OVERLAY_SHOT_UPDATE=<owner/repo>: the Advanced page's "Check for updates"
 * (xbox_update.h). The row is pressed once, against the real GitHub (one
 * request), and the answer is shot. OVERLAY_SHOT_UPDATE_EXPECT is none (no
 * release published yet), newer, latest or error; OVERLAY_SHOT_UPDATE_VERSION
 * is the running version (default 0.1.0). The download row is not pressed:
 * that would start a browser. */
static int update_tour(const char *pre, const char *repo)
{
    char path[1400];
    const char *expect = getenv("OVERLAY_SHOT_UPDATE_EXPECT");
    const char *ver = getenv("OVERLAY_SHOT_UPDATE_VERSION");
    if (!ver) ver = "0.1.0";
    xbox_OverlaySetAbout("Takedawn 0.1.0 \"Lazy Generation\"  (game 0123abc, toolkit 4567def)");
    xbox_OverlaySetUpdateCheck(repo, ver);

    tap(SDLK_F1);
    CHECK(xbox_OverlayOpen());
    press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);                 /* the last page: Advanced */
    run(10);
    CHECK(xbox_OverlayTestHasRow("Check for updates"));
    CHECK(xbox_OverlayTestRowIndex("Check for updates") == xbox_OverlayTestRowIndex("Takedawn 0.1.0") + 1);
    CHECK(!xbox_OverlayTestHasRow("Open the download page"));
    CHECK(xbox_OverlayTestSelectRow("Check for updates"));
    run(3);
    snprintf(path, sizeof path, "%supdate_before.bmp", pre);
    frame(path);
    press_a();
    for (int i = 0; i < 400 && xbox_OverlayTestHasRow("Checking for updates"); i++) {
        run(1);
        SDL_Delay(30);
    }
    run(5);
    CHECK(xbox_OverlayTestHasRow("Check for updates"));
    if (!expect || !strcmp(expect, "none")) {
        CHECK(xbox_OverlayTestHasRow("No release published yet"));
    } else if (!strcmp(expect, "newer")) {
        CHECK(xbox_OverlayTestHasRow("Version "));
        CHECK(xbox_OverlayTestHasRow("Open the download page"));
    } else if (!strcmp(expect, "latest")) {
        CHECK(xbox_OverlayTestHasRow("You have the latest version"));
    } else {
        CHECK(xbox_OverlayTestHasRow("Could not check: "));
    }
    snprintf(path, sizeof path, "%supdate_%s.bmp", pre, expect ? expect : "none");
    frame(path);
    pad(SDL_CONTROLLER_BUTTON_B, true);
    pad(SDL_CONTROLLER_BUTTON_B, false);
    xbox_OverlayPoll();
    CHECK(!xbox_OverlayOpen());
    fprintf(stderr, "overlay_shot update check (%s): %s\n", expect ? expect : "none", s_fail ? "FAILED" : "ok");
    xbox_OverlayShutdown();
    return s_fail ? 1 : 0;
}

/* The music folder row's port side, stood in for: a status line and two
 * notes, nothing chosen. */
static void music_status(char *out, unsigned n) { snprintf(out, n, "2 soundtracks, 34 songs: in the game's music menu."); }
static void music_chosen(const char *dir) { (void)dir; }
static int music_note(int i, char *out, unsigned n)
{
    if (i > 0)
        return 0;
    snprintf(out, n, "  one.mp3: no MPEG audio found in its first megabyte");
    return 1;
}
static const xbox_music_folder_ops k_music_ops = { music_status, music_chosen, music_note };

/* OVERLAY_SHOT_GAME_FONT="<executable>;<size>;<sha256>;<table>,<count>,<tex>,<w>,<h>;<same for the
 * title font>": the title's fonts, as a port names them (the numbers are the
 * port's; nothing of a title is in this tool). Waits until they are in use
 * or refused; OVERLAY_SHOT_GAME_FONT_EXPECT=0 expects a refusal. */
static void game_font_from_env(void)
{
    const char *e = getenv("OVERLAY_SHOT_GAME_FONT");
    if (!e || !e[0])
        return;
    char path[1024], sha[80];
    unsigned long long size = 0;
    xbox_overlay_bitmap_font f[2];
    memset(f, 0, sizeof f);
    const char *p = strchr(e, ';');
    if (!p || (size_t)(p - e) >= sizeof path) { CHECK(!"OVERLAY_SHOT_GAME_FONT"); return; }
    memcpy(path, e, (size_t)(p - e));
    path[p - e] = 0;
    if (sscanf(p + 1, "%llu;%64[0-9a-fA-F];%x,%u,%x,%u,%u;%x,%u,%x,%u,%u", &size, sha,
               &f[0].table_off, &f[0].count, &f[0].tex_off, &f[0].w, &f[0].h,
               &f[1].table_off, &f[1].count, &f[1].tex_off, &f[1].w, &f[1].h) != 12) {
        CHECK(!"OVERLAY_SHOT_GAME_FONT");
        return;
    }
    xbox_OverlaySetGameFonts(path, size, sha, &f[0], &f[1]);
    const char *x = getenv("OVERLAY_SHOT_GAME_FONT_EXPECT");
    const int want = x && x[0] == '0' ? -1 : 1;
    const int got = xbox_OverlayGameFontsState(10000);
    fprintf(stderr, "game font: %d\n", got);
    CHECK(got == want);
}

/* The title's menu sprites (xbox_OverlaySetGameArt) and its prompt sheets.
 * OVERLAY_SHOT_GAME_ART="<file>;<size>;<sha256>;<off>,<w>,<h>,<dxt>;..." with one
 * <off>,<w>,<h>,<dxt> for each of the nine sprites of the enum, as a port
 * names them; OVERLAY_SHOT_GAME_ART_EXPECT=0 expects a refusal.
 * OVERLAY_SHOT_PAD_SHEETS=<dir> holds set1.argb ... set8.argb, and set9.argb when
 * the port has a ninth (128x64 words A8R8G8B8, the port's drawings for glyph
 * sets 1..9; the ninth is the keyboard's key caps); set 5 replaces only the
 * lower rectangles (the Xbox pads print A B X Y as the title does), the others
 * the whole sheet, as a port's table does. OVERLAY_SHOT_GLYPHS=<mode> sets
 * input.glyphs (xbox_modern, playstation ...). Nothing of a title is in this tool. */
static uint32_t s_pad_img[9][128 * 64];
static const nv2a_texrep_rect k_pad_all[] = { { 0, 0, 128, 64 } };
static const nv2a_texrep_rect k_pad_lower[] = { { 0, 30, 62, 34 }, { 62, 29, 18, 35 }, { 80, 29, 48, 35 } };
static struct nv2a_texrep_variant s_pad_var[9];
static void game_art_from_env(void)
{
    const char *g = getenv("OVERLAY_SHOT_GLYPHS");
    if (g && g[0])
        xbox_InputSetGlyphMode(xbox_GlyphModeFromName(g));
    const char *dir = getenv("OVERLAY_SHOT_PAD_SHEETS");
    if (dir && dir[0]) {
        int n = 0;
        for (int i = 0; i < 9; i++) {
            char path[1100];
            snprintf(path, sizeof path, "%s/set%d.argb", dir, i + 1);
            FILE *f = fopen(path, "rb");
            if (!f && i == 8)
                break;                       /* sets 1..8 are all an older port had */
            const bool ok = f && fread(s_pad_img[i], sizeof s_pad_img[i], 1, f) == 1;
            if (f) fclose(f);
            CHECK(ok);
            s_pad_var[i].image = s_pad_img[i];
            s_pad_var[i].rects = i == 4 ? k_pad_lower : k_pad_all;
            s_pad_var[i].n_rects = i == 4 ? 3 : 1;
            n++;
        }
        xbox_OverlaySetPadSheets(s_pad_var, n);
    }
    const char *e = getenv("OVERLAY_SHOT_GAME_ART");
    if (!e || !e[0])
        return;
    char path[1024], sha[80];
    unsigned long long size = 0;
    xbox_overlay_sprite sp[XBOX_OVERLAY_ART_COUNT];
    memset(sp, 0, sizeof sp);
    const char *p = strchr(e, ';');
    if (!p || (size_t)(p - e) >= sizeof path) { CHECK(!"OVERLAY_SHOT_GAME_ART"); return; }
    memcpy(path, e, (size_t)(p - e));
    path[p - e] = 0;
    int used = 0;
    if (sscanf(p + 1, "%llu;%64[0-9a-fA-F]%n", &size, sha, &used) != 2) { CHECK(!"OVERLAY_SHOT_GAME_ART"); return; }
    p += 1 + used;
    for (int i = 0; i < XBOX_OVERLAY_ART_COUNT; i++) {
        int n = 0;
        if (sscanf(p, ";%x,%u,%u,%u%n", &sp[i].off, &sp[i].w, &sp[i].h, &sp[i].dxt, &n) != 4) {
            CHECK(!"OVERLAY_SHOT_GAME_ART");
            return;
        }
        p += n;
    }
    xbox_OverlaySetGameArt(path, size, sha, sp);
    const char *x = getenv("OVERLAY_SHOT_GAME_ART_EXPECT");
    const int want = x && x[0] == '0' ? -1 : 1;
    const int got = xbox_OverlayGameArtState(20000);
    fprintf(stderr, "game art: %d\n", got);
    CHECK(got == want);
}

/* The folder browser on folders named in Hungarian, Russian and Greek:
 * the game's letters where it has them, the fallback's for the rest. */
static void browser_utf8_shot(const char *pre)
{
    char root[1024], d[1200], path[1300];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/overlay_shot_utf8_%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    mkdir(root, 0755);
    static const char *const names[] = { "Zenék – őszi válogatás", "Музыка для гонок", "Ελληνικά τραγούδια",
                                         "Rock (Ünnepi)" };
    for (const char *n : names) {
        snprintf(d, sizeof d, "%s/%s", root, n);
        mkdir(d, 0755);
        snprintf(path, sizeof path, "%s/01 – első szám.mp3", d);
        FILE *f = fopen(path, "wb");
        if (f) fclose(f);
    }
    xsb_open(root);
    CHECK(xsb_is_open());
    run(2);
    CHECK(xbox_OverlayOpen());
    tap(SDLK_DOWN);                                  /* "Current folder: ..." */
    run(1);
    tap(SDLK_RETURN);                                /* into it: the four folders */
    run(3);
    {
        int found = 0;
        xsb_row r;
        for (int i = 0; xsb_row_at(i, &r); i++)
            found += !strcmp(r.label, "Zenék – őszi válogatás") || !strcmp(r.label, "Музыка для гонок");
        CHECK(found == 2);
    }
    snprintf(path, sizeof path, "%sbrowser_utf8.bmp", pre);
    frame(path);
    xsb_close();
    run(1);
}

/* Two cheats as a port registers them, for the Advanced page's rows (one
 * shown on); the save root a temporary folder. */
extern "C" void xbox_path_init(const char *game_dir, const char *save_dir);
static int s_cheat[2] = { 0, 1 };
static int cheat0_get(void) { return s_cheat[0]; }
static void cheat0_set(int on) { s_cheat[0] = on; }
static int cheat1_get(void) { return s_cheat[1]; }
static void cheat1_set(int on) { s_cheat[1] = on; }
static const xbox_cheat k_cheats[2] = {
    { "Unlock everything", "Every event, car and track opens for this session.", cheat0_get, cheat0_set },
    { "Endless boost", "The boost bar never empties.", cheat1_get, cheat1_set },
};

int main(int argc, char **argv)
{
    const char *pre = argc > 1 ? argv[1] : "overlay_";
    char path[512];

    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("RECOMP_SETTINGS", "none", 1);
    setenv("RECOMP_OVERLAY_PERF", "1", 1);
    xbox_InputOfferControlScheme(1);     /* a title with a second driving layout: the Input page shows the row */
    xbox_SettingsInit("Test");
    xbox_SettingsFinish();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 2; }
    SDL_Window *win = SDL_CreateWindow("shot", 0, 0, W, H, 0);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 2; }
    dev = MTLCreateSystemDefaultDevice();
    q = [dev newCommandQueue];
    xbox_OverlaySetWindow(win);
    game_font_from_env();
    game_art_from_env();
    xbox_BugReportSetMaker(fake_report);
    /* A port with a music folder (BO-108): its row is the Audio page's
     * first, and every tour below goes down that page as a port has it. */
    xbox_OverlaySetMusicFolder(&k_music_ops);
    if (getenv("OVERLAY_SHOT_MIXER_E2E"))
        return mixer_e2e(getenv("OVERLAY_SHOT_MIXER_E2E"));
    if (getenv("OVERLAY_SHOT_MIXER"))
        return mixer_tour(pre, getenv("OVERLAY_SHOT_MIXER"));
    if (getenv("OVERLAY_SHOT_UPDATE"))
        return update_tour(pre, getenv("OVERLAY_SHOT_UPDATE"));
    if (getenv("OVERLAY_SHOT_SAVEIO"))
        return saveio_tour(pre, getenv("OVERLAY_SHOT_SAVEIO"), argc, argv);

    recomp_restart_init(argc, argv);     /* the host can restart: the pending-restart row shows */
    {
        char root[1024];
        const char *tmp = getenv("TMPDIR");
        snprintf(root, sizeof root, "%s/overlay_shot_root_%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        mkdir(root, 0755);
        xbox_path_init(root, root);              /* the save root the cheat rows wait for */
        xbox_OverlaySetCheats(k_cheats, 2);
    }

    /* Flips for the graph: mostly 60 Hz, a few long ones. */
    for (int i = 0; i < 150; i++) {
        xbox_HostPauseHold();
        SDL_Delay(i % 37 == 5 ? 33 : i % 53 == 9 ? 24 : 16);
    }
    run(3);
    snprintf(path, sizeof path, "%sclosed_perf.bmp", pre);
    frame(path);

    CHECK(!xbox_HostPaused() && !xbox_InputBlocked());
    tap(SDLK_F1);
    CHECK(xbox_OverlayOpen() && xbox_HostPaused() && xbox_InputBlocked());
    run(12);
    snprintf(path, sizeof path, "%sdisplay.bmp", pre);
    frame(path);

    tap(SDLK_DOWN); tap(SDLK_DOWN); tap(SDLK_RIGHT);          /* filter: linear */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_FILTER) == 1);
    tap(SDLK_DOWN); tap(SDLK_RETURN);                         /* overscan: crop */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1);
    tap(SDLK_UP); tap(SDLK_UP); tap(SDLK_RIGHT);  /* aspect: 16:9 (restart) */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_ASPECT) == 1 && xbox_SettingsNeedRestart());
    snprintf(path, sizeof path, "%sdisplay2.bmp", pre);
    frame(path);

    for (int p = 1; p < 6; p++) {
        pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
        pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
        run(3);
        if (p == 2) {                                         /* audio */
            /* The music folder's row is the first, its lines under it, the
             * settings after them in the order they always had. */
            CHECK(xbox_OverlayTestRowIndex("Custom music folder") == 0);
            CHECK(xbox_OverlayTestRowIndex("2 soundtracks") == 1);
            CHECK(xbox_OverlayTestRowIndex("  one.mp3") == 2);
            CHECK(xbox_OverlayTestRowIndex("Custom music loudness") == 3);
            CHECK(xbox_OverlayTestRowIndex("Master volume") == 4);
            CHECK(xbox_OverlayTestRowIndex("Mute") == 5);
            CHECK(xbox_OverlayTestRowIndex("Speakers") > xbox_OverlayTestRowIndex("Mute in the background"));
            CHECK(xbox_OverlayTestRowIndex("Reset this page") > xbox_OverlayTestRowIndex("Speaker test, direct"));
            snprintf(path, sizeof path, "%saudio_top.bmp", pre);
            frame(path);
            CHECK(xbox_OverlayTestSelectRow("Master volume"));
            run(2);
            pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT, false);
            run(2);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);   /* speakers */
            run(2);
            for (int i = 0; i < 3; i++) {
                pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, true);
                pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, false);
                run(1);
            }
            CHECK(xbox_SettingValue(XS_AUDIO_SPEAKERS) != 2);   /* 5.1 cannot be picked */
        }
        CHECK(xbox_OverlayTestHasRow("Quit game"));           /* the foot of every page */
        if (p == 1) {                                         /* graphics: a second change waits */
            xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 2);
            run(2);
            CHECK(xbox_OverlayTestHasRow("Restart now to apply 2 changes"));
        } else {
            CHECK(xbox_OverlayTestHasRow("Restart now to apply "));
            CHECK(!xbox_OverlayTestHasRow("Restart game"));
        }
        snprintf(path, sizeof path, "%spage%d.bmp", pre, p);
        frame(path);
        if (p == 5) {                                         /* advanced: the bug report */
            /* By its text: the rows above it have grown since this was
             * written (it used to be one step down). */
            CHECK(xbox_OverlayTestSelectRow("Create bug report"));
            run(2);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_reports == 1 && s_report_shot == 0);
            snprintf(path, sizeof path, "%sreport.bmp", pre);
            frame(path);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            run(2);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_reports == 2 && s_report_shot == 1);
            /* The cheat rows are a tester's: none until RECOMP_DEBUG=cheats. */
            CHECK(!xbox_OverlayTestHasRow("Unlock everything") && !xbox_OverlayTestHasRow("Cheats"));
            recomp_env_set(RENV_CHEATS, "1");
            run(2);
            /* The cheats: label and state as a setting's row; A once shows
             * the warning and asks again. */
            CHECK(xbox_OverlayTestSelectRow("Unlock everything"));
            run(2);
            snprintf(path, sizeof path, "%scheats.bmp", pre);
            frame(path);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_cheat[0] == 0);                           /* asked, not yet on */
            snprintf(path, sizeof path, "%scheats_confirm.bmp", pre);
            frame(path);
            /* On, then both off: the question; "No" throws it away, and
             * the page and the foot say saving waits for a restart. */
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_cheat[0] == 1 && xbox_OverlayTestHasRow("Quit game (progress made with cheats is not saved)"));
            CHECK(xbox_OverlayTestSelectRow("Unlock everything"));
            run(1);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(1);
            CHECK(xbox_OverlayTestSelectRow("Endless boost"));
            run(1);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_cheat[0] == 0 && s_cheat[1] == 0 && xbox_OverlayTestHasRow("Yes: keep it in my save"));
            CHECK(!xbox_OverlayTestHasRow("Progress is not being saved until you restart."));
            CHECK(xbox_OverlayTestSelectRow("No: throw it away"));
            run(1);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(!xbox_OverlayTestHasRow("Yes: keep it in my save"));
            CHECK(xbox_OverlayTestHasRow("Progress is not being saved until you restart."));
            CHECK(xbox_OverlayTestHasRow("Restart now to apply 3 changes"));
            CHECK(xbox_OverlayTestHasRow("Quit game (progress is not being saved until a restart)"));
            snprintf(path, sizeof path, "%scheats_discarded.bmp", pre);
            frame(path);
            pad(SDL_CONTROLLER_BUTTON_DPAD_UP, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
            run(1);
        }
    }
    CHECK(xbox_SettingValue(XS_AUDIO_VOLUME) < 1.0);
    browser_utf8_shot(pre);

    /* B closes: the title is let go. */
    pad(SDL_CONTROLLER_BUTTON_B, true);
    pad(SDL_CONTROLLER_BUTTON_B, false);
    xbox_OverlayPoll();
    CHECK(!xbox_OverlayOpen() && !xbox_HostPaused() && !xbox_InputBlocked());
    run(10);
    snprintf(path, sizeof path, "%sclosed.bmp", pre);
    frame(path);

    /* Back + Start opens it; Esc closes. */
    pad(SDL_CONTROLLER_BUTTON_BACK, true);
    CHECK(!xbox_OverlayOpen());
    pad(SDL_CONTROLLER_BUTTON_START, true);
    CHECK(xbox_OverlayOpen() && xbox_HostPaused());
    pad(SDL_CONTROLLER_BUTTON_START, false);
    pad(SDL_CONTROLLER_BUTTON_BACK, false);
    tap(SDLK_ESCAPE);
    xbox_OverlayPoll();
    CHECK(!xbox_OverlayOpen() && !xbox_HostPaused());
    /* Cmd+, opens it. */
    key(SDLK_COMMA, KMOD_LGUI);
    CHECK(xbox_OverlayOpen());
    key(SDLK_F1, 0, false);
    tap(SDLK_F1);
    CHECK(!xbox_OverlayOpen());

    fprintf(stderr, "overlay_shot: %s\n", s_fail ? "FAILED" : "ok");
    xbox_OverlayShutdown();
    return s_fail ? 1 : 0;
}

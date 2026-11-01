/*
 * xbox_settings.c - the overlay's settings and settings.toml; see xbox_settings.h.
 */
#include "xbox_settings.h"

#include "recomp_cfg.h"
#include "recomp_save_dir.h"
#include "input_backend.h"
#include "input_map.h"
#include "pad_glyphs.h"
#include "touch_pad.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"
#include "nv2a_texscale.h"
#include "nv2a_texup.h"
#include "apu_xaudio2.h"
#include "apu.h"                /* mcpx_apu_set_fx_level */
#include "apu_bus.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define sx_strcasecmp _stricmp
/* Set unless already set: what the title's env_default does. */
static void sx_setenv_keep(const char *n, const char *v)
{
    if (!getenv(n))
        _putenv_s(n, v);
}
#else
#include <strings.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
#define sx_strcasecmp strcasecmp
#define sx_setenv_keep(n, v) setenv((n), (v), 0)
#endif

#define LOGP "[SETTINGS] "

static const char *const k_aspects[]  = { "4:3", "16:9", NULL };
static const char *const k_aspect_env[] = { "0", "1", NULL };
static const char *const k_filters[]  = { "nearest", "linear", "integer", NULL };
static const char *const k_overscan[] = { "off", "crop", "fill", NULL };
static const char *const k_fits[]     = { "letterbox", "fill16x10", NULL };
static const char *const k_speakers[] = { "auto", "stereo", "5.1", NULL };
#if defined(__ANDROID__)
static const char *const k_scale[]    = { "1", "1.5", "2", "3", "4", NULL };
#endif
static const char *const k_aniso[]    = { "off", "2x", "4x", "8x", "16x", NULL };
static const char *const k_aniso_env[] = { "1", "2", "4", "8", "16", NULL };
static const char *const k_ui_scale[] = { "off", "2x", "4x", NULL };
static const char *const k_texup[]    = { "off", "fsrcnnx", "nnedi3", NULL };
static const char *const k_texup_sc[] = { "2x", "4x", NULL };
static const char *const k_lod[]      = { "off", "auto", "-0.5", "-1", "-1.5", NULL };
static const char *const k_lod_env[]  = { "0", "auto", "-0.5", "-1", "-1.5", NULL };
static const char *const k_aa[]       = { "off", "fxaa", "smaa", NULL };
static const char *const k_renderer[] = { "standard", "native", NULL };
static const char *const k_renderer_env[] = { "vulkan", "native", NULL };
static const char *const k_units[]    = { "mph", "kmh", NULL };
static const char *const k_face[]     = { "xbox", "nintendo", NULL };
static const char *const k_scheme[]   = { "xbox", "ps2", NULL };
static const char *const k_first_pad[] = { "player1", "player2", NULL };
static const char *const k_glyphs[]   = { "auto", "xbox", "xbox_modern", "playstation", "playstation5", "nintendo", "steam", "positional", "keyboard", NULL };
_Static_assert(sizeof k_glyphs / sizeof k_glyphs[0] == XBOX_GLYPHS_MODE_COUNT + 1, "one choice per glyph mode, in pad_glyphs.h order");
#if defined(__ANDROID__)
static const char *const k_vibration[] = { "auto", "controller", "device", "off", NULL };
static const char *const k_touch[]     = { "auto", "always", "off", NULL };
#endif
static const char *const k_ultrawide[] = { "off", "21:9", "32:9", "window", NULL };
static const char *const k_vehicle_detail[] = { "original", "far", "farthest", NULL };
static const char *const k_scenery_detail[] = { "original", "far", NULL };
static const char *const k_score_board[] = { "original", "smooth", NULL };
static const char *const k_rival_marker[] = { "original", "farther", NULL };
static const char *const k_crashnav_map[] = { "stretched", "native", NULL };
static const char *const k_power[]    = { "stock", "performance", "battery", "auto", NULL };
static const char *const k_lfe_lowpass[] = { "off", "80", "100", "120", NULL };
static const char *const k_timing[]   = { "legacy", "auto", NULL };
#if defined(__APPLE__)
#define XS_TIMING_BUILTIN 1   /* auto (BO-239) */
#else
#define XS_TIMING_BUILTIN 0
#endif
static const char *const k_frame_pacing[] = { "auto", "low_latency", "smooth", NULL };
static const char *const k_custom_level[] = { "off", "quieter", "match", "louder", NULL };
static const char *const k_hdr_decode[] = { "2.2", "srgb", NULL };
/* XC_LANGUAGE 1..9 (kernel.h xbox_language_names): the kernel reads the name. */
static const char *const k_language[] = { "english", "japanese", "german", "french", "spanish", "italian", "korean", "chinese", "portuguese", NULL };

/* Designated, so the state half of the struct starts zero. */
#define SETTING(k, t, f, e, c, ev, mn, mx, b) \
    { .key = (k), .type = (t), .flags = (f), .env = (e), .choices = (c), \
      .env_vals = (ev), .min = (mn), .max = (mx), .builtin = (b) }
#define SETTING_STEP(k, t, f, e, mn, mx, b, st) \
    { .key = (k), .type = (t), .flags = (f), .env = (e), \
      .min = (mn), .max = (mx), .builtin = (b), .step = (st) }
static xs_setting s_set[XS_COUNT] = {
    [XS_DISPLAY_FULLSCREEN] = SETTING("display.fullscreen", XS_BOOL, 0, RENV_PRESENT_FULLSCREEN, NULL, NULL, 0, 0, 0),
    [XS_DISPLAY_ASPECT]     = SETTING("display.aspect", XS_CHOICE, XS_RESTART, RENV_DISPLAY_WIDESCREEN, k_aspects, k_aspect_env, 0, 0, 0),
    [XS_DISPLAY_FILTER]     = SETTING("display.filter", XS_CHOICE, 0, RENV_PRESENT_FILTER, k_filters, NULL, 0, 0, 0),
    [XS_DISPLAY_OVERSCAN]   = SETTING("display.overscan", XS_CHOICE, 0, RENV_PRESENT_OVERSCAN, k_overscan, NULL, 0, 0, 0),
    [XS_DISPLAY_FIT]        = SETTING("display.fit", XS_CHOICE, 0, RENV_PRESENT_FIT, k_fits, NULL, 0, 0, 0),
#if defined(__ANDROID__)     /* with 1.5 (the Vulkan backend's half step): a choice there */
    [XS_GRAPHICS_RENDER_SCALE] = SETTING("graphics.render_scale", XS_CHOICE, XS_RESTART, RENV_RENDER_SCALE, k_scale, k_scale, 0, 0, 0),
#else
    [XS_GRAPHICS_RENDER_SCALE] = SETTING("graphics.render_scale", XS_INT, XS_RESTART, RENV_RENDER_SCALE, NULL, NULL, 1, 4, 1),
#endif
    [XS_GRAPHICS_ANISO]     = SETTING("graphics.aniso", XS_CHOICE, 0, RENV_GFX_ANISO, k_aniso, k_aniso_env, 0, 0, 0),
    [XS_GRAPHICS_AA]        = SETTING("graphics.aa", XS_CHOICE, 0, RENV_GFX_AA, k_aa, NULL, 0, 0, 0),
    [XS_GRAPHICS_SHARPEN]   = SETTING("graphics.sharpen", XS_FLOAT, 0, RENV_GFX_SHARPEN, NULL, NULL, 0, 1, 0),
    [XS_GRAPHICS_RENDERER]  = SETTING("graphics.renderer", XS_CHOICE, XS_RESTART, RENV_PB_BACKEND, k_renderer, k_renderer_env, 0, 0, 0),
    [XS_AUDIO_VOLUME]       = SETTING("audio.volume", XS_FLOAT, 0, RENV_AUDIO_VOLUME, NULL, NULL, 0, 1, 1),
    [XS_AUDIO_MUTE]         = SETTING("audio.mute", XS_BOOL, 0, RENV_AUDIO_MUTE, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_MUTE_BG]      = SETTING("audio.mute_background", XS_BOOL, 0, RENV_AUDIO_MUTE_BG, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_SPEAKERS]     = SETTING("audio.speakers", XS_CHOICE, XS_RESTART, RENV_AUDIO_SPEAKERS, k_speakers, NULL, 0, 0, 0),
    [XS_AUDIO_FX_LEVEL]     = SETTING("audio.fx_level", XS_FLOAT, 0, RENV_AUDIO_FX_LEVEL, NULL, NULL, 0, 1, 0.25),
    [XS_AUDIO_MIXER]        = SETTING("audio.mixer", XS_BOOL, 0, RENV_AUDIO_MIXER, NULL, NULL, 0, 0, 0),
    [XS_INPUT_DEADZONE]     = SETTING("input.deadzone", XS_INT, 0, RENV_PAD_DEADZONE, NULL, NULL, 0, 32767, 0),
    [XS_INPUT_TRIGGER_DEADZONE] = SETTING("input.trigger_deadzone", XS_INT, 0, RENV_PAD_TRIGGER_DEADZONE, NULL, NULL, 0, 254, 0),
    /* 0..3 in quarters: the rumble curve's strength, motor = (speed /
     * peak) ^ (1 / strength) (input_core.c, rumble_curve.h). 1 is the
     * console's own, 0 off; a weak motor (a handheld's own vibrator) wants
     * more than a pad's. */
    [XS_INPUT_RUMBLE]       = SETTING_STEP("input.rumble", XS_FLOAT, 0, RENV_INPUT_RUMBLE_GAIN, 0, 3, 1, 0.25),
    [XS_INPUT_FACE_LAYOUT]  = SETTING("input.face_layout", XS_CHOICE, 0, RENV_INPUT_FACE_LAYOUT, k_face, NULL, 0, 0, 0),
    [XS_INPUT_CONTROL_SCHEME] = SETTING("input.control_scheme", XS_CHOICE, 0, RENV_INPUT_CONTROL_SCHEME, k_scheme, NULL, 0, 0, 0),
    [XS_INPUT_FIRST_PAD]    = SETTING("input.first_pad", XS_CHOICE, 0, RENV_INPUT_FIRST_PAD, k_first_pad, NULL, 0, 0, 0),
    [XS_POWER_MODE]         = SETTING("power.mode", XS_CHOICE, XS_VIRTUAL, RENV_COUNT, k_power, NULL, 0, 0, 0),
    [XS_ADVANCED_PERF]      = SETTING("advanced.perf_overlay", XS_BOOL, 0, RENV_OVERLAY_PERF, NULL, NULL, 0, 0, 0),
    [XS_GAME_SPEED_UNITS]   = SETTING("game.speed_units", XS_CHOICE, 0, RENV_SPEED_UNITS, k_units, NULL, 0, 0, 0),
    [XS_DISPLAY_HUD_FIX]    = SETTING("display.hud_fix", XS_BOOL, 0, RENV_GFX_HUD_FIX, NULL, NULL, 0, 0, 0),
    [XS_DISPLAY_ULTRAWIDE]  = SETTING("display.ultrawide", XS_CHOICE, 0, RENV_DISPLAY_ULTRAWIDE, k_ultrawide, NULL, 0, 0, 0),
    /* The title's own effects, full strength as shipped (BO-101, BO-212):
     * 0..1 in tenths, 1 the original, 0 not drawn. They were switches: a
     * file's or variable's true / false still reads as 1 / 0. */
    [XS_GRAPHICS_BLOOM]     = SETTING_STEP("graphics.bloom", XS_FLOAT, XS_FROM_BOOL, RENV_GFX_BLOOM, 0, 1, 1, 0.1),
    [XS_GRAPHICS_MOTION_BLUR] = SETTING_STEP("graphics.motion_blur", XS_FLOAT, XS_FROM_BOOL, RENV_GFX_MOTION_BLUR, 0, 1, 1, 0.1),
    /* The title's crash and takedown cameras at 60 instead of its own 30: the
     * patch is the "1". */
    [XS_GRAPHICS_CRASH_60FPS] = SETTING("graphics.crash_60fps", XS_BOOL, 0, RENV_GFX_CRASH_60FPS, NULL, NULL, 0, 0, 0),
    /* Split screen at 60 instead of the title's 30, and with the title's
     * music, which it keeps silent there: the patch is the "1" of each. */
    [XS_GRAPHICS_SPLIT_60FPS] = SETTING("graphics.split_60fps", XS_BOOL, 0, RENV_GFX_SPLIT_60FPS, NULL, NULL, 0, 0, 0),
    [XS_GRAPHICS_HIGH_REFRESH] = SETTING("graphics.high_refresh", XS_BOOL, 0, RENV_GFX_HIGH_REFRESH, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_SPLIT_MUSIC]  = SETTING("audio.split_music", XS_BOOL, 0, RENV_AUDIO_SPLIT_MUSIC, NULL, NULL, 0, 0, 0),
    /* Both off as shipped: today's picture, bit for bit. */
    [XS_GRAPHICS_LOD_BIAS]  = SETTING("graphics.lod_bias", XS_CHOICE, 0, RENV_GFX_LOD_BIAS, k_lod, k_lod_env, 0, 0, 0),
    [XS_GRAPHICS_DITHER]    = SETTING("graphics.dither", XS_BOOL, 0, RENV_GFX_DITHER, NULL, NULL, 0, 0, 0),
    /* The title's vehicle level-of-detail distances: the patch is anything
     * but "original". */
    [XS_GRAPHICS_VEHICLE_DETAIL] = SETTING("graphics.vehicle_detail", XS_CHOICE, 0, RENV_GFX_VEHICLE_DETAIL, k_vehicle_detail, NULL, 0, 0, 0),
    /* The same for the title's scenery objects (trees, signs): the patch is
     * "far". */
    [XS_GRAPHICS_SCENERY_DETAIL] = SETTING("graphics.scenery_detail", XS_CHOICE, 0, RENV_GFX_SCENERY_DETAIL, k_scenery_detail, NULL, 0, 0, 0),
    /* The title's button prompts for the pad in use (pad_glyphs.h). */
    [XS_INPUT_GLYPHS]       = SETTING("input.glyphs", XS_CHOICE, 0, RENV_INPUT_GLYPHS, k_glyphs, NULL, 0, 0, 0),
    /* The LFE feed's low-pass where the title's effects image is modelled
     * (apu_gp.h): 120 Hz is what the console's AC-3 encoder did after the
     * GP stage; off is the GP stage's own full-range feed. Live. */
    [XS_AUDIO_LFE_LOWPASS]  = SETTING("audio.lfe_lowpass", XS_CHOICE, 0, RENV_AUDIO_LFE_LOWPASS, k_lfe_lowpass, NULL, 0, 0, 3),
    /* When frames are presented (present_cadence.h, BO-115): legacy is
     * "when ready", as before; auto aims each frame at one refresh. Taken
     * while running (the presenter looks twice a second). auto on macOS,
     * where the Metal presenter takes it as the default too (BO-239). */
    [XS_DISPLAY_PRESENT_TIMING] = SETTING("display.present_timing", XS_CHOICE, 0, RENV_PRESENT_TIMING, k_timing, NULL, 0, 0, XS_TIMING_BUILTIN),
    [XS_ADVANCED_PERF_STEPS] = SETTING("advanced.perf_steps", XS_BOOL, 0, RENV_OVERLAY_PERF_STEPS, NULL, NULL, 0, 0, 0),
    /* The print gate (xr_log.h, BO-125). The built-in value is the build's:
     * on in a developer build, off in a release (set at load). Live. */
    [XS_ADVANCED_DIAG_LOG]  = SETTING("advanced.diagnostic_log", XS_BOOL, 0, RENV_LOG_DIAG, NULL, NULL, 0, 0, 1),
    /* Off as shipped: the title's own textures, bit for bit. */
    [XS_GRAPHICS_UI_SCALE]  = SETTING("graphics.ui_scale", XS_CHOICE, 0, RENV_GFX_UI_SCALE, k_ui_scale, NULL, 0, 0, 0),
    /* On as shipped by us: the title's Settings menu lists TAKEDAWN SETTINGS,
     * which opens this overlay (BO-133). Read when the menu is opened. */
    [XS_INTERFACE_MENU_ENTRY] = SETTING("interface.menu_entry", XS_BOOL, 0, RENV_MENU_ENTRY, NULL, NULL, 0, 0, 1),
    /* The custom soundtrack brought to the level of the title's own music
     * (apu_level.h, BO-132); off is the title's sends as they are, bit for
     * bit. Match as shipped. Live. */
    [XS_AUDIO_CUSTOM_LEVEL] = SETTING("audio.custom_music_level", XS_CHOICE, 0, RENV_AUDIO_CUSTOM_LEVEL, k_custom_level, NULL, 0, 0, 2),
    /* The title's score board (a host hook, BO-165):
     * original as shipped, nothing touched. Live. */
    [XS_DISPLAY_SCORE_BOARD] = SETTING("display.score_board", XS_CHOICE, 0, RENV_GFX_SCORE_BOARD, k_score_board, NULL, 0, 0, 0),
    /* Where the marker over a rival changes from its far look to its near
     * one (a guest patch at "farther"); original as shipped. Live. */
    [XS_DISPLAY_RIVAL_MARKER] = SETTING("display.rival_marker", XS_CHOICE, 0, RENV_GFX_RIVAL_MARKER, k_rival_marker, NULL, 0, 0, 0),
    /* With display.hud_fix in 16:9, a map screen of the title's (Burnout 3:
     * Crash Nav's Select Location) drawn across the picture with its route
     * and markers, or kept 4:3 with the rest of the menu (BO-222). Stretched
     * as shipped. Live. */
    [XS_DISPLAY_CRASHNAV_MAP] = SETTING("display.crashnav_map", XS_CHOICE, 0, RENV_DISPLAY_CRASHNAV_MAP, k_crashnav_map, NULL, 0, 0, 0),
    /* The title's own menu sounds in the overlay (BO-190): on as shipped
     * when the port names them. Live. */
    [XS_AUDIO_MENU_SOUNDS]  = SETTING("audio.menu_sounds", XS_BOOL, 0, RENV_AUDIO_MENU_SOUNDS, NULL, NULL, 0, 0, 1),
    [XS_GRAPHICS_HDR]       = SETTING("graphics.hdr", XS_BOOL, 0, RENV_GFX_HDR, NULL, NULL, 0, 0, 0),
    [XS_GRAPHICS_HDR_PEAK]  = SETTING_STEP("graphics.hdr_peak", XS_FLOAT, 0, RENV_GFX_HDR_PEAK, 0, 10000, 0, 1),
    [XS_GRAPHICS_HDR_PAPER] = SETTING_STEP("graphics.hdr_paper_white", XS_FLOAT, 0, RENV_GFX_HDR_PAPER, 80, 500, NV2A_HDR_PAPER_DEFAULT, 1),
    [XS_GRAPHICS_HDR_HUD]   = SETTING_STEP("graphics.hdr_hud", XS_FLOAT, 0, RENV_GFX_HDR_HUD, 0, 500, 0, 1),
    [XS_GRAPHICS_HDR_SHADOWS] = SETTING_STEP("graphics.hdr_shadows", XS_FLOAT, 0, RENV_GFX_HDR_SHADOWS, 1, 1.5, 1, 0.05),
    [XS_GRAPHICS_HDR_HIGHLIGHTS] = SETTING_STEP("graphics.hdr_highlights", XS_FLOAT, 0, RENV_GFX_HDR_HIGHLIGHTS, 0.5, 3, 1, 0.1),
    [XS_GRAPHICS_HDR_DECODE] = SETTING("graphics.hdr_decode", XS_CHOICE, 0, RENV_GFX_HDR_DECODE, k_hdr_decode, NULL, 0, 0, 0),
    [XS_DISPLAY_FRAME_PACING] = SETTING("display.frame_pacing", XS_CHOICE, 0, RENV_FRAME_PACING, k_frame_pacing, NULL, 0, 0, 0),
    /* The title's wall-clock stepping from a race's start instead of from
     * its first crash camera: the patch is the "1". Off as shipped. */
    /* BO-245: off as shipped (the title's textures). Live. */
    [XS_GRAPHICS_TEXUP]     = SETTING("graphics.texture_upscale", XS_CHOICE, 0, RENV_GFX_TEXUP, k_texup, NULL, 0, 0, 0),
    [XS_GRAPHICS_TEXUP_SCALE] = SETTING("graphics.texture_upscale_scale", XS_CHOICE, 0, RENV_GFX_TEXUP_SCALE, k_texup_sc, NULL, 0, 0, 0),
    [XS_GRAPHICS_TEXUP_ROADS] = SETTING("graphics.texup_roads", XS_BOOL, 0, RENV_GFX_TEXUP_ROADS, NULL, NULL, 0, 0, 0),
    /* BO-272: on as shipped; off spreads the jobs through the race. Live. */
    [XS_GRAPHICS_TEXUP_PRELOAD] = SETTING("graphics.texture_upscale_preload", XS_BOOL, 0, RENV_GFX_TEXUP_PRELOAD, NULL, NULL, 0, 0, 1),
    [XS_GRAPHICS_SOFTBODY]  = SETTING("graphics.softbody", XS_BOOL, XS_RESTART, RENV_GFX_SOFTBODY, NULL, NULL, 0, 0, 0),
    [XS_GAME_RACE_CLOCK]    = SETTING("game.race_clock", XS_BOOL, 0, RENV_GAME_RACE_CLOCK, NULL, NULL, 0, 0, 0),
    /* With a music folder set: the title starts on the custom soundtrack the
     * player last chose in its EA GAMES TRAX screen (the port does it;
     * audio.custom_music_soundtrack remembers which). On as shipped. Read at
     * the title's start. */
    [XS_AUDIO_CUSTOM_AT_START] = SETTING("audio.custom_music_at_start", XS_BOOL, 0, RENV_AUDIO_CUSTOM_AT_START, NULL, NULL, 0, 0, 1),
    /* Read by the kernel when the title asks, early in boot: a restart. */
    [XS_GAME_LANGUAGE]      = SETTING("game.language", XS_CHOICE, XS_RESTART, RENV_GAME_LANGUAGE, k_language, NULL, 0, 0, 0),
#if defined(__ANDROID__)
    [XS_INPUT_VIBRATION]    = SETTING("input.vibration", XS_CHOICE, XS_RESTART, RENV_ANDROID_VIBRATION, k_vibration, NULL, 0, 0, 0),
    /* The on-screen controls (touch_pad.h, BO-189). Live. */
    [XS_INPUT_TOUCH]        = SETTING("input.touch_controls", XS_CHOICE, 0, RENV_TOUCH_CONTROLS, k_touch, NULL, 0, 0, 0),
    [XS_INPUT_TOUCH_OPACITY] = SETTING_STEP("input.touch_opacity", XS_FLOAT, 0, RENV_TOUCH_OPACITY, 0.1, 1.0, 0.5, 0.1),
    [XS_INPUT_TOUCH_SCALE]  = SETTING_STEP("input.touch_scale", XS_FLOAT, 0, RENV_TOUCH_SCALE, 0.6, 1.6, 1.0, 0.1),
    [XS_INPUT_TOUCH_AUTO_GAS] = SETTING("input.touch_auto_gas", XS_BOOL, 0, RENV_TOUCH_AUTO_GAS, NULL, NULL, 0, 0, 0),
#endif
};

/* The title's own settings (xbox_SettingsAddTitle): its entry per slot, and
 * the toolkit's settings it has no row for. */
static const xbox_title_setting *s_title[XS_TITLE_MAX];
static unsigned char s_hidden[XS_COUNT];
static char s_menu_entry_hint[400];

void xbox_SettingsAddTitle(const xbox_title_setting *list, int n)
{
    xs_setting *s;
    int i, k;

    for (i = 0; i < n && list; i++) {
        for (k = 0; k < XS_TITLE_MAX && s_title[k]; k++)
            ;
        if (k == XS_TITLE_MAX) {
            XR_ERROR( LOGP "more than %d title settings: %s has no row\n", XS_TITLE_MAX, list[i].key);
            return;
        }
        s_title[k] = &list[i];
        s = &s_set[XS_TITLE_0 + k];
        s->key = list[i].key;
        s->type = list[i].values ? XS_CHOICE : XS_BOOL;
        s->flags = list[i].restart ? XS_RESTART : 0;
        s->env = list[i].env;
        s->choices = list[i].values;
        s->env_vals = NULL;
        s->builtin = 0;
    }
}

const xbox_title_setting *xbox_SettingsTitleOf(xs_id id)
{
    return id >= XS_TITLE_0 && id <= XS_TITLE_LAST ? s_title[id - XS_TITLE_0] : NULL;
}

void xbox_SettingsHide(xs_id id)
{
    if ((unsigned)id < XS_COUNT)
        s_hidden[id] = 1;
}

int xbox_SettingsHidden(xs_id id)
{
    return (unsigned)id < XS_COUNT && s_hidden[id];
}

void xbox_SettingsSetMenuEntryHint(const char *hint)
{
    snprintf(s_menu_entry_hint, sizeof s_menu_entry_hint, "%s", hint ? hint : "");
}

const char *xbox_SettingsMenuEntryHint(void)
{
    return s_menu_entry_hint[0] ? s_menu_entry_hint : NULL;
}

/* The power mode is two knobs. Balanced's spin lead is 8 ms on every host.
 * 2 ms on Linux saved ~2 W on the Steam Deck but ran the executor at
 * 2.4 GHz instead of 3.5 and doubled its late flips (BO-110 A/B, arm C). */
#define POWER_LEAD_MS 8.0
#define POWER_LEAD_STR "8"
static int s_power_locked_pacing, s_power_locked_lead;
static char s_game_pacing[16], s_game_lead[16];

static char s_path[1024];
static int s_dirty, s_ready, s_finished;
static int s_background;

/* Keys in the file no setting knows: kept as they were on the next write. */
#define KEEP_MAX 64
static struct { char key[64]; char val[192]; } s_keep[KEEP_MAX];
static int s_n_keep;

/* Text settings: a key whose value is a string (a folder), not one of the
 * stepped settings above. Read from the file into its variable like them,
 * written back as it is; a change shows after a restart. */
static struct {
    const char *key;
    recomp_env_id env;
    char val[1024];
    int user, locked;
    int keep_empty;                   /* an empty value is a value (written as ""); absent is not */
} s_text[] = {
    { "audio.custom_music_dir", RENV_AUDIO_MUSIC_DIR, "", 0, 0, 0 },
    { "display.present_mode", RENV_PRESENT_MODE, "", 0, 0, 0 },
    { "console.hdd_key", RENV_CONSOLE_HDKEY, "", 0, 0, 0 },
    { "console.eeprom", RENV_CONSOLE_EEPROM, "", 0, 0, 0 },
    /* BO-230: the soundtrack last chosen in the title's screen; "" is EA
     * GAMES TRAX chosen, no key is nothing chosen yet. */
    { "audio.custom_music_soundtrack", RENV_AUDIO_CUSTOM_SOUNDTRACK, "", 0, 0, 1 },
};
#define TEXT_COUNT ((int)(sizeof s_text / sizeof s_text[0]))

/* ── value <-> text ────────────────────────────────────────────────── */

static int parse_bool(const char *v, int *out)
{
    if (!sx_strcasecmp(v, "1") || !sx_strcasecmp(v, "true") || !sx_strcasecmp(v, "yes")
            || !sx_strcasecmp(v, "on")) {
        *out = 1;
        return 1;
    }
    if (!sx_strcasecmp(v, "0") || !sx_strcasecmp(v, "false") || !sx_strcasecmp(v, "no")
            || !sx_strcasecmp(v, "off")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static double clampd(const xs_setting *s, double v)
{
    if (s->type == XS_BOOL)
        return v != 0.0;
    if (s->type == XS_CHOICE) {
        int n = 0;
        while (s->choices[n])
            n++;
        return v < 0 ? 0 : v >= n ? n - 1 : floor(v + 0.5);
    }
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    /* A stepped value from anywhere (the file, a variable, the overlay) is
     * the nearest step: a file's 1.3 from a finer slider reads as 1.25. */
    if (s->type == XS_FLOAT && s->step > 0.0) {
        v = floor(v / s->step + 0.5) * s->step;
        if (v > s->max) v = s->max;
    }
    return s->type == XS_INT ? floor(v + 0.5) : v;
}

/* The environment's spelling of v. */
void xbox_SettingEnvString(const xs_setting *s, double v, char *out, unsigned n)
{
    switch (s->type) {
    case XS_BOOL:   snprintf(out, n, "%d", v != 0.0); break;
    case XS_INT:    snprintf(out, n, "%d", (int)v); break;
    case XS_FLOAT:  snprintf(out, n, "%g", v); break;
    default: {
        int i = (int)clampd(s, v);
        snprintf(out, n, "%s", s->env_vals ? s->env_vals[i] : s->choices[i]);
        break;
    }
    }
}

/* A knob's text as a value; 0 when it does not parse. */
static int parse_env(const xs_setting *s, const char *v, double *out)
{
    char *end;
    double d;
    int b, i;

    if (!v || !*v)
        return 0;
    switch (s->type) {
    case XS_BOOL:
        if (!parse_bool(v, &b))
            return 0;
        *out = b;
        return 1;
    case XS_INT:
    case XS_FLOAT:
        d = strtod(v, &end);
        if (end == v || *end) {
            if ((s->flags & XS_FROM_BOOL) && parse_bool(v, &b)) {
                *out = b;
                return 1;
            }
            return 0;
        }
        *out = clampd(s, d);
        return 1;
    default:
        for (i = 0; s->choices[i]; i++)
            if (!strcmp(v, s->env_vals ? s->env_vals[i] : s->choices[i])
                    || !strcmp(v, s->choices[i])) {
                *out = i;
                return 1;
            }
        return 0;
    }
}

/* ── the power mode: pacing and spin lead ──────────────────────────── */

static void power_env(double mode, const char **pacing, const char **lead)
{
    int m = (int)mode;
    *pacing = m == 0 ? "spin" : "sleep";
    *lead = m == 1 ? POWER_LEAD_STR : m == 3 ? "auto" : "0";
}

/* Where Maximum ("stock") is not offered (BO-75, a stand-in decision to be
 * confirmed: the Deck and Windows numbers show Balanced as steady and cooler;
 * macOS is unmeasured and keeps all three). Revert: set an entry to 0. */
static const unsigned char k_stock_hidden[] = {
    [XS_HOST_MACOS]   = 0,
    [XS_HOST_WINDOWS] = 1,
    [XS_HOST_ANDROID] = 1,
    [XS_HOST_LINUX]   = 1,
};

static xs_host s_host =
#if defined(__ANDROID__)
    XS_HOST_ANDROID;
#elif defined(_WIN32)
    XS_HOST_WINDOWS;
#elif defined(__linux__)
    XS_HOST_LINUX;
#else
    XS_HOST_MACOS;
#endif

xs_host xbox_SettingsHost(void) { return s_host; }
void xbox_SettingsSetHost(xs_host h) { s_host = h; }

int xbox_PowerStockHidden(xs_host h)
{
    return (unsigned)h < sizeof k_stock_hidden && k_stock_hidden[h];
}

double xbox_PowerModeAtLoad(xs_host h, double stored)
{
    return (int)stored == 0 && xbox_PowerStockHidden(h) ? 1 : stored;
}

/* The mode the two knobs make. */
static double power_from(const char *pacing, const char *lead)
{
    double l = lead && *lead ? atof(lead) : 0.0;
    /* No pacing named: the host's default, Balanced where Maximum is not
     * offered (as xbox_enhance_init applies it). */
    if (!pacing || !*pacing)
        return xbox_PowerStockHidden(s_host) ? 1 : 0;
    if (strcmp(pacing, "sleep"))
        return 0;
    if (lead && !strcmp(lead, "auto"))
        return 3;
    return l > 0.0 ? 1 : 2;
}

/* ── applying ──────────────────────────────────────────────────────── */

static void apply_audio(void)
{
    float g = (float)xbox_SettingValue(XS_AUDIO_VOLUME);
    if (xbox_SettingValue(XS_AUDIO_MUTE) != 0.0
            || (xbox_SettingValue(XS_AUDIO_MUTE_BG) != 0.0 && s_background))
        g = 0.0f;
    xa2_set_gain(g);
}

#if defined(__ANDROID__)
static void apply_touch(void)
{
    xbox_InputTouchSetMode((int)s_set[XS_INPUT_TOUCH].value);
    xbox_InputTouchSetOpacity((float)s_set[XS_INPUT_TOUCH_OPACITY].value);
    xbox_InputTouchSetScale((float)s_set[XS_INPUT_TOUCH_SCALE].value);
    xbox_InputTouchSetAutoGas(s_set[XS_INPUT_TOUCH_AUTO_GAS].value != 0.0);
}
#endif

/* Into the runtime, where it can take the value while running. */
static void apply(xs_id id)
{
    const xs_setting *s = &s_set[id];
    double v = s->value;
    char buf[32];

    /* The cached environment follows, for anything that reads it later. */
    if (s->env != RENV_COUNT) {
        xbox_SettingEnvString(s, v, buf, sizeof buf);
        recomp_env_set(s->env, buf);
    }
    switch (id) {
    case XS_DISPLAY_OVERSCAN:
        nv2a_present_set_overscan((int)v);
        break;
    case XS_DISPLAY_FIT:
        nv2a_present_set_fit(v == 1);
        break;
    case XS_DISPLAY_FILTER: {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.present_filter = (int)v;
        nv2a_host_opts_set(&o);
        break;
    }
    case XS_DISPLAY_FULLSCREEN: {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.fullscreen = v != 0.0;
        nv2a_host_opts_set(&o);      /* the window follows (overlay_ui.cpp) */
        break;
    }
    case XS_GRAPHICS_ANISO: {
        static const int levels[] = { 1, 2, 4, 8, 16 };
        nv2a_host_set_aniso(levels[(int)v]);
        break;
    }
    case XS_GRAPHICS_LOD_BIAS: {
        static const float bias[] = { 0.0f, 0.0f, -0.5f, -1.0f, -1.5f };
        int i = (int)v;
        nv2a_host_set_lod_bias(i == 0 ? NV2A_LOD_OFF : i == 1 ? NV2A_LOD_AUTO : NV2A_LOD_VALUE,
                               bias[i]);
        break;
    }
    case XS_GRAPHICS_DITHER:
        nv2a_host_set_dither(v != 0.0);
        break;
    case XS_GRAPHICS_HDR:
        nv2a_host_set_hdr(v != 0.0);
        break;
    case XS_GRAPHICS_HDR_PEAK:
        nv2a_host_set_hdr_levels(0, (float)v, -1.0f, -1.0f);
        break;
    case XS_GRAPHICS_HDR_PAPER:
        nv2a_host_set_hdr_levels(1, -1.0f, (float)v, -1.0f);
        break;
    case XS_GRAPHICS_HDR_HUD:
        nv2a_host_set_hdr_levels(2, -1.0f, -1.0f, (float)v);
        break;
    case XS_GRAPHICS_HDR_SHADOWS:
        nv2a_host_set_hdr_tone(0, (float)v);
        break;
    case XS_GRAPHICS_HDR_HIGHLIGHTS:
        nv2a_host_set_hdr_tone(1, (float)v);
        break;
    case XS_GRAPHICS_HDR_DECODE:
        nv2a_host_set_hdr_tone(2, (float)v);
        break;
    case XS_GRAPHICS_UI_SCALE:
        /* Held off (Android for now): a value stored in the file stays in
         * the file and does nothing; set_factor says so once. */
        nv2a_texscale_set_factor((int)v == 1 ? 2u : (int)v == 2 ? 4u : 0u);
        break;
    case XS_GRAPHICS_TEXUP:
        nv2a_texup_set_algo((int)v);
        break;
    case XS_GRAPHICS_TEXUP_SCALE:
        nv2a_texup_set_scale((int)v == 1 ? 4u : 2u);
        break;
    case XS_GRAPHICS_TEXUP_ROADS:
        nv2a_texup_set_roads(v != 0.0);
        break;
    case XS_GRAPHICS_TEXUP_PRELOAD:
        nv2a_texup_set_preload(v != 0.0);
        break;
    case XS_GRAPHICS_AA:
        nv2a_host_set_post_aa((int)v);
        break;
    case XS_GRAPHICS_SHARPEN:
        nv2a_host_set_sharpen((float)v);
        break;
    case XS_AUDIO_VOLUME:
    case XS_AUDIO_MUTE:
    case XS_AUDIO_MUTE_BG:
        apply_audio();
        break;
    case XS_AUDIO_FX_LEVEL:
        mcpx_apu_set_fx_level((float)v);
        break;
    case XS_AUDIO_MIXER:
        apu_bus_enable(v != 0.0);
        break;
    case XS_ADVANCED_DIAG_LOG: {
        int was = g_xr_log_diag, now = xr_log_set_diag(v != 0.0);
        if (was != now)
            XR_NOTICE("[LOG] diagnostic logging %s\n", now ? "on" : "off");
        break;
    }
    case XS_AUDIO_CUSTOM_LEVEL:
        mcpx_apu_set_custom_level((int)v);
        break;
    case XS_AUDIO_LFE_LOWPASS:
        mcpx_apu_set_lfe_lowpass(atoi(k_lfe_lowpass[(int)v]));
        break;
    case XS_INPUT_DEADZONE:
        xbox_InputSetDeadzone((int)v);
        break;
    case XS_INPUT_TRIGGER_DEADZONE:
        xbox_InputSetTriggerDeadzone((int)v);
        break;
    case XS_INPUT_RUMBLE:
        xbox_InputSetRumbleStrength(v);
        break;
    case XS_INPUT_FACE_LAYOUT:
        xbox_InputSetFaceLayoutNintendo(v != 0.0);
        xbox_InputGlyphsRefresh();     /* the Nintendo prompts follow it */
        break;
    case XS_INPUT_GLYPHS:
        xbox_InputSetGlyphMode((int)v);
        break;
    case XS_INPUT_CONTROL_SCHEME:
        xbox_InputSetControlScheme((int)v);
        break;
    case XS_INPUT_FIRST_PAD:
        xbox_InputSetFirstPadPort((int)v);
        break;
#if defined(__ANDROID__)
    case XS_INPUT_TOUCH:
    case XS_INPUT_TOUCH_OPACITY:
    case XS_INPUT_TOUCH_SCALE:
    case XS_INPUT_TOUCH_AUTO_GAS:
        apply_touch();
        break;
#endif
    case XS_POWER_MODE: {
        const char *p, *l;
        power_env(v, &p, &l);
        recomp_env_set(RENV_PRESENT_PACING, p);
        recomp_env_set(RENV_PRESENT_SPIN_LEAD, l);
        xbox_PacingSetMode(v == 0 ? XBOX_PACING_SPIN : XBOX_PACING_SLEEP);
        if (v == 3)
            xbox_PacingSetSpinLeadAuto();   /* BO-175: picked at run time */
        else
            xbox_PacingSetSpinLead(v == 1 ? (uint64_t)(POWER_LEAD_MS * 1e6) : 0);
        xbox_PacingWatchReset("power mode changed");   /* BO-211 */
        break;
    }
    case XS_DISPLAY_FRAME_PACING:
        /* The presenter reads the setting itself (keep_poll); the state its
         * keeper and the display lock learned starts over (BO-211). */
        xbox_PacingWatchReset("frame pacing changed");
        break;
    default: {
        const xbox_title_setting *t = xbox_SettingsTitleOf(id);
        if (t && t->apply)
            t->apply((int)v);
        break;
    }
    }
}

/* ── the file ──────────────────────────────────────────────────────── */

static const char *section_of(const char *key, char *out, size_t n)
{
    const char *dot = strchr(key, '.');
    size_t l = dot ? (size_t)(dot - key) : strlen(key);
    if (l >= n)
        l = n - 1;
    memcpy(out, key, l);
    out[l] = 0;
    return dot ? dot + 1 : key;
}

static void quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void write_value(FILE *f, const xs_setting *s, double v)
{
    switch (s->type) {
    case XS_BOOL:   fputs(v != 0.0 ? "true" : "false", f); break;
    case XS_INT:    fprintf(f, "%d", (int)v); break;
    case XS_FLOAT:  fprintf(f, "%g", v); break;
    default:        quote(f, s->choices[(int)clampd(s, v)]); break;
    }
}

/* Is the section of setting i already written by an earlier user setting? */
static int section_written_before(int i)
{
    char a[32], b[32];
    int j;
    section_of(s_set[i].key, a, sizeof a);
    for (j = 0; j < i; j++) {
        if (!s_set[j].user)
            continue;
        section_of(s_set[j].key, b, sizeof b);
        if (!strcmp(a, b))
            return 1;
    }
    return 0;
}

/* The text settings of section `sec`, under its header (in_section), or,
 * with sec NULL, those whose section no stepped setting opened, each under a
 * header of its own: a key and its [table] are written once (TOML). */
static void write_texts(FILE *f, const char *sec, int in_section)
{
    int t, i;

    for (t = 0; t < TEXT_COUNT; t++) {
        char ts[32], sj[32];
        const char *name;
        int opened = 0;

        if (!s_text[t].user || (!s_text[t].val[0] && !s_text[t].keep_empty))
            continue;
        name = section_of(s_text[t].key, ts, sizeof ts);
        for (i = 0; i < XS_COUNT; i++) {
            if (!s_set[i].user)
                continue;
            section_of(s_set[i].key, sj, sizeof sj);
            if (!strcmp(ts, sj))
                opened = 1;
        }
        if (in_section ? strcmp(ts, sec) != 0 : opened)
            continue;
        if (!in_section) {
            /* One header for all the texts of a section (console.hdd_key and
             * console.eeprom; audio.custom_music_dir and _soundtrack): a
             * second [table] of the same name is an error for TOML. */
            int u, again = 0;
            for (u = 0; u < t; u++) {
                char tu[32];
                section_of(s_text[u].key, tu, sizeof tu);
                if (!strcmp(tu, ts) && s_text[u].user && (s_text[u].val[0] || s_text[u].keep_empty))
                    again = 1;
            }
            if (!again)
                fprintf(f, "\n[%s]\n", ts);
        }
        fprintf(f, "%s = ", name);
        quote(f, s_text[t].val);
        fputc('\n', f);
    }
}

void xbox_SettingsFlush(void)
{
    char tmp[sizeof s_path + 8], sec[32];
    FILE *f;
    int i;

    /* Also called from the Android life-cycle callback, on another thread
     * than the overlay's: one writer at a time, the other's change stays
     * dirty for the next call. */
    static volatile LONG writing;
    if (!s_dirty || !s_path[0])
        return;
    if (InterlockedExchange(&writing, 1))
        return;
    s_dirty = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    f = fopen(tmp, "wb");
    if (!f) {
        XR_ERROR( LOGP "cannot write %s\n", tmp);
        InterlockedExchange(&writing, 0);
        return;
    }
    fputs("# Written by the in-game settings overlay (Back+Start, F1).\n"
          "# Only settings you changed are kept; the rest follow the defaults.\n"
          "# A RECOMP_* environment variable overrides what is here.\n", f);
    for (i = 0; i < s_n_keep; i++)
        fprintf(f, "%s = %s\n", s_keep[i].key, s_keep[i].val);
    /* One [table] per section, whatever order the enum keeps its keys in: a
     * second header of the same name is an error for the reader (and for
     * TOML). A section is written at its first key, with all of its keys. */
    for (i = 0; i < XS_COUNT; i++) {
        int j;
        if (!s_set[i].user || section_written_before(i))
            continue;
        section_of(s_set[i].key, sec, sizeof sec);
        fprintf(f, "\n[%s]\n", sec);
        for (j = i; j < XS_COUNT; j++) {
            const xs_setting *s = &s_set[j];
            char sj[32];
            const char *name;
            if (!s->user)
                continue;
            name = section_of(s->key, sj, sizeof sj);
            if (strcmp(sec, sj))
                continue;
            fprintf(f, "%s = ", name);
            /* A setting the environment overrides keeps what the file said. */
            write_value(f, s, s->locked ? s->filev : s->value);
            fputc('\n', f);
        }
        write_texts(f, sec, 1);
    }
    write_texts(f, NULL, 0);
    if (fclose(f) != 0) {
        XR_ERROR( LOGP "cannot write %s\n", tmp);
        InterlockedExchange(&writing, 0);
        return;
    }
#if defined(_WIN32)
    remove(s_path);
#endif
    if (rename(tmp, s_path) != 0)
        XR_ERROR( LOGP "cannot replace %s\n", s_path);
    else
        fprintf(stderr, LOGP "saved\n");
    InterlockedExchange(&writing, 0);
}

static void keep_unknown(const recomp_cfg *c, const char *key)
{
    char v[192];
    if (s_n_keep == KEEP_MAX || strlen(key) >= sizeof s_keep[0].key)
        return;
    switch (recomp_cfg_type_of(c, key)) {
    case RCFG_STRING: {
        const char *t = recomp_cfg_string(c, key, "");
        size_t n = 0, i;
        v[n++] = '"';
        for (i = 0; t[i] && n + 3 < sizeof v; i++) {
            if (t[i] == '"' || t[i] == '\\')
                v[n++] = '\\';
            v[n++] = t[i];
        }
        v[n++] = '"';
        v[n] = 0;
        break;
    }
    case RCFG_INT:   snprintf(v, sizeof v, "%lld", recomp_cfg_int(c, key, 0)); break;
    case RCFG_FLOAT: snprintf(v, sizeof v, "%.17g", recomp_cfg_float(c, key, 0)); break;
    case RCFG_BOOL:  snprintf(v, sizeof v, "%s", recomp_cfg_bool(c, key, 0) ? "true" : "false"); break;
    default: return;                  /* arrays are not kept */
    }
    snprintf(s_keep[s_n_keep].key, sizeof s_keep[0].key, "%s", key);
    snprintf(s_keep[s_n_keep].val, sizeof s_keep[0].val, "%s", v);
    s_n_keep++;
}

/* Read the file into the settings' values (user = 1). */
static void load_file(void)
{
    char err[512];
    recomp_cfg *c;
    FILE *probe = fopen(s_path, "rb");
    size_t k;
    int i;

    if (!probe) {
        XR_NOTICE( LOGP "%s: no file yet\n", s_path);
        return;
    }
    fclose(probe);
    /* Reopening a [table] is accepted: the writer used to split a section in
     * two when its keys were not next to each other in the enum. */
    c = recomp_cfg_load_file_flags(s_path, RCFG_REOPEN_TABLES, err, sizeof err);
    if (!c) {
        XR_ERROR( LOGP "%s (file ignored, the next change replaces it)\n", err);
        return;
    }
    /* The print gate first (BO-193): the file's value is applied to it before
     * anything below prints, so the lines of this very load (a key it could
     * not read, an unknown key) follow the saved choice, and the host's
     * start-up line and the kernel log (opened at its init) see the real
     * state. xr_log_set_diag leaves it alone while the environment decides
     * (RECOMP_LOG_DIAG, a developer switch); a variable of a spelling the
     * overlay cannot show locks the row, and the file does not touch it
     * either. Live changes go through apply() as before. */
    if (!s_set[XS_ADVANCED_DIAG_LOG].locked
            && recomp_cfg_type_of(c, s_set[XS_ADVANCED_DIAG_LOG].key) == RCFG_BOOL)
        xr_log_set_diag(recomp_cfg_bool(c, s_set[XS_ADVANCED_DIAG_LOG].key, 0));
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        recomp_cfg_type t = recomp_cfg_type_of(c, s->key);
        const char *want = NULL;
        double v = 0;
        if (t == RCFG_NONE)
            continue;
        switch (s->type) {
        case XS_BOOL:
            if (t == RCFG_BOOL) v = recomp_cfg_bool(c, s->key, 0); else want = "true or false";
            break;
        case XS_INT:
            if (t == RCFG_INT) v = (double)recomp_cfg_int(c, s->key, 0); else want = "an integer";
            break;
        case XS_FLOAT:
            if (t == RCFG_FLOAT || t == RCFG_INT) v = recomp_cfg_float(c, s->key, 0);
            else if (t == RCFG_BOOL && (s->flags & XS_FROM_BOOL)) v = recomp_cfg_bool(c, s->key, 0);
            else want = "a number";
            break;
        default: {
            int ch = t == RCFG_STRING ? recomp_cfg_choice(c, s->key, s->choices, -1) : -1;
            if (ch >= 0) v = ch; else want = "one of its choices";
            break;
        }
        }
        if (want) {
            fprintf(stderr, LOGP "%s:%d: %s should be %s; ignored\n", s_path,
                    recomp_cfg_line(c, s->key), s->key, want);
            continue;
        }
        s->value = s->filev = clampd(s, v);
        s->user = 1;
    }
    for (k = 0; k < recomp_cfg_count(c); k++) {
        const char *key = recomp_cfg_key_at(c, k);
        for (i = 0; i < XS_COUNT; i++)
            if (!strcmp(key, s_set[i].key))
                break;
        if (i == XS_COUNT) {
            int t;
            for (t = 0; t < TEXT_COUNT; t++)
                if (!strcmp(key, s_text[t].key))
                    break;
            if (t < TEXT_COUNT) {
                if (recomp_cfg_type_of(c, key) == RCFG_STRING) {
                    snprintf(s_text[t].val, sizeof s_text[t].val, "%s",
                             recomp_cfg_string(c, key, ""));
                    s_text[t].user = 1;
                } else {
                    fprintf(stderr, LOGP "%s:%d: %s should be a string; ignored\n", s_path,
                            recomp_cfg_line(c, key), key);
                }
                continue;
            }
            fprintf(stderr, LOGP "%s:%d: unknown key %s (kept)\n", s_path,
                    recomp_cfg_line(c, key), key);
            keep_unknown(c, key);
        }
    }
    recomp_cfg_free(c);
}

/* ── start-up ──────────────────────────────────────────────────────── */

static int env_real(recomp_env_id id)
{
    const char *v = getenv(recomp_env_name(id));
    return v && *v;
}

void xbox_SettingsInit(const char *title)
{
    const char *over;
    int i;

    if (s_ready)
        return;
    s_ready = 1;
    /* The slots no title filled: no key, no variable, never written. */
    for (i = 0; i < XS_TITLE_MAX; i++)
        if (!s_title[i]) {
            xs_setting *s = &s_set[XS_TITLE_0 + i];
            s->key = "";
            s->type = XS_BOOL;
            s->env = RENV_COUNT;
        }
    /* Diagnostic logging defaults to what this build does (xr_log.h). */
    s_set[XS_ADVANCED_DIAG_LOG].builtin = xr_log_default();
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        s->value = s->def = s->builtin;
        if (s->env != RENV_COUNT)
            s->locked = env_real(s->env);
    }
    for (i = 0; i < TEXT_COUNT; i++)
        s_text[i].locked = env_real(s_text[i].env);
    s_power_locked_pacing = env_real(RENV_PRESENT_PACING);
    s_power_locked_lead = env_real(RENV_PRESENT_SPIN_LEAD);
    s_set[XS_POWER_MODE].locked = s_power_locked_pacing || s_power_locked_lead;

    over = getenv("RECOMP_SETTINGS");
    if (over && !strcmp(over, "none")) {
        fprintf(stderr, LOGP "no settings file (RECOMP_SETTINGS=none)\n");
        return;
    }
    if (over && *over) {
        snprintf(s_path, sizeof s_path, "%s", over);
    } else {
        const char *dir = recomp_save_dir(title);
        if (!dir || !*dir)
            return;
        snprintf(s_path, sizeof s_path, "%s/settings.toml", dir);
    }
    load_file();

    /* The file's values into the environment, under what the user set and
     * over what the host defaults (which come after and do not replace). */
    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = &s_set[i];
        char buf[32];
        if (!s->user || s->locked)
            continue;
        if (i == XS_POWER_MODE) {
            const char *p, *l;
            power_env(xbox_PowerModeAtLoad(s_host, s->value), &p, &l);
            if (!s_power_locked_pacing) sx_setenv_keep("RECOMP_PRESENT_PACING", p);
            if (!s_power_locked_lead) sx_setenv_keep("RECOMP_PRESENT_SPIN_LEAD", l);
            continue;
        }
        xbox_SettingEnvString(s, s->value, buf, sizeof buf);
        sx_setenv_keep(recomp_env_name(s->env), buf);
    }
    for (i = 0; i < TEXT_COUNT; i++)
        if (s_text[i].user && s_text[i].val[0] && !s_text[i].locked)
            sx_setenv_keep(recomp_env_name(s_text[i].env), s_text[i].val);
    /* recomp_save_dir read the table before these were set. */
    recomp_env_reload();
    XR_NOTICE( LOGP "%s\n", s_path);
}

void xbox_SettingsGameDefault(const char *name, const char *value)
{
    int i;
    if (!name || !value)
        return;
    if (!strcmp(name, "RECOMP_PRESENT_PACING"))
        snprintf(s_game_pacing, sizeof s_game_pacing, "%s", value);
    else if (!strcmp(name, "RECOMP_PRESENT_SPIN_LEAD"))
        snprintf(s_game_lead, sizeof s_game_lead, "%s", value);
    for (i = 0; i < XS_COUNT; i++) {
        double v;
        if (s_set[i].env != RENV_COUNT && !strcmp(name, recomp_env_name(s_set[i].env))
                && parse_env(&s_set[i], value, &v))
            s_set[i].def = v;
    }
}

void xbox_SettingsFinish(void)
{
    int i;

    if (s_finished)
        return;
    s_finished = 1;
    recomp_env_reload();
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        double v;
        if (s->env != RENV_COUNT) {
            const char *e = recomp_env(s->env);
            if (e && parse_env(s, e, &v))
                s->value = v;
            else if (e && *e)
                s->locked = 1;     /* a spelling the overlay cannot show */
        }
    }
    {
        /* The power mode from the two knobs as they now stand, and the
         * title's own choice as the default. */
        s_set[XS_POWER_MODE].value = power_from(recomp_env(RENV_PRESENT_PACING),
                                                recomp_env(RENV_PRESENT_SPIN_LEAD));
        s_set[XS_POWER_MODE].def = power_from(
            s_game_pacing[0] ? s_game_pacing : NULL, s_game_lead[0] ? s_game_lead : NULL);
    }
    for (i = 0; i < XS_COUNT; i++) {
        s_set[i].launch = s_set[i].value;
        if (s_set[i].locked)
            XR_NOTICE( LOGP "%s: %s is set in the environment\n", s_set[i].key,
                    s_set[i].env == RENV_COUNT ? "RECOMP_PRESENT_PACING/SPIN_LEAD"
                                               : recomp_env_name(s_set[i].env));
    }
    /* What nothing else reads at start. */
    apply_audio();
    xbox_InputSetRumbleStrength(s_set[XS_INPUT_RUMBLE].value);
    xbox_InputSetFaceLayoutNintendo(s_set[XS_INPUT_FACE_LAYOUT].value != 0.0);
    xbox_InputSetControlScheme((int)s_set[XS_INPUT_CONTROL_SCHEME].value);
    xbox_InputSetFirstPadPort((int)s_set[XS_INPUT_FIRST_PAD].value);
    xbox_InputSetGlyphMode((int)s_set[XS_INPUT_GLYPHS].value);
#if defined(__ANDROID__)
    apply_touch();
#endif
}

/* ── access ────────────────────────────────────────────────────────── */

const xs_setting *xbox_SettingGet(xs_id id)
{
    return (unsigned)id < XS_COUNT ? &s_set[id] : NULL;
}

double xbox_SettingValue(xs_id id)
{
    return (unsigned)id < XS_COUNT ? s_set[id].value : 0.0;
}

void xbox_SettingSet(xs_id id, double v)
{
    xs_setting *s;
    if ((unsigned)id >= XS_COUNT)
        return;
    s = &s_set[id];
    if (s->locked)
        return;
    v = clampd(s, v);
    if (v == s->value && s->user)
        return;
    s->value = v;
    s->user = 1;
    s_dirty = 1;
    apply(id);
}

void xbox_SettingReset(xs_id id)
{
    xs_setting *s;
    if ((unsigned)id >= XS_COUNT)
        return;
    s = &s_set[id];
    if (s->locked)
        return;
    s->value = s->def;
    if (s->user)
        s_dirty = 1;
    s->user = 0;
    apply(id);
}

void xbox_SettingsResetAll(void)
{
    int i;
    for (i = 0; i < XS_COUNT; i++)
        xbox_SettingReset((xs_id)i);
}

int xbox_SettingsNeedRestart(void)
{
    int i;
    for (i = 0; i < XS_COUNT; i++)
        if ((s_set[i].flags & XS_RESTART) && s_set[i].value != s_set[i].launch)
            return 1;
    return 0;
}

const char *xbox_SettingsText(const char *key, int *locked)
{
    int i;
    for (i = 0; i < TEXT_COUNT; i++)
        if (!strcmp(key, s_text[i].key)) {
            if (locked)
                *locked = s_text[i].locked;
            if (s_text[i].locked) {
#if defined(_WIN32)
                /* The user's own variable (or an argument put there): the
                 * narrow copy is in the ANSI code page; the wide one is
                 * right for any name. */
                static char utf8[1024];
                wchar_t wname[64];
                const wchar_t *w;
                MultiByteToWideChar(CP_ACP, 0, recomp_env_name(s_text[i].env), -1, wname, 64);
                w = _wgetenv(wname);
                if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8, NULL, NULL) > 0)
                    return utf8;
                return "";
#else
                const char *v = getenv(recomp_env_name(s_text[i].env));
                return v ? v : "";
#endif
            }
            return s_text[i].val;
        }
    if (locked)
        *locked = 0;
    return "";
}

void xbox_SettingsSetText(const char *key, const char *value)
{
    int i;
    for (i = 0; i < TEXT_COUNT; i++)
        if (!strcmp(key, s_text[i].key)) {
            if (s_text[i].locked || (!strcmp(s_text[i].val, value ? value : "")
                                     && (s_text[i].user || !s_text[i].keep_empty)))
                return;
            snprintf(s_text[i].val, sizeof s_text[i].val, "%s", value ? value : "");
            s_text[i].user = 1;
            s_dirty = 1;
            return;
        }
}

int xbox_SettingsTextIsSet(const char *key)
{
    int i;
    for (i = 0; i < TEXT_COUNT; i++)
        if (!strcmp(key, s_text[i].key))
            return s_text[i].user && !s_text[i].locked;
    return 0;
}

const char *xbox_SettingsPath(void)
{
    return s_path;
}

static unsigned s_languages;

void xbox_SettingsSetLanguages(unsigned mask)
{
    s_languages = mask;
}

unsigned xbox_SettingsLanguages(void)
{
    return s_languages;
}

void xbox_SettingsSetBackground(int bg)
{
    if (s_background == (bg != 0))
        return;
    s_background = bg != 0;
    apply_audio();
}

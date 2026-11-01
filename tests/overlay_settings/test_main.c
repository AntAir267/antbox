/*
 * overlay_settings test: settings.toml read into the environment under the
 * precedence (real environment > file > the host's defaults), the live apply
 * of a change, the file written back with unknown keys kept and unchanged
 * settings left out. The runtime it applies to is stubbed.
 */
#include "xbox_settings.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"
#include "recomp_cfg.h"
#include "xr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    (fprintf)(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── the runtime, stubbed ──────────────────────────────────────────── */

static int st_deadzone = -2, st_trigger = -2, st_pacing = -2;
static double st_rumble = -2;
static uint64_t st_lead = 99;
static int st_overscan = -2, st_fit = -2;
static float st_gain = -2;
static struct nv2a_host_opts st_opts;

void xbox_InputSetDeadzone(int dz) { st_deadzone = dz; }
void xbox_InputSetTriggerDeadzone(int dz) { st_trigger = dz; }
void xbox_InputSetRumbleStrength(double g) { st_rumble = g; }
static int st_face;
void xbox_InputSetFaceLayoutNintendo(int on) { st_face = on; }
static int st_scheme = -1;
void xbox_InputSetControlScheme(int s) { st_scheme = s; }
static int st_first_pad = -1;
void xbox_InputSetFirstPadPort(int p) { st_first_pad = p; }
static int st_glyphs = -2, st_glyph_refresh;
void xbox_InputSetGlyphMode(int mode) { st_glyphs = mode; }
void xbox_InputGlyphsRefresh(void) { st_glyph_refresh++; }
void xbox_PacingSetMode(int m) { st_pacing = m; }
void xbox_PacingSetSpinLead(uint64_t ns) { st_lead = ns; }
static int st_auto;
void xbox_PacingSetSpinLeadAuto(void) { st_auto++; st_lead = 77; }
static int st_watch_resets;
void xbox_PacingWatchReset(const char *why) { (void)why; st_watch_resets++; }
int xbox_PacingAutoState(double *l, char *w, size_t n) { (void)l; (void)w; (void)n; return 0; }
void nv2a_present_set_overscan(int c) { st_overscan = c; }
void nv2a_present_set_fit(int f) { st_fit = f; }
void nv2a_host_opts_set(const struct nv2a_host_opts *o) { st_opts = *o; }
const struct nv2a_host_opts *nv2a_host_opts(void) { return &st_opts; }
void xa2_set_gain(float g) { st_gain = g; }
static float st_fx = -2;
void mcpx_apu_set_fx_level(float v) { st_fx = v; }
static int st_mixer = -2;
void apu_bus_enable(int on) { st_mixer = on; }
static int st_custom_level = -1;
void mcpx_apu_set_custom_level(int mode) { st_custom_level = mode; }
static int st_lfe = -1;
void mcpx_apu_set_lfe_lowpass(int hz) { st_lfe = hz; }
static int st_aniso = -2, st_aa = -2;
static float st_sharpen = -2;
void nv2a_host_set_post_aa(int a) { st_aa = a; }
void nv2a_host_set_sharpen(float v) { st_sharpen = v; }
void nv2a_host_set_aniso(int n) { st_aniso = n; }
static int st_lod_mode = -2, st_dither = -2;
static float st_lod_val = -2;
void nv2a_host_set_lod_bias(int m, float v) { st_lod_mode = m; st_lod_val = v; }
void nv2a_host_set_dither(int on) { st_dither = on; }
void nv2a_host_set_hdr(int on) { (void)on; }                                  /* BO-110 */
void nv2a_host_set_hdr_levels(int w, float a, float b, float c) { (void)w; (void)a; (void)b; (void)c; }
void nv2a_host_set_hdr_tone(int which, float v) { (void)which; (void)v; }        /* BO-110 */
static unsigned st_ui_scale = 99;
void nv2a_texscale_set_factor(unsigned f) { st_ui_scale = f; }
/* BO-245: graphics.texture_upscale, its scale and the road switch. */
static int st_texup = -1, st_texup_roads = -1;
static unsigned st_texup_scale;
void nv2a_texup_set_algo(int a) { st_texup = a; }
void nv2a_texup_set_scale(unsigned s) { st_texup_scale = s; }
void nv2a_texup_set_roads(int on) { st_texup_roads = on; }
static int st_texup_preload = -1;
void nv2a_texup_set_preload(int on) { st_texup_preload = on; }
#if !defined(_WIN32)
/* The file writer's one-at-a-time flag (src/platform/win32_compat.c). */
int32_t InterlockedExchange(volatile int32_t *p, int32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
#endif

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { (fprintf)(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* A title's own settings (xbox_SettingsAddTitle): a choice read at start
 * and one it takes at once through its callback. */
static int st_title_apply = -1;
static void title_apply(int v) { st_title_apply = v; }
static const char *const k_title_video[] = { "off", "480p", "720p", NULL };
static const char *const k_title_video_n[] = { "480i", "480p", "720p", NULL };
static const char *const k_title_crop[] = { "off", "clip", NULL };
static const char *const k_title_crop_n[] = { "Whole frame", "Picture", NULL };
static const xbox_title_setting k_title[] = {
    { "title.video", RENV_DISPLAY_HDTV, k_title_video, k_title_video_n, "Video mode", "", XS_PAGE_DISPLAY, 1, NULL },
    { "title.crop", RENV_PRESENT_CROP, k_title_crop, k_title_crop_n, "Picture area", "", XS_PAGE_DISPLAY, 0, title_apply },
};

/* The settings read once at start, by the table's flag: the only list of
 * them there is (the overlay's "(restart)" mark, its Restart row and
 * xbox_SettingsNeedRestart all read the flag). A setting added to the table
 * with or without the flag has to be put here too, on purpose. */
static void test_restart_flags(void)
{
    int i, flagged = 0;

    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = xbox_SettingGet((xs_id)i);
        int want = i == XS_DISPLAY_ASPECT || i == XS_GRAPHICS_RENDER_SCALE || i == XS_AUDIO_SPEAKERS
                   || i == XS_GRAPHICS_SOFTBODY || i == XS_GAME_LANGUAGE || i == XS_GRAPHICS_RENDERER
                   || (xbox_SettingsTitleOf((xs_id)i) && xbox_SettingsTitleOf((xs_id)i)->restart);
#if defined(__ANDROID__)
        want = want || i == XS_INPUT_VIBRATION;
#endif
        if (!!(s->flags & XS_RESTART) != want) {
            (printf)("FAIL restart flag of %s: %d, expected %d\n", s->key, !!(s->flags & XS_RESTART), want);
            s_fail++;
        }
        flagged += !!(s->flags & XS_RESTART);
    }
    CHECK(flagged >= 3);
}

/* Every setting has its row in the table: the table is indexed by the
 * enum (designated), so a setting added to the enum without one would be a
 * row of zeros. Each key once. */
static void test_table_whole(void)
{
    int i, j;

    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = xbox_SettingGet((xs_id)i);
        /* A title slot nothing filled is empty on purpose. */
        if (i >= XS_TITLE_0 && i <= XS_TITLE_LAST && !xbox_SettingsTitleOf((xs_id)i)) {
            if (s->key && *s->key) {
                (printf)("FAIL title slot %d has the key %s\n", i, s->key);
                s_fail++;
            }
            continue;
        }
        if (!s->key || !*s->key) {
            (printf)("FAIL setting %d has no row in the table\n", i);
            s_fail++;
            continue;
        }
        for (j = 0; j < i; j++)
            if (xbox_SettingGet((xs_id)j)->key && !strcmp(xbox_SettingGet((xs_id)j)->key, s->key)) {
                (printf)("FAIL %s twice in the table (%d, %d)\n", s->key, j, i);
                s_fail++;
            }
        if (s->type == XS_CHOICE && (!s->choices || !s->choices[0])) {
            (printf)("FAIL %s: a choice without choices\n", s->key);
            s_fail++;
        }
    }
}

int main(void)
{
    const char *path = "overlay_settings_test.toml";
    const xs_setting *s;
    char *text;

    write_file(path,
        "# a user's file\n"
        "mystery.key = 7\n"
        "[display]\n"
        "overscan = \"crop\"\n"
        "filter = 3\n"                 /* the wrong type: ignored */
        "[audio]\n"
        "volume = 0.5\n"               /* the environment sets it too */
        "mute = true\n"
        "custom_music_dir = \"/music/My \\\"Best\\\" \xC5\x90szi \xC5\xB1r\"\n"   /* a text setting, UTF-8 */
        "custom_music_soundtrack = \"\"\n"   /* BO-230: an empty value that means something (EA GAMES TRAX chosen) */
        "[input]\n"
        "deadzone = 3277\n"
        "rumble = 1.8\n"               /* an older slider's value, between two steps */
        "[power]\n"
        "mode = \"battery\"\n"
        "[display]\n"                 /* an older writer's split section */
        "hud_fix = false\n"
        "[graphics]\n"
        "bloom = false\n"             /* a title's effect, on by default: switched off */
        "[advanced]\n"
        "diagnostic_log = true\n"     /* BO-193: applied at start, not only on a change */
        "[title]\n"
        "crop = \"clip\"\n");       /* a title's own setting */
    set_env("RECOMP_SETTINGS", path);
    set_env("RECOMP_AUDIO_VOLUME", "0.25");          /* the user's own */
    set_env("RECOMP_AUDIO_MUTE", NULL);
    set_env("RECOMP_AUDIO_MUSIC_DIR", NULL);
    set_env("RECOMP_PRESENT_OVERSCAN", NULL);
    set_env("RECOMP_PRESENT_PACING", NULL);
    set_env("RECOMP_PRESENT_SPIN_LEAD", NULL);
    set_env("RECOMP_INPUT_RUMBLE_GAIN", NULL);
    set_env("RECOMP_PAD_DEADZONE", NULL);
    set_env("RECOMP_GFX_BLOOM", NULL);
    set_env("RECOMP_GFX_MOTION_BLUR", NULL);
    set_env("RECOMP_GFX_CRASH_60FPS", NULL);
    set_env("RECOMP_GFX_SPLIT_60FPS", NULL);
    set_env("RECOMP_AUDIO_SPLIT_MUSIC", NULL);
    set_env("RECOMP_AUDIO_CUSTOM_LEVEL", NULL);
    set_env("RECOMP_AUDIO_MENU_SOUNDS", NULL);
    set_env("RECOMP_GFX_VEHICLE_DETAIL", NULL);
    set_env("RECOMP_GFX_SCENERY_DETAIL", NULL);
    set_env("RECOMP_GFX_RIVAL_MARKER", NULL);
    set_env("RECOMP_DISPLAY_CRASHNAV_MAP", NULL);
    set_env("RECOMP_GAME_RACE_CLOCK", NULL);

    /* A release build, nothing in the environment: quiet until the file says. */
    set_env("RECOMP_LOG_DIAG", NULL);
    set_env("RECOMP_TRACE", NULL);
    set_env("RECOMP_DEBUG", NULL);
    set_env("RECOMP_STDIO_LOG", NULL);
    set_env("RECOMP_FLIP_MARK", NULL);
    set_env("RECOMP_HEADLESS", NULL);
    xr_log_init(1, 0);
    CHECK(g_xr_log_diag == 0);

    set_env("RECOMP_DISPLAY_HDTV", NULL);
    set_env("RECOMP_PRESENT_CROP", NULL);
    xbox_SettingsAddTitle(k_title, 2);
    xbox_SettingsInit("Test");
    CHECK(!strcmp(xbox_SettingsPath(), path));
    /* The saved diagnostic_log = true is in force at start, by "setting". */
    CHECK(g_xr_log_diag == 1 && !xr_log_forced() && !strcmp(xr_log_source(), "setting"));
    test_restart_flags();
    test_table_whole();
    /* A text setting: into its variable, readable, not "unknown". */
    CHECK(getenv("RECOMP_AUDIO_MUSIC_DIR")
          && !strcmp(getenv("RECOMP_AUDIO_MUSIC_DIR"), "/music/My \"Best\" \xC5\x90szi \xC5\xB1r"));
    {
        int locked = 1;
        CHECK(!strcmp(xbox_SettingsText("audio.custom_music_dir", &locked), "/music/My \"Best\" \xC5\x90szi \xC5\xB1r"));
        /* BO-230: the soundtrack choice: "" in the file is a value, no key is not */
        CHECK(xbox_SettingsTextIsSet("audio.custom_music_soundtrack") && !xbox_SettingsText("audio.custom_music_soundtrack", NULL)[0]);
        CHECK(xbox_SettingsTextIsSet("audio.custom_music_dir") && !xbox_SettingsTextIsSet("display.present_mode"));
        CHECK(xbox_SettingValue(XS_AUDIO_CUSTOM_AT_START) == 1 && !xbox_SettingGet(XS_AUDIO_CUSTOM_AT_START)->user
              && !strcmp(xbox_SettingGet(XS_AUDIO_CUSTOM_AT_START)->key, "audio.custom_music_at_start"));
        CHECK(!locked);
        CHECK(!strcmp(xbox_SettingsText("no.such_key", NULL), ""));
    }
    /* The file reached the environment, the real environment did not move. */
    CHECK(getenv("RECOMP_PRESENT_OVERSCAN") && !strcmp(getenv("RECOMP_PRESENT_OVERSCAN"), "crop"));
    CHECK(getenv("RECOMP_AUDIO_MUTE") && !strcmp(getenv("RECOMP_AUDIO_MUTE"), "1"));
    CHECK(getenv("RECOMP_AUDIO_VOLUME") && !strcmp(getenv("RECOMP_AUDIO_VOLUME"), "0.25"));
    CHECK(getenv("RECOMP_PAD_DEADZONE") && !strcmp(getenv("RECOMP_PAD_DEADZONE"), "3277"));
    CHECK(getenv("RECOMP_PRESENT_PACING") && !strcmp(getenv("RECOMP_PRESENT_PACING"), "sleep"));
    CHECK(getenv("RECOMP_PRESENT_SPIN_LEAD") && !strcmp(getenv("RECOMP_PRESENT_SPIN_LEAD"), "0"));
    CHECK(!getenv("RECOMP_PRESENT_FILTER"));
    /* The second [display] table was read, not refused (the whole file would
     * have been ignored): its key is the user's own. */
    CHECK(xbox_SettingGet(XS_DISPLAY_HUD_FIX)->user && xbox_SettingValue(XS_DISPLAY_HUD_FIX) == 0);

    /* The host's defaults: setenv without replacing, then told. */
    setenv("RECOMP_PRESENT_OVERSCAN", "crop", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_OVERSCAN", "crop");
    setenv("RECOMP_INPUT_RUMBLE_GAIN", "1.25", 0); xbox_SettingsGameDefault("RECOMP_INPUT_RUMBLE_GAIN", "1.25");
    setenv("RECOMP_PRESENT_PACING", "sleep", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_PACING", "sleep");
    setenv("RECOMP_PRESENT_SPIN_LEAD", "8", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_SPIN_LEAD", "8");
    setenv("RECOMP_DISPLAY_HDTV", "720p", 0); xbox_SettingsGameDefault("RECOMP_DISPLAY_HDTV", "720p");   /* a title setting's */
    xbox_SettingsFinish();

    /* The title's settings: in the slots in their order, the file's value
     * in the environment, the title's default, a change through the
     * callback, and the restart only for the one read at start. */
    {
        const xs_id video = (xs_id)XS_TITLE_0, crop = (xs_id)(XS_TITLE_0 + 1);
        const char *e;
        CHECK(xbox_SettingsTitleOf(video) == &k_title[0] && xbox_SettingsTitleOf(crop) == &k_title[1]);
        CHECK(!xbox_SettingsTitleOf((xs_id)(XS_TITLE_0 + 2)) && !xbox_SettingsTitleOf(XS_DISPLAY_FIT));
        CHECK(!strcmp(xbox_SettingGet((xs_id)(XS_TITLE_0 + 2))->key, ""));
        CHECK(xbox_SettingGet(video)->type == XS_CHOICE && (xbox_SettingGet(video)->flags & XS_RESTART));
        CHECK(xbox_SettingValue(video) == 2 && xbox_SettingGet(video)->def == 2 && !xbox_SettingGet(video)->user);
        CHECK(xbox_SettingValue(crop) == 1 && xbox_SettingGet(crop)->user);
        CHECK(getenv("RECOMP_PRESENT_CROP") && !strcmp(getenv("RECOMP_PRESENT_CROP"), "clip"));
        xbox_SettingSet(crop, 0);
        e = recomp_env(RENV_PRESENT_CROP);
        CHECK(st_title_apply == 0 && e && !strcmp(e, "off"));
        CHECK(!xbox_SettingsNeedRestart());
        xbox_SettingSet(video, 1);
        CHECK(xbox_SettingsNeedRestart());
        xbox_SettingReset(video);
        CHECK(!xbox_SettingsNeedRestart() && xbox_SettingValue(video) == 2);
        /* Rows a title has no use for. */
        CHECK(!xbox_SettingsHidden(XS_DISPLAY_FIT));
        xbox_SettingsHide(XS_DISPLAY_FIT);
        CHECK(xbox_SettingsHidden(XS_DISPLAY_FIT) && !xbox_SettingsHidden(XS_DISPLAY_FILTER));
        CHECK(!xbox_SettingsHidden(XS_COUNT));
        /* The menu entry's help, the title's words or the default. */
        CHECK(!xbox_SettingsMenuEntryHint());
        xbox_SettingsSetMenuEntryHint("Adds X to the game's OPTIONS.");
        CHECK(xbox_SettingsMenuEntryHint() && !strcmp(xbox_SettingsMenuEntryHint(), "Adds X to the game's OPTIONS."));
        xbox_SettingsSetMenuEntryHint(NULL);
        CHECK(!xbox_SettingsMenuEntryHint());
    }

    s = xbox_SettingGet(XS_AUDIO_VOLUME);
    CHECK(s->locked && s->value == 0.25 && s->user);
    xbox_SettingSet(XS_AUDIO_VOLUME, 0.9);           /* refused: locked */
    CHECK(xbox_SettingValue(XS_AUDIO_VOLUME) == 0.25);
    CHECK(xbox_SettingValue(XS_AUDIO_MUTE) == 1 && xbox_SettingGet(XS_AUDIO_MUTE)->user);
    CHECK(st_gain == 0.0f);                          /* muted at start */
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1);
    CHECK(xbox_SettingGet(XS_DISPLAY_OVERSCAN)->def == 1);
    CHECK(xbox_SettingValue(XS_DISPLAY_FILTER) == 0 && !xbox_SettingGet(XS_DISPLAY_FILTER)->user);
    CHECK(xbox_SettingValue(XS_INPUT_DEADZONE) == 3277);
    /* The file's rumble strength outranks the host's default (1.25 here) and
     * keeps its number, at the nearest quarter. */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.75 && xbox_SettingGet(XS_INPUT_RUMBLE)->user);
    CHECK(xbox_SettingGet(XS_INPUT_RUMBLE)->def == 1.25);
    CHECK(st_rumble == 1.75);
    CHECK(getenv("RECOMP_INPUT_RUMBLE_GAIN") && !strcmp(getenv("RECOMP_INPUT_RUMBLE_GAIN"), "1.75"));
    /* The power mode: the file said battery (sleep, no spin lead); the
     * host's default is performance (sleep, 8 ms). */
    CHECK(xbox_SettingValue(XS_POWER_MODE) == 2 && xbox_SettingGet(XS_POWER_MODE)->def == 1);
    CHECK(!xbox_SettingsNeedRestart());
    /* Maximum ("stock") is hidden on Windows, Android and Linux (BO-75): a
     * stored stock reads as performance there, battery and performance are
     * kept; macOS keeps all three. The host this was built for is one of
     * them, and the table is the whole decision. */
    {
        static const xs_host hosts[] = { XS_HOST_WINDOWS, XS_HOST_ANDROID, XS_HOST_LINUX };
        for (unsigned k = 0; k < 3; k++) {
            CHECK(xbox_PowerStockHidden(hosts[k]));
            CHECK(xbox_PowerModeAtLoad(hosts[k], 0) == 1);
            CHECK(xbox_PowerModeAtLoad(hosts[k], 1) == 1);
            CHECK(xbox_PowerModeAtLoad(hosts[k], 2) == 2);
        }
        CHECK(!xbox_PowerStockHidden(XS_HOST_MACOS));
        CHECK(xbox_PowerModeAtLoad(XS_HOST_MACOS, 0) == 0);
        CHECK(xbox_PowerModeAtLoad(XS_HOST_MACOS, 1) == 1);
        CHECK(xbox_PowerModeAtLoad(XS_HOST_MACOS, 2) == 2);
        CHECK(xbox_PowerModeAtLoad(XS_HOST_MACOS, 3) == 3);
        CHECK(xbox_PowerModeAtLoad(XS_HOST_LINUX, 3) == 3);
#if defined(__APPLE__)
        CHECK(xbox_SettingsHost() == XS_HOST_MACOS);
#endif
    }
    /* The title's own effects (guest patches on the "0"): on unless the
     * file or the overlay says otherwise, and a stored "off" is kept. */
    CHECK(xbox_SettingValue(XS_GRAPHICS_BLOOM) == 0 && xbox_SettingGet(XS_GRAPHICS_BLOOM)->user);
    CHECK(xbox_SettingGet(XS_GRAPHICS_BLOOM)->def == 1);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "0"));
    CHECK(xbox_SettingValue(XS_GRAPHICS_MOTION_BLUR) == 1 && !xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->user);
    {
        /* Untouched, the variable never reads "0": no patch is active. */
        const char *e = recomp_env(RENV_GFX_MOTION_BLUR);
        CHECK(!e || strcmp(e, "0") != 0);
    }
    {
        /* The vehicle detail distance (a guest patch on anything but
         * "original"): untouched it never reads far or farthest; a choice
         * reaches the environment at once and goes back. */
        const char *e = recomp_env(RENV_GFX_VEHICLE_DETAIL);
        CHECK(xbox_SettingValue(XS_GRAPHICS_VEHICLE_DETAIL) == 0 && !xbox_SettingGet(XS_GRAPHICS_VEHICLE_DETAIL)->user);
        CHECK(!e || !*e || !strcmp(e, "original"));
        xbox_SettingSet(XS_GRAPHICS_VEHICLE_DETAIL, 2);
        e = recomp_env(RENV_GFX_VEHICLE_DETAIL);
        CHECK(e && !strcmp(e, "farthest"));
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_GRAPHICS_VEHICLE_DETAIL);
        e = recomp_env(RENV_GFX_VEHICLE_DETAIL);
        CHECK(!e || !*e || !strcmp(e, "original"));
    }
    {
        /* The scenery detail (a guest patch at "far"): the same. */
        const char *e = recomp_env(RENV_GFX_SCENERY_DETAIL);
        CHECK(xbox_SettingValue(XS_GRAPHICS_SCENERY_DETAIL) == 0 && !xbox_SettingGet(XS_GRAPHICS_SCENERY_DETAIL)->user);
        CHECK(!e || !*e || !strcmp(e, "original"));
        xbox_SettingSet(XS_GRAPHICS_SCENERY_DETAIL, 1);
        e = recomp_env(RENV_GFX_SCENERY_DETAIL);
        CHECK(e && !strcmp(e, "far"));
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_GRAPHICS_SCENERY_DETAIL);
        e = recomp_env(RENV_GFX_SCENERY_DETAIL);
        CHECK(!e || !*e || !strcmp(e, "original"));
    }
    {
        /* The rival marker's switch (a guest patch at "farther"): as
         * shipped by default, live, and back. */
        const char *e = recomp_env(RENV_GFX_RIVAL_MARKER);
        CHECK(xbox_SettingValue(XS_DISPLAY_RIVAL_MARKER) == 0 && !xbox_SettingGet(XS_DISPLAY_RIVAL_MARKER)->user);
        CHECK(!e || !*e || !strcmp(e, "original"));
        CHECK(!strcmp(xbox_SettingGet(XS_DISPLAY_RIVAL_MARKER)->key, "display.rival_marker"));
        xbox_SettingSet(XS_DISPLAY_RIVAL_MARKER, 1);
        e = recomp_env(RENV_GFX_RIVAL_MARKER);
        CHECK(e && !strcmp(e, "farther"));
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_DISPLAY_RIVAL_MARKER);
        e = recomp_env(RENV_GFX_RIVAL_MARKER);
        CHECK(!e || !*e || !strcmp(e, "original"));
    }
    {
        /* The Crash Nav map's shape with the widescreen HUD (BO-222):
         * stretched by default, live, and back. */
        const char *e = recomp_env(RENV_DISPLAY_CRASHNAV_MAP);
        CHECK(xbox_SettingValue(XS_DISPLAY_CRASHNAV_MAP) == 0 && !xbox_SettingGet(XS_DISPLAY_CRASHNAV_MAP)->user);
        CHECK(!e || !*e || !strcmp(e, "stretched"));
        CHECK(!strcmp(xbox_SettingGet(XS_DISPLAY_CRASHNAV_MAP)->key, "display.crashnav_map"));
        xbox_SettingSet(XS_DISPLAY_CRASHNAV_MAP, 1);
        e = recomp_env(RENV_DISPLAY_CRASHNAV_MAP);
        CHECK(e && !strcmp(e, "native"));
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_DISPLAY_CRASHNAV_MAP);
        e = recomp_env(RENV_DISPLAY_CRASHNAV_MAP);
        CHECK(!e || !*e || !strcmp(e, "stretched"));
    }
    {
        /* The score board (a host hook, BO-165): original untouched, a
         * choice reaches the environment at once and goes back. */
        const char *e = recomp_env(RENV_GFX_SCORE_BOARD);
        CHECK(xbox_SettingValue(XS_DISPLAY_SCORE_BOARD) == 0 && !xbox_SettingGet(XS_DISPLAY_SCORE_BOARD)->user);
        CHECK(!e || !*e || !strcmp(e, "original"));
        CHECK(!strcmp(xbox_SettingGet(XS_DISPLAY_SCORE_BOARD)->key, "display.score_board"));
        xbox_SettingSet(XS_DISPLAY_SCORE_BOARD, 1);
        e = recomp_env(RENV_GFX_SCORE_BOARD);
        CHECK(e && !strcmp(e, "smooth"));
        xbox_SettingSet(XS_DISPLAY_SCORE_BOARD, 2);          /* past the last: smooth */
        CHECK(xbox_SettingValue(XS_DISPLAY_SCORE_BOARD) == 1);
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_DISPLAY_SCORE_BOARD);
        e = recomp_env(RENV_GFX_SCORE_BOARD);
        CHECK(!e || !*e || !strcmp(e, "original"));
    }
    {
        /* The overlay's menu sounds (BO-190): on as shipped, off reaches the
         * environment at once, live, and goes back. */
        const char *e = recomp_env(RENV_AUDIO_MENU_SOUNDS);
        CHECK(!strcmp(xbox_SettingGet(XS_AUDIO_MENU_SOUNDS)->key, "audio.menu_sounds"));
        CHECK(xbox_SettingValue(XS_AUDIO_MENU_SOUNDS) == 1 && !xbox_SettingGet(XS_AUDIO_MENU_SOUNDS)->user);
        CHECK(xbox_SettingGet(XS_AUDIO_MENU_SOUNDS)->type == XS_BOOL);
        CHECK(!e || strcmp(e, "0") != 0);
        xbox_SettingSet(XS_AUDIO_MENU_SOUNDS, 0);
        e = recomp_env(RENV_AUDIO_MENU_SOUNDS);
        CHECK(e && !strcmp(e, "0"));
        CHECK(xbox_SettingValue(XS_AUDIO_MENU_SOUNDS) == 0);
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingReset(XS_AUDIO_MENU_SOUNDS);
        CHECK(xbox_SettingValue(XS_AUDIO_MENU_SOUNDS) == 1);
    }
    xbox_SettingSet(XS_GRAPHICS_MOTION_BLUR, 0);
    CHECK(recomp_env(RENV_GFX_MOTION_BLUR) && !strcmp(recomp_env(RENV_GFX_MOTION_BLUR), "0"));
    CHECK(!xbox_SettingsNeedRestart());              /* live */
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 1);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "1"));
    /* BO-212: strengths in tenths. 0 is the old "off", 1 the original; a value
     * between is the nearest tenth, and the variable carries it. */
    CHECK(xbox_SettingGet(XS_GRAPHICS_BLOOM)->type == XS_FLOAT);
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 0.5);
    CHECK(xbox_SettingValue(XS_GRAPHICS_BLOOM) == 0.5);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "0.5"));
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 0.34);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "0.3"));
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 7);
    CHECK(xbox_SettingValue(XS_GRAPHICS_BLOOM) == 1);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "1"));
    xbox_SettingSet(XS_GRAPHICS_MOTION_BLUR, 0.7);
    CHECK(recomp_env(RENV_GFX_MOTION_BLUR) && !strcmp(recomp_env(RENV_GFX_MOTION_BLUR), "0.7"));
    xbox_SettingSet(XS_GRAPHICS_MOTION_BLUR, 0);
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 0);
    /* The title's 30 Hz phases at 60 (a guest patch on the "1"): off as
     * shipped, and untouched the variable never reads "1". */
    CHECK(!strcmp(xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->key, "graphics.crash_60fps"));
    CHECK(xbox_SettingValue(XS_GRAPHICS_CRASH_60FPS) == 0 && !xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->user);
    CHECK(xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->def == 0);
    {
        const char *e = recomp_env(RENV_GFX_CRASH_60FPS);
        CHECK(!e || strcmp(e, "1") != 0);
    }
    xbox_SettingSet(XS_GRAPHICS_CRASH_60FPS, 1);
    CHECK(recomp_env(RENV_GFX_CRASH_60FPS) && !strcmp(recomp_env(RENV_GFX_CRASH_60FPS), "1"));
    /* Split screen at 60 and with music (guest patches on the "1"): both off
     * as shipped, the variables never "1" untouched, live. */
    {
        static const struct { xs_id id; recomp_env_id env; const char *key; } k[] = {
            { XS_GRAPHICS_SPLIT_60FPS, RENV_GFX_SPLIT_60FPS, "graphics.split_60fps" },
            { XS_AUDIO_SPLIT_MUSIC, RENV_AUDIO_SPLIT_MUSIC, "audio.split_music" },
            { XS_GAME_RACE_CLOCK, RENV_GAME_RACE_CLOCK, "game.race_clock" },
        };
        for (int i = 0; i < 3; i++) {
            const char *e = recomp_env(k[i].env);
            CHECK(!strcmp(xbox_SettingGet(k[i].id)->key, k[i].key));
            CHECK(xbox_SettingValue(k[i].id) == 0 && !xbox_SettingGet(k[i].id)->user);
            CHECK(xbox_SettingGet(k[i].id)->def == 0);
            CHECK(!e || strcmp(e, "1") != 0);
            xbox_SettingSet(k[i].id, 1);
            CHECK(recomp_env(k[i].env) && !strcmp(recomp_env(k[i].env), "1"));
            xbox_SettingReset(k[i].id);
            CHECK(xbox_SettingValue(k[i].id) == 0 && !xbox_SettingGet(k[i].id)->user);
            CHECK(recomp_env(k[i].env) && !strcmp(recomp_env(k[i].env), "0"));
        }
        CHECK(!xbox_SettingsNeedRestart());
    }
    CHECK(!xbox_SettingsNeedRestart());              /* live */

    /* The custom music loudness (BO-132): match as shipped, the four
     * choices in order, the environment's spelling, live. */
    {
        static const char *const names[4] = { "off", "quieter", "match", "louder" };
        const xs_setting *cl = xbox_SettingGet(XS_AUDIO_CUSTOM_LEVEL);
        CHECK(!strcmp(cl->key, "audio.custom_music_level") && cl->type == XS_CHOICE);
        CHECK(xbox_SettingValue(XS_AUDIO_CUSTOM_LEVEL) == 2 && !cl->user && cl->def == 2);
        CHECK(cl->choices[0] && cl->choices[3] && !cl->choices[4]);
        for (int i = 0; i < 4; i++) {
            CHECK(!strcmp(cl->choices[i], names[i]));
            xbox_SettingSet(XS_AUDIO_CUSTOM_LEVEL, i);
            CHECK(st_custom_level == i);
            CHECK(xbox_SettingValue(XS_AUDIO_CUSTOM_LEVEL) == i);
            CHECK(recomp_env(RENV_AUDIO_CUSTOM_LEVEL) && !strcmp(recomp_env(RENV_AUDIO_CUSTOM_LEVEL), names[i]));
        }
        CHECK(!xbox_SettingsNeedRestart());          /* live */
        xbox_SettingSet(XS_AUDIO_CUSTOM_LEVEL, 3);
        xbox_SettingReset(XS_AUDIO_CUSTOM_LEVEL);
        CHECK(xbox_SettingValue(XS_AUDIO_CUSTOM_LEVEL) == 2 && !cl->user && st_custom_level == 2);
    }

    /* A live change reaches the runtime, the cached environment and the file. */
    /* Auto (BO-175): sleep pacing, the lead picked at run time, "auto" in
     * the environment; and back to a fixed lead. */
    xbox_SettingSet(XS_POWER_MODE, 3);
    CHECK(st_pacing == 1 && st_auto == 1 && st_lead == 77);
    CHECK(recomp_env(RENV_PRESENT_SPIN_LEAD) && !strcmp(recomp_env(RENV_PRESENT_SPIN_LEAD), "auto"));
    CHECK(recomp_env(RENV_PRESENT_PACING) && !strcmp(recomp_env(RENV_PRESENT_PACING), "sleep"));
    CHECK(!strcmp(xbox_SettingGet(XS_POWER_MODE)->choices[3], "auto"));
    xbox_SettingSet(XS_POWER_MODE, 1);
    CHECK(st_pacing == 1 && st_lead == 8000000ull);
    CHECK(recomp_env(RENV_PRESENT_SPIN_LEAD) && !strcmp(recomp_env(RENV_PRESENT_SPIN_LEAD), "8"));
    /* BO-211: a change of power mode or frame pacing resets the pacing state. */
    {
        int before = st_watch_resets;
        xbox_SettingSet(XS_DISPLAY_FRAME_PACING, 2);
        CHECK(st_watch_resets == before + 1);
        CHECK(st_watch_resets >= 3);
        xbox_SettingSet(XS_DISPLAY_FRAME_PACING, 0);
    }
    xbox_SettingSet(XS_INPUT_DEADZONE, 6554);
    CHECK(st_deadzone == 6554);
    xbox_SettingSet(XS_INPUT_TRIGGER_DEADZONE, 20);
    CHECK(st_trigger == 20);
    xbox_SettingSet(XS_DISPLAY_FIT, 1);
    CHECK(st_fit == 1);
    /* BO-235: the third choice reaches the layout as 2 and the environment as "fill". */
    xbox_SettingSet(XS_DISPLAY_OVERSCAN, 2);
    CHECK(st_overscan == 2);
    CHECK(recomp_env(RENV_PRESENT_OVERSCAN) && !strcmp(recomp_env(RENV_PRESENT_OVERSCAN), "fill"));
    xbox_SettingSet(XS_DISPLAY_OVERSCAN, 1);
    CHECK(st_overscan == 1 && !strcmp(recomp_env(RENV_PRESENT_OVERSCAN), "crop"));
    xbox_SettingSet(XS_DISPLAY_OVERSCAN, 0);
    CHECK(st_overscan == 0);
    xbox_SettingSet(XS_DISPLAY_FILTER, 1);
    CHECK(st_opts.present_filter == 1);
    xbox_SettingSet(XS_AUDIO_MUTE, 0);
    CHECK(st_gain == 0.25f);                         /* the environment's volume */
    xbox_SettingSet(XS_AUDIO_MUTE_BG, 1);
    xbox_SettingsSetBackground(1);
    CHECK(st_gain == 0.0f);
    xbox_SettingsSetBackground(0);
    CHECK(st_gain == 0.25f);
    /* Anisotropic filtering: the choice's level, the environment's spelling. */
    xbox_SettingSet(XS_GRAPHICS_ANISO, 3);
    CHECK(st_aniso == 8);
    CHECK(recomp_env(RENV_GFX_ANISO) && !strcmp(recomp_env(RENV_GFX_ANISO), "8"));
    /* LOD bias and dither: off as shipped (today's picture); the choices'
     * modes and values, the environment's spelling. */
    CHECK(xbox_SettingValue(XS_GRAPHICS_LOD_BIAS) == 0 && xbox_SettingValue(XS_GRAPHICS_DITHER) == 0);
    xbox_SettingSet(XS_GRAPHICS_LOD_BIAS, 1);
    CHECK(st_lod_mode == 1);
    CHECK(recomp_env(RENV_GFX_LOD_BIAS) && !strcmp(recomp_env(RENV_GFX_LOD_BIAS), "auto"));
    xbox_SettingSet(XS_GRAPHICS_LOD_BIAS, 3);
    CHECK(st_lod_mode == 2 && st_lod_val == -1.0f);
    CHECK(recomp_env(RENV_GFX_LOD_BIAS) && !strcmp(recomp_env(RENV_GFX_LOD_BIAS), "-1"));
    xbox_SettingSet(XS_GRAPHICS_LOD_BIAS, 0);
    CHECK(st_lod_mode == 0);
    CHECK(recomp_env(RENV_GFX_LOD_BIAS) && !strcmp(recomp_env(RENV_GFX_LOD_BIAS), "0"));
    xbox_SettingSet(XS_GRAPHICS_DITHER, 1);
    CHECK(st_dither == 1);
    CHECK(recomp_env(RENV_GFX_DITHER) && !strcmp(recomp_env(RENV_GFX_DITHER), "1"));
    xbox_SettingSet(XS_GRAPHICS_DITHER, 0);
    CHECK(st_dither == 0);
    /* The 2D art's scale: off as shipped; the choice's factor, the
     * environment's spelling. */
    CHECK(xbox_SettingValue(XS_GRAPHICS_UI_SCALE) == 0);
    CHECK(!strcmp(xbox_SettingGet(XS_GRAPHICS_UI_SCALE)->key, "graphics.ui_scale"));
    xbox_SettingSet(XS_GRAPHICS_UI_SCALE, 1);
    CHECK(st_ui_scale == 2);
    CHECK(recomp_env(RENV_GFX_UI_SCALE) && !strcmp(recomp_env(RENV_GFX_UI_SCALE), "2x"));
    xbox_SettingSet(XS_GRAPHICS_UI_SCALE, 2);
    CHECK(st_ui_scale == 4);
    CHECK(recomp_env(RENV_GFX_UI_SCALE) && !strcmp(recomp_env(RENV_GFX_UI_SCALE), "4x"));
    xbox_SettingSet(XS_GRAPHICS_UI_SCALE, 0);
    CHECK(st_ui_scale == 0);
    CHECK(recomp_env(RENV_GFX_UI_SCALE) && !strcmp(recomp_env(RENV_GFX_UI_SCALE), "off"));
    /* BO-245: the world's textures: off as shipped, live; the scale and
     * the road switch. */
    CHECK(xbox_SettingValue(XS_GRAPHICS_TEXUP) == 0);
    CHECK(!strcmp(xbox_SettingGet(XS_GRAPHICS_TEXUP)->key, "graphics.texture_upscale"));
    xbox_SettingSet(XS_GRAPHICS_TEXUP, 1);
    CHECK(st_texup == 1);
    CHECK(recomp_env(RENV_GFX_TEXUP) && !strcmp(recomp_env(RENV_GFX_TEXUP), "fsrcnnx"));
    xbox_SettingSet(XS_GRAPHICS_TEXUP, 2);
    CHECK(st_texup == 2);
    xbox_SettingSet(XS_GRAPHICS_TEXUP_SCALE, 1);
    CHECK(st_texup_scale == 4);
    xbox_SettingSet(XS_GRAPHICS_TEXUP_SCALE, 0);
    CHECK(st_texup_scale == 2);
    CHECK(xbox_SettingValue(XS_GRAPHICS_TEXUP_ROADS) == 0);
    xbox_SettingSet(XS_GRAPHICS_TEXUP_ROADS, 1);
    CHECK(st_texup_roads == 1);
    xbox_SettingSet(XS_GRAPHICS_TEXUP_ROADS, 0);
    CHECK(st_texup_roads == 0);
    CHECK(xbox_SettingValue(XS_GRAPHICS_TEXUP_PRELOAD) == 1);   /* BO-272: on as shipped */
    xbox_SettingSet(XS_GRAPHICS_TEXUP_PRELOAD, 0);
    CHECK(st_texup_preload == 0);
    xbox_SettingSet(XS_GRAPHICS_TEXUP_PRELOAD, 1);
    CHECK(st_texup_preload == 1);
    xbox_SettingSet(XS_GRAPHICS_TEXUP, 0);
    CHECK(st_texup == 0);
    xbox_SettingSet(XS_GRAPHICS_AA, 1);
    xbox_SettingSet(XS_GRAPHICS_SHARPEN, 0.4);
    CHECK(st_aa == 1 && st_sharpen > 0.39f && st_sharpen < 0.41f);
    CHECK(recomp_env(RENV_GFX_AA) && !strcmp(recomp_env(RENV_GFX_AA), "fxaa"));
    /* A restart-flagged setting. */
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 2);
    CHECK(xbox_SettingsNeedRestart());
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 1);
    CHECK(!xbox_SettingsNeedRestart());
    /* A value is clamped to its range. The rumble strength: 0 (off) to 3,
     * in quarters, and the runtime is given the stepped value. */
    xbox_SettingSet(XS_INPUT_RUMBLE, 9.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 3.0);     /* the range ends at 3 */
    xbox_SettingSet(XS_INPUT_RUMBLE, -1.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 0.0 && st_rumble == 0.0);
    CHECK(xbox_SettingGet(XS_INPUT_RUMBLE)->min == 0.0 && xbox_SettingGet(XS_INPUT_RUMBLE)->max == 3.0
          && xbox_SettingGet(XS_INPUT_RUMBLE)->step == 0.25);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.75);               /* on the step: as it is */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.75 && st_rumble == 1.75);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.3);                /* the old slider's 0.05 steps */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.25 && st_rumble == 1.25);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.4);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.5);
    xbox_SettingSet(XS_INPUT_RUMBLE, 2.95);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 3.0);
    for (double q = 0.0; q <= 3.0; q += 0.25) {           /* every step is a value */
        xbox_SettingSet(XS_INPUT_RUMBLE, q);
        CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == q);
    }
    {
        char e[32];
        xbox_SettingEnvString(xbox_SettingGet(XS_INPUT_RUMBLE), 1.75, e, sizeof e);
        CHECK(!strcmp(e, "1.75"));
    }
    xbox_SettingSet(XS_INPUT_RUMBLE, 2.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 2.0);

    xbox_SettingSet(XS_DISPLAY_HUD_FIX, 1);          /* late in the enum, section display */
    xbox_SettingSet(XS_AUDIO_MENU_SOUNDS, 0);        /* later still, section audio */
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text != NULL);
    if (text) {
        const char *ms = strstr(text, "menu_sounds = false");
        const char *au = strstr(text, "[audio]");
        CHECK(ms && au && ms > au);
        /* Each section once, its keys together (hud_fix after the other
         * display keys, before the next section), and the strict reader
         * takes the file back. */
        const char *d = strstr(text, "[display]");
        const char *a = strstr(text, "[audio]");
        const char *h = strstr(text, "hud_fix = true");
        CHECK(d && !strstr(d + 1, "[display]"));
        CHECK(d && h && h > d);
        CHECK(h && a && (h < a || a < d));
        {
            char err[256];
            recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
            CHECK(c != NULL);
            if (c) {
                CHECK(recomp_cfg_bool(c, "display.hud_fix", 0) == 1);
                CHECK(recomp_cfg_choice(c, "display.fit", (const char *const[]){ "letterbox", "fill16x10", NULL }, -1) == 1);
                recomp_cfg_free(c);
            }
        }
        CHECK(strstr(text, "mystery.key = 7"));                  /* kept */
        {
            /* A title setting under its own table; the one left at its
             * default is not written. */
            const char *t = strstr(text, "[title]");
            CHECK(t && strstr(t, "crop = \"off\""));
            CHECK(!strstr(text, "video ="));
        }
        {
            /* The text setting: in its section, once, escaped; the strict
             * reader above took the file, so no table is opened twice. */
            const char *m = strstr(text, "custom_music_dir = \"/music/My \\\"Best\\\" \xC5\x90szi \xC5\xB1r\"");
            CHECK(m && a && m > a);
            CHECK(!strstr(text, "audio.custom_music_dir"));
            CHECK(strstr(text, "custom_music_soundtrack = \"\""));             /* BO-230: kept empty */
            CHECK(!strstr(text, "custom_music_at_start"));                      /* untouched: not written */
        }
        CHECK(strstr(text, "mode = \"performance\""));
        CHECK(strstr(text, "deadzone = 6554"));
        CHECK(strstr(text, "overscan = \"off\""));
        CHECK(strstr(text, "fit = \"fill16x10\""));
        CHECK(strstr(text, "filter = \"linear\""));
        CHECK(strstr(text, "mute_background = true"));
        CHECK(strstr(text, "aniso = \"8x\""));
        CHECK(strstr(text, "volume = 0.5"));                     /* locked: the file's own */
        CHECK(!strstr(text, "fullscreen"));                      /* never touched */
        {
            /* The effects' "off" is stored, in the one [graphics] table. */
            const char *g = strstr(text, "[graphics]");
            const char *b = strstr(text, "bloom = 0");
            const char *m = strstr(text, "motion_blur = 0");
            CHECK(g && !strstr(g + 1, "[graphics]"));
            CHECK(g && b && m && b > g && m > g);
            /* And the 60 fps crash cameras' "on", in the same table. */
            {
                const char *c = strstr(text, "crash_60fps = true");
                CHECK(c && c > g);
            }
        }
    }

    /* Two text settings of a section no stepped setting opened: one header
     * (a second [console] would be an error for TOML and the reader). */
    xbox_SettingsSetText("console.hdd_key", "00112233445566778899aabbccddeeff");
    xbox_SettingsSetText("console.eeprom", "/tmp/eeprom.bin");
    xbox_SettingsFlush();
    text = read_file(path);
    {
        const char *cs = text ? strstr(text, "[console]") : NULL;
        char err[256];
        recomp_cfg *c;
        CHECK(cs && !strstr(cs + 1, "[console]"));
        CHECK(cs && strstr(cs, "hdd_key = ") && strstr(cs, "eeprom = "));
        c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c)
            recomp_cfg_free(c);
    }

    /* Back on: the default, and the file forgets it. */
    xbox_SettingReset(XS_GRAPHICS_MOTION_BLUR);
    CHECK(xbox_SettingValue(XS_GRAPHICS_MOTION_BLUR) == 1 && !xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->user);
    CHECK(recomp_env(RENV_GFX_MOTION_BLUR) && !strcmp(recomp_env(RENV_GFX_MOTION_BLUR), "1"));
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "motion_blur") && strstr(text, "bloom = 0"));
    /* Off again: the default, as shipped, and the file forgets it. */
    xbox_SettingReset(XS_GRAPHICS_CRASH_60FPS);
    CHECK(xbox_SettingValue(XS_GRAPHICS_CRASH_60FPS) == 0 && !xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->user);
    CHECK(recomp_env(RENV_GFX_CRASH_60FPS) && !strcmp(recomp_env(RENV_GFX_CRASH_60FPS), "0"));
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "crash_60fps"));

    /* Reset goes back to the host's default and forgets the setting. */
    xbox_SettingReset(XS_DISPLAY_OVERSCAN);
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1 && !xbox_SettingGet(XS_DISPLAY_OVERSCAN)->user);
    CHECK(st_overscan == 1);
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "overscan"));

    /* The shape an older writer left (a split [display]): the strict reader
     * refuses it, the settings reader (RCFG_REOPEN_TABLES) takes it; a key
     * repeated inside it is still an error. */
    {
        char err[256];
        const char *split = "[display]\nfullscreen = true\n[game]\nspeed_units = \"kmh\"\n[display]\nhud_fix = false\n";
        recomp_cfg *c;
        write_file(path, split);
        CHECK(recomp_cfg_load_file(path, err, sizeof err) == NULL && strstr(err, "defined twice"));
        c = recomp_cfg_load_file_flags(path, RCFG_REOPEN_TABLES, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(recomp_cfg_bool(c, "display.fullscreen", 0) == 1 && recomp_cfg_type_of(c, "display.hud_fix") == RCFG_BOOL);
            recomp_cfg_free(c);
        }
        write_file(path, "[display]\nfullscreen = true\n[display]\nfullscreen = false\n");
        CHECK(recomp_cfg_load_file_flags(path, RCFG_REOPEN_TABLES, err, sizeof err) == NULL);
    }

    /* The face-button layout reaches the input layer as it is set. */
    xbox_SettingSet(XS_INPUT_FACE_LAYOUT, 1);
    CHECK(st_face == 1);
    xbox_SettingReset(XS_INPUT_FACE_LAYOUT);
    CHECK(st_face == 0);
    /* The driving controls: xbox unless set, and the input layer hears of a
     * change at once (BO-106). */
    CHECK(xbox_SettingValue(XS_INPUT_CONTROL_SCHEME) == 0);
    CHECK(xbox_SettingGet(XS_INPUT_CONTROL_SCHEME)->def == 0);
    CHECK(!strcmp(xbox_SettingGet(XS_INPUT_CONTROL_SCHEME)->key, "input.control_scheme"));
    xbox_SettingSet(XS_INPUT_CONTROL_SCHEME, 1);
    CHECK(st_scheme == 1);
    xbox_SettingReset(XS_INPUT_CONTROL_SCHEME);
    CHECK(st_scheme == 0);
    CHECK(st_glyph_refresh >= 2);       /* the Nintendo prompts follow it */
    /* The first controller's player: player 1 unless set, and the input
     * layer hears of a change at once. */
    CHECK(xbox_SettingValue(XS_INPUT_FIRST_PAD) == 0);
    CHECK(xbox_SettingGet(XS_INPUT_FIRST_PAD)->def == 0);
    CHECK(!strcmp(xbox_SettingGet(XS_INPUT_FIRST_PAD)->key, "input.first_pad"));
    xbox_SettingSet(XS_INPUT_FIRST_PAD, 1);
    CHECK(st_first_pad == 1);
    xbox_SettingReset(XS_INPUT_FIRST_PAD);
    CHECK(st_first_pad == 0);

    /* The button prompts (input.glyphs): auto by default, a choice reaches
     * the input layer as it is set and is written by its name. */
    CHECK(xbox_SettingValue(XS_INPUT_GLYPHS) == 0);
    xbox_SettingSet(XS_INPUT_GLYPHS, 3);
    CHECK(st_glyphs == 3);
    xbox_SettingsFlush();
    {
        char err[256];
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(!strcmp(recomp_cfg_string(c, "input.glyphs", ""), "playstation"));
            recomp_cfg_free(c);
        }
    }
    xbox_SettingReset(XS_INPUT_GLYPHS);
    CHECK(st_glyphs == 0);
    xbox_SettingSet(XS_INPUT_GLYPHS, 7);                /* positional, the last choice */
    CHECK(st_glyphs == 7);
    xbox_SettingsFlush();
    {
        char err[256];
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(!strcmp(recomp_cfg_string(c, "input.glyphs", ""), "positional"));
            recomp_cfg_free(c);
        }
    }
    xbox_SettingReset(XS_INPUT_GLYPHS);
    CHECK(st_glyphs == 0);
    xbox_SettingSet(XS_INPUT_GLYPHS, 8);                /* keyboard, the last choice */
    CHECK(st_glyphs == 8);
    CHECK(xbox_SettingGet(XS_INPUT_GLYPHS)->choices[8] && !xbox_SettingGet(XS_INPUT_GLYPHS)->choices[9]);
    xbox_SettingsFlush();
    {
        char err[256];
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(!strcmp(recomp_cfg_string(c, "input.glyphs", ""), "keyboard"));
            recomp_cfg_free(c);
        }
    }
    xbox_SettingReset(XS_INPUT_GLYPHS);
    CHECK(st_glyphs == 0);

    /* audio.custom_music_level in the file: written by name when changed,
     * forgotten at the default, read back from the file at start. */
    xbox_SettingSet(XS_AUDIO_CUSTOM_LEVEL, 1);
    xbox_SettingsFlush();
    {
        char err[256];
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(!strcmp(recomp_cfg_string(c, "audio.custom_music_level", ""), "quieter"));
            recomp_cfg_free(c);
        }
    }
    xbox_SettingReset(XS_AUDIO_CUSTOM_LEVEL);
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "custom_music_level"));

    /* graphics.ui_scale in the file: written by name. */
    xbox_SettingSet(XS_GRAPHICS_UI_SCALE, 2);
    xbox_SettingsFlush();
    {
        char err[256];
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(!strcmp(recomp_cfg_string(c, "graphics.ui_scale", ""), "4x"));
            recomp_cfg_free(c);
        }
    }
    /* Where the upscaling is held off (Android for now) this layer does not
     * decide: the stored value stays in the file as the player set it and is
     * handed on as it is; the scaler ignores it and says so
     * (nv2a_texscale_held, tests/nv2a_texscale). So nothing here may rewrite
     * or drop the key. */
    CHECK(st_ui_scale == 4 && xbox_SettingValue(XS_GRAPHICS_UI_SCALE) == 2);
    xbox_SettingReset(XS_GRAPHICS_UI_SCALE);
    CHECK(st_ui_scale == 0);

    remove(path);
    (fprintf)(stderr, "overlay_settings: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/*
 * xbox_settings.h - the settings the in-game overlay edits, and settings.toml.
 *
 * Every setting is one of the runtime's RECOMP_* knobs (recomp_env.h) with a
 * key in the settings file, a type, a range and, where the runtime can take
 * it while running, a live apply. The precedence, high to low:
 *
 *   1. the environment: a RECOMP_* variable the user set; the overlay shows
 *      the setting locked
 *   2. settings.toml in the save directory (recomp_save_dir.h), or the file
 *      RECOMP_SETTINGS names; "none" reads and writes no file
 *   3. the title's defaults (the host's env_default calls, passed to
 *      xbox_SettingsGameDefault), then the toolkit's
 *
 * The file is applied by setting the variables, so every reader of the
 * environment (the backends, the window, the enhancements layer, which then
 * ranks the file over enhance.toml) sees it without knowing it exists.
 * Order in a host's main:
 *
 *   xbox_SettingsInit("Burnout3");       // first: before the defaults
 *   env_default(...) + xbox_SettingsGameDefault(name, value) for each
 *   xbox_SettingsFinish();               // after the last default
 *
 * Only settings the user changed are written; the rest follow the defaults.
 * TOML, not INI: the enhancements layer already reads it (recomp_cfg.h), the
 * file has typed values and sections, and enhance.toml is the same shape.
 * The overlay rewrites the whole file; comments in it are not kept.
 *
 * Values are doubles here: a bool is 0/1, a choice its index.
 */
#ifndef XBOX_OVERLAY_SETTINGS_H
#define XBOX_OVERLAY_SETTINGS_H

#include "recomp_env.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { XS_BOOL, XS_INT, XS_FLOAT, XS_CHOICE } xs_type;

enum {
    XS_RESTART = 1,       /* read at start: a change shows after a restart */
    XS_VIRTUAL = 2,       /* several knobs (the Power page's mode) */
    XS_FROM_BOOL = 4      /* was a switch: a file's or variable's true / false
                           * still reads, as the float's 1 / 0 (BO-212) */
};

typedef enum {
    XS_DISPLAY_FULLSCREEN,
    XS_DISPLAY_ASPECT,
    XS_DISPLAY_FILTER,
    XS_DISPLAY_OVERSCAN,
    XS_DISPLAY_FIT,
    XS_GRAPHICS_RENDER_SCALE,
    XS_GRAPHICS_ANISO,
    XS_GRAPHICS_AA,
    XS_GRAPHICS_SHARPEN,
    XS_GRAPHICS_RENDERER,
    XS_AUDIO_VOLUME,
    XS_AUDIO_MUTE,
    XS_AUDIO_MUTE_BG,
    XS_AUDIO_SPEAKERS,
    XS_AUDIO_FX_LEVEL,
    XS_AUDIO_MIXER,          /* the host mixer (apu_bus.h); its mix is in mixer.toml */
    XS_INPUT_DEADZONE,
    XS_INPUT_TRIGGER_DEADZONE,
    XS_INPUT_RUMBLE,
    XS_INPUT_FACE_LAYOUT,    /* xbox / nintendo (A-B and X-Y mirrored) */
    XS_INPUT_CONTROL_SCHEME, /* xbox / ps2: the driving controls, where the title offers them */
    XS_INPUT_FIRST_PAD,      /* player1 / player2: the first controller's player (keyboard split screen) */
    XS_POWER_MODE,
    XS_ADVANCED_PERF,
    XS_GAME_SPEED_UNITS,     /* an opt-in guest patch (recomp_patch.h) */
    XS_DISPLAY_HUD_FIX,      /* another: the HUD's shape in 16:9 */
    XS_DISPLAY_ULTRAWIDE,    /* another: the 3D wider than 16:9 */
    XS_GRAPHICS_BLOOM,       /* another: the title's own bloom, on as shipped */
    XS_GRAPHICS_MOTION_BLUR, /* another: the title's own motion blur, on as shipped */
    XS_GRAPHICS_CRASH_60FPS, /* another: the title's 30 Hz phases at 60, off as shipped */
    XS_GRAPHICS_SPLIT_60FPS, /* another: the title's 30 Hz split screen at 60, off as shipped */
    XS_GRAPHICS_HIGH_REFRESH, /* another: frames at the display's rate, the simulation at 60; needs the two above */
    XS_AUDIO_SPLIT_MUSIC,    /* another: the title's music in split screen, off as shipped */
    XS_GRAPHICS_LOD_BIAS,    /* extra mip LOD bias: off, auto (from the render scale), or a value */
    XS_GRAPHICS_DITHER,      /* noise dither in the presenter's scale to the window (Metal) */
    XS_GRAPHICS_VEHICLE_DETAIL, /* an opt-in guest patch: the title's vehicle LOD distances */
    XS_GRAPHICS_SCENERY_DETAIL, /* another: the title's scenery-object LOD distance */
    XS_INPUT_GLYPHS,         /* auto / xbox / xbox_modern / playstation / playstation5 / nintendo / steam / positional / keyboard (pad_glyphs.h) */
    XS_AUDIO_LFE_LOWPASS,    /* the LFE feed's low-pass: off / 80 / 100 / 120 Hz (apu_gp.h) */
    XS_DISPLAY_PRESENT_TIMING, /* legacy / auto: when frames are presented (present_cadence.h) */
    XS_ADVANCED_PERF_STEPS,  /* the graph marks frames of more than one simulation step */
    XS_ADVANCED_DIAG_LOG,    /* diagnostic logging: the print gate (platform/xr_log.h) */
    XS_GRAPHICS_UI_SCALE,    /* the title's 2D art scaled before upload: off, 2x, 4x (nv2a_texscale.h) */
    XS_INTERFACE_MENU_ENTRY, /* the overlay's entry in the title's own Settings menu, on by default */
    XS_AUDIO_CUSTOM_LEVEL,   /* the user's own music against the title's: off / quieter / match / louder (apu_level.h) */
    XS_DISPLAY_SCORE_BOARD,  /* a title hook: the HUD's score board as shipped or smooth */
    XS_DISPLAY_RIVAL_MARKER, /* an opt-in guest patch: where the marker over a rival changes look */
    XS_AUDIO_MENU_SOUNDS,    /* the title's own menu sounds in the overlay, on by default */
    XS_GRAPHICS_HDR,         /* BO-110: HDR output (EDR on macOS), off = the SDR path */
    XS_GRAPHICS_HDR_PEAK,    /* the display's peak, nits; 0: what the display reports */
    XS_GRAPHICS_HDR_PAPER,   /* the scene's paper white, nits */
    XS_GRAPHICS_HDR_HUD,     /* the HUD's and menus' white, nits; 0: the paper white */
    XS_GRAPHICS_HDR_SHADOWS, /* Vulkan: the power on luminance below mid grey, 1 = the art */
    XS_GRAPHICS_HDR_HIGHLIGHTS, /* Vulkan: the gain on light past paper white, 1 = as stacked */
    XS_GRAPHICS_HDR_DECODE,  /* Vulkan: 2.2 / srgb, how 8-bit values become light (no row) */
    XS_DISPLAY_FRAME_PACING, /* auto / low_latency / smooth: the present queue (fb_present_vulkan.c, BO-110) */
    XS_GAME_RACE_CLOCK,      /* an opt-in guest patch: a race keeps real time on a slow host from its start, off as shipped */
    XS_AUDIO_CUSTOM_AT_START, /* BO-230: with a music folder, start on the custom soundtrack last chosen, on as shipped */
    XS_GRAPHICS_TEXUP,       /* BO-245: the world's textures upscaled on the GPU: off / fsrcnnx / nnedi3 (nv2a_texup.h) */
    XS_GRAPHICS_TEXUP_SCALE, /* and its factor: 2x / 4x */
    XS_GRAPHICS_TEXUP_ROADS, /* and the road surfaces too (off: the title's road skip list) */
    XS_DISPLAY_CRASHNAV_MAP, /* BO-222: with the widescreen HUD, a title's map screen full width or 4:3 */
    XS_GRAPHICS_TEXUP_PRELOAD, /* BO-272: the upscaler works through the loading screen and countdown, on as shipped */
    XS_GRAPHICS_SOFTBODY,    /* a title's soft-body vehicle damage (drawn only), off as shipped; read at start */
    XS_GAME_LANGUAGE,        /* the dashboard language the kernel reports (XC_LANGUAGE); a row only where the title has more than one */
#if defined(__ANDROID__)
    XS_INPUT_VIBRATION,      /* pad motors or the device's vibrator (android_vibration.c) */
    XS_INPUT_TOUCH,          /* the on-screen controls: auto / always / off (touch_pad.h) */
    XS_INPUT_TOUCH_OPACITY,
    XS_INPUT_TOUCH_SCALE,
    XS_INPUT_TOUCH_AUTO_GAS,
#endif
    /* A title's own settings (xbox_SettingsAddTitle), in the order it gave
     * them; a slot no title filled has the key "" and no row. */
    XS_TITLE_0,
    XS_TITLE_LAST = XS_TITLE_0 + 7,
    XS_COUNT
} xs_id;
#define XS_TITLE_MAX (XS_TITLE_LAST - XS_TITLE_0 + 1)

typedef struct xs_setting {
    const char *key;                  /* "display.overscan" */
    xs_type type;
    unsigned flags;
    recomp_env_id env;                /* RENV_COUNT: none (XS_VIRTUAL) */
    const char *const *choices;       /* XS_CHOICE: NULL-terminated */
    const char *const *env_vals;      /* the knob's spelling per choice, or NULL */
    double min, max;                  /* INT and FLOAT */
    double builtin;                   /* the toolkit's default */
    double step;                      /* FLOAT: values are multiples of it; 0: any */
    /* state */
    double value;                     /* now */
    double filev;                     /* what the file said (kept while locked) */
    double def;                       /* with the title's defaults */
    double launch;                    /* when Finish ran: restart compares */
    int user;                         /* set by the file or the overlay */
    int locked;                       /* the environment sets it */
} xs_setting;

/* The save directory's settings.toml (or RECOMP_SETTINGS), read, and the
 * variables it sets applied. `title` names the save directory
 * (recomp_save_dir). Before any recomp_env lookup that matters: it ends by
 * re-reading the cached environment. */
void xbox_SettingsInit(const char *title);
/* A default the host gives a variable (its env_default); the setting's
 * "reset" value. Call after xbox_SettingsInit. */
void xbox_SettingsGameDefault(const char *env_name, const char *value);
/* After the host's last default: the settings take the values the
 * environment now holds, and are applied. */
void xbox_SettingsFinish(void);

const xs_setting *xbox_SettingGet(xs_id id);
double xbox_SettingValue(xs_id id);
/* Set, apply live where it can be, remember it for the file. No effect on a
 * locked setting. */
void xbox_SettingSet(xs_id id, double v);
/* Back to the default; the file forgets it. */
void xbox_SettingReset(xs_id id);
void xbox_SettingsResetAll(void);
/* A restart-flagged setting differs from the value at launch. */
int  xbox_SettingsNeedRestart(void);
/* Write settings.toml if anything changed since the last write. */
void xbox_SettingsFlush(void);
/* The file in use, or "" (none). */
const char *xbox_SettingsPath(void);

/* A text setting (audio.custom_music_dir: a folder): what the file holds, or
 * the environment's when it sets the variable (*locked, may be NULL). ""
 * when unset or unknown. Set: remembered for the file (an empty value
 * removes the key); the runtime reads it at the next start. */
/* Always UTF-8: the file is; on Windows the environment's value is taken
 * from the wide environment and converted (the narrow one is in the ANSI
 * code page), so a folder with any letters in its name opens. A reader of
 * such a setting uses this, not getenv. */
const char *xbox_SettingsText(const char *key, int *locked);
void        xbox_SettingsSetText(const char *key, const char *value);
/* 1 when the text setting was given a value (the file has the key, or it was
 * set): for a key whose empty value means something (audio.custom_music_soundtrack,
 * an empty string kept in the file), absent and "" differ. */
int         xbox_SettingsTextIsSet(const char *key);

/* A title's own settings: rows the overlay shows on one of its pages beside
 * the toolkit's, for what only that title has (Double-S.T.E.A.L 2: the
 * dashboard's video mode, the language, its logo movie). Each is a knob of
 * recomp_env.h like the others (the title's RECOMP_ENV_GAME_KEYS included):
 * kept in settings.toml under `key`, applied by setting the variable, its
 * default the title's own through xbox_SettingsGameDefault. `values` are the
 * spellings of the file and the variable and `names` what the row shows, as
 * many of each, NULL-terminated; values NULL is a switch (1 / 0, true /
 * false). `apply` (may be NULL) is called with the new index when the row
 * changes it, for a title that can take it at once; otherwise it is read at
 * the next start (`restart`: the row is tagged and a restart offered; 0 for
 * one that only matters at start anyway, like a logo movie). Up to
 * XS_TITLE_MAX, before xbox_SettingsInit; the table must stay valid. */
enum { XS_PAGE_DISPLAY, XS_PAGE_GRAPHICS, XS_PAGE_AUDIO, XS_PAGE_INPUT, XS_PAGE_ADVANCED };
typedef struct xbox_title_setting {
    const char *key;
    recomp_env_id env;
    const char *const *values;
    const char *const *names;
    const char *label;
    const char *hint;
    int page;                         /* XS_PAGE_* */
    int restart;
    void (*apply)(int value);
} xbox_title_setting;
void xbox_SettingsAddTitle(const xbox_title_setting *list, int n);
/* The title's entry behind a slot, NULL for any other setting. */
const xbox_title_setting *xbox_SettingsTitleOf(xs_id id);

/* A toolkit setting this title has no use for (a picture upscaler, a crop
 * its own setting does better): no row on any page. The value still follows
 * the file and the environment. Any time before the overlay opens. */
void xbox_SettingsHide(xs_id id);
int  xbox_SettingsHidden(xs_id id);

/* The help line of the "Entry in the game's menu" row, for the title whose
 * menu has one (the default names Burnout 3's TAKEDAWN SETTINGS); NULL keeps
 * it. Copied. */
void xbox_SettingsSetMenuEntryHint(const char *hint);
const char *xbox_SettingsMenuEntryHint(void);   /* NULL: the default */

/* The power mode's "stock" (Maximum, the busy-wait) is offered on some hosts
 * only (BO-75: on the Deck and Windows it measured no steadier than Balanced
 * and costs 2.7 W / stutters). Where it is hidden the overlay steps over it
 * and a stored "stock" is read as "performance" at load (the file itself is
 * left as it is until the next save). The spin pacing stays reachable with
 * RECOMP_PRESENT_PACING=spin. The choice is one table in xbox_settings.c. */
typedef enum { XS_HOST_MACOS, XS_HOST_WINDOWS, XS_HOST_ANDROID, XS_HOST_LINUX } xs_host;
xs_host xbox_SettingsHost(void);               /* the host this was built for */
void    xbox_SettingsSetHost(xs_host h);       /* tests */
int     xbox_PowerStockHidden(xs_host h);
/* The power mode (0 stock, 1 performance, 2 battery, 3 auto) a stored value means on h. */
double  xbox_PowerModeAtLoad(xs_host h, double stored);

/* The window is in the background: audio.mute_background silences it. */
void xbox_SettingsSetBackground(int bg);

/* The languages the title has on its disc, bit i for game.language's
 * choice i (bit 0 English ... bit 8 Portuguese, XC_LANGUAGE - 1). The
 * Language row shows when there is more than one and steps over the
 * others. 0 (the default) shows no row. */
void     xbox_SettingsSetLanguages(unsigned mask);
unsigned xbox_SettingsLanguages(void);

/* The bug report's parts that come from here (xbox_report.h): settings.toml
 * attached, the settings with their sources, the shader cache's counts.
 * Once, at start (xbox_settings_report.c). */
void xbox_SettingsReportRegister(void);

/* The environment spelling of a value, for the overlay's hints. */
void xbox_SettingEnvString(const xs_setting *s, double v, char *out, unsigned n);

/* The line at the top of the overlay's Advanced page: the port's name and
 * version (NULL or "": none). Here and not in overlay.h, which needs SDL. */
void xbox_OverlaySetAbout(const char *text);

/* The title's own fonts, for the overlay's titles, labels and keys: two
 * bitmap fonts in its executable, read from the player's copy at run time
 * (nothing of them is in this repository or written to disk). A font is a
 * DXT5 atlas (w x h, blocks in rows) at tex_off and a glyph table of `count`
 * 32-byte records at table_off: seven floats (u, v, w, h in the atlas;
 * x and y offset from the cell's top left; advance; all fractions of the
 * atlas's size) and the character as a 32-bit UTF-16 code. `body` is the
 * text font, `title` the large one. The file must be `size` bytes with this
 * SHA-256 (64 hex digits); else, or when it cannot be read, the overlay
 * keeps its own font, quietly. Read and prepared on a thread of its own;
 * call it once, before or after the window. Here and not in overlay.h,
 * which needs SDL.
 * RECOMP_OVERLAY_GAME_FONT=0 keeps the overlay's own font. */
typedef struct {
    uint32_t table_off, count;
    uint32_t tex_off, w, h;
} xbox_overlay_bitmap_font;
void xbox_OverlaySetGameFonts(const char *path, uint64_t size, const char *sha256_hex,
                              const xbox_overlay_bitmap_font *body, const xbox_overlay_bitmap_font *title);
/* 1 the fonts are in use, -1 they are not (none named, another file),
 * 0 still being prepared: waits up to wait_ms for an answer. */
int xbox_OverlayGameFontsState(unsigned wait_ms);

/* The title's own menu pictures, for the overlay's sprites (the arrows and
 * the cursor, the rounded value boxes, the tick bar, the d-pad, the grid and
 * the big arc behind the rows, the button prompts of the footer): small DXT
 * images inside a file of the player's install (Burnout 3: Data/Global.txd),
 * read at run time like the fonts (nothing of them is in this repository or
 * written to disk), made 4x larger with Super-xBR in memory on a thread of
 * its own, and drawn through one texture of the overlay's. The file must be
 * `size` bytes with this SHA-256; else, or when it cannot be read, the overlay
 * keeps the look it has without them (drawn shapes, text), quietly. A sprite
 * is `w` x `h` texels of DXT5 (dxt 5) or DXT1 (dxt 1), blocks in rows, from
 * `off` in the file. `sprites` has XBOX_OVERLAY_ART_COUNT entries, in the
 * order of the enum. Call once, after xbox_OverlaySetPadSheets, before or
 * after the window. RECOMP_OVERLAY_GAME_ART=0 keeps the drawn look. */
enum {
    XBOX_OVERLAY_ART_ARROW,        /* 64x32: up triangle, left triangle, the tick */
    XBOX_OVERLAY_ART_BUTTONS,      /* 128x64: the prompt sheet (B A X Y, white, black, L, R) */
    XBOX_OVERLAY_ART_DPAD,         /* 32x16: down and right triangles */
    XBOX_OVERLAY_ART_GRID,         /* 16x16: a line along the top and the left */
    XBOX_OVERLAY_ART_BIG_CURVE,    /* 128x128: the arc */
    XBOX_OVERLAY_ART_BOX_CURVE,    /* 32x32: a box's rounded corner, outline and shade */
    XBOX_OVERLAY_ART_BOX_CURVE_B,  /* 32x32: the same corner as a filled shape */
    XBOX_OVERLAY_ART_CURVE16,      /* 16x16: the small corner */
    XBOX_OVERLAY_ART_CURVE16_B,
    XBOX_OVERLAY_ART_COUNT
};
typedef struct {
    uint32_t off, w, h, dxt;
} xbox_overlay_sprite;
void xbox_OverlaySetGameArt(const char *path, uint64_t size, const char *sha256_hex,
                            const xbox_overlay_sprite *sprites);
/* 1 the pictures are in use, -1 they are not (none named, another file), 0
 * still being prepared: waits up to wait_ms for an answer. */
int xbox_OverlayGameArtState(unsigned wait_ms);

/* More of the title's menu pictures (BO-190), each named with its own file
 * (Burnout 3: Data/Global.txd and Data/Frontend.txd), read with the ones
 * above and checked the same way (`size` bytes with this SHA-256; each file
 * is read once). A file that cannot be used costs only its own pictures: the
 * rows they belong to keep their drawn look. `fmt` is 1 (DXT1), 5 (DXT5) or
 * 8 (8-bit palette, Xbox swizzled, the palette's 0x40-byte header and 256
 * B, G, R, A colours right after the pixels). An entry with path NULL is not
 * used. `sprites` has XBOX_OVERLAY_ART2_COUNT entries, in the order of the
 * enum; it is copied. Call before xbox_OverlaySetGameArt. */
enum {
    XBOX_OVERLAY_ART2_VOLUMEBAR,   /* 128x32 (P8): the audio volume bar, twenty cells
                                    * of 6 texels, full above (16 high) and empty below */
    XBOX_OVERLAY_ART2_TICK,        /* 64x64: a blue disc with a white tick (on) */
    XBOX_OVERLAY_ART2_CIRCLE,      /* 32x32: a white ring (off) */
    XBOX_OVERLAY_ART2_PADLOCK,     /* 32x32 (P8): a padlock (a setting fixed elsewhere) */
    XBOX_OVERLAY_ART2_COUNT
};
typedef struct {
    const char *path;
    uint64_t size;
    const char *sha256_hex;
    uint32_t off, w, h, fmt;
} xbox_overlay_sprite_file;
void xbox_OverlaySetGameArtMore(const xbox_overlay_sprite_file *sprites);

/* The title's own menu sounds for the overlay (BO-190): a cursor move, a
 * value change, a confirm, a back, the menu opening and closing, a page
 * turned. Each is a stretch of raw mono signed 16-bit PCM inside a file of
 * the player's install (Burnout 3: sound/Fe.awd and sound/Generic.awd),
 * read at run time like the pictures (nothing of it is in this repository
 * or written to disk): `bytes` from `off`, at `rate` Hz, played at `gain_db`
 * (the level the title plays it at). The file must be `size` bytes with this
 * SHA-256; else, or when it cannot be read, that sound is silent, quietly. A
 * cue with path NULL has no sound. They play on the host's output after the
 * mix (mcpx_apu_ui_cue), while the title is paused too, at the gain of the
 * host mixer's bus `bus_key` (apu_bus_find) when the mixer is on, and only
 * with audio.menu_sounds on. `cues` has XBOX_OVERLAY_SND_COUNT entries, in
 * the order of the enum; it is copied. Call once, before or after the
 * window. RECOMP_OVERLAY_MENU_SOUNDS=0 keeps the overlay silent. */
enum {
    XBOX_OVERLAY_SND_MOVE,         /* the cursor up or down */
    XBOX_OVERLAY_SND_CHANGE,       /* a value left or right */
    XBOX_OVERLAY_SND_SELECT,       /* A on a row */
    XBOX_OVERLAY_SND_BACK,         /* B out of a page within the menu */
    XBOX_OVERLAY_SND_OPEN,         /* the menu shows */
    XBOX_OVERLAY_SND_CLOSE,        /* the menu goes */
    XBOX_OVERLAY_SND_PAGE,         /* another page (LB / RB) */
    XBOX_OVERLAY_SND_COUNT
};
typedef struct {
    const char *path;
    uint64_t size;
    const char *sha256_hex;
    uint32_t off, bytes, rate;
    float gain_db;
} xbox_overlay_sound;
void xbox_OverlaySetMenuSounds(const xbox_overlay_sound *cues, const char *bus_key);
/* The same from sounds the port decoded itself (a format the call above
 * does not read, as an XACT wave bank's ADPCM): mono signed 16-bit PCM,
 * `samples` long at `rate` Hz, played at `gain_db`; pcm NULL is no sound.
 * Copied; this or the call above, once. */
typedef struct {
    const int16_t *pcm;
    uint32_t samples, rate;
    float gain_db;
} xbox_overlay_pcm_sound;
#define XBOX_OVERLAY_HAS_PCM_SOUNDS 1
void xbox_OverlaySetMenuSoundsPCM(const xbox_overlay_pcm_sound *cues, const char *bus_key);
/* 1 the sounds are ready (some may be silent), -1 there are none, 0 still
 * being read: waits up to wait_ms for an answer. */
int xbox_OverlayMenuSoundsState(unsigned wait_ms);

/* The button prompts the title draws for each pad (nv2a_texrep.h; the
 * title's Buttons sheet with the port's own drawings over it): variants[v - 1]
 * is the sheet for glyph set v of pad_glyphs.h (v >= 1; set 0 is the title's
 * own sheet, in the file above). The footer shows the prompts of the pad in
 * use from these. The images and rectangles must stay valid (static tables).
 * Call before xbox_OverlaySetGameArt. */
struct nv2a_texrep_variant;
void xbox_OverlaySetPadSheets(const struct nv2a_texrep_variant *variants, unsigned n);

/* The "Check for updates" row of the Advanced page (xbox_update.h): the
 * GitHub repository the port's releases are published in ("owner/name") and
 * the running version ("0.1.0"). Without this call, or on a build that
 * cannot make the request (Android), there is no row. The request is made
 * only when the row is pressed. */
void xbox_OverlaySetUpdateCheck(const char *repo, const char *version);

/* The "Add Steam library art" row of the Advanced page, for a port whose
 * setup program can write the pictures of its existing Steam shortcut. `run`
 * is called on its own thread when the row is pressed; it puts one line for
 * the player in `line` (what was done, or why not) and returns 1 when art was
 * written. The row is for the caller to offer where Steam shortcuts exist; with
 * run NULL, or without this call, there is none. */
void xbox_OverlaySetSteamArt(int (*run)(char *line, unsigned n));

/* Ask for the overlay to open, from any thread (a host hook on the title's
 * thread: a menu entry of the game's own). It opens on the main loop's next
 * turn, as the hotkey opens it. 0 when there is no overlay to open (not
 * built, or switched off). */
int xbox_OverlayRequestOpen(void);

/* Cheats, for the session only: rows on the Advanced page, each switched with
 * A (on asks once more, showing `warning`). While one is on the title's saves
 * go to a copy of the save (recomp_save_sandbox_begin); when the last goes
 * off the player keeps that progress or throws it away. get and set run on
 * the main thread while the overlay is open (the title is paused). Up to 4;
 * call once, after the save root is known (the kernel's paths are set up). */
typedef struct xbox_cheat {
    const char *name;
    const char *warning;
    int  (*get)(void);
    void (*set)(int on);
} xbox_cheat;
void xbox_OverlaySetCheats(const xbox_cheat *list, int n);
/* Whether there is an overlay to open (built and not switched off). */
int xbox_OverlayAvailable(void);

/* The "Custom music folder" row of the overlay's Audio page, for a port
 * whose title plays the user's music (xbox_soundtrack.h); none until this is
 * called. The row opens a folder browser inside the overlay (a pad is
 * enough); the folder picked there is stored as audio.custom_music_dir and
 * handed to `chosen` ("" for "turn off"), on the overlay's thread. `status`
 * fills the line under the row: what is in use, a scan's progress, when a
 * change shows ("" for no line). */
typedef struct xbox_music_folder_ops {
    void (*status)(char *out, unsigned n);
    void (*chosen)(const char *dir);
    /* May be NULL. Further lines under the status, i = 0..3 (files a scan
     * left out, by name and reason): 1 and the text, or 0 for no more. */
    int  (*note)(int i, char *out, unsigned n);
    /* May be NULL. Called when the row is pressed, before the browser
     * opens, on a thread of its own: it may block while the system asks the
     * player (Android's permission to read music files). 1 opens the
     * browser, 0 does not. `line` ("" for none) is shown in the row's hint
     * from then on, either way. While it runs the hint says the game is
     * waiting for an answer and the row does nothing. */
    int  (*access)(char *line, unsigned n);
} xbox_music_folder_ops;
void xbox_OverlaySetMusicFolder(const xbox_music_folder_ops *ops);

/* The "save data" rows of the overlay's Advanced page (recomp_save_dir.h's
 * export and import), for a port that names its title's save: "Export save
 * to home folder" copies UDATA/<title id> and TDATA/<title id> of the save
 * root to a dated folder in `export_dir` and shows the full path; "Import
 * save from home folder" takes the newest export there, "Import save from
 * another folder..." one picked in the folder browser. An import is checked,
 * staged, and done at the next start by the port's call of
 * recomp_save_import_apply (the save in use is set aside first); meanwhile
 * the rows offer "Restart now" and "Cancel the import". `note` is a line
 * for the export's text file (may be NULL). The strings are copied. */
typedef struct xbox_save_io {
    const char *save_dir;
    const char *title_id;
    const char *export_dir;
    const char *note;
} xbox_save_io;
void xbox_OverlaySetSaveIO(const xbox_save_io *io);

/* Restart (recomp_restart.h). The overlay offers "Restart game now" on a
 * page while one is wanted: a setting flagged XS_RESTART in the table
 * differs from its value at launch (xbox_SettingsNeedRestart: the table is
 * the only list), or the port says so here with its reason, a sentence for
 * the row's note (NULL or "": no longer). The Advanced page always has
 * "Restart game". Nothing restarts unless the player picks the row. */
void xbox_OverlayWantRestart(const char *reason);

/* A title's own look for the overlay, in place of the one it has without
 * (Burnout 3's front end): its colours, the frame of its menu screens and
 * its button prompts, made by the port at run time from the player's own
 * data (nothing of them is in this repository or written to disk). The
 * pictures are copied. Without this call nothing changes.
 *
 * A picture is RGBA8 with straight alpha, rows top down; its size and
 * place are texels of the title's 640 x 480 page, which is stretched to the
 * window as the title stretches its own screens. One without pixels (rgba
 * NULL) is not drawn. With a skin the page is opaque (`page` behind it, as
 * the title's own screens hide the game), there is no arc or grid, a row is
 * "LABEL : VALUE" around the middle with `row` behind it, the selected row
 * is told by its colours, the rows fade in without moving, the hint is in
 * the footer's band, and the footer
 * shows the prompts (a pad in use; the keyboard keeps its key caps). The
 * text is upright by `lean` (the overlay's own lean is 0.2) and outlined
 * by `outline_w` of its size. Call once, before or after the window; the
 * overlay takes it at its next frame. RECOMP_OVERLAY_SKIN=0 keeps the
 * overlay's own look. */
#define XBOX_OVERLAY_HAS_SKIN 1          /* for a port that builds on toolkits with and without it */
typedef struct { uint8_t r, g, b; } xbox_overlay_rgb;
typedef struct {
    const uint8_t *rgba;
    uint32_t w, h;
    float x, y;                       /* the top left, page texels (where a place is not implied) */
} xbox_overlay_picture;
typedef struct {
    xbox_overlay_rgb page;            /* behind everything */
    xbox_overlay_rgb text;            /* labels and values */
    xbox_overlay_rgb text_hi;         /* the selected row's label (its colon and value: hint) */
    xbox_overlay_rgb text_dim;        /* locked rows, the other pages' names */
    xbox_overlay_rgb hint;            /* the hint, the open page's name, the prompts' words */
    xbox_overlay_rgb attn;            /* a restart pending, a question asked */
    xbox_overlay_rgb outline;         /* around the letters */
    float outline_w;                  /* its width over the text's size; 0 none */
    float lean;                       /* the italics' lean; 0 upright */
    uint32_t flash_ms;                /* the selected row runs from white to its colours over
                                       * this, again and again; 0 steady */
    xbox_overlay_picture header;      /* at x, y */
    xbox_overlay_picture footer;      /* at x, y; its top is the footer's */
    xbox_overlay_picture title;       /* the page's name in the header, at x, y */
    xbox_overlay_picture row;         /* behind each row, centred on it (x, y unused) */
    xbox_overlay_picture arrow_left;  /* the value and page arrows, centred where they go */
    xbox_overlay_picture arrow_right;
    xbox_overlay_picture prompt_ok;   /* the footer's prompts, left to right from prompts_x: */
    xbox_overlay_picture prompt_back; /* A (select), B (back), Y (default); each whole, */
    xbox_overlay_picture prompt_default; /* its button and its word */
    float tabs_y;                     /* the page names' centre line, page texels */
    float rows_y;                     /* the first row's top */
    float row_pitch;                  /* from one row to the next */
    float hint_y;                     /* the hint's centre line (in the footer's band) */
    float prompts_x, prompts_y;       /* the first prompt's left, the prompts' centre line */
    float prompts_gap;                /* between two prompts */
} xbox_overlay_skin;
void xbox_OverlaySetSkin(const xbox_overlay_skin *skin);

/* The title's text font made in memory, for a port that builds it from the
 * title's data (a format the file fonts above do not read): `coverage` is
 * w x h bytes (0 clear, 255 solid); `cell` is the height text is sized by
 * (texels, the line from the top of the tallest capital's cell to the
 * lowest descender); a glyph is its box in the atlas, its place from the
 * cell's top left and its advance, all texels. It is the body and the
 * title font (as xbox_OverlaySetGameFonts, which it excludes), made 4x
 * larger with Super-xBR the same way. Copied; call once, before or after
 * the window. RECOMP_OVERLAY_GAME_FONT=0 keeps the overlay's own font. */
typedef struct {
    uint32_t code;                    /* UTF-16 */
    uint16_t x, y, w, h;              /* in the atlas */
    int16_t xo, yo;                   /* from the cell's top left */
    uint16_t adv;
} xbox_overlay_atlas_glyph;
void xbox_OverlaySetGameFontAtlas(const uint8_t *coverage, uint32_t w, uint32_t h, uint32_t cell,
                                  const xbox_overlay_atlas_glyph *glyphs, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_SETTINGS_H */

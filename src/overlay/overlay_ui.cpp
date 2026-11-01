/*
 * overlay_ui.cpp - the settings overlay: hotkeys, pause, input routing, the
 * pages and the frame-time graph. See overlay.h.
 *
 * Opening it does three things, on the main thread, in this order: the title
 * stops reading pads and keys (xbox_InputSetBlocked, input_core.c), the title
 * is paused (xbox_HostPause, kernel_pause.h: the GPU thread holds at the next
 * flip, the APU's frame thread idles), and the presenter starts presenting on
 * every turn. Closing undoes them, writes the settings, and keeps the pad
 * neutral until its buttons are let go (the press that closed it is not the
 * title's).
 *
 * The navigation is the overlay's own, not ImGui's: ImGui is the renderer
 * and the mouse. A pad, key or stick event becomes one step on a row (up,
 * down, left, right, activate, back, page, default), with key repeat for a
 * held direction, so what a button does does not depend on ImGui's focus
 * rules. Rows are drawn as ImGui items so the mouse works the same way.
 */
#include "overlay.h"
#include "overlay_internal.h"
#include "overlay_keys.h"
#include "xbox_settings.h"
#include "nv2a_texscale.h"
#include "nv2a_texup.h"
#include "xbox_shader_build.h"
#include "xbox_report.h"
#include "nv2a_pcache.h"
#include "recomp_patch.h"
#include "recomp_env.h"
#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include "imgui.h"
#include <atomic>
#include <mutex>
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"

#include "input_backend.h"
#include "input_map.h"
#include "touch_pad.h"
#include "kernel_pause.h"
#include "present_cadence.h"    /* what reached the glass, for the graph */
#include "kernel_pacing.h"     /* power mode Auto's state (BO-175) */
extern "C" uint64_t xbox_HostNowNs(void);   /* platform/host_time.h */
#include "apu.h"                /* the speaker test */
#include "apu_bus.h"            /* the Mixer page */
#include "overlay_sounds.h"      /* the title's menu sounds (BO-190) */
#include "apu_gp.h"             /* is the title's effects image modelled */
#include "recomp_restart.h"
#include "recomp_save_dir.h"
#include "kernel_save_import.h"   /* Import Xbox save */
#include "xbox_soundtrack.h"    /* the music folder browser's model (xsb_*) */
#include "xbox_update.h"        /* Check for updates */
/* BO-110, HDR (kernel/nv2a_backend_common.h, C). */
extern "C" {
float nv2a_host_hdr_potential(void);
void  nv2a_host_hdr_levels(float *peak_nits, float *paper_nits, float *hud_nits);
void *nv2a_host_hdr_patch(float symbol_nits);
void  nv2a_host_hdr_patch_at(float x0, float y0, float x1, float y1);
void  nv2a_host_hdr_pattern(int kind, float x0, float y0, float x1, float y1, float a, float b);
}

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "overlay_font_ui.h"    /* k_overlay_font_ui: Roboto Condensed Medium, the text font */
#include "overlay_font_ext.h"   /* k_overlay_font_ext: DejaVu Sans Mono, behind it */
#include "overlay_gamefont.h"   /* the title's own fonts, from the player's executable */
#include "overlay_gameart.h"    /* its menu sprites and button prompts, from the player's data */
#include "overlay_skin.h"       /* a title's own look, made by the port */
#include "pad_glyphs.h"          /* the glyph set of the pad in use */

#define LOGP "[OVERLAY] "

/* ── state ─────────────────────────────────────────────────────────── */

static SDL_Window *s_win;
static bool s_enabled;          /* RECOMP_OVERLAY is not 0 */
static bool s_open;
static float s_alpha;           /* 0..1, the fade */
/* The rows coming in (BO-190), as the title's menus bring theirs: each row
 * slides in from the left and fades up, one after another. Timed from frame
 * captures of the title at 60 Hz: a row takes 2 frames from -12% of the
 * screen's width, the next starts 3 frames later on a page change (the
 * front end's Driver Details) and 4 frames later when the pause menu opens.
 * Drawing only: a row takes input where it ends up from the first frame. */
static uint32_t s_rows_t0;      /* when the first row starts; 0: none running */
static uint32_t s_rows_step_ms;
#define ROWS_IN_MS 33u          /* one row: 2 frames */
#define ROWS_STEP_PAGE_MS 50u   /* the next row: 3 frames (a page change) */
#define ROWS_STEP_OPEN_MS 67u   /* 4 frames (the pause menu opening) */
#define ROWS_IN_SLOTS 6         /* rows further down come in with the sixth */
static bool s_core_ready;

/* The window presents on a thread of its own (fb_present_sdl.c, BO-210):
 * the main thread hands the overlay its events, its poll and ImGui's SDL
 * half (xbox_OverlayPlatformFrame), the present thread builds and draws the
 * frame. One mutex around each of those, so ImGui and the overlay's state
 * are used by one thread at a time; neither side holds it across a GPU or
 * display wait. */
static std::mutex s_ov_mu;
/* ImGui_ImplSDL2_NewFrame ran on the main thread and no frame has used it
 * yet (the next one is built with it); ever: once. */
static bool s_plat_pending, s_plat_ever;

void overlay_lock() { s_ov_mu.lock(); }
void overlay_unlock() { s_ov_mu.unlock(); }
static bool s_vulkan;
static uint32_t s_last_ms;
static bool s_last_input_pad;   /* the hints name pad buttons or keys */

/* Hotkey: Back and Start of one controller. */
static unsigned s_back_mask, s_start_mask;      /* by controller index */
static bool s_combo_fired;

/* Navigation: steps counted by the event handler, taken by the frame. */
enum { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_COUNT };
static bool s_dpad[D_COUNT], s_stick[D_COUNT], s_key[D_COUNT];
static uint32_t s_next_fire[D_COUNT];
static int s_steps[D_COUNT];
static int s_activate, s_back, s_page_delta, s_reset;

/* The page and the row on each. */
enum { P_DISPLAY, P_GRAPHICS, P_AUDIO, P_INPUT, P_POWER, P_ADVANCED, P_COUNT };
static const char *const k_page_name[P_COUNT] = {
    "Display", "Graphics", "Audio", "Input", "Power", "Advanced"
};
static int s_page;
static int s_sel[P_COUNT];
static bool s_scroll_to_sel;

/* Settings written a moment after the last change. */
static bool s_save_pending;
static uint32_t s_save_at;

/* Fullscreen follows the setting when the setting changes (not Android). */
#if !defined(__ANDROID__)
static int s_fs_applied = -1;
#endif

/* RECOMP_OVERLAY=toggle=<s>[,<s>...]: the overlay opens and closes at those
 * seconds after the window came up, for scripted runs (the pause, the
 * screenshots) with no pad to press. */
#define TOGGLE_MAX 8
static uint32_t s_toggle_ms[TOGGLE_MAX];
static int s_toggle_n, s_toggle_i;
static uint32_t s_t0_ms;
/* RECOMP_OVERLAY=file=<path>: the overlay toggles whenever that file appears
 * (and removes it), for driving it from a script that knows when. */
/* RECOMP_OVERLAY=aa_cycle=<s>: the anti-aliasing setting steps off, FXAA,
 * SMAA, off ... every <s> seconds, through the settings (the live switch
 * under test, with no pad to press). */
static uint32_t s_cycle_ms, s_cycle_next;
static int s_cycle_i;
static char s_trigger[512];
static uint32_t s_trigger_next;
/* RECOMP_OVERLAY=pacing_file=<path> (a test hook, BO-210): when that file
 * appears it is read (auto, low_latency or smooth) and removed, and
 * display.frame_pacing is set to it through the settings, as the Display
 * page does: a Frame pacing switch mid-race from a script. */
static char s_pacing_file[512];

/* The window's present rate for the graph. */
static volatile uint32_t s_presents;

/* ── rows ──────────────────────────────────────────────────────────── */

enum { FMT_PLAIN, FMT_PERCENT, FMT_PERCENT_OF_MAX, FMT_TIMES, FMT_GAIN, FMT_NITS };

struct Meta {
    xs_id id;                         /* its own index: checked below */
    const char *label;
    const char *hint;
    double step;                      /* a left/right step */
    int fmt;
    const char *const *names;         /* the choices as shown */
    unsigned disabled;                /* choice bits that cannot be picked */
};

static const char *const n_aspect[]    = { "4:3", "16:9", NULL };
static const char *const n_filter[]    = { "Nearest", "Linear", "Integer", NULL };
static const char *const n_overscan[]  = { "Show all", "Crop (TV safe area)", "Fill height", NULL };
static const char *const n_timing[]    = { "Legacy (when ready)", "Auto (even frames)", NULL };
static const char *const n_frame_pacing[] = { "Automatic", "Low latency", "Smooth", NULL };
static const char *const n_fit[]       = { "Letterbox", "Fill 16:10 (crops the sides)", NULL };
static const char *const n_aniso[]     = { "Off", "2x", "4x", "8x", "16x", NULL };
static const char *const n_lod[]       = { "Off", "Auto (from the render scale)", "-0.5", "-1.0", "-1.5", NULL };
static const char *const n_renderer[]  = { "Standard", "Native", NULL };
static const char *const n_aa[]        = { "Off", "FXAA", "SMAA", NULL };
static const char *const n_speakers[]  = { "Auto", "Stereo", "5.1", NULL };
static const char *const n_custom_level[] = { "Off", "Quieter", "Match game music", "Louder", NULL };
static const char *const n_lfe_lowpass[] = { "Off", "80 Hz", "100 Hz", "120 Hz", NULL };
#if defined(__ANDROID__)
static const char *const n_vibration[] = { "Auto", "Controller", "This device", "Off", NULL };
static const char *const n_touch[]     = { "Auto (hidden while a controller is used)", "Always", "Off", NULL };
#endif
#if defined(__ANDROID__)
static const char *const n_scale[]     = { "1x (640x480)", "1.5x (960x720)", "2x (1280x960)", "3x", "4x", NULL };
#endif
static const char *const n_face[]      = { "Xbox layout", "Nintendo layout", NULL };
static const char *const n_scheme[]    = { "Xbox layout", "PS2 layout", NULL };
static const char *const n_first_pad[] = { "Player 1", "Player 2", NULL };
static const char *const n_ui_scale[]  = { "Off", "2x", "4x", NULL };
static const char *const n_texup[]     = { "Off", "FSRCNNX", "NNEDI3", NULL };
static const char *const n_texup_sc[]  = { "2x", "4x", NULL };
static const char *const n_glyphs[]    = { "Auto (the controller in use)", "Xbox (original)", "Xbox (modern: LB RB LT RT)", "PlayStation (DualShock 3, 4)", "PlayStation 5 (DualSense)", "Nintendo", "Steam (Deck, Steam Controller)", "Positional (any pad)", "Keyboard", NULL };
static_assert(sizeof n_glyphs / sizeof n_glyphs[0] == XBOX_GLYPHS_MODE_COUNT + 1, "one name per glyph mode, in pad_glyphs.h order");
static const char *const n_units[]     = { "mph (as shipped)", "km/h", NULL };
static const char *const n_language[]  = { "English", "Japanese", "German", "French", "Spanish", "Italian", "Korean", "Chinese", "Portuguese", NULL };
static const char *const n_ultrawide[] = { "Off", "21:9", "32:9", "Window", NULL };
static const char *const n_vehicle_detail[] = { "Original", "Far (2x)", "Farthest (4x)", NULL };
static const char *const n_scenery_detail[] = { "Original", "Far", NULL };
static const char *const n_score_board[] = { "Original", "Smooth", NULL };
static const char *const n_rival_marker[] = { "Original", "Farther", NULL };
static const char *const n_crashnav_map[] = { "Full width", "4:3", NULL };
static const char *const n_power[]     = { "Maximum (~14 W on Deck)", "Balanced (~10 W, recommended)", "Battery saver (~8 W)", "Auto (adapts)", NULL };
/* Off the Steam Deck the watts are somebody else's: the names alone. */
static const char *const n_power_plain[] = { "Maximum", "Balanced (recommended)", "Battery saver", "Auto (adapts)", NULL };
static bool on_steam_deck()
{
    return recomp_host_is_steam_deck() != 0;
}

static constexpr Meta k_meta[XS_COUNT] = {
    /* XS_DISPLAY_FULLSCREEN */ { XS_DISPLAY_FULLSCREEN, "Fullscreen", "Borderless fullscreen on the desktop.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_ASPECT */     { XS_DISPLAY_ASPECT, "Aspect ratio", "16:9 draws the game in widescreen.", 1, FMT_PLAIN, n_aspect, 0 },
    /* XS_DISPLAY_FILTER */     { XS_DISPLAY_FILTER, "Filter", "How the picture is scaled to the window.", 1, FMT_PLAIN, n_filter, 0 },
    /* XS_DISPLAY_OVERSCAN */   { XS_DISPLAY_OVERSCAN, "Overscan", "Crop cuts the TV border all round. Fill height drops the top and bottom border and stretches to the full height (about 7% taller); the sides stay.", 1, FMT_PLAIN, n_overscan, 0 },
    /* XS_DISPLAY_FIT */        { XS_DISPLAY_FIT, "Present fit", "Fills the window's height on a 16:10 screen.", 1, FMT_PLAIN, n_fit, 0 },
#if defined(__ANDROID__)
    /* XS_GRAPHICS_RENDER_SCALE */ { XS_GRAPHICS_RENDER_SCALE, "Render scale", "Internal resolution as a multiple of the Xbox's 640x480. 1.5x is 960x720.", 1, FMT_PLAIN, n_scale, 0 },
#else
    /* XS_GRAPHICS_RENDER_SCALE */ { XS_GRAPHICS_RENDER_SCALE, "Render scale", "Internal resolution as a multiple of the Xbox's 640x480.", 1, FMT_TIMES, NULL, 0 },
#endif
    /* XS_GRAPHICS_ANISO */    { XS_GRAPHICS_ANISO, "Anisotropic filtering", "Sharper textures at a slant, like the road ahead. Costs GPU time.", 1, FMT_PLAIN, n_aniso, 0 },
    /* XS_GRAPHICS_AA */       { XS_GRAPHICS_AA, "Anti-aliasing", "Smooths jagged edges. FXAA is one cheap pass; SMAA takes three and looks cleaner.", 1, FMT_PLAIN, n_aa, 0 },
    /* XS_GRAPHICS_SHARPEN */  { XS_GRAPHICS_SHARPEN, "Sharpening", "Contrast-adaptive sharpening (AMD CAS). Offsets the softness from anti-aliasing and scaling up.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_GRAPHICS_RENDERER */ { XS_GRAPHICS_RENDERER, "Renderer", "Native is the new renderer, still being tried out. Standard is the one the game has always used.", 1, FMT_PLAIN, n_renderer, 0 },
    /* XS_AUDIO_VOLUME */       { XS_AUDIO_VOLUME, "Master volume", "The game's sound level.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_MUTE */         { XS_AUDIO_MUTE, "Mute", "Silences the game.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_MUTE_BG */      { XS_AUDIO_MUTE_BG, "Mute in the background", "Silences the game while its window is not in front.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_SPEAKERS */     { XS_AUDIO_SPEAKERS, "Speakers", "Auto uses 5.1 when the system's sound output is 5.1 or more, otherwise stereo.", 1, FMT_PLAIN, n_speakers, 0 },
    /* XS_AUDIO_FX_LEVEL */     { XS_AUDIO_FX_LEVEL, "Surround effects (reverb) level", "5.1 only. The level of the game's reverb sends, which play dry here because no effect DSP runs.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_MIXER */        { XS_AUDIO_MIXER, "Mixer", "Gains, sends, LFE and a limiter over the game's sound buses; the Mixer page is below. Off keeps the game's own mix.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_INPUT_DEADZONE */     { XS_INPUT_DEADZONE, "Stick deadzone", "How far the stick can move before it counts as centred.", 1638.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_TRIGGER_DEADZONE */ { XS_INPUT_TRIGGER_DEADZONE, "Trigger deadzone", "How far a trigger can be pulled and still count as released.", 13.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_RUMBLE */       { XS_INPUT_RUMBLE, "Rumble strength", "1.0 is the original Xbox rumble. Higher strengthens the weaker rumbles; the strongest stays at full. 0 turns rumble off.", 0.25, FMT_GAIN, NULL, 0 },
    /* XS_INPUT_FACE_LAYOUT */  { XS_INPUT_FACE_LAYOUT, "Face buttons", "Nintendo layout is for pads with A on the right and B below. It swaps A with B and X with Y, in the game and here.", 1, FMT_PLAIN, n_face, 0 },
    /* XS_INPUT_CONTROL_SCHEME */ { XS_INPUT_CONTROL_SCHEME, "Driving controls", "PS2 layout uses the PlayStation 2 version's driving controls: accelerate on the bottom face button, brake on the left one, boost on the right shoulder, look back on the left shoulder. Applies only while driving.", 1, FMT_PLAIN, n_scheme, 0 },
    /* XS_INPUT_FIRST_PAD */    { XS_INPUT_FIRST_PAD, "First controller", "Player 1: the first controller to press a button is player 1, and the keyboard drives the same player. Player 2: the keyboard is player 1 alone and controllers start at player 2, so a keyboard and a controller can play split screen. Press a button on each controller after a change.", 1, FMT_PLAIN, n_first_pad, 0 },
    /* XS_POWER_MODE */         { XS_POWER_MODE, "Power mode", "Maximum busy-waits throughout (steadiest, hottest). Balanced sleeps, then spins for the last 8 ms of each frame (steady 60). Battery saver only sleeps (coolest, with an occasional small dip). Auto picks the spin as it goes: less while frames are steady, more when they come late.", 1, FMT_PLAIN, n_power, 0 },
    /* XS_ADVANCED_PERF */      { XS_ADVANCED_PERF, "Performance overlay", "FPS and frame-time graph in the corner. Greetings to the DF team!", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GAME_SPEED_UNITS */   { XS_GAME_SPEED_UNITS, "Speed units", "Speed and distance in km/h and metres instead of mph and miles, using the game's own metric display. Applies from the next race. Text stays English.", 1, FMT_PLAIN, n_units, 0 },
    /* XS_DISPLAY_HUD_FIX */    { XS_DISPLAY_HUD_FIX, "Widescreen HUD", "In 16:9 the game stretches its HUD with the picture. On keeps the HUD at its 4:3 proportions, pinned to the screen edges. Applies from the next race. Nothing changes in 4:3.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_ULTRAWIDE */  { XS_DISPLAY_ULTRAWIDE, "Ultrawide", "For displays wider than 16:9, with Aspect set to 16:9. Races fill the whole width, showing more at the sides without stretching. Menus, loading screens and videos stay 16:9 in the middle. Window matches the window's or screen's own shape. Applies from the next race.", 1, FMT_PLAIN, n_ultrawide, 0 },
    /* XS_GRAPHICS_BLOOM */     { XS_GRAPHICS_BLOOM, "Bloom", "How strong the game's own bloom is. 100% is the original.", 0.1, FMT_PERCENT, NULL, 0 },
    /* XS_GRAPHICS_MOTION_BLUR */ { XS_GRAPHICS_MOTION_BLUR, "Motion blur", "How strong the game's own motion blur is. 100% is the original.", 0.1, FMT_PERCENT, NULL, 0 },
    /* XS_GRAPHICS_CRASH_60FPS */ { XS_GRAPHICS_CRASH_60FPS, "60 fps crash cameras", "The game drops to 30 fps in its crash and takedown cameras. On keeps them at 60. Needs a fast machine: if 60 can't be held, the game itself slows down, not just the picture. Leave it off on a Steam Deck or other handheld.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_SPLIT_60FPS */ { XS_GRAPHICS_SPLIT_60FPS, "60 fps split screen", "Two-player split screen runs at 30 fps in the original. On runs it at 60 and draws the scene twice per frame. Needs a fast machine: if 60 can't be held, the game itself slows down. Leave it off on a Steam Deck or other handheld. Applies from the next two-player event.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_HIGH_REFRESH */ { XS_GRAPHICS_HIGH_REFRESH, "High refresh rate", "Draws the game at your display's refresh rate instead of 60 fps, for 120, 144 Hz and faster displays. The game itself still runs at its own 60 steps a second; cars and the camera move smoothly between them. Menus, split screen, and crash cameras unless 60 fps crash cameras is on, stay at the game's own rate. Adds up to one 60 Hz step of delay. Needs a fast machine: every frame is drawn at the full render scale.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_SPLIT_MUSIC */  { XS_AUDIO_SPLIT_MUSIC, "Music in split screen", "The original plays no music in split screen. On plays EA GAMES TRAX or your custom music in two-player Race and Road Rage, as in one-player. Applies from the next two-player event.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_LOD_BIAS */  { XS_GRAPHICS_LOD_BIAS, "Texture sharpness (LOD bias)", "With a render scale above 1x, picks sharper texture levels so the extra pixels show detail. Auto uses -0.5 at 2x and -1.0 at 4x.", 1, FMT_PLAIN, n_lod, 0 },
    /* XS_GRAPHICS_DITHER */    { XS_GRAPHICS_DITHER, "Dither", "Faint noise added when scaling to the window, to hide banding in smooth gradients such as the sky. Metal only for now. Text and HUD at their exact size are unchanged.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_VEHICLE_DETAIL */ { XS_GRAPHICS_VEHICLE_DETAIL, "Vehicle detail distance", "How far away rival cars and traffic keep their detailed models before the game swaps in simpler ones. Far doubles the console's distance, Farthest quadruples it. Costs GPU time with many vehicles on screen. Scenery, the track and where traffic appears stay the same.", 1, FMT_PLAIN, n_vehicle_detail, 0 },
    /* XS_GRAPHICS_SCENERY_DETAIL */ { XS_GRAPHICS_SCENERY_DETAIL, "Scenery detail", "How far away trees and other roadside objects keep their detailed models before the game swaps in simpler ones. Original is the console's distance; Far moves the swap further out, so trees no longer change shape as you come round a bend. Costs a little GPU time. Changes at once.", 1, FMT_PLAIN, n_scenery_detail, 0 },
    /* XS_INPUT_GLYPHS */       { XS_INPUT_GLYPHS, "Button prompts", "The button icons in the game's menus and hints. Auto follows the controller you used last. Xbox (original) is the game's own art. Positional works with any pad: a diamond of four dots, with the button to press filled. Keyboard shows key caps. Auto picks it while you play on the keyboard. The game's text and videos still name Xbox buttons.", 1, FMT_PLAIN, n_glyphs, 0 },
    /* XS_AUDIO_LFE_LOWPASS */  { XS_AUDIO_LFE_LOWPASS, "Subwoofer low-pass (LFE)", "120 Hz matches the console's Dolby Digital encoder. Off sends the full-range LFE feed.", 1, FMT_PLAIN, n_lfe_lowpass, 0 },
    /* XS_DISPLAY_PRESENT_TIMING */ { XS_DISPLAY_PRESENT_TIMING, "Frame presentation", "When a finished frame goes to the display. Legacy: as soon as it is ready. Auto: each frame is aimed at one refresh, so frames stay on screen equally long and appear sooner after they are drawn. On a fixed 60, 120 or 240 Hz display the game's clock follows the display; on variable refresh (VRR, ProMotion) frames go out at even intervals. The performance overlay's on-glass line shows the result.", 1, FMT_PLAIN, n_timing, 0 },
    /* XS_ADVANCED_PERF_STEPS */ { XS_ADVANCED_PERF_STEPS, "Graph: simulation steps", "Marks frames where the game ran more than one simulation step on the frame-time graph. It does that when its frame rate drops.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_ADVANCED_DIAG_LOG */  { XS_ADVANCED_DIAG_LOG, "Diagnostic logging", "Off logs only errors and a few start-up facts. On writes the full log (about 830 KB a run at most, the last three runs kept), and bug reports include it. To capture a problem: switch it on, restart so the log has the start, reproduce it, then create the report.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_UI_SCALE */  { XS_GRAPHICS_UI_SCALE, "Menu and HUD upscaling", "Redraws the game's 2D art (menu text, HUD, icons, logos) at 2x or 4x with an edge-following scaler (Super-xBR), for render scales above 1x. The 3D world is untouched. Sharper versions are made in the background and kept on disk, so new art can look unscaled at first. Uses more texture memory.", 1, FMT_PLAIN, n_ui_scale, 0 },
    /* XS_INTERFACE_MENU_ENTRY */ { XS_INTERFACE_MENU_ENTRY, "Entry in the game's menu", "Adds TAKEDAWN SETTINGS to the game's Settings menu (Driver Details > Settings). It opens this menu, like the hotkey. Shows the next time that menu opens.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_CUSTOM_LEVEL */ { XS_AUDIO_CUSTOM_LEVEL, "Custom music loudness", "Modern songs are mastered louder than the game's music, and the game plays them louder still, so they distort. Match game music brings each song to the level of EA GAMES TRAX with a slow gain that never pumps within a song. Quieter and Louder sit 4 dB below and above that. Off plays songs as the game sends them. Custom music only.", 1, FMT_PLAIN, n_custom_level, 0 },
    /* XS_DISPLAY_SCORE_BOARD */ { XS_DISPLAY_SCORE_BOARD, "Score board", "The SCORE board at the top of the screen. Original is the console's look: the dark plate behind the score vanishes a moment after its text fades. Smooth fades the plate with the text, and fades the board back in after a crash camera. Changes at once.", 1, FMT_PLAIN, n_score_board, 0 },
    /* XS_DISPLAY_RIVAL_MARKER */ { XS_DISPLAY_RIVAL_MARKER, "Rival marker switch", "The marker over a rival car is a small arrow far away and its race position (4th) near. Original is the console's: it changes at 35 metres, checked twice a second, so a rival you close on can be nearer when it changes. Farther shows the position from 60 metres and the arrow again past 75, checked every frame, so the change is earlier and does not flip back and forth. Changes at once.", 1, FMT_PLAIN, n_rival_marker, 0 },
    /* XS_AUDIO_MENU_SOUNDS */  { XS_AUDIO_MENU_SOUNDS, "Menu sounds", "This menu clicks with the game's own menu sounds, read from your copy of the game.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_HDR */       { XS_GRAPHICS_HDR, "HDR", "High dynamic range output on a display that can show it (on macOS: a display with HDR or XDR brightness headroom; on Steam Deck and Linux: game mode with HDR switched on in the display settings; on Windows: Use HDR switched on for the display in Windows' display settings). The game's light keeps its strength: lamps, brake lights, sparks, the boost glow and the sun go brighter than white, while the HUD and menus stay at their own brightness. Off is the original picture.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_HDR_PEAK */  { XS_GRAPHICS_HDR_PEAK, "Peak brightness", "The brightest the display can show. Display uses what the display reports (on a Steam Deck that is right). To set it by eye: raise it until the symbol in the box disappears into the bright background, then stop.", 50.0, FMT_NITS, NULL, 0 },
    /* XS_GRAPHICS_HDR_PAPER */ { XS_GRAPHICS_HDR_PAPER, "Paper white", "How bright the game's ordinary white is: the scene's overall brightness. Light above it uses the room up to the peak. Raise it if the whole picture looks dimmer than without HDR. The box shows paper white (left) beside the HUD's white (right).", 10.0, FMT_NITS, NULL, 0 },
    /* XS_GRAPHICS_HDR_HUD */   { XS_GRAPHICS_HDR_HUD, "HUD and menu brightness", "How bright the HUD, the game's menus and its text are. Same as paper white unless set.", 10.0, FMT_NITS, NULL, 0 },
    /* XS_GRAPHICS_HDR_SHADOWS */ { XS_GRAPHICS_HDR_SHADOWS, "Shadows", "Deepens the dark parts of the picture. 1.00 is the game's own, as it looks without HDR; higher makes shadows and night darker without touching the bright parts. 1.10 suits an OLED in a dark room; above 1.20 the darkest steps start to merge.", 0.05, FMT_GAIN, NULL, 0 },
    /* XS_GRAPHICS_HDR_HIGHLIGHTS */ { XS_GRAPHICS_HDR_HIGHLIGHTS, "Highlight gain", "Strengthens the light above paper white (lamps, sparks, flames) before it rolls off to the peak. 1.00 is as the game stacks it; more helps a display with little room above paper white.", 0.1, FMT_GAIN, NULL, 0 },
    /* XS_GRAPHICS_HDR_DECODE */ { XS_GRAPHICS_HDR_DECODE, "Gamma", "How the game's colours become light: 2.2 as a TV shows it, or the sRGB curve (lighter near black).", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_FRAME_PACING */ { XS_DISPLAY_FRAME_PACING, "Frame pacing", "Low latency shows each frame at the first refresh it can make. Smooth keeps one finished frame in hand, so a late frame does not show as a repeated one; it adds one refresh (16.7 ms at 60 Hz) to the delay from the pad to the screen. Automatic picks Smooth on the Steam Deck and Low latency elsewhere. On a variable-refresh display (G-Sync, FreeSync) or one well above 60 Hz there is no queue to keep, and Smooth works like Low latency.", 1, FMT_PLAIN, n_frame_pacing, 0 },
    /* XS_GAME_RACE_CLOCK */    { XS_GAME_RACE_CLOCK, "Potato mode", "Keeps the race at real speed on a slow machine by skipping frames. Without it, a machine under 60 fps runs the race in slow motion until the first crash. Leave it off on a fast PC: at 60 fps it gains nothing, and lap times can come out a few seconds slower. Applies from the next race.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_CUSTOM_AT_START */ { XS_AUDIO_CUSTOM_AT_START, "Custom music at start", "With a music folder chosen, the game starts on the custom soundtrack you last picked in Driver Details > Settings > EA GAMES TRAX (the first one until you pick), so you do not have to pick it every time. Picking EA GAMES TRAX there starts it on EA GAMES TRAX from then on. Off starts on EA GAMES TRAX every time, as the original does. Applies from the next start.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_TEXUP */     { XS_GRAPHICS_TEXUP, "Texture upscale", "Sharpens the textures of the world (signs, buildings, cars) on the graphics card while you play. FSRCNNX is sharper, NNEDI3 softer. A texture first shows as the game has it and turns sharp a moment later. Sharpened textures are kept on disk, so the next race has them at once. See-through parts (fences, trees) keep the game's own edges. Uses more video memory. Off shows the game's own textures.", 1, FMT_PLAIN, n_texup, 0 },
    /* XS_GRAPHICS_TEXUP_SCALE */ { XS_GRAPHICS_TEXUP_SCALE, "Upscale scale", "How much larger the sharpened textures are. 4x looks finer up close, but it needs four times the video memory of 2x and takes longer to fill in. Textures over 256 pixels a side get 2x.", 1, FMT_PLAIN, n_texup_sc, 0 },
    /* XS_GRAPHICS_TEXUP_ROADS */ { XS_GRAPHICS_TEXUP_ROADS, "Upscale road surfaces", "Off leaves the road surfaces as the game has them: the upscalers can make their fine grain look noisy. On upscales them too.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_CRASHNAV_MAP */ { XS_DISPLAY_CRASHNAV_MAP, "Crash Nav map", "The map in Crash Nav (Select Location) with Widescreen HUD on in 16:9. Full width spreads the map, its route and its markers across the screen like the frame around it. 4:3 keeps the map the shape of the rest of the menu, with the frame beside it. Changes at once.", 1, FMT_PLAIN, n_crashnav_map, 0 },
    /* XS_GRAPHICS_TEXUP_PRELOAD */ { XS_GRAPHICS_TEXUP_PRELOAD, "Upscale while loading", "Does most of the sharpening during the loading screen and the countdown, so the first race on a new track has less of it to do; a fast PC can leave it off.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_SOFTBODY */  { XS_GRAPHICS_SOFTBODY, "Soft-body damage", "Cars crumple, tear and bend where they are hit, with the game's own sparks and debris where metal rips, instead of only losing panels. Drawn only: handling and the race are unchanged. Costs CPU in big pile-ups. Applies after a restart.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GAME_LANGUAGE */      { XS_GAME_LANGUAGE, "Language", "The language of the game's text, speech and films, as an Xbox set to that language would show them. Only the languages on your disc are offered. Applies after a restart.", 1, FMT_PLAIN, n_language, 0 },
#if defined(__ANDROID__)
    /* XS_INPUT_VIBRATION */    { XS_INPUT_VIBRATION, "Vibration", "Where rumble goes. Auto uses this device's vibrator while its built-in controls are in use. External controllers don't rumble on Android; This device vibrates anyway.", 1, FMT_PLAIN, n_vibration, 0 },
    /* XS_INPUT_TOUCH */        { XS_INPUT_TOUCH, "Touch controls", "On-screen controls. In a race: steer with the left thumb anywhere on the left half, GAS, BRAKE and BOOST on the right (BOOST keeps accelerating). In menus: a d-pad and A B X Y. Pause and this menu are at the top, left of centre. Auto hides them while a controller is used and shows them again when you touch the screen.", 1, FMT_PLAIN, n_touch, 0 },
    /* XS_INPUT_TOUCH_OPACITY */ { XS_INPUT_TOUCH_OPACITY, "Touch controls opacity", "How solid the on-screen controls look.", 0.1, FMT_PERCENT, NULL, 0 },
    /* XS_INPUT_TOUCH_SCALE */  { XS_INPUT_TOUCH_SCALE, "Touch controls size", "100% suits a phone or a 6-7 inch handheld held in both hands.", 0.1, FMT_PERCENT, NULL, 0 },
    /* XS_INPUT_TOUCH_AUTO_GAS */ { XS_INPUT_TOUCH_AUTO_GAS, "Touch: accelerate automatically", "In a race the car accelerates by itself; hold BRAKE to slow down or reverse. Touch controls only.", 1, FMT_PLAIN, NULL, 0 },
#endif
    /* The title's own (xbox_SettingsAddTitle): label, help and names come
     * from its table at run time (meta, below). */
    { (xs_id)(XS_TITLE_0 + 0), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 1), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 2), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 3), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 4), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 5), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 6), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
    { (xs_id)(XS_TITLE_0 + 7), NULL, NULL, 1, FMT_PLAIN, NULL, 0 },
};
static_assert(XS_TITLE_MAX == 8, "one k_meta entry per title slot");
/* One entry per setting, in xbox_settings.h's order: a row shows its own
 * label, help and choices only while k_meta[id] is id's (BO-245: a merge
 * put Custom music at start after the texture rows, and the Graphics and
 * Audio pages showed each other's rows). A missing entry is zeros, id 0. */
static constexpr bool meta_in_order()
{
    for (int i = 0; i < XS_COUNT; i++)
        if (k_meta[i].id != i)
            return false;
    return true;
}
static_assert(meta_in_order(), "k_meta: one entry per xs_id, in xbox_settings.h order");

/* A setting's row text: k_meta's, a title setting's from its own table, and
 * the menu entry's help as the title words it. */
static const Meta &meta(int id)
{
    static Meta title[XS_TITLE_MAX], entry;
    if (id >= XS_TITLE_0 && id <= XS_TITLE_LAST) {
        const xbox_title_setting *t = xbox_SettingsTitleOf((xs_id)id);
        Meta &m = title[id - XS_TITLE_0];
        m = k_meta[id];
        if (t) {
            m.label = t->label;
            m.hint = t->hint;
            m.names = t->values ? t->names : NULL;
        }
        return m;
    }
    if (id == XS_INTERFACE_MENU_ENTRY && xbox_SettingsMenuEntryHint()) {
        entry = k_meta[id];
        entry.hint = xbox_SettingsMenuEntryHint();
        return entry;
    }
    return k_meta[id];
}

/* R_SPEAKERS: the speaker test, six buttons in a row (action: 0 through
 * the game's mixbins and speaker map, 1 straight to the output). */
#if defined(__ANDROID__)
extern "C" const char *nvk_core_describe(void);   /* nv2a_vk_core.c */
#endif
enum RowKind { R_SETTING, R_ACTION, R_INFO, R_SPEAKERS, R_FOLDER };
enum Action { A_NONE,                                               /* 0 is "no question asked" (s_confirm) */
              A_RESET_PAGE, A_RESET_ALL, A_REPORT, A_REPORT_SHOT,
              A_HOST0, A_HOST1, A_HOST2, A_HOST3,                  /* xbox_OverlaySetHostRows */
              A_SHADERS,                                          /* the Graphics page's shader build */
              A_MIXER,                                            /* the Mixer page (overlay_mixer.inc) */
              A_UPDATE_CHECK, A_UPDATE_OPEN,                      /* Check for updates (xbox_update.h) */
              A_STEAM_ART,                                        /* xbox_OverlaySetSteamArt */
              A_SAVE_EXPORT, A_SAVE_IMPORT, A_SAVE_BROWSE, A_SAVE_XBOX, A_SAVE_RESTORE, A_SAVE_CANCEL,  /* xbox_OverlaySetSaveIO */
              A_CHEAT0, A_CHEAT1, A_CHEAT2, A_CHEAT3,              /* xbox_OverlaySetCheats */
              A_CHEAT_KEEP, A_CHEAT_DISCARD,
              A_HDR_RESET,                                        /* BO-110: HDR levels to the display's */
              A_RESTART_PIN, A_QUIT };       /* the two rows at the foot of every page: they ask first */

/* The most rows a page has (the Audio page, with the music folder's lines
 * above its settings; the Advanced page with the save and restart rows). */
#define OVERLAY_ROWS 56

/* ── restart (recomp_restart.h) ────────────────────────────────────── */

/* Something outside the settings table wants a restart too (a port's own:
 * a music folder that waits for the next start, a staged save import). */
static char s_restart_why[200];

void xbox_OverlayWantRestart(const char *reason)
{
    snprintf(s_restart_why, sizeof s_restart_why, "%s", reason ? reason : "");
}

/* And the overlay's own: a save import that waits for the start. */
static char s_restart_save[200];
/* And cheats' progress thrown away: the title still holds it in memory, so
 * its saves go to the side copy until a restart (cheat_action). Its own
 * slot: the port's reason (a music folder) must not overwrite it. */
static char s_restart_cheat[200];

static bool restart_wanted()
{
    return xbox_SettingsNeedRestart() || s_restart_why[0] || s_restart_save[0] || s_restart_cheat[0];
}

/* How many changes wait for a restart: the settings changed that are read at
 * start, the port's own reason, a staged save import. */
static int restart_count()
{
    int n = 0;
    for (int i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = xbox_SettingGet((xs_id)i);
        if ((s->flags & XS_RESTART) && s->value != s->launch)
            n++;
    }
    return n + (s_restart_why[0] ? 1 : 0) + (s_restart_save[0] ? 1 : 0) + (s_restart_cheat[0] ? 1 : 0);
}

/* Why: the settings changed that are read at start, by their table flag,
 * then the port's own reason. */
static void restart_reason(char *out, size_t n);
static void restart_now();

static int s_host_n;
static const char *(*s_host_fn)(int row, int activate);

void xbox_OverlaySetHostRows(int n, const char *(*fn)(int row, int activate))
{
    s_host_fn = fn;
    s_host_n = fn ? (n > XBOX_OVERLAY_HOST_ROWS ? XBOX_OVERLAY_HOST_ROWS : n) : 0;
}

/* ── cheats (xbox_OverlaySetCheats) ────────────────────────────────── */

/* A cheat session: from the first cheat switched on, the title's saves go
 * to a copy of its UDATA and TDATA (recomp_save_sandbox_begin,
 * xbox_PathSetUserRoot), so nothing it saves meanwhile reaches the player's
 * save. When the last cheat goes off the player decides: keep (the copy goes
 * over the save, saving is back to normal) or throw away (the save stays as
 * it was; the title still holds that progress in memory, so its saves keep
 * going to the copy until the game is started again). Quitting with a
 * session open throws it away; the copy left behind is removed at the next
 * start. */
extern "C" const char *xbox_PathUserRoot(void);           /* kernel/kernel.h */
extern "C" void xbox_PathSetUserRoot(const char *utf8);
extern "C" int xbox_SaveGuardProblem(char *out, unsigned n);  /* kernel/kernel_saveguard.h */
enum { CS_NONE, CS_ON, CS_ASK, CS_DISCARDED };
/* Restart game, Quit game and switching a cheat on ask once more: the
 * action armed, or 0 (activate, below). */
static int s_confirm;
static const xbox_cheat *s_cheats;
static int s_cheat_n, s_cs;
static char s_cs_root[1024], s_cs_dir[1100], s_cs_info[200];
static char s_cheat_label[4][120];

void xbox_OverlaySetCheats(const xbox_cheat *list, int n)
{
    s_cheats = list;
    s_cheat_n = list ? (n > 4 ? 4 : n) : 0;
}

/* The save root, once the kernel's paths are set up (the first time the
 * Advanced page is built): a copy an earlier run left is removed then. */
static bool cheat_ready()
{
    if (!s_cheat_n)
        return false;
    if (!s_cs_root[0]) {
        const char *root = xbox_PathUserRoot();
        if (!root || !root[0])
            return false;
        snprintf(s_cs_root, sizeof s_cs_root, "%s", root);
        snprintf(s_cs_dir, sizeof s_cs_dir, "%s/cheat-session", root);
        recomp_save_sandbox_discard(s_cs_dir);   /* an earlier run's, never kept */
    }
    return true;
}

static bool cheat_any_on()
{
    for (int i = 0; i < s_cheat_n; i++)
        if (s_cheats[i].get())
            return true;
    return false;
}

static bool s_sio_on;
static char s_sio_id[16];

static void cheat_action(int action)
{
    if (action == A_CHEAT_KEEP && s_cs == CS_ASK) {
        /* The save as it is, kept in the save history first (restorable
         * from "Restore an earlier save"); then each file of the copy goes
         * over it whole or not at all (recomp_save_sandbox_commit). No
         * history copy, no keep: the save is left as it is. */
        if (s_sio_on && s_sio_id[0]) {
            char made[1024];
            if (!recomp_save_history_take(s_cs_root, s_sio_id, made, sizeof made)
                && recomp_save_has_data(s_cs_root, s_sio_id)) {
                snprintf(s_cs_info, sizeof s_cs_info,
                         "Not kept: your save couldn't be backed up first, so nothing changed. Try again,"
                         " or throw the progress away.");
                fprintf(stderr, LOGP "cheats: not kept, the save could not be kept in the history first\n");
                return;
            }
            if (made[0])
                fprintf(stderr, LOGP "cheats: the save before keeping is in %s\n", made);
        }
        int ok = recomp_save_sandbox_commit(s_cs_dir, s_cs_root);
        xbox_PathSetUserRoot(NULL);
        if (ok)
            recomp_save_sandbox_discard(s_cs_dir);
        s_cs = CS_NONE;
        snprintf(s_cs_info, sizeof s_cs_info, ok
                 ? "Kept: your save now has what the game saved meanwhile."
                 : "Not everything could be copied into your save. The copy is in %.80s", s_cs_dir);
        fprintf(stderr, LOGP "cheats: progress kept (%s)\n", ok ? "copied" : "copy failed");
        return;
    }
    if (action == A_CHEAT_DISCARD && s_cs == CS_ASK) {
        /* The title still holds the cheats' progress in memory, so its saves
         * keep going to the side copy, and nothing played from now on
         * reaches the save until the game starts again: said in yellow on
         * the page and at the foot, and the restart offered. */
        s_cs = CS_DISCARDED;
        s_cs_info[0] = 0;
        snprintf(s_restart_cheat, sizeof s_restart_cheat,
                 "Progress is not being saved until you restart. The cheat progress was thrown away.");
        fprintf(stderr, LOGP "cheats: progress thrown away; saves go to the copy until restart\n");
        return;
    }
    int i = action - A_CHEAT0;
    if (i < 0 || i >= s_cheat_n || !cheat_ready())
        return;
    if (s_cheats[i].get()) {
        s_cheats[i].set(0);
        fprintf(stderr, LOGP "cheats: %s off\n", s_cheats[i].name);
        if (s_cs == CS_ON && !cheat_any_on())
            s_cs = CS_ASK;
        return;
    }
    if (s_cs == CS_NONE) {
        if (!recomp_save_sandbox_begin(s_cs_root, s_cs_dir)) {
            recomp_save_sandbox_discard(s_cs_dir);
            snprintf(s_cs_info, sizeof s_cs_info, "Not switched on: your save couldn't be copied first.");
            return;
        }
        xbox_PathSetUserRoot(s_cs_dir);
        fprintf(stderr, LOGP "cheats: the game's saves go to %s\n", s_cs_dir);
    }
    if (s_cs != CS_DISCARDED)
        s_cs = CS_ON;
    s_cs_info[0] = 0;
    s_cheats[i].set(1);
    fprintf(stderr, LOGP "cheats: %s on\n", s_cheats[i].name);
}

/* ── the bug report (xbox_report.h) ────────────────────────────────── */

static char s_rep_info[160] = "Bug reports are saved as a zip on this device.";
static char s_rep_done[1400];         /* where the last one went, or why none */

/* ── the port's name and version ───────────────────────────────────── */

static char s_about[200];

/* ── Check for updates (xbox_update.h) ─────────────────────────────── */

/* Nothing here runs until the row is pressed: then one thread makes the one
 * request and writes the answer, the overlay shows it from the next frame. */
static char s_up_repo[130], s_up_ver[48];
static std::atomic<int> s_up_state;      /* 0 nothing asked, 1 asking, 2 answered */
static xbox_update_result s_up_res;      /* written by the thread before s_up_state is 2 */
static char s_up_open_err[160];          /* the download page would not open */
static bool s_up_checkable;              /* a repository was registered and this build can ask */

/* ── the music folder (xbox_OverlaySetMusicFolder) ─────────────────── */

#define MUSIC_KEY "audio.custom_music_dir"
static const xbox_music_folder_ops *s_mf;
static char s_mf_status[200];
#define MUSIC_NOTES 4
static char s_mf_note[MUSIC_NOTES][200];
static int s_fb_sel;                  /* the browser's row */
static bool s_fb_scroll;

void xbox_OverlaySetMusicFolder(const xbox_music_folder_ops *ops)
{
    s_mf = ops;
}

/* The row's access hook (xbox_music_folder_ops.access), on its own thread:
 * Android's permission dialog must not stop the overlay's frames. */
static std::atomic<int> s_mf_acc_state;  /* 0 idle, 1 asking, 2 answered */
static int s_mf_acc_ok;                   /* written by the thread before state 2 */
static char s_mf_acc_line[400];           /* the hook's line, shown in the hint */
static char s_mf_acc_tmp[400];

static int music_access_thread(void *)
{
    s_mf_acc_tmp[0] = 0;
    s_mf_acc_ok = s_mf->access(s_mf_acc_tmp, sizeof s_mf_acc_tmp);
    s_mf_acc_state.store(2, std::memory_order_release);
    return 0;
}

static void music_browser_open();
static void xbox_browser_open();
static int s_mf_acc_for;                  /* 0: the music folder, 1: the Xbox save */

/* The answer, once a frame: the browser opens when the hook said so. */
static void music_access_poll()
{
    if (s_mf_acc_state.load(std::memory_order_acquire) != 2)
        return;
    snprintf(s_mf_acc_line, sizeof s_mf_acc_line, "%s", s_mf_acc_tmp);
    s_mf_acc_state.store(0, std::memory_order_release);
    fprintf(stderr, LOGP "music folder access: %s%s%s\n", s_mf_acc_ok ? "yes" : "no",
            s_mf_acc_line[0] ? ": " : "", s_mf_acc_line);
    if (s_mf_acc_ok && s_open) {
        if (s_mf_acc_for == 1)
            xbox_browser_open();
        else
            music_browser_open();
    }
}

/* The row is pressed: through the access hook when there is one. */
static void music_access_start(int for_xbox = 0)
{
    int expect = 0;
    if (!s_mf || !s_mf->access) {
        if (for_xbox)
            xbox_browser_open();
        else
            music_browser_open();
        return;
    }
    s_mf_acc_for = for_xbox;
    if (!s_mf_acc_state.compare_exchange_strong(expect, 1))
        return;                           /* still waiting for the answer */
    SDL_Thread *t = SDL_CreateThread(music_access_thread, "music access", NULL);
    if (!t) {
        s_mf_acc_state.store(0, std::memory_order_release);
        if (for_xbox)
            xbox_browser_open();
        else
            music_browser_open();
        return;
    }
    SDL_DetachThread(t);
}

static const char *path_leaf(const char *p)
{
    const char *e = p + strlen(p), *b;
    while (e > p && (e[-1] == '/' || e[-1] == '\\'))
        e--;
    b = e;
    while (b > p && b[-1] != '/' && b[-1] != '\\')
        b--;
    return b;
}

/* "Off", or the folder's own name. */
static void music_value(char *out, size_t n)
{
    const char *dir = xbox_SettingsText(MUSIC_KEY, NULL);
    if (!dir[0]) {
        snprintf(out, n, "Off");
        return;
    }
    const char *b = path_leaf(dir);
    int len = (int)strcspn(b, "/\\");
    snprintf(out, n, "%.*s", len > 40 ? 40 : len, b);
}

static uint32_t now_ms();

/* ── save data (xbox_OverlaySetSaveIO, recomp_save_dir.h) ─────────── */

static char s_sio_dir[1024], s_sio_dest[1024], s_sio_note[200];
static char s_sio_info[200];          /* the line under the rows */
static char s_sio_msg[2600];          /* the whole of it, for the rows' note */
enum { FB_MUSIC, FB_SAVE, FB_XBOX };
static int s_fb_purpose;              /* what the folder browser is open for */

void xbox_OverlaySetSaveIO(const xbox_save_io *io)
{
    s_sio_on = io && io->save_dir && io->title_id && io->export_dir;
    if (!s_sio_on)
        return;
    snprintf(s_sio_dir, sizeof s_sio_dir, "%s", io->save_dir);
    snprintf(s_sio_id, sizeof s_sio_id, "%s", io->title_id);
    snprintf(s_sio_dest, sizeof s_sio_dest, "%s", io->export_dir);
    snprintf(s_sio_note, sizeof s_sio_note, "%s", io->note ? io->note : "");
    /* What this start did with an import asked for before it. */
    if (recomp_save_import_result(s_sio_dir, s_sio_msg, sizeof s_sio_msg))
        snprintf(s_sio_info, sizeof s_sio_info, "%s",
                 strncmp(s_sio_msg, "Imported", 8) ? "The save was not imported (see the note below)."
                                                   : "The save was imported at this start (see the note below).");
}

static int sio_mark(const char *dir)
{
    return recomp_save_export_check(dir, s_sio_id, NULL, 0);
}

static int sio_stopping(void *) { return xsb_stopping(); }
static int sio_slow(const char *dir, void *) { return xsb_slow_volume(dir); }

/* On the browser's thread, bounded (BO-289): the look gives up when the
 * browser closes or moves on, and a network share is not walked. */
static int sio_mark_xbox(const char *dir)
{
    xbox_save_check_limits lim = {};
    lim.stop = sio_stopping;
    lim.shallow = sio_slow;
    return xbox_SaveImportCheckBounded(dir, s_sio_id, &lim);
}

static bool sio_staged(char *folder, unsigned n)
{
    return s_sio_on && recomp_save_import_staged(s_sio_dir, folder, n);
}

/* Ask for `folder` at the next start, and say what will happen. */
static void sio_stage(const char *folder)
{
    char err[1400] = "";
    if (recomp_save_import_stage(s_sio_dir, folder, s_sio_id, err, sizeof err)) {
        snprintf(s_sio_msg, sizeof s_sio_msg, "The save in %s will be imported at the next start,"
                 " before the game runs. Your current save is copied to %s/save-before-import"
                 " first. Until then the game keeps using the current save, and anything it saves"
                 " ends up in that copy.", folder, s_sio_dir);
        snprintf(s_sio_info, sizeof s_sio_info, "Import waiting for the next start: %.120s", path_leaf(folder));
        snprintf(s_restart_save, sizeof s_restart_save, "A save import is waiting for the next start.");
        fprintf(stderr, LOGP "save import staged: %s\n", folder);
    } else {
        snprintf(s_sio_msg, sizeof s_sio_msg, "%s", err);
        snprintf(s_sio_info, sizeof s_sio_info, "Not imported (reason in the note below).");
        fprintf(stderr, LOGP "save import refused: %s\n", err);
    }
}

/* An original Xbox save (folder) is made into an export-shaped folder, with
 * its profile signed again, and staged like any other import. */
static void sio_stage_xbox(const char *folder)
{
    char out[1024] = "", info[400] = "", err[1400] = "";
    if (!xbox_SaveImportPrepare(folder, s_sio_id, s_sio_dir, s_sio_dir, out, sizeof out, info, sizeof info,
                                err, sizeof err)) {
        snprintf(s_sio_msg, sizeof s_sio_msg, "%s", err);
        snprintf(s_sio_info, sizeof s_sio_info, "Not imported (reason in the note below).");
        fprintf(stderr, LOGP "xbox save import refused: %s\n", err);
        return;
    }
    sio_stage(out);
    if (recomp_save_import_staged(s_sio_dir, out, sizeof out)) {
        char more[2600];
        snprintf(more, sizeof more, "%s  %s", info, s_sio_msg);
        snprintf(s_sio_msg, sizeof s_sio_msg, "%s", more);
        snprintf(s_sio_info, sizeof s_sio_info, "Xbox save ready. Restart the game to import it.");
        snprintf(s_restart_save, sizeof s_restart_save, "An Xbox save import is waiting for the next start.");
        fprintf(stderr, LOGP "xbox save import staged: %s\n", info);
    }
}

static void xbox_browser_open()
{
    xsb_options o;
    memset(&o, 0, sizeof o);
    o.use_label = "Import the Xbox save in this folder";
    o.top_label = "Home folder";
    o.mark = sio_mark_xbox;
    s_fb_purpose = FB_XBOX;
    xsb_open_with(&o);
    s_fb_sel = 0;
    s_fb_scroll = true;
}

static void sio_action(int action)
{
    char path[1024] = "", err[1400] = "";

    if (!s_sio_on)
        return;
    switch (action) {
    case A_SAVE_EXPORT:
        if (recomp_save_export(s_sio_dir, s_sio_id, s_sio_dest, s_sio_note, path, sizeof path,
                               err, sizeof err)) {
            snprintf(s_sio_msg, sizeof s_sio_msg, "Exported to %s", path);
            snprintf(s_sio_info, sizeof s_sio_info, "Exported: %.80s (full path in the note below)",
                     path_leaf(path));
            fprintf(stderr, LOGP "save exported to %s\n", path);
        } else {
            snprintf(s_sio_msg, sizeof s_sio_msg, "%s", err);
            snprintf(s_sio_info, sizeof s_sio_info, "Not exported (reason in the note below).");
            fprintf(stderr, LOGP "save export failed: %s\n", err);
        }
        break;
    case A_SAVE_IMPORT:
        if (recomp_save_export_newest(s_sio_dest, s_sio_id, path, sizeof path)) {
            sio_stage(path);
        } else {
            snprintf(s_sio_msg, sizeof s_sio_msg, "No exported save in %s. Export one on the"
                     " other machine and copy its dated folder here, or choose a folder with"
                     " \"Import save from another folder\".", s_sio_dest);
            snprintf(s_sio_info, sizeof s_sio_info, "No exported save found (see the note below).");
        }
        break;
    case A_SAVE_BROWSE: {
        xsb_options o;
        memset(&o, 0, sizeof o);
        o.use_label = "Import the save in this folder";
        o.top_label = "Exported saves";
        o.top_path = s_sio_dest;
        o.mark = sio_mark;
        s_fb_purpose = FB_SAVE;
        xsb_open_with(&o);
        s_fb_sel = 0;
        s_fb_scroll = true;
        break;
    }
    case A_SAVE_XBOX:
        music_access_start(1);          /* Android asks for its file access first */
        break;
    case A_SAVE_RESTORE: {
        /* The save history (recomp_save_dir.h): each an export, put back the
         * same way, newest first. */
        static char hist[1024];
        xsb_options o;
        recomp_save_history_dir(s_sio_dir, hist, sizeof hist);
        memset(&o, 0, sizeof o);
        o.use_label = "Restore the save in this folder";
        o.top_label = "Earlier saves";
        o.top_path = hist;
        o.start_in = hist;
        o.newest_first = 1;
        o.mark = sio_mark;
        s_fb_purpose = FB_SAVE;
        xsb_open_with(&o);
        s_fb_sel = xsb_rows() > 2 ? 2 : 0;        /* on the newest */
        s_fb_scroll = true;
        break;
    }
    case A_SAVE_CANCEL:
        recomp_save_import_cancel(s_sio_dir);
        s_restart_save[0] = 0;
        snprintf(s_sio_msg, sizeof s_sio_msg, "Import cancelled. Your current save stays.");
        snprintf(s_sio_info, sizeof s_sio_info, "Import cancelled.");
        break;
    default:
        break;
    }
}

/* The music folder's browser, on the current folder when there is one
 * ("turn off" is above it). */
static void music_browser_open()
{
    const char *dir = xbox_SettingsText(MUSIC_KEY, NULL);
    s_fb_purpose = FB_MUSIC;
    xsb_open(dir);
    s_fb_sel = dir[0] ? 1 : 0;
    s_fb_scroll = true;
}

static void browser_count(const xsb_row &r, char *out, size_t n)
{
    out[0] = 0;
    if (r.kind == XSB_OFF || r.kind == XSB_UP)
        return;
    if (s_fb_purpose == FB_SAVE || s_fb_purpose == FB_XBOX) {
        snprintf(out, n, "%s", r.songs < 0 ? "..." : r.songs ? (s_fb_purpose == FB_XBOX ? "an Xbox save" : "a save of this game")
                               : r.more ? "not checked" : "");
        return;
    }
    if (r.songs < 0)
        snprintf(out, n, "...");
    else if (r.songs == 0)
        snprintf(out, n, r.more ? "none found yet" : "no music");
    else
        snprintf(out, n, "%d%s song%s", r.songs, r.more ? "+" : "", r.songs == 1 && !r.more ? "" : "s");
}

static void changed();

/* A browser row is picked. */
static void browser_pick(int i)
{
    char dir[1024] = "";
    int sel = 0;
    menusnd_play(XBOX_OVERLAY_SND_SELECT);
    xsb_result res = xsb_activate(i, dir, sizeof dir, &sel);
    if (res == XSB_MOVED) {
        s_fb_sel = sel;
        s_fb_scroll = true;
    } else if (res == XSB_CHOSEN && s_fb_purpose == FB_SAVE) {
        /* A folder for the import: checked, and either way back to the page. */
        sio_stage(dir);
        xsb_close();
    } else if (res == XSB_CHOSEN && s_fb_purpose == FB_XBOX) {
        sio_stage_xbox(dir);
        xsb_close();
    } else if (res == XSB_CHOSEN || res == XSB_TURNED_OFF) {
        if (res == XSB_TURNED_OFF)
            dir[0] = 0;
        xbox_SettingsSetText(MUSIC_KEY, dir);
        changed();
        fprintf(stderr, LOGP "music folder: %s\n", dir[0] ? dir : "off");
        if (s_mf && s_mf->chosen)
            s_mf->chosen(dir);
        xsb_close();
    }
}

void xbox_OverlaySetAbout(const char *text)
{
    snprintf(s_about, sizeof s_about, "%s", text ? text : "");
}

void xbox_OverlaySetUpdateCheck(const char *repo, const char *version)
{
    char url[200];
    s_up_checkable = repo && version && version[0] && xbox_update_api_url(repo, url, sizeof url)
                     && xbox_update_available();
    if (s_up_checkable) {
        snprintf(s_up_repo, sizeof s_up_repo, "%s", repo);
        snprintf(s_up_ver, sizeof s_up_ver, "%s", version);
    }
}

static int update_thread(void *)
{
    char url[200], ua[80], err[100] = "", *body = NULL;
    xbox_update_api_url(s_up_repo, url, sizeof url);
    snprintf(ua, sizeof ua, "Takedawn/%s", s_up_ver);
    fprintf(stderr, LOGP "update check: asking GitHub for the latest release of %s\n", s_up_repo);
    int status = xbox_update_http_get(url, ua, 10000, &body, err, sizeof err);
    xbox_update_evaluate(status, err, body, s_up_ver, &s_up_res);
    free(body);
    fprintf(stderr, LOGP "update check: %s\n", s_up_res.headline);
    s_up_state.store(2, std::memory_order_release);
    return 0;
}

/* Starts the one request; a press while one is out does nothing. */
static void update_check_start()
{
    int expect = 0;
    if (!s_up_checkable)
        return;
    if (!s_up_state.compare_exchange_strong(expect, 1)) {
        expect = 2;
        if (!s_up_state.compare_exchange_strong(expect, 1))
            return;
    }
    s_up_open_err[0] = 0;
    SDL_Thread *t = SDL_CreateThread(update_thread, "update check", NULL);
    if (!t) {
        xbox_update_evaluate(0, "could not start the request", NULL, s_up_ver, &s_up_res);
        s_up_state.store(2, std::memory_order_release);
        return;
    }
    SDL_DetachThread(t);
}

static void update_open_page()
{
    if (s_up_state.load(std::memory_order_acquire) != 2 || s_up_res.kind != XU_NEWER || !s_up_res.url[0])
        return;
    fprintf(stderr, LOGP "update check: opening %s\n", s_up_res.url);
    if (SDL_OpenURL(s_up_res.url) != 0)
        snprintf(s_up_open_err, sizeof s_up_open_err, "Could not open the page: %.120s", SDL_GetError());
}

/* ── Add Steam library art (xbox_OverlaySetSteamArt) ───────────────── */

/* The host's program runs on one thread when the row is pressed (it starts a
 * process and waits for it); the overlay shows its line from the next frame. */
static int (*s_sa_run)(char *line, unsigned n);
static std::atomic<int> s_sa_state;      /* 0 nothing asked, 1 running, 2 answered */
static char s_sa_line[240];              /* written by the thread before s_sa_state is 2 */

void xbox_OverlaySetSteamArt(int (*run)(char *line, unsigned n))
{
    s_sa_run = run;
}

static int steam_art_thread(void *)
{
    s_sa_line[0] = 0;
    s_sa_run(s_sa_line, sizeof s_sa_line);
    fprintf(stderr, LOGP "steam art: %s\n", s_sa_line);
    s_sa_state.store(2, std::memory_order_release);
    return 0;
}

static void steam_art_start()
{
    int expect = 0;
    if (!s_sa_run)
        return;
    if (!s_sa_state.compare_exchange_strong(expect, 1)) {
        expect = 2;
        if (!s_sa_state.compare_exchange_strong(expect, 1))
            return;
    }
    SDL_Thread *t = SDL_CreateThread(steam_art_thread, "steam art", NULL);
    if (!t) {
        snprintf(s_sa_line, sizeof s_sa_line, "Could not start the program.");
        s_sa_state.store(2, std::memory_order_release);
        return;
    }
    SDL_DetachThread(t);
}

/* ── the shader pre-build (xbox_shader_build.h) ────────────────────── */

static const xbox_shader_build_ops *s_sb;
static char s_sb_label[200], s_sb_info[200];

void xbox_OverlaySetShaderBuild(const xbox_shader_build_ops *ops)
{
    s_sb = ops;
}

static int sb_state(float *fraction, char *text, unsigned n)
{
    if (text && n)
        text[0] = 0;
    return s_sb && s_sb->poll ? s_sb->poll(fraction, text, n) : XBOX_SHADERS_IDLE;
}

static int page_rows(int page, struct Row *r);

/* The action row's label and the line above it, as the build stands. */
static void sb_texts()
{
    float fr = 0;
    char text[160];
    int st = sb_state(&fr, text, sizeof text);
    switch (st) {
    case XBOX_SHADERS_RUNNING:
        snprintf(s_sb_label, sizeof s_sb_label, "Stop building shaders");
        snprintf(s_sb_info, sizeof s_sb_info, "Building shaders: %d%%  (%s)",
                 (int)(fr * 100.0f + 0.5f), text);
        break;
    case XBOX_SHADERS_DONE:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders again");
        snprintf(s_sb_info, sizeof s_sb_info, "Shaders: built.");
        break;
    case XBOX_SHADERS_FAILED:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders now");
        snprintf(s_sb_info, sizeof s_sb_info, "Shaders: not finished (%.120s).", text);
        break;
    default:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders now");
        snprintf(s_sb_info, sizeof s_sb_info, "Shaders: not built yet. Each compiles the first time it is drawn.");
        break;
    }
}

struct Row {
    RowKind kind;
    xs_id id;                         /* R_SETTING */
    int action;                       /* R_ACTION */
    const char *text;                 /* R_ACTION label, R_INFO line */
};

/* The Advanced page's three lines on what reaches the glass (below, with
 * the frame-time graph). */
static char s_glass_info[3][160];
static void glass_info();

/* Where the log is, and what decides when the row does not. */
extern "C" const char *xbox_crash_log_dir(void);      /* kernel/xbox_crash.h */
static char s_diag_info[256];
static void diag_info()
{
    const char *dir = xbox_crash_log_dir();
    const char *home = getenv("HOME");
    char shown[200];
    if (dir && *dir && home && *home && !strncmp(dir, home, strlen(home)))
        snprintf(shown, sizeof shown, "~%s", dir + strlen(home));
    else
        snprintf(shown, sizeof shown, "%s", dir && *dir ? dir : "the logs folder beside the saves");
    if (xr_log_forced())
        snprintf(s_diag_info, sizeof s_diag_info, "Log: %s (set by %s for this run)", shown,
                 xr_log_source());
    else
        snprintf(s_diag_info, sizeof s_diag_info, "Log: %s", shown);
}

static int page_rows(int page, Row *r)
{
    int n = 0;
    bool staged = false;              /* a save import waits for the next start (Advanced) */
#define SET(i)  do { if (!xbox_SettingsHidden(i)) r[n++] = Row{ R_SETTING, (i), 0, NULL }; } while (0)
    /* The title's own settings for a page (xbox_SettingsAddTitle). */
#define TITLE_ROWS(xp) \
    for (int t_ = XS_TITLE_0; t_ <= XS_TITLE_LAST; t_++) { \
        const xbox_title_setting *ts_ = xbox_SettingsTitleOf((xs_id)t_); \
        if (ts_ && ts_->page == (xp)) \
            SET((xs_id)t_); \
    }
#define ACT(a, t) r[n++] = Row{ R_ACTION, XS_COUNT, (a), (t) }
#define INFO(t) r[n++] = Row{ R_INFO, XS_COUNT, 0, (t) }
#define WARN(t) r[n++] = Row{ R_INFO, XS_COUNT, 1, (t) }   /* in the attention yellow */
#define SPK(a, t) r[n++] = Row{ R_SPEAKERS, XS_COUNT, (a), (t) }
    switch (page) {
    case P_DISPLAY:
#if !defined(__ANDROID__)
        /* Android: the game is the whole screen always (immersive); out of
         * fullscreen SDL brings the system bars back over a smaller window,
         * and the touch controls with it. */
        SET(XS_DISPLAY_FULLSCREEN);
#endif
        SET(XS_DISPLAY_ASPECT); SET(XS_DISPLAY_FILTER);
        SET(XS_DISPLAY_OVERSCAN); SET(XS_DISPLAY_FIT);
#if defined(__APPLE__)
        if (!s_vulkan)                 /* the Metal presenter's (BO-115) */
            SET(XS_DISPLAY_PRESENT_TIMING);
#endif
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
        if (s_vulkan)                  /* the Vulkan presenter's queue (BO-110, BO-207) */
            SET(XS_DISPLAY_FRAME_PACING);
#endif
        if (recomp_patch_enabled(xbox_SettingGet(XS_GAME_SPEED_UNITS)->key))
            SET(XS_GAME_SPEED_UNITS);
        {
            unsigned langs = xbox_SettingsLanguages();
            if (langs & (langs - 1))    /* more than one on the title's disc */
                SET(XS_GAME_LANGUAGE);
        }
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_HUD_FIX)->key))
            SET(XS_DISPLAY_HUD_FIX);
        /* The map screen's shape: only with the widescreen HUD on. */
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_CRASHNAV_MAP)->key) &&
            xbox_SettingValue(XS_DISPLAY_HUD_FIX) != 0)
            SET(XS_DISPLAY_CRASHNAV_MAP);
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_ULTRAWIDE)->key))
            SET(XS_DISPLAY_ULTRAWIDE);
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_SCORE_BOARD)->key))
            SET(XS_DISPLAY_SCORE_BOARD);
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_RIVAL_MARKER)->key))
            SET(XS_DISPLAY_RIVAL_MARKER);
        TITLE_ROWS(XS_PAGE_DISPLAY);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_GRAPHICS:
        SET(XS_GRAPHICS_RENDER_SCALE); SET(XS_GRAPHICS_ANISO);
        SET(XS_GRAPHICS_AA); SET(XS_GRAPHICS_SHARPEN);
        {   /* The Vulkan path's two renderers; the other backends have one. */
            const char *be = recomp_env(RENV_PB_BACKEND);
            if (be && (!strcasecmp(be, "vulkan") || !strcasecmp(be, "vk") || !strcasecmp(be, "native")))
                SET(XS_GRAPHICS_RENDERER);
        }
        SET(XS_GRAPHICS_LOD_BIAS);
        if (!nv2a_texscale_held())      /* RECOMP_DEBUG=ui_scale_hold=1 (nv2a_texscale.h) */
            SET(XS_GRAPHICS_UI_SCALE);
        if (nv2a_texup_supported()) {   /* BO-245: a backend that can (Metal for now) */
            SET(XS_GRAPHICS_TEXUP);
            if (xbox_SettingValue(XS_GRAPHICS_TEXUP) != 0) {
                /* A Steam Deck is offered 2x only: no scale row there. */
                if (nv2a_texup_max_scale() > 2)
                    SET(XS_GRAPHICS_TEXUP_SCALE);
                SET(XS_GRAPHICS_TEXUP_ROADS);
                SET(XS_GRAPHICS_TEXUP_PRELOAD);
            }
        }
        /* Dither (graphics.dither) has no row: settings.toml or
         * RECOMP_GFX_DITHER only (docs/runtime/overlay.md). */
        /* BO-110: HDR, where the display can show more than SDR white. */
        if (nv2a_host_hdr_potential() > 1.0f) {
            SET(XS_GRAPHICS_HDR);
            if (xbox_SettingValue(XS_GRAPHICS_HDR) != 0.0) {
                SET(XS_GRAPHICS_HDR_PEAK); SET(XS_GRAPHICS_HDR_PAPER); SET(XS_GRAPHICS_HDR_HUD);
                /* BO-227: Metal's HDR pass takes these too now. */
                SET(XS_GRAPHICS_HDR_SHADOWS); SET(XS_GRAPHICS_HDR_HIGHLIGHTS);
                ACT(A_HDR_RESET, "Reset HDR to the display's defaults");
            }
        }
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_BLOOM)->key))
            SET(XS_GRAPHICS_BLOOM);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->key))
            SET(XS_GRAPHICS_MOTION_BLUR);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->key))
            SET(XS_GRAPHICS_CRASH_60FPS);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_SPLIT_60FPS)->key))
            SET(XS_GRAPHICS_SPLIT_60FPS);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_HIGH_REFRESH)->key))
            SET(XS_GRAPHICS_HIGH_REFRESH);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_SOFTBODY)->key))
            SET(XS_GRAPHICS_SOFTBODY);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GAME_RACE_CLOCK)->key))
            SET(XS_GAME_RACE_CLOCK);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_VEHICLE_DETAIL)->key))
            SET(XS_GRAPHICS_VEHICLE_DETAIL);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_SCENERY_DETAIL)->key))
            SET(XS_GRAPHICS_SCENERY_DETAIL);
#if defined(__ANDROID__)
        {
            /* Which driver draws: chosen before the game starts, in the app's
             * second launcher entry (the title's process has loaded it). */
            static char drv[420];
            const char *d = nvk_core_describe();
            if (d && *d) {
                snprintf(drv, sizeof drv, "Graphics driver in use: %s", d);
                INFO(drv);
            }
        }
#endif
        TITLE_ROWS(XS_PAGE_GRAPHICS);
        for (int i = 0; i < s_host_n; i++) {
            const char *t = s_host_fn(i, 0);
            if (t && *t)
                ACT(A_HOST0 + i, t);
        }
        if (s_sb) {
            sb_texts();
            INFO(s_sb_info);
            ACT(A_SHADERS, s_sb_label);
        }
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_AUDIO:
        /* The player's own music first: it is what this page is opened for
         * most (BO-108). Its status line and notes follow it. */
        if (s_mf) {
            r[n++] = Row{ R_FOLDER, XS_COUNT, 0, "Custom music folder" };
            s_mf_status[0] = 0;
            if (s_mf->status)
                s_mf->status(s_mf_status, sizeof s_mf_status);
            if (s_mf_status[0])
                INFO(s_mf_status);
            /* More lines of the port's (what a scan left out, by name), as
             * far as the page has room. */
            for (int k = 0; k < MUSIC_NOTES && s_mf->note && n < OVERLAY_ROWS - 12; k++) {
                s_mf_note[k][0] = 0;
                if (!s_mf->note(k, s_mf_note[k], sizeof s_mf_note[k]) || !s_mf_note[k][0])
                    break;
                INFO(s_mf_note[k]);
            }
        }
        if (s_mf)
            SET(XS_AUDIO_CUSTOM_AT_START);
        if (s_mf)
            SET(XS_AUDIO_CUSTOM_LEVEL);
        SET(XS_AUDIO_VOLUME); SET(XS_AUDIO_MUTE); SET(XS_AUDIO_MUTE_BG); SET(XS_AUDIO_SPEAKERS);
        if (menusnd_named())
            SET(XS_AUDIO_MENU_SOUNDS);
        /* The dry effect sends' level: only where no model of the title's
         * effects image runs (apu_gp.h); with one they are not dry. */
        if (!apu_gp_wired())
            SET(XS_AUDIO_FX_LEVEL);
        /* audio.lfe_lowpass has no row: the stock 120 Hz is the console's,
         * and the Mixer page has its own LFE low-pass for tuning. */
        if (apu_bus_count() > 0)
            ACT(A_MIXER, "Mixer: buses, sends, LFE, limiter, meters");
        if (recomp_patch_enabled(xbox_SettingGet(XS_AUDIO_SPLIT_MUSIC)->key))
            SET(XS_AUDIO_SPLIT_MUSIC);
        TITLE_ROWS(XS_PAGE_AUDIO);
        SPK(0, "Speaker test, game mix");
        SPK(1, "Speaker test, direct");
        if (mcpx_apu_title_channels() != 6)
            INFO("The output is stereo: each speaker is folded to left/right.");
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_INPUT:
        SET(XS_INPUT_DEADZONE); SET(XS_INPUT_TRIGGER_DEADZONE); SET(XS_INPUT_RUMBLE);
        SET(XS_INPUT_FACE_LAYOUT);
        if (xbox_InputControlSchemeOffered())
            SET(XS_INPUT_CONTROL_SCHEME);
#if !defined(__ANDROID__)
        SET(XS_INPUT_FIRST_PAD);
#endif
        SET(XS_INPUT_GLYPHS);
#if defined(__ANDROID__)
        SET(XS_INPUT_VIBRATION);
        SET(XS_INPUT_TOUCH);
        if ((int)xbox_SettingValue(XS_INPUT_TOUCH) != XBOX_TOUCH_OFF) {
            SET(XS_INPUT_TOUCH_OPACITY); SET(XS_INPUT_TOUCH_SCALE); SET(XS_INPUT_TOUCH_AUTO_GAS);
        }
#endif
        TITLE_ROWS(XS_PAGE_INPUT);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_POWER:
#if !defined(__linux__)
        {
            /* The note names the default this build has: the title's own
             * choice (xbox_SettingsGameDefault), Balanced where it takes the
             * Deck's pacing (Windows), the busy-wait where it sets none
             * (macOS). */
            const xs_setting *pm = xbox_SettingGet(XS_POWER_MODE);
            if (xbox_PowerStockHidden(xbox_SettingsHost())) {
                INFO("Tuned on the Steam Deck (Linux). Balanced is the default here too.");
            } else if (pm && (int)pm->def == 1) {
                INFO("Tuned on the Steam Deck (Linux). Balanced is the default here too.");
                INFO("Maximum is the game's own busy-wait.");
            } else if (pm && (int)pm->def == 2) {
                INFO("Tuned on the Steam Deck (Linux). Battery saver is the default here.");
                INFO("Maximum is the game's own busy-wait.");
            } else {
                INFO("Tuned on the Steam Deck (Linux). The default here is Maximum,");
                INFO("the game's own busy-wait; the other two sleep between frames.");
            }
        }
#endif
        SET(XS_POWER_MODE);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    default:
        if (s_about[0])
            INFO(s_about);
        if (s_up_checkable) {
            int st = s_up_state.load(std::memory_order_acquire);
            ACT(A_UPDATE_CHECK, st == 1 ? "Checking for updates..." : "Check for updates");
            if (st == 2) {
                INFO(s_up_res.headline);
                /* The way to the download first: the notes can be long. */
                if (s_up_res.kind == XU_NEWER && s_up_res.url[0])
                    ACT(A_UPDATE_OPEN, "Open the download page");
                if (s_up_open_err[0])
                    INFO(s_up_open_err);
                for (int k = 0; k < s_up_res.note_count; k++)
                    INFO(s_up_res.notes[k]);
            }
        }
        if (s_sa_run) {
            int st = s_sa_state.load(std::memory_order_acquire);
            ACT(A_STEAM_ART, st == 1 ? "Adding Steam library art..." : "Add Steam library art");
            if (st == 2)
                INFO(s_sa_line);
        }
        SET(XS_ADVANCED_PERF);
        {
            /* What Auto holds now (BO-175): one line, only while it is on. */
            static char au[160];
            double lead;
            char why[96];
            if (xbox_PacingAutoState(&lead, why, sizeof why)) {
                if (lead <= 0.0)
                    snprintf(au, sizeof au, "Auto now: sleeping through each wait (as Battery saver). Last change: %s.", why);
                else if (lead >= 16.0)
                    snprintf(au, sizeof au, "Auto now: spinning through each wait (busy-wait). Last change: %s.", why);
                else
                    snprintf(au, sizeof au, "Auto now: spinning the last %.0f ms of each frame. Last change: %s.", lead, why);
                INFO(au);
            }
        }
#if defined(__APPLE__)
        /* The Metal presenter's (BO-115); the other presenters do not
         * record when frames reach the glass, and their pages are as they
         * were. */
        if (!s_vulkan) {
            SET(XS_ADVANCED_PERF_STEPS);
            /* What reaches the glass, in words (the graph has one line of it). */
            glass_info();
            INFO(s_glass_info[0]);
            INFO(s_glass_info[1]);
            if (s_glass_info[2][0])    /* empty while there are no on-glass times */
                INFO(s_glass_info[2]);
        }
#endif
        /* The console key (kernel_console_keys.h): read at the start. */
        {
            static char ck[200];
            bool set = xbox_SettingsText("console.hdd_key", NULL)[0] || xbox_SettingsText("console.eeprom", NULL)[0];
            snprintf(ck, sizeof ck, set ? "Console key: set; read at the next start. Saves signed by an original Xbox load,"
                                          " the port's own earlier saves show as Unusable while it is."
                                        : "Console key: not set. For saves from an original Xbox or xemu, set console.eeprom"
                                          " (path of your eeprom.bin) or console.hdd_key in settings.toml.");
            INFO(ck);
        }
        TITLE_ROWS(XS_PAGE_ADVANCED);
        SET(XS_INTERFACE_MENU_ENTRY);
        /* The print gate (BO-125), next to the report it feeds. */
        SET(XS_ADVANCED_DIAG_LOG);
        diag_info();
        INFO(s_diag_info);
        if (xbox_BugReportHasMaker()) {
            INFO(s_rep_info);
            ACT(A_REPORT, "Create bug report");
            ACT(A_REPORT_SHOT, "Create bug report with a screenshot");
        }
        /* Save data: a copy out of the profile, and one back in. */
        if (s_sio_on) {
            static char imp[160];
            char found[1024];
            staged = sio_staged(found, sizeof found);
            ACT(A_SAVE_EXPORT, "Export save to home folder");
            if (staged) {
                ACT(A_SAVE_CANCEL, "Cancel the import");
            } else {
                if (recomp_save_export_newest(s_sio_dest, s_sio_id, found, sizeof found))
                    snprintf(imp, sizeof imp, "Import save from home folder (newest: %.60s)", path_leaf(found));
                else
                    snprintf(imp, sizeof imp, "Import save from home folder (none found there)");
                ACT(A_SAVE_IMPORT, imp);
                ACT(A_SAVE_BROWSE, "Import save from another folder...");
                ACT(A_SAVE_XBOX, "Import Xbox save...");
                /* Counted now and then, not at every frame. */
                static int kept = -1;
                static uint32_t kept_at;
                static char rest[96];
                if (kept < 0 || (int32_t)(now_ms() - kept_at) > 2000) {
                    kept = recomp_save_history_count(s_sio_dir, s_sio_id);
                    kept_at = now_ms();
                }
                if (kept > 0) {
                    snprintf(rest, sizeof rest, "Restore an earlier save (%d kept)...", kept);
                    ACT(A_SAVE_RESTORE, rest);
                }
            }
            if (s_sio_info[0])
                INFO(s_sio_info);
        }
        /* Cheats (xbox_OverlaySetCheats): a tester's rows, shown only with
         * RECOMP_DEBUG=cheats. cheat_ready() still runs, so a copy an earlier
         * session left behind is removed either way. */
        if (cheat_ready() && recomp_env(RENV_CHEATS)) {
            INFO(s_cs == CS_NONE ? "Cheats (this session only; your save is not changed):"
                                 : "While a cheat is on, the game saves to a copy, not to your save.");
            for (int i = 0; i < s_cheat_n; i++) {
                snprintf(s_cheat_label[i], sizeof s_cheat_label[i], "%s: %s",
                         s_cheats[i].name, s_cheats[i].get() ? "On" : "Off");
                ACT(A_CHEAT0 + i, s_cheat_label[i]);
            }
            if (s_cs == CS_ASK) {
                INFO("Save the progress made with cheats on?");
                ACT(A_CHEAT_KEEP, "Yes: keep it in my save");
                ACT(A_CHEAT_DISCARD, "No: throw it away (my save stays as it was)");
            }
            if (s_cs == CS_DISCARDED)
                WARN("Progress is not being saved until you restart.");
            if (s_cs_info[0])
                INFO(s_cs_info);
        }
        /* A save the save guard could not put in place (kernel_saveguard.h). */
        {
            static char sg[400];
            if (xbox_SaveGuardProblem(sg, sizeof sg))
                WARN(sg);
        }
        ACT(A_RESET_ALL, "Reset all settings to the defaults");
        break;
    }
    /* The foot of every page, kept in view below the page's own rows (the
     * last rows; see pin_count): Quit game always, and Restart only while a
     * restart is pending (the one place for it: there is no Restart row in
     * the pages). Each asks once more before it acts. */
    if (recomp_restart_available() && (restart_wanted() || staged)) {
        static char label[96];
        const int k = restart_count() + (staged && !s_restart_save[0] ? 1 : 0);
        snprintf(label, sizeof label, "Restart now to apply %d change%s", k, k == 1 ? "" : "s");
        ACT(A_RESTART_PIN, label);
    }
    ACT(A_QUIT, s_cs == CS_DISCARDED ? "Quit game (progress is not being saved until a restart)"
                : s_cs != CS_NONE ? "Quit game (progress made with cheats is not saved)" : "Quit game");
#undef SET
#undef ACT
#undef INFO
#undef WARN
#undef SPK
#undef TITLE_ROWS
    return n;
}

static void value_text(xs_id id, char *out, size_t n);

static void test_state(const char *path)
{
    static const char *const kinds[] = { "setting", "action", "info", "speakers", "folder" };
    static const char *const bkinds[] = { "use", "off", "up", "place", "dir" };
    Row r[OVERLAY_ROWS];
    int n = page_rows(s_page, r);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "open %d\npage %s\npaused %d\n", s_open ? 1 : 0, k_page_name[s_page],
            xbox_HostPaused());
    if (xsb_is_open()) {
        /* The folder browser, over the page: where it is and its rows. */
        char where[1024], c[48];
        xsb_row b;
        xsb_where(where, sizeof where);
        fprintf(f, "browser %s\n", where[0] ? where : "(places)");
        for (int i = 0; xsb_row_at(i, &b); i++) {
            browser_count(b, c, sizeof c);
            fprintf(f, "%c %s: %s%s%s\n", i == s_fb_sel ? '>' : ' ', bkinds[b.kind], b.label,
                    c[0] ? " = " : "", c);
        }
        fclose(f);
        return;
    }
    for (int i = 0; i < n; i++) {
        char v[96] = "";
        const char *label = r[i].text;
        if (r[i].kind == R_SETTING) {
            label = meta(r[i].id).label;
            value_text(r[i].id, v, sizeof v);
        } else if (r[i].kind == R_FOLDER) {
            music_value(v, sizeof v);
        }
        fprintf(f, "%c %s: %s%s%s\n", s_open && i == s_sel[s_page] ? '>' : ' ', kinds[r[i].kind],
                label ? label : "", v[0] ? " = " : "", v);
    }
    fclose(f);
}

/* Tests (overlay_shot): where on the open page the row whose text starts
 * with `prefix` is, -1 when it has none. */
int xbox_OverlayTestRowIndex(const char *prefix)
{
    Row r[OVERLAY_ROWS];
    int n = page_rows(s_page, r);
    for (int i = 0; i < n; i++) {
        const char *t = r[i].kind == R_SETTING ? meta(r[i].id).label : r[i].text;
        if (t && !strncmp(t, prefix, strlen(prefix)))
            return i;
    }
    return -1;
}

int xbox_OverlayTestHasRow(const char *prefix)
{
    return xbox_OverlayTestRowIndex(prefix) >= 0;
}

int xbox_OverlayTestSelectRow(const char *prefix)
{
    int i = xbox_OverlayTestRowIndex(prefix);
    if (i < 0)
        return 0;
    s_sel[s_page] = i;
    s_scroll_to_sel = true;
    return 1;
}

static bool page_has(int page, xs_id id)
{
    Row r[OVERLAY_ROWS];
    int n = page_rows(page, r), i;
    for (i = 0; i < n; i++)
        if (r[i].kind == R_SETTING && r[i].id == id)
            return true;
    return false;
}

static bool selectable(const Row &r) { return r.kind != R_INFO; }

/* ── the speaker test ──────────────────────────────────────────────── */

static const char *const k_spk_short[6] = { "FL", "FR", "C", "LFE", "SL", "SR" };
static const char *const k_spk_name[6] = { "Front Left", "Front Right", "Center", "LFE",
                                           "Surround Left", "Surround Right" };
static int s_spk_sel[2];              /* the button picked on each row */

static void spk_play(int row)
{
    mcpx_apu_speaker_test(s_spk_sel[row], row == 0);
}

/* The hint for a speaker row: which mixbins the game-mix row feeds. */
static void spk_hint(int row, char *out, size_t n)
{
    int sp = s_spk_sel[row];
    if (row == 1) {
        snprintf(out, n, "%s: noise straight into that output channel, after the mix. "
                 "If this row and the game-mix row disagree, the speaker map is wrong; "
                 "if both are wrong, the output order is.", k_spk_name[sp]);
        return;
    }
    if (mcpx_apu_title_channels() != 6) {
        snprintf(out, n, "%s: noise into mixbin %d (DirectSound's), through the stereo mix "
                 "(even bins left, odd right: C plays left, LFE right).",
                 k_spk_name[sp], sp);
        return;
    }
    int8_t map[32];
    char bins[128] = "";
    size_t len = 0;
    mcpx_apu_speaker_map_get(map);
    for (int b = 0; b < 32; b++)
        if (map[b] == sp && len < sizeof bins - 8)
            len += snprintf(bins + len, sizeof bins - len, "%s%d", len ? ", " : "", b);
    snprintf(out, n, "%s: noise into the mixbins the speaker map sends there (%s), "
             "mixed as the game's sounds are.", k_spk_name[sp], bins);
}

/* ── value text ────────────────────────────────────────────────────── */

static void value_text(xs_id id, char *out, size_t n)
{
    const xs_setting *s = xbox_SettingGet(id);
    const Meta &m = meta(id);
    double v = s->value;

    switch (s->type) {
    case XS_BOOL:
        snprintf(out, n, "%s", v != 0.0 ? "On" : "Off");
        break;
    case XS_CHOICE:
        {
            const char *const *names = m.names == n_power && !on_steam_deck() ? n_power_plain : m.names;
            int i = (int)v, nn = 0, nc = 0;
            while (names && names[nn])
                nn++;
            while (s->choices && s->choices[nc])
                nc++;
            /* The shown names are the table's choices, one for one; a list
             * that is not (BO-245: the Graphics page's texture row showed
             * "(null)" with another row's names) is said once and the
             * setting's own key is shown. */
            if (names && nn != nc) {
                static bool said[XS_COUNT];
                if (!said[id]) {
                    said[id] = true;
                    fprintf(stderr, "[OVERLAY] %s: %d names shown for %d choices\n", s->key, nn, nc);
                }
                names = NULL;
            }
            if (i < 0 || i >= nc)
                snprintf(out, n, "%g", v);
            else
                snprintf(out, n, "%s", names ? names[i] : s->choices[i]);
        }
        break;
    default:
        switch (m.fmt) {
        case FMT_PERCENT:        snprintf(out, n, "%d%%", (int)floor(v * 100.0 + 0.5)); break;
        case FMT_PERCENT_OF_MAX: snprintf(out, n, "%d%%", (int)floor(v / s->max * 100.0 + 0.5)); break;
        case FMT_TIMES:          snprintf(out, n, "%dx", (int)v); break;
        case FMT_NITS:
            {
                /* BO-244: nits on every platform (macOS outputs HDR10 too). */
                double q = v;
                if (v <= 0.0 && id == XS_GRAPHICS_HDR_PEAK) {
                    float pk, pw, hw;
                    nv2a_host_hdr_levels(&pk, &pw, &hw);
                    q = pk;
                }
                if (v <= 0.0 && id != XS_GRAPHICS_HDR_PEAK)
                    snprintf(out, n, "Same as paper white");
                else
                    snprintf(out, n, v <= 0.0 ? "Display (%d nits)" : "%d nits", (int)(q + 0.5));
            }
            break;
        case FMT_GAIN:
            if (v <= 0.0)
                snprintf(out, n, "Off");
            else
                snprintf(out, n, v == 1.0 ? "%.2f (original)" : "%.2f", v);
            break;
        default:                 snprintf(out, n, "%g", v); break;
        }
    }
}

/* ── changing a row ────────────────────────────────────────────────── */

static uint32_t now_ms() { return SDL_GetTicks(); }

static void changed()
{
    s_save_pending = true;
    s_save_at = now_ms() + 600;
}

/* A row that cannot be changed now: the environment sets it, or (Custom
 * music at start) there is no music folder for it to act on. */
static bool setting_inactive(xs_id id)
{
    const xs_setting *s = xbox_SettingGet(id);
    if (s->locked)
        return true;
    return id == XS_AUDIO_CUSTOM_AT_START && !xbox_SettingsText("audio.custom_music_dir", NULL)[0];
}

/* Left/right on a setting (dir -1/+1). */
static void adjust(xs_id id, int dir)
{
    const xs_setting *s = xbox_SettingGet(id);
    const Meta &m = meta(id);
    double v = s->value;

    if (setting_inactive(id))
        return;
    switch (s->type) {
    case XS_BOOL:
        v = dir > 0 ? 1 : 0;
        break;
    case XS_CHOICE: {
        int n = 0, i = (int)v;
        while (s->choices[n])
            n++;
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            unsigned off = m.disabled;
            if (id == XS_POWER_MODE && xbox_PowerStockHidden(xbox_SettingsHost()))
                off |= 1u;      /* Maximum: not offered here (BO-75) */
            if (id == XS_GAME_LANGUAGE)
                off |= ~xbox_SettingsLanguages();   /* not on the title's disc */
            if (!(off & (1u << i)))
                break;
        }
        v = i;
        break;
    }
    default:
        if (id == XS_GRAPHICS_HDR_PEAK || id == XS_GRAPHICS_HDR_HUD) {
            /* 0 is "the display's" / "same as paper white": the first step
             * up starts from the value in use, a step below the minimum goes
             * back to 0. */
            float pk, pw, hw;
            nv2a_host_hdr_levels(&pk, &pw, &hw);
            double lo = id == XS_GRAPHICS_HDR_PEAK ? 200.0 : 80.0;
            if (v <= 0.0)
                v = dir > 0 ? (id == XS_GRAPHICS_HDR_PEAK ? floor(pk / 50.0 + 0.5) * 50.0 : pw) : 0.0;
            else {
                v += dir * m.step;
                if (v < lo)
                    v = 0.0;
            }
            if (v > s->max)
                v = s->max;
            break;
        }
        v += dir * m.step;
        break;
    }
    if (v != s->value) {
        const double was = s->value;
        xbox_SettingSet(id, v);
        changed();
        if (xbox_SettingValue(id) != was)    /* not at the end of its range */
            menusnd_play(XBOX_OVERLAY_SND_CHANGE);
    }
}

/* Activate: a bool flips, a choice moves on, an action runs. */
static void mx_open();
static bool mx_back();
static void mx_poll();
static void mx_ab_toggle();
static bool s_mx_open;                 /* the Mixer page shows (overlay_mixer.inc) */

/* Restart game and Quit game ask once more: the first A (or click) arms the
 * row, the same row again does it; B, another row or another page cancels. */

/* How many rows at the end of the list are the pinned foot. */
static int pin_count(const Row *r, int n)
{
    int k = 0;
    while (k < n && r[n - 1 - k].kind == R_ACTION
           && (r[n - 1 - k].action == A_RESTART_PIN || r[n - 1 - k].action == A_QUIT))
        k++;
    return k;
}

static bool pad_hints();

/* The button that does the title's A (0, select), B (1, back), X (2) or Y (3,
 * reset) as the pad's hints name it: the same table as the glyphs in the
 * footer (pad_glyphs.h), so the Nintendo face layout, which swaps A with B and
 * X with Y here as in the game, is followed by the words too. On the keyboard
 * the overlay's own keys. `cap`: the first letter capital (a sentence's start). */
static const char *act_name(int action, bool cap = false)
{
    static char ring[4][40];
    static unsigned n;
    static const char *const keys[4] = { "Enter", "Esc", "", "Del" };
    const char *w = pad_hints() ? xbox_GlyphButtonName(xbox_InputGlyphSet(), action) : keys[action & 3];
    char *b;

    if (!cap)
        return w;
    b = ring[n++ & 3];
    snprintf(b, sizeof ring[0], "%s", w);
    if (b[0] >= 'a' && b[0] <= 'z')
        b[0] = (char)(b[0] - 32);
    return b;
}

static const char *confirm_text(int action)
{
    static char ring[4][160];
    static unsigned n;
    char *b = ring[n++ & 3];
    const char *what = action >= A_CHEAT0 && action <= A_CHEAT3 ? "switch it on"
                     : action == A_QUIT ? "quit"
                     : action == A_RESET_PAGE || action == A_RESET_ALL ? "reset" : "restart";

    snprintf(b, sizeof ring[0], "Press %s again to %s, %s to cancel", act_name(0), what, act_name(1));
    return b;
}

static void quit_now();

static void activate_row(const Row &r);
/* The title's confirm sound, after what the row did (it replaces a value
 * change's); the speaker test is heard alone, a locked setting is silent. */
static void activate(const Row &r)
{
    activate_row(r);
    if (r.kind != R_SPEAKERS && !(r.kind == R_SETTING && setting_inactive(r.id)))
        menusnd_play(XBOX_OVERLAY_SND_SELECT);
}

static void activate_row(const Row &r)
{
    if (s_confirm && !(r.kind == R_ACTION && r.action == s_confirm))
        s_confirm = 0;
    if (r.kind == R_ACTION && (r.action == A_RESTART_PIN || r.action == A_QUIT)) {
        if (s_confirm != r.action) {
            s_confirm = r.action;
            return;
        }
        s_confirm = 0;
        if (r.action == A_QUIT)
            quit_now();
        else
            restart_now();
        return;
    }
    if (r.kind == R_ACTION && r.action >= A_CHEAT0 && r.action <= A_CHEAT_DISCARD) {
        /* Switching a cheat on asks once more, under its warning. */
        int i = r.action - A_CHEAT0;
        if (r.action <= A_CHEAT3 && i < s_cheat_n && !s_cheats[i].get() && s_confirm != r.action) {
            s_confirm = r.action;
            return;
        }
        s_confirm = 0;
        cheat_action(r.action);
        return;
    }
    if (r.kind == R_ACTION && r.action == A_HDR_RESET) {
        xbox_SettingReset(XS_GRAPHICS_HDR_PEAK);
        xbox_SettingReset(XS_GRAPHICS_HDR_PAPER);
        xbox_SettingReset(XS_GRAPHICS_HDR_HUD);
        xbox_SettingReset(XS_GRAPHICS_HDR_SHADOWS);
        xbox_SettingReset(XS_GRAPHICS_HDR_HIGHLIGHTS);
        changed();
        return;
    }
    if (r.kind == R_ACTION && r.action == A_MIXER) {
        mx_open();
        return;
    }
    if (r.kind == R_SPEAKERS) {
        spk_play(r.action);
        return;
    }
    if (r.kind == R_FOLDER) {
        int locked = 0;
        xbox_SettingsText(MUSIC_KEY, &locked);
        if (!locked)
            music_access_start();
        return;
    }
    if (r.kind == R_ACTION && r.action == A_UPDATE_CHECK) {
        update_check_start();
        return;
    }
    if (r.kind == R_ACTION && r.action == A_UPDATE_OPEN) {
        update_open_page();
        return;
    }
    if (r.kind == R_ACTION && r.action == A_STEAM_ART) {
        steam_art_start();
        return;
    }
    if (r.kind == R_ACTION && r.action >= A_SAVE_EXPORT && r.action <= A_SAVE_CANCEL) {
        sio_action(r.action);
        return;
    }
    if (r.kind == R_ACTION && r.action >= A_HOST0 && r.action <= A_HOST3) {
        if (s_host_fn)
            s_host_fn(r.action - A_HOST0, 1);
        return;
    }
    if (r.kind == R_ACTION && (r.action == A_REPORT || r.action == A_REPORT_SHOT)) {
        /* On this thread, with the title paused behind the overlay: a second
         * or so (the frame is the executor's, waited for). The path is shown
         * as the archive writes paths: the home folder as ~. */
        char path[1200] = "";
        int ok = xbox_BugReportMake(r.action == A_REPORT_SHOT, path, sizeof path);
        char *shown = xbox_report_redact(NULL, path, strlen(path), NULL);
        const char *p = shown ? shown : path;
        const char *base = p;
        for (const char *c = p; *c; c++)
            if (*c == '/' || *c == '\\')
                base = c + 1;
        if (ok) {
            snprintf(s_rep_info, sizeof s_rep_info, "Saved: %.100s (path below)", base);
            snprintf(s_rep_done, sizeof s_rep_done, "Saved to %s", p);
        } else {
            snprintf(s_rep_info, sizeof s_rep_info, "Bug report: not made (reason below)");
            snprintf(s_rep_done, sizeof s_rep_done, "No report: %s", p);
        }
        fprintf(stderr, LOGP "bug report: %s\n", s_rep_done);
        free(shown);
        return;
    }
    if (r.kind == R_ACTION && r.action == A_SHADERS) {
        if (!s_sb)
            return;
        if (sb_state(NULL, NULL, 0) == XBOX_SHADERS_RUNNING)
            s_sb->cancel();
        else
            s_sb->start();
        return;
    }
    if (r.kind == R_ACTION) {
        /* Resetting asks once more: the cursor keeps its index when the rows
         * change under it (an import staged from the Advanced page shortens
         * the save rows), so one Enter can land here unseen, and a reset
         * rewrites settings.toml to the defaults. */
        if (s_confirm != r.action) {
            s_confirm = r.action;
            return;
        }
        s_confirm = 0;
        if (r.action == A_RESET_ALL) {
            xbox_SettingsResetAll();
        } else {
            for (int i = 0; i < XS_COUNT; i++)
                if (page_has(s_page, (xs_id)i))
                    xbox_SettingReset((xs_id)i);
        }
        changed();
        return;
    }
    if (r.kind != R_SETTING)
        return;
    const xs_setting *s = xbox_SettingGet(r.id);
    if (setting_inactive(r.id))
        return;
    if (s->type == XS_BOOL)
        xbox_SettingSet(r.id, s->value != 0.0 ? 0 : 1), changed();
    else if (s->type == XS_CHOICE)
        adjust(r.id, 1);
}

/* ── open and close ────────────────────────────────────────────────── */

static void clear_nav()
{
    memset(s_dpad, 0, sizeof s_dpad);
    memset(s_stick, 0, sizeof s_stick);
    memset(s_key, 0, sizeof s_key);
    memset(s_steps, 0, sizeof s_steps);
    s_activate = s_back = s_page_delta = s_reset = 0;
    s_confirm = 0;
}

/* The title is paused while the overlay is open, except on the Mixer page:
 * a mix is set by ear, so there the game keeps running and sounding (its
 * input still held: the car coasts). Called wherever either changes. */
static uint32_t s_pause_at;            /* the pause waits until then (set_open) */
static void pause_sync()
{
    static int applied = -1;
    int want = s_open && !s_mx_open && (int32_t)(now_ms() - s_pause_at) >= 0;
    if (want != applied) {
        applied = want;
        xbox_HostPause(want);
        if (s_open)
            fprintf(stderr, LOGP "%s\n", want ? "the title is paused" : "Mixer page: the title runs while it is open");
    }
}

/* from_title: asked for by the title's own menu row (xbox_OverlayRequestOpen).
 * The title has just played its confirm sound for that press: the overlay
 * plays no sound of its own over it, and the pause waits until that sound
 * is out (a paused title's voices stop where they are and would finish
 * when the overlay closes). Its input is held at once all the same. */
static void set_open(bool on, bool from_title = false)
{
    if (on == s_open)
        return;
    s_open = on;
    clear_nav();
    s_pause_at = on && from_title ? now_ms() + 700 : 0;
    if (on) {
        s_rows_t0 = now_ms();
        s_rows_step_ms = ROWS_STEP_OPEN_MS;
    }
    if (!from_title)
        menusnd_play(on ? XBOX_OVERLAY_SND_OPEN : XBOX_OVERLAY_SND_CLOSE);
    if (on) {
        s_scroll_to_sel = true;
        xbox_InputSetBlocked(1);
        pause_sync();
        fprintf(stderr, LOGP "open: the title is paused, its input is held\n");
    } else {
        /* A build beside a running title would cost it frames: it stops
         * with the overlay, and goes on from there the next time. */
        if (s_sb && sb_state(NULL, NULL, 0) == XBOX_SHADERS_RUNNING) {
            s_sb->cancel();
            fprintf(stderr, LOGP "the shader build is stopped with the overlay\n");
        }
        mcpx_apu_speaker_test(-1, 0);
        xsb_close();
        pause_sync();
        xbox_InputSetBlocked(0);
        xbox_SettingsFlush();
        s_save_pending = false;
        fprintf(stderr, LOGP "closed\n");
    }
}

/* ── restart ───────────────────────────────────────────────────────── */

static void restart_reason(char *out, size_t n)
{
    size_t at = 0;
    int count = 0;

    out[0] = 0;
    for (int i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = xbox_SettingGet((xs_id)i);
        if (!(s->flags & XS_RESTART) || s->value == s->launch)
            continue;
        at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s%s",
                               count++ ? ", " : "Applied at restart: ", meta(i).label);
        if (at >= n)
            return;
    }
    if (count)
        at += (size_t)snprintf(out + at, at < n ? n - at : 0, ".");
    if (s_restart_why[0] && at < n)
        at += (size_t)snprintf(out + at, n - at, "%s%s", at ? " " : "", s_restart_why);
    if (s_restart_save[0] && at < n)
        at += (size_t)snprintf(out + at, n - at, "%s%s", at ? " " : "", s_restart_save);
    if (s_restart_cheat[0] && at < n)
        snprintf(out + at, n - at, "%s%s", at ? " " : "", s_restart_cheat);
}

/* The overlay closes as at any time (the settings are written), then the
 * window is closed the ordinary way with the restart asked for: the
 * program's own shutdown runs, and its last step starts it again. */
static void restart_now()
{
    char why[400];
    SDL_Event q;

    restart_reason(why, sizeof why);
    set_open(false);
    recomp_restart_request(why[0] ? why : "the overlay's Restart game");
    memset(&q, 0, sizeof q);
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

/* Quit game: as closing the window. The overlay closes (the settings are
 * written), then SDL_QUIT takes the window layer's ordinary way out: pads
 * stopped, the window and audio closed, exit(0). On Android the host's hook
 * also finishes the activity (recomp_quit_request). */
static void quit_now()
{
    SDL_Event q;

    set_open(false);
    recomp_quit_request("the overlay's Quit game");
    memset(&q, 0, sizeof q);
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

/* ── events ────────────────────────────────────────────────────────── */

static void hot_toggle()
{
    set_open(!s_open);
}

/* xbox_OverlayRequestOpen: set on any thread, taken on the main loop. */
static std::atomic<int> s_open_request{0};

int xbox_OverlayAvailable(void)
{
    return s_enabled ? 1 : 0;
}

int xbox_OverlayRequestOpen(void)
{
    if (!s_enabled)
        return 0;
    s_open_request.store(1);
    return 1;
}

/* A direction goes down or up on one of the three sources. A press counts a
 * step at once (a tap shorter than a frame still moves), and a hold repeats
 * from the frame (collect_steps). */
static void dir_set(bool *arr, int d, bool on)
{
    bool was = s_dpad[d] || s_stick[d] || s_key[d];
    arr[d] = on;
    if (on && !was) {
        s_steps[d]++;
        s_next_fire[d] = now_ms() + 350;
    }
}

static void stick_axis(int axis, int value)
{
    /* Hysteresis: on past 16000, off under 8000. */
    bool neg = value < 0;
    int mag = neg ? -value : value;
    int a = axis == SDL_CONTROLLER_AXIS_LEFTX ? 0 : 1;
    int d_neg = a == 0 ? D_LEFT : D_UP, d_pos = a == 0 ? D_RIGHT : D_DOWN;

    if (mag > 16000) {
        dir_set(s_stick, neg ? d_neg : d_pos, true);
        dir_set(s_stick, neg ? d_pos : d_neg, false);
    } else if (mag < 8000) {
        dir_set(s_stick, d_neg, false);
        dir_set(s_stick, d_pos, false);
    }
}

int xbox_OverlayEvent(const SDL_Event *ev)
{
    if (!s_enabled)
        return 0;
    std::lock_guard<std::mutex> g(s_ov_mu);

    switch (ev->type) {
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
                || ev->window.event == SDL_WINDOWEVENT_MINIMIZED
                || ev->window.event == SDL_WINDOWEVENT_HIDDEN)
            xbox_SettingsSetBackground(1);
        else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED
                 || ev->window.event == SDL_WINDOWEVENT_SHOWN
                 || ev->window.event == SDL_WINDOWEVENT_RESTORED)
            xbox_SettingsSetBackground(0);
        break;

    case SDL_KEYDOWN: {
        SDL_Keycode k = ev->key.keysym.sym;
        if (ev->key.repeat)
            break;
        if (k == SDLK_F1 || (k == SDLK_COMMA && (ev->key.keysym.mod & KMOD_GUI))) {
            hot_toggle();
            return 1;
        }
        if (k == SDLK_F7 && apu_bus_enabled()) {
            mx_ab_toggle();             /* the mixer's A/B, menu open or not */
            return 1;
        }
        /* The Android system Back (the activity delivers it as this key
         * instead of finishing): closed, it opens the overlay, as F1 does;
         * open, it goes back like B and Esc. Leaving the game is the system's
         * app switcher. */
        if (k == SDLK_AC_BACK) {
            if (s_open) {
                s_last_input_pad = true;
                s_back++;
            } else
                hot_toggle();
            return 1;
        }
        if (!s_open)
            break;
        s_last_input_pad = false;
        switch (k) {
        case SDLK_UP:    dir_set(s_key, D_UP, true); break;
        case SDLK_DOWN:  dir_set(s_key, D_DOWN, true); break;
        case SDLK_LEFT:  dir_set(s_key, D_LEFT, true); break;
        case SDLK_RIGHT: dir_set(s_key, D_RIGHT, true); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: s_activate++; break;
        case SDLK_ESCAPE: s_back++; break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE: s_reset++; break;
        default:
            /* Page Up and Down, [ and ], Q and E, Tab and Shift+Tab. */
            s_page_delta += xbox_overlay_page_key(k, ev->key.keysym.mod);
            break;
        }
        return 1;
    }
    case SDL_KEYUP:
        if (!s_open)
            break;
        switch (ev->key.keysym.sym) {
        case SDLK_UP:    dir_set(s_key, D_UP, false); break;
        case SDLK_DOWN:  dir_set(s_key, D_DOWN, false); break;
        case SDLK_LEFT:  dir_set(s_key, D_LEFT, false); break;
        case SDLK_RIGHT: dir_set(s_key, D_RIGHT, false); break;
        default: break;
        }
        return 1;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = ev->type == SDL_CONTROLLERBUTTONDOWN;
        unsigned bit = 1u << ((unsigned)ev->cbutton.which & 31u);
        int b = ev->cbutton.button;

        if (b == SDL_CONTROLLER_BUTTON_BACK) {
            if (down) s_back_mask |= bit; else s_back_mask &= ~bit;
        } else if (b == SDL_CONTROLLER_BUTTON_START) {
            if (down) s_start_mask |= bit; else s_start_mask &= ~bit;
        }
        /* Back and Start of one pad together: once per hold. */
        if (down && (s_back_mask & s_start_mask) && !s_combo_fired) {
            s_combo_fired = true;
            hot_toggle();
            return 1;
        }
        if (!(s_back_mask | s_start_mask))
            s_combo_fired = false;
        /* Back held + Y: the mixer's A/B, menu open or not. */
        if (down && b == SDL_CONTROLLER_BUTTON_Y && (s_back_mask & bit) && apu_bus_enabled()) {
            mx_ab_toggle();
            return 1;
        }
        if (!s_open)
            break;
        s_last_input_pad = true;
        /* The Nintendo face layout (Input > Face buttons) holds here too:
         * the button printed A confirms, the one printed B goes back. */
        if (xbox_InputFaceLayoutNintendo()) {
            if (b == SDL_CONTROLLER_BUTTON_A) b = SDL_CONTROLLER_BUTTON_B;
            else if (b == SDL_CONTROLLER_BUTTON_B) b = SDL_CONTROLLER_BUTTON_A;
            else if (b == SDL_CONTROLLER_BUTTON_X) b = SDL_CONTROLLER_BUTTON_Y;
            else if (b == SDL_CONTROLLER_BUTTON_Y) b = SDL_CONTROLLER_BUTTON_X;
        }
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    dir_set(s_dpad, D_UP, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  dir_set(s_dpad, D_DOWN, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dir_set(s_dpad, D_LEFT, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dir_set(s_dpad, D_RIGHT, down); break;
        case SDL_CONTROLLER_BUTTON_A: if (down) s_activate++; break;
        case SDL_CONTROLLER_BUTTON_B: if (down) s_back++; break;
        case SDL_CONTROLLER_BUTTON_Y: if (down) s_reset++; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  if (down) s_page_delta--; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) s_page_delta++; break;
        default: break;
        }
        return 1;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (!s_open)
            break;
        if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            s_last_input_pad = true;
            stick_axis(ev->caxis.axis, ev->caxis.value);
        }
        return 1;

    default:
        break;
    }

    /* The mouse, and the window's own events, for ImGui while it shows. */
    if (s_open && s_core_ready) {
        ImGui_ImplSDL2_ProcessEvent(ev);
        if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN
                || ev->type == SDL_MOUSEBUTTONUP || ev->type == SDL_MOUSEWHEEL)
            return 1;
    }
    return 0;
}

/* A held direction repeats, after a first delay. */
static void collect_steps()
{
    uint32_t now = now_ms();
    for (int d = 0; d < D_COUNT; d++) {
        bool held = s_dpad[d] || s_stick[d] || s_key[d];
        if (held && (int32_t)(now - s_next_fire[d]) >= 0) {
            s_steps[d]++;
            s_next_fire[d] = now + 70;
        }
    }
}

/* ── RECOMP_OVERLAY_TEST: a test drives the overlay ────────────────── */

extern "C" void xbox_HostWindowShot(const char *path);     /* video_player.h */
extern "C" void xbox_HostWindowShotBurst(const char *prefix, unsigned n);

/* Each line appended to the file is done once: keys and pad buttons go into
 * the window's event queue as SDL events, so they take the same road as a
 * real press (xbox_OverlayEvent and the input layer after it); a shot is the
 * window as presented; a state dump is the open page's rows, the selected
 * one marked, for a test to assert on. */
static void test_key(SDL_Keycode sym, Uint16 mod = 0)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = mod;
    SDL_PushEvent(&ev);
    ev.type = SDL_KEYUP;
    ev.key.state = SDL_RELEASED;
    SDL_PushEvent(&ev);
}

static void test_pad(int button, bool down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.which = 30;            /* no such controller: the overlay only */
    ev.cbutton.button = (Uint8)button;
    ev.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&ev);
}

static void test_state(const char *path);

static void test_poll()
{
    static const char *path;
    static bool checked;
    static long consumed;
    static uint32_t next;
    static const struct { const char *name; SDL_Keycode sym; Uint16 mod; } keys[] = {
        {"up", SDLK_UP, 0}, {"down", SDLK_DOWN, 0}, {"left", SDLK_LEFT, 0}, {"right", SDLK_RIGHT, 0},
        {"enter", SDLK_RETURN, 0}, {"esc", SDLK_ESCAPE, 0}, {"f1", SDLK_F1, 0},
        {"pgup", SDLK_PAGEUP, 0}, {"pgdn", SDLK_PAGEDOWN, 0}, {"del", SDLK_DELETE, 0},
        {"q", SDLK_q, 0}, {"e", SDLK_e, 0}, {"tab", SDLK_TAB, 0}, {"shift+tab", SDLK_TAB, KMOD_LSHIFT},
    };
    static const struct { const char *name; int b; } pads[] = {
        {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B},
        {"y", SDL_CONTROLLER_BUTTON_Y}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    };
    char line[600];
    FILE *f;

    if (!checked) {
        checked = true;
        path = getenv("RECOMP_OVERLAY_TEST");
        if (path && *path)
            fprintf(stderr, LOGP "driven by %s\n", path);
    }
    if (!path || !*path || (int32_t)(now_ms() - next) < 0)
        return;
    next = now_ms() + 30;
    if (!(f = fopen(path, "rb")))
        return;
    /* One line a turn: the events of one are handled before the next. */
    if (fseek(f, consumed, SEEK_SET) == 0 && fgets(line, sizeof line, f)
            && strchr(line, '\n')) {
        consumed += (long)strlen(line);
        line[strcspn(line, "\r\n")] = 0;
        fprintf(stderr, LOGP "test: %s\n", line);
        if (!strncmp(line, "key ", 4)) {
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (!strcmp(line + 4, keys[i].name))
                    test_key(keys[i].sym, keys[i].mod);
        } else if (!strcmp(line, "pad back+start")) {
            test_pad(SDL_CONTROLLER_BUTTON_BACK, true);
            test_pad(SDL_CONTROLLER_BUTTON_START, true);
            test_pad(SDL_CONTROLLER_BUTTON_START, false);
            test_pad(SDL_CONTROLLER_BUTTON_BACK, false);
        } else if (!strncmp(line, "pad ", 4)) {
            for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++)
                if (!strcmp(line + 4, pads[i].name)) {
                    test_pad(pads[i].b, true);
                    test_pad(pads[i].b, false);
                }
        } else if (!strncmp(line, "burst ", 6)) {   /* burst <n> <prefix> */
            char *end;
            unsigned long n = strtoul(line + 6, &end, 10);
            if (*end == ' ')
                xbox_HostWindowShotBurst(end + 1, (unsigned)n);
        } else if (!strncmp(line, "shot ", 5)) {
            xbox_HostWindowShot(line + 5);
        } else if (!strncmp(line, "state ", 6)) {
            test_state(line + 6);
        }
    }
    fclose(f);
}

void xbox_OverlayPoll(void)
{
    if (!s_enabled)
        return;
    std::lock_guard<std::mutex> g(s_ov_mu);
    test_poll();

    if (s_open_request.exchange(0) && !s_open)
        set_open(true, true);
    /* The on-screen controls' menu button. */
    if (xbox_InputTouchTakeMenu() && !s_open)
        set_open(true);

    while (s_toggle_i < s_toggle_n && (int32_t)(now_ms() - s_t0_ms - s_toggle_ms[s_toggle_i]) >= 0) {
        s_toggle_i++;
        hot_toggle();
    }

    if (s_cycle_ms && (int32_t)(now_ms() - s_cycle_next) >= 0) {
        s_cycle_next = now_ms() + s_cycle_ms;
        s_cycle_i = (s_cycle_i + 1) % 3;
        xbox_SettingSet(XS_GRAPHICS_AA, s_cycle_i);
    }
    if (s_trigger[0] && (int32_t)(now_ms() - s_trigger_next) >= 0) {
        s_trigger_next = now_ms() + 200;
        if (unlink(s_trigger) == 0)
            hot_toggle();
    }
    if (s_pacing_file[0] && (int32_t)(now_ms() - s_trigger_next) >= 0) {
        FILE *f = fopen(s_pacing_file, "r");
        s_trigger_next = now_ms() + 200;
        if (f) {
            char w[32] = { 0 };
            int v;
            if (!fgets(w, sizeof w, f))
                w[0] = 0;
            fclose(f);
            unlink(s_pacing_file);
            v = !strncmp(w, "low_latency", 11) ? 1 : !strncmp(w, "smooth", 6) ? 2 : 0;
            fprintf(stderr, "[OVERLAY] pacing_file: frame pacing -> %s\n",
                    v == 1 ? "low_latency" : v == 2 ? "smooth" : "auto");
            xbox_SettingSet(XS_DISPLAY_FRAME_PACING, v);
        }
    }

    /* B or Esc: here and not in the frame, so a window that cannot present
     * (minimised) still closes. */
    mx_poll();
    pause_sync();
    if (s_back > 0) {
        s_back = 0;
        if (s_open && s_confirm) {
            s_confirm = 0;                /* B takes back a Restart / Quit question */
            menusnd_play(XBOX_OVERLAY_SND_BACK);
        } else if (s_open && s_mx_open) {
            mx_back();
            menusnd_play(XBOX_OVERLAY_SND_BACK);
        } else if (s_open && xsb_is_open()) {
            menusnd_play(XBOX_OVERLAY_SND_BACK);
            /* In the folder browser: one level up, then back to the page. */
            int sel = 0;
            if (xsb_back(&sel) == XSB_CLOSED)
                xsb_close();
            else {
                s_fb_sel = sel;
                s_fb_scroll = true;
            }
        } else if (s_open)
            set_open(false);
    }
    pause_sync();

    /* The fullscreen setting, when it changes (not on Android: always the
     * whole screen there, whatever an older settings file says). */
#if !defined(__ANDROID__)
    if (s_win) {
        int fs = xbox_SettingValue(XS_DISPLAY_FULLSCREEN) != 0.0;
        if (s_fs_applied < 0)
            s_fs_applied = fs;
        else if (fs != s_fs_applied) {
            s_fs_applied = fs;
            SDL_SetWindowFullscreen(s_win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
    }
#endif
    if (s_save_pending && (int32_t)(now_ms() - s_save_at) >= 0) {
        s_save_pending = false;
        xbox_SettingsFlush();
    }
}

void overlay_fail()
{
    if (s_open)
        set_open(false);
    s_enabled = false;
    xbox_InputSetComboGuard(0);
    fprintf(stderr, LOGP "no overlay (its renderer did not start)\n");
}

int xbox_OverlaySetWindow(SDL_Window *win)
{
    const char *e = getenv("RECOMP_OVERLAY");

    s_win = win;
    s_enabled = !(e && !strcmp(e, "0"));
    if (!s_enabled) {
        fprintf(stderr, LOGP "off (RECOMP_OVERLAY=0)\n");
        return 0;
    }
    xbox_InputSetComboGuard(1);
    s_t0_ms = now_ms();
    if (e && !strncmp(e, "toggle=", 7)) {
        const char *q = e + 7;
        while (*q && s_toggle_n < TOGGLE_MAX) {
            char *end;
            double sec = strtod(q, &end);
            if (end == q)
                break;
            s_toggle_ms[s_toggle_n++] = (uint32_t)(sec * 1000.0);
            q = *end == ',' ? end + 1 : end;
        }
    }
    if (e && !strncmp(e, "aa_cycle=", 9)) {
        s_cycle_ms = (uint32_t)(atof(e + 9) * 1000.0);
        s_cycle_next = now_ms() + s_cycle_ms;
    }
    if (e && !strncmp(e, "file=", 5))
        snprintf(s_trigger, sizeof s_trigger, "%s", e + 5);
    if (e && !strncmp(e, "pacing_file=", 12))
        snprintf(s_pacing_file, sizeof s_pacing_file, "%s", e + 12);
    if (e && !strcmp(e, "open"))
        set_open(true);
    return 1;
}

int xbox_OverlayOpen(void)
{
    return s_enabled && s_open;
}

/* The render thread is held for the start-up pre-warm (a key list whose
 * compiled shaders the system no longer has: up to 3 s, once, before the
 * title's first pictures): the window says what it is doing instead of
 * standing black. */
static bool s_notice_hdr;      /* BO-110: the HDR variants after a switch to HDR */
static bool notice_on()
{
    nv2a_pcache_stat st;
    nv2a_pcache_stat_get(&st);
    s_notice_hdr = st.warming && st.hdr_round;
    return st.waiting != 0 || s_notice_hdr;
}

/* A pad script is driving a pad (RECOMP_PAD_LIVE / RECOMP_PAD_SCRIPT: a test
 * run) and no controller has taken it back: said on the screen, so a run a
 * script started is not picked up as one's own with the script's buttons
 * still held (BO-96). */
static bool scripted_on()
{
    /* RECOMP_DEBUG=pad_script_note=0: not shown (a scripted run whose
     * window shots are compared). Drawn with the title's frames only: it
     * does not make the window redraw by itself, so a measured run's
     * pacing is what it was. */
    static int show = -1;
    if (show < 0) {
        const char *v = recomp_env(RENV_PAD_SCRIPT_NOTE);
        show = !(v && *v == '0');
    }
    return show && xbox_InputScriptedPads() != 0;
}

/* A save the save guard could not put in place (kernel_saveguard.h): a
 * new failure is shown over the game for a while, overlay open or not; the
 * Advanced page keeps the line while it stands. */
static char s_sg_text[400];
static bool sg_banner_on()
{
    static int seen;
    static uint32_t until;
    const int seq = xbox_SaveGuardProblem(s_sg_text, sizeof s_sg_text);
    const uint32_t now = now_ms();
    if (seq && seq != seen) {
        seen = seq;
        until = now + 15000u;
    }
    return seq && (int32_t)(until - now) > 0;
}

int xbox_OverlayNotice(void)
{
    return s_enabled && notice_on();
}

/* The on-screen controls show (touch_pad.h): drawn with the title's frames
 * while the overlay is closed. */
static bool touch_on()
{
    return xbox_InputTouchVisible() != 0;
}

int xbox_OverlayWants(void)
{
    return s_enabled && (s_open || s_alpha > 0.0f || xbox_SettingValue(XS_ADVANCED_PERF) != 0.0
                         || notice_on() || scripted_on() || sg_banner_on() || touch_on());
}

void xbox_OverlayFramePresented(void)
{
    s_presents++;
}

void xbox_OverlayPlatformFrame(int vulkan)
{
    if (!s_enabled)
        return;
    std::lock_guard<std::mutex> g(s_ov_mu);
    if (!s_core_ready) {
        /* The context and ImGui's SDL half (cursors, the window's id) are
         * made here, on the window's thread, before the first draw. */
        if (!overlay_core_init(vulkan != 0)) {
            overlay_fail();
            return;
        }
    }
    if (s_plat_pending || !xbox_OverlayWants())
        return;
    /* The window's size, the mouse and its cursor: SDL window calls, which
     * stay on this thread (AppKit's on macOS). */
    ImGui_ImplSDL2_NewFrame();
    s_plat_pending = s_plat_ever = true;
}

void xbox_OverlayShutdown(void)
{
    if (!s_enabled)
        return;
    std::lock_guard<std::mutex> g(s_ov_mu);
    if (s_open)
        set_open(false);
    xbox_SettingsFlush();
}

/* ── the colours ───────────────────────────────────────────────────── */

/* Every colour the overlay draws comes from one of these tables; nothing
 * below names a number. "game" is the main menu's own scheme (sampled from
 * its front end: ink-navy panels, steel-blue rules and accent, pale blue
 * text, the title red); "classic" is the earlier neutral grey and orange.
 * RECOMP_OVERLAY_THEME=classic picks the second; there is no setting for it
 * (a compile-time table plus that variable). Colours only. */
struct Rgb { uint8_t r, g, b; };

struct Theme {
    const char *name;
    Rgb scrim;          /* over the game, behind the panel */
    Rgb panel;          /* the settings window */
    Rgb edge;           /* the window's outline; border 0 = none */
    float border;       /* its width in points */
    Rgb rule;           /* thin separators */
    Rgb sel_fill;       /* the selected row's wash (alpha sel_fill_a) */
    float sel_fill_a;
    Rgb sel_bar;        /* the selected row's left bar */
    Rgb tab_on, tab_on_text, tab_text, tab_hint;
    Rgb text;           /* labels */
    Rgb text_hi;        /* the selected row's label and the values */
    Rgb text_dim;       /* locked rows, hints, the key line */
    Rgb text_info;      /* info rows */
    Rgb action;         /* action rows */
    Rgb track, track_off, fill, fill_dim, knob, on_accent;
    Rgb warn;           /* a locked row's reason, the restart note */
    Rgb notice_bg, notice_text;     /* the red scripted-input banner */
    Rgb toast_bg, toast_text;       /* the "Preparing shaders" line */
    Rgb perf_bg, perf_text, perf_text2, perf_good, perf_warn, perf_bad, perf_line;
    Rgb title;          /* the page title (the game's red) */
    Rgb hint;           /* hints, the footer, secondary lines */
    Rgb arrow;          /* the value arrows and the cursor */
    Rgb bar;            /* the footer bar */
    Rgb attn;           /* attention: restart pending, a locked row's reason (the game's yellow) */
    Rgb rule_hi;        /* the header's and the footer's rules */
    Rgb arc;            /* the big arc on the left */
    Rgb head_band;      /* behind the header, over the dimmed game */
    Rgb shadow, outline;/* the big title's shadow and outline */
    Rgb box_fill, box_edge;     /* the value boxes: the shade and the outline */
};

static const Theme k_theme_game = {
    "game",
    {0x00, 0x00, 0x00},             /* scrim: black */
    {0x16, 0x1d, 0x26},             /* panel: the menu body navy */
    {0x5a, 0x99, 0xc0}, 1.0f,       /* edge: the steel-blue rule */
    {0x30, 0x38, 0x40},             /* rule: the thin lines between the regions */
    {0x5a, 0x99, 0xc0}, 0.16f,      /* selected wash: steel blue */
    {0xc4, 0x1c, 0x1c},             /* selected bar: the title crimson */
    {0x5a, 0x99, 0xc0}, {0x0f, 0x13, 0x19}, {0x58, 0x98, 0xc0}, {0x78, 0x90, 0xb0},
    {0xa8, 0xd0, 0xf8},             /* text: the list items */
    {0xd0, 0xe0, 0xf8},             /* hi: the selected item and the values */
    {0x48, 0x58, 0x68},             /* dim: disabled (locked) */
    {0x58, 0x98, 0xc0},             /* info: the hint blue */
    {0xa8, 0xd0, 0xf8},             /* action */
    {0x2f, 0x39, 0x42}, {0x3f, 0x4d, 0x5d}, {0x5a, 0x99, 0xc0}, {0x4a, 0x5a, 0x6a},
    {0xd4, 0xe4, 0xfc}, {0x0f, 0x13, 0x19},
    {0xf8, 0xc0, 0x20},             /* warn: attention yellow */
    {0x94, 0x0c, 0x14}, {0xff, 0xff, 0xff},     /* notice: the title red band */
    {0x11, 0x15, 0x1a}, {0xd4, 0xe4, 0xfc},     /* toast: the header/footer plate */
    {0x0f, 0x13, 0x19}, {0xe6, 0xf0, 0xff}, {0xac, 0xd4, 0xfc},
    {0x5c, 0xb0, 0x1a}, {0xe8, 0xc8, 0x46}, {0xe8, 0x48, 0x40}, {0xd4, 0xe4, 0xfc},
    {0xc0, 0x18, 0x18},             /* title */
    {0x58, 0x98, 0xc0},             /* hint */
    {0x88, 0xc0, 0xe0},             /* arrow */
    {0x10, 0x10, 0x18},             /* bar */
    {0xf8, 0xc0, 0x20},             /* attn */
    {0x38, 0x58, 0x78},             /* rule_hi */
    {0x38, 0x50, 0x78},             /* arc */
    {0x08, 0x0a, 0x10},             /* head_band */
    {0x00, 0x00, 0x00}, {0xf0, 0xf0, 0xf0},
    {0x10, 0x18, 0x20}, {0x38, 0x58, 0x70},
};

static const Theme k_theme_classic = {
    "classic",
    {0, 0, 0},
    {18, 18, 20},
    {0, 0, 0}, 0.0f,
    {51, 51, 53},
    {255, 120, 20}, 0.22f,
    {255, 140, 30},
    {255, 140, 30}, {20, 14, 8}, {190, 190, 198}, {120, 120, 128},
    {240, 240, 244},
    {255, 255, 255},
    {130, 130, 136},
    {160, 160, 168},
    {255, 190, 120},
    {60, 60, 66}, {70, 70, 76}, {255, 140, 30}, {110, 110, 116},
    {250, 250, 252}, {20, 14, 8},
    {240, 170, 90},
    {120, 30, 20}, {255, 240, 230},
    {18, 18, 20}, {235, 235, 240},
    {8, 8, 10}, {255, 255, 255}, {190, 190, 195},
    {90, 210, 120}, {235, 200, 70}, {235, 90, 80}, {255, 255, 255},
    {255, 140, 30}, {130, 130, 136}, {190, 190, 198}, {12, 12, 14}, {240, 190, 70},
    {70, 70, 76}, {60, 60, 66}, {8, 8, 10}, {0, 0, 0}, {240, 240, 240},
    {12, 12, 14}, {70, 70, 76},
};

static const Theme *T = &k_theme_game;

static void theme_pick()
{
    const char *e = getenv("RECOMP_OVERLAY_THEME");
    T = e && !strcmp(e, "classic") ? &k_theme_classic : &k_theme_game;
}

/* A title's own look (overlay_skin.h, xbox_OverlaySetSkin): from the first
 * frame its pictures are in, the colours are its own (the game theme's for
 * what it does not name: the notices, the graph), and so is the lean.
 * Without one nothing here runs. */
static Theme s_theme_skin;
static bool s_skinned;
/* The italics' lean (SHEAR: a fifth of the height); a skin sets its own. */
static float s_lean = 0.2f;

static bool skin_frame()
{
    if (s_skinned)
        return true;
    if (!skin_attach())
        return false;
    const xbox_overlay_skin *k = skin();
    auto rgb = [](xbox_overlay_rgb c) { return Rgb{ c.r, c.g, c.b }; };
    Theme &t = s_theme_skin;
    t = k_theme_game;
    t.name = "skin";
    t.scrim = t.panel = t.head_band = t.bar = rgb(k->page);
    t.text = t.action = rgb(k->text);
    t.text_hi = rgb(k->text_hi);
    t.text_dim = t.tab_text = rgb(k->text_dim);
    t.hint = t.text_info = t.title = t.arrow = t.tab_on = rgb(k->hint);
    t.attn = t.warn = rgb(k->attn);
    t.outline = t.shadow = rgb(k->outline);
    t.fill = rgb(k->text_hi);
    T = &s_theme_skin;
    s_lean = k->lean;
    s_skinned = true;
    return true;
}

/* ── drawing ───────────────────────────────────────────────────────── */

static ImU32 col(Rgb c, float a = 1.0f)
{
    return IM_COL32(c.r, c.g, c.b, (int)(a * s_alpha * 255.0f));
}

/* Always drawn whole: the perf graph and the notices do not fade. */
static ImU32 solid(Rgb c, float a = 1.0f)
{
    return IM_COL32(c.r, c.g, c.b, (int)(a * 255.0f));
}

static void accent_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f;
    st.WindowBorderSize = T->border;
    st.ScrollbarRounding = 6.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(T->panel.r / 255.0f, T->panel.g / 255.0f, T->panel.b / 255.0f, 0.96f);
    st.Colors[ImGuiCol_Border] = ImVec4(T->edge.r / 255.0f, T->edge.g / 255.0f, T->edge.b / 255.0f, 0.8f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
}

/* The fonts. Three faces, each a chain Dear ImGui walks for a letter:
 *
 *   ui     Roboto Condensed Medium (OFL, overlay_font_ui.h): Latin with
 *          Latin-1 and Extended-A/-B, Greek, Cyrillic. The hints, the Mixer
 *          page, the folder browser, the graph. Behind it a subset of DejaVu
 *          Sans Mono (overlay_font_ext.h) for a few signs and U+25A1, the
 *          white square shown for a character neither has (CJK, emoji ...).
 *   body   the title's text font (GlobalFont in Burnout 3) when the port
 *          named it and it was read (overlay_gamefont.h), with ui's chain
 *          behind it: a folder called "Zenék – ősz" or "Музыка" mixes the
 *          game's letters with Roboto's on one baseline, at the same
 *          capital height. Titles, tabs, labels, values, the keys.
 *   title  the title's large font (HeadFont: capitals only), then body.
 *
 * body's and title's size is the game's cell (the tallest glyph), as the
 * game's menu is measured; ui's is Dear ImGui's usual (ascent + descent).
 * Without the game's fonts body and title are Roboto, scaled to the same
 * capital height for the same size, so a page looks the same size either
 * way. The game's fonts come from a thread: they are added between two
 * frames when they are ready (gamefont_attach). */
static ImFont *s_f_ui, *s_f_body, *s_f_title;
static bool s_gf_attached;

/* The game's capitals over its cell, for Roboto standing in: GlobalFont
 * 22 of 34 texels, HeadFont 38 of 47 (measured from the fonts; used only
 * when they are not there). */
#define CAPS_BODY_NOMINAL  0.647f
#define CAPS_TITLE_NOMINAL 0.809f
static float s_caps_body = CAPS_BODY_NOMINAL, s_caps_title = CAPS_TITLE_NOMINAL;
static const float k_ui_caps = (float)OVERLAY_FONT_UI_CAPS / (float)OVERLAY_FONT_UI_HEIGHT;

/* Roboto, and DejaVu behind it, into `dst` (merged) or as a font of their
 * own (dst NULL), scaled so the capitals are `caps` of the size (0: as
 * they are). Returns the font they are in. */
static ImFont *add_ui_chain(ImFont *dst, float caps)
{
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.MergeMode = dst != nullptr;
    cfg.DstFont = dst;
    cfg.FontDataOwnedByAtlas = false;
    cfg.ExtraSizeScale = caps > 0.0f ? caps / k_ui_caps : 1.0f;
    snprintf(cfg.Name, sizeof cfg.Name, "Roboto Condensed Medium subset");
    ImFont *f = io.Fonts->AddFontFromMemoryTTF((void *)k_overlay_font_ui, (int)sizeof k_overlay_font_ui,
                                               dst ? 0.0f : 13.0f, &cfg);
    ImFont *into = dst ? dst : f;
    ImFontConfig ext;
    ext.MergeMode = true;
    ext.DstFont = into;
    ext.FontDataOwnedByAtlas = false;
    ext.ExtraSizeScale = cfg.ExtraSizeScale;
    snprintf(ext.Name, sizeof ext.Name, "DejaVuSansMono subset");
    io.Fonts->AddFontFromMemoryTTF((void *)k_overlay_font_ext, (int)sizeof k_overlay_font_ext, 0.0f, &ext);
    into->FallbackChar = 0x25A1;
    return into;
}

static ImFont *add_ui_font(float caps)
{
    return add_ui_chain(nullptr, caps);
}

static void overlay_fonts()
{
    s_f_ui = add_ui_font(0.0f);
    s_f_body = add_ui_font(CAPS_BODY_NOMINAL);
    s_f_title = add_ui_font(CAPS_TITLE_NOMINAL);
    ImGui::GetIO().FontDefault = s_f_ui;
}

/* The game's fonts, once the thread has them: body = GlobalFont, then ui;
 * title = HeadFont, then GlobalFont, then ui, each scaled to the first
 * one's capitals. Between frames only (the atlas is not in use). */
static void gamefont_attach()
{
    if (s_gf_attached || gamefont_state() != 1)
        return;
    s_gf_attached = true;
    ImGuiIO &io = ImGui::GetIO();
    const GameFont *gb = gamefont(GF_BODY), *gt = gamefont(GF_TITLE);
    auto add_game = [&](const GameFont *g, ImFont *dst, float scale) -> ImFont * {
        ImFontConfig cfg;
        cfg.MergeMode = dst != nullptr;
        cfg.DstFont = dst;
        cfg.FontData = (void *)g;
        cfg.FontDataSize = 1;
        cfg.FontDataOwnedByAtlas = false;
        cfg.FontLoader = gamefont_loader();
        cfg.ExtraSizeScale = scale;
        snprintf(cfg.Name, sizeof cfg.Name, "%s", g == gb ? "game text font" : "game title font");
        ImFont *f = io.Fonts->AddFont(&cfg);
        return dst ? dst : f;
    };
    ImFont *body = add_game(gb, nullptr, 1.0f);
    add_ui_chain(body, gb->caps);
    body->FallbackChar = 0x25A1;
    ImFont *title = add_game(gt, nullptr, 1.0f);
    add_game(gb, title, gt->caps / gb->caps);
    add_ui_chain(title, gt->caps);
    title->FallbackChar = 0x25A1;
    s_f_body = body;
    s_f_title = title;
    s_caps_body = gb->caps;
    s_caps_title = gt->caps;
}

bool overlay_core_is_ready()
{
    return s_core_ready;
}

bool overlay_core_init(bool vulkan)
{
    if (s_core_ready)
        return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    /* Navigation is the overlay's own (see the top); ImGui is the renderer
     * and the mouse. */
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    /* The mouse cursor is the window loop's (fb_present_sdl.c): hidden in
     * play, shown while the overlay is open. ImGui's SDL backend would show
     * it on every frame it runs, a notice or the perf panel included. */
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    overlay_fonts();
    theme_pick();
    accent_style();
    bool ok = vulkan ? ImGui_ImplSDL2_InitForVulkan(s_win) : ImGui_ImplSDL2_InitForMetal(s_win);
    if (!ok) {
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
    /* No text field and no clipboard in the overlay. ImGui would call these
     * from the frame's end, on the present thread, and the IME one starts
     * and stops SDL's text input, an AppKit call on macOS (BO-210). */
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = nullptr;
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = nullptr;
    s_vulkan = vulkan;
    s_core_ready = true;
    s_last_ms = now_ms();
    return true;
}

/* ── the frame-time graph ──────────────────────────────────────────── */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* What reached the glass (present_cadence.h): looked at once per drawn
 * frame at most, shared by the graph and the Advanced page's lines. */
static pc_report s_glass;
static pc_status s_glass_st;
static pc_sample s_glass_smp[PC_RING];
static int s_glass_n;
static uint32_t s_glass_ms;

static void glass_update()
{
    uint32_t now = now_ms();
    if (s_glass_ms && now - s_glass_ms < 100)
        return;
    s_glass_ms = now ? now : 1;
    s_glass_n = xbox_PresentCadenceSnapshot(s_glass_smp, PC_RING, &s_glass_st);
    pc_analyze(s_glass_smp, s_glass_n, &s_glass_st.display, xbox_HostNowNs(), &s_glass);
}

/* "120 Hz fixed", "48-120 Hz VRR", "display rate unknown". */
static void glass_display_text(char *out, size_t n)
{
    const pc_display &d = s_glass_st.display;
    if (d.refresh_hz <= 0)
        snprintf(out, n, "display rate unknown");
    else if (d.vrr)
        snprintf(out, n, "%.0f-%.0f Hz VRR", d.min_hz, d.refresh_hz);
    else
        snprintf(out, n, "%.4g Hz fixed", d.refresh_hz);
}

static void glass_info()
{
    char disp[48];
    glass_update();
    glass_display_text(disp, sizeof disp);
    snprintf(s_glass_info[0], sizeof s_glass_info[0], "Display: %s. Presentation: %s%s", disp,
             pc_strategy_name(s_glass_st.strategy),
             s_glass_st.strategy == PC_LOCK ? " (the game's clock follows the display)" : "");
    if (!s_glass_st.measured) {
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On-glass times: not available (this graphics backend does not report them).");
        s_glass_info[2][0] = 0;
        return;
    }
    if (!s_glass.available) {
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On-glass times: not available right now (the window is hidden or covered).");
        s_glass_info[2][0] = 0;
        return;
    }
    if (s_glass.expected)
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On glass: a frame every %.2f ms (%.2f refreshes, should be %d); %d uneven of the last %d;"
                 " shown %.1f ms after its vblank (95%%: %.1f).",
                 s_glass.iv_p50_ms, s_glass.iv_p50_ms * s_glass_st.display.refresh_hz / 1000.0,
                 s_glass.expected, s_glass.uneven, s_glass.n_iv, s_glass.lat_p50_ms, s_glass.lat_p95_ms);
    else
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On glass: a frame every %.2f ms (%.2f to %.2f); %d uneven of the last %d;"
                 " shown %.1f ms after its vblank (95%%: %.1f).",
                 s_glass.iv_p50_ms, s_glass.iv_min_ms, s_glass.iv_max_ms, s_glass.uneven, s_glass.n_iv,
                 s_glass.lat_p50_ms, s_glass.lat_p95_ms);
    snprintf(s_glass_info[2], sizeof s_glass_info[2],
             "Since the start: %u frames skipped (no free drawable), %u late for their refresh%s. Source: %s.",
             s_glass_st.skips, s_glass_st.slips + s_glass_st.lates,
             s_glass.steps_max ? (s_glass.steps_multi ? "; some frames ran 2+ simulation steps"
                                                      : "; 1 simulation step per frame") : "",
             s_glass_st.source);
}

static void draw_perf(ImDrawList *dl, ImVec2 origin, float fs, float max_w = 0.0f, float max_h = 0.0f)
{
    uint64_t t[256];
    uint8_t asked[256];
    float iv[255], due[255];
    int n = xbox_HostFlipTimesIv(t, asked, 256), m = 0, i;
    uint64_t newest = 0;
    float fps = 0, last = 0, p50 = 0, p99 = 0, mx = 0;
    static uint32_t pres_prev, pres_ms;
    static float pres_rate;
    uint32_t now = now_ms();

    for (i = 1; i < n; i++)
        if (t[i] && t[i - 1]) {
            due[m] = 16.667f * (float)(asked[i] ? asked[i] : 1);
            iv[m++] = (float)((double)(t[i] - t[i - 1]) / 1e6);
        }
    for (i = 0; i < n; i++)
        if (t[i] > newest)
            newest = t[i];
    if (m) {
        float srt[255];
        int cnt = 0;
        /* The rate over the last second of flips. */
        for (i = n - 1; i >= 0 && t[i]; i--) {
            if (newest - t[i] > 1000000000ull)
                break;
            cnt++;
        }
        if (cnt > 1 && t[n - cnt] && newest > t[n - cnt])
            fps = (float)((double)(cnt - 1) * 1e9 / (double)(newest - t[n - cnt]));
        last = iv[m - 1];
        memcpy(srt, iv, (size_t)m * sizeof(float));
        qsort(srt, (size_t)m, sizeof(float), cmp_float);
        p50 = srt[m / 2];
        p99 = srt[(int)((m - 1) * 0.99)];
        mx = srt[m - 1];
    }
    if (now - pres_ms >= 1000) {
        uint32_t p = s_presents;
        pres_rate = (float)(p - pres_prev) * 1000.0f / (float)(now - pres_ms);
        pres_prev = p;
        pres_ms = now;
    }
    glass_update();
    const pc_report &g = s_glass;
    /* A presenter that records on-glass times has said so (its status);
     * without one the panel is the two lines and the bars it always was. */
    const bool have_glass = s_glass_st.source[0] != 0;
    const bool steps = have_glass && xbox_SettingValue(XS_ADVANCED_PERF_STEPS) != 0.0;

    /* Three lines of text: the title's flips, what reached the glass, and a
     * warning only when there is something to warn of. */
    char l1[128], l2[128], l3[160], l4[160], disp[48];
    glass_display_text(disp, sizeof disp);
    snprintf(l1, sizeof l1, "%.1f fps   %.1f ms", fps, last);
    snprintf(l2, sizeof l2, "p50 %.1f  p99 %.1f  max %.1f ms   window %.0f/s", p50, p99, mx, pres_rate);
    if (!s_glass_st.measured)
        snprintf(l3, sizeof l3, "on glass: not available (this graphics backend)   %s", disp);
    else if (!g.available)
        snprintf(l3, sizeof l3, "on glass: not available (window hidden?)   %s", disp);
    else if (g.expected)
        snprintf(l3, sizeof l3, "on glass %.1f ms = %.1f refr   %s   uneven %d   lag %.0f ms",
                 g.iv_p50_ms, g.iv_p50_ms * s_glass_st.display.refresh_hz / 1000.0, disp,
                 g.uneven, g.lat_p50_ms);
    else
        snprintf(l3, sizeof l3, "on glass %.1f ms (%.1f-%.1f)   %s   uneven %d   lag %.0f ms",
                 g.iv_p50_ms, g.iv_min_ms, g.iv_max_ms, disp, g.uneven, g.lat_p50_ms);
    l4[0] = 0;
    if (s_glass_st.measured && g.warn_rate)
        snprintf(l4, sizeof l4, "! %.4g Hz is not a multiple of 60: uneven frames",
                 s_glass_st.display.refresh_hz);
    else if (g.warn_uneven)
        snprintf(l4, sizeof l4, "! uneven on the glass: %d of %d frames", g.uneven, g.n_iv);
    else if (steps && g.steps_multi)
        snprintf(l4, sizeof l4, "%d frames ran 2+ simulation steps (dots)", g.steps_multi);

    /* The panel is as wide as its longest line: a fixed width left the stats
     * line running past the backdrop at larger values (a 100+ ms max). */
    if (!have_glass)
        l4[0] = 0;
    const int lines = !have_glass ? 2 : l4[0] ? 4 : 3;
    const char *const ls[4] = { l1, l2, l3, l4 };
    ImFont *const pf = ImGui::GetFont();
    float pad, lh, h, w;
    /* The box at text size `sz`; with max_w / max_h (the room the page's
     * header leaves) the text shrinks until the whole box fits. */
    for (int pass = 0; pass < 2; pass++) {
        pad = fs * 0.6f;
        lh = fs * 1.15f;
        h = fs * 4.1f + lh * lines;
        float tw = 0.0f;
        for (i = 0; i < lines; i++) {
            float w2 = pf->CalcTextSizeA(fs, FLT_MAX, 0.0f, ls[i]).x;
            if (w2 > tw) tw = w2;
        }
        w = tw + 2 * pad > fs * 15.0f ? tw + 2 * pad : fs * 15.0f;
        float k = 1.0f;
        if (max_w > 0.0f && w > max_w) k = max_w / w;
        if (max_h > 0.0f && h * k > max_h) k = max_h / h;
        if (pass || k >= 1.0f)
            break;
        fs *= k * 0.995f;
    }
    ImVec2 a = origin, b(origin.x + w, origin.y + h);
    dl->AddRectFilled(a, b, solid(T->perf_bg, 200 / 255.0f), fs * 0.5f);
    dl->AddText(pf, fs, ImVec2(a.x + pad, a.y + pad * 0.7f), solid(T->perf_text), l1);
    dl->AddText(pf, fs, ImVec2(a.x + pad, a.y + pad * 0.7f + lh), solid(T->perf_text2), l2);
    if (have_glass)
        dl->AddText(pf, fs, ImVec2(a.x + pad, a.y + pad * 0.7f + lh * 2),
                    solid(g.warn_uneven || g.warn_rate ? T->perf_warn : T->perf_text2), l3);
    if (l4[0])
        dl->AddText(pf, fs, ImVec2(a.x + pad, a.y + pad * 0.7f + lh * 3),
                    solid(g.warn_uneven || g.warn_rate ? T->perf_bad : T->perf_text2), l4);

    /* Bars: the last 120 flip intervals against a 50 ms ceiling; the thin
     * line across is one frame at 60 Hz. The colour is how late a flip was
     * against the interval the title asked for, not the interval itself: a
     * 33 ms frame where the title runs at 30 fps on purpose (its crash
     * camera) is on time, green; up to 0.8 ms over is still green, up to
     * half a 60 Hz frame yellow, more red. */
    const float gx = a.x + pad, gy1 = b.y - pad, gh = h - lh * lines - fs * 0.7f - pad, gw = w - 2 * pad;
    const int bars = 120;
    const float bw = gw / bars;
    for (i = 0; i < bars && i < m; i++) {
        float v = iv[m - 1 - i], late = v - due[m - 1 - i];
        float f = v / 50.0f;
        if (f > 1.0f) f = 1.0f;
        ImU32 c = late <= 0.833f ? solid(T->perf_good)
                : late <= 8.333f ? solid(T->perf_warn) : solid(T->perf_bad);
        float x1 = gx + gw - i * bw;
        dl->AddRectFilled(ImVec2(x1 - bw + 0.5f, gy1 - gh * f), ImVec2(x1, gy1), c);
    }
    float ly = gy1 - gh * (16.667f / 50.0f);
    dl->AddLine(ImVec2(gx, ly), ImVec2(gx + gw, ly), solid(T->perf_line, 90 / 255.0f));

    /* The second trace: the on-glass interval of the same frames, a line
     * over the bars on the same scale. Even frames draw it flat on the 60 Hz
     * line; a 1-3-1-3 repeat is a saw the bars do not have. An uneven
     * interval gets a mark. Nothing is drawn when the system gives no
     * on-glass times: a flat line there would say "even". */
    if (s_glass_st.measured && g.available && g.n_iv > 1) {
        ImU32 lc = solid(T->perf_text), bad = solid(T->perf_bad);
        ImVec2 prev(0, 0);
        bool have = false;
        for (i = 0; i < bars && i < g.n_iv; i++) {
            int k = g.n_iv - 1 - i;
            float v = g.iv_ms[k], f = v / 50.0f;
            if (f > 1.0f) f = 1.0f;
            ImVec2 pt(gx + gw - (i + 0.5f) * bw, gy1 - gh * f);
            if (have)
                dl->AddLine(prev, pt, lc, fs * 0.09f);
            if (g.iv_uneven[k])
                dl->AddCircleFilled(pt, fs * 0.16f, bad);
            prev = pt;
            have = true;
        }
    }
    /* The optional third: frames of more than one simulation step, a dot
     * under the bar (newest sample at the right, as the bars). */
    if (steps && g.steps_max) {
        for (i = 0; i < bars && i < s_glass_n; i++) {
            const pc_sample &sm = s_glass_smp[s_glass_n - 1 - i];
            if (sm.steps > 1)
                dl->AddCircleFilled(ImVec2(gx + gw - (i + 0.5f) * bw, gy1 + pad * 0.35f), fs * 0.13f,
                                    solid(T->perf_warn));
        }
    }
}

/* ── the pages ─────────────────────────────────────────────────────── */

static bool pad_hints() { return s_last_input_pad; }

/* ── the sizes ─────────────────────────────────────────────────────── */

/* The game's own proportions, measured on its menu frame (640x464): the
 * labels' caps are 3.7% of the screen's height, a row's pitch 6.25%, a hint
 * 2.6%, the footer's text 2.8%, the header's 4.7%; the labels start at 14%
 * of the width, the values are right-aligned in a column from 56% to 94%.
 * So every size here is a fraction of the window, with no upper cap (a 4K
 * window gets 4K text) and a floor for a tiny one. The overlay uses the
 * whole screen: it pauses the game anyway. A touch device (Android;
 * RECOMP_OVERLAY_TOUCH=1 to try it elsewhere) keeps a row at least 7 mm
 * high at the display's own density.
 *
 * The content is at most 1.8 H wide and centred (an ultrawide window keeps
 * the proportions); the rules and the footer bar span the window. */
struct Lay {
    float W, H;
    float cw, cx;                         /* the content's width and left edge */
    float label, hint, foot, head, tab, title;   /* text sizes (the game's cell; hint: ui's size) */
    float pitch;                          /* a row */
    float x_label, x_val0, x_val1;        /* the columns (screen x) */
    float y_rule, y_rows, y_foot;         /* the regions (screen y) */
    float body_x0, body_x1;               /* where a row's wash begins and ends */
};
static Lay LY;

static float display_dpi()
{
    const char *e = getenv("RECOMP_OVERLAY_DPI");
    float d = 0.0f;
    if (e && atof(e) > 0.0)
        return (float)atof(e);
    if (s_win) {
        int di = SDL_GetWindowDisplayIndex(s_win);
        if (di >= 0 && SDL_GetDisplayDPI(di, &d, NULL, NULL) == 0 && d > 0.0f)
            return d;
    }
    return 0.0f;
}

static bool touch_sizing()
{
#if defined(__ANDROID__)
    return true;
#else
    const char *e = getenv("RECOMP_OVERLAY_TOUCH");
    return e && e[0] == '1';
#endif
}

static void layout_compute(ImVec2 disp)
{
    const float W = disp.x, H = disp.y;
    /* The text follows the height; a window narrower than 1.3 : 1 follows
     * its width instead (the text would not fit a column). */
    const float ref = H < W / 1.3f ? H : W / 1.3f;
    LY.W = W;
    LY.H = H;
    LY.cw = W < H * 1.8f ? W : H * 1.8f;
    LY.cx = (W - LY.cw) * 0.5f;
    LY.label = fmaxf(0.037f * ref, 13.0f);
    LY.hint = fmaxf(0.026f * ref, 10.0f);
    LY.foot = fmaxf(0.032f * ref, 11.0f);
    LY.head = fmaxf(0.062f * ref, 18.0f);
    LY.tab = fmaxf(0.032f * ref, 11.0f);
    LY.title = fmaxf(0.105f * ref, 28.0f);
    LY.pitch = fmaxf(0.0625f * ref, LY.label * 1.5f);
    if (touch_sizing()) {
        float dpi = display_dpi();
        if (dpi <= 0.0f)
            dpi = 160.0f;
        const float mm7 = 7.0f * dpi / 25.4f;
        if (LY.pitch < mm7)
            LY.pitch = mm7;
    }
    LY.x_label = LY.cx + 0.14f * LY.cw;
    LY.x_val0 = LY.cx + 0.56f * LY.cw;
    LY.x_val1 = LY.cx + 0.94f * LY.cw;
    LY.y_rule = 0.19f * H;
    LY.y_rows = 0.265f * H;
    LY.y_foot = 0.87f * H;
    LY.body_x0 = LY.cx + 0.09f * LY.cw;
    LY.body_x1 = LY.cx + 0.97f * LY.cw;
    if (s_skinned) {
        /* The title's page: its header and footer, its rows' place and
         * pitch, from its 640 x 480 texels. */
        const xbox_overlay_skin *k = skin();
        const float sy = H / 480.0f;
        const SkinPic *hd = skin_pic(SK_HEADER), *ft = skin_pic(SK_FOOTER);
        if (hd)
            LY.y_rule = (hd->y + hd->h) * sy;
        if (ft)
            LY.y_foot = ft->y * sy;
        if (k->rows_y > 0.0f)
            LY.y_rows = k->rows_y * sy;
        if (k->row_pitch > 0.0f)
            LY.pitch = k->row_pitch * sy;
        LY.body_x0 = 0.0f;
        LY.body_x1 = W;
    }
}

/* ── text and shapes at an explicit size ───────────────────────────── */

/* What txt / tw / txt_fit draw with: a face (ui by default) and a shear,
 * set for a stretch with Face. The game's italics are its upright letters
 * leant at draw time (0.2: a fifth of the height); so are these, every
 * vertex moved right by its height over the baseline, the fallback's
 * letters too. */
static ImFont *s_cur;
static float s_shear;
struct Face {
    ImFont *pf;
    float ps;
    Face(ImFont *f, float shear = 0.0f) : pf(s_cur), ps(s_shear) { s_cur = f; s_shear = shear; }
    ~Face() { s_cur = pf; s_shear = ps; }
};
static ImFont *cur_font() { return s_cur ? s_cur : ImGui::GetFont(); }
#define SHEAR s_lean

static float tw(float size, const char *t)
{
    return cur_font()->CalcTextSizeA(size, FLT_MAX, 0.0f, t).x;
}

/* Leans what was drawn since vertex v0 around the baseline of text drawn at
 * y with `size`. */
static void lean(ImDrawList *dl, int v0, ImFont *f, float size, float y, float shear)
{
    if (shear == 0.0f)
        return;
    const float base = y + f->GetFontBaked(size)->Ascent;
    for (int i = v0; i < dl->VtxBuffer.Size; i++)
        dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * shear;
}

static void txt(ImDrawList *dl, float size, ImVec2 p, ImU32 c, const char *t, float wrap = 0.0f)
{
    ImFont *f = cur_font();
    const int v0 = dl->VtxBuffer.Size;
    if (s_skinned && skin()->outline_w > 0.0f && (f == s_f_body || f == s_f_title)) {
        /* The skin's outline: the text in its colour, eight ways round,
         * as transparent as the text. */
        static const float dx[8] = { -1, 1, -1, 1, 0, 0, -1, 1 }, dy[8] = { -1, -1, 1, 1, -1, 1, 0, 0 };
        const float o = fmaxf(1.0f, size * skin()->outline_w);
        const ImU32 oc = IM_COL32(T->outline.r, T->outline.g, T->outline.b, 0) | (c & IM_COL32_A_MASK);
        for (int k = 0; k < 8; k++)
            dl->AddText(f, size, ImVec2(p.x + dx[k] * o, p.y + dy[k] * o), oc, t, nullptr, wrap);
    }
    dl->AddText(f, size, p, c, t, nullptr, wrap);
    lean(dl, v0, f, size, p.y, s_shear);
}

/* The capitals' height over the size, by face. */
static float caps_of(ImFont *f)
{
    return f == s_f_title ? s_caps_title : f == s_f_body ? s_caps_body : k_ui_caps;
}

/* Where text of `size` starts (its top) for its capitals to be centred on
 * cy, in the current face. */
static float top_for(float cy, float size)
{
    ImFont *f = cur_font();
    return cy - (f->GetFontBaked(size)->Ascent - caps_of(f) * size * 0.5f);
}

/* The labels' capitals (ASCII; anything else as it is). */
static const char *caps(const char *t)
{
    static char buf[4][256];
    static int i;
    char *o = buf[i++ & 3];
    size_t k = 0;
    for (; t[k] && k < sizeof buf[0] - 1; k++)
        o[k] = (t[k] >= 'a' && t[k] <= 'z') ? (char)(t[k] - 32) : t[k];
    o[k] = 0;
    return o;
}

/* A text that has to fit `w`: a smaller size for it (down to 72%), and cut
 * at `w` when that is not enough. `right`: p.x is the right edge. Returns
 * the width it took. */
static float txt_fit(ImDrawList *dl, float size, ImVec2 p, ImU32 c, const char *t, float w, bool right = false,
                     bool centre = false)
{
    float x = tw(size, t), s = size;
    if (x > w && x > 0.0f) {
        s = size * (w / x);
        if (s < size * 0.72f)
            s = size * 0.72f;
        x = tw(s, t);
    }
    const float x0 = right ? p.x - (x < w ? x : w) : centre ? p.x - (x < w ? x : w) * 0.5f : p.x;
    const float y = p.y + (size - s) * 0.5f;
    dl->PushClipRect(ImVec2(x0, p.y - size * 0.2f), ImVec2(x0 + w + size * s_shear, p.y + size * 1.4f), true);
    txt(dl, s, ImVec2(x0, y), c, t);
    dl->PopClipRect();
    return x < w ? x : w;
}

/* A solid triangle pointing left (dir -1) or right (1), `r` high from the
 * centre. */
static void arrow_tri(ImDrawList *dl, float cx, float cy, float r, int dir, ImU32 c)
{
    dl->AddTriangleFilled(ImVec2(cx + dir * r * 0.7f, cy), ImVec2(cx - dir * r * 0.6f, cy - r),
                          ImVec2(cx - dir * r * 0.6f, cy + r), c);
}


/* ── the title's own pictures (overlay_gameart.h) ──────────────────── */

/* True once the sprites are in the texture Dear ImGui holds; the callers
 * draw their own shapes otherwise. */
static bool art_on() { return gameart_attach(); }

/* Sprite `sp` into the box p0..p1, white-tinted by `c`; flipped sideways or
 * upside down by swapping its coordinates. */
static void spr(ImDrawList *dl, int sp, ImVec2 p0, ImVec2 p1, ImU32 c, bool flip_x = false, bool flip_y = false)
{
    const ArtRect *r = gameart_sprite(sp);
    if (!r)
        return;
    const float u0 = flip_x ? r->u1 : r->u0, u1 = flip_x ? r->u0 : r->u1;
    const float v0 = flip_y ? r->v1 : r->v0, v1 = flip_y ? r->v0 : r->v1;
    dl->AddImage(gameart_tex(), p0, p1, ImVec2(u0, v0), ImVec2(u1, v1), c);
}

/* Sprite `sp` `k` pixels to a texel, its box centred on (cx, cy). */
static void spr_at(ImDrawList *dl, int sp, float cx, float cy, float k, ImU32 c, bool flip_x = false,
                   bool flip_y = false)
{
    const ArtRect *r = gameart_sprite(sp);
    if (!r)
        return;
    const float hw = r->w * k * 0.5f, hh = r->h * k * 0.5f;
    spr(dl, sp, ImVec2(floorf(cx - hw + 0.5f), floorf(cy - hh + 0.5f)), ImVec2(floorf(cx + hw + 0.5f), floorf(cy + hh + 0.5f)),
        c, flip_x, flip_y);
}

/* The page arrows and the value arrows: the title's left-pointing triangle
 * (dir -1), or its mirror (dir 1), `h` high; drawn shapes without it. */
static void arrow_spr(ImDrawList *dl, float cx, float cy, float h, int dir, ImU32 c, Rgb fallback)
{
    if (art_on()) {
        const ArtRect *r = gameart_sprite(AS_ARROW_LEFT);
        spr_at(dl, AS_ARROW_LEFT, cx, cy, h / r->h, c, dir > 0);
    } else {
        arrow_tri(dl, cx, cy, h * 0.4f, dir, col(fallback));
    }
}

/* The cursor before the selected row: the title's up triangle. */
static void cursor_spr(ImDrawList *dl, float cx, float cy, float w, ImU32 c, Rgb fallback)
{
    if (art_on()) {
        const ArtRect *r = gameart_sprite(AS_ARROW_UP);
        spr_at(dl, AS_ARROW_UP, cx, cy, w / r->w, c);
    } else {
        arrow_tri(dl, cx, cy, w * 0.3f, 1, col(fallback));
    }
}

/* A value box: a dark rounded box with a thin outline, its corners the
 * title's own (the small one for a small box), its sides plain. */
static void value_box(ImDrawList *dl, float x0, float y0, float x1, float y1, bool sel)
{
    x0 = floorf(x0 + 0.5f); y0 = floorf(y0 + 0.5f); x1 = floorf(x1 + 0.5f); y1 = floorf(y1 + 0.5f);
    const float bh = y1 - y0;
    const ImU32 fill = col(T->box_fill, 0.85f), edge = sel ? col(T->hint) : col(T->box_edge);
    float r = floorf(bh * 0.3f + 0.5f);
    if (!art_on()) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, r);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, r, 0, fmaxf(1.0f, bh * 0.04f));
        return;
    }
    const bool big = r > 40.0f;
    const int sf = big ? AS_FILL32 : AS_FILL16, se = big ? AS_EDGE32 : AS_EDGE16;
    const float t = fmaxf(1.0f, 2.0f * r / (big ? 32.0f : 16.0f));      /* the outline: 2 texels */
    dl->AddRectFilled(ImVec2(x0 + r, y0), ImVec2(x1 - r, y1), fill);
    dl->AddRectFilled(ImVec2(x0, y0 + r), ImVec2(x0 + r, y1 - r), fill);
    dl->AddRectFilled(ImVec2(x1 - r, y0 + r), ImVec2(x1, y1 - r), fill);
    dl->AddRectFilled(ImVec2(x0 + r, y0), ImVec2(x1 - r, y0 + t), edge);
    dl->AddRectFilled(ImVec2(x0 + r, y1 - t), ImVec2(x1 - r, y1), edge);
    dl->AddRectFilled(ImVec2(x0, y0 + r), ImVec2(x0 + t, y1 - r), edge);
    dl->AddRectFilled(ImVec2(x1 - t, y0 + r), ImVec2(x1, y1 - r), edge);
    for (int k = 0; k < 2; k++) {
        const int sp = k ? se : sf;
        const ImU32 c = k ? edge : fill;
        spr(dl, sp, ImVec2(x0, y0), ImVec2(x0 + r, y0 + r), c);
        spr(dl, sp, ImVec2(x1 - r, y0), ImVec2(x1, y0 + r), c, true);
        spr(dl, sp, ImVec2(x0, y1 - r), ImVec2(x0 + r, y1), c, false, true);
        spr(dl, sp, ImVec2(x1 - r, y1 - r), ImVec2(x1, y1), c, true, true);
    }
}

/* The title's tick bar in [x0, x1], `h` high at most, `on` of its ticks lit.
 * A tick is 4 x 8 texels at a pitch of 6; without the sprites, blocks. */
static void tick_bar(ImDrawList *dl, float x0, float x1, float cy, float h, double t, bool dim, bool sel)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (art_on() && gameart_sprite(AS_VOL_FULL) && gameart_sprite(AS_VOL_EMPTY)) {
        /* The title's own volume bar: its full cells (white on the selected
         * row, the text's pale blue on the others, as the pause menu's AUDIO
         * page draws them) up to the value, its empty blue outlines after. */
        const float k = h / 12.5f;                     /* pixels a texel: a cell 16 texels high */
        int n = (int)floorf((x1 - x0) / (6.0f * k));
        if (n < 4) n = 4;
        const int on = (int)floor(t * n + 0.5);
        const float pitch = (x1 - x0) / n;
        const ImU32 full = sel ? col({255, 255, 255}, dim ? 0.55f : 1.0f) : col(T->text, dim ? 0.5f : 1.0f);
        const ImU32 empty = col({255, 255, 255}, dim ? 0.45f : 0.9f);
        for (int i = 0; i < n; i++)
            spr_at(dl, i < on ? AS_VOL_FULL : AS_VOL_EMPTY, x0 + pitch * (i + 0.5f), cy, k, i < on ? full : empty);
        return;
    }
    if (!art_on()) {
        const float bh = h * 0.8f, by = cy - bh * 0.5f, seg = (x1 - x0) / 20.0f;
        const int on = (int)floor(t * 20 + 0.5);
        for (int k = 0; k < 20; k++)
            dl->AddRectFilled(ImVec2(x0 + seg * k, by), ImVec2(x0 + seg * (k + 1) - seg * 0.22f, by + bh),
                              k < on ? (dim ? col(T->fill_dim) : col(T->fill)) : col(T->track));
        return;
    }
    const float k = h / 8.0f;                          /* pixels a texel */
    int n = (int)floorf((x1 - x0) / (6.0f * k));
    if (n < 4) n = 4;
    const int on = (int)floor(t * n + 0.5);
    const float pitch = (x1 - x0) / n;                 /* spread over the bar */
    for (int i = 0; i < n; i++)
        spr_at(dl, AS_TICK, x0 + pitch * (i + 0.5f), cy, k,
               i < on ? col({255, 255, 255}, dim ? 0.55f : 1.0f) : col({0x30, 0x40, 0x50}, 0.9f));
}

/* A switch's state at the left of its value box, `h` high: the title's tick
 * in its blue disc for on, a ring for off. False without those pictures. */
static bool switch_icon(ImDrawList *dl, float x, float cy, float h, bool on, bool dim)
{
    if (!art_on() || !gameart_sprite(AS_CHECK) || !gameart_sprite(AS_RING))
        return false;
    const ArtRect *r = gameart_sprite(on ? AS_CHECK : AS_RING);
    const float k = h / r->h;
    if (on)
        spr_at(dl, AS_CHECK, x + h * 0.5f, cy, k, col({255, 255, 255}, dim ? 0.5f : 1.0f));
    else
        spr_at(dl, AS_RING, x + h * 0.5f, cy, k * 0.82f, col(T->text_dim, dim ? 0.5f : 0.9f));
    return true;
}

/* A setting fixed elsewhere (the environment): the title's padlock, `h`
 * high, centred on (cx, cy), where the left arrow would be. */
static void padlock_spr(ImDrawList *dl, float cx, float cy, float h)
{
    if (!art_on() || !gameart_sprite(AS_PADLOCK))
        return;
    spr_at(dl, AS_PADLOCK, cx, cy, h / gameart_sprite(AS_PADLOCK)->h, col({255, 255, 255}));
}

/* The d-pad: four triangles from the title's two, `gh` high. */
static void dpad_glyph(ImDrawList *dl, float cx, float cy, float gh, ImU32 c)
{
    const float k = gh / 24.0f;
    const ArtRect *dn = gameart_sprite(AS_DPAD_DOWN), *rt = gameart_sprite(AS_DPAD_RIGHT);
    if (!dn || !rt)
        return;
    auto put = [&](int sp, float x0, float y0, float w, float h, bool fx, bool fy) {
        spr(dl, sp, ImVec2(floorf(cx + x0 * k + 0.5f), floorf(cy + y0 * k + 0.5f)),
            ImVec2(floorf(cx + (x0 + w) * k + 0.5f), floorf(cy + (y0 + h) * k + 0.5f)), c, fx, fy);
    };
    put(AS_DPAD_DOWN, -8, 0, dn->w, dn->h, false, false);
    put(AS_DPAD_DOWN, -8, -dn->h, dn->w, dn->h, false, true);
    put(AS_DPAD_RIGHT, 0, -8, rt->w, rt->h, false, false);
    put(AS_DPAD_RIGHT, -rt->w, -8, rt->w, rt->h, true, false);
}

/* A cell of the prompt sheet of the glyph set in use, `k` pixels a texel,
 * its left edge at x, centred on cy. Returns its width. */
static float pad_glyph(ImDrawList *dl, int set, int cell, float x, float cy, float k)
{
    ArtRect r;
    if (!gameart_pad_cell(set, cell, &r))
        return 0.0f;
    const float w = r.w * k, h = r.h * k;
    dl->AddImage(gameart_pad_tex(), ImVec2(floorf(x + 0.5f), floorf(cy - h * 0.5f + 0.5f)),
                 ImVec2(floorf(x + w + 0.5f), floorf(cy - h * 0.5f + 0.5f) + floorf(h + 0.5f)),
                 ImVec2(r.u0, r.v0), ImVec2(r.u1, r.v1), col({255, 255, 255}));
    return w;
}

/* A key of the keyboard: its name in a small box. */
static float key_width(float size, const char *name)
{
    Face f(s_f_ui);
    const float sz = size * 0.78f;
    return tw(sz, name) + sz;
}

static float key_cap(ImDrawList *dl, float x, float cy, float size, const char *name, ImU32 c)
{
    Face f(s_f_ui);
    const float sz = size * 0.78f, pad = sz * 0.5f, w = tw(sz, name) + 2.0f * pad, h = sz * 1.55f;
    dl->AddRectFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), col(T->box_fill, 0.85f), sz * 0.3f);
    dl->AddRect(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), c, sz * 0.3f, 0, fmaxf(1.0f, sz * 0.09f));
    txt(dl, sz, ImVec2(x + pad, top_for(cy, sz)), c, name);
    return w;
}

/* The arrow keys as one cluster, up over left, down and right, each a small
 * cap with a triangle: as high as a key_cap. */
static float arrows_gap(float size) { return fmaxf(1.0f, size * 0.78f * 0.07f); }
static float arrows_cell(float size) { return (size * 0.78f * 1.55f - arrows_gap(size)) * 0.5f; }
static float arrows_width(float size) { return 3.0f * arrows_cell(size) + 2.0f * arrows_gap(size); }

/* Returns its width. */
static float arrows_cap(ImDrawList *dl, float x, float cy, float size, ImU32 c)
{
    const float g = arrows_gap(size), s = arrows_cell(size);
    const float y0 = cy - (2.0f * s + g) * 0.5f, r = s * 0.28f, t = s * 0.26f, th = fmaxf(1.0f, size * 0.78f * 0.07f);
    auto one = [&](int col_i, int row_i, int dir) {     /* dir: 0 up, 1 left, 2 down, 3 right */
        const float bx = x + col_i * (s + g), by = y0 + row_i * (s + g), mx = bx + s * 0.5f, my = by + s * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s, by + s), col(T->box_fill, 0.85f), r);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + s, by + s), c, r, 0, th);
        const float a = t, b = t * 0.75f;
        ImVec2 p0, p1, p2;
        switch (dir) {
        case 0:  p0 = ImVec2(mx, my - a); p1 = ImVec2(mx - a, my + b); p2 = ImVec2(mx + a, my + b); break;
        case 1:  p0 = ImVec2(mx - a, my); p1 = ImVec2(mx + b, my - a); p2 = ImVec2(mx + b, my + a); break;
        case 2:  p0 = ImVec2(mx, my + a); p1 = ImVec2(mx + a, my - b); p2 = ImVec2(mx - a, my - b); break;
        default: p0 = ImVec2(mx + a, my); p1 = ImVec2(mx - b, my + a); p2 = ImVec2(mx - b, my - a); break;
        }
        dl->AddTriangleFilled(p0, p1, p2, c);
    };
    one(1, 0, 0);
    one(0, 1, 1);
    one(1, 1, 2);
    one(2, 1, 3);
    return arrows_width(size);
}

/* One key by its name: Arrows is the cluster, the others a labelled cap. */
static float key_by_name(ImDrawList *dl, float x, float cy, float size, const char *name, ImU32 c)
{
    if (!strcmp(name, "Arrows"))
        return arrows_cap(dl, x, cy, size, c);
    return key_cap(dl, x, cy, size, name, c);
}

static float key_name_width(float size, const char *name)
{
    return !strcmp(name, "Arrows") ? arrows_width(size) : key_width(size, name);
}

/* The footer's key hints as caps. `keys` is groups of "Key  word" (two
 * spaces) apart by three or more; a key may be "Enter or Esc". Draws it from
 * x when it fits in w and returns true; else draws nothing and returns false
 * (the caller writes the text). */
static bool key_hints(ImDrawList *dl, float x, float cy, float size, ImU32 c, const char *keys, float w)
{
    struct Part { char key[48], word[96]; };
    Part parts[10];
    int n = 0;
    const char *p = keys;
    while (*p && n < 10) {
        const char *e = p;
        while (*e && !(e[0] == ' ' && e[1] == ' ' && e[2] == ' '))
            e++;
        const char *k = p;
        while (k < e && !(k[0] == ' ' && k[1] == ' '))
            k++;
        if (k >= e || (size_t)(k - p) >= sizeof parts[n].key || (size_t)(e - k - 2) >= sizeof parts[n].word)
            return false;
        memcpy(parts[n].key, p, (size_t)(k - p));
        parts[n].key[k - p] = 0;
        memcpy(parts[n].word, k + 2, (size_t)(e - k - 2));
        parts[n].word[e - k - 2] = 0;
        n++;
        while (*e == ' ')
            e++;
        p = e;
    }
    if (*p)
        return false;
    const float gap = size * 0.5f, sep = size * 1.4f;
    for (int pass = 0; pass < 2; pass++) {
        float cx = x;
        for (int i = 0; i < n; i++) {
            const char *k = parts[i].key;
            for (;;) {
                const char *o = strstr(k, " or ");
                char one[48];
                const size_t len = o ? (size_t)(o - k) : strlen(k);
                memcpy(one, k, len);
                one[len] = 0;
                if (pass == 0)
                    cx += key_name_width(size, one);
                else
                    cx += key_by_name(dl, cx, cy, size, one, c);
                if (!o)
                    break;
                Face body(s_f_body, SHEAR);
                if (pass == 1)
                    txt(dl, size, ImVec2(cx + gap, top_for(cy, size)), c, "or");
                cx += gap * 2.0f + tw(size, "or");
                k = o + 4;
            }
            Face body(s_f_body, SHEAR);
            if (pass == 1)
                txt(dl, size, ImVec2(cx + gap, top_for(cy, size)), c, parts[i].word);
            cx += gap + tw(size, parts[i].word) + sep;
        }
        if (pass == 0 && cx - sep - x > w)
            return false;
    }
    return true;
}

/* A small tag after a label: RESTART in attention yellow where the value
 * has changed since the game started (a restart applies it), in the
 * disabled grey where the setting is only read at start. Returns its width. */
static float restart_tag(ImDrawList *dl, float x, float cy, float label, bool changed)
{
    Face f(s_f_body, SHEAR);
    const float sz = label * 0.6f;
    txt(dl, sz, ImVec2(x, top_for(cy, sz)), changed ? col(T->attn) : col(T->text_dim), "RESTART");
    return tw(sz, "RESTART");
}

/* ── a title's own look (overlay_skin.h) ───────────────────────────── */

/* A skin picture at (x, y) of the title's 640 x 480 page, stretched with it. */
static void skin_picture(ImDrawList *dl, int which, float x, float y)
{
    const SkinPic *p = skin_pic(which);
    if (!p)
        return;
    const float sx = LY.W / 640.0f, sy = LY.H / 480.0f;
    dl->AddImage(p->tex, ImVec2(floorf(x * sx + 0.5f), floorf(y * sy + 0.5f)),
                 ImVec2(floorf((x + p->w) * sx + 0.5f), floorf((y + p->h) * sy + 0.5f)), ImVec2(0, 0), ImVec2(1, 1),
                 col({ 255, 255, 255 }));
}

/* An arrow of the skin (dir -1 left, 1 right) `h` high, centred on (cx, cy);
 * the drawn triangle without its picture. Returns its width. */
static float skin_arrow(ImDrawList *dl, int dir, float cx, float cy, float h)
{
    const SkinPic *p = skin_pic(dir < 0 ? SK_ARROW_L : SK_ARROW_R);
    if (!p) {
        arrow_tri(dl, cx, cy, h * 0.4f, dir, col(T->arrow));
        return h * 0.8f;
    }
    const float w = h * p->w / p->h;
    dl->AddImage(p->tex, ImVec2(cx - w * 0.5f, cy - h * 0.5f), ImVec2(cx + w * 0.5f, cy + h * 0.5f), ImVec2(0, 0),
                 ImVec2(1, 1), col({ 255, 255, 255 }));
    return w;
}

/* A selected row's colour in a skin: from white to `to` over flash_ms, then
 * again (the title's own menus flash their cursor so); `to` when steady. */
static ImU32 skin_flash(Rgb to)
{
    const uint32_t ms = skin()->flash_ms;
    if (!ms)
        return col(to);
    const float t = (float)(now_ms() % ms) / (float)ms;
    auto mix = [t](uint8_t a) { return (uint8_t)(255.0f + (a - 255.0f) * t + 0.5f); };
    return col({ mix(to.r), mix(to.g), mix(to.b) });
}

/* A value as the title writes its own: in capitals, but a unit straight
 * after a number stays as it is (720p, 16x, 480i). */
static const char *skin_value(const char *v)
{
    static char buf[2][128];
    static int i;
    char *o = buf[i++ & 1];
    size_t k = 0;
    int after_digit = 0;
    for (; v[k] && k < sizeof buf[0] - 1; k++) {
        const char c = v[k];
        const int lower = c >= 'a' && c <= 'z';
        o[k] = lower && !after_digit ? (char)(c - 32) : c;
        after_digit = (c >= '0' && c <= '9') || (after_digit && lower);
    }
    o[k] = 0;
    return o;
}

static void page_pick(int page);

/* The header of a skin: its picture, the title's name for the page (its
 * picture, or the word in the title colour), and the pages as a row of
 * names between the arrows, the open one in the hint colour. A click on a
 * name opens it, one on an arrow turns the page. */
static void draw_header_skin(ImDrawList *dl)
{
    const xbox_overlay_skin *k = skin();
    const SkinPic *hd = skin_pic(SK_HEADER), *ti = skin_pic(SK_TITLE);
    if (hd)
        skin_picture(dl, SK_HEADER, hd->x, hd->y);
    if (ti)
        skin_picture(dl, SK_TITLE, ti->x, ti->y);
    else {
        Face title(s_f_title, SHEAR);
        txt(dl, LY.head, ImVec2(LY.cx + 0.056f * LY.cw, top_for(LY.y_rule * 0.5f, LY.head)), col(T->title), "SETTINGS");
    }
    Face body(s_f_body, SHEAR);
    const float sz = LY.tab, cy = k->tabs_y * LY.H / 480.0f, gap = sz * 1.2f, ah = sz * 0.75f;
    float total = gap * (P_COUNT - 1);
    for (int i = 0; i < P_COUNT; i++)
        total += tw(sz, caps(k_page_name[i]));
    float x = floorf((LY.W - total) * 0.5f);
    for (int d = 0; d < 2; d++) {
        const float ax = d ? x + total + gap * 0.9f : x - gap * 0.9f;
        skin_arrow(dl, d ? 1 : -1, ax, cy, ah);
        ImGui::SetCursorScreenPos(ImVec2(ax - ah, cy - sz * 0.6f));
        ImGui::PushID(d + 100);
        ImGui::InvisibleButton("##pg", ImVec2(ah * 2.0f, sz * 1.2f));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            page_pick((s_page + (d ? 1 : -1) + P_COUNT) % P_COUNT);
        ImGui::PopID();
    }
    for (int i = 0; i < P_COUNT; i++) {
        const char *t = caps(k_page_name[i]);
        const float w = tw(sz, t);
        ImGui::SetCursorScreenPos(ImVec2(x - gap * 0.5f, cy - sz * 0.6f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##tab", ImVec2(w + gap, sz * 1.2f));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && s_page != i)
            page_pick(i);
        ImGui::PopID();
        txt(dl, sz, ImVec2(x, top_for(cy, sz)), i == s_page ? col(T->hint) : col(T->text_dim), t);
        x += w + gap;
    }
}

/* A row of a skin: its picture behind it, the label to the left of a colon
 * in the middle and the value after it, the arrows round the value of the
 * selected row, which is told by its colour. False for the rows it does not
 * draw (a cheat, the music folder, the speaker test): those keep the
 * overlay's own layout, in the skin's colours. */
static bool draw_row_skin(const Row &r, bool sel, float rh, float fs, ImDrawList *dl, float cy, int *click_dir)
{
    if (r.kind == R_FOLDER || r.kind == R_SPEAKERS
        || (r.kind == R_ACTION && r.action >= A_CHEAT0 && r.action <= A_CHEAT3 && strrchr(r.text, ':')))
        return false;
    const float mid = floorf(LY.W * 0.5f), sy = LY.H / 480.0f, cg = fs * 0.75f;
    const SkinPic *pl = skin_pic(SK_ROW);
    if (pl && r.kind != R_INFO)
        skin_picture(dl, SK_ROW, 320.0f - pl->w * 0.5f, cy / sy - pl->h * 0.5f);
    Face body(s_f_body, SHEAR);
    if (r.kind == R_INFO) {
        txt_fit(dl, LY.hint, ImVec2(mid, top_for(cy, LY.hint)), r.action ? col(T->attn) : col(T->hint), r.text,
                0.8f * LY.W, false, true);
        return true;
    }
    const float ty = top_for(cy, fs);
    const float lw = mid - cg - 0.1f * LY.W;               /* the label's room, left of the colon */
    if (r.kind == R_ACTION) {
        const bool pin = r.action == A_RESTART_PIN;
        const ImU32 c = pin ? col(T->attn) : sel ? skin_flash(T->text_hi) : col(T->text);
        if (s_confirm && r.action == s_confirm) {
            /* Asked once more: "LABEL : the question". */
            txt_fit(dl, fs, ImVec2(mid - cg, ty), c, caps(r.text), lw, true);
            txt(dl, fs, ImVec2(mid - tw(fs, ":") * 0.5f, ty), c, ":");
            txt_fit(dl, fs, ImVec2(mid + cg, ty), col(T->attn), confirm_text(r.action), LY.W - mid - cg - 0.06f * LY.W);
        } else {
            txt_fit(dl, fs, ImVec2(mid, ty), c, caps(r.text), 0.8f * LY.W, false, true);
        }
        return true;
    }
    const xs_setting *s = xbox_SettingGet(r.id);
    const Meta &m = meta(r.id);
    char v[96];
    value_text(r.id, v, sizeof v);
    const bool dim = setting_inactive(r.id);
    const ImU32 c = dim ? col(T->text_dim) : sel ? skin_flash(T->text_hi) : col(T->text);
    const ImU32 vc = dim ? col(T->text_dim) : sel ? skin_flash(T->hint) : col(T->text);
    txt_fit(dl, fs, ImVec2(mid - cg, ty), c, caps(m.label), lw, true);
    txt(dl, fs, ImVec2(mid - tw(fs, ":") * 0.5f, ty), vc, ":");
    const float ah = fs * 0.6f;
    const bool arrows = sel && !dim;
    float x = mid + cg;
    if (arrows)
        x += skin_arrow(dl, -1, x + ah * 0.4f, cy, ah) + ah * 0.5f;
    const float vx = x;
    x += txt_fit(dl, fs, ImVec2(x, ty), vc, skin_value(v), 0.3f * LY.W);
    if (arrows)
        x += ah * 0.5f + skin_arrow(dl, 1, x + ah * 0.9f, cy, ah);
    if (s->flags & XS_RESTART)
        restart_tag(dl, x + fs * 0.6f, cy, fs, s->value != s->launch);
    /* A click left of the value's middle steps back, anywhere else on. */
    const float mx = ImGui::GetIO().MousePos.x;
    *click_dir = mx > mid && mx < (vx + x) * 0.5f ? -1 : 1;
    (void)rh;
    return true;
}

/* ── touch gestures in the lists ─────────────────────────────────── */

/* A press in a list is a tap until it moves 2.5 mm: then it scrolls the
 * list (up or down) or drags a slider (sideways), and clicks nothing. The
 * mouse is the same, so a mouse drags a list too. SDL makes mouse events of
 * the fingers, which is what ImGui reads. */
enum { G_NONE, G_PRESS, G_SCROLL, G_SIDEWAYS };
static int s_gest;
static ImVec2 s_gest_at;
static float display_dpi();

static void gesture_frame()
{
    ImGuiIO &io = ImGui::GetIO();
    const float dpi = display_dpi();
    const float th = dpi > 0.0f ? 2.5f * dpi / 25.4f : 0.012f * io.DisplaySize.y;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_gest = G_PRESS;
        s_gest_at = io.MousePos;
    } else if (s_gest == G_PRESS && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - s_gest_at.x, dy = io.MousePos.y - s_gest_at.y;
        if (fabsf(dy) > th && fabsf(dy) >= fabsf(dx))
            s_gest = G_SCROLL;
        else if (fabsf(dx) > th)
            s_gest = G_SIDEWAYS;
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        s_gest = G_NONE;
    }
}

/* The press so far is a tap (on its release: act). */
static bool gesture_tap() { return s_gest == G_PRESS; }

/* In a scrolling child, right after BeginChild: a vertical drag that began
 * in it scrolls it with the finger. */
static void gesture_scroll()
{
    if (s_gest != G_SCROLL || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;
    const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    if (s_gest_at.x < p.x || s_gest_at.x > p.x + sz.x || s_gest_at.y < p.y || s_gest_at.y > p.y + sz.y)
        return;
    ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
}

/* One row. `fs` is the label's size, `rh` the pitch; the columns are the
 * layout's. */
static void draw_row(const Row &r, int idx, bool sel, float width, float rh, float fs, ImDrawList *dl,
                     bool *clicked, int *click_dir, bool *hovered, double *drag_t)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (r.kind == R_INFO)
        rh *= 0.7f;
    ImGui::PushID(idx);
    /* A row acts on release, and only for a tap: a finger that went on to
     * scroll the list (or to drag a slider) clicks nothing. */
    *clicked = ImGui::InvisibleButton("##row", ImVec2(width, rh)) && gesture_tap();
    *hovered = ImGui::IsItemHovered();
    ImVec2 mp = ImGui::GetIO().MousePos;
    /* The value box's left half steps back, the rest (its right half, the
     * label) forward: a tap anywhere on a choice's row moves it on. */
    *click_dir = mp.x > LY.x_val0 && mp.x < (LY.x_val0 + LY.x_val1) * 0.5f ? -1 : 1;
    ImGui::PopID();

    const float cy = p.y + rh * 0.5f;
    Face body(s_f_body);
    const float ty = top_for(cy, fs);
    const float ar = fs * 0.3f;                  /* the arrows' half height */
    const float lx = LY.x_label, label_w = LY.x_val0 - lx - ar * 4.0f;
    const float vr = LY.x_val1;                   /* where a value box ends; the arrows are outside it */
    const float bh = fmaxf(rh * 0.74f, fs * 1.2f), bpad = bh * 0.4f, ah = fs * 0.75f;
    const float by0 = cy - bh * 0.5f, by1 = cy + bh * 0.5f, bmid = (LY.x_val0 + vr) * 0.5f;
    const float arl = LY.x_val0 - ah * 0.9f, arr = vr + ah * 0.9f;

    if (s_skinned && r.kind == R_INFO && draw_row_skin(r, sel, rh, fs, dl, cy, click_dir))
        return;
    if (r.kind == R_INFO) {
        Face ui(s_f_ui);
        txt_fit(dl, LY.hint, ImVec2(lx, top_for(cy, LY.hint)), r.action ? col(T->attn) : col(T->hint), r.text,
                LY.x_val1 - lx);
        return;
    }
    if (sel && r.kind == R_SETTING && r.id == XS_GRAPHICS_HDR_PEAK) {
        /* BO-110: the calibration patch, right of the value: a symbol at the
         * peak set here on a background as bright as the display goes. */
        float pk, pw, hw;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        /* BO-227: the symbol at the value set here, not the level in use
         * (that one stops at what the display grants now), so it does
         * disappear where the display clips. */
        const double set = xbox_SettingValue(XS_GRAPHICS_HDR_PEAK);
        void *tex = nv2a_host_hdr_patch(set >= 10.0 ? (float)set : pk);
        const float ph = rh * 2.2f, pwid = ph * 2.0f;
        float px = arr + ah * 1.5f;
        if (px + pwid > p.x + width)
            px = arl - ah * 1.5f - pwid;   /* no room right of the value: left of its arrow */
        if (tex && px + pwid <= p.x + width) {
            dl->AddImage((ImTextureID)tex, ImVec2(px, cy - ph * 0.5f), ImVec2(px + pwid, cy + ph * 0.5f));
            const ImVec2 fs2 = ImGui::GetIO().DisplayFramebufferScale;
            nv2a_host_hdr_patch_at(px * fs2.x, (cy - ph * 0.5f) * fs2.y, (px + pwid) * fs2.x,
                                   (cy + ph * 0.5f) * fs2.y);
        }
        else if (!tex && px + pwid <= p.x + width) {
            /* Vulkan, and Metal's PQ layer (BO-244): drawn by the presenter
             * over the overlay (past its white). */
            const ImVec2 sc = ImGui::GetIO().DisplayFramebufferScale;
            nv2a_host_hdr_pattern(0, px * sc.x, (cy - ph * 0.5f) * sc.y, (px + pwid) * sc.x,
                                  (cy + ph * 0.5f) * sc.y, set >= 10.0 ? (float)set : pk, 10000.0f);
        }
    }
    if (sel && r.kind == R_SETTING && r.id == XS_GRAPHICS_HDR_PAPER) {
        /* BO-110: paper white (left) beside the HUD's white (right), for
         * setting the two together (drawn by a presenter that can). */
        float pk, pw, hw;
        nv2a_host_hdr_levels(&pk, &pw, &hw);
        const float ph = rh * 2.2f, pwid = ph * 2.0f;
        float px = arr + ah * 1.5f;
        if (px + pwid > p.x + width)
            px = arl - ah * 1.5f - pwid;
        const ImVec2 sc = ImGui::GetIO().DisplayFramebufferScale;
        if (px + pwid <= p.x + width)
            nv2a_host_hdr_pattern(1, px * sc.x, (cy - ph * 0.5f) * sc.y, (px + pwid) * sc.x,
                                  (cy + ph * 0.5f) * sc.y, pw, hw);
    }
    if (s_skinned && draw_row_skin(r, sel, rh, fs, dl, cy, click_dir))
        return;
    if (sel) {
        if (art_on())
            cursor_spr(dl, lx - fs * 0.9f, cy, fs * 0.85f, col({255, 255, 255}), T->arrow);
        else {
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rh), col(T->sel_fill, T->sel_fill_a), fs * 0.2f);
            arrow_tri(dl, lx - fs * 0.9f, cy, ar, 1, col(T->arrow));
        }
    }
    if (r.kind == R_ACTION) {
        const bool pin = r.action == A_RESTART_PIN;
        if (r.action >= A_CHEAT0 && r.action <= A_CHEAT3 && strrchr(r.text, ':')) {
            /* A cheat: "Name: On" as a setting's row, the name a label and
             * the state a value (attention yellow while it is on); while
             * switching it on is asked, the question in the value's place. */
            const char *c = strrchr(r.text, ':');
            char name[120];
            snprintf(name, sizeof name, "%.*s", (int)(c - r.text), r.text);
            const char *v = c[1] == ' ' ? c + 2 : c + 1;
            {
                Face it(s_f_body, SHEAR);
                txt_fit(dl, fs, ImVec2(lx, ty), sel ? col(T->text_hi) : col(T->text), caps(name), label_w);
            }
            const bool on = !strcmp(v, "On");
            if (s_confirm && r.action == s_confirm)
                txt_fit(dl, fs, ImVec2(vr, ty), col(T->attn), confirm_text(r.action), vr - LY.x_val0, true);
            else {
                value_box(dl, LY.x_val0, by0, vr, by1, sel);
                switch_icon(dl, LY.x_val0 + bpad * 0.6f, cy, bh * 0.78f, on, false);
                txt_fit(dl, fs, ImVec2(bmid, ty), on ? col(T->attn) : sel ? col(T->text_hi) : col(T->text), v,
                        vr - LY.x_val0 - 2.0f * bpad, false, true);
            }
            return;
        }
        {
            Face it(s_f_body, SHEAR);
            txt_fit(dl, fs, ImVec2(lx, ty), pin ? col(T->attn) : sel ? col(T->text_hi) : col(T->action), caps(r.text),
                    pin ? LY.x_val1 - lx : label_w);
        }
        if (s_confirm && r.action == s_confirm) {
            /* Restart / Quit, asked once more: the question in the row's note. */
            const char *q = confirm_text(r.action);
            txt_fit(dl, fs, ImVec2(LY.x_val1, ty), col(T->attn), q, LY.x_val1 - LY.x_val0, true);
        }
        return;
    }
    if (r.kind == R_FOLDER) {
        char v[64];
        int locked = 0;
        xbox_SettingsText(MUSIC_KEY, &locked);
        music_value(v, sizeof v);
        const ImU32 lc = locked ? col(T->text_dim) : sel ? col(T->text_hi) : col(T->text);
        {
            Face it(s_f_body, SHEAR);
            txt_fit(dl, fs, ImVec2(lx, ty), lc, caps(r.text), label_w);
        }
        value_box(dl, LY.x_val0, by0, vr, by1, sel);
        txt_fit(dl, fs, ImVec2(bmid, ty), locked ? col(T->text_dim) : sel ? col(T->text_hi) : col(T->text), v,
                vr - LY.x_val0 - 2.0f * bpad, false, true);
        if (sel && !locked)
            arrow_spr(dl, arr, cy, ah, 1, col({255, 255, 255}), T->arrow);
        if (locked)
            padlock_spr(dl, LY.x_val0 - ah * 0.9f, cy, bh * 0.9f);
        return;
    }
    if (r.kind == R_SPEAKERS) {
        /* Six buttons in the value column; the one playing lit, the picked
         * one outlined while the row is selected. A click plays its button. */
        int via = 0, playing = mcpx_apu_speaker_test_playing(&via);
        const float gap = fs * 0.3f, bw = (LY.x_val1 - LY.x_val0 - 5.0f * gap) / 6.0f, bh = fs * 1.35f;
        float bx = LY.x_val0;
        const float by = cy - bh * 0.5f;
        {
            Face it(s_f_body, SHEAR);
            txt_fit(dl, fs, ImVec2(lx, ty), sel ? col(T->text_hi) : col(T->text), caps(r.text), label_w);
        }
        for (int k = 0; k < 6; k++, bx += bw + gap) {
            bool lit = playing == k && (via != 0) == (r.action == 0);
            ImVec2 a(bx, by), b(bx + bw, by + bh);
            dl->AddRectFilled(a, b, lit ? col(T->fill) : col(T->track), bh * 0.3f);
            if (sel && s_spk_sel[r.action] == k)
                dl->AddRect(a, b, col(T->text_hi), bh * 0.3f, 0, fs * 0.1f);
            float w = tw(fs * 0.8f, k_spk_short[k]);
            txt(dl, fs * 0.8f, ImVec2(bx + (bw - w) * 0.5f, top_for(cy, fs * 0.8f)), lit ? col(T->on_accent) : col(T->text),
                k_spk_short[k]);
            if (*clicked && mp.x >= a.x && mp.x < b.x + gap) {
                s_spk_sel[r.action] = k;
                *click_dir = 0;
            }
        }
        return;
    }

    const xs_setting *s = xbox_SettingGet(r.id);
    const Meta &m = meta(r.id);
    char v[96];
    const bool dim = setting_inactive(r.id);
    const ImU32 vc = dim ? col(T->text_dim) : sel ? col(T->text_hi) : col(T->text);
    value_text(r.id, v, sizeof v);
    {
        /* The label, and after it the tag for a setting read at start. */
        float tag_w = 0.0f;
        if (s->flags & XS_RESTART)
            tag_w = tw(fs * 0.6f, "RESTART") + fs * 0.6f;
        Face it(s_f_body, SHEAR);
        const float w = txt_fit(dl, fs, ImVec2(lx, ty), dim ? col(T->text_dim) : sel ? col(T->text_hi) : col(T->text),
                                caps(m.label), label_w - tag_w);
        if (s->flags & XS_RESTART)
            restart_tag(dl, lx + w + fs * 0.5f, cy, fs, s->value != s->launch);
    }

    value_box(dl, LY.x_val0, by0, vr, by1, sel);
    if (s->type == XS_INT || s->type == XS_FLOAT) {
        /* The title's tick bar and the number, in the box. */
        const float nw = tw(fs, "100%") + fs * 0.2f;
        const float bx0 = LY.x_val0 + bpad, bx1 = vr - bpad - nw - bpad * 0.6f;
        const float bw = bx1 - bx0;
        double t = (s->value - s->min) / (s->max - s->min);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        tick_bar(dl, bx0, bx1, cy, bh * 0.55f, t, dim, sel);
        txt(dl, fs, ImVec2(vr - bpad - tw(fs, v), ty), vc, v);
        /* A sideways drag along the row moves it; a tap on the bar sets it. */
        const float mx = ImGui::GetIO().MousePos.x;
        if (drag_t && ((ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive() && s_gest == G_SIDEWAYS)
                       || (*clicked && mx >= bx0 - fs * 0.5f && mx <= bx1 + fs * 0.5f))) {
            double f = (mx - bx0) / bw;
            *drag_t = f < 0 ? 0 : f > 1 ? 1 : f;
        }
    } else {
        /* A choice or a switch: its value, centred in the box (On / Off for a
         * switch, with the title's tick or a ring at the box's left). */
        if (s->type == XS_BOOL)
            switch_icon(dl, LY.x_val0 + bpad * 0.6f, cy, bh * 0.78f, s->value != 0.0, dim);
        txt_fit(dl, fs, ImVec2(bmid, ty), vc, v, vr - LY.x_val0 - 2.0f * bpad, false, true);
    }
    if (sel && !dim) {
        arrow_spr(dl, arl, cy, ah, -1, col({255, 255, 255}), T->arrow);
        arrow_spr(dl, arr, cy, ah, 1, col({255, 255, 255}), T->arrow);
    }
    if (dim)
        padlock_spr(dl, arl, cy, bh * 0.9f);
}

/* The footer bar: a band under the page with the keys, in the hint blue
 * (the attention yellow while the page asks for something). The pages have
 * the title's own prompts there (B, Y, the d-pad, A: the buttons of the pad
 * in use, or the keys of the keyboard) when its pictures are in; the text
 * of foot_set is what is drawn without them, and on the Mixer page and in
 * the folder browser. */
static const char *s_foot_keys;
static bool s_foot_attn, s_foot_prompts;

static void foot_set(const char *keys, bool attn = false)
{
    s_foot_keys = keys;
    s_foot_attn = attn;
    s_foot_prompts = false;
}

/* After foot_set: the page's footer is B Back, Y Default, d-pad Change, A Select. */
static void foot_prompts() { s_foot_prompts = true; }

static void draw_prompts(ImDrawList *dl)
{
    const bool pad = pad_hints();
    const int set = pad ? (int)xbox_InputGlyphSet() : 0;
    const float sz = LY.foot, cy = (LY.y_foot + LY.H) * 0.5f, k = sz * 1.25f / 30.0f, gap = sz * 0.55f;
    const ImU32 c = s_foot_attn ? col(T->attn) : col(T->hint);
    Face body(s_f_body, SHEAR);
    const float x0 = LY.cx + 0.056f * LY.cw, x1 = LY.cx + 0.944f * LY.cw;
    /* left to right: the glyph (or key) and its word */
    auto item = [&](float x, int cell, const char *key, const char *word) {
        float w;
        if (pad)
            w = pad_glyph(dl, set, cell, x, cy, k);
        else {
            Face up(s_f_ui);
            w = key_cap(dl, x, cy, sz, key, c);
        }
        txt(dl, sz, ImVec2(x + w + gap, top_for(cy, sz)), c, word);
        return x + w + gap + tw(sz, word);
    };
    item(x0, AP_B, "Esc", "Back");
    item(x0 + 0.20f * LY.cw, AP_Y, "Del", "Default");
    {
        const float x = x0 + 0.38f * LY.cw;
        float w;
        if (pad && set != XBOX_GLYPH_SET_KEYBOARD) {
            w = sz * 1.25f;
            dpad_glyph(dl, x + w * 0.5f, cy, sz * 1.25f, col({255, 255, 255}));
        } else {
            Face up(s_f_ui);
            w = arrows_cap(dl, x, cy, sz, c);
        }
        txt(dl, sz, ImVec2(x + w + gap, top_for(cy, sz)), c, "Change");
    }
    {
        /* Select is at the right: its word, then its glyph. */
        float w;
        if (pad) {
            ArtRect r;
            w = gameart_pad_cell(set, AP_A, &r) ? r.w * k : sz;
        } else {
            w = key_width(sz, "Enter");
        }
        const float ww = tw(sz, "Select");
        txt(dl, sz, ImVec2(x1 - w - gap - ww, top_for(cy, sz)), c, "Select");
        if (pad)
            pad_glyph(dl, set, AP_A, x1 - w, cy, k);
        else {
            Face up(s_f_ui);
            key_cap(dl, x1 - w, cy, sz, "Enter", c);
        }
    }
}

/* The footer of a skin: its picture, then the prompts (the pad's pictures
 * left to right from prompts_x; the keyboard's key caps, or the page's
 * keys as text, from the same place). */
static void draw_footer_skin(ImDrawList *dl)
{
    const xbox_overlay_skin *k = skin();
    const SkinPic *ft = skin_pic(SK_FOOTER);
    if (ft)
        skin_picture(dl, SK_FOOTER, ft->x, ft->y);
    const float sx = LY.W / 640.0f, sy = LY.H / 480.0f, cy = k->prompts_y * sy;
    float x = floorf(k->prompts_x * sx);
    if (s_foot_prompts && pad_hints() && skin_pic(SK_OK)) {
        static const int order[3] = { SK_OK, SK_BACK, SK_DEFAULT };
        for (int i : order) {
            const SkinPic *p = skin_pic(i);
            if (!p)
                continue;
            const float w = p->w * sx, h = p->h * sy;
            dl->AddImage(p->tex, ImVec2(x, floorf(cy - h * 0.5f)), ImVec2(x + w, floorf(cy - h * 0.5f) + h),
                         ImVec2(0, 0), ImVec2(1, 1), col({ 255, 255, 255 }));
            x += w + k->prompts_gap * sx;
        }
        return;
    }
    if (!s_foot_keys)
        return;
    Face body(s_f_body);
    const ImU32 c = s_foot_attn ? col(T->attn) : col(T->hint);
    if (!pad_hints() && key_hints(dl, x, cy, LY.foot, c, s_foot_keys, LY.W - 2.0f * x))
        return;
    txt_fit(dl, LY.foot, ImVec2(x, top_for(cy, LY.foot)), c, s_foot_keys, LY.W - 2.0f * x);
}

/* A skin's hint in lines: one at its size (80% when it is long), else as
 * many as it takes at 80%, greedily by words. Returns how many, the size in
 * *size. In the current face. */
#define SKIN_HINT_LINES 8
static int skin_hint_wrap(const char *hint, float *size, char lines[][512])
{
    const float wmax = 0.9f * LY.W, base = LY.label * 0.9f;
    float sz = base, w = tw(sz, hint);
    int n = 0;
    if (w > wmax)
        sz *= fmaxf(0.8f, wmax / w), w = tw(sz, hint);
    *size = sz;
    if (w <= wmax) {
        snprintf(lines[0], 512, "%s", hint);
        return 1;
    }
    const char *p = hint;
    while (*p && n < SKIN_HINT_LINES) {
        const char *end = p, *best = NULL;
        char t[512];
        for (;;) {
            const char *sp = strchr(end, ' ');
            const char *stop = sp ? sp : end + strlen(end);
            snprintf(t, sizeof t, "%.*s", (int)(stop - p), p);
            if (best && tw(sz, t) > wmax)
                break;
            best = stop;
            if (!sp)
                break;
            end = sp + 1;
        }
        snprintf(lines[n++], 512, "%.*s", (int)(best - p), p);
        p = *best ? best + 1 : best;
    }
    return n;
}

/* The room a skin's page keeps above its footer for this hint: none while
 * it fits the footer's band (two lines), else its lines. */
static float skin_hint_room(const char *hint)
{
    char lines[SKIN_HINT_LINES][512];
    float sz;
    Face body(s_f_body, SHEAR);
    const int n = hint[0] ? skin_hint_wrap(hint, &sz, lines) : 0;
    return n > 2 ? n * sz * 1.08f : 0.0f;
}

/* The hint of a skin: in its footer's band, centred, in one or two lines;
 * longer, above the footer (the page keeps it room: skin_hint_room). */
static void draw_hint_skin(ImDrawList *dl, const char *hint, bool locked)
{
    Face body(s_f_body, SHEAR);
    char lines[SKIN_HINT_LINES][512];
    float sz;
    const int n = skin_hint_wrap(hint, &sz, lines);
    const float cy = skin()->hint_y * LY.H / 480.0f, mid = LY.W * 0.5f, lh = sz * 1.08f;
    const ImU32 c = locked ? col(T->attn) : col(T->hint);
    for (int i = 0; i < n; i++) {
        const float y = n <= 2 ? cy + (i - (n - 1) * 0.5f) * lh
                               : LY.y_foot - 0.009f * LY.H - (n - i - 0.5f) * lh;
        txt_fit(dl, sz, ImVec2(mid, top_for(y, sz)), c, lines[i], 0.9f * LY.W, false, true);
    }
}

static void draw_footer(ImDrawList *dl)
{
    if (s_skinned) {
        draw_footer_skin(dl);
        return;
    }
    dl->AddRectFilled(ImVec2(0, LY.y_foot), ImVec2(LY.W, LY.H), col(T->bar, 0.94f));
    dl->AddLine(ImVec2(0, LY.y_foot), ImVec2(LY.W, LY.y_foot), col(T->rule_hi), fmaxf(1.0f, 0.0025f * LY.H));
    if (s_foot_prompts && art_on()) {
        draw_prompts(dl);
        return;
    }
    if (!s_foot_keys)
        return;
    Face body(s_f_body);
    const float cy = (LY.y_foot + LY.H) * 0.5f;
    /* On the keyboard the keys are caps (Arrows is the cluster of four). */
    if (!pad_hints() && key_hints(dl, LY.cx + 0.056f * LY.cw, cy, LY.foot, s_foot_attn ? col(T->attn) : col(T->hint),
                                  s_foot_keys, 0.888f * LY.cw))
        return;
    txt_fit(dl, LY.foot, ImVec2(LY.cx + 0.056f * LY.cw, top_for(cy, LY.foot)),
            s_foot_attn ? col(T->attn) : col(T->hint), s_foot_keys, 0.888f * LY.cw);
}

/* The grid behind the page: the title's 16 x 16 tile (a line along its top
 * and left) over the rows' region, a texel to the pixels of a 640 x 480
 * frame scaled to this one; faint. */
static void draw_grid(ImDrawList *dl)
{
    const ArtRect *g = gameart_sprite(AS_GRID);
    if (!g)
        return;
    const float tile = floorf(16.0f * LY.H / 480.0f + 0.5f), y0 = LY.y_rule, y1 = LY.y_foot;
    const ImU32 c = col(T->edge, 0.13f);
    dl->PushClipRect(ImVec2(0, y0), ImVec2(LY.W, y1), true);
    for (float y = y0; y < y1; y += tile)
        for (float x = 0; x < LY.W; x += tile)
            dl->AddImage(gameart_tex(), ImVec2(x, y), ImVec2(x + tile, y + tile), ImVec2(g->u0, g->v0),
                         ImVec2(g->u1, g->v1), c);
    dl->PopClipRect();
}

/* The big blue arc of the title's menus: its big_curve picture, from the
 * left edge under the header, as wide as half the height. */
static void draw_bigcurve(ImDrawList *dl)
{
    const float s = 0.52f * LY.H;
    spr(dl, AS_BIGCURVE, ImVec2(0, LY.y_rule + 0.0125f * LY.H), ImVec2(s, LY.y_rule + 0.0125f * LY.H + s),
        col({0xb4, 0xc0, 0xd2}, 0.9f));
}

/* The big blue arc of the game's menus (its big_curve picture), drawn as a
 * curve when the picture is not there: from the left edge low on the page, up and to the right
 * to the rows' cursor, thick at the edge and thin at the end; a fainter
 * second one under it. */
static void draw_arc(ImDrawList *dl)
{
    const float s = 0.52f * LY.H, x0 = LY.cx, y0 = LY.y_rule;
    auto band = [&](ImVec2 a, ImVec2 c1, ImVec2 c2, ImVec2 b, float w0, float w1, float alpha) {
        /* One outline (out along one side, back along the other), filled
         * as one polygon so its edges are smoothed and it has no seams. */
        const int n = 64;
        ImVec2 side[2][n + 1];
        for (int i = 0; i <= n; i++) {
            const float t = (float)i / n, u = 1.0f - t;
            const ImVec2 p(u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                           u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y);
            const ImVec2 d(3 * u * u * (c1.x - a.x) + 6 * u * t * (c2.x - c1.x) + 3 * t * t * (b.x - c2.x),
                           3 * u * u * (c1.y - a.y) + 6 * u * t * (c2.y - c1.y) + 3 * t * t * (b.y - c2.y));
            const float len = sqrtf(d.x * d.x + d.y * d.y) + 1e-6f;
            /* Thick at the edge, thinning faster toward the end. */
            const float w = (w0 + (w1 - w0) * (1.0f - u * u)) * 0.5f;
            side[0][i] = ImVec2(p.x - d.y / len * w, p.y + d.x / len * w);
            side[1][n - i] = ImVec2(p.x + d.y / len * w, p.y - d.x / len * w);
        }
        dl->AddConcavePolyFilled(&side[0][0], 2 * (n + 1), col(T->arc, alpha));
    };
    band(ImVec2(x0 - 0.02f * s, y0 + 0.80f * s), ImVec2(x0 + 0.30f * s, y0 + 0.62f * s),
         ImVec2(x0 + 0.62f * s, y0 + 0.30f * s), ImVec2(x0 + 0.80f * s, y0 + 0.08f * s),
         0.075f * s, 0.006f * s, 0.85f);
    band(ImVec2(x0 + 0.10f * s, y0 + 1.03f * s), ImVec2(x0 + 0.40f * s, y0 + 0.98f * s),
         ImVec2(x0 + 0.65f * s, y0 + 0.86f * s), ImVec2(x0 + 0.80f * s, y0 + 0.76f * s),
         0.03f * s, 0.004f * s, 0.45f);
}

/* The header, as the game's menus have it: the one big word in the title
 * font (red, a white outline, a dark shadow, leant), the page's name in
 * white italics on the right between its arrows, a rule at 19% of the
 * height, the pages as a row of names under it. */
static void rows_in_page()
{
    s_rows_t0 = now_ms();
    s_rows_step_ms = ROWS_STEP_PAGE_MS;
}

/* Another page, from the skin's header (as the header's own clicks do). */
static void page_pick(int page)
{
    s_page = page;
    menusnd_play(XBOX_OVERLAY_SND_PAGE);
    rows_in_page();
    s_scroll_to_sel = true;
    s_mx_open = false;
    s_confirm = 0;
}

/* A row's vertices from v0 on, where it is in its coming in: moved left and
 * faded. `slot` is its place among the rows in view (0 the top one). */
static void rows_in_apply(ImDrawList *dl, int v0, int slot)
{
    if (!s_rows_t0)
        return;
    const int32_t t = (int32_t)(now_ms() - s_rows_t0)
                    - (int32_t)((slot < ROWS_IN_SLOTS ? slot : ROWS_IN_SLOTS) * s_rows_step_ms);
    if (t >= (int32_t)ROWS_IN_MS)
        return;
    const float f = t <= 0 ? 0.0f : (float)t / ROWS_IN_MS;
    const float e = 1.0f - (1.0f - f) * (1.0f - f);     /* eases out */
    const float dx = s_skinned ? 0.0f : -0.12f * LY.W * (1.0f - e);   /* a skin's rows only fade in */
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert &v = dl->VtxBuffer[i];
        v.pos.x += dx;
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)(a * f + 0.5f) << IM_COL32_A_SHIFT);
    }
}

static void draw_header(ImDrawList *dl)
{
    if (s_skinned) {
        draw_header_skin(dl);
        return;
    }
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(LY.W, LY.y_rule), col(T->head_band, 0.55f));
    {
        Face title(s_f_title, SHEAR);
        const float sz = LY.title, x = LY.cx + 0.056f * LY.cw;
        const float y = top_for(0.105f * LY.H, sz);
        const float o = fmaxf(1.0f, sz * 0.022f);           /* the outline */
        txt(dl, sz, ImVec2(x + sz * 0.03f, y + sz * 0.04f), col(T->shadow, 0.85f), "SETTINGS");
        for (int k = 0; k < 8; k++) {
            static const float dx[8] = { -1, 1, -1, 1, 0, 0, -1, 1 }, dy[8] = { -1, -1, 1, 1, -1, 1, 0, 0 };
            txt(dl, sz, ImVec2(x + dx[k] * o, y + dy[k] * o), col(T->outline), "SETTINGS");
        }
        txt(dl, sz, ImVec2(x, y), col(T->title), "SETTINGS");
    }
    {
        /* The page: < NAME >, a click on an arrow turns the page. */
        Face body(s_f_body, SHEAR);
        const char *name = caps(k_page_name[s_page]);
        const float sz = LY.head, cy = 0.105f * LY.H;
        const float ar = sz * 0.22f, nw = tw(sz, name) + sz * SHEAR * 0.5f, right = LY.cx + 0.944f * LY.cw;
        const float xn = right - ar * 2.8f - nw;
        txt(dl, sz, ImVec2(xn, top_for(cy, sz)), col(T->text_hi), name);
        if (art_on()) {
            arrow_spr(dl, xn - ar * 1.6f, cy, sz * 0.62f, -1, col({255, 255, 255}), T->text_hi);
            arrow_spr(dl, right - ar * 0.6f, cy, sz * 0.62f, 1, col({255, 255, 255}), T->text_hi);
        } else {
            arrow_tri(dl, xn - ar * 1.6f, cy, ar, -1, col(T->text_hi));
            arrow_tri(dl, right - ar * 0.6f, cy, ar, 1, col(T->text_hi));
        }
        for (int k = 0; k < 2; k++) {
            const float x0 = k ? right - ar * 2.4f : xn - ar * 3.4f, x1 = k ? right + ar : xn - ar * 0.2f;
            ImGui::SetCursorScreenPos(ImVec2(x0, cy - sz * 0.6f));
            ImGui::PushID(k + 100);
            ImGui::InvisibleButton("##pg", ImVec2(x1 - x0, sz * 1.2f));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                s_page = (s_page + (k ? 1 : -1) + P_COUNT) % P_COUNT;
                menusnd_play(XBOX_OVERLAY_SND_PAGE);
                rows_in_page();
                s_scroll_to_sel = true;
                s_mx_open = false;
                s_confirm = 0;
            }
            ImGui::PopID();
        }
    }
    dl->AddLine(ImVec2(0, LY.y_rule), ImVec2(LY.W, LY.y_rule), col(T->rule_hi), fmaxf(1.0f, 0.0025f * LY.H));

    /* The pages: names in a row under the rule, leant capitals. */
    {
        Face body(s_f_body, SHEAR);
        const float th = LY.y_rows - LY.y_rule - 0.004f * LY.H, y0 = LY.y_rule + 0.002f * LY.H;
        const float gap = LY.tab * 0.9f, cy = y0 + th * 0.5f;
        float x = LY.cx + 0.094f * LY.cw;
        for (int i = 0; i < P_COUNT; i++) {
            const char *t = caps(k_page_name[i]);
            const float w = tw(LY.tab, t);
            ImGui::SetCursorScreenPos(ImVec2(x - gap * 0.5f, y0));
            ImGui::PushID(i);
            ImGui::InvisibleButton("##tab", ImVec2(w + gap, th));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                if (s_page != i) {
                    menusnd_play(XBOX_OVERLAY_SND_PAGE);
                    rows_in_page();
                }
                s_page = i;
                s_scroll_to_sel = true;
                s_mx_open = false;
                s_confirm = 0;
            }
            ImGui::PopID();
            const bool on = i == s_page;
            txt(dl, LY.tab, ImVec2(x, top_for(cy, LY.tab)), on ? col(T->text_hi) : col(T->tab_text), t);
            if (on) {
                /* The open page: its name bright, a bar under it in the
                 * selected row's red (the others are dimmed). */
                const float bt = fmaxf(2.0f, 0.005f * LY.H), by = y0 + th - bt;
                dl->AddRectFilled(ImVec2(floorf(x - gap * 0.2f), floorf(by)), ImVec2(floorf(x + w + gap * 0.2f), floorf(by + bt)),
                                  col(T->sel_bar));
            }
            x += w + gap;
        }
        Face up(s_f_body);
        const float right = LY.cx + 0.944f * LY.cw;
        if (art_on() && pad_hints()) {
            /* The pad's shoulder buttons, as the title's own prompts draw them. */
            const int set = (int)xbox_InputGlyphSet();
            const float k = LY.foot * 1.25f / 30.0f, gap = LY.foot * 0.35f;
            ArtRect l, r;
            if (gameart_pad_cell(set, AP_LB, &l) && gameart_pad_cell(set, AP_RB, &r)) {
                const float x = right - l.w * k - gap - r.w * k;
                pad_glyph(dl, set, AP_LB, x, cy, k);
                pad_glyph(dl, set, AP_RB, x + l.w * k + gap, cy, k);
            }
        } else if (art_on()) {
            /* Keys of the keyboard. */
            const float gap = LY.foot * 0.35f, w1 = key_width(LY.foot, "Q"), w2 = key_width(LY.foot, "E");
            const ImU32 c = col(T->hint);
            key_cap(dl, right - w1 - gap - w2, cy, LY.foot, "Q", c);
            key_cap(dl, right - w2, cy, LY.foot, "E", c);
        } else {
            const char *hint = pad_hints() ? "LB / RB" : "Q / E";
            const float hw = tw(LY.foot, hint);
            txt(dl, LY.foot, ImVec2(right - hw, top_for(cy, LY.foot)), col(T->hint), hint);
        }
    }
}

/* ── the pages ─ (rows) */

/* The folder browser, in the place of a page's rows: where it is, the rows
 * (a place or folder with the music found in and below it), the keys. */
static void build_browser(float fs, ImDrawList *dl)
{
    ImGuiIO &io = ImGui::GetIO();
    int n = xsb_rows();

    collect_steps();
    if (s_fb_sel >= n) s_fb_sel = n - 1;
    if (s_fb_sel < 0) s_fb_sel = 0;
    {
        const int was = s_fb_sel;
        while (s_steps[D_UP]-- > 0)   { s_fb_sel = (s_fb_sel + n - 1) % (n ? n : 1); s_fb_scroll = true; }
        while (s_steps[D_DOWN]-- > 0) { s_fb_sel = (s_fb_sel + 1) % (n ? n : 1); s_fb_scroll = true; }
        if (s_fb_sel != was)
            menusnd_play(XBOX_OVERLAY_SND_MOVE);
    }
    memset(s_steps, 0, sizeof s_steps);
    s_reset = 0;
    s_page_delta = 0;
    if (s_activate > 0) {
        s_activate = 0;
        browser_pick(s_fb_sel);
        if (!xsb_is_open()) {
            /* Chosen: nothing more is drawn this frame, and the header left
             * the cursor moved with no item after it, which ImGui reports. */
            ImGui::Dummy(ImVec2(0, 0));
            return;
        }
        n = xsb_rows();
    }

    /* Where: the folder, cut from the left when it is too long. */
    char where[1024];
    xsb_where(where, sizeof where);
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        const char *t = where[0] ? where
                      : s_fb_purpose == FB_XBOX ? "Import Xbox save: pick the save's folder"
                      : s_fb_purpose == FB_SAVE ? "Import save: pick an exported save's folder"
                      : "Custom music folder: choose where to look";
        char cut[1100];
        if (ImGui::CalcTextSize(t).x > wrap_w) {
            const char *q = t;
            do {
                q++;
                snprintf(cut, sizeof cut, "...%s", q);
            } while (*q && ImGui::CalcTextSize(cut).x > wrap_w);
            t = cut;
        }
        dl->AddText(ImVec2(p.x, p.y), col(T->text_hi), t);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + fs * 1.6f));
    }

    const char *note = s_fb_purpose == FB_XBOX
        ? "Pick the save folder from your Xbox (UDATA/4541005b/<save>) or a folder that holds it. It is signed again for this port and imported at the next start. For a .zip, use --import-save."
        : s_fb_purpose == FB_SAVE
        ? "An exported save is a dated folder with UDATA inside. It is imported at the next start."
        : "Every folder with music in it becomes a soundtrack in the game's own music menu.";
    const float note_h = ImGui::GetFont()->CalcTextSizeA(LY.hint, FLT_MAX, wrap_w, note).y;
    const float footer = 0.012f * LY.H + note_h + 0.008f * LY.H;
    const float rh = LY.pitch;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("folders", ImVec2(avail.x, avail.y - footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav);
    gesture_scroll();
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x - fs * 0.5f;
    int picked = -1;
    xsb_row r;
    for (int i = 0; xsb_row_at(i, &r); i++) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = i == s_fb_sel;
        ImGui::PushID(i);
        const bool tap = ImGui::InvisibleButton("##dir", ImVec2(rw, rh)) && gesture_tap();
        if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            s_fb_sel = i;
            s_last_input_pad = false;
        }
        if (tap)
            picked = i;
        ImGui::PopID();
        if (!ImGui::IsRectVisible(p, ImVec2(p.x + rw, p.y + rh)) && !(sel && s_fb_scroll))
            continue;
        if (sel) {
            cdl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), col(T->sel_fill, T->sel_fill_a), fs * 0.2f);
            arrow_tri(cdl, p.x + fs * 0.35f, p.y + rh * 0.5f, fs * 0.36f, 1, col(T->arrow));
        }
        const float ty = p.y + (rh - fs) * 0.5f;
        bool act = r.kind == XSB_USE || r.kind == XSB_OFF;
        char c[48];
        browser_count(r, c, sizeof c);
        float cw = ImGui::CalcTextSize(c).x;
        cdl->PushClipRect(p, ImVec2(p.x + rw - cw - fs * 1.6f, p.y + rh), true);
        cdl->AddText(ImVec2(p.x + fs * 0.8f, ty),
                     act ? col(T->action) : col(sel ? T->text_hi : T->text), r.label);
        cdl->PopClipRect();
        if (c[0])
            cdl->AddText(ImVec2(p.x + rw - fs * 0.8f - cw, ty),
                         r.songs > 0 ? col(T->text_hi) : col(T->text_dim), c);
        if (sel && s_fb_scroll) {
            ImGui::SetScrollHereY(0.5f);
            s_fb_scroll = false;
        }
    }
    ImGui::EndChild();
    if (picked >= 0) {
        s_fb_sel = picked;
        browser_pick(picked);
    }

    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x - 0.05f * LY.cw, p.y + 0.004f * LY.H), ImVec2(p.x + avail.x + 0.03f * LY.cw, p.y + 0.004f * LY.H),
                    col(T->rule));
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + avail.x, p.y + footer), true);
        dl->AddText(ImGui::GetFont(), LY.hint, ImVec2(p.x, p.y + 0.012f * LY.H), col(T->hint), note, nullptr, wrap_w);
        dl->PopClipRect();
        static char keys[160];
        snprintf(keys, sizeof keys, pad_hints() ? "D-pad  select      %s  open / choose      %s  back"
                                                : "Arrows  select      %s  open / choose      %s  back",
                 act_name(0), act_name(1));
        foot_set(keys);
    }
}

#include "overlay_mixer.inc"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

/* A row's hint, the text under the rows when it is selected; true when it
 * is a warning or a locked setting (drawn in the attention colour). */
static bool row_hint(const Row &r, char *hint, size_t n)
{
    bool locked_out = false;
    hint[0] = 0;
    if (r.kind == R_SETTING) {
        const xs_setting *s = xbox_SettingGet(r.id);
        locked_out = s->locked;
        if (!s->locked && setting_inactive(r.id)) {
            locked_out = true;
            snprintf(hint, n, "Choose a music folder first (Custom music folder, above). This starts the game on the custom soundtrack you last picked.");
        } else if (s->locked) {
            if (s->env == RENV_COUNT)
                snprintf(hint, n, "Set by the environment (RECOMP_PRESENT_PACING / RECOMP_PRESENT_SPIN_LEAD); unset it to change it here.");
            else
                snprintf(hint, n, "Set by the environment (%s); unset it to change it here.", recomp_env_name(s->env));
        } else {
            if (r.id == XS_POWER_MODE && xbox_PowerStockHidden(xbox_SettingsHost()))
                snprintf(hint, n, "Balanced sleeps, then spins for the last 8 ms of each frame (steady 60,"
                         " recommended). Battery saver only sleeps (coolest, with an occasional small dip)."
                         " Auto picks the spin as it goes: less while frames are steady, more when they come late."
                         " Busy-waiting is not offered here: it was no steadier and used more power"
                         " (for diagnosis: RECOMP_PRESENT_PACING=spin).");
            else
                snprintf(hint, n, "%s", meta(r.id).hint);
            if (r.id == XS_GRAPHICS_UI_SCALE) {
                /* The scaled pictures are made in the background and kept on
                 * disk: say so while that is going on. */
                unsigned done = 0, total = 0;
                nv2a_texscale_progress(&done, &total);
                if (xbox_SettingValue(XS_GRAPHICS_UI_SCALE) != 0 && done < total) {
                    char whole[1800];
                    snprintf(whole, sizeof whole, "Preparing sharper menus and HUD... %u of %u. %s",
                             done, total, hint);
                    snprintf(hint, n, "%s", whole);
                }
            }
            if (r.id == XS_GRAPHICS_TEXUP && xbox_SettingValue(XS_GRAPHICS_TEXUP) != 0) {
                /* BO-245: what it has done so far, after the help; the
                 * Deck's one factor said here, as it has no scale row. */
                struct nv2a_texup_stats st;
                size_t at = strlen(hint);
                nv2a_texup_stats(&st);
                if (nv2a_texup_max_scale() <= 2 && at < n)
                    at += (size_t)snprintf(hint + at, n - at, " On a Steam Deck it sharpens to 2x.");
                if (at < n && !st.upscaled && !st.queued)
                    snprintf(hint + at, n - at, " None sharpened yet.");
                else if (at < n)
                    snprintf(hint + at, n - at, " So far: %u sharpened (%.0f MB), %u waiting;"
                             " %llu read from disk, %llu made.", st.upscaled,
                             (double)st.resident_bytes / 1048576.0, st.queued,
                             (unsigned long long)st.from_disk, (unsigned long long)st.computed);
            }
            /* The table's flag: this one is read when the game starts. */
            if (s->flags & XS_RESTART) {
                size_t at = strlen(hint);
                snprintf(hint + at, n - at, "%sNeeds a restart.", at ? " " : "");
            }
        }
    } else if (r.kind == R_ACTION && r.action == A_UPDATE_CHECK) {
        snprintf(hint, n, "Asks GitHub whether a newer release of %s is out, with one anonymous"
                 " request sent when you press this row.", s_up_repo);
    } else if (r.kind == R_ACTION && r.action == A_UPDATE_OPEN) {
        snprintf(hint, n, "Opens %s in your web browser.", s_up_res.url);
    } else if (r.kind == R_ACTION && r.action == A_STEAM_ART) {
        snprintf(hint, n, "Adds Steam's library pictures to the Takedawn shortcut you already have, made from your own"
                 " game files. Pictures you put there yourself are kept as backup copies. Steam can stay open.");
    } else if (r.kind == R_ACTION && r.action >= A_SAVE_EXPORT
               && r.action <= A_SAVE_CANCEL) {
        const char *what =
            r.action == A_SAVE_EXPORT
            ? "Copies your save to a dated folder in"
            : r.action == A_SAVE_IMPORT
            ? "Imports the newest exported save from"
            : r.action == A_SAVE_BROWSE
            ? "Pick an exported save's dated folder, on a drive or card too. The usual place is"
            : r.action == A_SAVE_XBOX
            ? "Pick a Burnout 3 save copied from an original Xbox or from xemu (the folder with Profile 1 in it, or one that holds it). Your save goes to"
            : r.action == A_SAVE_RESTORE
            ? "Your save as it was before each game save, newest first: the latest 20, then one a"
              " day for a week and one a week for a month. Kept in"
            : "Cancels the waiting import. The exported folder stays in";
        const char *then =
            r.action == A_SAVE_EXPORT
            ? "To move the save to another machine, copy that folder into the same-named folder there."
            : r.action == A_SAVE_CANCEL ? ""
            : "It replaces your save at the next start, after the current one is copied aside."
              " The running game is not affected.";
        char hist[1024];
        recomp_save_history_dir(s_sio_dir, hist, sizeof hist);
        if (r.action == A_SAVE_XBOX)
            snprintf(hist, sizeof hist, "%s/save-before-import", s_sio_dir);
        snprintf(hint, n, "%s%s%s %s. %s", s_sio_msg, s_sio_msg[0] ? "  |  " : "", what,
                 r.action == A_SAVE_RESTORE || r.action == A_SAVE_XBOX ? hist : s_sio_dest, then);
    } else if (r.kind == R_ACTION && r.action == A_RESTART_PIN) {
        char why[400];
        restart_reason(why, sizeof why);
        snprintf(hint, n, "%s%s%s%sCloses the game and starts it again with the same options."
                 " Anything the game hasn't saved is lost, as when quitting.",
                 r.action == A_RESTART_PIN && s_confirm == A_RESTART_PIN ? confirm_text(A_RESTART_PIN) : "",
                 r.action == A_RESTART_PIN && s_confirm == A_RESTART_PIN ? ".  " : "",
                 why, why[0] ? " " : "");
    } else if (r.kind == R_ACTION && r.action >= A_CHEAT0 && r.action <= A_CHEAT3) {
        int i = r.action - A_CHEAT0;
        locked_out = true;                   /* the warning, in the attention colour */
        snprintf(hint, n, "%s%s%s", s_confirm == r.action ? confirm_text(r.action) : "",
                 s_confirm == r.action ? ".  " : "",
                 i < s_cheat_n && s_cheats[i].warning ? s_cheats[i].warning : "");
    } else if (r.kind == R_ACTION && r.action == A_CHEAT_KEEP) {
        snprintf(hint, n, "Puts what the game saved during the cheats into your save and returns to normal"
                 " saving. Your current save goes into the save history first.");
    } else if (r.kind == R_ACTION && r.action == A_CHEAT_DISCARD) {
        snprintf(hint, n, "Your save stays as it was before the cheats. The game still holds that progress,"
                 " so nothing you play is saved until you restart. A Restart row appears at the bottom.");
    } else if (r.kind == R_ACTION && r.action == A_QUIT) {
        snprintf(hint, n, "%s%sSame as closing the window: your settings are saved and the game ends."
                 " Anything the game hasn't saved is lost.",
                 s_confirm == A_QUIT ? confirm_text(A_QUIT) : "", s_confirm == A_QUIT ? ".  " : "");
    } else if (r.kind == R_SPEAKERS) {
        spk_hint(r.action, hint, n);
    } else if (r.kind == R_FOLDER) {
        int env = 0;
        xbox_SettingsText(MUSIC_KEY, &env);
        locked_out = env != 0;
        if (env)
            snprintf(hint, n, "Set by the environment (RECOMP_AUDIO_MUSIC_DIR or --music-dir);"
                     " unset it to change it here.");
        else if (s_mf_acc_state.load(std::memory_order_acquire) == 1)
            snprintf(hint, n, "Waiting for Android's answer: may the game read your music files?");
        else if (s_mf && s_mf->access)
            /* The hook asks for what the browser needs (Android's audio
             * permission) and says how it went in its line. */
            snprintf(hint, n, "%s%sYour own music (mp3, flac, ogg, wav) for the game's music menu."
                     " Pick a folder of songs, or a folder of folders for one soundtrack each."
                     " A opens the folder list.", s_mf_acc_line, s_mf_acc_line[0] ? "  |  " : "");
        else
#if defined(__ANDROID__)
            /* No access hook: the app reads only its own files folder,
             * which the browser lists first. */
            snprintf(hint, n, "Your own music (mp3, flac, ogg, wav) for the game's music menu."
                     " Pick a folder of songs, or a folder of folders for one soundtrack each. On Android,"
                     " copy music folders into Android/data/<this app>/files/Music (the app can read"
                     " only its own folder), then press %s and choose \"This app's Music folder\".", act_name(0));
#else
            snprintf(hint, n, "Your own music (mp3, flac, ogg, wav) for the game's music menu."
                     " Pick a folder of songs, or a folder of folders for one soundtrack each."
                     " %s opens the folder list.", act_name(0, true));
#endif
    } else if (r.kind == R_ACTION
               && (r.action == A_REPORT || r.action == A_REPORT_SHOT)) {
        snprintf(hint, n, "%s%sA zip in the reports folder beside the settings, with the logs of"
                 " this run and the last ones, crash reports, your settings and a system summary%s."
                 " It has no saves or game data, and your home folder shows as ~. Attach it to an issue.",
                 s_rep_done, s_rep_done[0] ? ". " : "",
                 r.action == A_REPORT_SHOT ? ", plus the frame behind this menu" : "");
    } else if (r.kind == R_ACTION && r.action == A_SHADERS && s_sb && s_sb->cost) {
        snprintf(hint, n, "%s The game is paused while it runs. Closing the settings stops it;"
                 " the next run picks up where it left off.", s_sb->cost);
    }
    return locked_out;
}

static void build_pages_inner()
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const ImVec2 disp = io.DisplaySize;

    const bool skinned = skin_frame();
    layout_compute(disp);
    if (skinned) {
        /* A skin's page hides the game, as the title's own screens do. */
        bg->AddRectFilled(ImVec2(0, 0), disp, col(T->panel));
    } else {
        /* Dim the game (it stays in sight, paused), less than before (BO-190):
         * the title's own pause menu leaves its frame bright and puts dark
         * plates where its text is. So a half dim, and a plate behind the rows
         * (the header has its band, the footer its bar); the arc over it. */
        bg->AddRectFilled(ImVec2(0, 0), disp, col(T->scrim, 0.5f));
        bg->AddRectFilled(ImVec2(LY.body_x0, LY.y_rows - 0.008f * LY.H), ImVec2(LY.body_x1, LY.y_foot),
                          col(T->head_band, 0.72f), 0.012f * LY.H, ImDrawFlags_RoundCornersTop);
        if (art_on()) {
            draw_grid(bg);
            draw_bigcurve(bg);
        } else {
            draw_arc(bg);
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, fmaxf(8.0f, 0.011f * LY.H));
    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float fs = LY.label;
    foot_set(NULL);

    draw_header(dl);
    music_access_poll();

    if (xsb_is_open() || s_mx_open) {
        /* The folder browser and the Mixer page: in a body under the header,
         * their own rows and notes as they were; the keys go to the footer. */
        ImGui::SetCursorScreenPos(ImVec2(LY.body_x0, LY.y_rows));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.05f * LY.cw, 0));
        ImGui::BeginChild("body", ImVec2(LY.body_x1 - LY.body_x0, LY.y_foot - LY.y_rows - 0.008f * LY.H),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        ImDrawList *bdl = ImGui::GetWindowDrawList();
        if (xsb_is_open())
            build_browser(fs, bdl);
        else
            build_mixer(fs, bdl);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        draw_footer(dl);
        ImGui::End();
        ImGui::PopStyleVar(6);
        return;
    }

    /* The rows. */
    Row rows[OVERLAY_ROWS];
    int n = page_rows(s_page, rows);
    int &sel = s_sel[s_page];
    if (sel < 0 || sel >= n || !selectable(rows[sel])) {
        sel = 0;
        while (sel < n && !selectable(rows[sel]))
            sel++;
    }

    /* Steps from the pad and keys. */
    collect_steps();
    auto move = [&](int dir) {
        for (int k = 0; k < n; k++) {
            sel = (sel + dir + n) % n;
            if (selectable(rows[sel]))
                break;
        }
        s_scroll_to_sel = true;
    };
    {
        const int was = sel;
        while (s_steps[D_UP]-- > 0) move(-1);
        while (s_steps[D_DOWN]-- > 0) move(1);
        if (sel != was)
            menusnd_play(XBOX_OVERLAY_SND_MOVE);
    }
    s_steps[D_UP] = s_steps[D_DOWN] = 0;
    if (s_confirm && !(rows[sel].kind == R_ACTION && rows[sel].action == s_confirm))
        s_confirm = 0;                    /* moved off the row that was asked */
    if (rows[sel].kind == R_SETTING) {
        while (s_steps[D_LEFT]-- > 0) adjust(rows[sel].id, -1);
        while (s_steps[D_RIGHT]-- > 0) adjust(rows[sel].id, 1);
    } else if (rows[sel].kind == R_SPEAKERS) {
        int &k = s_spk_sel[rows[sel].action];
        while (s_steps[D_LEFT]-- > 0) k = (k + 5) % 6;
        while (s_steps[D_RIGHT]-- > 0) k = (k + 1) % 6;
    }
    s_steps[D_LEFT] = s_steps[D_RIGHT] = 0;
    while (s_activate-- > 0) activate(rows[sel]);
    s_activate = 0;
    while (s_reset-- > 0)
        if (rows[sel].kind == R_SETTING) {
            xbox_SettingReset(rows[sel].id);
            changed();
        }
    s_reset = 0;
    /* The footer: the selected row's hint (wrapped), a restart note, the keys. */
    char hint[1800];
    const bool hint_locked = row_hint(rows[sel], hint, sizeof hint);
    /* The foot of the page, bottom up: the footer bar, the selected row's
     * hint, the pinned rows (Quit game, and Restart while one is pending),
     * and the scrolling rows in what is left above. */
    const int pins = pin_count(rows, n);
    const bool note_only = !recomp_restart_available() && (restart_wanted() || xbox_SettingsNeedRestart());
    const float pitch = LY.pitch, gap = 0.009f * LY.H;
    const float hx = LY.cx + 0.06f * LY.cw, hw = 0.88f * LY.cw;
    const float pins_h = pins * pitch + (note_only ? pitch * 0.75f : 0.0f) + (pins || note_only ? gap : 0.0f);
    /* The hint's band is as tall as the page's longest hint, so the rows
     * above it stay where they are while the selection moves (BO-167). It
     * only grows while the page is shown (a confirm or a message can make
     * a hint longer), so it never jumps back either. */
    char longest[sizeof hint];
    float longest_h = 0.0f;
    longest[0] = 0;
    for (int i = 0; i < n; i++) {
        char t[sizeof hint];
        if (!selectable(rows[i]))
            continue;
        if (i == sel)
            snprintf(t, sizeof t, "%s", hint);
        else
            row_hint(rows[i], t, sizeof t);
        const float h = t[0] ? ImGui::GetFont()->CalcTextSizeA(LY.hint, FLT_MAX, hw, t).y : 0.0f;
        if (h > longest_h) {
            longest_h = h;
            memcpy(longest, t, sizeof t);
        }
    }
    float hs = LY.hint, hint_h = 0.0f;
    for (int k = 0; longest[0] && k < 10; k++) {
        /* A long hint shrinks (to 62%) before it takes the rows' room. */
        hint_h = ImGui::GetFont()->CalcTextSizeA(hs, FLT_MAX, hw, longest).y;
        const float rows_h = LY.y_foot - gap - hint_h - gap - pins_h - LY.y_rows;
        if ((hint_h <= 0.13f * LY.H && rows_h >= pitch * 3.0f) || hs <= LY.hint * 0.62f)
            break;
        hs *= 0.92f;
    }
    {
        static int band_page = -1;
        static float band_w, band_hgt, band_h, band_hs;
        if (band_page != s_page || band_w != LY.W || band_hgt != LY.H || !s_open) {
            band_page = s_open ? s_page : -1;
            band_w = LY.W;
            band_hgt = LY.H;
            band_h = 0.0f;
            band_hs = hs;
        }
        band_h = fmaxf(band_h, hint_h);
        band_hs = fminf(band_hs, hs);
        hint_h = band_h;
        hs = band_hs;
    }
    if (s_skinned)
        hint_h = skin_hint_room(longest);   /* a skin's hint is in its footer's band, if it fits */
    const float y_hint = LY.y_foot - gap - hint_h;
    const float y_pins = y_hint - gap - pins_h;
    float rows_h = y_pins - LY.y_rows;
    if (rows_h < pitch * 1.5f)
        rows_h = pitch * 1.5f;

    ImGui::SetCursorScreenPos(ImVec2(LY.body_x0, LY.y_rows));
    ImGui::BeginChild("rows", ImVec2(LY.body_x1 - LY.body_x0, rows_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    gesture_scroll();
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x;
    auto rows_run = [&](int from, int to, ImDrawList *rdl) {
    for (int i = from; i < to; i++) {
        bool clicked, hov;
        int cdir;
        double drag = -1;
        const int v0 = rdl->VtxBuffer.Size;
        const float ry = ImGui::GetCursorScreenPos().y;
        draw_row(rows[i], i, i == sel, rw, pitch, fs, rdl, &clicked, &cdir, &hov, &drag);
        rows_in_apply(rdl, v0, ry < LY.y_rows ? 0 : (int)((ry - LY.y_rows) / pitch));
        if (hov && selectable(rows[i]) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            sel = i;
            s_last_input_pad = false;
        }
        if (rows[i].kind == R_SETTING) {
            const xs_setting *s = xbox_SettingGet(rows[i].id);
            if (drag >= 0 && !setting_inactive(rows[i].id)) {
                double v = s->min + drag * (s->max - s->min);
                if (s->type == XS_INT)
                    v = floor(v + 0.5);
                else
                    v = floor(v / meta(rows[i].id).step + 0.5) * meta(rows[i].id).step;
                sel = i;
                if (v != s->value) {
                    /* A finger along a slider: the change sound per step, at
                     * most as often as a held d-pad repeats it. */
                    static uint32_t snd_at;
                    xbox_SettingSet(rows[i].id, v);
                    changed();
                    if (now_ms() - snd_at >= 80) {
                        snd_at = now_ms();
                        menusnd_play(XBOX_OVERLAY_SND_CHANGE);
                    }
                }
            } else if (clicked && s->type != XS_INT && s->type != XS_FLOAT) {
                sel = i;
                if (s->type == XS_BOOL)
                    activate(rows[i]);
                else
                    adjust(rows[i].id, cdir);
            } else if (clicked) {
                sel = i;                  /* a tap beside a slider's bar, or a locked row */
                menusnd_play(XBOX_OVERLAY_SND_MOVE);
            }
        } else if (clicked && (rows[i].kind == R_ACTION || rows[i].kind == R_FOLDER)) {
            sel = i;
            activate(rows[i]);
        } else if (clicked && rows[i].kind == R_SPEAKERS) {
            sel = i;
            if (cdir == 0)              /* on a button */
                activate(rows[i]);
        }
        if (i == sel && s_scroll_to_sel) {
            ImGui::SetScrollHereY(0.5f);
            s_scroll_to_sel = false;
        }
    }
    };
    rows_run(0, n - pins, cdl);
    ImGui::EndChild();
    if (pins || note_only) {
        /* The foot of the page, always in view. */
        dl->AddLine(ImVec2(LY.body_x0, y_pins), ImVec2(LY.body_x1, y_pins), col(T->rule));
        float y = y_pins + gap * 0.5f;
        if (note_only) {
            /* No restart on this platform (or a build without it): the
             * changes wait for the game to be started again. */
            const int k = restart_count();
            char t[96];
            snprintf(t, sizeof t, "Restart the game to apply %d change%s", k > 0 ? k : 1, k > 1 ? "s" : "");
            Face it(s_f_body, SHEAR);
            txt_fit(dl, fs, ImVec2(LY.x_label, top_for(y + pitch * 0.375f, fs)), col(T->attn), caps(t),
                    LY.x_val1 - LY.x_label);
            y += pitch * 0.75f;
        }
        if (pins) {
            ImGui::SetCursorScreenPos(ImVec2(LY.body_x0, y));
            ImGui::PushID("foot");
            rows_run(n - pins, n, dl);
            ImGui::PopID();
        }
    }
    if (hint[0] && !s_skinned) {
        dl->PushClipRect(ImVec2(hx, y_hint), ImVec2(hx + hw, LY.y_foot), true);
        dl->AddText(ImGui::GetFont(), hs, ImVec2(hx, y_hint), hint_locked ? col(T->attn) : col(T->hint),
                    hint, nullptr, hw);
        dl->PopClipRect();
    }
    {
        static char keys[200];
        snprintf(keys, sizeof keys, pad_hints() ? "D-pad  select / change      %s  toggle      %s  default      %s  close"
                                                : "Arrows  select / change      %s  toggle      %s  default      %s  close",
                 act_name(0), act_name(3), act_name(1));
        foot_set(keys);
    }
    foot_prompts();
    draw_footer(dl);
    if (hint[0] && s_skinned)
        draw_hint_skin(dl, hint, hint_locked);
    ImGui::End();
    ImGui::PopStyleVar(6);

    if (s_rows_t0 && now_ms() - s_rows_t0 > ROWS_IN_SLOTS * s_rows_step_ms + ROWS_IN_MS)
        s_rows_t0 = 0;
    /* A page change shows from the next frame. */
    if (s_page_delta) {
        s_page = (s_page + (s_page_delta > 0 ? 1 : -1) + P_COUNT) % P_COUNT;
        s_page_delta = 0;
        menusnd_play(XBOX_OVERLAY_SND_PAGE);
        rows_in_page();
        s_confirm = 0;
        s_scroll_to_sel = true;
    }
}

/* The whole page at the label's size (what the browser and the Mixer page
 * draw without naming one). */
static void build_pages(float)
{
    gesture_frame();
    layout_compute(ImGui::GetIO().DisplaySize);
    ImGui::PushFont(nullptr, LY.label);
    build_pages_inner();
    ImGui::PopFont();
}

/* ── the on-screen controls (touch_pad.h) ──────────────────────────── */

static ImU32 touch_col(uint32_t rgb, float a)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (int)(a * 255.0f + 0.5f));
}

/* Under everything else of the overlay's, over the game. White rings on a
 * dark wash, the accent of the button's colour, brighter while held. */
static void draw_touch(ImDrawList *dl, ImVec2 disp)
{
    xbox_touch_draw c[XBOX_TOUCH_MAX_CONTROLS];
    const float op = xbox_InputTouchOpacity();
    ImFont *font = ImGui::GetFont();
    int n;

    static uint32_t insets_at;
    if (!insets_at || now_ms() - insets_at > 2000u) {
        insets_at = now_ms() | 1u;
        xbox_InputTouchReadInsets();
    }
    xbox_InputTouchSetScreen(disp.x, disp.y);
    xbox_InputTouchSetDpi(display_dpi());
    n = xbox_InputTouchDrawList(c, XBOX_TOUCH_MAX_CONTROLS);
    for (int i = 0; i < n; i++) {
        const xbox_touch_draw &d = c[i];
        const ImVec2 o(d.x, d.y);
        const ImU32 ring = touch_col(0xFFFFFF, op * (d.pressed ? 1.0f : 0.75f));
        const ImU32 wash = touch_col(0x000000, op * 0.45f);
        const float th = fmaxf(2.0f, d.r * 0.05f);
        if (d.kind == XBOX_TOUCH_KIND_STICK) {
            dl->AddCircleFilled(o, d.r, touch_col(0x000000, op * 0.30f), 48);
            dl->AddCircle(o, d.r, touch_col(0xFFFFFF, op * 0.55f), 48, th);
            /* Left and right marks: it steers. */
            const float a = d.r * 0.16f, e = d.r * 0.80f;
            dl->AddTriangleFilled(ImVec2(d.x - e, d.y), ImVec2(d.x - e + a, d.y - a), ImVec2(d.x - e + a, d.y + a),
                                  touch_col(0xFFFFFF, op * 0.5f));
            dl->AddTriangleFilled(ImVec2(d.x + e, d.y), ImVec2(d.x + e - a, d.y + a), ImVec2(d.x + e - a, d.y - a),
                                  touch_col(0xFFFFFF, op * 0.5f));
            dl->AddCircleFilled(ImVec2(d.kx, d.ky), d.r * 0.42f,
                                touch_col(0xFFFFFF, op * (d.pressed ? 0.75f : 0.45f)), 36);
            continue;
        }
        if (d.kind == XBOX_TOUCH_KIND_DPAD) {
            const float w = d.r * 0.34f, l = d.r;
            for (int k = 0; k < 4; k++) {
                const float ux = k == 0 ? -1.0f : k == 1 ? 1.0f : 0.0f;
                const float uy = k == 2 ? -1.0f : k == 3 ? 1.0f : 0.0f;
                const bool held = d.pressed && d.kx == ux && d.ky == uy;
                /* The arm from the centre square's edge out to l, 2w wide. */
                const float g0 = w * 1.05f;
                const ImVec2 a(ux ? (ux < 0 ? d.x - l : d.x + g0) : d.x - w, uy ? (uy < 0 ? d.y - l : d.y + g0) : d.y - w);
                const ImVec2 b(ux ? (ux < 0 ? d.x - g0 : d.x + l) : d.x + w, uy ? (uy < 0 ? d.y - g0 : d.y + l) : d.y + w);
                dl->AddRectFilled(a, b, held ? touch_col(0xFFFFFF, op * 0.6f) : wash, w * 0.35f);
                dl->AddRect(a, b, ring, w * 0.35f, 0, th);
                /* The arrow near the arm's end. */
                const float tx = d.x + ux * l * 0.68f, ty = d.y + uy * l * 0.68f, t = w * 0.45f;
                const ImU32 ac = touch_col(held ? 0x000000 : 0xFFFFFF, op * 0.9f);
                if (ux)
                    dl->AddTriangleFilled(ImVec2(tx + ux * t, ty), ImVec2(tx - ux * t * 0.6f, ty - t),
                                          ImVec2(tx - ux * t * 0.6f, ty + t), ac);
                else
                    dl->AddTriangleFilled(ImVec2(tx, ty + uy * t), ImVec2(tx + t, ty - uy * t * 0.6f),
                                          ImVec2(tx - t, ty - uy * t * 0.6f), ac);
            }
            continue;
        }
        /* A button. */
        dl->AddCircleFilled(o, d.r, d.pressed ? touch_col(d.rgb ? d.rgb : 0xFFFFFF, op * 0.65f) : wash, 40);
        dl->AddCircle(o, d.r, d.rgb ? touch_col(d.rgb, op) : ring, 40, th);
        const ImU32 fg = touch_col(0xFFFFFF, fminf(1.0f, op * 1.3f));
        if (d.icon == XBOX_TOUCH_ICON_PAUSE) {
            const float bw = d.r * 0.16f, bh = d.r * 0.45f, gap = d.r * 0.14f;
            dl->AddRectFilled(ImVec2(d.x - gap - bw, d.y - bh), ImVec2(d.x - gap, d.y + bh), fg);
            dl->AddRectFilled(ImVec2(d.x + gap, d.y - bh), ImVec2(d.x + gap + bw, d.y + bh), fg);
        } else if (d.icon == XBOX_TOUCH_ICON_MENU) {
            const float hw = d.r * 0.45f, lh = d.r * 0.10f;
            for (int k = -1; k <= 1; k++) {
                const float y = d.y + k * d.r * 0.30f;
                dl->AddRectFilled(ImVec2(d.x - hw, y - lh), ImVec2(d.x + hw, y + lh), fg, lh);
            }
        } else if (d.label) {
            /* As large as fits inside the circle. */
            float size = d.r * (strlen(d.label) <= 2 ? 0.80f : 0.42f);
            const float w1 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, d.label).x;
            if (w1 > d.r * 1.6f)
                size *= d.r * 1.6f / w1;
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, d.label);
            dl->AddText(font, size, ImVec2(d.x - ts.x * 0.5f, d.y - ts.y * 0.5f), fg, d.label);
        }
    }
}

static float s_tgt_w, s_tgt_h;

void overlay_set_target(uint32_t w, uint32_t h)
{
    s_tgt_w = (float)w;
    s_tgt_h = (float)h;
}

bool overlay_begin_frame()
{
    if (!s_core_ready)
        return false;
    uint32_t now = now_ms();
    float dt = (float)(now - s_last_ms) / 1000.0f;
    s_last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    s_alpha += (s_open ? 1.0f : -1.0f) * dt / 0.14f;
    s_alpha = s_alpha < 0 ? 0 : s_alpha > 1 ? 1 : s_alpha;
    bool perf = xbox_SettingValue(XS_ADVANCED_PERF) != 0.0;
    bool note = notice_on(), scripted = scripted_on(), sg = sg_banner_on();
    bool touch = touch_on() && !s_open && s_alpha <= 0.0f;
    if (!s_open && s_alpha <= 0.0f && !perf && !note && !scripted && !sg && !touch)
        return false;

    ImGuiIO &io = ImGui::GetIO();
    gamefont_attach();
    /* One text size for the window: 17 points at 720 high, up to 2.2x. */
    float scale = io.DisplaySize.y / 720.0f;
    scale = scale < 1.0f ? 1.0f : scale > 2.2f ? 2.2f : scale;
    ImGui::GetStyle().FontSizeBase = 17.0f * scale;

    /* ImGui's SDL half ran on the main thread (xbox_OverlayPlatformFrame);
     * not yet once: nothing to draw with. */
    if (!s_plat_ever)
        return false;
    s_plat_pending = false;
    io.DeltaTime = dt > 0.0f ? dt : 0.001f;
    /* The image drawn into, in pixels, for the window's points: a swapchain
     * smaller or larger than the window (Android's scaled swapchain) still
     * gets the whole picture, every control where the fingers are. */
    if (s_tgt_w > 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
        io.DisplayFramebufferScale = ImVec2(s_tgt_w / io.DisplaySize.x, s_tgt_h / io.DisplaySize.y);
    ImGui::NewFrame();
    const float fs = ImGui::GetFontSize();

    if (perf) {
        ImVec2 at(fs * 0.6f, fs * 0.6f);
        float max_w = 0.0f, max_h = 0.0f;
        if (s_open || s_alpha > 0.0f) {
            /* In the header's free room: between the big title and the
             * widest page name with its arrows, above the rule, so it never
             * covers the title, the page name, the tabs or the rows; the
             * text shrinks to fit. */
            layout_compute(io.DisplaySize);
            float x0, x1;
            {
                Face title(s_f_title);
                x0 = LY.cx + 0.056f * LY.cw + tw(LY.title, "SETTINGS") * 1.08f + 0.025f * LY.cw;
            }
            {
                Face body(s_f_body, SHEAR);
                const float sz = LY.head, ar = sz * 0.22f, right = LY.cx + 0.944f * LY.cw;
                float nw = 0.0f;
                for (int i = 0; i < P_COUNT; i++) {
                    const float w = tw(sz, caps(k_page_name[i])) + sz * SHEAR * 0.5f;
                    if (w > nw) nw = w;
                }
                x1 = right - ar * 2.8f - nw - ar * 3.4f - 0.025f * LY.cw;
            }
            const float m = 0.012f * LY.H;
            max_w = x1 - x0;
            max_h = LY.y_rule - 2.0f * m;
            at = ImVec2(x0, m);
        }
        draw_perf(ImGui::GetForegroundDrawList(), at, fs, max_w, max_h);
    }
    if (note) {
        /* Centred, low on the screen; the dots move so it is seen to live. */
        static const char *const dots[4] = { "", ".", "..", "..." };
        char text[64];
        nv2a_pcache_stat pst;
        char what[64];
        char widest[64];
        nv2a_pcache_stat_get(&pst);
        if (s_notice_hdr)
            snprintf(what, sizeof what, "Preparing HDR shaders");
        else if (pst.hold_total)        /* BO-208: the first build, counted */
            snprintf(what, sizeof what, "Preparing graphics %u/%u", pst.hold_done,
                     pst.hold_total);
        else
            snprintf(what, sizeof what, "Preparing shaders");
        snprintf(text, sizeof text, "%s%s", what, dots[(now / 300u) & 3u]);
        snprintf(widest, sizeof widest, "Preparing graphics %u/%u...", pst.hold_total,
                 pst.hold_total);
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float tw = ImGui::CalcTextSize(widest).x;
        const ImVec2 p((disp.x - tw) * 0.5f, disp.y * 0.72f);
        dl->AddRectFilled(ImVec2(p.x - fs, p.y - fs * 0.5f), ImVec2(p.x + tw + fs, p.y + fs * 1.5f),
                          solid(T->toast_bg, 235 / 255.0f), fs * 0.5f);
        dl->AddText(p, solid(T->toast_text), text);
    }
    if (scripted) {
        /* Top centre, out of the HUD's way. */
        static const char text[] = "Test script is driving the controller - press a button to take over";
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float tw = ImGui::CalcTextSize(text).x;
        const ImVec2 p((disp.x - tw) * 0.5f, fs * 1.2f);
        dl->AddRectFilled(ImVec2(p.x - fs, p.y - fs * 0.5f), ImVec2(p.x + tw + fs, p.y + fs * 1.5f),
                          solid(T->notice_bg, 235 / 255.0f), fs * 0.5f);
        dl->AddText(p, solid(T->notice_text), text);
    }
    if (sg) {
        /* Top centre, below where the script line goes; the red band. */
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float wrap = disp.x * 0.8f;
        const ImVec2 ts = ImGui::CalcTextSize(s_sg_text, NULL, false, wrap);
        const ImVec2 p((disp.x - ts.x) * 0.5f, fs * (scripted ? 3.4f : 1.2f));
        dl->AddRectFilled(ImVec2(p.x - fs, p.y - fs * 0.5f), ImVec2(p.x + ts.x + fs, p.y + ts.y + fs * 0.5f),
                          solid(T->notice_bg, 235 / 255.0f), fs * 0.5f);
        dl->AddText(ImGui::GetFont(), fs, p, solid(T->notice_text), s_sg_text, NULL, wrap);
    }
    if (touch)
        draw_touch(ImGui::GetBackgroundDrawList(), io.DisplaySize);
    if (s_open || s_alpha > 0.0f) {
        /* A closing overlay still draws, fading. */
        build_pages(fs);
    }
    return true;
}

ImDrawData *overlay_end_frame()
{
    ImGui::Render();
    return ImGui::GetDrawData();
}

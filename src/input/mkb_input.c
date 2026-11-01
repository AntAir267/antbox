/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad, on SDL hosts.
 * See mkb_input.h.
 *
 * Ported from LegendaryXP's MechAssault2-recomp (MIT), where it read Win32
 * raw input on a thread of its own. Here the game window's SDL events
 * arrive from the presenter's loop (mkb_sdl_event), on the window's thread, and
 * the mouse lock is SDL's relative mode, which hides the pointer and reports
 * every movement while it holds it; mkb_sdl_pump applies it on that thread.
 * The poll side (mkb_merge) is upstream's, unchanged in what it computes:
 *
 * The poll drains the accumulated deltas and turns them into a stick
 * deflection proportional to the mouse's speed -- counts per 10 ms,
 * averaged over `smooth` ms of real time. The title integrates deflection
 * over its own frame, so the total rotation comes out proportional to total
 * mouse travel at any frame rate and any poll rate. Small movements get a
 * floor of `dz` of full throw, because every console title applies its own
 * dead zone to the stick, and a mouse correction smaller than it would be
 * silently eaten. A title hook that turns its camera itself takes the counts
 * instead (mkb_mouse_look_take), with none of the stick's deadzone, ramp or
 * speed limit.
 */
#include "mkb_input.h"
#include "pad_glyphs.h"         /* xbox_InputSetActivePad */
#include "platform/recomp_exe_dir.h"

#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xbox_InputBlocked(void);    /* input_core.c: the settings overlay is open */

/* Internal key codes are Win32 virtual-key values, as upstream's were, so
 * the layout names and the prompt names mean the same on both hosts. */
enum {
    VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_RETURN_ = 0x0D, VK_CAPITAL_ = 0x14,
    VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20, VK_PRIOR_ = 0x21, VK_NEXT_ = 0x22,
    VK_END_ = 0x23, VK_HOME_ = 0x24, VK_LEFT_ = 0x25, VK_UP_ = 0x26,
    VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28, VK_INSERT_ = 0x2D, VK_DELETE_ = 0x2E,
    VK_NUMPAD0_ = 0x60, VK_F1_ = 0x70, VK_F8_ = 0x77, VK_F9_ = 0x78,
    VK_F12_ = 0x7B, VK_LSHIFT_ = 0xA0, VK_RSHIFT_ = 0xA1, VK_LCONTROL_ = 0xA2,
    VK_RCONTROL_ = 0xA3, VK_LMENU_ = 0xA4, VK_RMENU_ = 0xA5,
    VK_OEM_1_ = 0xBA, VK_OEM_PLUS_ = 0xBB, VK_OEM_COMMA_ = 0xBC,
    VK_OEM_MINUS_ = 0xBD, VK_OEM_PERIOD_ = 0xBE, VK_OEM_2_ = 0xBF,
    VK_OEM_3_ = 0xC0, VK_OEM_4_ = 0xDB, VK_OEM_5_ = 0xDC, VK_OEM_6_ = 0xDD,
    VK_OEM_7_ = 0xDE,
};

/* F9 locks and frees the mouse; it cannot be bound. F8 marks the log. */
#define MKB_TOGGLE_VK  VK_F9_
#define MKB_MARK_VK    VK_F8_

/* ---- bindings ----------------------------------------------------------- *
 *
 * Every key, mouse button and wheel direction maps to one pad control. The
 * built-in table is the layout this layer started with; a title's own layout
 * comes from mkb.ini ("bind.SPACE = A", one per line) or RECOMP_MKB_BIND
 * ("SPACE=A,MOUSE1=RT,..."), and as soon as one binding is given the built-in
 * table is dropped, so a layout file says everything it means. Mouse movement
 * always drives the aim stick (swap_sticks moves it). */
enum { ACT_NONE, ACT_BTN, ACT_ANALOG, ACT_AXIS };
typedef struct { unsigned char kind, idx; } MkbAction;

#define IN_MOUSE(n)    (256 + (n))      /* 0 left 1 right 2 middle 3 X1 4 X2 */
#define IN_WHEEL_UP    261
#define IN_WHEEL_DOWN  262
#define IN_MAX         263

/* Axis actions: left stick up/down/left/right, then the right stick. */
enum { AX_LUP, AX_LDOWN, AX_LLEFT, AX_LRIGHT, AX_RUP, AX_RDOWN, AX_RLEFT, AX_RRIGHT };

static MkbAction s_bind[IN_MAX];
static int s_bind_custom;               /* a layout replaced the built-in one */
static unsigned short s_bind_order[IN_MAX], s_bind_seq; /* layout order, for prompts */

#define MKB_MODES 4
static struct {
    char name[16];
    MkbAction act[IN_MAX];
    unsigned short order[IN_MAX];       /* 0: not bound in this mode */
} s_modes[MKB_MODES];
static int s_nmodes;
static volatile LONG s_mode_on;         /* bit per mode (mkb_modes_now) */

static const struct { const char *name; MkbAction act; } k_actions[] = {
    {"A", {ACT_ANALOG, XBOX_BUTTON_A}},      {"B", {ACT_ANALOG, XBOX_BUTTON_B}},
    {"X", {ACT_ANALOG, XBOX_BUTTON_X}},      {"Y", {ACT_ANALOG, XBOX_BUTTON_Y}},
    {"BLACK", {ACT_ANALOG, XBOX_BUTTON_BLACK}}, {"WHITE", {ACT_ANALOG, XBOX_BUTTON_WHITE}},
    {"LT", {ACT_ANALOG, XBOX_BUTTON_LTRIGGER}}, {"RT", {ACT_ANALOG, XBOX_BUTTON_RTRIGGER}},
    {"START", {ACT_BTN, 4}},  {"BACK", {ACT_BTN, 5}},
    {"LTHUMB", {ACT_BTN, 6}}, {"RTHUMB", {ACT_BTN, 7}},
    {"DPAD_UP", {ACT_BTN, 0}}, {"DPAD_DOWN", {ACT_BTN, 1}},
    {"DPAD_LEFT", {ACT_BTN, 2}}, {"DPAD_RIGHT", {ACT_BTN, 3}},
    {"MOVE_FORWARD", {ACT_AXIS, AX_LUP}}, {"MOVE_BACK", {ACT_AXIS, AX_LDOWN}},
    {"MOVE_LEFT", {ACT_AXIS, AX_LLEFT}},  {"MOVE_RIGHT", {ACT_AXIS, AX_LRIGHT}},
    {"LOOK_UP", {ACT_AXIS, AX_RUP}},      {"LOOK_DOWN", {ACT_AXIS, AX_RDOWN}},
    {"LOOK_LEFT", {ACT_AXIS, AX_RLEFT}},  {"LOOK_RIGHT", {ACT_AXIS, AX_RRIGHT}},
    {"NONE", {ACT_NONE, 0}},
};

static const struct { const char *name; int in; } k_inputs[] = {
    {"SPACE", VK_SPACE_}, {"ENTER", VK_RETURN_}, {"ESC", VK_ESCAPE_}, {"TAB", VK_TAB_},
    {"BACKSPACE", VK_BACK_}, {"CAPSLOCK", VK_CAPITAL_},
    {"LSHIFT", VK_LSHIFT_}, {"RSHIFT", VK_RSHIFT_}, {"LCTRL", VK_LCONTROL_},
    {"RCTRL", VK_RCONTROL_}, {"LALT", VK_LMENU_}, {"RALT", VK_RMENU_},
    {"UP", VK_UP_}, {"DOWN", VK_DOWN_}, {"LEFT", VK_LEFT_}, {"RIGHT", VK_RIGHT_},
    {"INSERT", VK_INSERT_}, {"DELETE", VK_DELETE_}, {"HOME", VK_HOME_}, {"END", VK_END_},
    {"PGUP", VK_PRIOR_}, {"PGDN", VK_NEXT_},
    {"COMMA", VK_OEM_COMMA_}, {"PERIOD", VK_OEM_PERIOD_}, {"SLASH", VK_OEM_2_},
    {"SEMICOLON", VK_OEM_1_}, {"QUOTE", VK_OEM_7_}, {"LBRACKET", VK_OEM_4_},
    {"RBRACKET", VK_OEM_6_}, {"BACKSLASH", VK_OEM_5_}, {"MINUS", VK_OEM_MINUS_},
    {"EQUALS", VK_OEM_PLUS_}, {"GRAVE", VK_OEM_3_},
    {"NUM0", VK_NUMPAD0_ + 0}, {"NUM1", VK_NUMPAD0_ + 1}, {"NUM2", VK_NUMPAD0_ + 2},
    {"NUM3", VK_NUMPAD0_ + 3}, {"NUM4", VK_NUMPAD0_ + 4}, {"NUM5", VK_NUMPAD0_ + 5},
    {"NUM6", VK_NUMPAD0_ + 6}, {"NUM7", VK_NUMPAD0_ + 7}, {"NUM8", VK_NUMPAD0_ + 8},
    {"NUM9", VK_NUMPAD0_ + 9},
    {"MOUSE1", IN_MOUSE(0)}, {"MOUSE2", IN_MOUSE(1)}, {"MOUSE3", IN_MOUSE(2)},
    {"MOUSE4", IN_MOUSE(3)}, {"MOUSE5", IN_MOUSE(4)},
    {"MOUSELEFT", IN_MOUSE(0)}, {"MOUSERIGHT", IN_MOUSE(1)}, {"MOUSEMIDDLE", IN_MOUSE(2)},
    {"WHEELUP", IN_WHEEL_UP}, {"WHEELDOWN", IN_WHEEL_DOWN},
};

static int mkb_parse_input(const char *s)
{
    size_t i;
    if (s[0] && !s[1]) {
        char c = (char)toupper((unsigned char)s[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12)
            return VK_F1_ + n - 1;
    }
    for (i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++)
        if (!SDL_strcasecmp(s, k_inputs[i].name))
            return k_inputs[i].in;
    return -1;
}

/* "INPUT=ACTION". Returns 1 when both names were understood. */
static int mkb_bind(const char *input, const char *action)
{
    int in = mkb_parse_input(input);
    size_t i;
    if (in < 0 || in == MKB_TOGGLE_VK)
        return 0;
    for (i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++)
        if (!SDL_strcasecmp(action, k_actions[i].name)) {
            if (!s_bind_custom) {           /* the first binding replaces them all */
                memset(s_bind, 0, sizeof s_bind);
                memset(s_bind_order, 0, sizeof s_bind_order);
                s_bind_custom = 1;
            }
            s_bind[in] = k_actions[i].act;
            s_bind_order[in] = ++s_bind_seq;
            return 1;
        }
    return 0;
}

/* "mode.<name>.<KEY> = <CONTROL>". Returns 1 when all three were understood. */
static int mkb_mode_bind(const char *spec, const char *action)
{
    const char *dot = strchr(spec, '.');
    size_t i, nl = dot ? (size_t)(dot - spec) : 0;
    int m, in;
    if (!dot || !nl || nl >= sizeof s_modes[0].name || (in = mkb_parse_input(dot + 1)) < 0
        || in == MKB_TOGGLE_VK)
        return 0;
    for (m = 0; m < s_nmodes; m++)
        if (strlen(s_modes[m].name) == nl && !SDL_strncasecmp(s_modes[m].name, spec, nl))
            break;
    if (m == s_nmodes) {
        if (s_nmodes == MKB_MODES)
            return 0;
        memcpy(s_modes[m].name, spec, nl);
        s_modes[m].name[nl] = 0;
        s_nmodes++;
    }
    for (i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++)
        if (!SDL_strcasecmp(action, k_actions[i].name)) {
            s_modes[m].act[in] = k_actions[i].act;
            s_modes[m].order[in] = ++s_bind_seq;
            return 1;
        }
    return 0;
}

static int mkb_mode_index(const char *mode)
{
    int m;
    for (m = 0; m < s_nmodes; m++)
        if (!SDL_strcasecmp(s_modes[m].name, mode))
            return m;
    return -1;
}

/* When each mode lapses (SDL_GetTicks64 ms; ~0 = until turned off, 0 = off). */
static volatile Uint64 s_mode_until[MKB_MODES];

void mkb_set_mode(const char *mode, int ms)
{
    int m;
    mkb_enabled();
    if ((m = mkb_mode_index(mode)) < 0)
        return;
    s_mode_until[m] = ms < 0 ? ~(Uint64)0 : ms == 0 ? 0 : SDL_GetTicks64() + (Uint64)ms;
}

/* The modes on now, a bit each; logs each change. */
static LONG mkb_modes_now(void)
{
    Uint64 now = SDL_GetTicks64();
    LONG on = 0, was;
    int m;
    for (m = 0; m < s_nmodes; m++)
        if (s_mode_until[m] && now < s_mode_until[m])
            on |= 1L << m;
    was = InterlockedExchange(&s_mode_on, on);
    for (m = 0; m < s_nmodes; m++)
        if (((was ^ on) >> m) & 1) {
            fprintf(stderr, "  [MKB] %s keys %s\n", s_modes[m].name, (on >> m & 1) ? "on" : "off");
            fflush(stderr);
        }
    return on;
}

/* The n-th input (layout order) bound to a pad control, by its layout name,
 * in a mode's own bindings or (mode NULL) the normal ones. */
static const char *mkb_binding_in(const char *mode, const char *control, int n)
{
    static char names[IN_MAX][12];
    const MkbAction *want = NULL, *tab = s_bind;
    const unsigned short *order = s_bind_order;
    size_t i;
    int in, best = -1, k, m;
    unsigned short last = 0;

    mkb_enabled();                      /* loads the layout */
    if (mode) {
        if ((m = mkb_mode_index(mode)) < 0)
            return NULL;
        tab = s_modes[m].act;
        order = s_modes[m].order;
    }
    for (i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++)
        if (!SDL_strcasecmp(control, k_actions[i].name))
            want = &k_actions[i].act;
    if (!want || want->kind == ACT_NONE)
        return NULL;
    for (k = 0; k <= n; k++) {
        best = -1;
        for (in = 0; in < IN_MAX; in++)
            if (tab[in].kind == want->kind && tab[in].idx == want->idx
                && order[in] > last
                && (best < 0 || order[in] < order[best]))
                best = in;
        if (best < 0)
            return NULL;
        last = order[best];
    }
    if (!names[best][0]) {
        if ((best >= 'A' && best <= 'Z') || (best >= '0' && best <= '9'))
            names[best][0] = (char)best;
        else if (best >= VK_F1_ && best <= VK_F12_)
            snprintf(names[best], sizeof names[best], "F%d", best - VK_F1_ + 1);
        else
            for (i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++)
                if (k_inputs[i].in == best) {
                    snprintf(names[best], sizeof names[best], "%s", k_inputs[i].name);
                    break;
                }
    }
    return names[best][0] ? names[best] : NULL;
}

const char *mkb_binding(const char *control, int n)
{
    return mkb_binding_in(NULL, control, n);
}

const char *mkb_mode_binding(const char *mode, const char *control, int n)
{
    return mkb_binding_in(mode, control, n);
}

static void mkb_default_bindings(void)
{
    static const char *def[] = {
        "W", "MOVE_FORWARD", "S", "MOVE_BACK", "A", "MOVE_LEFT", "D", "MOVE_RIGHT",
        "MOUSE1", "RT", "MOUSE2", "LT", "MOUSE3", "Y", "MOUSE4", "WHITE", "MOUSE5", "BLACK",
        "WHEELUP", "Y", "WHEELDOWN", "Y",
        "SPACE", "A", "R", "X", "E", "B", "Q", "WHITE", "F", "BLACK",
        "ENTER", "START", "ESC", "START", "TAB", "BACK",
        "LSHIFT", "LTHUMB", "LCTRL", "RTHUMB",
        "UP", "DPAD_UP", "DOWN", "DPAD_DOWN", "LEFT", "DPAD_LEFT", "RIGHT", "DPAD_RIGHT",
        NULL
    };
    int i;
    for (i = 0; def[i]; i += 2)
        mkb_bind(def[i], def[i + 1]);
    s_bind_custom = 0;                  /* a layout file still replaces these */
}

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts per 10 ms for a full throw   */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
    double smooth;      /* ms over which the mouse speed is averaged */
    int    click_capture; /* a click in the window locks the mouse too */
    int    auto_capture;  /* locked while a title hook takes direct look */
    double look_deg;    /* degrees per count, for a title's direct look */
    int    look_invert_y; /* direct look: mouse up looks down */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 20, 0, 0, 25.0, 0, 1, 0.05, 0 };

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "smooth")) {
        d = strtod(val, NULL); if (d >= 1.0 && d <= 500.0) s_cfg.smooth = d;
    } else if (!strcmp(key, "click_capture")) {
        s_cfg.click_capture = atoi(val) != 0;
    } else if (!strcmp(key, "auto_capture")) {
        s_cfg.auto_capture = atoi(val) != 0;
    } else if (!strcmp(key, "look_deg")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.look_deg = d;
    } else if (!strcmp(key, "look_invert_y")) {
        s_cfg.look_invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else if (!SDL_strncasecmp(key, "mode.", 5)) {
        if (!mkb_mode_bind(key + 5, val))
            fprintf(stderr, "  [MKB] unknown mode binding %s = %s\n", key + 5, val);
    } else if (!SDL_strncasecmp(key, "bind.", 5)) {
        if (!mkb_bind(key + 5, val))
            fprintf(stderr, "  [MKB] unknown binding %s = %s\n", key + 5, val);
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_SMOOTH", "smooth",
        "RECOMP_MKB_LOOK_DEG", "look_deg",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[1024];
    const char *ini;
    FILE *f;
    int i;

    mkb_default_bindings();
    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini)
        snprintf(path, sizeof path, "%s", ini);
    else if (recomp_exe_dir())
        snprintf(path, sizeof path, "%s/mkb.ini", recomp_exe_dir());   /* beside the executable */
    else
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            k = line;
            while (*k == ' ' || *k == '\t') k++;
            if (*k == '#' || *k == ';') continue;   /* comment */
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
    fprintf(stderr, "  [MKB] layout: %s\n", path);
}

/* RECOMP_MKB_BIND="SPACE=A,MOUSE1=RT": bindings without a file, applied
 * after it. */
static void mkb_env_bindings(void)
{
    const char *e = getenv("RECOMP_MKB_BIND");
    char buf[1024], *tok, *ctx = NULL;
    if (!e || !*e)
        return;
    snprintf(buf, sizeof buf, "%s", e);
    for (tok = SDL_strtokr(buf, ",;", &ctx); tok; tok = SDL_strtokr(NULL, ",;", &ctx)) {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = 0;
        if (!mkb_bind(tok, eq + 1))
            fprintf(stderr, "  [MKB] unknown binding %s = %s\n", tok, eq + 1);
    }
}

/* ---- the window's events ------------------------------------------------ */

/* Accumulators, written by the window's thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the window adds with
 * InterlockedExchangeAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
/* The mouse speed the aim stick shows, counts per ms (see mkb_merge). */
static double s_aim_vx, s_aim_vy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_mouse_btn;    /* 1 L, 2 R, 4 M, 8 X1, 16 X2: held */
static volatile LONG s_mouse_tap;    /* pressed since the last poll         */
static volatile unsigned char s_vk_down[256];
static volatile unsigned char s_vk_tap[256];   /* pressed since the last poll */
/* RECOMP_MKB_SCRIPT drives the layer instead of the real devices, for
 * unattended tests: real input is ignored and the focus check is skipped. */
static int s_scripted;

/* The game's window has the keyboard focus (SDL sends keys only to it, but
 * a lock must not outlive the focus). */
static volatile LONG s_focus;

/* The mouse is the game's only while locked: it starts free, F9 locks it,
 * and F9 again, Esc (the game pauses as well), the settings overlay or
 * leaving the window free it. With click_capture = 1 a left click inside
 * the window locks it too (that click does not fire). With auto_capture = 1
 * (the default) it locks by itself while the title is in gameplay -- a title
 * hook keeps taking direct look (mkb_mouse_look_take) -- and is handed back
 * when that stops: a menu, the pause screen, a cutscene. */
static volatile LONG s_captured;
static volatile LONG s_swallow_click;   /* the capturing click, until released */
static int s_auto_play;     /* auto_capture: gameplay as last seen */
static int s_auto_away;     /* ... the window was left since */

static void mkb_set_captured(LONG on, const char *why)
{
    if (InterlockedExchange(&s_captured, on) != on) {
        fprintf(stderr, "  [MKB] mouse %s (%s)\n", on ? "captured" : "released", why);
        fflush(stderr);
    }
}

/* F8 presses not yet taken by a title hook (mkb_take_mark), and in all. */
static volatile LONG s_marks_pending, s_marks_made;

int mkb_take_mark(void)
{
    return (int)InterlockedExchange(&s_marks_pending, 0);
}

int mkb_mouse_captured(void)
{
    return mkb_enabled() && s_captured;
}

static int sdl_key_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return (int)(k - SDLK_a) + 'A';
    if (k >= SDLK_0 && k <= SDLK_9) return (int)(k - SDLK_0) + '0';
    if (k >= SDLK_F1 && k <= SDLK_F12) return (int)(k - SDLK_F1) + VK_F1_;
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return (int)(k - SDLK_KP_1) + VK_NUMPAD0_ + 1;
    switch (k) {
    case SDLK_KP_0:         return VK_NUMPAD0_;
    case SDLK_SPACE:        return VK_SPACE_;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:     return VK_RETURN_;
    case SDLK_ESCAPE:       return VK_ESCAPE_;
    case SDLK_TAB:          return VK_TAB_;
    case SDLK_BACKSPACE:    return VK_BACK_;
    case SDLK_CAPSLOCK:     return VK_CAPITAL_;
    case SDLK_LSHIFT:       return VK_LSHIFT_;
    case SDLK_RSHIFT:       return VK_RSHIFT_;
    case SDLK_LCTRL:        return VK_LCONTROL_;
    case SDLK_RCTRL:        return VK_RCONTROL_;
    case SDLK_LALT:         return VK_LMENU_;
    case SDLK_RALT:         return VK_RMENU_;
    case SDLK_UP:           return VK_UP_;
    case SDLK_DOWN:         return VK_DOWN_;
    case SDLK_LEFT:         return VK_LEFT_;
    case SDLK_RIGHT:        return VK_RIGHT_;
    case SDLK_INSERT:       return VK_INSERT_;
    case SDLK_DELETE:       return VK_DELETE_;
    case SDLK_HOME:         return VK_HOME_;
    case SDLK_END:          return VK_END_;
    case SDLK_PAGEUP:       return VK_PRIOR_;
    case SDLK_PAGEDOWN:     return VK_NEXT_;
    case SDLK_COMMA:        return VK_OEM_COMMA_;
    case SDLK_PERIOD:       return VK_OEM_PERIOD_;
    case SDLK_SLASH:        return VK_OEM_2_;
    case SDLK_SEMICOLON:    return VK_OEM_1_;
    case SDLK_QUOTE:        return VK_OEM_7_;
    case SDLK_LEFTBRACKET:  return VK_OEM_4_;
    case SDLK_RIGHTBRACKET: return VK_OEM_6_;
    case SDLK_BACKSLASH:    return VK_OEM_5_;
    case SDLK_MINUS:        return VK_OEM_MINUS_;
    case SDLK_EQUALS:       return VK_OEM_PLUS_;
    case SDLK_BACKQUOTE:    return VK_OEM_3_;
    }
    return 0;
}

/* The mouse or a key was used: the keyboard is the device in use, for the
 * button prompts (pad_glyphs.c), as the fixed key map reports it. Once per
 * change, not per motion event. */
static void mkb_active(void)
{
    if (xbox_InputActivePadFamily() != XBOX_PAD_FAMILY_KEYBOARD)
        xbox_InputSetActivePad(XBOX_PAD_FAMILY_KEYBOARD, 0);
}

static void release_all(void)
{
    memset((void *)s_vk_down, 0, sizeof s_vk_down);
    InterlockedExchange(&s_mouse_btn, 0);
}

void mkb_sdl_event(const void *sdl_event)
{
    const SDL_Event *ev = (const SDL_Event *)sdl_event;
    int vk, bit;

    if (!ev || !mkb_enabled() || s_scripted)
        return;
    switch (ev->type) {
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            InterlockedExchange(&s_focus, 1);
        } else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
                   || ev->window.event == SDL_WINDOWEVENT_HIDDEN
                   || ev->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            InterlockedExchange(&s_focus, 0);
            if (s_captured)
                mkb_set_captured(0, "window left");
            s_auto_away = 1;
            release_all();
        }
        return;
    case SDL_MOUSEMOTION:
        /* Relative mode reports every movement, unclamped by the screen's
         * edge; outside it the pointer is the desktop's. */
        if (s_captured) {
            if (ev->motion.xrel) InterlockedExchangeAdd(&s_dx, ev->motion.xrel);
            if (ev->motion.yrel) InterlockedExchangeAdd(&s_dy, ev->motion.yrel);
            if (ev->motion.xrel || ev->motion.yrel)
                mkb_active();
        }
        return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        switch (ev->button.button) {
        case SDL_BUTTON_LEFT:   bit = 1;  break;
        case SDL_BUTTON_RIGHT:  bit = 2;  break;
        case SDL_BUTTON_MIDDLE: bit = 4;  break;
        case SDL_BUTTON_X1:     bit = 8;  break;
        case SDL_BUTTON_X2:     bit = 16; break;
        default: return;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            if (bit == 1 && s_cfg.click_capture && !s_captured && s_focus
                    && !xbox_InputBlocked()) {
                mkb_set_captured(1, "click in the window");
                InterlockedExchange(&s_swallow_click, 1);
            }
            /* A click that starts and ends between two polls still counts. */
            if (s_captured || s_cfg.click_capture)
                mkb_active();
            InterlockedOr(&s_mouse_tap, bit);
            InterlockedOr(&s_mouse_btn, bit);
        } else {
            if (bit == 1 && s_swallow_click) {
                InterlockedExchange(&s_swallow_click, 0);
                InterlockedAnd(&s_mouse_tap, ~1);
            }
            InterlockedAnd(&s_mouse_btn, ~bit);
        }
        return;
    case SDL_MOUSEWHEEL: {
        int y = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            y = -y;
        if (y)
            InterlockedExchangeAdd(&s_wheel, y);
        return;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        vk = sdl_key_to_vk(ev->key.keysym.sym);
        if (!vk)
            return;
        if (ev->type == SDL_KEYDOWN) {
            if (ev->key.repeat)
                return;
            if (vk == MKB_TOGGLE_VK && !xbox_InputBlocked())
                mkb_set_captured(!s_captured, "F9");
            /* F8 marks the moment in the log, for a player pointing at a bug. */
            if (vk == MKB_MARK_VK) {
                LONG n = InterlockedIncrement(&s_marks_made);
                InterlockedIncrement(&s_marks_pending);
                fprintf(stderr, "[MARK] F8 #%ld at %llu ms\n", (long)n,
                        (unsigned long long)SDL_GetTicks64());
                fflush(stderr);
            }
            /* Esc pauses the game and hands the pointer back with it. */
            if (vk == VK_ESCAPE_ && s_captured)
                mkb_set_captured(0, "Esc");
            /* Alt+Enter is the window's fullscreen toggle, not a menu press. */
            if (vk == VK_RETURN_ && (ev->key.keysym.mod & KMOD_ALT))
                return;
            s_vk_down[vk] = 1;
            s_vk_tap[vk] = 1;
            if (s_bind[vk].kind != ACT_NONE)
                mkb_active();
        } else {
            s_vk_down[vk] = 0;
        }
        return;
    }
}

void mkb_sdl_pump(void)
{
    static int applied = -1;
    int want;

    if (!mkb_enabled() || s_scripted)
        return;
    /* The settings overlay takes the mouse while it is open. */
    if (s_captured && xbox_InputBlocked())
        mkb_set_captured(0, "settings menu");
    want = s_captured && s_focus;
    if (want != applied) {
        if (SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE) != 0 && want) {
            static int warned;
            if (!warned++) {
                fprintf(stderr, "  [MKB] relative mouse mode unavailable: %s\n",
                        SDL_GetError());
                fflush(stderr);
            }
        }
        /* Counts from before the lock are the desktop's. */
        if (want) {
            InterlockedExchange(&s_dx, 0);
            InterlockedExchange(&s_dy, 0);
        }
        applied = want;
    }
}

/* ---- direct look --------------------------------------------------------- */

/* When each axis was last asked for (SDL_GetTicks64 ms; 0 = never), the
 * counts mkb_merge set aside for title hooks, and which axes it is handing
 * over (bit 0 x, bit 1 y). The axes are asked for separately: a title may
 * turn and tilt in two different places, each once a frame. */
static volatile Uint64 s_look_asked[2];
static volatile LONG s_look_dx, s_look_dy, s_look_live;

/* The script clock (ms since the first pad poll; 0 = not started). */
static volatile LONG s_script_t0;
static unsigned long mkb_now_ms(void);
static void mkb_script_mouse(unsigned long t, LONG *dx, LONG *dy);

/* auto_capture, once a poll: lock the mouse when the title starts taking
 * direct look (gameplay) and free it when the title stops (no ask for 300
 * ms: a menu or the pause screen took over). Only on those edges, and on
 * coming back to the window in gameplay, so F9 still frees or locks it in
 * between. */
static void mkb_auto_capture(void)
{
    Uint64 at = SDL_GetTicks64(), a0 = s_look_asked[0], a1 = s_look_asked[1];
    Uint64 last = a0 > a1 ? a0 : a1;
    /* A layout mode on (a minigame) is gameplay too. */
    int play = (last && at - last < 300) || InterlockedCompareExchange(&s_mode_on, 0, 0);

    if (!s_cfg.auto_capture)
        return;
    if (play && (!s_auto_play || s_auto_away) && !s_captured && !xbox_InputBlocked()) {
        mkb_set_captured(1, s_auto_play ? "back in the window, in gameplay" : "gameplay");
        if (InterlockedCompareExchange(&s_mouse_btn, 0, 0) & 1)
            InterlockedExchange(&s_swallow_click, 1);
        InterlockedAnd(&s_mouse_tap, ~1);
    } else if (!play && s_auto_play && s_captured) {
        mkb_set_captured(0, "menu");
    }
    s_auto_play = play;
    s_auto_away = 0;
}

int mkb_mouse_look_take(float *yaw_deg, float *pitch_deg)
{
    LONG dx = 0, dy = 0, live = 0;
    Uint64 now;
    double k;

    if (yaw_deg)
        *yaw_deg = 0.0f;
    if (pitch_deg)
        *pitch_deg = 0.0f;
    if (!mkb_enabled())
        return 0;
    now = SDL_GetTicks64();
    if (yaw_deg)
        s_look_asked[0] = now;
    if (pitch_deg)
        s_look_asked[1] = now;
    /* The counts are read here, when the title wants them, straight from the
     * accumulators -- not only what mkb_merge moved across. The title polls
     * the pad in clumps (USB reports), so a steady mouse reached the camera
     * as nothing for two frames and a lump on the third. A script's travel
     * is due by the clock, so it is handed over the same way. */
    if (s_scripted || (s_captured && s_focus))
        live = (yaw_deg ? 1 : 0) | (pitch_deg ? 2 : 0);
    if (!live)
        return 0;
    if (s_scripted && s_script_t0) {
        LONG sx = 0, sy = 0;
        mkb_script_mouse(mkb_now_ms() - (unsigned long)s_script_t0, &sx, &sy);
        if (sx) InterlockedExchangeAdd(&s_dx, sx);
        if (sy) InterlockedExchangeAdd(&s_dy, sy);
    }
    if (live & 1)
        dx = InterlockedExchange(&s_look_dx, 0) + InterlockedExchange(&s_dx, 0);
    if (live & 2)
        dy = InterlockedExchange(&s_look_dy, 0) + InterlockedExchange(&s_dy, 0);
    k = s_cfg.look_deg * s_cfg.sens;
    if (live & 1)
        *yaw_deg = (float)(dx * k);
    if (live & 2)
        *pitch_deg = (float)(-dy * k * (s_cfg.look_invert_y ? -1.0 : 1.0));
    return 1;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in: a keyboard and mouse silently acting as player 1 is
         * surprising when a real pad is what you meant to use. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            int n = 0, i;
            s_scripted = getenv("RECOMP_MKB_SCRIPT") != NULL;
            /* The script clock starts with the layer: a headless run under
             * RECOMP_PAD_SCRIPT never polls the host pad, so "the first
             * poll" never came and a scripted mouse never moved. A title
             * starts the layer at boot (mkb_enabled) for a clock that
             * matches its own. */
            InterlockedExchange(&s_script_t0, (LONG)(mkb_now_ms() ? mkb_now_ms() : 1));
            mkb_load_config();
            mkb_env_bindings();
            for (i = 0; i < IN_MAX; i++)
                n += s_bind[i].kind != ACT_NONE;
            fprintf(stderr, "  [MKB] mouse+keyboard %s (%sF9 locks the mouse%s, F9/Esc"
                    " frees it; sens=%.2f dz=%.2f full=%ld look_deg=%.3f; %d bindings, %s)\n",
                    s_scripted ? "scripted (real devices ignored)" : "ready",
                    s_cfg.auto_capture ? "locked in gameplay, free in menus; " : "",
                    s_cfg.click_capture ? ", so does a click in the window" : "",
                    s_cfg.sens, s_cfg.dz, s_cfg.full, s_cfg.look_deg, n,
                    s_bind_custom ? "from the layout file" : "built-in layout");
            fflush(stderr);
        }
    }
    return on;
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's own
 * deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* ---- scripted input (RECOMP_MKB_SCRIPT) --------------------------------- *
 *
 *   <ms>:<INPUT>[:<hold ms>]          a key or mouse button, held (default 150)
 *   <ms>:mouse:<dx>:<dy>[:<ms>]       mouse travel, spread over the time given
 *
 * Times count from the layer's start (mkb_enabled). Each step is
 * printed as it starts, so a run log says what was pressed when. */
typedef struct { unsigned long at, len; int in; long dx, dy; int shown; } MkbStep;
#define MKB_SCRIPT_MAX 256
static MkbStep s_steps[MKB_SCRIPT_MAX];
static int s_nsteps = -1;

static void mkb_script_load(void)
{
    const char *e = getenv("RECOMP_MKB_SCRIPT");
    char buf[8192], *tok, *ctx = NULL;
    s_nsteps = 0;
    if (!e || !*e)
        return;
    snprintf(buf, sizeof buf, "%s", e);
    for (tok = SDL_strtokr(buf, ",", &ctx); tok && s_nsteps < MKB_SCRIPT_MAX;
         tok = SDL_strtokr(NULL, ",", &ctx)) {
        MkbStep st;
        char *f[5];
        int n = 0;
        char *c = tok, *p;
        memset(&st, 0, sizeof st);
        while (n < 5 && (p = strchr(c, ':')) != NULL) { *p = 0; f[n++] = c; c = p + 1; }
        if (n < 5) f[n++] = c;
        if (n < 2)
            continue;
        st.at = strtoul(f[0], NULL, 10);
        if (!SDL_strcasecmp(f[1], "mouse")) {
            if (n < 4) continue;
            st.in = -1;
            st.dx = strtol(f[2], NULL, 10);
            st.dy = strtol(f[3], NULL, 10);
            st.len = n > 4 ? strtoul(f[4], NULL, 10) : 100;
        } else {
            st.in = mkb_parse_input(f[1]);
            if (st.in < 0) {
                fprintf(stderr, "  [MKB] script: unknown input %s\n", f[1]);
                continue;
            }
            st.len = n > 2 ? strtoul(f[2], NULL, 10) : 150;
        }
        if (!st.len) st.len = 1;
        s_steps[s_nsteps++] = st;
    }
    fprintf(stderr, "  [MKB] script of %d step(s)\n", s_nsteps);
    fflush(stderr);
}

static void mkb_active(void);

static void mkb_script_shown(MkbStep *st, int i, unsigned long t)
{
    if (!st->shown) {
        st->shown = 1;
        mkb_active();                   /* as a real key or mouse would */
        fprintf(stderr, "  [MKB] script t=%lu ms: step %d\n", t, i);
        fflush(stderr);
    }
}

/* Apply the script's held inputs at time t into down[]. */
static void mkb_script_apply(unsigned long t, unsigned char *down)
{
    int i;
    for (i = 0; i < s_nsteps; i++) {
        MkbStep *st = &s_steps[i];
        if (st->in < 0 || t < st->at || t >= st->at + st->len)
            continue;
        mkb_script_shown(st, i, t);
        down[st->in] = 1;
    }
}

/* The script's mouse travel due between the last call and t, into *dx and *dy.
 * Called by the pad poll and by mkb_mouse_look_take, whichever comes first;
 * the clock only moves forward. Counts due by b less counts due by a, both
 * rounded: a step hands out exactly its total, a slow one included. */
static void mkb_script_mouse(unsigned long t, LONG *dx, LONG *dy)
{
    static volatile LONG last;
    LONG prev;
    int i;
    if (s_nsteps < 0)
        mkb_script_load();
    do {
        prev = last;
        if ((LONG)t <= prev)
            return;
    } while (InterlockedCompareExchange(&last, (LONG)t, prev) != prev);
    for (i = 0; i < s_nsteps; i++) {
        MkbStep *st = &s_steps[i];
        unsigned long end = st->at + st->len, a, b;
        if (st->in >= 0 || t < st->at || (unsigned long)prev >= end)
            continue;
        mkb_script_shown(st, i, t);
        a = (unsigned long)prev > st->at ? (unsigned long)prev : st->at;
        b = t < end ? t : end;
        if (b > a) {
            double fa = (double)(a - st->at) / st->len, fb = (double)(b - st->at) / st->len;
            *dx += (LONG)floor(st->dx * fb + 0.5) - (LONG)floor(st->dx * fa + 0.5);
            *dy += (LONG)floor(st->dy * fb + 0.5) - (LONG)floor(st->dy * fa + 0.5);
        }
    }
}

/* A wheel notch is a button press: down for two polls' worth, up for one,
 * then the next queued notch. */
typedef struct { LONG queued; unsigned long until; int down; } MkbPulse;

static int mkb_pulse(MkbPulse *p, LONG add, unsigned long now)
{
    p->queued += add;
    if (p->queued > 8) p->queued = 8;
    if (now < p->until)
        return p->down;
    if (p->down) {                       /* release before the next press */
        p->down = 0;
        p->until = now + 60;
    } else if (p->queued > 0) {
        p->queued--;
        p->down = 1;
        p->until = now + 90;
    }
    return p->down;
}

static unsigned long mkb_now_ms(void)
{
    return (unsigned long)SDL_GetTicks64();
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0, look_x = 0, look_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    int any = 0, i, ax[8];
    unsigned char down[IN_MAX];
    static DWORD packet;
    static unsigned long t0;
    static MkbPulse wheel_up, wheel_dn;
    unsigned long now, t;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;           /* NULL: the backend asking whether the layer is on */
    now = mkb_now_ms();
    if (!t0)
        t0 = (unsigned long)s_script_t0;
    t = now - t0;

    if (!s_scripted) {
        /* Focus gate: while the user is elsewhere the mouse and keys belong
         * to whoever has them, and the game reads a pad at rest. */
        if (!s_focus) {
            s_aim_vx = s_aim_vy = 0.0;
            InterlockedExchange(&s_dx, 0);
            InterlockedExchange(&s_dy, 0);
            InterlockedExchange(&s_look_live, 0);
            InterlockedExchange(&s_look_dx, 0);
            InterlockedExchange(&s_look_dy, 0);
            InterlockedExchange(&s_wheel, 0);
            InterlockedExchange(&s_mouse_tap, 0);
            memset((void *)s_vk_tap, 0, sizeof s_vk_tap);
            return 0;
        }
        mkb_auto_capture();
        if (!s_captured) {                  /* keys yes, the mouse no */
            InterlockedExchange(&s_dx, 0);
            InterlockedExchange(&s_dy, 0);
            InterlockedExchange(&s_wheel, 0);
            InterlockedExchange(&s_mouse_tap, 0);
            InterlockedExchange(&s_mouse_btn, 0);
        }
    } else {
        mkb_auto_capture();                 /* logged, for scripted checks */
    }

    dx    = InterlockedExchange(&s_dx, 0);
    dy    = InterlockedExchange(&s_dy, 0);
    wheel = InterlockedExchange(&s_wheel, 0);
    /* Held buttons stay held; a click shorter than the poll still lands. */
    btn   = InterlockedCompareExchange(&s_mouse_btn, 0, 0)
          | InterlockedExchange(&s_mouse_tap, 0);
    if (s_swallow_click)
        btn &= ~1;                          /* the click that captured */

    /* Everything held this poll, as one table indexed like s_bind. */
    for (i = 0; i < 256; i++) {
        down[i] = (unsigned char)(s_vk_down[i] | s_vk_tap[i]);
        s_vk_tap[i] = 0;
    }
    for (i = 0; i < 5; i++)
        down[IN_MOUSE(i)] = (btn >> i) & 1;
    if (s_scripted) {
        if (s_nsteps < 0)
            mkb_script_load();
        mkb_script_apply(t, down);
        mkb_script_mouse(t, &dx, &dy);
    }
    /* A title hook turning its camera from the mouse (mkb_mouse_look_take)
     * gets an axis's counts instead of the aim stick, for as long as it
     * keeps asking for that axis. */
    {
        Uint64 at = SDL_GetTicks64(), ax0 = s_look_asked[0], ax1 = s_look_asked[1];
        int on = s_scripted || s_captured;
        LONG live = (on && ax0 && at - ax0 < 500 ? 1 : 0)
                  | (on && ax1 && at - ax1 < 500 ? 2 : 0);
        InterlockedExchange(&s_look_live, live);
        if (live & 1) {
            InterlockedExchangeAdd(&s_look_dx, dx);
            dx = 0;
            s_aim_vx = 0.0;
        }
        if (live & 2) {
            InterlockedExchangeAdd(&s_look_dy, dy);
            dy = 0;
            s_aim_vy = 0.0;
        }
    }
    down[IN_WHEEL_UP]   = (unsigned char)mkb_pulse(&wheel_up, wheel > 0 ? wheel : 0, now);
    down[IN_WHEEL_DOWN] = (unsigned char)mkb_pulse(&wheel_dn, wheel < 0 ? -wheel : 0, now);

    memset(analog, 0, sizeof analog);
    memset(ax, 0, sizeof ax);
    {
        LONG on = mkb_modes_now();
        int m;
        for (i = 0; i < IN_MAX; i++) {
            const MkbAction *a = &s_bind[i];
            for (m = 0; on && m < s_nmodes; m++)
                if ((on >> m & 1) && s_modes[m].order[i])
                    a = &s_modes[m].act[i];
            if (!down[i] || a->kind == ACT_NONE)
                continue;
            if (a->kind == ACT_BTN)
                b |= (WORD)(1u << a->idx);
            else if (a->kind == ACT_ANALOG)
                analog[a->idx] = 255;
            else if (a->kind == ACT_AXIS)
                ax[a->idx] = 1;
        }
    }

    /* ---- keys on the sticks: movement, and keyboard look ---- */
    {
        int mx = ax[AX_LRIGHT] - ax[AX_LLEFT], my = ax[AX_LUP] - ax[AX_LDOWN];
        int lx = ax[AX_RRIGHT] - ax[AX_RLEFT], ly = ax[AX_RUP] - ax[AX_RDOWN];
        if (mx || my)
            mkb_aim_vector(mx * (my ? 0.7071 : 1.0), my * (mx ? 0.7071 : 1.0),
                           &mov_x, &mov_y);
        if (lx || ly)
            mkb_aim_vector(lx * (ly ? 0.7071 : 1.0), ly * (lx ? 0.7071 : 1.0),
                           &look_x, &look_y);
    }

    /* ---- mouse: aim ----
     *
     * A stick reading is a speed, and the game samples the pad on its own
     * schedule, so the mouse is turned into a speed too: counts per 10 ms,
     * averaged over `smooth` ms of real time, held on the stick until the
     * mouse slows. */
    {
        static Uint64 qf, qlast;
        Uint64 qn = SDL_GetPerformanceCounter();
        double dt_ms = 0.0;

        if (!qf)
            qf = SDL_GetPerformanceFrequency();
        if (qlast)
            dt_ms = (double)(qn - qlast) * 1000.0 / (double)qf;
        if (dt_ms >= 0.05 || !qlast) {
            double a;
            if (dt_ms > 100.0) dt_ms = 100.0;          /* after a pause */
            if (dt_ms < 0.05)  dt_ms = 0.05;
            a = 1.0 - exp(-dt_ms / s_cfg.smooth);
            s_aim_vx += ((double)dx / dt_ms - s_aim_vx) * a;   /* counts/ms */
            s_aim_vy += ((double)dy / dt_ms - s_aim_vy) * a;
            if (s_aim_vx * s_aim_vx + s_aim_vy * s_aim_vy < 1e-4)
                s_aim_vx = s_aim_vy = 0.0;             /* < 10 counts/s */
            qlast = qn;
        } else {
            /* Two polls in the same instant: keep the counts for the next. */
            InterlockedExchangeAdd(&s_dx, dx);
            InterlockedExchangeAdd(&s_dy, dy);
        }
    }
    if (s_aim_vx || s_aim_vy) {
        double fx =  s_aim_vx * 10.0 / (double)s_cfg.full;
        double fy = -s_aim_vy * 10.0 / (double)s_cfg.full;
        if (s_cfg.invert_y) fy = -fy;
        mkb_aim_vector(fx, fy, &aim_x, &aim_y);
    } else if (look_x || look_y) {
        aim_x = look_x;
        aim_y = look_y;
    }

    /* RECOMP_MKB_TRACE=1: once a second, how often the pad was polled, how
     * far the mouse moved, and the largest aim deflection handed out. */
    {
        static int trace = -1;
        static unsigned long t_last, polls;
        static long sum_x, sum_y;
        static int peak;
        int m;
        if (trace < 0) {
            const char *e = getenv("RECOMP_MKB_TRACE");
            trace = e && *e && *e != '0';
        }
        if (trace) {
            polls++;
            sum_x += dx; sum_y += dy;
            m = abs(aim_x) > abs(aim_y) ? abs(aim_x) : abs(aim_y);
            if (m > peak) peak = m;
            if (now - t_last >= 1000) {
                fprintf(stderr, "  [MKB] %lu polls/s, mouse %ld,%ld counts,"
                        " aim peak %d (%.0f%%)\n", polls, sum_x, sum_y, peak,
                        peak * 100.0 / 32767.0);
                fflush(stderr);
                t_last = now; polls = 0; sum_x = sum_y = 0; peak = 0;
            }
        }
    }

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}

/*
 * The keyboard as a pad on port 0. See keyboard.h.
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * On by default (not on Android), with the map below as its defaults, which
 * the player can change (the rebinding section of keyboard.h);
 * RECOMP_KEYBOARD=0 turns it off, =1 on. It only ever answers for port 0, and is merged on top of a
 * pad connected there (input_core.c), so a real controller keeps working
 * beside it.
 *
 * Keys come from the game's window, which only receives them while it has
 * the focus, so typing in another window does not drive the game. Losing
 * the focus releases every key, or a key held across Alt-Tab or Cmd-Tab
 * would stay held for ever.
 */
#include "keyboard.h"
#include "recomp_env.h"

#include <stdio.h>
#include <string.h>

/* Reading this needs no lock. Each entry is written only by the window's
 * thread and read only by the game's poll, one byte at a time, and a press
 * seen a poll late is indistinguishable from one made a poll later. */
static volatile unsigned char s_key_down[256];

static void (*volatile s_activity)(void);
static int kb_on(void);
static int vk_mapped(int vk);

void xbox_InputKeyboardSetActivityHook(void (*fn)(void))
{
    s_activity = fn;
}

void xbox_InputKeySet(int vk, int down)
{
    if ((unsigned)vk < 256) {
        s_key_down[vk] = down ? 1 : 0;
        if (down && s_activity && vk_mapped(vk) && kb_on())
            s_activity();
    }
}

void xbox_InputKeysClear(void)
{
    memset((void *)s_key_down, 0, sizeof s_key_down);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* The defaults of the one map, on every host. Driving is on the
 * letters and the arrows both: W and Up the accelerator (right trigger), S
 * and Down the brake (left trigger), A D and Left Right the left stick. The
 * arrows are also the d-pad, so they move through the menus; a menu takes
 * the d-pad and the stick as one direction (checked on the game, BO3), and
 * driving ignores the d-pad, so one press of an arrow is not two inputs
 * anywhere. Space is A (boost, select), Backspace B (Crashbreaker, back),
 * Shift X (look back), Ctrl Y (change view), Q and E the white and black
 * buttons (E: the next track), Enter Start, Esc Back. */
const xbox_key_row xbox_key_map[] = {
    { "Up arrow",    { XBOX_VK_UP },     XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_UP,    0, "D-pad up" },
    { "Down arrow",  { XBOX_VK_DOWN },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_DOWN,  0, "D-pad down" },
    { "Left arrow",  { XBOX_VK_LEFT },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_LEFT,  0, "D-pad left" },
    { "Right arrow", { XBOX_VK_RIGHT },  XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_RIGHT, 0, "D-pad right" },
    { "Enter",       { XBOX_VK_RETURN }, XBOX_KEY_BUTTON, XBOX_GAMEPAD_START,      0, "START" },
    { "Esc",         { XBOX_VK_ESCAPE }, XBOX_KEY_BUTTON, XBOX_GAMEPAD_BACK,       0, "BACK" },
    { "Space",       { XBOX_VK_SPACE },  XBOX_KEY_ANALOG, XBOX_BUTTON_A,           0, "A" },
    { "Backspace",   { XBOX_VK_BACK },   XBOX_KEY_ANALOG, XBOX_BUTTON_B,           0, "B" },
    { "Shift",       { XBOX_VK_SHIFT },  XBOX_KEY_ANALOG, XBOX_BUTTON_X,           0, "X" },
    { "Ctrl",        { XBOX_VK_CONTROL },XBOX_KEY_ANALOG, XBOX_BUTTON_Y,           0, "Y" },
    { "Q",           { 'Q' },            XBOX_KEY_ANALOG, XBOX_BUTTON_WHITE,       0, "White" },
    { "E",           { 'E' },            XBOX_KEY_ANALOG, XBOX_BUTTON_BLACK,       0, "Black" },
    { "W or Up arrow",    { 'W', XBOX_VK_UP },    XBOX_KEY_ANALOG, XBOX_BUTTON_RTRIGGER, 0, "Right trigger" },
    { "S or Down arrow",  { 'S', XBOX_VK_DOWN },  XBOX_KEY_ANALOG, XBOX_BUTTON_LTRIGGER, 0, "Left trigger" },
    { "A or Left arrow",  { 'A', XBOX_VK_LEFT },  XBOX_KEY_AXIS, XBOX_KEY_LX, -1, "Left stick left" },
    { "D or Right arrow", { 'D', XBOX_VK_RIGHT }, XBOX_KEY_AXIS, XBOX_KEY_LX, +1, "Left stick right" },
};
const size_t xbox_key_map_count = sizeof xbox_key_map / sizeof xbox_key_map[0];

/* ── the keys now (the defaults, then the player's) ─────────────────── */

#define KEY_ROWS 16
#define KEY_ACT_ROW0 4          /* xbox_key_map[4..15] are the twelve actions */
static volatile unsigned char s_vk[KEY_ROWS][XBOX_KEY_SLOTS];
static int s_ready;             /* 0 not yet, 1 being made, 2 made */

static void keys_defaults(void)
{
    for (size_t i = 0; i < KEY_ROWS && i < xbox_key_map_count; i++) {
        s_vk[i][0] = xbox_key_map[i].vk[0];
        s_vk[i][1] = xbox_key_map[i].vk[1];
    }
}

static void keys_ensure(void)
{
    int zero = 0;

    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) == 2)
        return;
    if (__atomic_compare_exchange_n(&s_ready, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const char *v;

        keys_defaults();
        v = recomp_env(RENV_KEYBOARD_KEYS);
        if (v && *v)
            xbox_InputKeyBindingsParse(v);
        __atomic_store_n(&s_ready, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != 2)
            ;
    }
}

static int vk_mapped(int vk)
{
    keys_ensure();
    for (size_t i = 0; i < xbox_key_map_count && i < KEY_ROWS; i++)
        if (vk && (s_vk[i][0] == vk || s_vk[i][1] == vk))
            return 1;
    return 0;
}

/* RECOMP_KEYBOARD_ARROWS=dpad: the arrows are the d-pad alone. A title
 * whose driving reads the d-pad (Double-S.T.E.A.L 2: up changes the camera,
 * down is the rear view) changed the camera on every press of Up to
 * accelerate; W A S D still drive. */
static int arrows_dpad_only(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = recomp_env(RENV_KEYBOARD_ARROWS);
        on = v && !strcmp(v, "dpad");
    }
    return on;
}

static int is_arrow(int vk)
{
    return vk == XBOX_VK_UP || vk == XBOX_VK_DOWN || vk == XBOX_VK_LEFT || vk == XBOX_VK_RIGHT;
}

/* Row i's keys, a d-pad row's arrows aside under RECOMP_KEYBOARD_ARROWS=dpad. */
static int row_held(size_t i)
{
    int k, held = 0;
    for (k = 0; k < XBOX_KEY_SLOTS && !held; k++) {
        int vk = s_vk[i][k];
        if (!vk || (xbox_key_map[i].kind != XBOX_KEY_BUTTON && arrows_dpad_only() && is_arrow(vk)))
            continue;
        held = xbox_FramebufferKeyDown(vk);
    }
    return held;
}

void xbox_InputKeyboardState(XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    /* Per thumb axis, whether a key holds it each way: a digital key means
     * full deflection, and opposite keys together cancel. */
    int pos[4] = { 0, 0, 0, 0 }, neg[4] = { 0, 0, 0, 0 }, axis[4];

    memset(out, 0, sizeof *out);
    keys_ensure();
    for (size_t i = 0; i < xbox_key_map_count && i < KEY_ROWS; i++) {
        const xbox_key_row *r = &xbox_key_map[i];
        if (!row_held(i))
            continue;
        switch (r->kind) {
        case XBOX_KEY_BUTTON:
            out->Gamepad.wButtons |= r->target;
            break;
        case XBOX_KEY_ANALOG:
            /* Analog on the console, so a key is 255 rather than a flag --
             * a title that reads these as a pressure never sees a press if
             * they are 1. */
            out->Gamepad.bAnalogButtons[r->target] = 255;
            break;
        case XBOX_KEY_AXIS:
            if (r->sign > 0) pos[r->target] = 1; else neg[r->target] = 1;
            break;
        }
    }
    for (int a = 0; a < 4; a++)
        axis[a] = (pos[a] - neg[a]) * 32767;
    out->Gamepad.sThumbLX = (SHORT)axis[XBOX_KEY_LX];
    out->Gamepad.sThumbLY = (SHORT)axis[XBOX_KEY_LY];
    out->Gamepad.sThumbRX = (SHORT)axis[XBOX_KEY_RX];
    out->Gamepad.sThumbRY = (SHORT)axis[XBOX_KEY_RY];

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    out->dwPacketNumber = ++packet;
}

/* On unless RECOMP_KEYBOARD starts with 0. Not on Android unless it is set
 * to 1: a handheld's own buttons can reach SDL as key events as well as pad
 * buttons, and the keys would add a Start to every A. */
static int kb_on(void)
{
#if defined(__ANDROID__)
    return recomp_env_on(RENV_KEYBOARD);
#else
    const char *v = recomp_env(RENV_KEYBOARD);
    return !(v && *v == '0');
#endif
}

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out)
{
    if (!kb_on())
        return 0;
    xbox_InputKeyboardState(out);
    return 1;
}

static const char *const k_readme_head[] = {
    "| Key | Guest control |",
    "|---|---|",
};

size_t xbox_InputKeyMapReadmeRow(size_t i, char *buf, size_t size)
{
    const size_t nh = sizeof k_readme_head / sizeof k_readme_head[0];
    int n;

    if (!buf || !size)
        return 0;
    if (i < nh)
        n = snprintf(buf, size, "%s", k_readme_head[i]);
    else if (i - nh < xbox_key_map_count)
        n = snprintf(buf, size, "| %s | %s |",
                     xbox_key_map[i - nh].keys, xbox_key_map[i - nh].guest);
    else
        return 0;
    return n > 0 ? (size_t)n : 0;
}

/* ── rebinding (keyboard.h) ──────────────────────────────────────────── */

static const struct { const char *id, *name; } k_action[XBOX_KEYACT_COUNT] = {
    { "start",        "Start (pause, select)" },
    { "back",         "Back (back, pause while driving)" },
    { "boost",        "Boost, select (A)" },
    { "crashbreaker", "Crashbreaker, back (B)" },
    { "look_back",    "Look back (X)" },
    { "change_view",  "Change view (Y)" },
    { "white",        "White" },
    { "black",        "Next music track (Black)" },
    { "accelerate",   "Accelerate" },
    { "brake",        "Brake, reverse" },
    { "steer_left",   "Steer left" },
    { "steer_right",  "Steer right" },
};

/* The keys that have a name, and the names (files and menus): letters and
 * digits are themselves. */
static const struct { int vk; const char *name; } k_key[] = {
    { XBOX_VK_UP, "Up" }, { XBOX_VK_DOWN, "Down" }, { XBOX_VK_LEFT, "Left" }, { XBOX_VK_RIGHT, "Right" },
    { XBOX_VK_SPACE, "Space" }, { XBOX_VK_RETURN, "Enter" }, { XBOX_VK_ESCAPE, "Esc" },
    { XBOX_VK_BACK, "Backspace" }, { XBOX_VK_SHIFT, "Shift" }, { XBOX_VK_CONTROL, "Ctrl" },
    { XBOX_VK_TAB, "Tab" }, { 0x21, "PageUp" }, { 0x22, "PageDown" }, { 0x23, "End" }, { 0x24, "Home" },
    { 0x2D, "Insert" },
    { 0xBA, "Semicolon" }, { 0xBB, "Equals" }, { 0xBC, "Comma" }, { 0xBD, "Minus" }, { 0xBE, "Period" },
    { 0xBF, "Slash" }, { 0xC0, "Backquote" }, { 0xDB, "LeftBracket" }, { 0xDC, "Backslash" },
    { 0xDD, "RightBracket" }, { 0xDE, "Quote" },
};
#define K_KEYS ((int)(sizeof k_key / sizeof k_key[0]))

const char *xbox_InputKeyActionName(int a)
{
    return (unsigned)a < XBOX_KEYACT_COUNT ? k_action[a].name : "";
}

const char *xbox_InputKeyActionId(int a)
{
    return (unsigned)a < XBOX_KEYACT_COUNT ? k_action[a].id : "";
}

const char *xbox_InputKeyName(int vk)
{
    static _Thread_local char buf[8][8];
    static _Thread_local int n;
    char *o;

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        o = buf[n++ & 7];
        o[0] = (char)vk;
        o[1] = 0;
        return o;
    }
    for (int i = 0; i < K_KEYS; i++)
        if (k_key[i].vk == vk)
            return k_key[i].name;
    return "";
}

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y)
            return 0;
    }
    return !*a && !*b;
}

int xbox_InputKeyFromName(const char *name)
{
    if (!name || !*name)
        return 0;
    if (!name[1]) {
        int c = name[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
        return 0;
    }
    for (int i = 0; i < K_KEYS; i++)
        if (ci_eq(name, k_key[i].name))
            return k_key[i].vk;
    return 0;
}

int xbox_InputKeyBindable(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return 1;
    for (int i = 0; i < K_KEYS; i++)
        if (k_key[i].vk == vk)
            return 1;
    return 0;
}

int xbox_InputKeyDefault(int a, int slot)
{
    if ((unsigned)a >= XBOX_KEYACT_COUNT || (unsigned)slot >= XBOX_KEY_SLOTS)
        return 0;
    return xbox_key_map[KEY_ACT_ROW0 + a].vk[slot];
}

int xbox_InputKeyBinding(int a, int slot)
{
    if ((unsigned)a >= XBOX_KEYACT_COUNT || (unsigned)slot >= XBOX_KEY_SLOTS)
        return 0;
    keys_ensure();
    return s_vk[KEY_ACT_ROW0 + a][slot];
}

int xbox_InputKeyActionHeld(int a)
{
    if ((unsigned)a >= XBOX_KEYACT_COUNT)
        return 0;
    keys_ensure();
    return row_held((size_t)(KEY_ACT_ROW0 + a));
}

int xbox_InputKeyOwner(int vk)
{
    keys_ensure();
    for (int a = 0; a < XBOX_KEYACT_COUNT; a++)
        for (int s = 0; s < XBOX_KEY_SLOTS; s++)
            if (vk && s_vk[KEY_ACT_ROW0 + a][s] == vk)
                return a;
    return -1;
}

static void (*volatile s_listener)(void);

void xbox_InputKeyBindingsSetListener(void (*fn)(void))
{
    s_listener = fn;
}

static void listener_call(void)
{
    void (*fn)(void) = s_listener;
    if (fn && __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) == 2)
        fn();
}

/* The binding itself, for what runs inside keys_ensure too. */
static int key_bind(int a, int slot, int vk, int *other)
{
    if ((unsigned)a >= XBOX_KEYACT_COUNT || (unsigned)slot >= XBOX_KEY_SLOTS)
        return -1;
    if (!vk) {
        s_vk[KEY_ACT_ROW0 + a][slot] = 0;
        return 0;
    }
    if (!xbox_InputKeyBindable(vk))
        return -1;
    for (int b = 0; b < XBOX_KEYACT_COUNT; b++)
        for (int s = 0; s < XBOX_KEY_SLOTS; s++) {
            if (b == a && s == slot)
                continue;
            if (s_vk[KEY_ACT_ROW0 + b][s] == vk) {
                if (other)
                    *other = b;
                return -2;
            }
        }
    s_vk[KEY_ACT_ROW0 + a][slot] = (unsigned char)vk;
    return 0;
}

int xbox_InputKeyBind(int a, int slot, int vk, int *other)
{
    int r, was;

    keys_ensure();
    was = (unsigned)a < XBOX_KEYACT_COUNT && (unsigned)slot < XBOX_KEY_SLOTS ? s_vk[KEY_ACT_ROW0 + a][slot] : -1;
    r = key_bind(a, slot, vk, other);
    if (r == 0 && was != (int)s_vk[KEY_ACT_ROW0 + a][slot])
        listener_call();
    return r;
}

void xbox_InputKeyBindingsReset(void)
{
    keys_ensure();
    keys_defaults();
    listener_call();
}

/* The slots of an action, the empty ones last. */
static void act_keys(int a, int out[XBOX_KEY_SLOTS])
{
    int n = 0;

    out[0] = out[1] = 0;
    for (int s = 0; s < XBOX_KEY_SLOTS; s++)
        if (s_vk[KEY_ACT_ROW0 + a][s])
            out[n++] = s_vk[KEY_ACT_ROW0 + a][s];
}

static int act_is_default(int a)
{
    int now[XBOX_KEY_SLOTS], def[XBOX_KEY_SLOTS], n = 0;

    act_keys(a, now);
    def[0] = def[1] = 0;
    for (int s = 0; s < XBOX_KEY_SLOTS; s++)
        if (xbox_key_map[KEY_ACT_ROW0 + a].vk[s])
            def[n++] = xbox_key_map[KEY_ACT_ROW0 + a].vk[s];
    return now[0] == def[0] && now[1] == def[1];
}

int xbox_InputKeyBindingsAreDefault(void)
{
    keys_ensure();
    for (int a = 0; a < XBOX_KEYACT_COUNT; a++)
        if (!act_is_default(a))
            return 0;
    return 1;
}

size_t xbox_InputKeyBindingsFormat(char *buf, size_t size)
{
    size_t at = 0;
    char tmp[512];

    keys_ensure();
    tmp[0] = 0;
    for (int a = 0; a < XBOX_KEYACT_COUNT; a++) {
        int k[XBOX_KEY_SLOTS], w;

        if (act_is_default(a))
            continue;
        act_keys(a, k);
        w = snprintf(tmp + at, sizeof tmp - at, "%s%s=%s%s%s", at ? ";" : "", k_action[a].id,
                     k[0] ? xbox_InputKeyName(k[0]) : "", k[1] ? "," : "",
                     k[1] ? xbox_InputKeyName(k[1]) : "");
        if (w < 0 || (size_t)w >= sizeof tmp - at)
            break;
        at += (size_t)w;
    }
    if (buf && size)
        snprintf(buf, size, "%s", tmp);
    return at;
}

int xbox_InputKeyBindingsParse(const char *text)
{
    /* Two passes: the actions the text names are emptied first, so that a
     * swap (boost=Shift;look_back=Space) does not meet the old owner of a
     * key halfway. */
    int set = 0;
    const char *p;

    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) == 0)
        keys_ensure();               /* the environment's text, then this one over it */
    keys_defaults();
    if (!text || !*text) {
        listener_call();
        return 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (p = text; *p;) {
            const char *e = p, *eq = NULL;
            char id[32];
            int a = -1;

            while (*e && *e != ';') {
                if (*e == '=' && !eq)
                    eq = e;
                e++;
            }
            if (eq && (size_t)(eq - p) < sizeof id) {
                size_t n = (size_t)(eq - p);
                while (n && (p[0] == ' ')) { p++; n--; }
                while (n && p[n - 1] == ' ') n--;
                memcpy(id, p, n);
                id[n] = 0;
                for (int i = 0; i < XBOX_KEYACT_COUNT; i++)
                    if (ci_eq(id, k_action[i].id))
                        a = i;
            }
            if (a >= 0) {
                if (pass == 0) {
                    s_vk[KEY_ACT_ROW0 + a][0] = s_vk[KEY_ACT_ROW0 + a][1] = 0;
                } else {
                    const char *k = eq + 1;
                    int slot = 0;

                    set++;
                    while (k < e && slot < XBOX_KEY_SLOTS) {
                        const char *ke = k;
                        char name[24];
                        size_t n;
                        int vk;

                        while (ke < e && *ke != ',')
                            ke++;
                        n = (size_t)(ke - k);
                        while (n && *k == ' ') { k++; n--; }
                        while (n && k[n - 1] == ' ') n--;
                        if (n && n < sizeof name) {
                            memcpy(name, k, n);
                            name[n] = 0;
                            vk = xbox_InputKeyFromName(name);
                            if (vk && key_bind(a, slot, vk, NULL) == 0)
                                slot++;
                        }
                        k = ke < e ? ke + 1 : e;
                    }
                }
            }
            p = *e ? e + 1 : e;
        }
    }
    listener_call();
    return set;
}

void xbox_InputKeyActionKeys(int a, char *out, size_t size)
{
    int k[XBOX_KEY_SLOTS];

    if (!out || !size)
        return;
    out[0] = 0;
    if ((unsigned)a >= XBOX_KEYACT_COUNT)
        return;
    keys_ensure();
    act_keys(a, k);
    if (!k[0])
        snprintf(out, size, "none");
    else if (!k[1])
        snprintf(out, size, "%s", xbox_InputKeyName(k[0]));
    else
        snprintf(out, size, "%s, %s", xbox_InputKeyName(k[0]), xbox_InputKeyName(k[1]));
}

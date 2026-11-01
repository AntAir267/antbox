/*
 * The keyboard as a pad on port 0. See keyboard.h.
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * On by default (not on Android), with a fixed map; RECOMP_KEYBOARD=0 turns
 * it off, =1 on. It only ever answers for port 0, and is merged on top of a
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

/* The one map, on every host, fixed (no remapping). Driving is on the
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

static int vk_mapped(int vk)
{
    for (size_t i = 0; i < xbox_key_map_count; i++)
        if (xbox_key_map[i].vk[0] == vk || (xbox_key_map[i].vk[1] && xbox_key_map[i].vk[1] == vk))
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

static int row_held(const xbox_key_row *r)
{
    int second = r->vk[1] && !(r->kind != XBOX_KEY_BUTTON && arrows_dpad_only()
                               && is_arrow(r->vk[1]));
    return xbox_FramebufferKeyDown(r->vk[0])
        || (second && xbox_FramebufferKeyDown(r->vk[1]));
}

void xbox_InputKeyboardState(XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    /* Per thumb axis, whether a key holds it each way: a digital key means
     * full deflection, and opposite keys together cancel. */
    int pos[4] = { 0, 0, 0, 0 }, neg[4] = { 0, 0, 0, 0 }, axis[4];

    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < xbox_key_map_count; i++) {
        const xbox_key_row *r = &xbox_key_map[i];
        if (!row_held(r))
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

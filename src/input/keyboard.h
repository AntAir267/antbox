/**
 * xbox_input keyboard -- the host keyboard as a port-0 pad.
 *
 * One key table and one key map for every host. The table is indexed by
 * Windows virtual-key codes, because that is what the Win32 windows already
 * receive; the SDL window (keyboard_sdl.c) turns SDL keycodes into the same
 * codes. Writers: the GDI window (fb_present.c), the D3D11 window
 * (nv2a_pb_d3d11.c) and the SDL main loop. Reader: the game's poll, through
 * xbox_InputKeyboardPoll() as a backend's keyboard hook.
 *
 * No platform header here, so tests/input_keyboard checks the map on every
 * host and checks the table in src/input/README.md against
 * xbox_InputKeyMapReadmeRow().
 */
#ifndef XBOX_INPUT_KEYBOARD_H
#define XBOX_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Windows VK codes the map uses (winuser.h). */
#define XBOX_VK_BACK      0x08
#define XBOX_VK_TAB       0x09
#define XBOX_VK_RETURN    0x0D
#define XBOX_VK_SHIFT     0x10
#define XBOX_VK_CONTROL   0x11
#define XBOX_VK_ESCAPE    0x1B
#define XBOX_VK_SPACE     0x20
#define XBOX_VK_LEFT      0x25
#define XBOX_VK_UP        0x26
#define XBOX_VK_RIGHT     0x27
#define XBOX_VK_DOWN      0x28
/* Letters and digits are their uppercase ASCII codes, as in Win32. */

/* Record a key as held (down != 0) or released. vk outside 0..255 is
 * ignored. */
void xbox_InputKeySet(int vk, int down);
/* Called (from the thread that records the key) when a key the map uses goes
 * down while the keyboard is on: the keyboard is the device in use, so the
 * button prompts follow it (pad_glyphs.h, XBOX_PAD_FAMILY_KEYBOARD).
 * input_core.c sets it. NULL: none. */
void xbox_InputKeyboardSetActivityHook(void (*fn)(void));
/* Release every key (focus lost). */
void xbox_InputKeysClear(void);
/* Non-zero while that virtual key is held in the game's window. Always zero
 * when there is no window, which is the right answer: with nothing to focus
 * there is nothing to type into. Kept under its old name for usb_gamepad.c's
 * diagnostic. */
int  xbox_FramebufferKeyDown(int vk);

/* The pad the held keys make, through the one map. Moves the packet number
 * on every call, as the title looks for edges. */
void xbox_InputKeyboardState(XBOX_INPUT_STATE *out);
/* A backend's keyboard hook: 0 when RECOMP_KEYBOARD is off, else 1 with *out
 * filled by xbox_InputKeyboardState(). */
int  xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);

/* The map, row by row (tests and the README). */
enum { XBOX_KEY_BUTTON, XBOX_KEY_ANALOG, XBOX_KEY_AXIS };
enum { XBOX_KEY_LX, XBOX_KEY_LY, XBOX_KEY_RX, XBOX_KEY_RY };
typedef struct xbox_key_row {
    const char *keys;          /* as the README names them */
    uint8_t     vk[2];         /* 0 = no second key */
    uint8_t     kind;          /* XBOX_KEY_* */
    uint16_t    target;        /* wButtons bit, bAnalogButtons index, or axis */
    int8_t      sign;          /* XBOX_KEY_AXIS: -1 or +1 */
    const char *guest;         /* the guest control, as the README names it */
} xbox_key_row;

extern const xbox_key_row xbox_key_map[];
extern const size_t       xbox_key_map_count;

/* README line i of the generated table (header lines first); 0 past the
 * end, else the length written. */
size_t xbox_InputKeyMapReadmeRow(size_t i, char *buf, size_t size);

/* ── rebinding ──────────────────────────────────────────────────────────
 *
 * The map above is the defaults. Twelve of its rows are actions a player can
 * give other keys (every row but the four arrows, which are the d-pad of the
 * menus and stay): rows 4 to 15 of xbox_key_map, in that order. Each action
 * has two keys (the driving ones are on a letter and an arrow by default); a
 * slot may be empty. A key belongs to one action: a binding that would give
 * a key a second action is refused, so a keypress is never two buttons. The
 * arrows themselves are free to be bound as keys of an action (they are the
 * d-pad as well, as in the defaults).
 *
 * Saved as text in settings.toml (input.keys, RECOMP_KEYBOARD_KEYS): only the
 * actions that differ from the defaults, "boost=Shift;look_back=Space"; an
 * action with no keys is "name=". Read once at the first use. Any thread.
 */
enum {
    XBOX_KEYACT_START, XBOX_KEYACT_BACK, XBOX_KEYACT_BOOST, XBOX_KEYACT_CRASHBREAKER,
    XBOX_KEYACT_LOOK_BACK, XBOX_KEYACT_CHANGE_VIEW, XBOX_KEYACT_WHITE, XBOX_KEYACT_BLACK,
    XBOX_KEYACT_ACCELERATE, XBOX_KEYACT_BRAKE, XBOX_KEYACT_STEER_LEFT, XBOX_KEYACT_STEER_RIGHT,
    XBOX_KEYACT_COUNT
};
#define XBOX_KEY_SLOTS 2

/* "Accelerate", "Boost, select (A)": for a row of a menu. */
const char *xbox_InputKeyActionName(int action);
/* "accelerate", "boost": the name in the file. */
const char *xbox_InputKeyActionId(int action);
/* The key (a VK) in a slot, 0 for none; the default of the action. */
int  xbox_InputKeyBinding(int action, int slot);
int  xbox_InputKeyDefault(int action, int slot);
/* Whether a key of the action is held in the game's window. */
int  xbox_InputKeyActionHeld(int action);
/* Whether `vk` is a key that can be bound (not the arrows' d-pad rows' own
 * business: they are allowed; not F1, Alt or Delete, which are the system's
 * and the menu's). */
int  xbox_InputKeyBindable(int vk);
/* Bind a key to a slot (vk 0 empties it). 0 done (also when it already was);
 * -1 the key is not bindable; -2 another action has the key, its number in
 * *other (when not NULL), nothing changed. */
int  xbox_InputKeyBind(int action, int slot, int vk, int *other);
/* Which action has `vk` now, or -1. */
int  xbox_InputKeyOwner(int vk);
/* Every action to its defaults. */
void xbox_InputKeyBindingsReset(void);
/* Whether every action has its defaults. */
int  xbox_InputKeyBindingsAreDefault(void);
/* The text for the file ("" when everything is the default); size, with the
 * end, as snprintf. */
size_t xbox_InputKeyBindingsFormat(char *buf, size_t size);
/* Take the bindings from text (as above), over the defaults; a name it does
 * not know and a key that would be a second action's are left out. Returns
 * how many actions it set. NULL or "": the defaults. */
int  xbox_InputKeyBindingsParse(const char *text);
/* One function that is called after the bindings changed (a key bound or
 * emptied, a reset, a text parsed after the first use), on the thread that
 * changed them: a title redraws its key prompts from it (xbox_InputKeyBinding
 * has the keys now). NULL: none. */
void xbox_InputKeyBindingsSetListener(void (*fn)(void));
/* A key's name ("W", "Up", "Shift", "Comma"), "" for none; the VK of a name
 * (case does not matter), 0 when it is none. */
const char *xbox_InputKeyName(int vk);
int  xbox_InputKeyFromName(const char *name);
/* The keys of an action as a line for a menu: "W, Up", or "none". */
void xbox_InputKeyActionKeys(int action, char *out, size_t size);

#define XBOX_KEY_MAP_README_BEGIN "<!-- keyboard_map:begin (generated from src/input/keyboard.c; tests/input_keyboard checks it) -->"
#define XBOX_KEY_MAP_README_END   "<!-- keyboard_map:end -->"

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_H */

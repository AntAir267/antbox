/**
 * xbox_input touch -- on-screen controls as a port-0 pad (Android).
 *
 * The layout is the physical screen's: its corners less the cutout's
 * insets, never the game's picture (its aspect, fit, ultrawide or render
 * scale do not move a control).
 *
 * The layout follows the controls of Android racing games and emulator
 * overlays (PPSSPP, Dolphin, RetroArch): a stick or d-pad under the left
 * thumb, the buttons under the right, pause and the settings menu in a
 * corner. Two layouts, picked each frame by a host hook (driving or not):
 *
 *   driving   a floating stick on the left half (it starts where the thumb
 *             lands: steering, aftertouch), GAS (right trigger) and BRAKE
 *             (left trigger) bottom right, BOOST above GAS (A; it holds the
 *             right trigger too, so one thumb boosts), BREAKER (B), LOOK
 *             (X) and VIEW (Y) smaller.
 *   menus     a fixed d-pad on the left, A B X Y as on the pad, LT and RT.
 *   both      pause (Start) and the settings menu (the overlay), at the
 *             top left of centre, clear of the HUD
 *
 * Without a hook the menu layout is used: a whole pad, minus the sticks'
 * clicks and White/Black.
 *
 * Every finger owns the control it went down on (multi-touch: a thumb on
 * the stick and one on GAS are independent). A finger on a button glides:
 * slid onto a neighbour it presses that one instead (PPSSPP's "glide"), and
 * slid off into the gap it keeps the one it had until it is well away. A
 * tap is held until the title has read it twice.
 *
 * Visibility (input.touch_controls): auto hides the controls while a
 * controller is in use (a button, a stick past a third, a key) and shows
 * them at the next touch; always keeps them; off is no touch at all. Port 0
 * reads as connected while touch is not off, so a phone with no pad has a
 * player 1. The pad and the touch are merged like the keyboard
 * (input_core.c): a button held by either, a trigger the larger.
 *
 * Threads: the finger and controller events, the layout and the drawing are
 * the window's main thread; xbox_InputTouchState is the title's pad poll.
 * Nothing here calls SDL or the platform but for a lock (touch_pad_sdl.c is
 * the SDL glue), so tests/input_touch drives it on every host.
 */
#ifndef XBOX_INPUT_TOUCH_PAD_H
#define XBOX_INPUT_TOUCH_PAD_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* input.touch_controls, in the setting's order. */
enum { XBOX_TOUCH_AUTO, XBOX_TOUCH_ALWAYS, XBOX_TOUCH_OFF };

/* Settings (any thread). The mode's default is off; the Android settings
 * table gives auto. Opacity 0.1..1, scale 0.5..2 (sizes are fractions of
 * the screen's height times this). Auto-gas: the right trigger is held
 * while driving unless BRAKE is. */
void xbox_InputTouchSetMode(int mode);
int  xbox_InputTouchMode(void);
void xbox_InputTouchSetOpacity(float opacity);
void xbox_InputTouchSetScale(float scale);
void xbox_InputTouchSetAutoGas(int on);
/* The title is taking driving input now (main thread; may be NULL). */
void xbox_InputTouchSetDrivingHook(int (*driving)(void));

/* The screen in pixels, and the part a notch or rounded corner covers on
 * each side (pixels; the layout keeps out of it). Main thread. */
void xbox_InputTouchSetScreen(float w, float h);
/* The display's density (dots per inch; 0 unknown): the controls are sized
 * from the screen's height, but no larger than on a 90 mm tall screen, so a
 * tablet does not get huge buttons. Phones and 5-7" handhelds are under the
 * cap. */
void xbox_InputTouchSetDpi(float dpi);
void xbox_InputTouchSetInsets(float left, float top, float right, float bottom);
/* The insets from RECOMP_TOUCH_INSETS=<left>,<top>,<right>,<bottom>, which
 * the Android activity sets (and sets again) from the window's display
 * cutout. Main thread; cheap enough for every frame. */
void xbox_InputTouchReadInsets(void);

/* A finger, in pixels. Main thread. */
enum { XBOX_TOUCH_DOWN, XBOX_TOUCH_MOVE, XBOX_TOUCH_UP };
void xbox_InputTouchFinger(int what, int64_t id, float x, float y);
/* Every finger let go (the overlay opened, the window lost the focus). */
void xbox_InputTouchReleaseAll(void);
/* Called (main thread) when a finger goes down on a control: the touch is
 * the device in use, so the button prompts follow it. input_core.c sets
 * it. NULL: none. */
void xbox_InputTouchSetActivityHook(void (*fn)(void));
/* The overlay is open (input_core.c's xbox_InputSetBlocked): every finger
 * is let go of and a new one is not taken; the fingers are the overlay's
 * (SDL makes mouse events of them). Main thread. */
void xbox_InputTouchSetBlocked(int on);
/* A controller or key was used: auto mode hides the controls. */
void xbox_InputTouchControllerUsed(void);

/* For the title's poll of port 0 (input_core.c): 0 when touch is off, else
 * 1 with *out the controls' pad (neutral when nothing is held). The packet
 * number moves when the state does. */
int  xbox_InputTouchState(XBOX_INPUT_STATE *out);

/* The touch is the device in use: a finger went down since a controller or
 * key was last used (touch not off). For a title's host code that treats
 * the touch like the keys (one layout), so its choice does not flip with
 * each press and release. */
int  xbox_InputTouchInUse(void);

/* The settings button was pressed since the last call (the overlay opens
 * itself on it). */
int  xbox_InputTouchTakeMenu(void);

/* What to draw now, or 0 controls when hidden. Main thread. Re-reads the
 * driving hook, so the layout follows the title. */
enum { XBOX_TOUCH_KIND_BUTTON, XBOX_TOUCH_KIND_STICK, XBOX_TOUCH_KIND_DPAD };
enum { XBOX_TOUCH_ICON_NONE, XBOX_TOUCH_ICON_PAUSE, XBOX_TOUCH_ICON_MENU };
typedef struct xbox_touch_draw {
    int kind, icon;
    float x, y, r;          /* the control's centre and radius (a stick's: its base, where it is now) */
    float kx, ky;           /* STICK: the knob; DPAD: the held direction's unit vector, 0 0 none */
    int pressed;
    const char *label;      /* short, upper case ("GAS"), or NULL */
    uint32_t rgb;           /* 0xRRGGBB accent, 0 = plain */
} xbox_touch_draw;
#define XBOX_TOUCH_MAX_CONTROLS 16
int   xbox_InputTouchDrawList(xbox_touch_draw *out, int max);
int   xbox_InputTouchVisible(void);
float xbox_InputTouchOpacity(void);

/* Tests: forget everything (settings to their defaults). */
void xbox_InputTouchReset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_TOUCH_PAD_H */

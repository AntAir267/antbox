/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad (SDL hosts).
 *
 * The SDL port of the layer LegendaryXP's MechAssault2-recomp (MIT) built on
 * Win32 raw input: the API, the layout file and the feel are the same, so a
 * title's hooks written against one work on the other.
 *
 * The goal: let a recompiled title be played like a PC game -- the mouse
 * drives the aim stick (or, through a title hook, the camera directly), keys
 * drive movement and buttons, mouse buttons the triggers. It merges in
 * xbox_InputGetState (port 0), which every path a title reads its pad
 * through goes by.
 *
 * Opt-in (RECOMP_MKB=1). While it is on, the fixed keyboard map
 * (keyboard.c) is off: the layout here replaces it. Knobs, all also keys of
 * the layout file:
 *
 *   RECOMP_MKB=0|1        master switch            (default off)
 *   RECOMP_MKB_INI=path   layout file              (default mkb.ini beside
 *                                                    the executable)
 *   RECOMP_MKB_SENS=f     sens: mouse multiplier   (default 1.0)
 *   RECOMP_MKB_FULL=n     full: counts per 10 ms for a full stick throw (20)
 *   RECOMP_MKB_DZ=f       dz: the title's stick deadzone, skipped (0.18)
 *   RECOMP_MKB_ACC=f      acc: aim acceleration exponent (1 = off)
 *   RECOMP_MKB_SMOOTH=ms  smooth: mouse speed averaged over (25)
 *   RECOMP_MKB_LOOK_DEG=f look_deg: degrees per count for direct look (0.05)
 *   RECOMP_MKB_INVERT_Y=1 invert_y (stick mode only)
 *   RECOMP_MKB_BIND=...   bindings, "SPACE=A,MOUSE1=RT" (after the file)
 *   RECOMP_MKB_SCRIPT=... scripted input for unattended tests, instead of
 *                         the real devices: "<ms>:<KEY>[:<hold>]" and
 *                         "<ms>:mouse:<dx>:<dy>[:<ms>]", comma separated,
 *                         times from the layer's start (a title starts it
 *                         at boot with mkb_enabled())
 *   RECOMP_MKB_TRACE=1    once a second: polls, mouse travel, aim peak
 *
 * Bindings: "bind.<KEY> = <CONTROL>" in the file. KEY is a letter, digit,
 * F1-F12 (not F9), a named key (SPACE ENTER ESC TAB LSHIFT RCTRL UP ...),
 * MOUSE1-MOUSE5 or WHEELUP/WHEELDOWN; CONTROL is a pad button (A B X Y BLACK
 * WHITE LT RT START BACK LTHUMB RTHUMB DPAD_*), a stick direction
 * (MOVE_FORWARD/BACK/LEFT/RIGHT, LOOK_UP/DOWN/LEFT/RIGHT) or NONE. The first
 * binding replaces the built-in layout.
 *
 * The mouse starts free. F9 locks it to the game (SDL relative mode: hidden,
 * every movement reported); F9 again, Esc, the settings overlay and leaving
 * the window free it. click_capture = 1 makes a left click in the window lock
 * it too. auto_capture = 1 (default) locks it by itself while a title hook
 * keeps calling mkb_mouse_look_take (gameplay) and frees it once the hook
 * stops (a menu, the pause screen).
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim stick is
 * the mouse's whenever it is moving. Bumps dwPacketNumber when the virtual
 * pad contributed anything, so a title watching for edges sees the presses.
 * Returns 1 when it contributed (input_core.c then reports a connected pad
 * even with none plugged in), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* The mouse is locked to the game. */
int mkb_mouse_captured(void);

/*
 * Direct mouse look, for a title hook that turns its own camera: how far the
 * mouse moved since the last call, in degrees (look_deg per count, times
 * sens; x right, y up; look_invert_y flips y). Returns 0 and zeros when the
 * mouse is not driving the game (not locked, window not focused, layer off),
 * 1 otherwise. While a title keeps asking for an axis the mouse stops
 * steering that axis of the aim stick; an axis nobody asked for in half a
 * second drives the stick again. NULL for one axis takes the other only.
 */
int mkb_mouse_look_take(float *yaw_deg, float *pitch_deg);

/* Keyboard prompts: the n-th key or mouse button (0 first, in layout-file
 * order) bound to a pad control given by its layout name ("A", "RT",
 * "WHITE", "DPAD_UP", "MOVE_FORWARD", ...), named as the layout names it
 * ("E", "ENTER", "MOUSE1", "WHEELUP", "F5"); NULL past the last or for an
 * unknown control. */
const char *mkb_binding(const char *control, int n);

/* Layout modes: "mode.<name>.<KEY> = <CONTROL>" lines bind keys differently
 * while the title has that mode on. mkb_set_mode turns one on for `ms`
 * milliseconds (a hook that sees its minigame every frame keeps calling it),
 * until further notice (ms < 0) or off (0); unknown names are ignored.
 * mkb_mode_binding is mkb_binding over a mode's own lines. */
void mkb_set_mode(const char *mode, int ms);
const char *mkb_mode_binding(const char *mode, const char *control, int n);

/* F8 (bug hunting): logs "[MARK] F8 #n"; this returns how many presses a
 * title hook has not yet taken, so it can log its own state beside each. */
int mkb_take_mark(void);

/* SDL hosts: the window's events (fb_present_sdl.c passes every one), and a
 * pump on the window's thread once a loop, which applies the mouse lock
 * (SDL_SetRelativeMouseMode belongs to that thread). Both are no-ops while
 * the layer is off. The SDL_Event is passed as a pointer to keep SDL out of
 * this header. */
void mkb_sdl_event(const void *sdl_event);
void mkb_sdl_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */

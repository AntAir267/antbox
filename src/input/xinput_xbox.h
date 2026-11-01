/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system (idempotent; the other calls do it lazily).
 * Port N is XInput user index N, or SDL GameController slot N. Registers
 * xbox_InputShutdown() with atexit().
 */
void xbox_InputInit(void);

/**
 * Stop every motor the title started. Safe to call more than once.
 */
void xbox_InputShutdown(void);

/**
 * Get the state of a controller, mapped to the Duke layout (input_map.h).
 * Port: 0-3 (Xbox controller ports). A port last seen disconnected returns
 * ERROR_DEVICE_NOT_CONNECTED without asking the host until a second has
 * passed since it was last probed.
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * Set controller vibration: the title's motor speeds. What the host motor is
 * given is the rumble curve's (rumble_curve.h): the title's peak, a gate
 * under its idle hum, the strength, and a release when a speed falls.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * The rumble's clock: moves a port's falling motors on and re-issues a held
 * rumble. xbox_InputGetState calls it; a caller that polls the pad some other
 * way (a scripted pad, src/usb/usb_gamepad.c) calls it at its poll.
 */
void xbox_InputRumbleTick(DWORD dwPort);

/**
 * Check if a controller is connected (the cache; refreshed at most once a
 * second for a port the title does not read).
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

/* Bit n: pad n is driven by a pad script (RECOMP_PAD_SCRIPT /
 * RECOMP_PAD_LIVE, src/usb/usb_gamepad.c) and no controller has taken it
 * back. The overlay shows a note while any is set, so a run a test script is
 * driving is not mistaken for one's own (BO-96). */
void xbox_InputSetScriptedPads(unsigned mask);
unsigned xbox_InputScriptedPads(void);

/* What the last xbox_InputGetState of a port was made of: a host controller
 * that answered, and keyboard keys held at that read (port 0, when the
 * keyboard is on), on-screen controls held (port 0, touch on). For a title's host code that treats a controller's state
 * differently from the keys' (a control scheme for the pad, BO-106). */
#define XBOX_INPUT_SRC_PAD  1u
#define XBOX_INPUT_SRC_KEYS 2u
/* The on-screen controls (touch_pad.h), port 0: held, or the device in use
 * (a finger since the last controller input). Xbox layout like the keys. */
#define XBOX_INPUT_SRC_TOUCH 4u
unsigned xbox_InputSources(DWORD dwPort);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */

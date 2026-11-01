# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Files

| File | Purpose |
|------|---------|
| `xinput_xbox.h` | Public header: types, button constants, function prototypes |
| `input_map.h`, `input_map.c` | The host → Xbox mapping table, the pure map and deadzone helpers |
| `pad_glyphs.h`, `pad_glyphs.c` | Which button prompts go with the pad in use (`input.glyphs`): a pad's family from SDL's type and its USB ids, the glyph set from the setting, the family and the face layout. Pure, plus the state a title listens to |
| `input_core.c` | The public API: connection cache, probe backoff, keyboard merge, shutdown |
| `input_backend.h` | The seam between `input_core.c` and a host backend (tests swap it) |
| `xinput_device.c` | Host backends: XInput (Windows, Proton) and SDL2 GameController |
| `keyboard.c` | The key table and the one key map, every host |
| `keyboard_sdl.c` | The SDL window's key and focus events into that table (POSIX) |
| `touch_pad.h`, `touch_pad.c` | The on-screen controls (Android): two layouts, fingers, auto-hide, port 0's touch state. Pure |
| `touch_pad_sdl.c` | The SDL window's fingers into them, and the controller and key events that hide them |

## Quick Start

```c
#include "xinput_xbox.h"

// Initialize input system
xbox_InputInit();

// Poll controller state (port 0-3)
XBOX_INPUT_STATE state;
if (xbox_InputGetState(0, &state) == 0) {
    // Digital buttons
    if (state.Gamepad.wButtons & XBOX_GAMEPAD_START)
        pause_game();

    // Analog buttons (0-255 pressure)
    uint8_t trigger_r = state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    if (trigger_r > XBOX_ANALOG_BUTTON_THRESHOLD)
        accelerate(trigger_r / 255.0f);

    // Stick axes (-32768 to +32767)
    float steer = state.Gamepad.sThumbLX / 32768.0f;
}

// Vibration feedback
XBOX_VIBRATION vib = { .wLeftMotorSpeed = 32000, .wRightMotorSpeed = 16000 };
xbox_InputSetState(0, &vib);
```

## API

```c
// Initialize (idempotent; the other calls do it lazily)
void xbox_InputInit(void);

// Stop every motor (also run at exit)
void xbox_InputShutdown(void);

// Poll controller state (returns 0 on success, non-zero if disconnected)
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

// Set vibration motors
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

// Check if controller is connected
BOOL xbox_InputIsConnected(DWORD dwPort);

// Query controller capabilities
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);
```

## Types

```c
typedef struct {
    WORD  wButtons;                     // Digital button bitmask
    BYTE  bAnalogButtons[8];            // Analog button pressure (0-255)
    SHORT sThumbLX, sThumbLY;           // Left stick (-32768 to +32767)
    SHORT sThumbRX, sThumbRY;           // Right stick
} XBOX_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;               // Increments on state change
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;               // 0-65535
    WORD wRightMotorSpeed;              // 0-65535
} XBOX_VIBRATION;
```

## Button Constants

### Digital Buttons (wButtons bitmask)

```c
XBOX_GAMEPAD_DPAD_UP         0x0001
XBOX_GAMEPAD_DPAD_DOWN       0x0002
XBOX_GAMEPAD_DPAD_LEFT       0x0004
XBOX_GAMEPAD_DPAD_RIGHT      0x0008
XBOX_GAMEPAD_START            0x0010
XBOX_GAMEPAD_BACK             0x0020
XBOX_GAMEPAD_LEFT_THUMB       0x0040    // Left stick click
XBOX_GAMEPAD_RIGHT_THUMB      0x0080    // Right stick click
```

### Analog Buttons (bAnalogButtons[] indices)

The original Xbox had pressure-sensitive face buttons (0-255):

```c
XBOX_BUTTON_A          0    // Also used for "boost" in racing games
XBOX_BUTTON_B          1
XBOX_BUTTON_X          2
XBOX_BUTTON_Y          3
XBOX_BUTTON_BLACK      4    // Right shoulder (RB) on a modern pad
XBOX_BUTTON_WHITE      5    // Left shoulder (LB) on a modern pad
XBOX_BUTTON_LTRIGGER   6    // Left trigger
XBOX_BUTTON_RTRIGGER   7    // Right trigger

XBOX_ANALOG_BUTTON_THRESHOLD  30   // Recommended press threshold
```

### Host controller → Xbox mapping

One table in `input_map.c` drives both backends (XInput on Windows and
Proton, SDL2 GameController elsewhere); `tests/input_map` checks every row
and fails if the table below and the code disagree. Regenerate it with
`INPUT_MAP_README_OUT=<file>` set when running the test.

- White is the left shoulder (LB) and Black the right shoulder (RB):
  Microsoft's own Xbox 360 convention, and xemu's default.
- Face buttons and shoulders are digital on a modern pad, so the guest sees
  255 or 0; the Duke's pressure sensitivity is not available. A title that
  thresholds at `XBOX_ANALOG_BUTTON_THRESHOLD` (30) sees no difference.
- Sticks pass at the host's full range with no deadzone (the title applies
  its own). `RECOMP_PAD_DEADZONE=N` (0..32767, default 0) adds a radial
  deadzone of N, rescaled so full deflection still reads full, for pads
  that drift.
- SDL's stick Y points down and is inverted as `-1 - v`, so -32768 and
  32767 swap exactly (an SDL stick at rest reads -1 on Y).

<!-- input_map:begin (generated from src/input/input_map.c; tests/input_map checks it) -->
| Host control | Guest field | Guest value | XInput | SDL2 |
|---|---|---|---|---|
| A | `bAnalogButtons[A]` | 255 held, 0 released | `A` | `A` |
| B | `bAnalogButtons[B]` | 255 held, 0 released | `B` | `B` |
| X | `bAnalogButtons[X]` | 255 held, 0 released | `X` | `X` |
| Y | `bAnalogButtons[Y]` | 255 held, 0 released | `Y` | `Y` |
| Left shoulder (LB) | `bAnalogButtons[WHITE]` | 255 held, 0 released | `LEFT_SHOULDER` | `LEFTSHOULDER` |
| Right shoulder (RB) | `bAnalogButtons[BLACK]` | 255 held, 0 released | `RIGHT_SHOULDER` | `RIGHTSHOULDER` |
| Start / Menu | `wButtons START` | bit set while held | `START` | `START` |
| Back / View | `wButtons BACK` | bit set while held | `BACK` | `BACK` |
| Left stick click | `wButtons LEFT_THUMB` | bit set while held | `LEFT_THUMB` | `LEFTSTICK` |
| Right stick click | `wButtons RIGHT_THUMB` | bit set while held | `RIGHT_THUMB` | `RIGHTSTICK` |
| D-pad up | `wButtons DPAD_UP` | bit set while held | `DPAD_UP` | `DPAD_UP` |
| D-pad down | `wButtons DPAD_DOWN` | bit set while held | `DPAD_DOWN` | `DPAD_DOWN` |
| D-pad left | `wButtons DPAD_LEFT` | bit set while held | `DPAD_LEFT` | `DPAD_LEFT` |
| D-pad right | `wButtons DPAD_RIGHT` | bit set while held | `DPAD_RIGHT` | `DPAD_RIGHT` |
| Left trigger | `bAnalogButtons[LTRIGGER]` | 0..255 | `bLeftTrigger` | `TRIGGERLEFT` |
| Right trigger | `bAnalogButtons[RTRIGGER]` | 0..255 | `bRightTrigger` | `TRIGGERRIGHT` |
| Left stick X | `sThumbLX` | -32768..32767, right positive | `sThumbLX` | `LEFTX` |
| Left stick Y | `sThumbLY` | -32768..32767, up positive | `sThumbLY` | `LEFTY` as `-1 - v` |
| Right stick X | `sThumbRX` | -32768..32767, right positive | `sThumbRX` | `RIGHTX` |
| Right stick Y | `sThumbRY` | -32768..32767, up positive | `sThumbRY` | `RIGHTY` as `-1 - v` |
<!-- input_map:end -->

### Keyboard → Xbox mapping

The keyboard is a pad on port 0, on by default (`RECOMP_KEYBOARD=0` turns it
off; on Android it is off unless `RECOMP_KEYBOARD=1`, because a handheld's
own buttons can arrive as key events too), merged on top of a controller there: a real pad keeps working and the
keys only add (buttons OR, triggers the larger, a held key sets its stick
axis). The map is fixed; there is no remapping. One map in `keyboard.c`
serves every host: the Win32 windows (GDI and D3D11, under Proton too) record
Windows virtual-key codes, and the SDL window on macOS and Linux
turns SDL keycodes into the same codes (`keyboard_sdl.c`). `tests/input_keyboard`
checks every row and fails if the table below and the code disagree;
regenerate it with `INPUT_KEYBOARD_README_OUT=<file>` set when running the
test.

- The arrows are the d-pad (menus) and also steer, accelerate and brake like
  A D W S: one press of an arrow is the d-pad and the stick (or trigger)
  at once. F1 (and Cmd+, on a Mac) are the settings
  overlay's, not the pad's. While the overlay is open the title reads a
  neutral pad and no keys (`xbox_InputSetBlocked`), and after it closes the
  keys held stay neutral until they are let go.
- A key of the map going down makes the keyboard the device in use for the
  button prompts (`XBOX_PAD_FAMILY_KEYBOARD`, `pad_glyphs.h`); the next
  button of a pad takes the prompts back.
- Keys are read only while the game's window has the focus. Losing the
  focus (and on SDL, hiding or minimising the window) releases every key.
- Letters follow the keyboard layout (the key labelled W), not the physical
  position. Auto-repeat is ignored, and on macOS Cmd+key is left to the
  system (Cmd going down releases the held keys). Shift and Ctrl are either
  side's key. macOS keeps Ctrl+arrow for Mission Control, so Y with an arrow
  steer is better done with A and D there.
- Face buttons, shoulders and triggers read 255 held, 0 released; stick keys
  give full deflection, and opposite keys cancel.
- `RECOMP_KEY_TRACE=1` logs the first 40 key-downs as `[KEY] down vk=0x..`.

<!-- keyboard_map:begin (generated from src/input/keyboard.c; tests/input_keyboard checks it) -->
| Key | Guest control |
|---|---|
| Up arrow | D-pad up |
| Down arrow | D-pad down |
| Left arrow | D-pad left |
| Right arrow | D-pad right |
| Enter | START |
| Esc | BACK |
| Space | A |
| Backspace | B |
| Shift | X |
| Ctrl | Y |
| Q | White |
| E | Black |
| W or Up arrow | Right trigger |
| S or Down arrow | Left trigger |
| A or Left arrow | Left stick left |
| D or Right arrow | Left stick right |
<!-- keyboard_map:end -->

## The first controller's player

`input.first_pad` (settings.toml, the overlay's Input page as "First
controller", `RECOMP_INPUT_FIRST_PAD=player1|player2`; not on Android).
`player1`, the default: a controller takes the lowest free port at its first
button press, so the first one is player 1 and the keyboard adds to it on
port 0. `player2`: port 0 is the keyboard's alone and controllers take ports
from 1 up, so a keyboard and a controller are two players (split screen).
The XInput backend reads host pad `port - 1` then. A change frees the ports
already taken; each controller takes one again at its next button press.

## A title's second control scheme

`input.control_scheme` (settings.toml, the overlay's Input page as "Driving
controls", `RECOMP_INPUT_CONTROL_SCHEME=xbox|ps2`) is a choice the toolkit
only keeps: `xbox_InputControlScheme()` is 0 for the title's own layout and 1
for an alternative that the title's host code provides, and the overlay shows
the row once that code has called `xbox_InputOfferControlScheme(1)`. What the
alternative is, and when it holds, is the title's business, because only the
title knows when it is in a menu: it rewrites the pad state its game has just
read (Burnout 3: `src/game/controls.c` there, the PS2 version's driving
controls, only while driving). Two things here serve that code:

- `xbox_InputSources(port)`: whether the port's last state came from a
  controller (`XBOX_INPUT_SRC_PAD`) and whether a keyboard key was held in it
  (`XBOX_INPUT_SRC_KEYS`), so the keys can keep their documented buttons;
- `xbox_InputScriptedPads()`: the pads a pad script drives, so a script's
  `rt` means the same on every machine whatever the player's setting.

The Nintendo face layout (`input.face_layout`) is applied before this, as the
host pad is read.

## Connection state and rumble

`xbox_InputGetState` reads a connected port at every call. A port last seen
disconnected is not asked again until a second has passed since its last
probe (XInput's guidance); `xbox_InputIsConnected` answers from the same
cache and refreshes it under the same rule, so a pad plugged in later
appears within a second. Port N is XInput user index N, or the Nth SDL game
controller found at init. `xbox_InputSetState` reaches only connected ports;
`xbox_InputShutdown` (also registered with `atexit` by `xbox_InputInit`)
stops every motor.

### The rumble curve

A title's motor speeds were written for the Duke's and the S pad's
eccentric-mass motors. What a host motor is given is worked out in one place,
`input_core.c` with the arithmetic in `rumble_curve.c`, before any backend
sees it: an SDL pad, Windows XInput and an Android device's own vibrator all
get the same level.

```
u    = min(1, speed / peak)     peak: RECOMP_INPUT_RUMBLE_PEAK, the title's own
                                strongest speed (default 65535)
gate : 0 while speed < 800; once off, on again only from 1200 (per motor)
out  = u ^ (1 / strength)       strength: RECOMP_INPUT_RUMBLE_GAIN, the
                                overlay's "Rumble strength"; 1.0 the title's
                                speeds as the console played them, 0 off
time : a rise is immediate; a fall follows down exponentially, release
       about 90 ms (37 % of a step left after 90 ms), and ends in a zero
```

The strength is the exponent, never a multiplier before a clip: only the
peak is full, at any strength, so a strong rumble and a weak one stay apart.
Above 1 it lifts the low speeds (a pad whose motors barely answer them, a
phone's vibrator); the setting goes from 0 to 3 in steps of 0.25. The gate is
the console motors' start-up threshold: an idle hum of a hundredth of full
did not turn them. The release is their spin-down.

The release needs a clock: `xbox_InputRumbleTick(port)`, which
`xbox_InputGetState` calls and the emulated USB pad calls at every poll
(`src/usb/usb_gamepad.c`). The same tick gives a held rumble to the host
again every 150 ms (SDL's rumble runs for a set time) and writes a falling
level to the pad at most every 16 ms.

Nothing is sent, the refresh included, while the settings overlay is open
(`xbox_InputSetBlocked`) or the window is not in front
(`xbox_InputSetBackground`, from the window's focus events): a zero goes out
first, and what the title last asked for plays again after.

### Overlay hotkey (Back+Start)

With the overlay on (`xbox_InputSetComboGuard`), each pad's Back reaches the
title a moment late, so that Back+Start can open the settings without the
title seeing either button. This is SpeedBreaker's behaviour: a tap arrives
when you let go, a hold after a quarter of a second. Per port, on the pad's
buttons before the keyboard is merged (the keyboard is not touched); several
pads are independent. In `xbox_InputGetState`:

| Pad | The title sees |
|---|---|
| Back down | nothing of Back, and Start is hidden while it is down |
| Start comes with Back down | neither; nothing of the pair until both are let go |
| Back let go alone | Back pressed for `XBOX_INPUT_BACK_TAP_MS` (50 ms), at least `XBOX_INPUT_BACK_TAP_POLLS` (2) reads, then released |
| Back down `XBOX_INPUT_BACK_DELAY_MS` (250 ms), no Start | a held Back from then on; Start still hidden |
| Start first, Back after | Start at once (no grace: it would delay every pause); the overlay opens |

Where this changes the buttons without the pad having, the packet number
moves on too, since the title records edges by it. While the overlay is open
(or its port is latched) a Back press is swallowed whole. The same combo closes
the overlay; View+Menu on a Steam Deck arrive as Back+Start.

### A pad is never left shaking

What ends a run, and what stops the motors then:

| End | Stop |
|---|---|
| the window closed, Cmd+Q, the overlay's quit | the window's thread calls `xbox_InputShutdown` before SDL is taken down |
| `exit()`, a return from `main`, the title's own quit | `atexit` (POSIX); the exit hooks before `ExitProcess` (Windows, `recomp_exit_hook.h`) |
| SIGTERM, SIGINT, SIGHUP | a handler wakes a thread that calls `xbox_InputShutdown`, waits up to 150 ms, and the signal then ends the process as before (only where the signal had its default action) |
| a crash | the host's crash handler calls `xbox_InputPanicStop` (signal-safe: the same thread does the work) |
| the pad poll hangs, the process alive | the host's rumble effect runs `XBOX_RUMBLE_EFFECT_MS` (250 ms) and is renewed every `XBOX_RUMBLE_REFRESH_MS` (100 ms) only by the poll: SDL ends it at its next update |
| SIGKILL, power | nothing here can run. SDL's rumble time is kept by SDL in this process, not by the pad: what the pad does with its last report is the pad's own, and not measured here |

`xbox_InputShutdown` sets every motor to zero, then the backend's `shutdown`
writes the stop out (rumble and trigger rumble at zero, 40 ms for SDL's
HIDAPI rumble queue, which a process that ends at once would take with it
unwritten) and closes every controller. A pad that played a rumble in the
last 600 ms gets SDL's updates for up to 150 ms first: SDL's Xbox One driver
(a Series pad over Bluetooth on macOS) writes one rumble packet per 50 ms and
keeps a newer one, the stop included, until its next update, and a pad closed
with the stop still held back plays its last level on its own (its packet
loops) until it is switched off. The first end that runs prints one line,
`[INPUT] rumble stopped on N pads (quit|atexit|exit hook|signal)`, in every
log. After it nothing reaches a pad
again: the title's threads run on until the process is gone, and the USB pad
would otherwise give its held speeds back 150 ms later, to a pad whose SDL
timer no longer runs. The overlay, the window out of focus, a pad unplugged
(SDL stops a pad it closes) each stop the motors as before; a pad plugged in
again gets the held level at the next refresh.

`RECOMP_TRACE=input_diag` prints each change handed to the host, with the
title's speeds beside it; it runs without a pad too (`(no pad)`).

On Android SDL2 cannot rumble a pad (2.32's joystick driver has no rumble):
the first pad's rumble goes to the device's own vibrator when that pad is the
device's built-in one (`android_vibration.c`, `RECOMP_ANDROID_VIBRATION`),
and an external pad does not rumble.

## Touch controls (Android)

`touch_pad.c` draws nothing itself: the overlay asks it for the controls
(`xbox_InputTouchDrawList`) and draws them over the game while it is
closed. It answers port 0 like the keyboard (`XBOX_INPUT_SRC_TOUCH`):
merged on top of a pad, and the whole state when there is none, so a phone
has a player 1. The layout follows the title through a hook
(`xbox_InputTouchSetDrivingHook`): Burnout 3 gives a race layout (a floating
stick on the left half, GAS, BRAKE, BOOST, BREAKER, LOOK, VIEW) and a menu
layout (d-pad, A B X Y, LT RT); pause and the settings menu sit at the top, left of centre, in
both. Each finger keeps the control it went down on; a finger on a button
slides onto a neighbour (BOOST holds the accelerator too). The settings are
`input.touch_controls` (auto: hidden while a controller is used, back at the
next touch; always; off), `input.touch_opacity`, `input.touch_scale` and
`input.touch_auto_gas`, on the Input page; only the Android table has them,
so on the other hosts touch stays off. The Android activity passes the
display cutout's safe insets in `RECOMP_TOUCH_INSETS`; the layout keeps out
of them. `tests/input_touch` covers the layouts, glide, the stick,
auto-hide, auto-gas and the merge.

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.

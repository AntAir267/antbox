/**
 * Xbox Input Compatibility Layer
 *
 * The host backends behind input_core.c (input_backend.h): each one reads
 * a host pad and maps it to the Duke layout through the table in
 * input_map.c, so both hosts map controls the same way.
 *
 *   _WIN32 -> Windows XInput (also Proton/Wine)
 *   POSIX  -> SDL2 GameController
 */

#include "input_backend.h"
#include "recomp_env.h"
#include "input_map.h"
#include "keyboard.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32) && !defined(XBOXRECOMP_WIN_VULKAN)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#include <string.h>
#include "pad_glyphs.h"
#pragma comment(lib, "xinput.lib")

/* The keyboard on port 0 is shared with the SDL backend (keyboard.c); the
 * window procedures in fb_present.c and nv2a_pb_d3d11.c record the keys. */

_Static_assert(sizeof(XINPUT_GAMEPAD) == sizeof(XBOX_HOST_XINPUT_GAMEPAD)
               && offsetof(XINPUT_GAMEPAD, bLeftTrigger) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, bLeftTrigger)
               && offsetof(XINPUT_GAMEPAD, sThumbRY) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, sThumbRY),
               "XBOX_HOST_XINPUT_GAMEPAD must match XINPUT_GAMEPAD");

static DWORD xi_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    XINPUT_STATE xi;
    XBOX_HOST_XINPUT_GAMEPAD pad;
    DWORD rc;

    /* Input > First controller: player 2 leaves port 0 to the keyboard. */
    if (port < (DWORD)xbox_InputFirstPadPort())
        return ERROR_DEVICE_NOT_CONNECTED;
    rc = XInputGetState(port - xbox_InputFirstPadPort(), &xi);

    if (rc != ERROR_SUCCESS)
        return rc;
    memcpy(&pad, &xi.Gamepad, sizeof pad);
    out->dwPacketNumber = xi.dwPacketNumber;
    if (xbox_InputFaceLayoutNintendo())
        xbox_InputMirrorFaceXInput(&pad);
    xbox_InputMapXInput(&pad, &out->Gamepad);
    /* XInput has no per-press event: a pad button held after the keyboard
     * was last used makes the pad the device in use again (an XInput pad is
     * an Xbox 360 / One / Series one). */
    if (xbox_InputActivePadFamily() == XBOX_PAD_FAMILY_KEYBOARD
            && (out->Gamepad.wButtons || memchr(out->Gamepad.bAnalogButtons, 0xFF, 8)))
        xbox_InputSetActivePad(XBOX_PAD_FAMILY_XBOX_MODERN, 0);
    return ERROR_SUCCESS;
}

static DWORD xi_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    XINPUT_VIBRATION xv;
    xv.wLeftMotorSpeed  = v->wLeftMotorSpeed;
    xv.wRightMotorSpeed = v->wRightMotorSpeed;
    if (port < (DWORD)xbox_InputFirstPadPort())
        return ERROR_DEVICE_NOT_CONNECTED;
    return XInputSetState(port - xbox_InputFirstPadPort(), &xv);
}

static uint64_t xi_now_ms(void) { return GetTickCount64(); }

/* Under Wine XInputGetState has been seen to return ERROR_SUCCESS and an
 * idle pad for an empty user index. If a Wine/Proton build does that, the
 * connection probe has to move to XInputGetCapabilities. */
const xbox_input_backend xbox_input_host_backend = {
    NULL, xi_get_state, xi_set_state, xbox_InputKeyboardPoll, xi_now_ms, NULL
};

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include "pad_glyphs.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

/* Port -> controller. Read by the pad thread (OHCI) under g_pads_lock;
 * changed only by the thread that pumps SDL events (or, headless, by the
 * pad thread itself). */
static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static pthread_mutex_t g_pads_lock = PTHREAD_MUTEX_INITIALIZER;
/* sdl_shutdown ran: the pads are closed and none is opened again. Under
 * g_pads_lock. */
static int g_pads_closed;
/* How long the stop is given to reach the devices before the pads close. */
#define XBOX_RUMBLE_FLUSH_MS 40
/* A pad that was playing a rumble a moment ago may still have its stop
 * waiting inside SDL (see sdl_shutdown): the stop is given this long to go
 * out while SDL is updated. SDL's Xbox One driver sends one rumble packet
 * per 50 ms on Bluetooth (HIDAPI_DriverXboxOne_UpdateRumble, 10 ms on USB)
 * and holds a newer one back until its next update; the packet itself gets
 * the rumble thread and the close another 30 ms. */
#define XBOX_RUMBLE_SETTLE_MS 150
/* A rumble that ended longer ago than this has had its stop sent by the
 * title's own polling: no settling at the end. */
#define XBOX_RUMBLE_RECENT_MS 600
static uint64_t sdl_now_ms(void);
/* When a nonzero level was last given to a pad (0: never). Under
 * g_pads_lock. */
static uint64_t g_last_on_ms;

/* Set by the window (src/video/fb_present_sdl.c) when the process main thread
 * runs the SDL event loop. Its SDL_PumpEvents already updates the
 * controllers, so this side only reads them: one event loop, not two threads
 * racing to pump. */
static volatile int g_main_pump;
void xbox_InputSetMainPump(int on) { g_main_pump = on; }

/* Whether a controller is open, i.e. whether SDL may still be in use here. */
int xbox_InputAnyOpen(void)
{
    int any = 0;
    pthread_mutex_lock(&g_pads_lock);
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            any = 1;
    pthread_mutex_unlock(&g_pads_lock);
    return any;
}
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

_Static_assert(SDL_CONTROLLER_BUTTON_A == XBOX_SDL_BUTTON_A
               && SDL_CONTROLLER_BUTTON_START == XBOX_SDL_BUTTON_START
               && SDL_CONTROLLER_BUTTON_LEFTSHOULDER == XBOX_SDL_BUTTON_LEFTSHOULDER
               && SDL_CONTROLLER_BUTTON_DPAD_RIGHT == XBOX_SDL_BUTTON_DPAD_RIGHT,
               "XBOX_SDL_BUTTON_* must match SDL_GameControllerButton");
_Static_assert(SDL_CONTROLLER_AXIS_LEFTX == XBOX_SDL_AXIS_LEFTX
               && SDL_CONTROLLER_AXIS_RIGHTY == XBOX_SDL_AXIS_RIGHTY
               && SDL_CONTROLLER_AXIS_TRIGGERRIGHT == XBOX_SDL_AXIS_TRIGGERRIGHT,
               "XBOX_SDL_AXIS_* must match SDL_GameControllerAxis");

/* ── With a window: every game controller open, ports by first input ──
 *
 * The window's thread pumps SDL events, and everything that opens, closes
 * or enumerates controllers happens there (xbox_InputPadEvent): under
 * sdl2-compat a joystick call from a second thread races the event pump
 * (fb_present_sdl.c, BO-76). Every attached game controller is opened, and
 * a port goes to a controller when it first presses a button: player 1 is
 * the pad the player picked up. (Not an axis: a device whose trigger rests
 * at full, a mapping off by a sign, would take port 1 by doing nothing.)
 * Ports in SDL's device order put some other device on port 1 on the
 * Bazzite and the Steam Controller (Steam Input's virtual pad) on port 2,
 * while the title reads player 1. Hotplug: a controller added is opened,
 * one removed frees its port.
 *
 * Without a window (headless, nothing pumps) the pad thread opens the
 * controllers present at its first read, in device order, as before. */
#define PAD_OPEN_MAX 8
static struct {
    SDL_GameController *c;
    SDL_JoystickID id;
    int port;                         /* -1: no input yet */
} g_open[PAD_OPEN_MAX];

static void pad_describe(SDL_GameController *c, char *out, size_t n)
{
    SDL_Joystick *j = SDL_GameControllerGetJoystick(c);
    const char *name = SDL_GameControllerName(c);
    snprintf(out, n, "\"%s\" %04x:%04x player %d", name ? name : "?",
             SDL_GameControllerGetVendor(c), SDL_GameControllerGetProduct(c),
             j ? SDL_JoystickGetPlayerIndex(j) : -1);
}

/* The pad whose prompts the title should draw (pad_glyphs.h): this one
 * pressed a button, or is the first one of a headless run. SDL's own type
 * first, the USB ids for what it has no type for. A Nintendo pad's face
 * buttons come by their printed letters when SDL's hint says so (its
 * default). */
static void pad_active(SDL_GameController *c)
{
    static SDL_GameController *last;
    static int said;
    xbox_pad_family f = XBOX_PAD_FAMILY_XBOX;
    const char *sd = getenv("SteamDeck");
    int labels = 0;

    if (!c)
        return;
    switch (SDL_GameControllerGetType(c)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        f = XBOX_PAD_FAMILY_XBOX_MODERN;
        break;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
        f = XBOX_PAD_FAMILY_PLAYSTATION;
        break;
    case SDL_CONTROLLER_TYPE_PS5:
        f = XBOX_PAD_FAMILY_PLAYSTATION5;
        break;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        f = XBOX_PAD_FAMILY_NINTENDO;
        labels = SDL_GetHintBoolean(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, SDL_TRUE)
                     == SDL_TRUE;
        break;
    default:
        break;
    }
    f = xbox_PadFamilyFromIds(f, SDL_GameControllerGetVendor(c),
                              SDL_GameControllerGetProduct(c), SDL_GameControllerName(c),
                              sd && sd[0] == '1');
    if (c != last || !said) {
        static const char *const fam[] = { "unknown (xbox original)", "xbox modern", "playstation", "nintendo", "steam", "playstation 5 (DualSense)", "none", "keyboard" };
        char d[160];
        const char *name = SDL_GameControllerName(c);
        snprintf(d, sizeof d, "\"%s\" %04x:%04x", name ? name : "?",
                 SDL_GameControllerGetVendor(c), SDL_GameControllerGetProduct(c));
        fprintf(stderr, "[INPUT] controller %s in use: family %s (SDL type %d)\n", d, fam[f],
                (int)SDL_GameControllerGetType(c));
        last = c;
        said = 1;
    }
    xbox_InputSetActivePad(f, labels);
}

/* Window thread: open the controller at device index i if it is not open. */
static void pad_open_index(int i)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
    int k, free_k = -1;
    char d[160];

    if (!SDL_IsGameController(i))
        return;
    for (k = 0; k < PAD_OPEN_MAX; k++) {
        if (g_open[k].c && g_open[k].id == id)
            return;
        if (!g_open[k].c && free_k < 0)
            free_k = k;
    }
    if (free_k < 0)
        return;
    /* Under the lock: the end of the run (sdl_shutdown) reads the table
     * from another thread, and after it nothing is opened. */
    pthread_mutex_lock(&g_pads_lock);
    if (!g_pads_closed)
        g_open[free_k].c = SDL_GameControllerOpen(i);
    if (g_open[free_k].c) {
        g_open[free_k].id = id;
        g_open[free_k].port = -1;
    }
    pthread_mutex_unlock(&g_pads_lock);
    if (!g_open[free_k].c)
        return;
    pad_describe(g_open[free_k].c, d, sizeof d);
#if defined(__APPLE__) && SDL_VERSION_ATLEAST(2, 24, 0)
    {
        /* Which of SDL's macOS drivers has the pad decides how its rumble
         * ends (sdl_shutdown): HIDAPI has a device path, Apple's
         * GameController framework (SDL_mfijoystick.m) has none. */
        const char *pa = SDL_GameControllerPath(g_open[free_k].c);
        size_t l = strlen(d);
        snprintf(d + l, sizeof d - l, " [%s]", pa && *pa ? "HIDAPI" : "GameController framework");
    }
#endif
    XR_NOTICE( "[INPUT] controller %s opened; it gets a port at its first input\n", d);
}

/* Window thread: Input > First controller changed, so every controller gives
 * its port back and takes one again at its next button press. */
static unsigned g_first_pad_gen;

static void pad_reassign(void)
{
    unsigned gen = xbox_InputFirstPadGeneration();
    int freed;

    if (gen == g_first_pad_gen)
        return;
    g_first_pad_gen = gen;
    freed = 0;
    pthread_mutex_lock(&g_pads_lock);
    for (int k = 0; k < PAD_OPEN_MAX; k++)
        if (g_open[k].c && g_open[k].port >= 0) {
            g_pads[g_open[k].port] = NULL;
            g_open[k].port = -1;
            freed++;
        }
    pthread_mutex_unlock(&g_pads_lock);
    if (freed)
        fprintf(stderr, "[INPUT] first controller is player %d now; ports taken again at the next press\n",
            xbox_InputFirstPadPort() + 1);
}

/* Window thread: the lowest free port for the controller with this
 * instance id, if it has none yet (from player 2 with Input > First
 * controller at Player 2). */
static void pad_claim(SDL_JoystickID id)
{
    char d[160];

    for (int k = 0; k < PAD_OPEN_MAX; k++) {
        if (!g_open[k].c || g_open[k].id != id || g_open[k].port >= 0)
            continue;
        pthread_mutex_lock(&g_pads_lock);
        for (int p = xbox_InputFirstPadPort(); p < XBOX_MAX_CONTROLLERS; p++) {
            if (!g_pads[p]) {
                g_pads[p] = g_open[k].c;
                g_open[k].port = p;
                break;
            }
        }
        pthread_mutex_unlock(&g_pads_lock);
        if (g_open[k].port >= 0) {
            pad_describe(g_open[k].c, d, sizeof d);
            fprintf(stderr, "[INPUT] controller %s -> port %d (player %d), at its first input\n",
                    d, g_open[k].port, g_open[k].port + 1);
        }
        return;
    }
}

void xbox_InputPadEvent(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERDEVICEADDED:
        pad_open_index(ev->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (int k = 0; k < PAD_OPEN_MAX; k++) {
            if (!g_open[k].c || g_open[k].id != ev->cdevice.which)
                continue;
            pthread_mutex_lock(&g_pads_lock);
            if (g_open[k].port >= 0)
                g_pads[g_open[k].port] = NULL;
            SDL_GameControllerClose(g_open[k].c);
            pthread_mutex_unlock(&g_pads_lock);
            if (g_open[k].port >= 0)
                fprintf(stderr, "[INPUT] controller on port %d removed; the port is free\n",
                        g_open[k].port);
            else
                fprintf(stderr, "[INPUT] controller removed (it had no port)\n");
            g_open[k].c = NULL;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        pad_reassign();
        pad_claim(ev->cbutton.which);
        for (int k = 0; k < PAD_OPEN_MAX; k++)
            if (g_open[k].c && g_open[k].id == ev->cbutton.which)
                pad_active(g_open[k].c);
        break;
    /* A pad in the hand must not go on shaking for a game that is not in
     * front: the motors stop with the window's focus and come back with it
     * (input_core.c). */
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
                || ev->window.event == SDL_WINDOWEVENT_MINIMIZED
                || ev->window.event == SDL_WINDOWEVENT_HIDDEN)
            xbox_InputSetBackground(1);
        else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            xbox_InputSetBackground(0);
        else if (ev->window.event == SDL_WINDOWEVENT_SHOWN
                 || ev->window.event == SDL_WINDOWEVENT_RESTORED) {
            /* Back on screen, in front or not: as its focus says. */
            SDL_Window *w = SDL_GetWindowFromID(ev->window.windowID);
            if (w)
                xbox_InputSetBackground(!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS));
        }
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        xbox_InputSetBackground(1);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        xbox_InputSetBackground(0);
        break;
    default:
        break;
    }
}

/* Headless: up to XBOX_MAX_CONTROLLERS attached game controllers, in
 * device order, on the pad thread (nothing else uses SDL then). */
static void open_controllers(void)
{
    int slot = xbox_InputFirstPadPort();
    if (g_pads_closed)
        return;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot])
            g_pads[slot] = SDL_GameControllerOpen(i);
        slot++;
    }
}

static void sdl_init(void)
{
    /* Game controllers pull in SDL events; keep SIGINT/SIGTERM default. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
#if defined(__ANDROID__)
    {
        /* RECOMP_ANDROID_VIBRATION_FLOOR=<0..65535>: the title's motor
         * speeds under it are silence, in place of the rumble gate's 800
         * (and on again from half as much more, as the gate's 1200 is).
         * 0: no gate. It was the only floor before the gate; an override
         * now. */
        const char *f = getenv("RECOMP_ANDROID_VIBRATION_FLOOR");
        if (f && *f) {
            int floor_v = atoi(f);
            if (floor_v < 0) floor_v = 0;
            if (floor_v > 43690) floor_v = 43690;
            xbox_InputSetRumbleGate((unsigned)floor_v, (unsigned)floor_v * 3u / 2u);
        }
    }
#endif
    if (g_main_pump) {
        /* The window's thread (xbox_InputInit from fb_present_sdl.c): open
         * what is attached now; the ADDED events SDL also sends for these
         * find them open. */
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            pad_open_index(i);
        return;
    }
    pthread_mutex_lock(&g_pads_lock);
    open_controllers();
    pad_active(g_pads[0]);
    pthread_mutex_unlock(&g_pads_lock);
}

static DWORD sdl_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    SDL_GameController *c;
    XBOX_HOST_SDL_GAMEPAD pad;

    /* Held across the reads: the window's thread closes a removed
     * controller only under the lock. */
    pthread_mutex_lock(&g_pads_lock);
    c = g_pads[port];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        pthread_mutex_unlock(&g_pads_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!g_main_pump)
        SDL_GameControllerUpdate();

    for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
        pad.button[b] = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b) ? 1 : 0;
    for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
        pad.axis[a] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a);
    pthread_mutex_unlock(&g_pads_lock);
    out->dwPacketNumber = ++g_packet[port];
    if (xbox_InputFaceLayoutNintendo())
        xbox_InputMirrorFaceSdl(&pad);
    xbox_InputMapSdl(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

/* The motors' levels, after the rumble curve (input_core.c): given to the
 * pad as they are. */
static DWORD sdl_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    SDL_GameController *c;

    pthread_mutex_lock(&g_pads_lock);
    c = g_pads[port];
    if (!c || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        pthread_mutex_unlock(&g_pads_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    /* SDL rumble needs a duration: XBOX_RUMBLE_EFFECT_MS, which
     * input_core.c renews every XBOX_RUMBLE_REFRESH_MS while the level is
     * held (the same level again only moves SDL's expiry on: no write to the
     * device, no gap). SDL ends an effect that is not renewed, at its next
     * update, so a pad poll that hangs is silence in a quarter of a second.
     * That timer is SDL's, in this process: a process killed outright has
     * nothing left to end the effect, which is why every other end goes
     * through sdl_shutdown. 0 stops it. */
#if defined(__ANDROID__)
    /* SDL2's Android joystick driver cannot rumble a pad (2.32.10,
     * src/joystick/android/SDL_sysjoystick.c: ANDROID_JoystickRumble is
     * SDL_Unsupported), and a handheld's built-in pad has no motors anyway:
     * the first pad's rumble goes to the device's own vibrator when that pad
     * is the built-in one (android_vibration.c), and the pad is told to
     * stop. */
    if (port == 0) {
        extern int xbox_AndroidVibration(unsigned left, unsigned right, int pad_can_rumble,
                                         int vendor, int product);
        if (!xbox_AndroidVibration(v->wLeftMotorSpeed, v->wRightMotorSpeed,
                                   SDL_GameControllerHasRumble(c) == SDL_TRUE,
                                   SDL_GameControllerGetVendor(c),
                                   SDL_GameControllerGetProduct(c))) {
            SDL_GameControllerRumble(c, 0, 0, 0);
            pthread_mutex_unlock(&g_pads_lock);
            return ERROR_SUCCESS;
        }
    }
#endif
    if (v->wLeftMotorSpeed || v->wRightMotorSpeed)
        g_last_on_ms = sdl_now_ms();
    SDL_GameControllerRumble(c, v->wLeftMotorSpeed, v->wRightMotorSpeed,
                             (v->wLeftMotorSpeed || v->wRightMotorSpeed) ? XBOX_RUMBLE_EFFECT_MS : 0);
    pthread_mutex_unlock(&g_pads_lock);
    return ERROR_SUCCESS;
}

/* The end of the run (input_core.c, motors already set to zero through
 * sdl_set_state): every open pad stopped once more by hand, triggers too,
 * then closed. SDL's HIDAPI drivers write rumble from a queue on a thread
 * of their own, and a stop still in that queue when the process ends is
 * never written: the pad then plays the last level for ever. The wait lets
 * the queue drain, and closing a pad waits for its pending rumble as well
 * (HIDAPI_JoystickClose).
 *
 * That is not all of it. SDL's Xbox One driver (a Series pad over Bluetooth
 * on macOS) sends a rumble packet only when its previous one is 50 ms old
 * (10 ms on USB); a rumble asked for earlier is kept as "pending" and goes
 * out from the driver's next update, which is SDL_JoystickUpdate. At the end
 * of the run nobody updated SDL any more: a stop asked for within 50 ms of
 * the last level change stayed pending, the pad was closed, and the pad
 * (its packet says "loop 235 times") played the last level on its own for
 * minutes. So a pad that played a rumble a moment ago is given SDL's updates
 * here until the stop is out.
 *
 * Nothing opens or reads a pad after this: the title's threads run on until
 * the process is gone. */
static void sdl_shutdown(void)
{
    SDL_GameController *all[PAD_OPEN_MAX + XBOX_MAX_CONTROLLERS];
    int n = 0, recent = 0;

    pthread_mutex_lock(&g_pads_lock);
    g_pads_closed = 1;
    for (int k = 0; k < PAD_OPEN_MAX; k++) {
        if (g_open[k].c)
            all[n++] = g_open[k].c;
        g_open[k].c = NULL;
    }
    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        int seen = 0;
        for (int i = 0; i < n; i++)
            seen |= all[i] == g_pads[p];
        if (g_pads[p] && !seen)             /* headless: opened by port */
            all[n++] = g_pads[p];
        g_pads[p] = NULL;
    }
    if (n && SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        for (int i = 0; i < n; i++) {
            SDL_GameControllerRumble(all[i], 0, 0, 0);
            SDL_GameControllerRumbleTriggers(all[i], 0, 0, 0);
        }
        recent = g_last_on_ms && sdl_now_ms() - g_last_on_ms < XBOX_RUMBLE_RECENT_MS;
        if (recent) {
            /* The drivers send what they held back; the zero stays zero
             * (asking again changes nothing in SDL). */
            for (int t = 0; t < XBOX_RUMBLE_SETTLE_MS; t += 5) {
                SDL_GameControllerUpdate();
                SDL_Delay(5);
            }
        } else {
            SDL_Delay(XBOX_RUMBLE_FLUSH_MS);
        }
        for (int i = 0; i < n; i++)
            SDL_GameControllerClose(all[i]);
    }
    pthread_mutex_unlock(&g_pads_lock);
}

static uint64_t sdl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const xbox_input_backend xbox_input_host_backend = {
    sdl_init, sdl_get_state, sdl_set_state, xbox_InputKeyboardPoll, sdl_now_ms,
    sdl_shutdown
};

#endif /* _WIN32 */

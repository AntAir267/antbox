/**
 * xbox_input -- the seam between the public API (input_core.c) and a host
 * backend (xinput_device.c: XInput on Windows, SDL2 GameController
 * elsewhere). Internal to src/input and its tests.
 *
 * input_core.c owns the per-port connection cache and the probe backoff;
 * a backend only answers "what is on port N now". tests/input_map installs
 * a fake backend with xbox_InputSetBackend() and counts the calls.
 */
#ifndef XBOX_INPUT_BACKEND_H
#define XBOX_INPUT_BACKEND_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xbox_input_backend {
    /* Called once from xbox_InputInit(); may be NULL. */
    void  (*init)(void);
    /* Read port's pad. ERROR_SUCCESS with *out filled (already mapped
     * through input_map), or ERROR_DEVICE_NOT_CONNECTED. */
    DWORD (*get_state)(DWORD port, XBOX_INPUT_STATE *out);
    /* Set port's motors: host levels 0..65535, after input_core.c's rumble
     * curve. The backend applies no curve of its own. */
    DWORD (*set_state)(DWORD port, const XBOX_VIBRATION *vib);
    /* Port 0 keyboard, merged on top of the pad when RECOMP_KEYBOARD is on.
     * Returns 1 when the keyboard is enabled and *out was filled; NULL when
     * the backend has no keyboard. */
    int   (*keyboard)(XBOX_INPUT_STATE *out);
    /* Monotonic milliseconds. */
    uint64_t (*now_ms)(void);
    /* The end of the run, after input_core.c has set every motor to zero:
     * the stop written out to the devices and the pads closed. No call
     * reaches the backend's pads after it. May be NULL. */
    void  (*shutdown)(void);
} xbox_input_backend;

/* A motor is never given a level for longer than this: the host's rumble
 * effect runs XBOX_RUMBLE_EFFECT_MS, and input_core.c gives a held level
 * again every XBOX_RUMBLE_REFRESH_MS, well inside it, so what the pad plays
 * has no gap. If the giving stops (the pad poll hangs, the process is lost
 * to the host's rumble timer), the effect ends by itself. */
#define XBOX_RUMBLE_EFFECT_MS   250
#define XBOX_RUMBLE_REFRESH_MS  100

/* How long a signal waits for the motors' stop before the process ends. */
#define XBOX_RUMBLE_PANIC_MS    150

/* From a crash or signal handler: every motor off, by a thread woken for it
 * (POSIX; async-signal-safe, waits at most wait_ms) or at once if the
 * rumble lock is free (Windows). Best effort: the process is ending. */
void xbox_InputPanicStop(int wait_ms);

/* Rumble (input_core.c, rumble_curve.h), every backend alike. The strength
 * is the curve's exponent: 1.0 the title's speeds as the console played
 * them, 0 off, above 1 the low speeds lifted; RECOMP_INPUT_RUMBLE_GAIN until
 * this is called (the overlay's "Rumble strength"). The peak is the title's
 * own full speed: RECOMP_INPUT_RUMBLE_PEAK, 65535 unless set. The gate's two
 * speeds (off below, on from): 800 and 1200; on = 0 is no gate. Any thread. */
void xbox_InputSetRumbleStrength(double strength);
void xbox_InputSetRumblePeak(double peak);
void xbox_InputSetRumbleGate(unsigned off, unsigned on);

/* The window is not in front (or the app is in the background): the motors
 * stop and nothing is sent, the refresh included, until it is back. The
 * title's speeds are kept and play again then. Any thread. */
void xbox_InputSetBackground(int on);

/* The in-game overlay is open (input_core.c): the title reads neutral pads
 * and keys, and its rumble is silenced, until it closes. After it closes a
 * port stays neutral until its buttons have been let go once, so the press
 * that closed the overlay is not also the title's. Any thread. */
void xbox_InputSetBlocked(int on);

/* The overlay's hotkey is Back+Start of one pad (View+Menu on a Steam Deck),
 * so with the guard on a pad's Back reaches the title a moment late, as in
 * SpeedBreaker. Per port, the pad part only (the keyboard is not touched):
 *   - Back goes down: neither it nor Start reaches the title.
 *   - Start comes while Back is down: the overlay opens; the title never
 *     sees either, and nothing of the pair until both are let go.
 *   - Back is let go without Start: the title gets a Back press then, kept
 *     for XBOX_INPUT_BACK_TAP_MS and at least XBOX_INPUT_BACK_TAP_POLLS reads.
 *   - Back stays down XBOX_INPUT_BACK_DELAY_MS without Start: from then on
 *     it is passed as a normal held Back (Start stays hidden while it is).
 *   - Start first is not delayed: it reaches the title at once, and Back
 *     coming after it opens the overlay all the same. */
#define XBOX_INPUT_BACK_DELAY_MS 250
#define XBOX_INPUT_BACK_TAP_MS   50
#define XBOX_INPUT_BACK_TAP_POLLS 2
void xbox_InputSetComboGuard(int on);
int  xbox_InputBlocked(void);

/* The host backend this build uses (xinput_device.c). */
extern const xbox_input_backend xbox_input_host_backend;

/* Swap the backend and reset the cache and the init flag (tests). NULL
 * restores the host backend. */
void xbox_InputSetBackend(const xbox_input_backend *b);

/* Disconnected ports are probed at most this often (XInput's guidance). */
#define XBOX_INPUT_PROBE_MS 1000

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_BACKEND_H */

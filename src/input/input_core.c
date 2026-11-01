/**
 * xbox_input -- the public API over a host backend, with the per-port
 * connection cache.
 *
 * Connected ports are read at every call. A port last seen disconnected is
 * not asked again until XBOX_INPUT_PROBE_MS has passed since the last probe:
 * XInput's own guidance, because XInputGetState on an empty user index is
 * slow on Windows, and harmless elsewhere. xbox_InputIsConnected() answers
 * from the cache and refreshes it under the same rule, so a pad plugged in
 * later shows up within a second while the title polls the mask every frame.
 *
 * Rumble: a title's motor speeds pass through one curve here (rumble_curve.h:
 * peak, gate, strength, release) before any backend sees them, so the SDL
 * pads, Windows XInput and a handheld's own vibrator are all given the same
 * thing. The release needs a clock: xbox_InputRumbleTick, from the pad poll.
 *
 * Nothing here calls the platform but for a lock: the backend
 * (input_backend.h) does, and tests/input_map swaps in a fake one to count
 * the calls.
 */
#include "input_backend.h"
#include "keyboard.h"
#include "pad_glyphs.h"
#include "rumble_curve.h"
#include "touch_pad.h"
#include "recomp_env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include "recomp_exit_hook.h"
static SRWLOCK g_rumble_lock = SRWLOCK_INIT;
#define RUMBLE_LOCK()    AcquireSRWLockExclusive(&g_rumble_lock)
#define RUMBLE_TRYLOCK() TryAcquireSRWLockExclusive(&g_rumble_lock)
#define RUMBLE_UNLOCK()  ReleaseSRWLockExclusive(&g_rumble_lock)
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t g_rumble_lock = PTHREAD_MUTEX_INITIALIZER;
#define RUMBLE_LOCK()   pthread_mutex_lock(&g_rumble_lock)
#define RUMBLE_UNLOCK() pthread_mutex_unlock(&g_rumble_lock)
#endif

static const xbox_input_backend *g_be = &xbox_input_host_backend;
static int      g_inited;
static BOOL     g_connected[XBOX_MAX_CONTROLLERS];
static uint64_t g_last_probe[XBOX_MAX_CONTROLLERS];
static int      g_probed[XBOX_MAX_CONTROLLERS];   /* ever probed */

/* ── rumble ───────────────────────────────────────────────────────────── */

/* A held rumble is given to the host again this often
 * (XBOX_RUMBLE_REFRESH_MS, input_backend.h): the host's effect runs for
 * XBOX_RUMBLE_EFFECT_MS only, so a rumble nobody renews ends by itself. */
#define RUMBLE_REFRESH_MS XBOX_RUMBLE_REFRESH_MS
/* A falling level is given to the host no more often than this (a pad's
 * output report is a write to the device); a rise and the stop go at once. */
#define RUMBLE_SEND_MS 16

/* All under g_rumble_lock: the title's thread sets, the pad poll ticks, the
 * window's thread mutes. The lock is held across the backend's call. */
static struct rumble_port {
    WORD     target[2];         /* the title's last speeds, left and right */
    xbox_rumble_motor m[2];
    WORD     out[2];            /* what the host was last given */
    uint64_t step_ms, out_ms;
    int      live;              /* stepped before: step_ms means something */
} g_rumble[XBOX_MAX_CONTROLLERS];
static xbox_rumble_params g_rumble_params = XBOX_RUMBLE_PARAMS_DEFAULT;
static int g_rumble_params_read;
static int g_background;        /* the window is not in front */
/* xbox_InputShutdown ran: the motors are off for good. The title's threads
 * run on until the process is gone (exit() does not stop them, and the USB
 * pad gives a held rumble again every 150 ms): nothing of theirs may start a
 * motor after the stop. */
static int g_shutdown;

/* The in-game overlay: while open the title reads neutral state; after it
 * closes, a port reads neutral until it was seen with nothing held. */
static volatile int g_blocked;
static volatile unsigned g_latched;   /* bit per port */

static int any_held(const XBOX_GAMEPAD *g)
{
    int i;
    if (g->wButtons)
        return 1;
    for (i = 0; i < 8; i++)
        if (g->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            return 1;
    return 0;
}

static void neutral(XBOX_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
}

static volatile int g_combo_guard;

/* The overlay's Back+Start hotkey, per port (see input_backend.h): Back is
 * held back from the title until it is known whether Start follows. */
enum hk_state {
    HK_IDLE,       /* Back is up */
    HK_PENDING,    /* Back went down; the title sees neither it nor Start */
    HK_TAP,        /* Back came up alone: the title is given a short press */
    HK_HELD,       /* Back stayed down XBOX_INPUT_BACK_DELAY_MS: passed on */
    HK_SWALLOW     /* The hotkey (or a closed overlay's): nothing until let go */
};
static struct hk_port {
    enum hk_state st;
    uint64_t t;             /* PENDING: Back went down; TAP: the press began */
    int      polls;         /* TAP: reads that carried the press */
    DWORD    in_pkt, last_pkt;  /* the pad's, and the last one given on */
    DWORD    extra;         /* added to the pad's, so a change we made shows */
    WORD     last_btn;
    int      have_last;
} g_hk[XBOX_MAX_CONTROLLERS];

/* Pads a script drives (src/usb/usb_gamepad.c), for the overlay's note. */
static volatile unsigned g_scripted;

/* What each port's last read was made of (XBOX_INPUT_SRC_*). */
static volatile unsigned g_sources[XBOX_MAX_CONTROLLERS];

unsigned xbox_InputSources(DWORD dwPort)
{
    return dwPort < XBOX_MAX_CONTROLLERS ? g_sources[dwPort] : 0;
}

void xbox_InputSetScriptedPads(unsigned mask) { g_scripted = mask; }
unsigned xbox_InputScriptedPads(void) { return g_scripted; }

void xbox_InputSetComboGuard(int on) { g_combo_guard = on != 0; }

int xbox_InputBlocked(void) { return g_blocked; }

/* The pad's buttons for the title with the Back+Start hotkey taken out. Every
 * read of a connected pad goes through it, whatever the overlay is doing, so
 * the state follows the pad. The title's input layer records edges by the
 * packet number: where this changes the buttons without the pad having done
 * so (the end of a tap, the late Back), the number is moved on as well. */
static void hotkey_filter(DWORD port, XBOX_INPUT_STATE *s)
{
    struct hk_port *h = &g_hk[port];
    WORD *b = &s->Gamepad.wButtons;
    int back = (*b & XBOX_GAMEPAD_BACK) != 0;
    int start = (*b & XBOX_GAMEPAD_START) != 0;
    int blocked = g_blocked || (g_latched & (1u << port));
    uint64_t now = g_be->now_ms();
    DWORD pkt;

    h->in_pkt = s->dwPacketNumber;
again:
    switch (h->st) {
    case HK_IDLE:
        if (!back)
            break;
        h->st = HK_PENDING;
        h->t = now;
        /* fall through */
    case HK_PENDING:
        *b &= (WORD)~(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START);
        if (blocked || start) {
            h->st = HK_SWALLOW;
        } else if (!back) {
            h->st = HK_TAP;
            h->t = now;
            h->polls = 0;
            goto again;
        } else if (now - h->t >= XBOX_INPUT_BACK_DELAY_MS) {
            h->st = HK_HELD;
            *b |= XBOX_GAMEPAD_BACK;
        }
        break;
    case HK_TAP:
        if (blocked) {
            h->st = back ? HK_SWALLOW : HK_IDLE;
            *b &= (WORD)~(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START);
        } else if (h->polls >= XBOX_INPUT_BACK_TAP_POLLS
                   && now - h->t >= XBOX_INPUT_BACK_TAP_MS) {
            h->st = HK_IDLE;
            goto again;
        } else {
            h->polls++;
            *b &= (WORD)~XBOX_GAMEPAD_START;
            *b |= XBOX_GAMEPAD_BACK;
        }
        break;
    case HK_HELD:
        if (!back) {
            h->st = HK_IDLE;
            break;
        }
        *b &= (WORD)~XBOX_GAMEPAD_START;
        break;
    case HK_SWALLOW:
        if (!back && !start) {
            h->st = HK_IDLE;
            break;
        }
        *b &= (WORD)~(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START);
        break;
    }

    pkt = h->in_pkt + h->extra;
    if (h->have_last && pkt <= h->last_pkt && *b != h->last_btn) {
        h->extra += h->last_pkt + 1 - pkt;
        pkt = h->last_pkt + 1;
    }
    s->dwPacketNumber = pkt;
    h->last_pkt = pkt;
    h->last_btn = *b;
    h->have_last = 1;
}

/* RECOMP_INPUT_RUMBLE_GAIN and RECOMP_INPUT_RUMBLE_PEAK, once; the setters
 * below come after and replace them. A value that is no number is left at
 * the default. Lock held. */
static void rumble_params_read(void)
{
    const char *v;
    char *end;
    double d;

    if (g_rumble_params_read)
        return;
    g_rumble_params_read = 1;
    v = recomp_env(RENV_INPUT_RUMBLE_GAIN);
    if (v && *v) {
        d = strtod(v, &end);
        if (end != v && !*end)
            g_rumble_params.strength = d < 0.0 ? 0.0 : d;
    }
    v = recomp_env(RENV_INPUT_RUMBLE_PEAK);
    if (v && *v) {
        d = strtod(v, &end);
        if (end != v && !*end && d >= 1.0)
            g_rumble_params.peak = d > 65535.0 ? 65535.0 : d;
    }
}

void xbox_InputSetRumbleStrength(double strength)
{
    RUMBLE_LOCK();
    rumble_params_read();
    g_rumble_params.strength = strength > 0.0 ? strength : 0.0;
    RUMBLE_UNLOCK();
}

void xbox_InputSetRumblePeak(double peak)
{
    RUMBLE_LOCK();
    rumble_params_read();
    g_rumble_params.peak = peak >= 1.0 && peak <= 65535.0 ? peak : XBOX_RUMBLE_PEAK;
    RUMBLE_UNLOCK();
}

void xbox_InputSetRumbleGate(unsigned off, unsigned on)
{
    RUMBLE_LOCK();
    g_rumble_params.gate_off = off < on ? off : on;
    g_rumble_params.gate_on = on;
    RUMBLE_UNLOCK();
}

static int rumble_muted(void) { return g_blocked || g_background || g_shutdown; }

static WORD rumble_word(double level)
{
    level = level * 65535.0 + 0.5;
    return level >= 65535.0 ? 65535 : level <= 0.0 ? 0 : (WORD)level;
}

/* Move a port's motors on to now and give the host what changed. Runs for a
 * port with no pad too (nothing is sent), so the levels are right when one
 * appears and RECOMP_TRACE=input_diag shows them on a machine without a pad.
 * Lock held. */
static DWORD rumble_step(DWORD port)
{
    struct rumble_port *r = &g_rumble[port];
    uint64_t now = g_be->now_ms();
    double dt = r->live ? (double)(now - r->step_ms) : 0.0;
    XBOX_VIBRATION v;
    WORD o[2];
    DWORD rc = ERROR_SUCCESS;

    rumble_params_read();
    for (int i = 0; i < 2; i++)
        o[i] = rumble_word(xbox_RumbleStep(&r->m[i], r->target[i], &g_rumble_params, dt));
    r->step_ms = now;
    r->live = 1;
    if (rumble_muted())
        return ERROR_SUCCESS;

    if (o[0] == r->out[0] && o[1] == r->out[1]) {
        if (!(o[0] | o[1]) || now - r->out_ms < RUMBLE_REFRESH_MS)
            return ERROR_SUCCESS;
    } else if (o[0] <= r->out[0] && o[1] <= r->out[1] && (o[0] | o[1])
               && now - r->out_ms < RUMBLE_SEND_MS) {
        return ERROR_SUCCESS;               /* still falling: not yet */
    }
    if (recomp_env(RENV_INPUT_DIAG) && (o[0] != r->out[0] || o[1] != r->out[1])) {
        fprintf(stderr, "  [RUMBLE] port %lu t=%llu ms title %u/%u -> host %u/%u%s\n",
                (unsigned long)port, (unsigned long long)now, r->target[0], r->target[1],
                o[0], o[1], g_connected[port] ? "" : " (no pad)");
        fflush(stderr);
    }
    v.wLeftMotorSpeed = o[0];
    v.wRightMotorSpeed = o[1];
    if (g_connected[port])
        rc = g_be->set_state(port, &v);
    r->out[0] = o[0];
    r->out[1] = o[1];
    r->out_ms = now;
    return rc;
}

/* Every motor off at the host, now. Lock held. */
static void rumble_silence(void)
{
    static const XBOX_VIBRATION off = { 0, 0 };

    if (!g_inited)
        return;
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        if (g_connected[p] || g_rumble[p].out[0] || g_rumble[p].out[1])
            g_be->set_state(p, &off);
        g_rumble[p].out[0] = g_rumble[p].out[1] = 0;
    }
}

void xbox_InputSetBlocked(int on)
{
    on = on != 0;
    /* The fingers are the overlay's while it is open. */
    if (on != g_blocked)
        xbox_InputTouchSetBlocked(on);
    RUMBLE_LOCK();
    if (g_blocked != on) {
        if (!on)
            g_latched = (1u << XBOX_MAX_CONTROLLERS) - 1u;
        if (on && !rumble_muted())
            rumble_silence();
        g_blocked = on;
    }
    RUMBLE_UNLOCK();
}

void xbox_InputSetBackground(int on)
{
    on = on != 0;
    RUMBLE_LOCK();
    if (g_background != on) {
        if (on && !rumble_muted())
            rumble_silence();
        g_background = on;
    }
    RUMBLE_UNLOCK();
}

void xbox_InputRumbleTick(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !g_inited || !g_rumble[dwPort].live)
        return;
    RUMBLE_LOCK();
    rumble_step(dwPort);
    RUMBLE_UNLOCK();
}

static void input_shutdown(const char *path);

static void shutdown_atexit(void) { input_shutdown("atexit"); }

/* ── the ends atexit does not see ─────────────────────────────────────── */

#if defined(_WIN32)

/* ExitProcess skips atexit: the windows and the kernel's exits run the exit
 * hooks before it (recomp_exit_hook.h). */
static void shutdown_exit_hook(const char *why)
{
    (void)why;
    input_shutdown("exit hook");
}

static void panic_install(void)
{
    recomp_exit_hook_add(shutdown_exit_hook);
}

/* A crash filter's stop: at once if the lock is free (the crashed thread
 * may hold it), else nothing. */
void xbox_InputPanicStop(int wait_ms)
{
    static const XBOX_VIBRATION off = { 0, 0 };

    (void)wait_ms;
    if (!g_inited || !RUMBLE_TRYLOCK())
        return;
    g_shutdown = 1;
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        g_be->set_state(p, &off);
    RUMBLE_UNLOCK();
}

#else

/* A signal handler cannot stop a motor (the host's pad calls take locks and
 * allocate), so a thread that waits on a pipe does it: the handler writes a
 * byte and gives the thread a moment. SIGTERM, SIGINT and SIGHUP end the
 * process as they did, after that moment. */
static int g_panic_pipe[2] = { -1, -1 };
static volatile sig_atomic_t g_panic_done;

static void *panic_thread(void *arg)
{
    char b;

    (void)arg;
    for (;;) {
        ssize_t n = read(g_panic_pipe[0], &b, 1);
        if (n == 1)
            break;
        if (n == 0 || (n < 0 && errno != EINTR))
            return NULL;
    }
    input_shutdown("signal");
    g_panic_done = 1;
    return NULL;
}

/* Async-signal-safe. */
void xbox_InputPanicStop(int wait_ms)
{
    struct timespec ts = { 0, 2000000 };
    int saved = errno;

    if (g_panic_pipe[1] >= 0 && write(g_panic_pipe[1], "x", 1) == 1)
        for (int i = 0; i < wait_ms / 2 && !g_panic_done; i++)
            nanosleep(&ts, NULL);
    errno = saved;
}

static void panic_signal(int sig)
{
    xbox_InputPanicStop(XBOX_RUMBLE_PANIC_MS);
    /* SA_RESETHAND: the default again, and the signal ends the process when
     * this returns, as the shell or the script that sent it expects. */
    raise(sig);
}

static void panic_install(void)
{
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    pthread_t th;

    if (g_panic_pipe[1] >= 0 || pipe(g_panic_pipe) != 0)
        return;
    if (pthread_create(&th, NULL, panic_thread, NULL) != 0) {
        close(g_panic_pipe[0]);
        close(g_panic_pipe[1]);
        g_panic_pipe[0] = g_panic_pipe[1] = -1;
        return;
    }
    pthread_detach(th);
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction old, sa;
        /* Only where nothing else has a say: a host's own handler, or an
         * ignored signal, stays. */
        if (sigaction(sigs[i], NULL, &old) != 0 || (old.sa_flags & SA_SIGINFO)
                || old.sa_handler != SIG_DFL)
            continue;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = panic_signal;
        sa.sa_flags = SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, NULL);
    }
}

#endif

static DWORD probe(DWORD port, XBOX_INPUT_STATE *st)
{
    DWORD rc = g_be->get_state(port, st);
    g_last_probe[port] = g_be->now_ms();
    g_probed[port] = 1;
    g_connected[port] = (rc == ERROR_SUCCESS);
    return rc;
}

/* Whether a probe of a port is due. */
static int due(DWORD port)
{
    return !g_probed[port] || g_connected[port]
        || g_be->now_ms() - g_last_probe[port] >= XBOX_INPUT_PROBE_MS;
}

/* A key of the map went down: the keyboard is the device in use. */
static void keyboard_active(void)
{
    xbox_InputSetActivePad(XBOX_PAD_FAMILY_KEYBOARD, 0);
}

/* A finger went down on an on-screen control: the prompts are the title's
 * own, which the controls' A B X Y match. */
static void touch_active(void)
{
    xbox_InputSetActivePad(XBOX_PAD_FAMILY_NONE, 0);
}

void xbox_InputInit(void)
{
    static int atexit_done;
    XBOX_INPUT_STATE st;

    if (g_inited)
        return;
    g_inited = 1;
    xbox_InputKeyboardSetActivityHook(keyboard_active);
    xbox_InputTouchSetActivityHook(touch_active);
    if (g_be->init)
        g_be->init();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        probe(p, &st);
    /* An exit, a signal or a crash while the title has the motors on must
     * not leave a pad buzzing: atexit for exit() and a return from main, the
     * exit hooks where ExitProcess skips atexit, a thread the signal
     * handlers wake (and a crash handler, xbox_InputPanicStop). */
    if (!atexit_done && g_be == &xbox_input_host_backend) {
        atexit_done = 1;
        atexit(shutdown_atexit);
        panic_install();
    }
}

void xbox_InputSetBackend(const xbox_input_backend *b)
{
    g_be = b ? b : &xbox_input_host_backend;
    g_inited = 0;
    memset(g_connected, 0, sizeof g_connected);
    memset(g_last_probe, 0, sizeof g_last_probe);
    memset(g_probed, 0, sizeof g_probed);
    memset((void *)g_sources, 0, sizeof g_sources);
    memset(g_hk, 0, sizeof g_hk);
    RUMBLE_LOCK();
    memset(g_rumble, 0, sizeof g_rumble);
    g_rumble_params = (xbox_rumble_params)XBOX_RUMBLE_PARAMS_DEFAULT;
    g_rumble_params_read = 0;
    g_background = 0;
    g_shutdown = 0;
    RUMBLE_UNLOCK();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb, tp;
    DWORD rc;
    unsigned src;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();

    rc = due(dwPort) ? probe(dwPort, pState) : ERROR_DEVICE_NOT_CONNECTED;
    if (rc != ERROR_SUCCESS || !g_combo_guard)
        g_hk[dwPort].st = HK_IDLE;
    else
        hotkey_filter(dwPort, pState);
    src = rc == ERROR_SUCCESS ? XBOX_INPUT_SRC_PAD : 0;
    /* The title's pad poll is the rumble's clock. */
    xbox_InputRumbleTick(dwPort);

    /* The keyboard answers for port 0 with or without a pad there, and is
     * merged on top of one: a real pad keeps working, the keys only add. */
    if (dwPort == 0 && g_be->keyboard && g_be->keyboard(&kb)) {
        if (any_held(&kb.Gamepad) || kb.Gamepad.sThumbLX || kb.Gamepad.sThumbLY
                || kb.Gamepad.sThumbRX || kb.Gamepad.sThumbRY)
            src |= XBOX_INPUT_SRC_KEYS;
        if (rc != ERROR_SUCCESS) {
            /* No pad here: the keys are the port's whole state, and go
             * through the overlay's neutral below like a pad's. */
            *pState = kb;
            rc = ERROR_SUCCESS;
        } else {
            /* Merged: a button is held if either holds it, a trigger is the
             * larger of the two, and a held key sets its stick axis (a key
             * is full deflection; with no key the pad's value stays). */
            XBOX_GAMEPAD *g = &pState->Gamepad;
            g->wButtons |= kb.Gamepad.wButtons;
            for (int i = 0; i < 8; i++)
                if (kb.Gamepad.bAnalogButtons[i] > g->bAnalogButtons[i])
                    g->bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
            if (kb.Gamepad.sThumbLX) g->sThumbLX = kb.Gamepad.sThumbLX;
            if (kb.Gamepad.sThumbLY) g->sThumbLY = kb.Gamepad.sThumbLY;
            if (kb.Gamepad.sThumbRX) g->sThumbRX = kb.Gamepad.sThumbRX;
            if (kb.Gamepad.sThumbRY) g->sThumbRY = kb.Gamepad.sThumbRY;
        }
        /* The title's input layer records edges, so an unchanged packet
         * number reads as the same state and a key press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }
    /* The on-screen controls answer for port 0 while touch is on, merged
     * like the keys; a phone with no pad has a player 1 in them. */
    if (dwPort == 0 && xbox_InputTouchState(&tp)) {
        /* While the touch is the device in use, not only while a finger is
         * down: a title's layout choice must not flip at every press. */
        if (any_held(&tp.Gamepad) || tp.Gamepad.sThumbLX || tp.Gamepad.sThumbLY
                || xbox_InputTouchInUse())
            src |= XBOX_INPUT_SRC_TOUCH;
        if (rc != ERROR_SUCCESS) {
            *pState = tp;
            rc = ERROR_SUCCESS;
        } else {
            XBOX_GAMEPAD *g = &pState->Gamepad;
            g->wButtons |= tp.Gamepad.wButtons;
            for (int i = 0; i < 8; i++)
                if (tp.Gamepad.bAnalogButtons[i] > g->bAnalogButtons[i])
                    g->bAnalogButtons[i] = tp.Gamepad.bAnalogButtons[i];
            if (tp.Gamepad.sThumbLX) g->sThumbLX = tp.Gamepad.sThumbLX;
            if (tp.Gamepad.sThumbLY) g->sThumbLY = tp.Gamepad.sThumbLY;
            /* Either one moving moves the number: the sum of the two
             * counters, both of which only go up. */
            pState->dwPacketNumber += tp.dwPacketNumber;
        }
    }
    g_sources[dwPort] = src;
    if (rc == ERROR_SUCCESS && (g_blocked || (g_latched & (1u << dwPort)))) {
        if (!g_blocked && !any_held(&pState->Gamepad))
            g_latched &= ~(1u << dwPort);
        else
            neutral(&pState->Gamepad);
    }
    return rc;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();
    RUMBLE_LOCK();
    /* The old speeds up to now, then the new ones from now: a fall starts
     * here, not at the last tick. Muted (the overlay, the background), the
     * speeds are kept for after. */
    rumble_step(dwPort);
    g_rumble[dwPort].target[0] = pVibration->wLeftMotorSpeed;
    g_rumble[dwPort].target[1] = pVibration->wRightMotorSpeed;
    rc = rumble_step(dwPort);
    RUMBLE_UNLOCK();
    return g_connected[dwPort] ? rc : ERROR_DEVICE_NOT_CONNECTED;
}

/* "path" is which end of the run got here first, for the one line the log
 * keeps of it (a quiet release log shows it: a pad that buzzes on after the
 * game is gone is then a question of which end ran, and what it did). */
static void input_shutdown(const char *path)
{
    if (!g_inited)
        return;
    RUMBLE_LOCK();
    /* Twice is once: atexit, an exit hook and a signal can all get here. */
    if (!g_shutdown) {
        int pads = 0;

        for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
            pads += g_connected[p] ? 1 : 0;
        rumble_silence();
        g_shutdown = 1;
        for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
            memset(g_rumble[p].target, 0, sizeof g_rumble[p].target);
            memset(g_rumble[p].m, 0, sizeof g_rumble[p].m);
        }
        /* The backend's own end: the stop written out and the pads let go,
         * before the process (and the host's rumble queue with it) is gone. */
        if (g_be->shutdown)
            g_be->shutdown();
        XR_NOTICE("[INPUT] rumble stopped on %d pad%s (%s)\n", pads, pads == 1 ? "" : "s", path);
    }
    RUMBLE_UNLOCK();
}

/* The window layers' own end (SDL_QUIT: Cmd+Q, the close button, the
 * overlay's Quit game), before the process is on its way out. */
void xbox_InputShutdown(void)
{
    input_shutdown("quit");
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    XBOX_INPUT_STATE st;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    xbox_InputInit();
    /* Connected ports are refreshed by the title's own reads; only a port
     * not read for a probe interval is asked here. */
    if (g_be->now_ms() - g_last_probe[dwPort] >= XBOX_INPUT_PROBE_MS)
        probe(dwPort, &st);
    return g_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!xbox_InputIsConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}

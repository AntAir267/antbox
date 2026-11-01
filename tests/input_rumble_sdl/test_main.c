/*
 * input_rumble_sdl test: see CMakeLists.txt.
 */
#include "input_backend.h"

#include <SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int xbox_InputAnyOpen(void);     /* src/input/xinput_device.c */

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What the virtual pad was told, in order. A child process writes each
 * record down a pipe too. */
struct rec { Uint32 ms; Uint16 low, high; int triggers; };
static struct rec g_log[1024];
static int g_n, g_pipe = -1;

static void record(Uint16 low, Uint16 high, int triggers)
{
    struct rec r = { SDL_GetTicks(), low, high, triggers };
    if (g_n < (int)(sizeof g_log / sizeof g_log[0]))
        g_log[g_n++] = r;
    if (g_pipe >= 0)
        (void)!write(g_pipe, &r, sizeof r);
}

/* What a virtual pad's callback returns for "done": 0 in SDL2. sdl2-compat
 * (SDL2's API over SDL3, version 2.x.50 and up) hands the value to SDL3 as
 * it is, where the callback is a bool and 0 is a failure: SDL then takes the
 * rumble as not started and never sends its stop. */
static int v_ok(void)
{
    SDL_version v;
    SDL_GetVersion(&v);
    return v.patch >= 50 ? 1 : 0;
}

/* A pad that behaves as SDL's Xbox One driver does (hid/SDL_hidapi_xboxone.c,
 * HIDAPI_DriverXboxOne_UpdateRumble): a rumble is written at once only when
 * the last write is 50 ms old; otherwise it is held back, and the pad is told
 * at SDL's next update after the 50 ms. The call is taken as done either way. */
static int g_holdback;
static int hb_pending;
static Uint16 hb_low, hb_high;
static Uint32 hb_written;
#define HB_BUSY_MS 50

static int SDLCALL v_rumble(void *ud, Uint16 low, Uint16 high)
{
    (void)ud;
    if (g_holdback && hb_written && SDL_GetTicks() - hb_written < HB_BUSY_MS) {
        hb_pending = 1;
        hb_low = low;
        hb_high = high;
        return v_ok();
    }
    hb_pending = 0;
    hb_written = SDL_GetTicks();
    record(low, high, 0);
    return v_ok();
}

static void SDLCALL v_update(void *ud)
{
    (void)ud;
    if (g_holdback && hb_pending && SDL_GetTicks() - hb_written >= HB_BUSY_MS) {
        hb_pending = 0;
        hb_written = SDL_GetTicks();
        record(hb_low, hb_high, 0);
    }
}

static int SDLCALL v_triggers(void *ud, Uint16 left, Uint16 right)
{
    (void)ud;
    record(left, right, 1);
    return v_ok();
}

/* The last motor record (not the triggers'), or NULL. */
static const struct rec *last_motor(const struct rec *log, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (!log[i].triggers)
            return &log[i];
    return NULL;
}

/* The title's poll for ms: the pad read (SDL's update with it) and the
 * rumble's clock. */
static void poll_for(int ms)
{
    XBOX_INPUT_STATE st;
    Uint32 end = SDL_GetTicks() + (Uint32)ms;

    while (SDL_GetTicks() < end) {
        xbox_InputGetState(0, &st);
        SDL_Delay(4);
    }
}

/* SDL with nothing but a virtual pad. 0: a real device is listed (skip). */
static int pad_up(void)
{
    SDL_VirtualJoystickDesc d;

    /* No host driver: a controller on this machine must not be opened, let
     * alone rumbled. The names are SDL2's and SDL3's (sdl2-compat). */
    setenv("SDL_JOYSTICK_HIDAPI", "0", 1);
    setenv("SDL_JOYSTICK_MFI", "0", 1);
    setenv("SDL_JOYSTICK_IOKIT", "0", 1);
    setenv("SDL_JOYSTICK_RAWINPUT", "0", 1);
    setenv("SDL_JOYSTICK_LINUX_CLASSIC", "0", 1);
    /* As the runtime's windows do: SIGINT and SIGTERM stay the process's. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("SKIP: SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    SDL_JoystickUpdate();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (!SDL_JoystickIsVirtual(i)) {
            printf("SKIP: a real joystick is listed (\"%s\"); not touching it\n",
                   SDL_JoystickNameForIndex(i));
            return 0;
        }
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "rumble test pad";
    d.Rumble = v_rumble;
    d.RumbleTriggers = v_triggers;
    d.Update = v_update;
    if (SDL_JoystickAttachVirtualEx(&d) < 0) {
        printf("SKIP: no virtual joystick: %s\n", SDL_GetError());
        return 0;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (!SDL_JoystickIsVirtual(i)) {
            printf("SKIP: a real joystick appeared; not touching it\n");
            return 0;
        }
    if (SDL_NumJoysticks() != 1 || !SDL_IsGameController(0)) {
        printf("SKIP: the virtual pad is not a game controller here\n");
        return 0;
    }
    return 1;
}

enum { END_EXIT, END_SIGTERM, END_KILLED, END_EXIT_HELD_BACK };

/* A process holding a rumble on the virtual pad, ended the given way. */
static int child(int how, struct rec *log, int max, int *status)
{
    int fd[2], n = 0;
    pid_t pid;

    if (pipe(fd) != 0)
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        XBOX_VIBRATION v = { 40000, 30000 };
        close(fd[0]);
        g_pipe = fd[1];
        g_holdback = how == END_EXIT_HELD_BACK;
        if (!pad_up())
            _exit(77);
        xbox_InputInit();
        if (!xbox_InputIsConnected(0))
            _exit(78);
        xbox_InputSetState(0, &v);
        poll_for(120);
        if (how == END_EXIT_HELD_BACK) {
            /* The level changes, and the process is asked to end at once
             * (Cmd+Q in a crash): the stop comes inside the pad's 50 ms. */
            v.wLeftMotorSpeed = 65535;
            xbox_InputSetState(0, &v);
            exit(0);
        }
        if (how == END_EXIT)
            exit(0);
        raise(how == END_SIGTERM ? SIGTERM : SIGKILL);
        _exit(99);
    }
    close(fd[1]);
    while (n < max && read(fd[0], &log[n], sizeof log[n]) == (ssize_t)sizeof log[n])
        n++;
    close(fd[0]);
    waitpid(pid, status, 0);
    return n;
}

int main(void)
{
    XBOX_VIBRATION v = { 40000, 0 };
    const struct rec *m;
    static struct rec log[256];
    int n, status, held, zeros;
    Uint32 t_last, t_end;

    /* The ends of a process first, each in a child of its own (SDL is not
     * up in this process yet, so the children start clean). */
    n = child(END_EXIT, log, 256, &status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
        return 77;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exit(): status 0x%x", status);
    m = last_motor(log, n);
    CHECK(n >= 2 && log[0].low && m && !m->low && !m->high,
          "exit(): the pad was left at %u/%u (%d records)", m ? m->low : 0, m ? m->high : 0, n);

    /* The same, with the pad's own limit of one write per 50 ms: the stop
     * is held back inside SDL and needs an update to go out. */
    n = child(END_EXIT_HELD_BACK, log, 256, &status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "held back: status 0x%x", status);
    m = last_motor(log, n);
    CHECK(n >= 3 && m && !m->low && !m->high,
          "exit() inside the pad's busy time: the pad was left at %u/%u (%d records)",
          m ? m->low : 0, m ? m->high : 0, n);

    n = child(END_SIGTERM, log, 256, &status);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "SIGTERM: status 0x%x", status);
    m = last_motor(log, n);
    CHECK(n >= 2 && log[0].low && m && !m->low && !m->high,
          "SIGTERM: the pad was left at %u/%u (%d records)", m ? m->low : 0, m ? m->high : 0, n);

    /* SIGKILL: nothing can run. The pad's last word is the level (this is
     * the case only the pad's own hardware can end). */
    n = child(END_KILLED, log, 256, &status);
    m = last_motor(log, n);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && m && m->low,
          "SIGKILL: status 0x%x, %d records", status, n);

    if (!pad_up())
        return 77;
    xbox_InputInit();
    CHECK(xbox_InputIsConnected(0) && xbox_InputAnyOpen(), "the virtual pad was not opened");

    /* 1. A held rumble, 800 ms of it: the pad is told the level and never a
     * zero, however often the level is given again. */
    xbox_InputSetState(0, &v);
    poll_for(800);
    held = zeros = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_log[i].triggers)
            continue;
        if (g_log[i].low == 40000 && !g_log[i].high)
            held++;
        else
            zeros++;
    }
    CHECK(held >= 1 && zeros == 0, "held: %d level writes, %d others (a gap or a pulse)", held, zeros);

    /* 2. The giving stops (the title's poll hangs) while SDL goes on: SDL
     * ends the effect itself, inside the limit. */
    t_last = SDL_GetTicks();
    t_end = 0;
    n = g_n;
    while (SDL_GetTicks() - t_last < 600) {
        SDL_GameControllerUpdate();
        m = last_motor(g_log, g_n);
        if (g_n > n && m && !m->low && !m->high) {
            t_end = m->ms;
            break;
        }
        SDL_Delay(2);
    }
    CHECK(t_end && t_end - t_last <= XBOX_RUMBLE_EFFECT_MS + 30,
          "not renewed: the effect %s after %u ms (limit %d)", t_end ? "ended" : "did not end",
          t_end ? (unsigned)(t_end - t_last) : 600u, XBOX_RUMBLE_EFFECT_MS);
    /* The poll is back: so is the level the title still holds. */
    poll_for(XBOX_RUMBLE_REFRESH_MS + 60);
    m = last_motor(g_log, g_n);
    CHECK(m && m->low == 40000, "the held level did not come back (%u)", m ? m->low : 0);

    /* 3. The end of the run: stopped, closed, and nothing after. */
    xbox_InputShutdown();
    m = last_motor(g_log, g_n);
    CHECK(m && !m->low && !m->high, "shutdown: the pad was left at %u/%u", m ? m->low : 0,
          m ? m->high : 0);
    CHECK(!xbox_InputAnyOpen(), "shutdown: a pad is still open");
    n = g_n;
    v.wLeftMotorSpeed = 50000;
    xbox_InputSetState(0, &v);
    poll_for(300);
    CHECK(g_n == n, "shutdown: %d writes to the pad afterwards", g_n - n);
    CHECK(!xbox_InputAnyOpen(), "shutdown: a pad was opened again");

    printf("input_rumble_sdl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

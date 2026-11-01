/*
 * input_map test: see CMakeLists.txt.
 *
 * The expected mapping below is written out independently of the table in
 * src/input/input_map.c (it is the spec's: Black = RB, White = LB, faces 255,
 * triggers 0..255, sticks up/right positive), so a wrong table row fails here
 * rather than being compared with itself.
 */
#include "input_map.h"
#include "input_backend.h"
#include "recomp_env.h"
#include "keyboard.h"
#include "pad_glyphs.h"
#include "recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[128];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();   /* the runtime reads the environment once */
}

/* ── 1. every row, both helpers ────────────────────────────────────────── */

enum { F_BTN, F_ANALOG, F_STICK };
struct expect {
    const char *host;
    uint16_t xi_bit;            /* XInput button, or 0 for an axis */
    int      xi_axis;           /* XBOX_XI_SRC_* when xi_bit == 0 */
    int      sdl;               /* SDL button or axis */
    int      field;             /* F_* */
    int      index;             /* wButtons bit / bAnalogButtons idx / axis */
};

static const struct expect k_expect[] = {
    { "A", XBOX_XI_A, 0, XBOX_SDL_BUTTON_A, F_ANALOG, 0 },
    { "B", XBOX_XI_B, 0, XBOX_SDL_BUTTON_B, F_ANALOG, 1 },
    { "X", XBOX_XI_X, 0, XBOX_SDL_BUTTON_X, F_ANALOG, 2 },
    { "Y", XBOX_XI_Y, 0, XBOX_SDL_BUTTON_Y, F_ANALOG, 3 },
    { "Left shoulder (LB)",  0x0100, 0, 9,  F_ANALOG, 5 },   /* WHITE */
    { "Right shoulder (RB)", 0x0200, 0, 10, F_ANALOG, 4 },   /* BLACK */
    { "Start / Menu",      0x0010, 0, 6,  F_BTN, 0x0010 },
    { "Back / View",       0x0020, 0, 4,  F_BTN, 0x0020 },
    { "Left stick click",  0x0040, 0, 7,  F_BTN, 0x0040 },
    { "Right stick click", 0x0080, 0, 8,  F_BTN, 0x0080 },
    { "D-pad up",          0x0001, 0, 11, F_BTN, 0x0001 },
    { "D-pad down",        0x0002, 0, 12, F_BTN, 0x0002 },
    { "D-pad left",        0x0004, 0, 13, F_BTN, 0x0004 },
    { "D-pad right",       0x0008, 0, 14, F_BTN, 0x0008 },
    { "Left trigger",      0, XBOX_XI_SRC_LT, 4, F_ANALOG, 6 },
    { "Right trigger",     0, XBOX_XI_SRC_RT, 5, F_ANALOG, 7 },
    { "Left stick X",      0, XBOX_XI_SRC_LX, 0, F_STICK, 0 },
    { "Left stick Y",      0, XBOX_XI_SRC_LY, 1, F_STICK, 1 },
    { "Right stick X",     0, XBOX_XI_SRC_RX, 2, F_STICK, 2 },
    { "Right stick Y",     0, XBOX_XI_SRC_RY, 3, F_STICK, 3 },
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static SHORT stick(const XBOX_GAMEPAD *g, int a)
{
    return a == 0 ? g->sThumbLX : a == 1 ? g->sThumbLY
         : a == 2 ? g->sThumbRX : g->sThumbRY;
}

/* Everything in g is at rest except the one field e names, which is want.
 * At rest is 0, except the SDL helper's Y axes: SDL's 0 inverts to -1
 * (design D4 keeps (-1 - v) so -32768 and 32767 swap exactly). */
static void check_only(const char *who, const struct expect *e,
                       const XBOX_GAMEPAD *g, int want)
{
    XBOX_GAMEPAD z;
    memset(&z, 0, sizeof z);
    if (!strcmp(who, "sdl"))
        z.sThumbLY = z.sThumbRY = -1;
    switch (e->field) {
    case F_BTN:
        CHECK(g->wButtons == (WORD)e->index, "%s %s: wButtons 0x%04x, want 0x%04x",
              who, e->host, g->wButtons, e->index);
        z.wButtons = g->wButtons;
        break;
    case F_ANALOG:
        CHECK(g->bAnalogButtons[e->index] == want, "%s %s: bAnalogButtons[%d] = %d, want %d",
              who, e->host, e->index, g->bAnalogButtons[e->index], want);
        z.bAnalogButtons[e->index] = g->bAnalogButtons[e->index];
        break;
    default:
        CHECK(stick(g, e->index) == want, "%s %s: axis %d = %d, want %d",
              who, e->host, e->index, stick(g, e->index), want);
        if (e->index == 0) z.sThumbLX = g->sThumbLX;
        if (e->index == 1) z.sThumbLY = g->sThumbLY;
        if (e->index == 2) z.sThumbRX = g->sThumbRX;
        if (e->index == 3) z.sThumbRY = g->sThumbRY;
        break;
    }
    CHECK(!memcmp(&z, g, sizeof z), "%s %s: other guest fields touched", who, e->host);
}

static void test_rows(void)
{
    CHECK(xbox_input_map_count == N_EXPECT, "table has %u rows, want %u",
          (unsigned)xbox_input_map_count, (unsigned)N_EXPECT);
    for (size_t i = 0; i < N_EXPECT && i < xbox_input_map_count; i++) {
        const struct expect *e = &k_expect[i];
        XBOX_HOST_XINPUT_GAMEPAD xi;
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;

        CHECK(!strcmp(xbox_input_map[i].host, e->host), "row %u is '%s', want '%s'",
              (unsigned)i, xbox_input_map[i].host, e->host);

        memset(&xi, 0, sizeof xi);
        memset(&sd, 0, sizeof sd);
        if (e->field != F_STICK && e->xi_bit) {
            xi.wButtons = e->xi_bit;
            sd.button[e->sdl] = 1;
            xbox_InputMapXInput(&xi, &g);
            check_only("xinput", e, &g, 255);
            xbox_InputMapSdl(&sd, &g);
            check_only("sdl", e, &g, 255);
        } else if (e->field == F_ANALOG) {           /* triggers */
            static const struct { int xi, sdl, want; } t[] = {
                { 0, 0, 0 }, { 128, 16384, 128 }, { 255, 32767, 255 } };
            for (int k = 0; k < 3; k++) {
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                if (e->xi_axis == XBOX_XI_SRC_LT) xi.bLeftTrigger = (uint8_t)t[k].xi;
                else xi.bRightTrigger = (uint8_t)t[k].xi;
                sd.axis[e->sdl] = (int16_t)t[k].sdl;
                xbox_InputMapXInput(&xi, &g);
                check_only("xinput", e, &g, t[k].want);
                xbox_InputMapSdl(&sd, &g);
                check_only("sdl", e, &g, t[k].want);
            }
        } else {                                      /* sticks */
            static const int vals[] = { 32767, -32768, 300, -200, 0 };
            int is_y = e->index == 1 || e->index == 3;
            for (int k = 0; k < 5; k++) {
                int v = vals[k];
                int16_t *xa = e->xi_axis == XBOX_XI_SRC_LX ? &xi.sThumbLX
                            : e->xi_axis == XBOX_XI_SRC_LY ? &xi.sThumbLY
                            : e->xi_axis == XBOX_XI_SRC_RX ? &xi.sThumbRX : &xi.sThumbRY;
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                *xa = (int16_t)v;
                xbox_InputMapXInput(&xi, &g);
                check_only("xinput", e, &g, v);        /* XInput: Y already up */
                /* SDL Y points down: the host value for "guest v" is -1 - v. */
                sd.axis[e->sdl] = (int16_t)(is_y ? -1 - v : v);
                xbox_InputMapSdl(&sd, &g);
                check_only("sdl", e, &g, v);
            }
        }
    }

    /* The two that were swapped before: RB held -> BLACK 255, WHITE 0. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { XBOX_XI_RIGHT_SHOULDER, 0, 0, 0, 0, 0, 0 };
        XBOX_GAMEPAD g;
        xbox_InputMapXInput(&xi, &g);
        CHECK(g.bAnalogButtons[XBOX_BUTTON_BLACK] == 255
              && g.bAnalogButtons[XBOX_BUTTON_WHITE] == 0, "RB is not BLACK");
    }
    /* Full down on the right stick reaches the guest as -32768 (SDL +32767). */
    {
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;
        memset(&sd, 0, sizeof sd);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = 32767;
        xbox_InputMapSdl(&sd, &g);
        CHECK(g.sThumbRY == -32768, "SDL RY full down = %d, want -32768", g.sThumbRY);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = -32768;
        xbox_InputMapSdl(&sd, &g);
        CHECK(g.sThumbRY == 32767, "SDL RY full up = %d, want 32767", g.sThumbRY);
    }
}

/* ── 2. deadzone ───────────────────────────────────────────────────────── */

static void dz(int zone, int x, int y, int wx, int wy)
{
    SHORT sx = (SHORT)x, sy = (SHORT)y;
    xbox_InputApplyDeadzone(&sx, &sy, zone);
    CHECK(sx == wx && sy == wy, "deadzone %d (%d,%d) -> (%d,%d), want (%d,%d)",
          zone, x, y, sx, sy, wx, wy);
}

static void test_deadzone(void)
{
    dz(0, 300, -200, 300, -200);
    dz(0, -32768, 32767, -32768, 32767);
    dz(4000, 300, -200, 0, 0);
    dz(4000, 32767, 0, 32767, 0);
    dz(4000, 0, -32768, 0, -32768);
    dz(4000, -32768, 0, -32768, 0);
    dz(4000, 32767, 32767, 32767, 32767);      /* corners unchanged */
    dz(4000, 4000, 0, 0, 0);                   /* the edge reads 0 */
    {   /* rescaled: halfway between the edge and full reads half */
        SHORT x = (SHORT)(4000 + (32767 - 4000) / 2), y = 0;
        xbox_InputApplyDeadzone(&x, &y, 4000);
        CHECK(x >= 16382 && x <= 16385 && y == 0, "deadzone midpoint -> %d", x);
    }

    /* Through the helpers: unset -> identity; 4000 -> zeroed rest. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { 0, 0, 0, 300, -200, 300, -200 };
        XBOX_GAMEPAD g;
        xbox_InputSetDeadzone(0);
        xbox_InputMapXInput(&xi, &g);
        CHECK(g.sThumbLX == 300 && g.sThumbLY == -200, "no deadzone: (%d,%d)", g.sThumbLX, g.sThumbLY);
        xbox_InputSetDeadzone(4000);
        xbox_InputMapXInput(&xi, &g);
        CHECK(!g.sThumbLX && !g.sThumbLY && !g.sThumbRX && !g.sThumbRY,
              "deadzone 4000 through the helper: (%d,%d)", g.sThumbLX, g.sThumbLY);
    }

    /* RECOMP_PAD_DEADZONE is read once. */
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", "4000");
    CHECK(xbox_InputDeadzone() == 4000, "env deadzone %d", xbox_InputDeadzone());
    set_env("RECOMP_PAD_DEADZONE", "100");
    CHECK(xbox_InputDeadzone() == 4000, "deadzone re-read from env");
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", NULL);
    CHECK(xbox_InputDeadzone() == 0, "default deadzone %d", xbox_InputDeadzone());
    xbox_InputSetDeadzone(0);
}

/* ── 3. README ─────────────────────────────────────────────────────────── */

static void test_readme(const char *path)
{
    char line[512], want[512];
    FILE *f;
    size_t row = 0;
    int in = 0, found = 0;
    const char *out = getenv("INPUT_MAP_README_OUT");

    if (out) {
        FILE *o = fopen(out, "w");
        if (o) {
            fprintf(o, "%s\n", XBOX_INPUT_MAP_README_BEGIN);
            for (size_t i = 0; xbox_InputMapReadmeRow(i, want, sizeof want); i++)
                fprintf(o, "%s\n", want);
            fprintf(o, "%s\n", XBOX_INPUT_MAP_README_END);
            fclose(o);
        }
    }

    f = fopen(path, "r");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!in) {
            if (!strcmp(line, XBOX_INPUT_MAP_README_BEGIN)) in = found = 1;
            continue;
        }
        if (!strcmp(line, XBOX_INPUT_MAP_README_END)) { in = 0; break; }
        if (!xbox_InputMapReadmeRow(row, want, sizeof want)) {
            CHECK(0, "README has an extra table line: %s", line);
            break;
        }
        CHECK(!strcmp(line, want), "README line %u:\n  have: %s\n  want: %s",
              (unsigned)row, line, want);
        row++;
    }
    fclose(f);
    CHECK(found, "README has no '%s' marker", XBOX_INPUT_MAP_README_BEGIN);
    CHECK(!in, "README table is not closed by '%s'", XBOX_INPUT_MAP_README_END);
    CHECK(!xbox_InputMapReadmeRow(row, want, sizeof want),
          "README table is missing line %u: %s", (unsigned)row, want);
}

/* ── 4. connection cache ───────────────────────────────────────────────── */

static uint64_t f_now;
static int f_plugged[4], f_get_calls[4], f_set_calls[4], f_init_calls, f_kb_on;
static XBOX_VIBRATION f_last_vib[4];

static void f_init(void) { f_init_calls++; }
static int f_hold_start = 1, f_hold_back;
static int f_pad_lt, f_pad_lx;      /* a pad's left trigger and stick X, for the keyboard merge */
static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    f_get_calls[p]++;
    if (!f_plugged[p]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    s->Gamepad.wButtons = (f_hold_start ? XBOX_GAMEPAD_START : 0) | (f_hold_back ? XBOX_GAMEPAD_BACK : 0);
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (BYTE)f_pad_lt;
    s->Gamepad.sThumbLX = (SHORT)f_pad_lx;
    return ERROR_SUCCESS;
}
/* Every call that reaches the pad, in order: a motor write (port, levels,
 * when) or the backend's shutdown. test_quit reads it back; a child process
 * of that test writes each record down a pipe as well (f_pipe). */
struct f_rec { int shutdown; DWORD port; WORD l, r; uint64_t ms; };
static struct f_rec f_log[4096];
static int f_n_log, f_shutdown_calls, f_pipe = -1;
static uint64_t f_clock(void);
static void f_record(int shutdown, DWORD p, const XBOX_VIBRATION *v)
{
    struct f_rec rec = { shutdown, p, v ? v->wLeftMotorSpeed : 0, v ? v->wRightMotorSpeed : 0,
                         f_clock() };
    if (f_n_log < (int)(sizeof f_log / sizeof f_log[0]))
        f_log[f_n_log++] = rec;
#ifndef _WIN32
    if (f_pipe >= 0)
        (void)!write(f_pipe, &rec, sizeof rec);
#endif
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v)
{
    f_set_calls[p]++;
    f_last_vib[p] = *v;
    f_record(0, p, v);
    return ERROR_SUCCESS;
}
static void f_shutdown(void)
{
    f_shutdown_calls++;
    f_record(1, 0, NULL);
}
static int f_kb(XBOX_INPUT_STATE *s)
{
    if (!f_kb_on) return 0;
    memset(s, 0, sizeof *s);
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    s->dwPacketNumber = 77;
    return 1;
}
static uint64_t (*f_clock_fn)(void);
static uint64_t f_clock(void) { return f_clock_fn ? f_clock_fn() : f_now; }

static const xbox_input_backend k_fake = { f_init, f_get, f_set, f_kb, f_clock, f_shutdown };

/* input_core.c's default: the same fake pad. Only test_quit's child
 * processes run on it, for the ends input_core.c guards the host backend
 * against (atexit, the signals, a crash handler's stop). */
const xbox_input_backend xbox_input_host_backend = { NULL, f_get, f_set, NULL, f_clock, f_shutdown };

static void test_cache(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 2000, 3000 };  /* over the rumble gate */

    f_now = 5000;
    f_plugged[0] = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    xbox_InputInit();                                 /* idempotent */
    CHECK(f_init_calls == 1, "backend init called %d times", f_init_calls);
    for (int p = 0; p < 4; p++)
        CHECK(f_get_calls[p] == 1, "init probed port %d %d times", p, f_get_calls[p]);
    CHECK(xbox_InputIsConnected(0) && !xbox_InputIsConnected(1), "init mask wrong");

    /* Connected: every call reaches the backend. */
    for (int i = 0; i < 5; i++)
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS, "port 0 read failed");
    CHECK(f_get_calls[0] == 6, "port 0 backend calls %d, want 6", f_get_calls[0]);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START, "port 0 state not passed on");

    /* Disconnected: no backend call until a second after the last probe. */
    for (int i = 0; i < 10; i++) {
        f_now += 99;
        CHECK(xbox_InputGetState(1, &st) == ERROR_DEVICE_NOT_CONNECTED, "port 1 connected?");
        CHECK(!xbox_InputIsConnected(1), "port 1 cache says connected");
    }
    CHECK(f_get_calls[1] == 1, "port 1 probed %d times inside the backoff", f_get_calls[1]);
    f_now += 10;                                      /* 1000 ms since init */
    xbox_InputGetState(1, &st);
    CHECK(f_get_calls[1] == 2, "port 1 not probed after 1 s (%d)", f_get_calls[1]);

    /* Plugged in: the mask follows within a second, through IsConnected. */
    f_plugged[1] = 1;
    f_now += 500;
    CHECK(!xbox_InputIsConnected(1), "port 1 probed before the backoff");
    CHECK(f_get_calls[1] == 2, "IsConnected probed inside the backoff");
    f_now += 500;
    CHECK(xbox_InputIsConnected(1), "port 1 not seen 1 s after plug-in");
    CHECK(f_get_calls[1] == 3, "port 1 probes %d, want 3", f_get_calls[1]);

    /* Unplugged: seen at the next read, then backed off again. */
    f_plugged[0] = 0;
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "unplug not seen");
    CHECK(!xbox_InputIsConnected(0), "cache still connected after unplug");
    {
        int before = f_get_calls[0];
        f_now += 999;
        xbox_InputGetState(0, &st);
        CHECK(f_get_calls[0] == before, "probed an unplugged port inside the backoff");
    }

    /* Rumble: only to connected ports; shutdown stops it, twice is safe. */
    CHECK(xbox_InputSetState(2, &vib) == ERROR_DEVICE_NOT_CONNECTED && !f_set_calls[2],
          "rumble reached a disconnected port");
    CHECK(xbox_InputSetState(1, &vib) == ERROR_SUCCESS && f_set_calls[1] == 1, "rumble lost");
    xbox_InputShutdown();
    CHECK(f_set_calls[1] == 2 && !f_last_vib[1].wLeftMotorSpeed
          && !f_last_vib[1].wRightMotorSpeed, "shutdown did not stop port 1's motors");
    xbox_InputShutdown();

    /* Keyboard on port 0 answers with no pad there. */
    f_kb_on = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && st.dwPacketNumber == 77,
          "keyboard not merged on an empty port 0");
    f_kb_on = 0;

    xbox_InputSetBackend(NULL);
}

/* The overlay's block: neutral while open, and after it until the buttons
 * are let go; the motors silenced. */
static void test_block(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 2000, 3000 };  /* over the rumble gate */

    f_now = 90000;
    f_plugged[0] = 1;
    f_kb_on = 0;
    f_hold_start = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START,
          "block: no pass-through before");
    xbox_InputSetState(0, &vib);
    {
        int sets = f_set_calls[0];
        xbox_InputSetBlocked(1);
        CHECK(xbox_InputBlocked(), "block: not blocked");
        CHECK(f_set_calls[0] == sets + 1 && !f_last_vib[0].wLeftMotorSpeed,
              "block: motors not stopped");
        sets = f_set_calls[0];
        CHECK(xbox_InputSetState(0, &vib) == ERROR_SUCCESS && f_set_calls[0] == sets,
              "block: rumble reached the pad");
    }
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons
          && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_A], "block: state not neutral");
    f_kb_on = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_A],
          "block: keyboard got through");
    f_kb_on = 0;
    /* Closed with START still down: neutral until it is let go. */
    xbox_InputSetBlocked(0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons,
          "block: the closing press reached the title");
    f_hold_start = 0;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons, "block: released");
    f_hold_start = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START,
          "block: still neutral after the release");
    CHECK(xbox_InputSetState(0, &vib) == ERROR_SUCCESS && f_last_vib[0].wLeftMotorSpeed == 2000,
          "block: rumble stayed off after closing");
    xbox_InputSetBackend(NULL);
}

/* The real key map behind input_core.c: the keys alone on port 0, merged
 * with a pad by OR and max, silent while the overlay is open, and the
 * prompts follow the device in use. */
static const xbox_input_backend k_realkb = { f_init, f_get, f_set, xbox_InputKeyboardPoll, f_clock, f_shutdown };

static void test_keyboard_play(void)
{
    XBOX_INPUT_STATE st;

    f_now = 120000;
    f_kb_on = 0;
    f_hold_start = f_hold_back = 0;
    f_pad_lt = f_pad_lx = 0;
    recomp_env_set(RENV_KEYBOARD, NULL);
    xbox_InputKeysClear();
    xbox_InputSetBackend(&k_realkb);

    /* No pad: the keys are port 0. */
    f_plugged[0] = f_plugged[1] = f_plugged[2] = f_plugged[3] = 0;
    xbox_InputKeySet('W', 1);
    xbox_InputKeySet('D', 1);
    xbox_InputKeySet(0x20, 1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255
          && st.Gamepad.sThumbLX == 32767 && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER],
          "keys alone: W D Space");
    CHECK(xbox_InputSources(0) == XBOX_INPUT_SRC_KEYS, "keys alone: sources %u", xbox_InputSources(0));
    CHECK(xbox_InputGetState(1, &st) == ERROR_DEVICE_NOT_CONNECTED, "the keyboard is port 0's only");

    /* A pad on port 0 as well: buttons OR, triggers max, a held key sets
     * its stick axis and a free axis keeps the pad's. */
    f_plugged[0] = 1;
    f_now += 5000;
    f_hold_start = 1;
    f_pad_lt = 90;
    f_pad_lx = -20000;
    xbox_InputKeySet('D', 0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.wButtons == XBOX_GAMEPAD_START
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 90
          && st.Gamepad.sThumbLX == -20000,
          "pad and keys: START %04X LT %d LX %d", st.Gamepad.wButtons,
          st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER], st.Gamepad.sThumbLX);
    xbox_InputKeySet('S', 1);
    xbox_InputKeySet('D', 1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 255 && st.Gamepad.sThumbLX == 32767,
          "pad and keys: a key is full deflection");
    CHECK(xbox_InputSources(0) == (XBOX_INPUT_SRC_PAD | XBOX_INPUT_SRC_KEYS), "both: sources %u",
          xbox_InputSources(0));
    xbox_InputKeysClear();
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 90 && st.Gamepad.sThumbLX == -20000
          && xbox_InputSources(0) == XBOX_INPUT_SRC_PAD, "keys let go: the pad alone");

    /* The overlay open: no pad, held keys neutral, and still neutral after
     * it closes until they are let go. */
    f_plugged[0] = 0;
    f_now += 5000;
    f_hold_start = 0;
    f_pad_lt = f_pad_lx = 0;
    xbox_InputKeySet('W', 1);
    xbox_InputKeySet('A', 1);
    xbox_InputSetBlocked(1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons && !st.Gamepad.sThumbLX
          && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER], "overlay open: the title saw a key");
    xbox_InputSetBlocked(0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
          "overlay closed with W held: still neutral");
    xbox_InputKeysClear();
    xbox_InputGetState(0, &st);             /* nothing held: the latch ends here */
    xbox_InputKeysClear();
    xbox_InputKeySet('W', 1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255,
          "overlay closed, keys let go, W again: not through");

    /* The prompts follow the device: a key, then a pad button. */
    xbox_InputKeysClear();
    xbox_InputSetActivePad(XBOX_PAD_FAMILY_PLAYSTATION, 0);
    xbox_InputKeySet('P', 1);
    CHECK(xbox_InputActivePadFamily() == XBOX_PAD_FAMILY_PLAYSTATION, "an unmapped key took the prompts");
    xbox_InputKeySet(0x0D, 1);
    CHECK(xbox_InputActivePadFamily() == XBOX_PAD_FAMILY_KEYBOARD, "a mapped key did not take the prompts");
    xbox_InputSetActivePad(XBOX_PAD_FAMILY_PLAYSTATION, 0);
    CHECK(xbox_InputActivePadFamily() == XBOX_PAD_FAMILY_PLAYSTATION, "a pad button did not take them back");

    /* Off: no keys at all. */
    xbox_InputKeysClear();
    recomp_env_set(RENV_KEYBOARD, "0");
    xbox_InputKeySet('W', 1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "RECOMP_KEYBOARD=0: the keyboard answered");
    recomp_env_set(RENV_KEYBOARD, NULL);
    xbox_InputKeysClear();
    xbox_InputSetBackend(NULL);
}

/* The rumble curve between the title and a backend (input_core.c): what the
 * fake pad is handed, with the clock in the test's hands. The curve's own
 * numbers are tests/input_rumble's. */
static void rumble_begin(const char *strength, const char *peak)
{
    set_env("RECOMP_INPUT_RUMBLE_GAIN", strength);
    set_env("RECOMP_INPUT_RUMBLE_PEAK", peak);
    memset(f_plugged, 0, sizeof f_plugged);
    f_plugged[0] = 1;
    f_kb_on = 0;
    xbox_InputSetBackend(&k_fake);      /* also: the variables are read again */
    xbox_InputInit();
    memset(f_set_calls, 0, sizeof f_set_calls);
    memset(f_last_vib, 0, sizeof f_last_vib);
}

#define FRAC(w) ((w) / 65535.0)
#define NEAR(w, f) ((w) / 65535.0 > (f) - 0.005 && (w) / 65535.0 < (f) + 0.005)

static void test_rumble(void)
{
    XBOX_VIBRATION v = { 0, 0 };
    int sets;

    /* No variables: the title's speeds as they are (strength 1, peak 65535),
     * but for the gate. */
    f_now = 200000;
    rumble_begin(NULL, NULL);
    v.wLeftMotorSpeed = 30000; v.wRightMotorSpeed = 658;
    CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS && f_set_calls[0] == 1
          && f_last_vib[0].wLeftMotorSpeed == 30000 && f_last_vib[0].wRightMotorSpeed == 0,
          "default: %u/%u", f_last_vib[0].wLeftMotorSpeed, f_last_vib[0].wRightMotorSpeed);

    /* The title's peak and a strength: the decision's table, at the pad. */
    f_now += 5000;
    rumble_begin("1.5", "52054");
    v.wLeftMotorSpeed = 52054; v.wRightMotorSpeed = 18349;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535 && NEAR(f_last_vib[0].wRightMotorSpeed, 0.50),
          "1.5: %.3f/%.3f", FRAC(f_last_vib[0].wLeftMotorSpeed), FRAC(f_last_vib[0].wRightMotorSpeed));
    /* The live setter (the overlay) replaces the variable. */
    xbox_InputSetRumbleStrength(2.0);
    v.wRightMotorSpeed = 52054;                 /* up and down again: a fresh level */
    xbox_InputSetState(0, &v);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    v.wLeftMotorSpeed = 5734; v.wRightMotorSpeed = 1791;
    xbox_InputSetState(0, &v);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    CHECK(NEAR(f_last_vib[0].wLeftMotorSpeed, 0.33) && NEAR(f_last_vib[0].wRightMotorSpeed, 0.19),
          "2.0: %.3f/%.3f", FRAC(f_last_vib[0].wLeftMotorSpeed), FRAC(f_last_vib[0].wRightMotorSpeed));
    /* Strength 0: off, and the pad is told. */
    xbox_InputSetRumbleStrength(0.0);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    CHECK(!f_last_vib[0].wLeftMotorSpeed && !f_last_vib[0].wRightMotorSpeed, "strength 0 not off");
    /* A spelling that is no number ("linear" was one once): the default. */
    f_now += 5000;
    rumble_begin("linear", NULL);
    v.wLeftMotorSpeed = 30000; v.wRightMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 30000, "no number: %u", f_last_vib[0].wLeftMotorSpeed);

    /* The release, by the pad poll's clock: full, then the title's zero. */
    f_now += 5000;
    rumble_begin("1", "52054");
    v.wLeftMotorSpeed = 52054; v.wRightMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "release: no rise");
    f_now += 1000;
    xbox_InputRumbleTick(0);                    /* held: as it was */
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "a held level moved");
    v.wLeftMotorSpeed = 0;
    sets = f_set_calls[0];
    xbox_InputSetState(0, &v);
    CHECK(f_set_calls[0] == sets && f_last_vib[0].wLeftMotorSpeed == 65535,
          "the zero cut the motor at once");
    for (int i = 0; i < 90; i++) {              /* a 1 ms poll */
        XBOX_INPUT_STATE st;
        f_now += 1;
        if (i & 1)
            xbox_InputRumbleTick(0);
        else
            xbox_InputGetState(0, &st);         /* the title's own poll ticks too */
    }
    CHECK(f_last_vib[0].wLeftMotorSpeed / 65535.0 > 0.36
          && f_last_vib[0].wLeftMotorSpeed / 65535.0 < 0.44,
          "90 ms after the zero: %.3f", FRAC(f_last_vib[0].wLeftMotorSpeed));
    /* A falling level is not written to the pad at every poll. */
    CHECK(f_set_calls[0] - sets >= 4 && f_set_calls[0] - sets <= 7,
          "%d writes in 90 ms of falling", f_set_calls[0] - sets);
    /* A rise in the fall goes out at once. */
    v.wLeftMotorSpeed = 52054;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "rise in a fall held back");
    /* And the fall ends in a zero that is sent, after which nothing is. */
    v.wLeftMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(!f_last_vib[0].wLeftMotorSpeed && !f_last_vib[0].wRightMotorSpeed, "no final zero");
    sets = f_set_calls[0];
    for (int i = 0; i < 1000; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(f_set_calls[0] == sets, "%d writes to a stopped pad", f_set_calls[0] - sets);

    /* A held rumble is given again every 150 ms (SDL's rumble runs out). */
    v.wLeftMotorSpeed = 20000;
    xbox_InputSetState(0, &v);
    sets = f_set_calls[0];
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(f_set_calls[0] - sets == 600 / XBOX_RUMBLE_REFRESH_MS, "%d refreshes in 600 ms",
          f_set_calls[0] - sets);

    /* The window out of focus: a zero, then nothing, the refresh included;
     * back in front, what the title last asked for plays again. */
    sets = f_set_calls[0];
    xbox_InputSetBackground(1);
    CHECK(f_set_calls[0] == sets + 1 && !f_last_vib[0].wLeftMotorSpeed, "background: no stop");
    sets = f_set_calls[0];
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    v.wLeftMotorSpeed = 52054;
    CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS && f_set_calls[0] == sets,
          "background: %d writes", f_set_calls[0] - sets);
    xbox_InputSetBackground(1);                 /* twice is once */
    CHECK(f_set_calls[0] == sets, "background twice");
    xbox_InputSetBackground(0);
    f_now += 1;
    xbox_InputRumbleTick(0);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "foreground: %u", f_last_vib[0].wLeftMotorSpeed);
    /* The overlay over a window out of focus: silent until both are over. */
    xbox_InputSetBlocked(1);
    xbox_InputSetBackground(1);
    xbox_InputSetBlocked(0);
    {
        /* (The block's latch on the buttons, let go: not this test's.) */
        XBOX_INPUT_STATE st;
        f_hold_start = 0;
        xbox_InputGetState(0, &st);
        f_hold_start = 1;
    }
    f_now += 200;
    xbox_InputRumbleTick(0);
    CHECK(!f_last_vib[0].wLeftMotorSpeed, "unblocked in the background: rumble");
    xbox_InputSetBackground(0);
    f_now += 1;
    xbox_InputRumbleTick(0);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "both over: no rumble");

    /* No pad: the title's speeds are kept, and a pad that appears gets them. */
    f_now += 5000;
    rumble_begin("1", NULL);
    v.wLeftMotorSpeed = 40000; v.wRightMotorSpeed = 0;
    CHECK(xbox_InputSetState(1, &v) == ERROR_DEVICE_NOT_CONNECTED && !f_set_calls[1],
          "rumble reached an empty port");
    f_plugged[1] = 1;
    f_now += 1000;
    CHECK(xbox_InputIsConnected(1), "port 1 not seen");
    xbox_InputRumbleTick(1);
    CHECK(f_set_calls[1] == 1 && f_last_vib[1].wLeftMotorSpeed == 40000,
          "a pad that appeared: %d writes, %u", f_set_calls[1], f_last_vib[1].wLeftMotorSpeed);

    /* Shutdown: every motor off and forgotten. */
    xbox_InputShutdown();
    CHECK(!f_last_vib[1].wLeftMotorSpeed, "shutdown left port 1 on");
    sets = f_set_calls[1];
    f_now += 1000;
    xbox_InputRumbleTick(1);
    CHECK(f_set_calls[1] == sets, "rumble after shutdown");

    set_env("RECOMP_INPUT_RUMBLE_GAIN", NULL);
    set_env("RECOMP_INPUT_RUMBLE_PEAK", NULL);
    xbox_InputSetBackend(NULL);
}

/* ── the end of a run ────────────────────────────────────────────────────
 *
 * A pad must never be left shaking. Every way out that can still run code
 * ends in a motor write of zero and then the backend's shutdown, with no
 * motor write after it; and a way out that runs nothing leaves a level the
 * host plays for XBOX_RUMBLE_EFFECT_MS at most. */

/* What the pad plays, from the log: a level given at t holds until the next
 * write or t + XBOX_RUMBLE_EFFECT_MS, whichever is first (the host's rumble
 * effect; xinput_device.c gives SDL that duration). The longest stretch of
 * silence between from and to, while the level asked for was not zero. */
static uint64_t quit_longest_gap(int first, uint64_t from, uint64_t to)
{
    uint64_t gap = 0, covered = from;

    for (int i = first; i < f_n_log; i++) {
        const struct f_rec *r = &f_log[i];
        if (r->shutdown || r->port != 0)
            continue;
        if (r->ms > covered && r->ms - covered > gap)
            gap = r->ms - covered;
        covered = (r->l | r->r) ? r->ms + XBOX_RUMBLE_EFFECT_MS : r->ms;
    }
    if (to > covered && to - covered > gap)
        gap = to - covered;
    return gap;
}

/* The log ends: ... a zero to every port that had a level, the shutdown,
 * and nothing after. */
static int quit_ended_stopped(const struct f_rec *log, int n)
{
    WORD on[XBOX_MAX_CONTROLLERS] = { 0 };
    int i, shut = -1;

    for (i = 0; i < n; i++) {
        if (log[i].shutdown) {
            if (shut < 0)
                shut = i;
            continue;
        }
        if (shut >= 0)
            return 0;                   /* a motor write after the shutdown */
        on[log[i].port] = log[i].l | log[i].r;
    }
    if (shut < 0)
        return 0;
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (on[i])
            return 0;                   /* a port left with a level */
    return 1;
}

static void quit_begin(void)
{
    f_now += 5000;
    rumble_begin("1.5", "52054");
    f_n_log = 0;
    f_shutdown_calls = 0;
}

#ifndef _WIN32
static uint64_t f_real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void quit_crash_handler(int sig)
{
    (void)sig;
    xbox_InputPanicStop(500);           /* what a host's crash handler calls */
    _exit(9);
}

enum { QUIT_EXIT, QUIT_RETURN_FROM_MAIN, QUIT_SIGTERM, QUIT_SIGINT, QUIT_SIGHUP,
       QUIT_CRASH, QUIT_KILLED };

/* A process on the host backend (the fake pad), rumbling, ended the given
 * way; what its pad was sent comes back down a pipe. */
static int quit_child(int how, struct f_rec *log, int max, int *status)
{
    int fd[2], n = 0;
    pid_t pid;

    if (pipe(fd) != 0)
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        XBOX_VIBRATION v = { 52054, 20000 };
        XBOX_INPUT_STATE st;
        close(fd[0]);
        f_pipe = fd[1];
        f_n_log = 0;
        memset(f_plugged, 0, sizeof f_plugged);
        f_plugged[0] = f_plugged[1] = 1;
        xbox_InputSetBackend(NULL);     /* the host backend: the guards go in */
        xbox_InputInit();
        xbox_InputSetState(0, &v);
        xbox_InputSetState(1, &v);
        f_now += 20;
        xbox_InputGetState(0, &st);
        switch (how) {
        case QUIT_EXIT:    exit(0);
        case QUIT_SIGTERM: raise(SIGTERM); break;
        case QUIT_SIGINT:  raise(SIGINT); break;
        case QUIT_SIGHUP:  raise(SIGHUP); break;
        case QUIT_CRASH:
            signal(SIGSEGV, quit_crash_handler);
            raise(SIGSEGV);
            break;
        case QUIT_KILLED:  raise(SIGKILL); break;
        default: break;
        }
        if (how == QUIT_RETURN_FROM_MAIN)
            exit(0);                    /* what returning from main does */
        _exit(99);                      /* a signal that did not end it */
    }
    close(fd[1]);
    while (n < max && read(fd[0], &log[n], sizeof log[n]) == (ssize_t)sizeof log[n])
        n++;
    close(fd[0]);
    waitpid(pid, status, 0);
    return n;
}
#endif

static void test_quit(void)
{
    XBOX_VIBRATION v = { 52054, 0 };
    XBOX_INPUT_STATE st;
    int sets, first;
    uint64_t t0;

    /* 1. Shutdown in the middle of a rumble (a window closed, the title's
     * own quit: both reach xbox_InputShutdown). */
    quit_begin();
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "quit: no rumble to stop");
    xbox_InputShutdown();
    CHECK(quit_ended_stopped(f_log, f_n_log) && f_shutdown_calls == 1,
          "quit: not stopped (%d records, %d shutdowns)", f_n_log, f_shutdown_calls);
    /* The title's threads run on until the process is gone: the USB pad
     * gives its held speeds again every 150 ms, the poll ticks. None of it
     * may reach the pad. */
    sets = f_n_log;
    for (int i = 0; i < 2000; i++) {
        f_now += 1;
        if (i % 150 == 0)
            xbox_InputSetState(0, &v);
        xbox_InputRumbleTick(0);
        xbox_InputGetState(0, &st);
    }
    v.wLeftMotorSpeed = 30000; v.wRightMotorSpeed = 30000;
    xbox_InputSetState(0, &v);
    xbox_InputSetBlocked(1); xbox_InputSetBlocked(0);
    xbox_InputSetBackground(1); xbox_InputSetBackground(0);
    f_hold_start = 0;                           /* (the block's latch, let go) */
    xbox_InputGetState(0, &st);
    f_hold_start = 1;
    f_now += 300;
    xbox_InputRumbleTick(0);
    CHECK(f_n_log == sets, "quit: %d calls reached the pad after the shutdown", f_n_log - sets);
    /* atexit after the window's own call, a signal after that: once. */
    xbox_InputShutdown();
    xbox_InputShutdown();
    CHECK(f_shutdown_calls == 1 && f_n_log == sets, "quit: shut down %d times", f_shutdown_calls);

    /* 2. The same from every state the motors can be silent or falling in. */
    v.wLeftMotorSpeed = 52054; v.wRightMotorSpeed = 0;
    quit_begin();                               /* the overlay open */
    xbox_InputSetState(0, &v);
    xbox_InputSetBlocked(1);
    xbox_InputShutdown();
    CHECK(quit_ended_stopped(f_log, f_n_log), "quit in the overlay: not stopped");
    xbox_InputSetBlocked(0);
    f_now += 200;
    xbox_InputRumbleTick(0);
    CHECK(quit_ended_stopped(f_log, f_n_log), "quit in the overlay: rumble after closing it");
    f_hold_start = 0;                           /* (the block's latch, let go) */
    xbox_InputGetState(0, &st);
    f_hold_start = 1;
    quit_begin();                               /* the window not in front */
    xbox_InputSetState(0, &v);
    xbox_InputSetBackground(1);
    xbox_InputShutdown();
    xbox_InputSetBackground(0);
    f_now += 200;
    xbox_InputRumbleTick(0);
    CHECK(quit_ended_stopped(f_log, f_n_log), "quit in the background: not stopped");
    quit_begin();                               /* the release, after the title's zero */
    xbox_InputSetState(0, &v);
    v.wLeftMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    f_now += 30;
    xbox_InputRumbleTick(0);
    CHECK(f_last_vib[0].wLeftMotorSpeed != 0, "quit in the release: already silent");
    xbox_InputShutdown();
    CHECK(quit_ended_stopped(f_log, f_n_log), "quit in the release: not stopped");
    quit_begin();                               /* nothing playing: still let go */
    xbox_InputShutdown();
    CHECK(quit_ended_stopped(f_log, f_n_log) && f_shutdown_calls == 1, "quit at rest");

    /* 3. No way out at all (the process killed outright, a pad poll that
     * hangs): what the pad was last given has an end. A held level is given
     * again before the host's effect runs out, whatever the poll's rate, so
     * the pad plays without a gap; and once the giving stops the last
     * effect is over XBOX_RUMBLE_EFFECT_MS later, which is the limit. */
    CHECK(XBOX_RUMBLE_EFFECT_MS <= 250, "the effect's limit grew: %d ms", XBOX_RUMBLE_EFFECT_MS);
    CHECK(XBOX_RUMBLE_REFRESH_MS * 2 <= XBOX_RUMBLE_EFFECT_MS, "no margin between refresh and effect");
    {
        static const int poll_ms[] = { 1, 4, 16, 33, 50 };
        for (unsigned k = 0; k < sizeof poll_ms / sizeof poll_ms[0]; k++) {
            uint64_t gap, last = 0;
            int writes = 0;
            v.wLeftMotorSpeed = 40000; v.wRightMotorSpeed = 0;
            quit_begin();
            first = f_n_log;
            t0 = f_now;
            xbox_InputSetState(0, &v);
            for (int t = 0; t < 3000; t += poll_ms[k]) {
                f_now += (uint64_t)poll_ms[k];
                xbox_InputRumbleTick(0);
            }
            gap = quit_longest_gap(first, t0, f_now);
            CHECK(gap == 0, "poll %d ms: the pad was silent for %llu ms of a held rumble",
                  poll_ms[k], (unsigned long long)gap);
            /* The held level never changes: nothing that could pulse. */
            for (int i = first; i < f_n_log; i++) {
                CHECK(f_log[i].l == f_log[first].l && f_log[i].r == 0,
                      "poll %d ms: the held level moved (%u)", poll_ms[k], f_log[i].l);
                last = f_log[i].ms;
                writes++;
            }
            CHECK(writes >= 3000 / (XBOX_RUMBLE_REFRESH_MS + poll_ms[k]),
                  "poll %d ms: %d writes in 3 s", poll_ms[k], writes);
            /* The process dies here: no tick, no shutdown. */
            CHECK(f_now - last < XBOX_RUMBLE_REFRESH_MS + (uint64_t)poll_ms[k],
                  "poll %d ms: last write %llu ms old", poll_ms[k], (unsigned long long)(f_now - last));
        }
    }
    /* The curve is BO-98's, untouched by the re-arming: the decision's
     * levels, and a release that still falls through them. */
    quit_begin();
    v.wLeftMotorSpeed = 18349; v.wRightMotorSpeed = 5734;
    xbox_InputSetState(0, &v);
    CHECK(NEAR(f_last_vib[0].wLeftMotorSpeed, 0.50) && NEAR(f_last_vib[0].wRightMotorSpeed, 0.23),
          "curve: %.3f/%.3f", FRAC(f_last_vib[0].wLeftMotorSpeed), FRAC(f_last_vib[0].wRightMotorSpeed));
    xbox_InputSetBackend(NULL);

#ifndef _WIN32
    /* 4. A real process on the host backend, ended each way while its pads
     * rumble. Whatever can still run code ends stopped; the signals still
     * end the process as signals. */
    {
        static const struct { int how; const char *name; int sig; } way[] = {
            { QUIT_EXIT, "exit()", 0 },
            { QUIT_RETURN_FROM_MAIN, "return from main", 0 },
            { QUIT_SIGTERM, "SIGTERM", SIGTERM },
            { QUIT_SIGINT, "SIGINT", SIGINT },
            { QUIT_SIGHUP, "SIGHUP", SIGHUP },
            { QUIT_CRASH, "a crash handler", 0 },
        };
        static struct f_rec log[64];
        int status, n;

        f_clock_fn = f_real_ms;                 /* the children wait in real time */
        for (unsigned k = 0; k < sizeof way / sizeof way[0]; k++) {
            n = quit_child(way[k].how, log, 64, &status);
            CHECK(n >= 3 && (log[0].l | log[0].r), "%s: the child did not rumble (%d records)",
                  way[k].name, n);
            CHECK(quit_ended_stopped(log, n), "%s: the pads were left on (%d records)", way[k].name, n);
            if (way[k].sig)
                CHECK(WIFSIGNALED(status) && WTERMSIG(status) == way[k].sig,
                      "%s: ended with status 0x%x, not by the signal", way[k].name, status);
            else
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 99, "%s: status 0x%x",
                      way[k].name, status);
        }
        /* SIGKILL: nothing runs. The pad's last word is a level, and what
         * ends it is the effect's own time. */
        n = quit_child(QUIT_KILLED, log, 64, &status);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "SIGKILL: status 0x%x", status);
        CHECK(n >= 2 && !quit_ended_stopped(log, n) && (log[n - 1].l | log[n - 1].r),
              "SIGKILL: a stop was sent?");
        f_clock_fn = NULL;
    }
#endif
}

/* The exit hooks (recomp_exit_hook.h): where ExitProcess skips atexit, the
 * windows and the kernel's exits run them, and the pads' stop is one. */
static int f_hook_a, f_hook_b;
static void f_hook_fn_a(const char *why) { (void)why; f_hook_a++; }
static void f_hook_fn_b(const char *why) { (void)why; f_hook_b++; }
static void test_exit_hooks(void)
{
    recomp_exit_hook_set(f_hook_fn_a);
    recomp_exit_hook_add(f_hook_fn_b);
    recomp_exit_hook_add(f_hook_fn_b);          /* twice is once */
    recomp_exit_hook_run("test");
    CHECK(f_hook_a == 1 && f_hook_b == 1, "exit hooks ran %d and %d times", f_hook_a, f_hook_b);
    recomp_exit_hook_set(NULL);
    recomp_exit_hook_run("test");
    CHECK(f_hook_a == 1 && f_hook_b == 2, "added hook lost with the first");
}

/* The overlay's hotkey filter (Back+Start): a backend whose pads are driven
 * per port, with a packet number that moves on every change as a real one. */
static WORD h_btn[4];
static DWORD h_pkt[4];
static DWORD h_get(DWORD p, XBOX_INPUT_STATE *s)
{
    static WORD last[4];
    if (!f_plugged[p]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    if (last[p] != h_btn[p]) { last[p] = h_btn[p]; h_pkt[p]++; }
    s->Gamepad.wButtons = h_btn[p];
    s->dwPacketNumber = h_pkt[p];
    return ERROR_SUCCESS;
}
static const xbox_input_backend k_hot = { f_init, h_get, f_set, f_kb, f_clock, f_shutdown };

/* One title read of a port at time t (ms after the test's origin). */
static WORD hk_read(DWORD port, uint64_t t, DWORD *pkt)
{
    XBOX_INPUT_STATE st;
    f_now = 300000 + t;
    CHECK(xbox_InputGetState(port, &st) == ERROR_SUCCESS, "hotkey: read failed");
    if (pkt) *pkt = st.dwPacketNumber;
    return st.Gamepad.wButtons;
}

/* The title's view over a stretch of 16 ms frames: the buttons ever seen, and
 * whether the packet number moved wherever the buttons did. */
static WORD hk_run(DWORD port, uint64_t from, uint64_t to, int *edge_ok, WORD *last_btn, DWORD *last_pkt)
{
    WORD seen = 0;
    for (uint64_t t = from; t < to; t += 16) {
        DWORD pkt;
        WORD b = hk_read(port, t, &pkt);
        seen |= b;
        if (b != *last_btn && pkt == *last_pkt) *edge_ok = 0;
        *last_btn = b;
        *last_pkt = pkt;
    }
    return seen;
}

static void hk_begin(void)
{
    memset(h_btn, 0, sizeof h_btn);
    f_plugged[0] = f_plugged[1] = 1;
    f_kb_on = 0;
    xbox_InputSetBackend(&k_hot);
    xbox_InputInit();
    xbox_InputSetBlocked(0);
    xbox_InputSetComboGuard(1);
    /* A closed overlay leaves its ports latched until seen with nothing held. */
    hk_read(0, 0, NULL);
    hk_read(1, 0, NULL);
}

static void test_combo_guard(void)
{
    WORD last = 0, seen;
    DWORD lp = 0, pkt;
    int edge_ok = 1;

    /* Guard off: the pad is passed as it is. */
    hk_begin();
    xbox_InputSetComboGuard(0);
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
    CHECK(hk_read(0, 0, NULL) == (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START), "guard off: the buttons were changed");
    xbox_InputSetComboGuard(1);
    h_btn[0] = 0;
    hk_read(0, 16, NULL);

    /* A tap: nothing while it is down, a press of >= TAP_MS (and >= 2 reads)
     * once it is let go, then released again, each a change of packet. */
    h_btn[0] = XBOX_GAMEPAD_BACK;
    seen = hk_run(0, 1000, 1096, &edge_ok, &last, &lp);
    CHECK(!seen, "tap: Back reached the title while it was down (%04X)", seen);
    h_btn[0] = 0;
    seen = hk_run(0, 1096, 1112, &edge_ok, &last, &lp);
    CHECK(seen == XBOX_GAMEPAD_BACK, "tap: no Back after the release (%04X)", seen);
    hk_run(0, 1112, 1200, &edge_ok, &last, &lp);
    seen = hk_run(0, 1200, 1400, &edge_ok, &last, &lp);
    CHECK(!seen && !last, "tap: Back was held on (%04X)", seen);
    CHECK(edge_ok, "tap: a change of buttons came with the same packet number");
    {
        /* How long the press lasted and how many reads carried it. */
        int n = 0;
        uint64_t first = 0, lastms = 0;
        hk_begin();
        h_btn[0] = XBOX_GAMEPAD_BACK;
        hk_read(0, 0, NULL);
        h_btn[0] = 0;
        for (uint64_t t = 16; t < 400; t += 16)
            if (hk_read(0, t, NULL) & XBOX_GAMEPAD_BACK) { if (!n) first = t; lastms = t; n++; }
        CHECK(n >= 2 && lastms - first + 16 >= XBOX_INPUT_BACK_TAP_MS,
              "tap: Back lasted %d reads, %u ms", n, (unsigned)(lastms - first + 16));
        /* A slow title (one read every 100 ms) still sees it on two reads. */
        hk_begin();
        h_btn[0] = XBOX_GAMEPAD_BACK;
        hk_read(0, 0, NULL);
        h_btn[0] = 0;
        n = 0;
        for (uint64_t t = 100; t < 1000; t += 100)
            if (hk_read(0, t, NULL) & XBOX_GAMEPAD_BACK) n++;
        CHECK(n >= 2, "tap: a slow title saw Back on %d reads", n);
    }

    /* A hold: hidden until XBOX_INPUT_BACK_DELAY_MS, then passed on as held. */
    hk_begin();
    last = 0; lp = 0; edge_ok = 1;
    h_btn[0] = XBOX_GAMEPAD_BACK;
    seen = hk_run(0, 2000, 2000 + XBOX_INPUT_BACK_DELAY_MS - 8, &edge_ok, &last, &lp);
    CHECK(!seen, "hold: Back arrived early (%04X)", seen);
    seen = hk_run(0, 2000 + XBOX_INPUT_BACK_DELAY_MS, 2300, &edge_ok, &last, &lp);
    CHECK(last == XBOX_GAMEPAD_BACK, "hold: Back not passed after %d ms (%04X)", XBOX_INPUT_BACK_DELAY_MS, last);
    CHECK(edge_ok, "hold: Back came with the same packet number");
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
    CHECK(hk_read(0, 2400, NULL) == XBOX_GAMEPAD_BACK, "hold: Start reached the title with Back held");
    h_btn[0] = 0;
    CHECK(hk_read(0, 2416, NULL) == 0, "hold: Back stuck after the release");
    CHECK(hk_read(0, 2600, NULL) == 0, "hold: a tap followed a hold");

    /* Back, then Start: the title sees neither, and nothing till both go. */
    hk_begin();
    h_btn[0] = XBOX_GAMEPAD_BACK;
    seen = hk_read(0, 3000, NULL) | hk_read(0, 3016, NULL);
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
    seen |= hk_read(0, 3032, NULL) | hk_read(0, 3500, NULL);
    CHECK(!seen, "combo: the title saw %04X", seen);
    xbox_InputSetBlocked(1);                          /* the overlay opens */
    h_btn[0] = XBOX_GAMEPAD_START;                    /* Back let go first */
    seen = hk_read(0, 3600, NULL);
    h_btn[0] = 0;
    seen |= hk_read(0, 3616, NULL);
    xbox_InputSetBlocked(0);                          /* and closes */
    seen |= hk_read(0, 3700, NULL) | hk_read(0, 3900, NULL);
    CHECK(!seen, "combo: the title saw %04X around the overlay", seen);

    /* Both in one read, and the overlay closed by the combo (Start let go
     * last): still nothing for the title. */
    hk_begin();
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
    seen = hk_read(0, 4000, NULL);
    xbox_InputSetBlocked(1);
    seen |= hk_read(0, 4100, NULL);
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
    seen |= hk_read(0, 4200, NULL);
    xbox_InputSetBlocked(0);
    seen |= hk_read(0, 4300, NULL);
    h_btn[0] = XBOX_GAMEPAD_START;
    seen |= hk_read(0, 4400, NULL);
    h_btn[0] = 0;
    seen |= hk_read(0, 4500, NULL) | hk_read(0, 4600, NULL);
    CHECK(!seen, "combo, same read: the title saw %04X", seen);

    /* Start first is not delayed. */
    hk_begin();
    h_btn[0] = XBOX_GAMEPAD_START;
    CHECK(hk_read(0, 5000, NULL) == XBOX_GAMEPAD_START, "Start first was delayed");

    /* Two pads are independent: pad 1's tap and pad 0's combo. */
    hk_begin();
    h_btn[0] = XBOX_GAMEPAD_BACK;
    h_btn[1] = XBOX_GAMEPAD_BACK;
    seen = hk_read(0, 6000, NULL) | hk_read(1, 6000, NULL);
    h_btn[0] = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;     /* pad 0 opens it */
    h_btn[1] = 0;                                          /* pad 1 taps */
    seen |= hk_read(0, 6016, NULL);
    pkt = 0;
    CHECK(!seen, "two pads: the combo of pad 0 leaked %04X", seen);
    CHECK(hk_read(1, 6016, &pkt) == XBOX_GAMEPAD_BACK, "two pads: pad 1's tap was lost");
    CHECK(hk_read(0, 6032, NULL) == 0, "two pads: pad 0 showed a button");
    hk_read(1, 6400, NULL);                         /* its second read of the tap */
    CHECK(hk_read(1, 6416, NULL) == 0, "two pads: pad 1's tap stuck");

    /* A pad pulled out in the middle starts over. */
    h_btn[0] = XBOX_GAMEPAD_BACK;
    hk_read(0, 7000, NULL);
    f_plugged[0] = 0;
    f_now = 300000 + 7016;
    { XBOX_INPUT_STATE st; xbox_InputGetState(0, &st); }
    f_plugged[0] = 1;
    h_btn[0] = 0;
    CHECK(hk_read(0, 9000, NULL) == 0, "unplug: a tap came from a pad that was gone");

    xbox_InputSetComboGuard(0);
    xbox_InputSetBackend(NULL);
}

static void test_trigger_deadzone(void)
{
    XBOX_HOST_XINPUT_GAMEPAD in;
    XBOX_GAMEPAD out;

    memset(&in, 0, sizeof in);
    in.bLeftTrigger = 20;
    in.bRightTrigger = 255;
    xbox_InputSetTriggerDeadzone(0);
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 20, "trigger dz 0 is not the identity");
    xbox_InputSetTriggerDeadzone(30);
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0, "trigger under the deadzone reads");
    CHECK(out.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "full pull not full");
    in.bLeftTrigger = 142;                              /* (142 - 30) * 255 / 225 */
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 126, "trigger rescale %d",
          out.bAnalogButtons[XBOX_BUTTON_LTRIGGER]);
    xbox_InputSetTriggerDeadzone(0);
}

/* The Nintendo face layout: A and B, X and Y change places, nothing else. */
static void test_face_layout(void)
{
    XBOX_HOST_XINPUT_GAMEPAD xi;
    XBOX_HOST_SDL_GAMEPAD sdl;
    XBOX_GAMEPAD out;

    CHECK(!xbox_InputFaceLayoutNintendo(), "the default is the Xbox layout");
    memset(&xi, 0, sizeof xi);
    xi.wButtons = XBOX_XI_A | XBOX_XI_X | XBOX_XI_START | XBOX_XI_DPAD_UP;
    xbox_InputMirrorFaceXInput(&xi);
    CHECK(xi.wButtons == (XBOX_XI_B | XBOX_XI_Y | XBOX_XI_START | XBOX_XI_DPAD_UP),
          "xinput mirror %04X", xi.wButtons);
    xbox_InputMirrorFaceXInput(&xi);
    CHECK(xi.wButtons == (XBOX_XI_A | XBOX_XI_X | XBOX_XI_START | XBOX_XI_DPAD_UP),
          "xinput mirror twice %04X", xi.wButtons);
    memset(&sdl, 0, sizeof sdl);
    sdl.button[XBOX_SDL_BUTTON_B] = 1;          /* the button printed A on such a pad */
    sdl.button[XBOX_SDL_BUTTON_Y] = 1;
    xbox_InputMirrorFaceSdl(&sdl);
    CHECK(sdl.button[XBOX_SDL_BUTTON_A] && !sdl.button[XBOX_SDL_BUTTON_B]
          && sdl.button[XBOX_SDL_BUTTON_X] && !sdl.button[XBOX_SDL_BUTTON_Y], "sdl mirror");
    xbox_InputMapSdl(&sdl, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_A] == 255 && out.bAnalogButtons[XBOX_BUTTON_B] == 0
          && out.bAnalogButtons[XBOX_BUTTON_X] == 255 && out.bAnalogButtons[XBOX_BUTTON_Y] == 0,
          "the title's A and X after the mirror");
    xbox_InputSetFaceLayoutNintendo(1);
    CHECK(xbox_InputFaceLayoutNintendo(), "the setting is kept");
    xbox_InputSetFaceLayoutNintendo(0);
}

/* The driving-controls choice is kept for the title's host code, and that
 * code can tell a controller's state from the keyboard's (BO-106). */
/* Input > First controller: the port the first controller takes, and a
 * change counted so the backend can free the ports already taken. */
static void test_first_pad(void)
{
    unsigned g0 = xbox_InputFirstPadGeneration();

    CHECK(xbox_InputFirstPadPort() == 0, "the default is player 1");
    xbox_InputSetFirstPadPort(1);
    CHECK(xbox_InputFirstPadPort() == 1, "player 2 is kept");
    CHECK(xbox_InputFirstPadGeneration() == g0 + 1, "a change is counted");
    xbox_InputSetFirstPadPort(1);
    CHECK(xbox_InputFirstPadGeneration() == g0 + 1, "the same choice again is no change");
    xbox_InputSetFirstPadPort(7);
    CHECK(xbox_InputFirstPadPort() == 0, "anything else is player 1");
    CHECK(xbox_InputFirstPadGeneration() == g0 + 2, "and counted");
}

static void test_control_scheme(void)
{
    XBOX_INPUT_STATE st;

    CHECK(xbox_InputControlScheme() == 0, "the default is the title's own layout");
    CHECK(!xbox_InputControlSchemeOffered(), "no title has offered a second layout");
    xbox_InputOfferControlScheme(1);
    xbox_InputSetControlScheme(1);
    CHECK(xbox_InputControlSchemeOffered() && xbox_InputControlScheme() == 1, "the choice is kept");
    xbox_InputSetControlScheme(-3);
    CHECK(xbox_InputControlScheme() == 0, "a negative scheme is the default");
    xbox_InputOfferControlScheme(0);

    xbox_InputSetBackend(&k_fake);
    memset(f_plugged, 0, sizeof f_plugged);
    f_now = 5000;
    f_hold_start = 0;
    f_kb_on = 0;
    CHECK(xbox_InputSources(0) == 0, "sources before any read");
    f_plugged[0] = f_plugged[1] = 1;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == XBOX_INPUT_SRC_PAD, "a controller alone: %u", xbox_InputSources(0));
    f_kb_on = 1;                                /* the fake keyboard holds A */
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == (XBOX_INPUT_SRC_PAD | XBOX_INPUT_SRC_KEYS),
          "controller and a held key: %u", xbox_InputSources(0));
    xbox_InputGetState(1, &st);
    CHECK(xbox_InputSources(1) == XBOX_INPUT_SRC_PAD, "the keyboard is port 0's only");
    f_plugged[0] = 0;
    f_now += 2 * XBOX_INPUT_PROBE_MS;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == XBOX_INPUT_SRC_KEYS, "keys alone: %u", xbox_InputSources(0));
    f_kb_on = 0;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == 0, "nothing on the port: %u", xbox_InputSources(0));
    CHECK(xbox_InputSources(9) == 0, "a port that does not exist");
    f_hold_start = 1;
    xbox_InputSetBackend(NULL);
}

int main(int argc, char **argv)
{
    xbox_InputSetDeadzone(0);
    test_rows();
    test_deadzone();
    test_readme(argc > 1 ? argv[1] : "input_README.md");
    test_cache();
    test_block();
    test_keyboard_play();
    test_rumble();
    test_quit();
    test_exit_hooks();
    test_combo_guard();
    test_trigger_deadzone();
    test_face_layout();
    test_first_pad();
    test_control_scheme();
    printf("input_map: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

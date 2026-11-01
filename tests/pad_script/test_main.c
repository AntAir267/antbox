/*
 * pad_script test: see CMakeLists.txt (BO-96).
 *
 * The report layout checked here is the Xbox pad's (usb_gamepad.c): byte 2
 * the digital buttons, bytes 4..11 the analog ones (A B X Y Black White,
 * then the left and the right trigger).
 */
#include "usb_gamepad.h"
#include "input_backend.h"
#include "recomp_env.h"
#include "keyboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <sys/utime.h>
#define utime _utime
#define utimbuf _utimbuf
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <unistd.h>
#include <utime.h>
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
#endif

enum { R_A = 4, R_LT = 10, R_RT = 11 };

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();
}

/* The fake host: what the controllers on the four ports hold. */
static XBOX_GAMEPAD g_host[XBOX_MAX_CONTROLLERS];
static int g_reads;

static DWORD f_get(DWORD port, XBOX_INPUT_STATE *out)
{
    g_reads++;
    memset(out, 0, sizeof *out);
    out->Gamepad = g_host[port];
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD port, const XBOX_VIBRATION *v) { (void)port; (void)v; return ERROR_SUCCESS; }
static uint64_t f_clock(void) { return (uint64_t)clock() * 1000u / CLOCKS_PER_SEC; }

static const xbox_input_backend k_fake = { NULL, f_get, f_set, NULL, f_clock };
static const xbox_input_backend k_kb = { NULL, f_get, f_set, xbox_InputKeyboardPoll, f_clock };
const xbox_input_backend xbox_input_host_backend = { NULL, f_get, f_set, NULL, f_clock };

static uint8_t rep[4][20];
static void read_pads(void)
{
    for (int p = 0; p < 4; p++)
        CHECK(usb_gamepad_report(p, rep[p], 20) == 20, "pad %d: no report", p);
}

/* The accelerator held by the script on pads 1 and 2 for the whole test. */
#define SCRIPT "0:rt:600000,0:p2-rt:600000"

static void test_takeover(void)
{
    set_env("RECOMP_PAD_SCRIPT", SCRIPT);
    read_pads();
    CHECK(rep[0][R_RT] == 255 && rep[1][R_RT] == 255, "script: rt %u %u", rep[0][R_RT], rep[1][R_RT]);
    CHECK(xbox_InputScriptedPads() == 0xFu, "scripted pads %X", xbox_InputScriptedPads());
    CHECK(usb_gamepad_script_drives(0) && usb_gamepad_script_drives(1), "drives");

    /* A stick alone is not a person: the script keeps the pad. */
    g_host[0].sThumbLX = 20000;
    read_pads();
    CHECK(rep[0][R_RT] == 255, "stick only: rt %u", rep[0][R_RT]);
    CHECK(usb_gamepad_script_drives(0), "stick only: still scripted");
    /* Nor is a trigger at rest a little off zero. */
    g_host[0].bAnalogButtons[XBOX_BUTTON_LTRIGGER] = XBOX_ANALOG_BUTTON_THRESHOLD;
    read_pads();
    CHECK(rep[0][R_RT] == 255, "resting trigger: rt %u", rep[0][R_RT]);

    /* The reported case: the script holds the accelerator, the player
     * pulls the left trigger to brake and reverse. From that report on the
     * title must read the player's pad alone. */
    g_host[0].sThumbLX = 0;
    g_host[0].bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    read_pads();
    CHECK(rep[0][R_LT] == 255 && rep[0][R_RT] == 0, "taken: lt %u rt %u", rep[0][R_LT], rep[0][R_RT]);
    CHECK(rep[1][R_RT] == 255, "pad 2 is still the script's: rt %u", rep[1][R_RT]);
    CHECK(xbox_InputScriptedPads() == 0xEu, "scripted pads %X", xbox_InputScriptedPads());
    CHECK(!usb_gamepad_script_drives(0) && usb_gamepad_script_drives(1), "drives after");

    /* For good: letting go does not hand the pad back. */
    g_host[0].bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 0;
    read_pads();
    CHECK(rep[0][R_LT] == 0 && rep[0][R_RT] == 0, "released: lt %u rt %u", rep[0][R_LT], rep[0][R_RT]);

    /* A digital button takes pad 2. */
    g_host[1].wButtons = XBOX_GAMEPAD_START;
    read_pads();
    CHECK(rep[1][R_RT] == 0 && (rep[1][2] & 0x10), "pad 2 taken: rt %u btn %02X", rep[1][R_RT], rep[1][2]);
    CHECK(xbox_InputScriptedPads() == 0xCu, "scripted pads %X", xbox_InputScriptedPads());
}

static void test_headless(void)
{
    set_env("RECOMP_HEADLESS", "1");
    set_env("RECOMP_PAD_SCRIPT", SCRIPT);
    g_host[0].bAnalogButtons[XBOX_BUTTON_A] = 255;
    g_host[0].bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    g_reads = 0;
    read_pads();
    read_pads();
    CHECK(rep[0][R_RT] == 255 && rep[0][R_LT] == 0 && rep[0][R_A] == 0,
          "headless: rt %u lt %u a %u", rep[0][R_RT], rep[0][R_LT], rep[0][R_A]);
    CHECK(g_reads == 0, "headless: the host pad was read %d times", g_reads);
    CHECK(usb_gamepad_script_drives(0), "headless: still scripted");
}

static void test_keep(void)
{
    set_env("RECOMP_DEBUG", "pad_script_keep=1");
    set_env("RECOMP_PAD_SCRIPT", SCRIPT);
    g_host[0].bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    read_pads();
    read_pads();
    CHECK(rep[0][R_RT] == 255 && rep[0][R_LT] == 255, "keep: rt %u lt %u", rep[0][R_RT], rep[0][R_LT]);
    CHECK(usb_gamepad_script_drives(0), "keep: still scripted");
}

static void test_stale(void)
{
    static const char path[] = "pad_script_stale.txt";
    struct utimbuf old;
    FILE *f = fopen(path, "wb");
    int i;

    CHECK(f != NULL, "cannot write %s", path);
    if (!f)
        return;
    fputs("rt:600000\n", f);
    fclose(f);
    old.actime = old.modtime = time(NULL) - 600;    /* an earlier run's */
    CHECK(utime(path, &old) == 0, "utime");
    set_env("RECOMP_PAD_LIVE", path);

    for (i = 0; i < 4; i++) {           /* the file is polled every 50 ms */
        read_pads();
        sleep_ms(60);
    }
    CHECK(rep[0][R_RT] == 0, "stale line pressed: rt %u", rep[0][R_RT]);
    CHECK(usb_gamepad_live_taken() == 0, "stale lines taken: %u", usb_gamepad_live_taken());

    f = fopen(path, "ab");
    if (f) {
        fputs("lt=77+a:600000\n", f);
        fclose(f);
    }
    for (i = 0; i < 6; i++) {
        read_pads();
        sleep_ms(60);
    }
    CHECK(usb_gamepad_live_taken() == 1, "new line taken: %u", usb_gamepad_live_taken());
    CHECK(rep[0][R_LT] == 77 && rep[0][R_A] == 255 && rep[0][R_RT] == 0,
          "new line: lt %u a %u rt %u", rep[0][R_LT], rep[0][R_A], rep[0][R_RT]);
    remove(path);
}

/* RECOMP_KEY_LIVE: keys held from a file, through the real key table and
 * map, with no pad connected on port 0 (the fake host pad is there, idle). */
static void test_keylive(void)
{
    static const char path[] = "pad_script_keys.txt";
    FILE *f = fopen(path, "wb");
    int i;

    CHECK(f != NULL, "cannot write %s", path);
    if (!f)
        return;
    fclose(f);
    xbox_InputSetBackend(&k_kb);
    xbox_InputKeysClear();
    set_env("RECOMP_KEY_LIVE", path);
    read_pads();                            /* the file is read from here on */
    f = fopen(path, "ab");
    if (f) {
        fputs("w+d:600000\nspace:400\nnotakey\n", f);
        fclose(f);
    }
    for (i = 0; i < 4; i++) {
        read_pads();
        sleep_ms(60);
    }
    CHECK(rep[0][R_RT] == 255 && rep[0][R_A] == 255, "keys: rt %u a %u", rep[0][R_RT], rep[0][R_A]);
    CHECK(rep[0][12] == 0xFF && rep[0][13] == 0x7F, "D: stick X %02X%02X", rep[0][13], rep[0][12]);
    CHECK(rep[0][R_LT] == 0 && !(rep[0][2] & 0x1F), "nothing else: lt %u btn %02X", rep[0][R_LT], rep[0][2]);
    CHECK(rep[1][R_RT] == 0, "pad 2 gets no keys: rt %u", rep[1][R_RT]);
    for (i = 0; i < 6; i++) {
        read_pads();
        sleep_ms(60);
    }
    CHECK(rep[0][R_A] == 0 && rep[0][R_RT] == 255, "Space let go, W held: a %u rt %u", rep[0][R_A], rep[0][R_RT]);
    xbox_InputKeysClear();
    remove(path);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    set_env("RECOMP_HEADLESS", NULL);
    set_env("RECOMP_DEBUG", NULL);
    set_env("RECOMP_PAD_SCRIPT", NULL);
    set_env("RECOMP_PAD_LIVE", NULL);
    set_env("RECOMP_KEY_LIVE", NULL);
    set_env("RECOMP_INPUT_RUMBLE", "0");
    xbox_InputSetBackend(&k_fake);
    if (!strcmp(mode, "takeover"))      test_takeover();
    else if (!strcmp(mode, "headless")) test_headless();
    else if (!strcmp(mode, "keep"))     test_keep();
    else if (!strcmp(mode, "stale"))    test_stale();
    else if (!strcmp(mode, "keylive"))  test_keylive();
    else { printf("usage: pad_script_test takeover|headless|keep|stale|keylive\n"); return 2; }
    printf("pad_script %s: %d checks, %d failed\n", mode, g_checks, g_fail);
    return g_fail ? 1 : 0;
}

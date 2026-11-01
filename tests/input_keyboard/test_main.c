/*
 * input_keyboard test: see CMakeLists.txt.
 *
 * The expected map below is written out independently of the table in
 * src/input/keyboard.c (it is the spec's: arrows the d-pad, W S and Up Down
 * the triggers, A D and Left Right the left stick, Space A, Backspace B,
 * Shift X, Ctrl Y, Q E White Black, Enter START, Esc BACK), so a wrong row
 * fails here rather than being compared with itself.
 */
#include "keyboard.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum { F_BTN, F_ANALOG, F_STICK };
struct effect {
    int field;      /* F_* */
    int index;      /* wButtons bit / bAnalogButtons index / axis 0..3 LX LY RX RY */
    int value;      /* the stick value; 0 for the others */
};
struct expect {
    int vk;
    int n;
    struct effect e[2];     /* what the key alone does; nothing else moves */
};

#define BTN(bit)  { F_BTN, bit, 0 }
#define ANA(i)    { F_ANALOG, i, 0 }
#define STK(a, v) { F_STICK, a, v }

static const struct expect k_expect[] = {
    { 0x26, 2, { BTN(0x0001), ANA(7) } },          /* Up arrow    -> DPAD_UP, right trigger */
    { 0x28, 2, { BTN(0x0002), ANA(6) } },          /* Down arrow  -> DPAD_DOWN, left trigger */
    { 0x25, 2, { BTN(0x0004), STK(0, -32767) } },  /* Left arrow  -> DPAD_LEFT, stick left */
    { 0x27, 2, { BTN(0x0008), STK(0, 32767) } },   /* Right arrow -> DPAD_RIGHT, stick right */
    { 0x0D, 1, { BTN(0x0010) } },                  /* Enter       -> START */
    { 0x1B, 1, { BTN(0x0020) } },                  /* Esc         -> BACK */
    { 0x20, 1, { ANA(0) } },                       /* Space       -> A */
    { 0x08, 1, { ANA(1) } },                       /* Backspace   -> B */
    { 0x10, 1, { ANA(2) } },                       /* Shift       -> X */
    { 0x11, 1, { ANA(3) } },                       /* Ctrl        -> Y */
    { 'E',  1, { ANA(4) } },                       /* BLACK */
    { 'Q',  1, { ANA(5) } },                       /* WHITE */
    { 'S',  1, { ANA(6) } },                       /* left trigger (brake) */
    { 'W',  1, { ANA(7) } },                       /* right trigger (accelerate) */
    { 'A',  1, { STK(0, -32767) } },
    { 'D',  1, { STK(0, 32767) } },
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static int stick(const XBOX_GAMEPAD *g, int a)
{
    return a == 0 ? g->sThumbLX : a == 1 ? g->sThumbLY
         : a == 2 ? g->sThumbRX : g->sThumbRY;
}

/* Everything in g is at rest except what e names. */
static void check_only(const XBOX_GAMEPAD *g, const struct expect *e)
{
    WORD want_btn = 0;
    int want_an[8] = { 0 }, want_st[4] = { 0 };

    for (int k = 0; k < e->n; k++) {
        if (e->e[k].field == F_BTN) want_btn |= (WORD)e->e[k].index;
        else if (e->e[k].field == F_ANALOG) want_an[e->e[k].index] = 255;
        else want_st[e->e[k].index] = e->e[k].value;
    }
    CHECK(g->wButtons == want_btn, "vk 0x%02X: wButtons 0x%04X, want 0x%04X",
          e->vk, g->wButtons, want_btn);
    for (int i = 0; i < 8; i++)
        CHECK(g->bAnalogButtons[i] == want_an[i], "vk 0x%02X: analog[%d] %d, want %d",
              e->vk, i, g->bAnalogButtons[i], want_an[i]);
    for (int a = 0; a < 4; a++)
        CHECK(stick(g, a) == want_st[a], "vk 0x%02X: axis %d %d, want %d",
              e->vk, a, stick(g, a), want_st[a]);
}

/* ── 1. every key alone, and nothing else moves ─────────────────────────── */

static void test_rows(void)
{
    XBOX_INPUT_STATE st;
    DWORD last;

    xbox_InputKeysClear();
    xbox_InputKeyboardState(&st);
    last = st.dwPacketNumber;
    {
        struct expect rest = { 0, 0, { { 0, 0, 0 } } };
        check_only(&st.Gamepad, &rest);
    }

    for (size_t i = 0; i < N_EXPECT; i++) {
        xbox_InputKeysClear();
        xbox_InputKeySet(k_expect[i].vk, 1);
        CHECK(xbox_FramebufferKeyDown(k_expect[i].vk), "vk 0x%02X not held", k_expect[i].vk);
        xbox_InputKeyboardState(&st);
        check_only(&st.Gamepad, &k_expect[i]);
        CHECK(st.dwPacketNumber != last, "packet number did not move");
        last = st.dwPacketNumber;
    }

    /* The keys the code uses are exactly the expected ones. */
    {
        int used[256] = { 0 }, want[256] = { 0 };
        for (size_t r = 0; r < xbox_key_map_count; r++)
            for (int k = 0; k < 2; k++)
                if (xbox_key_map[r].vk[k]) used[xbox_key_map[r].vk[k]] = 1;
        for (size_t i = 0; i < N_EXPECT; i++)
            want[k_expect[i].vk] = 1;
        for (int vk = 0; vk < 256; vk++)
            CHECK(used[vk] == want[vk], "vk 0x%02X: map uses it %d, expected %d", vk, used[vk], want[vk]);
    }
}

/* ── 2. keys outside the map do nothing ───────────────────────────────── */

static void test_unmapped(void)
{
    int mapped[256] = { 0 };
    XBOX_INPUT_STATE st;
    struct expect rest = { 0, 0, { { 0, 0, 0 } } };

    for (size_t i = 0; i < N_EXPECT; i++)
        mapped[k_expect[i].vk] = 1;
    for (int vk = 1; vk < 256; vk++) {
        if (mapped[vk]) continue;
        xbox_InputKeysClear();
        xbox_InputKeySet(vk, 1);
        xbox_InputKeyboardState(&st);
        rest.vk = vk;
        check_only(&st.Gamepad, &rest);
    }
    xbox_InputKeysClear();
}

/* ── 3. combinations, release, clear, range ────────────────────────────── */

static void test_combos(void)
{
    XBOX_INPUT_STATE st;

    /* Opposite directions cancel; a letter and an arrow of one direction
     * are one. */
    xbox_InputKeysClear();
    xbox_InputKeySet('A', 1);
    xbox_InputKeySet('D', 1);
    xbox_InputKeySet('W', 1);
    xbox_InputKeySet(0x26, 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.sThumbLX == 0, "A+D: LX %d, want 0", st.Gamepad.sThumbLX);
    CHECK(st.Gamepad.bAnalogButtons[7] == 255, "W+Up: RT %d", st.Gamepad.bAnalogButtons[7]);

    /* Release one of W and Up: still accelerating. Release both: rest. */
    xbox_InputKeySet('W', 0);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.bAnalogButtons[7] == 255, "Up alone: RT %d", st.Gamepad.bAnalogButtons[7]);
    xbox_InputKeySet(0x26, 0);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.bAnalogButtons[7] == 0 && st.Gamepad.wButtons == 0, "released: RT %d", st.Gamepad.bAnalogButtons[7]);

    /* Left arrow with A: one stick direction, and the d-pad. */
    xbox_InputKeysClear();
    xbox_InputKeySet(0x25, 1);
    xbox_InputKeySet('A', 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.sThumbLX == -32767 && st.Gamepad.wButtons == 0x0004, "Left+A: LX %d btn 0x%04X",
          st.Gamepad.sThumbLX, st.Gamepad.wButtons);

    /* Driving: accelerate, steer, boost, brake together. */
    xbox_InputKeysClear();
    xbox_InputKeySet('W', 1);
    xbox_InputKeySet('D', 1);
    xbox_InputKeySet(0x20, 1);
    xbox_InputKeySet('S', 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.bAnalogButtons[7] == 255 && st.Gamepad.bAnalogButtons[6] == 255
          && st.Gamepad.bAnalogButtons[0] == 255 && st.Gamepad.sThumbLX == 32767,
          "W+S+Space+D: RT %d LT %d A %d LX %d", st.Gamepad.bAnalogButtons[7],
          st.Gamepad.bAnalogButtons[6], st.Gamepad.bAnalogButtons[0], st.Gamepad.sThumbLX);

    /* Buttons together. */
    xbox_InputKeysClear();
    xbox_InputKeySet(0x0D, 1);
    xbox_InputKeySet(0x1B, 1);
    xbox_InputKeySet(0x20, 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.wButtons == (0x0010 | 0x0020), "Enter+Esc: 0x%04X", st.Gamepad.wButtons);
    CHECK(st.Gamepad.bAnalogButtons[0] == 255, "Space: A %d", st.Gamepad.bAnalogButtons[0]);

    /* Clear releases everything. */
    xbox_InputKeysClear();
    for (int vk = 0; vk < 256; vk++)
        CHECK(!xbox_FramebufferKeyDown(vk), "vk 0x%02X held after Clear", vk);

    /* Out of range is ignored, both ways. */
    xbox_InputKeySet(-1, 1);
    xbox_InputKeySet(256, 1);
    CHECK(!xbox_FramebufferKeyDown(-1) && !xbox_FramebufferKeyDown(256), "out of range");
}

/* ── 4. RECOMP_KEYBOARD gates the backend hook ─────────────────────────── */

static void test_enable(void)
{
    XBOX_INPUT_STATE st;

    xbox_InputKeysClear();
    xbox_InputKeySet(0x0D, 1);
    set_env("RECOMP_KEYBOARD", NULL);
    CHECK(xbox_InputKeyboardPoll(&st), "unset: the keyboard is on by default");
    set_env("RECOMP_KEYBOARD", "0");
    CHECK(!xbox_InputKeyboardPoll(&st), "=0: poll answered");
    set_env("RECOMP_KEYBOARD", "1");
    memset(&st, 0, sizeof st);
    CHECK(xbox_InputKeyboardPoll(&st), "=1: poll did not answer");
    CHECK(st.Gamepad.wButtons == 0x0010, "=1: wButtons 0x%04X", st.Gamepad.wButtons);
    set_env("RECOMP_KEYBOARD", NULL);
    xbox_InputKeysClear();
}

/* ── 4b. the activity hook (the prompts follow the device in use) ───────── */

static int g_hook;
static void hook(void) { g_hook++; }

static void test_activity(void)
{
    set_env("RECOMP_KEYBOARD", NULL);
    xbox_InputKeysClear();
    xbox_InputKeyboardSetActivityHook(hook);
    xbox_InputKeySet('W', 1);
    CHECK(g_hook == 1, "a mapped key down: hook %d", g_hook);
    xbox_InputKeySet('W', 0);
    CHECK(g_hook == 1, "a key up called the hook");
    xbox_InputKeySet('P', 1);
    xbox_InputKeySet(0x70, 1);          /* F1: the overlay's, not the pad's */
    CHECK(g_hook == 1, "an unmapped key called the hook: %d", g_hook);
    set_env("RECOMP_KEYBOARD", "0");
    xbox_InputKeySet(0x20, 1);
    CHECK(g_hook == 1, "keyboard off: hook %d", g_hook);
    set_env("RECOMP_KEYBOARD", NULL);
    xbox_InputKeyboardSetActivityHook(NULL);
    xbox_InputKeySet(0x0D, 1);
    CHECK(g_hook == 1, "no hook set");
    xbox_InputKeysClear();
}

/* ── 5. README ─────────────────────────────────────────────────────────── */

static void test_readme(const char *path)
{
    char line[512], want[512];
    FILE *f;
    size_t row = 0;
    int in = 0, found = 0;
    const char *out = getenv("INPUT_KEYBOARD_README_OUT");

    if (out) {
        FILE *o = fopen(out, "w");
        if (o) {
            fprintf(o, "%s\n", XBOX_KEY_MAP_README_BEGIN);
            for (size_t i = 0; xbox_InputKeyMapReadmeRow(i, want, sizeof want); i++)
                fprintf(o, "%s\n", want);
            fprintf(o, "%s\n", XBOX_KEY_MAP_README_END);
            fclose(o);
        }
    }

    f = fopen(path, "r");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!in) {
            if (!strcmp(line, XBOX_KEY_MAP_README_BEGIN)) in = found = 1;
            continue;
        }
        if (!strcmp(line, XBOX_KEY_MAP_README_END)) { in = 0; break; }
        if (!xbox_InputKeyMapReadmeRow(row, want, sizeof want)) {
            CHECK(0, "README has an extra key line: %s", line);
            break;
        }
        CHECK(!strcmp(line, want), "README key line %u:\n  have: %s\n  want: %s",
              (unsigned)row, line, want);
        row++;
    }
    fclose(f);
    CHECK(found, "README has no '%s' marker", XBOX_KEY_MAP_README_BEGIN);
    CHECK(!in, "README key table is not closed by '%s'", XBOX_KEY_MAP_README_END);
    CHECK(!xbox_InputKeyMapReadmeRow(row, want, sizeof want),
          "README key table is missing line %u: %s", (unsigned)row, want);
}

int main(int argc, char **argv)
{
    test_rows();
    test_unmapped();
    test_combos();
    test_enable();
    test_activity();
    test_readme(argc > 1 ? argv[1] : "input_README.md");
    printf("input_keyboard: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

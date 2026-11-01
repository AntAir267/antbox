/*
 * input_touch: the on-screen controls (src/input/touch_pad.c) and their
 * merge into port 0 (src/input/input_core.c), against a fake backend.
 */
#include "touch_pad.h"
#include "input_backend.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── a fake host: no pad unless g_pad ─────────────────────────────── */

static int g_pad;
static XBOX_GAMEPAD g_pad_state;
static uint64_t g_now = 1000;

static DWORD f_get(DWORD port, XBOX_INPUT_STATE *out)
{
    if (port != 0 || !g_pad)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(out, 0, sizeof *out);
    out->Gamepad = g_pad_state;
    out->dwPacketNumber = 7;
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD port, const XBOX_VIBRATION *v) { (void)port; (void)v; return ERROR_SUCCESS; }
static uint64_t f_clock(void) { return g_now += 2000; }   /* every read probes */

const xbox_input_backend xbox_input_host_backend = { NULL, f_get, f_set, NULL, f_clock, NULL };

/* ── helpers ───────────────────────────────────────────────────────── */

static int g_driving;
static int driving(void) { return g_driving; }

static int find(const char *label, xbox_touch_draw *out)
{
    xbox_touch_draw d[XBOX_TOUCH_MAX_CONTROLS];
    int n = xbox_InputTouchDrawList(d, XBOX_TOUCH_MAX_CONTROLS);
    for (int i = 0; i < n; i++) {
        if ((label && d[i].label && !strcmp(d[i].label, label))
                || (!label && d[i].kind == XBOX_TOUCH_KIND_DPAD)) {
            *out = d[i];
            return 1;
        }
    }
    return 0;
}

static int find_icon(int icon, xbox_touch_draw *out)
{
    xbox_touch_draw d[XBOX_TOUCH_MAX_CONTROLS];
    int n = xbox_InputTouchDrawList(d, XBOX_TOUCH_MAX_CONTROLS);
    for (int i = 0; i < n; i++)
        if (d[i].icon == icon) {
            *out = d[i];
            return 1;
        }
    return 0;
}

static XBOX_GAMEPAD read0(DWORD *rc, unsigned *src)
{
    XBOX_INPUT_STATE s;
    memset(&s, 0, sizeof s);
    *rc = xbox_InputGetState(0, &s);
    *src = xbox_InputSources(0);
    return s.Gamepad;
}

static void setup(void)
{
    xbox_InputTouchReset();
    xbox_InputSetBackend(NULL);
    g_pad = 0;
    memset(&g_pad_state, 0, sizeof g_pad_state);
    xbox_InputTouchSetMode(XBOX_TOUCH_AUTO);
    xbox_InputTouchSetScreen(1920, 1080);
    xbox_InputTouchSetDrivingHook(driving);
}

int main(void)
{
    xbox_touch_draw c;
    XBOX_GAMEPAD g;
    DWORD rc;
    unsigned src;

    /* Off: port 0 has nothing without a pad, and nothing is drawn. */
    setup();
    xbox_InputTouchSetMode(XBOX_TOUCH_OFF);
    g_driving = 0;
    g = read0(&rc, &src);
    CHECK(rc == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(!find("A", &c));

    /* Menus: A B X Y and a d-pad; port 0 is connected with none held. */
    setup();
    g_driving = 0;
    g = read0(&rc, &src);
    CHECK(rc == ERROR_SUCCESS && g.bAnalogButtons[XBOX_BUTTON_A] == 0 && !(src & XBOX_INPUT_SRC_TOUCH));
    CHECK(find("A", &c));
    CHECK(c.x > 1920 * 0.6f && c.y > 1080 * 0.6f);   /* bottom right */
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 1, c.x, c.y);
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255 && (src & XBOX_INPUT_SRC_TOUCH));
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 1, c.x, c.y);
    /* A tap the title has read once is held for one more read. */
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255);
    find("A", &c);                                     /* a frame: the hold ends */
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 0);
    /* The touch is the device in use until a controller is used. */
    CHECK(src & XBOX_INPUT_SRC_TOUCH);
    CHECK(find(NULL, &c));
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 2, c.x, c.y - c.r * 0.8f);
    g = read0(&rc, &src);
    CHECK(g.wButtons == XBOX_GAMEPAD_DPAD_UP);
    xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 2, c.x + c.r * 0.8f, c.y - c.r * 0.2f);
    g = read0(&rc, &src);
    CHECK(g.wButtons == XBOX_GAMEPAD_DPAD_RIGHT);       /* no diagonal */
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 2, 0, 0);

    /* Pause is Start; the menu button asks for the overlay. */
    CHECK(find_icon(XBOX_TOUCH_ICON_PAUSE, &c));
    CHECK(c.x < 1920 * 0.5f && c.y < 1080 * 0.2f);     /* top, left of centre */
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 3, c.x, c.y);
    g = read0(&rc, &src);
    CHECK(g.wButtons & XBOX_GAMEPAD_START);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 3, c.x, c.y);
    CHECK(!xbox_InputTouchTakeMenu());
    CHECK(find_icon(XBOX_TOUCH_ICON_MENU, &c));
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 4, c.x, c.y);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 4, c.x, c.y);
    CHECK(xbox_InputTouchTakeMenu());
    CHECK(!xbox_InputTouchTakeMenu());

    /* Driving: the stick floats; GAS with it; BOOST is A with the gas. */
    setup();
    g_driving = 1;
    CHECK(find("GAS", &c));
    CHECK(!find("A", &c));
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 10, 500, 700);    /* left half: the stick */
    xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 10, 500 + 1080 * 0.15f, 700);  /* full right */
    find("GAS", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 11, c.x, c.y);
    g = read0(&rc, &src);
    CHECK(g.sThumbLX > 32000 && g.sThumbLY == 0);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255 && g.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 10, 500 - 1080 * 0.05f, 700);  /* a third left */
    g = read0(&rc, &src);
    CHECK(g.sThumbLX < -8000 && g.sThumbLX > -14000);
    /* Glide from GAS to BOOST. */
    find("BOOST", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 11, c.x, c.y);
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255 && g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    /* Back to GAS through the gap between them: the throttle never drops. */
    {
        xbox_touch_draw gas;
        find("GAS", &gas);
        for (int k = 1; k <= 10; k++) {
            float t = k / 10.0f;
            xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 11, c.x + (gas.x - c.x) * t, c.y + (gas.y - c.y) * t);
            g = read0(&rc, &src);
            CHECK(g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
        }
        CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 0);
        /* Well away from every button: let go. */
        xbox_InputTouchFinger(XBOX_TOUCH_MOVE, 11, 1920 * 0.6f, 1080 * 0.5f);
        g = read0(&rc, &src);
        CHECK(g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0);
    }
    read0(&rc, &src);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 11, c.x, c.y);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 10, 0, 0);
    g = read0(&rc, &src);
    CHECK(!g.sThumbLX && !g.bAnalogButtons[XBOX_BUTTON_A] && !g.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);

    /* A race ends under a held GAS: let go, and it is not the menu's A. */
    find("GAS", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 12, c.x, c.y);
    g_driving = 0;
    find("A", &c);                                     /* the layout follows */
    g = read0(&rc, &src);
    CHECK(!g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] && !g.bAnalogButtons[XBOX_BUTTON_A]);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 12, 0, 0);

    /* Auto-gas: held while driving, BRAKE lets it go. */
    g_driving = 1;
    xbox_InputTouchSetAutoGas(1);
    find("GAS", &c);
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    find("BRAKE", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 13, c.x, c.y);
    g = read0(&rc, &src);
    CHECK(!g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] && g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 255);
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 13, 0, 0);
    xbox_InputTouchSetAutoGas(0);

    /* Auto: a controller hides them and lets go; a touch shows them. A pad
     * and the touch are merged. */
    find("GAS", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 14, c.x, c.y);
    g_pad = 1;
    g_pad_state.wButtons = XBOX_GAMEPAD_BACK;   /* swallowed by nothing: no guard */
    g_pad_state.bAnalogButtons[XBOX_BUTTON_X] = 200;
    g = read0(&rc, &src);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_X] == 200 && g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    CHECK((src & (XBOX_INPUT_SRC_PAD | XBOX_INPUT_SRC_TOUCH)) == (XBOX_INPUT_SRC_PAD | XBOX_INPUT_SRC_TOUCH));
    xbox_InputTouchControllerUsed();
    CHECK(!xbox_InputTouchVisible() && !xbox_InputTouchInUse());
    CHECK(!find("GAS", &c));
    g = read0(&rc, &src);
    CHECK(!g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] && !(src & XBOX_INPUT_SRC_TOUCH));
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 14, 0, 0);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 15, 10, 10);
    CHECK(xbox_InputTouchVisible());
    xbox_InputTouchFinger(XBOX_TOUCH_UP, 15, 10, 10);
    /* Always: a controller does not hide them. */
    xbox_InputTouchSetMode(XBOX_TOUCH_ALWAYS);
    xbox_InputTouchControllerUsed();
    CHECK(xbox_InputTouchVisible());
    g_pad = 0;

    /* The overlay open: fingers are let go and new ones not taken. */
    xbox_InputTouchSetMode(XBOX_TOUCH_AUTO);
    find("GAS", &c);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 16, c.x, c.y);
    xbox_InputTouchSetBlocked(1);
    xbox_InputTouchFinger(XBOX_TOUCH_DOWN, 17, c.x, c.y);
    xbox_InputTouchSetBlocked(0);
    g = read0(&rc, &src);
    CHECK(!g.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);

    /* A notch on the right moves the right controls left. */
    {
        xbox_touch_draw p0, p1;
        find("GAS", &p0);
        xbox_InputTouchSetInsets(0, 0, 120, 0);
        find("GAS", &p1);
        CHECK(p1.x == p0.x - 120);
    }
    /* Clear of the HUD's corners at 16:9 and 20:9: nothing in the bottom
     * 0.15 H right of 0.6 W (speedometer), nor the top 0.19 H within
     * 0.22 H of either side (POS, LAP), nor the score board's top centre. */
    for (int k = 0; k < 2; k++) {
        const float w = k ? 2400 : 1920, h = 1080;
        xbox_touch_draw d[XBOX_TOUCH_MAX_CONTROLS];
        int n;
        xbox_InputTouchSetInsets(0, 0, 0, 0);
        xbox_InputTouchSetScreen(w, h);
        n = xbox_InputTouchDrawList(d, XBOX_TOUCH_MAX_CONTROLS);
        CHECK(n > 8);
        for (int i = 0; i < n; i++) {
            const float x0 = d[i].x - d[i].r, x1 = d[i].x + d[i].r;
            const float y0 = d[i].y - d[i].r, y1 = d[i].y + d[i].r;
            if (d[i].kind == XBOX_TOUCH_KIND_STICK)
                continue;               /* floats; its rest is checked below */
            CHECK(!(y1 > h * 0.855f && x1 > w - h * 0.37f));
            CHECK(!(y0 < h * 0.19f && (x0 < h * 0.22f || x1 > w - h * 0.22f)));
            CHECK(!(y0 < h * 0.16f && x1 > w * 0.5f - h * 0.205f && x0 < w * 0.5f + h * 0.205f));
        }
    }

    /* A tablet: 1600 rows at 240 dpi is 169 mm; sized as 90 mm (850 rows). */
    {
        xbox_touch_draw g0, g1;
        xbox_InputTouchSetInsets(0, 0, 0, 0);
        xbox_InputTouchSetScreen(2560, 1600);
        find("GAS", &g0);
        xbox_InputTouchSetDpi(240);
        find("GAS", &g1);
        CHECK(g1.r < g0.r * 0.6f && g1.r > g0.r * 0.5f);
        CHECK(g1.x > 2560 - 1600 * 0.3f && g1.y > 1600 * 0.6f);   /* still bottom right */
        /* A phone (1080 rows at 440 dpi, 62 mm): unchanged. */
        xbox_InputTouchSetScreen(2400, 1080);
        xbox_InputTouchSetDpi(0);
        find("GAS", &g0);
        xbox_InputTouchSetDpi(440);
        find("GAS", &g1);
        CHECK(g1.r == g0.r && g1.x == g0.x);
    }

    xbox_InputSetBackend(NULL);
    printf("input_touch: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}

/*
 * usb_gamepad.c -- an Xbox controller, as the console's USB stack expects it.
 *
 * Descriptors and the input report, from the USB 2.0 specification for the
 * standard requests and from the device's own published interface class for
 * the rest. The gamepad is not a HID device: it reports interface class 0x58
 * subclass 0x42, which is Microsoft's own, and its report has a fixed layout
 * rather than one described by a HID report descriptor. That is why there is
 * no report descriptor here and why nothing asks for one.
 *
 * Input comes from the host through the existing xbox_input layer, so a real
 * pad plugged into the PC drives this one.
 */
#include "usb_gamepad.h"
#include "recomp_env.h"
#include "latency_trace.h"
#include "host_time.h"
#include "../input/xinput_xbox.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18,             /* bLength                                    */
    0x01,           /* bDescriptorType: DEVICE                    */
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x00,           /* bDeviceClass: per interface                */
    0x00,           /* bDeviceSubClass                            */
    0x00,           /* bDeviceProtocol                            */
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x89, 0x02,     /* idProduct 0x0289 Controller S              */
    0x21, 0x01,     /* bcdDevice                                  */
    0x00,           /* iManufacturer: none                        */
    0x00,           /* iProduct: none                             */
    0x00,           /* iSerialNumber: none                        */
    0x01            /* bNumConfigurations                         */
};

/* Configuration, interface and both endpoints, in the one block a
 * GET_DESCRIPTOR(CONFIGURATION) returns. wTotalLength covers all of it. */
static const uint8_t s_config_desc[32] = {
    /* configuration */
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    /* interface: class 0x58 subclass 0x42, the Xbox gamepad's own */
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
    /* endpoint 0x81 IN, interrupt, 32 bytes, 4 ms */
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    /* endpoint 0x02 OUT, interrupt, 32 bytes, 4 ms -- rumble */
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04
};

static uint8_t s_address[USB_GAMEPAD_MAX];
static uint8_t s_configuration[USB_GAMEPAD_MAX];

uint8_t usb_gamepad_address(int pad) { return s_address[pad & 3]; }
int usb_gamepad_configured(int pad) { return s_configuration[pad & 3] != 0; }

/* ---- control transfers ------------------------------------------------- */

#define REQ_GET_STATUS         0x00
#define REQ_CLEAR_FEATURE      0x01
#define REQ_SET_FEATURE        0x03
#define REQ_SET_ADDRESS        0x05
#define REQ_GET_DESCRIPTOR     0x06
#define REQ_GET_CONFIGURATION  0x08
#define REQ_SET_CONFIGURATION  0x09
#define REQ_SET_INTERFACE      0x0B

#define DESC_DEVICE            0x01
#define DESC_CONFIGURATION     0x02
#define DESC_STRING            0x03

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    /* A device sends the smaller of what was asked for and what it has. */
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

int usb_gamepad_control(int pad, const UsbSetup *setup, uint8_t *out, int max)
{
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;   /* 0 standard, 1 class */

    if (type == 0) {
        switch (setup->bRequest) {
        case REQ_GET_DESCRIPTOR:
            switch (setup->wValue >> 8) {
            case DESC_DEVICE:
                return copy_out(out, max, s_device_desc,
                                (int)sizeof s_device_desc, setup->wLength);
            case DESC_CONFIGURATION:
                return copy_out(out, max, s_config_desc,
                                (int)sizeof s_config_desc, setup->wLength);
            case DESC_STRING:
                /* No string descriptors. Stalling is the correct answer and
                 * the one a host expects; returning an empty descriptor gets
                 * read as a malformed one. */
                return -1;
            default:
                return -1;
            }

        case REQ_SET_ADDRESS:
            s_address[pad & 3] = (uint8_t)(setup->wValue & 0x7F);
            return 0;                    /* zero-length status stage */

        case REQ_SET_CONFIGURATION:
            s_configuration[pad & 3] = (uint8_t)(setup->wValue & 0xFF);
            return 0;

        case REQ_GET_CONFIGURATION:
            if (!is_in || max < 1) return -1;
            out[0] = s_configuration[pad & 3];
            return 1;

        case REQ_GET_STATUS:
            /* Bus-powered, no remote wakeup. */
            if (!is_in || max < 2) return -1;
            out[0] = 0; out[1] = 0;
            return 2;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;

        default:
            return -1;
        }
    }

    /* Class requests on the interface -- GET_REPORT.
     *
     * XAPI reads the pad through the interrupt endpoint and also asks for
     * the same report over the control pipe. Stalling that is not a small
     * omission: a stalled control transfer reads to the driver as a broken
     * device, and because the stall also halts the endpoint, the pad stops
     * being polled for good.
     *
     * Measured on Shin Megami Tensei: Nine, the periodic list runs at about
     * a hundred descriptors a second and then collapses to nothing the
     * moment one `A1 01 value 0100 len 20` arrives.
     *
     * The answer is the report the interrupt endpoint would have sent.
     */
    if ((setup->bmRequestType & 0x60u) == 0x20u        /* class */
        && setup->bRequest == 0x01u                    /* GET_REPORT */
        && is_in) {
        uint8_t report[20];
        int n = usb_gamepad_report(pad, report, (int)sizeof report);
        if (n <= 0)
            return -1;
        return copy_out(out, max, report, n, setup->wLength);
    }

    /* Vendor requests on the interface -- the XID protocol.
     *
     * This is how XAPI tells a controller from any other USB device. The
     * standard descriptors say "interface class 0x58", which gets the device
     * enumerated and no further: XAPI then asks for the XID descriptor to
     * learn what kind of controller it is and how big its reports are, and a
     * stall there means "not a controller", so the device is enumerated,
     * configured, and then ignored. Which is exactly what it looked like --
     * a clean enumeration and a title that still saw no gamepad.
     */
    if ((setup->bmRequestType & 0x60u) == 0x40u) {   /* vendor */
        static const uint8_t xid_desc[16] = {
            0x10,           /* bLength                                  */
            0x42,           /* bDescriptorType: XID                     */
            0x00, 0x01,     /* bcdXid 1.00                              */
            0x01,           /* bType: gamepad                           */
            0x02,           /* bSubType: gamepad S                      */
            20,             /* bMaxInputReportSize                      */
            6,              /* bMaxOutputReportSize                     */
            0xFF, 0xFF, 0xFF, 0xFF,   /* wAlternateProductIds[0..1]     */
            0xFF, 0xFF, 0xFF, 0xFF    /* wAlternateProductIds[2..3]     */
        };
        /* Capabilities are the report with every supported field set to all
         * ones: the same layout, read as a mask. A gamepad S supports every
         * field of both, so both are filled in apart from the id and length
         * bytes, which are values rather than flags. */
        static const uint8_t caps_in[20] = {
            0x00, 20,
            0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        static const uint8_t caps_out[6] = {
            0x00, 6, 0xFF, 0xFF, 0xFF, 0xFF
        };

        if (!is_in)
            return 0;                     /* accept, nothing to send back */
        if (setup->bRequest == REQ_GET_DESCRIPTOR
                && (setup->wValue >> 8) == 0x42)
            return copy_out(out, max, xid_desc, (int)sizeof xid_desc,
                            setup->wLength);
        if (setup->bRequest == 0x01) {    /* GET_CAPABILITIES */
            if ((setup->wValue >> 8) == 0x01)
                return copy_out(out, max, caps_in, (int)sizeof caps_in,
                                setup->wLength);
            if ((setup->wValue >> 8) == 0x02)
                return copy_out(out, max, caps_out, (int)sizeof caps_out,
                                setup->wLength);
        }
        return -1;
    }

    /* SET_REPORT on the interface (xemu's xid_handle_control): the rumble
     * report sent over the control pipe instead of the OUT endpoint. The
     * data stage arrives through usb_gamepad_output(); this is the status
     * stage, which only has to succeed. */
    if ((setup->bmRequestType & 0x7Fu) == 0x21u && setup->bRequest == 0x09u)
        return 0;

    /* Class requests: not ours to guess at. */
    return -1;
}

/* ---- the output report: rumble ----------------------------------------- */

/* The XDK's XInputSetState reaches the pad as the XID output report, on the
 * OUT interrupt endpoint (or as a SET_REPORT control transfer):
 *
 *   0      report id, 0
 *   1      length, 6
 *   2..3   left actuator,  little endian -- the big, low-frequency motor
 *   4..5   right actuator, little endian -- the small, high-frequency motor
 *
 * Layout as in xemu hw/xbox/xid.c (XIDGamepadOutputReport, xid_handle_data /
 * xid_handle_control). The values are the same 0..65535 XInput uses, so they
 * go straight to xbox_InputSetState, where the rumble curve is
 * (src/input/input_core.c, rumble_curve.h).
 *
 * RECOMP_INPUT_RUMBLE=0 turns it off. RECOMP_TRACE=input_diag prints every
 * report that changes the motors; the first non-zero one of each pad is
 * always logged. */
static struct {
    uint16_t left, right;
    unsigned long last_ms;      /* when the host pad was last told */
    int      announced;         /* the first-use line has been printed */
    int      seen;              /* any report has arrived */
} s_rumble[USB_GAMEPAD_MAX];

static int rumble_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = recomp_env(RENV_INPUT_RUMBLE);
        on = !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'
                     || v[0] == 'f' || v[0] == 'F'));
    }
    return on;
}

static void rumble_apply(int pad, int log)
{
    XBOX_VIBRATION vib;
    DWORD rc;

    vib.wLeftMotorSpeed  = s_rumble[pad].left;
    vib.wRightMotorSpeed = s_rumble[pad].right;
    rc = xbox_InputSetState((DWORD)pad, &vib);
    s_rumble[pad].last_ms = (unsigned long)GetTickCount();
    if (log) {
        fprintf(stderr, "  [RUMBLE] pad %d left=%u right=%u -> host rc=%lu\n",
                pad, vib.wLeftMotorSpeed, vib.wRightMotorSpeed,
                (unsigned long)rc);
        fflush(stderr);
    }
}

void usb_gamepad_output(int pad, const uint8_t *data, int len)
{
    uint16_t left, right;
    int diag = recomp_env(RENV_INPUT_DIAG) != NULL;

    pad &= 3;
    if (len < 6 || data[1] < 6)
        return;                         /* not a rumble report */
    if (!rumble_enabled())
        return;
    left  = (uint16_t)(data[2] | (data[3] << 8));
    right = (uint16_t)(data[4] | (data[5] << 8));

    if (left == s_rumble[pad].left && right == s_rumble[pad].right
            && s_rumble[pad].seen)
        return;                         /* unchanged: usb_gamepad_report refreshes */
    s_rumble[pad].left  = left;
    s_rumble[pad].right = right;
    s_rumble[pad].seen = 1;
    if (!s_rumble[pad].announced && (left || right)) {
        s_rumble[pad].announced = 1;
        fprintf(stderr, "  [RUMBLE] pad %d: the title uses rumble\n", pad);
        fflush(stderr);
        diag = 1;
    }
    rumble_apply(pad, diag);
}

/* From the pad poll. The title only sends a report when the motors change:
 * its speeds are given again every 150 ms while it holds one (a pad that
 * appeared since gets them), and every poll is the rumble's clock, which
 * runs on after the title's zero until the motor has let go. */
static void rumble_refresh(int pad)
{
    if (!rumble_enabled())
        return;
    if ((s_rumble[pad].left | s_rumble[pad].right)
            && (unsigned long)GetTickCount() - s_rumble[pad].last_ms >= 150)
        rumble_apply(pad, 0);
    else
        xbox_InputRumbleTick((DWORD)pad);
}

/* ---- the input report -------------------------------------------------- */

/* The host's own pad, through the layer that already maps one to XInput.
 * A real controller plugged into the PC drives this emulated one. */
#include "../input/xinput_xbox.h"
#include "../input/keyboard.h"   /* xbox_FramebufferKeyDown */

/*
 * The Xbox report is 20 bytes and fixed:
 *
 *   0      report id, always 0
 *   1      length, always 20
 *   2      digital buttons: dpad, start, back, thumb clicks
 *   3      reserved
 *   4..11  analog buttons A B X Y Black White, then the two triggers
 *   12..19 four signed 16-bit stick axes, little endian
 */
/* A synthetic press, for bringing a title up without a pad on the desk.
 *
 * RECOMP_PAD_PRESS=0x10 holds Start for a quarter of a second every two
 * seconds. A title sitting on a "press Start" screen needs an edge, not a
 * level, so this pulses rather than latching -- and it repeats because the
 * moment the title starts reading input is not knowable from here.
 *
 * Bit values are the Xbox digital button mask: 0x01/02/04/08 dpad
 * up/down/left/right, 0x10 Start, 0x20 Back, 0x40/0x80 thumb clicks.
 *
 * This is a bring-up probe and nothing else. It is off unless the variable
 * is set, and a real pad on the host is always the better input.
 */
static uint8_t synthetic_buttons(void)
{
    static int      configured = -1;
    static unsigned mask, period_ms, hold_ms;
    static unsigned long t0;
    unsigned long now, phase;

    if (configured < 0) {
        const char *spec = recomp_env(RENV_PAD_PRESS);
        configured = 0;
        if (spec && *spec) {
            char *end;
            mask = (unsigned)strtoul(spec, &end, 0) & 0xFFu;
            period_ms = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 2000u;
            hold_ms   = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 250u;
            if (!period_ms) period_ms = 2000u;
            if (!hold_ms || hold_ms >= period_ms) hold_ms = period_ms / 4u;
            if (mask) {
                configured = 1;
                t0 = (unsigned long)GetTickCount();
                fprintf(stderr, "  PAD: synthesising button mask 0x%02X for "
                        "%u ms every %u ms\n", mask, hold_ms, period_ms);
                fflush(stderr);
            }
        }
    }
    if (!configured)
        return 0;
    now = (unsigned long)GetTickCount();
    phase = (now - t0) % period_ms;
    return (uint8_t)((phase < hold_ms) ? mask : 0u);
}

/* A scripted pad, for driving a title through its menus with nobody at it.
 *
 * RECOMP_PAD_PRESS pulses one digital mask forever, which gets past a
 * "press Start" screen and no further: a menu wants A, which is an analog
 * button on this pad, and it wants presses in order. This takes a timeline:
 *
 *   RECOMP_PAD_SCRIPT=40000:start,44000:a,46000:down,47000:a:400
 *
 * Each entry is <ms>:<button>[+<button>...][:<hold ms>], with times measured
 * from the first report the title asks for (so from enumeration, not from
 * process start) and a default hold of 150 ms. RECOMP_PAD_SCRIPT=@file reads
 * the same syntax from a file, one entry per line or comma separated.
 *
 * Buttons: up down left right start back lthumb rthumb (the digital byte),
 * a b x y black white lt rt (the analog bytes, reported fully pressed, or
 * at a value with "=": lt=200 is the left trigger at 200 of 255), and
 * lleft lright lup ldown / rleft rright rup rdown (a stick at full throw).
 * Every entry is printed as it fires, so a run log says what was pressed when.
 *
 * RECOMP_PAD_LIVE=<file> is the same thing driven while the title runs: each
 * line appended to the file -- <button>[+<button>...][:<hold ms>], no time --
 * is pressed as soon as it is read. With RECOMP_FB_DUMP and a short
 * RECOMP_PB_REPORT_MS that makes a loop: look at the frame, append a press,
 * look again. A fixed timeline cannot do that on a rasterised run, where
 * how long a screen takes to appear depends on how much the software
 * rasteriser has to draw.
 *
 * Like RECOMP_PAD_PRESS this is a bring-up tool, off unless set. */
#define PAD_SCRIPT_MAX 256

typedef struct {
    unsigned long at_ms, hold_ms;
    uint8_t digital;         /* report byte 2 */
    uint8_t analog;          /* bit n = report byte 4 + n */
    uint8_t level[8];        /* that byte's value; 0: fully pressed (255) */
    uint8_t pad;             /* which controller: "p2-a" is pad 1 */
    uint8_t stick;           /* full deflection: 1 L-left 2 L-right 4 L-up
                                8 L-down, then the same four for the right */
    int announced;
} PadStep;

static PadStep s_script[PAD_SCRIPT_MAX];
static int     s_script_len = -1;
static uint8_t s_taken;         /* bit per pad: a controller took it back
                                   from the script (see pad_scripted) */

static int same_word(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int pad_button_bits(const char *name, size_t n, uint8_t *dig,
                           uint8_t *ana, uint8_t *stick)
{
    /* Stick directions, for screens and games that read the stick and
     * ignore the d-pad: Burnout 3 steers and picks map locations with it. */
    static const struct { const char *name; uint8_t bit; } sk[] = {
        {"lleft", 0x01}, {"lright", 0x02}, {"lup", 0x04}, {"ldown", 0x08},
        {"rleft", 0x10}, {"rright", 0x20}, {"rup", 0x40}, {"rdown", 0x80},
    };
    static const struct { const char *name; uint8_t dig, ana; } k[] = {
        {"up", 0x01, 0}, {"down", 0x02, 0}, {"left", 0x04, 0},
        {"right", 0x08, 0}, {"start", 0x10, 0}, {"back", 0x20, 0},
        {"lthumb", 0x40, 0}, {"rthumb", 0x80, 0},
        {"a", 0, 0x01}, {"b", 0, 0x02}, {"x", 0, 0x04}, {"y", 0, 0x08},
        {"black", 0, 0x10}, {"white", 0, 0x20}, {"lt", 0, 0x40}, {"rt", 0, 0x80},
    };
    size_t i;
    for (i = 0; i < sizeof sk / sizeof sk[0]; i++) {
        if (strlen(sk[i].name) == n && same_word(sk[i].name, name, n)) {
            *stick |= sk[i].bit;
            return 1;
        }
    }
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strlen(k[i].name) == n && same_word(k[i].name, name, n)) {
            *dig |= k[i].dig;
            *ana |= k[i].ana;
            return 1;
        }
    }
    return 0;
}

static void pad_script_parse(const char *text)
{
    const char *p = text;

    while (*p && s_script_len < PAD_SCRIPT_MAX) {
        PadStep st;
        char *end;
        const char *b;

        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
            p++;
        if (!*p)
            break;
        memset(&st, 0, sizeof st);
        st.hold_ms = 150;
        st.at_ms = strtoul(p, &end, 0);
        if (end == p || *end != ':') {
            fprintf(stderr, "  PAD: script entry without <ms>: at \"%.20s\"\n", p);
            break;
        }
        b = end + 1;
        /* "p2-" .. "p4-": the step is for that controller, not the first. */
        if ((b[0] == 'p' || b[0] == 'P') && b[1] >= '1' && b[1] <= '4'
            && b[2] == '-') {
            st.pad = (uint8_t)(b[1] - '1');
            b += 3;
        }
        for (;;) {
            size_t n = strcspn(b, "+:,\r\n "), nn = n;
            const char *eq = memchr(b, '=', n);
            uint8_t ana = 0;
            int j;
            /* "lt=200": an analog button at that value instead of 255. */
            if (eq)
                nn = (size_t)(eq - b);
            if (!pad_button_bits(b, nn, &st.digital, &ana, &st.stick))
                fprintf(stderr, "  PAD: unknown button \"%.*s\"\n", (int)n, b);
            else if (eq && !ana)
                fprintf(stderr, "  PAD: \"%.*s\": only an analog button takes a value\n", (int)n, b);
            st.analog |= ana;
            for (j = 0; j < 8; j++)
                if ((ana & (1u << j)) && eq) {
                    unsigned long v = strtoul(eq + 1, NULL, 0);
                    st.level[j] = (uint8_t)(v > 255 ? 255 : v);
                    if (!v)
                        st.analog &= (uint8_t)~(1u << j);
                }
            b += n;
            if (*b != '+')
                break;
            b++;
        }
        if (*b == ':')
            st.hold_ms = strtoul(b + 1, (char **)&b, 0);
        s_script[s_script_len++] = st;
        p = b;
    }
}

static void pad_script_load(void)
{
    const char *spec = recomp_env(RENV_PAD_SCRIPT);

    s_script_len = 0;
    if (!spec || !*spec)
        return;
    if (spec[0] == '@') {
        FILE *f = fopen(spec + 1, "rb");
        static char buf[16384];
        size_t n;
        if (!f) {
            fprintf(stderr, "  PAD: cannot open script %s\n", spec + 1);
            return;
        }
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        pad_script_parse(buf);
    } else {
        pad_script_parse(spec);
    }
    fprintf(stderr, "  PAD: script of %d step(s)\n", s_script_len);
    fflush(stderr);
}

/* Drop steps that have finished, so a long live session never fills up. */
static void pad_script_compact(unsigned long t)
{
    int i, n = 0;
    for (i = 0; i < s_script_len; i++)
        if (t < s_script[i].at_ms + s_script[i].hold_ms)
            s_script[n++] = s_script[i];
    s_script_len = n;
}

/* Lines of RECOMP_PAD_LIVE taken so far: an in-process driver
 * (xbox_autoplay.c) waits on it as the scene scripts wait on the log line. */
static volatile unsigned s_live_taken;
unsigned usb_gamepad_live_taken(void)
{
    return s_live_taken;
}

#define PAD_LIVE_STALE_S 30

/* Read whatever has been appended to RECOMP_PAD_LIVE since last time, and
 * schedule each complete line at t. Polled at most every 50 ms. */
static void pad_live_poll(unsigned long t)
{
    static const char *path;
    static int checked;
    static long consumed;
    static unsigned long last_poll;
    static char pending[512];
    static size_t npending;
    static unsigned long next_free[USB_GAMEPAD_MAX];
    FILE *f;
    int c;

    if (!checked) {
        checked = 1;
        path = recomp_env(RENV_PAD_LIVE);
        if (path && *path) {
            struct stat sb;
            fprintf(stderr, "  PAD: live input from %s\n", path);
            /* Lines from an earlier run are not this run's: a driver empties
             * the file before it starts the game and appends while it runs.
             * A file that already has lines, last written more than
             * PAD_LIVE_STALE_S before the title's first pad read, was left
             * behind (a driver killed before its clean-up, with the variable
             * still set in a launcher's environment file); replaying it
             * would press buttons in somebody's own session (BO-96). */
            if (stat(path, &sb) == 0 && sb.st_size > 0
                    && time(NULL) - sb.st_mtime > PAD_LIVE_STALE_S) {
                consumed = (long)sb.st_size;
                fprintf(stderr, "  PAD: %s has %ld bytes from an earlier run "
                        "(written %ld s ago): ignored, only new lines are pressed\n",
                        path, consumed, (long)(time(NULL) - sb.st_mtime));
            }
            fflush(stderr);
        }
    }
    if (!path || !*path || (t - last_poll < 50 && last_poll))
        return;
    last_poll = t;
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fseek(f, consumed, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    while ((c = fgetc(f)) != EOF) {
        consumed++;
        if (c == '\n' || c == '\r') {
            if (npending) {
                char line[560];
                /* Several lines in one read go one after another, with a
                 * 200 ms release between: a title polling its pad a few times
                 * a second would otherwise see "down" and "a" in the same
                 * report, which is a different input from "down, then a". */
                /* Serialised per pad: pad 2's throttle must not wait out
                 * pad 1's -- split-screen players press at the same time. */
                int pd = (pending[0] == 'p' || pending[0] == 'P')
                         && pending[1] >= '1' && pending[1] <= '4'
                         && pending[2] == '-' ? pending[1] - '1' : 0;
                unsigned long at = t > next_free[pd] ? t : next_free[pd];
                pending[npending] = 0;
                snprintf(line, sizeof line, "%lu:%s", at, pending);
                if (s_script_len >= PAD_SCRIPT_MAX - 1)
                    pad_script_compact(t);
                pad_script_parse(line);
                if (s_script_len > 0)
                    next_free[pd] = at + s_script[s_script_len - 1].hold_ms + 200;
                fprintf(stderr, "  PAD: live \"%s\" at t=%lu ms%s\n", pending, at,
                        (s_taken & (1u << pd)) ? " (ignored: a controller has the pad)" : "");
                fflush(stderr);
                s_live_taken++;
                npending = 0;
            }
        } else if (npending < sizeof pending - 1) {
            pending[npending++] = (char)c;
        }
    }
    fclose(f);
}

/* RECOMP_KEY_LIVE=<file>: keyboard keys held for a headless run, one line
 * appended to the file at a time: <key>[+<key>...][:<hold ms>] (default 150),
 * keys named w a s d (any letter or digit), up down left right, enter esc
 * space backspace shift ctrl. They go into the same key table the window's
 * events do (keyboard.c), so the keyboard map, the merge with a pad and the
 * overlay's block are what is tested. Unlike RECOMP_PAD_LIVE the lines are
 * not spaced out: keys on separate lines are held together. Not read in a
 * run with a pad script (those runs do not read the keyboard at all). */
#define KEY_LIVE_MAX 64
static struct { int vk[4]; int n; unsigned long at, hold; int down; } s_klive[KEY_LIVE_MAX];

static int key_live_vk(const char *w, size_t n)
{
    static const struct { const char *name; int vk; } names[] = {
        {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27}, {"enter", 0x0D},
        {"esc", 0x1B}, {"space", 0x20}, {"backspace", 0x08}, {"shift", 0x10}, {"ctrl", 0x11},
    };
    size_t i;
    if (n == 1 && isalnum((unsigned char)w[0]))
        return toupper((unsigned char)w[0]);
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].name) == n && same_word(names[i].name, w, n))
            return names[i].vk;
    return 0;
}

static void key_live_line(const char *line, unsigned long t)
{
    size_t k;
    int slot = -1;
    unsigned long hold = 150;
    const char *b = line, *colon = strchr(line, ':');

    for (k = 0; k < KEY_LIVE_MAX; k++)
        if (s_klive[k].n == 0) { slot = (int)k; break; }
    if (slot < 0) {
        fprintf(stderr, "  KEY: live \"%s\": too many keys in flight\n", line);
        return;
    }
    if (colon)
        hold = strtoul(colon + 1, NULL, 0);
    s_klive[slot].n = 0;
    for (;;) {
        size_t n = strcspn(b, "+:");
        int vk = key_live_vk(b, n);
        if (!vk)
            fprintf(stderr, "  KEY: unknown key \"%.*s\"\n", (int)n, b);
        else if (s_klive[slot].n < 4)
            s_klive[slot].vk[s_klive[slot].n++] = vk;
        b += n;
        if (*b != '+')
            break;
        b++;
    }
    s_klive[slot].at = t;
    s_klive[slot].hold = hold;
    s_klive[slot].down = 0;
    fprintf(stderr, "  KEY: live \"%s\" at t=%lu ms for %lu ms\n", line, t, hold);
    fflush(stderr);
}

static void key_live_poll(unsigned long t)
{
    static const char *path;
    static int checked;
    static long consumed;
    static unsigned long last_poll;
    static char pending[128];
    static size_t npending;
    FILE *f;
    int c;
    size_t k;

    if (!checked) {
        checked = 1;
        path = recomp_env(RENV_KEY_LIVE);
        if (path && *path)
            fprintf(stderr, "  KEY: live input from %s\n", path);
    }
    if (!path || !*path)
        return;
    for (k = 0; k < KEY_LIVE_MAX; k++) {
        int j;
        if (!s_klive[k].n || t < s_klive[k].at)
            continue;
        if (!s_klive[k].down) {
            s_klive[k].down = 1;
            for (j = 0; j < s_klive[k].n; j++)
                xbox_InputKeySet(s_klive[k].vk[j], 1);
        } else if (t >= s_klive[k].at + s_klive[k].hold) {
            for (j = 0; j < s_klive[k].n; j++)
                xbox_InputKeySet(s_klive[k].vk[j], 0);
            s_klive[k].n = 0;
        }
    }
    if (t - last_poll < 50 && last_poll)
        return;
    last_poll = t;
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fseek(f, consumed, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    while ((c = fgetc(f)) != EOF) {
        consumed++;
        if (c == '\n' || c == '\r') {
            if (npending) {
                pending[npending] = 0;
                key_live_line(pending, t);
                npending = 0;
            }
        } else if (npending < sizeof pending - 1) {
            pending[npending++] = (char)c;
        }
    }
    fclose(f);
}

static void key_live_report(void)
{
    static unsigned long t0;
    unsigned long now = (unsigned long)GetTickCount() + (unsigned long)(xbox_GuestSkewNs() / 1000000u);

    if (!t0)
        t0 = now;
    key_live_poll(now - t0);
}


/* Scripted input and a person's controller on the same pad (BO-96).
 *
 * A step sets its buttons in the report whatever the host pad says, so while
 * a driver holds the accelerator ("rt:600000", the scene drivers' race) the
 * title reads the right trigger at 255 however the controller is held: a
 * person who picks up a run a script is driving brakes to a stop with the
 * left trigger and then cannot reverse, because the title is still told to
 * accelerate. So:
 *
 *   - a pad the script drives is announced (the log line below, and
 *     xbox_InputSetScriptedPads for the overlay's note);
 *   - the first button or trigger a controller holds on that pad takes the
 *     pad back: the script's steps stop reaching it for the rest of the run
 *     (the lines of RECOMP_PAD_LIVE are still read and logged, as ignored);
 *   - a headless run, which nobody is looking at, does not read the host's
 *     controllers while a script is set: the pad in a player's hands, who
 *     may be playing another instance, neither steers a test nor ends it.
 *
 * RECOMP_DEBUG=pad_script_keep=1 is the old behaviour: script and controller
 * merged, the script's buttons on top. */
static int s_scripted = -1;     /* a script is set for this run */
static int s_script_keep;
static int s_script_only;       /* headless: host controllers not read */

static int pad_scripted(void)
{
    if (s_scripted < 0) {
        const char *a = recomp_env(RENV_PAD_SCRIPT), *b = recomp_env(RENV_PAD_LIVE);
        const char *k = recomp_env(RENV_PAD_SCRIPT_KEEP), *h = recomp_env(RENV_HEADLESS);
        const char *o = recomp_env(RENV_PAD_SCRIPT_ONLY);
        s_scripted = (a && *a) || (b && *b);
        s_script_keep = k && *k && *k != '0';
        s_script_only = s_scripted && !s_script_keep
                     && ((h && *h && *h != '0') || (o && *o && *o != '0'));
        if (s_scripted) {
            fprintf(stderr, "  PAD: SCRIPTED INPUT drives the pads in this run (%s)%s\n",
                    (b && *b) ? "RECOMP_PAD_LIVE" : "RECOMP_PAD_SCRIPT",
                    s_script_keep ? "; pad_script_keep: merged with the controllers"
                    : s_script_only ? "; headless: the host's controllers are not read"
                    : "; a controller's first button takes its pad back");
            fflush(stderr);
            xbox_InputSetScriptedPads((1u << USB_GAMEPAD_MAX) - 1u);
        }
    }
    return s_scripted;
}

/* A controller holds something on `pad`: from now on the script leaves that
 * pad alone. Sticks do not count (a stick that drifts is not a person). */
static void pad_script_takeover(int pad, const XBOX_GAMEPAD *g)
{
    int i, held = g->wButtons != 0;

    if (!s_scripted || s_script_keep || (s_taken & (1u << pad)))
        return;
    for (i = 0; i < 8; i++)
        if (g->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            held = 1;
    if (!held)
        return;
    s_taken |= (uint8_t)(1u << pad);
    xbox_InputSetScriptedPads(((1u << USB_GAMEPAD_MAX) - 1u) & ~(unsigned)s_taken);
    fprintf(stderr, "  PAD: a controller is used on pad %d: the script no longer "
            "drives that pad (RECOMP_DEBUG=pad_script_keep=1 keeps both)\n", pad);
    fflush(stderr);
}

int usb_gamepad_script_drives(int pad)
{
    return pad_scripted() && !(s_taken & (1u << (pad & 3)));
}

static void pad_script_apply(int pad, uint8_t *out)
{
    static unsigned long t0;
    unsigned long now, t;
    int i, j;

    if (s_script_len < 0)
        pad_script_load();
    /* On the guest's clock (host_time.h), so a step's hold is as long to
     * the title in a run that does not wait for the clock as in any other. */
    now = (unsigned long)GetTickCount() + (unsigned long)(xbox_GuestSkewNs() / 1000000u);
    if (!t0)
        t0 = now;
    t = now - t0;
    pad_live_poll(t);
    if (s_script_len == 0)
        return;
    for (i = 0; i < s_script_len; i++) {
        PadStep *st = &s_script[i];
        if (st->pad != pad || (s_taken & (1u << pad)))
            continue;
        if (t < st->at_ms || t >= st->at_ms + st->hold_ms)
            continue;
        if (!st->announced) {
            st->announced = 1;
            fprintf(stderr, "  PAD: t=%lu ms step %d (digital 0x%02X analog 0x%02X)\n",
                    t, i, st->digital, st->analog);
            fflush(stderr);
        }
        out[2] |= st->digital;
        for (j = 0; j < 8; j++)
            if (st->analog & (1u << j))
                out[4 + j] = st->level[j] ? st->level[j] : 0xFF;
        if (st->stick) {
            /* Axes at 12..19: LX LY RX RY, signed 16-bit, up positive. */
            static const int16_t full = 32767;
            int16_t ax[4];
            memcpy(ax, &out[12], sizeof ax);
            if (st->stick & 0x01) ax[0] = (int16_t)-full;
            if (st->stick & 0x02) ax[0] = full;
            if (st->stick & 0x04) ax[1] = full;
            if (st->stick & 0x08) ax[1] = (int16_t)-full;
            if (st->stick & 0x10) ax[2] = (int16_t)-full;
            if (st->stick & 0x20) ax[2] = full;
            if (st->stick & 0x40) ax[3] = full;
            if (st->stick & 0x80) ax[3] = (int16_t)-full;
            memcpy(&out[12], ax, sizeof ax);
        }
    }
}

int usb_gamepad_report(int pad, uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    const XBOX_GAMEPAD *g;
    uint8_t synth = synthetic_buttons();
    int i;

    if (max < 20)
        return 0;
    memset(out, 0, 20);
    out[0] = 0;
    out[1] = 20;
    rumble_refresh(pad & 3);

    if (!pad_scripted())
        key_live_report();

    /* A disconnected host pad is not an error here: the device is present on
     * the bus either way, it just reports nothing pressed. */
    /* RECOMP_INPUT_DIAG: the whole chain on one line, once a second.
     *
     * "Nothing happens when I press a key" has several candidate causes and
     * guessing between them costs a rebuild each: the variable not read,
     * XInput claiming a pad so the keyboard fallback never runs, the window
     * not receiving the key, or the report going out without it. Printing
     * all four together answers it in one run.
     *
     * It samples the held state, so pair it with the window's own
     * RECOMP_KEY_TRACE: that answers "did the key arrive", this answers
     * "is the chain wired". */
    {
        static int diag = -1;
        if (diag < 0)
            diag = recomp_env(RENV_INPUT_DIAG) != NULL;
        if (diag) {
            static unsigned long last;
            unsigned long now = (unsigned long)GetTickCount();
            if (now - last > 1000) {
                XBOX_INPUT_STATE probe;
                DWORD rc = xbox_InputGetState(0, &probe);
                last = now;
                fprintf(stderr, "  [INPUT] kbd_env=%d window_has_RETURN=%d "
                        "InputGetState=%lu buttons=0x%04X"
                        " lx=%d ly=%d rx=%d ry=%d\n",
                        recomp_env(RENV_KEYBOARD) ? 1 : 0,
                        xbox_FramebufferKeyDown(0x0D),
                        (unsigned long)rc,
                        rc == 0 ? probe.Gamepad.wButtons : 0,
                        rc == 0 ? probe.Gamepad.sThumbLX : 0,
                        rc == 0 ? probe.Gamepad.sThumbLY : 0,
                        rc == 0 ? probe.Gamepad.sThumbRX : 0,
                        rc == 0 ? probe.Gamepad.sThumbRY : 0);
                fflush(stderr);
            }
        }
    }

    /* The synthetic press and the host pad drive pad n from host pad n. */
    if (pad != 0)
        synth = 0;
    /* Headless with a script: the host's controllers are left alone. */
    if ((pad_scripted() && s_script_only)
            || xbox_InputGetState((DWORD)pad, &state) != 0) {
        out[2] = synth;
        pad_script_apply(pad, out);
        xbox_LatPadReport(pad, out, 20);
        return 20;
    }

    g = &state.Gamepad;
    pad_script_takeover(pad & 3, g);
    out[2] = (uint8_t)((g->wButtons & 0xFF) | synth);
    out[3] = (uint8_t)((g->wButtons >> 8) & 0xFF);
    for (i = 0; i < 8; i++)
        out[4 + i] = g->bAnalogButtons[i];
    out[12] = (uint8_t)(g->sThumbLX & 0xFF);
    out[13] = (uint8_t)((g->sThumbLX >> 8) & 0xFF);
    out[14] = (uint8_t)(g->sThumbLY & 0xFF);
    out[15] = (uint8_t)((g->sThumbLY >> 8) & 0xFF);
    out[16] = (uint8_t)(g->sThumbRX & 0xFF);
    out[17] = (uint8_t)((g->sThumbRX >> 8) & 0xFF);
    out[18] = (uint8_t)(g->sThumbRY & 0xFF);
    out[19] = (uint8_t)((g->sThumbRY >> 8) & 0xFF);
    pad_script_apply(pad, out);
    xbox_LatPadReport(pad, out, 20);
    return 20;
}

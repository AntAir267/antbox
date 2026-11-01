/*
 * input_rumble test: see CMakeLists.txt.
 *
 * The table is the decision's (Burnout 3's crash: 52054, then 18349, 5734,
 * 1791 as it dies away, and an engine hum of 658), written out here and not
 * taken from the code under test.
 */
#include "rumble_curve.h"

#include <math.h>
#include <stdio.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PEAK 52054.0

/* To two decimals. */
static int near2(double got, double want)
{
    return fabs(floor(got * 100.0 + 0.5) / 100.0 - want) < 1e-9;
}

/* ── 1. the table ─────────────────────────────────────────────────────── */

static void test_table(void)
{
    static const double strength[4] = { 1.0, 1.5, 1.75, 2.0 };
    static const struct { unsigned speed; double want[4]; } row[] = {
        { 52054, { 1.00, 1.00, 1.00, 1.00 } },
        { 18349, { 0.35, 0.50, 0.55, 0.59 } },
        {  5734, { 0.11, 0.23, 0.28, 0.33 } },
        {  1791, { 0.03, 0.11, 0.15, 0.19 } },
        {   658, { 0.00, 0.00, 0.00, 0.00 } },      /* gated */
    };

    for (unsigned r = 0; r < sizeof row / sizeof row[0]; r++)
        for (int s = 0; s < 4; s++) {
            /* A motor at rest given this speed: gate, then curve. */
            xbox_rumble_motor m = { 0, 0.0 };
            xbox_rumble_params p = XBOX_RUMBLE_PARAMS_DEFAULT;
            double got;
            p.peak = PEAK;
            p.strength = strength[s];
            got = xbox_RumbleStep(&m, row[r].speed, &p, 0.0);
            CHECK(near2(got, row[r].want[s]), "speed %u strength %.2f: %.4f, want %.2f",
                  row[r].speed, strength[s], got, row[r].want[s]);
        }
}

/* ── 2. the curve's ends ──────────────────────────────────────────────── */

static void test_curve(void)
{
    /* Linear at 1.0: the title's own. */
    CHECK(fabs(xbox_RumbleCurve(32768, 65535.0, 1.0) - 32768 / 65535.0) < 1e-12, "1.0 not linear");
    CHECK(fabs(xbox_RumbleCurve(13014, PEAK, 1.0) - 0.25) < 1e-4, "peak not the divisor");
    /* Full only at the peak, and never past it. */
    CHECK(xbox_RumbleCurve(52054, PEAK, 3.0) == 1.0, "peak not full");
    CHECK(xbox_RumbleCurve(65535, PEAK, 1.0) == 1.0, "above the peak not clamped");
    for (double s = 0.25; s <= 3.0; s += 0.25)
        CHECK(xbox_RumbleCurve(52053, PEAK, s) < 1.0, "strength %.2f clips under the peak", s);
    /* The strength lifts, in order, and keeps the order of the speeds. */
    CHECK(xbox_RumbleCurve(5734, PEAK, 1.0) < xbox_RumbleCurve(5734, PEAK, 1.5)
          && xbox_RumbleCurve(5734, PEAK, 1.5) < xbox_RumbleCurve(5734, PEAK, 2.0)
          && xbox_RumbleCurve(5734, PEAK, 2.0) < xbox_RumbleCurve(5734, PEAK, 3.0),
          "strength does not lift");
    CHECK(xbox_RumbleCurve(18349, PEAK, 3.0) < xbox_RumbleCurve(40000, PEAK, 3.0)
          && xbox_RumbleCurve(40000, PEAK, 3.0) < xbox_RumbleCurve(52054, PEAK, 3.0),
          "strength 3 flattens the top");
    /* Under 1 the low speeds drop. */
    CHECK(xbox_RumbleCurve(18349, PEAK, 0.5) < xbox_RumbleCurve(18349, PEAK, 1.0), "0.5 lifts");
    /* Off, and nothing from nothing. */
    CHECK(xbox_RumbleCurve(52054, PEAK, 0.0) == 0.0, "strength 0 not off");
    CHECK(xbox_RumbleCurve(52054, PEAK, -1.0) == 0.0, "negative strength not off");
    CHECK(xbox_RumbleCurve(0, PEAK, 2.0) == 0.0, "speed 0 not 0");
    /* A peak that is none: the default. */
    CHECK(fabs(xbox_RumbleCurve(32768, 0.0, 1.0) - 32768 / 65535.0) < 1e-12, "peak 0");
}

/* ── 3. the gate ──────────────────────────────────────────────────────── */

static void test_gate(void)
{
    int open = 0;

    CHECK(!xbox_RumbleGate(&open, 0, 800, 1200), "0 open");
    CHECK(!xbox_RumbleGate(&open, 658, 800, 1200), "the hum open");
    CHECK(!xbox_RumbleGate(&open, 799, 800, 1200), "799 open");
    /* Off: not on again under 1200. */
    CHECK(!xbox_RumbleGate(&open, 800, 800, 1200), "800 opened a closed gate");
    CHECK(!xbox_RumbleGate(&open, 1199, 800, 1200), "1199 opened a closed gate");
    CHECK(xbox_RumbleGate(&open, 1200, 800, 1200), "1200 did not open");
    /* On: stays on down to 800. */
    CHECK(xbox_RumbleGate(&open, 1199, 800, 1200), "1199 closed an open gate");
    CHECK(xbox_RumbleGate(&open, 800, 800, 1200), "800 closed an open gate");
    CHECK(!xbox_RumbleGate(&open, 799, 800, 1200), "799 did not close");
    CHECK(!xbox_RumbleGate(&open, 1000, 800, 1200), "1000 reopened");
    CHECK(xbox_RumbleGate(&open, 52054, 800, 1200), "full did not open");
    /* No gate. */
    open = 0;
    CHECK(xbox_RumbleGate(&open, 1, 0, 0) && !xbox_RumbleGate(&open, 0, 0, 0), "gate 0");

    /* Through the motor: a hum wandering about the thresholds. */
    {
        xbox_rumble_motor m = { 0, 0.0 };
        xbox_rumble_params p = XBOX_RUMBLE_PARAMS_DEFAULT;
        CHECK(xbox_RumbleStep(&m, 1000, &p, 0.0) == 0.0, "1000 from rest");
        CHECK(xbox_RumbleStep(&m, 1300, &p, 5.0) > 0.0, "1300");
        CHECK(xbox_RumbleStep(&m, 1000, &p, 5.0) > 0.0, "1000 after 1300");
        /* Closed: down from there, not held. */
        xbox_RumbleStep(&m, 700, &p, 0.0);
        CHECK(xbox_RumbleStep(&m, 700, &p, 1000.0) == 0.0, "700 did not let go");
        CHECK(xbox_RumbleStep(&m, 1000, &p, 5.0) == 0.0, "1000 after 700");
    }
}

/* ── 4. the release ───────────────────────────────────────────────────── */

static void test_release(void)
{
    xbox_rumble_params p = XBOX_RUMBLE_PARAMS_DEFAULT;
    xbox_rumble_motor m = { 0, 0.0 };
    double v;

    p.peak = PEAK;
    /* A rise is immediate, from rest and from a level. */
    CHECK(xbox_RumbleStep(&m, 52054, &p, 0.0) == 1.0, "rise from rest");
    CHECK(xbox_RumbleStep(&m, 52054, &p, 500.0) == 1.0, "a held speed moved");
    /* The new speed at dt 0: nothing has fallen yet. */
    CHECK(xbox_RumbleStep(&m, 0, &p, 0.0) == 1.0, "fell in no time");
    /* A step down from full: about 37 % (1/e) after 90 ms, in one step... */
    v = xbox_RumbleStep(&m, 0, &p, 90.0);
    CHECK(fabs(v - 0.3679) < 0.001, "after 90 ms: %.4f", v);
    /* ...or in many: the poll's rate does not change the fall. */
    m.open = 0; m.level = 0.0;
    xbox_RumbleStep(&m, 52054, &p, 0.0);
    xbox_RumbleStep(&m, 0, &p, 0.0);
    for (int i = 0; i < 90; i++)
        v = xbox_RumbleStep(&m, 0, &p, 1.0);
    CHECK(fabs(v - 0.3679) < 0.001, "after 90 x 1 ms: %.4f", v);
    for (int i = 0; i < 45; i++)
        v = xbox_RumbleStep(&m, 0, &p, 2.0);
    CHECK(fabs(v - 0.1353) < 0.001, "after 180 ms: %.4f", v);
    /* It ends: exactly 0, inside half a second. */
    for (int i = 0; i < 80; i++)
        v = xbox_RumbleStep(&m, 0, &p, 4.0);
    CHECK(v == 0.0, "never reached 0: %g", v);

    /* A fall to a lower speed lands on that speed's level, not on 0. */
    m.open = 0; m.level = 0.0;
    xbox_RumbleStep(&m, 52054, &p, 0.0);
    xbox_RumbleStep(&m, 18349, &p, 0.0);
    v = xbox_RumbleStep(&m, 18349, &p, 90.0);
    CHECK(fabs(v - (0.3525 + (1.0 - 0.3525) * 0.3679)) < 0.001, "to a lower speed: %.4f", v);
    v = xbox_RumbleStep(&m, 18349, &p, 2000.0);
    CHECK(fabs(v - 0.3525) < 0.0001, "did not land: %.4f", v);
    /* And a rise out of a fall is immediate. */
    xbox_RumbleStep(&m, 0, &p, 0.0);
    xbox_RumbleStep(&m, 0, &p, 30.0);
    CHECK(xbox_RumbleStep(&m, 52054, &p, 1.0) == 1.0, "rise out of a fall");
}

/* ── 5. the device vibrator's steps ───────────────────────────────────── */

static void test_quantise(void)
{
    int playing, changes, prev;

    /* From rest: the nearest of 16 levels, never 0 for something wanted. */
    CHECK(xbox_RumbleQuantise(0, 0) == 0, "0");
    CHECK(xbox_RumbleQuantise(0, 1) == 16, "1 -> %d", xbox_RumbleQuantise(0, 1));
    CHECK(xbox_RumbleQuantise(0, 23) == 16 && xbox_RumbleQuantise(0, 24) == 32, "rounding");
    CHECK(xbox_RumbleQuantise(0, 128) == 128, "128");
    CHECK(xbox_RumbleQuantise(0, 255) == 255 && xbox_RumbleQuantise(0, 999) == 255, "top");
    for (int a = 1; a <= 255; a++) {
        int q = xbox_RumbleQuantise(0, a);
        CHECK(q == 255 || (q % 16 == 0 && q >= 16 && q <= 240), "amp %d -> %d", a, q);
    }
    /* Hysteresis: nothing changes inside one step of what plays. */
    CHECK(xbox_RumbleQuantise(128, 113) == 128 && xbox_RumbleQuantise(128, 143) == 128, "inside a step");
    CHECK(xbox_RumbleQuantise(128, 112) == 112 && xbox_RumbleQuantise(128, 144) == 144, "a step away");
    CHECK(xbox_RumbleQuantise(255, 240) == 255 && xbox_RumbleQuantise(255, 239) == 240, "from the top");
    /* 0 is 0 at once, whatever plays. */
    CHECK(xbox_RumbleQuantise(128, 0) == 0 && xbox_RumbleQuantise(16, 0) == 0, "stop");
    CHECK(xbox_RumbleQuantise(16, 1) == 16, "a dying level restarted the motor");

    /* A level wandering by less than a step never changes the motor (the
     * old steps of 16 changed it at every crossing of a multiple of 16). */
    playing = xbox_RumbleQuantise(0, 128);
    changes = 0;
    for (int i = 0; i < 200; i++) {
        int q = xbox_RumbleQuantise(playing, 128 + (i % 2 ? 9 : -9));
        changes += q != playing;
        playing = q;
    }
    CHECK(changes == 0, "a wandering level changed the motor %d times", changes);

    /* A fall from full to nothing: every level once at most, in order. */
    playing = 255;
    changes = 0;
    prev = 255;
    for (int a = 255; a >= 0; a--) {
        int q = xbox_RumbleQuantise(playing, a);
        if (q != playing) {
            changes++;
            CHECK(q < prev, "not falling: %d after %d", q, prev);
            prev = q;
        }
        playing = q;
    }
    CHECK(playing == 0 && changes <= 16, "a fall took %d changes, ended at %d", changes, playing);
}

int main(void)
{
    test_table();
    test_curve();
    test_gate();
    test_release();
    test_quantise();
    printf("input_rumble: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

/* recomp_patch: names, marks and settings all registered and read back. */
#include "recomp_patch.h"

#include <stdio.h>
#include <string.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

#define NNAMES 40
static char s_names[NNAMES][16];
static double fn_last(void) { return 7.5; }

static int s_marked[NNAMES];
#define MARK_FN(i) static void mark_##i(void) { s_marked[i]++; }
MARK_FN(0) MARK_FN(1) MARK_FN(2) MARK_FN(3) MARK_FN(4) MARK_FN(5) MARK_FN(6)
MARK_FN(7) MARK_FN(8) MARK_FN(9) MARK_FN(10) MARK_FN(11) MARK_FN(12)
MARK_FN(13) MARK_FN(14) MARK_FN(15) MARK_FN(16) MARK_FN(17) MARK_FN(18)
MARK_FN(19)
static void (*const k_marks[20])(void) = {
    mark_0, mark_1, mark_2, mark_3, mark_4, mark_5, mark_6, mark_7, mark_8,
    mark_9, mark_10, mark_11, mark_12, mark_13, mark_14, mark_15, mark_16,
    mark_17, mark_18, mark_19,
};

int main(void)
{
    int i;

    /* Burnout 3 registers 17 names (int027): every one must read back,
     * the last one too. */
    for (i = 0; i < NNAMES; i++) {
        snprintf(s_names[i], sizeof s_names[i], "v%d", i);
        if (i == NNAMES - 1)
            recomp_patch_set_var_fn(s_names[i], fn_last);
        else
            recomp_patch_set_var(s_names[i], (double)i + 0.25);
    }
    for (i = 0; i < NNAMES - 1; i++) {
        CHECK(recomp_patch_has_var(s_names[i]));
        CHECK(recomp_patch_var(s_names[i]) == (double)i + 0.25);
    }
    CHECK(recomp_patch_has_var(s_names[NNAMES - 1]));
    CHECK(recomp_patch_var(s_names[NNAMES - 1]) == 7.5);
    /* Set again: the same slot, a new value. */
    recomp_patch_set_var("v3", 99.0);
    CHECK(recomp_patch_var("v3") == 99.0);
    CHECK(!recomp_patch_has_var("nobody"));
    CHECK(recomp_patch_var("nobody") == 0.0);

    /* Marks past 16. */
    for (i = 0; i < 20; i++) {
        static char names[20][16];
        snprintf(names[i], sizeof names[i], "m%d", i);
        recomp_patch_on_mark(names[i], k_marks[i]);
    }
    for (i = 0; i < 20; i++) {
        char n[16];
        snprintf(n, sizeof n, "m%d", i);
        recomp_patch_mark(n);
        CHECK(s_marked[i] == 1);
    }
    recomp_patch_mark("unregistered");

    /* A setting enabled reads as such; one nobody enabled is off. */
    recomp_patch_enable("display.hud_fix", RENV_GFX_HUD_FIX);
    CHECK(recomp_patch_enabled("display.hud_fix"));
    CHECK(!recomp_patch_enabled("display.nothing"));
    CHECK(!recomp_patch_on("display.nothing", "1", 0));

    if (s_fail)
        return 1;
    printf("recomp_patch: ok\n");
    return 0;
}

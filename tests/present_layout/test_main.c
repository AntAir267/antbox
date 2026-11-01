/* nv2a_present_layout: what a window shows of a frame, and where; and the
 * visible-columns helpers a host lays a HUD out by.
 *
 * golden.inc is the output of the loop in same_as_golden at the commit
 * before these helpers took an aspect: three aspects x overscan off/crop x
 * fit off/fill16x10 x three filters x four frame sizes x nine outputs.
 * Every one must give the same rectangles, with no frame-aspect function,
 * with one that answers 0, and after one was used and removed. */
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct row { int aspect, crop, fill, filter; uint32_t fw, fh, dw, dh; int whole; int32_t s[4], d[4]; };
static const struct row k_golden[] = {
#include "golden.inc"
};

static void mode(int aspect, int crop, int fill)
{
    struct nv2a_host_opts h = *nv2a_host_opts();
    h.present_aspect = aspect;
    nv2a_host_opts_set(&h);
    nv2a_present_set_overscan(crop);
    nv2a_present_set_fit(fill);
}

static int same_as_golden(void)
{
    size_t i, n = sizeof k_golden / sizeof k_golden[0];
    for (i = 0; i < n; i++) {
        const struct row *g = &k_golden[i];
        struct nv2a_rect s, d;
        int whole;
        mode(g->aspect, g->crop, g->fill);
        whole = nv2a_present_layout(g->fw, g->fh, g->dw, g->dh, g->filter, &s, &d);
        if (whole != g->whole || s.x != g->s[0] || s.y != g->s[1] || s.w != g->s[2] || s.h != g->s[3]
            || d.x != g->d[0] || d.y != g->d[1] || d.w != g->d[2] || d.h != g->d[3]) {
            printf("FAIL: row %u differs from the golden table\n", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/* The columns of a 640x480 frame an output shows, in the configured mode
 * and for a 4:3 picture in the same output. */
static int visible(int aspect, int crop, int fill, uint32_t dw, uint32_t dh,
                   double want0, double want1, double ref0, double ref1)
{
    struct nv2a_rect s, d;
    double x0, x1, r0, r1;
    mode(aspect, crop, fill);
    nv2a_present_layout(640, 480, dw, dh, NV2A_PRESENT_LINEAR, &s, &d);   /* a present */
    nv2a_present_visible_x(640, 480, &x0, &x1);
    nv2a_present_visible_x_at(640, 480, NV2A_ASPECT_4_3, &r0, &r1);
    if (x0 != want0 || x1 != want1 || r0 != ref0 || r1 != ref1) {
        printf("visible: aspect %d crop %d fill %d %ux%u: %.0f..%.0f (4:3: %.0f..%.0f)\n",
               aspect, crop, fill, dw, dh, x0, x1, r0, r1);
        return 0;
    }
    return 1;
}

/* A frame's own aspect, by flip number (nv2a_present_frame_aspect_fn). */
static double s_answer;
static uint32_t s_asked;
static double aspect_of(uint32_t flip) { s_asked = flip; return flip == 7 ? s_answer : 0.0; }

static int frame_aspect(void)
{
    struct nv2a_rect s, d;
    double x0, x1;

    /* A function that answers 0 for the frame: every layout as recorded. */
    nv2a_present_frame_aspect_fn(aspect_of);
    nv2a_present_frame(3);
    CHECK(s_asked == 3);
    CHECK(same_as_golden());

    /* 16:9 configured, the overscan crop, a 21:9 output (2560x1080). An
     * ordinary frame: 16:9 in the middle, bars at the sides. */
    mode(NV2A_ASPECT_16_9, 1, 0);
    nv2a_present_frame(3);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.x == 21 && s.y == 16 && s.w == 598 && s.h == 448);
    CHECK(d.x == 320 && d.y == 0 && d.w == 1920 && d.h == 1080);
    /* The wide frame (flip 7, 64:27): the whole output, the same crop. */
    s_answer = 64.0 / 27.0;
    nv2a_present_frame(7);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.x == 21 && s.y == 16 && s.w == 598 && s.h == 448);
    CHECK(d.x == 0 && d.y == 0 && d.w == 2560 && d.h == 1080);
    /* Its visible columns, and a 4:3 picture's in the same output. */
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    nv2a_present_visible_x_at(640, 480, NV2A_ASPECT_4_3, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    /* The wide frame in a 16:9 output: letterboxed, never cropped. */
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.w == 598 && d.x == 0 && d.w == 1920 && d.h == 810 && d.y == 135);
    /* 32:9 in a 32:9 output. */
    s_answer = 32.0 / 9.0;
    nv2a_present_frame(7);
    nv2a_present_layout(640, 480, 3840, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.x == 0 && d.y == 0 && d.w == 3840 && d.h == 1080);
    CHECK(nv2a_present_output_aspect() > 3.55 && nv2a_present_output_aspect() < 3.56);
    /* The next frame is an ordinary one again. */
    nv2a_present_frame(8);
    nv2a_present_layout(640, 480, 3840, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.x == 960 && d.w == 1920 && d.h == 1080);
    /* What a frame of a given shape will show, asked before it is presented
     * (a stage set up for 64:27 while the loading screen is up): in a 21:9
     * output all of the crop; in the Deck's 16:10 with the fill, the middle. */
    mode(NV2A_ASPECT_16_9, 1, 0);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    nv2a_present_visible_x_ratio(640, 480, 64.0 / 27.0, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    mode(NV2A_ASPECT_16_9, 1, 1);
    nv2a_present_layout(640, 480, 1280, 800, NV2A_PRESENT_LINEAR, &s, &d);
    nv2a_present_visible_x_ratio(640, 480, 64.0 / 27.0, &x0, &x1);
    CHECK(x0 == 118.0 && x1 == 521.0);
    nv2a_present_visible_x_ratio(640, 480, 0.0, &x0, &x1);
    CHECK(x0 == 51.0 && x1 == 589.0);

    /* The window's opening shape is its own knob. */
    CHECK(nv2a_present_window_width(640, 480) == nv2a_present_width(640, 480));
    nv2a_present_window_aspect(64.0 / 27.0);
    mode(NV2A_ASPECT_16_9, 1, 0);
    CHECK(nv2a_present_window_width(640, 480) == 1138 && nv2a_present_width(640, 480) == 853);
    nv2a_present_window_aspect(0.0);

    /* The function removed: every layout as recorded. */
    nv2a_present_frame_aspect_fn(NULL);
    CHECK(same_as_golden());
    return 0;
}

/* Overscan "fill" (BO-235): the 640x448 band at the full width, into the
 * rectangle the whole frame gets. Off and crop are the golden table's. */
#define R4(r, a, b, c, d) ((r).x == (a) && (r).y == (b) && (r).w == (c) && (r).h == (d))
static int fill_overscan(void)
{
    struct nv2a_rect s, d, s0, d0;
    double x0, x1;
    int whole;

    /* 16:9 picture, 16:9 output: the whole window, no border, no side crop. */
    mode(NV2A_ASPECT_16_9, 2, 0);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 0, 0, 1920, 1080));
    /* The same output with no overscan: the whole frame in the same box, so
     * the destination is what Show all has and only the source differs. */
    mode(NV2A_ASPECT_16_9, 0, 0);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s0, &d0);
    CHECK(R4(s0, 0, 0, 640, 480) && R4(d0, 0, 0, 1920, 1080));
    /* 4:3 and the frame's own shape in a 16:9 window: pillarboxed as ever,
     * the height filled. */
    mode(NV2A_ASPECT_4_3, 2, 0);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 240, 0, 1440, 1080));
    mode(NV2A_ASPECT_FRAME, 2, 0);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 240, 0, 1440, 1080));
    /* A 4:3 window: 4:3 fills it, 16:9 is letterboxed by the window, not by
     * the title's border. */
    mode(NV2A_ASPECT_4_3, 2, 0);
    nv2a_present_layout(640, 480, 1440, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 0, 0, 1440, 1080));
    mode(NV2A_ASPECT_16_9, 2, 0);
    nv2a_present_layout(640, 480, 1440, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 0, 135, 1440, 810));
    /* The picture is about 7% taller than with no overscan (480/448): the
     * same box height over fewer source rows. */
    mode(NV2A_ASPECT_16_9, 2, 0);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.h == d0.h && s.h == 448 && s0.h == 480);
    CHECK(480.0 / 448.0 > 1.0714 && 480.0 / 448.0 < 1.0715);
    /* 21:9 output: bars at the sides, the full width of the picture. */
    mode(NV2A_ASPECT_16_9, 2, 0);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 320, 0, 1920, 1080));

    /* The visible columns are the whole width, as with no overscan: the
     * widescreen HUD lays out by them. */
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 0.0 && x1 == 640.0);
    nv2a_present_visible_x_at(640, 480, NV2A_ASPECT_4_3, &x0, &x1);
    CHECK(x0 == 0.0 && x1 == 640.0);
    nv2a_present_visible_x_ratio(640, 480, 64.0 / 27.0, &x0, &x1);
    CHECK(x0 == 0.0 && x1 == 640.0);
    CHECK(visible(NV2A_ASPECT_16_9, 2, 0, 1920, 1080, 0, 640, 0, 640));
    CHECK(visible(NV2A_ASPECT_4_3, 2, 0, 1440, 1080, 0, 640, 0, 640));

    /* With the 16:10 fit on the Deck's 1280x800: the fit crops the sides, as
     * it does with no overscan (32..608), and the band fills the height. */
    mode(NV2A_ASPECT_16_9, 2, 1);
    nv2a_present_layout(640, 480, 1280, 800, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 32, 16, 576, 448) && R4(d, 0, 0, 1280, 800));
    CHECK(visible(NV2A_ASPECT_16_9, 2, 1, 1280, 800, 32, 608, 0, 640));
    /* A 4:3 picture there is pillarboxed and nothing is cropped. */
    mode(NV2A_ASPECT_4_3, 2, 1);
    nv2a_present_layout(640, 480, 1280, 800, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 16, 640, 448) && R4(d, 106, 0, 1067, 800));

    /* The integer filter: the box is the whole frame's multiple (2x here), the
     * band is stretched into it, so it is filtered, not nearest ("whole" 0). */
    mode(NV2A_ASPECT_FRAME, 2, 0);
    whole = nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_INTEGER, &s, &d);
    CHECK(whole == 0 && R4(s, 0, 16, 640, 448) && R4(d, 320, 60, 1280, 960));
    /* A scaled frame (2x render scale): the same share of rows. */
    mode(NV2A_ASPECT_16_9, 2, 0);
    nv2a_present_layout(1280, 960, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 32, 1280, 896) && R4(d, 0, 0, 1920, 1080));
    /* No frame, no rectangle. */
    nv2a_present_layout(0, 0, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.w == 0 && d.h == 0);

    /* A value that is not a mode is no overscan. */
    nv2a_present_set_overscan(7);
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(R4(s, 0, 0, 640, 480));
    /* The layout cuts something for crop, fill and the fit; nothing for none. */
    mode(NV2A_ASPECT_16_9, 0, 0);
    CHECK(nv2a_present_layout_cuts() == 0);
    mode(NV2A_ASPECT_16_9, 1, 0);
    CHECK(nv2a_present_layout_cuts() == 1);
    mode(NV2A_ASPECT_16_9, 2, 0);
    CHECK(nv2a_present_layout_cuts() == 1);
    mode(NV2A_ASPECT_16_9, 0, 1);
    CHECK(nv2a_present_layout_cuts() == 1);
    mode(NV2A_ASPECT_16_9, 0, 0);
    return 0;
}

int main(void)
{
    double x0, x1;

    CHECK(sizeof k_golden / sizeof k_golden[0] == 1296);

    /* Before any present and without a window: the overscan crop alone. */
    mode(NV2A_ASPECT_16_9, 1, 1);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    /* A window, not yet presented to: its shape counts (the Deck's panel). */
    nv2a_present_note_output(1280, 800);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 51.0 && x1 == 589.0);

    CHECK(same_as_golden());

    /* 4:3, overscan crop: 21 columns a side, in any output. */
    CHECK(visible(NV2A_ASPECT_4_3, 1, 0, 1440, 1080, 21, 619, 21, 619));
    CHECK(visible(NV2A_ASPECT_FRAME, 1, 0, 1280, 800, 21, 619, 21, 619));
    /* 16:9, overscan crop, a 16:9 output: the same columns. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 0, 1920, 1080, 21, 619, 21, 619));
    /* No crop: the whole frame. */
    CHECK(visible(NV2A_ASPECT_16_9, 0, 0, 1920, 1080, 0, 640, 0, 640));
    /* 16:9 with the 16:10 fill on the Deck's 1280x800: the picture fills the
     * height and loses 30 more columns a side; a 4:3 picture there loses
     * none, which is the margin a HUD fix has to keep. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 1280, 800, 51, 589, 21, 619));
    CHECK(visible(NV2A_ASPECT_16_9, 0, 1, 1280, 800, 32, 608, 0, 640));
    /* The fill in a 16:9 output crops nothing. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 1920, 1080, 21, 619, 21, 619));
    /* 4:3 with the fill on the Deck: pillarboxed, nothing cropped. */
    CHECK(visible(NV2A_ASPECT_4_3, 1, 1, 1280, 800, 21, 619, 21, 619));
    /* A wide output (21:9, 2560x1080) with a 16:9 picture: bars, no crop. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 0, 2560, 1080, 21, 619, 21, 619));
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 2560, 1080, 21, 619, 21, 619));
    /* A present's size replaces the noted one; a later note does not. */
    nv2a_present_note_output(640, 480);
    mode(NV2A_ASPECT_16_9, 1, 1);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);       /* the 2560x1080 present still stands */

    CHECK(same_as_golden());
    if (fill_overscan())
        return 1;
    CHECK(same_as_golden());
    if (frame_aspect())
        return 1;
    printf("present_layout: ok (%u golden rows, visible columns)\n",
           (unsigned)(sizeof k_golden / sizeof k_golden[0]));
    return 0;
}

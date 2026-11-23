/* nv2a_filmup (BO-136): the setting, the film rule, the cost window. */
#include "nv2a_filmup.h"

#include <stdio.h>
#include <stdlib.h>

static int s_fail, s_n;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", \
                      __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    struct nv2a_filmup_stats st;
    uint64_t last = 0;
    uint8_t n = 0;
    int i;

    /* The default and the environment. */
    nv2a_filmup_reset_for_test();
    CHECK(nv2a_filmup_mode() == nv2a_filmup_default());
    CHECK(nv2a_filmup_default() == NV2A_FILMUP_BEST || nv2a_filmup_default() == NV2A_FILMUP_CHEAP);
    nv2a_filmup_set_mode(NV2A_FILMUP_CHEAP);
    CHECK(nv2a_filmup_mode() == NV2A_FILMUP_CHEAP);
    nv2a_filmup_set_mode(7);
    CHECK(nv2a_filmup_mode() == NV2A_FILMUP_OFF);

    /* The shape: linear, a film's size, not a render target. */
    CHECK(nv2a_filmup_shape(1, 0, 640, 480));
    CHECK(nv2a_filmup_shape(1, 0, 640, 352));
    CHECK(!nv2a_filmup_shape(0, 0, 640, 480));     /* swizzled */
    CHECK(!nv2a_filmup_shape(1, 1, 640, 480));     /* a render target */
    CHECK(!nv2a_filmup_shape(1, 0, 512, 512));
    CHECK(!nv2a_filmup_shape(1, 0, 640, 448));

    /* A film on its second change; a still picture never. */
    CHECK(!nv2a_filmup_changed(10, &last, &n));    /* first upload */
    CHECK(nv2a_filmup_changed(12, &last, &n));     /* changed 2 flips later */
    CHECK(nv2a_filmup_changed(14, &last, &n));
    CHECK(!nv2a_filmup_changed(14 + NV2A_FILMUP_GAP + 1, &last, &n));   /* stood still: over */
    CHECK(nv2a_filmup_changed(14 + NV2A_FILMUP_GAP + 3, &last, &n));

    /* The cost window: 5 s of 60 Hz flips with a film frame every other. */
    nv2a_filmup_stats(&st);
    CHECK(st.frames == 0 && st.gpu_ms == 0);
    for (i = 0; i < 400; i++) {
        if (!(i & 1))
            nv2a_filmup_frame_done(NV2A_FILMUP_BEST, i == 100 ? 2.0 : 1.0);
        nv2a_filmup_flip((uint64_t)i, 16.667);
    }
    nv2a_filmup_stats(&st);
    CHECK(st.frames == 200);
    CHECK(st.mode == NV2A_FILMUP_BEST);
    CHECK(st.gpu_ms > 0.99 && st.gpu_ms < 1.02);
    CHECK(st.gpu_max_ms > 1.99 && st.gpu_max_ms < 2.01);
    CHECK(st.frame_ms > 16.6 && st.frame_ms < 16.7);
    CHECK(st.recent);
    /* No film for 3 s: no longer recent, the numbers stay. */
    for (i = 0; i < 180; i++)
        nv2a_filmup_flip(400u + (uint64_t)i, 16.667);
    nv2a_filmup_stats(&st);
    CHECK(!st.recent);
    CHECK(st.gpu_ms > 0.99);

    printf("nv2a_filmup: %d checks, %d failed\n", s_n, s_fail);
    return s_fail != 0;
}

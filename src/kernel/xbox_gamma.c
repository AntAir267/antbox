/*
 * xbox_gamma.c - the display's gamma ramp (xbox_gamma.h).
 *
 * Written rarely (a fade writes one a frame at most) and read once a
 * present, so a generation count is the only synchronisation: a reader that
 * copies during a write shows one frame with a mix of two ramps.
 */
#include "xbox_gamma.h"

#include <stdio.h>
#include <string.h>

static uint8_t s_ramp[768];
static volatile uint32_t s_gen;
static volatile int s_identity = 1;

void xbox_GammaSetRamp(const uint8_t rgb[768])
{
    int i, ident = 1;

    if (s_gen && !memcmp(rgb, s_ramp, sizeof s_ramp))
        return;
    for (i = 0; i < 256 && ident; i++)
        ident = rgb[i] == i && rgb[256 + i] == i && rgb[512 + i] == i;
    memcpy(s_ramp, rgb, sizeof s_ramp);
    s_identity = ident;
    s_gen++;
    {
        static unsigned said;
        if (said++ < 4)
            fprintf(stderr, "  [DISPLAY] gamma ramp %s: red 0/64/128/192/255 -> %u %u %u %u %u,"
                    " green -> %u %u %u %u %u, blue -> %u %u %u %u %u\n",
                    ident ? "identity" : "set",
                    rgb[0], rgb[64], rgb[128], rgb[192], rgb[255],
                    rgb[256], rgb[256 + 64], rgb[256 + 128], rgb[256 + 192], rgb[256 + 255],
                    rgb[512], rgb[512 + 64], rgb[512 + 128], rgb[512 + 192], rgb[512 + 255]);
    }
}

int xbox_GammaRamp(uint8_t out[768], uint32_t *gen)
{
    uint32_t g = s_gen;

    if (gen)
        *gen = g;
    if (!g || s_identity)
        return 0;
    if (out)
        memcpy(out, s_ramp, sizeof s_ramp);
    return 1;
}

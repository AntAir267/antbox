/* present_composite: what a window would show of a frame dump, by the real
 * layout (nv2a_present_layout). A headless run dumps the title's 640x480
 * frame (RECOMP_FB_DUMP, a 24-bit BMP) and never presents it; this reads one,
 * places it as the presenter does for the given display aspect, overscan,
 * fit and output size, and writes the output as a BMP. It prints the source
 * and destination rectangles and the box of non-black pixels of the output,
 * to see a border (or not) without a window.
 *
 *   present_composite <in.bmp> <out.bmp> <4:3|16:9|frame> <off|crop|fill>
 *                     <letterbox|fill16x10> <out_w> <out_h> [nearest|linear]
 *
 * Standalone: cmake -S tests/present_layout -B build/present_layout; the
 * target is built with the test and is not a ctest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nv2a_backend_common.h"

static uint8_t *read_bmp(const char *path, uint32_t *w, uint32_t *h)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[54], *rgb;
    uint32_t off, y, rb;
    int32_t bh;
    if (!f || fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[28] != 24) {
        fprintf(stderr, "not a 24-bit BMP: %s\n", path);
        exit(2);
    }
    memcpy(w, hdr + 18, 4);
    memcpy(&bh, hdr + 22, 4);
    memcpy(&off, hdr + 10, 4);
    *h = (uint32_t)(bh < 0 ? -bh : bh);
    rb = (*w * 3 + 3) & ~3u;
    rgb = malloc((size_t)*w * *h * 4);
    fseek(f, (long)off, SEEK_SET);
    for (y = 0; y < *h; y++) {
        uint8_t *row = malloc(rb);
        uint32_t x, yy = bh < 0 ? y : *h - 1 - y;
        if (fread(row, 1, rb, f) != rb)
            exit(2);
        for (x = 0; x < *w; x++) {
            uint8_t *p = rgb + ((size_t)yy * *w + x) * 4;
            p[0] = row[x * 3 + 2];
            p[1] = row[x * 3 + 1];
            p[2] = row[x * 3];
            p[3] = 255;
        }
        free(row);
    }
    fclose(f);
    return rgb;
}

static void write_bmp(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54];
    uint32_t rb = (w * 3 + 3) & ~3u, fs = 54 + rb * h, y, x;
    uint8_t *row = calloc(1, rb);
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fs, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)(h - 1 - y) * w * 4;
        for (x = 0; x < w; x++) {
            row[x * 3] = s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4];
        }
        fwrite(row, 1, rb, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char **argv)
{
    uint32_t fw, fh, dw, dh, x, y, bx0 = ~0u, bx1 = 0, by0 = ~0u, by1 = 0;
    struct nv2a_host_opts h = *nv2a_host_opts();
    struct nv2a_rect s, d;
    uint8_t *in, *out;
    int linear;

    if (argc < 8) {
        fprintf(stderr, "usage: %s in.bmp out.bmp <4:3|16:9|frame> <off|crop|fill> <letterbox|fill16x10> w h [nearest|linear]\n", argv[0]);
        return 2;
    }
    in = read_bmp(argv[1], &fw, &fh);
    h.present_aspect = !strcmp(argv[3], "4:3") ? NV2A_ASPECT_4_3
                     : !strcmp(argv[3], "16:9") ? NV2A_ASPECT_16_9 : NV2A_ASPECT_FRAME;
    nv2a_host_opts_set(&h);
    nv2a_present_set_overscan(!strcmp(argv[4], "crop") ? NV2A_OVERSCAN_CROP
                              : !strcmp(argv[4], "fill") ? NV2A_OVERSCAN_FILL : NV2A_OVERSCAN_OFF);
    nv2a_present_set_fit(!strcmp(argv[5], "fill16x10"));
    dw = (uint32_t)atoi(argv[6]);
    dh = (uint32_t)atoi(argv[7]);
    linear = argc > 8 && !strcmp(argv[8], "linear");
    nv2a_present_layout(fw, fh, dw, dh, linear ? NV2A_PRESENT_LINEAR : NV2A_PRESENT_NEAREST, &s, &d);
    printf("frame %ux%u -> output %ux%u: src x%d y%d %dx%d, dst x%d y%d %dx%d\n",
           fw, fh, dw, dh, s.x, s.y, s.w, s.h, d.x, d.y, d.w, d.h);
    out = calloc((size_t)dw * dh, 4);
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            uint8_t *o = out + ((size_t)y * dw + x) * 4;
            double u, v;
            uint32_t ix, iy, ix1, iy1, c;
            double fx, fy;
            if ((int)x < d.x || (int)x >= d.x + d.w || (int)y < d.y || (int)y >= d.y + d.h) {
                o[3] = 255;
                continue;
            }
            u = s.x + ((double)(x - d.x) + 0.5) * s.w / d.w - 0.5;
            v = s.y + ((double)(y - d.y) + 0.5) * s.h / d.h - 0.5;
            if (u < s.x) u = s.x;
            if (v < s.y) v = s.y;
            if (u > s.x + s.w - 1) u = s.x + s.w - 1;
            if (v > s.y + s.h - 1) v = s.y + s.h - 1;
            ix = (uint32_t)u; iy = (uint32_t)v;
            ix1 = ix + 1 < (uint32_t)(s.x + s.w) ? ix + 1 : ix;
            iy1 = iy + 1 < (uint32_t)(s.y + s.h) ? iy + 1 : iy;
            fx = u - ix; fy = v - iy;
            for (c = 0; c < 3; c++) {
                double a = in[((size_t)iy * fw + ix) * 4 + c], b = in[((size_t)iy * fw + ix1) * 4 + c];
                double e = in[((size_t)iy1 * fw + ix) * 4 + c], g = in[((size_t)iy1 * fw + ix1) * 4 + c];
                o[c] = (uint8_t)((a * (1 - fx) + b * fx) * (1 - fy) + (e * (1 - fx) + g * fx) * fy + 0.5);
            }
            o[3] = 255;
        }
    /* The box of pixels that are not black (any channel over 8). */
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            const uint8_t *o = out + ((size_t)y * dw + x) * 4;
            if (o[0] > 8 || o[1] > 8 || o[2] > 8) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    printf("non-black box: x %u..%u y %u..%u (output %ux%u): black rows top %u bottom %u, black columns left %u right %u\n",
           bx0, bx1, by0, by1, dw, dh, by0, dh - 1 - by1, bx0, dw - 1 - bx1);
    write_bmp(argv[2], out, dw, dh);
    return 0;
}

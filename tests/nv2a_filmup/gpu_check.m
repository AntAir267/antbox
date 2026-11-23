/* film_gpu_check (BO-136): the Metal film filters on a synthetic 640x480
 * film frame (a smooth picture cut into 8x8 blocks with small steps between
 * them, as WMV2 leaves it, a hard diagonal edge, an alpha pattern):
 *   - the result is 2x, finite, its alpha the frame's (nearest);
 *   - it stays near the frame (mean difference from a bilinear 2x);
 *   - the steps at the block edges are smaller than the bilinear 2x's;
 *   - the GPU time of a frame, Cheap and Best (median of 60).
 * Manual retain/release, as the code it tests. */
#import <Metal/Metal.h>
#include "nv2a_texup_metal.h"
#include "nv2a_filmup.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 640
#define H 480

static int s_fail, s_n;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", \
                      __FILE__, __LINE__, #c); } } while (0)

static uint8_t clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5); }

static void make_frame(uint32_t *px)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            double base = 90 + 60 * sin(x * 0.013) * cos(y * 0.017);
            /* the block's own small offset: a step at every 8th texel */
            int bx = x >> 3, by = y >> 3;
            double step = (double)(((bx * 7 + by * 13) % 5) - 2) * 3.0;
            double v = base + step;
            uint8_t r, g, b, a;
            if (x + y > 600)               /* a hard edge */
                v += 80;
            r = clamp8(v * 1.05);
            g = clamp8(v);
            b = clamp8(v * 0.9);
            a = ((x >> 4) + (y >> 4)) & 1 ? 0xFF : 0x80;
            px[y * W + x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
}

static double lum(uint32_t p)
{
    return 0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255);
}

/* The bilinear 2x of the frame (the recombine's base), luma only. */
static double bil(const uint32_t *px, int qx, int qy)
{
    double fx = (qx + 0.5) * 0.5 - 0.5, fy = (qy + 0.5) * 0.5 - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double tx = fx - x0, ty = fy - y0, v = 0;
    int i, j;
    for (j = 0; j < 2; j++)
        for (i = 0; i < 2; i++) {
            int x = x0 + i, y = y0 + j;
            x = x < 0 ? 0 : x >= W ? W - 1 : x;
            y = y < 0 ? 0 : y >= H ? H - 1 : y;
            v += lum(px[y * W + x]) * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
        }
    return v;
}

/* Mean |luma step| across the block edges (output columns 16k-1 | 16k)
 * away from the hard edge. */
static double edge_steps(const uint32_t *out, int bilinear, const uint32_t *px)
{
    double s = 0;
    int n = 0, x, y;
    for (y = 8; y < 2 * H - 8; y += 3)
        for (x = 16; x < 2 * W - 16; x += 16) {
            double a, b;
            if (x / 2 + y / 2 > 560)
                continue;
            a = bilinear ? bil(px, x - 1, y) : lum(out[y * 2 * W + x - 1]);
            b = bilinear ? bil(px, x, y) : lum(out[y * 2 * W + x]);
            s += fabs(b - a);
            n++;
        }
    return s / n;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* film_gpu_check <frame.bgra> <w> <h> <out-prefix>: a real film frame (raw
 * BGRA, e.g. ffmpeg -pix_fmt bgra) through Cheap and Best, written as
 * <out-prefix>.cheap.bgra / .best.bgra (2w x 2h raw BGRA): same-frame
 * before/after pictures without the game. */
static int one_frame(const char *in, int w, int h, const char *prefix)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        size_t n = (size_t)w * h * 4;
        uint32_t *px = (uint32_t *)malloc(n), *out = (uint32_t *)malloc(n * 4);
        FILE *f = fopen(in, "rb");
        MTLTextureDescriptor *td;
        id<MTLTexture> src, rb;
        TumFilm *film;
        int mode, waited;
        if (!f || fread(px, 1, n, f) != n) {
            fprintf(stderr, "cannot read %s (%dx%d BGRA)\n", in, w, h);
            return 1;
        }
        fclose(f);
        tum_load(dev, TEXUP_SHADERS, 0);
        tum_film_load_async(dev);
        for (waited = 0; tum_film_state() == 0 && waited < 1000; waited++)
            usleep(10000);
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:w height:h mipmapped:NO];
        src = [dev newTextureWithDescriptor:td];
        [src replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px
               bytesPerRow:w * 4];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:w * 2 height:h * 2
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        rb = [dev newTextureWithDescriptor:td];
        film = tum_film_new(dev, w, h);
        for (mode = NV2A_FILMUP_CHEAP; mode <= NV2A_FILMUP_BEST; mode++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            int ran = 0;
            id<MTLTexture> o = tum_film_encode(film, cb, src, mode, &ran, NULL);
            id<MTLBlitCommandEncoder> b;
            char path[1024];
            if (!o)
                return 1;
            b = [cb blitCommandEncoder];
            [b copyFromTexture:o toTexture:rb];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            [rb getBytes:out bytesPerRow:w * 8 fromRegion:MTLRegionMake2D(0, 0, w * 2, h * 2)
             mipmapLevel:0];
            snprintf(path, sizeof path, "%s.%s.bgra", prefix, nv2a_filmup_mode_name(ran));
            f = fopen(path, "wb");
            if (!f || fwrite(out, 1, n * 4, f) != n * 4)
                return 1;
            fclose(f);
            printf("wrote %s\n", path);
        }
        tum_film_free(film);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5)
        return one_frame(argv[1], atoi(argv[2]), atoi(argv[3]), argv[4]);
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q;
        MTLTextureDescriptor *td;
        id<MTLTexture> src, rb;
        uint32_t *px = (uint32_t *)malloc((size_t)W * H * 4);
        uint32_t *out = (uint32_t *)malloc((size_t)W * H * 16);
        TumFilm *f;
        int mode, i, waited;
        if (!dev) {
            printf("film_gpu_check: no Metal device, skipped\n");
            return 0;
        }
        q = [dev newCommandQueue];
        if (!tum_load(dev, TEXUP_SHADERS, 0))
            fprintf(stderr, "FSRCNNX shaders not loaded: Best runs as Cheap\n");
        tum_film_load_async(dev);
        for (waited = 0; tum_film_state() == 0 && waited < 1000; waited++)
            usleep(10000);
        CHECK(tum_film_state() == 1);
        make_frame(px);
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        src = [dev newTextureWithDescriptor:td];
        [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px
               bytesPerRow:W * 4];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:W * 2 height:H * 2
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        rb = [dev newTextureWithDescriptor:td];
        f = tum_film_new(dev, W, H);
        printf("bilinear 2x: block-edge step %.3f\n", edge_steps(NULL, 1, px));
        for (mode = NV2A_FILMUP_CHEAP; mode <= NV2A_FILMUP_BEST; mode++) {
            double ms[60], mad = 0, st;
            int ran = 0, bad = 0, alpha_bad = 0;
            for (i = 0; i < 60; i++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                int smp = -1;
                id<MTLTexture> o = tum_film_encode(f, cb, src, mode, &ran, &smp);
                CHECK(o != nil);
                if (!o)
                    break;
                CHECK([o width] == W * 2 && [o height] == H * 2);
                if (i == 59) {
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    [b copyFromTexture:o toTexture:rb];
                    [b endEncoding];
                }
                [cb commit];
                [cb waitUntilCompleted];
                ms[i] = tum_film_sample_ms(smp);
                if (ms[i] < 0)
                    ms[i] = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
            }
            CHECK(ran == mode || (mode == NV2A_FILMUP_BEST && tum_state() != 1));
            [rb getBytes:out bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W * 2, H * 2)
             mipmapLevel:0];
            for (i = 0; i < W * H * 4; i++) {
                int x = i % (W * 2), y = i / (W * 2);
                double d = lum(out[i]) - bil(px, x, y);
                if (d != d)
                    bad++;
                mad += fabs(d);
                if ((out[i] >> 24) != (px[(y >> 1) * W + (x >> 1)] >> 24))
                    alpha_bad++;
            }
            mad /= (double)W * H * 4;
            st = edge_steps(out, 0, px);
            qsort(ms, 60, sizeof ms[0], cmp_d);
            printf("%s: GPU %.3f ms a frame (median of 60, min %.3f, max %.3f); "
                   "mean |luma - bilinear| %.2f; block-edge step %.3f\n",
                   nv2a_filmup_mode_name(ran), ms[30], ms[0], ms[59], mad, st);
            CHECK(bad == 0);
            CHECK(alpha_bad == 0);
            CHECK(mad < 6.0);
            CHECK(st < edge_steps(NULL, 1, px));
        }
        tum_film_free(f);
        [rb release];
        [src release];
        [q release];
        free(px);
        free(out);
    }
    printf("film_gpu_check: %d checks, %d failed\n", s_n, s_fail);
    return s_fail != 0;
}

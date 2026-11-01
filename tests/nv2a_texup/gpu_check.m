/* texup_gpu_check: graphics.texture_upscale's Metal upscalers (BO-245) on a
 * texture, through the same code the backend runs (nv2a_texup_metal.m).
 *
 *   texup_gpu_check                          synthetic textures, checks only
 *   texup_gpu_check <in.bgra> <w> <h> <out>  a real texture (B G R A bytes):
 *       also writes <out>_<algo>_x<s>.bgra (the result's level 0),
 *       <out>_<algo>_luma.f32 and _model.f32 (the luma in, the model's last
 *       image out, RGBA float) for check_fsrcnnx.py / check_nnedi3.py, and
 *       one line of GPU times per pass class. TEXUP_WRAP=0..3 sets the
 *       edge rule (default 3, wrap on both axes; 0 clamps).
 *
 * Checks, every algorithm and scale:
 *   - alpha: every output texel's alpha is exactly the title's texel under
 *     it (nearest), so alpha kill and masks are the title's;
 *   - the title's levels follow the model's, unchanged;
 *   - chunked = whole: the job cut into the smallest chunks gives the same
 *     bytes as one dispatch a pass;
 *   - nothing black or NaN where the title's texture is not.
 * Exit 0 on pass. Needs a Metal device (skips with 0 without one). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nv2a_texup.h"
#include "nv2a_texup_metal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks, s_fail;
static unsigned s_wrap = 3;        /* TEXUP_WRAP: the edge rule, bit 0 u, bit 1 v (wrap) */
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static id<MTLTexture> make_src(id<MTLDevice> dev, const uint32_t *px, uint32_t w, uint32_t h,
                               uint32_t levels)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w height:h mipmapped:levels > 1];
    id<MTLTexture> t;
    uint32_t l;
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead;
    t = [dev newTextureWithDescriptor:td];
    for (l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, *lv;
        size_t i;
        lv = (uint32_t *)malloc((size_t)lw * lh * 4);
        for (i = 0; i < (size_t)lw * lh; i++)
            lv[i] = l ? 0xFF000000u | (l * 0x112233u) : px[i];
        [t replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:l withBytes:lv
             bytesPerRow:lw * 4];
        free(lv);
    }
    return t;
}

static void dump_scratch(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t,
                         const char *path)
{
    id<MTLBuffer> b;
    id<MTLCommandBuffer> cb;
    id<MTLBlitCommandEncoder> e;
    size_t px, bpp;
    FILE *f;
    if (!t)
        return;
    bpp = t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 8;
    px = t.width * t.height;
    b = [dev newBufferWithLength:px * bpp options:MTLResourceStorageModeShared];
    cb = [q commandBuffer];
    e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0
destinationBytesPerRow:t.width * bpp destinationBytesPerImage:px * bpp];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    f = fopen(path, "wb");
    if (f) {
        if (bpp == 16) {
            fwrite([b contents], 1, px * 16, f);
        } else {
            const __fp16 *s = (const __fp16 *)[b contents];
            size_t i;
            for (i = 0; i < px * 4; i++) {
                float v = (float)s[i];
                fwrite(&v, 4, 1, f);
            }
        }
        fclose(f);
    }
    [b release];
}

/* Runs one job; whole: one dispatch a pass, timed per class into ms[]. */
static uint32_t *run(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> src, uint32_t w,
                     uint32_t h, uint32_t levels, int algo, unsigned scale, int whole,
                     double *ms, double *tex, const char *dump, uint32_t **lv1)
{
    TumJob *j = tum_job_new(dev, src, w, h, levels, algo, scale, s_wrap);
    uint32_t *out;
    size_t bytes;
    CHECK(j != NULL);
    if (!j)
        return NULL;
    while (!tum_job_encoded(j)) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        int cls = -1;
        double n = 0;
        if (whole)
            cls = tum_job_encode_pass(j, cb, &n);
        else
            tum_job_encode(j, cb, 0.0001, 1.0, 1, -1);
        [cb commit];
        [cb waitUntilCompleted];
        CHECK([cb status] == MTLCommandBufferStatusCompleted);
        if (whole && cls >= 0 && ms) {
            ms[cls] += ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
            tex[cls] += n;
        }
    }
    out = tum_job_readback(j, &bytes);
    if (lv1) {
        /* The first of the title's levels in the result, to compare. */
        id<MTLTexture> t = tum_job_texture(j);
        uint32_t l = scale == 4 ? 2 : 1;
        *lv1 = (uint32_t *)malloc((size_t)w * h * 4);
        [t getBytes:*lv1 bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l];
    }
    if (dump) {
        char p[1200];
        snprintf(p, sizeof p, "%s_%s_luma.f32", dump, nv2a_texup_algo_name(algo));
        dump_scratch(dev, q, tum_job_scratch(j, 0), p);
        snprintf(p, sizeof p, "%s_%s_model.f32", dump, nv2a_texup_algo_name(algo));
        dump_scratch(dev, q, tum_job_scratch(j, algo == NV2A_TEXUP_FSRCNNX ? 5 : 1), p);
    }
    tum_job_free(j);
    return out;
}

static void check_one(id<MTLDevice> dev, id<MTLCommandQueue> q, const uint32_t *px, uint32_t w,
                      uint32_t h, const char *out)
{
    static const char *cls_name[NV2A_TEXUP_PASSES] = { "luma", "fsr_feature", "fsr_model",
                                                       "fsr_subconv", "nnedi3", "recombine" };
    int algo, c;
    unsigned scale;
    uint32_t levels = 3;
    id<MTLTexture> src = make_src(dev, px, w, h, levels);
    for (algo = NV2A_TEXUP_FSRCNNX; algo <= NV2A_TEXUP_NNEDI3; algo++) {
        for (scale = 2; scale <= 4; scale += 2) {
            double ms[NV2A_TEXUP_PASSES] = { 0 }, tex[NV2A_TEXUP_PASSES] = { 0 }, total = 0;
            uint32_t *lv1 = NULL, *a, *b;
            uint32_t W = w * scale, H = h * scale, x, y, bad_a = 0, black = 0, diff = 0;
            char dump[1200];
            snprintf(dump, sizeof dump, "%s", out ? out : "");
            if (w > 256 && scale == 4)
                continue;
            a = run(dev, q, src, w, h, levels, algo, scale, 1, ms, tex,
                    out && scale == 2 ? dump : NULL, &lv1);
            b = run(dev, q, src, w, h, levels, algo, scale, 0, NULL, NULL, NULL, NULL);
            if (!a || !b)
                continue;
            if (tum_samples_ok()) {
                /* The stage-boundary sample against the buffer's span, one
                 * buffer with the whole job. */
                TumJob *jj = tum_job_new(dev, src, w, h, levels, algo, scale, s_wrap);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                double sm, span;
                tum_job_encode(jj, cb, 1e9, 1.0, 1, 0);
                [cb commit];
                [cb waitUntilCompleted];
                sm = tum_sample_ms(0);
                span = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
                CHECK(sm > 0 && sm <= span * 1.05 + 0.05);
                printf("  one buffer: counters %.3f ms, span %.3f ms\n", sm, span);
                tum_job_free(jj);
            }
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    uint32_t o = a[(size_t)y * W + x], s = px[(size_t)(y / scale) * w + x / scale];
                    if ((o >> 24) != (s >> 24))
                        bad_a++;
                    /* Black where the title's texel is clearly not dark
                     * (the additive colour rule may take a near-black
                     * texel to 0, as the bilinear can). */
                    if (!(o & 0xFFFFFF) && ((s >> 16 & 0xFF) * 54 + (s >> 8 & 0xFF) * 183
                                            + (s & 0xFF) * 19) / 256 > 32)
                        black++;
                }
            for (x = 0; x < W * H; x++)
                diff += a[x] != b[x];
            CHECK(bad_a == 0);
            CHECK(diff == 0);
            CHECK(black < W * H / 1000 + 1);
            for (x = 0; lv1 && x < w * h; x++)
                if (lv1[x] != px[x]) {
                    CHECK(lv1[x] == px[x]);
                    break;
                }
            for (c = 0; c < NV2A_TEXUP_PASSES; c++)
                total += ms[c];
            printf("%s x%u %ux%u: %.3f ms GPU (one buffer a pass); alpha off %u, black %u,"
                   " chunked differs %u;", nv2a_texup_algo_name(algo), scale, w, h, total,
                   bad_a, black, diff);
            for (c = 0; c < NV2A_TEXUP_PASSES; c++)
                if (tex[c] > 0)
                    printf(" %s %.4f ms/64K", cls_name[c], ms[c] / tex[c] * 65536.0);
            printf("\n");
            if (out) {
                char p[1200];
                FILE *f;
                snprintf(p, sizeof p, "%s_%s_x%u.bgra", out, nv2a_texup_algo_name(algo), scale);
                f = fopen(p, "wb");
                if (f) {
                    fwrite(a, 4, (size_t)W * H, f);
                    fclose(f);
                }
            }
            free(a);
            free(b);
            free(lv1);
        }
    }
    [src release];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q;
        const char *e = getenv("TEXUP_FP32"), *wr = getenv("TEXUP_WRAP");
        if (!dev) {
            printf("texup_gpu_check: no Metal device, skipped\n");
            return 0;
        }
        if (wr && *wr)
            s_wrap = (unsigned)atoi(wr) & 3;
        q = [dev newCommandQueue];
        if (!tum_load(dev, TEXUP_SHADERS, e && *e == '1')) {
            printf("FAIL: shaders\n");
            return 1;
        }
        if (argc >= 5) {
            uint32_t w = (uint32_t)atoi(argv[2]), h = (uint32_t)atoi(argv[3]);
            uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
            FILE *f = fopen(argv[1], "rb");
            if (!f || fread(px, 4, (size_t)w * h, f) != (size_t)w * h) {
                printf("cannot read %s\n", argv[1]);
                return 2;
            }
            fclose(f);
            check_one(dev, q, px, w, h, argv[4]);
            free(px);
        } else {
            /* Synthetic: a cut-out (alpha 0/255 checker of 3 texels), a shine
             * mask (alpha a smooth ramp), colour bars with a sharp edge. */
            uint32_t w = 64, h = 48, x, y;
            uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    uint32_t a = x < 32 ? (((x / 3) + (y / 3)) & 1 ? 255u : 0u)
                                        : (uint32_t)(x * 255 / 63) ^ (y & 7);
                    uint32_t r = x < 20 ? 220 : 30, g = (y * 5) & 0xFF, b = (x * y) & 0xFF;
                    px[(size_t)y * w + x] = a << 24 | r << 16 | g << 8 | b;
                }
            check_one(dev, q, px, w, h, NULL);
            free(px);
        }
    }
    printf("texup_gpu_check: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/*
 * nv2a_texup_metal.m - the texture upscalers on Metal; see nv2a_texup_metal.h
 * and nv2a_texup.h. Manual retain/release, as nv2a_pb_metal.m.
 *
 * A job is a list of steps (luma, the model's passes, the recombine), each
 * a compute pass over a domain of rows; a chunk is a range of one step's
 * rows, one dispatch. A step starts only when the one before is complete
 * (they are encoded in order in serial compute encoders, so the GPU runs
 * them in order). Intermediates are RGBA16F (RGBA32F with fp32), private.
 */
#import "nv2a_texup_metal.h"
#include "nv2a_texup.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGP "[TEXUP] "

enum {
    PIPE_LUMA, PIPE_RECOMBINE, PIPE_NNEDI3,
    PIPE_FSR0,                       /* + 0..12 */
    PIPE_N = PIPE_FSR0 + 13
};
static const char *const k_pipe_file[PIPE_N] = {
    "luma", "recombine", "nnedi3",
    "fsrcnnx_00", "fsrcnnx_01", "fsrcnnx_02", "fsrcnnx_03", "fsrcnnx_04", "fsrcnnx_05",
    "fsrcnnx_06", "fsrcnnx_07", "fsrcnnx_08", "fsrcnnx_09", "fsrcnnx_10", "fsrcnnx_11",
    "fsrcnnx_12",
};

static id<MTLComputePipelineState> s_pipe[PIPE_N];
static id<MTLSamplerState>         s_point;
static id<MTLCounterSampleBuffer>  s_csb;       /* stage-boundary timestamps, 2 a pair */
static id<MTLDevice>               s_csb_dev;
static atomic_int                  s_state;     /* 0 / 1 / -1 */
static int                         s_fp32;

int tum_state(void)
{
    return atomic_load(&s_state);
}

int tum_load(id<MTLDevice> dev, const char *dir, int fp32)
{
    int i, ok = 1;
    @autoreleasepool {
        MTLCompileOptions *opt = [[MTLCompileOptions alloc] init];
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        if (@available(macOS 15.0, *))
            opt.mathMode = MTLMathModeSafe;
        for (i = 0; i < PIPE_N && ok; i++) {
            NSError *err = nil;
            NSString *path = [NSString stringWithFormat:@"%s/%s.metal", dir, k_pipe_file[i]];
            NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding
                                                         error:&err];
            id<MTLLibrary> lib;
            id<MTLFunction> fn;
            if (!src) {
                fprintf(stderr, LOGP "cannot read %s\n", [path UTF8String]);
                ok = 0;
                break;
            }
            lib = [dev newLibraryWithSource:src options:opt error:&err];
            if (!lib) {
                fprintf(stderr, LOGP "%s: %s\n", k_pipe_file[i],
                        [[err localizedDescription] UTF8String]);
                ok = 0;
                break;
            }
            fn = [lib newFunctionWithName:@"main0"];
            s_pipe[i] = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
            [fn release];
            [lib release];
            if (!s_pipe[i]) {
                fprintf(stderr, LOGP "%s: no pipeline: %s\n", k_pipe_file[i],
                        err ? [[err localizedDescription] UTF8String] : "?");
                ok = 0;
            }
        }
        /* Timing of the chunks' own work: stage-boundary timestamps. */
        if (@available(macOS 11.0, *)) {
            if ([dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
                for (id<MTLCounterSet> cs in [dev counterSets]) {
                    if ([[cs name] isEqualToString:MTLCommonCounterSetTimestamp]) {
                        MTLCounterSampleBufferDescriptor *cd = [[MTLCounterSampleBufferDescriptor alloc] init];
                        NSError *e2 = nil;
                        cd.counterSet = cs;
                        cd.storageMode = MTLStorageModeShared;
                        cd.sampleCount = TUM_SAMPLES * 2;
                        s_csb = [dev newCounterSampleBufferWithDescriptor:cd error:&e2];
                        [cd release];
                        s_csb_dev = [dev retain];
                        break;
                    }
                }
            }
        }
        fprintf(stderr, LOGP "chunk timing: %s\n", s_csb ? "stage-boundary counters"
                                                    : "command buffer span");
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        s_point = [dev newSamplerStateWithDescriptor:sd];
        [sd release];
        [opt release];
    }
    s_fp32 = fp32;
    atomic_store(&s_state, ok && s_point ? 1 : -1);
    fprintf(stderr, LOGP "shaders from %s: %s\n", dir, ok ? "ready" : "failed");
    return ok;
}

typedef struct {
    id<MTLDevice> dev;
    char dir[1024];
    int fp32;
} LoadArg;

static void *load_main(void *p)
{
    LoadArg *a = (LoadArg *)p;
    tum_load(a->dev, a->dir, a->fp32);
    [a->dev release];
    free(a);
    return NULL;
}

void tum_load_async(id<MTLDevice> dev, const char *dir, int fp32)
{
    static int started;
    pthread_t th;
    LoadArg *a;
    if (started)
        return;
    started = 1;
    a = (LoadArg *)calloc(1, sizeof *a);
    if (!a) {
        atomic_store(&s_state, -1);
        return;
    }
    a->dev = [dev retain];
    snprintf(a->dir, sizeof a->dir, "%s", dir);
    a->fp32 = fp32;
    if (pthread_create(&th, NULL, load_main, a) != 0) {
        [dev release];
        free(a);
        atomic_store(&s_state, -1);
        return;
    }
    pthread_detach(th);
}

/* ── jobs ─────────────────────────────────────────────────────────────── */

#define MAX_STEPS 40
typedef struct {
    int      pipe, cls;
    id<MTLTexture> in[4];            /* not retained: the job's own */
    int      nin;
    id<MTLTexture> out;
    uint32_t dom_w, dom_h;           /* the domain in threads */
    uint32_t size_w, size_h;         /* the push constants' size (the input's) */
    uint32_t group, align;           /* threadgroup side, row granularity */
    int      mode;
    uint32_t row;                    /* rows done */
    uint32_t col;                    /* in the band of rows at `row`: columns done */
} Step;

struct TumJob {
    id<MTLTexture> src, dst;
    id<MTLTexture> view[3];          /* dst's levels 0, 1 (4x) as their own textures */
    id<MTLTexture> scratch[16];
    int      nscratch;
    uint32_t w, h, levels, dlevels, scale, wrap;
    int      algo;
    Step     step[MAX_STEPS];
    int      nstep, cur;
    int      blitted;
};

static id<MTLTexture> scratch(TumJob *j, id<MTLDevice> dev, uint32_t w, uint32_t h)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s_fp32 ? MTLPixelFormatRGBA32Float
                                                                        : MTLPixelFormatRGBA16Float
                                                           width:w height:h mipmapped:NO];
    id<MTLTexture> t;
    if (j->nscratch >= 16)
        return nil;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [dev newTextureWithDescriptor:td];
    if (t)
        j->scratch[j->nscratch++] = t;
    return t;
}

static Step *add(TumJob *j, int pipe, int cls, uint32_t dw, uint32_t dh, uint32_t sw,
                 uint32_t sh, uint32_t group)
{
    Step *s;
    if (j->nstep >= MAX_STEPS)
        return NULL;
    s = &j->step[j->nstep++];
    memset(s, 0, sizeof *s);
    s->pipe = pipe;
    s->cls = cls;
    s->dom_w = dw;
    s->dom_h = dh;
    s->size_w = sw;
    s->size_h = sh;
    s->group = group;
    s->align = group;
    return s;
}

/* One 2x round: in (BGRA8, w x h) to out (BGRA8, 2w x 2h). 0: no memory. */
static int round2x(TumJob *j, id<MTLDevice> dev, id<MTLTexture> in, id<MTLTexture> out,
                   uint32_t w, uint32_t h)
{
    id<MTLTexture> y = scratch(j, dev, w, h);
    Step *s;
    if (!y)
        return 0;
    s = add(j, PIPE_LUMA, NV2A_TEXUP_P_LUMA, w, h, w, h, 16);
    s->in[0] = in;
    s->nin = 1;
    s->out = y;
    if (j->algo == NV2A_TEXUP_FSRCNNX) {
        /* FEATURE1 FEATURE2 MODEL21 MODEL22 MODEL1 MODEL2; RES1 and RES2
         * reuse MODEL21/22, SUBCONV1 MODEL1 (each is dead by then). */
        id<MTLTexture> f1 = scratch(j, dev, w, h), f2 = scratch(j, dev, w, h);
        id<MTLTexture> a1 = scratch(j, dev, w, h), a2 = scratch(j, dev, w, h);
        id<MTLTexture> b1 = scratch(j, dev, w, h), b2 = scratch(j, dev, w, h);
        static const int cls[13] = {
            NV2A_TEXUP_P_FSR_FEATURE, NV2A_TEXUP_P_FSR_FEATURE,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_MODEL,
            NV2A_TEXUP_P_FSR_MODEL, NV2A_TEXUP_P_FSR_SUBCONV,
        };
        id<MTLTexture> io[13][4] = {
            /* inputs..., output last in [3] */
            { y, nil, nil, f1 }, { y, nil, nil, f2 },
            { f1, f2, nil, a1 }, { f1, f2, nil, a2 },
            { a1, a2, nil, b1 }, { a1, a2, nil, b2 },
            { b1, b2, nil, a1 }, { b1, b2, nil, a2 },
            { a1, a2, nil, b1 }, { a1, a2, nil, b2 },
            { b1, b2, f1, a1 }, { b1, b2, f2, a2 },
            { a1, a2, nil, b1 },
        };
        int i, k;
        if (!f1 || !f2 || !a1 || !a2 || !b1 || !b2)
            return 0;
        for (i = 0; i < 13; i++) {
            s = add(j, PIPE_FSR0 + i, cls[i], w, h, w, h, 16);
            for (k = 0; k < 3 && io[i][k]; k++)
                s->in[k] = io[i][k];
            s->nin = k;
            s->out = io[i][3];
        }
        s = add(j, PIPE_RECOMBINE, NV2A_TEXUP_P_RECOMBINE, w * 2, h * 2, w, h, 16);
        s->in[0] = in;
        s->in[1] = b1;               /* SUBCONV1 */
        s->nin = 2;
        s->out = out;
        s->mode = 0;
    } else {
        id<MTLTexture> y2 = scratch(j, dev, w * 2, h * 2);
        if (!y2)
            return 0;
        s = add(j, PIPE_NNEDI3, NV2A_TEXUP_P_NNEDI3, w, h, w, h, 16);
        s->in[0] = y;
        s->nin = 1;
        s->out = y2;
        s = add(j, PIPE_RECOMBINE, NV2A_TEXUP_P_RECOMBINE, w * 2, h * 2, w, h, 16);
        s->in[0] = in;
        s->in[1] = y2;
        s->nin = 2;
        s->out = out;
        s->mode = 1;
    }
    return 1;
}

static uint32_t log2u(unsigned s)
{
    return s == 4 ? 2u : 1u;
}

static id<MTLTexture> dst_new(id<MTLDevice> dev, uint32_t w, uint32_t h, uint32_t levels,
                              unsigned scale, int write)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w * scale height:h * scale
                                                       mipmapped:YES];
    td.mipmapLevelCount = levels + log2u(scale);
    td.usage = MTLTextureUsageShaderRead | (write ? MTLTextureUsageShaderWrite : 0);
    td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}

TumJob *tum_job_new(id<MTLDevice> dev, id<MTLTexture> src, uint32_t w, uint32_t h,
                    uint32_t levels, int algo, unsigned scale, unsigned wrap)
{
    TumJob *j;
    if (tum_state() != 1 || (algo != NV2A_TEXUP_FSRCNNX && algo != NV2A_TEXUP_NNEDI3)
            || (scale != 2 && scale != 4) || !src)
        return NULL;
    j = (TumJob *)calloc(1, sizeof *j);
    if (!j)
        return NULL;
    j->src = [src retain];
    j->w = w;
    j->h = h;
    j->levels = levels;
    j->scale = scale;
    j->wrap = wrap;
    j->algo = algo;
    j->dlevels = levels + log2u(scale);
    j->dst = dst_new(dev, w, h, levels, scale, 1);
    if (!j->dst)
        goto fail;
    j->view[0] = [j->dst newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                           textureType:MTLTextureType2D
                                                levels:NSMakeRange(0, 1)
                                                slices:NSMakeRange(0, 1)];
    if (!j->view[0])
        goto fail;
    if (scale == 2) {
        if (!round2x(j, dev, src, j->view[0], w, h))
            goto fail;
    } else {
        j->view[1] = [j->dst newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                               textureType:MTLTextureType2D
                                                    levels:NSMakeRange(1, 1)
                                                    slices:NSMakeRange(0, 1)];
        if (!j->view[1] || !round2x(j, dev, src, j->view[1], w, h)
                || !round2x(j, dev, j->view[1], j->view[0], w * 2, h * 2))
            goto fail;
    }
    return j;
fail:
    tum_job_free(j);
    return NULL;
}

void tum_job_free(TumJob *j)
{
    int i;
    if (!j)
        return;
    for (i = 0; i < j->nscratch; i++)
        [j->scratch[i] release];
    for (i = 0; i < 3; i++)
        [j->view[i] release];
    [j->dst release];
    [j->src release];
    free(j);
}

static double row_cost(const Step *s)
{
    return nv2a_texup_pass_cost(s->cls, (double)s->dom_w);
}


double tum_job_cost(const TumJob *j)
{
    double c = 0;
    int i;
    for (i = 0; i < j->nstep; i++)
        c += row_cost(&j->step[i]) * j->step[i].dom_h;
    return c;
}

static void blit_levels(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst,
                        uint32_t w, uint32_t h, uint32_t levels, unsigned scale)
{
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    uint32_t l;
    for (l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        [b copyFromTexture:src sourceSlice:0 sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(lw, lh, 1) toTexture:dst destinationSlice:0
          destinationLevel:l + log2u(scale) destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [b endEncoding];
}

struct up_params {
    int32_t origin[2], size[2], wrap[2], mode, pad0;
};

static void dispatch(id<MTLComputeCommandEncoder> e, const TumJob *j, const Step *s,
                     uint32_t col0, uint32_t row0, uint32_t cols, uint32_t rows)
{
    struct up_params pc;
    int k;
    memset(&pc, 0, sizeof pc);
    pc.origin[0] = (int32_t)col0;
    pc.origin[1] = (int32_t)row0;
    pc.size[0] = (int32_t)s->size_w;
    pc.size[1] = (int32_t)s->size_h;
    pc.wrap[0] = (j->wrap & 1) != 0;
    pc.wrap[1] = (j->wrap & 2) != 0;
    pc.mode = s->mode;
    [e setComputePipelineState:s_pipe[s->pipe]];
    for (k = 0; k < s->nin; k++)
        [e setTexture:s->in[k] atIndex:(NSUInteger)k];
    [e setTexture:s->out atIndex:4];
    [e setSamplerState:s_point atIndex:0];
    [e setBytes:&pc length:sizeof pc atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake((cols + s->group - 1) / s->group,
                                        (rows + s->group - 1) / s->group, 1)
      threadsPerThreadgroup:MTLSizeMake(s->group, s->group, 1)];
}

/* A chunk: whole rows while they fit (a multiple of the group height), else
 * a tile of one group's rows and as many groups' columns as fit, so a slow
 * device or a large texture never needs a chunk over the budget. */
int tum_samples_ok(void)
{
    return s_csb != nil;
}

double tum_sample_ms(int i)
{
    static double ns_per_tick;
    NSData *d;
    const MTLCounterResultTimestamp *ts;
    if (!s_csb || i < 0 || i >= TUM_SAMPLES)
        return -1;
    if (!ns_per_tick && [s_csb_dev hasUnifiedMemory]) {
        /* Apple silicon: the counter results are nanoseconds already (on
         * the M5 they come out a little under the buffer's GPU span with
         * 1:1; the sampleTimestamps pair is on the mach tick clock and
         * would scale them by its timebase, 41.67). */
        ns_per_tick = 1.0;
    }
    if (!ns_per_tick) {
        /* Elsewhere, GPU ticks to ns: two correlated samples apart. */
        MTLTimestamp c0, g0, c1, g1;
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        [s_csb_dev sampleTimestamps:&c0 gpuTimestamp:&g0];
        usleep(2000);
        [s_csb_dev sampleTimestamps:&c1 gpuTimestamp:&g1];
        if (g1 > g0 && c1 > c0)
            ns_per_tick = (double)(c1 - c0) * tb.numer / tb.denom / (double)(g1 - g0);
        if (ns_per_tick <= 0)
            ns_per_tick = 1.0;
    }
    d = [s_csb resolveCounterRange:NSMakeRange((NSUInteger)i * 2, 2)];
    if (!d || [d length] < 2 * sizeof *ts)
        return -1;
    ts = (const MTLCounterResultTimestamp *)[d bytes];
    if (ts[0].timestamp == MTLCounterErrorValue || ts[1].timestamp == MTLCounterErrorValue
            || ts[1].timestamp <= ts[0].timestamp)
        return -1;
    return (double)(ts[1].timestamp - ts[0].timestamp) * ns_per_tick / 1e6;
}

static id<MTLComputeCommandEncoder> tum_encoder(id<MTLCommandBuffer> cb, int sample)
{
    if (s_csb && sample >= 0 && sample < TUM_SAMPLES) {
        if (@available(macOS 11.0, *)) {
            MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
            pd.sampleBufferAttachments[0].sampleBuffer = s_csb;
            pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)sample * 2;
            pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)sample * 2 + 1;
            return [cb computeCommandEncoderWithDescriptor:pd];
        }
    }
    return [cb computeCommandEncoder];
}

double tum_job_encode(TumJob *j, id<MTLCommandBuffer> cb, double budget, double factor,
                      int min_one, int sample)
{
    id<MTLComputeCommandEncoder> e = nil;
    double used = 0;
    if (!j || j->cur >= j->nstep)
        return 0;
    if (!j->blitted) {
        blit_levels(cb, j->src, j->dst, j->w, j->h, j->levels, j->scale);
        j->blitted = 1;
    }
    while (j->cur < j->nstep) {
        Step *s = &j->step[j->cur];
        double per_texel = nv2a_texup_pass_cost(s->cls, 1.0) * factor;
        uint32_t left = s->dom_h - s->row, rows = 0, cols, band;
        double room = budget - used;
        if (per_texel <= 0)
            per_texel = 1e-9;
        if (!s->col) {
            rows = room > 0 ? (uint32_t)(room / (per_texel * s->dom_w)) : 0;
            rows = rows / s->align * s->align;
            if (rows > left)
                rows = left;
        }
        if (rows) {
            if (!e)
                e = tum_encoder(cb, sample);
            dispatch(e, j, s, 0, s->row, s->dom_w, rows);
            used += per_texel * s->dom_w * rows;
            s->row += rows;
        } else {
            band = s->align < left ? s->align : left;
            cols = room > 0 ? (uint32_t)(room / (per_texel * band)) : 0;
            cols = cols / s->group * s->group;
            if (!cols) {
                if (!(min_one && used == 0))
                    break;
                cols = s->group;
            }
            if (cols > s->dom_w - s->col)
                cols = s->dom_w - s->col;
            if (!e)
                e = tum_encoder(cb, sample);
            dispatch(e, j, s, s->col, s->row, cols, band);
            used += per_texel * cols * band;
            s->col += cols;
            if (s->col >= s->dom_w) {
                s->col = 0;
                s->row += band;
            }
        }
        if (s->row >= s->dom_h)
            j->cur++;
    }
    if (e)
        [e endEncoding];
    return used;
}

int tum_job_encode_pass(TumJob *j, id<MTLCommandBuffer> cb, double *texels)
{
    id<MTLComputeCommandEncoder> e;
    Step *s;
    if (!j || j->cur >= j->nstep)
        return -1;
    if (!j->blitted) {
        blit_levels(cb, j->src, j->dst, j->w, j->h, j->levels, j->scale);
        j->blitted = 1;
    }
    s = &j->step[j->cur++];
    e = [cb computeCommandEncoder];
    dispatch(e, j, s, 0, 0, s->dom_w, s->dom_h);
    [e endEncoding];
    s->row = s->dom_h;
    if (texels)
        *texels = (double)s->dom_w * s->dom_h;
    return s->cls;
}

int tum_job_encoded(const TumJob *j)
{
    return j && j->cur >= j->nstep;
}

id<MTLTexture> tum_job_texture(const TumJob *j)
{
    return j ? j->dst : nil;
}

uint32_t tum_job_levels(const TumJob *j)
{
    return j ? j->dlevels : 0;
}

uint32_t *tum_job_readback(const TumJob *j, size_t *bytes)
{
    uint32_t W = j->w * j->scale, H = j->h * j->scale;
    size_t n = (size_t)W * H + (j->scale == 4 ? (size_t)W / 2 * (H / 2) : 0);
    uint32_t *px = (uint32_t *)malloc(n * 4);
    if (!px)
        return NULL;
    [j->dst getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    if (j->scale == 4)
        [j->dst getBytes:px + (size_t)W * H bytesPerRow:W * 2 fromRegion:MTLRegionMake2D(0, 0, W / 2, H / 2)
             mipmapLevel:1];
    *bytes = n * 4;
    return px;
}

int tum_job_check(const TumJob *j, const uint32_t *px)
{
    uint32_t *t;
    int ok;
    if (!j || !px)
        return 0;
    t = (uint32_t *)malloc((size_t)j->w * j->h * 4u);
    if (!t)
        return 1;   /* no memory for the check: as before */
    [j->dst getBytes:t bytesPerRow:j->w * 4 fromRegion:MTLRegionMake2D(0, 0, j->w, j->h)
         mipmapLevel:(j->scale == 4 ? 2 : 1)];
    ok = nv2a_texup_result_ok((const uint8_t *)px, (const uint8_t *)t, j->w, j->h, j->scale);
    free(t);
    return ok;
}

id<MTLTexture> tum_from_file(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> src,
                             uint32_t w, uint32_t h, uint32_t levels, unsigned scale,
                             const uint32_t *texels)
{
    id<MTLTexture> t = dst_new(dev, w, h, levels, scale, 0);
    uint32_t W = w * scale, H = h * scale;
    if (!t)
        return nil;
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:texels
         bytesPerRow:W * 4];
    if (scale == 4)
        [t replaceRegion:MTLRegionMake2D(0, 0, W / 2, H / 2) mipmapLevel:1
               withBytes:texels + (size_t)W * H bytesPerRow:W * 2];
    blit_levels(cb, src, t, w, h, levels, scale);
    return t;
}

id<MTLTexture> tum_job_scratch(const TumJob *j, int i)
{
    return j && i >= 0 && i < j->nscratch ? j->scratch[i] : nil;
}

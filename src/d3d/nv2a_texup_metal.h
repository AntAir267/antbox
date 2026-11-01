/**
 * nv2a_texup_metal.h - the texture upscalers on Metal (nv2a_texup.h, BO-245).
 *
 * The compute pipelines (one a pass, from the .metal files of the shader
 * folder, compiled off the render thread) and one job: the title's texture
 * in, a new texture out whose level 0 is the model's (4x: levels 0 and 1)
 * and whose further levels are the title's, copied on the GPU. A job is
 * encoded in chunks of rows into command buffers the caller commits; the
 * caller decides how many predicted milliseconds a buffer may hold.
 *
 * Used by nv2a_pb_metal.m and by tests/nv2a_texup's GPU check. Objective-C.
 */
#ifndef NV2A_TEXUP_METAL_H
#define NV2A_TEXUP_METAL_H

#import <Metal/Metal.h>
#include <stdint.h>

/* Compiles every pass from dir (synchronous: call it on a thread of its
 * own). 1: ready. fp32: 32-bit intermediates (RECOMP_DEBUG=texup_fp32). */
int  tum_load(id<MTLDevice> dev, const char *dir, int fp32);
/* 0 not yet, 1 ready, -1 failed. */
int  tum_state(void);
/* tum_load on a background thread, once. */
void tum_load_async(id<MTLDevice> dev, const char *dir, int fp32);

typedef struct TumJob TumJob;

/* src: the title's texture (BGRA8, w x h, `levels` levels), retained by the
 * job. scale 2 or 4, wrap bit 0 u, bit 1 v. NULL: no memory. */
TumJob *tum_job_new(id<MTLDevice> dev, id<MTLTexture> src, uint32_t w, uint32_t h,
                    uint32_t levels, int algo, unsigned scale, unsigned wrap);
/* Encodes the next chunks into cb while their predicted cost (times
 * factor) fits budget_ms; with min_one at least one chunk. Returns the
 * predicted ms encoded (with the factor). sample: a pair index of the
 * counter samples (tum_sample_ms) to time the chunks' encoder with, or -1. */
double tum_job_encode(TumJob *j, id<MTLCommandBuffer> cb, double budget_ms, double factor,
                      int min_one, int sample);

/* The GPU's own time of the encoder sampled as pair i (stage-boundary
 * timestamps: the compute work alone, not the span of its command buffer,
 * which on Apple GPUs runs beside the next frame's). After the buffer
 * completed; -1 when the device has no such counters (the caller falls
 * back to GPUStartTime..GPUEndTime). TUM_SAMPLES pairs. */
#define TUM_SAMPLES 32
int    tum_samples_ok(void);
double tum_sample_ms(int i);
/* Every chunk encoded. */
int  tum_job_encoded(const TumJob *j);
/* The texture made (valid once the buffer with the last chunk completed). */
id<MTLTexture> tum_job_texture(const TumJob *j);
uint32_t tum_job_levels(const TumJob *j);
/* The predicted ms of the whole job (factor 1). */
double tum_job_cost(const TumJob *j);
/* The result's level 0 (and for 4x the 2x level after it) as BGRA8 texels,
 * for the disk cache: malloc'd, *bytes set. After completion only. */
uint32_t *tum_job_readback(const TumJob *j, size_t *bytes);
/* The result check (nv2a_texup_result_ok) of a completed job: its level 0
 * (px, tum_job_readback's) against the title's level 0 in its lower level. */
int tum_job_check(const TumJob *j, const uint32_t *px);
void tum_job_free(TumJob *j);

/* A texture of the result's shape from file texels (tum_job_readback's
 * layout) and the title's texture: the file's levels by the CPU, the
 * title's levels by a blit encoded into cb. NULL: no memory. */
id<MTLTexture> tum_from_file(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> src,
                             uint32_t w, uint32_t h, uint32_t levels, unsigned scale,
                             const uint32_t *texels);

/* For the cost table and tests: the whole next pass into cb. Returns its
 * class (nv2a_texup_pass) and *texels its domain, or -1 when none is left. */
int tum_job_encode_pass(TumJob *j, id<MTLCommandBuffer> cb, double *texels);

/* Tests: the job's intermediate image i (0: the luma; FSRCNNX 7: SUBCONV1
 * after the last hook; NNEDI3 1: the 2x luma), nil past the end. */
id<MTLTexture> tum_job_scratch(const TumJob *j, int i);

#endif

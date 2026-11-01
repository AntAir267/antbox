/**
 * GLSL -> SPIR-V for the Vulkan backend, at runtime with glslang's C API
 * (glslang/Include/glslang_c_interface.h), as xemu's Vulkan renderer compiles
 * its generated shaders (hw/xbox/nv2a/pgraph/vk/glsl.c), plus an on-disk
 * cache of the results keyed by the source text, so a warm start runs no
 * glslang at all. The driver's own compile is cached by the backend's
 * VkPipelineCache, saved next to it.
 *
 * Cache directory: render.shader_cache (nv2a_pcache_dir), as the Metal
 * pipeline cache: by default macOS ~/Library/Caches/<executable>, elsewhere
 * $XDG_CACHE_HOME/<executable> or ~/.cache/<executable>.
 */
#ifndef XBOXRECOMP_NV2A_VK_GLSL_H
#define XBOXRECOMP_NV2A_VK_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nvk_stage { NVK_STAGE_VERTEX = 0, NVK_STAGE_FRAGMENT = 1, NVK_STAGE_COMPUTE = 2 };

/* SPIR-V for `src`: *words and *n point into the cache (valid for the life of
 * the process). Returns 1, or 0 with the compiler's log in err. Compiles
 * only on a cache miss; *compiled says whether it did. Thread-safe. */
int nvk_glsl_spirv(enum nvk_stage stage, const char *src, const uint32_t **words,
                   size_t *n, int *compiled, char *err, size_t errn);

/* Reads the cache file again: what another process (a pre-build) appended
 * since it was loaded. Thread-safe. */
void nvk_glsl_reload(void);

/* The cache directory (created), or 0 when the disk cache is off. */
int nvk_cache_dir(char *out, size_t n);

/* Since startup: SPIR-V served from the disk cache, and compiled. */
void nvk_glsl_stats(uint64_t *hits, uint64_t *compiles);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_VK_GLSL_H */

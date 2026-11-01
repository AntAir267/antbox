/*
 * nvr_backend.h - the native Vulkan renderer's backend (nvr_backend.c),
 * selected with RECOMP_PB_BACKEND=native.
 */
#ifndef NVR_BACKEND_H
#define NVR_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nv2a_pb_backend;

/* The backend (its device made on first use); NULL without a Vulkan device. */
const struct nv2a_pb_backend *nvr_backend(void);
/* RECOMP_PB_BACKEND=native: the executor draws through it. */
void nvr_register_from_env(void);
/* The target at a surface offset as BGRA8 rows at render.scale, *hw x *hh,
 * read back now (tools/nvr_replay); NULL when there is none. */
const uint8_t *nvr_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh);
/* Its counters, on stderr (tools/nvr_replay at the end). */
void nvr_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif

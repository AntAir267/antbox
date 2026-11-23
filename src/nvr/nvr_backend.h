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

/* The install or first-launch step: builds the shaders a shipped list names, from the
 * player's own executable, on their GPU, into the same disk cache a run reads. No window
 * and no guest needed: call it from a game's --precache after its settings are read, and
 * end the process with what it returns. progress may be NULL; killing the process is a safe
 * skip, since what's left then builds at the next start. Returns 0 done, 1 unreadable, 2 a list
 * for another executable (nothing built), 3 no device, 4 the native renderer isn't selected
 * (nothing built: choosing it later builds them then, through the variables below). Without this,
 * RECOMP_NVR_SHADER_LIST and RECOMP_NVR_SHADER_XBE do the same in the background of a run. */
int nvr_precache_main(const char *list, const char *xbe,
                      void (*progress)(unsigned done, unsigned total, void *ctx), void *ctx);
/* Its counters, on stderr (tools/nvr_replay at the end). */
void nvr_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif

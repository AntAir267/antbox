#ifndef REF_GATHER_H
#define REF_GATHER_H
#include <stdint.h>
#include "nv2a_backend_common.h"

/* The gather and decode before BO-282 (ref_gather.c). */
void ref_vtx_gather(const struct nv2a_pb_draw *d, const int *slot, int nf4,
                    uint32_t lo, uint32_t count, const float (*inl)[4],
                    const uint8_t *mem, float *out);
int ref_vtx_decode_fn(uint32_t type, uint32_t size, const uint8_t *p, float out[4]);
#endif

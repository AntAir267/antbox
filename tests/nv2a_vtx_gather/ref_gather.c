/*
 * The vertex gather and decode as they were before BO-282 (toolkit
 * e01f04c4, src/kernel/nv2a_backend_common.c and .h), word for word but for
 * the names: the reference tests/nv2a_vtx_gather compares the fast paths
 * with, bit for bit.
 */
#include "nv2a_backend_common.h"
#include "ref_gather.h"

/* One attribute of `size` components of `type` at p as float4, missing
 * components (0,0,0,1). Returns 0 for a type with no decoder (out is then
 * (0,0,0,1)). Decoded the way xemu hands them to GL. */
static inline int ref_vtx_decode(uint32_t type, uint32_t size,
                                  const uint8_t *p, float out[4])
{
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case NV2A_VTX_D3DCOLOR:
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV2A_VTX_FLOAT:
        for (i = 0; i < size && i < 4; i++)
            memcpy(&out[i], p + 4 * i, 4);
        return 1;
    case NV2A_VTX_UBYTE:
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case NV2A_VTX_S1:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            float f;
            memcpy(&s, p + 2 * i, 2);
            f = (float)s / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case NV2A_VTX_S32K:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (float)s;
        }
        return 1;
    case NV2A_VTX_CMP: {
        uint32_t w;
        int32_t x, y, z;
        memcpy(&w, p, 4);
        x = (int32_t)(w << 21) >> 21;
        y = (int32_t)(w << 10) >> 21;
        z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

void ref_vtx_gather(const struct nv2a_pb_draw *d, const int *slot, int nf4,
                     uint32_t lo, uint32_t count, const float (*inl)[4],
                     const uint8_t *mem, float *out)
{
    struct {
        const uint8_t *p;       /* the element of the vertex being written */
        size_t         stride;
        uint32_t       type, size;
        const float   *dflt;    /* SET_VERTEX_DATA value */
    } s[NV_VERTEX_ATTRS];
    const int inl_data = d->inline_data;
    uint32_t i;
    int k;

    if (nf4 > NV_VERTEX_ATTRS)
        nf4 = NV_VERTEX_ATTRS;
    for (k = 0; k < nf4; k++) {
        const VertexAttr *a = &d->attr[slot[k]];
        s[k].dflt = inl[slot[k]];
        s[k].stride = a->stride;
        s[k].type = a->type;
        s[k].size = a->size;
        s[k].p = NULL;
        if (!a->size || !a->stride)
            continue;
        if (inl_data)
            s[k].p = (const uint8_t *)d->inline_buf + a->offset + (size_t)lo * a->stride;
        else if (a->offset)
            s[k].p = mem + a->offset + (size_t)lo * a->stride;
    }
    for (i = 0; i < count; i++) {
        for (k = 0; k < nf4; k++, out += 4) {
            const uint8_t *p = s[k].p;
            if (p) {
                s[k].p = p + s[k].stride;
                if (inl_data && (size_t)(p - (const uint8_t *)d->inline_buf) + 4 > d->inline_bytes)
                    p = NULL;
            }
            if (!p) {
                memcpy(out, s[k].dflt, 16);
                continue;
            }
            if (s[k].type == NV2A_VTX_FLOAT) {
                /* Loaded into registers and stored once each: a partial
                 * store read back whole stalls the store forwarding. */
                float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
                switch (s[k].size) {
                default: memcpy(&w, p + 12, 4); /* fall through */
                case 3:  memcpy(&z, p + 8, 4);  /* fall through */
                case 2:  memcpy(&y, p + 4, 4);  /* fall through */
                case 1:  memcpy(&x, p, 4);      break;
                }
                out[0] = x;
                out[1] = y;
                out[2] = z;
                out[3] = w;
            } else if (!ref_vtx_decode(s[k].type, s[k].size, p, out)) {
                memcpy(out, s[k].dflt, 16);
            }
        }
    }
}


int ref_vtx_decode_fn(uint32_t type, uint32_t size, const uint8_t *p, float out[4])
{
    return ref_vtx_decode(type, size, p, out);
}

/**
 * nv2a_tset_memo.h - the Vulkan backend's last texture descriptor set, kept
 * so a draw with the same four (view, sampler) pairs as the draw before it
 * binds that set again (nv2a_pb_vulkan.c texture_set).
 *
 * The memo is keyed by handle values. A handle destroyed while the memo
 * names it can come back from the driver as another object with the same
 * value, and the memo would then hand out a set that holds the old object.
 * So every place that destroys a view or sampler mid-frame forgets the memo:
 * here, smp_evict (the sampler table is full and a slot is reused, its
 * sampler destroyed); in the backend, the out-of-memory garbage path.
 *
 * No Vulkan types (handles as 64-bit words), so tests/nv2a_tset_memo runs it
 * on every host without a GPU.
 */
#ifndef NV2A_TSET_MEMO_H
#define NV2A_TSET_MEMO_H

#include <stdint.h>
#include <string.h>

#define TMEMO_WORDS 8                       /* four views, then four samplers */

typedef struct {
    uint64_t set;                           /* 0: none */
    uint64_t key[TMEMO_WORDS];
} TsetMemo;

/* The set kept for key, or 0. */
static inline uint64_t tmemo_find(const TsetMemo *m, const uint64_t key[TMEMO_WORDS])
{
    return m->set && !memcmp(m->key, key, sizeof m->key) ? m->set : 0;
}

static inline void tmemo_keep(TsetMemo *m, const uint64_t key[TMEMO_WORDS], uint64_t set)
{
    memcpy(m->key, key, sizeof m->key);
    m->set = set;
}

static inline void tmemo_forget(TsetMemo *m)
{
    m->set = 0;
}

/* The sampler table is full: the slot the new sampler takes, in turn. The
 * caller destroys that slot's sampler, so the memo is forgotten. */
static inline int smp_evict(unsigned *next, unsigned slots, TsetMemo *m)
{
    tmemo_forget(m);
    return (int)((*next)++ % slots);
}

#endif /* NV2A_TSET_MEMO_H */

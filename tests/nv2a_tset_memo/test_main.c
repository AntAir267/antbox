/*
 * nv2a_tset_memo test: src/d3d/nv2a_tset_memo.h as nv2a_pb_vulkan.c uses it.
 *
 * sampler() and texture_set() are modelled step for step: the sampler
 * table (SMP_MAX slots; a full table creates the new sampler first, then
 * takes a slot with smp_evict and destroys that slot's sampler) and the
 * last-set memo in front of the set writes. The fake device hands out the
 * lowest free handle value, so a destroyed sampler's value comes back for
 * the next one created, with other state, as a driver may do. Every draw
 * checks that the set it gets holds the sampler states its handles mean
 * now. Without the forget in smp_evict, the memo returns the set written
 * for the destroyed sampler and the test fails.
 */
#include <stdio.h>
#include <string.h>
#include "nv2a_tset_memo.h"

#define SMP_MAX 128
#define HANDLES 1024

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, \
    __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* The fake device's samplers: handle value -> state (-1: free). */
static int g_state[HANDLES];
static uint64_t dev_create_sampler(int state)
{
    uint64_t h;
    for (h = 1; h < HANDLES; h++)
        if (g_state[h] < 0) { g_state[h] = state; return h; }
    return 0;
}
static void dev_destroy_sampler(uint64_t h) { g_state[h] = -1; }

/* Sets: what each was written with, as sampler states (the views fixed). */
static int g_set_states[4096][4];
static uint64_t g_sets;

/* nv2a_pb_vulkan.c sampler(): the table, by the state as key. */
static struct { int key; uint64_t s; } s_smp[SMP_MAX];
static int s_smp_n;
static TsetMemo s_tmemo;
static uint64_t sampler(int state)
{
    static unsigned next;
    uint64_t st;
    int i;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == state)
            return s_smp[i].s;
    st = dev_create_sampler(state);
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        i = smp_evict(&next, SMP_MAX, &s_tmemo);
        dev_destroy_sampler(s_smp[i].s);
    }
    s_smp[i].key = state;
    s_smp[i].s = st;
    return st;
}

/* nv2a_pb_vulkan.c texture_set(): the memo, else a new set written. */
static uint64_t texture_set(const uint64_t v[4], const uint64_t s[4])
{
    uint64_t key[TMEMO_WORDS], set;
    int k;
    memcpy(key, v, 4 * sizeof *key);
    memcpy(key + 4, s, 4 * sizeof *key);
    if ((set = tmemo_find(&s_tmemo, key)) != 0)
        return set;
    set = ++g_sets;
    for (k = 0; k < 4; k++)
        g_set_states[set][k] = g_state[s[k]];
    tmemo_keep(&s_tmemo, key, set);
    return set;
}

/* A draw with these sampler states: the set must hold them. */
static void draw(int a, int b, int c, int d, const char *what)
{
    static const uint64_t views[4] = { 0x1000, 0x2000, 0x3000, 0x4000 };
    int want[4] = { a, b, c, d }, k;
    uint64_t s[4], set;
    for (k = 0; k < 4; k++)
        s[k] = sampler(want[k]);
    set = texture_set(views, s);
    for (k = 0; k < 4; k++)
        CHECK(g_set_states[set][k] == want[k], "%s: stage %d samples with sampler state %d,"
              " the draw asked for %d (set %llu)", what, k, g_set_states[set][k], want[k],
              (unsigned long long)set);
}

int main(void)
{
    int i;
    for (i = 0; i < HANDLES; i++)
        g_state[i] = -1;

    /* The table full: states 0..127, handles 1..128. */
    for (i = 0; i < SMP_MAX; i++)
        (void)sampler(i);
    CHECK(s_smp_n == SMP_MAX && s_smp[0].s == 1, "table: %d samplers, slot 0 handle %llu",
          s_smp_n, (unsigned long long)s_smp[0].s);

    /* A draw whose stage 0 uses slot 0's sampler (state 0, handle 1): the
     * memo keeps its set. */
    draw(0, 1, 2, 3, "first draw");

    /* State 200: created as handle 129, evicts slot 0, handle 1 destroyed.
     * State 201: created as handle 1 again (the value came back), evicts
     * slot 1, handle 2 destroyed. */
    (void)sampler(200);
    CHECK(sampler(201) == 1, "the fake device did not reuse handle 1");

    /* Same view and sampler handles as the first draw (1 now means state
     * 201), so the memo's key matches. The set must hold state 201, not 0.
     * State 1's sampler was handle 2, destroyed: it is made again. */
    draw(201, 1, 2, 3, "after the eviction");

    /* The memo still works when nothing was evicted. */
    {
        uint64_t before = g_sets;
        draw(201, 1, 2, 3, "repeat");
        CHECK(g_sets == before, "a repeat draw wrote a set (%llu -> %llu)",
              (unsigned long long)before, (unsigned long long)g_sets);
    }

    /* A forget always makes the next draw write. */
    {
        uint64_t before = g_sets;
        tmemo_forget(&s_tmemo);
        draw(201, 1, 2, 3, "after forget");
        CHECK(g_sets == before + 1, "forget: %llu -> %llu sets", (unsigned long long)before,
              (unsigned long long)g_sets);
    }

    if (fails) {
        fprintf(stderr, "nv2a_tset_memo: %d failures\n", fails);
        return 1;
    }
    printf("nv2a_tset_memo: ok\n");
    return 0;
}

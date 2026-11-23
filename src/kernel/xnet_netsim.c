/*
 * xnet_netsim.c - see xnet_netsim.h.
 */
#include "xnet_netsim.h"

#include <stdlib.h>
#include <string.h>

#define QUEUE 1024

static int s_on;
static xn_cfg s_cfg;
static xn_pkt *s_q;             /* QUEUE entries, n == 0 free */
static int s_held;
static uint32_t s_rng;
static uint64_t s_passed, s_lost, s_overflow;

/* xorshift32: repeatable from the seed, on every platform */
static uint32_t rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

/* "12.5" as hundredths; -1 when not a number from 0 to 100 */
static int centi(const char *s, const char **end)
{
    int v = 0, d = 0, k = 0;
    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9' && v < 1000)
        v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (k < 2) {
                d = d * 10 + (*s - '0');
                k++;
            }
            s++;
        }
    }
    while (k++ < 2)
        d *= 10;
    *end = s;
    v = v * 100 + d;
    return v > 10000 ? -1 : v;
}

static int number(const char *s, const char **end)
{
    int v = 0;
    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9') {
        if (v > 100000)
            return -1;
        v = v * 10 + (*s++ - '0');
    }
    *end = s;
    return v;
}

int xn_parse(const char *spec, xn_cfg *out)
{
    xn_cfg c;
    const char *p = spec;
    memset(&c, 0, sizeof c);
    c.seed = 1;
    if (!spec || !*spec)
        return 0;
    while (*p) {
        const char *e = p;
        int v;
        if (!strncmp(p, "delay=", 6)) {
            if ((v = number(p + 6, &e)) < 0)
                return 0;
            c.delay_ms = v;
        } else if (!strncmp(p, "jitter=", 7)) {
            if ((v = number(p + 7, &e)) < 0)
                return 0;
            c.jitter_ms = v;
        } else if (!strncmp(p, "loss=", 5)) {
            if ((v = centi(p + 5, &e)) < 0)
                return 0;
            c.loss_centi = v;
        } else if (!strncmp(p, "seed=", 5)) {
            if ((v = number(p + 5, &e)) < 0)
                return 0;
            c.seed = (uint32_t)v;
        } else {
            return 0;
        }
        p = e;
        if (*p == ',')
            p++;
        else if (*p)
            return 0;
    }
    if (!c.delay_ms && !c.jitter_ms && !c.loss_centi)
        return 0;
    if (!c.seed)
        c.seed = 1;
    if (out)
        *out = c;
    return 1;
}

int xn_start(const xn_cfg *cfg)
{
    xn_stop();
    s_q = (xn_pkt *)calloc(QUEUE, sizeof *s_q);
    if (!s_q)
        return 0;
    s_cfg = *cfg;
    s_rng = cfg->seed;
    s_on = 1;
    return 1;
}

void xn_stop(void)
{
    free(s_q);
    s_q = NULL;
    s_on = 0;
    s_held = 0;
    s_passed = s_lost = s_overflow = 0;
}

int xn_on(void) { return s_on; }
int xn_held(void) { return s_held; }

void xn_counts(uint64_t *passed, uint64_t *lost, uint64_t *overflow)
{
    if (passed) *passed = s_passed;
    if (lost) *lost = s_lost;
    if (overflow) *overflow = s_overflow;
}

int xn_push(int dir, uint32_t sock, const void *buf, int n, uint32_t ip, uint16_t port, uint64_t now)
{
    int i;
    int64_t d;
    xn_pkt *q;
    if (!s_on || n <= 0 || n > XN_MAX_DGRAM)
        return XN_DROPPED;
    if (s_cfg.loss_centi && (int)(rnd() % 10000u) < s_cfg.loss_centi) {
        s_lost++;
        return XN_DROPPED;
    }
    for (i = 0; i < QUEUE && s_q[i].n; i++)
        ;
    if (i == QUEUE) {
        s_overflow++;
        return XN_DROPPED;
    }
    d = s_cfg.delay_ms / 2;
    if (s_cfg.jitter_ms)
        d += (int64_t)(rnd() % (uint32_t)(s_cfg.jitter_ms + 1)) - s_cfg.jitter_ms / 2;
    if (d < 0)
        d = 0;
    q = &s_q[i];
    q->due = now + (uint64_t)d;
    q->sock = sock;
    q->ip = ip;
    q->port = port;
    q->dir = (uint8_t)dir;
    q->n = n;
    memcpy(q->data, buf, (size_t)n);
    s_held++;
    return XN_HELD;
}

int xn_pop(uint64_t now, xn_pkt *out)
{
    int i, best = -1;
    if (!s_on || !s_held)
        return 0;
    for (i = 0; i < QUEUE; i++)
        if (s_q[i].n && s_q[i].due <= now && (best < 0 || s_q[i].due < s_q[best].due))
            best = i;
    if (best < 0)
        return 0;
    *out = s_q[best];
    s_q[best].n = 0;
    s_held--;
    s_passed++;
    return 1;
}

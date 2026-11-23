/*
 * xnet_netsim.h - a bad network between two copies, for tests (BO-357).
 *
 * TAKEDAWN_NET_SIM=delay=150,jitter=50,loss=2,seed=7 on one copy:
 *   delay   added round trip in ms: half goes on each datagram out, half on
 *           each datagram in, so a copy alone adds this much to the round
 *           trip with any peer; set on both copies, the two add up
 *   jitter  each direction's delay moves by up to jitter/2 either way, so the
 *           round trip moves by up to +/-jitter; datagrams may swap order
 *   loss    percent of datagrams lost, each direction on its own
 *   seed    the random numbers, default 1
 *
 * Unset, nothing here runs: xn_on() is 0, nothing is allocated, and the
 * online layer takes its usual path after one test of a static. A build with
 * XNET_NO_NET_SIM (a game's release build) never reads the variable.
 *
 * Pure C with no sockets: the online layer (xnet_hle.c) pushes datagrams and
 * sends what comes due. Only the peers' UDP goes through it. The lobby's TCP
 * connection does not.
 */
#ifndef XNET_NETSIM_H
#define XNET_NETSIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int delay_ms, jitter_ms;
    int loss_centi;              /* loss, in hundredths of a percent */
    uint32_t seed;
} xn_cfg;

#define XN_MAX_DGRAM 1600
enum { XN_OUT = 0, XN_IN = 1 };
enum { XN_DROPPED = 0, XN_HELD = 1 };

typedef struct {
    uint64_t due;
    uint32_t sock;               /* the title's socket handle */
    uint32_t ip;                 /* network order: where to, or where from */
    uint16_t port;               /* host order */
    uint8_t dir;
    int n;
    uint8_t data[XN_MAX_DGRAM];
} xn_pkt;

/* parse a TAKEDAWN_NET_SIM value: 1 when it is good and asks for something */
int  xn_parse(const char *spec, xn_cfg *out);
/* start with this configuration; 0 when no memory, and it stays off */
int  xn_start(const xn_cfg *cfg);
void xn_stop(void);
int  xn_on(void);
/* a datagram enters: XN_DROPPED (lost, too long, or the queue is full) or
 * XN_HELD. Not thread safe: the caller holds its lock. */
int  xn_push(int dir, uint32_t sock, const void *buf, int n, uint32_t ip, uint16_t port, uint64_t now_ms);
/* the next datagram due by now_ms into out: 1, or 0 when none */
int  xn_pop(uint64_t now_ms, xn_pkt *out);
int  xn_held(void);
/* totals since start, for the log */
void xn_counts(uint64_t *passed, uint64_t *lost, uint64_t *overflow);

#ifdef __cplusplus
}
#endif
#endif

/*
 * xnet_route.c - where a peer's datagrams go; see xnet_route.h.
 */
#include "xnet_route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PEERS   XR_MAX_PEERS
#define MAX_LEARNED XR_MAX_LEARNED
#define HELLO_FAST_MS 500      /* until the peer's own hello arrives */
#define HELLO_KEEP_MS 15000    /* then now and then, to keep the router's path open */

typedef struct {
    int used;
    uint32_t id;               /* title address (the server's synthetic one when real is set) */
    uint32_t pub;              /* public address from the lobby, 0 = unknown */
    uint32_t real;             /* its real local address (TKRADDRn, TKRLA), 0 = id is the real one */
    uint16_t off;
    int has_off;
    int member;                /* in the current game (the lobby's member records) */
    int heard;                 /* a hello of its own came since it joined this game */
    int lan_denied;            /* BO-349: sends to its LAN address fail here: its public one */
    uint64_t touch;            /* when it was last used (s_clock), for eviction */
    /* the relay fallback (BO-381): when our hellos to it began in this
     * game (ms, 0 = not yet), whether its hello came through the relay, and
     * the endpoints the relay's answers gave per title port */
    uint64_t member_at;
    int relay_heard;
    struct { uint16_t tport; xr_ep ep; } disc[4];
    struct { uint16_t tport; xr_ep ep; const char *why; } last[4];   /* per port, for "changed" */
} peer_t;

typedef struct {
    int used;
    xr_ep src;
    uint32_t id;
    uint16_t tport;
    uint64_t seen;             /* ms of the last hello from src */
    int relay;                 /* src is the relay: the datagrams are wrapped (BO-381) */
} learned_t;

#define LEARNED_STALE_MS 30000 /* another path replaces a learned one after this */

/* Where a player's datagrams arrive from, per socket of ours (BO-345): every
 * source a signed hello of that player came from, keyed by the source and our
 * title port it reached. A router's NAT loopback can give two flows to two of
 * our sockets one source (int037 game 56: 192.168.0.1:1145 for two players),
 * so the source alone is not a player. Only for xr_source: routes stay in
 * s_learn. */
typedef struct {
    int used;
    xr_ep src;
    uint16_t ltport;           /* our title port it reached */
    uint16_t tport;            /* the player's title port it came from */
    uint32_t id;
    uint64_t seen;
} rxsrc_t;

/* hellos sent, per peer and port: each of the title's sockets opens its own path */
typedef struct {
    int used;
    uint32_t id;
    uint16_t tport;
    uint64_t last;
    int sent;
    uint64_t direct_in;        /* when the peer's last direct hello reached this port (BO-381) */
} hello_t;

static peer_t s_peer[MAX_PEERS];
static hello_t s_hs[MAX_LEARNED];
static learned_t s_learn[MAX_LEARNED];
static rxsrc_t s_rx[MAX_LEARNED];
static uint64_t s_rx_newest;   /* the newest hello's time: older sources than LEARNED_STALE_MS before it are not trusted */
static uint32_t s_self_id, s_self_pub;
/* this machine's own LAN address when the title's differs (TAKEDAWN_NET_ID
 * tests): what our router's NAT loopback is next to (BO-345) */
static uint32_t s_self_lan;
/* the game's secret from the lobby (member-only records), keys the hellos */
static uint8_t s_secret[16];
static int s_has_secret, s_secret_seq;
static uint16_t s_self_off;
static int s_force;
/* the server's synthetic title address for this copy (TKADDR), 0 = none */
static uint32_t s_self_synth;
/* a member record has been seen: from then on only the members' datagrams
 * reach the title */
static int s_members_seen;
static uint64_t s_clock;       /* counts peer uses, for eviction */
/* the relay (BO-381): its endpoint (port 0: none), the port a member record
 * named, the game's IDENT, the switch, and the one-call source trust */
static xr_ep s_relay;
static uint16_t s_relay_port_seen;
static int s_relay_game;
static int s_relay_off;
static uint32_t s_relay_trust;

/* the reactions channel's send bucket, per-sender receive buckets and queue
 * (below); the resets clear them too */
static void react_clear(void);
static void ping_clear(int keep_can);
static void ping_can(uint32_t id, int can);

void xr_reset(void)
{
    memset(s_peer, 0, sizeof s_peer);
    memset(s_learn, 0, sizeof s_learn);
    memset(s_rx, 0, sizeof s_rx);
    s_rx_newest = 0;
    memset(s_hs, 0, sizeof s_hs);
    s_self_id = s_self_pub = s_self_synth = s_self_lan = 0;
    memset(s_secret, 0, sizeof s_secret);
    s_has_secret = 0;
    s_self_off = 0;
    s_force = 0;
    s_members_seen = 0;
    s_clock = 0;
    memset(&s_relay, 0, sizeof s_relay);
    s_relay_port_seen = 0;
    s_relay_game = 0;
    s_relay_trust = 0;
    react_clear();
    ping_clear(0);
}

/* Learned paths and the hello schedule stay: a player of the next game is
 * often one of the last (the same path), and resetting them changed the
 * title's second game (the guest never linked); a path of someone who left
 * is no longer used (xr_source wants a member) and is the first to go. */
void xr_game_reset(void)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++)
        s_peer[i].member = 0;
    if (s_has_secret) {
        memset(s_secret, 0, sizeof s_secret);
        s_has_secret = 0;
        s_secret_seq++;
    }
    react_clear();
    ping_clear(1);
}

/* A new lobby session (BO-343): the server hands its synthetic addresses
 * out again (a restart starts the pool over), so a peer we knew by one may
 * now be someone else. Unlike a new game, nothing of the last session's
 * players is worth keeping: no live link spans a sign-in (the title signs in
 * from its menus), and the session's first game starts like the first game
 * after launch. A legacy peer goes too: its key is the address it claims,
 * which a reused 10.64 address can equal; the next records bring it back. */
int xr_session_reset(void)
{
    int n = xr_peer_count() + xr_learned_count();
    memset(s_peer, 0, sizeof s_peer);
    memset(s_learn, 0, sizeof s_learn);
    memset(s_rx, 0, sizeof s_rx);
    s_rx_newest = 0;
    memset(s_hs, 0, sizeof s_hs);
    s_members_seen = 0;
    if (s_has_secret) {
        memset(s_secret, 0, sizeof s_secret);
        s_has_secret = 0;
        s_secret_seq++;
    }
    memset(&s_relay, 0, sizeof s_relay);
    s_relay_port_seen = 0;
    s_relay_game = 0;
    react_clear();
    ping_clear(0);
    return n;
}

void xr_self_synth(uint32_t synth) { s_self_synth = synth; }
void xr_self_lan(uint32_t lan) { s_self_lan = lan; }

static int is_loopback(uint32_t ip) { return ((const uint8_t *)&ip)[0] == 127; }

uint32_t xr_pick_lan(const uint32_t *a, int n, int *why)
{
    int r, i, other = -1;
    /* the private ranges, most likely a home or party LAN first */
    if (why)
        *why = XR_LAN_NONE;
    for (r = 0; r < 3; r++)
        for (i = 0; i < n; i++) {
            const uint8_t *b = (const uint8_t *)&a[i];
            int hit = r == 0 ? (b[0] == 192 && b[1] == 168) : r == 1 ? b[0] == 10 : (b[0] == 172 && (b[1] & 0xF0) == 16);
            if (a[i] && hit) {
                if (why)
                    *why = XR_LAN_PRIVATE;
                return a[i];
            }
        }
    for (i = 0; i < n && other < 0; i++) {
        const uint8_t *b = (const uint8_t *)&a[i];
        if (!a[i] || b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] >= 224 || b[0] == 0)
            continue;
        other = i;
    }
    if (other < 0)
        return 0;
    if (why)
        *why = XR_LAN_OTHER;
    return a[other];
}

uint32_t xr_lan_lobby_at(uint32_t lobby_ip, uint32_t lan)
{
    if (lobby_ip && is_loopback(lobby_ip) && lan && !is_loopback(lan))
        return lan;
    return lobby_ip;
}
uint32_t xr_self_synthetic(void) { return s_self_synth; }

void xr_self(uint32_t title_ip, uint16_t offset, int force_public)
{
    s_self_id = title_ip;
    s_self_off = offset;
    s_force = force_public;
}

uint32_t xr_self_public(void) { return s_self_pub; }

/* RFC 1918, loopback, link-local and CGNAT (100.64/10): never a public address */
int xr_is_private(uint32_t ip)
{
    const uint8_t *a = (const uint8_t *)&ip;
    return a[0] == 10 || a[0] == 127 || (a[0] == 172 && (a[1] & 0xF0) == 16) ||
           (a[0] == 192 && a[1] == 168) || (a[0] == 169 && a[1] == 254) ||
           (a[0] == 100 && (a[1] & 0xC0) == 64);
}

/* SipHash-2-4 (Aumasson, Bernstein), the hellos' tag under the game's secret */
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); \
                      v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
                      v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; \
                      v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)
static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}
uint64_t xr_siphash(const uint8_t key[16], const uint8_t *m, int n)
{
    uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    uint64_t b = (uint64_t)n << 56, mi;
    int i, left = n & 7;
    for (i = 0; i + 8 <= n; i += 8) {
        mi = le64(m + i);
        v3 ^= mi; SIPROUND; SIPROUND; v0 ^= mi;
    }
    for (i = 0; i < left; i++)
        b |= (uint64_t)m[n - left + i] << (8 * i);
    v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

int xr_has_secret(void) { return s_has_secret; }
int xr_secret_seq(void) { return s_secret_seq; }

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A log line budget (hellos from unknown senders, dropped datagrams): at
 * most 10 lines in any 10 s. */
int xr_log_ok(uint64_t now)
{
    static uint64_t start;
    static int n;
    if (now - start >= 10000) {
        start = now;
        n = 0;
    }
    return n++ < 10;
}

/* UPnP's internal client: this copy's address when it is on the router's
 * network (the same /24 as the address UPnP found), else that address.
 * igd_lan_ip is miniupnpc's "lanaddr": the local end of the connection to
 * the router, so this computer's own address on the router's network (a VPN
 * route to the lobby gives local_ip another one), never the router's. */
uint32_t xr_upnp_client(uint32_t local_ip, uint32_t igd_lan_ip)
{
    const uint8_t *a = (const uint8_t *)&local_ip, *b = (const uint8_t *)&igd_lan_ip;
    if (local_ip && igd_lan_ip && a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
        return local_ip;
    return igd_lan_ip ? igd_lan_ip : local_ip;
}

static int member_ok(const peer_t *p)
{
    return !s_members_seen || p->member;
}

/* A peer's record, made when make is set. A full table gives up the
 * longest unused peer that is not in the current game; with every slot a
 * member (cannot happen with 8 players and 32 slots) nothing is replaced. */
static peer_t *find(uint32_t id, int make)
{
    int i, free_i = -1, old_i = -1;
    if (!id)
        return NULL;
    for (i = 0; i < MAX_PEERS; i++) {
        if (s_peer[i].used && s_peer[i].id == id) {
            if (make)
                s_peer[i].touch = ++s_clock;
            return &s_peer[i];
        }
        if (!s_peer[i].used && free_i < 0)
            free_i = i;
        if (s_peer[i].used && !s_peer[i].member && (old_i < 0 || s_peer[i].touch < s_peer[old_i].touch))
            old_i = i;
    }
    if (!make)
        return NULL;
    if (free_i < 0)
        free_i = old_i;
    if (free_i < 0)
        return NULL;
    if (s_peer[free_i].used) {   /* its learned paths and hellos go with it */
        uint32_t gone = s_peer[free_i].id;
        for (i = 0; i < MAX_LEARNED; i++) {
            if (s_learn[i].used && s_learn[i].id == gone)
                s_learn[i].used = 0;
            if (s_rx[i].used && s_rx[i].id == gone)
                s_rx[i].used = 0;
            if (s_hs[i].used && s_hs[i].id == gone)
                s_hs[i].used = 0;
        }
    }
    memset(&s_peer[free_i], 0, sizeof s_peer[free_i]);
    s_peer[free_i].used = 1;
    s_peer[free_i].id = id;
    s_peer[free_i].touch = ++s_clock;
    return &s_peer[free_i];
}

/* where the peer is on its own network: its real address, which is the
 * title address unless the server gave it a synthetic one */
static uint32_t lan_of(const peer_t *p)
{
    return p->real ? p->real : p->id;
}

static int same24(uint32_t a, uint32_t b)
{
    const uint8_t *x = (const uint8_t *)&a, *y = (const uint8_t *)&b;
    return a && b && x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
}

/* whether a datagram from the peer's local address can be that peer's: only
 * when it is on our network (our public address, or for an old-style
 * record our /24), or nothing tells yet (no public address known). A
 * synthetic peer's local address is only ever its LAN path, so only on our
 * public address: someone else's 192.168.1.100 is not it. */
static int local_ok(const peer_t *p)
{
    uint32_t lan = lan_of(p);
    if (p->real)
        return p->pub && s_self_pub && p->pub == s_self_pub;
    if (!xr_is_private(lan))
        return 0;   /* a public one counts only as the lobby's (p->pub) */
    if (!p->pub)
        return 1;
    return (s_self_pub && p->pub == s_self_pub) || same24(lan, s_self_id);
}

static void lobby_rec(uint32_t title_ip, uint32_t public_ip, uint32_t real)
{
    peer_t *p;
    if (!title_ip || !public_ip)
        return;
    if (title_ip == s_self_id && !real) {
        s_self_pub = public_ip;
        return;
    }
    p = find(title_ip, 1);
    if (p) {
        p->pub = public_ip;
        if (real)
            p->real = real;
    }
}

void xr_lobby(uint32_t title_ip, uint32_t public_ip)
{
    lobby_rec(title_ip, public_ip, 0);
}

void xr_offset(uint32_t title_ip, uint16_t offset)
{
    peer_t *p;
    if (title_ip == s_self_id)
        return;
    p = find(title_ip, 1);
    if (p) {
        p->off = offset;
        p->has_off = 1;
    }
}

/* "a.b.c.d" -> network order; 0 when it is not one */
static uint32_t ip4(const char *s, int n)
{
    uint32_t part[4] = { 0, 0, 0, 0 };
    int k = 0, i, digits = 0;
    uint8_t b[4];
    uint32_t r;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            part[k] = part[k] * 10 + (uint32_t)(c - '0');
            if (part[k] > 255 || ++digits > 3)
                return 0;
        } else if (c == '.' && digits && k < 3) {
            k++;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (k != 3 || !digits)
        return 0;
    for (i = 0; i < 4; i++)
        b[i] = (uint8_t)part[i];
    memcpy(&r, b, 4);
    return r;
}

#define MAX_SLOTS 16

void xr_lobby_body(const char *body, int n)
{
    uint32_t la = 0, a = 0, rla = 0, lad[MAX_SLOTS], ad[MAX_SLOTS], rad[MAX_SLOTS];
    int i = 0, ma_off = -1;
    memset(lad, 0, sizeof lad);
    memset(ad, 0, sizeof ad);
    memset(rad, 0, sizeof rad);
    while (i < n && body[i]) {
        int ks, ke, vs, ve;
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        ks = i;
        while (i < n && body[i] && body[i] != '=' && body[i] != '\n')
            i++;
        ke = i;
        if (i >= n || body[i] != '=') {
            while (i < n && body[i] && body[i] != '\n')
                i++;
            continue;
        }
        vs = ++i;
        while (i < n && body[i] && body[i] != '\n' && body[i] != '\r' && body[i] != ' ' && body[i] != '\t')
            i++;
        ve = i;
        {
            const char *k = body + ks;
            int kl = ke - ks;
            uint32_t v = ip4(body + vs, ve - vs);
            if (kl == 8 && !memcmp(k, "TKSECRET", 8) && ve - vs == 32) {
                uint8_t sec[16];
                int j, bad = 0;
                for (j = 0; j < 16; j++) {
                    int h = hexval(body[vs + 2 * j]), l = hexval(body[vs + 2 * j + 1]);
                    if (h < 0 || l < 0)
                        bad = 1;
                    else
                        sec[j] = (uint8_t)(h << 4 | l);
                }
                if (!bad && (!s_has_secret || memcmp(s_secret, sec, 16))) {
                    memcpy(s_secret, sec, 16);
                    s_has_secret = 1;
                    s_secret_seq++;
                }
            } else if (kl == 7 && !memcmp(k, "TKRELAY", 7)) {
                long port = 0;
                int j;
                for (j = vs; j < ve && body[j] >= '0' && body[j] <= '9' && port < 65536; j++)
                    port = port * 10 + (body[j] - '0');
                s_relay_port_seen = (port > 0 && port < 65536 && j == ve) ? (uint16_t)port : 0;
            } else if (kl == 2 && !memcmp(k, "LA", 2))
                la = v;
            else if (kl == 5 && !memcmp(k, "TKRLA", 5))
                rla = v;
            else if (kl == 2 && !memcmp(k, "MA", 2) && ve - vs == 17 && body[vs] == '$') {
                /* the XUID, "$" and its 8 bytes from the last: bytes 3 and 2
                 * are the copy's port offset (online.c fill_user), known
                 * here before the title looks the player up */
                int h3 = hexval(body[vs + 9]), l3 = hexval(body[vs + 10]);
                int h2 = hexval(body[vs + 11]), l2 = hexval(body[vs + 12]);
                if (h3 >= 0 && l3 >= 0 && h2 >= 0 && l2 >= 0)
                    ma_off = (h3 << 12 | l3 << 8 | h2 << 4 | l2);
            }
            else if (kl > 7 && !memcmp(k, "TKRADDR", 7) && k[7] >= '0' && k[7] <= '9') {
                int slot = atoi(k + 7);
                if (slot >= 0 && slot < MAX_SLOTS)
                    rad[slot] = v;
            }
            else if (kl == 1 && k[0] == 'A')
                a = v;
            else if (kl > 5 && !memcmp(k, "LADDR", 5)) {
                int slot = atoi(k + 5);   /* stops at '=' */
                if (slot >= 0 && slot < MAX_SLOTS)
                    lad[slot] = v;
            } else if (kl > 4 && !memcmp(k, "ADDR", 4) && k[4] >= '0' && k[4] <= '9') {
                int slot = atoi(k + 4);
                if (slot >= 0 && slot < MAX_SLOTS)
                    ad[slot] = v;
            }
        }
    }
    if (la && a) {
        lobby_rec(la, a, rla);
        if (ma_off > 0 && !(la == s_self_id && !rla)) {
            peer_t *p = find(la, 0);
            if (p && !p->has_off) {
                p->off = (uint16_t)ma_off;
                p->has_off = 1;
            }
        }
    }
    for (i = 0; i < MAX_SLOTS; i++)
        if (lad[i] && ad[i])
            lobby_rec(lad[i], ad[i], rad[i]);
}

/* the title's UDP ports move up by the offset (xnet_hle.c port_out) */
static uint16_t host_port(uint16_t title_port, uint16_t off)
{
    return title_port < XR_TITLE_PORTS ? (uint16_t)(title_port + off) : title_port;
}

/* A player who has just become a member of our game (a new game, or one it
 * joined): it may not know us yet, and a path we kept from the last game
 * (xr_game_reset keeps them) would put our hellos on the 15 s keep-alive.
 * Its first hellos to us may also have been refused (it knew us before we
 * knew it), and it slows down too once it has our path. So until a hello of
 * its own comes in this game, ours go at the fast rate, the next datagram
 * carrying one at once (BO-344: Ubu2 joined game 10 in the session it had
 * played game 9 in; Mac2, on the keep-alive for Ubu2 since game 9, came from
 * a new router port Ubu2 did not know, and Ubu2 dropped its datagrams for
 * 15 s until Mac2's next hello; the host kicked Ubu2 for the missing link). */
static void member_new(peer_t *p)
{
    int i;
    p->heard = 0;
    p->member_at = 0;
    p->relay_heard = 0;
    memset(p->disc, 0, sizeof p->disc);
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_hs[i].used && s_hs[i].id == p->id)
            s_hs[i].sent = 0;
}

void xr_members_body(const char *body, int n)
{
    uint32_t lad[MAX_SLOTS], rad[MAX_SLOTS];
    int was[MAX_PEERS];
    int i = 0, k;
    memset(lad, 0, sizeof lad);
    memset(rad, 0, sizeof rad);
    while (i < n && body[i]) {
        int ks, ke, vs, ve;
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        ks = i;
        while (i < n && body[i] && body[i] != '=' && body[i] != '\n')
            i++;
        ke = i;
        if (i >= n || body[i] != '=') {
            while (i < n && body[i] && body[i] != '\n')
                i++;
            continue;
        }
        vs = ++i;
        while (i < n && body[i] && body[i] != '\n' && body[i] != '\r' && body[i] != ' ' && body[i] != '\t')
            i++;
        ve = i;
        if (ke - ks > 5 && !memcmp(body + ks, "LADDR", 5)) {
            int slot = atoi(body + ks + 5);
            if (slot >= 0 && slot < MAX_SLOTS)
                lad[slot] = ip4(body + vs, ve - vs);
        } else if (ke - ks > 7 && !memcmp(body + ks, "TKRADDR", 7)) {
            int slot = atoi(body + ks + 7);
            if (slot >= 0 && slot < MAX_SLOTS)
                rad[slot] = ip4(body + vs, ve - vs);
        }
    }
    for (k = 0; k < MAX_SLOTS && !lad[k]; k++)
        ;
    if (k == MAX_SLOTS)
        return;   /* not a member record */
    s_members_seen = 1;
    for (i = 0; i < MAX_PEERS; i++) {
        was[i] = s_peer[i].used && s_peer[i].member;
        s_peer[i].member = 0;
    }
    for (k = 0; k < MAX_SLOTS; k++) {
        peer_t *p;
        if (!lad[k] || (lad[k] == s_self_id && !rad[k]))
            continue;
        p = find(lad[k], 1);
        if (p) {
            if (!was[p - s_peer])
                member_new(p);
            p->member = 1;
            if (rad[k])
                p->real = rad[k];
        }
    }
}

int xr_member_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_PEERS; i++)
        n += s_peer[i].used && s_peer[i].member;
    return n;
}

int xr_learned_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_LEARNED; i++)
        n += s_learn[i].used;
    return n;
}

/* the addr reply: TKADDR= is this copy's synthetic address (a server with
 * the identity scheme); none = an old server, the copy's own address */
int xr_addr_reply(const char *body, int n)
{
    int i = 0;
    uint32_t synth = 0;
    while (i < n && body[i]) {
        int ks, vs, ve;
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        ks = i;
        while (i < n && body[i] && body[i] != '=' && body[i] != '\n')
            i++;
        if (i >= n || body[i] != '=') {
            while (i < n && body[i] && body[i] != '\n')
                i++;
            continue;
        }
        vs = ++i;
        while (i < n && body[i] && body[i] != '\n' && body[i] != '\r' && body[i] != ' ' && body[i] != '\t')
            i++;
        ve = i;
        if (vs - 1 - ks == 6 && !memcmp(body + ks, "TKADDR", 6))
            synth = ip4(body + vs, ve - vs);
    }
    s_self_synth = synth;
    return synth != 0;
}

int xr_is_member(uint32_t title_ip)
{
    peer_t *p = find(title_ip, 0);
    return p && p->member;
}

void xr_secret_prefix(char out[5])
{
    static const char hx[] = "0123456789abcdef";
    out[0] = hx[s_secret[0] >> 4]; out[1] = hx[s_secret[0] & 15];
    out[2] = hx[s_secret[1] >> 4]; out[3] = hx[s_secret[1] & 15];
    out[4] = 0;
    if (!s_has_secret)
        memcpy(out, "none", 5);
}

static learned_t *learned_for(uint32_t id, uint16_t tport)
{
    int i;
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_learn[i].used && s_learn[i].id == id && s_learn[i].tport == tport)
            return &s_learn[i];
    return NULL;
}

/* BO-345: a learned path that is our own router's NAT loopback (a member
 * behind our public address whose hello came from the router's LAN side,
 * 192.168.0.1:<port>). That endpoint is only the reply side of the other
 * copy's flow through the router: it works while that copy sends to our
 * public address and fails once it sends to our loopback endpoint too (int035
 * game 17, int036 game 26). Datagrams from it still count (xr_source); we
 * send to the player's public address (forced) or its LAN address instead. A
 * carrier NAT's loopback (not on our /24) is the only path there, and stays. */
static int our_loopback(const peer_t *p, xr_ep src)
{
    return p->pub && s_self_pub && p->pub == s_self_pub && xr_is_private(src.ip) &&
           src.ip != lan_of(p) && (same24(src.ip, s_self_id) || same24(src.ip, s_self_lan));
}

int xr_route(uint32_t title_ip, uint16_t title_port, xr_ep *out, const char **why, int *changed)
{
    peer_t *p = find(title_ip, 0);
    learned_t *l;
    xr_ep ep;
    const char *w;
    uint16_t off;
    uint32_t lan;
    if (changed)
        *changed = 0;
    if (!p)
        return 0;
    p->touch = ++s_clock;   /* in use: the last to be given up */
    lan = lan_of(p);
    off = p->has_off ? p->off : s_self_off;
    l = learned_for(title_ip, title_port);
    if (l && our_loopback(p, l->src) && (s_force || same24(lan, s_self_id) || same24(lan, s_self_lan)))
        l = NULL;
    if (l && p->lan_denied && p->pub && l->src.ip == lan)
        l = NULL;
    if (l && l->relay && (!s_relay.port || s_relay_off))
        l = NULL;
    if (l && l->relay) {
        ep = s_relay;
        w = "through the relay (no direct path)";
    } else if (l) {
        ep = l->src;
        w = "where its hello came from";
    } else if (!p->pub) {
        ep.ip = lan;
        ep.port = host_port(title_port, off);
        w = s_force ? "local address (forced public, but the lobby has not told its public one)"
                    : "local address (the lobby has not told its public one)";
    } else if (s_force) {
        ep.ip = p->pub;
        ep.port = host_port(title_port, off);
        w = "public address (forced for the test)";
    } else if (p->lan_denied) {
        ep.ip = p->pub;
        ep.port = host_port(title_port, off);
        w = "public address (this machine may not send to its local address: Local Network access?)";
    } else if (s_self_pub && p->pub == s_self_pub) {
        ep.ip = lan;
        ep.port = host_port(title_port, off);
        w = "local address (same public address as ours: same network)";
    } else {
        ep.ip = p->pub;
        ep.port = host_port(title_port, off);
        w = s_self_pub ? "public address (another network)" : "public address (ours not known yet)";
    }
    /* An address the title took from a player's XUID, which that player
     * chose: a public one only when the lobby saw it there too (or a hello
     * came from the path), never a third party's. */
    if (!l && ep.ip == lan && !xr_is_private(lan) && p->pub != lan) {
        ep.ip = 0;
        ep.port = 0;
        w = "refused: a public address the lobby did not confirm";
    }
    if (changed) {   /* only the caller that logs keeps the record */
        int i, slot = -1;
        for (i = 0; i < 4; i++)
            if (p->last[i].tport == title_port || (slot < 0 && !p->last[i].tport))
                slot = i;
        for (i = 0; i < 4; i++)
            if (p->last[i].tport == title_port)
                slot = i;
        if (slot < 0)
            slot = 0;
        if (p->last[slot].tport != title_port || ep.ip != p->last[slot].ep.ip ||
            ep.port != p->last[slot].ep.port || w != p->last[slot].why) {
            p->last[slot].tport = title_port;
            p->last[slot].ep = ep;
            p->last[slot].why = w;
            *changed = 1;
        }
    }
    *out = ep;
    if (why)
        *why = w;
    return ep.ip ? 1 : 2;
}

int xr_lan_denied(uint32_t title_ip, uint32_t to_ip)
{
    peer_t *p = find(title_ip, 0);
    if (!p || !p->pub || p->lan_denied || !to_ip || to_ip != lan_of(p) || to_ip == p->pub)
        return 0;
    p->lan_denied = 1;
    return 1;
}

/* the endpoint the relay's answer gave for this peer's title port, if any */
static const xr_ep *disc_for(const peer_t *p, uint16_t tport)
{
    int i;
    for (i = 0; i < 4; i++)
        if (p->disc[i].ep.port && p->disc[i].tport == tport)
            return &p->disc[i].ep;
    return NULL;
}

int xr_candidates(uint32_t title_ip, uint16_t title_port, xr_ep out[XR_MAX_CAND])
{
    peer_t *p = find(title_ip, 0);
    const learned_t *l;
    const char *w;
    int n = 0, i;
    uint16_t off;
    const xr_ep *d;
    if (!p || xr_route(title_ip, title_port, &out[0], &w, NULL) != 1)
        return 0;
    n = 1;
    l = learned_for(title_ip, title_port);
    if (l && l->relay && s_relay.port && !s_relay_off) {
        /* the route is the relay: the hellos try the direct paths too (the
         * guess as if no path were known), so a direct one can take over */
        learned_t *lw = (learned_t *)l;
        lw->relay = 0;
        lw->used = 0;      /* hidden for one call */
        if (xr_route(title_ip, title_port, &out[0], &w, NULL) != 1)
            n = 0;
        lw->used = 1;
        lw->relay = 1;
        l = NULL;
        if (!n)
            return 0;
    }
    if (l && !(our_loopback(p, l->src) && (s_force || same24(lan_of(p), s_self_id) || same24(lan_of(p), s_self_lan))))
        return n;
    off = p->has_off ? p->off : s_self_off;
    /* one network by the public address (or two homes behind one, CGNAT):
     * the other path too, through the router; whichever answers first wins */
    if (!s_force && p->pub && p->pub == s_self_pub && out[0].ip == lan_of(p)) {
        out[n].ip = p->pub;
        out[n].port = host_port(title_port, off);
        n++;
    }
    /* the relay's answer said where that port really is (BO-381) */
    d = disc_for(p, title_port);
    if (d) {
        for (i = 0; i < n; i++)
            if (out[i].ip == d->ip && out[i].port == d->port)
                break;
        if (i == n && n < XR_MAX_CAND)
            out[n++] = *d;
    }
    return n;
}

int xr_route_is_relay(uint32_t title_ip, uint16_t title_port)
{
    const learned_t *l = learned_for(title_ip, title_port);
    return l && l->relay && s_relay.port && !s_relay_off;
}

int xr_known_source(xr_ep src)
{
    uint32_t id;
    uint16_t tp;
    return xr_source(src, &id, &tp);
}

int xr_peer_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_PEERS; i++)
        n += s_peer[i].used;
    return n;
}

int xr_source(xr_ep src, uint32_t *title_ip, uint16_t *title_port)
{
    return xr_source_on(src, 0, title_ip, title_port);
}

/* whether p's title port tp, behind its offset, is our socket's (ltport) or,
 * with that not known, one of the title's game ports */
static int port_fits(const peer_t *p, uint16_t port, uint16_t ltport, uint16_t *tp)
{
    uint16_t t;
    if (!p->has_off || port < p->off)
        return 0;
    t = (uint16_t)(port - p->off);
    if (ltport ? t != ltport : (t != 1000 && t != 3074 && t != 6000))
        return 0;
    *tp = t;
    return 1;
}

/* BO-345: the title drops its link to a player the moment that player's
 * datagram comes from an address other than that player's (0x1B7FD8 ->
 * 0x102B50, "the link was lost"). So a datagram we cannot tie to one player
 * is dropped (a lost datagram), never handed to a guess (a lost link). */
int xr_source_on(xr_ep src, uint16_t ltport, uint32_t *title_ip, uint16_t *title_port)
{
    int i, n, best;
    const peer_t *hit = NULL;
    uint16_t hit_tp = 0;
    /* only the players of the current game reach the title (once the lobby
     * has said who they are), never a peer of an earlier game */
    /* 1. a source a signed hello of the player came from, on this socket,
     * lately: a live flow brings one at least every 15 s (the keep-alive),
     * and a router hands an old flow's port to someone else (int037: the
     * loopback ports 1133-1150 went round the players from game to game) */
    best = -1;
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_rx[i].used && s_rx[i].src.ip == src.ip && s_rx[i].src.port == src.port &&
            (!ltport || s_rx[i].ltport == ltport) && s_rx[i].seen + LEARNED_STALE_MS >= s_rx_newest) {
            const peer_t *p = find(s_rx[i].id, 0);
            if (!p || !member_ok(p))
                continue;
            if (best < 0 || s_rx[i].seen > s_rx[best].seen)
                best = i;
        }
    if (best >= 0) {
        *title_ip = s_rx[best].id;
        *title_port = s_rx[best].tport;
        return 1;
    }
    /* 2. a learned path (the same, kept for routing) */
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_learn[i].used && s_learn[i].src.ip == src.ip && s_learn[i].src.port == src.port &&
            (!ltport || s_learn[i].tport == ltport) && s_learn[i].seen + LEARNED_STALE_MS >= s_rx_newest) {
            const peer_t *p = find(s_learn[i].id, 0);
            if (!p || !member_ok(p))
                continue;
            *title_ip = s_learn[i].id;
            *title_port = s_learn[i].tport;
            return 1;
        }
    /* 3. a member's public or local address: the one member there, or the
     * one whose port offset fits (several copies behind one address) */
    for (n = 0, i = 0; i < MAX_PEERS; i++) {
        const peer_t *p = &s_peer[i];
        if (!p->used || !member_ok(p))
            continue;
        if ((p->pub && src.ip == p->pub) || (src.ip == lan_of(p) && local_ok(p))) {
            n++;
            hit = p;
        }
    }
    if (n == 1) {
        uint16_t off = hit->has_off ? hit->off : s_self_off;
        *title_ip = hit->id;
        *title_port = (src.port >= off && src.port - off < XR_TITLE_PORTS) ? (uint16_t)(src.port - off) : src.port;
        return 1;
    }
    if (n > 1) {
        int m = 0;
        for (i = 0; i < MAX_PEERS; i++) {
            const peer_t *p = &s_peer[i];
            uint16_t tp;
            if (!p->used || !member_ok(p))
                continue;
            if (((p->pub && src.ip == p->pub) || (src.ip == lan_of(p) && local_ok(p))) &&
                port_fits(p, src.port, ltport, &tp)) {
                m++;
                hit = p;
                hit_tp = tp;
            }
        }
        if (m == 1) {
            *title_ip = hit->id;
            *title_port = hit_tp;
            return 1;
        }
        return 0;
    }
    /* 4. A member of this game behind our own router: NAT loopback shows the
     * router's (private) address as the source, with the sender's own port.
     * Only a member sharing our public address, and only on one of its title
     * ports (its offset tells which member). */
    if (xr_is_private(src.ip) && s_self_pub) {
        int m = 0;
        for (i = 0; i < MAX_PEERS; i++) {
            const peer_t *p = &s_peer[i];
            uint16_t tp;
            if (!p->used || !p->member || p->pub != s_self_pub || !port_fits(p, src.port, ltport, &tp))
                continue;
            m++;
            hit = p;
            hit_tp = tp;
        }
        if (m == 1) {
            *title_ip = hit->id;
            *title_port = hit_tp;
            return 1;
        }
    }
    return 0;
}

/* "TKDH", version 2, 0, the title address (4), the offset (2, big-endian),
 * the title port it was sent from (2, big-endian), 2 zero, then 8 bytes:
 * SipHash of the first 16 under the game's secret (zero without one). */
static void hello_tag(const uint8_t *b, uint8_t *tag)
{
    uint64_t t = s_has_secret ? xr_siphash(s_secret, b, 16) : 0;
    int i;
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(t >> (8 * i));
}

int xr_hello_make(uint8_t *b, uint16_t title_port)
{
    memset(b, 0, XR_HELLO_LEN);
    memcpy(b, "TKDH", 4);
    b[4] = 2;
    /* the server's synthetic address when it gave one: the others know us by it */
    memcpy(b + 6, s_self_synth ? &s_self_synth : &s_self_id, 4);
    b[10] = (uint8_t)(s_self_off >> 8); b[11] = (uint8_t)s_self_off;
    b[12] = (uint8_t)(title_port >> 8); b[13] = (uint8_t)title_port;
    b[14] = 1;   /* this copy answers TKPG (older copies leave it 0, and ignore it) */
    hello_tag(b, b + 16);
    return XR_HELLO_LEN;
}

static hello_t *hello_for(uint32_t id, uint16_t tport, int make, uint64_t now);

int xr_hello_make_for(uint8_t *b, uint16_t title_port, uint32_t peer)
{
    const hello_t *h = hello_for(peer, title_port, 0, 0);
    int n = xr_hello_make(b, title_port);
    /* bit 1: its direct hello reached this port lately, so it may leave the
     * relay for the direct path (the tag is made after) */
    if (h && h->direct_in && (h->last <= h->direct_in || h->last - h->direct_in < LEARNED_STALE_MS)) {
        b[14] |= 2;
        hello_tag(b, b + 16);
    }
    return n;
}

int xr_is_hello(const uint8_t *b, int n)
{
    return n == XR_HELLO_LEN && !memcmp(b, "TKDH", 4) && b[4] == 2;
}

/* A source endpoint is one socket of one player: once a hello ties it to a
 * peer, another peer's path from there is stale (an address handed out again,
 * BO-343) and would hand that player's datagrams to the wrong peer. */
static void learned_claim(const learned_t *l)
{
    int i;
    /* per title port (BO-345): one router source can carry two players'
     * flows to two of our sockets */
    for (i = 0; i < MAX_LEARNED; i++)
        if (&s_learn[i] != l && s_learn[i].used && s_learn[i].id != l->id && s_learn[i].tport == l->tport &&
            s_learn[i].src.ip == l->src.ip && s_learn[i].src.port == l->src.port)
            s_learn[i].used = 0;
}

/* A signed hello of player id (its title port tport) reached our socket
 * ltport from src: from now on src's datagrams to that socket are id's. A
 * source another player had on that socket is this one's now (BO-343). */
static void rx_note(xr_ep src, uint16_t ltport, uint32_t id, uint16_t tport, uint64_t now)
{
    int i, slot = -1, old = -1;
    for (i = 0; i < MAX_LEARNED; i++) {
        if (s_rx[i].used && s_rx[i].ltport == ltport && s_rx[i].src.ip == src.ip && s_rx[i].src.port == src.port) {
            slot = i;
            break;
        }
        if (!s_rx[i].used) {
            if (slot < 0)
                slot = i;
        } else {
            const peer_t *q = find(s_rx[i].id, 0);
            int gone = !q || !member_ok(q);
            if (old < 0)
                old = i;
            else {
                const peer_t *o = find(s_rx[old].id, 0);
                int ogone = !o || !member_ok(o);
                if (gone > ogone || (gone == ogone && s_rx[i].seen < s_rx[old].seen))
                    old = i;
            }
        }
    }
    if (slot < 0)
        slot = old;
    if (slot < 0)
        return;
    s_rx[slot].used = 1;
    s_rx[slot].src = src;
    s_rx[slot].ltport = ltport;
    s_rx[slot].tport = tport;
    s_rx[slot].id = id;
    s_rx[slot].seen = now;
    if (now > s_rx_newest)
        s_rx_newest = now;
}

/* A path just learned: the peer knows us (its hello carried the game's tag),
 * but our own early hellos may have reached it before it knew us and been
 * refused; now that we have its path ours would slow to one in 15 s. So the
 * next datagram to it carries a hello at once (BO-343: two guests joining
 * together linked only on the 15 s keep-alive). */
static void learned_new(const learned_t *l)
{
    int i;
    learned_claim(l);
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_hs[i].used && s_hs[i].id == l->id)
            s_hs[i].sent = 0;
}

int xr_hello_seen(xr_ep src, const uint8_t *b, int n, uint32_t *title_ip, uint64_t now)
{
    return xr_hello_seen_on(src, 0, b, n, title_ip, now);
}

static int hello_seen_impl(xr_ep src, uint16_t ltport, const uint8_t *b, int n, uint32_t *title_ip,
                           uint64_t now, int via_relay)
{
    uint32_t id;
    uint16_t off, tport;
    peer_t *p;
    learned_t *l;
    hello_t *h;
    uint8_t tag[8];
    int i, both;
    if (title_ip)
        *title_ip = 0;
    if (!xr_is_hello(b, n))
        return XR_HELLO_BAD;
    memcpy(&id, b + 6, 4);
    off = (uint16_t)(b[10] << 8 | b[11]);
    tport = (uint16_t)(b[12] << 8 | b[13]);
    if (title_ip)
        *title_ip = id;
    if (!id || id == s_self_id || (s_self_synth && id == s_self_synth))
        return XR_HELLO_BAD;
    /* only a player of the current game the lobby told us about, never one
     * a hello invents (nor one of an earlier game) */
    p = find(id, 0);
    if (!p || !member_ok(p))
        return XR_HELLO_UNKNOWN;
    hello_tag(b, tag);
    if (memcmp(tag, b + 16, 8))
        return XR_HELLO_BADTAG;
    /* from that player's own addresses: its public one (a router may change
     * the port, not the address), its local one, or, when it shares our
     * public address, any address of the local network (NAT loopback shows
     * the router's own). Through the relay the tag is the proof. */
    if (!via_relay && !((src.ip == lan_of(p) && local_ok(p)) || (p->pub && src.ip == p->pub) ||
                        (p->pub && p->pub == s_self_pub && xr_is_private(src.ip))))
        return XR_HELLO_WRONGSRC;
    ping_can(id, b[14] & 1);
    if (!p->has_off) {
        p->off = off;
        p->has_off = 1;
    }
    if (via_relay)
        p->relay_heard = 1;
    else
        rx_note(src, ltport ? ltport : tport, id, tport, now);
    /* The test switch: only a path through the router counts, not the
     * local network's direct one. */
    if (s_force && !via_relay && src.ip == lan_of(p))
        return XR_HELLO_OLD;
    /* its first hello in this game: it knows us now, and our hellos so far
     * may have reached it before it did and been refused, so the next
     * datagram to it carries one at once (BO-344) */
    if (!p->heard) {
        p->heard = 1;
        for (i = 0; i < MAX_LEARNED; i++)
            if (s_hs[i].used && s_hs[i].id == id)
                s_hs[i].sent = 0;
    }
    /* a direct hello: noted for our own hellos' bit 1 (it hears us
     * directly when that bit comes back) */
    h = hello_for(id, tport, 1, now);
    if (!via_relay && h)
        h->direct_in = now;
    both = (b[14] & 2) != 0;
    l = learned_for(id, tport);
    if (l) {
        if (l->relay && !via_relay) {
            /* on the relay: the direct path takes over only once the peer
             * says it hears us directly too (one-way paths would lose the
             * link), else the relay stays */
            if (!both)
                return XR_HELLO_ONEWAY;
            l->src = src;
            l->seen = now;
            l->relay = 0;
            learned_new(l);
            return XR_HELLO_NEW;
        }
        if (!l->relay && via_relay) {
            /* a direct path is better than the relay while it is alive */
            if (now - l->seen < LEARNED_STALE_MS)
                return XR_HELLO_OLD;
            l->src = src;
            l->seen = now;
            l->relay = 1;
            learned_new(l);
            return XR_HELLO_NEW;
        }
        if (l->src.ip == src.ip && l->src.port == src.port) {
            l->seen = now;
            if (!l->relay)
                learned_claim(l);
            return XR_HELLO_OLD;
        }
        /* another path: the same address with a new port (the router moved
         * it), or any once the one we have went quiet */
        if (l->src.ip != src.ip && now - l->seen < LEARNED_STALE_MS)
            return XR_HELLO_OLD;
        l->src = src;
        l->seen = now;
        learned_new(l);
        return XR_HELLO_NEW;
    }
    for (i = 0; i < MAX_LEARNED && s_learn[i].used; i++)
        ;
    if (i == MAX_LEARNED) {
        /* full: the oldest path of a peer no longer in the game, or one
         * that went quiet; else keep what we have */
        int j, old = -1;
        for (j = 0; j < MAX_LEARNED; j++) {
            const peer_t *q = find(s_learn[j].id, 0);
            if ((!q || !member_ok(q) || now - s_learn[j].seen >= LEARNED_STALE_MS) &&
                (old < 0 || s_learn[j].seen < s_learn[old].seen))
                old = j;
        }
        if (old < 0)
            return XR_HELLO_OLD;
        i = old;
    }
    s_learn[i].used = 1;
    s_learn[i].src = src;
    s_learn[i].id = id;
    s_learn[i].tport = tport;
    s_learn[i].seen = now;
    s_learn[i].relay = via_relay;
    learned_new(&s_learn[i]);
    return XR_HELLO_NEW;
}

int xr_hello_seen_on(xr_ep src, uint16_t ltport, const uint8_t *b, int n, uint32_t *title_ip, uint64_t now)
{
    return hello_seen_impl(src, ltport, b, n, title_ip, now, 0);
}

/* the hello schedule's record for (id, tport), made when make is set (a
 * full table gives up the one that went longest without a hello) */
static hello_t *hello_for(uint32_t id, uint16_t tport, int make, uint64_t now)
{
    hello_t *h;
    int i, old = 0;
    (void)now;
    for (i = 0; i < MAX_LEARNED; i++)
        if (s_hs[i].used && s_hs[i].id == id && s_hs[i].tport == tport)
            return &s_hs[i];
    if (!make)
        return NULL;
    for (i = 0; i < MAX_LEARNED && s_hs[i].used; i++)
        if (s_hs[i].last < s_hs[old].last)
            old = i;
    if (i == MAX_LEARNED)
        i = old;
    h = &s_hs[i];
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->id = id;
    h->tport = tport;
    return h;
}

int xr_hello_due(uint32_t title_ip, uint16_t title_port, uint64_t now)
{
    hello_t *h;
    peer_t *p = find(title_ip, 0);
    const learned_t *l;
    int got;
    if (!p)
        return 0;
    h = hello_for(title_ip, title_port, 1, now);
    if (!p->member_at)
        p->member_at = now;   /* our hellos to it begin: the relay waits XR_RELAY_AFTER_MS */
    /* the keep-alive only once its own hello came (in this game, member_new);
     * on the relay the fast rate stays a while longer, so a direct path can
     * still be found (BO-381) */
    l = learned_for(title_ip, title_port);
    got = l != NULL && p->heard && (!l->relay || now - p->member_at >= XR_RELAY_DIRECT_FAST_MS);
    if (h->sent && now - h->last < (uint64_t)(got ? HELLO_KEEP_MS : HELLO_FAST_MS))
        return 0;
    h->sent++;
    h->last = now;
    return 1;
}

/* a TKPG/TKRX/TKVT from the relay's endpoint is peer p's when the caller
 * said so for this one call (xr_relay_trust: it unwrapped the datagram and
 * checked its tag) */
static int relay_src_ok(xr_ep src, const peer_t *p)
{
    int ok = s_relay.port && src.ip == s_relay.ip && src.port == s_relay.port && s_relay_trust == p->id;
    s_relay_trust = 0;
    return ok;
}

/* ── the reactions side channel (BO-352), see xnet_route.h ───────── */

int xr_member_list(uint32_t *ids, int max)
{
    int i, n = 0;
    for (i = 0; i < MAX_PEERS && n < max; i++)
        if (s_peer[i].used && s_peer[i].member)
            ids[n++] = s_peer[i].id;
    return n;
}

int xr_is_self(uint32_t id)
{
    return id && (id == s_self_id || id == s_self_synth);
}

/* the send/receive rate: a token bucket (a burst of 3, then one per 1.5 s)
 * and a rolling minute cap (10 a minute). Each sender has its own; this
 * copy's own send is the "self" bucket. */
#define REACT_BURST      3
#define REACT_REFILL_MS  1500
#define REACT_MINUTE     10
#define REACT_MINUTE_MS  60000
#define REACT_QUEUE      8

typedef struct {
    uint32_t id;               /* the sender's title address; 0 = unused */
    int tokens;                /* 0..REACT_BURST */
    uint64_t last;             /* when it last refilled (ms); 0 = fresh */
    uint64_t minute[REACT_MINUTE];
    int minute_n;
    uint32_t last_seq;         /* the last sequence seen from this sender */
} react_bucket_t;

static react_bucket_t s_react_self;
static react_bucket_t s_react_rx[MAX_PEERS];
typedef struct { uint32_t id; int kind; int value; } react_item_t;
static react_item_t s_react_q[REACT_QUEUE];
static int s_react_q_head, s_react_q_n;
static uint32_t s_react_seq;

static void vote_clear(void);

static void react_clear(void)
{
    memset(&s_react_self, 0, sizeof s_react_self);
    memset(s_react_rx, 0, sizeof s_react_rx);
    s_react_q_head = s_react_q_n = 0;
    vote_clear();
}

/* the SipHash tag over the packet's first 24 bytes, under the game's secret */
static void react_tag(const uint8_t *b, uint8_t *tag)
{
    uint64_t t = s_has_secret ? xr_siphash(s_secret, b, 24) : 0;
    int i;
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(t >> (8 * i));
}

int xr_react_make(uint8_t *b, int kind, int value)
{
    uint32_t seq = ++s_react_seq;
    memset(b, 0, XR_REACT_LEN);
    memcpy(b, "TKRX", 4);
    b[4] = 1;
    b[5] = (uint8_t)kind;
    /* the server's synthetic address when it gave one, as the hellos do */
    memcpy(b + 6, s_self_synth ? &s_self_synth : &s_self_id, 4);
    b[10] = (uint8_t)(seq >> 24);
    b[11] = (uint8_t)(seq >> 16);
    b[12] = (uint8_t)(seq >> 8);
    b[13] = (uint8_t)seq;
    b[14] = (uint8_t)value;
    react_tag(b, b + 24);
    return XR_REACT_LEN;
}

int xr_is_react(const uint8_t *b, int n)
{
    return n == XR_REACT_LEN && !memcmp(b, "TKRX", 4) && b[4] == 1;
}

/* a sender's receive bucket, made as needed (the peers' table is the bound) */
static react_bucket_t *react_rx(uint32_t id, int make)
{
    int i, free_i = -1;
    for (i = 0; i < MAX_PEERS; i++) {
        if (s_react_rx[i].id == id)
            return &s_react_rx[i];
        if (!s_react_rx[i].id && free_i < 0)
            free_i = i;
    }
    if (!make || free_i < 0)
        return NULL;
    memset(&s_react_rx[free_i], 0, sizeof s_react_rx[free_i]);
    s_react_rx[free_i].id = id;
    return &s_react_rx[free_i];
}

/* consume one send/receive from the bucket, or refuse (the limit) */
static int bucket_ok(react_bucket_t *b, uint64_t now)
{
    int i, o = 0, add;
    uint64_t elapsed;
    if (!b->last) {
        b->last = now;
        b->tokens = REACT_BURST;
    }
    /* the minute window: forget sends older than a minute */
    for (i = 0; i < b->minute_n; i++)
        if (now - b->minute[i] < REACT_MINUTE_MS)
            b->minute[o++] = b->minute[i];
    b->minute_n = o;
    if (b->minute_n >= REACT_MINUTE)
        return 0;   /* 10 in the last minute already */
    /* the burst bucket: one token per 1.5 s, at most 3 */
    elapsed = now - b->last;
    add = (int)(elapsed / REACT_REFILL_MS);
    if (add > REACT_BURST - b->tokens)
        add = REACT_BURST - b->tokens;
    if (add > 0) {
        b->tokens += add;
        b->last += (uint64_t)add * REACT_REFILL_MS;
    }
    if (b->tokens <= 0)
        return 0;
    b->tokens--;
    b->minute[b->minute_n++] = now;
    return 1;
}

int xr_react_send_ok(uint64_t now)
{
    return bucket_ok(&s_react_self, now);
}

int xr_react_seen(xr_ep src, const uint8_t *b, int n, uint32_t *title_ip,
                  int *kind, int *value, uint64_t now)
{
    uint32_t id, seq;
    int k, v;
    peer_t *p;
    react_bucket_t *rb;
    uint8_t tag[8];
    if (title_ip) *title_ip = 0;
    if (kind) *kind = 0;
    if (value) *value = 0;
    if (!xr_is_react(b, n))
        return XR_REACT_BAD;
    memcpy(&id, b + 6, 4);
    if (title_ip) *title_ip = id;
    k = b[5];
    v = b[14];
    if (k != XR_REACT_REACTION && k != XR_REACT_COUNTDOWN && k != XR_REACT_LEFT)
        return XR_REACT_BAD;
    if (k == XR_REACT_REACTION && v >= XR_REACT_N)
        return XR_REACT_BAD;
    if (!id || id == s_self_id || (s_self_synth && id == s_self_synth))
        return XR_REACT_BAD;
    /* only a member of the current game, never one a packet invents */
    p = find(id, 0);
    if (!p || !member_ok(p))
        return XR_REACT_UNKNOWN;
    react_tag(b, tag);
    if (memcmp(tag, b + 24, 8))
        return XR_REACT_BADTAG;
    /* from that player's own addresses, the same rule as its hellos */
    if (!relay_src_ok(src, p) && !((src.ip == lan_of(p) && local_ok(p)) || (p->pub && src.ip == p->pub) ||
          (p->pub && p->pub == s_self_pub && xr_is_private(src.ip))))
        return XR_REACT_WRONGSRC;
    seq = (uint32_t)b[10] << 24 | (uint32_t)b[11] << 16 | (uint32_t)b[12] << 8 | b[13];
    rb = react_rx(id, 1);
    if (!rb)
        return XR_REACT_LIMIT;   /* no slot left: treat as over the limit */
    if (rb->last_seq && seq <= rb->last_seq)
        return XR_REACT_DUP;     /* the same packet sent twice stays one */
    if (!bucket_ok(rb, now))
        return XR_REACT_LIMIT;
    rb->last_seq = seq;
    if (kind) *kind = k;
    if (value) *value = v;
    if (s_react_q_n < REACT_QUEUE) {
        react_item_t *it = &s_react_q[(s_react_q_head + s_react_q_n) % REACT_QUEUE];
        it->id = id;
        it->kind = k;
        it->value = v;
        s_react_q_n++;
    }
    return XR_REACT_OK;
}

int xr_react_get(uint32_t *title_ip, int *kind, int *value)
{
    react_item_t *it;
    if (!s_react_q_n)
        return 0;
    it = &s_react_q[s_react_q_head];
    if (title_ip) *title_ip = it->id;
    if (kind) *kind = it->kind;
    if (value) *value = it->value;
    s_react_q_head = (s_react_q_head + 1) % REACT_QUEUE;
    s_react_q_n--;
    return 1;
}

/* ── the peers' ping (BO-357) ─────────────────────────────────────── */

#define PING_KEEP 16           /* pings remembered: the loss window */
#define PING_RTTS 9            /* answers remembered: min and median */
#define PING_ANSWER_MS 200     /* at most one answer per sender in this time */

typedef struct {
    uint32_t seq;
    uint64_t at;
    int got;
} ping_sent_t;

typedef struct {
    uint32_t id;
    int can;                   /* its hello says it answers */
    uint64_t next;             /* our next ping */
    uint64_t answered;         /* when we last answered it */
    uint32_t last_asked;       /* the newest of its pings we saw */
    ping_sent_t sent[PING_KEEP];
    int sent_at;
    unsigned rtt[PING_RTTS];
    int rtt_n, rtt_at;
} ping_t;

static ping_t s_ping[MAX_PEERS];
static uint32_t s_ping_seq;

/* a new game: the measurements go. keep_can: who can answer stays, a hello
 * comes only every 15 s and the next game's players are often these. */
static void ping_clear(int keep_can)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++) {
        uint32_t id = s_ping[i].id;
        int can = s_ping[i].can;
        memset(&s_ping[i], 0, sizeof s_ping[i]);
        if (keep_can && can) {
            s_ping[i].id = id;
            s_ping[i].can = 1;
        }
    }
}

static ping_t *ping_for(uint32_t id, int make)
{
    int i, free_i = -1;
    for (i = 0; i < MAX_PEERS; i++) {
        if (s_ping[i].id == id)
            return &s_ping[i];
        if (!s_ping[i].id && free_i < 0)
            free_i = i;
    }
    if (!make)
        return NULL;
    /* full of the earlier games' players: one of those makes room */
    for (i = 0; free_i < 0 && i < MAX_PEERS; i++) {
        const peer_t *p = find(s_ping[i].id, 0);
        if (!p || !member_ok(p))
            free_i = i;
    }
    if (free_i < 0)
        return NULL;
    memset(&s_ping[free_i], 0, sizeof s_ping[free_i]);
    s_ping[free_i].id = id;
    return &s_ping[free_i];
}

static void ping_can(uint32_t id, int can)
{
    ping_t *g = ping_for(id, can);
    if (g)
        g->can = can;
}

static void ping_tag(const uint8_t *b, uint8_t *tag)
{
    uint64_t t = s_has_secret ? xr_siphash(s_secret, b, 24) : 0;
    int i;
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(t >> (8 * i));
}

static void ping_make(uint8_t *b, int kind, uint32_t seq)
{
    memset(b, 0, XR_PING_LEN);
    memcpy(b, "TKPG", 4);
    b[4] = 1;
    b[5] = (uint8_t)kind;
    memcpy(b + 6, s_self_synth ? &s_self_synth : &s_self_id, 4);
    b[10] = (uint8_t)(seq >> 24);
    b[11] = (uint8_t)(seq >> 16);
    b[12] = (uint8_t)(seq >> 8);
    b[13] = (uint8_t)seq;
    ping_tag(b, b + 24);
}

int xr_is_ping(const uint8_t *b, int n)
{
    return n == XR_PING_LEN && !memcmp(b, "TKPG", 4) && b[4] == 1;
}

int xr_ping_next(uint64_t now, uint32_t *title_ip, uint8_t *b)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++) {
        ping_t *g = &s_ping[i];
        peer_t *p;
        ping_sent_t *e;
        if (!g->id || !g->can || now < g->next)
            continue;
        p = find(g->id, 0);
        if (!p || !member_ok(p))
            continue;
        g->next = now + XR_PING_EVERY_MS;
        if (!++s_ping_seq)
            s_ping_seq = 1;
        e = &g->sent[g->sent_at];
        g->sent_at = (g->sent_at + 1) % PING_KEEP;
        e->seq = s_ping_seq;
        e->at = now;
        e->got = 0;
        ping_make(b, XR_PING_PING, s_ping_seq);
        *title_ip = g->id;
        return 1;
    }
    return 0;
}

int xr_ping_seen(xr_ep src, const uint8_t *b, int n, uint32_t *title_ip, uint8_t *reply,
                 unsigned *rtt_ms, uint64_t now)
{
    uint32_t id, seq;
    peer_t *p;
    ping_t *g;
    uint8_t tag[8];
    int i;
    if (title_ip) *title_ip = 0;
    if (!xr_is_ping(b, n) || (b[5] != XR_PING_PING && b[5] != XR_PING_ANSWER))
        return XR_PING_BAD;
    memcpy(&id, b + 6, 4);
    if (title_ip) *title_ip = id;
    if (!id || id == s_self_id || (s_self_synth && id == s_self_synth))
        return XR_PING_BAD;
    p = find(id, 0);
    if (!p || !member_ok(p))
        return XR_PING_UNKNOWN;
    ping_tag(b, tag);
    if (memcmp(tag, b + 24, 8))
        return XR_PING_BADTAG;
    if (!relay_src_ok(src, p) && !((src.ip == lan_of(p) && local_ok(p)) || (p->pub && src.ip == p->pub) ||
          (p->pub && p->pub == s_self_pub && xr_is_private(src.ip))))
        return XR_PING_WRONGSRC;
    seq = (uint32_t)b[10] << 24 | (uint32_t)b[11] << 16 | (uint32_t)b[12] << 8 | b[13];
    g = ping_for(id, 1);
    if (!g)
        return XR_PING_LIMIT;
    if (b[5] == XR_PING_PING) {
        /* sent from each socket along each path: one answer is enough */
        if (seq == g->last_asked)
            return XR_PING_NOTHING;
        if (g->answered && now - g->answered < PING_ANSWER_MS)
            return XR_PING_LIMIT;
        g->last_asked = seq;
        g->answered = now;
        ping_make(reply, XR_PING_ANSWER, seq);
        return XR_PING_ANSWER_IT;
    }
    for (i = 0; i < PING_KEEP; i++) {
        ping_sent_t *e = &g->sent[i];
        if (e->seq == seq && e->at && !e->got) {
            unsigned ms = (unsigned)(now - e->at);
            e->got = 1;
            g->rtt[g->rtt_at] = ms;
            g->rtt_at = (g->rtt_at + 1) % PING_RTTS;
            if (g->rtt_n < PING_RTTS)
                g->rtt_n++;
            if (rtt_ms) *rtt_ms = ms;
            return XR_PING_MEASURED;
        }
    }
    return XR_PING_NOTHING;   /* a second path's copy, or one from before the last clear */
}

int xr_peer_rtt(uint32_t title_ip, uint64_t now, xr_rtt *out)
{
    const ping_t *g = ping_for(title_ip, 0);
    unsigned s[PING_RTTS];
    int i, j;
    memset(out, 0, sizeof *out);
    if (!g || !g->rtt_n)
        return 0;
    for (i = 0; i < PING_KEEP; i++) {
        const ping_sent_t *e = &g->sent[i];
        if (!e->at)
            continue;
        if (e->got) {
            out->sent++;
        } else if (now - e->at >= XR_PING_LOST_MS) {
            out->sent++;
            out->lost++;
        }
    }
    for (i = 0; i < g->rtt_n; i++) {
        unsigned v = g->rtt[i];
        for (j = i; j > 0 && s[j - 1] > v; j--)
            s[j] = s[j - 1];
        s[j] = v;
    }
    out->samples = g->rtt_n;
    out->min_ms = s[0];
    out->med_ms = s[g->rtt_n / 2];
    out->last_ms = g->rtt[(g->rtt_at + PING_RTTS - 1) % PING_RTTS];
    return 1;
}

/* ── the track vote's side channel, see xnet_route.h ─────────────── */

#define VOTE_QUEUE     32
#define VOTE_PER_SEC   40      /* packets a second from one sender: far above what the vote sends */

typedef struct { uint32_t id; uint32_t last_seq; uint64_t window; int n; } vote_rx_t;
typedef struct { uint32_t id; uint8_t data[XR_VOTE_DATA]; } vote_item_t;
static vote_rx_t s_vote_rx[MAX_PEERS];
static vote_item_t s_vote_q[VOTE_QUEUE];
static int s_vote_q_head, s_vote_q_n;
static uint32_t s_vote_seq;

static void vote_clear(void)
{
    memset(s_vote_rx, 0, sizeof s_vote_rx);
    s_vote_q_head = s_vote_q_n = 0;
}

static void vote_tag(const uint8_t *b, uint8_t *tag)
{
    uint64_t t = s_has_secret ? xr_siphash(s_secret, b, 40) : 0;
    int i;
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(t >> (8 * i));
}

int xr_vote_make(uint8_t *b, const uint8_t data[XR_VOTE_DATA])
{
    uint32_t seq = ++s_vote_seq, id = s_self_synth ? s_self_synth : s_self_id;
    memset(b, 0, XR_VOTE_LEN);
    memcpy(b, "TKVT", 4);
    b[4] = 1;
    memcpy(b + 6, &id, 4);
    b[10] = (uint8_t)(seq >> 24);
    b[11] = (uint8_t)(seq >> 16);
    b[12] = (uint8_t)(seq >> 8);
    b[13] = (uint8_t)seq;
    memcpy(b + 14, data, XR_VOTE_DATA);
    vote_tag(b, b + 40);
    return XR_VOTE_LEN;
}

int xr_is_vote(const uint8_t *b, int n)
{
    return n == XR_VOTE_LEN && !memcmp(b, "TKVT", 4) && b[4] == 1;
}

int xr_vote_seen(xr_ep src, const uint8_t *b, int n, uint32_t *title_ip, uint64_t now)
{
    uint32_t id, seq;
    uint8_t tag[8];
    peer_t *p;
    vote_rx_t *rx = NULL;
    int i;
    if (title_ip) *title_ip = 0;
    if (!xr_is_vote(b, n))
        return XR_REACT_BAD;
    memcpy(&id, b + 6, 4);
    if (title_ip) *title_ip = id;
    if (!id || id == s_self_id || (s_self_synth && id == s_self_synth))
        return XR_REACT_BAD;
    p = find(id, 0);
    if (!p || !member_ok(p))
        return XR_REACT_UNKNOWN;
    vote_tag(b, tag);
    if (memcmp(tag, b + 40, 8))
        return XR_REACT_BADTAG;
    if (!relay_src_ok(src, p) && !((src.ip == lan_of(p) && local_ok(p)) || (p->pub && src.ip == p->pub) ||
          (p->pub && p->pub == s_self_pub && xr_is_private(src.ip))))
        return XR_REACT_WRONGSRC;
    seq = (uint32_t)b[10] << 24 | (uint32_t)b[11] << 16 | (uint32_t)b[12] << 8 | b[13];
    for (i = 0; i < MAX_PEERS; i++)
        if (s_vote_rx[i].id == id) {
            rx = &s_vote_rx[i];
            break;
        } else if (!s_vote_rx[i].id && !rx)
            rx = &s_vote_rx[i];
    if (!rx)
        return XR_REACT_LIMIT;
    if (rx->id != id) {
        memset(rx, 0, sizeof *rx);
        rx->id = id;
    }
    if (rx->last_seq && seq <= rx->last_seq)
        return XR_REACT_DUP;     /* the same packet from the title's other sockets */
    if (now / 1000u != rx->window) {
        rx->window = now / 1000u;
        rx->n = 0;
    }
    if (rx->n >= VOTE_PER_SEC)
        return XR_REACT_LIMIT;
    rx->n++;
    rx->last_seq = seq;
    if (s_vote_q_n < VOTE_QUEUE) {
        vote_item_t *it = &s_vote_q[(s_vote_q_head + s_vote_q_n) % VOTE_QUEUE];
        it->id = id;
        memcpy(it->data, b + 14, XR_VOTE_DATA);
        s_vote_q_n++;
    }
    return XR_REACT_OK;
}

int xr_vote_get(uint32_t *title_ip, uint8_t data[XR_VOTE_DATA])
{
    vote_item_t *it;
    if (!s_vote_q_n)
        return 0;
    it = &s_vote_q[s_vote_q_head];
    if (title_ip) *title_ip = it->id;
    memcpy(data, it->data, XR_VOTE_DATA);
    s_vote_q_head = (s_vote_q_head + 1) % VOTE_QUEUE;
    s_vote_q_n--;
    return 1;
}

/* ── the relay fallback (BO-381), see xnet_route.h ─────────────────── */

void xr_relay_set(uint32_t ip, uint16_t port)
{
    s_relay.ip = port ? ip : 0;
    s_relay.port = ip ? port : 0;
}

xr_ep xr_relay_ep(void) { return s_relay; }
uint16_t xr_relay_port_seen(void) { return s_relay_port_seen; }
void xr_relay_game(int ident) { s_relay_game = ident; }
void xr_relay_off(int off) { s_relay_off = off; }
void xr_relay_trust(uint32_t id) { s_relay_trust = id; }

int xr_is_relay_src(xr_ep src)
{
    return s_relay.port && src.ip == s_relay.ip && src.port == s_relay.port;
}

int xr_relay_want(uint32_t title_ip, uint16_t title_port, uint64_t now)
{
    const peer_t *p = find(title_ip, 0);
    const learned_t *l;
    if (!p || !p->member || !s_relay.port || s_relay_off || !s_has_secret || !s_relay_game)
        return 0;
    l = learned_for(title_ip, title_port);
    if (l && !l->relay)
        return 0;   /* a direct path: nothing to fall back from */
    if (p->relay_heard)
        return 1;   /* it reached us through the relay: answer in kind at once */
    return p->member_at && now - p->member_at >= XR_RELAY_AFTER_MS;
}

static void relay_tag(const uint8_t *b, uint8_t *tag)
{
    uint64_t t = s_has_secret ? xr_siphash(s_secret, b, 24) : 0;
    int i;
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(t >> (8 * i));
}

int xr_relay_wrap(uint8_t *out, int cap, uint32_t to, uint16_t from_tport, uint16_t to_tport,
                  const uint8_t *payload, int n)
{
    uint32_t id = s_self_synth ? s_self_synth : s_self_id;
    if (!s_relay.port || s_relay_off || !s_has_secret || !s_relay_game || n < 0 || n > XR_RELAY_PAYLOAD ||
        cap < XR_RELAY_HDR + n)
        return 0;
    memset(out, 0, XR_RELAY_HDR);
    memcpy(out, "TKRL", 4);
    out[4] = 1;
    out[5] = XR_RELAY_DATA;
    out[6] = (uint8_t)((uint32_t)s_relay_game >> 24);
    out[7] = (uint8_t)((uint32_t)s_relay_game >> 16);
    out[8] = (uint8_t)((uint32_t)s_relay_game >> 8);
    out[9] = (uint8_t)s_relay_game;
    memcpy(out + 10, &id, 4);
    memcpy(out + 14, &to, 4);
    out[18] = (uint8_t)(from_tport >> 8); out[19] = (uint8_t)from_tport;
    out[20] = (uint8_t)(to_tport >> 8);   out[21] = (uint8_t)to_tport;
    relay_tag(out, out + 24);
    if (n)
        memcpy(out + XR_RELAY_HDR, payload, (size_t)n);
    return XR_RELAY_HDR + n;
}

int xr_is_relay_pkt(const uint8_t *b, int n)
{
    return n >= XR_RELAY_HDR && !memcmp(b, "TKRL", 4) && b[4] == 1;
}

int xr_relay_unwrap(const uint8_t *b, int n, uint32_t *from, uint16_t *from_tport,
                    uint16_t *to_tport, int *kind, int *payload_off)
{
    uint32_t f, t, me = s_self_synth ? s_self_synth : s_self_id;
    int ident;
    uint8_t tag[8];
    const peer_t *p;
    if (!xr_is_relay_pkt(b, n) || n > XR_RELAY_HDR + XR_RELAY_PAYLOAD)
        return XR_RELAY_BAD;
    if (b[5] != XR_RELAY_DATA && b[5] != XR_RELAY_ANSWER)
        return XR_RELAY_BAD;
    ident = (int)((uint32_t)b[6] << 24 | (uint32_t)b[7] << 16 | (uint32_t)b[8] << 8 | b[9]);
    memcpy(&f, b + 10, 4);
    memcpy(&t, b + 14, 4);
    if (from) *from = f;
    if (from_tport) *from_tport = (uint16_t)(b[18] << 8 | b[19]);
    if (to_tport) *to_tport = (uint16_t)(b[20] << 8 | b[21]);
    if (kind) *kind = b[5];
    if (payload_off) *payload_off = XR_RELAY_HDR;
    if (!s_has_secret || !s_relay_game || ident != s_relay_game)
        return XR_RELAY_UNKNOWN;
    if (!f || f == me)
        return XR_RELAY_BAD;
    p = find(f, 0);
    if (!p || !member_ok(p))
        return XR_RELAY_UNKNOWN;
    relay_tag(b, tag);
    if (memcmp(tag, b + 24, 8))
        return XR_RELAY_BADTAG;
    if (t != me)
        return XR_RELAY_NOTME;
    return XR_RELAY_OK;
}

int xr_relay_hello_seen(uint32_t from, uint16_t from_tport, uint16_t ltport, const uint8_t *b, int n,
                        uint64_t now)
{
    uint32_t id = 0;
    int r;
    if (!xr_is_hello(b, n))
        return XR_HELLO_BAD;
    memcpy(&id, b + 6, 4);
    if (id != from || (uint16_t)(b[12] << 8 | b[13]) != from_tport)
        return XR_HELLO_BAD;   /* the wrapper and the hello disagree */
    r = hello_seen_impl(s_relay, ltport, b, n, &id, now, 1);
    return r;
}

void xr_relay_answer(uint32_t from, uint16_t tport, xr_ep ep)
{
    peer_t *p = find(from, 0);
    int i, slot = -1;
    if (!p || !ep.ip || !ep.port)
        return;
    /* only a public address or one on our own network, never a third party
     * the relay could not have seen (and never ourselves) */
    if (xr_is_private(ep.ip) && !(s_self_pub && p->pub == s_self_pub))
        return;
    for (i = 0; i < 4; i++) {
        if (p->disc[i].ep.port && p->disc[i].tport == tport) {
            slot = i;
            break;
        }
        if (!p->disc[i].ep.port && slot < 0)
            slot = i;
    }
    if (slot < 0)
        slot = 0;
    p->disc[slot].tport = tport;
    p->disc[slot].ep = ep;
}

/* xnet_route test: see CMakeLists.txt. */
#include <stdio.h>
#include <string.h>

#include "xnet_route.h"

/* BO-345: our receiving socket's title port. Built against a toolkit
 * without it (int037), the old calls stand in, so the new tests show what
 * the old code does. */
#ifndef XR_HAS_LAN_LOBBY_AT
#define xr_lan_lobby_at(lobby, lan) ((void)(lan), (lobby))   /* before BO-349: always the lobby as given */
#endif
#ifndef XR_HAS_PICK_LAN
enum { XR_LAN_NONE = 0, XR_LAN_PRIVATE = 1, XR_LAN_OTHER = 2 };
static uint32_t xr_pick_lan(const uint32_t *a, int n, int *why) { (void)a; (void)n; if (why) *why = XR_LAN_NONE; return 0; }   /* before BO-349: none */
#endif
#ifndef XR_HAS_LAN_DENIED
#define xr_lan_denied(id, to) ((void)(id), (void)(to), 0)   /* before BO-349: the LAN path stays */
#endif
#ifndef XR_HAS_SOURCE_ON
#define xr_source_on(src, lt, id, tp) ((void)(lt), xr_source(src, id, tp))
#define xr_hello_seen_on(src, lt, b, n, id, now) ((void)(lt), xr_hello_seen(src, b, n, id, now))
#endif

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t ip(int a, int b, int c, int d)
{
    uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    uint32_t r;
    memcpy(&r, x, 4);
    return r;
}

static xr_ep ep(uint32_t i, uint16_t p) { xr_ep e; e.ip = i; e.port = p; return e; }

/* a lobby body as the server sends it */
static void lobby(const char *body) { xr_lobby_body(body, (int)strlen(body) + 1); }

static const uint32_t HOME = 0, ME = 1;
#define PUB_HOME ip(77, 234, 95, 82)
#define PUB_AWAY ip(85, 66, 10, 7)

static void test_choice(void)
{
    xr_ep e;
    const char *why;
    int ch;
    (void)HOME; (void)ME;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    /* unknown address: the caller keeps the old behaviour */
    CHECK(!xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch));
    /* our own record, then a peer at home and one away (+who and +mgm) */
    lobby("I=1\nN=Host\nA=77.234.95.82\nLA=192.168.0.108\nMA=$x\n");
    CHECK(xr_self_public() == PUB_HOME);
    lobby("IDENT=3\nNAME=Host\nOPPO0=Host\nADDR0=77.234.95.82\nLADDR0=192.168.0.108\n"
          "OPPO1=Home\nADDR1=77.234.95.82\nLADDR1=192.168.0.192\n"
          "OPPO2=Away\nADDR2=85.66.10.7\nLADDR2=192.168.1.20\n");
    xr_offset(ip(192, 168, 0, 192), 30000);
    xr_offset(ip(192, 168, 1, 20), 30100);
    /* same public address: the local one */
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch));
    CHECK(e.ip == ip(192, 168, 0, 192) && e.port == 31000 && ch == 1);
    CHECK(strstr(why, "same network") != NULL);
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch) && ch == 0);   /* logged once */
    /* another port of the same player is its own record: no flip-flop between them */
    CHECK(xr_route(ip(192, 168, 0, 192), 6000, &e, &why, &ch) && ch == 1);
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch) && ch == 0);
    CHECK(xr_route(ip(192, 168, 0, 192), 6000, &e, &why, &ch) && ch == 0);
    /* another public address: that one, with the peer's offset */
    CHECK(xr_route(ip(192, 168, 1, 20), 1000, &e, &why, &ch));
    CHECK(e.ip == PUB_AWAY && e.port == 31100 && strstr(why, "another network") != NULL);
    /* the title's other UDP ports move too (6000); 10000 and up keep their number */
    CHECK(xr_route(ip(192, 168, 1, 20), 6000, &e, &why, &ch) && e.ip == PUB_AWAY && e.port == 36100);
    CHECK(xr_route(ip(192, 168, 1, 20), 40000, &e, &why, &ch) && e.port == 40000);
    /* where datagrams came from, without hellos: by address and offset */
    {
        uint32_t id; uint16_t tp;
        CHECK(xr_source(ep(PUB_AWAY, 31100), &id, &tp) && id == ip(192, 168, 1, 20) && tp == 1000);
        CHECK(xr_source(ep(ip(192, 168, 0, 192), 31000), &id, &tp) && id == ip(192, 168, 0, 192) && tp == 1000);
        CHECK(!xr_source(ep(ip(192, 168, 0, 1), 31000), &id, &tp));
        CHECK(xr_source(ep(PUB_AWAY, 36100), &id, &tp) && id == ip(192, 168, 1, 20) && tp == 6000);
    }
}

static void test_force(void)
{
    xr_ep e;
    const char *why;
    int ch;
    xr_reset();
    xr_self(ip(10, 77, 0, 1), 30000, 1);
    lobby("A=77.234.95.82\nLA=10.77.0.1\n");
    lobby("A=77.234.95.82\nLA=10.77.0.2\n");
    xr_offset(ip(10, 77, 0, 2), 30200);
    CHECK(xr_route(ip(10, 77, 0, 2), 1000, &e, &why, &ch));
    CHECK(e.ip == PUB_HOME && e.port == 31200 && strstr(why, "forced") != NULL);
    /* no public address known: the local one, said so */
    lobby("LA=10.77.0.3\n");
    xr_offset(ip(10, 77, 0, 3), 30300);
    CHECK(xr_route(ip(10, 77, 0, 3), 1000, &e, &why, &ch) && e.ip == ip(10, 77, 0, 3));
}

static void test_unknown_public(void)
{
    xr_ep e;
    const char *why;
    int ch;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    xr_offset(ip(192, 168, 0, 50), 30000);         /* XUID seen, lobby not yet */
    CHECK(xr_route(ip(192, 168, 0, 50), 1000, &e, &why, &ch));
    CHECK(e.ip == ip(192, 168, 0, 50) && e.port == 31000 && strstr(why, "has not told") != NULL);
    /* a peer's public address before ours */
    lobby("A=85.66.10.7\nLA=192.168.0.50\n");
    CHECK(xr_route(ip(192, 168, 0, 50), 1000, &e, &why, &ch) && e.ip == PUB_AWAY && ch == 1);
    /* a loopback lobby: everyone 127.0.0.1, so the local address */
    xr_reset();
    xr_self(ip(127, 0, 0, 2), 30000, 0);
    lobby("A=127.0.0.1\nLA=127.0.0.2\n");
    lobby("ADDR0=127.0.0.1\nLADDR0=127.0.0.2\nADDR1=127.0.0.1\nLADDR1=127.0.0.3\n");
    xr_offset(ip(127, 0, 0, 3), 30000);
    CHECK(xr_route(ip(127, 0, 0, 3), 1000, &e, &why, &ch) && e.ip == ip(127, 0, 0, 3) && e.port == 31000);
}

static void test_hello(void)
{
    uint8_t h[XR_HELLO_LEN];
    xr_ep e;
    const char *why;
    int ch;
    uint32_t id;
    uint16_t tp;
    /* the other copy makes a hello */
    xr_reset();
    xr_self(ip(192, 168, 0, 192), 30100, 1);
    CHECK(xr_hello_make(h, 1000) == XR_HELLO_LEN && xr_is_hello(h, XR_HELLO_LEN));
    CHECK(!xr_is_hello(h, XR_HELLO_LEN - 1));
    /* this copy: forced public, the router shows itself as the source */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 1);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    lobby("A=77.234.95.82\nLA=192.168.0.192\n");
    /* a hello straight over the local network does not count when forced */
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 192), 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_OLD);
    CHECK(id == ip(192, 168, 0, 192));
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch) && e.ip == PUB_HOME && e.port == 31100);
    /* through the router (NAT loopback: 192.168.0.1, a new port) */
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 1), 45123), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 1), 45123), h, XR_HELLO_LEN, &id, 1100) == XR_HELLO_OLD);
    /* BO-345: that endpoint is only the reply side of its flow, so we keep
     * sending to its public address; its datagrams from there are its own */
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch));
    CHECK(e.ip == PUB_HOME && e.port == 31100 && strstr(why, "forced") != NULL);
    CHECK(xr_source(ep(ip(192, 168, 0, 1), 45123), &id, &tp) && id == ip(192, 168, 0, 192) && tp == 1000);
    /* the router moved it: the new source is its own too; still the public address */
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 1), 45200), h, XR_HELLO_LEN, &id, 1200) == XR_HELLO_NEW);
    CHECK(xr_source(ep(ip(192, 168, 0, 1), 45200), &id, &tp) && id == ip(192, 168, 0, 192) && tp == 1000);
    CHECK(xr_route(ip(192, 168, 0, 192), 1000, &e, &why, &ch) && e.ip == PUB_HOME && e.port == 31100);
    /* a hello from our own address is ignored */
    xr_reset();
    xr_self(ip(192, 168, 0, 192), 30100, 0);
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 192), 31100), h, XR_HELLO_LEN, &id, 1300) == XR_HELLO_BAD);
}

static void test_hello_due(void)
{
    uint8_t h[XR_HELLO_LEN];
    uint32_t id;
    xr_reset();
    xr_self(ip(192, 168, 0, 192), 30000, 0);
    xr_hello_make(h, 1000);
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(!xr_hello_due(ip(192, 168, 0, 192), 1000, 1000));        /* not a peer */
    xr_offset(ip(192, 168, 0, 192), 30000);
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 1000, 1000));
    CHECK(!xr_hello_due(ip(192, 168, 0, 192), 1000, 1200));
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 1000, 1500));
    /* once its own hello came: one answer at once (BO-343), then only now and then */
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 192), 31000), h, XR_HELLO_LEN, &id, 1500) == XR_HELLO_NEW);
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 1000, 1600));
    CHECK(!xr_hello_due(ip(192, 168, 0, 192), 1000, 2500));
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 1000, 16600));
    /* the title's other socket (6000) has its own hellos, fast until its own came */
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 6000, 16600));
    CHECK(!xr_hello_due(ip(192, 168, 0, 192), 6000, 16800));
    CHECK(xr_hello_due(ip(192, 168, 0, 192), 6000, 17200));
}

static void test_parse(void)
{
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    /* not addresses, partial ones, the server's own ADDR: nothing */
    lobby("ADDR=79.172.222.137\nPORT=21810\n");
    lobby("A=77.234.95\nLA=192.168.0.9\n");
    lobby("A=77.234.95.82.1\nLA=192.168.0.9\n");
    lobby("A=300.1.1.1\nLA=192.168.0.9\n");
    CHECK(xr_self_public() == 0);
    {
        xr_ep e; const char *why; int ch;
        CHECK(!xr_route(ip(192, 168, 0, 9), 1000, &e, &why, &ch));
    }
    /* blanks as separators, as the client's requests use */
    lobby("A=77.234.95.82 LA=192.168.0.108");
    CHECK(xr_self_public() == PUB_HOME);
}

/* BO-334 #4: hellos from anyone */
static void test_hello_trust(void)
{
    uint8_t h[XR_HELLO_LEN];
    uint32_t id;
    int n0;
    /* a copy at 192.168.1.20 behind 85.66.10.7 makes a hello */
    xr_reset();
    xr_self(ip(192, 168, 1, 20), 30100, 0);
    xr_hello_make(h, 1000);
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* not a player the lobby told us about: refused, no peer made */
    n0 = xr_peer_count();
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_UNKNOWN);
    CHECK(xr_peer_count() == n0);
    lobby("A=85.66.10.7\nLA=192.168.1.20\n");
    /* from a third party, or from a private address while it is on another network */
    CHECK(xr_hello_seen(ep(ip(1, 2, 3, 4), 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_WRONGSRC);
    CHECK(xr_hello_seen(ep(ip(192, 168, 0, 1), 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_WRONGSRC);
    /* from its public address, the port changed by its router: yes */
    CHECK(xr_hello_seen(ep(PUB_AWAY, 52000), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    {
        xr_ep e; const char *why; int ch;
        CHECK(xr_route(ip(192, 168, 1, 20), 1000, &e, &why, &ch) == 1 && e.ip == PUB_AWAY && e.port == 52000);
    }
    /* a full table never evicts a real peer */
    {
        int i;
        for (i = 0; i < 40; i++) {
            uint8_t g[XR_HELLO_LEN];
            memcpy(g, h, sizeof g);
            g[9] = (uint8_t)(100 + i);   /* random ids */
            xr_hello_seen(ep(ip(1, 2, 3, 4), 31100), g, XR_HELLO_LEN, &id, 1000);
        }
        CHECK(xr_peer_count() == n0 + 1);
    }
}

/* BO-334 #4: the game's secret signs the hellos */
static void test_hello_secret(void)
{
    uint8_t h[XR_HELLO_LEN];
    uint32_t id;
    xr_reset();
    xr_self(ip(192, 168, 1, 20), 30100, 0);
    lobby("IDENT=4\nTKSECRET=00112233445566778899aabbccddeeff\n");
    CHECK(xr_has_secret() && xr_secret_seq() == 1);
    lobby("TKSECRET=00112233445566778899aabbccddeeff\n");      /* the same again: not new */
    CHECK(xr_secret_seq() == 1);
    xr_hello_make(h, 1000);
    /* the same game: yes */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=85.66.10.7\nLA=192.168.1.20\n");
    lobby("TKSECRET=00112233445566778899aabbccddeeff\n");
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    /* another game's secret, or a forged tag: no */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=85.66.10.7\nLA=192.168.1.20\n");
    lobby("TKSECRET=ffeeddccbbaa99887766554433221100\n");
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_BADTAG);
    h[20] ^= 1;
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31100), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_BADTAG);
    /* not 32 hex digits: ignored */
    xr_reset();
    lobby("TKSECRET=0011\n");
    lobby("TKSECRET=zz112233445566778899aabbccddeeff\n");
    CHECK(!xr_has_secret());
    /* SipHash-2-4 reference vectors (key 00..0f, message 00.. of length 0 and 15) */
    {
        uint8_t k[16], m[15];
        int i;
        for (i = 0; i < 16; i++) k[i] = (uint8_t)i;
        for (i = 0; i < 15; i++) m[i] = (uint8_t)i;
        CHECK(xr_siphash(k, m, 0) == 0x726fdb47dd0e0e31ull);
        CHECK(xr_siphash(k, m, 15) == 0xa129ca6149be45e5ull);
    }
}

/* BO-334 #6 (a public address only the XUID claims), #10 (both paths on one
 * public address), #14 (strangers), the learned path kept while it is fresh */
static void test_paths(void)
{
    uint8_t h[XR_HELLO_LEN];
    xr_ep e, c[2];
    const char *why;
    int ch;
    uint32_t id;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* #6: a XUID with someone else's public address, the lobby silent */
    xr_offset(ip(8, 8, 8, 8), 30000);
    CHECK(xr_route(ip(8, 8, 8, 8), 1000, &e, &why, &ch) == 2 && strstr(why, "refused") != NULL);
    /* the lobby saw it there: a machine on the internet itself */
    lobby("A=8.8.8.8\nLA=8.8.8.8\n");
    CHECK(xr_route(ip(8, 8, 8, 8), 1000, &e, &why, &ch) == 1 && e.ip == ip(8, 8, 8, 8));
    /* #10: same public address, so the local path, and hellos to the router's too */
    lobby("A=77.234.95.82\nLA=10.0.0.5\n");
    xr_offset(ip(10, 0, 0, 5), 30200);
    CHECK(xr_candidates(ip(10, 0, 0, 5), 1000, c) == 2);
    CHECK(c[0].ip == ip(10, 0, 0, 5) && c[0].port == 31200 && c[1].ip == PUB_HOME && c[1].port == 31200);
    /* the router's path answers first (two homes behind one address) */
    xr_reset();
    xr_self(ip(10, 0, 0, 5), 30200, 0);
    xr_hello_make(h, 1000);
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    lobby("A=77.234.95.82\nLA=10.0.0.5\n");
    CHECK(xr_hello_seen(ep(ip(100, 64, 0, 1), 31200), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    CHECK(xr_candidates(ip(10, 0, 0, 5), 1000, c) == 1 && c[0].ip == ip(100, 64, 0, 1));
    /* the local path answering later does not take over while the first is fresh */
    CHECK(xr_hello_seen(ep(ip(10, 0, 0, 5), 31200), h, XR_HELLO_LEN, &id, 2000) == XR_HELLO_OLD);
    CHECK(xr_route(ip(10, 0, 0, 5), 1000, &e, &why, &ch) == 1 && e.ip == ip(100, 64, 0, 1));
    /* ... but does once that went quiet */
    CHECK(xr_hello_seen(ep(ip(10, 0, 0, 5), 31200), h, XR_HELLO_LEN, &id, 40000) == XR_HELLO_NEW);
    CHECK(xr_route(ip(10, 0, 0, 5), 1000, &e, &why, &ch) == 1 && e.ip == ip(10, 0, 0, 5));
    /* #14: who may send the title datagrams */
    CHECK(xr_known_source(ep(ip(10, 0, 0, 5), 31200)));
    CHECK(xr_known_source(ep(PUB_HOME, 31200)));
    CHECK(!xr_known_source(ep(ip(1, 2, 3, 4), 31200)));
    CHECK(xr_peer_count() >= 1);
}

/* Members of this game behind our own router: NAT loopback gives the router's
 * address and the sender's port; only members, only their title ports. */
static void test_same_router_members(void)
{
    uint32_t id;
    uint16_t tp;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 1);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* the room: two players at home, one away; the game has one of them */
    lobby("A=77.234.95.82\nLA=10.77.2.2\n");
    lobby("A=77.234.95.82\nLA=10.77.2.3\n");
    lobby("A=85.66.10.7\nLA=192.168.1.20\n");
    xr_offset(ip(10, 77, 2, 2), 30300);
    xr_offset(ip(10, 77, 2, 3), 30900);
    xr_offset(ip(192, 168, 1, 20), 30100);
    xr_members_body("IDENT=4\nLADDR0=192.168.0.108\nADDR0=77.234.95.82\nLADDR1=10.77.2.2\nADDR1=77.234.95.82\n"
                    "LADDR2=192.168.1.20\nADDR2=85.66.10.7\n", 200);
    CHECK(xr_is_member(ip(10, 77, 2, 2)) && !xr_is_member(ip(10, 77, 2, 3)) && xr_is_member(ip(192, 168, 1, 20)));
    /* a member at home, through the router, on its race port and its 6000 */
    CHECK(xr_source(ep(ip(192, 168, 0, 1), 31300), &id, &tp) && id == ip(10, 77, 2, 2) && tp == 1000);
    CHECK(xr_source(ep(ip(192, 168, 0, 1), 36300), &id, &tp) && id == ip(10, 77, 2, 2) && tp == 6000);
    /* a non-member at home on its own race port, another port, a member away: no */
    CHECK(!xr_source(ep(ip(192, 168, 0, 1), 31900), &id, &tp));
    CHECK(!xr_source(ep(ip(192, 168, 0, 1), 40000), &id, &tp));
    CHECK(!xr_source(ep(ip(192, 168, 0, 1), 31100), &id, &tp));
    /* a stranger on the LAN who is not in the lobby at all */
    CHECK(!xr_source(ep(ip(192, 168, 0, 77), 31000), &id, &tp));
    /* a new member record replaces the list */
    xr_members_body("LADDR0=192.168.0.108\nLADDR1=10.77.2.3\n", 60);
    CHECK(!xr_is_member(ip(10, 77, 2, 2)) && xr_is_member(ip(10, 77, 2, 3)));
    CHECK(xr_source(ep(ip(192, 168, 0, 1), 31900), &id, &tp) && id == ip(10, 77, 2, 3));
    CHECK(!xr_source(ep(ip(192, 168, 0, 1), 31300), &id, &tp));
    /* a body without members changes nothing */
    xr_members_body("IDENT=4\nNAME=Host1\n", 30);
    CHECK(xr_is_member(ip(10, 77, 2, 3)));
    /* the secret's prefix for the logs */
    {
        char px[5];
        xr_secret_prefix(px);
        CHECK(!strcmp(px, "none"));
        lobby("TKSECRET=abcd2233445566778899aabbccddeeff\n");
        xr_secret_prefix(px);
        CHECK(!strcmp(px, "abcd"));
    }
}

static void test_misc(void)
{
    int i, ok = 0;
    /* private ranges */
    CHECK(xr_is_private(ip(10, 1, 2, 3)) && xr_is_private(ip(172, 16, 0, 1)) && xr_is_private(ip(172, 31, 9, 9)));
    CHECK(xr_is_private(ip(192, 168, 7, 7)) && xr_is_private(ip(127, 0, 0, 1)) && xr_is_private(ip(100, 64, 0, 1)));
    CHECK(xr_is_private(ip(100, 127, 255, 1)) && xr_is_private(ip(169, 254, 1, 1)));
    CHECK(!xr_is_private(ip(172, 32, 0, 1)) && !xr_is_private(ip(100, 128, 0, 1)) && !xr_is_private(PUB_HOME));
    /* #15: UPnP's internal client */
    CHECK(xr_upnp_client(ip(192, 168, 0, 108), ip(192, 168, 0, 108)) == ip(192, 168, 0, 108));
    CHECK(xr_upnp_client(ip(192, 168, 0, 108), ip(192, 168, 0, 50)) == ip(192, 168, 0, 108));
    CHECK(xr_upnp_client(ip(10, 8, 0, 2), ip(192, 168, 0, 50)) == ip(192, 168, 0, 50));     /* a VPN */
    CHECK(xr_upnp_client(ip(10, 77, 1, 2), 0) == ip(10, 77, 1, 2));
    /* #17: the log budget */
    for (i = 0; i < 15; i++)
        ok += xr_log_ok(100000 + (uint64_t)i);
    CHECK(ok == 10);
    CHECK(xr_log_ok(110000));
}

/* a hello as the copy with title address id (offset, title port) sends it
 * before the game has a secret */
static void mk_hello(uint8_t *b, uint32_t id, uint16_t off, uint16_t tport)
{
    xr_self(id, off, 0);
    xr_hello_make(b, tport);
}

/* BO-337 H3: the tables give up old peers, never the game's players */
static void test_eviction(void)
{
    xr_ep e;
    const char *why;
    int ch, i;
    uint32_t id;
    uint16_t tp;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* the game: two players away */
    xr_members_body("IDENT=7\nLADDR0=192.168.0.108\nADDR0=77.234.95.82\nLADDR1=192.168.5.1\nADDR1=85.66.10.7\n"
                    "LADDR2=192.168.5.2\nADDR2=85.66.10.8\n", 200);
    lobby("LADDR1=192.168.5.1\nADDR1=85.66.10.7\nLADDR2=192.168.5.2\nADDR2=85.66.10.8\n");
    CHECK(xr_member_count() == 2);
    /* an evening of other players: far more than the table holds */
    for (i = 0; i < 200; i++) {
        char b[80];
        snprintf(b, sizeof b, "A=85.66.%d.%d\nLA=10.%d.%d.9\n", i / 200, i % 200 + 1, i / 200, i % 200);
        lobby(b);
    }
    CHECK(xr_peer_count() == XR_MAX_PEERS);
    CHECK(xr_is_member(ip(192, 168, 5, 1)) && xr_is_member(ip(192, 168, 5, 2)));
    CHECK(xr_route(ip(192, 168, 5, 1), 1000, &e, &why, &ch) == 1 && e.ip == ip(85, 66, 10, 7));
    CHECK(xr_route(ip(192, 168, 5, 2), 1000, &e, &why, &ch) == 1 && e.ip == ip(85, 66, 10, 8));
    /* the latest ones are there, the first are gone (not slot 0 again and again) */
    CHECK(xr_route(ip(10, 0, 199, 9), 1000, &e, &why, &ch) == 1);
    CHECK(xr_route(ip(10, 0, 198, 9), 1000, &e, &why, &ch) == 1);
    CHECK(!xr_route(ip(10, 0, 0, 9), 1000, &e, &why, &ch));
    /* a peer in use is the last to go */
    CHECK(xr_route(ip(10, 0, 180, 9), 1000, &e, &why, &ch) == 1);
    for (i = 0; i < 29; i++) {
        char b[80];
        snprintf(b, sizeof b, "A=86.66.0.%d\nLA=10.9.%d.9\n", i + 1, i);
        lobby(b);
    }
    CHECK(xr_route(ip(10, 0, 180, 9), 1000, &e, &why, &ch) == 1);
    CHECK(!xr_route(ip(10, 0, 181, 9), 1000, &e, &why, &ch));
    /* the members' datagrams still reach the title */
    CHECK(xr_source(ep(ip(85, 66, 10, 7), 31000), &id, &tp) && id == ip(192, 168, 5, 1));
    /* an unknown hello never takes a slot */
    {
        uint8_t h[XR_HELLO_LEN];
        int n0 = xr_peer_count();
        mk_hello(h, ip(10, 200, 0, 1), 30000, 1000);
        xr_self(ip(192, 168, 0, 108), 30000, 0);
        CHECK(xr_hello_seen(ep(ip(5, 5, 5, 5), 31000), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_UNKNOWN);
        CHECK(xr_peer_count() == n0 && xr_is_member(ip(192, 168, 5, 1)));
    }
}

/* BO-337 H3: a full learned table gives up a quiet path or one of a peer
 * that left the game, never a fresh one of a member */
static void test_learned_full(void)
{
    uint8_t h[XR_HELLO_LEN];
    char body[2048];
    int i, o = 0;
    uint32_t id;
    xr_ep e;
    const char *why;
    int ch;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* 16 players away, each on 4 title ports: 64 paths */
    o += snprintf(body + o, sizeof body - (size_t)o, "IDENT=9\nLADDR0=192.168.0.108\nADDR0=77.234.95.82\n");
    for (i = 1; i < 16; i++)
        o += snprintf(body + o, sizeof body - (size_t)o, "LADDR%d=192.168.9.%d\nADDR%d=85.66.9.%d\n", i, i, i, i);
    lobby(body);
    xr_members_body(body, o + 1);
    lobby("A=85.66.9.99\nLA=192.168.9.99\n");   /* one more, not in the game */
    for (i = 1; i < 16; i++) {
        uint16_t ports[4] = { 1000, 3074, 6000, 7000 };
        int k;
        for (k = 0; k < 4; k++) {
            mk_hello(h, ip(192, 168, 9, i), 30000, ports[k]);
            xr_self(ip(192, 168, 0, 108), 30000, 0);
            CHECK(xr_hello_seen(ep(ip(85, 66, 9, i), (uint16_t)(40000 + i * 4 + k)), h, XR_HELLO_LEN, &id, 1000) ==
                  XR_HELLO_NEW);
        }
    }
    CHECK(xr_learned_count() == 60);
    for (i = 0; i < 4; i++) {   /* the last four slots */
        mk_hello(h, ip(192, 168, 9, 15), 30000, (uint16_t)(8000 + i));
        xr_self(ip(192, 168, 0, 108), 30000, 0);
        xr_hello_seen(ep(ip(85, 66, 9, 15), (uint16_t)(41000 + i)), h, XR_HELLO_LEN, &id, 1000);
    }
    CHECK(xr_learned_count() == XR_MAX_LEARNED);
    /* full, everything fresh: a new path waits */
    mk_hello(h, ip(192, 168, 9, 1), 30000, 9000);
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_hello_seen(ep(ip(85, 66, 9, 1), 42000), h, XR_HELLO_LEN, &id, 2000) == XR_HELLO_OLD);
    /* player 14 leaves the game: its paths give way at once */
    o = 0;
    o += snprintf(body + o, sizeof body - (size_t)o, "IDENT=9\nLADDR0=192.168.0.108\n");
    for (i = 1; i < 16; i++)
        if (i != 14)
            o += snprintf(body + o, sizeof body - (size_t)o, "LADDR%d=192.168.9.%d\n", i, i);
    xr_members_body(body, o + 1);
    CHECK(xr_hello_seen(ep(ip(85, 66, 9, 1), 42000), h, XR_HELLO_LEN, &id, 2000) == XR_HELLO_NEW);
    CHECK(xr_route(ip(192, 168, 9, 1), 9000, &e, &why, &ch) == 1 && e.port == 42000);
    /* full of members' fresh paths again; 30 s later the quiet ones give way */
    for (i = 0; i < 3; i++) {
        mk_hello(h, ip(192, 168, 9, 2), 30000, (uint16_t)(9000 + i));
        xr_self(ip(192, 168, 0, 108), 30000, 0);
        xr_hello_seen(ep(ip(85, 66, 9, 2), (uint16_t)(43000 + i)), h, XR_HELLO_LEN, &id, 2000);
    }
    CHECK(xr_learned_count() == XR_MAX_LEARNED);
    mk_hello(h, ip(192, 168, 9, 3), 30000, 9000);
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_hello_seen(ep(ip(85, 66, 9, 3), 42001), h, XR_HELLO_LEN, &id, 2000) == XR_HELLO_OLD);
    CHECK(xr_hello_seen(ep(ip(85, 66, 9, 3), 42001), h, XR_HELLO_LEN, &id, 40000) == XR_HELLO_NEW);
}

/* BO-337 M3: the game is over (the lobby's empty record, glea, gdel) */
static void test_game_reset(void)
{
    uint8_t h[XR_HELLO_LEN];
    uint32_t id;
    uint16_t tp;
    int seq;
    xr_reset();
    xr_self(ip(192, 168, 1, 20), 30000, 0);
    lobby("TKSECRET=00112233445566778899aabbccddeeff\n");
    xr_hello_make(h, 1000);
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    lobby("IDENT=5\nLADDR0=192.168.0.108\nADDR0=77.234.95.82\nLADDR1=192.168.1.20\nADDR1=85.66.10.7\n"
          "TKSECRET=00112233445566778899aabbccddeeff\n");
    xr_members_body("IDENT=5\nLADDR0=192.168.0.108\nLADDR1=192.168.1.20\n", 60);
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31000), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    CHECK(xr_source(ep(PUB_AWAY, 31000), &id, &tp) && id == ip(192, 168, 1, 20));
    CHECK(xr_learned_count() == 1 && xr_member_count() == 1 && xr_has_secret());
    seq = xr_secret_seq();
    xr_game_reset();
    CHECK(!xr_has_secret() && xr_secret_seq() == seq + 1);
    CHECK(xr_member_count() == 0 && !xr_is_member(ip(192, 168, 1, 20)));
    CHECK(xr_learned_count() == 1);   /* kept, but not for a non-member: */
    /* the old game's player no longer reaches the title, its hellos no longer count */
    CHECK(!xr_source(ep(PUB_AWAY, 31000), &id, &tp));
    CHECK(xr_hello_seen(ep(PUB_AWAY, 31000), h, XR_HELLO_LEN, &id, 2000) == XR_HELLO_UNKNOWN);
    /* the peer is still known (the next game's records fill it in again) */
    CHECK(xr_peer_count() == 1);
    xr_game_reset();   /* again: nothing more to clear */
    CHECK(xr_secret_seq() == seq + 1);
}

/* BO-337 M4: datagrams reach the title only from the current game's players,
 * and a peer's local address counts only on our own network */
static void test_members_only(void)
{
    uint32_t id;
    uint16_t tp;
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    /* a player away with an address on our /24, one away elsewhere, one at home */
    lobby("A=85.66.10.7\nLA=192.168.0.50\n");
    lobby("A=85.66.10.8\nLA=192.168.7.50\n");
    lobby("A=77.234.95.82\nLA=192.168.0.60\n");
    /* before any member record: the lobby's players (as before) */
    CHECK(xr_source(ep(ip(85, 66, 10, 8), 31000), &id, &tp) && id == ip(192, 168, 7, 50));
    /* the local address of a player on another network and another /24: not it */
    CHECK(!xr_source(ep(ip(192, 168, 7, 50), 31000), &id, &tp));
    /* on our /24 (an old record: a LAN through a tunnel) or at home: yes */
    CHECK(xr_source(ep(ip(192, 168, 0, 50), 31000), &id, &tp) && id == ip(192, 168, 0, 50));
    CHECK(xr_source(ep(ip(192, 168, 0, 60), 31000), &id, &tp) && id == ip(192, 168, 0, 60));
    /* the game has two of them: the third is out */
    xr_members_body("IDENT=8\nLADDR0=192.168.0.108\nLADDR1=192.168.0.50\nLADDR2=192.168.0.60\n", 80);
    CHECK(!xr_source(ep(ip(85, 66, 10, 8), 31000), &id, &tp));
    CHECK(xr_source(ep(ip(85, 66, 10, 7), 31000), &id, &tp) && id == ip(192, 168, 0, 50));
    CHECK(xr_source(ep(ip(192, 168, 0, 60), 31000), &id, &tp) && id == ip(192, 168, 0, 60));
}

/* BO-337 H1: two players with the same private address in two homes, and a
 * third at the same address in ours, are three peers (identity spec) */
static void test_synthetic(void)
{
    uint8_t h[XR_HELLO_LEN];
    xr_ep e, c[2];
    const char *why;
    int ch;
    uint32_t id;
    uint16_t tp;
#define S1 ip(10, 64, 0, 1)
#define S2 ip(10, 64, 0, 2)
#define S3 ip(10, 64, 0, 3)
#define REAL ip(192, 168, 1, 100)
#define PUB_A ip(85, 66, 10, 7)
#define PUB_B ip(91, 120, 4, 2)
#define PUB_C ip(77, 234, 95, 82)
    /* the addr reply: our own synthetic address, or none from an old server */
    xr_reset();
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 20) && xr_self_synthetic() == ip(10, 64, 0, 9));
    CHECK(!xr_addr_reply("ADDR=1.2.3.4\n", 14) && xr_self_synthetic() == 0);
    /* player 1's hello carries its synthetic address */
    xr_reset();
    xr_self(REAL, 30000, 0);
    xr_self_synth(S1);
    xr_hello_make(h, 1000);
    /* us: 192.168.1.50 behind PUB_C */
    xr_reset();
    xr_self(ip(192, 168, 1, 50), 30000, 0);
    xr_self_synth(ip(10, 64, 0, 9));
    lobby("A=77.234.95.82\nLA=192.168.1.50\n");   /* our own +who: the real address */
    {
        const char *g = "IDENT=11\nLADDR0=192.168.1.50\nADDR0=77.234.95.82\n"
                        "LADDR1=10.64.0.1\nTKRADDR1=192.168.1.100\nADDR1=85.66.10.7\n"
                        "LADDR2=10.64.0.2\nTKRADDR2=192.168.1.100\nADDR2=91.120.4.2\n";
        lobby(g);
        xr_members_body(g, (int)strlen(g) + 1);
    }
    CHECK(xr_self_public() == PUB_C);
    CHECK(xr_peer_count() == 2 && xr_is_member(S1) && xr_is_member(S2));
    xr_offset(S1, 30000);
    xr_offset(S2, 30000);
    /* each by its own public address */
    CHECK(xr_route(S1, 1000, &e, &why, &ch) == 1 && e.ip == PUB_A && e.port == 31000);
    CHECK(xr_route(S2, 1000, &e, &why, &ch) == 1 && e.ip == PUB_B && e.port == 31000);
    CHECK(xr_candidates(S1, 1000, c) == 1 && c[0].ip == PUB_A);
    /* arriving datagrams map back to the synthetic address */
    CHECK(xr_source(ep(PUB_A, 31000), &id, &tp) && id == S1 && tp == 1000);
    CHECK(xr_source(ep(PUB_B, 31000), &id, &tp) && id == S2 && tp == 1000);
    /* 192.168.1.100 on our own network is neither of them */
    CHECK(!xr_source(ep(REAL, 31000), &id, &tp));
    /* their hellos: by the synthetic address, from their public one */
    CHECK(xr_hello_seen(ep(PUB_A, 52000), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == S1);
    CHECK(xr_hello_seen(ep(REAL, 31000), h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_WRONGSRC);
    CHECK(xr_route(S1, 1000, &e, &why, &ch) == 1 && e.ip == PUB_A && e.port == 52000);
    CHECK(xr_route(S2, 1000, &e, &why, &ch) == 1 && e.ip == PUB_B && e.port == 31000);
    /* a third at 192.168.1.100 in our own home (its +usr record): the LAN
     * path, and its port offset from its XUID (MA, bytes 3 and 2: 0x7DC8 =
     * 32200) before the title looks it up */
    lobby("I=3\nN=Home\nMA=$0300400a7dc81234\nA=77.234.95.82\nLA=10.64.0.3\nTKRLA=192.168.1.100\n");
    CHECK(xr_route(S3, 1000, &e, &why, &ch) == 1 && e.port == 33200);
    xr_members_body("IDENT=11\nLADDR0=192.168.1.50\nLADDR1=10.64.0.1\nTKRADDR1=192.168.1.100\n"
                    "LADDR2=10.64.0.2\nTKRADDR2=192.168.1.100\nLADDR3=10.64.0.3\nTKRADDR3=192.168.1.100\n", 200);
    xr_offset(S3, 30000);   /* the title's lookup wins */
    CHECK(xr_peer_count() == 3);
    CHECK(xr_route(S3, 1000, &e, &why, &ch) == 1 && e.ip == REAL && e.port == 31000 && strstr(why, "same network"));
    CHECK(xr_candidates(S3, 1000, c) == 2 && c[0].ip == REAL && c[1].ip == PUB_C);
    CHECK(xr_source(ep(REAL, 31000), &id, &tp) && id == S3);
    CHECK(xr_source(ep(PUB_B, 31000), &id, &tp) && id == S2);
    /* a hello naming our own synthetic address is not a peer's */
    {
        uint8_t me[XR_HELLO_LEN];
        xr_hello_make(me, 1000);
        CHECK(xr_hello_seen(ep(PUB_A, 31000), me, XR_HELLO_LEN, &id, 1000) == XR_HELLO_BAD);
    }
    /* an old server: no TKR keys, the address is the real one, as before */
    xr_reset();
    xr_self(ip(192, 168, 1, 50), 30000, 0);
    lobby("A=77.234.95.82\nLA=192.168.1.50\n");
    lobby("LADDR1=192.168.1.100\nADDR1=85.66.10.7\n");
    CHECK(xr_route(REAL, 1000, &e, &why, &ch) == 1 && e.ip == PUB_A);
    CHECK(xr_source(ep(PUB_A, 31000), &id, &tp) && id == REAL);
}

/* the hello of the copy at synthetic address s (port offset off) from title
 * port tport, under the game's secret sec (32 hex digits) */
static void mk_synth_hello(uint8_t *b, uint32_t s, uint16_t off, uint16_t tport, const char *sec)
{
    char body[64];
    xr_reset();
    xr_self(ip(192, 168, 0, 200), off, 0);
    xr_self_synth(s);
    snprintf(body, sizeof body, "TKSECRET=%s\n", sec);
    lobby(body);
    xr_hello_make(b, tport);
}

/* us: 192.168.0.108 (offset 30000) at home; X and Y behind one router away
 * (85.66.10.7), X with offset 30900, Y with 30500; a game of the three */
#define SA ip(10, 64, 0, 1)
#define SB ip(10, 64, 0, 2)
#define AWAY ip(85, 66, 10, 7)
static void game_of(uint32_t sx, uint32_t sy, const char *sec)
{
    char g[512];
    snprintf(g, sizeof g,
             "IDENT=3\nLADDR0=192.168.0.108\nADDR0=77.234.95.82\n"
             "LADDR1=%u.%u.%u.%u\nTKRADDR1=192.168.1.10\nADDR1=85.66.10.7\n"
             "LADDR2=%u.%u.%u.%u\nTKRADDR2=192.168.1.11\nADDR2=85.66.10.7\nTKSECRET=%s\n",
             sx & 255, sx >> 8 & 255, sx >> 16 & 255, sx >> 24,
             sy & 255, sy >> 8 & 255, sy >> 16 & 255, sy >> 24, sec);
    lobby(g);
    xr_members_body(g, (int)strlen(g) + 1);
    /* their +usr records: MA carries the synthetic address and the offset */
    snprintf(g, sizeof g, "N=X\nMA=$%02x%02x%02x%02x78b40001\nA=85.66.10.7\nLA=%u.%u.%u.%u\nTKRLA=192.168.1.10\n",
             sx >> 24, sx >> 16 & 255, sx >> 8 & 255, sx & 255, sx & 255, sx >> 8 & 255, sx >> 16 & 255, sx >> 24);
    lobby(g);
    snprintf(g, sizeof g, "N=Y\nMA=$%02x%02x%02x%02x77240002\nA=85.66.10.7\nLA=%u.%u.%u.%u\nTKRLA=192.168.1.11\n",
             sy >> 24, sy >> 16 & 255, sy >> 8 & 255, sy & 255, sy & 255, sy >> 8 & 255, sy >> 16 & 255, sy >> 24);
    lobby(g);
}

/* BO-343: a new lobby session hands the synthetic addresses out again. Two
 * sign-ins where X and Y swap addresses: the first game of the second
 * session reaches X at X's path and gives X's datagrams to X; within one
 * session (game 2 of the same sign-in) the learned paths and the hello
 * schedule stay (BO-337). */
static void test_session_reset(void)
{
    const char *SEC1 = "00112233445566778899aabbccddeeff", *SEC2 = "ffeeddccbbaa99887766554433221100";
    uint8_t hx[XR_HELLO_LEN], hy[XR_HELLO_LEN], hx2[XR_HELLO_LEN], hy2[XR_HELLO_LEN];
    xr_ep e, ex = ep(AWAY, 31900), ey = ep(AWAY, 31500);   /* X's and Y's sockets for title port 1000 */
    const char *why;
    int ch, seq;
    uint32_t id;
    uint16_t tp;
    /* session 1: X = 10.64.0.1, Y = 10.64.0.2; session 2: swapped */
    mk_synth_hello(hx, SA, 30900, 1000, SEC1);
    mk_synth_hello(hy, SB, 30500, 1000, SEC1);
    mk_synth_hello(hx2, SB, 30900, 1000, SEC2);
    mk_synth_hello(hy2, SA, 30500, 1000, SEC2);

    /* session 1 */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 18));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SA, SB, SEC1);
    CHECK(xr_member_count() == 2);
    /* before any hello: by the public address and each one's own offset */
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == AWAY && e.port == 31900);
    CHECK(xr_route(SB, 1000, &e, &why, &ch) == 1 && e.ip == AWAY && e.port == 31500);
    /* their routers move the ports: the hellos teach the real paths */
    CHECK(xr_hello_seen(ep(AWAY, 52001), hx, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SA);
    CHECK(xr_hello_seen(ep(AWAY, 52002), hy, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SB);
    ex = ep(AWAY, 52001);
    ey = ep(AWAY, 52002);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.port == 52001);
    CHECK(xr_hello_due(SA, 1000, 2000) && !xr_hello_due(SA, 1000, 2600));   /* learned: every 15 s */

    /* within the session, game 2 (same players, same addresses): paths and
     * the hello schedule stay, only members and the secret go */
    xr_game_reset();
    CHECK(xr_learned_count() == 2 && xr_peer_count() == 2 && !xr_has_secret());
    game_of(SA, SB, SEC1);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == ex.ip && e.port == ex.port);
    CHECK(xr_route(SB, 1000, &e, &why, &ch) == 1 && e.ip == ey.ip && e.port == ey.port);
    CHECK(xr_source(ex, &id, &tp) && id == SA && tp == 1000);
    CHECK(xr_source(ey, &id, &tp) && id == SB && tp == 1000);
    /* a member of the new game: one hello at once, fast until its own comes
     * (BO-344), then the slow keep-alive again */
    CHECK(xr_hello_due(SA, 1000, 3000) && !xr_hello_due(SA, 1000, 3200) && xr_hello_due(SA, 1000, 3500));
    CHECK(xr_hello_seen(ex, hx, XR_HELLO_LEN, &id, 3500) == XR_HELLO_OLD && id == SA);
    CHECK(xr_hello_due(SA, 1000, 3550));   /* its first hello in this game: one answer */
    CHECK(!xr_hello_due(SA, 1000, 4100) && xr_hello_due(SA, 1000, 18600));

    /* the lobby restarts: session 2 swaps the addresses (X = .2, Y = .1)
     * and gives us another one. Same sockets, same router ports. */
    seq = xr_secret_seq();
    CHECK(xr_addr_reply("TKADDR=10.64.0.3\n", 18));
    CHECK(xr_session_reset() == 4);   /* two peers, two paths */
    CHECK(xr_peer_count() == 0 && xr_learned_count() == 0 && xr_member_count() == 0);
    CHECK(!xr_has_secret() && xr_secret_seq() == seq + 1);
    CHECK(xr_self_public() == PUB_HOME && xr_self_synthetic() == ip(10, 64, 0, 3));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SB, SA, SEC2);
    /* the first game: .2 is X now, by X's offset, not Y's old path */
    CHECK(xr_route(SB, 1000, &e, &why, &ch) == 1 && e.ip == AWAY && e.port == 31900);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == AWAY && e.port == 31500);
    /* no path of the last session steers X's socket to Y (.1) */
    CHECK(xr_learned_count() == 0);
    /* the hello schedule starts over: every 500 ms until their hellos come */
    CHECK(xr_hello_due(SB, 1000, 10000) && xr_hello_due(SB, 1000, 10500));
    CHECK(xr_hello_seen(ex, hx2, XR_HELLO_LEN, &id, 10600) == XR_HELLO_NEW && id == SB);
    /* its path just learned: one hello back at once (ours may have been
     * refused before it knew us), then the 15 s keep-alive */
    CHECK(xr_hello_due(SB, 1000, 10650) && !xr_hello_due(SB, 1000, 11200));
    CHECK(xr_hello_seen(ey, hy2, XR_HELLO_LEN, &id, 10600) == XR_HELLO_NEW && id == SA);
    CHECK(xr_route(SB, 1000, &e, &why, &ch) == 1 && e.ip == ex.ip && e.port == ex.port);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == ey.ip && e.port == ey.port);
    /* X's datagrams are X's (.2), Y's are Y's (.1) */
    CHECK(xr_source(ex, &id, &tp) && id == SB && tp == 1000);
    CHECK(xr_source(ey, &id, &tp) && id == SA && tp == 1000);
    /* a hello of the last session's secret does not count */
    CHECK(xr_hello_seen(ex, hx, XR_HELLO_LEN, &id, 10700) == XR_HELLO_BADTAG);
    /* nothing to forget twice */
    CHECK(xr_session_reset() == 4 && xr_session_reset() == 0);
}

/* BO-343 defence: without a new session of our own (another player's
 * re-sign-in reuses an address), a stale path from a socket that a hello
 * now ties to another member stops steering its datagrams */
static void test_path_claim(void)
{
    const char *SEC1 = "00112233445566778899aabbccddeeff";
    uint8_t hy[XR_HELLO_LEN];
    uint32_t id;
    uint16_t tp;
    mk_synth_hello(hy, SB, 30500, 1000, SEC1);
    {
        uint8_t hx[XR_HELLO_LEN];
        mk_synth_hello(hx, SA, 30900, 1000, SEC1);
        xr_reset();
        xr_self(ip(192, 168, 0, 108), 30000, 0);
        xr_self_synth(ip(10, 64, 0, 9));
        lobby("A=77.234.95.82\nLA=192.168.0.108\n");
        game_of(SA, SB, SEC1);
        /* X's socket learned as .1 */
        CHECK(xr_hello_seen(ep(AWAY, 52001), hx, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SA);
    }
    /* the lobby now says the socket's owner is .2 (the hello from it says
     * so): the old tie is dropped, its datagrams go to .2 */
    CHECK(xr_hello_seen(ep(AWAY, 52001), hy, XR_HELLO_LEN, &id, 2000) == XR_HELLO_NEW && id == SB);
    CHECK(xr_source(ep(AWAY, 52001), &id, &tp) && id == SB && tp == 1000);
    CHECK(xr_learned_count() == 1);
}

/* BO-344 (int034 long run, games 9 and 10): A (Mac2) and B (Ubu2) played game
 * 9 in one lobby session, so each kept the other's path and was on the 15 s
 * keep-alive. In game 10, A learned B from the lobby first; its hello reached
 * B before B knew A (refused), and from a new router port. Then B learned A
 * from the lobby. Both must hello the other within about a second, not on
 * the keep-alive, or B drops A's datagrams from the new port for 15 s. Also
 * the plain case: B's hello reaches A before A knows B at all. */
static void test_member_hellos(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    uint8_t ha[XR_HELLO_LEN], hb[XR_HELLO_LEN];
    xr_ep a_old = ep(AWAY, 31900), a_new = ep(AWAY, 1123), b_path = ep(AWAY, 31500);
    uint32_t id;
    uint16_t tp;
    uint64_t t;
    int n;
    mk_synth_hello(ha, SA, 30900, 1000, SEC);
    mk_synth_hello(hb, SB, 30500, 1000, SEC);

    /* the plain case, this copy is A: B's hello comes before the record */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 18));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    CHECK(xr_hello_seen(b_path, hb, XR_HELLO_LEN, &id, 1000) == XR_HELLO_UNKNOWN);
    game_of(SA, SB, SEC);   /* now it knows B */
    CHECK(xr_hello_due(SB, 1000, 1100));
    CHECK(!xr_hello_due(SB, 1000, 1300) && xr_hello_due(SB, 1000, 1600));

    /* game 9, this copy is B: A's path learned, both on the keep-alive */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 18));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SA, SB, SEC);
    CHECK(xr_hello_due(SA, 1000, 1000));
    CHECK(xr_hello_seen(a_old, ha, XR_HELLO_LEN, &id, 1100) == XR_HELLO_NEW && id == SA);
    CHECK(xr_hello_due(SA, 1000, 1150));   /* the one answer at once (BO-343) */
    CHECK(!xr_hello_due(SA, 1000, 2000));  /* then every 15 s */
    CHECK(xr_hello_due(SA, 1000, 16200));
    /* game 9 is over; game 10 in the same session */
    xr_game_reset();
    /* A's hello, from a new router port, before we know A is in game 10 */
    CHECK(xr_hello_seen(a_new, ha, XR_HELLO_LEN, &id, 20000) == XR_HELLO_UNKNOWN);
    /* now the lobby tells us: we hello A within a second, and keep at it
     * every 500 ms until A's own hello comes */
    game_of(SA, SB, SEC);
    for (n = 0, t = 20100; t <= 21100; t += 50)
        n += xr_hello_due(SA, 1000, t);
    CHECK(n >= 2);
    /* A, which also hellos fast now (the other side of the same fix), is
     * heard from its new port: its path is learned at once */
    CHECK(xr_hello_seen(a_new, ha, XR_HELLO_LEN, &id, 21200) == XR_HELLO_NEW && id == SA);
    CHECK(xr_source(a_new, &id, &tp) && id == SA && tp == 1000);
    CHECK(xr_hello_due(SA, 1000, 21250));
    CHECK(!xr_hello_due(SA, 1000, 22000));   /* heard: the keep-alive again */

    /* the same game from A's side: it knew B since game 9 and is told B is
     * in game 10 first; its first hello is refused by B (not shown here),
     * so it must not wait 15 s for the next */
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 18));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SA, SB, SEC);
    CHECK(xr_hello_seen(b_path, hb, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SB);
    CHECK(xr_hello_due(SB, 1000, 1050) && !xr_hello_due(SB, 1000, 2000));
    xr_game_reset();
    game_of(SA, SB, SEC);
    CHECK(xr_hello_due(SB, 1000, 20000));    /* at once (refused by B) */
    CHECK(!xr_hello_due(SB, 1000, 20100));
    /* B learns A and its hello comes (its old path: OLD) before our next
     * fast one: it knows us now and refused ours, so we answer at once,
     * from the new port B has to learn (what game 10 lacked) */
    CHECK(xr_hello_seen(b_path, hb, XR_HELLO_LEN, &id, 20150) == XR_HELLO_OLD);
    CHECK(xr_hello_due(SB, 1000, 20200));
    CHECK(!xr_hello_due(SB, 1000, 21000));   /* heard: the keep-alive */
    /* a repeated record of the same game changes nothing */
    CHECK(xr_hello_seen(b_path, hb, XR_HELLO_LEN, &id, 21100) == XR_HELLO_OLD);
    game_of(SA, SB, SEC);
    CHECK(!xr_hello_due(SB, 1000, 21300));
    /* without B's hello we would have gone on every 500 ms */
    xr_game_reset();
    game_of(SA, SB, SEC);
    CHECK(xr_hello_due(SB, 1000, 40000) && xr_hello_due(SB, 1000, 40600));
}

/* BO-345: the int037 soak (six copies behind one router, 77.234.95.82,
 * TAKEDAWN_NET_FORCE_PUBLIC=1). This copy is Ubu2 (ubuntu 192.168.0.108,
 * synthetic 10.64.82.216, offset 30200); the others by synthetic address,
 * real address and offset. The title drops a link the moment a peer's
 * datagram comes from an address other than that peer's (0x1B7FD8 ->
 * 0x102B50: 'the link was lost'), so every wrong answer of xr_source is a
 * lost link, and a dropped datagram is only a lost datagram. */
#define G_HOST ip(10, 64, 82, 213)
#define G_MAC2 ip(10, 64, 82, 214)
#define G_MAC1 ip(10, 64, 82, 215)
#define G_UBU2 ip(10, 64, 82, 216)
#define G_DEE1 ip(10, 64, 82, 217)
#define G_DEE2 ip(10, 64, 82, 218)
#define G_ROUTER ip(192, 168, 0, 1)
#define G_SEC "0c0f2233445566778899aabbccddeeff"
static struct { uint32_t id; const char *real; uint16_t off; } g_soak[5] = {
    { 0, "192.168.0.108", 30000 }, { 0, "192.168.0.50", 30100 },
    { 0, "192.168.0.50", 30300 }, { 0, "192.168.0.243", 30500 },
    { 0, "192.168.0.243", 30700 },
};
static uint8_t g_hello[5][2][XR_HELLO_LEN];   /* [player][0: port 1000, 1: port 6000] */

static void soak_hellos(void)
{
    int i;
    g_soak[0].id = G_HOST;
    g_soak[1].id = G_DEE1;
    g_soak[2].id = G_DEE2;
    g_soak[3].id = G_MAC1;
    g_soak[4].id = G_MAC2;
    for (i = 0; i < 5; i++) {
        mk_synth_hello(g_hello[i][0], g_soak[i].id, g_soak[i].off, 1000, G_SEC);
        mk_synth_hello(g_hello[i][1], g_soak[i].id, g_soak[i].off, 6000, G_SEC);
    }
}

static void soak_game(uint32_t self_real, uint32_t self_synth, uint16_t self_off, int force)
{
    char g[1024];
    int i, o;
    xr_reset();
    xr_self(self_real, self_off, force);
    xr_self_synth(self_synth);
    snprintf(g, sizeof g, "A=77.234.95.82\nLA=%u.%u.%u.%u\n", self_real & 255, self_real >> 8 & 255,
             self_real >> 16 & 255, self_real >> 24);
    lobby(g);   /* our +who (the address we gave as ours) */
    o = snprintf(g, sizeof g, "IDENT=56\nLADDR0=%u.%u.%u.%u\nADDR0=77.234.95.82\n",
                 self_synth & 255, self_synth >> 8 & 255, self_synth >> 16 & 255, self_synth >> 24);
    for (i = 0; i < 5; i++) {
        uint32_t s = g_soak[i].id;
        if (s == self_synth)
            continue;
        o += snprintf(g + o, sizeof g - (size_t)o, "LADDR%d=%u.%u.%u.%u\nTKRADDR%d=%s\nADDR%d=77.234.95.82\n",
                      i + 1, s & 255, s >> 8 & 255, s >> 16 & 255, s >> 24, i + 1, g_soak[i].real, i + 1);
    }
    snprintf(g + o, sizeof g - (size_t)o, "TKSECRET=%s\n", G_SEC);
    lobby(g);
    xr_members_body(g, (int)strlen(g) + 1);
    for (i = 0; i < 5; i++)
        if (g_soak[i].id != self_synth)
            xr_offset(g_soak[i].id, g_soak[i].off);
}

static int src_is(xr_ep src, uint16_t ltport, uint32_t want, uint16_t want_tp)
{
    uint32_t id = 0;
    uint16_t tp = 0;
    return xr_source_on(src, ltport, &id, &tp) && id == want && tp == want_tp;
}

/* int037 game 56 at Ubu2: the router's hairpin NAT showed Dee2 (its 1000
 * port, to our 1000) and Dee1 (its 6000, to our 6000) with one source,
 * 192.168.0.1:1145: two flows to two of our sockets may share it. Ubu2 then
 * lost peer 5 and dropped ten datagrams from :1145. Each socket's datagrams
 * must stay with its own player, whatever order the hellos come in. */
static void test_shared_router_source(void)
{
    xr_ep s1145 = ep(G_ROUTER, 1145);
    uint32_t id;
    int i, ok = 1;
    soak_hellos();
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 1);
    /* the order in Ubu2's log: Dee2 x3, Dee1 x2, then datagrams */
    for (i = 0; i < 3; i++)
        CHECK(xr_hello_seen_on(s1145, 1000, g_hello[2][0], XR_HELLO_LEN, &id, 1000 + i * 100) >= 0 && id == G_DEE2);
    CHECK(src_is(s1145, 1000, G_DEE2, 1000));
    for (i = 0; i < 2; i++)
        CHECK(xr_hello_seen_on(s1145, 6000, g_hello[1][1], XR_HELLO_LEN, &id, 1400 + i * 100) >= 0 && id == G_DEE1);
    CHECK(src_is(s1145, 1000, G_DEE2, 1000));
    CHECK(src_is(s1145, 6000, G_DEE1, 6000));
    /* both keep helloing every 500 ms: no flip either way, ever */
    for (i = 0; i < 40; i++) {
        xr_hello_seen_on(s1145, (i & 1) ? 6000 : 1000, g_hello[(i & 1) ? 1 : 2][i & 1], XR_HELLO_LEN, &id,
                         2000 + (uint64_t)i * 250);
        ok &= src_is(s1145, 1000, G_DEE2, 1000) && src_is(s1145, 6000, G_DEE1, 6000);
    }
    CHECK(ok);
    /* what we send them goes to their public address, not to the router's
     * own (that endpoint is only the reply side of their flow) */
    {
        xr_ep e;
        const char *why;
        int ch;
        CHECK(xr_route(G_DEE2, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 31300);
        CHECK(xr_route(G_DEE1, 6000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 36100);
    }
}

/* int037 game 47 at the host (offset 30000): 192.168.0.1:1141 for Ubu2 and
 * then Mac1; the host lost peer 2. */
static void test_game47_host(void)
{
    xr_ep s1141 = ep(G_ROUTER, 1141);
    uint8_t hu[XR_HELLO_LEN];
    uint32_t id;
    soak_hellos();
    mk_synth_hello(hu, G_UBU2, 30200, 1000, G_SEC);
    soak_game(ip(192, 168, 0, 108), G_HOST, 30000, 1);
    /* soak_game left Ubu2 out (it is in g_soak only as us elsewhere): add it */
    {
        const char *g = "IDENT=47\nLADDR0=10.64.82.213\nADDR0=77.234.95.82\n"
                        "LADDR1=10.64.82.216\nTKRADDR1=192.168.0.108\nADDR1=77.234.95.82\n"
                        "LADDR2=10.64.82.215\nTKRADDR2=192.168.0.243\nADDR2=77.234.95.82\n"
                        "LADDR3=10.64.82.217\nTKRADDR3=192.168.0.50\nADDR3=77.234.95.82\n"
                        "TKSECRET=" G_SEC "\n";
        lobby(g);
        xr_members_body(g, (int)strlen(g) + 1);
        xr_offset(G_UBU2, 30200);
    }
    CHECK(xr_hello_seen_on(s1141, 1000, hu, XR_HELLO_LEN, &id, 1000) >= 0 && id == G_UBU2);
    CHECK(xr_hello_seen_on(s1141, 6000, g_hello[3][1], XR_HELLO_LEN, &id, 1100) >= 0 && id == G_MAC1);
    CHECK(src_is(s1141, 1000, G_UBU2, 1000));
    CHECK(src_is(s1141, 6000, G_MAC1, 6000));
    CHECK(xr_hello_seen_on(s1141, 1000, hu, XR_HELLO_LEN, &id, 1500) >= 0 && id == G_UBU2);
    CHECK(src_is(s1141, 6000, G_MAC1, 6000));
}

/* all six share one public address: a datagram from 77.234.95.82:<port>
 * before any hello from there (the reply side of our own flow to that
 * player) belongs to the member whose offset fits our socket's port; one
 * that fits nobody is dropped, never handed to the first member */
static void test_shared_public_source(void)
{
    soak_hellos();
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 1);
    CHECK(src_is(ep(PUB_HOME, 31300), 1000, G_DEE2, 1000));
    CHECK(src_is(ep(PUB_HOME, 31700), 1000, G_MAC2, 1000));
    CHECK(src_is(ep(PUB_HOME, 36100), 6000, G_DEE1, 6000));
    CHECK(src_is(ep(PUB_HOME, 31000), 1000, G_HOST, 1000));
    /* NAT loopback with the sender's own port: the same rule */
    CHECK(src_is(ep(G_ROUTER, 31500), 1000, G_MAC1, 1000));
    CHECK(src_is(ep(G_ROUTER, 36300), 6000, G_DEE2, 6000));
    /* a remapped port nobody's offset explains: not anyone's */
    {
        uint32_t id;
        uint16_t tp;
        CHECK(!xr_source_on(ep(PUB_HOME, 1777), 1000, &id, &tp));
        CHECK(!xr_source_on(ep(G_ROUTER, 1145), 1000, &id, &tp));
    }
    /* ... until a hello from it says whose it is */
    {
        uint32_t id;
        CHECK(xr_hello_seen_on(ep(PUB_HOME, 1777), 1000, g_hello[4][0], XR_HELLO_LEN, &id, 1000) >= 0 && id == G_MAC2);
        CHECK(src_is(ep(PUB_HOME, 1777), 1000, G_MAC2, 1000));
    }
}

/* BO-345: a learned NAT-loopback path (192.168.0.1:<port>) is the reply side
 * of the other copy's flow and stops working once that copy sends there too
 * (int035 game 17, int036 game 26): sends go to the public address when
 * forced, to the LAN address otherwise. Datagrams from it still count. */
static void test_loopback_route(void)
{
    xr_ep e, lp = ep(G_ROUTER, 31500);
    const char *why;
    int ch;
    uint32_t id;
    soak_hellos();
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 1);
    CHECK(xr_hello_seen_on(lp, 1000, g_hello[3][0], XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == G_MAC1);
    CHECK(xr_route(G_MAC1, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 31500);
    CHECK(src_is(lp, 1000, G_MAC1, 1000));
    /* without the test switch: the LAN address, the same network */
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 0);
    CHECK(xr_hello_seen_on(lp, 1000, g_hello[3][0], XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == G_MAC1);
    CHECK(xr_route(G_MAC1, 1000, &e, &why, &ch) == 1 && e.ip == ip(192, 168, 0, 243) && e.port == 31500);
    CHECK(src_is(lp, 1000, G_MAC1, 1000));
    /* a learned public path (another network) still wins as before */
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 1);
    CHECK(xr_hello_seen_on(ep(PUB_HOME, 31500), 1000, g_hello[3][0], XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW);
    CHECK(xr_route(G_MAC1, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 31500 &&
          strstr(why, "hello") != NULL);
}

/* two players behind one router away (85.66.10.7), the router remapping
 * ports: each source a hello came from is that player's; a port no hello
 * came from and no offset explains is nobody's; a lone player behind its
 * own address keeps any port (the old rule) */
static void test_remote_shared_nat(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    uint8_t ha[XR_HELLO_LEN], hb[XR_HELLO_LEN];
    uint32_t id;
    uint16_t tp;
    mk_synth_hello(ha, SA, 30900, 1000, SEC);
    mk_synth_hello(hb, SB, 30500, 1000, SEC);
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    xr_self_synth(ip(10, 64, 0, 9));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SA, SB, SEC);
    CHECK(xr_hello_seen_on(ep(AWAY, 1123), 1000, ha, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SA);
    CHECK(xr_hello_seen_on(ep(AWAY, 1124), 1000, hb, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == SB);
    CHECK(src_is(ep(AWAY, 1123), 1000, SA, 1000));
    CHECK(src_is(ep(AWAY, 1124), 1000, SB, 1000));
    CHECK(!xr_source_on(ep(AWAY, 1500), 1000, &id, &tp));
    /* their own ports through a router that keeps them: the offsets */
    CHECK(src_is(ep(AWAY, 31900), 1000, SA, 1000));
    CHECK(src_is(ep(AWAY, 31500), 1000, SB, 1000));
}

/* int037: the router's loopback ports went round the players from game to
 * game (Mac2: :1139 was Dee2's in game 37 and Mac1's in game 41). A source
 * whose last hello is old says nothing about whose it is now; a fresh hello
 * from it does. */
static void test_router_port_reuse(void)
{
    xr_ep s1139 = ep(G_ROUTER, 1139);
    uint32_t id;
    uint16_t tp;
    soak_hellos();
    soak_game(ip(192, 168, 0, 108), G_UBU2, 30200, 1);
    CHECK(xr_hello_seen_on(s1139, 1000, g_hello[2][0], XR_HELLO_LEN, &id, 1000) >= 0 && id == G_DEE2);
    CHECK(src_is(s1139, 1000, G_DEE2, 1000));
    /* a minute of other players' hellos later (a new game) */
    CHECK(xr_hello_seen_on(ep(PUB_HOME, 31000), 1000, g_hello[0][0], XR_HELLO_LEN, &id, 61000) >= 0);
    CHECK(!xr_source_on(s1139, 1000, &id, &tp));
    /* Mac1's flow has it now; its hello comes first on that flow */
    CHECK(xr_hello_seen_on(s1139, 1000, g_hello[3][0], XR_HELLO_LEN, &id, 61100) >= 0 && id == G_MAC1);
    CHECK(src_is(s1139, 1000, G_MAC1, 1000));
}

/* the soak's second copy on a machine has TAKEDAWN_NET_ID: its title address
 * (10.77.1.2) is not on the LAN; the machine's own address tells where the
 * router's loopback is (int037 recheck: Ubu2 still sent to 192.168.0.1:31000) */
static void test_loopback_net_id(void)
{
    xr_ep e;
    const char *why;
    int ch;
    uint32_t id;
    soak_hellos();
    soak_game(ip(10, 77, 1, 2), G_UBU2, 30200, 1);
#ifdef XR_HAS_SOURCE_ON
    xr_self_lan(ip(192, 168, 0, 108));
#endif
    CHECK(xr_hello_seen_on(ep(G_ROUTER, 31000), 1000, g_hello[0][0], XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == G_HOST);
    CHECK(xr_route(G_HOST, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 31000);
    CHECK(src_is(ep(G_ROUTER, 31000), 1000, G_HOST, 1000));
}

/* BO-349: where a copy reaches the lobby it runs itself (LAN mode) */
static void test_lan_lobby_at(void)
{
    const uint32_t lo = ip(127, 0, 0, 1), lan = ip(192, 168, 0, 108);
    CHECK(xr_lan_lobby_at(lo, lan) == lan);
    CHECK(xr_lan_lobby_at(ip(127, 0, 0, 5), lan) == lan);           /* all of 127/8 is loopback */
    CHECK(xr_lan_lobby_at(lo, ip(10, 64, 0, 3)) == ip(10, 64, 0, 3));
    /* no LAN address (no route, or loopback only): as it is */
    CHECK(xr_lan_lobby_at(lo, 0) == lo);
    CHECK(xr_lan_lobby_at(lo, lo) == lo);
    CHECK(xr_lan_lobby_at(lo, ip(127, 0, 1, 1)) == lo);
    /* a lobby on another machine, or none: never changed */
    CHECK(xr_lan_lobby_at(ip(192, 168, 0, 108), ip(192, 168, 0, 192)) == ip(192, 168, 0, 108));
    CHECK(xr_lan_lobby_at(ip(79, 172, 222, 137), lan) == ip(79, 172, 222, 137));
    CHECK(xr_lan_lobby_at(0, lan) == 0);
}

/* BO-349, int041 LAN check try2 at guest C (192.168.0.192): host A
 * (192.168.0.108, offset 30700) runs the LAN lobby; B is 192.168.0.148
 * (30800). The lobby records each player at the address it signed in from:
 * A at the address its own copy reached the lobby at. Over loopback that
 * was 127.0.0.1, so C's game had a player 127.0.0.1 and A's hellos (its
 * title address, 192.168.0.108) were "not a player the lobby told us
 * about"; B's were fine. */
static void test_lan_lobby_host(void)
{
    const uint32_t A = ip(192, 168, 0, 108), B = ip(192, 168, 0, 148), C = ip(192, 168, 0, 192);
    const char *sec = "5a5a2233445566778899aabbccddeeff";
    uint8_t ha[XR_HELLO_LEN], hb[XR_HELLO_LEN];
    uint32_t a_rec = xr_lan_lobby_at(ip(127, 0, 0, 1), A), id;
    char g[512];
    xr_ep e;
    const char *why;
    int ch;
    mk_synth_hello(ha, A, 30700, 1000, sec);
    mk_synth_hello(hb, B, 30800, 1000, sec);
    xr_reset();
    xr_self(C, 30900, 0);
    lobby("A=192.168.0.192\nLA=192.168.0.192\n");                /* C's own pers: no synthetic address on a LAN lobby */
    snprintf(g, sizeof g, "IDENT=2\nLADDR0=%u.%u.%u.%u\nADDR0=%u.%u.%u.%u\nLADDR1=192.168.0.148\nADDR1=192.168.0.148\n"
             "LADDR2=192.168.0.192\nADDR2=192.168.0.192\nTKSECRET=%s\n",
             a_rec & 255, a_rec >> 8 & 255, a_rec >> 16 & 255, a_rec >> 24,
             a_rec & 255, a_rec >> 8 & 255, a_rec >> 16 & 255, a_rec >> 24, sec);
    lobby(g);
    xr_members_body(g, (int)strlen(g) + 1);
    xr_offset(A, 30700);
    xr_offset(B, 30800);
    /* the host is a player of the game, at its LAN address */
    CHECK(xr_is_member(A));
    CHECK(!xr_is_member(ip(127, 0, 0, 1)));
    /* its hellos count (int041: refused, not a player the lobby told us about) */
    CHECK(xr_hello_seen_on(ep(A, 31700), 1000, ha, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == A);
    CHECK(xr_route(A, 1000, &e, &why, &ch) == 1 && e.ip == A && e.port == 31700);
    /* the guest's were and still are */
    CHECK(xr_hello_seen_on(ep(B, 31800), 1000, hb, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == B);
    CHECK(xr_route(B, 1000, &e, &why, &ch) == 1 && e.ip == B && e.port == 31800);
}

/* BO-349: the LAN address from the up interfaces when no route probe gave
 * one (a LAN party with no gateway) */
/* BO-349, int042 game 18 as the owner's Mac saw it (Owner5 192.168.0.243,
 * offset 30000, synthetic 10.64.1.7): Host1 (10.64.1.5, really
 * 192.168.0.108, offset 30600) shares our public address, so the LAN path;
 * its hellos come through the router (192.168.0.1:31600). macOS refused
 * every send to 192.168.0.108 (no Local Network access): from then on the
 * public address, through the router, as the hellos already went. Another
 * player on the network keeps its LAN path; a new lobby session forgets. */
static void test_lan_denied(void)
{
    uint8_t h[XR_HELLO_LEN];
    xr_ep e, c[2];
    const char *why = NULL;
    int ch;
    uint32_t id;
    uint16_t tp;
    const uint32_t host = ip(10, 64, 1, 5), other = ip(10, 64, 1, 8), host_lan = ip(192, 168, 0, 108);
    xr_reset();
    xr_self(host_lan, 30600, 1);
    xr_self_synth(host);
    xr_hello_make(h, 1000);
    xr_reset();
    xr_self(ip(192, 168, 0, 243), 30000, 0);
    xr_self_synth(ip(10, 64, 1, 7));
    lobby("A=77.234.95.82\nLA=192.168.0.243\n");
    {
        const char *g = "IDENT=18\nLADDR0=10.64.1.5\nTKRADDR0=192.168.0.108\nADDR0=77.234.95.82\n"
                        "LADDR1=10.64.1.7\nTKRADDR1=192.168.0.243\nADDR1=77.234.95.82\n"
                        "LADDR2=10.64.1.8\nTKRADDR2=192.168.0.192\nADDR2=77.234.95.82\n";
        lobby(g);
        xr_members_body(g, (int)strlen(g) + 1);
    }
    xr_offset(host, 30600);
    xr_offset(other, 30100);
    CHECK(xr_hello_seen_on(ep(ip(192, 168, 0, 1), 31600), 1000, h, XR_HELLO_LEN, &id, 1000) == XR_HELLO_NEW && id == host);
    /* sends work: the LAN path, as on int041 */
    CHECK(xr_route(host, 1000, &e, &why, &ch) == 1 && e.ip == host_lan && e.port == 31600);
    CHECK(xr_route(host, 6000, &e, &why, &ch) == 1 && e.ip == host_lan && e.port == 36600);
    /* a refusal for some other address is not this player's LAN path */
    CHECK(xr_lan_denied(host, ip(77, 234, 95, 82)) == 0);
    CHECK(xr_lan_denied(ip(10, 64, 9, 9), host_lan) == 0);
    /* the refusal: once newly, then the public address on every port */
    CHECK(xr_lan_denied(host, host_lan) == 1);
    CHECK(xr_lan_denied(host, host_lan) == 0);
    CHECK(xr_route(host, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 31600 &&
          strstr(why, "Local Network") != NULL);
    CHECK(xr_route(host, 6000, &e, &why, &ch) == 1 && e.ip == PUB_HOME && e.port == 36600);
    CHECK(xr_candidates(host, 1000, c) == 1 && c[0].ip == PUB_HOME && c[0].port == 31600);
    /* its datagrams still come in through the router */
    CHECK(xr_source_on(ep(ip(192, 168, 0, 1), 31600), 1000, &id, &tp) && id == host && tp == 1000);
    /* the other player on our network: still its LAN address */
    CHECK(xr_route(other, 1000, &e, &why, &ch) == 1 && e.ip == ip(192, 168, 0, 192) && e.port == 31100);
    /* the next game (often the same player): still public */
    xr_game_reset();
    CHECK(xr_route(host, 1000, &e, &why, &ch) == 1 && e.ip == PUB_HOME);
    /* a new lobby session starts over */
    xr_session_reset();
    {
        const char *g = "IDENT=19\nLADDR0=10.64.1.5\nTKRADDR0=192.168.0.108\nADDR0=77.234.95.82\n";
        lobby(g);
        xr_members_body(g, (int)strlen(g) + 1);
    }
    xr_offset(host, 30600);
    CHECK(xr_route(host, 1000, &e, &why, &ch) == 1 && e.ip == host_lan);
}

static void test_pick_lan(void)
{
    uint32_t a[8];
    int why = -1;
    /* none: nothing, or only loopback / link-local / multicast / broadcast / 0 */
    CHECK(xr_pick_lan(a, 0, &why) == 0 && why == XR_LAN_NONE);
    a[0] = ip(127, 0, 0, 1); a[1] = ip(169, 254, 12, 7); a[2] = ip(224, 0, 0, 251); a[3] = ip(255, 255, 255, 255); a[4] = 0;
    CHECK(xr_pick_lan(a, 5, &why) == 0 && why == XR_LAN_NONE);
    /* the one LAN address, past the ones that never count */
    a[5] = ip(192, 168, 1, 23);
    CHECK(xr_pick_lan(a, 6, &why) == ip(192, 168, 1, 23) && why == XR_LAN_PRIVATE);
    /* a private one before a public one, whatever the order */
    a[0] = ip(85, 66, 10, 7); a[1] = ip(10, 0, 0, 5);
    CHECK(xr_pick_lan(a, 2, &why) == ip(10, 0, 0, 5) && why == XR_LAN_PRIVATE);
    /* 192.168 before 10 before 172.16-31 (docker's 172.17 never wins over the LAN) */
    a[0] = ip(172, 17, 0, 1); a[1] = ip(10, 1, 2, 3); a[2] = ip(192, 168, 0, 108);
    CHECK(xr_pick_lan(a, 3, &why) == ip(192, 168, 0, 108) && why == XR_LAN_PRIVATE);
    CHECK(xr_pick_lan(a, 2, &why) == ip(10, 1, 2, 3) && why == XR_LAN_PRIVATE);
    CHECK(xr_pick_lan(a, 1, &why) == ip(172, 17, 0, 1) && why == XR_LAN_PRIVATE);
    /* 172.32 is not private; within one range, the first given */
    a[0] = ip(172, 32, 0, 1); a[1] = ip(192, 168, 5, 1); a[2] = ip(192, 168, 0, 9);
    CHECK(xr_pick_lan(a, 3, &why) == ip(192, 168, 5, 1));
    CHECK(xr_pick_lan(a, 1, &why) == ip(172, 32, 0, 1) && why == XR_LAN_OTHER);
    /* no private one: the first other address (a public LAN), not link-local */
    a[0] = ip(169, 254, 1, 1); a[1] = ip(100, 72, 3, 4); a[2] = ip(85, 66, 10, 7);
    CHECK(xr_pick_lan(a, 3, &why) == ip(100, 72, 3, 4) && why == XR_LAN_OTHER);
    /* why may be left out */
    CHECK(xr_pick_lan(a, 3, NULL) == ip(100, 72, 3, 4));
}

/* BO-352 step 6: the reactions side channel (a "TKRX" datagram). The packet
 * is made by a copy with a synthetic address under a game's secret, then
 * checked by a receiver in that game: the tag, the source, the sequence (the
 * sender sends it twice, 60 ms apart, so the second is a duplicate) and the
 * per-sender rate limit. */
#define RC ip(10, 64, 0, 7)   /* a synthetic address not in our game */

static void mk_react(uint8_t *b, uint32_t synth, const char *sec, int kind, int value)
{
    char body[64];
    xr_reset();
    xr_self(ip(192, 168, 0, 200), 30000, 0);
    xr_self_synth(synth);
    snprintf(body, sizeof body, "TKSECRET=%s\n", sec);
    lobby(body);
    xr_react_make(b, kind, value);
}

/* the receiver: us at home (192.168.0.108, synthetic 10.64.0.9), SA and SB
 * in a game behind AWAY */
static void react_receiver(const char *sec)
{
    xr_reset();
    xr_self(ip(192, 168, 0, 108), 30000, 0);
    CHECK(xr_addr_reply("TKADDR=10.64.0.9\n", 18));
    lobby("A=77.234.95.82\nLA=192.168.0.108\n");
    game_of(SA, SB, sec);
}

static void test_react(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    const char *OTHER = "ffeeddccbbaa99887766554433221100";
    uint8_t r[XR_REACT_LEN], bad[XR_REACT_LEN], wrong[XR_REACT_LEN], foreign[XR_REACT_LEN], mine[XR_REACT_LEN];
    uint32_t id, ids[4];
    int kind, value;

    /* the packet: the right size and version */
    mk_react(r, SA, SEC, XR_REACT_REACTION, 2);
    CHECK(xr_is_react(r, XR_REACT_LEN) && !xr_is_react(r, XR_REACT_LEN - 1) && !xr_is_react(r, 0));
    r[4] = 9;
    CHECK(!xr_is_react(r, XR_REACT_LEN));   /* another version */
    r[4] = 1;

    /* a bad kind, and a reaction id past the last (0..4) */
    memcpy(bad, r, sizeof bad);
    bad[5] = 9;
    memcpy(wrong, r, sizeof wrong);
    wrong[14] = XR_REACT_N;
    /* the wrong secret, a non-member, and our own address */
    mk_react(foreign, SA, OTHER, XR_REACT_REACTION, 2);
    mk_react(mine, ip(10, 64, 0, 9), SEC, XR_REACT_REACTION, 0);
    mk_react(r, SA, SEC, XR_REACT_REACTION, 2);   /* the real one again */

    react_receiver(SEC);
    /* the members, for the send side */
    CHECK(xr_member_list(ids, 4) == 2 && ((ids[0] == SA && ids[1] == SB) || (ids[0] == SB && ids[1] == SA)));

    /* a member's reaction from its public address: accepted and queued */
    CHECK(xr_react_seen(ep(AWAY, 52000), r, XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(id == SA && kind == XR_REACT_REACTION && value == 2);
    CHECK(xr_react_get(&id, &kind, &value) && id == SA && kind == XR_REACT_REACTION && value == 2);
    CHECK(!xr_react_get(&id, &kind, &value));

    /* the same packet again (the 60 ms resend): a duplicate, never queued */
    CHECK(xr_react_seen(ep(AWAY, 52000), r, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_DUP);
    CHECK(!xr_react_get(&id, &kind, &value));

    /* a bad kind, and a reaction id past the last */
    CHECK(xr_react_seen(ep(AWAY, 52000), bad, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_BAD);
    CHECK(xr_react_seen(ep(AWAY, 52000), wrong, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_BAD);
    /* signed with another game's secret */
    CHECK(xr_react_seen(ep(AWAY, 52000), foreign, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_BADTAG);
    /* our own packet back at us: not a peer's */
    CHECK(xr_react_seen(ep(AWAY, 52000), mine, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_BAD);
    /* from a third-party address: not the member's */
    CHECK(xr_react_seen(ep(ip(1, 2, 3, 4), 52000), r, XR_REACT_LEN, &id, &kind, &value, 2000) == XR_REACT_WRONGSRC);

    /* a non-member (not in our game): refused, whatever its signature */
    mk_react(foreign, RC, SEC, XR_REACT_REACTION, 0);
    react_receiver(SEC);
    CHECK(xr_react_seen(ep(AWAY, 52000), foreign, XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_UNKNOWN);

    /* the countdown (kind 2) carries seconds, not a reaction id */
    mk_react(r, SA, SEC, XR_REACT_COUNTDOWN, 7);
    react_receiver(SEC);
    CHECK(xr_react_seen(ep(AWAY, 52000), r, XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(id == SA && kind == XR_REACT_COUNTDOWN && value == 7);

    /* a player that quit on the results says so (kind 3) */
    mk_react(r, SA, SEC, XR_REACT_LEFT, XR_LEFT_RESULTS);
    react_receiver(SEC);
    CHECK(xr_react_seen(ep(AWAY, 52000), r, XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(id == SA && kind == XR_REACT_LEFT && value == XR_LEFT_RESULTS);
}

static void mk_vote(uint8_t *b, uint32_t synth, const char *sec, uint8_t first)
{
    uint8_t d[XR_VOTE_DATA];
    char body[64];
    memset(d, 0, sizeof d);
    d[0] = first;
    d[XR_VOTE_DATA - 1] = 0x5A;
    xr_reset();
    xr_self(ip(192, 168, 0, 200), 30000, 0);
    xr_self_synth(synth);
    snprintf(body, sizeof body, "TKSECRET=%s\n", sec);
    lobby(body);
    xr_vote_make(b, d);
}

static void test_vote(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    uint8_t v[XR_VOTE_LEN], bad[XR_VOTE_LEN], d[XR_VOTE_DATA], foreign[XR_VOTE_LEN], many[45][XR_VOTE_LEN];
    uint32_t id;
    int i, ok;
    mk_vote(v, SA, SEC, 7);
    CHECK(xr_is_vote(v, XR_VOTE_LEN));
    CHECK(!xr_is_vote(v, XR_VOTE_LEN - 1));
    CHECK(!xr_is_react(v, XR_VOTE_LEN));
    mk_vote(foreign, SA, "ffeeddccbbaa99887766554433221100", 7);
    react_receiver(SEC);
    CHECK(xr_vote_seen(ep(AWAY, 52000), v, XR_VOTE_LEN, &id, 1000) == XR_REACT_OK);
    CHECK(id == SA);
    CHECK(xr_vote_get(&id, d) && id == SA && d[0] == 7 && d[XR_VOTE_DATA - 1] == 0x5A);
    CHECK(!xr_vote_get(&id, d));
    /* the same packet from the title's other socket stays one */
    CHECK(xr_vote_seen(ep(AWAY, 52000), v, XR_VOTE_LEN, &id, 1000) == XR_REACT_DUP);
    CHECK(!xr_vote_get(&id, d));
    /* a wrong tag, a changed byte, a stranger's address */
    CHECK(xr_vote_seen(ep(AWAY, 52000), foreign, XR_VOTE_LEN, &id, 1000) == XR_REACT_BADTAG);
    memcpy(bad, v, sizeof bad);
    bad[14] ^= 1;
    CHECK(xr_vote_seen(ep(AWAY, 52000), bad, XR_VOTE_LEN, &id, 1000) == XR_REACT_BADTAG);
    /* a non-member */
    mk_vote(foreign, RC, SEC, 1);
    react_receiver(SEC);
    CHECK(xr_vote_seen(ep(AWAY, 52000), foreign, XR_VOTE_LEN, &id, 1000) == XR_REACT_UNKNOWN);
    /* a sender's packets a second are capped; the next second they pass */
    for (i = 0; i < 45; i++)
        mk_vote(many[i], SA, SEC, (uint8_t)i);
    react_receiver(SEC);
    for (i = 0, ok = 0; i < 44; i++)
        ok += xr_vote_seen(ep(AWAY, 52000), many[i], XR_VOTE_LEN, &id, 5000) == XR_REACT_OK;
    CHECK(ok == 40);
    CHECK(xr_vote_seen(ep(AWAY, 52000), many[44], XR_VOTE_LEN, &id, 6000) == XR_REACT_OK);
    /* a new game forgets what was queued */
    react_receiver(SEC);
    CHECK(!xr_vote_get(&id, d));
    /* a member list holds us too: xr_is_self tells */
    CHECK(xr_is_self(ip(192, 168, 0, 108)));
    CHECK(!xr_is_self(SA));
    CHECK(!xr_is_self(0));
}


/* ── BO-381: the relay fallback ─────────────────────────────────────── */
#define VPS ip(79, 172, 222, 137)

static void sec_bytes(const char *sec, uint8_t out[16])
{
    int i;
    for (i = 0; i < 16; i++) {
        unsigned v;
        sscanf(sec + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* a hello of peer id (title port tport, offset 30900 as its MA says), bits in byte 14 */
static void mk_hello_of(uint8_t *b, uint32_t id, const char *sec, uint16_t tport, int bits)
{
    uint8_t k[16];
    uint64_t t;
    int i;
    sec_bytes(sec, k);
    memset(b, 0, XR_HELLO_LEN);
    memcpy(b, "TKDH", 4);
    b[4] = 2;
    memcpy(b + 6, &id, 4);
    b[10] = 0x78; b[11] = 0xb4;
    b[12] = (uint8_t)(tport >> 8); b[13] = (uint8_t)tport;
    b[14] = (uint8_t)bits;
    t = xr_siphash(k, b, 16);
    for (i = 0; i < 8; i++)
        b[16 + i] = (uint8_t)(t >> (8 * i));
}

/* a TKRL datagram as the relay forwards it (kind, game ident, from, to, ports, payload) */
static int mk_relay(uint8_t *out, const char *sec, int kind, int ident, uint32_t from, uint32_t to,
                    uint16_t ftp, uint16_t ttp, const uint8_t *payload, int n)
{
    uint8_t k[16];
    uint64_t t;
    int i;
    sec_bytes(sec, k);
    memset(out, 0, XR_RELAY_HDR);
    memcpy(out, "TKRL", 4);
    out[4] = 1;
    out[5] = (uint8_t)kind;
    out[6] = (uint8_t)(ident >> 24); out[7] = (uint8_t)(ident >> 16); out[8] = (uint8_t)(ident >> 8); out[9] = (uint8_t)ident;
    memcpy(out + 10, &from, 4);
    memcpy(out + 14, &to, 4);
    out[18] = (uint8_t)(ftp >> 8); out[19] = (uint8_t)ftp;
    out[20] = (uint8_t)(ttp >> 8); out[21] = (uint8_t)ttp;
    t = xr_siphash(k, out, 24);
    for (i = 0; i < 8; i++)
        out[24 + i] = (uint8_t)(t >> (8 * i));
    memcpy(out + XR_RELAY_HDR, payload, (size_t)n);
    return XR_RELAY_HDR + n;
}

static void test_relay(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    const char *ME_ID = "TKADDR=10.64.0.9\n";
    uint32_t me = ip(10, 64, 0, 9), from, sa = SA;
    uint8_t h[XR_HELLO_LEN], w[XR_RELAY_HDR + XR_RELAY_PAYLOAD], mine[XR_HELLO_LEN], k[16];
    uint16_t ftp, ttp;
    int n, kind, off, i;
    xr_ep e, c[XR_MAX_CAND], relay;
    const char *why;
    int ch;
    (void)ME_ID;
    relay = ep(VPS, 21811);
    sec_bytes(SEC, k);

    react_receiver(SEC);                       /* our game 3: SA and SB, the secret */
    CHECK(xr_relay_port_seen() == 0);
    lobby("IDENT=3\nTKRELAY=21811\n");
    CHECK(xr_relay_port_seen() == 21811);
    lobby("TKRELAY=70000\n");
    CHECK(xr_relay_port_seen() == 0);          /* not a port */
    lobby("TKRELAY=21811\n");
    xr_relay_set(VPS, 21811);
    xr_relay_game(3);
    CHECK(xr_relay_ep().ip == VPS && xr_relay_ep().port == 21811);
    CHECK(xr_is_relay_src(relay) && !xr_is_relay_src(ep(VPS, 21810)));

    /* direct first: the relay is wanted only 2 s after our hellos to the peer began */
    CHECK(!xr_relay_want(SA, 1000, 1000));
    CHECK(xr_hello_due(SA, 1000, 1000));
    CHECK(!xr_relay_want(SA, 1000, 1500));
    CHECK(!xr_relay_want(SA, 1000, 2999));
    CHECK(xr_relay_want(SA, 1000, 3000));
    xr_relay_off(1);
    CHECK(!xr_relay_want(SA, 1000, 3000));
    xr_relay_off(0);
    CHECK(!xr_relay_want(RC, 1000, 3000));     /* not a member */
    /* the route is still the direct guess, nothing relayed */
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == AWAY);
    CHECK(!xr_route_is_relay(SA, 1000));

    /* wrapping our own hello for the relay */
    xr_hello_make_for(mine, 1000, SA);
    CHECK((mine[14] & 2) == 0);                /* no direct hello of SA came yet */
    n = xr_relay_wrap(w, (int)sizeof w, SA, 1000, 1000, mine, XR_HELLO_LEN);
    CHECK(n == XR_RELAY_HDR + XR_HELLO_LEN);
    CHECK(xr_is_relay_pkt(w, n) && !xr_is_relay_pkt(w, 10));
    CHECK(w[5] == XR_RELAY_DATA && w[9] == 3 && !memcmp(w + 10, &me, 4) && !memcmp(w + 14, &sa, 4));
    CHECK(w[18] == 3 && w[19] == 0xE8 && w[20] == 3 && w[21] == 0xE8);
    CHECK(!memcmp(w + XR_RELAY_HDR, mine, XR_HELLO_LEN));
    {
        uint64_t t = xr_siphash(k, w, 24);
        for (i = 0; i < 8; i++)
            CHECK(w[24 + i] == (uint8_t)(t >> (8 * i)));
    }
    /* our own packet back (from = us): refused; too long: not made */
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_BAD);
    CHECK(xr_relay_wrap(w, (int)sizeof w, SA, 1000, 1000, mine, XR_RELAY_PAYLOAD + 1) == 0);
    CHECK(xr_relay_wrap(w, 40, SA, 1000, 1000, mine, XR_HELLO_LEN) == 0);

    /* SA's hello through the relay: a relay path, the datagrams go wrapped */
    mk_hello_of(h, SA, SEC, 1000, 1);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, SA, me, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_OK);
    CHECK(from == SA && ftp == 1000 && ttp == 1000 && kind == XR_RELAY_DATA && off == XR_RELAY_HDR);
    CHECK(xr_relay_hello_seen(SA, 1000, 1000, w + off, n - off, 3200) == XR_HELLO_NEW);
    CHECK(xr_relay_hello_seen(SA, 1000, 1000, w + off, n - off, 3300) == XR_HELLO_OLD);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == VPS && e.port == 21811 && strstr(why, "relay"));
    CHECK(xr_route_is_relay(SA, 1000));
    CHECK(!xr_route_is_relay(SA, 6000));
    CHECK(xr_relay_want(SA, 1000, 3300));      /* it reached us that way: answer in kind */
    /* the hellos keep trying the direct paths: the guess, never the relay */
    n = xr_candidates(SA, 1000, c);
    CHECK(n >= 1 && c[0].ip == AWAY);
    for (i = 0; i < n; i++)
        CHECK(!(c[i].ip == VPS && c[i].port == 21811));
    /* and stay fast for a while (direct may still be found), then slow down */
    CHECK(xr_hello_due(SA, 1000, 3300));
    CHECK(!xr_hello_due(SA, 1000, 3500));
    CHECK(xr_hello_due(SA, 1000, 3900));
    CHECK(xr_hello_due(SA, 1000, 25000));
    CHECK(!xr_hello_due(SA, 1000, 26000));
    CHECK(xr_hello_due(SA, 1000, 41000));
    /* the relay's answer: SA's endpoint for that port, one more direct candidate */
    {
        uint8_t ans[XR_RELAY_HDR + 6], pl[6];
        uint32_t ipv = ip(85, 66, 10, 7);
        memcpy(pl, &ipv, 4);
        pl[4] = 0xD4; pl[5] = 0x31;            /* 54321 */
        /* the server answers with from = the peer, from-port = the peer's port, to-port = ours */
        n = mk_relay(ans, SEC, XR_RELAY_ANSWER, 3, SA, me, 1000, 6000, pl, 6);
        CHECK(xr_relay_unwrap(ans, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_OK && kind == XR_RELAY_ANSWER);
        CHECK(from == SA && ftp == 1000 && ttp == 6000);
        xr_relay_answer(from, ftp, ep(ipv, 54321));
        n = xr_candidates(SA, 1000, c);
        for (i = 0; i < n; i++)
            if (c[i].ip == ipv && c[i].port == 54321)
                break;
        CHECK(i < n);
        /* a private address the relay cannot have seen for a peer elsewhere: ignored */
        xr_relay_answer(SA, 6000, ep(ip(192, 168, 7, 7), 31000));
        n = xr_candidates(SA, 6000, c);
        for (i = 0; i < n; i++)
            CHECK(c[i].ip != ip(192, 168, 7, 7));
    }
    /* a direct hello from SA without bit 1 (it does not hear us directly): the relay stays */
    mk_hello_of(h, SA, SEC, 1000, 1);
    CHECK(xr_hello_seen(ep(AWAY, 54321), h, XR_HELLO_LEN, &from, 42000) == XR_HELLO_ONEWAY);
    CHECK(xr_route_is_relay(SA, 1000));
    /* but our next hello tells it we hear it directly (bit 1, the tag redone) */
    xr_hello_make_for(mine, 1000, SA);
    CHECK((mine[14] & 2) != 0);
    {
        uint64_t t = xr_siphash(k, mine, 16);
        for (i = 0; i < 8; i++)
            CHECK(mine[16 + i] == (uint8_t)(t >> (8 * i)));
    }
    xr_hello_make_for(mine, 6000, SA);
    CHECK((mine[14] & 2) == 0);                /* per port */
    /* with bit 1 from it as well: the direct path takes over */
    mk_hello_of(h, SA, SEC, 1000, 3);
    CHECK(xr_hello_seen(ep(AWAY, 54321), h, XR_HELLO_LEN, &from, 42500) == XR_HELLO_NEW);
    CHECK(xr_route(SA, 1000, &e, &why, &ch) == 1 && e.ip == AWAY && e.port == 54321);
    CHECK(!xr_route_is_relay(SA, 1000));
    CHECK(!xr_relay_want(SA, 1000, 43000));    /* a direct path: no more relay hellos */
    /* a relayed hello while the direct path is fresh: the direct one stays */
    mk_hello_of(h, SA, SEC, 1000, 3);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, SA, me, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_hello_seen(SA, 1000, 1000, w + XR_RELAY_HDR, XR_HELLO_LEN, 43000) == XR_HELLO_OLD);
    CHECK(!xr_route_is_relay(SA, 1000));
    /* ... and takes over once the direct one went quiet */
    CHECK(xr_relay_hello_seen(SA, 1000, 1000, w + XR_RELAY_HDR, XR_HELLO_LEN, 80000) == XR_HELLO_NEW);
    CHECK(xr_route_is_relay(SA, 1000));

    /* refusals: a bad tag, another game, a non-member, not for us, short, a mismatching hello */
    mk_hello_of(h, SA, SEC, 1000, 1);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, SA, me, 1000, 1000, h, XR_HELLO_LEN);
    w[30] ^= 1;
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_BADTAG);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 4, SA, me, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_UNKNOWN);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, RC, me, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_UNKNOWN);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, SA, SB, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_NOTME);
    n = mk_relay(w, SEC, XR_RELAY_DATA, 3, SA, me, 1000, 1000, h, XR_HELLO_LEN);
    CHECK(xr_relay_unwrap(w, 20, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_BAD);
    w[5] = 7;
    CHECK(xr_relay_unwrap(w, n, &from, &ftp, &ttp, &kind, &off) == XR_RELAY_BAD);
    mk_hello_of(h, SB, SEC, 1000, 1);          /* the wrapper says SA, the hello SB */
    CHECK(xr_relay_hello_seen(SA, 1000, 1000, h, XR_HELLO_LEN, 6000) == XR_HELLO_BAD);
    /* without a relay (an old server) nothing is wrapped or wanted */
    xr_relay_set(0, 0);
    CHECK(xr_relay_wrap(w, (int)sizeof w, SA, 1000, 1000, mine, XR_HELLO_LEN) == 0);
    CHECK(!xr_relay_want(SA, 1000, 9000));
    CHECK(!xr_route_is_relay(SA, 1000));
    xr_relay_set(VPS, 21811);

    /* a ping through the relay: trusted for that one call, from the relay's endpoint */
    {
        uint8_t pg[XR_PING_LEN], reply[XR_PING_LEN];
        unsigned ms;
        uint64_t t;
        memset(pg, 0, sizeof pg);
        memcpy(pg, "TKPG", 4);
        pg[4] = 1; pg[5] = XR_PING_PING;
        memcpy(pg + 6, &sa, 4);
        pg[13] = 9;
        t = xr_siphash(k, pg, 24);
        for (i = 0; i < 8; i++)
            pg[24 + i] = (uint8_t)(t >> (8 * i));
        CHECK(xr_ping_seen(relay, pg, XR_PING_LEN, &from, reply, &ms, 7000) == XR_PING_WRONGSRC);
        xr_relay_trust(SA);
        CHECK(xr_ping_seen(relay, pg, XR_PING_LEN, &from, reply, &ms, 7000) == XR_PING_ANSWER_IT);
        xr_relay_trust(SB);                    /* trust is for the named peer only */
        pg[13] = 10;
        t = xr_siphash(k, pg, 24);
        for (i = 0; i < 8; i++)
            pg[24 + i] = (uint8_t)(t >> (8 * i));
        CHECK(xr_ping_seen(relay, pg, XR_PING_LEN, &from, reply, &ms, 7400) == XR_PING_WRONGSRC);
    }
    /* a new lobby session forgets the relay (the next member record names it again) */
    xr_session_reset();
    CHECK(xr_relay_ep().port == 0 && xr_relay_port_seen() == 0);
}

static void test_react_rate(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    uint8_t p[11][XR_REACT_LEN];
    int i, kind, value;
    uint32_t id;
    uint64_t t;
    for (i = 0; i < 11; i++)
        mk_react(p[i], SA, SEC, XR_REACT_REACTION, i % XR_REACT_N);
    react_receiver(SEC);
    /* a burst of three, then the fourth refused */
    CHECK(xr_react_seen(ep(AWAY, 52000), p[0], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[1], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[2], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[3], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_LIMIT);
    /* one per 1.5 s from then on */
    CHECK(xr_react_seen(ep(AWAY, 52000), p[3], XR_REACT_LEN, &id, &kind, &value, 2400) == XR_REACT_LIMIT);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[3], XR_REACT_LEN, &id, &kind, &value, 2500) == XR_REACT_OK);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[4], XR_REACT_LEN, &id, &kind, &value, 2500) == XR_REACT_LIMIT);
    /* ten in a minute, whatever the tokens */
    for (i = 4, t = 4000; i < 10; i++, t += 1500)
        CHECK(xr_react_seen(ep(AWAY, 52000), p[i], XR_REACT_LEN, &id, &kind, &value, t) == XR_REACT_OK);
    /* the 11th in that minute is refused */
    CHECK(xr_react_seen(ep(AWAY, 52000), p[10], XR_REACT_LEN, &id, &kind, &value, t) == XR_REACT_LIMIT);
    /* a minute on, it is allowed again */
    CHECK(xr_react_seen(ep(AWAY, 52000), p[10], XR_REACT_LEN, &id, &kind, &value, 62000) == XR_REACT_OK);

    /* the send bucket is the same rule (a fresh reset: a new self bucket) */
    react_receiver(SEC);
    CHECK(xr_react_send_ok(1000) && xr_react_send_ok(1000) && xr_react_send_ok(1000));
    CHECK(!xr_react_send_ok(1000));
    CHECK(xr_react_send_ok(2500) && !xr_react_send_ok(2500));

    /* a new game clears the receive buckets (the sender starts fresh) */
    react_receiver(SEC);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[0], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
    xr_game_reset();
    game_of(SA, SB, SEC);
    CHECK(xr_react_seen(ep(AWAY, 52000), p[1], XR_REACT_LEN, &id, &kind, &value, 1000) == XR_REACT_OK);
}

/* BO-357: the peers' ping: this table against packets signed as a member would. */
static uint32_t ping_seq(const uint8_t *b)
{
    return (uint32_t)b[10] << 24 | (uint32_t)b[11] << 16 | (uint32_t)b[12] << 8 | b[13];
}

static void signed_ping(uint8_t *out, uint32_t from, const char *sec, int kind, uint32_t seq)
{
    memset(out, 0, XR_PING_LEN);
    memcpy(out, "TKPG", 4);
    out[4] = 1;
    out[5] = (uint8_t)kind;
    memcpy(out + 6, &from, 4);
    out[10] = (uint8_t)(seq >> 24);
    out[11] = (uint8_t)(seq >> 16);
    out[12] = (uint8_t)(seq >> 8);
    out[13] = (uint8_t)seq;
    {
        uint8_t key[16];
        int i;
        uint64_t t;
        for (i = 0; i < 16; i++) {
            unsigned v;
            sscanf(sec + 2 * i, "%2x", &v);
            key[i] = (uint8_t)v;
        }
        t = xr_siphash(key, out, 24);
        for (i = 0; i < 8; i++)
            out[24 + i] = (uint8_t)(t >> (8 * i));
    }
}

static void test_ping(void)
{
    const char *SEC = "00112233445566778899aabbccddeeff";
    const char *OTHER = "ffeeddccbbaa99887766554433221100";
    uint8_t b[XR_PING_LEN], in[XR_PING_LEN], reply[XR_PING_LEN], hello[XR_HELLO_LEN];
    xr_rtt r;
    uint32_t id, seq;
    unsigned ms = 0;
    int i;

    /* the packet: the right size, magic and version */
    signed_ping(in, SA, SEC, XR_PING_PING, 5);
    CHECK(xr_is_ping(in, XR_PING_LEN) && !xr_is_ping(in, XR_PING_LEN - 1) && !xr_is_ping(in, 0));
    in[4] = 9;
    CHECK(!xr_is_ping(in, XR_PING_LEN));

    react_receiver(SEC);
    /* our hello says we answer; an older copy's byte is 0 */
    CHECK(xr_hello_make(hello, 1000) == XR_HELLO_LEN && hello[14] == 1 && xr_is_hello(hello, XR_HELLO_LEN));

    /* no hello from the peer: it may be an older copy, no ping goes to it */
    CHECK(!xr_ping_next(1000, &id, b));
    CHECK(!xr_peer_rtt(SA, 1000, &r) && r.samples == 0);

    /* a ping from a member: one answer, with the same sequence, signed */
    signed_ping(in, SA, SEC, XR_PING_PING, 5);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1000) == XR_PING_ANSWER_IT);
    CHECK(id == SA && xr_is_ping(reply, XR_PING_LEN) && reply[5] == XR_PING_ANSWER && ping_seq(reply) == 5);
    /* the second socket's copy of it: nothing */
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1001) == XR_PING_NOTHING);
    /* the rate limit: a new sequence too soon */
    signed_ping(in, SA, SEC, XR_PING_PING, 6);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1100) == XR_PING_LIMIT);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1300) == XR_PING_ANSWER_IT);

    /* refused: the wrong secret, a stranger's address, our own id, a non-member, a bad kind */
    signed_ping(in, SA, OTHER, XR_PING_PING, 7);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 2000) == XR_PING_BADTAG);
    signed_ping(in, SA, SEC, XR_PING_PING, 7);
    CHECK(xr_ping_seen(ep(ip(1, 2, 3, 4), 52000), in, XR_PING_LEN, &id, reply, &ms, 2000) == XR_PING_WRONGSRC);
    signed_ping(in, ip(10, 64, 0, 9), SEC, XR_PING_PING, 7);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 2000) == XR_PING_BAD);
    signed_ping(in, ip(10, 64, 0, 99), SEC, XR_PING_PING, 7);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 2000) == XR_PING_UNKNOWN);
    signed_ping(in, SA, SEC, 9, 7);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 2000) == XR_PING_BAD);

    /* the peer's hello says it answers: pings go, once a second */
    {
        uint8_t hh[XR_HELLO_LEN];
        char body[64];
        xr_reset();
        xr_self(ip(192, 168, 0, 200), 30000, 0);
        xr_self_synth(SA);
        snprintf(body, sizeof body, "TKSECRET=%s\n", SEC);
        lobby(body);
        xr_hello_make(hh, 1000);
        react_receiver(SEC);
        CHECK(xr_hello_seen(ep(AWAY, 52000), hh, XR_HELLO_LEN, &id, 900) == XR_HELLO_NEW);
    }
    CHECK(xr_ping_next(1000, &id, b) && id == SA && b[5] == XR_PING_PING);
    seq = ping_seq(b);
    CHECK(!xr_ping_next(1500, &id, b));

    /* the answer: the time it took */
    signed_ping(in, SA, SEC, XR_PING_ANSWER, seq);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1120) == XR_PING_MEASURED && ms == 120);
    CHECK(xr_peer_rtt(SA, 1120, &r) && r.samples == 1 && r.med_ms == 120 && r.min_ms == 120 && r.sent == 1 && r.lost == 0);
    /* answered once only, even from a second path; a sequence never sent is nothing */
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1130) == XR_PING_NOTHING);
    signed_ping(in, SA, SEC, XR_PING_ANSWER, seq + 100);
    CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, 1130) == XR_PING_NOTHING);

    /* a spread: the median holds a spike off, min is the floor */
    for (i = 0; i < 8; i++) {
        unsigned t = 2000 + (unsigned)i * 1000;
        unsigned w = i == 3 ? 900 : 100 + (unsigned)i;
        CHECK(xr_ping_next(t, &id, b));
        signed_ping(in, SA, SEC, XR_PING_ANSWER, ping_seq(b));
        CHECK(xr_ping_seen(ep(AWAY, 52000), in, XR_PING_LEN, &id, reply, &ms, t + w) == XR_PING_MEASURED);
    }
    CHECK(xr_peer_rtt(SA, 9500, &r) && r.samples == 9 && r.min_ms == 100 && r.med_ms == 105);

    /* pings with no answer are lost, once they are old enough */
    CHECK(xr_ping_next(10000, &id, b));
    CHECK(xr_ping_next(11000, &id, b));
    CHECK(xr_peer_rtt(SA, 11500, &r) && r.lost == 0);
    CHECK(xr_peer_rtt(SA, 14500, &r) && r.lost == 2 && r.sent == 11);

    /* a new game: measurements gone, who can answer stays */
    xr_game_reset();
    CHECK(!xr_peer_rtt(SA, 15000, &r));
    game_of(SA, SB, SEC);
    CHECK(xr_ping_next(15000, &id, b) && id == SA);
    /* a player who left gets no more pings */
    xr_game_reset();
    game_of(SB, ip(10, 64, 0, 3), SEC);
    CHECK(!xr_ping_next(20000, &id, b));
    /* a new session forgets everything */
    game_of(SA, SB, SEC);
    xr_session_reset();
    game_of(SA, SB, SEC);
    CHECK(!xr_ping_next(30000, &id, b));
}

int main(void)
{
    test_choice();
    test_force();
    test_unknown_public();
    test_hello();
    test_hello_due();
    test_parse();
    test_hello_trust();
    test_hello_secret();
    test_paths();
    test_misc();
    test_same_router_members();
    test_eviction();
    test_learned_full();
    test_game_reset();
    test_members_only();
    test_synthetic();
    test_session_reset();
    test_path_claim();
    test_member_hellos();
    test_shared_router_source();
    test_game47_host();
    test_shared_public_source();
    test_loopback_route();
    test_remote_shared_nat();
    test_router_port_reuse();
    test_loopback_net_id();
    test_lan_lobby_at();
    test_lan_lobby_host();
    test_pick_lan();
    test_lan_denied();
    test_react();
    test_vote();
    test_ping();
    test_react_rate();
    test_relay();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

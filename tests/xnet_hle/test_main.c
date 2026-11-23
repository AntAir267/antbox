/* xnet_hle test: see CMakeLists.txt. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xnet_hle.h"
#include "xnet_lobby.h"
#include "xnet_route.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* UPnP stand-in: what xnet_hle asked for */
static int s_host = -1, s_host_calls;
void xnet_upnp_start(unsigned port) { (void)port; }
void xnet_upnp_port(unsigned port) { (void)port; }
void xnet_upnp_host(int on) { s_host = on; s_host_calls++; }

/* the title's sockaddr: family (LE), port (BE), address */
static void xsa(uint8_t *x, uint32_t ip_be, uint16_t port)
{
    uint16_t fam = 2, p = htons(port);
    memset(x, 0, 16);
    memcpy(x, &fam, 2);
    memcpy(x + 2, &p, 2);
    memcpy(x + 4, &ip_be, 4);
}

static int listener(uint16_t port)
{
    struct sockaddr_in a;
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_within(int l, int ms)
{
    struct pollfd p;
    p.fd = l;
    p.events = POLLIN;
    if (poll(&p, 1, ms) != 1)
        return -1;
    return accept(l, NULL, NULL);
}

/* a lobby packet as the server sends it (status "" = success) */
static int pkt(uint8_t *b, const char *cmd, const char *st, const char *body)
{
    int bl = body && *body ? (int)strlen(body) + 1 : 0, n = 12 + bl;
    memcpy(b, cmd, 4);
    memset(b + 4, 0, 4);
    if (st && *st)
        memcpy(b + 4, st, 4);
    b[8] = (uint8_t)(n >> 24); b[9] = (uint8_t)(n >> 16); b[10] = (uint8_t)(n >> 8); b[11] = (uint8_t)n;
    if (bl)
        memcpy(b + 12, body, (size_t)bl);
    return n;
}

/* the server sends; the title reads it all (blocking socket) */
static void feed(int srv, uint32_t s, const uint8_t *b, int n)
{
    uint8_t r[4096];
    int got = 0, e = 0, k;
    CHECK(send(srv, b, (size_t)n, 0) == n);
    while (got < n) {
        k = xnet_hle_recv(s, r, (int)sizeof r, 0, &e);
        if (k <= 0)
            break;
        got += k;
    }
    CHECK(got == n);
}

static void serve(int srv, uint32_t s, const char *cmd, const char *st, const char *body)
{
    uint8_t b[4096];
    feed(srv, s, b, pkt(b, cmd, st, body));
}

static int game_ident(void)
{
    char g[256];
    const char *p;
    xnet_hle_game_state(g, sizeof g);
    p = strstr(g, "game=");
    return p ? atoi(p + 5) : -99;
}

/* BO-349: this machine's address, for a LAN lobby run in-process (its
 * beacon): the pinned one, none for loopback, never a loopback or link-local
 * one with the route probes off. Called before anything else here: it must
 * not start the layer (main's xnet_hle_on() == 1 below checks that). */
#ifndef XNET_HLE_HAS_LAN_ADDR
static uint32_t xnet_hle_lan_addr(void) { return 0; }   /* before BO-349 */
#endif
static void test_lan_addr(void)
{
    uint32_t a;
    struct in_addr ia;
    setenv("TAKEDAWN_LOCAL_IP", "10.1.2.3", 1);
    CHECK(xnet_hle_lan_addr() == inet_addr("10.1.2.3"));
    setenv("TAKEDAWN_LOCAL_IP", "127.0.0.1", 1);
    CHECK(xnet_hle_lan_addr() == 0);
    unsetenv("TAKEDAWN_LOCAL_IP");
    setenv("TAKEDAWN_NET_NO_ROUTE", "1", 1);
    a = xnet_hle_lan_addr();
    ia.s_addr = a;
    printf("no route probes: %s\n", a ? inet_ntoa(ia) : "none");
    CHECK(a == 0 || ((ntohl(a) >> 24) != 127 && (ntohl(a) >> 16) != 0xA9FEu));
    unsetenv("TAKEDAWN_NET_NO_ROUTE");
    a = xnet_hle_lan_addr();
    ia.s_addr = a;
    printf("with the route probes: %s\n", a ? inet_ntoa(ia) : "none");
    CHECK(a == 0 || (ntohl(a) >> 24) != 127);
}

/* BO-351: Burnout 3's lobby send queue (0x00221320 and its callers): packets
 * are appended at used, sent with send(buf + sent, used - sent), sent moves by
 * what send returned; before the next append the sent packets are dropped by
 * walking whole packets (their length at +8, big-endian) up to sent. A send
 * that says more than was queued leaves sent past used; the walk then reads a
 * length of 0 past the last packet and never ends. Here it stops after 64
 * steps and says so. */
typedef struct { uint8_t b[512]; int used, sent; } title_queue;

static int tq_drop_sent(title_queue *q)
{
    int at = 0, len = 0, steps = 0;
    while (at < q->sent) {
        len = q->b[at + 8] << 24 | q->b[at + 9] << 16 | q->b[at + 10] << 8 | q->b[at + 11];
        if (++steps > 64)
            return -1;   /* guest-main would spin here for good */
        at += len;
    }
    if (at > q->sent)
        at -= len;
    memmove(q->b, q->b + at, (size_t)(q->used - at));
    memset(q->b + q->used - at, 0, (size_t)at);
    q->used -= at;
    q->sent -= at;
    return 0;
}

static int tq_append(title_queue *q, const uint8_t *p, int n)
{
    if (tq_drop_sent(q) < 0)
        return -1;
    memcpy(q->b + q->used, p, (size_t)n);
    q->used += n;
    return 0;
}

/* the int044 game's send hook: it tagged the packet itself and handed the
 * tagged copy to xnet_hle_send */
static int s_old_wrapper;

static int tag_gset(const uint8_t *pkt, int n, uint8_t *out, int cap)
{
    if (n < 12 || memcmp(pkt, "gset", 4))
        return 0;
    return xl_add_line(pkt, n, "TKWHY=nolink", out, cap);
}

static void tq_flush(title_queue *q, uint32_t s, int srv)
{
    uint8_t r[1024], t[1024];
    const uint8_t *p = q->b + q->sent;
    int n = q->used - q->sent, e = 0, k, m;
    if (n <= 0)
        return;
    if (s_old_wrapper && (m = tag_gset(p, n, t, (int)sizeof t)) > 0) {
        p = t;
        n = m;
    }
    k = xnet_hle_send(s, p, n, 0, &e);
    if (k > 0)
        q->sent += k;
    k = (int)recv(srv, r, sizeof r, 0);
    (void)k;
}

/* T3: a failed non-blocking connect is in the except set only, as Winsock
 * reports it (not writable, not readable), and SO_ERROR says why, once; a
 * connect that succeeds is writable and not in the except set. */
static int free_port(int listen_on)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &al)) {
        close(fd);
        return -1;
    }
    port = ntohs(a.sin_port);
    if (listen_on && listen(fd, 4) == 0)
        return fd << 16 | port;
    close(fd);
    return port;
}

static void test_nb_connect(void)
{
    uint8_t sa[16];
    uint32_t s, nb = 1, wr[65], ex[65], rd[65];
    int32_t tv[2] = { 0, 500000 }, v = -1, vl = 4;
    int e = 0, port, lfd, round;
    /* refused: a port nobody listens on */
    for (round = 0; round < 2; round++) {
        port = free_port(0);
        CHECK(port > 0);
        s = xnet_hle_socket(2, 1, 0, &e);
        CHECK(xnet_hle_ioctlsocket(s, 0x8004667Eu, &nb, &e) == 0);
        xsa(sa, htonl(0x7F000001), (uint16_t)port);
        CHECK(xnet_hle_connect(s, sa, 16, &e) == -1 && e == 10035);   /* WSAEWOULDBLOCK */
        rd[0] = 1; rd[1] = s;
        wr[0] = 1; wr[1] = s;
        ex[0] = 1; ex[1] = s;
        /* round 0: write + except; round 1: read, write and except at once */
        CHECK(xnet_hle_select(round ? rd : NULL, wr, ex, tv, &e) == 1);
        CHECK(wr[0] == 0 && ex[0] == 1 && ex[1] == s);
        if (round)
            CHECK(rd[0] == 0);
        /* a second select says the same */
        wr[0] = 1; wr[1] = s;
        ex[0] = 1; ex[1] = s;
        CHECK(xnet_hle_select(NULL, wr, ex, tv, &e) == 1 && wr[0] == 0 && ex[0] == 1);
        v = -1; vl = 4;
        CHECK(xnet_hle_getsockopt(s, 0xFFFF, 0x1007, &v, &vl, &e) == 0 && v == 10061);   /* WSAECONNREFUSED */
        v = -1; vl = 4;
        CHECK(xnet_hle_getsockopt(s, 0xFFFF, 0x1007, &v, &vl, &e) == 0 && v == 0);       /* read clears it */
        xnet_hle_closesocket(s, &e);
    }
    /* accepted: writable, not in except */
    lfd = free_port(1);
    CHECK(lfd > 0xFFFF);
    s = xnet_hle_socket(2, 1, 0, &e);
    CHECK(xnet_hle_ioctlsocket(s, 0x8004667Eu, &nb, &e) == 0);
    xsa(sa, htonl(0x7F000001), (uint16_t)(lfd & 0xFFFF));
    xnet_hle_connect(s, sa, 16, &e);
    wr[0] = 1; wr[1] = s;
    ex[0] = 1; ex[1] = s;
    CHECK(xnet_hle_select(NULL, wr, ex, tv, &e) == 1 && wr[0] == 1 && ex[0] == 0);
    v = -1; vl = 4;
    CHECK(xnet_hle_getsockopt(s, 0xFFFF, 0x1007, &v, &vl, &e) == 0 && v == 0);
    xnet_hle_closesocket(s, &e);
    close(lfd >> 16);
}

/* T1: the lobby stream keeps its packets whatever the title's receive sizes
 * (held bytes plus a chunk past the 16 KB buffer, a chunk past it on its
 * own), and a bad header ends the connection instead of misreading it. */
static int big_mgm(uint8_t *b, int ident, int size)
{
    char body[8192];
    int o = snprintf(body, sizeof body, "IDENT=%d\nCOUNT=1\nOPPO0=Me\nLADDR0=127.0.0.1\nPAD=", ident);
    memset(body + o, 'x', (size_t)(size - 12 - 1 - 1 - o));
    o = size - 12 - 2;
    body[o] = '\n';
    body[o + 1] = 0;
    return pkt(b, "+mgm", "", body);
}

static void recv_exact(uint32_t s, uint8_t *r, int want)
{
    int e = 0;
    CHECK(xnet_hle_recv(s, r, want, 0, &e) == want);
}

static void test_lobby_stream(int ls, uint32_t lo)
{
    static uint8_t b[32768], r[32768];
    uint8_t sa[16];
    uint32_t s, set[65];
    int32_t tv[2] = { 1, 0 };
    int e = 0, srv, n = 0, i;
    struct pollfd pf;
    s = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, lo, 21810);
    CHECK(xnet_hle_connect(s, sa, 16, &e) == 0);
    srv = accept_within(ls, 2000);
    CHECK(srv >= 0);
    if (srv < 0)
        return;
    /* three 6000-byte packets read as one 18000-byte chunk */
    for (i = 0; i < 3; i++)
        n += big_mgm(b + n, 60 + i, 6000);
    CHECK(n == 18000 && send(srv, b, (size_t)n, 0) == n);
    usleep(100000);
    recv_exact(s, r, n);
    CHECK(game_ident() == 62);
    /* four: 10000 (one packet and 4000 held), then 14000 (18000 together) */
    for (n = 0, i = 0; i < 4; i++)
        n += big_mgm(b + n, 70 + i, 6000);
    CHECK(n == 24000 && send(srv, b, (size_t)n, 0) == n);
    usleep(100000);
    recv_exact(s, r, 10000);
    CHECK(game_ident() == 70);
    recv_exact(s, r, 14000);
    CHECK(game_ident() == 73);
    /* still in step: a small packet after */
    serve(srv, s, "+mgm", "", "IDENT=74\nCOUNT=1\nOPPO0=Me\nLADDR0=127.0.0.1\n");
    CHECK(game_ident() == 74);
    /* a length under 12: the connection ends, both ends see it */
    memcpy(b, "+mgm\0\0\0\0\0\0\0\5", 12);
    CHECK(send(srv, b, 12, 0) == 12);
    recv_exact(s, r, 12);
    set[0] = 1;
    set[1] = s;
    CHECK(xnet_hle_select(set, NULL, NULL, tv, &e) == 1 && set[0] == 1);
    if (set[0] == 1)
        CHECK(xnet_hle_recv(s, r, 64, 0, &e) == 0);
    pf.fd = srv;
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 1000) == 1 && recv(srv, r, 64, 0) == 0);
    CHECK(game_ident() == 74);
    xnet_hle_closesocket(s, &e);
    close(srv);
}

/* T6: a select whose only socket is held for the TCP hold sees the data
 * once the hold ends, not after its whole timeout; a select with nothing to
 * wait on and no timeout fails (WSAEINVAL) instead of waiting for ever. */
static uint64_t ms_now(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_usec / 1000;
}

/* a connected TCP pair: the title's end and the host's end */
static int tcp_pair(uint32_t *s, int *peer)
{
    int e = 0, lfd = free_port(1);
    uint8_t sa[16];
    if (lfd <= 0xFFFF)
        return -1;
    *s = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, htonl(0x7F000001), (uint16_t)(lfd & 0xFFFF));
    if (xnet_hle_connect(*s, sa, 16, &e) != 0) {
        close(lfd >> 16);
        return -1;
    }
    *peer = accept_within(lfd >> 16, 2000);
    close(lfd >> 16);
    return *peer >= 0 ? 0 : -1;
}

static void test_select_hold(void)
{
    uint32_t s, set[65], empty[65];
    int32_t tv[2] = { 1, 0 };
    int e = 0, peer, st = 0;
    uint64_t t0;
    pid_t c;
    CHECK(tcp_pair(&s, &peer) == 0);
    CHECK(send(peer, "data", 4, 0) == 4);
    usleep(20000);
    CHECK(xnet_hle_send(s, "req", 3, 0, &e) == 3);   /* held for 50 ms from here */
    set[0] = 1;
    set[1] = s;
    t0 = ms_now();
    CHECK(xnet_hle_select(set, NULL, NULL, tv, &e) == 1 && set[0] == 1);
    CHECK(ms_now() - t0 < 400);
    xnet_hle_closesocket(s, &e);
    close(peer);
    /* nothing to wait on, no timeout: a child, so a hang is a failure, not a stuck test */
    c = fork();
    if (c == 0) {
        empty[0] = 0;
        _exit(xnet_hle_select(empty, NULL, NULL, NULL, &e) == -1 && e == 10022 ? 0 : 1);
    }
    for (t0 = ms_now(); ms_now() - t0 < 1000; usleep(10000))
        if (waitpid(c, &st, WNOHANG) == c)
            break;
    if (ms_now() - t0 >= 1000) {
        kill(c, SIGKILL);
        waitpid(c, &st, 0);
        st = -1;
    }
    CHECK(st == 0);
}

/* T5: a title UDP port already bound is in use for a second socket, as on
 * the Xbox (Winsock without SO_REUSEADDR): a re-bound race port never sits
 * beside a leaked one and splits its traffic. A TCP port in TIME_WAIT can
 * still be listened on again. */
static void test_bind_twice(void)
{
    uint8_t sa[16];
    uint32_t u1, u2, l1, l2, a;
    int e = 0, port, c, i;
    uint16_t tp = 0;
    struct sockaddr_in me;
    int32_t len = 16;
    for (i = 0; i < 20 && !tp; i++) {   /* a free title port (its host port is +offset) */
        uint16_t t = (uint16_t)(4000 + (getpid() % 3000) + i * 13);
        u1 = xnet_hle_socket(2, 2, 17, &e);
        xsa(sa, 0, t);
        if (xnet_hle_bind(u1, sa, 16, &e) == 0)
            tp = t;
        else
            xnet_hle_closesocket(u1, &e);
    }
    CHECK(tp != 0);
    u2 = xnet_hle_socket(2, 2, 17, &e);
    xsa(sa, 0, tp);
    e = 0;
    CHECK(xnet_hle_bind(u2, sa, 16, &e) == -1 && e == 10048);   /* WSAEADDRINUSE */
    xnet_hle_closesocket(u2, &e);
    xnet_hle_closesocket(u1, &e);
    /* TCP: listen, a connection closed from our side (TIME_WAIT), listen again */
    l1 = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, htonl(0x7F000001), 0);
    CHECK(xnet_hle_bind(l1, sa, 16, &e) == 0 && xnet_hle_listen(l1, 2, &e) == 0);
    CHECK(xnet_hle_getsockname(l1, sa, &len, &e) == 0);
    port = sa[2] << 8 | sa[3];
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(0x7F000001);
    me.sin_port = htons((uint16_t)port);
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&me, sizeof me) == 0);
    len = 16;
    a = xnet_hle_accept(l1, sa, &len, &e);
    xnet_hle_closesocket(a, &e);   /* the active close: TIME_WAIT on the listener's port */
    usleep(20000);
    close(c);
    xnet_hle_closesocket(l1, &e);
    l2 = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, htonl(0x7F000001), (uint16_t)port);
    CHECK(xnet_hle_bind(l2, sa, 16, &e) == 0);
    xnet_hle_closesocket(l2, &e);
}

/* T5: WSACleanup / XNetCleanup close what the title left open: the table
 * is back where it was and the old handles are gone. */
static void test_close_all(void)
{
    uint32_t h[3];
    int e = 0, base = xnet_hle_socket_count(), i;
    CHECK(xnet_hle_close_all() == base);   /* nothing of this test's own yet */
    base = 0;
    for (i = 0; i < 3; i++)
        h[i] = xnet_hle_socket(2, i ? 2 : 1, 0, &e);
    CHECK(xnet_hle_socket_count() == base + 3);
    CHECK(xnet_hle_close_all() == 3);
    CHECK(xnet_hle_socket_count() == base);
    e = 0;
    CHECK(xnet_hle_closesocket(h[0], &e) == -1 && e == 10038);   /* WSAENOTSOCK */
    CHECK(xnet_hle_close_all() == 0);
}

/* T2: a title that sends more often than the TCP hold (50 ms) still gets
 * what waits for it (here within 300 ms), and the hold still keeps a reply
 * back for 50 ms after a single send. */
static void test_hold_starve(void)
{
    uint32_t s, set[65], nb = 1;
    int32_t tv0[2] = { 0, 0 };
    int e = 0, peer, seen = -1;
    uint8_t r[64];
    uint64_t t0;
    CHECK(tcp_pair(&s, &peer) == 0);
    CHECK(xnet_hle_ioctlsocket(s, 0x8004667Eu, &nb, &e) == 0);
    /* a single send: the reply is held, then readable */
    CHECK(xnet_hle_send(s, "req", 3, 0, &e) == 3);
    CHECK(send(peer, "rep", 3, 0) == 3);
    usleep(10000);
    set[0] = 1; set[1] = s;
    CHECK(xnet_hle_select(set, NULL, NULL, tv0, &e) == 0);
    CHECK(xnet_hle_recv(s, r, sizeof r, 0, &e) == -1 && e == 10035);
    usleep(60000);
    set[0] = 1; set[1] = s;
    CHECK(xnet_hle_select(set, NULL, NULL, tv0, &e) == 1);
    CHECK(xnet_hle_recv(s, r, sizeof r, 0, &e) == 3);
    /* a request every 30 ms, each followed by a look: the data gets through */
    CHECK(send(peer, "data", 4, 0) == 4);
    for (t0 = ms_now(); ms_now() - t0 < 600 && seen < 0; usleep(30000)) {
        CHECK(xnet_hle_send(s, "req", 3, 0, &e) == 3);
        set[0] = 1; set[1] = s;
        if (xnet_hle_select(set, NULL, NULL, tv0, &e) == 1)
            seen = (int)(ms_now() - t0);
    }
    CHECK(seen >= 0 && seen < 300);
    if (seen >= 0)
        CHECK(xnet_hle_recv(s, r, sizeof r, 0, &e) == 4);
    /* read: the next send holds again */
    CHECK(xnet_hle_send(s, "req", 3, 0, &e) == 3);
    CHECK(send(peer, "rep", 3, 0) == 3);
    usleep(10000);
    set[0] = 1; set[1] = s;
    CHECK(xnet_hle_select(set, NULL, NULL, tv0, &e) == 0);
    xnet_hle_closesocket(s, &e);
    close(peer);
}

int main(void)
{
    uint8_t sa[16], b[4096];
    char line[300];
    int lport = 0, ls = -1, bs = -1, srv, bud, e = 0, n, i;
    uint32_t lo = htonl(0x7F000001), s1, s2;
    /* a lobby port with a free "buddy" port next to it */
    for (i = 0; i < 50 && ls < 0; i++) {
        lport = (uint16_t)(42000 + (getpid() % 2000) + i * 7);
        ls = listener((uint16_t)lport);
        if (ls >= 0 && (bs = listener((uint16_t)(lport + 1))) < 0) {
            close(ls);
            ls = -1;
        }
    }
    if (ls < 0) {
        printf("no free ports\n");
        return 1;
    }
    test_lan_addr();
    snprintf(line, sizeof line, "127.0.0.1:%d", lport);
    setenv("TAKEDAWN_ONLINE", "internet", 1);
    setenv("TAKEDAWN_LOBBY", line, 1);
    setenv("TAKEDAWN_LOCAL_IP", "127.0.0.1", 1);
    CHECK(xnet_hle_on() == 1);
    test_nb_connect();
    test_select_hold();
    test_bind_twice();
    test_hold_starve();

    /* H4: the title's 21810 is the lobby, moved to the port set */
    s1 = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, lo, 21810);
    CHECK(xnet_hle_connect(s1, sa, 16, &e) == 0);
    srv = accept_within(ls, 2000);
    CHECK(srv >= 0);
    /* not online yet: the lobby has not answered on it */
    CHECK(!xnet_hle_session_up());
    /* the server's buddy port (its port + 1) stays as it is and is not the lobby */
    s2 = xnet_hle_socket(2, 1, 0, &e);
    xsa(sa, lo, (uint16_t)(lport + 1));
    CHECK(xnet_hle_connect(s2, sa, 16, &e) == 0);
    bud = accept_within(bs, 2000);
    CHECK(bud >= 0);
    CHECK(accept_within(ls, 100) < 0);   /* nothing more reached the lobby port */

    /* H1: addr goes out with TKID=1 (and TKNAME), its length fixed; the reply's TKADDR is ours */
    n = pkt(b, "addr", "", "ADDR=127.0.0.1\nPORT=0\n");
    CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
    {
        uint8_t r[512];
        int k = (int)recv(srv, r, sizeof r, 0), len = k >= 12 ? (r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]) : -1;
        /* BO-343: and the gamertag, for the server's address lease */
        CHECK(k == n + 7 + 13 && len == k && strstr((char *)r + 12, "PORT=0\nTKID=1\nTKNAME=Racer\n") != NULL);
    }
    /* BO-340: an old server sends no TKSID: no ids, nothing new in the state line */
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\n");
    CHECK(xr_self_synthetic() == inet_addr("10.64.0.7"));
    {
        char sid[32] = "x", gid[32] = "x";
        CHECK(xnet_hle_ids(sid, sizeof sid, gid, sizeof gid) == 0 && !sid[0] && !gid[0]);
        xnet_hle_game_state(line, sizeof line);
        CHECK(!strstr(line, "sid=") && !strstr(line, "gid="));
        /* a TKSID that is not hex digits is not taken */
        serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\nTKSID=not-hex!\n");
        CHECK(xnet_hle_ids(sid, sizeof sid, gid, sizeof gid) == 0);
        serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\nTKSID=d8fa84fd\n");
        CHECK(xnet_hle_ids(sid, sizeof sid, gid, sizeof gid) == 1 && !strcmp(sid, "d8fa84fd") && !gid[0]);
        xnet_hle_game_state(line, sizeof line);
        CHECK(strstr(line, " sid=d8fa84fd") && !strstr(line, "gid="));
    }

    /* H4: the buddy's bytes never reach the lobby's stream: a record split
     * over two reads with buddy bytes read in between */
    {
        const char *rec = "IDENT=21\nNAME=Host\nCOUNT=2\nOPPO0=Host\nLADDR0=127.0.0.1\nADDR0=127.0.0.1\n"
                          "OPPO1=Guest\nLADDR1=10.64.0.8\nTKRADDR1=192.168.1.100\nADDR1=85.66.10.7\n"
                          "TKSECRET=00112233445566778899aabbccddeeff\nTKGID=aec4ed3eaf2b\n";
        int m = pkt(b, "+mgm", "", rec), k;
        uint8_t junk[64], r[64];
        memset(junk, 0x7F, sizeof junk);
        feed(srv, s1, b, 20);
        CHECK(send(bud, junk, sizeof junk, 0) == (int)sizeof junk);
        k = xnet_hle_recv(s2, r, sizeof r, 0, &e);
        CHECK(k == (int)sizeof junk);
        feed(srv, s1, b + 20, m - 20);
        CHECK(game_ident() == 21 && xr_has_secret() && xr_is_member(inet_addr("10.64.0.8")));
    }
    xnet_hle_game_state(line, sizeof line);
    CHECK(strstr(line, "peers=1/32 learned=0/64 members=1") != NULL);
    CHECK(strstr(line, " sid=d8fa84fd gid=aec4ed3eaf2b") != NULL);

    /* M3: the lobby's empty record ends the game */
    serve(srv, s1, "+mgm", "", "IDENT=0\nNAME=\nHOST=\nCOUNT=0");
    CHECK(game_ident() == -1 && !xr_has_secret() && !xr_is_member(inet_addr("10.64.0.8")));
    /* signed in, no game: still online (the overlay does not pause the title) */
    CHECK(xnet_hle_session_up());
    {   /* the last game's id is kept for the bug report after the game is over */
        char sid[32], gid[32];
        xnet_hle_ids(sid, sizeof sid, gid, sizeof gid);
        CHECK(!strcmp(sid, "d8fa84fd") && !strcmp(gid, "aec4ed3eaf2b"));
    }

    /* M6: a gcre the server refuses opens no forwarding; one it answers does */
    s_host = -1;
    serve(srv, s1, "gcre", "full", "");
    CHECK(s_host == -1);
    serve(srv, s1, "gcre", "", "IDENT=22\nNAME=Host\nCOUNT=1\nOPPO0=Host\nLADDR0=127.0.0.1\nADDR0=127.0.0.1\nTKGID=0123456789ab\n");
    CHECK(s_host == 1 && game_ident() == 22);
    {
        char gid[32];
        xnet_hle_ids(NULL, 0, gid, sizeof gid);
        CHECK(!strcmp(gid, "0123456789ab"));
    }
    /* a new game in a member record: the last one's state goes first */
    serve(srv, s1, "+mgm", "", "IDENT=22\nCOUNT=2\nOPPO0=Host\nLADDR0=127.0.0.1\nOPPO1=G\nLADDR1=10.64.0.9\n"
          "TKRADDR1=192.168.1.9\nADDR1=85.66.10.9\nTKSECRET=ffeeddccbbaa99887766554433221100\n");
    CHECK(xr_is_member(inet_addr("10.64.0.9")) && xr_has_secret());
    /* M3: glea's answer ends it too, and the forwarding with it */
    serve(srv, s1, "glea", "", "");
    CHECK(s_host == 0 && game_ident() == -1 && !xr_has_secret());
    serve(srv, s1, "gcre", "", "IDENT=23\nCOUNT=1\nOPPO0=Host\nLADDR0=127.0.0.1\n");
    CHECK(s_host == 1);
    serve(srv, s1, "gdel", "", "");
    CHECK(s_host == 0 && game_ident() == -1);

    /* L6: "full" inside a name is not an answer; COUNT= inside a value is not a count */
    xnet_hle_message_get(line, sizeof line);
    {
        int seq = xnet_hle_message_get(NULL, 0);
        serve(srv, s1, "+gam", "", "IDENT=5\nNAME=gcrefull\nCOUNT=1\n");
        CHECK(xnet_hle_message_get(NULL, 0) == seq);
        serve(srv, s1, "gjoi", "full", "");
        CHECK(xnet_hle_message_get(line, sizeof line) == seq + 1 && strstr(line, "That game is full") != NULL);
    }

    /* BO-335 + L2: a 15-character code goes out with gjoi, then clears on move */
    xnet_hle_set_code("ABCDEFGHJKMNPQR");
    CHECK(!strcmp(xnet_hle_code(), "ABCDEFGHJKMNPQR"));
    n = pkt(b, "gjoi", "", "IDENT=23\n");
    CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
    {
        uint8_t r[512];
        int k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k > n && strstr((char *)r + 12, "TKCODE=ABCDEFGHJKMNPQR\n") != NULL);
    }
    xnet_hle_set_code("");
    n = pkt(b, "move", "", "IDENT=1\n");
    CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
    {
        uint8_t r[512];
        int k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k > n && strstr((char *)r + 12, "TKCODE=\n") != NULL);
    }

    /* BO-351: a host's kick goes out with the game's TKWHY line, and the title
     * is told the length it queued: its queue stays whole and the next append
     * finds the end of the kick. */
    {
        title_queue q;
        uint8_t r[512];
        int k;
        memset(&q, 0, sizeof q);
        n = pkt(b, "gset", "", "NAME=Host1\nKICK=Mac1\n");
        CHECK(n == 0x22);
        /* the int044 way, for the record: the tagged copy's length came back
         * and the queue walk ran away on the next packet */
        s_old_wrapper = 1;
        CHECK(tq_append(&q, b, n) == 0);
        tq_flush(&q, s1, srv);
        CHECK(q.sent == n + 13 && q.used == n);
        n = pkt(b, "~png", "", "REF=1\nTIME=30\n");
        CHECK(tq_append(&q, b, n) == -1);
        s_old_wrapper = 0;

        /* the fix: the toolkit adds the game's line itself */
        memset(&q, 0, sizeof q);
        xnet_hle_set_lobby_tag(tag_gset);
        n = pkt(b, "gset", "", "NAME=Host1\nKICK=Mac1\n");
        CHECK(tq_append(&q, b, n) == 0);
        CHECK(xnet_hle_send(s1, q.b, q.used, 0, &e) == n);
        q.sent += n;
        k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k == n + 13 && r[11] == n + 13 && strstr((char *)r + 12, "KICK=Mac1\nTKWHY=nolink\n") != NULL);
        n = pkt(b, "~png", "", "REF=1\nTIME=30\n");
        CHECK(tq_append(&q, b, n) == 0 && q.used == n && q.sent == 0);
        tq_flush(&q, s1, srv);
        CHECK(q.sent == q.used);
        /* other packets go as they are */
        n = pkt(b, "mesg", "", "PRIV=Mac1\nTEXT=Mac1\n");
        CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
        k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k == n && strstr((char *)r + 12, "TKWHY") == NULL);
        /* T8: a code went out, then was cleared, then a kick (tagged, no code
         * field): the next move still clears the code at the server */
        xnet_hle_set_code("KZ7MQ");
        n = pkt(b, "gjoi", "", "IDENT=23\n");
        CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
        k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k > n && strstr((char *)r + 12, "TKCODE=KZ7MQ\n") != NULL);
        xnet_hle_set_code("");
        n = pkt(b, "gset", "", "NAME=Host1\nKICK=Mac1\n");
        CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
        k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k == n + 13 && strstr((char *)r + 12, "TKCODE") == NULL);
        n = pkt(b, "move", "", "IDENT=1\n");
        CHECK(xnet_hle_send(s1, b, n, 0, &e) == n);
        k = (int)recv(srv, r, sizeof r, 0);
        CHECK(k > n && strstr((char *)r + 12, "TKCODE=\n") != NULL);
        /* T9: two whole packets in one send are each tagged, each length
         * fixed; the title is told its own length */
        {
            int n1, n2, l1, l2;
            xnet_hle_set_code("KZ7MQ");
            n1 = pkt(b, "gsea", "", "IDENT=0\n");
            n2 = pkt(b + n1, "gjoi", "", "IDENT=23\n");
            CHECK(xnet_hle_send(s1, b, n1 + n2, 0, &e) == n1 + n2);
            usleep(50000);
            k = (int)recv(srv, r, sizeof r, 0);
            l1 = k >= 12 ? r[10] << 8 | r[11] : 0;
            CHECK(l1 > n1 && !memcmp(r, "gsea", 4) && strstr((char *)r + 12, "TKCODE=KZ7MQ\n") != NULL);
            l2 = k >= l1 + 12 ? r[l1 + 10] << 8 | r[l1 + 11] : 0;
            CHECK(l2 > n2 && !memcmp(r + l1, "gjoi", 4) && strstr((char *)r + l1 + 12, "TKCODE=KZ7MQ\n") != NULL);
            CHECK(k == l1 + l2);
            xnet_hle_set_code("");
            /* a kick with another packet behind it */
            n1 = pkt(b, "gset", "", "NAME=Host1\nKICK=Mac1\n");
            n2 = pkt(b + n1, "~png", "", "REF=1\nTIME=30\n");
            CHECK(xnet_hle_send(s1, b, n1 + n2, 0, &e) == n1 + n2);
            usleep(50000);
            k = (int)recv(srv, r, sizeof r, 0);
            CHECK(k == n1 + 13 + n2 && r[11] == n1 + 13 && strstr((char *)r + 12, "TKWHY=nolink\n") != NULL &&
                  !memcmp(r + n1 + 13, "~png", 4));
            /* half a packet goes as it is (the rest in the next send) */
            n1 = pkt(b, "gset", "", "NAME=Host1\nKICK=Mac1\n");
            CHECK(xnet_hle_send(s1, b, 10, 0, &e) == 10);
            CHECK(xnet_hle_send(s1, b + 10, n1 - 10, 0, &e) == n1 - 10);
            usleep(50000);
            k = (int)recv(srv, r, sizeof r, 0);
            CHECK(k == n1);
        }
        xnet_hle_set_lobby_tag(NULL);
    }

    /* BO-343: a new lobby session hands the synthetic addresses out again:
     * the last session's peers, paths and secret go; the same session does not */
    serve(srv, s1, "gjoi", "", "IDENT=30\nCOUNT=2\nOPPO0=Host\nLADDR0=10.64.0.5\nTKRADDR0=192.168.1.5\nADDR0=85.66.10.5\n"
          "OPPO1=Me\nLADDR1=127.0.0.1\nADDR1=127.0.0.1\nTKSECRET=00112233445566778899aabbccddeeff\n");
    CHECK(game_ident() == 30 && xr_is_member(inet_addr("10.64.0.5")) && xr_has_secret());
    n = xr_peer_count();
    CHECK(n >= 1);
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\nTKSID=d8fa84fd\n");   /* the same session */
    CHECK(xr_is_member(inet_addr("10.64.0.5")) && xr_has_secret() && xr_peer_count() == n);
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\nTKSID=0badc0de\n");   /* the lobby restarted */
    CHECK(!xr_is_member(inet_addr("10.64.0.5")) && !xr_has_secret() && xr_peer_count() == 0);
    xnet_hle_game_state(line, sizeof line);
    CHECK(strstr(line, "peers=0/32 learned=0/64 members=0") != NULL && strstr(line, " sid=0badc0de") != NULL);
    /* an old server without TKSID: our own address changing is a new session too */
    serve(srv, s1, "+mgm", "", "IDENT=31\nCOUNT=2\nOPPO0=Host\nLADDR0=10.64.0.5\nTKRADDR0=192.168.1.5\nADDR0=85.66.10.5\n"
          "OPPO1=Me\nLADDR1=127.0.0.1\nADDR1=127.0.0.1\n");
    CHECK(xr_is_member(inet_addr("10.64.0.5")));
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\n");
    CHECK(xr_is_member(inet_addr("10.64.0.5")));
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.8\n");
    CHECK(!xr_is_member(inet_addr("10.64.0.5")) && xr_peer_count() == 0);
    serve(srv, s1, "addr", "", "TKADDR=10.64.0.7\nTKSID=d8fa84fd\n");

    /* the title closes the lobby connection after gsta and races without it:
     * the game, its players and the forwarding stay */
    serve(srv, s1, "gcre", "", "IDENT=24\nCOUNT=2\nOPPO0=Host\nLADDR0=127.0.0.1\nOPPO1=G\nLADDR1=10.64.0.9\n");
    CHECK(s_host == 1);
    CHECK(xnet_hle_closesocket(s1, &e) == 0);
    CHECK(s_host == 1 && game_ident() == 24 && xr_is_member(inet_addr("10.64.0.9")));
    /* racing with no lobby connection: the game keeps the session up */
    CHECK(xnet_hle_session_up());
    /* M2: a closed handle is gone for every call */
    e = 0;
    CHECK(xnet_hle_send(s1, b, 12, 0, &e) == -1 && e == 10038);
    CHECK(xnet_hle_closesocket(s1, &e) == -1);
    xnet_hle_closesocket(s2, &e);
    close(srv);
    close(bud);

    test_lobby_stream(ls, lo);

    /* M1: an accepted socket is reported by select once data is there */
    {
        uint32_t l = xnet_hle_socket(2, 1, 0, &e), a;
        struct sockaddr_in me;
        socklen_t ml = sizeof me;
        uint8_t lsa[16];
        int32_t len = 16;
        int c;
        uint32_t set[65];
        int32_t tv[2] = { 1, 0 };
        xsa(lsa, lo, 0);
        CHECK(xnet_hle_bind(l, lsa, 16, &e) == 0 && xnet_hle_listen(l, 2, &e) == 0);
        CHECK(xnet_hle_getsockname(l, lsa, &len, &e) == 0);
        memset(&me, 0, sizeof me);
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = lo;
        memcpy(&me.sin_port, lsa + 2, 2);
        c = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(c, (struct sockaddr *)&me, ml) == 0);
        len = 16;
        a = xnet_hle_accept(l, lsa, &len, &e);
        CHECK(a != 0xFFFFFFFFu);
        CHECK(send(c, "hi", 2, 0) == 2);
        set[0] = 1;
        set[1] = a;
        CHECK(xnet_hle_select(set, NULL, NULL, tv, &e) == 1 && set[0] == 1);
        xnet_hle_closesocket(a, &e);
        xnet_hle_closesocket(l, &e);
        close(c);
    }
    close(ls);
    close(bs);
    test_close_all();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

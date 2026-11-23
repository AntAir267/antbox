/*
 * xnet_hle.c - the XDK's XNet (Winsock) and a signed-in XOnline, on host
 * sockets. See xnet_hle.h.
 *
 * The title's structures use the Xbox (Winsock) layouts, which are not the
 * host's everywhere: sockaddr_in is {u16 family, u16 port (BE), u32 addr,
 * 8 zero} (BSD hosts put a length byte first), fd_set is a count and an
 * array of handles, timeval is two 32-bit longs. Every one of them is
 * translated here, field by field.
 */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
typedef SOCKET host_sock;
#  define HOST_BAD INVALID_SOCKET
#  define host_close closesocket
#  define host_errno() WSAGetLastError()
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <pthread.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int host_sock;
#  define HOST_BAD (-1)
#  define host_close close
#  define host_errno() errno
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xnet_hle.h"
#include "xnet_route.h"
#include "xnet_netsim.h"
#include "xnet_lobby.h"

/* ── Winsock numbers as the title knows them ─────────────────────── */

#define X_AF_INET        2
#define X_SOCK_STREAM    1
#define X_SOCK_DGRAM     2
#define X_IPPROTO_TCP    6
#define X_IPPROTO_UDP    17
#define X_IPPROTO_VDP    254   /* XNet's voice/data protocol: UDP here */
#define X_SOL_SOCKET     0xFFFF
#define X_SO_REUSEADDR   0x0004
#define X_SO_BROADCAST   0x0020
#define X_SO_LINGER      0x0080
#define X_SO_SNDBUF      0x1001
#define X_SO_RCVBUF      0x1002
#define X_SO_ERROR       0x1007
#define X_SO_TYPE        0x1008
#define X_TCP_NODELAY    0x0001
#define X_FIONBIO        0x8004667Eu
#define X_FIONREAD       0x4004667Fu
#define X_MSG_PEEK       0x2

#define WSAEBADF         10009
#define WSAEFAULT        10014
#define WSAEINVAL        10022
#define WSAEMFILE        10024
#define WSAEWOULDBLOCK   10035
#define WSAEALREADY      10037
#define WSAENOTSOCK      10038
#define WSAEMSGSIZE      10040
#define WSAENOPROTOOPT   10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP    10045
#define WSAEAFNOSUPPORT  10047
#define WSAEADDRINUSE    10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN      10050
#define WSAENETUNREACH   10051
#define WSAECONNABORTED  10053
#define WSAECONNRESET    10054
#define WSAENOBUFS       10055
#define WSAEISCONN       10056
#define WSAENOTCONN      10057
#define WSAETIMEDOUT     10060
#define WSAECONNREFUSED  10061
#define WSAEHOSTUNREACH  10065

#define X_SOCKET_ERROR   (-1)
#define X_INVALID_SOCKET 0xFFFFFFFFu

/* ── settings ─────────────────────────────────────────────────────── */

static int s_on = -1, s_trace;
static uint32_t s_lobby_ip;       /* network order */
static uint16_t s_lobby_port;     /* host order; 0 = the title's own port */
static char s_lobby_name[128];    /* as the setting gives it (the messages name it) */
static int s_lobby_unresolved;    /* it has no IPv4 address */
static volatile int s_lobby_lazy; /* the name is not looked up yet (lobby_resolve) */
/* BO-335: the game code (online.code); empty = public games */
static char s_code[XL_CODE_MAX + 1];   /* under s_lock (the overlay sets it) */
static volatile int s_code_quiet;      /* the game says what a code did itself (xnet_hle_set_code_quiet) */
static volatile int s_join_quiet;      /* a "full" join is "not yet" (xnet_hle_set_join_quiet, BO-365) */
/* BO-336: what the server says about itself (TKONLINE, TKWAITING) */
static xl_status s_status;
/* the last record of our game: its players and their ready values (logged on change) */
static xl_game s_game;

/* What an in-process test driver reads of the lobby (xnet_hle_lobby_view) */
static void lock(void);
static void unlock(void);
static uint64_t now_ms(void);
static xnet_hle_lobby_view s_view = { .search_count = -1, .my_flag = -1, .mode = -1, .room = -1 };
static const char k_lv_cmd[XNET_LV_N][5] = {
    "gcre", "gjoi", "gsea", "gset", "gsta", "move", "glea", "gdel", "+mgm", "+ses", "+gam", "rank", "sviw", "+who"
};

/* BO-372: the browse states of the last game records (xnet_hle_browse_state) */
#define XL_BROWSE_N 32
static struct { int ident, state, secs; uint64_t at; } s_browse[XL_BROWSE_N];
static unsigned s_browse_next;

static int lv_kind(const uint8_t *hd)
{
    int i;
    for (i = 0; i < XNET_LV_N; i++)
        if (!memcmp(hd, k_lv_cmd[i], 4))
            return i;
    return -1;
}

/* the idx-th comma-separated number of KEY=a,b,c in body; -1 when not there */
static int lv_csv(const char *body, int n, const char *key, int idx)
{
    int kl = (int)strlen(key), i;
    for (i = 0; i + kl < n; i++) {
        if ((i == 0 || body[i - 1] == '\n' || body[i - 1] == '\t' || body[i - 1] == ' ' || body[i - 1] == '\r' ||
             (unsigned char)body[i - 1] < 32) && !memcmp(body + i, key, (size_t)kl) && body[i + kl] == '=') {
            int j = i + kl + 1, k = 0;
            while (k < idx && j < n && body[j] && body[j] != '\n') {
                if (body[j] == ',')
                    k++;
                j++;
            }
            if (k == idx && j < n && ((body[j] >= '0' && body[j] <= '9') || body[j] == '-'))
                return atoi(body + j);
            return -1;
        }
    }
    return -1;
}

/* under the lock: a request this copy sends to the lobby (whole packets) */
static void lv_out(const uint8_t *b, int n)
{
    while (n >= 12) {
        int plen = (int)((uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11]);
        int k = lv_kind(b), v;
        if (plen < 12 || plen > n)
            plen = n;
        if (k >= 0) {
            const char *body = (const char *)b + 12;
            int bl = plen - 12;
            s_view.sent[k]++;
            if (k == XNET_LV_GSET && (v = lv_csv(body, bl, "USERPARAMS", 2)) >= 0)
                s_view.my_flag = v;
            if (k == XNET_LV_GCRE && (v = lv_csv(body, bl, "PARAMS", 2)) >= 0)
                s_view.mode = v;
            if (k == XNET_LV_MOVE) {
                int id = -1;
                s_view.room = xl_key_int(body, bl, "IDENT", &id) ? id : 0;
            }
            if (k == XNET_LV_GJOI || k == XNET_LV_GCRE)
                s_view.my_flag = -1;
        }
        b += plen;
        n -= plen;
    }
}

/* under the lock: an answer or push from the lobby */
static void lv_in(const uint8_t *hd, const char *body, int bl)
{
    int k = lv_kind(hd), v;
    s_view.last_ms = now_ms();
    if (k < 0)
        return;
    s_view.got[k]++;
    if (hd[4] || hd[5] || hd[6] || hd[7])
        s_view.got_err[k]++;
    if (k == XNET_LV_GSEA) {
        char w[16];
        s_view.search_count = xl_key_int(body, bl, "COUNT", &v) ? v : 0;
        s_view.racing = xl_key_int(body, bl, "TKRACING", &v) ? v : 0;
        s_view.race_id = xl_key_int(body, bl, "TKRACEID", &v) ? v : 0;
        s_view.race_players = xl_key_int(body, bl, "TKRACEPLAYERS", &v) ? v : 0;
        s_view.race_max = xl_key_int(body, bl, "TKRACEMAX", &v) ? v : 0;
        s_view.race_secs = xl_key_int(body, bl, "TKRACESECS", &v) ? v : 0;
        xl_key_str(body, bl, "TKRACENAME", s_view.race_name, (int)sizeof s_view.race_name);
        s_view.wait_state = xl_key_str(body, bl, "TKWAITSTATE", w, (int)sizeof w) ? xl_wait_state(w) : XNET_WAIT_NONE;
        if (xl_key_str(body, bl, "TKWAITSTATE", w, (int)sizeof w))
            s_view.wait_seq++;
        s_view.wait_players = xl_key_int(body, bl, "TKWAITPLAYERS", &v) ? v : 0;
        s_view.wait_secs = xl_key_int(body, bl, "TKWAITSECS", &v) ? v : 0;
        s_view.wait_queue = xl_key_int(body, bl, "TKWAITQUEUE", &v) ? v : 0;
        xl_key_str(body, bl, "TKWAITNAME", s_view.wait_name, (int)sizeof s_view.wait_name);
    }
    if (k == XNET_LV_GAM) {
        char st[16];
        int id = 0, i, slot = -1, state = XNET_GS_NONE;
        if (!xl_key_int(body, bl, "IDENT", &id) || id <= 0)
            return;
        if (xl_key_str(body, bl, "TKSTATE", st, (int)sizeof st))
            state = !strcmp(st, "open") ? XNET_GS_OPEN : !strcmp(st, "racing") ? XNET_GS_RACING
                  : !strcmp(st, "full") ? XNET_GS_FULL : XNET_GS_NONE;
        for (i = 0; i < XL_BROWSE_N && slot < 0; i++)
            if (s_browse[i].ident == id)
                slot = i;
        if (slot < 0) {
            if (state == XNET_GS_NONE)
                return;                 /* a plain record: nothing to keep */
            slot = (int)(s_browse_next++ % XL_BROWSE_N);
        }
        s_browse[slot].ident = id;
        s_browse[slot].state = state;
        s_browse[slot].secs = xl_key_int(body, bl, "TKSECS", &v) ? v : 0;
        s_browse[slot].at = s_view.last_ms;
    }
}

int xnet_hle_browse_state(int ident, int *secs, unsigned long long *at_ms)
{
    int i, r = XNET_GS_NONE;
    if (secs)
        *secs = 0;
    if (at_ms)
        *at_ms = 0;
    lock();
    for (i = 0; i < XL_BROWSE_N; i++)
        if (ident > 0 && s_browse[i].ident == ident) {
            r = s_browse[i].state;
            if (secs)
                *secs = s_browse[i].secs;
            if (at_ms)
                *at_ms = s_browse[i].at;
            break;
        }
    unlock();
    return r;
}

int xnet_hle_lobby_view_get(xnet_hle_lobby_view *v)
{
    int i;
    if (!v)
        return 0;
    lock();
    *v = s_view;
    v->game = s_game.seen ? s_game.ident : -1;
    v->np = s_game.np;
    for (i = 0; i < XL_MAX_PLAYERS && i < 8; i++) {
        memcpy(v->name[i], s_game.name[i], sizeof v->name[i]);
        v->name[i][sizeof v->name[i] - 1] = 0;
        v->ready[i] = s_game.ready[i];
    }
    unlock();
    return 1;
}
static char s_gamertag[16] = "Racer";
static uint32_t s_local_ip;       /* network order */
static uint8_t s_mac[6];
/* The title's ports below 1024 (system link's 1000, 3074) need root on a
 * host: they move up by this much, on every copy alike. */
static uint16_t s_port_offset;
/* TAKEDAWN_LOCAL_IP pins this copy to one local address: binds to "any" go
 * there, and unbound sockets take it before they connect or send. Two
 * copies on one machine (127.0.0.2, 127.0.0.3) then look like two consoles. */
static int s_local_pinned;
/* TAKEDAWN_NET_FORCE_PUBLIC=1 (tests): peers through their public address
 * even on our own network (xnet_route.h). */
static int s_force_public;
/* TAKEDAWN_NET_ID gave this copy its title address: every socket reports it */
static int s_net_id;

static void lock(void);
static void unlock(void);

/* BO-340: the lobby's correlation ids. Under s_lock; "" = the server sent none (an old server). */
static char s_sid[XL_ID_MAX + 1];        /* TKSID: this lobby connection's session id */
static char s_last_gid[XL_ID_MAX + 1];   /* TKGID of the last game record, kept after the game ends */

/* The always-on online lines: a handful per session (sign-in, a game made,
 * joined or left, a peer linked, every message shown to the player, the
 * lobby closing the connection), never one per packet and no address. They
 * go through the real fputs, past the print gate (xr_log.h), so a release
 * build with diagnostic logging off still has them in the log a bug report
 * collects. xnet_hle_log is the gated trace and stays behind it. */
void xnet_hle_note(const char *fmt, ...)
{
    char line[400];
    size_t n;
    va_list ap;
    memcpy(line, "[NET] ", 6);
    va_start(ap, fmt);
    vsnprintf(line + 6, sizeof line - 8, fmt, ap);
    va_end(ap);
    n = strlen(line);
    line[n] = '\n';
    line[n + 1] = 0;
    (fputs)(line, stderr);
}

/* " sid=<hex>" for a note, or nothing when the server sent no id; the caller holds s_lock */
static const char *sid_tag(char *out, size_t n)
{
    if (s_sid[0])
        snprintf(out, n, " sid=%s", s_sid);
    else
        out[0] = 0;
    return out;
}

int xnet_hle_ids(char *sid, int sn, char *gid, int gn)
{
    lock();
    if (sid && sn > 0)
        snprintf(sid, (size_t)sn, "%s", s_sid);
    if (gid && gn > 0)
        snprintf(gid, (size_t)gn, "%s", s_last_gid);
    unlock();
    return s_sid[0] != 0;
}

void xnet_hle_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[XNET] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#ifdef _WIN32
static void net_startup(void)
{
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}
#else
static void net_startup(void) {}
#endif

static uint32_t resolve4(const char *host)
{
    struct addrinfo hints, *res = NULL;
    uint32_t ip = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;          /* the title's addresses are IPv4 */
    hints.ai_socktype = SOCK_STREAM;    /* one answer per address */
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
    return ip;
}

/* The host's own address on the way to the lobby: a UDP "connect" picks the
 * route without sending anything. Falls back to loopback. */
static uint32_t route_ip(uint32_t to)
{
    struct sockaddr_in sa;
    socklen_t n = sizeof sa;
    uint32_t ip = htonl(0x7F000001);
    host_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == HOST_BAD)
        return ip;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    sa.sin_addr.s_addr = to;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0 &&
        getsockname(s, (struct sockaddr *)&sa, &n) == 0 && sa.sin_addr.s_addr)
        ip = sa.sin_addr.s_addr;
    host_close(s);
    return ip;
}

/* BO-349: the IPv4 addresses of the interfaces that are up, loopback left
 * out, for when no route probe gives this machine's address (a LAN with no
 * gateway). At most max; the count. */
static int iface_addrs(uint32_t *out, int max)
{
    int n = 0;
#ifdef _WIN32
    ULONG len = 16384, r;
    IP_ADAPTER_ADDRESSES *buf = (IP_ADAPTER_ADDRESSES *)malloc(len), *ad;
    const ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (!buf)
        return 0;
    r = GetAdaptersAddresses(AF_INET, fl, NULL, buf, &len);
    if (r == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES *b2 = (IP_ADAPTER_ADDRESSES *)realloc(buf, len);
        if (!b2) {
            free(buf);
            return 0;
        }
        buf = b2;
        r = GetAdaptersAddresses(AF_INET, fl, NULL, buf, &len);
    }
    if (r == NO_ERROR)
        for (ad = buf; ad && n < max; ad = ad->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (u = ad->FirstUnicastAddress; u && n < max; u = u->Next)
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET)
                    out[n++] = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
        }
    free(buf);
#else
    struct ifaddrs *all = NULL, *i;
    if (getifaddrs(&all) != 0)
        return 0;
    for (i = all; i && n < max; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
            !(i->ifa_flags & IFF_LOOPBACK))
            out[n++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
    freeifaddrs(all);
#endif
    return n;
}

/* This machine's address on the network: the route to the lobby, else the
 * route out (a UDP "connect", nothing is sent), else (BO-349: a LAN party's
 * switch with no gateway) an up interface's address, a private one first.
 * Loopback when there is none. TAKEDAWN_NET_NO_ROUTE=1 (tests): as if no
 * route probe worked. say: log how it was found. */
static uint32_t lan_addr(uint32_t lobby, int say)
{
    const char *e = getenv("TAKEDAWN_NET_NO_ROUTE");
    uint32_t ip = htonl(0x7F000001);
    if (e && *e && *e != '0') {
        if (say)
            xnet_hle_log("route probes off (TAKEDAWN_NET_NO_ROUTE)");
    } else {
        if (lobby)
            ip = route_ip(lobby);
        if ((ntohl(ip) >> 24) == 127)
            ip = route_ip(htonl(0x08080808));
    }
    if ((ntohl(ip) >> 24) == 127) {
        uint32_t a[32], pick;
        int n = iface_addrs(a, 32), why;
        pick = xr_pick_lan(a, n, &why);
        if (pick) {
            struct in_addr ia;
            ia.s_addr = pick;
            if (say)
                xnet_hle_log("no route probe gave this machine's address (no gateway?): %s, %s of %d interface"
                             " address(es)", inet_ntoa(ia),
                             why == XR_LAN_PRIVATE ? "the first private one" : "no private one, the first", n);
            ip = pick;
        } else if (say) {
            xnet_hle_log("no network address found (%d interface address(es)): loopback only, other machines cannot"
                         " reach this copy", n);
        }
    }
    return ip;
}

/* A setting's value: the settings layer exports settings.toml as RECOMP_*
 * variables; the TAKEDAWN_* names also work (test scripts use them). */
static const char *cfg(const char *recomp, const char *takedawn)
{
    const char *e = getenv(recomp);
    if (e && *e)
        return e;
    e = getenv(takedawn);
    return (e && *e) ? e : NULL;
}

/* The lobby's name to its IPv4 address, the first time anything needs it. */
static void lobby_resolve(void)
{
    uint32_t ip;
    if (!s_lobby_lazy)
        return;
    ip = resolve4(s_lobby_name);
    if (!ip) {
        xnet_hle_log("lobby '%s' does not resolve to an IPv4 address; using 127.0.0.1", s_lobby_name);
        ip = htonl(0x7F000001);
        s_lobby_unresolved = 1;
    } else {
        struct in_addr a;
        a.s_addr = ip;
        xnet_hle_log("lobby '%s' is %s", s_lobby_name, inet_ntoa(a));
    }
    s_lobby_ip = ip;
    s_lobby_lazy = 0;
}

static void init_once(void)
{
    const char *e;
    uint32_t real_lan = 0;
    if (s_on >= 0)
        return;
    e = cfg("RECOMP_ONLINE", "TAKEDAWN_ONLINE");
    s_on = 0;
    if (e && (!strcmp(e, "internet") || !strcmp(e, "1")))
        s_on = 1;
    else if (e && !strcmp(e, "lan"))
        s_on = 2;
    if (!s_on)
        return;
    net_startup();
    e = getenv("TAKEDAWN_NET_TRACE");
    s_trace = e && *e && *e != '0';
    e = cfg("RECOMP_ONLINE_NAME", "TAKEDAWN_GAMERTAG");
    if (e) {
        strncpy(s_gamertag, e, sizeof s_gamertag - 1);
        s_gamertag[sizeof s_gamertag - 1] = 0;
    }
    e = cfg("RECOMP_ONLINE_SERVER", "TAKEDAWN_LOBBY");
    /* LAN mode: the port found a lobby on the network or started one,
     * and set online.server to it (Burnout 3: online.c). */
    {
        char host[256];
        const char *colon;
        snprintf(host, sizeof host, "%s", e ? e : XNET_HLE_DEFAULT_LOBBY);
        colon = strrchr(host, ':');
        if (colon) {
            s_lobby_port = (uint16_t)atoi(colon + 1);
            host[colon - host] = 0;
        }
        snprintf(s_lobby_name, sizeof s_lobby_name, "%s", host);
        /* A name is looked up when the lobby is first needed (lobby_resolve),
         * not at the start: a title with online play on by default must not
         * touch the network before the player goes online. An address
         * written as numbers needs no lookup. */
        {
            struct in_addr lit;
            if (inet_pton(AF_INET, host, &lit) == 1)
                s_lobby_ip = lit.s_addr;
            else
                s_lobby_lazy = 1;
        }
    }
    s_local_ip = lan_addr(s_lobby_ip, 1);
    real_lan = s_local_ip;
    /* BO-349: this copy runs the LAN lobby (online.c points it at
     * 127.0.0.1): it signs in to it at its LAN address, like everyone else.
     * Over loopback the lobby recorded 127.0.0.1 for it (its addr request
     * and its source), the others refused its hellos (its title address is
     * the LAN one) and no game formed. */
    if (s_on == 2) {
        uint32_t at;
        lobby_resolve();     /* LAN: the lobby is on this network, a name too */
        at = xr_lan_lobby_at(s_lobby_ip, real_lan);
        if (at != s_lobby_ip) {
            struct in_addr a;
            a.s_addr = at;
            xnet_hle_log("this copy runs the LAN lobby: it signs in to it at %s, its address on the network",
                         inet_ntoa(a));
            s_lobby_ip = at;
        }
    }
    e = getenv("TAKEDAWN_LOCAL_IP");
    if (e && *e) {
        uint32_t ip = resolve4(e);
        if (ip) {
            s_local_ip = ip;
            s_local_pinned = 1;
        }
    }
    /* TAKEDAWN_NET_ID (tests): this copy's title address only, sockets stay
     * on "any", so two copies on one machine are two players on the public
     * path (with TAKEDAWN_NET_FORCE_PUBLIC and their own port offsets). */
    e = getenv("TAKEDAWN_NET_ID");
    if (e && *e && !s_local_pinned) {
        uint32_t ip = resolve4(e);
        if (ip) {
            s_local_ip = ip;
            s_net_id = 1;
        }
    }
    e = cfg("RECOMP_ONLINE_CODE", "TAKEDAWN_GAME_CODE");
    xl_code_clean(e, s_code, sizeof s_code);
#ifndef XNET_NO_NET_SIM     /* a release build has no such switch: xn_on() stays 0 */
    e = getenv("TAKEDAWN_NET_SIM");
    if (e && *e) {
        xn_cfg c;
        if (xn_parse(e, &c) && xn_start(&c))
            xnet_hle_log("TAKEDAWN_NET_SIM: round trip +%d ms, jitter +/-%d ms, loss %d.%02d%% each way (peers' UDP only)",
                         c.delay_ms, c.jitter_ms, c.loss_centi / 100, c.loss_centi % 100);
        else
            xnet_hle_log("TAKEDAWN_NET_SIM ignored: %s", e);
    }
#endif
    e = getenv("TAKEDAWN_PORT_OFFSET");
    s_port_offset = (e && *e) ? (uint16_t)atoi(e) : 30000;
    e = getenv("TAKEDAWN_NET_FORCE_PUBLIC");
    s_force_public = e && *e && *e != '0';
    xr_self(s_local_ip, s_port_offset, s_force_public);
    xr_self_lan(real_lan);
    /* BO-381: the relay fallback can be turned off (tests, the rig) */
    e = getenv("TAKEDAWN_NET_RELAY");
    xr_relay_off(e && *e && *e == '0');
    /* Internet mode: the host's race port (the title's 1000) through the
     * router, by UPnP when the router has it. */
    if (s_on == 1)
        xnet_upnp_start(1000u + s_port_offset);
    /* A locally administered MAC from the address, so two copies on two
     * machines differ. */
    s_mac[0] = 0x02; s_mac[1] = 0x54;
    memcpy(s_mac + 2, &s_local_ip, 4);
    {
        struct in_addr a, b;
        char sa[32], sb[32];
        a.s_addr = s_lobby_ip; b.s_addr = s_local_ip;
        snprintf(sa, sizeof sa, "%s", s_lobby_lazy ? "not looked up yet" : inet_ntoa(a));
        snprintf(sb, sizeof sb, "%s", inet_ntoa(b));
        xnet_hle_log("online layer on: lobby %s (%s) port %u, this machine %s, user '%s', port offset %u%s",
                     s_lobby_name, sa, (unsigned)s_lobby_port, sb, s_gamertag, (unsigned)s_port_offset,
                     s_force_public ? ", peers by their public address (TAKEDAWN_NET_FORCE_PUBLIC)" : "");
    }
}

int xnet_hle_on(void) { init_once(); return s_on; }
int xnet_hle_started(void) { return s_on > 0 ? s_on : 0; }

static void lobby_unreachable(void);
static void lock(void);
static void unlock(void);

static char s_msg[256];
static volatile int s_msg_seq;

void xnet_hle_message(const char *text)
{
    snprintf(s_msg, sizeof s_msg, "%s", text ? text : "");
    s_msg_seq++;
    xnet_hle_note("message for the player: %s", s_msg);
}

void xnet_hle_local_network_hint(void)
{
    static volatile int shown;
    if (shown)
        return;
    shown = 1;
    xnet_hle_message("Online play may fail: allow Takedawn in System Settings > Privacy & Security > Local Network.");
}

int xnet_hle_message_get(char *out, int n)
{
    int seq = s_msg_seq;
    if (seq && out && n > 0)
        snprintf(out, (size_t)n, "%s", s_msg);
    return seq;
}
uint32_t xnet_hle_lobby_ip(void) { init_once(); lobby_resolve(); return s_lobby_ip; }
uint16_t xnet_hle_lobby_port(void) { init_once(); return s_lobby_port; }
const char *xnet_hle_gamertag(void) { init_once(); return s_gamertag; }
uint32_t xnet_hle_local_ip(void) { init_once(); return s_local_ip; }

uint32_t xnet_hle_lan_addr(void)
{
    const char *e = getenv("TAKEDAWN_LOCAL_IP");
    uint32_t ip = 0;
    net_startup();
    if (e && *e)
        ip = resolve4(e);
    if (!ip)
        ip = lan_addr(htonl(0x08080808), 0);
    return (ntohl(ip) >> 24) == 127 ? 0 : ip;
}
uint16_t xnet_hle_port_offset(void) { init_once(); return s_port_offset; }

void xnet_hle_set_code(const char *code)
{
    char c[XL_CODE_MAX + 1];
    int changed;
    init_once();
    xl_code_clean(code, c, sizeof c);
    lock();
    changed = strcmp(c, s_code) != 0;
    snprintf(s_code, sizeof s_code, "%s", c);
    unlock();
    if (changed)
        xnet_hle_log("game code: %s", c[0] ? "set" : "none");
}

void xnet_hle_set_code_quiet(int quiet)
{
    s_code_quiet = quiet != 0;
}

/* the code now, copied under the lock */
static void code_now(char out[XL_CODE_MAX + 1])
{
    lock();
    memcpy(out, s_code, XL_CODE_MAX + 1);
    unlock();
}

const char *xnet_hle_code(void)
{
    static char c[XL_CODE_MAX + 1];
    init_once();
    code_now(c);
    return c;
}

/* ── XNet ─────────────────────────────────────────────────────────── */

uint32_t xnet_hle_link_status(void)
{
    return 0x01 | 0x02 | 0x08;   /* LINK_ACTIVE | 100MBPS | FULL_DUPLEX */
}

/* XNADDR: IN_ADDR ina, IN_ADDR inaOnline, WORD wPortOnline, BYTE abEnet[6],
 * BYTE abOnline[20] = 36 bytes. */
void xnet_hle_make_xnaddr(void *xna, uint32_t ina)
{
    uint8_t *p = (uint8_t *)xna;
    memset(p, 0, 36);
    memcpy(p, &ina, 4);
    memcpy(p + 4, &ina, 4);
    p[8] = 0x0C; p[9] = 0x02;                 /* 3074, big-endian */
    p[10] = 0x02; p[11] = 0x54;
    memcpy(p + 12, &ina, 4);
    memcpy(p + 16, &ina, 4);                  /* abOnline: an id, the address */
}

uint32_t xnet_hle_title_xnaddr(void *xna)
{
    init_once();
    if (xna) {
        xnet_hle_make_xnaddr(xna, s_local_ip);
        memcpy((uint8_t *)xna + 10, s_mac, 6);
    }
    /* ETHERNET | STATIC | GATEWAY | DNS | ONLINE */
    return 0x02 | 0x04 | 0x20 | 0x40 | 0x80;
}

void xnet_hle_random(void *p, uint32_t n)
{
    static uint32_t x;
    uint8_t *b = (uint8_t *)p;
    if (!x)
        x = (uint32_t)time(NULL) ^ 0x9E3779B9u ^ s_local_ip;
    while (n--) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *b++ = (uint8_t)x;
    }
}

/* ── the socket table ─────────────────────────────────────────────── */

#define MAX_SOCK 64
#define HANDLE_BASE 0x100u

/* A lobby connection's own stream (BO-337 H4): its packets as they come
 * in, and what the socket did not take yet of a request we lengthened (M8). */
typedef struct {
    uint8_t buf[16384 + 1];    /* +1: a NUL after the packet being read */
    int len;
    int bad;                   /* a header made no sense: the connection was ended */
    uint8_t out[8192];
    int outlen;
} lobby_st;

/* Every field is read and written under s_lock. A socket in use by a call
 * (refs) is closed when the last call returns (BO-337 M2): its fd number is
 * never handed to a new socket while another thread still uses it. */
typedef struct {
    int used;
    int refs;          /* calls using it now */
    int closing;       /* closesocket was called: no new calls, closed at refs 0 */
    host_sock fd;
    int type;          /* X_SOCK_* */
    int nonblock;
    int bound;
    int active;        /* TCP: connect called, listening or accepted */
    int connecting;    /* TCP: a non-blocking connect not yet resolved (POSIX select) */
    int conn_failed;   /* TCP: that connect failed: except set only, as Winsock */
    int conn_err;      /* its host error for SO_ERROR (read clears it) */
    uint64_t hold_until; /* TCP: no data reported before this (ms), see TCP_MIN_RTT_MS */
    uint64_t hold_start; /* TCP: the first send of this run of held sends */
    int hold_capped;     /* TCP: that run reached TCP_HOLD_CAP_MS */
    int lobby;           /* TCP to the lobby server: its failures and "full" answers are told */
    int lobby_ok;        /* the lobby has answered on it */
    int lobby_eof;       /* its loss was logged (once per connection) */
    int code_sent;       /* a game code went out on this lobby connection */
    lobby_st *st;        /* a lobby connection's stream */
    uint16_t tport;      /* UDP: the title's port it was bound to (0 = none) */
    int bind_pending;    /* UDP: bind answered, the host bind waits for the first use (realize_bind) */
    struct sockaddr_in bind_sa;
} xsock;

/* The lobby link is the game's only TCP. Xbox Live answered a request no
 * sooner than a few frames later, and the title relies on that: Burnout 3
 * fills its per-peer "view" message (type 9, 0x102610) in the lobby frame
 * that also sends gset, and the lobby's reply re-enumerates the peers
 * (0xFD6B0) and resets every message slot. With a same-frame reply (our own
 * lobby on the same machine or LAN) the reset comes before the next net tick
 * sends the slot, the host never hears that the guest sees it, and the race
 * never starts. So a TCP socket reports no data until this long after its
 * last send. */
#define TCP_MIN_RTT_MS 50
/* A title that sends more often than that would never see its data (T2): a
 * run of sends holds at most this long from its first send, and what is
 * waiting when it ends stays readable until the title has read it (a send
 * then starts no new hold). */
#define TCP_HOLD_CAP_MS (2 * TCP_MIN_RTT_MS)

/* a host call cut short by a signal: try it again (Winsock has none) */
static int interrupted(void)
{
#ifdef _WIN32
    return 0;
#else
    return errno == EINTR;
#endif
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* The lobby could not be reached (a refused or failed connection): told
 * once a minute at most. */
static void lobby_unreachable(void)
{
    static uint64_t last;
    uint64_t now = now_ms();
    struct in_addr a;
    char m[200];
    if (last && now - last < 60000)
        return;
    last = now;
    lobby_resolve();
    a.s_addr = s_lobby_ip;
    /* The name as the player knows it (a name that did not resolve has
     * loopback behind it, which would only confuse). */
    if (s_lobby_unresolved)
        snprintf(m, sizeof m, "The online server name '%s' does not resolve to an IPv4 address. Check the Server row"
                 " on the System page.", s_lobby_name);
    else
        snprintf(m, sizeof m, "Can't reach the online server (%s:%u). Check the Server row on the System page,"
                 " or whether the server is running.", s_lobby_name[0] ? s_lobby_name : inet_ntoa(a),
                 (unsigned)(s_lobby_port ? s_lobby_port : 21810));
    xnet_hle_message(m);
}

/* The lobby's answers by their status (the header's bytes 4..7,
 * tools/lobby/aries_server.c), one whole packet at a time (BO-337 L6). */
static void lobby_answers(const uint8_t *hd, int code_set)
{
    /* A wrong or missing game code (BO-335): the server answers gjoi "gone",
     * which the title shows as a game that has gone, and the game says why
     * itself (its refused-join message): only a log line here, so the player
     * sees one message */
    if (code_set && !memcmp(hd, "gjoi", 4) && !memcmp(hd + 4, "gone", 4)) {
        xnet_hle_log("a join with the game code was refused: no open game with that code");
        return;
    }
    if (memcmp(hd + 4, "full", 4))
        return;
    if (!memcmp(hd, "@dir", 4))
        xnet_hle_message("The online server is full. Try again later.");
    else if (!memcmp(hd, "gjoi", 4)) {
        if (s_join_quiet)
            xnet_hle_log("a join was answered full while waiting for a race: waiting on");
        else
            xnet_hle_message("That game is full.");
    }
    else if (!memcmp(hd, "gcre", 4))
        xnet_hle_message("The online server cannot open more games right now. Try again later.");
}

static xsock s_tab[MAX_SOCK];
#ifdef _WIN32
static CRITICAL_SECTION s_lock;
static volatile LONG s_lock_init;
static void lock(void)
{
    if (InterlockedCompareExchange(&s_lock_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_lock);
        s_lock_init = 2;
    }
    while (s_lock_init != 2) Sleep(0);
    EnterCriticalSection(&s_lock);
}
static void unlock(void) { LeaveCriticalSection(&s_lock); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
#endif

/* under the lock */
static xsock *get(uint32_t h)
{
    uint32_t i = h - HANDLE_BASE;
    if (i >= MAX_SOCK || !s_tab[i].used || s_tab[i].closing)
        return NULL;
    return &s_tab[i];
}

static int held(xsock *k)
{
    int r;
    lock();
    r = k->type == X_SOCK_STREAM && k->hold_until && now_ms() < k->hold_until;
    unlock();
    return r;
}

static int host_pending(host_sock fd)
{
#ifdef _WIN32
    u_long v = 0;
    return ioctlsocket(fd, FIONREAD, &v) == 0 && v > 0;
#else
    int v = 0;
    return ioctl(fd, FIONREAD, &v) == 0 && v > 0;
#endif
}

/* A send: no data reported for TCP_MIN_RTT_MS after it, within the cap
 * (T2). Under s_lock. */
static void hold_locked(xsock *k)
{
    uint64_t now = now_ms();
    if (k->hold_until && now < k->hold_until) {
        k->hold_until = now + TCP_MIN_RTT_MS;   /* the run goes on */
        if (k->hold_until >= k->hold_start + TCP_HOLD_CAP_MS) {
            k->hold_until = k->hold_start + TCP_HOLD_CAP_MS;
            k->hold_capped = 1;
        }
    } else if (!(k->hold_capped && host_pending(k->fd))) {
        k->hold_start = now;                    /* a new run */
        k->hold_until = now + TCP_MIN_RTT_MS;
        k->hold_capped = 0;
    }
}

static void hold(xsock *k)
{
    lock();
    hold_locked(k);
    unlock();
}

/* A socket for one call: NULL when there is none (or it is being closed). */
static xsock *acquire_nb(uint32_t h)
{
    xsock *k;
    lock();
    k = get(h);
    if (k)
        k->refs++;
    unlock();
    return k;
}

static int map_err(int e);
static const char *sa_str(const struct sockaddr_in *h, char *buf, size_t n);
/* The host bind of a UDP socket the title bound before it used it (its race
 * socket, at boot): the title is answered at once, the port is taken from
 * the host at the first real use, so a player who never goes online is
 * never asked by a firewall about a port nobody talks on. */
static void realize_bind(xsock *k)
{
    struct sockaddr_in h;
    char b[40];
    lock();
    if (!k->bind_pending) {
        unlock();
        return;
    }
    k->bind_pending = 0;
    h = k->bind_sa;
    unlock();
    if (bind(k->fd, (struct sockaddr *)&h, sizeof h) != 0)
        xnet_hle_log("deferred bind(%s) failed, WSA %d", sa_str(&h, b, sizeof b), map_err(host_errno()));
    else
        xnet_hle_log("bind(%s): the host port taken at the first use", sa_str(&h, b, sizeof b));
}

static xsock *acquire(uint32_t h)
{
    xsock *k = acquire_nb(h);
    if (k)
        realize_bind(k);
    return k;
}

/* under the lock: the slot's last use; the fd and the stream to free. A
 * lobby connection closing does not end the game: the title closes it right
 * after gsta and races without it (BO-337 M6: the forwarding stays). */
static void retire(xsock *k, host_sock *fd, lobby_st **st)
{
    *fd = k->fd;
    *st = k->st;
    memset(k, 0, sizeof *k);
}

static void release(xsock *k)
{
    host_sock fd = HOST_BAD;
    lobby_st *st = NULL;
    if (!k)
        return;
    lock();
    if (--k->refs == 0 && k->closing)
        retire(k, &fd, &st);
    unlock();
    if (fd != HOST_BAD)
        host_close(fd);
    free(st);
}

/* ── peers: the lobby's addresses, the XUIDs' offsets (xnet_route.h) ── */

void xnet_hle_peer_offset(uint32_t ina, uint16_t offset)
{
    lock();
    xr_offset(ina, offset);
    unlock();
}

/* where the lobby's @dir sends the copy on (LAN mode: another address of the
 * embedded lobby's machine): that connection is the lobby's too */
static uint32_t s_dir_ip;
static uint16_t s_dir_port;

static void dir_answer(const char *b, int n)
{
    char v[32];
    const char *a = NULL, *p = NULL;
    int i, k;
    for (i = 0; i + 5 < n; i++) {
        if (!a && !memcmp(b + i, "ADDR=", 5) && (i == 0 || b[i - 1] == '\n'))
            a = b + i + 5;
        if (!p && !memcmp(b + i, "PORT=", 5) && (i == 0 || b[i - 1] == '\n'))
            p = b + i + 5;
    }
    if (!a || !p)
        return;
    for (k = 0; k < (int)sizeof v - 1 && a + k < b + n && a[k] && a[k] != '\n'; k++)
        v[k] = a[k];
    v[k] = 0;
    s_dir_ip = inet_addr(v);
    s_dir_port = (uint16_t)atoi(p);
}

/* under the lock: our game is over (the lobby's empty record, glea, gdel):
 * its players, learned paths and secret are forgotten (BO-337 M3), and its
 * ports no longer forwarded */
static void game_over(const char *why)
{
    if (s_game.seen) {
        char t[32], g[32] = "";
        if (s_game.gid[0])
            snprintf(g, sizeof g, " gid=%s", s_game.gid);
        xnet_hle_note("game %d%s over (%s)%s", s_game.ident, g, why, sid_tag(t, sizeof t));
        xnet_hle_log("game %d over (%s): its players, paths and secret forgotten", s_game.ident, why);
    }
    memset(&s_game, 0, sizeof s_game);
    xr_game_reset();
    if (s_on == 1)
        xnet_upnp_host(0);
}

static int is_cmd(const uint8_t *hd, const char *c) { return !memcmp(hd, c, 4); }

/* under the lock: one whole packet of a lobby connection (hd: its 12-byte
 * header, then body, bl bytes, with a NUL after them) */
static void lobby_packet(const uint8_t *hd, const char *body, int bl)
{
    uint32_t self_before = xr_self_public(), synth_before = xr_self_synthetic();
    int seq_before = xr_secret_seq(), id;
    /* success: no status in the header (an error is "full", "gone" ...) */
    int ok = !hd[4] && !hd[5] && !hd[6] && !hd[7];
    /* our game's member records: its players (the router path) and their ready values */
    int member = ok && (is_cmd(hd, "+mgm") || is_cmd(hd, "gcre") || is_cmd(hd, "gjoi"));
    lv_in(hd, body, bl);
    lobby_answers(hd, s_code[0] != 0);
    if (is_cmd(hd, "auth") || is_cmd(hd, "gcre") || is_cmd(hd, "gjoi")) {
        char t[32], st[5];
        int i;
        for (i = 0; i < 4; i++)
            st[i] = hd[4 + i] >= 0x20 && hd[4 + i] < 0x7F ? (char)hd[4 + i] : '?';
        st[4] = 0;
        sid_tag(t, sizeof t);
        if (is_cmd(hd, "auth") && ok)
            xnet_hle_note("signed in to the lobby%s", t);
        else if (is_cmd(hd, "auth"))
            xnet_hle_note("sign-in refused by the lobby: status '%s'%s", st, t);
        else if (!ok)
            xnet_hle_note("%s refused by the lobby: status '%s'%s", is_cmd(hd, "gcre") ? "creating a game" : "joining a game", st, t);
    }
    /* the identity scheme: our synthetic address, or an old server */
    if (ok && is_cmd(hd, "addr")) {
        char sid[XL_ID_MAX + 1];
        int fresh = 0;
        xr_addr_reply(body, bl);
        if (xl_key_hex(body, bl, "TKSID", sid, sizeof sid)) {
            if (strcmp(sid, s_sid)) {
                snprintf(s_sid, sizeof s_sid, "%s", sid);
                xnet_hle_note("lobby session sid=%s", s_sid);
                fresh = 1;
            }
        } else {
            s_sid[0] = 0;               /* an old server: no id, nothing new in the log */
        }
        /* BO-343: a new session (or our own address changed): the server
         * hands its synthetic addresses out again, so the last session's
         * peers, paths, hellos and secret are no one's any more. An old
         * server (neither key) keeps its addresses: nothing changes there. */
        if (fresh || xr_self_synthetic() != synth_before) {
            int gone = xr_session_reset();
            if (gone)
                xnet_hle_log("a new lobby session: the last one's %d peer(s) and path(s) forgotten "
                             "(the server hands its player addresses out again)", gone);
        }
        if (xr_self_synthetic() != synth_before) {
            struct in_addr a;
            a.s_addr = xr_self_synthetic();
            if (a.s_addr)
                xnet_hle_log("the lobby gave this copy its own player address %s (the others know it by that)",
                             inet_ntoa(a));
            else
                xnet_hle_log("the lobby gives no player address of its own: the copy's own address, as before");
        }
    }
    /* another game, or none: what we knew of the last one goes first, so
     * this record's addresses and secret are the new game's */
    if (member) {
        if (xl_game_gone(body, bl))
            game_over("the lobby's empty record");
        else if (xl_key_int(body, bl, "IDENT", &id) && id > 0 && (!s_game.seen || id != s_game.ident)) {
            if (s_game.seen)
                xnet_hle_log("game %d -> game %d: the last one's players, paths and secret forgotten", s_game.ident, id);
            xr_game_reset();
            memset(&s_game, 0, sizeof s_game);
        }
    }
    if (is_cmd(hd, "@dir"))
        dir_answer(body, bl);
    xr_lobby_body(body, bl);
    {
        int on0 = s_status.online, wt0 = s_status.waiting;
        int rc0 = s_status.racing, v_rc;
        xl_status_body(body, bl, &s_status);
        /* TKRACING is the room's count for the online page only when a room
         * record says it: a search's answer has its own (the view's) */
        if (!is_cmd(hd, "+rom"))
            s_status.racing = rc0;
        else if (xl_key_int(body, bl, "TKRACING", &v_rc))
            s_status.racing = v_rc;
        if (s_status.seen && (on0 != s_status.online || wt0 != s_status.waiting))
            xnet_hle_log("server: %d online, %d game(s) waiting", s_status.online, s_status.waiting);
    }
    /* A search with a code found nothing: say so, at most once in 30 s.
     * The server ends a session after 10 misses a minute: no hammering. */
    if (s_code[0] && !s_code_quiet && is_cmd(hd, "gsea")) {
        static uint64_t said;
        int cnt = -1;
        xl_key_int(body, bl, "COUNT", &cnt);
        if (cnt == 0 && now_ms() - said > 30000) {
            char m[128];
            said = now_ms();
            snprintf(m, sizeof m, "No game with code %s is open yet. Wait for its host, then try again.", s_code);
            xnet_hle_message(m);
        }
    }
    if (member) {
        xl_game g;
        xr_members_body(body, bl);
        /* BO-381: the lobby's relay (TKRELAY=<port> next to the secret), and
         * the game the relay knows us in */
        if (xl_key_int(body, bl, "IDENT", &id) && id > 0)
            xr_relay_game(id);
        {
            uint16_t rp = xr_relay_port_seen();
            xr_ep was = xr_relay_ep();
            if (rp) {
                xr_relay_set(s_lobby_ip, rp);
                if (!was.port || was.port != rp)
                    xnet_hle_log("the lobby runs a relay on UDP %u: a player whose hello does not come in %u s is"
                                 " reached through it (direct stays the first choice)", (unsigned)rp,
                                 (unsigned)(XR_RELAY_AFTER_MS / 1000u));
            }
        }
        if (xl_game_body(body, bl, &g)) {
            char a[160], b[160];
            xl_game_line(&s_game, a, sizeof a);
            xl_game_line(&g, b, sizeof b);
            if (strcmp(a, b) || g.ident != s_game.ident)
                xnet_hle_log("game %d: %d player(s), ready values %s", g.ident, g.np, b);
            s_game = g;
            if (g.gid[0])
                snprintf(s_last_gid, sizeof s_last_gid, "%s", g.gid);
            if (is_cmd(hd, "gcre") || is_cmd(hd, "gjoi")) {
                char t[32], gt[32] = "";
                if (g.gid[0])
                    snprintf(gt, sizeof gt, " gid=%s", g.gid);
                xnet_hle_note("game %s: game %d%s, %d player(s)%s", is_cmd(hd, "gcre") ? "created" : "joined", g.ident, gt,
                              g.np, sid_tag(t, sizeof t));
            }
            /* our own game is open: the router forwards its ports now (not
             * when gcre went out: the server may answer "full") (M6) */
            if (is_cmd(hd, "gcre") && g.ident > 0) {
                if (s_code[0] && !s_code_quiet) {
                    char msg[120];
                    snprintf(msg, sizeof msg, "Private game %s created: tell your friends the code.", s_code);
                    xnet_hle_message(msg);
                }
                if (s_on == 1)
                    xnet_upnp_host(1);
            }
        }
    }
    if (is_cmd(hd, "glea"))
        game_over("we left it");
    else if (is_cmd(hd, "gdel"))
        game_over("we deleted it");
    if (xr_secret_seq() != seq_before && xr_has_secret()) {
        char px[5];
        xr_secret_prefix(px);
        xnet_hle_log("the lobby gave this game's secret (in its %.4s record; it starts %s): hellos are signed with it"
                     " from now on", (const char *)hd, px);
    }
    if (!self_before && xr_self_public()) {
        struct in_addr a;
        a.s_addr = xr_self_public();
        xnet_hle_log("the lobby sees this copy at %s", inet_ntoa(a));
    }
}

/* A lobby connection's TCP stream, cut into its packets (12-byte header,
 * the length big-endian at 8). Each connection has its own (H4). */
static void lobby_stream(xsock *k, const uint8_t *b, int n)
{
    lobby_st *st;
    if (n <= 0)
        return;
    lock();
    st = k->st;
    if (!st) {
        unlock();
        return;
    }
    /* The chunk goes in as far as the buffer takes it, whole packets come
     * out, and the rest follows: any chunk size keeps the packets in step
     * (a packet is never longer than the buffer, so each round frees room). */
    while (n > 0 && !st->bad) {
        int take = (int)sizeof st->buf - 1 - st->len;
        if (take > n)
            take = n;
        memcpy(st->buf + st->len, b, (size_t)take);
        st->len += take;
        b += take;
        n -= take;
        while (st->len >= 12) {
            int plen = (int)((uint32_t)st->buf[8] << 24 | (uint32_t)st->buf[9] << 16 |
                             (uint32_t)st->buf[10] << 8 | st->buf[11]);
            uint8_t keep;
            if (plen < 12 || plen > (int)sizeof st->buf - 1) {
                /* Not a lobby stream any more: end the connection as a
                 * loss (the title reconnects) rather than read on out of
                 * step. */
                char t[32];
                st->bad = 1;
                st->len = 0;
                xnet_hle_note("the lobby stream went out of step (a packet length of %d): connection ended%s",
                              plen, sid_tag(t, sizeof t));
#ifdef _WIN32
                shutdown(k->fd, SD_BOTH);
#else
                shutdown(k->fd, SHUT_RDWR);
#endif
                break;
            }
            if (st->len < plen)
                break;
            keep = st->buf[plen];      /* a NUL after the body while it is read (L8) */
            st->buf[plen] = 0;
            lobby_packet(st->buf, (const char *)st->buf + 12, plen - 12);
            st->buf[plen] = keep;
            memmove(st->buf, st->buf + plen, (size_t)(st->len - plen));
            st->len -= plen;
        }
    }
    unlock();
}

int xnet_hle_game_state(char *out, int n)
{
    char px[5], g[160];
    int np;
    if (!out || n <= 0)
        return 0;
    lock();
    xr_secret_prefix(px);
    xl_game_line(&s_game, g, sizeof g);
    /* the route tables' use too (BO-337 H3): peers known / room, learned paths / room */
    snprintf(out, (size_t)n, "game=%d players=%d ready=[%s] secret=%d:%s peers=%d/%d learned=%d/%d members=%d",
             s_game.seen ? s_game.ident : -1, s_game.np, g, xr_secret_seq(), px, xr_peer_count(), XR_MAX_PEERS,
             xr_learned_count(), XR_MAX_LEARNED, xr_member_count());
    {   /* BO-340: the ids the server's log lines carry; nothing when the server sent none */
        size_t o = strlen(out);
        if (s_sid[0] && o < (size_t)n)
            o += (size_t)snprintf(out + o, (size_t)n - o, " sid=%s", s_sid);
        if (s_last_gid[0] && o < (size_t)n)
            snprintf(out + o, (size_t)n - o, " gid=%s", s_last_gid);
    }
    np = s_game.np;
    unlock();
    return np;
}

int xnet_hle_lobby_racing(void)
{
    int r;
    lock();
    r = s_status.seen ? s_status.racing : -1;
    unlock();
    return r;
}

void xnet_hle_set_join_quiet(int quiet)
{
    s_join_quiet = quiet != 0;
}

int xnet_hle_lobby_status(int *online, int *waiting)
{
    int seen;
    lock();
    seen = s_status.seen;
    if (online) *online = s_status.online;
    if (waiting) *waiting = s_status.waiting;
    unlock();
    return seen;
}

static int map_err(int e)
{
#ifdef _WIN32
    return e;   /* already a WSA number */
#else
    switch (e) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EINPROGRESS:   return WSAEWOULDBLOCK;   /* Winsock says this for connect too */
    case EALREADY:      return WSAEALREADY;
    case EBADF:
    case ENOTSOCK:      return WSAENOTSOCK;
    case EFAULT:        return WSAEFAULT;
    case EINVAL:        return WSAEINVAL;
    case EMFILE:        return WSAEMFILE;
    case EMSGSIZE:      return WSAEMSGSIZE;
    case ENOPROTOOPT:   return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP:    return WSAEOPNOTSUPP;
    case EAFNOSUPPORT:  return WSAEAFNOSUPPORT;
    case EADDRINUSE:    return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN:      return WSAENETDOWN;
    case ENETUNREACH:   return WSAENETUNREACH;
    case ECONNABORTED:  return WSAECONNABORTED;
    case ECONNRESET:
    case EPIPE:         return WSAECONNRESET;
    case ENOBUFS:       return WSAENOBUFS;
    case EISCONN:       return WSAEISCONN;
    case ENOTCONN:      return WSAENOTCONN;
    case ETIMEDOUT:     return WSAETIMEDOUT;
    case ECONNREFUSED:  return WSAECONNREFUSED;
    case EHOSTUNREACH:  return WSAEHOSTUNREACH;
    default:            return WSAEINVAL;
    }
#endif
}

/* Xbox sockaddr_in -> host. Only AF_INET exists on the Xbox. */
static int to_host_sa(const void *x, int len, struct sockaddr_in *h)
{
    const uint8_t *p = (const uint8_t *)x;
    uint16_t fam;
    if (!x || len < 8)
        return -1;
    memcpy(&fam, p, 2);
    memset(h, 0, sizeof *h);
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, p + 2, 2);
    memcpy(&h->sin_addr.s_addr, p + 4, 4);
    (void)fam;
    return 0;
}

static void to_x_sa(const struct sockaddr_in *h, void *x, int32_t *len)
{
    uint8_t *p = (uint8_t *)x;
    uint16_t fam = X_AF_INET;
    if (!x)
        return;
    if (len && *len < 16) {
        *len = 16;
        return;
    }
    memset(p, 0, 16);
    memcpy(p, &fam, 2);
    memcpy(p + 2, &h->sin_port, 2);
    memcpy(p + 4, &h->sin_addr.s_addr, 4);
    if (len)
        *len = 16;
}

static const char *sa_str(const struct sockaddr_in *h, char *buf, size_t n)
{
    const uint8_t *a = (const uint8_t *)&h->sin_addr.s_addr;
    snprintf(buf, n, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], (unsigned)ntohs(h->sin_port));
    return buf;
}

/* The title's own UDP ports (1000 for the race, 6000 and 3074 for its
 * other sockets) all move by the offset, so every copy on a machine and
 * every router mapping has its own (XR_TITLE_PORTS, xnet_route.h); TCP
 * keeps the old rule (below 1024 only), so a lobby port is never moved. */
static uint16_t shift_limit(int udp) { return udp ? XR_TITLE_PORTS : 1024; }

static void port_out(struct sockaddr_in *h, int udp)
{
    uint16_t p = ntohs(h->sin_port);
    if (p && p < shift_limit(udp) && s_port_offset)
        h->sin_port = htons((uint16_t)(p + s_port_offset));
}

static void port_in(struct sockaddr_in *h, int udp)
{
    uint16_t p = ntohs(h->sin_port);
    if (s_port_offset && p >= s_port_offset && p < s_port_offset + shift_limit(udp))
        h->sin_port = htons((uint16_t)(p - s_port_offset));
}

/* A pinned copy's unbound socket takes its address before it talks. */
static void pin_source(xsock *k)
{
    struct sockaddr_in h;
    if (!s_local_pinned || k->bound)
        return;
    memset(&h, 0, sizeof h);
    h.sin_family = AF_INET;
    h.sin_addr.s_addr = s_local_ip;
    if (bind(k->fd, (struct sockaddr *)&h, sizeof h) == 0) {
        lock();
        k->bound = 1;
        unlock();
    }
}

/* BO-349: a send the system refused for that address (no route to it, or
 * not permitted: macOS Local Network privacy, a host firewall) */
#ifdef _WIN32
#  define HOST_EPERM WSAEACCES
static int lan_refused(int e) { return e == WSAEHOSTUNREACH; }
#else
#  define HOST_EPERM EPERM
static int lan_refused(int e) { return e == EHOSTUNREACH || e == EPERM; }
#endif

static int trace_n(int *count)
{
    if (s_trace)
        return 1;
    return (*count)++ < 40;
}

uint32_t xnet_hle_socket(int af, int type, int proto, int *werr)
{
    int htype, hproto, i;
    host_sock fd;
    init_once();
    if (af != X_AF_INET) {
        *werr = WSAEAFNOSUPPORT;
        return X_INVALID_SOCKET;
    }
    if (type == X_SOCK_STREAM) {
        htype = SOCK_STREAM; hproto = IPPROTO_TCP;
    } else if (type == X_SOCK_DGRAM) {
        htype = SOCK_DGRAM; hproto = IPPROTO_UDP;
    } else {
        *werr = WSAEPROTONOSUPPORT;
        return X_INVALID_SOCKET;
    }
    (void)proto;   /* 0, TCP, UDP or VDP (a UDP datagram here) */
    fd = socket(AF_INET, htype, hproto);
    if (fd == HOST_BAD) {
        *werr = map_err(host_errno());
        return X_INVALID_SOCKET;
    }
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    lock();
    for (i = 0; i < MAX_SOCK && s_tab[i].used; i++)
        ;
    if (i == MAX_SOCK) {
        unlock();
        host_close(fd);
        *werr = WSAEMFILE;
        return X_INVALID_SOCKET;
    }
    memset(&s_tab[i], 0, sizeof s_tab[i]);
    s_tab[i].used = 1;
    s_tab[i].fd = fd;
    s_tab[i].type = type;
    unlock();
    xnet_hle_log("socket(type %d, proto %d) = %u", type, proto, HANDLE_BASE + i);
    return HANDLE_BASE + (uint32_t)i;
}

/* Closed now, or by the last call still using it (M2); a call blocked on
 * it is woken by the shutdown, as Winsock's closesocket would. */
int xnet_hle_closesocket(uint32_t s, int *werr)
{
    xsock *k;
    host_sock fd = HOST_BAD;
    lobby_st *st = NULL;
    lock();
    k = get(s);
    if (!k) {
        unlock();
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    k->closing = 1;
    if (k->refs == 0)
        retire(k, &fd, &st);
    else
#ifdef _WIN32
        shutdown(k->fd, SD_BOTH);
#else
        shutdown(k->fd, SHUT_RDWR);
#endif
    unlock();
    if (fd != HOST_BAD)
        host_close(fd);
    free(st);
    xnet_hle_log("closesocket(%u)%s", s, fd == HOST_BAD ? ": closed when the call using it returns" : "");
    return 0;
}

int xnet_hle_session_up(void)
{
    int i, up;
    if (s_on <= 0)
        return 0;
    lock();
    up = s_game.seen;
    for (i = 0; !up && i < MAX_SOCK; i++)
        up = s_tab[i].used && !s_tab[i].closing && s_tab[i].lobby && s_tab[i].lobby_ok && !s_tab[i].lobby_eof;
    unlock();
    return up;
}

int xnet_hle_socket_count(void)
{
    int i, n = 0;
    lock();
    for (i = 0; i < MAX_SOCK; i++)
        n += s_tab[i].used && !s_tab[i].closing;
    unlock();
    return n;
}

int xnet_hle_close_all(void)
{
    int i, n = 0, e;
    for (i = 0; i < MAX_SOCK; i++) {
        int open;
        lock();
        open = s_tab[i].used && !s_tab[i].closing;
        unlock();
        if (open && xnet_hle_closesocket(HANDLE_BASE + (uint32_t)i, &e) == 0)
            n++;
    }
    if (n)
        xnet_hle_log("cleanup: %d socket(s) the title left open closed", n);
    return n;
}

static int shutdown_k(xsock *k, uint32_t s, int how, int *werr)
{
    (void)s;
    if (shutdown(k->fd, how) != 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    return 0;
}

int xnet_hle_shutdown(uint32_t s, int how, int *werr)
{
    int r;
    xsock *k = acquire_nb(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = shutdown_k(k, s, how, werr);
    release(k);
    return r;
}

static int set_nonblock(host_sock fd, int on)
{
#ifdef _WIN32
    u_long v = (u_long)on;
    return ioctlsocket(fd, FIONBIO, &v);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static int ioctlsocket_k(xsock *k, uint32_t s, uint32_t cmd, uint32_t *argp, int *werr)
{
    if (!argp) {
        *werr = WSAEFAULT;
        return X_SOCKET_ERROR;
    }
    if (cmd == X_FIONBIO) {
        if (set_nonblock(k->fd, *argp != 0) != 0) {
            *werr = map_err(host_errno());
            return X_SOCKET_ERROR;
        }
        lock();
        k->nonblock = *argp != 0;
        unlock();
        return 0;
    }
    if (cmd == X_FIONREAD) {
#ifdef _WIN32
        u_long v = 0;
        if (ioctlsocket(k->fd, FIONREAD, &v) != 0) {
#else
        int v = 0;
        if (ioctl(k->fd, FIONREAD, &v) != 0) {
#endif
            *werr = map_err(host_errno());
            return X_SOCKET_ERROR;
        }
        *argp = (uint32_t)v;
        return 0;
    }
    xnet_hle_log("ioctlsocket(%u, 0x%08X): not handled", s, cmd);
    *werr = WSAEINVAL;
    return X_SOCKET_ERROR;
}

int xnet_hle_ioctlsocket(uint32_t s, uint32_t cmd, uint32_t *argp, int *werr)
{
    int r;
    xsock *k = acquire_nb(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = ioctlsocket_k(k, s, cmd, argp, werr);
    release(k);
    return r;
}

static int setsockopt_k(xsock *k, uint32_t s, int level, int opt, const void *val, int len, int *werr)
{
    int hl = -1, ho = -1, iv = 0;
    if (val && len >= 4)
        memcpy(&iv, val, 4);
    else if (val && len >= 1)
        iv = *(const uint8_t *)val;
    if (level == X_SOL_SOCKET) {
        hl = SOL_SOCKET;
        switch (opt) {
        case X_SO_REUSEADDR: ho = SO_REUSEADDR; break;
        case X_SO_BROADCAST: ho = SO_BROADCAST; break;
        case X_SO_SNDBUF:    ho = SO_SNDBUF; break;
        case X_SO_RCVBUF:    ho = SO_RCVBUF; break;
        case X_SO_LINGER:    return 0;   /* the title's linger is advisory */
        }
    } else if (level == X_IPPROTO_TCP && opt == X_TCP_NODELAY) {
        hl = IPPROTO_TCP; ho = TCP_NODELAY;
    }
    if (ho < 0) {
        xnet_hle_log("setsockopt(%u, 0x%X, 0x%X, %d): ignored", s, level, opt, iv);
        return 0;
    }
    if (setsockopt(k->fd, hl, ho, (const char *)&iv, sizeof iv) != 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    return 0;
}

int xnet_hle_setsockopt(uint32_t s, int level, int opt, const void *val, int len, int *werr)
{
    int r;
    xsock *k = acquire_nb(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = setsockopt_k(k, s, level, opt, val, len, werr);
    release(k);
    return r;
}

static int getsockopt_k(xsock *k, uint32_t s, int level, int opt, void *val, int32_t *len, int *werr)
{
    int iv = 0;
    socklen_t n = sizeof iv;
    if (!val || !len || *len < 4) {
        *werr = WSAEFAULT;
        return X_SOCKET_ERROR;
    }
    if (level == X_SOL_SOCKET && opt == X_SO_ERROR) {
        lock();
        iv = k->conn_err;   /* a failed connect select already read (POSIX) */
        k->conn_err = 0;
        unlock();
        if (!iv)
            getsockopt(k->fd, SOL_SOCKET, SO_ERROR, (char *)&iv, &n);
        iv = iv ? map_err(iv) : 0;
    } else if (level == X_SOL_SOCKET && opt == X_SO_TYPE) {
        iv = k->type;
    } else {
        xnet_hle_log("getsockopt(%u, 0x%X, 0x%X): answered 0", s, level, opt);
    }
    memcpy(val, &iv, 4);
    *len = 4;
    return 0;
}

int xnet_hle_getsockopt(uint32_t s, int level, int opt, void *val, int32_t *len, int *werr)
{
    int r;
    xsock *k = acquire_nb(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = getsockopt_k(k, s, level, opt, val, len, werr);
    release(k);
    return r;
}

static int getsockname_k(xsock *k, uint32_t s, void *name, int32_t *len, int *werr)
{
    struct sockaddr_in h;
    socklen_t n = sizeof h;
    (void)s;
    if (getsockname(k->fd, (struct sockaddr *)&h, &n) != 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    /* The title wants its LAN address, not 0.0.0.0; with TAKEDAWN_NET_ID its
     * own title address, also for a connected socket (the lobby's "addr"
     * request carries it, and two copies on one machine must differ). */
    if (!h.sin_addr.s_addr || s_net_id)
        h.sin_addr.s_addr = s_local_ip;
    port_in(&h, k->type == X_SOCK_DGRAM);
    to_x_sa(&h, name, len);
    return 0;
}

int xnet_hle_getsockname(uint32_t s, void *name, int32_t *len, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = getsockname_k(k, s, name, len, werr);
    release(k);
    return r;
}

static int getpeername_k(xsock *k, uint32_t s, void *name, int32_t *len, int *werr)
{
    struct sockaddr_in h;
    socklen_t n = sizeof h;
    (void)s;
    if (getpeername(k->fd, (struct sockaddr *)&h, &n) != 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    port_in(&h, k->type == X_SOCK_DGRAM);
    to_x_sa(&h, name, len);
    return 0;
}

int xnet_hle_getpeername(uint32_t s, void *name, int32_t *len, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = getpeername_k(k, s, name, len, werr);
    release(k);
    return r;
}

static int bind_k(xsock *k, uint32_t s, const void *name, int len, int *werr)
{
    struct sockaddr_in h;
    char b[40];
    uint16_t tp;
    if (to_host_sa(name, len, &h) != 0) {
        *werr = WSAEFAULT;
        return X_SOCKET_ERROR;
    }
    tp = ntohs(h.sin_port);
    port_out(&h, k->type == X_SOCK_DGRAM);
    if (s_local_pinned && !h.sin_addr.s_addr)
        h.sin_addr.s_addr = s_local_ip;
    /* TCP only: a listening port with old connections in TIME_WAIT binds
     * again. Never UDP: there it lets a second socket share a port that is
     * in use (Linux, Windows), so a re-bound race port would sit beside a
     * leaked one and the traffic split; the Xbox says WSAEADDRINUSE (T5). The
     * title's own setsockopt(SO_REUSEADDR) still goes through. */
    if (k->type == X_SOCK_STREAM) {
        int one = 1;
        setsockopt(k->fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    }
    /* A title UDP port nobody else holds: the host bind waits for the first
     * use (realize_bind). With another socket on the port the bind is made
     * now, after that one's, so the title gets its WSAEADDRINUSE (T5). */
    if (k->type == X_SOCK_DGRAM && tp && tp < XR_TITLE_PORTS) {
        int i, dup = 0;
        for (i = 0; i < MAX_SOCK; i++) {
            xsock *o = &s_tab[i];
            int hit;
            lock();
            hit = o != k && o->used && !o->closing && o->tport == tp;
            unlock();
            if (hit) {
                dup = 1;
                realize_bind(o);
            }
        }
        if (!dup) {
            lock();
            k->bind_sa = h;
            k->bind_pending = 1;
            k->tport = tp;
            k->bound = 1;
            unlock();
            if (s_on == 1)
                xnet_upnp_port((unsigned)ntohs(h.sin_port));
            xnet_hle_log("bind(%u, %s): the host port is taken at the first use", s, sa_str(&h, b, sizeof b));
            return 0;
        }
    }
    if (bind(k->fd, (struct sockaddr *)&h, sizeof h) != 0) {
        *werr = map_err(host_errno());
        xnet_hle_log("bind(%u, %s) failed, WSA %d", s, sa_str(&h, b, sizeof b), *werr);
        return X_SOCKET_ERROR;
    }
    lock();
    k->tport = tp;
    k->bound = 1;
    unlock();
    /* Internet mode: the router forwards each of the title's UDP ports (one
     * that is bound: a failed bind is no port of ours) (M6) */
    if (k->type == X_SOCK_DGRAM && s_on == 1 && tp && tp < XR_TITLE_PORTS)
        xnet_upnp_port((unsigned)ntohs(h.sin_port));
    xnet_hle_log("bind(%u, %s)", s, sa_str(&h, b, sizeof b));
    return 0;
}

int xnet_hle_bind(uint32_t s, const void *name, int len, int *werr)
{
    int r;
    xsock *k = acquire_nb(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = bind_k(k, s, name, len, werr);
    release(k);
    return r;
}

static int connect_k(xsock *k, uint32_t s, const void *name, int len, int *werr)
{
    struct sockaddr_in h;
    char b[40];
    if (to_host_sa(name, len, &h) != 0) {
        *werr = WSAEFAULT;
        return X_SOCKET_ERROR;
    }
    /* The lobby (BO-337 H4), by where the title connects: the server's
     * address on the title's own lobby port (21810) or the one set
     * (TAKEDAWN_LOBBY=host:port), or where its @dir sent us (LAN mode:
     * another address of the embedded lobby's machine). Not the "buddy" port
     * the server names next to it (its port + 1), which stays as it is:
     * only the title's 21810 moves to the port set. */
    {
        uint16_t want = ntohs(h.sin_port), lport = s_lobby_port ? s_lobby_port : 21810;
        lobby_resolve();
        int at_lobby = k->type == X_SOCK_STREAM && h.sin_addr.s_addr == s_lobby_ip;
        int buddy = at_lobby && want == (uint16_t)(lport + 1) && want != 21810;
        int lobby = k->type == X_SOCK_STREAM && !buddy &&
            ((at_lobby && (want == 21810 || want == lport)) ||
             (s_dir_ip && h.sin_addr.s_addr == s_dir_ip && want == s_dir_port));
        lobby_st *st = NULL, *old = NULL;
        if (lobby && at_lobby && want != lport) {
            xnet_hle_log("connect: lobby port %u -> %u", (unsigned)want, (unsigned)lport);
            h.sin_port = htons(lport);
        }
        if (lobby && !(st = (lobby_st *)calloc(1, sizeof *st))) {
            *werr = WSAENOBUFS;
            return X_SOCKET_ERROR;
        }
        lock();
        old = k->st;
        k->st = st;
        k->lobby = lobby;
        k->lobby_ok = 0;
        k->code_sent = 0;
        k->active = 1;
        k->connecting = 0;
        k->conn_failed = 0;
        k->conn_err = 0;
        unlock();
        free(old);
    }
    port_out(&h, k->type == X_SOCK_DGRAM);
    pin_source(k);
    if (connect(k->fd, (struct sockaddr *)&h, sizeof h) != 0) {
        *werr = map_err(host_errno());
        xnet_hle_log("connect(%u, %s) = -1, WSA %d", s, sa_str(&h, b, sizeof b), *werr);
        if (k->lobby && *werr != WSAEWOULDBLOCK)
            lobby_unreachable();
        if (*werr == WSAEWOULDBLOCK && k->type == X_SOCK_STREAM) {
            lock();
            k->connecting = 1;
            unlock();
        }
        return X_SOCKET_ERROR;
    }
    xnet_hle_log("connect(%u, %s) = 0", s, sa_str(&h, b, sizeof b));
    return 0;
}

int xnet_hle_connect(uint32_t s, const void *name, int len, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = connect_k(k, s, name, len, werr);
    release(k);
    return r;
}

static int listen_k(xsock *k, uint32_t s, int backlog, int *werr)
{
    lock();
    k->active = 1;
    unlock();
    if (listen(k->fd, backlog) != 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    xnet_hle_log("listen(%u)", s);
    return 0;
}

int xnet_hle_listen(uint32_t s, int backlog, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = listen_k(k, s, backlog, werr);
    release(k);
    return r;
}

uint32_t xnet_hle_accept(uint32_t s, void *addr, int32_t *len, int *werr)
{
    struct sockaddr_in h;
    socklen_t n = sizeof h;
    host_sock fd;
    int i, nb;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_INVALID_SOCKET;
    }
    fd = accept(k->fd, (struct sockaddr *)&h, &n);
    if (fd == HOST_BAD) {
        *werr = map_err(host_errno());
        release(k);
        return X_INVALID_SOCKET;
    }
    lock();
    nb = k->nonblock;
    unlock();
    release(k);
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    /* Winsock: an accepted socket takes the listening one's blocking mode
     * (a POSIX one does not) */
    if (nb && set_nonblock(fd, 1) != 0)
        nb = 0;
    lock();
    for (i = 0; i < MAX_SOCK && s_tab[i].used; i++)
        ;
    if (i == MAX_SOCK) {
        unlock();
        host_close(fd);
        *werr = WSAEMFILE;
        return X_INVALID_SOCKET;
    }
    /* every field, as socket() does (BO-337 M1): connected, so select
     * reports it, and nothing left from the slot's last socket */
    memset(&s_tab[i], 0, sizeof s_tab[i]);
    s_tab[i].used = 1;
    s_tab[i].fd = fd;
    s_tab[i].type = X_SOCK_STREAM;
    s_tab[i].nonblock = nb;
    s_tab[i].bound = 1;
    s_tab[i].active = 1;
    unlock();
    port_in(&h, 0);
    to_x_sa(&h, addr, len);
    xnet_hle_log("accept(%u) = %u", s, HANDLE_BASE + i);
    return HANDLE_BASE + (uint32_t)i;
}

/* One entry of a select set: the socket held for the call (M2) and what
 * select needs of it, read under the lock. */
typedef struct {
    xsock *k;
    host_sock fd;
    int skip;          /* not reported: TCP not connected/listening, or held */
    int lobby_new;     /* a lobby connection that has not answered yet */
    uint64_t hold_end; /* skipped for the TCP hold: when it ends (ms) */
} sel_t;

static int out_flush(xsock *k);

static void sel_take(sel_t *e, uint32_t h, int rd)
{
    xsock *k = acquire(h);
    e->k = k;
    e->skip = 1;
    e->lobby_new = 0;
    e->hold_end = 0;
    if (!k)
        return;
    lock();
    out_flush(k);   /* what a lobby connection still owes the server (M8) */
    e->fd = k->fd;
    /* Winsock never reports a TCP socket that has not connected or
     * listened; a host poll says POLLOUT|POLLHUP for it, and the title then
     * takes it for a dead connection (DirtySock's SocketInfo 'stat',
     * 0x220E80). */
    e->skip = (k->type == X_SOCK_STREAM && !k->active) ||
              (rd && k->type == X_SOCK_STREAM && k->hold_until && now_ms() < k->hold_until);
    if (e->skip && k->type == X_SOCK_STREAM && k->active)
        e->hold_end = k->hold_until;
    e->lobby_new = k->lobby && !k->lobby_ok;
    unlock();
}

#ifndef _WIN32
/* Where a TCP socket's non-blocking connect stands after a poll with these
 * revents: 0 still running, 1 connected (or never connecting), 2 failed.
 * The failure is kept (Winsock keeps reporting it) and its error is held for
 * SO_ERROR, since reading it here clears the host's copy. */
static int conn_state(xsock *k, short rev)
{
    int st, err = 0, lobby_fail = 0;
    socklen_t n = sizeof err;
    lock();
    if (k->conn_failed) {
        unlock();
        return 2;
    }
    if (!k->connecting) {
        unlock();
        return 1;
    }
    if (!(rev & (POLLIN | POLLOUT | POLLERR | POLLHUP))) {
        unlock();
        return 0;
    }
    if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &n) != 0)
        err = errno;
    if (!err && (rev & (POLLERR | POLLHUP)) && !(rev & (POLLIN | POLLOUT)))
        err = ECONNREFUSED;   /* failed, but the host gave no reason */
    k->connecting = 0;
    if (err) {
        k->conn_failed = 1;
        k->conn_err = err;
        lobby_fail = k->lobby && !k->lobby_ok;
        st = 2;
    } else {
        st = 1;
    }
    unlock();
    if (lobby_fail)
        lobby_unreachable();
    return st;
}
#endif

/* fd_set: u32 count, u32 handles[64]. */
static void sim_pump(void);
static void ping_tick(void);

/* One pass: wait at most wait_ms (-1: no limit), less when a socket held
 * for the TCP hold would come free sooner (*early is then set: the caller
 * looks again when this pass found nothing, T6). */
static int select_once(void *rd, void *wr, void *ex, int wait_ms, int *werr, int *early)
{
    void *xs[3] = { rd, wr, ex };
    uint32_t cnt[3] = { 0, 0, 0 }, *arr[3] = { NULL, NULL, NULL };
    uint32_t i, j, out;
    int total = 0, r = 0;
#ifdef _WIN32
    fd_set hs[3];
    int n, nset = 0;
    struct timeval t;
#else
    struct pollfd p[192];
    int idx[3][64];
    int np = 0, n;
#endif
    int ms = wait_ms;
    uint64_t now;
    sel_t ep[3][64];   /* per call: two threads may select at once */
    if (xn_on())
        sim_pump();
    ping_tick();
    for (j = 0; j < 3; j++) {
        if (!xs[j]) continue;
        cnt[j] = ((uint32_t *)xs[j])[0];
        arr[j] = (uint32_t *)xs[j] + 1;
        if (cnt[j] > 64) cnt[j] = 64;
        for (i = 0; i < cnt[j]; i++)
            sel_take(&ep[j][i], arr[j][i], j == 0);
    }
    /* a held socket comes free at its hold's end: wait no longer than that */
    *early = 0;
    now = now_ms();
    for (i = 0; i < cnt[0]; i++)
        if (ep[0][i].hold_end > now && (ms < 0 || ep[0][i].hold_end - now < (uint64_t)ms)) {
            ms = (int)(ep[0][i].hold_end - now);
            *early = 1;
        }
#ifdef _WIN32
    for (j = 0; j < 3; j++) {
        FD_ZERO(&hs[j]);
        for (i = 0; i < cnt[j]; i++)
            if (ep[j][i].k && !ep[j][i].skip) {
                FD_SET(ep[j][i].fd, &hs[j]);
                nset++;
            }
    }
    if (ms >= 0) { t.tv_sec = ms / 1000; t.tv_usec = (ms % 1000) * 1000; }
    n = select(0, xs[0] ? &hs[0] : NULL, xs[1] ? &hs[1] : NULL, xs[2] ? &hs[2] : NULL, ms >= 0 ? &t : NULL);
    if (n == SOCKET_ERROR && nset == 0) {
        if (ms >= 0) Sleep((DWORD)ms);
        n = 0;
    }
    if (n == SOCKET_ERROR) {
        *werr = map_err(host_errno());
        r = X_SOCKET_ERROR;
        goto done;
    }
    for (j = 0; j < 3; j++) {
        if (!xs[j]) continue;
        out = 0;
        for (i = 0; i < cnt[j]; i++) {
            const sel_t *q = &ep[j][i];
            if (q->k && !q->skip && FD_ISSET(q->fd, &hs[j])) {
                /* a failed non-blocking connect is in the except set */
                if (j == 2 && q->lobby_new)
                    lobby_unreachable();
                arr[j][out++] = arr[j][i];
            }
        }
        ((uint32_t *)xs[j])[0] = out;
        total += (int)out;
    }
#else
    for (j = 0; j < 3; j++)
        for (i = 0; i < cnt[j]; i++) {
            idx[j][i] = -1;
            if (!ep[j][i].k || ep[j][i].skip)
                continue;
            p[np].fd = ep[j][i].fd;
            p[np].events = (short)(j == 0 ? POLLIN : j == 1 ? POLLOUT : POLLPRI);
            p[np].revents = 0;
            idx[j][i] = np++;
        }
    do {
        n = poll(p, (nfds_t)np, ms);   /* a signal: wait again (the full time; the title polls often) */
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        *werr = map_err(errno);
        r = X_SOCKET_ERROR;
        goto done;
    }
    for (j = 0; j < 3; j++) {
        if (!xs[j]) continue;
        out = 0;
        for (i = 0; i < cnt[j]; i++) {
            int q = idx[j][i], cs;
            short want = (short)(j == 0 ? (POLLIN | POLLHUP | POLLERR) :
                                 j == 1 ? (POLLOUT | POLLERR | POLLHUP) : POLLPRI);
            if (q < 0)
                continue;
            /* A non-blocking connect: Winsock says nothing while it runs,
             * "writable" when it succeeds, and only "except" when it fails
             * (the host says POLLOUT|POLLERR|POLLHUP for that). */
            cs = conn_state(ep[j][i].k, p[q].revents);
            if (cs == 0)
                continue;
            if (cs == 2) {
                if (j == 2)
                    arr[j][out++] = arr[j][i];
                continue;
            }
            if (p[q].revents & want)
                arr[j][out++] = arr[j][i];
            if ((p[q].revents & (POLLERR | POLLHUP)) && ep[j][i].lobby_new)
                lobby_unreachable();
        }
        ((uint32_t *)xs[j])[0] = out;
        total += (int)out;
    }
#endif
    r = total;
done:
    for (j = 0; j < 3; j++)
        for (i = 0; i < cnt[j]; i++)
            release(ep[j][i].k);
    return r;
}

int xnet_hle_select(void *rd, void *wr, void *ex, const int32_t *tv, int *werr)
{
    void *xs[3] = { rd, wr, ex };
    uint32_t save[3][65];
    uint64_t deadline = 0;
    int j, r, early, any = 0, wait;
    for (j = 0; j < 3; j++) {
        uint32_t c;
        if (!xs[j]) continue;
        c = ((uint32_t *)xs[j])[0];
        if (c > 64) c = 64;
        any |= c != 0;
        memcpy(save[j], xs[j], (c + 1) * sizeof(uint32_t));
    }
    /* Winsock: nothing to wait on is an error, not a wait for ever */
    if (!any && !tv) {
        *werr = WSAEINVAL;
        return X_SOCKET_ERROR;
    }
    if (tv)
        deadline = now_ms() + (uint64_t)(tv[0] * 1000 + tv[1] / 1000);
    for (;;) {
        uint64_t now = now_ms();
        wait = !tv ? -1 : deadline > now ? (int)(deadline - now) : 0;
        r = select_once(rd, wr, ex, wait, werr, &early);
        if (r != 0 || !early || (tv && now_ms() >= deadline))
            return r;
        for (j = 0; j < 3; j++)   /* look again with the sets as given */
            if (xs[j])
                memcpy(xs[j], save[j], (save[j][0] > 64 ? 65 : save[j][0] + 1) * sizeof(uint32_t));
    }
}

/* TAKEDAWN_NET_TS=1: every UDP datagram with a millisecond clock and its first
 * 16 bytes (the desync study: send and receive times of the same message). */
static void tsdump(const char *what, const void *buf, int n)
{
    static int on = -1;
    const uint8_t *b = (const uint8_t *)buf;
    char hx[40];
    int i, o = 0;
    if (on < 0) {
        const char *e = getenv("TAKEDAWN_NET_TS");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    for (i = 0; i < n && i < 16; i++)
        o += snprintf(hx + o, sizeof hx - (size_t)o, "%02x", b[i]);
    xnet_hle_log("ts %s %llu %d %s", what, (unsigned long long)now_ms(), n, hx);
}

static void dump_from(const char *what, uint32_t s, const void *buf, int n, const char *from);

static void dump(const char *what, uint32_t s, const void *buf, int n)
{
    dump_from(what, s, buf, n, NULL);
}

/* from: where it came from and whose the title was told it is (BO-345), or NULL */
static void dump_from(const char *what, uint32_t s, const void *buf, int n, const char *from)
{
    static int count;
    /* 128 characters: enough for a lobby request's fields (test scripts
     * read them from the log) */
    char line[128 + 1];
    const uint8_t *b = (const uint8_t *)buf;
    int i, m = n < 128 ? n : 128, o = 0;
    if (!trace_n(&count))
        return;
    for (i = 0; i < m; i++)
        o += snprintf(line + o, sizeof line - (size_t)o, "%c",
                      (b[i] >= 32 && b[i] < 127) ? b[i] : '.');
    if (from)
        xnet_hle_log("%s(%u) %d bytes from %s: %s", what, s, n, from, line);
    else
        xnet_hle_log("%s(%u) %d bytes: %s", what, s, n, line);
}

static int recv_k(xsock *k, uint32_t s, void *buf, int len, int flags, int *werr)
{
    int n;
    if (k->nonblock && held(k)) {
        *werr = WSAEWOULDBLOCK;
        return X_SOCKET_ERROR;
    }
    do {
        n = (int)recv(k->fd, (char *)buf, (size_t)len, (flags & X_MSG_PEEK) ? MSG_PEEK : 0);
    } while (n < 0 && interrupted());
    if (n < 0) {
        *werr = map_err(host_errno());
        if (k->lobby && *werr != WSAEWOULDBLOCK && !k->lobby_eof) {
            char t[32];
            lock();
            k->lobby_eof = 1;
            xnet_hle_note("lobby connection lost: WSA %d%s", *werr, sid_tag(t, sizeof t));
            unlock();
        }
        return X_SOCKET_ERROR;
    }
    if (k->lobby && n == 0 && len > 0 && !k->lobby_eof) {
        char t[32];
        lock();
        k->lobby_eof = 1;
        xnet_hle_note("the lobby closed the connection%s", sid_tag(t, sizeof t));
        unlock();
    }
    dump("recv", s, buf, n);
    if (k->lobby && n > 0) {
        lock();
        k->lobby_ok = 1;
        out_flush(k);
        unlock();
        if (!(flags & X_MSG_PEEK))
            lobby_stream(k, (const uint8_t *)buf, n);
    }
    return n;
}

int xnet_hle_recv(uint32_t s, void *buf, int len, int flags, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = recv_k(k, s, buf, len, flags, werr);
    release(k);
    return r;
}

/* TAKEDAWN_NET_OPEN=1: hand the title datagrams from anyone (the old way) */
static int net_open(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TAKEDAWN_NET_OPEN");
        on = e && *e && *e != '0';
    }
    return on;
}

static const char *hello_refusal(int r)
{
    switch (r) {
    case XR_HELLO_UNKNOWN:  return "not a player the lobby told us about";
    case XR_HELLO_BADTAG:   return "not signed with this game's secret";
    case XR_HELLO_WRONGSRC: return "not from that player's addresses";
    default:                return "not a hello";
    }
}

/* BO-352: why a reaction was refused (logged, at most 10 lines a 10 s) */
static const char *react_refusal(int r)
{
    switch (r) {
    case XR_REACT_UNKNOWN:  return "not a player of this game";
    case XR_REACT_BADTAG:   return "not signed with this game's secret";
    case XR_REACT_WRONGSRC: return "not from that player's addresses";
    case XR_REACT_DUP:      return "a duplicate";
    case XR_REACT_LIMIT:    return "over the rate limit";
    default:                return "not a reaction";
    }
}

/* ── BO-357: TAKEDAWN_NET_SIM ─────────────────────────────────────────
 * xnet_netsim.h holds the datagrams; here they leave. One going out is sent
 * from its socket when due. One coming in has already left the host's socket
 * buffer, and the title only reads what select calls readable, so when due it
 * is sent to our own socket in a wrapper that keeps its real source, and
 * recvfrom_k takes the wrapper off. All of it behind xn_on(). */
#define SIM_WRAP 10
static const uint8_t k_sim_magic[4] = { 'T', 'K', 'N', 'S' };

static void sim_send(const xn_pkt *p)
{
    struct sockaddr_in to, me;
    socklen_t mn = sizeof me;
    uint8_t out[XN_MAX_DGRAM + SIM_WRAP];
    xsock *k = acquire(p->sock);
    if (!k)
        return;     /* closed while the datagram was on its way */
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    if (p->dir == XN_OUT) {
        to.sin_addr.s_addr = p->ip;
        to.sin_port = htons(p->port);
        sendto(k->fd, (const char *)p->data, (size_t)p->n, 0, (struct sockaddr *)&to, sizeof to);
    } else if (getsockname(k->fd, (struct sockaddr *)&me, &mn) == 0) {
        memcpy(out, k_sim_magic, 4);
        memcpy(out + 4, &p->ip, 4);
        out[8] = (uint8_t)(p->port >> 8);
        out[9] = (uint8_t)p->port;
        memcpy(out + SIM_WRAP, p->data, (size_t)p->n);
        to.sin_addr.s_addr = me.sin_addr.s_addr ? me.sin_addr.s_addr : htonl(INADDR_LOOPBACK);
        to.sin_port = me.sin_port;
        sendto(k->fd, (const char *)out, (size_t)p->n + SIM_WRAP, 0, (struct sockaddr *)&to, sizeof to);
    }
    release(k);
}

/* what is due goes now: every select and send of the title comes here */
static void sim_pump(void)
{
    uint64_t now = now_ms();
    for (;;) {
        xn_pkt p;
        int got;
        lock();
        got = xn_pop(now, &p);
        unlock();
        if (!got)
            return;
        sim_send(&p);
    }
}

/* a peer's datagram going out: 1 when the switch took it (held or lost) */
static int sim_out(uint32_t s, const void *buf, int len, const struct sockaddr_in *to)
{
    if (!xn_on())
        return 0;
    lock();
    xn_push(XN_OUT, s, buf, len, to->sin_addr.s_addr, ntohs(to->sin_port), now_ms());
    unlock();
    return 1;
}

/* BO-357: pings due go out (xnet_route.h), from every socket along every path
 * worth trying, as the reactions do. The title's calls drive it. */
static void reacts_from_all(uint32_t tip, const uint8_t *buf, int n);
static uint64_t s_ping_tick;

static void ping_tick(void)
{
    uint8_t b[XR_PING_LEN];
    uint32_t id;
    uint64_t now = now_ms();
    if (now < s_ping_tick)
        return;
    s_ping_tick = now + 100;
    for (;;) {
        int got;
        lock();
        got = xr_ping_next(now, &id, b);
        unlock();
        if (!got)
            return;
        reacts_from_all(id, b, XR_PING_LEN);
    }
}

int xnet_hle_peer_rtt(uint32_t title_ip, unsigned *min_ms, unsigned *med_ms, int *sent, int *lost)
{
    xr_rtt r;
    int got;
    lock();
    got = xr_peer_rtt(title_ip, now_ms(), &r);
    unlock();
    if (!got)
        return 0;
    if (min_ms) *min_ms = r.min_ms;
    if (med_ms) *med_ms = r.med_ms;
    if (sent) *sent = r.sent;
    if (lost) *lost = r.lost;
    return 1;
}

static void relay_send(host_sock fd, const uint8_t *wrapped, int n);

/* BO-381: whether a relay is known (its datagrams need our own buffer) */
static int relay_known(void)
{
    int r;
    lock();
    r = xr_relay_ep().port != 0;
    unlock();
    return r;
}

static int recvfrom_k(xsock *k, uint32_t s, void *ubuf, int ulen, int flags,
                      void *from, int32_t *fromlen, int *werr)
{
    struct sockaddr_in h, hsrc;
    socklen_t hn = sizeof h;
    int n, rounds = 0, known = 0;
    uint32_t kid = 0, rl_from = 0;
    uint16_t ktp = 0, rl_ftp = 0;
    /* with the switch on a wrapped datagram may be longer than the title's
     * buffer: read to our own; so may a relayed one (BO-381), so our own
     * buffer whenever a relay is known (own: copied out at the end) */
    uint8_t simbuf[XN_MAX_DGRAM + SIM_WRAP + XR_RELAY_HDR];
    int sim = xn_on() && k->type == X_SOCK_DGRAM && !(flags & X_MSG_PEEK);
    int own = sim || (k->type == X_SOCK_DGRAM && relay_known());
    void *buf = own ? (void *)simbuf : ubuf;
    int len = own ? (int)sizeof simbuf : ulen;
    for (;;) {
        xr_ep src;
        uint32_t id = 0;
        int r, np;
        uint64_t now;
        memset(&h, 0, sizeof h);
        hn = sizeof h;
        do {
            n = (int)recvfrom(k->fd, (char *)buf, (size_t)len, (flags & X_MSG_PEEK) ? MSG_PEEK : 0,
                              (struct sockaddr *)&h, &hn);
        } while (n < 0 && interrupted());
        if (n < 0) {
            *werr = map_err(host_errno());
            return X_SOCKET_ERROR;
        }
        if (k->type != X_SOCK_DGRAM)
            break;
        src.ip = h.sin_addr.s_addr;
        src.port = ntohs(h.sin_port);
        now = now_ms();
        if (sim) {
            if (n >= SIM_WRAP && !memcmp(buf, k_sim_magic, 4)) {
                /* back from the queue: the datagram as it arrived */
                memcpy(&src.ip, simbuf + 4, 4);
                src.port = (uint16_t)(simbuf[8] << 8 | simbuf[9]);
                h.sin_addr.s_addr = src.ip;
                h.sin_port = htons(src.port);
                n -= SIM_WRAP;
                memmove(simbuf, simbuf + SIM_WRAP, (size_t)n);
            } else {
                int peer;
                lock();
                peer = xr_is_hello((const uint8_t *)buf, n) || xr_is_ping((const uint8_t *)buf, n) ||
                       xr_is_react((const uint8_t *)buf, n) || xr_known_source(src) ||
                       (xr_is_relay_pkt((const uint8_t *)buf, n) && xr_is_relay_src(src));
                if (peer)
                    xn_push(XN_IN, s, buf, n, src.ip, src.port, now);
                unlock();
                if (peer) {
                    if (++rounds >= 32) {
                        *werr = WSAEWOULDBLOCK;
                        return X_SOCKET_ERROR;
                    }
                    continue;
                }
            }
        }
        /* BO-381: a datagram the relay forwarded: checked and unwrapped
         * here; what is inside then goes its usual way, as the peer's */
        rl_from = 0;
        if (xr_is_relay_pkt((const uint8_t *)buf, n)) {
            uint32_t from = 0;
            uint16_t ftp = 0, ttp = 0;
            int kind = 0, off = 0, rr;
            lock();
            rr = xr_is_relay_src(src) ? xr_relay_unwrap((const uint8_t *)buf, n, &from, &ftp, &ttp, &kind, &off)
                                      : XR_RELAY_UNKNOWN;
            if (rr == XR_RELAY_OK && kind == XR_RELAY_ANSWER && n - off >= 6) {
                /* the peer's endpoint as the relay saw it: a direct candidate
                 * (the answer's from-port is the peer's port, its to-port ours) */
                xr_ep d;
                const uint8_t *q = (const uint8_t *)buf + off;
                memcpy(&d.ip, q, 4);
                d.port = (uint16_t)(q[4] << 8 | q[5]);
                xr_relay_answer(from, ftp, d);
            }
            unlock();
            if (rr == XR_RELAY_OK && kind == XR_RELAY_DATA && ttp == k->tport && n - off > 0) {
                rl_from = from;
                rl_ftp = ftp;
                n -= off;
                memmove(buf, (const uint8_t *)buf + off, (size_t)n);
            } else {
                if (flags & X_MSG_PEEK) {
                    uint8_t drop[64];
                    recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
                }
                if (rr != XR_RELAY_OK && xr_log_ok(now)) {
                    char b1[40];
                    xnet_hle_log("relayed datagram from %s refused (%d)", sa_str(&h, b1, sizeof b1), rr);
                }
                if (++rounds >= 32) {
                    *werr = WSAEWOULDBLOCK;
                    return X_SOCKET_ERROR;
                }
                continue;
            }
        }
        if (xr_is_ping((const uint8_t *)buf, n)) {
            /* a peer's ping or its answer (BO-357): before the title, as the hellos */
            uint8_t reply[XR_PING_LEN];
            unsigned ms = 0;
            if (flags & X_MSG_PEEK) {
                uint8_t drop[XR_PING_LEN];
                recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
            }
            lock();
            if (rl_from)
                xr_relay_trust(rl_from);
            r = xr_ping_seen(src, (const uint8_t *)buf, n, &id, reply, &ms, now);
            unlock();
            if (r == XR_PING_ANSWER_IT && rl_from) {
                /* back the way it came: through the relay */
                uint8_t w[XR_RELAY_HDR + XR_PING_LEN];
                int nw;
                lock();
                nw = xr_relay_wrap(w, (int)sizeof w, rl_from, k->tport, rl_ftp, reply, XR_PING_LEN);
                unlock();
                if (nw > 0)
                    relay_send(k->fd, w, nw);
            } else if (r == XR_PING_ANSWER_IT && !sim_out(s, reply, XR_PING_LEN, &h))
                sendto(k->fd, (const char *)reply, XR_PING_LEN, 0, (struct sockaddr *)&h, sizeof h);
            if ((r == XR_PING_UNKNOWN || r == XR_PING_BADTAG || r == XR_PING_WRONGSRC) && xr_log_ok(now)) {
                char b1[40];
                xnet_hle_log("ping from %s refused (%d)", sa_str(&h, b1, sizeof b1), r);
            }
        } else if (xr_is_react((const uint8_t *)buf, n)) {
            /* a peer's reaction (BO-352, xnet_route.h): noted and dropped
             * before the title, like the hellos */
            if (flags & X_MSG_PEEK) {
                uint8_t drop[XR_REACT_LEN];
                recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
            }
            {
                int kind = 0, value = 0;
                lock();
                if (rl_from)
                    xr_relay_trust(rl_from);
                r = xr_react_seen(src, (const uint8_t *)buf, n, &id, &kind, &value, now);
                unlock();
                /* duplicates (the double-send) and over-limit packets are
                 * dropped silently; an unsigned or spoofed one is worth a
                 * line, at the log budget */
                if ((r == XR_REACT_UNKNOWN || r == XR_REACT_BADTAG || r == XR_REACT_WRONGSRC) && xr_log_ok(now)) {
                    char b1[40];
                    xnet_hle_log("reaction from %s refused: %s", sa_str(&h, b1, sizeof b1), react_refusal(r));
                }
                (void)kind; (void)value;
            }
        } else if (xr_is_vote((const uint8_t *)buf, n)) {
            /* the track vote's packet: as a reaction */
            if (flags & X_MSG_PEEK) {
                uint8_t drop[XR_VOTE_LEN];
                recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
            }
            lock();
            if (rl_from)
                xr_relay_trust(rl_from);
            r = xr_vote_seen(src, (const uint8_t *)buf, n, &id, now);
            unlock();
            if ((r == XR_REACT_UNKNOWN || r == XR_REACT_BADTAG || r == XR_REACT_WRONGSRC) && xr_log_ok(now)) {
                char b1[40];
                xnet_hle_log("vote packet from %s refused: %s", sa_str(&h, b1, sizeof b1), react_refusal(r));
            }
        } else if (xr_is_hello((const uint8_t *)buf, n)) {
            if (flags & X_MSG_PEEK) {
                uint8_t drop[XR_HELLO_LEN];
                recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
            }
            lock();
            if (rl_from)
                r = xr_relay_hello_seen(rl_from, rl_ftp, k->tport, (const uint8_t *)buf, n, now);
            else
                r = xr_hello_seen_on(src, k->tport, (const uint8_t *)buf, n, &id, now);
            unlock();
            if (rl_from)
                id = rl_from;
            if (r == XR_HELLO_ONEWAY) {
                /* once per peer and path: the other side is not there yet */
                static uint32_t said_id;
                static uint16_t said_port;
                if (said_id != id || said_port != src.port) {
                    char b1[40];
                    struct in_addr a;
                    said_id = id;
                    said_port = src.port;
                    a.s_addr = id;
                    xnet_hle_log("direct hello from %s (the player at %s) while on the relay: our direct hellos do not"
                                 " reach it yet, the relay stays", sa_str(&h, b1, sizeof b1), inet_ntoa(a));
                }
            } else if (r != XR_HELLO_OLD && (r == XR_HELLO_NEW || xr_log_ok(now))) {
                char b1[40];
                struct in_addr a;
                a.s_addr = id;
                if (r == XR_HELLO_NEW && rl_from)
                    xnet_hle_log("hello through the relay (%s): that is the player at %s; its datagrams come that way"
                                 " until a direct path works both ways", sa_str(&h, b1, sizeof b1), inet_ntoa(a));
                else if (r == XR_HELLO_NEW)
                    xnet_hle_log("hello from %s: that is the player at %s", sa_str(&h, b1, sizeof b1), inet_ntoa(a));
                else
                    xnet_hle_log("hello from %s refused: %s", sa_str(&h, b1, sizeof b1), hello_refusal(r));
            }
        } else if (rl_from) {
            /* the title's datagram through the relay: the wrapper named the
             * player and its port, and the tag was checked */
            known = 1;
            kid = rl_from;
            ktp = rl_ftp;
            break;
        } else {
            lock();
            known = xr_source_on(src, k->tport, &kid, &ktp);
            np = xr_peer_count();
            unlock();
            if (known || !np || net_open())
                break;
            /* a datagram from someone who is not a player of the games the
             * lobby told us about: never to the title */
            if (flags & X_MSG_PEEK) {
                uint8_t drop[64];
                recvfrom(k->fd, (char *)drop, sizeof drop, 0, NULL, NULL);
            }
            if (xr_log_ok(now)) {
                char b1[40];
                xnet_hle_log("dropped a datagram from %s: not a player of this game", sa_str(&h, b1, sizeof b1));
            }
        }
        /* nothing for the title yet: at most a few rounds per call, so a
         * flood of hellos or strangers cannot hold the title here */
        if (++rounds >= 32) {
            *werr = WSAEWOULDBLOCK;
            return X_SOCKET_ERROR;
        }
    }
    hsrc = h;
    if (known) {
        h.sin_addr.s_addr = kid;
        h.sin_port = htons(ktp);
    } else {
        port_in(&h, k->type == X_SOCK_DGRAM);
    }
    {
        char b1[40], b2[64];
        struct in_addr a;
        a.s_addr = kid;
        if (k->type == X_SOCK_DGRAM && known)
            snprintf(b2, sizeof b2, "%s as %s:%u", sa_str(&hsrc, b1, sizeof b1), inet_ntoa(a), (unsigned)ktp);
        else
            snprintf(b2, sizeof b2, "%s", sa_str(&hsrc, b1, sizeof b1));
        to_x_sa(&h, from, fromlen);
        dump_from("recvfrom", s, buf, n, b2);
    }
    tsdump("rx", buf, n);
    if (own) {
        if (n > ulen)
            n = ulen;
        memcpy(ubuf, buf, (size_t)n);
    }
    return n;
}

int xnet_hle_recvfrom(uint32_t s, void *buf, int len, int flags,
                      void *from, int32_t *fromlen, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = recvfrom_k(k, s, buf, len, flags, from, fromlen, werr);
    release(k);
    return r;
}

static int send_flags(void)
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

/* under the lock: what a lobby connection still owes the server of a request
 * we lengthened (M8), sent as far as the socket takes it without waiting;
 * the bytes left (an error shows on the title's next call) */
static int out_flush(xsock *k)
{
    lobby_st *st = k->st;
    int n, f = send_flags();
#ifdef MSG_DONTWAIT
    f |= MSG_DONTWAIT;
#endif
    if (!st || !st->outlen)
        return 0;
#ifndef MSG_DONTWAIT
    if (!k->nonblock)
        return st->outlen;   /* only a non-blocking socket leaves a rest (below) */
#endif
    for (;;) {
        n = (int)send(k->fd, (const char *)st->out, (size_t)st->outlen, f);
        if (n > 0) {
            memmove(st->out, st->out + n, (size_t)(st->outlen - n));
            st->outlen -= n;
            if (!st->outlen)
                break;
            continue;
        }
        if (n < 0 && interrupted())
            continue;
        break;
    }
    return st->outlen;
}

/* A lobby request we send lengthened (TKID=1 and TKNAME= on addr, TKCODE= with a game
 * code): the title is told its own length went out. Without waiting (M8):
 * when the socket takes none of it the title is told WSAEWOULDBLOCK and sends
 * it again (nothing went, so nothing twice); when it takes part, the rest
 * waits in the connection's queue, sent before anything else. */
static xnet_hle_lobby_tag_fn s_lobby_tag;

void xnet_hle_set_lobby_tag(xnet_hle_lobby_tag_fn fn)
{
    lock();
    s_lobby_tag = fn;
    unlock();
}

/* One whole lobby packet with our lines added (TKID/TKNAME on addr, the
 * game's tag, TKCODE): its new length in out, 0 when it goes as it is.
 * *code_added: it carried a TKCODE line. */
static int tag_one(const uint8_t *in, int len, const char *code, int code_sent, xnet_hle_lobby_tag_fn tag,
                   uint8_t *out, int cap, int *code_added)
{
    uint8_t ta[4200], tb[4200];   /* each step writes to the one p is not in */
    const uint8_t *p = in;
#define NEXT_T (p == ta ? tb : ta)
    int m = len, any = 0, r;
    /* the identity scheme (BO-337 H1): this client takes a synthetic address */
    if (!memcmp(in, "addr", 4) && (r = xl_add_line(p, m, "TKID=1", NEXT_T, (int)sizeof ta)) > 0) {
        p = NEXT_T;
        m = r;
        any = 1;
        /* and the gamertag (BO-343): the server keeps a player's address for
         * a while after a sign-in ends and gives it back to the same name from
         * the same public address, so the others' routes to it stay right.
         * Only printable characters, no '=' (one KEY=VALUE line). */
        {
            char nm[8 + sizeof s_gamertag];
            size_t o = 7, i;
            memcpy(nm, "TKNAME=", 7);
            for (i = 0; s_gamertag[i] && o < sizeof nm - 1; i++)
                if ((unsigned char)s_gamertag[i] >= 0x20 && s_gamertag[i] != 0x7F && s_gamertag[i] != '=')
                    nm[o++] = s_gamertag[i];
            nm[o] = 0;
            if (o > 7 && (r = xl_add_line(p, m, nm, NEXT_T, (int)sizeof ta)) > 0) {
                p = NEXT_T;
                m = r;
            }
        }
    }
    /* the game's own field (BO-351). It used to be added by the game around
     * xnet_hle_send, which then returned the tagged length: the title's lobby
     * send queue moved past the end of what it had queued, its next walk of
     * that queue read a zero packet length there and guest-main spun forever
     * (the int044 Host1 freeze right after a kick). */
    if (tag && (r = tag(p, m, NEXT_T, (int)sizeof ta)) > 0 && r < (int)sizeof ta) {
        p = NEXT_T;
        m = r;
        any = 1;
    }
    /* a game code (BO-335); no code now, but one went out on this connection:
     * "TKCODE=" on move and sele clears it at the server */
    if ((code[0] || code_sent) && (r = xl_add_code(p, m, code, NEXT_T, (int)sizeof ta)) > 0) {
        p = NEXT_T;
        m = r;
        any = 1;
        *code_added = 1;
    }
#undef NEXT_T
    if (!any || m > cap)
        return 0;
    memcpy(out, p, (size_t)m);
    return m;
}

/* the big-endian packet length at b + 8 */
static int pkt_len(const uint8_t *b)
{
    return (int)((uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11]);
}

static int send_lobby(xsock *k, uint32_t s, const uint8_t *buf, int len, int *werr)
{
    uint8_t all[8192], one[4200];
    xnet_hle_lobby_tag_fn tag;
    const uint8_t *p = all;
    char code[XL_CODE_MAX + 1];
    int m = 0, any = 0, code_sent, code_added = 0, gjoi = 0, pending, sent = 0, n = 0, r, e = 0, off;
    lock();
    pending = out_flush(k);
    memcpy(code, s_code, sizeof code);
    code_sent = k->code_sent;
    tag = s_lobby_tag;
    unlock();
    if (pending) {
        *werr = WSAEWOULDBLOCK;
        return X_SOCKET_ERROR;
    }
    if (len < 12 || len >= 4000)
        return 0;
    /* Whole packets only, each tagged on its own (T9: two in one send kept
     * none of their lines); a send that ends mid-packet goes as it is. */
    for (off = 0; off < len; ) {
        int plen = len - off >= 12 ? pkt_len(buf + off) : 0;
        if (plen < 12 || plen > len - off)
            return 0;
        off += plen;
    }
    for (off = 0; off < len; ) {
        int plen = pkt_len(buf + off);
        const uint8_t *q = buf + off;
        r = tag_one(q, plen, code, code_sent, tag, one, (int)sizeof one, &code_added);
        if (r > 0) {
            any = 1;
            q = one;
        } else {
            r = plen;
        }
        if (m + r > (int)sizeof all) {
            xnet_hle_log("send(%u): %d bytes of lobby packets too long to tag: sent as they are", s, len);
            return 0;
        }
        memcpy(all + m, q, (size_t)r);
        m += r;
        if (!memcmp(buf + off, "gjoi", 4))
            gjoi = 1;
        off += plen;
    }
    if (!any)
        return 0;   /* as it is (send_k) */
    while (sent < m) {
        n = (int)send(k->fd, (const char *)p + sent, (size_t)(m - sent), send_flags());
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n < 0 && interrupted())
            continue;
        e = n < 0 ? map_err(host_errno()) : WSAECONNRESET;
        break;
    }
    if (sent == 0) {
        *werr = e;
        return X_SOCKET_ERROR;
    }
    if (sent < m) {
        if (e != WSAEWOULDBLOCK) {   /* the connection broke on the way */
            *werr = e;
            return X_SOCKET_ERROR;
        }
        lock();
        if (k->st && m - sent <= (int)sizeof k->st->out - k->st->outlen) {
            memcpy(k->st->out + k->st->outlen, p + sent, (size_t)(m - sent));
            k->st->outlen += m - sent;
            e = 0;
        }
        unlock();
        if (e) {
            *werr = WSAECONNABORTED;
            return X_SOCKET_ERROR;
        }
    }
    lock();
    hold_locked(k);
    /* only a send that carried TKCODE changes what the server holds (T8):
     * a kick-tagged gset without one leaves it as it was */
    if (code_added)
        k->code_sent = code[0] != 0;
    unlock();
    if (gjoi && code[0]) {
        /* a code also finds a public game (CREATE GAME's, BO-363): no "private" */
        char msg[120];
        snprintf(msg, sizeof msg, "Joining game %s.", code);
        xnet_hle_message(msg);
    }
    if (gjoi && s_on == 1)
        xnet_upnp_host(0);
    dump("send", s, p, m);
    return len;
}

static int send_k(xsock *k, uint32_t s, const void *buf, int len, int flags, int *werr)
{
    int n, lobby;
    (void)flags;
    lock();
    lobby = k->lobby;
    unlock();
    if (lobby) {
        n = send_lobby(k, s, (const uint8_t *)buf, len, werr);
        if (n > 0) {
            lock();
            lv_out((const uint8_t *)buf, len);
            unlock();
        }
        if (n)
            return n;
    }
    do {
        n = (int)send(k->fd, (const char *)buf, (size_t)len, send_flags());
    } while (n < 0 && interrupted());
    if (n < 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    if (lobby) {
        lock();
        lv_out((const uint8_t *)buf, n);
        unlock();
    }
    if (k->type == X_SOCK_STREAM)
        hold(k);
    /* joining someone else's game: no forwarding (it starts when our own
     * gcre is answered, lobby_packet) */
    if (lobby && n >= 4 && s_on == 1 && !memcmp(buf, "gjoi", 4))
        xnet_upnp_host(0);
    dump("send", s, buf, n);
    return n;
}

int xnet_hle_send(uint32_t s, const void *buf, int len, int flags, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    r = send_k(k, s, buf, len, flags, werr);
    release(k);
    return r;
}

/* BO-381: a wrapped datagram to the relay, from the socket whose path it
 * carries (the relay binds that socket's own router mapping) */
static void relay_send(host_sock fd, const uint8_t *wrapped, int n)
{
    struct sockaddr_in to;
    xr_ep r;
    lock();
    r = xr_relay_ep();
    unlock();
    if (!r.port)
        return;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = r.ip;
    to.sin_port = htons(r.port);
    sendto(fd, (const char *)wrapped, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
}

/* Hellos to peer tip from each of the title's UDP sockets (1000, 6000 ...),
 * to the same port of the peer, when one is due: a socket that only
 * receives would otherwise never open its path through the routers. */
static void hellos_from_all(uint32_t tip)
{
    int i, c, nc;
    for (i = 0; i < MAX_SOCK; i++) {
        struct sockaddr_in to;
        uint8_t hello[XR_HELLO_LEN], wrapped[XR_RELAY_HDR + XR_HELLO_LEN];
        xr_ep ep, cand[XR_MAX_CAND];
        const char *why;
        int ch, due = 0, nw = 0;
        host_sock fd;
        uint16_t myport;
        lock();
        if (!s_tab[i].used || s_tab[i].closing || s_tab[i].type != X_SOCK_DGRAM || !s_tab[i].tport ||
            s_tab[i].tport >= XR_TITLE_PORTS) {
            unlock();
            continue;
        }
        s_tab[i].refs++;   /* its fd stays ours until the hellos are out (M2) */
        fd = s_tab[i].fd;
        myport = s_tab[i].tport;
        (void)ch;
        if (xr_route(tip, myport, &ep, &why, NULL) == 1 && xr_hello_due(tip, myport, now_ms())) {
            due = 1;
            xr_hello_make_for(hello, myport, tip);
            nc = xr_candidates(tip, myport, cand);
            if (xr_relay_want(tip, myport, now_ms()))
                nw = xr_relay_wrap(wrapped, (int)sizeof wrapped, tip, myport, myport, hello, (int)sizeof hello);
        }
        unlock();
        if (due && xr_has_secret()) {
            static int said_seq;
            if (said_seq != xr_secret_seq()) {
                said_seq = xr_secret_seq();
                xnet_hle_log("hellos carry this game's tag (secret %d)", said_seq);
            }
        }
        for (c = 0; due && c < nc; c++) {
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = cand[c].ip;
            to.sin_port = htons(cand[c].port);
            sendto(fd, (const char *)hello, sizeof hello, 0, (struct sockaddr *)&to, sizeof to);
        }
        if (nw > 0)
            relay_send(fd, wrapped, nw);
        release(&s_tab[i]);
    }
}

static int sendto_k(xsock *k, uint32_t s, const void *buf, int len, int flags,
                    const void *to, int tolen, int *werr)
{
    struct sockaddr_in h;
    char b[40];
    int n;
    static int count;
    (void)flags;
    if (!to || tolen < 8)
        return send_k(k, s, buf, len, flags, werr);
    to_host_sa(to, tolen, &h);
    pin_source(k);
    {
        xr_ep ep, cand[XR_MAX_CAND];
        const char *why = NULL;
        int routed = 0, changed = 0, due = 0, nc = 0, c, nw = 0, relayed = 0, nd = 0;
        uint32_t tip = h.sin_addr.s_addr;
        uint16_t tport = ntohs(h.sin_port);
        uint8_t hello[XR_HELLO_LEN], wrapped[XR_RELAY_HDR + XR_HELLO_LEN];
        uint8_t data[XR_RELAY_HDR + XR_RELAY_PAYLOAD];
        if (k->type == X_SOCK_DGRAM) {
            struct sockaddr_in me;
            socklen_t mn = sizeof me;
            uint16_t myport = 0;
            if (getsockname(k->fd, (struct sockaddr *)&me, &mn) == 0) {
                port_in(&me, 1);
                myport = ntohs(me.sin_port);
            }
            lock();
            routed = xr_route(tip, tport, &ep, &why, &changed);
            due = routed == 1 && myport && xr_hello_due(tip, tport, now_ms());
            if (due) {
                xr_hello_make_for(hello, myport, tip);
                nc = xr_candidates(tip, tport, cand);
                if (xr_relay_want(tip, tport, now_ms()))
                    nw = xr_relay_wrap(wrapped, (int)sizeof wrapped, tip, myport, tport, hello, (int)sizeof hello);
            }
            /* BO-381: the route is the relay: the datagram goes wrapped */
            if (routed == 1 && myport && xr_route_is_relay(tip, tport)) {
                relayed = 1;
                nd = xr_relay_wrap(data, (int)sizeof data, tip, myport, tport, (const uint8_t *)buf, len);
            }
            unlock();
            if (routed == 1)
                hellos_from_all(tip);   /* the other sockets' (this one's is below) */
        }
        if (routed == 2) {
            /* not sent: an address only the player's own XUID claims */
            if (changed) {
                struct in_addr a;
                a.s_addr = tip;
                xnet_hle_log("peer %s:%u: %s", inet_ntoa(a), (unsigned)tport, why);
            }
            tsdump("tx-refused", buf, len);
            return len;
        }
        if (routed) {
            h.sin_addr.s_addr = ep.ip;
            h.sin_port = htons(ep.port);
            if (changed) {
                struct in_addr a;
                a.s_addr = tip;
                xnet_hle_log("peer %s:%u -> %s: %s", inet_ntoa(a), (unsigned)tport, sa_str(&h, b, sizeof b), why);
            }
            /* hellos to each path worth trying (two homes behind one
             * address: the router's too); the first to answer is learned */
            for (c = 0; due && c < nc; c++) {
                struct sockaddr_in hh = h;
                hh.sin_addr.s_addr = cand[c].ip;
                hh.sin_port = htons(cand[c].port);
                sendto(k->fd, (const char *)hello, sizeof hello, 0, (struct sockaddr *)&hh, sizeof hh);
            }
            if (nw > 0)
                relay_send(k->fd, wrapped, nw);
        } else {
            port_out(&h, k->type == X_SOCK_DGRAM);
        }
        if (relayed) {
            /* through the relay (h is its endpoint): the wrapped datagram,
             * or nothing when it does not fit (the title's are far shorter) */
            if (nd <= 0) {
                tsdump("tx-refused", buf, len);
                return len;
            }
            do {
                n = (int)sendto(k->fd, (const char *)data, (size_t)nd, 0, (struct sockaddr *)&h, sizeof h);
            } while (n < 0 && interrupted());
            if (n < 0) {
                *werr = map_err(host_errno());
                return X_SOCKET_ERROR;
            }
            if (trace_n(&count))
                xnet_hle_log("sendto(%u, %s) %d bytes through the relay", s, sa_str(&h, b, sizeof b), len);
            tsdump("tx", buf, len);
            return len;
        }
        if (routed == 1 && k->type == X_SOCK_DGRAM && sim_out(s, buf, len, &h)) {
            tsdump("tx", buf, len);
            return len;
        }
    }
    do {
        n = (int)sendto(k->fd, (const char *)buf, (size_t)len, 0, (struct sockaddr *)&h, sizeof h);
    } while (n < 0 && interrupted());
    if (n < 0 && k->type == X_SOCK_DGRAM) {
        /* BO-349: this machine may not send to the player's LAN address
         * (macOS Local Network privacy: int042, the owner's Mac reached only
         * the router, and no link came up): its public address from now on,
         * through our router, and this datagram again there */
        int er = host_errno();
        struct sockaddr_in t;
        uint32_t at = h.sin_addr.s_addr;
        xr_ep ep;
        const char *why = NULL;
        int again = 0;
        if (lan_refused(er)) {
            to_host_sa(to, tolen, &t);
            lock();
            if (xr_lan_denied(t.sin_addr.s_addr, at) &&
                xr_route(t.sin_addr.s_addr, ntohs(t.sin_port), &ep, &why, NULL) == 1 && ep.ip != at) {
                again = 1;
                h.sin_addr.s_addr = ep.ip;
                h.sin_port = htons(ep.port);
            }
            unlock();
        }
        if (again) {
            struct in_addr ia;
            char b2[40], b3[40];
            ia.s_addr = t.sin_addr.s_addr;
            snprintf(b2, sizeof b2, "%s", inet_ntoa(ia));
            ia.s_addr = at;
            snprintf(b3, sizeof b3, "%s", inet_ntoa(ia));
            xnet_hle_log("peer %s: sending to its local address %s failed (%s): %s from now on, %s", b2, b3,
                         er == HOST_EPERM ? "not permitted" : "no route to host", sa_str(&h, b, sizeof b),
                         why ? why : "");
#ifdef __APPLE__
            xnet_hle_local_network_hint();
#endif
            do {
                n = (int)sendto(k->fd, (const char *)buf, (size_t)len, 0, (struct sockaddr *)&h, sizeof h);
            } while (n < 0 && interrupted());
        } else {
#ifdef _WIN32
            WSASetLastError(er);
#else
            errno = er;
#endif
        }
    }
    if (n < 0) {
        *werr = map_err(host_errno());
        return X_SOCKET_ERROR;
    }
    if (trace_n(&count))
        xnet_hle_log("sendto(%u, %s) %d bytes", s, sa_str(&h, b, sizeof b), n);
    tsdump("tx", buf, n);
    return n;
}

int xnet_hle_sendto(uint32_t s, const void *buf, int len, int flags,
                    const void *to, int tolen, int *werr)
{
    int r;
    xsock *k = acquire(s);
    if (!k) {
        *werr = WSAENOTSOCK;
        return X_SOCKET_ERROR;
    }
    if (xn_on())
        sim_pump();
    r = sendto_k(k, s, buf, len, flags, to, tolen, werr);
    release(k);
    return r;
}

/* ── BO-352: sending reactions ────────────────────────────────────── */

/* the packet being sent again in 60 ms (the double-send; the receiver drops
 * the second by its sequence) */
static uint8_t s_react_out[XR_REACT_LEN];
static int s_react_out_len;
static uint64_t s_react_resend_at;

/* send buf (n bytes) to peer tip from each of the title's UDP sockets, to
 * the peer's matching port, along every path worth trying (as the hellos) */
static void reacts_from_all(uint32_t tip, const uint8_t *buf, int n)
{
    int i, c, nc;
    for (i = 0; i < MAX_SOCK; i++) {
        struct sockaddr_in to;
        xr_ep ep, cand[XR_MAX_CAND];
        uint8_t wrapped[XR_RELAY_HDR + XR_VOTE_LEN];
        const char *why;
        int got = 0, nw = 0;
        host_sock fd;
        uint16_t myport;
        lock();
        if (!s_tab[i].used || s_tab[i].closing || s_tab[i].type != X_SOCK_DGRAM || !s_tab[i].tport ||
            s_tab[i].tport >= XR_TITLE_PORTS) {
            unlock();
            continue;
        }
        s_tab[i].refs++;   /* its fd stays ours until the sends are out (M2) */
        fd = s_tab[i].fd;
        myport = s_tab[i].tport;
        if (xr_route(tip, myport, &ep, &why, NULL) == 1) {
            got = 1;
            nc = xr_candidates(tip, myport, cand);
            if (xr_route_is_relay(tip, myport))
                nw = xr_relay_wrap(wrapped, (int)sizeof wrapped, tip, myport, myport, buf, n);
        }
        unlock();
        for (c = 0; got && c < nc; c++) {
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = cand[c].ip;
            to.sin_port = htons(cand[c].port);
            if (!sim_out(HANDLE_BASE + (uint32_t)i, buf, n, &to))
                sendto(fd, (const char *)buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
        }
        if (nw > 0)
            relay_send(fd, wrapped, nw);
        release(&s_tab[i]);
    }
}

int xnet_hle_react_send(int kind, int value)
{
    uint32_t ids[XR_MAX_PEERS];
    uint8_t out[XR_REACT_LEN];
    int i, nm;
    uint64_t now;
    if (!xnet_hle_on())
        return 0;
    now = now_ms();
    lock();
    if (!xr_react_send_ok(now)) {
        unlock();
        return 0;
    }
    xr_react_make(out, kind, value);
    memcpy(s_react_out, out, sizeof out);   /* kept for the 60 ms resend */
    nm = xr_member_list(ids, XR_MAX_PEERS);
    unlock();
    /* send the copy: s_react_out may be rewritten by another thread's send */
    for (i = 0; i < nm; i++)
        reacts_from_all(ids[i], out, XR_REACT_LEN);
    lock();
    s_react_out_len = XR_REACT_LEN;
    s_react_resend_at = now + 60;
    unlock();
    return 1;
}

void xnet_hle_react_tick(void)
{
    uint32_t ids[XR_MAX_PEERS];
    uint8_t out[XR_REACT_LEN];
    int i, nm;
    uint64_t now;
    if (!xnet_hle_on())
        return;
    now = now_ms();
    lock();
    if (!s_react_out_len || now < s_react_resend_at) {
        unlock();
        return;
    }
    s_react_out_len = 0;   /* one resend, then done */
    memcpy(out, s_react_out, sizeof out);
    nm = xr_member_list(ids, XR_MAX_PEERS);
    unlock();
    for (i = 0; i < nm; i++)
        reacts_from_all(ids[i], out, XR_REACT_LEN);
}

int xnet_hle_vote_members(uint32_t *ids, int max)
{
    uint32_t all[XR_MAX_PEERS];
    int i, n, o = 0;
    if (!xnet_hle_on())
        return 0;
    lock();
    n = xr_member_list(all, XR_MAX_PEERS);
    for (i = 0; i < n && o < max; i++)
        if (!xr_is_self(all[i]))
            ids[o++] = all[i];
    unlock();
    return o;
}

int xnet_hle_vote_send(uint32_t title_ip, const uint8_t *data)
{
    uint8_t out[XR_VOTE_LEN];
    if (!xnet_hle_on())
        return 0;
    lock();
    xr_vote_make(out, data);
    unlock();
    reacts_from_all(title_ip, out, XR_VOTE_LEN);
    return 1;
}

int xnet_hle_vote_get(uint32_t *title_ip, uint8_t *data)
{
    int r;
    lock();
    r = xr_vote_get(title_ip, data);
    unlock();
    return r;
}

int xnet_hle_react_get(uint32_t *title_ip, int *kind, int *value)
{
    int r;
    lock();
    r = xr_react_get(title_ip, kind, value);
    unlock();
    return r;
}

/*
 * xnet_hle.h - the XDK's XNet (Winsock) and a signed-in XOnline, on host
 * sockets.
 *
 * An original-Xbox title links XNet and XOnline statically: its own TCP/IP
 * stack drives the NIC, and Xbox Live sign-in is a Kerberos exchange with
 * servers that are gone. Instead of emulating the NIC, a port replaces the
 * libraries' public entry points (the title's API stubs, by address) with
 * these functions: the title's sockets become host sockets, XNet reports a
 * cable, an address and an XNADDR, and XOnline reports a signed-in user and a
 * configurable server for the title's services.
 *
 * Off unless online.mode says otherwise. Settings (settings.toml, exported
 * as RECOMP_* variables; the TAKEDAWN_* names still work):
 *
 *   online.mode    RECOMP_ONLINE         off / internet / lan
 *   online.server  RECOMP_ONLINE_SERVER  where XOnlineGetServiceInfo points
 *                  (TAKEDAWN_LOBBY)      every service: name-or-IPv4[:port];
 *                                        the name is resolved here, on the
 *                                        host, since the title has no DNS of
 *                                        its own for it; empty: the
 *                                        project's community server
 *                                        (XNET_HLE_DEFAULT_LOBBY)
 *   online.name    RECOMP_ONLINE_NAME    the signed-in user's name (the port
 *                  (TAKEDAWN_GAMERTAG)   stores a random RacerNNNN once)
 *   TAKEDAWN_LOCAL_IP=a.b.c.d    pin this copy to one local address
 *   TAKEDAWN_PORT_OFFSET=n       where ports below 1024 move (default 30000)
 *   TAKEDAWN_NET_TRACE=1         log every socket call, not only the first ones
 *
 * The functions take host pointers to the title's structures (the port's
 * glue turns guest addresses into host pointers) and return what the XDK
 * function returns; a Winsock failure also leaves the WSA error in *werr for
 * the glue to put where the title's WSAGetLastError reads it.
 *
 * Written from the XDK's documented API and Xenia's (BSD) XNet notes; no
 * code from GPL projects.
 */
#ifndef XNET_HLE_H
#define XNET_HLE_H

#include <stdint.h>
#include "xnet_route.h"   /* XR_REACT_* and XR_HELLO_* for the game's hooks */

/* The lobby server when online.server is empty: the community server the
 * project runs (TCP 21810, the title's own port), by name; it is looked up
 * at the start (getaddrinfo, IPv4) like any online.server. The one place it
 * is named. A player's online.server wins. */
#ifndef XNET_HLE_DEFAULT_LOBBY
#define XNET_HLE_DEFAULT_LOBBY "vps4954.rackhostvps.com"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int  xnet_hle_on(void);
/* Like xnet_hle_on, but never starts the layer: 0 until something has called
 * xnet_hle_on (or a stub), then the mode. For per-frame polls, so that a
 * title with online play on by default opens no socket, looks nothing up and
 * starts no UPnP before the player goes online. */
int  xnet_hle_started(void);

/* The lobby server every XOnline service points at (network byte order). */
uint32_t xnet_hle_lobby_ip(void);
uint16_t xnet_hle_lobby_port(void);          /* host order, 0 = title's own */
const char *xnet_hle_gamertag(void);
uint32_t xnet_hle_local_ip(void);            /* network order */
uint16_t xnet_hle_port_offset(void);          /* TAKEDAWN_PORT_OFFSET, default 30000 */
/* BO-349: this machine's address on the network, found the way this layer
 * finds its own (TAKEDAWN_LOCAL_IP, the route out, else an up interface's, a
 * private one first); 0 = loopback only. Network order. Works before
 * anything else here and changes nothing: a LAN lobby run in-process tells
 * the other machines this address (its beacon). */
uint32_t xnet_hle_lan_addr(void);
#define XNET_HLE_HAS_LAN_ADDR 1
/* BO-351: the game's own field for a lobby packet the title sends (Burnout 3:
 * TKWHY on a host's "gset KICK"). fn gets the packet as the title wrote it and
 * returns the new length with the packet in out, or 0 to send it as it is. It
 * runs with no lock held. The title is told its own length back, as for TKID
 * and TKCODE: its send queue advances by what it queued, never by the added
 * bytes, and a short host send keeps the rest queued here. NULL = none. */
typedef int (*xnet_hle_lobby_tag_fn)(const uint8_t *pkt, int n, uint8_t *out, int cap);
void        xnet_hle_set_lobby_tag(xnet_hle_lobby_tag_fn fn);
#define XNET_HLE_HAS_LOBBY_TAG 1
/* BO-335: the game code (online.code, RECOMP_ONLINE_CODE); "" or NULL = public */
void        xnet_hle_set_code(const char *code);
const char *xnet_hle_code(void);
/* Takedawn Online: the game's own menus say what a code did (the code entry's
 * "no open game with that code", the lobby's CODE line): the overlay's code
 * messages (no game with this code, private game created, wrong code) stay
 * off. 0 = the overlay says them (the Game code row's way). */
void        xnet_hle_set_code_quiet(int quiet);
#define XNET_HLE_HAS_CODE_QUIET 1
/* BO-336: the server's counts (TKONLINE, TKWAITING; -1 = not said); 0 until
 * the server has said anything */
int         xnet_hle_lobby_status(int *online, int *waiting);
/* BO-365: races a stranger could wait for (the room record's TKRACING), -1
 * = not said (an old server) */
int         xnet_hle_lobby_racing(void);
/* while the game waits for a race (BO-365), a join the server answers
 * "full" is "not yet": no "That game is full." message. 0 = the message */
void        xnet_hle_set_join_quiet(int quiet);
#define XNET_HLE_HAS_RACEWAIT 1
enum { XNET_WAIT_NONE, XNET_WAIT_RACING, XNET_WAIT_LOBBY, XNET_WAIT_OPEN, XNET_WAIT_FULL, XNET_WAIT_GONE };
/* the game this copy is in, as its last member record said: "game=N players=N
 * ready=[name:value ...] secret=seq:prefix" (the [ONLINE] state line) */
int         xnet_hle_game_state(char *out, int n);
/* An in-process test driver's view of the lobby (Burnout 3's online
 * navigator, BO-134): what this copy sent and was sent, and its game.
 * Requests are counted when the socket takes them; answers and pushes when a
 * whole packet has come in (got_err: with a status in the header, "full",
 * "gone" ...). */
enum {
    XNET_LV_GCRE, XNET_LV_GJOI, XNET_LV_GSEA, XNET_LV_GSET, XNET_LV_GSTA, XNET_LV_MOVE, XNET_LV_GLEA,
    XNET_LV_GDEL, XNET_LV_MGM, XNET_LV_SES, XNET_LV_GAM, XNET_LV_RANK, XNET_LV_SVIW, XNET_LV_WHO, XNET_LV_N
};
typedef struct {
    unsigned sent[XNET_LV_N], got[XNET_LV_N], got_err[XNET_LV_N];
    int search_count;          /* COUNT of the last gsea answer; -1 none yet */
    int my_flag;               /* third USERPARAMS value of our last gset since gcre/gjoi; -1 none */
    int mode;                  /* third PARAMS value of our last gcre (the mode); -1 none */
    int room;                  /* IDENT of our last move (0: out of the room); -1 never */
    int game;                  /* our game's IDENT, -1 none */
    int np;                    /* its players, as the last member record said */
    char name[8][16];
    int ready[8];              /* each one's third USERPARAMS value */
    unsigned long long last_ms;   /* last packet from the lobby (monotonic) */
    /* BO-365, from the last gsea answer (absent keys: 0 / "" / XNET_WAIT_NONE) */
    int racing;                /* TKRACING: races this search could wait for */
    int race_id, race_players, race_max, race_secs;   /* the best one (TKRACEID ...) */
    char race_name[32];
    int wait_state;            /* TKWAITSTATE as XNET_WAIT_* (a waiter's poll) */
    int wait_players, wait_secs, wait_queue;
    char wait_name[32];        /* TKWAITNAME: the game made again */
    unsigned wait_seq;         /* gsea answers that carried TKWAITSTATE */
} xnet_hle_lobby_view;
int xnet_hle_lobby_view_get(xnet_hle_lobby_view *v);
/* BO-372: BROWSE GAMES. A browse search (the game adds TKBROWSE=1 to its
 * gsea) lists racing and full games too; each +gam record says its state
 * (TKSTATE=open|racing|full) and a race how long it has run (TKSECS). The
 * states of the last records are kept by IDENT: returns XNET_GS_* for the
 * game (XNET_GS_NONE: its record said nothing, or none came), *secs the race's
 * seconds when its record came and *at_ms when that was (monotonic). */
enum { XNET_GS_NONE, XNET_GS_OPEN, XNET_GS_RACING, XNET_GS_FULL };
int xnet_hle_browse_state(int ident, int *secs, unsigned long long *at_ms);
#define XNET_HLE_HAS_BROWSE 1

/* a peer's port offset, from its XUID (xnet_route.h) */
void     xnet_hle_peer_offset(uint32_t ina, uint16_t offset);

/* XNet */
uint32_t xnet_hle_link_status(void);          /* XNetGetEthernetLinkStatus */
uint32_t xnet_hle_title_xnaddr(void *xna);    /* XNetGetTitleXnAddr, 36 bytes */
void     xnet_hle_make_xnaddr(void *xna, uint32_t ina);
void     xnet_hle_random(void *p, uint32_t n);

/* Winsock. Socket handles are the layer's own small numbers. */
uint32_t xnet_hle_socket(int af, int type, int proto, int *werr);
int xnet_hle_closesocket(uint32_t s, int *werr);
/* WSACleanup / XNetCleanup: every socket still open is closed, as Winsock
 * does at the last cleanup (one in use by a call closes when it returns).
 * Returns how many were open. Defined: XNET_HLE_HAS_CLOSE_ALL. */
#define XNET_HLE_HAS_CLOSE_ALL 1
int xnet_hle_close_all(void);
/* An online session is up: a lobby connection the server has answered on
 * and not lost (signed in: the online menus, a lobby), or a game this copy is
 * in (its lobby, the race, the results). 0 before xnet_hle_on. Cheap enough
 * to poll every frame. */
int xnet_hle_session_up(void);
#define XNET_HLE_HAS_SESSION_UP 1
int xnet_hle_socket_count(void);              /* sockets open now (the 64-slot table) */
int xnet_hle_shutdown(uint32_t s, int how, int *werr);
int xnet_hle_ioctlsocket(uint32_t s, uint32_t cmd, uint32_t *argp, int *werr);
int xnet_hle_setsockopt(uint32_t s, int level, int opt, const void *val, int len, int *werr);
int xnet_hle_getsockopt(uint32_t s, int level, int opt, void *val, int32_t *len, int *werr);
int xnet_hle_getsockname(uint32_t s, void *name, int32_t *len, int *werr);
int xnet_hle_getpeername(uint32_t s, void *name, int32_t *len, int *werr);
int xnet_hle_bind(uint32_t s, const void *name, int len, int *werr);
int xnet_hle_connect(uint32_t s, const void *name, int len, int *werr);
int xnet_hle_listen(uint32_t s, int backlog, int *werr);
uint32_t xnet_hle_accept(uint32_t s, void *addr, int32_t *len, int *werr);
/* fd sets in the title's layout: u32 count, u32 handles[64]; tv: 2 x i32 */
int xnet_hle_select(void *rd, void *wr, void *ex, const int32_t *tv, int *werr);
int xnet_hle_recv(uint32_t s, void *buf, int len, int flags, int *werr);
int xnet_hle_recvfrom(uint32_t s, void *buf, int len, int flags,
                      void *from, int32_t *fromlen, int *werr);
int xnet_hle_send(uint32_t s, const void *buf, int len, int flags, int *werr);
int xnet_hle_sendto(uint32_t s, const void *buf, int len, int flags,
                    const void *to, int tolen, int *werr);

/* One log line, "[XNET] ..." (stderr). */
void xnet_hle_log(const char *fmt, ...);
/* BO-340: the few always-on online lines ("[NET] ..."): printed even when
 * diagnostic logging is off, so a bug report's log has them. A handful per
 * session; never a packet, never an address. */
void xnet_hle_note(const char *fmt, ...);
/* the lobby's session id (TKSID) and the last game's id (TKGID), "" for none
 * (an old server); 1 when the session id is known */
int  xnet_hle_ids(char *sid, int sn, char *gid, int gn);

/* A message for the player about online play ("can't reach the host",
 * "the server is full"...): shown over the game for a while by the overlay.
 * xnet_hle_message sets it; xnet_hle_message_get copies the latest and
 * returns its sequence number (0: none yet). */
void xnet_hle_message(const char *text);
/* BO-342/BO-349: macOS refused a send on the local network (Local Network
 * privacy: the player said no, or the system never asked): the one hint,
 * shown once per run. */
void xnet_hle_local_network_hint(void);
int  xnet_hle_message_get(char *out, int n);

/* BO-352: the quick-reactions side channel (xnet_route.h, xnet_route.c).
 * The game calls xnet_hle_react_send when a player picks a reaction: a
 * "TKRX" datagram goes to every member of the game, twice 60 ms apart (the
 * receiver drops the second by its sequence). It returns 1 when it sent, 0
 * when the send bucket refused or online is off. xnet_hle_react_get pulls
 * one reaction another player sent (the game's lobby/results hook calls it
 * each frame); xnet_hle_react_tick sends the due second copy. */
int  xnet_hle_react_send(int kind, int value);
void xnet_hle_react_tick(void);
int  xnet_hle_react_get(uint32_t *title_ip, int *kind, int *value);

/* The track vote's side channel (xnet_route.h, TKVT). The game's vote logic
 * owns resends: xnet_hle_vote_send sends 24 opaque bytes once to one member
 * of the game (by the title address xnet_hle_vote_members gives), 0 when
 * online is off. xnet_hle_vote_get pulls one received packet. */
#define XNET_HLE_HAS_VOTE 1
#define XNET_HLE_VOTE_DATA 24
int  xnet_hle_vote_members(uint32_t *ids, int max);
int  xnet_hle_vote_send(uint32_t title_ip, const uint8_t *data);
int  xnet_hle_vote_get(uint32_t *title_ip, uint8_t *data);

/* BO-357: the measured path to a peer (xnet_route.h, the TKPG ping), by the
 * title address its XNADDR carries: 1 with values, 0 before the first answer
 * or when that peer cannot answer (an older copy). */
int  xnet_hle_peer_rtt(uint32_t title_ip, unsigned *min_ms, unsigned *med_ms, int *sent, int *lost);

/* UPnP (xnet_upnp.c): on in Internet mode; port is the race port (messages) */
void xnet_upnp_start(unsigned port);
/* a host UDP port to forward (a title socket bound in Internet mode) */
void xnet_upnp_port(unsigned port);
/* this copy hosts a game (the lobby answered its gcre) or no longer (gjoi,
 * glea, gdel, the lobby's empty record): forward or not */
void xnet_upnp_host(int on);

#ifdef __cplusplus
}
#endif
#endif

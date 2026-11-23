/*
 * xnet_upnp.c - open the online ports on the router with UPnP (BO-134).
 *
 * The player who hosts a game must be reachable on UDP 31000 (the title's
 * port 1000 plus TAKEDAWN_PORT_OFFSET), and on the title's other UDP ports
 * (6000 ...) the same way. In Internet mode, from the moment the lobby
 * answers this copy's "gcre" with its new game (xnet_upnp_host(1)) until the
 * game is over (glea, gdel, the lobby's empty record; not when the title
 * closes the lobby connection, which it does for every race) or it joins
 * someone else's (xnet_upnp_host(0)), a thread asks the
 * router (an Internet Gateway
 * Device, found by SSDP) to forward each of them to this computer, the same
 * number outside as inside, ten minutes at a time (TAKEDAWN_UPNP_LEASE
 * seconds), renewed at half time, and removes them when it stops hosting or
 * the program ends normally. A copy that only joins asks nothing. Nothing is
 * asked of a router without UPnP; the log says what happened, and while the
 * copy hosts it looks for one again every 30 s (a router that answers late,
 * a network that came up later). Only ports the title has bound are asked
 * for. TAKEDAWN_UPNP=0
 * turns it off; TAKEDAWN_UPNP_DESC names the mappings (tests: "Takedawn
 * test ..."); TAKEDAWN_UPNP_ALWAYS=1 (tests on one router) forwards from the
 * start, joining or not.
 *
 * miniupnpc 2.2.8 (BSD 3-Clause, third_party/miniupnpc) does the protocol.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"
#include "xnet_hle.h"
#include "xnet_route.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

#define UPNP_LEASE_S 600
#define MAX_PORTS 8

static int s_lease = UPNP_LEASE_S;
static char s_desc[64] = "Takedawn online race";
static struct UPNPUrls s_urls;
static struct IGDdatas s_data;
static char s_lan[16], s_client[16];
static int s_on, s_started;
static unsigned s_hint;   /* the race port, for the messages before any bind */
static volatile int s_hosting;
/* the ports asked for (under s_plock: the title's thread adds, ours reads);
 * mapped[i]: when it was last mapped (0 = not yet), ours only */
static unsigned s_ports[MAX_PORTS];
static int s_nports;
static time_t s_mapped[MAX_PORTS];
#ifdef _WIN32
static SRWLOCK s_plock = SRWLOCK_INIT;
static void plock(void) { AcquireSRWLockExclusive(&s_plock); }
static void punlock(void) { ReleaseSRWLockExclusive(&s_plock); }
#else
static pthread_mutex_t s_plock = PTHREAD_MUTEX_INITIALIZER;
static void plock(void) { pthread_mutex_lock(&s_plock); }
static void punlock(void) { pthread_mutex_unlock(&s_plock); }
#endif

/* the ports now, copied under the lock */
static int ports_now(unsigned out[MAX_PORTS])
{
    int n, i;
    plock();
    n = s_nports;
    for (i = 0; i < n; i++)
        out[i] = s_ports[i];
    punlock();
    return n;
}

static int add_mapping(unsigned port)
{
    char p[8], lease[16];
    int r;
    snprintf(p, sizeof p, "%u", port);
    snprintf(lease, sizeof lease, "%d", s_lease);
    r = UPNP_AddPortMapping(s_urls.controlURL, s_data.first.servicetype, p, p, s_client,
                            s_desc, "UDP", NULL, lease);
    if (r != UPNPCOMMAND_SUCCESS) {
        xnet_hle_log("UPnP: the router refused UDP %s -> %s (%d %s)", p, s_client, r, strupnperror(r));
        return 0;
    }
    return 1;
}

static void remove_mappings(void)
{
    unsigned ports[MAX_PORTS];
    int i, n = ports_now(ports);
    for (i = 0; i < n; i++)
        if (s_mapped[i]) {
            char p[8];
            snprintf(p, sizeof p, "%u", ports[i]);
            UPNP_DeletePortMapping(s_urls.controlURL, s_data.first.servicetype, p, "UDP", NULL);
            xnet_hle_log("UPnP: removed the forwarding of UDP %s", p);
            s_mapped[i] = 0;
        }
}

static void sleep_s(int s)
{
#ifdef _WIN32
    Sleep((DWORD)s * 1000);
#else
    sleep((unsigned)s);
#endif
}

/* the router: 1 when one with UPnP was found (s_urls, s_data, s_client set);
 * say: log why not */
static int find_router(int say, char *wan, int wan_n)
{
    struct UPNPDev *devs;
    unsigned first[MAX_PORTS];
    int err = 0, r, i;
    if (!ports_now(first))
        first[0] = s_hint;
    devs = upnpDiscover(2000, NULL, NULL, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
    if (!devs) {
#ifdef __APPLE__
        /* BO-342: macOS's local network privacy fails the SSDP send itself
         * (the player said no, or the system never asked) */
        if (say && err == UPNPDISCOVER_SOCKET_ERROR) {
            xnet_hle_note("UPnP: macOS refused the search on the local network (Local Network access off?)");
            xnet_hle_local_network_hint();   /* the one wording, once per run (BO-349 shares it) */
            return 0;
        }
#endif
        if (say)
            xnet_hle_log("UPnP: no router answered (UPnP off or not supported); if you host, forward UDP %u yourself",
                         first[0]);
        return 0;
    }
    r = UPNP_GetValidIGD(devs, &s_urls, &s_data, s_lan, sizeof s_lan, wan, wan_n);
    freeUPNPDevlist(devs);
    if (r != 1) {
        if (say)
            xnet_hle_log("UPnP: no usable router found (%d); if you host, forward UDP %u yourself", r, first[0]);
        if (r > 0)
            FreeUPNPUrls(&s_urls);
        return 0;
    }
    /* the internal client: this copy's own address when it is on the
     * router's network (a VPN or a second interface may differ); s_lan is
     * our address on the way to the router (miniupnpc's lanaddr) */
    {
        unsigned q[4];
        uint32_t lan = 0, c;
        uint8_t b[4];
        snprintf(s_client, sizeof s_client, "%s", s_lan);
        if (sscanf(s_lan, "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]) == 4) {
            for (i = 0; i < 4; i++)
                b[i] = (uint8_t)q[i];
            memcpy(&lan, b, 4);
            c = xr_upnp_client(xnet_hle_local_ip(), lan);
            memcpy(b, &c, 4);
            snprintf(s_client, sizeof s_client, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
    }
    return 1;
}

static void upnp_run(void)
{
    int i, found = 0, said = 0, n;
    char wan[64] = "";
    unsigned ports[MAX_PORTS];
    for (;;) {
        time_t now = time(NULL);
        if (!s_hosting) {
            if (found)
                remove_mappings();
            said = 0;   /* the next hosting says again what it finds */
            sleep_s(2);
            continue;
        }
        if (!found) {
            found = find_router(!said, wan, (int)sizeof wan);
            said = 1;
            if (!found) {
                sleep_s(30);
                continue;
            }
            atexit(remove_mappings);
        }
        n = ports_now(ports);
        for (i = 0; i < n; i++) {
            if (s_mapped[i] && now - s_mapped[i] < s_lease / 2)
                continue;
            if (add_mapping(ports[i])) {
                if (!s_mapped[i])
                    xnet_hle_log("UPnP: the router forwards UDP %u to %s (internet address %s, '%s', %d s at a time)",
                                 ports[i], s_client, wan[0] ? wan : "?", s_desc, s_lease);
                s_mapped[i] = now;
            }
        }
        sleep_s(2);   /* new ports (a socket bound later) and leaving within 2 s */
    }
}

#ifdef _WIN32
static DWORD WINAPI upnp_thread(LPVOID p) { (void)p; upnp_run(); return 0; }
#else
static void *upnp_thread(void *p) { (void)p; upnp_run(); return NULL; }
#endif

void xnet_upnp_port(unsigned port)
{
    int i;
    if (!s_on || !port)
        return;
    plock();
    for (i = 0; i < s_nports && s_ports[i] != port; i++)
        ;
    if (i == s_nports && s_nports < MAX_PORTS)
        s_ports[s_nports++] = port;
    punlock();
}

static void start_thread(void)
{
    if (s_started)
        return;
    s_started = 1;
#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, upnp_thread, NULL) == 0)
            pthread_detach(t);
    }
#endif
}

static int s_always;

void xnet_upnp_host(int on)
{
    if (!s_on || s_hosting == on || (s_always && !on))
        return;
    s_hosting = on;
    xnet_hle_log("UPnP: %s", on ? "hosting a game: forwarding its ports" : "not hosting: no forwarding");
    if (on)
        start_thread();
}

void xnet_upnp_start(unsigned port)
{
    const char *e = getenv("TAKEDAWN_UPNP");
    if (e && *e == '0')
        return;
    e = getenv("TAKEDAWN_UPNP_LEASE");
    if (e && atoi(e) >= 60)
        s_lease = atoi(e);
    e = getenv("TAKEDAWN_UPNP_DESC");
    if (e && *e)
        snprintf(s_desc, sizeof s_desc, "%s", e);
    s_on = 1;
    s_hint = port;   /* asked for once the title has bound it (M6) */
    /* tests through one router's NAT loopback: the guests' ports too, since
     * hole punching through one's own router is not something to count on */
    e = getenv("TAKEDAWN_UPNP_ALWAYS");
    if (e && *e && *e != '0') {
        xnet_upnp_host(1);
        s_always = 1;
    }
}

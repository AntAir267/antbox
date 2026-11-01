/*
 * xbox_autoplay.c - a title driven through its screens by a script, from
 * inside the process; see xbox_autoplay.h.
 */
#include "xbox_autoplay.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "kernel.h"             /* xbox_FileOpenHook */
#include "xbox_crash.h"         /* g_xbox_flip_count */
#include "platform/host_time.h" /* xbox_TimeWarpOn */

#define LOGP "[AUTOPLAY] "

/* The title's file opens, as they come: the path's tail and the flip. */
#define EV_MAX  8192
#define EV_NAME 96
typedef struct {
    char     name[EV_NAME];     /* lower case */
    uint32_t flip;
} Event;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static Event    s_ev[EV_MAX];
static unsigned s_nev;          /* opens recorded (the ring keeps the last EV_MAX) */
static const xap_run *s_run;
static void (*s_prev_hook)(const char *, uint32_t);

static void nap(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

/* How often a wait looks at the flip count: 20 ms is a flip or two; a run
 * that does not wait for the clock (host_time.h) makes a flip in a
 * millisecond or two. */
static unsigned poll_ms(void)
{
    return xbox_TimeWarpOn() ? 1 : 20;
}

static time_t now_s(void)
{
    return time(NULL);
}

/* Paths and what a step looks for in them are compared in lower case and
 * with one separator, so a step's "dir\\file" finds "DIR/File" too. */
static char fold(char c)
{
    return c == '/' ? '\\' : (char)tolower((unsigned char)c);
}

static void on_open(const char *path, uint32_t status)
{
    if (s_prev_hook)
        s_prev_hook(path, status);
    if (status || !path)
        return;
    {
        size_t n = strlen(path), k;
        const char *tail = n >= EV_NAME ? path + n - (EV_NAME - 1) : path;
        Event *e;
        pthread_mutex_lock(&s_mu);
        e = &s_ev[s_nev % EV_MAX];
        for (k = 0; tail[k]; k++)
            e->name[k] = fold(tail[k]);
        e->name[k] = 0;
        e->flip = g_xbox_flip_count;
        s_nev++;
        pthread_mutex_unlock(&s_mu);
    }
}

/* The first open at or after `from` whose path contains `what` (lower
 * case): 1 and its flip. */
static int find_open(unsigned from, const char *what, uint32_t *flip)
{
    unsigned i, lo;
    int hit = 0;
    pthread_mutex_lock(&s_mu);
    lo = s_nev > EV_MAX ? s_nev - EV_MAX : 0;
    for (i = from > lo ? from : lo; i < s_nev; i++)
        if (strstr(s_ev[i % EV_MAX].name, what)) {
            *flip = s_ev[i % EV_MAX].flip;
            hit = 1;
            break;
        }
    pthread_mutex_unlock(&s_mu);
    return hit;
}

static void lower(char *out, size_t n, const char *in)
{
    size_t k;
    for (k = 0; in && in[k] && k + 1 < n; k++)
        out[k] = fold(in[k]);
    out[k] = 0;
}

/* Appends a press and waits until the pad has taken it. 1, 0 timed out,
 * -1 cancelled. */
static int press(const xap_run *r, const char *what, int timeout)
{
    unsigned before = r->pad_taken ? r->pad_taken() : 0;
    time_t t0 = now_s();
    FILE *f = fopen(r->pad_file, "ab");
    if (!f)
        return 0;
    fprintf(f, "%s\n", what);
    fclose(f);
    if (!r->pad_taken) {
        nap(150);
        return 1;
    }
    while (r->pad_taken() == before) {
        if (r->cancelled && r->cancelled(r->ctx))
            return -1;
        if (now_s() - t0 > timeout)
            return 0;
        nap(poll_ms());
    }
    return 1;
}

/* Until the flip count reaches `target` (or `file`, when given, has been
 * opened since `from`: then 2). 1 reached, 0 timed out, -1 cancelled. */
static int wait_flip(const xap_run *r, uint32_t target, const char *file, unsigned from,
                     uint32_t *anchor, int timeout)
{
    time_t t0 = now_s();
    for (;;) {
        if (file && find_open(from, file, anchor))
            return 2;
        if ((int32_t)(g_xbox_flip_count - target) >= 0)
            return 1;
        if (r->cancelled && r->cancelled(r->ctx))
            return -1;
        if (now_s() - t0 > timeout)
            return 0;
        nap(poll_ms());
    }
}

static void *driver(void *arg)
{
    const xap_run *r = (const xap_run *)arg;
    const xap_step *st;
    int timeout = r->wait_timeout_s > 0 ? r->wait_timeout_s : 300;
    unsigned sync = 0;
    uint32_t anchor = 0;
    char why[160] = "", a[EV_NAME];
    int rc = 1;
    uint64_t t_start = xbox_HostNowNs();

    for (st = r->steps; st->op != XAP_END && rc == 1; st++) {
        switch (st->op) {
        case XAP_WAIT_FILE:
            lower(a, sizeof a, st->s);
            rc = wait_flip(r, g_xbox_flip_count + (st->n > 0 ? (uint32_t)st->n : 0x40000000u),
                           a, sync, &anchor, timeout);
            if (rc == 2) {
                rc = 1;
                fprintf(stderr, LOGP "%s opened at flip %u\n", st->s, (unsigned)anchor);
            } else if (rc == 1) {
                /* The n flips passed first: go on from here. */
                anchor = g_xbox_flip_count;
                fprintf(stderr, LOGP "%s not opened in %d flips: going on at flip %u\n",
                        st->s, st->n, (unsigned)anchor);
            }
            break;
        case XAP_PRESS:
            rc = press(r, st->s, timeout);
            if (rc == 1) {
                fprintf(stderr, LOGP "press %s (flip %u)\n", st->s, (unsigned)g_xbox_flip_count);
                rc = wait_flip(r, g_xbox_flip_count + (uint32_t)st->n, NULL, 0, NULL, timeout);
            }
            break;
        case XAP_PRESS_UNTIL: {
            time_t t0 = now_s();
            lower(a, sizeof a, st->s2);
            while (rc == 1 && !find_open(sync, a, &anchor)) {
                if (now_s() - t0 > timeout) {
                    rc = 0;
                    break;
                }
                rc = press(r, st->s, timeout);
                if (rc == 1) {
                    rc = wait_flip(r, g_xbox_flip_count + (uint32_t)st->n, a, sync, &anchor,
                                   timeout);
                    rc = rc == 2 ? 1 : rc;
                }
            }
            if (rc == 1)
                fprintf(stderr, LOGP "%s opened at flip %u after pressing %s\n", st->s2,
                        (unsigned)anchor, st->s);
            break;
        }
        case XAP_WAIT_FLIPS:
            rc = wait_flip(r, g_xbox_flip_count + (uint32_t)st->n, NULL, 0, NULL, timeout);
            break;
        case XAP_WAIT_ANCHOR:
            rc = wait_flip(r, anchor + (uint32_t)st->n, NULL, 0, NULL, timeout);
            break;
        case XAP_SYNC:
            pthread_mutex_lock(&s_mu);
            sync = s_nev;
            pthread_mutex_unlock(&s_mu);
            break;
        case XAP_MARK:
            fprintf(stderr, LOGP "%s (flip %u, %.1f s)\n", st->s ? st->s : "mark",
                    (unsigned)g_xbox_flip_count, (double)(xbox_HostNowNs() - t_start) / 1e9);
            if (r->mark)
                r->mark(r->ctx, st->s, st->n);
            break;
        default:
            break;
        }
        if (rc == 0)
            snprintf(why, sizeof why, "step %d (%s%s%s) waited more than %d s",
                     (int)(st - r->steps), st->s ? st->s : "", st->s2 ? " until " : "",
                     st->s2 ? st->s2 : "", timeout);
    }
    if (rc == 0)
        fprintf(stderr, LOGP "FAIL: %s\n", why);
    if (r->done)
        r->done(r->ctx, rc, rc < 0 ? "cancelled" : why);
    s_run = NULL;
    return NULL;
}

int xbox_AutoplayStart(const xap_run *run)
{
    pthread_t th;
    if (s_run || !run || !run->steps || !run->pad_file)
        return 0;
    s_run = run;
    if (xbox_FileOpenHook != on_open) {
        s_prev_hook = xbox_FileOpenHook;
        xbox_FileOpenHook = on_open;
    }
    if (pthread_create(&th, NULL, driver, (void *)run) != 0) {
        s_run = NULL;
        return 0;
    }
    pthread_detach(th);
    return 1;
}

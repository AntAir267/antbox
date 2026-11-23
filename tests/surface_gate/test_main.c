/* surface_gate test (BO-324). The life-cycle thread does what SDL 2.32 does
 * on Android when the app leaves the screen and comes back: the pause is
 * pumped on SDL's main thread (our watcher closes the gate there), then the
 * Java thread releases the window and SDL's pointer goes NULL; later a new
 * window, then the foreground event (our watcher opens the gate). The
 * presenter thread re-makes its surface as fast as it can, as resurface()
 * does while the surface is marked lost.
 *
 * Checked: with the gate, no surface is ever made on a NULL window and no
 * window is released while a surface is being made on it (the crash at
 * address 0x98 in the #97 reports). Without the gate (the code before the
 * fix) the same schedule is run once and its count printed, to show the
 * test can see the race. */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "surface_gate.h"

struct window { volatile int alive; };

static struct surface_gate g = SURFACE_GATE_INIT;
static struct window *volatile s_window;      /* SDL's data->native_window */
static volatile int s_stop, s_use_gate;
static volatile long s_made, s_null_used, s_released_under;

static void nap_us(long us)
{
    struct timespec ts = { 0, us * 1000L };
    nanosleep(&ts, NULL);
}

static void *presenter(void *arg)
{
    (void)arg;
    while (!s_stop) {
        struct window *w;
        if (s_use_gate) {
            if (surface_gate_is_away(&g)) {     /* resurface's early out */
                nap_us(20);
                continue;
            }
            if (!surface_gate_begin(&g))
                continue;
        }
        w = s_window;
        if (s_use_gate && !w) {                 /* android_has_window() */
            surface_gate_end(&g);
            continue;
        }
        /* vkCreateAndroidSurfaceKHR: reads the window for a while. */
        if (!w)
            s_null_used++;
        else {
            nap_us(30);
            if (!w->alive)
                s_released_under++;
        }
        s_made++;
        if (s_use_gate)
            surface_gate_end(&g);
        nap_us(10);
    }
    return NULL;
}

static void run(int use_gate, int cycles)
{
    pthread_t t;
    int i;
    struct window *w = calloc(1, sizeof *w);

    w->alive = 1;
    s_window = w;
    s_stop = 0;
    s_use_gate = use_gate;
    s_made = s_null_used = s_released_under = 0;
    pthread_create(&t, NULL, presenter, NULL);
    for (i = 0; i < cycles; i++) {
        nap_us(200 + rand() % 400);             /* playing */
        surface_gate_away(&g);                  /* WILL/DIDENTERBACKGROUND watcher */
        nap_us(rand() % 50);                    /* SDL's pump sets backup_done */
        w = s_window;                           /* Java: onNativeSurfaceDestroyed */
        s_window = NULL;
        w->alive = 0;                           /* ANativeWindow_release */
        nap_us(100 + rand() % 300);             /* in the background */
        w->alive = 1;                           /* surfaceCreated: a (reused) window */
        s_window = w;
        nap_us(rand() % 50);                    /* nativeResume, pumped */
        surface_gate_back(&g);                  /* DIDENTERFOREGROUND watcher */
    }
    s_stop = 1;
    pthread_join(t, NULL);
    free(w);
}

int main(void)
{
    int fail = 0;

    srand(12345);
    run(0, 300);
    printf("without the gate: %ld surfaces made, %ld on a NULL window, %ld on a window"
           " released meanwhile\n", s_made, s_null_used, s_released_under);
    run(1, 300);
    printf("with the gate:    %ld surfaces made, %ld on a NULL window, %ld on a window"
           " released meanwhile\n", s_made, s_null_used, s_released_under);
    if (s_null_used || s_released_under) {
        printf("FAIL: a surface was made while the window was going away\n");
        fail = 1;
    }
    if (s_made == 0) {
        printf("FAIL: the presenter made no surface at all\n");
        fail = 1;
    }
    if (!fail)
        printf("PASS\n");
    return fail;
}

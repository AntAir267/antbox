/* surface_gate.h - when the presenter may make a window surface (BO-324).
 *
 * On Android the window's ANativeWindow belongs to the app's life cycle:
 * SDL's surfaceDestroyed() (Java thread) releases it and sets SDL's pointer
 * to NULL once the SDL main thread has pumped the pause (SDL 2.32
 * SDL_android.c onNativeSurfaceDestroyed waits for backup_done, which the
 * pump sets after it has sent SDL_APP_WILLENTERBACKGROUND and
 * SDL_APP_DIDENTERBACKGROUND); SDL_Vulkan_CreateSurface passes that pointer
 * to vkCreateAndroidSurfaceKHR unchecked (SDL_androidvulkan.c). A surface
 * made on the presenter's thread while that happens gets a NULL or a
 * released window: the SIGSEGV at address 0x98 inside libvulkan.so in the
 * GitHub #97 reports.
 *
 * The gate: the life-cycle watcher (on the SDL main thread, before the pump
 * lets the Java thread release the window) closes it, and closing waits for
 * a surface being made to be done; the presenter makes a surface only with
 * the gate open and holds it while it does. The foreground event (sent by
 * SDL only once the new surface is ready: SDLActivity.handleNativeState
 * resumes on mIsSurfaceReady) opens it again. Header only, so that the
 * presenter and a standalone test (tests/surface_gate) share it.
 */
#ifndef XBOXRECOMP_SURFACE_GATE_H
#define XBOXRECOMP_SURFACE_GATE_H

#include <pthread.h>

struct surface_gate {
    pthread_mutex_t mu;
    int away;                /* the app is away: no window to make a surface on */
};

#define SURFACE_GATE_INIT { PTHREAD_MUTEX_INITIALIZER, 0 }

/* The life cycle: the app leaves the screen. Returns once no surface is
 * being made, and none is made until surface_gate_back. */
static inline void surface_gate_away(struct surface_gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->away = 1;
    pthread_mutex_unlock(&g->mu);
}

/* The life cycle: the app is back in front with its new window. */
static inline void surface_gate_back(struct surface_gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->away = 0;
    pthread_mutex_unlock(&g->mu);
}

/* The presenter, before making a surface: 1 = go ahead (the gate is held
 * until surface_gate_end), 0 = the app is away, make nothing. */
static inline int surface_gate_begin(struct surface_gate *g)
{
    pthread_mutex_lock(&g->mu);
    if (g->away) {
        pthread_mutex_unlock(&g->mu);
        return 0;
    }
    return 1;
}

static inline void surface_gate_end(struct surface_gate *g)
{
    pthread_mutex_unlock(&g->mu);
}

static inline int surface_gate_is_away(struct surface_gate *g)
{
    int a;
    pthread_mutex_lock(&g->mu);
    a = g->away;
    pthread_mutex_unlock(&g->mu);
    return a;
}

#endif

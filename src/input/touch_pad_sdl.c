/*
 * The SDL window's fingers into the on-screen controls (touch_pad.h), and
 * the controller and key events that hide them in auto mode. Called from
 * xbox_InputSdlEvent (keyboard_sdl.c) for every event of the main loop.
 *
 * While the overlay is open the fingers are the overlay's: SDL makes mouse
 * events of them, which it reads (xbox_InputTouchSetBlocked, touch_pad.c).
 *
 * The cutout's insets (RECOMP_TOUCH_INSETS, touch_pad.h) are read again at
 * each finger down and when the window changes.
 */
#include "touch_pad.h"

#include <SDL.h>

void xbox_InputTouchSdlEvent(const SDL_Event *ev);


/* The window's size in the units the overlay draws in; 0 when unknown. */
static int screen_of(Uint32 window_id, int *ww, int *wh)
{
    SDL_Window *w = window_id ? SDL_GetWindowFromID(window_id) : NULL;
    *ww = *wh = 0;
    if (!w)
        w = SDL_GetMouseFocus();
    if (!w)
        w = SDL_GetKeyboardFocus();
    if (!w)
        return 0;
    SDL_GetWindowSize(w, ww, wh);
    if (*ww <= 0 || *wh <= 0)
        return 0;
    xbox_InputTouchSetScreen((float)*ww, (float)*wh);
    return 1;
}

void xbox_InputTouchSdlEvent(const SDL_Event *ev)
{
    static int inited;

    if (xbox_InputTouchMode() == XBOX_TOUCH_OFF)
        return;
    if (!inited) {
        inited = 1;
        xbox_InputTouchReadInsets();
    }
    switch (ev->type) {
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
        int what = ev->type == SDL_FINGERDOWN ? XBOX_TOUCH_DOWN
                 : ev->type == SDL_FINGERUP ? XBOX_TOUCH_UP : XBOX_TOUCH_MOVE;
        if (what == XBOX_TOUCH_DOWN)
            xbox_InputTouchReadInsets();
        {
            int ww, wh;
            if (!screen_of(ev->tfinger.windowID, &ww, &wh))
                return;
            xbox_InputTouchFinger(what, (int64_t)ev->tfinger.fingerId,
                                  ev->tfinger.x * (float)ww, ev->tfinger.y * (float)wh);
        }
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
        xbox_InputTouchControllerUsed();
        break;
    case SDL_CONTROLLERAXISMOTION:
        /* A third of the travel: not a resting stick's noise. */
        if (ev->caxis.value > 11000 || ev->caxis.value < -11000)
            xbox_InputTouchControllerUsed();
        break;
    case SDL_KEYDOWN: {
        SDL_Keycode k = ev->key.keysym.sym;
        /* The system Back and the volume keys are the device's, not a
         * controller's. */
        if (k == SDLK_AC_BACK || k == SDLK_VOLUMEUP || k == SDLK_VOLUMEDOWN
                || k == SDLK_AUDIOMUTE || ev->key.repeat)
            break;
        xbox_InputTouchControllerUsed();
        break;
    }
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            xbox_InputTouchReleaseAll();
        else if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                 || ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            int ww, wh;
            xbox_InputTouchReadInsets();
            screen_of(ev->window.windowID, &ww, &wh);
        }
        break;
    default:
        break;
    }
}

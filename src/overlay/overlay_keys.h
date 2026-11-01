/*
 * overlay_keys.h - which keys change the settings overlay's page.
 *
 * Pure (SDL's key names only), so a test can check the rule without the
 * overlay. A Mac keyboard has no Page Up or Page Down, so the page keys are
 * also Q and E (the pad's bumpers, as on the Xbox pad's LB and RB, and the
 * same letters the game keys give White and Black) and Tab (Shift+Tab goes
 * back). Page Up, Page Down and the square brackets stay.
 *
 * Nothing else in the overlay reads these keys: it has no text field, and
 * the music folder browser takes arrows, Enter and Esc. On the Mixer page
 * the same keys mute and solo the open strip, as LB and RB do.
 *
 * Q with Cmd, Ctrl or Alt held is a shortcut of the system (Cmd+Q quits a
 * Mac app), not a page key.
 */
#ifndef XBOX_OVERLAY_KEYS_H
#define XBOX_OVERLAY_KEYS_H

#include <SDL.h>

/* -1 previous page, +1 next page, 0 not a page key. mod: SDL_Keymod bits. */
static inline int xbox_overlay_page_key(SDL_Keycode k, Uint16 mod)
{
    const Uint16 sys = KMOD_GUI | KMOD_CTRL | KMOD_ALT;

    switch (k) {
    case SDLK_PAGEUP:
    case SDLK_LEFTBRACKET:
        return -1;
    case SDLK_PAGEDOWN:
    case SDLK_RIGHTBRACKET:
        return +1;
    case SDLK_q:
        return (mod & sys) ? 0 : -1;
    case SDLK_e:
        return (mod & sys) ? 0 : +1;
    case SDLK_TAB:
        if (mod & sys)
            return 0;
        return (mod & KMOD_SHIFT) ? -1 : +1;
    default:
        return 0;
    }
}

#endif

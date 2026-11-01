/*
 * overlay_sounds.h - the title's own menu sounds for the overlay (BO-190),
 * read at run time from the player's copy of the title's data
 * (xbox_OverlaySetMenuSounds in xbox_settings.h), never from this
 * repository.
 *
 * A thread reads each named file once, checks its size and SHA-256, cuts the
 * waves out of it and makes them 48 kHz (linear interpolation; they are
 * short clicks); nothing is written anywhere. A cue is then one call to the
 * APU's one-shot (mcpx_apu_ui_cue), which plays it after the mix, also while
 * the title is paused. Until the thread is done, and for good when a file is
 * missing or another version, that cue is silent.
 */
#ifndef OVERLAY_SOUNDS_H
#define OVERLAY_SOUNDS_H

/* True when the port named menu sounds (the setting's row shows then). */
bool menusnd_named();

/* Plays cue `id` (XBOX_OVERLAY_SND_*), replacing one playing, when
 * audio.menu_sounds is on and the cue has a sound. */
void menusnd_play(int id);

#endif

/*
 * apu_level.h -- the custom-music level (BO-132, BO-246).
 *
 * A player's own songs are anything from a modern master (-6 LUFS, true
 * peaks past 0 dBFS) to a dynamic one from the nineties (-18 LUFS); the
 * title sends a custom-soundtrack stream about 8 dB hotter than its EA GAMES
 * TRAX streams, and in stereo the fold sums the fronts, the centre and the
 * surrounds on top. A chain on the voices of the custom soundtrack, and only
 * those (not EA TRAX, not effects), brings each song to the level the
 * title's own music has in the game, the way a broadcast loudness processor
 * does:
 *
 *   meter     K-weighted (ITU-R BS.1770) power of the voice's two channels
 *             (a mono voice counts twice, as it plays on both sides);
 *             short-term loudness over the last 3 s (the first 0.3 s of a
 *             song on its own), read every 100 ms.
 *   gate      a reading under -50 LUFS, or more than 10 LU under the
 *             song's running level, holds the gain (an intro, a breakdown,
 *             a fade, silence do not wind it up). In the first
 *             APU_LEVEL_OWN_S of a song's own accepted readings, a level
 *             10 to 20 LU under and over APU_LEVEL_SONG_FLOOR_DB (-24
 *             LUFS: a quiet intro is under any song's level) for
 *             APU_LEVEL_GATE_RESET_S in a row is a new level, not a pause
 *             (a quiet song after a loud one): the count starts again from
 *             it.
 *   level     the running level: the song's integrated loudness so far
 *             (the accepted readings' power mean, gated 10 LU under itself
 *             as BS.1770 gates), weighed against the level the last song
 *             ended on as if that were APU_LEVEL_PRIOR_S of this one, a
 *             weight that fades in APU_LEVEL_PRIOR_FADE_S of the song. It
 *             settles as the song goes on: a breakdown or an outro four
 *             minutes in hardly moves it. A reading 4 LU over it for 2 s
 *             in a row (the body after a long quiet intro) starts the
 *             count again from there.
 *   gain      target + offset - running level, limited to -20 .. +15 dB,
 *             followed with a symmetric APU_LEVEL_TAU_GAIN_S, interpolated
 *             across each 0.67 ms block (no steps).
 *   ceiling   the short-term output is kept within APU_LEVEL_CEIL_DB of the
 *             target: over it, the gain comes down with a 0.2 s time
 *             constant, and the slow gain never climbs past it. A song that
 *             comes in louder than the last one is down within a second or
 *             two (BO-246: the slow gain alone left 34 of 60 songs over
 *             target + 3 dB for seconds at their start).
 *   comp      a gentle compressor after the gain: 2:1, 6 dB soft knee,
 *             threshold APU_LEVEL_COMP_ABOVE_DB over the target, detector
 *             on the same K-weighted power, 10 ms attack / 150 ms release
 *             (it rides a waveform's peaks: up to 3 dB over the mean power).
 *             A loud master only gets turned down, not reshaped.
 *   next song a new stream starts at the gain the last one had, with its
 *             level as the prior; the very first starts at -6 dB and takes
 *             the gain of its first second.
 *
 * Cost: two biquads and a one-pole a sample and channel, a few logs a
 * 0.67 ms block, a 600-bin pass every 100 ms (tools/level_sim prints it).
 *
 * The safety limiter (apu_limit.h) on the final output stays the last
 * step. RECOMP_AUDIO_CUSTOM_AGC=old runs the first leveller (BO-132: an
 * unweighted 3 s envelope, -15 .. +6 dB, 0.3 s down / 4 s up) instead, for
 * comparison.
 *
 * Off (the stock behaviour) is not a gain of 0 dB, it is no code at all:
 * a voice is never touched, so the mix is bit-identical to a build without
 * this file. The setting is a level: quieter / match / louder are offsets
 * of -4 / 0 / +4 dB on the target.
 */
#ifndef APU_LEVEL_H
#define APU_LEVEL_H

#include "apu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The loudness a song is brought to, LUFS of the stream as the voice plays
 * it (before the title's sends). The title adds about 8 dB to a custom
 * stream and the sends lose some of it again: a song at this level is
 * heard at about -20 LUFS in the game, the average of EA GAMES TRAX
 * (music-only previews of three songs: -19.7, -23.1, -18.6 LUFS). Set from
 * headless runs with real songs (BO-246). One constant. (The #ifndef
 * guards are for tools/level_sim, which builds variants.) */
#ifndef APU_LEVEL_TARGET_DB
#define APU_LEVEL_TARGET_DB     (-17.0f)
#endif
#define APU_LEVEL_MIN_DB        (-20.0f)
#define APU_LEVEL_MAX_DB        (15.0f)
#define APU_LEVEL_GATE_ABS_DB   (-50.0f)
#ifndef APU_LEVEL_GATE_REL_DB
#define APU_LEVEL_GATE_REL_DB   (-10.0f)
#endif
#define APU_LEVEL_GATE_NEW_DB   (-20.0f)
#ifndef APU_LEVEL_GATE_RESET_S
#define APU_LEVEL_GATE_RESET_S  (4.0f)
#endif
#ifndef APU_LEVEL_OWN_S
#define APU_LEVEL_OWN_S         (20u)
#endif
#ifndef APU_LEVEL_STEP_UP_DB
#define APU_LEVEL_STEP_UP_DB    (4.0f)
#endif
#define APU_LEVEL_STEP_UP_S     (2.0f)
#ifndef APU_LEVEL_RESTART_PRIOR_S
#define APU_LEVEL_RESTART_PRIOR_S (15.0f)
#endif
#ifndef APU_LEVEL_SONG_FLOOR_DB
#define APU_LEVEL_SONG_FLOOR_DB (-24.0f)
#endif
#ifndef APU_LEVEL_PRIOR_FADE_S
#define APU_LEVEL_PRIOR_FADE_S  (4.0f)
#endif
#ifndef APU_LEVEL_PRIOR_S
#define APU_LEVEL_PRIOR_S       (5.0f)
#endif
#ifndef APU_LEVEL_TAU_GAIN_S
#define APU_LEVEL_TAU_GAIN_S    (3.0f)
#endif
#define APU_LEVEL_FIRST_DB      (-6.0f)
#ifndef APU_LEVEL_CEIL_DB
#define APU_LEVEL_CEIL_DB       (3.0f)
#endif
#ifndef APU_LEVEL_TAU_CEIL_S
#define APU_LEVEL_TAU_CEIL_S    (0.2f)
#endif
#ifndef APU_LEVEL_COMP_ABOVE_DB
#define APU_LEVEL_COMP_ABOVE_DB (8.0f)
#endif
#ifndef APU_LEVEL_COMP_RATIO
#define APU_LEVEL_COMP_RATIO    (2.0f)
#endif
#ifndef APU_LEVEL_COMP_KNEE_DB
#define APU_LEVEL_COMP_KNEE_DB  (6.0f)
#endif
#ifndef APU_LEVEL_COMP_ATTACK_MS
#define APU_LEVEL_COMP_ATTACK_MS  (10.0f)
#endif
#ifndef APU_LEVEL_COMP_RELEASE_MS
#define APU_LEVEL_COMP_RELEASE_MS (150.0f)
#endif

/* The first leveller (RECOMP_AUDIO_CUSTOM_AGC=old): its target in its own
 * units (an unweighted reading, 0.2 to 0.8 dB above LUFS) and its limits. */
#define APU_LEVEL_OLD_TARGET_DB (-16.5f)
#define APU_LEVEL_OLD_MIN_DB    (-15.0f)
#define APU_LEVEL_OLD_MAX_DB    (6.0f)
#define APU_LEVEL_FREEZE_DB     (-40.0f)
#define APU_LEVEL_HOLD_DB       (-60.0f)

enum { APU_LEVEL_OFF = 0, APU_LEVEL_QUIETER = 1, APU_LEVEL_MATCH = 2, APU_LEVEL_LOUDER = 3,
       APU_LEVEL_MODES };
/* Offset of a mode on the target, dB. */
float apu_level_mode_offset_db(int mode);

/* The mode: RECOMP_AUDIO_CUSTOM_LEVEL (off, quieter, match, louder) on first
 * use, match when unset; any thread, followed live. */
void apu_level_set_mode(int mode);
int  apu_level_mode(void);

/* The title's port says which voices are the custom soundtrack. Called on
 * the frame thread when a voice starts. */
typedef int (*apu_level_class_fn)(const apu_bus_voice *v);
void apu_level_set_classifier(apu_level_class_fn fn);
/* 1 when voices have to be tagged at start and tested per frame: a
 * classifier is set and the mode is not off. */
int  apu_level_active(void);

/* One VP frame of one voice (frame thread): when the voice is a custom
 * soundtrack stream, its samples are scaled in place. */
void apu_level_voice(unsigned voice, unsigned serial,
                     int (*fill)(void *ctx, apu_bus_voice *v), void *ctx,
                     float (*samples)[2], unsigned channels);

/* The chain alone, for tests and tools: state of one stream. */
#define APU_LEVEL_RING 30                   /* 100 ms blocks in the 3 s window */
#define APU_LEVEL_BINS 600                  /* -50 .. +10 LUFS */
typedef struct {
    float  kz[2][4];                        /* K-weighting, two biquads a channel */
    double sub_acc;                         /* the 100 ms block being summed */
    unsigned sub_n;
    float  ring[APU_LEVEL_RING];
    unsigned ring_n, ring_pos;
    float  st_db;                           /* short-term loudness, last reading (-200: none) */
    float  run_db;                          /* running level (< -100: none yet) */
    float  prior_db;                        /* the level the last song ended on (< -100: none) */
    float  prior_w;                         /* what it is worth, seconds of this song */
    unsigned bins[APU_LEVEL_BINS];          /* this song's accepted readings, 0.1 dB from -50 LUFS */
    unsigned n_read;                        /* readings in bins */
    unsigned own_read;                      /* readings accepted since the song started */
    float  up_s;                            /* seconds over the running level by 4 LU in a row */
    float  gated_s;                         /* seconds under the relative gate in a row */
    int    hold;                            /* the last reading was gated */
    float  comp_pow, comp_db;               /* compressor detector (power), its gain (<= 0) */
    float  gain_db;                         /* the level's gain */
    int    primed;                          /* gain_db is a settled value (or was jumped to) */
    float  env_db;                          /* the reading in use, last frame */
    unsigned frames;
    /* what the chain did, since init (tools, the log) */
    float  comp_max_db;                     /* deepest compressor gain reduction, dB >= 0 */
    double comp_sum_db;
    /* the first leveller's own */
    double slow_num, slow_wt, fast_num, fast_wt;
} apu_level_state;
/* gain_db < -100: nothing known yet (the first song: -6 dB, then the gain
 * of its first reading). run_db < -100: no running level yet. */
void apu_level_state_init(apu_level_state *s, float gain_db);
/* One frame of 32 samples in place; target_db includes the mode's offset.
 * Returns the linear gain at the end of the frame. */
float apu_level_frame(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db);
/* The first leveller (BO-132), on the same state; its own target units. */
float apu_level_frame_old(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db);
/* 1 when RECOMP_AUDIO_CUSTOM_AGC=old (read once); tests may set it. */
int  apu_level_use_old(void);
void apu_level_set_old(int on);

#ifdef __cplusplus
}
#endif

#endif /* APU_LEVEL_H */

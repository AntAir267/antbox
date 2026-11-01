/*
 * apu_bus.h -- the host mixer: buses over the title's voices (BO-114).
 *
 * The VP mixes every voice into the 32 mixbins and the DSP stage turns the
 * bins into speaker feeds. With the mixer on, each voice belongs to a bus
 * (the title's port says which: apu_bus_set_classifier) and the bus sets how
 * the voice is summed:
 *
 *   voice -> bus gain / mute / solo / ducking
 *         -> route "title": the title's own bins and volumes, with a trim
 *            per speaker group (front, centre, LFE, rear)
 *            route "sends": six sends FL FR C LFE SL SR at the voice's own
 *            level instead of the title's bins
 *         -> optional LFE send through a low-pass (the crossover)
 *   speaker feeds -> LFE low-pass, mains high-pass, trim and delay per
 *            speaker, limiter or soft clip -> the device
 *
 * Off (the default) none of this runs: one branch a voice and one a frame.
 * On with the neutral configuration (every gain 0 dB, route "title", no
 * send, no output processing) the sums are the same operations in the same
 * order as with it off, and the output is bit-identical; tests/apu_bus
 * holds that.
 *
 * Levels are dB, times ms, frequencies Hz. "off" for a level is silence.
 */
#ifndef APU_BUS_H
#define APU_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_BUS_MAX      16
#define APU_BUS_SPEAKERS 6              /* FL FR C LFE SL SR (apu_speakers.h) */
#define APU_BUS_OFF_DB   (-200.0f)      /* at or below: silence */
#define APU_BUS_FRAME    32             /* samples a VP frame */
#define APU_BUS_BINS     32

enum { APU_BUS_ROUTE_TITLE = 0, APU_BUS_ROUTE_SENDS = 1, APU_BUS_ROUTE_PAN = 2 };
enum { APU_BUS_TRIM_FRONT = 0, APU_BUS_TRIM_CENTRE, APU_BUS_TRIM_LFE, APU_BUS_TRIM_REAR,
       APU_BUS_TRIMS };
enum { APU_BUS_LIM_OFF = 0, APU_BUS_LIM_SOFT = 1, APU_BUS_LIM_PEAK = 2 };

typedef struct {
    float gain_db;                      /* the whole bus */
    int   mute, solo;
    int   route;                        /* APU_BUS_ROUTE_* */
    float trim_db[APU_BUS_TRIMS];       /* route title: front, centre, LFE, rear */
    float send_db[APU_BUS_SPEAKERS];    /* route sends: FL FR C LFE SL SR */
    /* Route pan: a place in the 5.1 plane (apu_bus_pan_gains). */
    float pan_x, pan_y;                 /* -1 left .. +1 right, -1 rear .. +1 front */
    float pan_width;                    /* 0 a point .. 1 the two channels a full width apart */
    float pan_divergence;               /* 0 the centre speaker .. 1 a phantom centre */
    /* Route title: shaping of the title's own panning. Neutral 1 and 0. */
    float field_width;                  /* 0 everything on the centre line .. 1 as panned .. 2 wider */
    float field_divergence;             /* > 0: the centre send to FL/FR; < 0: FL/FR's common part to the centre */
    float lfe_send_db;                  /* extra send to the LFE, off by default */
    int   lfe_crossover_hz;             /* low-pass on that send: 0 none, 40..250 */
} apu_bus_params;

typedef struct {
    int   enabled;                      /* the duck slot */
    int   source, target;               /* bus indices */
    float threshold_db;                 /* source level (dBFS RMS a frame) that ducks */
    float depth_db;                     /* reduction of the target, > 0 */
    float attack_ms, release_ms;
} apu_bus_duck;

typedef struct {
    apu_bus_params bus[APU_BUS_MAX];
    float speaker_trim_db[APU_BUS_SPEAKERS];
    float speaker_delay_ms[APU_BUS_SPEAKERS];   /* 0..50 */
    int   lfe_lowpass_hz;               /* on the LFE feed: 0 off, else 40..250 (120: AC-3's) */
    int   mains_highpass_hz;            /* on the five mains: 0 off */
    int   limiter;                      /* APU_BUS_LIM_* */
    float ceiling_db;                   /* limiter / soft clip ceiling, <= 0 */
    float limiter_release_ms;
    apu_bus_duck duck;
} apu_bus_config;

/* The neutral configuration ("Factory"): the mix as without the mixer. */
void apu_bus_config_neutral(apu_bus_config *c);
/* The buses the (disabled) duck slot of the neutral configuration names:
 * what the overlay offers first. */
void apu_bus_set_duck_default(int source, int target);
/* 1 when c changes nothing (what the bit-identity holds for). */
int  apu_bus_config_is_neutral(const apu_bus_config *c);

/* The pan law of route "pan" (pure; the overlay draws with it).
 *
 * The plane is a square room: x -1 left .. +1 right, y -1 rear .. +1 front,
 * the listener in the middle, the speakers on its edge: C (0, 1),
 * FL (-1, 1), FR (1, 1), SL (-1, -1), SR (1, -1). A point on the edge is
 * panned between the two speakers next to it with the constant-power
 * sine/cosine law, over the angle between them as ITU-R BS.775 places them
 * (C 0, FL/FR +-30, SL/SR +-110 degrees; the room's corners are mapped
 * onto those angles, linearly in between). Inside the room, at distance r
 * from the middle (r = max(|x|, |y|)), the power of each speaker is
 * (1 - r) / 5 + r * edge power: in the middle all five play equally.
 * divergence moves that share of the centre speaker's power to FL and FR,
 * half each. The squares of the five gains always sum to 1.
 * out: FL FR C SL SR. */
void apu_bus_pan_gains(float x, float y, float divergence, float out[5]);
/* The stereo output's: x only, constant power, squares sum to 1. out: L R. */
void apu_bus_pan_gains_stereo(float x, float out[2]);
/* The title's FL/FR (or SL/SR) pair of one voice with field_width w:
 * amplitudes in and out, power kept. */
void apu_bus_field_width(float w, float *left, float *right);
/* field_divergence d on one voice's FL, FR and C amplitudes, power kept. */
void apu_bus_field_divergence(float d, float *fl, float *fr, float *c);

/* ── the buses (set once by the title's port, before the mixer is used) ── */
void apu_bus_set_names(const char *const *names, int count);
int  apu_bus_count(void);
const char *apu_bus_name(int bus);      /* display name */
/* The key of a bus in the preset file: its name in lower case, letters and
 * digits only, others '_' ("Tyres/road/wind" -> "tyres_road_wind"). */
void apu_bus_key(int bus, char *out, size_t n);
int  apu_bus_find(const char *key_or_name);

/* What the classifier is told about a voice. */
typedef struct {
    int      stream;                    /* a DirectSound stream voice */
    int      stereo;
    uint32_t fmt;                       /* NV_PAVS_VOICE_CFG_FMT */
    uint8_t  bins[8];                   /* the mixbin of each of its eight sends */
    uint32_t bytes;                     /* buffer length (end offset, samples) */
    const uint8_t *head;                /* the buffer's first bytes (16), NULL for a stream */
    const uint8_t *at_400, *at_440;     /* 16 bytes at +0x400 / +0x440, NULL when
                                         * that range crosses a page or the end */
    const uint32_t *stack;              /* guest return addresses at turn-on */
    int      nstack;
    int      gp_return;                 /* a voice playing the GP model's export
                                         * (apu_gp.h): 1 + the mixbin it plays
                                         * back; 0 for any other voice */
} apu_bus_voice;
/* A classifier's answer for a voice that is only the input of an in-place
 * GP effect whose output a return voice plays: it is mixed as with the
 * mixer off (the title's bins and gains; no bus gain, mute, solo or meter),
 * and the bus of the return voice is where the sound is heard. */
#define APU_BUS_BYPASS (-2)
/* Returns the bus of a voice, -1 for the last bus ("Other"), or
 * APU_BUS_BYPASS. Called on
 * the APU frame thread when a voice starts or its buffer changes. */
typedef int (*apu_bus_classify_fn)(const apu_bus_voice *v);
void apu_bus_set_classifier(apu_bus_classify_fn fn);
/* The classifier's table changed (e.g. the wave table finished loading):
 * every voice is asked again. */
void apu_bus_reclassify(void);

/* ── on/off and configuration (any thread) ── */
void apu_bus_enable(int on);
int  apu_bus_enabled(void);
void apu_bus_set_config(const apu_bus_config *c);
void apu_bus_get_config(apu_bus_config *c);

/* ── presets: a TOML file ── */
/* Parses text into c (start it neutral or from a preset). Unknown keys and
 * sections are reported in err (first one) but do not fail; returns 0, or
 * -1 on a syntax error (c untouched). */
int  apu_bus_config_parse(const char *text, apu_bus_config *c, char *err, size_t nerr);
/* Writes c as TOML into out; returns the length (truncated to n - 1). */
size_t apu_bus_config_write(const apu_bus_config *c, char *out, size_t n);
/* The file the mixer follows. With the mixer on it is read at once (a
 * neutral one is written first when there is none, as a file to edit) and
 * again whenever it changes (checked once a second on the frame thread);
 * with the mixer off it is only named, and read when the mixer is turned
 * on. */
void apu_bus_set_file(const char *path);
const char *apu_bus_file(void);
int  apu_bus_load_file(const char *path, apu_bus_config *c, char *err, size_t nerr);
int  apu_bus_save_file(const char *path, const apu_bus_config *c);
/* Counts the times the followed file was read (it changed on disk). */
unsigned apu_bus_file_gen(void);
/* Last load's message ("" when fine), for the overlay. */
const char *apu_bus_file_status(void);

/* Built-in presets, registered by the title's port: a name, a sentence for
 * the overlay, and TOML applied over the neutral configuration. The strings
 * must stay valid. Applying one also writes it to the followed file. */
void apu_bus_add_preset(const char *name, const char *about, const char *toml);
int  apu_bus_preset_count(void);
const char *apu_bus_preset_name(int i);
const char *apu_bus_preset_about(int i);
int  apu_bus_preset_get(int i, apu_bus_config *c);
void apu_bus_preset_apply(int i);
/* A change made in the overlay: the configuration, and the followed file. */
void apu_bus_commit(const apu_bus_config *c);

/* ── meters (read by the overlay; written by the frame thread) ── */
typedef struct {
    float bus_rms_db[APU_BUS_MAX];      /* level of each bus into the mix, last ~100 ms */
    float bus_voices[APU_BUS_MAX];      /* sounding voices */
    float duck_db;                      /* reduction on the duck target now, >= 0 */
    float spk_rms_db[APU_BUS_SPEAKERS]; /* speaker feeds after the output stage */
    float spk_peak_db[APU_BUS_SPEAKERS];/* peak hold, ~1.5 s */
    uint32_t spk_over[APU_BUS_SPEAKERS];/* samples past full scale before the clamp, since reset */
    float limiter_db;                   /* gain reduction now, >= 0 */
    int   channels;                     /* 6 or 2 (then FL = L, FR = R) */
} apu_bus_meters;
void apu_bus_get_meters(apu_bus_meters *m);
void apu_bus_reset_overs(void);

/* ── audition: a stretch of the game's own sound, looped through the mixer ──
 *
 * While the mixer is on and recording is asked for (apu_bus_audition_keep),
 * the last seconds of what it mixed are kept: per
 * frame, each sounding voice's samples (16 bit), its eight bins and gains
 * and its bus. Looping that clip puts those voices through the mix again,
 * frame for frame, in place of the live ones (which stay silent meanwhile):
 * every bus control, route, send, the ducking, the output stage and the
 * meters act on it exactly as on live sound, on the same material each
 * time round. With the neutral configuration the loop's sum is the live
 * one's to the 16-bit quantisation of the samples (about -96 dB).
 *
 * The clip is the player's own recording; it is kept in memory, and in a
 * file only when asked. */
struct apu_bus_rt;
enum { APU_BUS_AUD_OFF = 0, APU_BUS_AUD_LAST = 1, APU_BUS_AUD_FILE = 2 };
/* How much is kept: 0 (the default: nothing is recorded and no memory is
 * held) to 60 seconds. The store takes 3 MB a second kept; a stretch too
 * dense for that is kept shorter. Takes effect at the next frame. */
void apu_bus_audition_keep(int seconds);
int  apu_bus_audition_kept_seconds(void);
/* Loop what was kept until now (the capture stops while it loops). */
int  apu_bus_audition_loop_last(void);
/* Loop a saved clip; 0, or -1 when it cannot be read. */
int  apu_bus_audition_loop_file(const char *path);
/* Back to live sound; the capture starts afresh. */
void apu_bus_audition_stop(void);
/* The clip that loops, to a file; 0, or -1. Only while a clip loops. */
int  apu_bus_audition_save(const char *path);
int  apu_bus_audition_state(void);              /* APU_BUS_AUD_* */
typedef struct {
    float position_s, length_s;                 /* of the loop (the cross-fade taken off) */
    float kept_s;                               /* captured so far, when not looping */
    size_t bytes;                               /* held in memory */
    float crossfade_s;
} apu_bus_audition_info;
void apu_bus_audition_get(apu_bus_audition_info *info);
/* Frame thread (apu_vp.c): 1 while a clip loops: the live voices are not
 * mixed; then the clip's frame into the mixbins. */
int  apu_bus_audition_playing(void);
void apu_bus_audition_mix(struct apu_bus_rt *rt, float (*mixbins)[APU_BUS_FRAME]);

/* ── the mix (apu_vp.c / apu_dsp.c; frame thread) ── */
struct apu_bus_rt;
/* Start of a VP frame: the runtime state to mix with, NULL when off. */
struct apu_bus_rt *apu_bus_frame_begin(int channels, const int8_t spk_map[APU_BUS_BINS]);
/* One voice into the mixbins. g[b] is the title's gain of send b (envelope
 * times volume over headroom), bin[b] its mixbin. */
void apu_bus_mix_voice(struct apu_bus_rt *rt, int bus,
                       float (*mixbins)[APU_BUS_FRAME],
                       const float (*samples)[2], unsigned channels,
                       const int bin[8], const float g[8]);
/* End of a VP frame: the buses' LFE sends, the duck detector, the meters. */
void apu_bus_frame_end(struct apu_bus_rt *rt, float (*mixbins)[APU_BUS_FRAME]);
/* The output stage on n samples of nch planar feeds (feeds[c][i]) before
 * the conversion to s16: returns 0 when it has nothing to do (the caller
 * then converts as it always did). */
int  apu_bus_output_active(void);
void apu_bus_output(float (*feeds)[APU_BUS_FRAME], int nch, int n);
/* A voice's bus, or APU_BUS_BYPASS (frame thread): asked again when serial
 * or buffer changed. */
int  apu_bus_voice_bus(unsigned voice, unsigned serial, uint32_t buffer_pa,
                       int (*fill)(void *ctx, apu_bus_voice *v), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* APU_BUS_H */

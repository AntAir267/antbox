/*
 * MCPX APU Audio Emulation - Public API
 *
 * Extracted from xemu (LGPL v2+), adapted for standalone use.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCPXAPUState MCPXAPUState;

/* Initialize the APU emulation.
 * ram_ptr: pointer to the base of Xbox physical RAM (64MB).
 * Returns the APU state, or NULL on failure. */
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);

/* Shut down and free the APU state. */
void mcpx_apu_shutdown(MCPXAPUState *d);

/* MMIO read from APU register space (addr is offset from 0xFE800000). */
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);

/* MMIO write to APU register space (addr is offset from 0xFE800000). */
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* RECOMP_APU_FAULT_BENCH: time n trapped reads of PIO_FREE through the page
 * fault + VEH + decoder path; prints one line, returns us per access (0 if
 * not run). Windows x86-64 only (apu_mmio_hook.c). */
double apu_hook_fault_bench(unsigned n);

/* RECOMP_APU_TRACE: start the periodic access-rate reporter (no-op when the
 * variable is unset, or off Windows). */
void apu_hook_trace_start(void);

/* The speaker layout the title mixes for, 2 or 6 (RECOMP_AUDIO_SPEAKERS,
 * decided when the APU comes up), and the dashboard audio word (XC_AUDIO)
 * that tells the title so: 0 for stereo, SURROUND | ENABLE_AC3 for 5.1. */
int mcpx_apu_title_channels(void);
uint32_t mcpx_apu_xc_audio(void);

/* Which speaker each of the 32 mixbins feeds in 5.1, 0..5 = FL FR FC LFE
 * SL SR, -1 = left out (apu_speakers.h). Without it: DirectSound's layout,
 * the FX sends folded to the front pair. A title that mixes its 5.1 sounds
 * into FX sends routes them with this, as its GP effects image does. Call
 * before mcpx_apu_init_standalone. */
void mcpx_apu_set_speaker_map(const int8_t map[32]);

/* The settings overlay's speaker test: about a second of noise on one
 * speaker, 0..5 = FL FR FC LFE SL SR (a 60 Hz tone on the LFE), through the
 * device and layout the title plays on. via_map: put into the mixbins the
 * speaker map sends to that speaker (stereo: DirectSound's bin for it) and
 * mixed as the title's sounds are; else straight into that output channel.
 * Meant for while the title is paused; a new call replaces a test playing,
 * a speaker of -1 stops it.
 * _playing: the speaker playing now, -1 when none. */
void mcpx_apu_speaker_test(int speaker, int via_map);
int mcpx_apu_speaker_test_playing(int *via_map);

/* The settings overlay's menu sounds (BO-190): a one-shot of mono 16-bit
 * samples at 48 kHz, at `gain`, added to the front left and right of the
 * output after the mix. It plays while the title is paused too (the frame
 * thread runs for it alone then, as for the speaker test; the title's
 * voices stay still) and over the title's sound when it is not. A new call
 * replaces the one playing; pcm NULL stops it. The samples must stay valid
 * while it plays (the caller's static tables). */
void mcpx_apu_ui_cue(const int16_t *pcm, uint32_t n, float gain);

/* 5.1: the bins (a mask, bit = bin) the title's wiring names as effect
 * sends, and their level, 0..1 (RECOMP_AUDIO_FX_LEVEL, audio.fx_level,
 * default 0.25; live). No effect runs here, so a send reaches its speaker
 * dry; this keeps it down. Stereo does not use it. */
void mcpx_apu_set_fx_bins(uint32_t mask);
void mcpx_apu_set_fx_level(float level);

/* A title whose GP effects image has been read wires it with apu_gp.h
 * (apu_gp_direct, _eq, _drive, _export, before mcpx_apu_init_standalone):
 * the speaker feeds then come from that model of the image, in 5.1 and in
 * stereo, and the calls above (speaker map, effect-send level) are not
 * used. */

/* The low-pass on the LFE feed of such a title (RECOMP_AUDIO_LFE_LOWPASS,
 * audio.lfe_lowpass: off, 80, 100, 120 Hz; the settings file or the
 * variable, no overlay row; a change is faded over 21 ms). The
 * title's wiring sets it until this or the variable does. Any thread. */
void mcpx_apu_set_lfe_lowpass(int hz);

/* The level of a title's custom soundtrack (apu_level.h): 0 off, 1 quieter,
 * 2 match, 3 louder. RECOMP_AUDIO_CUSTOM_LEVEL, audio.custom_music_level;
 * live, any thread. */
void mcpx_apu_set_custom_level(int mode);

/* The speaker map in use (mcpx_apu_set_speaker_map, or the default). */
void mcpx_apu_speaker_map_get(int8_t map[32]);

/* RECOMP_MIX_TRACE (apu_vp.c, the mix study): the host asks this for who is
 * turning a voice on -- the title's return addresses on the calling guest
 * thread's stack, innermost first, at most max; returns how many. Not set,
 * or the trace off: never called. */
typedef int (*mcpx_apu_voice_tag_fn)(uint32_t *out, int max);
void mcpx_apu_set_voice_tag_hook(mcpx_apu_voice_tag_fn fn);
/* The gain a mixbin reaches its speaker with in 5.1 (the effects level and
 * the title's GP EQ when that is a plain gain; 1 otherwise). Trace only. */
float mcpx_apu_bin_feed_gain(int bin);
/* Trace only: the output before the clamp, per channel, from the DSP stage. */
void mcpx_apu_mix_trace_out(const float *acc, int nch, int n);
int mcpx_apu_mix_trace_on(void);

/* Play a 440Hz test tone through the APU pipeline to verify audio output.
 * Directly programs a voice without going through DirectSound. */
void mcpx_apu_play_test_tone(MCPXAPUState *d);

/* ============================================================
 * Software mixer - DirectSound buffer bridge
 *
 * DirectSound buffers register here. The APU frame thread
 * mixes active buffers directly into the waveOut output,
 * bypassing the VP hardware voice pipeline.
 * ============================================================ */

#define APU_MIXER_MAX_VOICES 64

typedef struct APUMixerVoice {
    volatile int     active;       /* 1 = playing, 0 = stopped */
    volatile int     looping;      /* 1 = loop, 0 = one-shot */
    const int16_t   *pcm_data;     /* Pointer to 16-bit PCM samples */
    uint32_t         pcm_bytes;    /* Total buffer size in bytes */
    uint32_t         num_channels;  /* 1=mono, 2=stereo */
    uint32_t         sample_rate;   /* e.g. 22050, 44100, 48000 */
    uint64_t         play_offset; /* Source frame position, 16 fractional bits */
    float            volume;       /* 0.0 to 1.0 */
} APUMixerVoice;

/* Allocate a mixer voice slot. Returns slot index or -1 if full. */
int apu_mixer_alloc_voice(void);

/* Free a mixer voice slot. */
void apu_mixer_free_voice(int slot);

/* Get pointer to a mixer voice for configuration. */
APUMixerVoice *apu_mixer_get_voice(int slot);

/* Seek in PCM bytes (rounded down to a whole frame). Returns 0 if invalid. */
int apu_mixer_set_position(int slot, uint32_t byte_offset);

/* Snapshot playback state under the mixer lock; output pointers may be NULL. */
void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping);

/* Start/resume or stop playback without changing the current position. */
void apu_mixer_play(int slot, int looping);
void apu_mixer_stop(int slot);

#ifdef __cplusplus
}
#endif

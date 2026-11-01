/*
 * apu_gp.h -- a title's GP effects image, modelled.
 *
 * On the console the Global Processor (a DSP56300) runs the effects image
 * the title gave DirectSound: it takes the 32 mixbins the voice processor
 * summed, runs the image's effect chain over them and leaves the six
 * speaker feeds. No DSP56300 runs here. A title whose image has been read
 * describes it with the calls below (which effect sits on which bin, where
 * its state is) and this module does what those effects do, with the
 * parameters the title itself keeps writing:
 *
 *   state    DirectSound keeps every effect's state block in GP scratch
 *            memory and tells the GP program to load a part of it with a
 *            command (apu_gp_command). The model has the same state words
 *            and loads them at the same moments, so a parameter moves when
 *            it does on the console, and an effect's own running state is
 *            overwritten when a load covers it, as there.
 *   direct   a bin summed into a speaker with a fixed gain (the speaker
 *            bins themselves).
 *   eq       the image's IIR2 effect: a bin through a biquad into one
 *            speaker. Its b0 is the send's gain and sign.
 *   drive    the image's in-place distortion (a wave folder into a state
 *            variable low-pass, mixed under the amplified dry signal), in
 *            the DSP's own 56-bit fixed point.
 *   export   the GP program's copy of a run of bins, after the in-place
 *            effects, into GP scratch memory every frame: what a title's
 *            "return" voices play from (their buffers lie on it).
 *   LFE      a 120 Hz low-pass on the LFE feed, as the console's AC-3
 *            encoder had after the GP. Not part of the image.
 *
 * Bins nothing is wired to reach no speaker, as on the console.
 *
 * Pure: no APU state, no clock; memory is reached through apu_gp_mem. One
 * instance, used on the APU frame thread.
 */
#ifndef APU_GP_H
#define APU_GP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_GP_BINS     32
#define APU_GP_FRAME    32
#define APU_GP_SPEAKERS 6               /* FL FR C LFE SL SR (apu_speakers.h) */
#define APU_GP_STATE_MAX 4096           /* words of effect state */
#define APU_GP_CODE_MAX 2048            /* words of a program run as it is */

/* GP scratch memory, by byte offset: a dword read, and n dwords written. */
typedef struct {
    uint32_t (*read)(void *ctx, uint32_t off);
    void (*write)(void *ctx, uint32_t off, const uint32_t *words, int n);
    void *ctx;
    /* The guest's view of the GP's X memory, a dword an X word from X:0,
     * or NULL. DirectSound does not only load effect states through
     * commands: it reads them back from here (an I3DL2 listener change
     * starts by reading the reverb's delay table, which the reverb's own
     * code completed) and writes some words here directly. The model
     * keeps its state words and this window the same: a word the guest
     * changed is taken over before a command or a frame, a word the model
     * changed is written out after it. */
    uint32_t *xwin;
} apu_gp_mem;

/* ── wiring: the title's port, before the APU starts ── */
void apu_gp_reset(void);                /* nothing wired */
int  apu_gp_wired(void);
/* bin -> speaker at gain (signed). */
void apu_gp_direct(int bin, int speaker, float gain);
/* bin -> the image's IIR2 effect -> speaker. state: the word index of the
 * effect's state block in the state area (b0 b1 b2 a1 a2 at +7..+11). */
void apu_gp_eq(int bin, int speaker, uint32_t state);
/* The image's distortion, in place on bin. state as above (parameters at
 * +7..+11, +13, +15; its filter at +0, +17, +18). */
void apu_gp_drive(int bin, uint32_t state);
/* An effect of the image run as the title's own code (apu_dsp56k.h): its
 * code_words words at GP scratch code_scratch_off, taken when the title
 * loads the image's state. For an effect too large to restate by hand (a
 * reverb with its delay lines in GP scratch): it runs once a frame after
 * the in-place effects above, reads the mixbins where the GP has them
 * (X:0x1400 + 0x20 * bin), adds to the speaker sums (X:0xC00 + 0x20 *
 * speaker) and finds its state words where the image put them (state word
 * n at X:0x80 + n), loaded by the title's commands like every other
 * effect's. Its last word must be the NOP the image ends every effect
 * with. If it meets something the interpreter does not have, the log says
 * so once and the program is left out. code_words 0: no program. */
void apu_gp_program(uint32_t code_scratch_off, uint32_t code_words);
/* DSP instructions run for it so far. */
unsigned long apu_gp_program_steps(void);

/* Each frame bins first .. first + count - 1 are written, after the
 * in-place effects, to GP scratch at scratch_off: 32 dwords a bin, a
 * signed 24-bit sample in each. */
void apu_gp_export(int first, int count, uint32_t scratch_off);
/* The block: 0 when nothing is exported. */
int  apu_gp_export_range(uint32_t *scratch_off, uint32_t *bytes, int *first_bin);
/* The LFE feed's low-pass (4th order Butterworth). 0: none. Also while it
 * plays (frame thread): the feed then fades to the new setting over 21 ms;
 * a call during a fade is ignored, so a caller that follows a setting
 * compares with apu_gp_lfe_lowpass_hz() every frame. */
void apu_gp_lfe_lowpass(int hz);
int  apu_gp_lfe_lowpass_hz(void);       /* the setting in force or being faded to */

/* An effect of the image that is not modelled: its input bin, and the
 * state words that say how far the title has it open (count words from
 * state, harmless up to quiet, as signed 24-bit integers). When the bin is
 * fed while a word is larger the log says so, once for each new maximum:
 * a sound that is missing can then be traced to it. what must stay valid. */
void apu_gp_unmodelled(const char *what, int bin, uint32_t state, int count, int32_t quiet);
/* The largest value those words had while the bin was fed. */
int32_t apu_gp_unmodelled_max(void);

/* What a bin reaches (for meters and the host mixer): its speaker, or -1,
 * and its gain there now. */
int   apu_gp_bin_speaker(int bin);
float apu_gp_bin_gain(int bin);

/* ── running (frame thread) ── */
/* A command the title posted to the GP program: cmd, and the words of its
 * block (word 0, word 2, word 3). 3 = load the whole state area: count
 * words from scratch offset src. 2 = load count words at state word dst
 * from scratch offset src. Others are ignored. */
void apu_gp_command(const apu_gp_mem *m, uint32_t cmd, uint32_t dst,
                    uint32_t src, uint32_t count);
/* One frame: bins as the voice processor left them (limited to 24 bits and
 * run through the in-place effects here), out the six speaker feeds (not
 * clamped: the GP saturates them at full scale, the caller's clamp). */
void apu_gp_frame(const apu_gp_mem *m, float bins[APU_GP_BINS][APU_GP_FRAME],
                  float out[APU_GP_SPEAKERS][APU_GP_FRAME]);

/* The distortion alone, for tests: n samples (signed 24-bit) in place with
 * the 23 state words st (parameters read, filter state updated). */
void apu_gp_drive_run(int32_t *st, int32_t *x, int n);

#ifdef __cplusplus
}
#endif

#endif /* APU_GP_H */

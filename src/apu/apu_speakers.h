/*
 * apu_speakers.h -- the speaker layout of the APU's output: stereo or 5.1.
 *
 * On the console the VP mixes every voice into 32 mixbins, the GP's DSP
 * program runs the effects (crosstalk, I3DL2 reverb, the title's FX chains)
 * and leaves the six speaker feeds in mixbins 0..5, which the EP encodes to
 * AC-3 for the optical/HDMI output or folds to stereo for the analog one.
 * Neither DSP is emulated here, so the speaker feeds are taken from the
 * mixbins directly (apu_spk_mix51) and handed to the host as 5.1 LPCM, or
 * folded to stereo with the ITU-R BS.775 coefficients (apu_spk_downmix) when
 * the device has two channels.
 *
 * Pure functions: no state, no environment, no device. apu_core.c decides
 * the layout and the kernel reports it to the title (XC_AUDIO).
 */
#ifndef APU_SPEAKERS_H
#define APU_SPEAKERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RECOMP_AUDIO_SPEAKERS (audio.speakers in settings.toml). */
enum {
    APU_SPK_AUTO = 0,       /* 5.1 when the default output has 6+ channels */
    APU_SPK_STEREO = 1,
    APU_SPK_51 = 2,
};

/* The output channel order, the WAVE/SMPTE one, which is also SDL's and
 * PipeWire's for six channels and the order of DirectSound's speaker
 * mixbins (DSMIXBIN_FRONT_LEFT .. DSMIXBIN_BACK_RIGHT = 0..5). */
enum {
    APU_SPK_FL = 0, APU_SPK_FR, APU_SPK_FC, APU_SPK_LFE, APU_SPK_SL, APU_SPK_SR,
    APU_SPK_CHANNELS
};

/* "auto", "stereo", "5.1" (also "2", "6", "surround"); NULL, empty or
 * anything else is auto. */
int apu_spk_parse(const char *s);

/* The layout the title is told about and mixes for: 6 or 2. device_channels
 * is what the default output reports, <= 0 when it could not be asked. */
int apu_spk_title_channels(int mode, int device_channels);

/* The channel count to open the device with for a title mixing
 * title_channels: 6 when the device takes six (or cannot be asked: the host
 * converts), 2 when it is known to be stereo (we downmix). */
int apu_spk_device_channels(int title_channels, int device_channels);

/* The XC_AUDIO dashboard word for a layout (XC_AUDIO_FLAGS_*, Cxbx-Reloaded
 * src/common/EmuEEPROM.h): stereo 0; 5.1 is SURROUND | ENABLE_AC3, what a
 * console with "Dolby Digital" ticked reports. */
#define APU_XC_AUDIO_STEREO      0x00000000u
#define APU_XC_AUDIO_SURROUND    0x00000002u
#define APU_XC_AUDIO_ENABLE_AC3  0x00010000u
uint32_t apu_spk_xc_audio(int title_channels);

/* Which speaker each of the 32 mixbins feeds: APU_SPK_FL..APU_SPK_SR, or
 * APU_SPK_NONE to leave the bin out. */
#define APU_SPK_BINS 32
#define APU_SPK_NONE (-1)
typedef int8_t apu_spk_map[APU_SPK_BINS];

/* DirectSound's own layout, for a title whose effects image does not say
 * otherwise (apu_speakers.c). */
void apu_spk_default_map(apu_spk_map map);

/* The six speaker feeds of n samples from the 32 mixbins, s16 interleaved
 * FL FR FC LFE SL SR. mixbins is float[32][stride], n at most 32. gain:
 * a level per bin, or NULL for unity. */
void apu_spk_mix51(const float *mixbins, int stride, const apu_spk_map map,
                   const float *gain, int16_t (*out)[APU_SPK_CHANNELS], int n);

/* ITU-R BS.775 5.1 -> 2.0: L = FL + 0.7071 C + 0.7071 SL,
 * R = FR + 0.7071 C + 0.7071 SR, LFE dropped; saturating, unnormalised
 * (PipeWire's channelmix default). in and out may not overlap. */
void apu_spk_downmix(const int16_t (*in)[APU_SPK_CHANNELS], int16_t (*out)[2], int n);

#ifdef __cplusplus
}
#endif

#endif /* APU_SPEAKERS_H */

/*
 * apu_speakers.c -- mixbins to the six speaker feeds, and 5.1 to stereo.
 *
 * Which bin is which speaker is DirectSound's, not the hardware's: the XDK's
 * DSMIXBIN_* numbering (Cxbx-Reloaded src/core/hle/DSOUND/XbDSoundTypes.h
 * lists the speaker half, XDSMIXBIN_FRONT_LEFT .. XDSMIXBIN_BACK_RIGHT = 0..5,
 * XDSMIXBIN_SPEAKERS_MAX 6):
 *
 *     0 front left    1 front right   2 center    3 LFE
 *     4 back left     5 back right
 *     6..9  XTLK front left/right, back left/right: 3D voices after the VP's
 *           HRTF, which the GP's crosstalk effect folds back into 0,1,4,5
 *     10    I3DL2 reverb send
 *     11..  FX sends, the inputs of the title's own GP effect chains
 *
 * On the console the GP's DSP program reads all 32 bins from its mix buffer
 * and leaves the finished speaker feeds in bins 0..5 of that same buffer:
 * xemu reads the GP's output back from X:0x1400 + bin * 0x20, which is
 * GP_DSP_MIXBUF_BASE, where it wrote the VP's bins (xemu
 * hw/xbox/mcpx/apu/dsp/gp_ep.c, mcpx_apu_dsp_frame; apu_regs.h). The EP then
 * encodes those six to AC-3, or folds them for the analog output.
 *
 * No DSP runs here, so the effects are bypassed and every bin goes to one
 * speaker (apu_spk_default_map): 0..5 to their own, the XTLK bins to the
 * speakers the crosstalk effect would have folded them into (without its
 * filter), and the sends (10 and up) to the front pair, even bins left and
 * odd right -- the same fold the stereo path (apu_dsp.c) gives every bin, so
 * a send is as loud in 5.1 as in stereo. A reverb send heard dry is not a
 * reverb; it is what the stereo path has always done with it.
 *
 * Which speaker an FX send ends up on is the title's: its effects image
 * routes each send through its own chain to a speaker, with a gain and a
 * sign, and may take a send to no speaker at all (a bin that only feeds an
 * in-place effect whose output the title plays back through a voice). A
 * map of bin to speaker cannot say that: a title that mixes its 5.1 sounds
 * into sends (Burnout 3 does) wires a model of its image instead
 * (apu_gp.h), and the map here is then the model's routing. A title
 * without one may still hand a plain map to mcpx_apu_set_speaker_map.
 */
#include "apu_speakers.h"
#include "apu_limit.h"

#include <string.h>
#if defined(_WIN32)
#define spk_strcasecmp _stricmp
#else
#include <strings.h>
#define spk_strcasecmp strcasecmp
#endif

void apu_spk_default_map(apu_spk_map map)
{
    for (int b = 0; b < APU_SPK_BINS; b++)
        map[b] = (int8_t)((b & 1) ? APU_SPK_FR : APU_SPK_FL);
    map[0] = APU_SPK_FL;  map[1] = APU_SPK_FR;
    map[2] = APU_SPK_FC;  map[3] = APU_SPK_LFE;
    map[4] = APU_SPK_SL;  map[5] = APU_SPK_SR;
    map[6] = APU_SPK_FL;  map[7] = APU_SPK_FR;      /* XTLK front */
    map[8] = APU_SPK_SL;  map[9] = APU_SPK_SR;      /* XTLK back */
}

int apu_spk_parse(const char *s)
{
    if (!s || !*s)
        return APU_SPK_AUTO;
    if (!spk_strcasecmp(s, "stereo") || !strcmp(s, "2"))
        return APU_SPK_STEREO;
    if (!strcmp(s, "5.1") || !strcmp(s, "6") || !spk_strcasecmp(s, "surround"))
        return APU_SPK_51;
    return APU_SPK_AUTO;
}

int apu_spk_title_channels(int mode, int device_channels)
{
    if (mode == APU_SPK_51)
        return APU_SPK_CHANNELS;
    if (mode == APU_SPK_STEREO)
        return 2;
    return device_channels >= APU_SPK_CHANNELS ? APU_SPK_CHANNELS : 2;
}

int apu_spk_device_channels(int title_channels, int device_channels)
{
    if (title_channels != APU_SPK_CHANNELS)
        return 2;
    return device_channels > 0 && device_channels < APU_SPK_CHANNELS
        ? 2 : APU_SPK_CHANNELS;
}

uint32_t apu_spk_xc_audio(int title_channels)
{
    return title_channels == APU_SPK_CHANNELS
        ? APU_XC_AUDIO_SURROUND | APU_XC_AUDIO_ENABLE_AC3
        : APU_XC_AUDIO_STEREO;
}

/* The stereo path's conversion (apu_dsp.c): clamp to [-1, 1], scale by
 * 32767, truncate. */
static inline int16_t to_s16(float x)
{
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)(x * 32767.0f);
}

void apu_spk_mix51(const float *mixbins, int stride, const apu_spk_map map,
                   const float *gain, int16_t (*out)[APU_SPK_CHANNELS], int n)
{
    /* A bin at a time into six accumulators: 32 adds a sample, as the
     * stereo fold. */
    float acc[APU_SPK_CHANNELS][32];

    if (n > 32)
        n = 32;
    memset(acc, 0, sizeof(acc));
    for (int b = 0; b < APU_SPK_BINS; b++) {
        const float *src = mixbins + (size_t)b * (size_t)stride;
        float *dst;
        if (map[b] < 0 || map[b] >= APU_SPK_CHANNELS)
            continue;
        dst = acc[map[b]];
        if (gain && gain[b] != 1.0f) {
            const float g = gain[b];
            for (int i = 0; i < n; i++)
                dst[i] += g * src[i];
        } else {
            for (int i = 0; i < n; i++)
                dst[i] += src[i];
        }
    }
    apu_limit(acc, APU_SPK_CHANNELS, n);    /* the safety limiter (apu_limit.h) */
    for (int i = 0; i < n; i++)
        for (int c = 0; c < APU_SPK_CHANNELS; c++)
            out[i][c] = to_s16(acc[c][i]);
}

/* ITU-R BS.775-3, table 2 (3/2 to 2/0): the centre and each surround at
 * -3 dB into its side; the LFE is not part of the downmix. Q15 so the
 * frame thread does no float work for it. */
#define ITU_M3DB 23170                  /* 0.70711 * 32768 */

static inline int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void apu_spk_downmix(const int16_t (*in)[APU_SPK_CHANNELS], int16_t (*out)[2], int n)
{
    /* Three full-scale terms reach 2.41 * 2^30, past int32: sum in 64. */
    for (int i = 0; i < n; i++) {
        int64_t c = (int64_t)in[i][APU_SPK_FC] * ITU_M3DB;
        int64_t l = (int64_t)in[i][APU_SPK_FL] * 32768 + c
                  + (int64_t)in[i][APU_SPK_SL] * ITU_M3DB;
        int64_t r = (int64_t)in[i][APU_SPK_FR] * 32768 + c
                  + (int64_t)in[i][APU_SPK_SR] * ITU_M3DB;
        int32_t lo = (int32_t)(l >> 15), ro = (int32_t)(r >> 15);
        apu_limit_fold16(&lo, &ro);         /* after the fold, before the saturation (apu_limit.h) */
        out[i][0] = sat16(lo);
        out[i][1] = sat16(ro);
    }
}

/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* The device backend behind these functions: XAudio2 on Windows
 * (apu_xaudio2.c), SDL2 on POSIX hosts (apu_sdl2.c). Same contract. */
#if defined(_WIN32) && !defined(XBOXRECOMP_WIN_VULKAN)
#define XA2_BACKEND_NAME "XAudio2"
#else
#define XA2_BACKEND_NAME "SDL2"
#endif

/* The speaker layout (apu_speakers.h). Before xa2_init: xa2_probe_channels
 * brings the backend's audio subsystem up if it has to and returns the
 * default output's channel count, <= 0 when it cannot be asked;
 * xa2_request_channels sets the layout the title mixes for (2 or 6), which
 * xa2_init opens the device for (apu_spk_device_channels). */
int xa2_probe_channels(void);
void xa2_request_channels(int title_channels);

/* Initialize the device backend. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* The channel count of the open device, 2 or 6: what xa2_submit_samples
 * takes interleaved. Called by the frame thread once per period, before it
 * lays out that period; that is also where a default device that came or
 * went (an SDL audio hotplug event) is acted on, by reopening the device
 * for the layout the new default takes. Never changes inside a period. */
int xa2_channels(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved 16-bit samples, xa2_channels() to a sample frame.
 * Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Output gain, 0..1 (master volume, mute): applied to what is submitted from
 * now on, ramped across the next period so a change does not click. Any
 * thread; 1 is the default. */
void xa2_set_gain(float gain);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Number of device buffers the voice may hold. */
int xa2_get_buffer_count(void);

/* Samples handed over and not yet played (queued buffers plus the partly
 * filled one). Also counts underruns: see xa2_underruns. */
int xa2_queued_samples(void);

/* The queue is about to drain on purpose (the frame thread is pausing):
 * the dry spell that follows is not an underrun. */
void xa2_expect_drain(void);

/* Pacing marks in samples for count buffers of buf_samples, fed one period
 * (period samples) at a time. A period is submitted only while queued <
 * high, so high is capped where that period could fill a buffer the voice
 * has no slot for: queued + period must stay below (count + 1) buffers
 * (count on the voice, one staging). Exact in samples, so buffer sizes that
 * are not multiples of the period work too. low is half of high. */
static inline void xa2_pacing_marks(int buf_samples, int count, int period,
                                    int *low, int *high)
{
    int h = buf_samples * count;
    int cap = buf_samples * (count + 1) - period;
    if (h > cap) h = cap;
    if (h < 1) h = 1;
    *high = h;
    *low = h / 2;
}

/* Times the voice ran dry after audio started ([XA2] underrun x%u). */
unsigned xa2_underruns(void);

/* Samples the device has consumed since init: XAudio2's SamplesPlayed, or
 * on SDL2 everything queued minus what is still queued. Read against the
 * wall clock this is the host-side starvation measure: while the device
 * plays, it advances 48000 a second; whatever it falls behind is silence
 * the device inserted (an underrun the WAV tap, which records submissions,
 * cannot see). */
uint64_t xa2_samples_played(void);

#endif /* APU_XAUDIO2_H */

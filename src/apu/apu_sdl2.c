/**
 * SDL2 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux: the APU frame
 * thread hands over one EP period (256 stereo frames) at a time, and the
 * device queue paces it (mcpx_apu_pace_step reads xa2_queued_samples).
 * SDL's push API (SDL_QueueAudio) is the queue; its device thread pulls one
 * device buffer per callback and pads a short pull with silence, which is
 * why the pacing marks keep at least one device buffer queued.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIODRIVER is already set: it drains the queue in real time without
 * a device, so pacing and the WAV tap behave as on a real device and nothing
 * reaches the speakers. SDL_AUDIODRIVER=coreaudio (or pulseaudio, pipewire)
 * with RECOMP_HEADLESS=1 is the way to hear a headless run.
 *
 * 5.1: the device is opened with six channels when the title mixes 5.1 and
 * the default output takes six (or cannot say: SDL and the sound server
 * convert then), with two when the default output is known to be stereo, in
 * which case apu_core.c folds the period down first. SDL_GetDefaultAudioInfo
 * (SDL >= 2.24) is the probe; PipeWire, PulseAudio and CoreAudio answer it
 * with the channel count the system's output profile is set to. An audio
 * device added or removed (SDL_AUDIODEVICEADDED/REMOVED, seen by an event
 * watch on whichever thread SDL posts them from) makes the frame thread ask
 * again at its next period and reopen the device if the answer changed.
 *
 * A device that stops (BO-118). The frame thread waits on the queue's depth,
 * so a device thread that no longer pulls would stop the APU, and a title
 * that waits on the APU with it (Burnout 3 spins in DirectSound's doorbell
 * poll). It happened on Linux after the machine woke: SDL 3's PipeWire sink
 * moved the stream to the new default output on its hotplug thread, waited
 * there for ever for a stream that never started, and held the device's
 * lock, which the old device's thread then waited on. So:
 *
 *   - Half a second without a sample taken, with a buffer queued, is a
 *     stall. From then the queue is a pretend one that empties at 48 kHz
 *     (what SDL's dummy driver does): the APU keeps its pace and what it
 *     renders is dropped. A buffer of silence is left queued; when the
 *     device takes from it, it is back.
 *   - Still stalled after four seconds, or gone: the device is closed and
 *     the default opened again, every five seconds until one opens.
 *   - Whatever of SDL can wait on a device's lock (probe, close, open) runs
 *     on a worker thread, never on the frame thread: a call that does not
 *     come back costs the sound, not the game, and the log says so.
 *   - After a gap in the run (host_time.h: the machine slept, the process
 *     was stopped) the queue is dropped, the device closed and the default
 *     output opened anew (why: service()).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_speakers.h"
#include "apu_xaudio2.h"
#include "recomp_env.h"
#include "../platform/host_time.h"

#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)

#include <SDL.h>
#include <time.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS_MAX  6
/* Device buffer size and count: the same defaults and knobs as the XAudio2
 * backend (512 x 4: 10.7 ms per buffer, 43 ms queued at the high mark). The
 * count is a pacing mark here, not a slot limit: SDL's queue is unbounded. */
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT    4
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        8

static SDL_AudioDeviceID g_dev;
static int       g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = SDLA_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static unsigned  g_underruns;
static int       g_dry;
static int64_t   g_underrun_logged_ms;
static int       g_channels = 2;    /* the open device's, 2 or 6 */
static int       g_title_channels = 2;
static SDL_atomic_t g_hotplug;      /* a device came or went: probe again */
static int       g_watching;

/* The device that stopped taking samples, and the worker (see the top). */
#define SDLA_STALL_MS        500
#define SDLA_REOPEN_MS       4000
#define SDLA_RETRY_MS        5000
#define SDLA_WORKER_LATE_MS  10000
static int       g_virt;            /* the queue is the pretend one */
static int64_t   g_virt_q;          /* its depth, samples */
static uint64_t  g_virt_ns;         /* emptied up to here */
static int64_t   g_stall_ms;        /* when the stall began */
static Uint32    g_probe_bytes;     /* silence left queued on a stalled device */
static uint64_t  g_played_seen;     /* stall detector: samples taken, */
static int64_t   g_progress_ms;     /* when that last grew, */
static int64_t   g_polled_ms;       /* and the last look */
static int64_t   g_retry_ms;        /* no device: the next open */
static int       g_open_failed_said;
static SDL_atomic_t g_woke;         /* a gap in the run (any thread) */
static int       g_wake_listening;
enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };
static SDL_atomic_t g_job_state;
static struct {
    int probe_only;                 /* in: ask only, touch no device */
    SDL_AudioDeviceID old;          /* in: close this first (reopen) */
    SDL_AudioDeviceID status_of;    /* in: is this one still playing (probe) */
    int want;                       /* out: channels the default takes */
    int stopped;                    /* out (probe): status_of plays nothing */
    SDL_AudioDeviceID dev;          /* out (reopen): the new device, or 0 */
    int buf_samples;                /* out (reopen) */
    char err[160];
} g_job;
static int64_t   g_job_ms;          /* when it started */
static int       g_job_late_said;
static int64_t   g_init_ms;
static double    g_hook_stall_at = -1.0, g_hook_stall_for = -1.0;
static int       g_hook_state;      /* 0 armed, 1 stalled, 2 over */

static int env_int(recomp_env_id id, int def, int lo, int hi)
{
    int v = recomp_env_int(id, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The audio subsystem, brought up once by whichever of probe or init comes
 * first. */
static int audio_subsystem_up(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO))
        return 1;
    /* Headless: no speakers unless the caller names a driver. Must be set
     * before the audio subsystem starts; 0 keeps an existing value.
     * SDL_AUDIODRIVER is SDL's own variable, not one of ours. */
    if (recomp_env_on(RENV_HEADLESS))
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);

    /* Audio pulls in SDL's event subsystem, which would trap SIGINT and
     * SIGTERM into SDL_QUIT; nothing pumps events in a headless run, so
     * Ctrl-C would be swallowed. Keep the process defaults. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        XR_ERROR( "[SDL2] audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_audio_opened = 1;
    return 1;
}

int xa2_probe_channels(void)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_AudioSpec spec;
    char *name = NULL;

    if (!audio_subsystem_up())
        return 0;
    if (SDL_GetDefaultAudioInfo(&name, &spec, 0) != 0)
        return 0;
    SDL_free(name);
    return spec.channels;
#else
    return 0;
#endif
}

void xa2_request_channels(int title_channels)
{
    g_title_channels = title_channels == SDLA_CHANNELS_MAX ? SDLA_CHANNELS_MAX : 2;
}

/* SDL calls this from the thread posting the event; only a flag is set
 * here, the frame thread does the rest (xa2_channels). */
static int SDLCALL hotplug_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    if ((e->type == SDL_AUDIODEVICEADDED || e->type == SDL_AUDIODEVICEREMOVED) &&
        !e->adevice.iscapture)
        SDL_AtomicSet(&g_hotplug, 1);
    return 1;
}

static SDL_AudioDeviceID open_device_id(int channels, int *buf_samples)
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;

    memset(&want, 0, sizeof(want));
    want.freq     = SDLA_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples  = (Uint16)g_buf_samples;
    want.callback = NULL;                       /* push: SDL_QueueAudio */

    /* No format changes allowed: the mixer's 48 kHz s16 is what the queue
     * takes, and SDL converts for the device if it has to. */
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0)
        return 0;
    *buf_samples = have.samples > 0 ? have.samples : g_buf_samples;
    SDL_PauseAudioDevice(dev, 0);
    return dev;
}

static int open_device(int channels)
{
    int buf = g_buf_samples;

    g_dev = open_device_id(channels, &buf);
    if (g_dev == 0) {
        XR_ERROR( "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    if (buf != g_buf_samples) {
        fprintf(stderr, "[SDL2] device buffer is %d samples (asked %d)\n",
                buf, g_buf_samples);
        g_buf_samples = buf;
    }
    g_channels = channels;
    g_dry = 0;
    return 1;
}

/* ── The worker, and the pretend queue ───────────────────── */

static int SDLCALL job_main(void *ud)
{
    (void)ud;
    g_job.want = apu_spk_device_channels(g_title_channels,
                     g_title_channels == SDLA_CHANNELS_MAX ? xa2_probe_channels() : 0);
    if (g_job.probe_only) {
        g_job.stopped = g_job.status_of &&
            SDL_GetAudioDeviceStatus(g_job.status_of) == SDL_AUDIO_STOPPED;
    } else {
        if (g_job.old)
            SDL_CloseAudioDevice(g_job.old);
        while (recomp_env_on(RENV_AUDIO_OPEN_HANG))
            SDL_Delay(1000);                    /* test hook */
        g_job.dev = open_device_id(g_job.want, &g_job.buf_samples);
        if (!g_job.dev)
            SDL_strlcpy(g_job.err, SDL_GetError(), sizeof g_job.err);
    }
    SDL_AtomicSet(&g_job_state, JOB_DONE);
    return 0;
}

static int job_start(int probe_only, SDL_AudioDeviceID dev)
{
    SDL_Thread *t;

    if (SDL_AtomicGet(&g_job_state) != JOB_IDLE)
        return 0;
    memset(&g_job, 0, sizeof g_job);
    g_job.probe_only = probe_only;
    if (probe_only)
        g_job.status_of = dev;
    else
        g_job.old = dev;
    g_job.buf_samples = g_buf_samples;
    g_job_ms = now_ms();
    g_job_late_said = 0;
    SDL_AtomicSet(&g_job_state, JOB_RUNNING);
    t = SDL_CreateThread(job_main, "audio-device", NULL);
    if (!t) {
        SDL_AtomicSet(&g_job_state, JOB_IDLE);
        return 0;
    }
    SDL_DetachThread(t);
    return 1;
}

static int real_queued(void)
{
    return (int)(SDL_GetQueuedAudioSize(g_dev) / (g_channels * sizeof(int16_t)));
}

/* The pretend queue takes over at depth q. */
static void virt_enter(int q)
{
    g_virt = 1;
    g_virt_q = q;
    g_virt_ns = xbox_HostNowNs();
    g_stall_ms = now_ms();
    g_dry = 1;                          /* nothing here is an underrun */
}

static int virt_queued(void)
{
    uint64_t now = xbox_HostNowNs();
    uint64_t took = (now - g_virt_ns) * SDLA_SAMPLE_RATE / 1000000000ull;

    if (took) {
        g_virt_ns += took * 1000000000ull / SDLA_SAMPLE_RATE;
        g_virt_q = g_virt_q > (int64_t)took ? g_virt_q - (int64_t)took : 0;
    }
    return (int)g_virt_q;
}

static void virt_leave(void)
{
    g_virt = 0;
    g_played_seen = 0;
    g_progress_ms = g_polled_ms = now_ms();
    g_dry = 1;
}

/* The device goes to the worker to be closed and the default opened. */
static void reopen_start(void)
{
    SDL_AudioDeviceID old = g_dev;

    if (!g_virt)
        virt_enter(g_dev ? real_queued() : 0);
    g_dev = 0;
    if (!job_start(0, old)) {
        g_dev = old;                    /* no thread: asked again later */
        g_probe_bytes = 0;
        g_stall_ms = now_ms();
    }
}

/* What the worker brought back. A reopen may change the channel count, so
 * a 5.1 title's is taken only where a period begins (xa2_channels). */
static void job_adopt(void)
{
    if (SDL_AtomicGet(&g_job_state) != JOB_DONE)
        return;
    if (g_job.probe_only) {
        SDL_AtomicSet(&g_job_state, JOB_IDLE);
        if (g_dev && g_dev == g_job.status_of &&
            (g_job.want != g_channels || g_job.stopped)) {
            fprintf(stderr, "[SDL2] default output changed: reopening for %s\n",
                    g_job.want == SDLA_CHANNELS_MAX ? "5.1" : "stereo (5.1 folded down)");
            reopen_start();
        }
        return;
    }
    if (g_job.dev) {
        g_dev = g_job.dev;
        g_channels = g_job.want;
        g_buf_samples = g_job.buf_samples;
        virt_leave();
        g_open_failed_said = 0;
        fprintf(stderr, "[SDL2] audio device open again after %lld ms (%s)\n",
                (long long)(now_ms() - g_stall_ms),
                g_channels == SDLA_CHANNELS_MAX ? "5.1" : "stereo");
    } else {
        if (!g_open_failed_said++)
            fprintf(stderr, "[SDL2] the audio device did not open (%s): the game"
                    " goes on without sound; trying again every %d s\n",
                    g_job.err, SDLA_RETRY_MS / 1000);
        g_retry_ms = now_ms() + SDLA_RETRY_MS;
    }
    SDL_AtomicSet(&g_job_state, JOB_IDLE);
}

static void wake_flag(uint64_t gap_ns)
{
    (void)gap_ns;
    SDL_AtomicSet(&g_woke, 1);
}

/* RECOMP_DEBUG=audio_stall: the device paused is a device whose thread
 * takes nothing, which is all the frame thread sees of a blocked one. */
static void stall_hook(int64_t now)
{
    double t = (double)(now - g_init_ms) / 1000.0;

    if (g_hook_stall_at < 0.0 || g_hook_state == 2 || !g_dev)
        return;
    if (g_hook_state == 0 && t >= g_hook_stall_at) {
        fprintf(stderr, "[SDL2] test hook: the device stops taking samples\n");
        SDL_PauseAudioDevice(g_dev, 1);
        g_hook_state = g_hook_stall_for < 0.0 ? 2 : 1;
    } else if (g_hook_state == 1 && t >= g_hook_stall_at + g_hook_stall_for) {
        fprintf(stderr, "[SDL2] test hook: the device takes samples again\n");
        SDL_PauseAudioDevice(g_dev, 0);
        g_hook_state = 2;
    }
}

/* Once a period, on the frame thread, before the queue is read. */
static void service(void)
{
    int64_t now = now_ms();
    int job;

    if (g_title_channels != SDLA_CHANNELS_MAX)
        job_adopt();
    stall_hook(now);
    job = SDL_AtomicGet(&g_job_state);
    if (SDL_AtomicGet(&g_woke) && (job == JOB_IDLE || !g_dev)) {
        /* A gap in the run: the device is closed and the default opened
         * anew, which also drops what was queued. Carrying on with the old
         * one is not enough: CoreAudio under SDL 3 took samples at 2.8
         * times the rate for a third as long as the process had been
         * stopped (4 s after 12 s, more than 30 s after 45 s), every pull
         * running the queue dry, and the APU, paced by the queue, played
         * the title's sound that much too fast meanwhile. */
        SDL_AtomicSet(&g_woke, 0);
        g_played_seen = 0;
        g_progress_ms = g_polled_ms = now;
        if (g_dev) {
            reopen_start();
            job = SDL_AtomicGet(&g_job_state);
        }
    }

    if (job == JOB_RUNNING && !g_job_late_said &&
        now - g_job_ms > SDLA_WORKER_LATE_MS) {
        g_job_late_said = 1;
        fprintf(stderr, "[SDL2] SDL has not come back from the audio device after"
                " %d s (%s): the game goes on without it\n", SDLA_WORKER_LATE_MS / 1000,
                g_job.probe_only ? "asking about the default output" : "closing and opening it");
    }

    if (!g_dev) {
        if (job == JOB_IDLE && now >= g_retry_ms)
            reopen_start();
        return;
    }
    if (g_virt) {
        /* Stalled, the device still ours: is it taking the silence? */
        if (SDL_GetQueuedAudioSize(g_dev) < g_probe_bytes) {
            fprintf(stderr, "[SDL2] the audio device takes samples again after %lld ms\n",
                    (long long)(now - g_stall_ms));
            virt_leave();
        } else if (now - g_stall_ms > SDLA_REOPEN_MS && job == JOB_IDLE) {
            fprintf(stderr, "[SDL2] still nothing taken after %d s: closing the device"
                    " and opening the default output again\n", SDLA_REOPEN_MS / 1000);
            reopen_start();
        }
        return;
    }
    /* Playing: has it taken anything lately? Only while this is asked every
     * period (a pause, a gap in the run and the first call start over), and
     * only with a buffer's worth queued for it to take. */
    {
        int q = real_queued();
        uint64_t played = g_samples_queued > (uint64_t)q ? g_samples_queued - (uint64_t)q : 0;

        if (played != g_played_seen || q < g_buf_samples || now - g_polled_ms > 100) {
            g_played_seen = played;
            g_progress_ms = now;
        } else if (now - g_progress_ms > SDLA_STALL_MS) {
            static const int16_t silence[SDLA_BUF_SAMPLES_MAX * SDLA_CHANNELS_MAX];
            int n = g_buf_samples < SDLA_BUF_SAMPLES_MAX ? g_buf_samples : SDLA_BUF_SAMPLES_MAX;

            fprintf(stderr, "[SDL2] the audio device has taken nothing for %lld ms"
                    " (%d samples queued): the game goes on without it\n",
                    (long long)(now - g_progress_ms), q);
            SDL_ClearQueuedAudio(g_dev);
            SDL_QueueAudio(g_dev, silence, (Uint32)n * g_channels * sizeof(int16_t));
            g_probe_bytes = SDL_GetQueuedAudioSize(g_dev);
            virt_enter(q);
        }
        g_polled_ms = now;
    }
}

int xa2_init(void)
{
    const char *driver;
    int probed;

    if (g_initialized) return 1;

    g_buf_samples = env_int(RENV_AUDIO_BUF_SAMPLES, SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int(RENV_AUDIO_BUF_COUNT, SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    if (!audio_subsystem_up())
        return 0;

    probed = g_title_channels == SDLA_CHANNELS_MAX ? xa2_probe_channels() : 0;
    if (!open_device(apu_spk_device_channels(g_title_channels, probed))) {
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    g_frames_written = 0;
    g_initialized = 1;
    g_init_ms = g_progress_ms = g_polled_ms = now_ms();
    g_virt = 0;
    g_hook_state = 0;
    {
        const char *h = recomp_env(RENV_AUDIO_STALL);
        if (h && *h) {
            const char *c = strchr(h, ':');
            g_hook_stall_at = atof(h);
            g_hook_stall_for = c ? atof(c + 1) : -1.0;
        }
    }
    if (!g_wake_listening) {
        xbox_HostWakeListener("the audio device (its queue dropped, closed and the"
                              " default output opened anew)", wake_flag);
        g_wake_listening = 1;
    }
    /* A stereo title plays on anything: only a 5.1 one follows the default
     * output around. */
    if (g_title_channels == SDLA_CHANNELS_MAX && !g_watching) {
        SDL_AtomicSet(&g_hotplug, 0);
        SDL_AddEventWatch(hotplug_watch, NULL);
        g_watching = 1;
    }

    driver = SDL_GetCurrentAudioDriver();
    XR_NOTICE( "[SDL2] audio initialized: driver %s (%d Hz %s 16-bit, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE,
            g_channels == SDLA_CHANNELS_MAX ? "5.1" : "stereo",
            g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: no device output]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_watching) {
        SDL_DelEventWatch(hotplug_watch, NULL);
        g_watching = 0;
    }
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_ClearQueuedAudio(g_dev);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL2] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

int xa2_channels(void)
{
    if (!g_initialized)
        return g_channels;
    job_adopt();
    /* A device that came or went: the worker asks what the default output
     * takes now, and whether ours still plays. A device we had open that is
     * gone plays nothing (SDL drains its queue in time, so pacing carries
     * on): the default is opened again. */
    if (SDL_AtomicGet(&g_hotplug) && g_dev && job_start(1, g_dev))
        SDL_AtomicSet(&g_hotplug, 0);
    return g_channels;
}

/* Hand one EP period to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. Nothing is ever refused by depth: the queue is
 * unbounded and pacing by queue depth holds it at the marks. */
static volatile float g_gain = 1.0f;
static float g_gain_applied = 1.0f;     /* the submit side's, last period */

void xa2_set_gain(float gain)
{
    g_gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int16_t scaled[1024 * SDLA_CHANNELS_MAX];
    const int ch = g_channels;
    float target = g_gain;

    if (!g_initialized || num_samples <= 0) return 0;
    if (g_virt || !g_dev) {
        /* No device taking samples: counted, paced, dropped. */
        g_gain_applied = target;
        g_virt_q += num_samples;
        g_frames_written++;
        g_samples_queued += (uint64_t)num_samples;
        return 1;
    }
    if (target != 1.0f || g_gain_applied != 1.0f) {
        /* Scaled in chunks, the gain stepping linearly from the last
         * period's to the new one across this call. */
        int done = 0;
        float from = g_gain_applied;
        while (done < num_samples) {
            int n = num_samples - done < 1024 ? num_samples - done : 1024, i;
            for (i = 0; i < n; i++) {
                float g = from + (target - from) * (float)(done + i + 1) / (float)num_samples;
                int c;
                for (c = 0; c < ch; c++) {
                    int16_t v = samples[(size_t)(done + i) * ch + c];
                    scaled[i * ch + c] = (int16_t)((float)v * g);
                }
            }
            if (SDL_QueueAudio(g_dev, scaled,
                               (Uint32)n * ch * sizeof(int16_t)) != 0) {
                fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
                return 0;
            }
            done += n;
        }
        g_gain_applied = target;
        g_frames_written++;
        g_samples_queued += (uint64_t)num_samples;
        return 1;
    }
    if (SDL_QueueAudio(g_dev, samples,
                       (Uint32)num_samples * ch * sizeof(int16_t)) != 0) {
        fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread. The device holds
 * up to one more buffer in flight, which this does not count, the same way
 * XAudio2's BuffersQueued does not count the one playing. Also the underrun
 * detector: an empty queue after audio has started means the device's last
 * pull was short and padded with silence; counted once per dry spell and
 * reported at most once a second. A drain announced by xa2_expect_drain
 * (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    Uint32 bytes;

    if (!g_initialized) return 0;
    service();
    if (g_virt || !g_dev)
        return virt_queued();
    bytes = SDL_GetQueuedAudioSize(g_dev);
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL2] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)(bytes / (g_channels * sizeof(int16_t)));
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* What the device thread has pulled off the queue; it pulls one device
 * buffer per callback, so this leads the speaker by up to one buffer. */
uint64_t xa2_samples_played(void)
{
    uint64_t queued;

    if (!g_initialized) return 0;
    queued = g_virt || !g_dev ? (uint64_t)g_virt_q
           : SDL_GetQueuedAudioSize(g_dev) / (g_channels * sizeof(int16_t));
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */

/*
 * android_vsync.c - the display's refresh times on Android, for
 * RECOMP_VBLANK_SYNC=display (kernel_pacing.h, xbox_VblankDisplayRefresh).
 *
 * The other hosts take them from VK_KHR_present_wait (Linux) or
 * IDXGIOutput::WaitForVBlank (Windows). Android's Vulkan loader offers no
 * present_wait, so without this the emulated 60 Hz vblank is a free-running
 * timer beside the panel's own. The system's source is the Choreographer:
 * a callback per display refresh carrying that refresh's timestamp
 * (CLOCK_MONOTONIC, the runtime's clock), which is what video players and
 * game streamers pace on. AChoreographer_postFrameCallback64 (API 29) on a
 * thread of its own with a looper; each callback re-posts itself.
 *
 * First cut: the timestamps only. The frame timelines and deadlines of
 * AChoreographer_postVsyncCallback (API 33) are not used.
 */
#if defined(__ANDROID__)

#include <android/choreographer.h>
#include <android/looper.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns);
int xbox_VblankDisplayLockOn(void);

static volatile int s_run;
static pthread_t s_th;
static uint64_t s_n, s_last;

static void on_frame(int64_t frame_ns, void *data)
{
    AChoreographer *c = (AChoreographer *)data;
    if (!s_run)
        return;
    if (s_n == 1 && s_last)
        fprintf(stderr, "[PRESENT] vblank sync: the emulated vblank follows the display's"
                " refresh (Choreographer, %.2f ms apart)\n", (double)(frame_ns - s_last) / 1e6);
    s_last = (uint64_t)frame_ns;
    s_n++;
    xbox_VblankDisplayRefresh((uint64_t)frame_ns, 0);
    AChoreographer_postFrameCallback64(c, on_frame, c);
}

static void *vsync_main(void *arg)
{
    AChoreographer *c;
    (void)arg;
    pthread_setname_np(pthread_self(), "vsync");
    ALooper_prepare(0);
    c = AChoreographer_getInstance();
    if (!c) {
        fprintf(stderr, "[PRESENT] vblank sync: no Choreographer; the vblank keeps its own"
                " 60 Hz timer\n");
        return NULL;
    }
    AChoreographer_postFrameCallback64(c, on_frame, c);
    while (s_run)
        ALooper_pollOnce(250, NULL, NULL, NULL);
    return NULL;
}

void xbox_AndroidVsyncStart(void)
{
    if (s_run || !xbox_VblankDisplayLockOn())
        return;
    s_run = 1;
    if (pthread_create(&s_th, NULL, vsync_main, NULL) != 0)
        s_run = 0;
}

void xbox_AndroidVsyncStop(void)
{
    if (!s_run)
        return;
    s_run = 0;
    pthread_join(s_th, NULL);
}

#else
typedef int xbox_android_vsync_unused;
#endif

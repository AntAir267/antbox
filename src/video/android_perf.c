/*
 * android_perf.c - what the game asks of Android for a steady 60 fps.
 *
 * Played with a controller alone, a phone sees no touches: some systems
 * (MIUI on a Poco F4) then let the display's refresh fall and end the
 * touch boost, and the game ran at ~30 fps until the screen was touched.
 * Two requests here, both looked up at run time (dlsym) so that a system
 * without them only says so:
 *
 *   frame rate   ANativeWindow_setFrameRate(60, FIXED_SOURCE) (API 30) on
 *                every window surface the presenter makes, the first and
 *                each one after the app comes back (fb_present_vulkan.c,
 *                make_surface): only with RECOMP_ANDROID_FRAMERATE=1 (see
 *                xbox_AndroidSetFrameRate). The display mode is asked for by
 *                the activity (GameActivity.requestDisplayMode).
 *
 *   ADPF         APerformanceHint (API 33): a session per hot thread
 *                (guest-main: the title; nv2a-ack: the pushbuffer executor;
 *                sdl-main: the present; mcpx-apu: the audio processor),
 *                each with a 16.67 ms target. Once
 *                per presented frame each session is told how long its
 *                thread ran since the frame before: the thread's own CPU
 *                time (its CLOCK_THREAD_CPUTIME), not the wall time between
 *                frames, so a thread that sleeps or waits on the GPU asks
 *                for nothing. Closed in the way out (fb_present_sdl.c).
 *
 *   big cores    guest-main and nv2a-ack are pinned to a core each
 *                (kernel_thread.c, kernel_bridge.c). Every other thread of
 *                the game's (the present, the audio processor, SDL's audio,
 *                the pipeline pre-warm, the vsync and input threads, the
 *                overlay's loaders) may run on the big cores only, not on
 *                one of them: the cores whose cpu_capacity (or, without it,
 *                cpuinfo_max_freq) is at least 60 % of the largest, less the
 *                two pinned ones when that leaves any. Left on any core the
 *                scheduler kept the audio processor (53 % of a core) on a
 *                little one on a Poco F4. The Java runtime's and the
 *                system's threads in the process are left alone. The
 *                threads are looked over at the first frame and every 5 s.
 *
 * RECOMP_ANDROID_ADPF=0 / RECOMP_ANDROID_BIGCORES=0: off, for A/B.
 */
#if defined(__ANDROID__)

#include <android/api-level.h>
#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGA "[ANDROID-PERF] "
/* RECOMP_ANDROID_ADPF_TARGET_MS=<ms>: another target, for tests. */
static int64_t s_target_ns = 16666667ll;
#define TARGET_NS s_target_ns

static void *libandroid(void)
{
    static void *h;
    static int tried;
    if (!tried) {
        tried = 1;
        h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    }
    return h;
}

static int env_off(const char *name)
{
    const char *e = getenv(name);
    return e && e[0] == '0';
}

/* ── frame rate ─────────────────────────────────────────────────────────── */

typedef int32_t (*set_frame_rate_fn)(ANativeWindow *, float, int8_t);

void xbox_AndroidSetFrameRate(void *window, const char *why)
{
    static set_frame_rate_fn fn;
    static int resolved;
    int api = android_get_device_api_level();
    int32_t r;

    if (!window)
        return;
    {
        /* Off unless asked for: on a 120 Hz Galaxy S20 FE the system kept
         * the panel at 120 Hz but held the app to a 60 Hz grid (a frame
         * rate override for its uid), and a race that could not make 60
         * fell to 26-29 fps (every late frame waited a whole 16.7 ms)
         * where it ran 36-38 without the request. */
        const char *e = getenv("RECOMP_ANDROID_FRAMERATE");
        static int said;
        if (!(e && e[0] == '1')) {
            if (!said++)
                XR_NOTICE(LOGA "frame rate: not asked for (RECOMP_ANDROID_FRAMERATE=1 asks"
                          " for 60 fps)\n");
            return;
        }
    }
    if (!resolved) {
        resolved = 1;
        if (api >= 30 && libandroid())
            fn = (set_frame_rate_fn)dlsym(libandroid(), "ANativeWindow_setFrameRate");
    }
    if (!fn) {
        XR_NOTICE(LOGA "frame rate: ANativeWindow_setFrameRate is not there (API %d)\n",
                api);
        return;
    }
    /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1 */
    r = fn((ANativeWindow *)window, 60.0f, 1);
    XR_NOTICE(LOGA "frame rate: 60 fps, fixed source, asked for on the %s surface"
            " (API %d): %s (%d)\n", why, api, r == 0 ? "ok" : "refused", (int)r);
}

/* ── ADPF performance hint sessions ─────────────────────────────────────── */

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef APerformanceHintManager *(*ph_get_manager_fn)(void);
typedef APerformanceHintSession *(*ph_create_fn)(APerformanceHintManager *, const int32_t *,
                                                 size_t, int64_t);
typedef int64_t (*ph_rate_fn)(APerformanceHintManager *);
typedef int (*ph_report_fn)(APerformanceHintSession *, int64_t);
typedef void (*ph_close_fn)(APerformanceHintSession *);

static struct {
    int state;                       /* 0 not tried, 1 ok, -1 not available */
    APerformanceHintManager *mgr;
    ph_create_fn create;
    ph_report_fn report;
    ph_close_fn close;
} s_ph;

static const char *const k_roles[] = { "guest-main", "nv2a-ack", "sdl-main", "mcpx-apu" };
#define NROLES (int)(sizeof k_roles / sizeof k_roles[0])

struct hot_thread {
    volatile int32_t tid;            /* written last, by the thread itself */
    clockid_t clock;                 /* its CPU-time clock */
    int32_t session_tid;             /* the tid the session was made for */
    APerformanceHintSession *session;
    int64_t last_cpu;                /* its CPU time at the frame before */
    int failed;                      /* a session could not be made */
};
static struct hot_thread s_hot[NROLES];
static int64_t s_last_wall;
static volatile int s_closed;
static uint64_t s_frames;

static int ph_load(void)
{
    int api;
    ph_get_manager_fn get;
    ph_rate_fn rate;

    if (s_ph.state)
        return s_ph.state > 0;
    s_ph.state = -1;
    api = android_get_device_api_level();
    if (env_off("RECOMP_ANDROID_ADPF")) {
        XR_NOTICE(LOGA "ADPF: off (RECOMP_ANDROID_ADPF=0)\n");
        return 0;
    }
    if (api < 33 || !libandroid()) {
        XR_NOTICE(LOGA "ADPF: not on this system (API %d, needs 33)\n", api);
        return 0;
    }
    get = (ph_get_manager_fn)dlsym(libandroid(), "APerformanceHint_getManager");
    rate = (ph_rate_fn)dlsym(libandroid(), "APerformanceHint_getPreferredUpdateRateNanos");
    s_ph.create = (ph_create_fn)dlsym(libandroid(), "APerformanceHint_createSession");
    s_ph.report = (ph_report_fn)dlsym(libandroid(), "APerformanceHint_reportActualWorkDuration");
    s_ph.close = (ph_close_fn)dlsym(libandroid(), "APerformanceHint_closeSession");
    if (!get || !s_ph.create || !s_ph.report || !s_ph.close) {
        XR_NOTICE(LOGA "ADPF: APerformanceHint is missing from libandroid (API %d)\n", api);
        return 0;
    }
    {
        const char *e = getenv("RECOMP_ANDROID_ADPF_TARGET_MS");
        double ms = e ? atof(e) : 0.0;
        if (ms >= 1.0 && ms <= 100.0)
            s_target_ns = (int64_t)(ms * 1e6);
    }
    s_ph.mgr = get();
    if (!s_ph.mgr) {
        XR_NOTICE(LOGA "ADPF: no performance hint manager (API %d): the device"
                " does not support it\n", api);
        return 0;
    }
    XR_NOTICE(LOGA "ADPF: performance hint manager ok (API %d, preferred update"
            " every %.1f ms)\n", api, rate ? (double)rate(s_ph.mgr) / 1e6 : -1.0);
    s_ph.state = 1;
    return 1;
}


/* ── the big cores ──────────────────────────────────────────────────────── */

static long cpu_num(int cpu, const char *what)
{
    char path[128];
    long v = -1;
    FILE *f;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, what);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static int s_big_state;          /* 0 not worked out, 1 a mask, -1 none / off */
static cpu_set_t s_big;
static char s_big_desc[160];

static void mask_desc(const cpu_set_t *m, char *out, size_t n)
{
    int i, first = 1;
    size_t k = 0;
    out[0] = 0;
    for (i = 0; i < CPU_SETSIZE && i < 64 && k + 4 < n; i++)
        if (CPU_ISSET(i, m)) {
            k += (size_t)snprintf(out + k, n - k, "%s%d", first ? "" : ",", i);
            first = 0;
        }
}

static int big_cores(void)
{
    cpu_set_t proc, big, rest;
    long cap[64], top = 0;
    int i, n = 0, pinned[2], use_freq = 0;
    char b[64], r[64];

    if (s_big_state)
        return s_big_state > 0;
    s_big_state = -1;
    if (env_off("RECOMP_ANDROID_BIGCORES")) {
        XR_NOTICE(LOGA "big cores: off (RECOMP_ANDROID_BIGCORES=0)\n");
        return 0;
    }
    if (sched_getaffinity(0, sizeof proc, &proc) != 0)
        return 0;
    for (i = 0; i < 64; i++) {
        cap[i] = -1;
        if (i < CPU_SETSIZE && CPU_ISSET(i, &proc))
            cap[i] = cpu_num(i, "cpu_capacity");
    }
    for (i = 0; i < 64; i++)
        if (i < CPU_SETSIZE && CPU_ISSET(i, &proc) && cap[i] < 0)
            use_freq = 1;
    if (use_freq)
        for (i = 0; i < 64; i++)
            cap[i] = i < CPU_SETSIZE && CPU_ISSET(i, &proc)
                     ? cpu_num(i, "cpufreq/cpuinfo_max_freq") : -1;
    for (i = 0; i < 64; i++)
        if (cap[i] > top)
            top = cap[i];
    if (top <= 0)
        return 0;
    CPU_ZERO(&big);
    for (i = 0; i < 64; i++)
        if (cap[i] >= top * 6 / 10) {
            CPU_SET(i, &big);
            n++;
        }
    /* The two pinned cores (the same choice as kernel_thread.c and
     * kernel_bridge.c make), left out when that leaves any. */
    {
        extern int recomp_android_cpu_rank(int rank);
        pinned[0] = recomp_android_cpu_rank(0);
        pinned[1] = recomp_android_cpu_rank(1);
    }
    rest = big;
    for (i = 0; i < 2; i++)
        if (pinned[i] >= 0 && pinned[i] < CPU_SETSIZE)
            CPU_CLR(pinned[i], &rest);
    mask_desc(&big, b, sizeof b);
    if (CPU_COUNT(&rest) > 0) {
        s_big = rest;
        mask_desc(&rest, r, sizeof r);
        snprintf(s_big_desc, sizeof s_big_desc, "cpus %s (big %s by %s, less the pinned %d and %d)",
                 r, b, use_freq ? "max frequency" : "capacity", pinned[0], pinned[1]);
    } else {
        s_big = big;
        snprintf(s_big_desc, sizeof s_big_desc, "cpus %s (big by %s; the pinned %d and %d"
                 " are all of them)", b, use_freq ? "max frequency" : "capacity",
                 pinned[0], pinned[1]);
    }
    if (n == CPU_COUNT(&proc)) {
        XR_NOTICE(LOGA "big cores: every core is of one kind (%s); threads left where"
                  " they are\n", b);
        return 0;
    }
    s_big_state = 1;
    XR_NOTICE(LOGA "big cores: the game's other threads go on %s\n", s_big_desc);
    return 1;
}

/* Threads of the process that are not the game's: the Java runtime's and
 * the system's. And the two that are pinned. */
static int leave_alone(const char *c)
{
    static const char *const pre[] = {
        "binder:", "HwBinder:", "HeapTaskDaemon", "Jit thread pool", "Signal Catcher",
        "ReferenceQueueD", "FinalizerDaemon", "FinalizerWatchd", "Profile Saver",
        "RenderThread", "hwuiTask", "perfetto", "ADB-JDWP", "AudioTrack", "Binder:",
        "InsetsAnimation", "queued-work", "Thread-JDWP", "CleanupReference", "SDLActivity",
        "guest-main", "guest-w-", "nv2a-ack", "takedawn-exit", "magnifier", "HidlServiceMa",
        "AsyncTask", "ConnectivityThr", "Okio", "android.", "GC", "Runtime worker",
        "mali-", "ged-", "Turnip", "sensor", "UIMonitor", "FileObserver", "OkHttp",
    };
    size_t i;
    for (i = 0; i < sizeof pre / sizeof pre[0]; i++)
        if (!strncmp(c, pre[i], strlen(pre[i])))
            return 1;
    return 0;
}

#define MAX_PLACED 128
static int32_t s_placed[MAX_PLACED];
static int s_nplaced;

static void place_threads(void)
{
    DIR *d;
    struct dirent *e;
    char names[512];
    size_t k = 0;
    int moved = 0, mytid = gettid();

    if (!big_cores())
        return;
    d = opendir("/proc/self/task");
    if (!d)
        return;
    names[0] = 0;
    while ((e = readdir(d)) != NULL) {
        int tid = atoi(e->d_name), i, seen = 0;
        char path[64], comm[32] = "";
        FILE *f;
        if (tid <= 0)
            continue;
        for (i = 0; i < s_nplaced; i++)
            if (s_placed[i] == tid)
                seen = 1;
        if (seen)
            continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(comm, sizeof comm, f))
            comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        /* The process's first thread is the activity's (the Java main). */
        if (tid == getpid() || leave_alone(comm))
            continue;
        {
            /* Pinned to one core already (a guest thread whose name is not
             * set yet): left as it is. */
            cpu_set_t cur;
            if (sched_getaffinity(tid, sizeof cur, &cur) == 0 && CPU_COUNT(&cur) == 1)
                continue;
        }
        if (sched_setaffinity(tid, sizeof s_big, &s_big) != 0)
            continue;
        if (s_nplaced < MAX_PLACED)
            s_placed[s_nplaced++] = tid;
        moved++;
        if (k + strlen(comm) + 16 < sizeof names)
            k += (size_t)snprintf(names + k, sizeof names - k, "%s%s%s", k ? ", " : "",
                                  comm, tid == mytid ? " (this)" : "");
    }
    closedir(d);
    if (moved)
        XR_NOTICE(LOGA "big cores: %d thread%s on %s: %s\n", moved, moved == 1 ? "" : "s",
                  s_big_desc, names);
}

/* From xbox_log_thread_role (kernel_bridge.c), on the thread itself. */
void xbox_AndroidPerfThread(const char *role)
{
    int i;
    clockid_t c;
    for (i = 0; i < NROLES; i++)
        if (!strcmp(role, k_roles[i]))
            break;
    if (i == NROLES)
        return;
    if (pthread_getcpuclockid(pthread_self(), &c) != 0)
        return;
    s_hot[i].clock = c;
    __atomic_store_n(&s_hot[i].tid, (int32_t)gettid(), __ATOMIC_RELEASE);
}

static int64_t clock_ns(clockid_t c, int *ok)
{
    struct timespec t;
    if (clock_gettime(c, &t) != 0) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void close_slot(struct hot_thread *h)
{
    if (h->session)
        s_ph.close(h->session);
    h->session = NULL;
    h->session_tid = 0;
}

/* Once per presented frame, on the present thread (sdl-main). */
void xbox_AndroidPerfFrame(void)
{
    int i, ok;
    int64_t wall, gap;

    if (s_closed)
        return;
    {
        static int64_t placed_at;
        int64_t now = clock_ns(CLOCK_MONOTONIC, &ok);
        if (!placed_at || now - placed_at >= 5000000000ll) {
            placed_at = now;
            place_threads();
        }
    }
    if (!ph_load())
        return;
    wall = clock_ns(CLOCK_MONOTONIC, &ok);
    gap = s_last_wall ? wall - s_last_wall : 0;
    s_last_wall = wall;
    s_frames++;
    for (i = 0; i < NROLES; i++) {
        struct hot_thread *h = &s_hot[i];
        int32_t tid = __atomic_load_n(&h->tid, __ATOMIC_ACQUIRE);
        int64_t cpu;

        if (!tid || h->failed)
            continue;
        if (tid != h->session_tid) {
            close_slot(h);
            h->session = s_ph.create(s_ph.mgr, &tid, 1, TARGET_NS);
            if (!h->session) {
                h->failed = 1;
                XR_NOTICE(LOGA "ADPF: no session for %s (tid %d): the system refused it (no"
                          " ADPF support in this device's power HAL?)\n", k_roles[i], (int)tid);
                continue;
            }
            h->session_tid = tid;
            h->last_cpu = clock_ns(h->clock, &ok);
            XR_NOTICE(LOGA "ADPF: session for %s (tid %d), target %.2f ms: ok\n",
                    k_roles[i], (int)tid, (double)TARGET_NS / 1e6);
            continue;
        }
        cpu = clock_ns(h->clock, &ok);
        if (!ok) {
            /* The thread is gone. */
            XR_NOTICE(LOGA "ADPF: %s (tid %d) ended; its session is closed\n",
                    k_roles[i], (int)tid);
            close_slot(h);
            h->failed = 1;
            continue;
        }
        /* A gap (a pause, the app in the background, a long load) is no
         * frame's work: started over from here. */
        if (gap > 0 && gap < 100000000ll && cpu > h->last_cpu) {
            int64_t busy = cpu - h->last_cpu;
            int r = s_ph.report(h->session, busy);
            if (r != 0) {
                static int said;
                if (!said++)
                    XR_NOTICE(LOGA "ADPF: report for %s refused (%d)\n", k_roles[i], r);
            }
        }
        h->last_cpu = cpu;
    }
    if (s_frames == 600) {
        int n = 0;
        for (i = 0; i < NROLES; i++)
            n += s_hot[i].session != NULL;
        fprintf(stderr, LOGA "ADPF: %d session%s reporting each frame's work (600 frames so"
                " far)\n", n, n == 1 ? "" : "s");
    }
}

/* The way out (fb_present_sdl.c, android_teardown_begin), on sdl-main. */
void xbox_AndroidPerfClose(void)
{
    int i, n = 0;
    if (s_closed)
        return;
    s_closed = 1;
    if (s_ph.state <= 0)
        return;
    for (i = 0; i < NROLES; i++) {
        if (s_hot[i].session)
            n++;
        close_slot(&s_hot[i]);
    }
    XR_NOTICE(LOGA "ADPF: %d session%s closed\n", n, n == 1 ? "" : "s");
}

#else
typedef int xbox_android_perf_unused;
#endif

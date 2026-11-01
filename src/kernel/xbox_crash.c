/*
 * xbox_crash.c - crash and hang reports in <save dir>/crash/<time>-<kind>/.
 * See xbox_crash.h for what a report holds.
 *
 * Sources of the pieces:
 *  - The guest call stack is tools/stackwalk.py done in place: a recompiled
 *    call pushes the guest return address, so the guest stack still has one
 *    for every guest frame. stackwalk.py keeps a word that lands inside a
 *    function body (functions.json bounds); here, with no bounds at hand, a
 *    word counts when it lies in an executable XBE section and the bytes
 *    before it decode as a call (E8 rel32 or FF /2), the test debuggers use
 *    for return addresses without unwind data. The function is the nearest
 *    start below it that recomp_lookup knows.
 *  - The POSIX host backtrace is the BLiNX 2 handler's frame-record walk
 *    (apexJCL/blinx2-recomp src/main.c, MIT) on macOS, where every frame
 *    keeps one; on Linux x86-64 Release code has no frame pointers, so it is
 *    glibc's backtrace(), which unwinds through the signal frame with the
 *    .eh_frame tables.
 *  - The hang watchdog generalises the Windows host's hang_watchdog
 *    (burnout3 main.c, from hl2/src/game/main.c): that one printed every
 *    thread's PC every N seconds; this one waits for the flips to stop and
 *    writes a report.
 */
#include "xbox_crash.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_env.h"
#include "platform/recomp_restart.h"   /* recomp_restart_stdio */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <execinfo.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
#endif
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi, g_ebp;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;
extern ptrdiff_t g_xbox_mem_offset;

typedef void (*crash_func_t)(void);
crash_func_t recomp_lookup(uint32_t xbox_va);   /* the title's dispatch table */

volatile uint32_t g_xbox_flip_count;
volatile int g_xbox_crash_test;

/* ── small I/O, the same on every host ─────────────────────────────── */

#if defined(_WIN32)
static int cr_write(int fd, const char *b, size_t n) { return _write(fd, b, (unsigned)n); }
static int cr_open_new(const char *p)
{
    return _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
static int cr_open_read(const char *p) { return _open(p, _O_RDONLY | _O_BINARY); }
static int cr_mkdir(const char *p) { return _mkdir(p); }
#define cr_close _close
#define cr_read(fd, b, n) _read(fd, b, (unsigned)(n))
#define cr_lseek_end(fd, off) _lseeki64(fd, -(__int64)(off), SEEK_END)
#define cr_lseek_start(fd) _lseeki64(fd, 0, SEEK_SET)
#else
static int cr_write(int fd, const char *b, size_t n) { return (int)write(fd, b, n); }
static int cr_open_new(const char *p)
{
    return open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}
static int cr_open_read(const char *p) { return open(p, O_RDONLY | O_CLOEXEC); }
static int cr_mkdir(const char *p) { return mkdir(p, 0755); }
#define cr_close close
#define cr_read(fd, b, n) read(fd, b, n)
#define cr_lseek_end(fd, off) lseek(fd, -(off_t)(off), SEEK_END)
#define cr_lseek_start(fd) lseek(fd, 0, SEEK_SET)
#endif

static void write_all(int fd, const char *b, size_t n)
{
    while (fd >= 0 && n) {
        int r = cr_write(fd, b, n);
        if (r <= 0) {
#if !defined(_WIN32)
            if (r < 0 && errno == EINTR)
                continue;
#endif
            return;
        }
        b += r;
        n -= (size_t)r;
    }
}

static void write_str(int fd, const char *s) { write_all(fd, s, strlen(s)); }

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
#endif
}

static uint64_t now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* mkdir -p, for the root at init (not in a handler). */
static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);

    if (!n || n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            if (i == 2 && tmp[1] == ':')
                continue;       /* C:\ */
            tmp[i] = '\0';
            cr_mkdir(tmp);
            tmp[i] = c;
        }
    }
    cr_mkdir(tmp);
}

/* ── the log ring and the tee that fills it ────────────────────────── */

#define RING_SIZE  (256u * 1024u)
#define LOG_LINES  500

static char s_ring[RING_SIZE];
static volatile uint64_t s_ring_head;    /* bytes ever appended */
static volatile int s_tee_on;
static int s_err_fd = 2;                 /* the original stderr */

#if defined(_WIN32)
static CRITICAL_SECTION s_ring_lock;
#define RING_LOCK()   EnterCriticalSection(&s_ring_lock)
#define RING_UNLOCK() LeaveCriticalSection(&s_ring_lock)
#else
static pthread_mutex_t s_ring_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING_LOCK()   pthread_mutex_lock(&s_ring_lock)
#define RING_UNLOCK() pthread_mutex_unlock(&s_ring_lock)
#endif

static void ring_append(const char *b, size_t n)
{
    RING_LOCK();
    {
        uint64_t h = s_ring_head;
        if (n > RING_SIZE) {
            b += n - RING_SIZE;
            h += n - RING_SIZE;
            n = RING_SIZE;
        }
        while (n) {
            size_t at = (size_t)(h % RING_SIZE);
            size_t k = RING_SIZE - at < n ? RING_SIZE - at : n;
            memcpy(s_ring + at, b, k);
            b += k;
            n -= k;
            h += k;
        }
        s_ring_head = h;
    }
    RING_UNLOCK();
}

/* ── the run log: what the tee saw, kept on disk for the bug report ──
 *
 * <save dir>/logs/run.log is the first RUNLOG_HEAD bytes of this run's
 * output (the start: the device, the display, the settings), run.tail.log
 * what came after, moved to run.tail.old.log whenever it passes
 * RUNLOG_TAIL_ROTATE, so a run of any length leaves at most
 * RUNLOG_HEAD + 2 * RUNLOG_TAIL_ROTATE bytes. Written as the output comes,
 * so it is there after a crash, a hang or a kill. At start the files of the
 * last three runs move on to prev1.*, prev2.*, prev3.* (xbox_report.c reads
 * them by these names). */
#define RUNLOG_HEAD         (64u * 1024u)
#define RUNLOG_TAIL_ROTATE  (384u * 1024u)
#define RUNLOG_PREV         3

static int s_runlog_want = 1;
static int s_head_fd = -1, s_tail_fd = -1;
static size_t s_head_left, s_tail_bytes;
static char s_log_dir[1100];

static void runlog_path(char *out, size_t n, int gen, const char *suffix)
{
    if (gen)
        snprintf(out, n, "%s/prev%d%s", s_log_dir, gen, suffix);
    else
        snprintf(out, n, "%s/run%s", s_log_dir, suffix);
}

/* Under the ring lock. */
static void runlog_write(const char *b, size_t n)
{
    char path[1200], old[1200];

    if (s_head_fd >= 0 && s_head_left) {
        size_t k = n < s_head_left ? n : s_head_left;
        write_all(s_head_fd, b, k);
        s_head_left -= k;
        b += k;
        n -= k;
        if (!s_head_left) {
            cr_close(s_head_fd);
            s_head_fd = -1;
        }
    }
    if (!n || s_head_left)
        return;
    runlog_path(path, sizeof path, 0, ".tail.log");
    if (s_tail_fd >= 0 && s_tail_bytes + n > RUNLOG_TAIL_ROTATE) {
        cr_close(s_tail_fd);
        s_tail_fd = -1;
        runlog_path(old, sizeof old, 0, ".tail.old.log");
        remove(old);
        rename(path, old);
    }
    if (s_tail_fd < 0) {
        s_tail_fd = cr_open_new(path);
        s_tail_bytes = 0;
        if (s_tail_fd < 0) {
            s_head_left = (size_t)-1;   /* the folder is gone: stop trying */
            return;
        }
    }
    write_all(s_tail_fd, b, n);
    s_tail_bytes += n;
}

typedef struct {
    int rd;                   /* the pipe's read end */
    int out;                  /* where the stream went before (-1: nowhere) */
    volatile int busy;        /* between a read and its ring append */
#if !defined(_WIN32)
    /* The disk side (BO-245): the pump only reads the pipe into the ring and
     * into this queue; a thread of its own writes the queue to `out` and the
     * run log. Every thread's stderr goes into the pipe, and a pipe that
     * fills blocks its writer: when the disk is slow (the Deck's SD card,
     * busy with the texture upscaler's cache files) the render thread's own
     * log lines waited for it. The pipe is now drained at memory speed; a
     * full queue drops bytes (counted, said in the log) rather than wait.
     * Windows keeps the direct write. */
    char *q;
    size_t qh, qn;            /* the writer's start, bytes waiting */
    volatile int qbusy;       /* the writer has bytes out of the queue */
    unsigned long long lost;  /* bytes dropped, not yet said */
    pthread_mutex_t qmx;
    pthread_cond_t qcv;
#endif
} TeeChan;

static TeeChan s_tee[2];
static int s_ntee;

#if !defined(_WIN32)
#define TEE_Q_BYTES (2u * 1024u * 1024u)

/* Pump thread: 1 when the bytes are the writer's now, 0 to write them here. */
static int tee_q_put(TeeChan *c, const char *b, size_t n)
{
    size_t at, first;
    if (!c->q)
        return 0;
    pthread_mutex_lock(&c->qmx);
    if (c->qn + n > TEE_Q_BYTES) {
        c->lost += n;
        pthread_mutex_unlock(&c->qmx);
        return 1;
    }
    at = (c->qh + c->qn) % TEE_Q_BYTES;
    first = n < TEE_Q_BYTES - at ? n : TEE_Q_BYTES - at;
    memcpy(c->q + at, b, first);
    memcpy(c->q, b + first, n - first);
    c->qn += n;
    pthread_cond_signal(&c->qcv);
    pthread_mutex_unlock(&c->qmx);
    return 1;
}

static void tee_disk_write(TeeChan *c, const char *b, size_t n)
{
    write_all(c->out, b, n);
    if (s_log_dir[0]) {
        RING_LOCK();
        runlog_write(b, n);
        RING_UNLOCK();
    }
}

static void *tee_q_thread(void *arg)
{
    TeeChan *c = (TeeChan *)arg;
    pthread_mutex_lock(&c->qmx);
    for (;;) {
        size_t n;
        unsigned long long lost;
        while (!c->qn && !c->lost)
            pthread_cond_wait(&c->qcv, &c->qmx);
        lost = c->lost;
        c->lost = 0;
        n = c->qn < TEE_Q_BYTES - c->qh ? c->qn : TEE_Q_BYTES - c->qh;
        c->qbusy = 1;
        pthread_mutex_unlock(&c->qmx);
        /* The bytes [qh, qh + n) are this thread's until it moves qh. */
        if (n)
            tee_disk_write(c, c->q + c->qh, n);
        if (lost) {
            char line[160];
            int k = snprintf(line, sizeof line, "[LOG] %llu bytes of the log dropped:"
                             " the disk was too slow to take them\n", lost);
            if (k > 0 && (size_t)k < sizeof line)
                tee_disk_write(c, line, (size_t)k);
        }
        pthread_mutex_lock(&c->qmx);
        c->qh = (c->qh + n) % TEE_Q_BYTES;
        c->qn -= n;
        c->qbusy = 0;
    }
    return NULL;
}

static void tee_q_start(TeeChan *c)
{
    pthread_t th;
    pthread_mutex_init(&c->qmx, NULL);
    pthread_cond_init(&c->qcv, NULL);
    c->q = (char *)malloc(TEE_Q_BYTES);
    if (!c->q)
        return;
    if (pthread_create(&th, NULL, tee_q_thread, c) != 0) {
        free(c->q);
        c->q = NULL;
        return;
    }
    pthread_detach(th);
}

static int tee_q_idle(TeeChan *c)
{
    return !c->q || (!c->qbusy && !c->qn);
}
#endif

static void tee_pump(TeeChan *c)
{
    char buf[4096];
    for (;;) {
        int n;
#if defined(_WIN32)
        /* Wait for data by polling, never inside a read: PeekNamedPipe on a
         * synchronous pipe handle blocks for as long as another thread sits
         * in ReadFile/_read on it, and tee_pending() (the exit-time flush)
         * peeks from another thread. With the read parked on a quiet pipe
         * the flush, and so exit(), never returned: closing the window left
         * the game running (Windows Vulkan build, BO-75). */
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe((HANDLE)_get_osfhandle(c->rd), NULL, 0, NULL, &avail, NULL))
                return;   /* the write end is gone */
            if (avail)
                break;
            Sleep(1);
        }
        c->busy = 1;   /* before the bytes leave the pipe: the flush must see them */
        n = (int)cr_read(c->rd, buf, sizeof buf);
#else
        n = (int)cr_read(c->rd, buf, sizeof buf);
#endif
        if (n <= 0) {
#if !defined(_WIN32)
            if (n < 0 && errno == EINTR)
                continue;
#else
            c->busy = 0;
#endif
            break;
        }
#if !defined(_WIN32)
        c->busy = 1;
        ring_append(buf, (size_t)n);
        if (!tee_q_put(c, buf, (size_t)n))
            tee_disk_write(c, buf, (size_t)n);
#else
        write_all(c->out, buf, (size_t)n);
        ring_append(buf, (size_t)n);
        if (s_log_dir[0]) {
            RING_LOCK();
            runlog_write(buf, (size_t)n);
            RING_UNLOCK();
        }
#endif
        c->busy = 0;
    }
}

#if defined(_WIN32)
static DWORD WINAPI tee_thread(LPVOID arg) { tee_pump((TeeChan *)arg); return 0; }
#else
static void *tee_thread(void *arg) { tee_pump((TeeChan *)arg); return NULL; }
#endif

/* Bytes waiting in a channel's pipe. */
static long tee_pending(const TeeChan *c)
{
#if defined(_WIN32)
    DWORD avail = 0;
    if (!PeekNamedPipe((HANDLE)_get_osfhandle(c->rd), NULL, 0, NULL, &avail, NULL))
        return 0;
    return (long)avail;
#else
    int avail = 0;
    if (ioctl(c->rd, FIONREAD, &avail) != 0)
        return 0;
    return avail;
#endif
}

void xbox_crash_log_flush(unsigned max_ms)
{
    unsigned waited = 0;
    int i;

    if (!s_tee_on)
        return;
    for (;;) {
        int idle = 1;
        for (i = 0; i < s_ntee; i++) {
            if (s_tee[i].busy || tee_pending(&s_tee[i]) > 0)
                idle = 0;
#if !defined(_WIN32)
            if (!tee_q_idle(&s_tee[i]))
                idle = 0;
#endif
        }
        if (idle || waited >= max_ms)
            return;
        sleep_ms(1);
        waited++;
    }
}

static void tee_atexit(void) { xbox_crash_log_flush(500); }

/* A sanitizer report goes to stderr, now the tee, and then the process
 * ends from inside the runtime: pass the report on before it does. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define CRASH_HAVE_SANITIZER 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define CRASH_HAVE_SANITIZER 1
#endif
#if defined(CRASH_HAVE_SANITIZER)
void __sanitizer_set_death_callback(void (*callback)(void));
#endif

/* Point fd (the stream's own descriptor; std = 1 stdout, 2 stderr) at a
 * new pipe whose old destination becomes c->out. */
static int tee_redirect(int fd, int std, TeeChan *c)
{
#if defined(_WIN32)
    int p[2];
    if (_pipe(p, 65536, _O_BINARY | _O_NOINHERIT) != 0)
        return 0;
    c->out = _dup(fd);
    if (_dup2(p[1], fd) != 0) {
        _close(p[0]); _close(p[1]);
        return 0;
    }
    _close(p[1]);
    c->rd = p[0];
    SetStdHandle(std == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,
                 (HANDLE)_get_osfhandle(fd));
    return 1;
#else
    int p[2];
    if (pipe(p) != 0)
        return 0;
    c->out = dup(fd);
    if (c->out >= 0)
        fcntl(c->out, F_SETFD, FD_CLOEXEC);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    if (dup2(p[1], fd) < 0) {
        close(p[0]); close(p[1]);
        return 0;
    }
    close(p[1]);
    c->rd = p[0];
    return 1;
#endif
}

static void tee_start(void)
{
    static int started;

    if (started)
        return;
    started = 1;
#if defined(_WIN32)
    /* A window program started with no console and no redirection (from
     * Explorer, or by a restart, which passes no handles on) has no stderr:
     * the run log would stay empty. The streams go to NUL, and the tee has
     * a descriptor to put its pipe on. */
    if (_fileno(stderr) < 0 || _get_osfhandle(_fileno(stderr)) == -1) {
        if (!freopen("NUL", "w", stderr))
            return;
    }
    if (_fileno(stdout) < 0 || _get_osfhandle(_fileno(stdout)) == -1)
        freopen("NUL", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    InitializeCriticalSection(&s_ring_lock);
#endif
    fflush(stdout);
    fflush(stderr);
    {
        /* One pipe when stdout and stderr go to the same place (a log file,
         * 2>&1), so their order survives; two otherwise. */
        int same = 0;
#if !defined(_WIN32)
        struct stat a, b;
        same = fstat(1, &a) == 0 && fstat(2, &b) == 0
            && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
        int efd = fileno(stderr), ofd = fileno(stdout);
        if (!tee_redirect(efd, 2, &s_tee[0]))
            return;
        s_err_fd = s_tee[0].out;
        s_ntee = 1;
        if (same) {
#if defined(_WIN32)
            _dup2(efd, ofd);
#else
            dup2(efd, ofd);
#endif
        } else if (ofd >= 0 && tee_redirect(ofd, 1, &s_tee[1])) {
            s_ntee = 2;
        }
#if !defined(_WIN32)
        /* A restart in place gives the new run these, not the pipes. */
        recomp_restart_stdio(same ? s_tee[0].out : s_ntee == 2 ? s_tee[1].out : -1,
                             s_tee[0].out);
#endif
    }
    {
        int i;
        for (i = 0; i < s_ntee; i++) {
#if defined(_WIN32)
            HANDLE h = CreateThread(NULL, 0, tee_thread, &s_tee[i], 0, NULL);
            if (h)
                CloseHandle(h);
#else
            pthread_t th;
            tee_q_start(&s_tee[i]);
            if (pthread_create(&th, NULL, tee_thread, &s_tee[i]) == 0)
                pthread_detach(th);
#endif
        }
    }
    s_tee_on = 1;
    atexit(tee_atexit);
#if defined(CRASH_HAVE_SANITIZER)
    __sanitizer_set_death_callback(tee_atexit);
#endif
}

/* ── the report folder ─────────────────────────────────────────────── */

static char s_root[1024] = "crash";
static char s_dir[1100];
static int s_rep_fd = -1;
static volatile long s_report_owner;
static char s_settings[16384];
static const char *s_stdio_log;   /* RECOMP_STDIO_LOG, the fallback for log.txt */
static long s_tz_off;             /* seconds east of UTC, at init */

const char *xbox_crash_root(void) { return s_root; }

void xbox_crash_log_start(void) { tee_start(); }
void xbox_crash_set_run_log(int on) { s_runlog_want = on; }
const char *xbox_crash_log_dir(void) { return s_log_dir; }

/* The last runs' files move on one place, then this run's are opened and
 * given what the tee has seen so far. */
static void runlog_open(const char *save_dir)
{
    static const char *const sfx[] = { ".log", ".tail.log", ".tail.old.log" };
    char dir[1100], a[1200], b[1200];
    int g, k;

    if (!s_runlog_want || !s_tee_on)
        return;
    snprintf(dir, sizeof dir, "%s/logs", save_dir && *save_dir ? save_dir : ".");
    mkdir_p(dir);
    xbox_crash_log_flush(100);
    RING_LOCK();
    snprintf(s_log_dir, sizeof s_log_dir, "%s", dir);
    for (g = RUNLOG_PREV; g >= 1; g--)
        for (k = 0; k < 3; k++) {
            runlog_path(b, sizeof b, g, sfx[k]);
            runlog_path(a, sizeof a, g - 1, sfx[k]);
            remove(b);
            rename(a, b);
        }
    runlog_path(a, sizeof a, 0, ".log");
    s_head_fd = cr_open_new(a);
    if (s_head_fd < 0) {
        s_log_dir[0] = 0;
    } else {
        uint64_t head = s_ring_head;
        size_t have = head < RING_SIZE ? (size_t)head : RING_SIZE;
        uint64_t from = head - have;
        s_head_left = RUNLOG_HEAD;
        while (have) {
            size_t at = (size_t)(from % RING_SIZE);
            size_t n = RING_SIZE - at < have ? RING_SIZE - at : have;
            runlog_write(s_ring + at, n);
            from += n;
            have -= n;
        }
    }
    RING_UNLOCK();
}

void xbox_crash_init(const char *save_dir)
{
    static int done;

    if (done)
        return;
    done = 1;
    if (save_dir && *save_dir)
        snprintf(s_root, sizeof s_root, "%s/crash", save_dir);
    mkdir_p(s_root);
    s_stdio_log = recomp_env(RENV_STDIO_LOG);
#if !defined(_WIN32)
    {
        time_t t = time(NULL);
        struct tm lt;
        if (localtime_r(&t, &lt))
            s_tz_off = (long)lt.tm_gmtoff;
    }
#if defined(__linux__)
    {
        /* backtrace() loads libgcc on its first call; not in a handler. */
        void *pc[2];
        backtrace(pc, 2);
    }
#endif
#endif
    tee_start();
    runlog_open(save_dir);
}

void xbox_crash_set_settings(const char *text)
{
    snprintf(s_settings, sizeof s_settings, "%s", text ? text : "");
}

/* "20261010-142305", local time, without localtime (not signal-safe). */
static void stamp(char *out, size_t n)
{
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, n, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
#else
    long long t = (long long)time(NULL) + s_tz_off;
    long long days = t / 86400, sec = t % 86400;
    /* days to civil, Howard Hinnant's algorithm */
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(out, n, "%04lld%02u%02u-%02lld%02lld%02lld", y, m, d,
             sec / 3600, (sec / 60) % 60, sec % 60);
#endif
}

/* The last LOG_LINES lines of buf[head - avail, head) in a ring of `size`. */
static void write_tail(int fd, const char *ring, size_t size, uint64_t head, size_t avail)
{
    size_t i, lines = 0, take = avail;
    uint64_t from;

    for (i = 1; i <= avail; i++) {
        if (ring[(size_t)((head - i) % size)] == '\n' && i > 1 && ++lines >= LOG_LINES) {
            take = i - 1;
            break;
        }
    }
    from = head - take;
    while (take) {
        size_t at = (size_t)(from % size);
        size_t k = size - at < take ? size - at : take;
        write_all(fd, ring + at, k);
        from += k;
        take -= k;
    }
}

static void write_log(const char *dir)
{
    char path[1200];
    int fd;

    snprintf(path, sizeof path, "%s/log.txt", dir);
    fd = cr_open_new(path);
    if (fd < 0)
        return;
    if (s_tee_on) {
        uint64_t head = s_ring_head;
        write_tail(fd, s_ring, RING_SIZE, head, head < RING_SIZE ? (size_t)head : RING_SIZE);
    } else if (s_stdio_log && *s_stdio_log) {
        /* No tee (a Windows GUI run): the end of the RECOMP_STDIO_LOG file.
         * The ring is unused then, so it holds the read. */
        int in = cr_open_read(s_stdio_log);
        if (in >= 0) {
            long long got = 0;
            int r;
            if (cr_lseek_end(in, RING_SIZE) < 0)
                cr_lseek_start(in);     /* shorter than the ring: all of it */
            while (got < (long long)RING_SIZE
                   && (r = (int)cr_read(in, s_ring + got, RING_SIZE - (size_t)got)) > 0)
                got += r;
            cr_close(in);
            write_tail(fd, s_ring, RING_SIZE, (uint64_t)got, (size_t)got);
        }
    } else {
        write_str(fd, "(no log: stdout/stderr were not captured)\n");
    }
    cr_close(fd);
}

#if defined(_WIN32)
#define cr_environ _environ
#else
extern char **environ;
#define cr_environ environ
#endif

static void write_settings(const char *dir)
{
    char path[1200], line[1024];
    int fd, i;
    char **e;

    snprintf(path, sizeof path, "%s/settings.txt", dir);
    fd = cr_open_new(path);
    if (fd < 0)
        return;
    write_str(fd, s_settings);
    write_str(fd, "\n[runtime table: every value set, after enhance.toml]\n");
    for (i = 0; i < RENV_COUNT; i++) {
        const char *v = recomp_env_value_[i];
        const char *list = recomp_env_list_var((recomp_env_id)i);
        if (!v)
            continue;
        if (list)
            snprintf(line, sizeof line, "%s: %s=%s\n", list, recomp_env_name((recomp_env_id)i), v);
        else
            snprintf(line, sizeof line, "%s=%s\n", recomp_env_name((recomp_env_id)i), v);
        write_str(fd, line);
    }
    write_str(fd, "\n[environment: RECOMP_ BO3_ XBOX_ SDL_ ASAN_ UBSAN_]\n");
    for (e = cr_environ; e && *e; e++) {
        static const char *const pre[] = { "RECOMP_", "BO3_", "XBOX_", "SDL_",
                                           "ASAN_", "UBSAN_" };
        int k;
        for (k = 0; k < (int)(sizeof pre / sizeof pre[0]); k++)
            if (!strncmp(*e, pre[k], strlen(pre[k]))) {
                write_str(fd, *e);
                write_str(fd, "\n");
                break;
            }
    }
    cr_close(fd);
}

static int claim_report(void)
{
#if defined(_MSC_VER)
    return InterlockedCompareExchange(&s_report_owner, 1, 0) == 0;
#else
    long expect = 0;
    return __atomic_compare_exchange_n(&s_report_owner, &expect, 1, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

int xbox_crash_begin(const char *kind, const char *headline)
{
    g_xr_log_diag = 1;      /* a crash or a hang: everything from here on is said (xr_log.h) */
    char ts[32], path[1200];
    int k;

    if (!claim_report()) {
        if (headline) {
            write_str(s_err_fd, headline);
            write_str(s_err_fd, "\n");
        }
        return 0;
    }
    xbox_crash_log_flush(200);
    stamp(ts, sizeof ts);
    snprintf(s_dir, sizeof s_dir, "%s/%s-%s", s_root, ts, kind);
    for (k = 2; cr_mkdir(s_dir) != 0 && k < 10; k++)   /* two in one second */
        snprintf(s_dir, sizeof s_dir, "%s/%s-%s-%d", s_root, ts, kind, k);
    write_log(s_dir);
    write_settings(s_dir);
    snprintf(path, sizeof path, "%s/report.txt", s_dir);
    s_rep_fd = cr_open_new(path);
    if (headline)
        xbox_crash_printf("%s\n", headline);
    return 1;
}

void xbox_crash_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    write_all(s_rep_fd, buf, (size_t)n);
    write_all(s_err_fd, buf, (size_t)n);
}

const char *xbox_crash_end(void)
{
    if (s_rep_fd >= 0) {
        xbox_crash_printf("[CRASH] report: %s\n", s_dir);
        cr_close(s_rep_fd);
        s_rep_fd = -1;
    }
    s_report_owner = 0;
    return s_dir;
}

/* ── the guest side ────────────────────────────────────────────────── */

static int guest_readable(uint32_t va, uint32_t n)
{
    return g_xbox_mem_offset && va >= XBOX_BASE_ADDRESS
        && (uint64_t)va + n <= (uint64_t)XBOX_BASE_ADDRESS + xbox_GetMappedSize();
}

static const uint8_t *guest_ptr(uint32_t va)
{
    return (const uint8_t *)((uintptr_t)g_xbox_mem_offset + va);
}

/* In an executable section of the XBE (section headers as kernel_frametime.c
 * reads them: count at 0x1011C, headers at 0x10120, 0x38 bytes each). */
static int in_code(uint32_t va)
{
    uint32_t n, hdr, i;

    if (!guest_readable(0x10120, 4))
        return 0;
    n = *(const uint32_t *)guest_ptr(0x1011C);
    hdr = *(const uint32_t *)guest_ptr(0x10120);
    if (n > 64 || !guest_readable(hdr, n * 0x38u))
        return 0;
    for (i = 0; i < n; i++) {
        const uint8_t *h = guest_ptr(hdr + i * 0x38u);
        uint32_t flags = *(const uint32_t *)h;
        uint32_t lo = *(const uint32_t *)(h + 4), sz = *(const uint32_t *)(h + 8);
        if ((flags & 4) && va >= lo && va < lo + sz)
            return 1;
    }
    return 0;
}

/* The bytes before `ret` are a call: E8 rel32, or FF /2 in each length. */
static int after_call(uint32_t ret)
{
    const uint8_t *p;
    int len;

    if (!guest_readable(ret - 7, 7))
        return 0;
    p = guest_ptr(ret);
    if (p[-5] == 0xE8)
        return 1;
    for (len = 2; len <= 7; len++) {
        uint8_t modrm = p[-len + 1];
        int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        int want;
        if (p[-len] != 0xFF || reg != 2)
            continue;
        if (mod == 3)
            want = 2;
        else if (mod == 0)
            want = rm == 4 ? 3 : rm == 5 ? 6 : 2;
        else if (mod == 1)
            want = rm == 4 ? 4 : 3;
        else
            want = rm == 4 ? 7 : 6;
        if (want == len)
            return 1;
    }
    return 0;
}

/* The translated function a guest code address belongs to. */
static uint32_t func_start(uint32_t va)
{
    uint32_t a, k;
    for (a = va - 1, k = 0; k < 0x10000 && a >= XBOX_BASE_ADDRESS; a--, k++)
        if (recomp_lookup(a))
            return a;
    return 0;
}

static void guest_walk(uint32_t esp, int max_frames, int raw_words)
{
    uint32_t i, slot;
    int depth = 0;

    if (!guest_readable(esp, 4)) {
        xbox_crash_printf("  guest esp %08X is not in guest RAM\n", esp);
        return;
    }
    xbox_crash_printf("  guest call stack from esp %08X (tools/stackwalk.py format):\n", esp);
    for (i = 0; i < 4096 && depth < max_frames; i++) {
        uint32_t w;
        slot = esp + i * 4;
        if (!guest_readable(slot, 4))
            break;
        w = *(const uint32_t *)guest_ptr(slot);
        if (w > XBOX_BASE_ADDRESS && in_code(w) && after_call(w)) {
            uint32_t f = func_start(w);
            depth++;
            if (f)
                xbox_crash_printf("  #%-3d %08X -> %08X  sub_%08X+0x%X\n", depth, slot, w,
                                  f, w - f);
            else
                xbox_crash_printf("  #%-3d %08X -> %08X  ?\n", depth, slot, w);
        }
    }
    if (!depth)
        xbox_crash_printf("  no guest return addresses on the stack\n");
    for (i = 0; i < (uint32_t)raw_words; i++) {
        slot = esp + i * 4;
        if (!guest_readable(slot, 4))
            break;
        xbox_crash_printf("    GS %08X %08X\n", slot, *(const uint32_t *)guest_ptr(slot));
    }
}

void xbox_crash_guest_stack(uint32_t esp)
{
    guest_walk(esp, 40, 256);
}

static void print_regs(const uint32_t r[9])
{
    xbox_crash_printf("  guest regs: eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                      "              esp=%08X ebp=%08X esi=%08X edi=%08X seh_ebp=%08X\n",
                      r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
}

/* ── hang only: guest memory ───────────────────────────────────────
 *
 * A hang report names where a thread is, not what it is looking at: the
 * BO-173 freeze sat in a DirectSound DPC walking a list, and the report
 * could not show the list. So a hang report also keeps guest RAM and a
 * window of memory around every register that points into it. Hang only:
 * a crash may have the memory itself in question, and the watchdog's
 * thread is not a fault handler, so it can take its time. */

/* The guest RAM address a register value stands for: RAM and its mirrors
 * (the contiguous 0x80000000 view among them) wrap at the mapped size. */
static int guest_ram_addr(uint32_t v, uint32_t *pa)
{
    uint64_t size = xbox_GetMappedSize();
    if (!g_xbox_mem_offset || !size || v >= 0xF0000000u)
        return 0;                       /* MMIO, the BIOS, the kernel's */
    if ((size & (size - 1)) == 0)
        v &= (uint32_t)(size - 1);
    else if (v >= size)
        return 0;
    if (v < XBOX_BASE_ADDRESS)
        return 0;                       /* small numbers, not pointers */
    *pa = v;
    return 1;
}

/* 256 bytes around each register that points into guest RAM, 16 a line. */
static void reg_windows(const uint32_t r[9])
{
    static const char *const names[9] = { "eax", "ecx", "edx", "ebx", "esp",
                                          "ebp", "esi", "edi", "seh_ebp" };
    uint64_t size = xbox_GetMappedSize();
    int k;

    for (k = 0; k < 9; k++) {
        uint32_t pa, lo, a;
        int j;
        if (!r[k] || !guest_ram_addr(r[k], &pa))
            continue;
        lo = (pa & ~15u) >= 0x80 ? (pa & ~15u) - 0x80 : 0;
        if ((uint64_t)lo + 256 > size)
            lo = (uint32_t)(size - 256);
        xbox_crash_printf("  mem %s=%08X (ram %08X):\n", names[k], r[k], pa);
        for (a = lo; a < lo + 256; a += 16) {
            const uint8_t *b = guest_ptr(a);
            char line[96];
            int n = snprintf(line, sizeof line, "    %08X", a);
            for (j = 0; j < 16; j += 4)
                n += snprintf(line + n, sizeof line - (size_t)n, " %08X",
                              *(const uint32_t *)(b + j));
            xbox_crash_printf("%s%s\n", line, a == (pa & ~15u) ? "  <" : "");
        }
    }
}

/* guestram.bin: guest RAM from address 0, raw, as many bytes as are mapped
 * (64 MB on a retail layout). write() fails rather than faults on a page
 * that cannot be read; such a block is written as zeros. */
static void dump_guest_ram(void)
{
    static const char zeros[65536];
    char path[1200];
    uint64_t size = xbox_GetMappedSize(), off;
    unsigned bad = 0;
    int fd;

    if (!g_xbox_mem_offset || !size || !s_dir[0])
        return;
    snprintf(path, sizeof path, "%s/guestram.bin", s_dir);
    fd = cr_open_new(path);
    if (fd < 0) {
        xbox_crash_printf("  guestram.bin: cannot be written\n");
        return;
    }
    for (off = 0; off < size; off += sizeof zeros) {
        size_t n = (size_t)(size - off < sizeof zeros ? size - off : sizeof zeros);
        const char *p = (const char *)guest_ptr((uint32_t)off);
        size_t done = 0;
        while (done < n) {
            int w = cr_write(fd, p + done, n - done);
            if (w <= 0) {
#if !defined(_WIN32)
                if (w < 0 && errno == EINTR)
                    continue;
#endif
                break;
            }
            done += (size_t)w;
        }
        if (done < n) {
            bad++;
            write_all(fd, zeros, n - done);
        }
    }
    cr_close(fd);
    xbox_crash_printf("  guestram.bin: guest RAM 0x00000000-0x%08llX, raw%s"
                      " (tools/dsound_done_list.py reads it)\n",
                      (unsigned long long)size - 1,
                      bad ? ", unreadable blocks as zeros" : "");
}

static int s_hang_mem;   /* report_threads: the register windows too */

static xbox_crash_note_fn s_notes[8];
static volatile LONG s_n_notes;

void xbox_crash_add_note(xbox_crash_note_fn fn)
{
    LONG i = InterlockedIncrement(&s_n_notes) - 1;
    if (i < 8)
        s_notes[i] = fn;
}

static void print_notes(void)
{
    LONG i, n = s_n_notes < 8 ? s_n_notes : 8;
    for (i = 0; i < n; i++)
        if (s_notes[i])
            s_notes[i]();
}

static void print_icalls(void)
{
    uint32_t k;
    xbox_crash_printf("  indirect calls so far: %llu; recent targets:",
                      (unsigned long long)g_icall_count);
    for (k = 0; k < 16; k++)
        xbox_crash_printf(" %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
    xbox_crash_printf("\n");
}

void xbox_crash_guest_state(void)
{
    uint32_t r[9] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_seh_ebp };
    print_regs(r);
    print_icalls();
    if (g_esp)
        xbox_crash_guest_stack(g_esp);
}

/* ── host symbols and backtraces ───────────────────────────────────── */

void xbox_crash_host_symbol(const char *label, uintptr_t pc)
{
#if defined(_WIN32)
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod) && mod) {
        char name[MAX_PATH], *base;
        DWORD n = GetModuleFileNameA(mod, name, sizeof name);
        base = n ? strrchr(name, '\\') : NULL;
        xbox_crash_printf("  %s 0x%llX at %s+0x%llX\n", label, (unsigned long long)pc,
                          base ? base + 1 : "?", (unsigned long long)(pc - (uintptr_t)mod));
    } else {
        xbox_crash_printf("  %s 0x%llX\n", label, (unsigned long long)pc);
    }
#else
    Dl_info info;
    int found;
    memset(&info, 0, sizeof info);
    found = dladdr((void *)pc, &info);
    if (found && info.dli_sname && info.dli_saddr)
        xbox_crash_printf("  %s 0x%llX in %s+0x%lX\n", label, (unsigned long long)pc,
                          info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
    else if (found && info.dli_fname && info.dli_fbase) {
        const char *base = strrchr(info.dli_fname, '/');
        xbox_crash_printf("  %s 0x%llX at %s+0x%lX\n", label, (unsigned long long)pc,
                          base ? base + 1 : info.dli_fname,
                          (unsigned long)(pc - (uintptr_t)info.dli_fbase));
    } else
        xbox_crash_printf("  %s 0x%llX\n", label, (unsigned long long)pc);
#endif
}

/* ── the thread registry ───────────────────────────────────────────── */

#define MAX_THREADS 64
#define MAX_FRAMES  24

typedef struct {
    volatile int used;
    char role[24];
    unsigned long tid;
    /* The thread's own guest registers (thread-locals have a fixed address
     * for the thread's life), read by the watchdog as xbox_WatchdogStart
     * reads guest-main's. */
    volatile uint32_t *regs[9];
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t th;
    uintptr_t stk_lo, stk_hi;
    volatile int done;          /* the sample below is fresh */
    int nframes;
    uintptr_t frames[MAX_FRAMES];
#endif
} CrashThread;

static CrashThread s_threads[MAX_THREADS];
static RECOMP_TLS int t_guest_main;

#if !defined(_WIN32)
static pthread_key_t s_thread_key;
static pthread_once_t s_thread_once = PTHREAD_ONCE_INIT;

/* At thread exit: the slot's pthread_t and thread-locals are gone. */
static void thread_gone(void *slot) { ((CrashThread *)slot)->used = 0; }
static void thread_key_make(void) { pthread_key_create(&s_thread_key, thread_gone); }
#endif

void xbox_crash_thread_register(const char *role)
{
    unsigned long me = (unsigned long)GetCurrentThreadId();
    int i;

    if (role && !strcmp(role, "guest-main"))
        t_guest_main = 1;
    /* A thread named again (host-main runs the entry point, which names
     * itself guest-main) keeps its slot under the newer name. */
    for (i = 0; i < MAX_THREADS; i++)
        if (s_threads[i].used == 1 && s_threads[i].tid == me) {
            snprintf(s_threads[i].role, sizeof s_threads[i].role, "%s", role ? role : "?");
            return;
        }
    for (i = 0; i < MAX_THREADS; i++) {
        CrashThread *t = &s_threads[i];
#if defined(_MSC_VER)
        if (InterlockedCompareExchange((volatile LONG *)&t->used, 2, 0) != 0)
            continue;
#else
        int expect = 0;
        if (!__atomic_compare_exchange_n(&t->used, &expect, 2, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
#endif
        snprintf(t->role, sizeof t->role, "%s", role ? role : "?");
        t->tid = me;
        t->regs[0] = &g_eax; t->regs[1] = &g_ecx; t->regs[2] = &g_edx;
        t->regs[3] = &g_ebx; t->regs[4] = &g_esp; t->regs[5] = &g_ebp;
        t->regs[6] = &g_esi; t->regs[7] = &g_edi; t->regs[8] = &g_seh_ebp;
#if defined(_WIN32)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
        t->th = pthread_self();
#if defined(__APPLE__)
        t->stk_hi = (uintptr_t)pthread_get_stackaddr_np(t->th);
        t->stk_lo = t->stk_hi - pthread_get_stacksize_np(t->th);
#elif defined(__linux__)
        {
            pthread_attr_t a;
            void *lo = NULL;
            size_t sz = 0;
            if (pthread_getattr_np(t->th, &a) == 0) {
                pthread_attr_getstack(&a, &lo, &sz);
                pthread_attr_destroy(&a);
            }
            t->stk_lo = (uintptr_t)lo;
            t->stk_hi = (uintptr_t)lo + sz;
        }
#endif
        pthread_once(&s_thread_once, thread_key_make);
        pthread_setspecific(s_thread_key, t);
#endif
        t->used = 1;
        return;
    }
}

#if !defined(_WIN32)
static CrashThread *self_slot(void)
{
    pthread_t me = pthread_self();
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (s_threads[i].used == 1 && pthread_equal(s_threads[i].th, me))
            return &s_threads[i];
    return NULL;
}

/* Frames of an interrupted context. macOS: frame records ([fp] = caller's
 * fp, [fp+8] = return address) from the context's pc/lr/fp, bounded by the
 * thread's stack when it is known. Linux: the unwinder, from here; the
 * frames before the signal trampoline are this handler's. */
static int host_frames(void *ucv, const CrashThread *t, uintptr_t *out, int max)
{
    int n = 0;
#if defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t fp, lo, hi;
#if defined(__aarch64__)
    out[n++] = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    out[n++] = (uintptr_t)uc->uc_mcontext->__ss.__lr;
    fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
#else
    out[n++] = (uintptr_t)uc->uc_mcontext->__ss.__rip;
    fp = (uintptr_t)uc->uc_mcontext->__ss.__rbp;
#endif
    lo = t && t->stk_lo ? t->stk_lo : fp;
    hi = t && t->stk_hi ? t->stk_hi : fp + (8u << 20);
    while (n < max && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
        uintptr_t next = ((const uintptr_t *)fp)[0];
        uintptr_t ret = ((const uintptr_t *)fp)[1];
        if (!ret)
            break;
        out[n++] = ret;
        if (next <= fp)
            break;
        fp = next;
    }
#elif defined(__linux__)
    void *pcs[MAX_FRAMES + 8];
    int i, got, skip = 0;
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t pc;
#if defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    pc = 0;
#endif
    (void)t;
    got = backtrace(pcs, MAX_FRAMES + 8);
    /* Start at the interrupted pc when the unwinder went through it. */
    for (i = 0; i < got; i++)
        if ((uintptr_t)pcs[i] == pc) {
            skip = i;
            break;
        }
    if (pc && skip == 0)
        out[n++] = pc;
    for (i = skip; i < got && n < max; i++)
        out[n++] = (uintptr_t)pcs[i];
#else
    (void)ucv; (void)t;
#endif
    return n;
}

void xbox_crash_host_backtrace(void *ucontext)
{
    uintptr_t f[MAX_FRAMES];
    int i, n = host_frames(ucontext, self_slot(), f, MAX_FRAMES);
    xbox_crash_printf("  host backtrace:\n");
    for (i = 0; i < n; i++)
        xbox_crash_host_symbol(i ? "<-" : "pc", f[i]);
}

/* The hang sample: each registered thread takes this signal and records
 * its own frames. SIGURG: ignored by default and unused by this runtime
 * (the guest-CPU token uses SIGUSR2). */
#define SAMPLE_SIGNAL SIGURG

static void sample_handler(int sig, siginfo_t *si, void *ucv)
{
    CrashThread *t;
    int e = errno;
    (void)sig; (void)si;
    t = self_slot();
    if (t) {
        t->nframes = host_frames(ucv, t, t->frames, MAX_FRAMES);
        t->done = 1;
    }
    errno = e;
}

static void sample_threads(void)
{
    static int installed;
    pthread_t me = pthread_self();
    uint64_t t0;
    int i;

    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = sample_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SAMPLE_SIGNAL, &sa, NULL);
        installed = 1;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        CrashThread *t = &s_threads[i];
        t->done = 0;
        t->nframes = 0;
        if (t->used == 1 && !pthread_equal(t->th, me))
            pthread_kill(t->th, SAMPLE_SIGNAL);
    }
    /* A thread blocked with the signal masked never answers; 300 ms. */
    for (t0 = now_ms(); now_ms() - t0 < 300;) {
        int waiting = 0;
        for (i = 0; i < MAX_THREADS; i++)
            if (s_threads[i].used == 1 && !s_threads[i].done
                && !pthread_equal(s_threads[i].th, me))
                waiting = 1;
        if (!waiting)
            break;
        sleep_ms(2);
    }
}
#endif /* !_WIN32 */

static xbox_crash_sampler_fn s_sampler;
void xbox_crash_set_sampler(xbox_crash_sampler_fn fn) { s_sampler = fn; }

/* Every registered thread: role, host frames (POSIX), guest registers and
 * guest stack when it has one. */
static void report_threads(void)
{
    int i;
#if !defined(_WIN32)
    sample_threads();
#endif
    if (s_sampler)
        s_sampler();
    for (i = 0; i < MAX_THREADS; i++) {
        CrashThread *t = &s_threads[i];
        uint32_t r[9];
        int k;
        if (t->used != 1 || t->tid == (unsigned long)GetCurrentThreadId())
            continue;           /* the watchdog itself */
#if defined(_WIN32)
        {
            DWORD code = 0;
            if (!GetExitCodeThread(t->handle, &code) || code != STILL_ACTIVE)
                continue;
        }
#endif
        for (k = 0; k < 9; k++)
            r[k] = t->regs[k] ? *t->regs[k] : 0;
        xbox_crash_printf("\nthread %s (tid %lu)\n", t->role, t->tid);
#if !defined(_WIN32)
        if (t->done)
            for (k = 0; k < t->nframes; k++)
                xbox_crash_host_symbol(k ? "<-" : "pc", t->frames[k]);
        else
            xbox_crash_printf("  (no host sample: the thread did not answer)\n");
#endif
        if (r[4]) {
            print_regs(r);
            if (s_hang_mem)
                reg_windows(r);
            guest_walk(r[4], 24, !strcmp(t->role, "guest-main") ? 256 : 0);
        }
    }
}

/* ── RECOMP_DEBUG=crash_test ───────────────────────────────────────── */

static int s_test_mode;        /* 1 segv, 2 hang */
static unsigned s_test_secs;

void xbox_crash_test_fire(void)
{
    if (!t_guest_main)
        return;               /* the title's main thread, the one that flips */
    g_xbox_crash_test = 0;
    if (s_test_mode == 1) {
        fprintf(stderr, "[CRASH] crash_test: faulting the guest main thread\n");
        *(volatile uint32_t *)(uintptr_t)8 = 0xDEAD;
    } else {
        fprintf(stderr, "[CRASH] crash_test: hanging the guest main thread\n");
        for (;;)
            sleep_ms(1000);
    }
}

/* ── the hang watchdog ─────────────────────────────────────────────── */

static unsigned s_hang_secs;

static void hang_report(unsigned stalled_s)
{
    char head[160];
    snprintf(head, sizeof head, "[HANG] no flip for %u s (flips so far: %u)",
             stalled_s, (unsigned)g_xbox_flip_count);
    if (!xbox_crash_begin("hang", head))
        return;
    print_icalls();
    print_notes();
    s_hang_mem = 1;
    report_threads();
    s_hang_mem = 0;
    dump_guest_ram();
    xbox_crash_end();
}

static void hang_loop(void)
{
    uint32_t last = g_xbox_flip_count;
    uint64_t start = now_ms(), t_last = start, next_note = 0;
    int reported = 0;

    xbox_crash_thread_register("hang-watchdog");
    for (;;) {
        uint64_t now;
        uint32_t c;
        unsigned limit;

        sleep_ms(250);
        now = now_ms();
        if (s_test_mode && s_test_secs != ~0u && now - start >= s_test_secs * 1000ull) {
            s_test_secs = ~0u;
            g_xbox_crash_test = 1;
        }
        if (!s_hang_secs)
            continue;
        c = g_xbox_flip_count;
        if (c != last) {
            if (reported)
                fprintf(stderr, "[HANG] flips resumed after %llu s\n",
                        (unsigned long long)((now - t_last) / 1000));
            last = c;
            t_last = now;
            reported = 0;
            continue;
        }
        /* Boot to the first flip loads and decodes; give it longer. */
        limit = c ? s_hang_secs : (s_hang_secs > 60 ? s_hang_secs : 60);
        if (!reported && now - t_last >= limit * 1000ull) {
            hang_report((unsigned)((now - t_last) / 1000));
            reported = 1;
            next_note = now + s_hang_secs * 1000ull;
        } else if (reported && now >= next_note) {
            fprintf(stderr, "[HANG] still no flip after %llu s\n",
                    (unsigned long long)((now - t_last) / 1000));
            next_note = now + s_hang_secs * 1000ull;
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI hang_thread(LPVOID arg) { (void)arg; hang_loop(); return 0; }
#else
static void *hang_thread(void *arg) { (void)arg; hang_loop(); return NULL; }
#endif

void xbox_crash_watchdog_start(void)
{
    const char *test = recomp_env(RENV_CRASH_TEST);

    s_hang_secs = (unsigned)recomp_env_int(RENV_HANG_WATCHDOG, 15);
    if (test && *test) {
        const char *colon = strchr(test, ':');
        s_test_mode = !strncmp(test, "hang", 4) ? 2 : 1;
        s_test_secs = colon ? (unsigned)atoi(colon + 1) : 5;
        fprintf(stderr, "[CRASH] crash_test=%s armed: %s in %u s\n", test,
                s_test_mode == 1 ? "fault" : "hang", s_test_secs);
    }
    if (!s_hang_secs && !s_test_mode)
        return;
    {
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 0, hang_thread, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, hang_thread, NULL) == 0)
            pthread_detach(th);
#endif
    }
}
